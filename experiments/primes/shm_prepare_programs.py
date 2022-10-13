import os
from subprocess import run, TimeoutExpired

# generate shamon programs with different buffer size
for buffsize in [4, 16, 128, 1024]:
    for param2 in [0.05, 0.1, 0.2, 0.25, 0.4, 0.5]:
        file = open("./primes.txt.in", "r")
        outfile = open(f"./programs/primes_{buffsize}_{param2}.txt", "w")
        for line in file:
            if "@BUFSIZE" in line:
                line = line.replace("@BUFSIZE", str(buffsize))
            if "@PARAM2" in line:
                line = line.replace("@PARAM2", str(param2*buffsize) )
            outfile.write(line)
        outfile.close()
        file.close()

CURRENT_PATH = os.getcwd()
assert("/".join(CURRENT_PATH.split("/")[-3:]) == 'shamon/experiments/primes') # assert we are currently on shamon/experiments/primes
COMPILER_PATH = f"{CURRENT_PATH}/../../compiler/main.py"

# compile shamon programs into c programs
for buffsize in [4, 16, 128, 1024]:
    for param2 in [0.05, 0.1, 0.2, 0.25, 0.4, 0.5]:
        run(["python3", COMPILER_PATH, f"{CURRENT_PATH}/programs/primes_{buffsize}_{param2}.txt", f"{CURRENT_PATH}/programs/monitor_{buffsize}_{param2}.c"], check=True)


# generate oject file of intmap
run(["g++", "-c", f"{CURRENT_PATH}/../../compiler/cfiles/intmap.cpp" ], check=True)

COMPILE_SCRIPT= f"{CURRENT_PATH}/../../gen/compile_primes6.sh"
for buffsize in [4, 16, 128, 1024]:
    for param2 in [0.05, 0.1, 0.2, 0.25, 0.4, 0.5]:
        # compile c files
        run(["bash", COMPILE_SCRIPT, f"{CURRENT_PATH}/programs/monitor_{buffsize}_{param2}.c" ], check=True)

    # move exec to /primes/programs
    run(["mv", "monitor", f"{CURRENT_PATH}/programs/monitor{buffsize}_{param2}" ], check=True)


# compile empty monitor

COMPILE_SCRIPT= f"{CURRENT_PATH}/../../gen/compile.sh"
for buffsize in [4, 16, 128, 1024]:
    # compile c files
    new_env = os.environ.copy()
    new_env["ARBITER_BUFSIZE"] = str(buffsize)
    run(["bash", COMPILE_SCRIPT, f"{CURRENT_PATH}/../../mmtest/mmempty.c"],
        check=True, env=new_env)

    # move exec to /primes/programs
    run(["mv", "monitor", f"{CURRENT_PATH}/programs/empty_monitor{buffsize}" ], check=True)


