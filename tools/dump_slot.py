#!/usr/bin/env python3
"""Hexdump + float interpretation of one SourceConfiguration slot."""
import struct
import sys

path = sys.argv[1]
slot = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0
with open(path, 'rb') as f:
    blob = f.read()
for name, base in (('R0', 0x50000), ('R1', 0x70000)):
    off = base + 0x800 + slot * 192
    raw = blob[off:off + 192]
    print('--- %s slot%d fileoff=%06X ---' % (name, slot, off))
    for row in range(0, 192, 16):
        chunk = raw[row:row + 16]
        hexs = ' '.join('%02X' % b for b in chunk)
        words = []
        for k in range(0, 16, 4):
            if row + k + 4 <= 192:
                w = struct.unpack_from('<I', raw, row + k)[0]
                fval = struct.unpack_from('<f', raw, row + k)[0]
                words.append('%08X:%.4f' % (w, fval))
        print('+%03X  %s   %s' % (row, hexs, '  '.join(words)))
