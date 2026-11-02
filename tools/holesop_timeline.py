#!/usr/bin/env python3
"""t_a22413f1: brightness/colour timeline of a captured run directory."""
import sys, os, glob
from PIL import Image
import numpy as np


def timeline(d):
    out = []
    for f in sorted(x for x in glob.glob(os.path.join(d, "t0*.ppm")) if ".bot." not in x):
        if os.path.getsize(f) < 288015:
            continue
        a = np.asarray(Image.open(f).convert("RGB")).astype(np.int16)
        out.append((os.path.basename(f)[1:5], float(a.mean()), int(a.max()),
                    len(np.unique(a.reshape(-1, 3), axis=0))))
    return out


for d in sys.argv[1:]:
    tl = timeline(d)
    every = 20 if not os.environ.get("HOLES_ALL") else 1
    print("== %s (%d frames)" % (d, len(tl)))
    prev = None
    for name, mean, mx, uq in tl:
        tag = "BLACK" if mean < 1.0 else ("dim" if mean < 10 else "")
        if int(name) % every == 0 or tag or (prev is not None and abs(mean - prev) > 5):
            print("  t%s mean=%7.2f max=%3d uniq=%6d %s" % (name, mean, mx, uq, tag))
        prev = mean
