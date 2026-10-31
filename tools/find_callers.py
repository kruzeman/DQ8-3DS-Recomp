#!/usr/bin/env python3
# Find every ARM BL/BLX-imm call site whose target lies in [lo, hi).
# Usage: python3 tools/find_callers.py <lo_hex> <hi_hex> [scan_lo_hex] [scan_hi_hex]
import struct
import sys

CODE = "/home/hermes/DQ8/DQ8.code"
BASE = 0x00100000

lo = int(sys.argv[1], 16)
hi = int(sys.argv[2], 16)
slo = int(sys.argv[3], 16) if len(sys.argv) > 3 else BASE
shi = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0x00600000

with open(CODE, "rb") as fh:
    data = fh.read()

hits = []
for off in range(0, len(data) - 3, 4):
    addr = BASE + off
    if not (slo <= addr < shi):
        continue
    w = struct.unpack_from("<I", data, off)[0]
    if (w & 0x0F000000) == 0x0B000000:  # BL
        imm24 = w & 0x00FFFFFF
        if imm24 & 0x800000:
            imm24 -= 0x1000000
        t = addr + 8 + (imm24 << 2)
        if lo <= t < hi:
            hits.append((addr, t))

print("%d BL site(s) -> [0x%08X, 0x%08X)" % (len(hits), lo, hi))
for a, t in hits:
    print("  0x%08X -> 0x%08X" % (a, t))
