#!/usr/bin/env python3
# Scan DQ8.code for ARMv6 parallel (SIMD) arithmetic encodings and report a
# histogram of (op1=bits[23:20], op2=bits[7:5]) pairs, globally and inside a
# guest-address region. Usage: scan_parallel.py [START END]
import collections
import sys

with open("/home/hermes/DQ8/DQ8.code", "rb") as fh:
    blob = fh.read()

OP1 = {1: "S", 2: "Q", 3: "SH", 5: "U", 6: "UQ", 7: "UH", 8: "SEL"}
OP2 = {0: "16add", 1: "16asx", 2: "16sax", 3: "16sub",
       4: "8add", 5: "SEL", 6: "8asx", 7: "8sub"}

lo = int(sys.argv[1], 16) if len(sys.argv) > 2 else 0x00100000
hi = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x00100000 + len(blob)

hist = collections.Counter()
for off in range(0, len(blob) - 3, 4):
    addr = 0x00100000 + off
    w = int.from_bytes(blob[off:off + 4], "little")
    if (w >> 28) != 0xE:
        continue
    if (w & 0x0C000000) != 0x04000000:
        continue
    if w & 0x03000000 != 0x02000000:
        continue
    if (w & 0x00000F10) != 0x00000F10:
        continue
    op1 = (w >> 20) & 15
    op2 = (w >> 5) & 7
    if op1 not in OP1 or op2 not in OP2:
        continue
    if op1 == 8 and op2 != 5:
        continue
    if op1 != 8 and op2 == 5:
        continue
    name = "sel" if op1 == 8 else "%s%s" % (OP1[op1], OP2[op2])
    hist[name] += 1
    if lo <= addr < hi:
        hist[("_region_" + name)] += 1

for name, count in sorted(hist.items()):
    print("%-20s %d" % (name, count))
