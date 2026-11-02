#!/usr/bin/env python3
"""Align our render (400x240 PPM) to an operator video frame (553x150) and diff.

usage: t6_align.py <our.ppm> <vid.png>
Scales our frame by 553/400 = 1.3825 and searches the crop offset that best
matches the video frame, then writes /tmp/t6/aligned.png and /tmp/t6/diff.png.
"""
import sys, os
from PIL import Image, ImageChops, ImageFilter

our = Image.open(sys.argv[1]).convert("RGB")
vid = Image.open(sys.argv[2]).convert("RGB")
W = vid.width
scale = W / our.width
big = our.resize((W, int(round(our.height * scale))), Image.BILINEAR)
os.makedirs("/tmp/t6", exist_ok=True)

best = None
for dy in range(0, big.height - vid.height + 1, 2):
    for dx in (-8, -4, 0, 4, 8):
        crop = big.crop((dx, dy, dx + W, dy + vid.height))
        d = ImageChops.difference(crop, vid).convert("L")
        px = list(d.getdata())
        score = sum(px) / len(px)
        if best is None or score < best[0]:
            best = (score, dx, dy)
print("best mean|diff| = %.2f at dx=%d dy=%d (scale %.4f, scaled height %d)"
      % (best[0], best[1], best[2], scale, big.height))
score, dx, dy = best
crop = big.crop((dx, dy, dx + W, dy + vid.height))
crop.save("/tmp/t6/aligned.png")
vid.save("/tmp/t6/vid.png")
d = ImageChops.difference(crop, vid)
d = d.point(lambda v: min(255, v * 3))
d.save("/tmp/t6/diff.png")
print("wrote /tmp/t6/aligned.png /tmp/t6/diff.png")
