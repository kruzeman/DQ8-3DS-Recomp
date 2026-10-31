#!/usr/bin/env python3
# Find B/BL branches AND literal-pool pointers to given target addresses.
# Usage: python3 tools/scan_refs.py 0x002FA2EC 0x002FBBE0 ...
import struct
import sys

CODE = "/home/hermes/DQ8/DQ8.code"
BASE = 0x00100000
targets = {int(t, 16) for t in sys.argv[1:]}

with open(CODE, "rb") as fh:
    data = fh.read()

print("--- branches ---")
for off in range(0, len(data) - 3, 4):
    w = struct.unpack_from("<I", data, off)[0]
    if (w & 0x0F000000) in (0x0A000000, 0x0B000000):
        imm24 = w & 0xFFFFFF
        if imm24 & 0x800000:
            imm24 -= 0x1000000
        addr = BASE + off
        t = addr + 8 + (imm24 << 2)
        if t in targets:
            print("0x%08X: %s -> 0x%08X" % (addr, "BL" if w & 0x01000000 else "B", t))

print("--- literal pools / data pointers ---")
for off in range(0, len(data) - 3, 4):
    w = struct.unpack_from("<I", data, off)[0]
    if w in targets:
        print("@0x%08X = 0x%08X" % (BASE + off, w))
