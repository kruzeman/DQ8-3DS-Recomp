#!/usr/bin/env python3
"""t_a22413f1: align two runs that follow the same content at a constant time
offset, then diff the aligned frames.

usage: holesop_offset.py A B [tmin]
Finds, for each A frame with t >= tmin, the B frame with the smallest mean |d|,
reports the offset histogram and the aligned residual.  A constant offset with a
zero residual means the two builds produced the same pixels at the same guest
moment (i.e. no regression between them).
"""
import sys, os, glob
import numpy as np
from PIL import Image


def load(d):
    out = {}
    for f in sorted(x for x in glob.glob(os.path.join(d, "t0*.ppm")) if ".bot." not in x):
        if os.path.getsize(f) < 288015:
            continue
        t = int(os.path.basename(f)[1:5])
        out[t] = np.asarray(Image.open(f).convert("RGB")).astype(np.int16)
    return out


A, B = load(sys.argv[1]), load(sys.argv[2])
tmin = int(sys.argv[3]) if len(sys.argv) > 3 else 0
offsets = {}
zero = 0
tot = 0
for t, a in sorted(A.items()):
    if t < tmin:
        continue
    best = None
    for u, b in B.items():
        if b.shape != a.shape:
            continue
        dm = float(np.abs(a - b).mean())
        if best is None or dm < best[0]:
            best = (dm, u)
    if best is None:
        continue
    tot += 1
    offsets[best[1] - t] = offsets.get(best[1] - t, 0) + 1
    if best[0] == 0.0:
        zero += 1
print("A=%s B=%s  t>=%d" % (sys.argv[1], sys.argv[2], tmin))
print("  frames compared: %d" % tot)
print("  best-match offset histogram (B_t - A_t): %s"
      % sorted(offsets.items(), key=lambda kv: -kv[1])[:6])
print("  byte-identical aligned frames: %d/%d" % (zero, tot))
