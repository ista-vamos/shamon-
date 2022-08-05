import sys
from typing import Dict

from parser import parse_program
from type_checker import TypeChecker
from cfile_utils import *

input_file = sys.argv[1] # second argument should be input file
output_path = sys.argv[2]

file = " ".join(open(input_file).readlines())

# Type checker initialization
TypeChecker.clean_checker()
TypeChecker.add_reserved_keywords()

# Parser
ast = parse_program(file)
assert(ast[0] == "main_program")


# Produce C file
output_file = open(output_path, "w")

streams_to_events_map = dict()
get_stream_to_events_mapping(ast[1], streams_to_events_map )
stream_types : Dict[str, Tuple[str, str]] = dict() # maps an event source to (input_stream_type, output_stream_type)
get_stream_types(ast[2], stream_types)


program = f'''#include "shamon.h"
#include "mmlib.h"
#include <threads.h>


struct _EVENT_hole
{"{"}
  uint64_t n;
{"}"};
typedef struct _EVENT_hole EVENT_hole;

{event_stream_structs(ast[1])}

{build_should_keep_funcs(ast[2], streams_to_events_map)}

int count_event_streams = {get_count_events_sources(ast)};

// declare event streams
{declare_event_sources(ast)}
//declare flags for event streams
{declare_event_sources_flags(ast)}
// event sources threads
{declare_evt_srcs_threads(ast)}
// declare arbiter thread
thrd_t ARBITER_THREAD;
{declare_arbiter_buffers(ast)}
{event_sources_thread_funcs(ast[2], streams_to_events_map)}
{exists_open_streams(ast)}
bool check_n_events(shm_stream* s, size_t n) {"{"}
    // checks if there are exactly n elements on a given stream s
    void* e1; size_t i1;
	void* e2; size_t i2;
	return shm_arbiter_buffer_peek(b,0, &e1, &i1, &e2, &i2) == n;
{"}"}

bool are_events_in_head(shm_stream *s, shm_arbiter_buffer *b, size_t ev_size, int event_kinds[], int n_events) {"{"}
    char* e1; size_t i1;
	char* e2; size_t i2;
	int count = shm_arbiter_buffer_peek(b, c_events, &e1, &i1, &e2, &i2);
	if (count < n_events) {"{"}
	    return false;
	{"}"}
	
	int i = 0;
	while (i1 > 0) {"{"}
	    i1--;
	    shm_event * ev = (shm_event *) (e1 + (i1*ev_size));
	     if (ev->head.kind != event_kinds[i]) {"{"}
	        return false;
	    {"}"}
	    i+=1;
	{"}"}
	
	while (i2 > 0) {"{"}
	    i2--;
	    shm_event * ev = (shm_event *) (e2 + (i2*ev_size));
	     if (ev->head.kind != event_kinds[i]) {"{"}
	        return false;
	    {"}"}
	    i+=1;
	{"}"}
	
	return true;
{"}"}

{declare_rule_sets(ast[3])}
{build_rule_set_functions(ast[3], streams_to_events_map, stream_types)}
{arbiter_code(ast[3])}
int main(int argc, char **argv) {"{"}
    initialize_events(); // Always call this first
    
{event_sources_conn_code(ast)}
    // activate buffers
{activate_buffers(ast)}

    // create source-events threads
{activate_threads(ast)}

    // create arbiter thread
    thrd_create(&ARBITER_THREAD, arbiter);
     
    // destroy event sources
{destroy_streams(ast)}
    // destroy arbiter buffers
{destroy_buffers(ast)}
{"}"}
'''

output_file.write(program)


