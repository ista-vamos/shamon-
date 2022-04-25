#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "stream-stdin.h"

bool stdin_has_event(shm_stream *stream) {
        //return fseek(stdin, 0, SEEK_END), ftell(stdin) > 0;
    return true;
}

size_t stdin_buffer_events(shm_stream *stream,
                           shm_arbiter_buffer *buffer) {
    static shm_event_stdin ev;
    shm_stream_stdin *ss = (shm_stream_stdin *) stream;
    ssize_t len = getline(&ss->line, &ss->line_len, stdin);

    // TODO: return end-of-stream event
    if (len == -1)
        return 0;

    ev.time = clock();
    ev.base.stream = stream;
    ev.base.kind = ss->ev_kind;
    ev.base.id = shm_stream_get_next_id(stream);
    ev.fd = fileno(stdin);
    ev.str_ref.size = len;
    ev.str_ref.data = ss->line;

    shm_arbiter_buffer_push(buffer, &ev, sizeof(ev));
    return 1;
}

shm_stream *shm_create_stdin_stream() {
    shm_stream_stdin *ss = malloc(sizeof *ss);
    shm_stream_init((shm_stream *)ss, sizeof(shm_event_stdin),
                     stdin_has_event, stdin_buffer_events,
                     "stdin-stream");
    ss->line = NULL;
    ss->line_len = 0;
    ss->ev_kind = shm_mk_event_kind("stdin", sizeof(shm_event_stdin), NULL, NULL);
    return (shm_stream *) ss;
}

void shm_destroy_stdin_stream(shm_stream_stdin *ss) {
    free(ss->line);
    free(ss);
}
