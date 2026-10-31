#!/usr/bin/env python3
# Scan DQ8.code for the ARMv6 extend family (SXTB/SXTH/UXTB/UXTH and the
# *B16 and accumulate forms). Encoding: bits[27:24]=0110, bits[23:20] in
# {8,A,B,C,E,F}, bits[7:4]=0111. Reports a histogram of
# (op1=bits[23:20], Rn) pairs, globally and inside a guest-address region.
# Usage: scan_extend.py [START END]
import collections
import sys

with open("/home/hermes/DQ8/DQ8.code", "rb") as fh:
    blob = fh.read()

OP1 = {8: "SXTB16", 10: "SXTB", 11: "SXTH", 12: "UXTB16", 14: "UXTB", 15: "UXTH"}

lo = int(sys.argv[1], 16) if len(sys.argv) > 2 else 0x00100000
hi = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x00100000 + len(blob)

hist = collections.Counter()
for off in range(0, len(blob) - 3, 4):
    addr = 0x00100000 + off
    if not (lo <= addr < hi):
        continue
    w = int.from_bytes(blob[off:off + 4], "little")
    if (w & 0x0F0000F0) != 0x06000070:
        continue
    op1 = (w >> 20) & 15
    if op1 not in OP1:
        continue
    rn = (w >> 16) & 15
    rd = (w >> 12) & 15
    rm = w & 15
    rot = (w >> 10) & 3
    name = OP1[op1] + ("" if rn == 15 else " (acc)")
    hist[(name, rn, rd, rm, rot * 8)] += 1

if not hist:
    print("no extend-family instructions found")
for (name, rn, rd, rm, rot), n in hist.most_common(60):
    print(f"{n:6d}  {name:12s} rn=r{rn} rd=r{rd} rm=r{rm} ror={rot}")
