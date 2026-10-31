#!/usr/bin/env python3
# Find ARM add/sub-immediate instructions whose decoded immediate equals a value.
# Usage: python3 tools/scan_addimm.py <imm_hex> [scan_lo_hex] [scan_hi_hex]
import struct
import sys

CODE = "/home/hermes/DQ8/DQ8.code"
BASE = 0x00100000

want = int(sys.argv[1], 16)
slo = int(sys.argv[2], 16) if len(sys.argv) > 2 else BASE
shi = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x00600000


def imm_from(w):
    rot = ((w >> 8) & 0xF) * 2
    imm8 = w & 0xFF
    return ((imm8 >> rot) | (imm8 << (32 - rot))) & 0xFFFFFFFF if rot else imm8


with open(CODE, "rb") as fh:
    data = fh.read()

hits = []
for off in range(0, len(data) - 3, 4):
    addr = BASE + off
    if not (slo <= addr < shi):
        continue
    w = struct.unpack_from("<I", data, off)[0]
    if (w >> 28) != 0xE:
        continue
    # add/sub immediate: cond 000/001 S 100 ...
    if (w & 0x0FE00000) in (0x02800000, 0x02400000):
        op = "add" if (w & 0x01E00000) == 0x00400000 else "sub"
        rn = (w >> 16) & 0xF
        rd = (w >> 12) & 0xF
        if imm_from(w) == want:
            hits.append((addr, "%s r%d, r%d, #0x%X" % (op, rd, rn, want)))

print("%d add/sub site(s) with immediate 0x%X" % (len(hits), want))
for a, txt in hits:
    print("  0x%08X: %s" % (a, txt))
