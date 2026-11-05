#!/usr/bin/env python3
"""t_e216fc37: mean colour / luminance of the same region in our capture and
in the Azahar reference (top screen), plus the fraction of near-black pixels.

usage: python3 tools/sq_region.py <ours.ppm|png> x0,y0,x1,y1
"""
import sys
from PIL import Image

REF_BOX = (94, 138, 814, 570)

def stats(im, box, label):
    c = im.crop(box)
    px = list(c.getdata())
    n = len(px)
    r = sum(p[0] for p in px) / n
    g = sum(p[1] for p in px) / n
    b = sum(p[2] for p in px) / n
    dark = sum(1 for p in px if (p[0] * 299 + p[1] * 587 + p[2] * 114) // 1000 < 24)
    print("%-8s mean=(%5.1f,%5.1f,%5.1f) lum=%5.1f dark<24=%5.1f%%" %
          (label, r, g, b, 0.299 * r + 0.587 * g + 0.114 * b, 100.0 * dark / n))

ours = Image.open(sys.argv[1]).convert("RGB")
box = tuple(int(v) for v in sys.argv[2].split(","))
ref = Image.open("/home/hermes/DQ8/evidence/render-cape-path/azahar-reference.jpg").convert("RGB")
ref = ref.crop(REF_BOX).resize(ours.size, Image.LANCZOS)
stats(ours, box, "ours")
stats(ref, box, "azahar")
