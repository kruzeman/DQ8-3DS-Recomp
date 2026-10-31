#!/usr/bin/env python3
# Find ARM data-processing instructions with a given immediate (e.g. 0x3C):
# add/sub/adc/sbc/rsb with imm12 == value (address formation is a strong hint).
# Usage: python3 tools/scan_dpimm.py 0x3C [opcodes]   opcodes: add,sub,rsb,adc,sbc
import struct
import sys

CODE = "/home/hermes/DQ8/DQ8.code"
BASE = 0x00100000

OPS = {"and": 0x000, "eor": 0x020, "sub": 0x040, "rsb": 0x060, "add": 0x080,
       "adc": 0x0A0, "sbc": 0x0C0, "rsc": 0x0E0, "orr": 0x180, "mov": 0x1A0,
       "bic": 0x1C0, "mvn": 0x1E0}

with open(CODE, "rb") as fh:
    data = fh.read()

value = int(sys.argv[1], 16) & 0xFFF
names = [n.strip() for n in sys.argv[2].split(",")] if len(sys.argv) > 2 else ["add", "sub", "rsb", "adc", "sbc"]
wanted = [OPS[n] for n in names]

hits = []
for i in range(0, len(data) - 4, 4):
    w = struct.unpack_from("<I", data, i)[0]
    if (w & 0x0FF00000) == 0x02000000 and (w & 0xFFF) == value and (w & 0x01F00000) in wanted:
        hits.append((BASE + i, w))
print("%d dp-imm hit(s) for 0x%X %s" % (len(hits), value, ",".join(names)))
for addr, w in hits:
    print("  %08X  %08X" % (addr, w))
