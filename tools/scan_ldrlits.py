#!/usr/bin/env python3
# Find pc-relative LDR instructions whose literal address falls in a range.
# Usage: python3 tools/scan_ldrlits.py <lo_hex> <hi_hex>
import struct
import sys

CODE = "/home/hermes/DQ8/DQ8.code"
BASE = 0x00100000
lo = int(sys.argv[1], 16)
hi = int(sys.argv[2], 16)

with open(CODE, "rb") as fh:
    data = fh.read()

hits = []
for off in range(0, len(data) - 3, 4):
    w = struct.unpack_from("<I", data, off)[0]
    # ldr rd, [pc, #imm12] (U bit set or clear), rn == 15
    if (w & 0x0F7F0000) == 0x051F0000:
        add = (w >> 23) & 1
        imm12 = w & 0xFFF
        addr = BASE + off
        lit = addr + 8 + imm12 if add else addr + 8 - imm12
        if lo <= lit < hi:
            rd = (w >> 12) & 0xF
            hits.append((addr, rd, lit))

print("%d ldr site(s) -> literal in [0x%08X, 0x%08X)" % (len(hits), lo, hi))
for a, rd, lit in hits:
    print("  0x%08X: ldr r%d, [pc, #...] -> lit 0x%08X" % (a, rd, lit))
