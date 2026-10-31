#!/usr/bin/env python3
# Scan DQ8.code for absolute references (literal-pool words) to given guest
# addresses and print every hit with context.
# Usage: python3 tools/scan_xrefs.py 0x0060E45C [0x0060E498 ...]
import struct
import sys

CODE = "/home/hermes/DQ8/DQ8.code"
BASE = 0x00100000

with open(CODE, "rb") as fh:
    data = fh.read()

for arg in sys.argv[1:]:
    target = int(arg, 16)
    needle = struct.pack("<I", target)
    hits = []
    off = 0
    while True:
        i = data.find(needle, off)
        if i < 0:
            break
        hits.append(i)
        off = i + 1
    print("=== %s: %d hit(s)" % (arg, len(hits)))
    for i in hits:
        addr = BASE + i
        # context: 6 words before and after
        lo = max(0, i - 24)
        hi = min(len(data), i + 28)
        ctx = []
        for j in range(lo, hi, 4):
            w = struct.unpack_from("<I", data, j)[0]
            mark = "*" if j == i else " "
            ctx.append("%s%08X:%08X" % (mark, BASE + j, w))
        print("  hit @ %08X" % addr)
        print("    " + " ".join(ctx))
