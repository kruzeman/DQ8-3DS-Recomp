#!/usr/bin/env python3
"""Per-frame color signature scan: report mean R/G/B and a coarse
'classification' hint for each top-screen PPM in a frames dir."""
import glob, os, sys

d = sys.argv[1]
files = sorted(glob.glob(os.path.join(d, "t*.ppm")))
files = [f for f in files if "bottom" not in f and "press" not in f]
for f in files:
    data = open(f, "rb").read()
    try:
        idx = data.index(b"255\n") + 4
    except ValueError:
        continue
    px = data[idx:]
    n = len(px) // 3
    if n == 0:
        continue
    r = sum(px[0::3]) / n
    g = sum(px[1::3]) / n
    b = sum(px[2::3]) / n
    name = os.path.basename(f)[1:5]
    print("t=%s R=%5.1f G=%5.1f B=%5.1f" % (name, r, g, b))
