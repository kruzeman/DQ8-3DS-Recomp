#!/usr/bin/env python3
"""t_d559a510: side-by-side PNG of selected arm frames (labelled) + diff heatmap.

usage: glb_shot.py <out.png> <label:ppm> <label:ppm> [...]
Each frame is scaled 2x and stacked horizontally with its label; a diff heatmap
of the first two frames is appended when two frames are given.
"""
import os
import sys

from PIL import Image, ImageChops, ImageDraw


def main():
    out = sys.argv[1]
    items = []
    for a in sys.argv[2:]:
        lab, p = a.split(":", 1)
        im = Image.open(p).convert("RGB")
        items.append((lab, im.resize((im.width * 2, im.height * 2), Image.NEAREST)))
    w = sum(i[1].width for i in items) + 4 * len(items)
    h = max(i[1].height for i in items) + 20
    canvas = Image.new("RGB", (w, h), (32, 32, 32))
    x = 0
    for lab, im in items:
        canvas.paste(im, (x, 20))
        ImageDraw.Draw(canvas).text((x + 4, 4), lab, fill=(255, 255, 0))
        x += im.width + 4
    canvas.save(out)
    print("wrote", out, canvas.size)
    if len(items) >= 2:
        a, b = items[0][1], items[1][1]
        if a.size == b.size:
            d = ImageChops.difference(a, b)
            d = d.point(lambda v: min(255, v * 24))
            d.save(os.path.splitext(out)[0] + "_heat.png")
            print("wrote", os.path.splitext(out)[0] + "_heat.png")


main()
