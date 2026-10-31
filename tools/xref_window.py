#!/usr/bin/env python3
"""Scan DQ8.code for any word (aligned or not) that points into an address
window, so we can tell whether a code region is referenced statically at all.

usage: python3 tools/xref_window.py 0x00101900 0x00101980 [0x00101F00 0x00101F80]...
"""
import struct
import sys

CODE = "/home/hermes/DQ8/DQ8.code"
BASE = 0x00100000
args = [int(a, 16) for a in sys.argv[1:]]
windows = list(zip(args[0::2], args[1::2]))

data = open(CODE, "rb").read()
n = len(data)

for lo, hi in windows:
    found = []
    for off in range(0, n - 3):
        w = struct.unpack_from("<I", data, off)[0]
        if lo <= w < hi:
            found.append((BASE + off, w, "aligned" if (BASE + off) % 4 == 0 else "UNALIGNED"))
    print("window 0x%08X..0x%08X: %d pointer word(s)" % (lo, hi, len(found)))
    for a, v, kind in found[:40]:
        print("   at 0x%08X -> 0x%08X (%s)" % (a, v, kind))
