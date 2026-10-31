#!/usr/bin/env python3
"""Bucketed density map of code-like pointer words in a range, plus root gaps.
usage: tools/density_map.py 0xSTART 0xEND [bucket_size]"""
import struct
import sys

CODE = '/home/hermes/DQ8/DQ8.code'
ROOTS = '/home/hermes/DQ8/recomp-roots.txt'
BASE = 0x00100000

code = open(CODE, 'rb').read()
roots = set()
for line in open(ROOTS):
    line = line.strip()
    if line[:2] in ('0x', '0X'):
        try:
            roots.add(int(line, 16))
        except ValueError:
            pass

start = int(sys.argv[1], 0)
end = int(sys.argv[2], 0)
bucket = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x1000

def gw(a):
    o = a - BASE
    if o < 0 or o + 4 > len(code):
        return None
    return struct.unpack_from('<I', code, o)[0]

for b in range(start, end, bucket):
    n = m = 0
    for a in range(b, b + bucket, 4):
        w = gw(a)
        if w is None:
            continue
        if 0x00100000 <= w < 0x0053BCB4 and w % 4 == 0:
            n += 1
            if w not in roots:
                m += 1
    print('%08X: code=%4d missing=%4d%s' % (b, n, m, '  <<<' if m else ''))
