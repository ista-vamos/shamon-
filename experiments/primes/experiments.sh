#!/bin/bash

set -x
set -e

rm -f /tmp/log.txt

cd $(dirname 0)

SRCDIR="$(dirname $0)/../.."
SHM_BUFSIZE_FILE="${SRCDIR}/shmbuf/buffer-size.h"

#for SHM_BUFSIZE in 1 8 32 64; do
for SHM_BUFSIZE in 2; do
        make clean -j  -C $SRCDIR
        sed -i "s/#define\\s*SHM_BUFFER_SIZE_PAGES.*/#define SHM_BUFFER_SIZE_PAGES $SHM_BUFSIZE/" $SHM_BUFSIZE_FILE
        make -j  -C $SRCDIR
        #for ARBITER_BUFSIZE in 8 16 64 128; do
        for DROP_WAIT in 0.05 0.1 0.2 0.25 0.4 0.5; do
            for ARBITER_BUFSIZE in 1024; do
                    for PRIMES_NUM in 10000 20000 30000 40000; do
                            ./run.sh $SHM_BUFSIZE $ARBITER_BUFSIZE $PRIMES_NUM $DROP_WAIT
                    done
            done
        done
done
