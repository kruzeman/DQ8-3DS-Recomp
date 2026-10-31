#!/usr/bin/env python3
"""Find 32-bit little-endian pointer occurrences of a guest address in DQ8.code.

Usage: scan_ptrs.py 0x002FC018
"""
import struct
import sys

VAL = int(sys.argv[1], 16)
with open('/home/hermes/DQ8/DQ8.code', 'rb') as fh:
    data = fh.read()
pat = struct.pack('<I', VAL)
idxs = []
i = data.find(pat)
while i >= 0:
    idxs.append(i)
    i = data.find(pat, i + 1)
print('pointer occurrences of 0x%08X: %d' % (VAL, len(idxs)))
for i in idxs[:60]:
    print('  guest addr 0x%08X' % (0x00100000 + i))
