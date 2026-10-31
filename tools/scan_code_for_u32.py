#!/usr/bin/env python3
"""t_41f38df0: locate 32-bit literals in the guest code image.

usage: scan_code_for_u32.py <hex> [<hex> ...]
"""
import struct
import sys

CODE = "/home/hermes/DQ8/DQ8.code"
TEXT_BASE = 0x00100000

blob = open(CODE, "rb").read()
for h in sys.argv[1:]:
    v = int(h, 16)
    pat = struct.pack("<I", v)
    hits = []
    start = 0
    while True:
        i = blob.find(pat, start)
        if i < 0:
            break
        hits.append(i)
        start = i + 1
    print(f"{h}: {len(hits)} hit(s)" +
          ("" if not hits else " -> " + ", ".join(f"{TEXT_BASE + x:#010x}" for x in hits[:16])))
