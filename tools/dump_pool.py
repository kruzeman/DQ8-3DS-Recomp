#!/usr/bin/env python3
"""Dump aligned words around a literal-pool address and report root gaps.
usage: tools/dump_pool.py 0xADDR [span]"""
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

addr = int(sys.argv[1], 0)
span = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x80

def gw(a):
    o = a - BASE
    if o < 0 or o + 4 > len(code):
        return None
    return struct.unpack_from('<I', code, o)[0]

missing = []
for a in range(addr - span, addr + span, 4):
    w = gw(a)
    if w is None:
        continue
    if a == addr:
        mark = ' <== pool xref'
    else:
        mark = ''
    codey = 0x00100000 <= w < 0x0053BCB4 and w % 4 == 0
    if codey:
        mark += '  [rooted]' if w in roots else '  [CODE NOT ROOTED]'
        if w not in roots:
            missing.append(w)
    print('%08X: %08X%s' % (a, w, mark))
print('')
print('missing code entries: %d' % len(missing))
if missing:
    print('MISSING: ' + ' '.join('0x%08X' % m for m in missing))
