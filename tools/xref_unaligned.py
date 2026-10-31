#!/usr/bin/env python3
"""Unaligned-capable xref scan for a word value in DQ8.code (round-9 method).

Round-9 proved 0x005CB6E0 had zero unaligned word xrefs before concluding it was
not code; apply the same test to 0x00101942/0x00101943 (the esc22 escape target).
Also reports Thumb/ARM branch-instruction encodings that land on the address.
"""
import struct
import sys

CODE = "/home/hermes/DQ8/DQ8.code"
BASE = 0x00100000
targets = [int(t, 16) for t in sys.argv[1:]]

data = open(CODE, "rb").read()
n = len(data)

for t in targets:
    needle = struct.pack("<I", t)
    hits = []
    pos = 0
    while True:
        pos = data.find(needle, pos)
        if pos < 0:
            break
        hits.append(BASE + pos)
        pos += 1
    # ARM B/BL/BLX immediate reaching t
    arm = []
    for off in range(0, n - 3, 4):
        w = struct.unpack_from("<I", data, off)[0]
        if (w & 0x0E000000) == 0x0A000000 or (w & 0xFE000000) in (0xFA000000, 0xFB000000):
            imm24 = w & 0x00FFFFFF
            if imm24 & 0x800000:
                imm24 -= 0x1000000
            if BASE + off + 8 + (imm24 << 2) == t:
                arm.append(BASE + off)
    # Thumb B/BL (16-bit and 32-bit) reaching t, plus literal pool coverage
    thumb = []
    for off in range(0, n - 4, 2):
        hw1, hw2 = struct.unpack_from("<HH", data, off)
        # 16-bit B (T2): 11100 imm11
        if hw1 & 0xF800 == 0xE000:
            imm11 = hw1 & 0x7FF
            if imm11 & 0x400:
                imm11 -= 0x800
            if BASE + off + 4 + (imm11 << 1) == t:
                thumb.append(BASE + off)
        # 32-bit BL pair
        if hw1 & 0xF800 == 0xF000 and hw2 & 0xD000 == 0xD000 and (hw2 & 0x1000):
            S = (hw1 >> 10) & 1
            I1 = (~((hw2 >> 13) & 1 ^ S)) & 1
            I2 = (~((hw2 >> 11) & 1 ^ S)) & 1
            bits = (S << 24) | (I1 << 23) | (I2 << 22) | ((hw1 & 0x3FF) << 12) | ((hw2 & 0x7FF) << 1)
            if S:
                bits -= 1 << 25
            if BASE + off + 4 + bits == t:
                thumb.append(BASE + off)

    print("0x%08X: unaligned word xrefs=%d %s | ARM branch sites=%d %s | Thumb branch sites=%d %s"
          % (t, len(hits), ["0x%08X" % h for h in hits[:8]],
             len(arm), ["0x%08X" % a for a in arm[:8]],
             len(thumb), ["0x%08X" % x for x in thumb[:8]]))
