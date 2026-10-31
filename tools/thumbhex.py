#!/usr/bin/env python3
# thumbhex.py GUEST_ADDR LEN_BYTES — print hex bytes of DQ8.code at guest addr.
import sys

addr = int(sys.argv[1], 16)
n = int(sys.argv[2])
with open('/home/hermes/DQ8/DQ8.code', 'rb') as f:
    f.seek(addr - 0x00100000)
    b = f.read(n)
print(" ".join("%02x" % x for x in b))
