#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "buffer.h"
#include "list.h"
#include "shm.h"
#include "source.h"
#include "spsc_ringbuf.h"
#include "utils.h"
#include "vector-macro.h"

#define SLEEP_TIME_NS 10000

#define MAX_AUX_BUF_KEY_SIZE 16
#define DROPPED_RANGES_NUM   5

struct dropped_range {
    /* the range of autodropped events
    (for garbage collection) */
    shm_eventid begin;
    shm_eventid end;
};

#define _ringbuf(buff) (&buff->shmbuffer->info.ringbuf)

struct buffer_info {
    shm_spsc_ringbuf ringbuf;

    size_t               allocated_size;
    size_t               capacity;
    size_t               elem_size;
    shm_eventid          last_processed_id;
    struct dropped_range dropped_ranges[DROPPED_RANGES_NUM];
    size_t               dropped_ranges_next;
    _Atomic _Bool        dropped_ranges_lock; /* spin lock */
    /* Number of sub-buffers. Sub-buffers are numbered from 1. */
    volatile _Atomic size_t subbuffers_no;
    /* the monitored program exited/destroyed the buffer */
    volatile CACHELINE_ALIGNED _Atomic _Bool destroyed;
    volatile CACHELINE_ALIGNED _Atomic _Bool monitor_attached;
} __attribute__((aligned(CACHELINE_SIZE)));

struct shmbuffer {
    struct buffer_info info;
    /* pointer to the beginning of data */
    unsigned char data[];
};

/* FIXME: this may not be always true */
#define PAGE_SIZE 4096

static size_t compute_shm_size(size_t elem_size, size_t capacity) {
    /* compute how much memory we need */
    size_t size = (elem_size * capacity) + sizeof(struct shmbuffer);
    /* round it up to page size.
     * XXX: do we need that? mmap will do this internally anyway */
    size_t roundup = (size % PAGE_SIZE);
    if (roundup > (PAGE_SIZE) / 4) {
        fprintf(stderr,
                "The required capacity '%lu' of SHM buffer will result in %lu "
                "unused bytes "
                "in a memory page, consider changing it.\n"
                "You have space for %lu more elements...\n",
                capacity, roundup, roundup / elem_size);
    }
    size += roundup;
    return size;
}

struct aux_buffer {
    size_t        size;
    size_t        head;
    size_t        idx;
    uint64_t      first_event_id;
    uint64_t      last_event_id;
    bool          reusable;
    unsigned char data[];
};

/* TODO: cache the shared state in local state
   (e.g., elem_size, etc.). Maybe we could also inline
   the shm_spsc_ringbuf so that we can keep local cache */
struct buffer {
    struct shmbuffer      *shmbuffer;
    struct source_control *control;
    /* shared memory of auxiliary buffer */
    struct aux_buffer *cur_aux_buff;
    /* the known aux_buffers that might still be needed */
    VEC(aux_buffers, struct aux_buffer *);
    size_t   aux_buf_idx;
    shm_list aux_buffers_age;
    /* shm filedescriptor */
    int fd;
    /* shm key */
    char *key;
    /* mode to set to the created SHM file */
    mode_t mode;
    /* The number of the last subbufer */
    _Atomic size_t last_subbufer_no;
};

static size_t aux_buffer_free_space(struct aux_buffer *buff);
static void   aux_buffer_release(struct aux_buffer *buffer);
// static void aux_buffer_destroy(struct aux_buffer *buffer);
static size_t   _buffer_push_strn(struct buffer *buff, const void *data,
                                  size_t size);
static uint64_t buffer_push_str(struct buffer *buff, uint64_t evid,
                                const char *str);
static uint64_t buffer_push_strn(struct buffer *buff, uint64_t evid,
                                 const char *str, size_t len);

static inline void drop_ranges_lock(struct buffer *buff) {
    _Atomic bool *l = &buff->shmbuffer->info.dropped_ranges_lock;
    bool          unlocked;
    do {
        unlocked = false;
    } while (atomic_compare_exchange_weak(l, &unlocked, true));
}

static inline void drop_ranges_unlock(struct buffer *buff) {
    /* FIXME: use explicit memory ordering, seq_cnt is not needed here */
    buff->shmbuffer->info.dropped_ranges_lock = false;
}

static inline bool buffer_is_destroyed(struct buffer *buff) {
    return atomic_load_explicit(&buff->shmbuffer->info.destroyed,
	                        memory_order_relaxed);
}



bool buffer_is_ready(struct buffer *buff) {
    return !buffer_is_destroyed(buff);
}

bool buffer_monitor_attached(struct buffer *buff) {
    return buff->shmbuffer->info.monitor_attached;
}

size_t buffer_capacity(struct buffer *buff) {
    return buff->shmbuffer->info.capacity;
}

size_t buffer_size(struct buffer *buff) {
    return shm_spsc_ringbuf_size(_ringbuf(buff));
}

size_t buffer_elem_size(struct buffer *buff) {
    return buff->shmbuffer->info.elem_size;
}

const char *buffer_get_key(struct buffer *buffer) {
    return buffer->key;
}

int buffer_get_key_path(struct buffer *buff, char keypath[],
                        size_t keypathsize) {
    if (SHM_NAME_MAXLEN <= keypathsize)
        return -1;

    if (shm_mapname(buff->key, keypath) == NULL) {
        return -2;
    }

    return 0;
}

int buffer_get_ctrl_key_path(struct buffer *buff, char keypath[],
                             size_t keypathsize) {
    if (SHM_NAME_MAXLEN <= keypathsize)
        return -1;

    char ctrlkey[SHM_NAME_MAXLEN];
    if (shamon_map_ctrl_key(buff->key, ctrlkey) == NULL) {
        return -3;
    }

    if (shm_mapname(ctrlkey, keypath) == NULL) {
        return -2;
    }

    return 0;
}

/* Pointer to the beginning of the allocated memory. */
#define BUFF_START(b) ((unsigned char *)b->data)
/* Pointer to the first byte after the allocated memory. We allocate 1 more byte than
   is the desired capacity. */
#define BUFF_END(b) ((unsigned char *)b->data + (b->info.elem_size * (b->info.capacity + 1)))

static struct buffer *initialize_shared_buffer(const char *key, mode_t mode,
                                               size_t                 elem_size,
                                               size_t                 capacity,
                                               struct source_control *control) {
    assert(elem_size > 0 && "Element size is 0");
    assert(capacity > 0 && "Capacity is 0");
    /* the ringbuffer has one unusable dummy element, so increase the capacity
     * by one */
    const size_t memsize = compute_shm_size(elem_size, capacity + 1);

    fprintf(stderr,
            "Initializing buffer '%s' with elem size '%lu' and minimum "
            "capacity '%lu' (%lu pages)\n",
            key, elem_size, capacity, memsize / PAGE_SIZE);
    int fd = shamon_shm_open(key, O_RDWR | O_CREAT | O_TRUNC, mode);
    if (fd < 0) {
        perror("shm_open");
        return NULL;
    }

    if ((ftruncate(fd, memsize)) == -1) {
        perror("ftruncate");
        return NULL;
    }

    void *shmem = mmap(0, memsize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shmem == MAP_FAILED) {
        perror("mmap failure");
        if (close(fd) == -1) {
            perror("closing fd after mmap failure");
        }
        if (shamon_shm_unlink(key) != 0) {
            perror("shm_unlink after mmap failure");
        }
        return NULL;
    }

    struct buffer *buff = xalloc(sizeof(struct buffer));
    buff->shmbuffer     = (struct shmbuffer *)shmem;
    assert(ADDR_IS_CACHE_ALIGNED(buff->shmbuffer->data));
    assert(ADDR_IS_CACHE_ALIGNED(&buff->shmbuffer->info.ringbuf));

    memset(buff->shmbuffer, 0, sizeof(struct buffer_info));

    buff->shmbuffer->info.allocated_size = memsize;
    buff->shmbuffer->info.capacity       = capacity;
    /* ringbuf has one dummy element and we allocated the space for it */
    shm_spsc_ringbuf_init(_ringbuf(buff), capacity + 1);
    buff->shmbuffer->info.elem_size           = elem_size;
    buff->shmbuffer->info.last_processed_id   = 0;
    buff->shmbuffer->info.dropped_ranges_next = 0;
    buff->shmbuffer->info.dropped_ranges_lock = false;
    buff->shmbuffer->info.subbuffers_no       = 0;

    fprintf(stderr, "  .. buffer allocated size = %lu, capacity = %lu\n",
            buff->shmbuffer->info.allocated_size,
            buff->shmbuffer->info.capacity);
    fprintf(stderr, "  .. buffer memory range:  %p - %p\n",
            (void*)BUFF_START(buff->shmbuffer), (void*)BUFF_END(buff->shmbuffer));

#ifndef NDEBUG
    assert((size_t)(BUFF_END(buff->shmbuffer) - BUFF_START(buff->shmbuffer)) == (capacity + 1)*elem_size);
    /* In debugging mode, set the allocated memory to test if it is really accessible
     * and that our structures are not (incorrectly) overlapping with the memory */
    memset(BUFF_START(buff->shmbuffer), 0xff, capacity*elem_size);
#endif

    buff->key = strdup(key);
    VEC_INIT(buff->aux_buffers);
    shm_list_init(&buff->aux_buffers_age);
    buff->aux_buf_idx      = 0;
    buff->cur_aux_buff     = NULL;
    buff->fd               = fd;
    buff->control          = control;
    buff->mode             = mode;
    buff->last_subbufer_no = 0;

    puts("Done");
    return buff;
}

static struct source_control *
create_shared_control_buffer(const char *buff_key, mode_t mode,
                             const struct source_control *control);

struct buffer *create_shared_buffer(const char *key, size_t capacity,
                                    const struct source_control *control) {
    struct source_control *ctrl =
        create_shared_control_buffer(key, S_IRWXU, control);
    if (!ctrl) {
        fprintf(stderr, "Failed creating control buffer\n");
        return NULL;
    }

    size_t elem_size = source_control_max_event_size(ctrl);
    return initialize_shared_buffer(key, S_IRWXU, elem_size, capacity, ctrl);
}

struct buffer *create_shared_buffer_adv(const char *key, mode_t mode,
                                        size_t elem_size, size_t capacity,
                                        const struct source_control *control) {
    struct source_control *ctrl =
        create_shared_control_buffer(key, mode, control);
    if (!ctrl) {
        fprintf(stderr, "Failed creating control buffer\n");
        return NULL;
    }

    if (elem_size == 0) {
        elem_size = source_control_max_event_size(ctrl);
    }

    if (mode == 0) {
        mode = S_IRWXU;
    }

    return initialize_shared_buffer(key, mode, elem_size, capacity, ctrl);
}

char *get_sub_buffer_key(const char *key, size_t idx) {
    size_t tmpsize = strlen(key) + 16 /* space for index */;
    char  *tmp     = xalloc(tmpsize);
    int    written = snprintf(tmp, tmpsize, "%s.sub.%lu", key, idx);
    if (written < 0 || written >= (int)tmpsize) {
        fprintf(stderr, "Failed creating sub-buffer key for '%s'\n", key);
        abort();
    }
    return tmp;
}

struct buffer *create_shared_sub_buffer(struct buffer *buffer, size_t capacity,
                                        const struct source_control *control) {
    char *key = get_sub_buffer_key(buffer->key, ++buffer->last_subbufer_no);
    struct source_control *ctrl =
        create_shared_control_buffer(key, S_IRWXU, control);
    if (!ctrl) {
        fprintf(stderr, "Failed creating control buffer\n");
        return NULL;
    }

    size_t elem_size = source_control_max_event_size(ctrl);
    if (capacity == 0)
        capacity = buffer_capacity(buffer);
    struct buffer *sbuf =
        initialize_shared_buffer(key, S_IRWXU, elem_size, capacity, ctrl);
    /* TODO: we copy the key in 'initialize_shared_buffer' which is redundant as
     * we have created it in `get_sub_buffer_key` and can just move it */
    free(key);

    ++buffer->shmbuffer->info.subbuffers_no;

    return sbuf;
}

size_t buffer_get_sub_buffers_no(struct buffer *buffer) {
    return buffer->shmbuffer->info.subbuffers_no;
}

/* FOR TESTING */
struct buffer *initialize_local_buffer(const char *key, size_t elem_size,
                                       size_t                 capacity,
                                       struct source_control *control) {
    assert(elem_size > 0 && "Element size is 0");
    printf("Initializing LOCAL buffer '%s' with elem size '%lu'\n", key,
           elem_size);
    void        *mem;
    const size_t memsize = compute_shm_size(elem_size, capacity);
    int          succ    = posix_memalign(&mem, 64, memsize);
    if (succ != 0) {
        perror("allocation failure");
        return NULL;
    }

    struct buffer *buff = malloc(sizeof(struct buffer));
    assert(buff && "Memory allocation failed");
    buff->shmbuffer = (struct shmbuffer *)mem;

    assert(ADDR_IS_CACHE_ALIGNED(buff->shmbuffer->data));
    assert(ADDR_IS_CACHE_ALIGNED(&buff->shmbuffer->info.ringbuf));

    memset(buff->shmbuffer, 0, sizeof(struct buffer_info));

    /* ringbuf has one dummy element */
    buff->shmbuffer->info.capacity       = capacity;
    buff->shmbuffer->info.allocated_size = memsize;
    shm_spsc_ringbuf_init(_ringbuf(buff), capacity + 1);
    printf("  .. buffer allocated size = %lu, capacity = %lu\n",
           buff->shmbuffer->info.allocated_size,
           buff->shmbuffer->info.capacity);
    buff->shmbuffer->info.elem_size           = elem_size;
    buff->shmbuffer->info.last_processed_id   = 0;
    buff->shmbuffer->info.dropped_ranges_next = 0;
    buff->shmbuffer->info.dropped_ranges_lock = false;

    buff->key = strdup(key);
    VEC_INIT(buff->aux_buffers);
    shm_list_init(&buff->aux_buffers_age);
    buff->aux_buf_idx  = 0;
    buff->cur_aux_buff = NULL;
    buff->fd           = -1;
    buff->control      = control;

    puts("Done");
    return buff;
}

void release_local_buffer(struct buffer *buff) {
    free(buff->key);

    size_t vecsize = VEC_SIZE(buff->aux_buffers);
    for (size_t i = 0; i < vecsize; ++i) {
        struct aux_buffer *ab = buff->aux_buffers[i];
        aux_buffer_release(ab);
    }
    VEC_DESTROY(buff->aux_buffers);

    free(buff);
}

static struct source_control *get_shared_control_buffer(const char *buff_key);

struct buffer *try_get_shared_buffer(const char *key, size_t retry) {
    fprintf(stderr, "getting shared buffer '%s'\n", key);

    int fd = -1;
    ++retry;
    do {
        fd = shamon_shm_open(key, O_RDWR, S_IRWXU);
        if (fd >= 0) {
            break;
        }
        sleep_ms(300);
    } while (--retry > 0);

    if (fd == -1) {
        perror("shm_open");
        fprintf(stderr, "Failed getting shared buffer '%s'\n", key);
        return NULL;
    }

    struct buffer_info info;
    if (pread(fd, &info, sizeof(info), 0) == -1) {
        perror("reading info of shared buffer");
        close(fd);
        return NULL;
    }

    fprintf(stderr, "   ... its size is %lu\n", info.allocated_size);
    if (info.allocated_size == 0) {
        fprintf(stderr, "Invalid allocated size of SHM buffer: %lu\n",
                info.allocated_size);
        close(fd);
        return NULL;
    }

    void *shmmem =
        mmap(0, info.allocated_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shmmem == MAP_FAILED) {
        perror("mmap failure");
        goto before_mmap_clean;
    }

    struct buffer *buff = malloc(sizeof(*buff));
    if (!buff) {
        fprintf(stderr, "%s:%d: memory allocation failed\n", __func__,
                __LINE__);
        goto mmap_clean;
    }

    buff->key = strdup(key);
    if (!buff->key) {
        fprintf(stderr, "%s:%d: memory allocation failed\n", __func__,
                __LINE__);
        goto buff_clean_key;
    }

    VEC_INIT(buff->aux_buffers);

    buff->control = get_shared_control_buffer(key);
    if (!buff->control) {
        fprintf(stderr, "%s:%d: failed getting control buffer\n", __func__,
                __LINE__);
        goto buff_clean_all;
    }

    buff->shmbuffer    = (struct shmbuffer *)shmmem;
    buff->aux_buf_idx  = 0;
    buff->cur_aux_buff = NULL;
    buff->fd           = fd;
    buff->mode         = 0;

    return buff;

buff_clean_all:
    VEC_DESTROY(buff->aux_buffers);
    free(buff->key);
buff_clean_key:
    free(buff);
mmap_clean:
    munmap(shmmem, info.allocated_size);
before_mmap_clean:
    if (close(fd) == -1) {
        perror("closing fd after mmap failure");
    }
    if (shamon_shm_unlink(key) != 0) {
        perror("shm_unlink after mmap failure");
    }
    return NULL;
}

struct buffer *get_shared_buffer(const char *key) {
    return try_get_shared_buffer(key, 10);
}

struct event_record *buffer_get_avail_events(struct buffer *buff,
                                             size_t        *evs_num) {
    assert(buff);
    assert(evs_num);
    assert(buff->control);

    *evs_num = source_control_get_records_num(buff->control);
    return buff->control->events;
}

void buffer_set_attached(struct buffer *buff, bool val) {
    if (!buffer_is_destroyed(buff))
        buff->shmbuffer->info.monitor_attached = val;
}

/* set the ID of the last processed event */
void buffer_set_last_processed_id(struct buffer *buff, shm_eventid id) {
    assert(buff->shmbuffer->info.last_processed_id <= id &&
           "The IDs are not monotonic");
    buff->shmbuffer->info.last_processed_id = id;
}

static void release_shared_control_buffer(struct source_control *buffer) {
    if (munmap(buffer, buffer->size) != 0) {
        perror("munmap failure");
    }

    /* TODO: we leak fd */
}

static void destroy_shared_control_buffer(const char            *buffkey,
                                          struct source_control *buffer) {
    release_shared_control_buffer(buffer);

    char key[SHM_NAME_MAXLEN];
    shamon_map_ctrl_key(buffkey, key);
    if (shamon_shm_unlink(key) != 0) {
        perror("destroy_shared_control_buffer: shm_unlink failure");
    }
}

/* for readers */
void release_shared_buffer(struct buffer *buff) {
    if (munmap(buff->shmbuffer, buff->shmbuffer->info.allocated_size) != 0) {
        perror("release_shared_buffer: munmap failure");
    }
    if (close(buff->fd) == -1) {
        perror("release_shared_buffer: failed closing mmap fd");
    }

    size_t vecsize = VEC_SIZE(buff->aux_buffers);
    for (size_t i = 0; i < vecsize; ++i) {
        struct aux_buffer *ab = buff->aux_buffers[i];
        aux_buffer_release(ab);
    }
    VEC_DESTROY(buff->aux_buffers);

    release_shared_control_buffer(buff->control);

    free(buff->key);
    free(buff);
}

/* for writers */
void destroy_shared_sub_buffer(struct buffer *buff) {
    atomic_store_explicit(&buff->shmbuffer->info.destroyed,
		          1, memory_order_release);

    size_t vecsize = VEC_SIZE(buff->aux_buffers);
    for (size_t i = 0; i < vecsize; ++i) {
        struct aux_buffer *ab = buff->aux_buffers[i];
        // we must first wait until the monitor finishes
        // aux_buffer_destroy(ab);
        aux_buffer_release(ab);
    }
    VEC_DESTROY(buff->aux_buffers);
    fprintf(stderr, "Totally used %lu aux buffers\n", vecsize);

    if (munmap(buff->shmbuffer, buff->shmbuffer->info.allocated_size) != 0) {
        perror("destroy_shared_buffer: munmap failure");
    }
    if (close(buff->fd) == -1) {
        perror("destroy_shared_buffer: failed closing mmap fd");
    }

    release_shared_control_buffer(buff->control);

    free(buff->key);
    free(buff);
}

/* for readers */
void release_shared_sub_buffer(struct buffer *buff) {
    if (munmap(buff->shmbuffer, buff->shmbuffer->info.allocated_size) != 0) {
        perror("release_shared_sub_buffer: munmap failure");
    }
    if (close(buff->fd) == -1) {
        perror("release_shared_sub_buffer: failed closing mmap fd");
    }

    size_t vecsize = VEC_SIZE(buff->aux_buffers);
    for (size_t i = 0; i < vecsize; ++i) {
        struct aux_buffer *ab = buff->aux_buffers[i];
        aux_buffer_release(ab);
    }
    VEC_DESTROY(buff->aux_buffers);

    if (shamon_shm_unlink(buff->key) != 0) {
        perror("release_shared_sub_buffer: shm_unlink failure");
    }

    destroy_shared_control_buffer(buff->key, buff->control);

    free(buff->key);
    free(buff);
}

/* for writers */
void destroy_shared_buffer(struct buffer *buff) {
    atomic_store_explicit(&buff->shmbuffer->info.destroyed,
		          1, memory_order_release);

    size_t vecsize = VEC_SIZE(buff->aux_buffers);
    for (size_t i = 0; i < vecsize; ++i) {
        struct aux_buffer *ab = buff->aux_buffers[i];
        // we must first wait until the monitor finishes
        // aux_buffer_destroy(ab);
        aux_buffer_release(ab);
    }
    VEC_DESTROY(buff->aux_buffers);
    fprintf(stderr, "Totally used %lu aux buffers\n", vecsize);

    if (munmap(buff->shmbuffer, buff->shmbuffer->info.allocated_size) != 0) {
        perror("destroy_shared_buffer: munmap failure");
    }
    if (close(buff->fd) == -1) {
        perror("destroy_shared_buffer: failed closing mmap fd");
    }
    if (shamon_shm_unlink(buff->key) != 0) {
        perror("destroy_shared_buffer: shm_unlink failure");
    }

    destroy_shared_control_buffer(buff->key, buff->control);

    free(buff->key);
    free(buff);
}

void *buffer_read_pointer(struct buffer *buff, size_t *size) {
    struct buffer_info *info = &buff->shmbuffer->info;
    size_t tail = shm_spsc_ringbuf_read_off_nowrap(&info->ringbuf, size);
    if (*size == 0)
        return NULL;
    /* TODO: get rid of the multiplication,
     * incrementally shift a pointer instead */
    return buff->shmbuffer->data + tail * info->elem_size;
}

bool buffer_drop_k(struct buffer *buff, size_t k) {
    return shm_spsc_ringbuf_consume_upto(_ringbuf(buff), k) == k;
}

size_t buffer_consume(struct buffer *buff, size_t k) {
    return shm_spsc_ringbuf_consume_upto(_ringbuf(buff), k);
}

/* buffer_push broken down into several operations:
 *
 *  p = buffer_start_push(...)
 *  p = buffer_partial_push(..., p, ...)
 *  ...
 *  p = buffer_partial_push(..., p, ...)
 *  buffer_partial_push(..., p, ...)
 *  buffer_finish_push(...)
 *
 * All the partial push may push only a single element in sum,
 * i.e., what can be done with buffer_push. Partial pushes
 * cannot be mixed nor combined with normal push operations.
 * These are really just one buffer_push broken down into
 * multiple steps.
 */

void *buffer_start_push(struct buffer *buff) {
    struct buffer_info *info = &buff->shmbuffer->info;
    assert(!buffer_is_destroyed(buff) && "Writing to a destroyed buffer");

    size_t n;
    size_t off = shm_spsc_ringbuf_write_off_nowrap(_ringbuf(buff), &n);
    if (n == 0) {
        /* ++info->dropped; */
        return NULL;
    }

    /* all ok, return the pointer to the data */
    /* FIXME: do not use multiplication, maintain the pointer to the head of
     * data */
    void *mem = buff->shmbuffer->data + off * info->elem_size;
    assert((void*)BUFF_START(buff->shmbuffer) <= mem);
    assert(mem < (void*)BUFF_END(buff->shmbuffer));
    return mem;
}

void *buffer_partial_push(struct buffer *buff, void *prev_push,
                          const void *elem, size_t size) {
    assert(buffer_is_ready(buff) && "Writing to a destroyed buffer");
    assert(BUFF_START(buff->shmbuffer) <= (unsigned char *)prev_push);
    assert((unsigned char *)prev_push < BUFF_END(buff->shmbuffer));
    assert((unsigned char *)prev_push <= BUFF_END(buff->shmbuffer) - size);
    (void)buff;

    memcpy(prev_push, elem, size);
    return (unsigned char *)prev_push + size;
}

void *buffer_partial_push_str(struct buffer *buff, void *prev_push,
                              uint64_t evid, const char *str) {
    assert(buffer_is_ready(buff) && "Writing to a destroyed buffer");
    assert(BUFF_START(buff->shmbuffer) <= (unsigned char *)prev_push);
    assert((unsigned char *)prev_push < BUFF_END(buff->shmbuffer));

    *((uint64_t *)prev_push) = buffer_push_str(buff, evid, str);
    /*printf("Pushed str: %lu\n", *((uint64_t *)prev_push));*/
    return (unsigned char *)prev_push + sizeof(uint64_t);
}

void *buffer_partial_push_str_n(struct buffer *buff, void *prev_push,
                                uint64_t evid, const char *str, size_t len) {
    assert(buffer_is_ready(buff) && "Writing to a destroyed buffer");
    assert(BUFF_START(buff->shmbuffer) <= (unsigned char *)prev_push);
    assert((unsigned char *)prev_push < BUFF_END(buff->shmbuffer));

    *((uint64_t *)prev_push) = buffer_push_strn(buff, evid, str, len);
    /*printf("Pushed str: %lu\n", *((uint64_t *)prev_push));*/
    return (unsigned char *)prev_push + sizeof(uint64_t);
}

void buffer_finish_push(struct buffer *buff) {
    assert(buffer_is_ready(buff) && "Writing to a destroyed buffer");
    shm_spsc_ringbuf_write_finish(_ringbuf(buff), 1);
}

bool buffer_push(struct buffer *buff, const void *elem, size_t size) {
    assert(buffer_is_ready(buff) && "Writing to a destroyed buffer");
    assert(buff->shmbuffer->info.elem_size >= size &&
           "Size does not fit the slot");

    void *dst = buffer_start_push(buff);
    if (dst == NULL)
        return false;

    memcpy(dst, elem, size);
    buffer_finish_push(buff);

    return true;
}

bool buffer_pop(struct buffer *buff, void *dst) {
    assert(buffer_is_ready(buff) && "Writing to a destroyed buffer");

    size_t size;
    void  *pos = buffer_read_pointer(buff, &size);
    if (size > 0) {
        memcpy(dst, pos, buff->shmbuffer->info.elem_size);
        shm_spsc_ringbuf_consume(_ringbuf(buff), 1);
        return true;
    }

    return false;
}

#if 0
/* copy k elements out of the buffer */
bool buffer_pop_k(struct buffer *buff, void *dst, size_t k) {
    struct buffer_info *info = &buff->shmbuffer->info;
    assert(!info->destroyed && "Reading from a destroyed buffer");
    if (elem_num(info) < k) {
        return false;
    }

    unsigned char *pos = buff->shmbuffer->data + info->tail * info->elem_size;
    size_t end = info->tail + k;
    if (end > info->capacity) {
        memcpy(dst, pos, (info->capacity - info->tail) * info->elem_size);
        memcpy((unsigned char *)dst +
                   info->elem_size * (info->capacity - info->tail),
               buff->shmbuffer->data, (end - info->capacity) * info->elem_size);
        info->tail = end - info->capacity;
    } else {
        memcpy(dst, pos, k * info->elem_size);
        info->tail = end;
    }

    assert(elem_num(info) > 0);
    elem_num_dec(info, k);
    return true;
}
#endif

/*** CONTROL BUFFER ****/

struct source_control *
create_shared_control_buffer(const char *buff_key, mode_t mode,
                             const struct source_control *control) {
    char key[SHM_NAME_MAXLEN];
    shamon_map_ctrl_key(buff_key, key);
    size_t size = control->size;

    printf("Initializing control buffer '%s' of size '%lu'\n", key, size);
    int fd = shamon_shm_open(key, O_RDWR | O_CREAT, mode);
    if (fd < 0) {
        perror("shm_open");
        return NULL;
    }

    /* The user does not want a control buffer, but we expect to have it,
     * event if it is empty. Make the size be at least such that it can
     * hold the size variable */
    if (size == 0) {
        size = sizeof(control->size);
    }
    assert(size >= sizeof(control->size));

    if ((ftruncate(fd, size)) == -1) {
        perror("ftruncate");
        return NULL;
    }

    void *mem = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) {
        perror("mmap failure");
        if (close(fd) == -1) {
            perror("closing fd after mmap failure");
        }
        if (shamon_shm_unlink(key) != 0) {
            perror("shm_unlink after mmap failure");
        }
        return NULL;
    }

    memcpy(mem, control, size);

    return (struct source_control *)mem;
}

static struct source_control *get_shared_control_buffer(const char *buff_key) {
    char key[SHM_NAME_MAXLEN];
    shamon_map_ctrl_key(buff_key, key);

    printf("Getting control buffer '%s'\n", key);
    int fd = shamon_shm_open(key, O_RDWR | O_CREAT, S_IRWXU);
    if (fd < 0) {
        perror("shm_open");
        return NULL;
    }

    size_t size;
    if (pread(fd, &size, sizeof(size), 0) == -1) {
        perror("reading size of ctrl buffer");
        return NULL;
    }
    printf("   ... its size is %lu\n", size);

    void *mem = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) {
        perror("mmap failure");
        if (close(fd) == -1) {
            perror("closing fd after mmap failure");
        }
        if (shamon_shm_unlink(key) != 0) {
            perror("shm_unlink after mmap failure");
        }
        return NULL;
    }

    /* FIXME: we leak fd */
    return (struct source_control *)mem;
}

/***** AUX BUFFERS ********/
size_t aux_buffer_free_space(struct aux_buffer *buff) {
    return buff->size - buff->head;
}

static struct aux_buffer *new_aux_buffer(struct buffer *buff, size_t size) {
    size_t       idx     = buff->aux_buf_idx++;
    const size_t pg_size = sysconf(_SC_PAGESIZE);
    size = (((size + sizeof(struct aux_buffer)) / pg_size) + 2) * pg_size;

    /* create the key */
    char key[20];
    snprintf(key, 19, "/aux.%lu", idx);

    /* printf("Initializing aux buffer %s\n", key); */

    int fd = shamon_shm_open(key, O_RDWR | O_CREAT, buff->mode);
    if (fd < 0) {
        perror("shm_open");
        abort();
    }

    if ((ftruncate(fd, size)) == -1) {
        perror("ftruncate");
        abort();
    }

    void *mem = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) {
        perror("mmap failure");
        if (close(fd) == -1) {
            perror("closing fd after mmap failure");
        }
        if (shamon_shm_unlink(key) != 0) {
            perror("shm_unlink after mmap failure");
        }
        abort();
    }

    struct aux_buffer *ab = (struct aux_buffer *)mem;
    ab->head              = 0;
    ab->size              = size - sizeof(struct aux_buffer);
    ab->idx               = idx;
    ab->first_event_id    = 0;
    ab->last_event_id     = ~(0LL);
    ab->reusable          = false;

    VEC_PUSH(buff->aux_buffers, &ab);
    assert(VEC_TOP(buff->aux_buffers) == ab);
    shm_list_append(&buff->aux_buffers_age, ab);
    assert(shm_list_last(&buff->aux_buffers_age)->data == ab);
    buff->cur_aux_buff = ab;

    return ab;
}

static inline bool ab_was_dropped(struct aux_buffer *ab, struct buffer *buff) {
    struct buffer_info *info = &buff->shmbuffer->info;
    drop_ranges_lock(buff);
    for (size_t i = 0; i < DROPPED_RANGES_NUM; ++i) {
        struct dropped_range *r = &info->dropped_ranges[i];
        if (r->end == 0)
            continue;
        if (r->begin <= ab->first_event_id && r->end >= ab->last_event_id) {
            drop_ranges_unlock(buff);
            return true;
        }
    }
    drop_ranges_unlock(buff);

    return false;
}

static struct aux_buffer *writer_get_aux_buffer(struct buffer *buff,
                                                size_t         size) {
    if (!buff->cur_aux_buff ||
        aux_buffer_free_space(buff->cur_aux_buff) < size) {
        /* try to find a free buffer */
        struct aux_buffer *ab;
        shm_list_elem     *cur = shm_list_first(&buff->aux_buffers_age);
        while (cur) {
            ab = (struct aux_buffer *)cur->data;
            if (ab->last_event_id <= buff->shmbuffer->info.last_processed_id ||
                ab_was_dropped(ab, buff)) {
                ab->reusable       = true;
                ab->head           = 0;
                ab->first_event_id = 0;
                ab->last_event_id  = ~(0LL);
            }
            if (ab->reusable && ab->size >= size) {
                assert(shm_list_last(&buff->aux_buffers_age)->data ==
                       buff->cur_aux_buff);
                shm_list_remove(&buff->aux_buffers_age, cur);
                shm_list_append_elem(&buff->aux_buffers_age, cur);
                buff->cur_aux_buff = ab;
                ab->reusable       = false;
                return ab;
            }
            cur = cur->next;
        }

        ab = new_aux_buffer(buff, size);
        return ab;
    }

    /* use the current buffer */
    /* it must always be the last in the aux_buffers_age */
    assert(shm_list_last(&buff->aux_buffers_age)->data == buff->cur_aux_buff);
    return buff->cur_aux_buff;
}

static struct aux_buffer *reader_get_aux_buffer(struct buffer *buff,
                                                size_t         idx) {
    /* cache the last use */
    if (buff->cur_aux_buff && buff->cur_aux_buff->idx == idx)
        return buff->cur_aux_buff;

    /* try to find one with the idx */
    /* FIXME: make it constant access */
    for (size_t i = 0; i < VEC_SIZE(buff->aux_buffers); ++i) {
        struct aux_buffer *ab = buff->aux_buffers[i];
        if (ab->idx == idx) {
            buff->cur_aux_buff = ab;
            return ab;
        }
    }

    /* create the key */
    char key[20];
    snprintf(key, 19, "/aux.%lu", idx);

    // printf("Getting aux buffer %s\n", key);

    int fd = shamon_shm_open(key, O_RDWR, S_IRWXU);
    if (fd < 0) {
        perror("shm_open");
        abort();
    }

    size_t size;
    if (read(fd, &size, sizeof(size)) == -1) {
        perror("reading size of aux buffer");
        return NULL;
    }
    // printf("   ... its size is %lu\n", size);

    void *mem = mmap(0, size + 3 * sizeof(size_t), PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) {
        perror("mmap failure");
        if (close(fd) == -1) {
            perror("closing fd after mmap failure");
        }
        if (shamon_shm_unlink(key) != 0) {
            perror("shm_unlink after mmap failure");
        }
        abort();
    }

    struct aux_buffer *ab = (struct aux_buffer *)mem;
    assert(ab->idx == idx && "Got wrong buffer");
    assert(ab->size > 0);

    buff->cur_aux_buff = ab;
    VEC_PUSH(buff->aux_buffers, &ab);
    assert(VEC_TOP(buff->aux_buffers) == ab);

    return ab;
}

void aux_buffer_release(struct aux_buffer *buffer) {
    assert((buffer->size + sizeof(struct aux_buffer)) % sysconf(_SC_PAGESIZE) ==
           0);
    // int fd = buffer->fd;
    if (munmap(buffer, buffer->size + sizeof(struct aux_buffer)) != 0) {
        perror("aux_buffer_destroy: munmap failure");
    }
    /* FIXME
    if (close(fd) == -1) {
        perror("aux_buffer_destroy: closing fd after mmap failure");
    }
    */
}

/*
void aux_buffer_destroy(struct aux_buffer *buffer) {
    char key[20];
    snprintf(key, 19, "/aux.%lu", buffer->idx);

    aux_buffer_release(buffer);

    if (shamon_shm_unlink(key) != 0) {
        perror("aux_buffer_destroy: shm_unlink failure");
    }
}
*/

size_t _buffer_push_strn(struct buffer *buff, const void *data, size_t size) {
    struct aux_buffer *ab = writer_get_aux_buffer(buff, size);
    assert(ab);
    assert(ab == buff->cur_aux_buff);
    assert(shm_list_last(&buff->aux_buffers_age)->data == buff->cur_aux_buff);

    size_t off = ab->head;
    assert(off < (1LU << 32));

    memcpy(ab->data + off, data, size);
    ab->head += size;
    return off;
}

void *buffer_get_str(struct buffer *buff, uint64_t elem) {
    size_t             idx = elem >> 32;
    struct aux_buffer *ab  = reader_get_aux_buffer(buff, idx);
    size_t             off = elem & 0xffffffff;
    return ab->data + off;
}

uint64_t buffer_push_str(struct buffer *buff, uint64_t evid, const char *str) {
    size_t len = strlen(str) + 1;
    size_t off = buffer_push_strn(buff, evid, str, len);
    assert(buff->cur_aux_buff);
    return (off | (buff->cur_aux_buff->idx << 32));
}

uint64_t buffer_push_strn(struct buffer *buff, uint64_t evid, const char *str,
                          size_t len) {
    size_t             off = _buffer_push_strn(buff, str, len);
    struct aux_buffer *ab  = buff->cur_aux_buff;
    assert(ab);
    if (ab->first_event_id == 0)
        ab->first_event_id = evid;
    ab->last_event_id = evid;
    return (off | (buff->cur_aux_buff->idx << 32));
}

void buffer_notify_dropped(struct buffer *buff, uint64_t begin_id,
                           uint64_t end_id) {
    struct buffer_info   *info = &buff->shmbuffer->info;
    size_t                idx  = info->dropped_ranges_next;
    struct dropped_range *r    = &info->dropped_ranges[idx];
    if (r->begin == begin_id || r->end == r->begin - 1) {
        drop_ranges_lock(buff);
        r->end = end_id;
        drop_ranges_unlock(buff);
        return;
    }

    if (idx + 1 == DROPPED_RANGES_NUM) {
        r                         = &info->dropped_ranges[0];
        info->dropped_ranges_next = 0;
    } else {
        ++r;
        ++info->dropped_ranges_next;
    }

    drop_ranges_lock(buff);
    r->begin = begin_id;
    r->end   = end_id;
    drop_ranges_unlock(buff);
}

int buffer_register_event(struct buffer *b, const char *name, uint64_t kind) {
    struct event_record *rec = source_control_get_event(b->control, name);
    if (rec == NULL) {
        return -1;
    }

    rec->kind = kind;
    return 0;
}

int buffer_register_events(struct buffer *b, size_t ev_nums, ...) {
    va_list ap;
    va_start(ap, ev_nums);

    for (size_t i = 0; i < ev_nums; ++i) {
        const char *name = va_arg(ap, const char *);
        shm_kind    kind = va_arg(ap, shm_kind);
        if (buffer_register_event(b, name, kind) < 0) {
            va_end(ap);
            return -1;
        }
    }

    va_end(ap);

    return 0;
}

int buffer_register_all_events(struct buffer *b) {
    struct event_record *recs    = b->control->events;
    const size_t         ev_nums = source_control_get_records_num(b->control);

    for (size_t i = 0; i < ev_nums; ++i) {
        recs[i].kind = 1 + i + shm_get_last_special_kind();
    }

    return 0;
}
