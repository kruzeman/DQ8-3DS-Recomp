#!/usr/bin/env python3
"""Compare two fn_emit lists (top300 vs pcrel2800)."""
import sys


def load(p):
    s = set()
    for line in open(p):
        line = line.strip()
        if line.startswith("0x"):
            s.add(line.split()[0])
    return s


a = load(sys.argv[1])
b = load(sys.argv[2])
print("a=%d b=%d shared=%d a-only=%d" % (len(a), len(b), len(a & b), len(a - b)))
print(sorted(a - b)[:8])
