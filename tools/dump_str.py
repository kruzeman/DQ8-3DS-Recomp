#!/usr/bin/env python3
# Print a NUL-terminated ASCII string at a guest address.
# Usage: python3 tools/dump_str.py <addr_hex> [max_len]
import sys

CODE = "/home/hermes/DQ8/DQ8.code"
BASE = 0x00100000
addr = int(sys.argv[1], 16)
max_len = int(sys.argv[2]) if len(sys.argv) > 2 else 64

with open(CODE, "rb") as fh:
    data = fh.read()

i = addr - BASE
out = []
for j in range(max_len):
    c = data[i + j]
    if c == 0:
        break
    out.append(chr(c) if 32 <= c < 127 else "\\x%02X" % c)
print("0x%08X: %r" % (addr, "".join(out)))
