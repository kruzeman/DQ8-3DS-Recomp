#!/usr/bin/env python3
"""Per-frame panel statistics on the operator video (panel found by profile).

Panel: cols 36..520, rows 17..132 in the 553x150 crop.
Reports mean luminance for: whole panel interior, the 'patch' region, and a
reference dark region; plus the max 16x16-block mean (to catch bright patches).
"""
import sys, glob
from PIL import Image

d = sys.argv[1]
PANEL = (36, 19, 521, 133)
PATCH = (66, 38, 200, 68)     # the bright patch behind 'Continue' (fr_0001)
DARK = (300, 90, 480, 130)    # right-lower panel area, away from text
files = sorted(glob.glob(d + "/fr_*.png"))


def stats(px, w, box):
    x0, y0, x1, y1 = box
    vals = [px[y * w + x] for y in range(y0, y1) for x in range(x0, x1)]
    n = len(vals)
    return sum(vals) / n, max(vals)


print("frame  panel_mean  patch_mean  patch_max  dark_mean  block_max")
for i, f in enumerate(files):
    im = Image.open(f).convert("L")
    w, h = im.size
    px = list(im.getdata())
    pm, _ = stats(px, w, PANEL)
    qm, qx = stats(px, w, PATCH)
    dm, _ = stats(px, w, DARK)
    # 16x16 block max inside the panel
    best = 0
    for y in range(20, 132, 16):
        for x in range(40, 512, 16):
            s = n = 0
            for yy in range(y, min(y + 16, 132)):
                for xx in range(x, min(x + 16, 512)):
                    s += px[yy * w + xx]; n += 1
            best = max(best, s / n)
    print("%3d  %8.2f  %9.2f  %8d  %8.2f  %8.1f" % (i + 1, pm, qm, qx, dm, best))
