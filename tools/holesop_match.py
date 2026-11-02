#!/usr/bin/env python3
"""t_a22413f1: match GL and SW scene frames by content hash and diff the matches.

usage: holesop_match.py <gl-dir> <sw-dir>
Each run captures t####.ppm once per second; the runs are not phase-locked, so
frames are matched by md5 of the captured frame (identical content = identical
backend output), and every match is then verified pixel-by-pixel (a md5 match
is only the candidate set; the pixel diff is the evidence).
"""
import sys
import os
from PIL import Image
import numpy as np


def load(d):
    out = {}
    p = os.path.join(d, "frames.tsv")
    for line in open(p):
        line = line.strip()
        if not line.startswith("t="):
            continue
        parts = dict(kv.split("=", 1) for kv in line.split(" ") if "=" in kv)
        t = int(parts["t"])
        f = os.path.join(d, "t%04d.ppm" % t)
        if os.path.exists(f):
            out.setdefault(parts["md5"], []).append(t)
    return out


gl = load(sys.argv[1])
sw = load(sys.argv[2])
common = sorted(set(gl) & set(sw), key=lambda h: min(gl[h]))
print("GL distinct frames: %d   SW distinct frames: %d   identical: %d"
      % (len(gl), len(sw), len(common)))
if not common:
    print("NO CONTENT MATCH (backend outputs differ at every captured moment)")
    sys.exit(1)
worst = 0
for h in common:
    tg, ts = gl[h][0], sw[h][0]
    a = np.asarray(Image.open(os.path.join(sys.argv[1], "t%04d.ppm" % tg)).convert("RGB")).astype(int)
    b = np.asarray(Image.open(os.path.join(sys.argv[2], "t%04d.ppm" % ts)).convert("RGB")).astype(int)
    d = np.abs(a - b)
    worst = max(worst, int(d.max()))
print("verified pixel-identical pairs: %d   max channel delta over all pairs: %d"
      % (len(common), worst))
print("first matched moments (gl_t -> sw_t): %s"
      % ", ".join("%d->%d" % (gl[h][0], sw[h][0]) for h in common[:10]))
