#!/bin/bash

REP=10
export PYTHON_OPTIMIZE=1
export PYTHONDONTWRITEBYTECODE=1

for i in `seq 1 $REP`; do
        ./run-c.py $@
done

for i in `seq 1 $REP`; do
        ./run-cpy.py $@
done

for i in `seq 1 $REP`; do
        ./run-py.py $@
done
