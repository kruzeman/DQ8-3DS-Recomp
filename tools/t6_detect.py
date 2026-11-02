#!/usr/bin/env python3
"""Artifact detector for the adventure-log menu: find 8x8 blocks inside the
panel interior whose mean luminance is far above the panel's dark fill.

usage: t6_detect.py <frame1.ppm> [frame2.ppm ...]
Prints, per frame, the brightest non-text block and a verdict.
Panel interior (DS 400x240 coords): x 18..260, y 9..66.
Text rows (glyph rows) are excluded: y ranges 18..35, 41..58, 64..66 are
skipped only for blocks that contain >3 pixels above 200 (glyph pixels).
"""
import sys
from PIL import Image

PANEL = (18, 9, 260, 67)


def blocks(path):
    im = Image.open(path).convert("L")
    w, h = im.size
    px = list(im.getdata())
    out = []
    for y in range(PANEL[1], PANEL[3] - 7, 4):
        for x in range(PANEL[0], PANEL[2] - 7, 4):
            vals = []
            glyph = 0
            for yy in range(y, y + 8):
                base = yy * w
                for xx in range(x, x + 8):
                    v = px[base + xx]
                    vals.append(v)
                    if v > 200:
                        glyph += 1
            if glyph > 3:
                continue
            out.append((sum(vals) / len(vals), x, y))
    out.sort(reverse=True)
    return out


for p in sys.argv[1:]:
    b = blocks(p)
    top = b[:4]
    print("%s  top=%s" % (p.split("/")[-1][:26],
                          " ".join("[%d,%d]=%.0f" % (x, y, m) for m, x, y in top)))
