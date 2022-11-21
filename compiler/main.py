import sys

from parser import parse_program
from type_checker import TypeChecker
import argparse
from cfile_utils import get_c_program
from utils import *
import os
from tessla_utils import get_rust_file, get_c_interface, update_toml

parser = argparse.ArgumentParser()
parser.add_argument("inputfile", type=str, help="VAMOS program to compile.")
parser.add_argument("-o", "--out", help="Outputfile to write program (this option is not used when --with-tessla).")
parser.add_argument("-t", "--with-tessla",
					help="Compile into a Rust file that makes use of a library generated by Tessla.",
					action="store_true")
parser.add_argument("-d", "--dir",
					help="Directory where the library generated by Tessla is (not needed if --with-tessla flag is present).")
parser.add_argument("-b", "--bufsize")

args = parser.parse_args()
bufsize = args.bufsize

input_file = args.inputfile  # second argument should be input file
parsed_args_file = replace_cmd_args(open(input_file).readlines(), bufsize)
file = " ".join(parsed_args_file)


# Type checker initialization
TypeChecker.clean_checker()
TypeChecker.add_reserved_keywords()

# Parser
ast = parse_program(file)
assert (ast[0] == "main_program")
components = dict()
get_components_dict(ast[1], components)
# # Type checker again
# TypeChecker.get_stream_events(components["stream_type"])
# if "stream_processor" in components.keys():
# 	TypeChecker.get_stream_processors_data(components["stream_processor"])
# TypeChecker.check_event_sources_types(ast[PMAIN_PROGRAM_EVENT_SOURCES])
# TypeChecker.check_arbiter(ast[PMAIN_PROGRAM_ARBITER])
# TypeChecker.check_monitor(ast[PMAIN_PROGRAM_MONITOR])
#
# Produce C file

#
streams_to_events_map = get_stream_to_events_mapping(components["stream_type"], TypeChecker.stream_processors_data)

stream_types: Dict[str, Tuple[str, str]] = get_stream_types(components["event_source"])
arbiter_event_source = get_arbiter_event_source(ast[2])
existing_buffers = get_existing_buffers(TypeChecker)

TypeChecker.arbiter_output_type = arbiter_event_source

if args.out is None:
	print("provide the path of the file where the c program must be written.")
	exit(1)
output_path = args.out

if args.with_tessla:
	if args.dir is None:
		print("ERROR: Must provide the directory path where Tessla files are located")
		exit(1)
	if not os.path.isdir(args.dir):
		raise Exception(f"{output_path} directory does not exist!")

	update_toml(args.dir)

	# BEGIN writing c-file interface
	c_interface = get_c_interface(components, ast, streams_to_events_map, stream_types, arbiter_event_source,
								  existing_buffers)
	file = open(output_path, "w")
	file.write(c_interface)
	file.close()

	# BEGIN writing rust file
	program = get_rust_file(streams_to_events_map, arbiter_event_source)
	file = open(f"{args.dir}/src/monitor.rs", "r")
	lines = file.readlines()
	file.close()

	file = open(f"{args.dir}/src/monitor.rs", "w")
	extern_keyword_found = False
	is_there_prev_compilation = False
	for line in lines:
		if "#[no_mangle]" in line:
			print("Code from previous compilation found. Removing it...")
			is_there_prev_compilation = True
			break

	for line in lines:
		file.write(line)
		if (not is_there_prev_compilation) and ("extern crate tessla_stdlib;" in line):
			assert(not extern_keyword_found)
			extern_keyword_found = True
			file.write("use std::os::raw::c_int;\n")
			file.write("use std::os::raw::c_long;\n")
		if "#[no_mangle]" in line:
			break
	file.write(program)
	file.close()
	print("DO NOT FORGET to add target/debug/libmonitor.a to the build file of your monitor")
else:

	program = get_c_program(components, ast, streams_to_events_map, stream_types, arbiter_event_source,
							existing_buffers)
	output_file = open(output_path, "w")
	output_file.write(program)
