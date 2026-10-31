#!/usr/bin/env python3
# List every 4-byte-aligned literal in DQ8.code whose value lies in a given
# guest address range (default the 0x0060xxxx BSS window).
# Usage: python3 tools/scan_lits.py [lo_hex] [hi_hex]
import struct
import sys

CODE = "/home/hermes/DQ8/DQ8.code"
BASE = 0x00100000
lo = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x00600000
hi = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x00610000

with open(CODE, "rb") as fh:
    data = fh.read()

hits = []
for i in range(0, len(data) - 3, 4):
    w = struct.unpack_from("<I", data, i)[0]
    if lo <= w < hi:
        hits.append((BASE + i, w))
print("%d literal(s) in [0x%08X, 0x%08X)" % (len(hits), lo, hi))
for a, w in hits:
    print("  @%08X = 0x%08X" % (a, w))
