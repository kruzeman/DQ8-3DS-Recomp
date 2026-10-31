#!/usr/bin/env python3
"""Find Thumb BL/BLX-immediate call sites whose target equals a given address.

The round-12 escape chain fires from ONE place: the Thumb for-each dispatcher at
0x00100C8C (`push {r0-r7,lr}; r4=count; r6=cb; r7=stride; ... blx r6`), reached
through the ARM->Thumb veneer at 0x00100C84.  Enumerating every caller of that
dispatcher is how you get the whole callback family instead of playing
whack-a-mole one escape at a time.

Usage: python3 tools/find_thumb_callers.py 0x00100C8C [scan_lo scan_hi]
"""
import struct
import sys

CODE = "/home/hermes/DQ8/DQ8.code"
BASE = 0x00100000

target = int(sys.argv[1], 16)
slo = int(sys.argv[2], 16) if len(sys.argv) > 2 else BASE
shi = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x00600000

with open(CODE, "rb") as fh:
    data = fh.read()

hits = []
total = 0
for off in range(0, len(data) - 4, 2):
    addr = BASE + off
    if not (slo <= addr < shi):
        continue
    hw1, hw2 = struct.unpack_from("<HH", data, off)
    if hw1 & 0xF800 != 0xF000:          # 11110 S imm10
        continue
    if hw2 & 0xD000 != 0xD000:          # 11 J1 x J2 imm11  (x=1 -> BL)
        continue
    if not (hw2 & 0x1000):
        continue                         # BLX has bit12 clear (targets ARM)
    S = (hw1 >> 10) & 1
    imm10 = hw1 & 0x3FF
    J1 = (hw2 >> 13) & 1
    J2 = (hw2 >> 11) & 1
    imm11 = hw2 & 0x7FF
    I1 = (~(J1 ^ S)) & 1
    I2 = (~(J2 ^ S)) & 1
    bits = (S << 24) | (I1 << 23) | (I2 << 22) | (imm10 << 12) | (imm11 << 1)
    if S:
        bits -= 1 << 25
    t = addr + 4 + bits
    total += 1
    if t == target:
        hits.append(addr)

print("%d Thumb BL sites decoded in total; %d -> 0x%08X" % (total, len(hits), target))
for a in hits:
    print("  0x%08X" % a)
