#!/usr/bin/env python3
# Dump N hex words starting at a guest address.
# Usage: python3 tools/dump_words.py <addr_hex> [count] [step]
import struct
import sys

CODE = "/home/hermes/DQ8/DQ8.code"
BASE = 0x00100000
addr = int(sys.argv[1], 16)
count = int(sys.argv[2]) if len(sys.argv) > 2 else 16
step = int(sys.argv[3]) if len(sys.argv) > 3 else 4

with open(CODE, "rb") as fh:
    data = fh.read()

for a in range(addr, addr + count * step, step):
    i = a - BASE
    if 0 <= i <= len(data) - 4:
        w = struct.unpack_from("<I", data, i)[0]
        print("@0x%08X = 0x%08X" % (a, w))
