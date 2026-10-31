#!/usr/bin/env python3
# Find ARM instructions that access a given immediate offset (e.g. 0x3C) on a
# register: str/strb/ldr/ldrb (immediate, pre or post index).
# Usage: python3 tools/scan_offset.py 0x3C [start] [end]
import struct
import sys

CODE = "/home/hermes/DQ8/DQ8.code"
BASE = 0x00100000

with open(CODE, "rb") as fh:
    data = fh.read()

offset = int(sys.argv[1], 16) & 0xFFF
start = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x00100000
end = int(sys.argv[3], 16) if len(sys.argv) > 3 else BASE + len(data)

hits = []
for i in range(0, len(data) - 4, 4):
    addr = BASE + i
    if addr < start or addr > end:
        continue
    w = struct.unpack_from("<I", data, i)[0]
    if (w & 0x0FF00000) in (0x05800000, 0x05900000, 0x04800000, 0x04900000,
                            0x05C00000, 0x05D00000, 0x04C00000, 0x04D00000):
        if (w & 0xFFF) == offset:
            hits.append((addr, w))
print("%d access(es) to offset 0x%X" % (len(hits), offset))
for addr, w in hits:
    print("  %08X  %08X" % (addr, w))
