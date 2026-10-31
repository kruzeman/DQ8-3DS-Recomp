#!/usr/bin/env python3
"""Pair each transfer: which (src,dst,dims) combos exist and when they occur."""
import re
import sys
from collections import Counter

path = sys.argv[1]
lines = open(path, errors='replace').read().splitlines()
pat = re.compile(r'GSP display-transfer dump: src=([0-9A-F]+) dst=([0-9A-F]+) (\d+)x(\d+)')
idx = 0
combos = Counter()
seq = []
for n, l in enumerate(lines, 1):
    m = pat.search(l)
    if m:
        idx += 1
        key = (m.group(1), m.group(2), m.group(3), m.group(4))
        combos[key] += 1
        seq.append((idx, key))
for k, v in combos.most_common():
    print(v, k)
print('--- transitions (combo changes) ---')
prev = None
for i, k in seq:
    if k != prev:
        print(f'#{i:4d} -> {k}')
        prev = k
