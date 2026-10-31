#!/usr/bin/env python3
# Hex-dump a guest-address range from DQ8.code. Usage: hexdump_at.py START LEN
import binascii, sys

start = int(sys.argv[1], 16)
length = int(sys.argv[2], 16) if sys.argv[2].startswith("0x") else int(sys.argv[2])
with open("/home/hermes/DQ8/DQ8.code", "rb") as fh:
    fh.seek(start - 0x00100000)
    data = fh.read(length)
for i in range(0, len(data), 16):
    print("%08X  %s" % (start + i, binascii.hexlify(data[i:i + 16], " ").decode()))
