#!/usr/bin/env python3
"""One-line stats for a directory of PPM frames: name, nonzero bytes, max."""
import glob
import os
import sys

d = sys.argv[1]
pat = sys.argv[2] if len(sys.argv) > 2 else "*.ppm"
for f in sorted(glob.glob(os.path.join(d, pat))):
    if f.endswith(".bot.ppm"):
        continue
    data = open(f, "rb").read()
    try:
        idx = data.index(b"255\n") + 4
    except ValueError:
        print(f, "bad header")
        continue
    px = data[idx:]
    nz = sum(1 for i in range(0, len(px), 3)
             if px[i] | px[i + 1] | px[i + 2])
    print(os.path.basename(f), "nonzero_px=%d" % nz, "max=%d" % max(px))
