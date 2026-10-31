#!/usr/bin/env python3
"""t_41f38df0: row-band brightness profile of probe frames.

Prints, per frame, the fraction of non-black pixels in 8 horizontal bands, so
the black lower band (missing ground/character?) can be tracked across the run
without eyeballing every frame.

usage: band_profile.py <frames_dir> <t0> <t1> [<step>]
"""
import os
import sys
from PIL import Image

d = sys.argv[1]
t0, t1 = int(sys.argv[2]), int(sys.argv[3])
step = int(sys.argv[4]) if len(sys.argv) > 4 else 1
NB = 8
print(f"{d}  t={t0}..{t1} step={step}   per-band non-black%% (8 bands of 30 rows)")
hdr = "     t  " + "".join(f" b{i}" for i in range(NB)) + "   total%"
print(hdr)
for t in range(t0, t1 + 1, step):
    p = os.path.join(d, f"t{t:04d}.ppm")
    if not os.path.exists(p):
        continue
    im = Image.open(p).convert("RGB")
    w, h = im.size
    px = im.load()
    bh = h // NB
    out = []
    tot = 0
    for b in range(NB):
        nz = 0
        for y in range(b * bh, (b + 1) * bh, 2):
            for x in range(0, w, 2):
                r, g, bl = px[x, y]
                if r | g | bl:
                    nz += 1
        n = (w // 2) * (bh // 2)
        tot += nz
        out.append(100.0 * nz / n)
    print(f"{t:6d}  " + " ".join(f"{v:3.0f}" for v in out) +
          f"   {100.0 * tot / ((w // 2) * (h // 2)):5.1f}")
