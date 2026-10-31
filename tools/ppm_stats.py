#!/usr/bin/env python3
"""Report pixel stats for PPM files passed as arguments."""
import sys

for path in sys.argv[1:]:
    d = open(path, "rb").read()
    try:
        i = d.index(b"255\n") + 4
    except ValueError:
        print(path, "unparsed header")
        continue
    px = d[i:]
    nz = sum(1 for b in px if b > 0)
    print(path, "len", len(px), "max", max(px), "nonzero", nz)
