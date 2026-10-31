#!/usr/bin/env python3
"""Scan a pointer-table region for code-like words that are NOT in recomp-roots.
usage: tools/scan_table_roots.py 0xSTART 0xEND"""
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

def gw(a):
    o = a - BASE
    if o < 0 or o + 4 > len(code):
        return None
    return struct.unpack_from('<I', code, o)[0]

missing = {}
rooted = 0
total_code = 0
for a in range(start & ~3, end, 4):
    w = gw(a)
    if w is None:
        continue
    if 0x00100000 <= w < 0x005FA000 and w % 4 == 0:
        total_code += 1
        if w in roots:
            rooted += 1
        else:
            missing.setdefault(w, []).append(a)

print('region 0x%08X..0x%08X: code ptrs=%d rooted=%d missing=%d' %
      (start, end, total_code, rooted, len(missing)))
for w in sorted(missing):
    locs = missing[w]
    print('  MISSING 0x%08X  at %s' % (w, ','.join('0x%X' % l for l in locs[:6])))
