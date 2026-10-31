#!/usr/bin/env python3
# xref_bl.py TARGET — find ARM BL/BLX instructions in DQ8.code targeting TARGET.
import struct
import sys

BASE = 0x00100000
target = int(sys.argv[1], 16)
with open('/home/hermes/DQ8/DQ8.code', 'rb') as f:
    code = f.read()
hits = []
for off in range(0, len(code) - 4, 4):
    w = struct.unpack_from('<I', code, off)[0]
    if (w & 0x0E000000) == 0x0A000000 or (w & 0x0F000000) == 0x0B000000:
        imm = w & 0x00FFFFFF
        if imm & 0x800000:
            imm -= 0x1000000
        dest = off + BASE + 8 + (imm << 2)
        if dest == target:
            hits.append((off + BASE, w))
print("BL/BLX xrefs to 0x%08X: %d" % (target, len(hits)))
for a, w in hits[:30]:
    print("  0x%08X  raw=0x%08X" % (a, w))
