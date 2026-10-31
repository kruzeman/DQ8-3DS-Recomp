#!/usr/bin/env python3
"""Extract GSP display-transfer dump lines with index -> correlate with time."""
import re
import sys

path = sys.argv[1]
lines = open(path, errors='replace').read().splitlines()
pat = re.compile(r'GSP display-transfer dump: src=([0-9A-F]+) dst=([0-9A-F]+) (\d+)x(\d+)')
idx = 0
seq = []
for n, l in enumerate(lines, 1):
    m = pat.search(l)
    if m:
        idx += 1
        seq.append((idx, n, m.group(1), m.group(2), m.group(3), m.group(4)))

print(f'total transfers: {len(seq)}')
# summarize by dst address in blocks of 25 transfers
from collections import Counter
c = Counter(s[3] for s in seq)
print('dst addresses:', dict(c))
c2 = Counter(s[2] for s in seq)
print('src addresses:', dict(c2))
c3 = Counter((s[4], s[5]) for s in seq)
print('dims:', dict(c3))
# print first/last transfer index for each dst
first = {}
last = {}
for s in seq:
    d = s[3]
    first.setdefault(d, s[0])
    last[d] = s[0]
for d in first:
    print(f'dst {d}: first #{first[d]} last #{last[d]}')
# print a window of lines around the transitions: every 40th
print('--- sample (every 40) ---')
for s in seq[::40]:
    print(f'#{s[0]:4d} logline {s[1]:6d} src={s[2]} dst={s[3]} {s[4]}x{s[5]}')
