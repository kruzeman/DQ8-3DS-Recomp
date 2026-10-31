#!/usr/bin/env python3
# Find ARM str/strd instructions with a given immediate offset (imm12 for str,
# imm8*4 for strd), optionally filtered to a code range.
# Usage: python3 tools/scan_store_off.py <offset_hex> [scan_lo_hex] [scan_hi_hex]
import struct
import sys

CODE = "/home/hermes/DQ8/DQ8.code"
BASE = 0x00100000

offset = int(sys.argv[1], 16)
slo = int(sys.argv[2], 16) if len(sys.argv) > 2 else BASE
shi = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x00600000

with open(CODE, "rb") as fh:
    data = fh.read()

hits = []
for off in range(0, len(data) - 3, 4):
    addr = BASE + off
    if not (slo <= addr < shi):
        continue
    w = struct.unpack_from("<I", data, off)[0]
    cond = w >> 28
    if cond != 0xE:
        continue
    # str/ldr immediate: cond 010 P U 0 W L rn rd imm12
    if (w & 0x0E500000) == 0x04000000:
        L = (w >> 20) & 1
        imm12 = w & 0xFFF
        if not L and imm12 == offset:
            rn = (w >> 16) & 0xF
            rd = (w >> 12) & 0xF
            hits.append((addr, "str r%d, [r%d, #0x%X]" % (rd, rn, imm12)))
    # strd/ldrd immediate (P=1,U=1,I=1): cond 000 P U 1 W 0 rn rd imm8
    if (w & 0x0E500090) == 0x04100010:
        L = (w >> 20) & 1
        imm8 = (w & 0xF0) >> 4 | (w & 0xF)  # imm8 split oddly; use standard decode
        # standard: imm8 = (w & 0xF0) | ((w >> 4) & 0xF)? use bits [11:8]|[3:0]?
        # ARM strd imm8 = (w & 0xF0) | ((w >> 4) & 0xF) is wrong; correct:
        # imm8 = (w & 0xF0) | ((w >> 4) & 0xF)?? Use: imm8 = (w & 0xF0) | ((w >> 4) & 0xF)
        # Actual encoding: imm8 = (bits 11:8)(bits 3:0) — no: bits[7:4]=imm8 high? Simplify:
        imm8 = ((w >> 4) & 0xF0) | (w & 0xF)
        if not L and imm8 * 4 == offset:
            rn = (w >> 16) & 0xF
            rd = (w >> 12) & 0xF
            hits.append((addr, "strd r%d, r%d, [r%d, #0x%X]" % (rd, rd + 1, rn, imm8 * 4)))

print("%d store site(s) with offset 0x%X" % (len(hits), offset))
for a, txt in hits:
    print("  0x%08X: %s" % (a, txt))
