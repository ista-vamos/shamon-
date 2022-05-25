#!/usr/bin/python3

from sys import argv, stderr
from itertools import cycle, accumulate
from time import process_time

if __name__ == "__main__":
    NUM = int(argv[1])
    primes=[]

    start = process_time()
    for i in range(1,3):
        print(f"#{i}: {i+1}")
        if i >= NUM:
            exit(0)

    NUM -= 2
    def iszero(x): return x == 0

    for n in accumulate(cycle((2,4)), initial=5):
        #print(primes, n, list(takewhile(iszero, filter(iszero, map(lambda p: n % p, primes)))))
        if not any(map(lambda p: n % p == 0, primes)):
            print(f"#{len(primes)+3}: {n}")
            primes.append(n)
            if len(primes) >= NUM:
                break

    end = process_time()
    print(f"time: {end - start} seconds.", file=stderr)
  # num = 1
  # for step in cycle((4,2)):
  #     if len(primes) >= NUM:
  #         break
  #     num += step

  #     if isprime(num):
  #         primes.append(num)
  #         print(f"#{len(primes)}: {primes[-1]}")

            
