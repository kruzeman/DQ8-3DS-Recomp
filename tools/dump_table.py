#!/usr/bin/env python3
"""Dump a pointer-table window around an address and report root coverage.
usage: tools/dump_table.py 0xADDR [span_bytes] [stride]"""
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

def gw(a):
    o = a - BASE
    if o < 0 or o + 4 > len(code):
        return None
    return struct.unpack_from('<I', code, o)[0]

addr = int(sys.argv[1], 0)
span = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x100
stride = int(sys.argv[3], 0) if len(sys.argv) > 3 else 4

start = addr - (addr % stride) - (span // 2)
missing = []
seen = []
for a in range(start, start + span, stride):
    w = gw(a)
    if w is None:
        continue
    inroots = w in roots
    looks_code = 0x00100000 <= w < 0x005FA000 and w % 4 == 0
    flag = ''
    if a <= addr < a + stride:
        flag += ' <== probe'
    if looks_code and not inroots:
        flag += '  [CODE NOT ROOTED]'
        missing.append(w)
    if looks_code and inroots:
        flag += '  [rooted]'
    if w == 0:
        continue
    seen.append(w)
    print('%08X: %08X%s' % (a, w, flag))

print('')
print('code-like entries: %d, missing roots: %d' % (
    sum(1 for w in seen if 0x00100000 <= w < 0x005FA000 and w % 4 == 0),
    len(missing)))
if missing:
    print('MISSING: ' + ' '.join('0x%08X' % m for m in missing))
