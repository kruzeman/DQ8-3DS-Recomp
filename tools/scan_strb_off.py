#!/usr/bin/env python3
# Find ARM strb instructions with a given immediate offset, optionally in a code range.
# Usage: python3 tools/scan_strb_off.py <offset_hex> [scan_lo_hex] [scan_hi_hex]
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
    if addr < slo or addr >= shi:
        continue
    w = struct.unpack_from("<I", data, off)[0]
    # strb immediate pre-indexed, no writeback: cond 0101 U100 imm12 (L=0, B=1, P=1, W=0)
    # encoding bits 27-26 = 01, I=0, P=1, B=1, L=0:  bits[27:20] = 0101 U 1 0 0
    top = (w >> 20) & 0xFF
    if top in (0x5C, 0x54):  # U=1 (0x5C) or U=0 (0x54)
        imm12 = w & 0xFFF
        val = imm12 if top == 0x5C else -imm12
        if val == offset:
            rn = (w >> 16) & 0xF
            rd = (w >> 12) & 0xF
            hits.append((addr, rn, rd, "add" if top == 0x5C else "sub"))

print(f"{len(hits)} strb site(s) with offset {offset:#x}")
for addr, rn, rd, sign in hits:
    print(f"  {addr:#010x}: strb r{rd}, [r{rn}, #{sign} #{offset:#x}]")
