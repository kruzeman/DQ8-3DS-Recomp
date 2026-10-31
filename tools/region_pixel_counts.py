#!/usr/bin/env python3
"""Region pixel forensics: non-black pixel counts in a screen region.

Used by t_a3ac49e9 to prove/disprove character-model pixel content in a
scene capture and to build the before/after pixel-count table.

Usage:
  region_pixel_counts.py [--crop x0,y0,x1,y1] [--region x0,y0,x1,y1]
                         [--thr N] [--label NAME] file [file...]

--crop/--region are normalized 0..1 fractions of the image (after crop for
--region). Defaults: region = x 0.36..0.64, y 0.15..0.50 (the character-model
zone per the Azahar reference ref-azahar-yangus-hut.jpg: Yangus spans x
0.38-0.62, y 0.18-0.45 of the game area).

A pixel counts as non-black when max(r,g,b) > thr (default 8) and as
"content" when max(r,g,b) > thr2 (default 24, separates dim world geometry
from hard black). Prints one TSV row per file.
"""
import sys

from PIL import Image


def parse_box(s):
    x0, y0, x1, y1 = (float(v) for v in s.split(","))
    return x0, y0, x1, y1


def main():
    args = sys.argv[1:]
    crop = None
    region = (0.36, 0.15, 0.64, 0.50)
    thr = 8
    thr2 = 24
    label = ""
    files = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--crop":
            i += 1
            crop = parse_box(args[i])
        elif a == "--region":
            i += 1
            region = parse_box(args[i])
        elif a == "--thr":
            i += 1
            thr = int(args[i])
        elif a == "--thr2":
            i += 1
            thr2 = int(args[i])
        elif a == "--label":
            i += 1
            label = args[i]
        else:
            files.append(a)
        i += 1

    print("label\tfile\tWxH\tregion_px\tnonblack\tnonblack_pct\tcontent\theavy\theavy_pct\tmaxlum\tmeanlum")
    for path in files:
        img = Image.open(path).convert("RGB")
        if crop:
            W, H = img.size
            box = (
                int(crop[0] * W), int(crop[1] * H),
                int(crop[2] * W), int(crop[3] * H),
            )
            img = img.crop(box)
        W, H = img.size
        rx0, ry0, rx1, ry1 = region
        box = (
            int(rx0 * W), int(ry0 * H),
            max(int(rx0 * W) + 1, int(rx1 * W)),
            max(int(ry0 * H) + 1, int(ry1 * H)),
        )
        reg = img.crop(box)
        px = list(reg.getdata())  # type: ignore[reportArgumentType]
        n = len(px)
        nonblack = content = heavy = 0
        lum_sum = 0
        maxlum = 0
        for (r, g, b) in px:
            m = r if r > g else g
            if b > m:
                m = b
            lum_sum += (r + g + b) / 3.0
            if m > maxlum:
                maxlum = m
            if m > thr:
                nonblack += 1
            if m > thr2:
                content += 1
            if m > 64:
                heavy += 1
        print(
            f"{label}\t{path}\t{W}x{H}\t{box}\t{nonblack}\t{100.0*nonblack/n:.2f}%\t"
            f"{content}\t{heavy}\t{100.0*heavy/n:.2f}%\t{maxlum}\t{lum_sum/n:.2f}"
        )


if __name__ == "__main__":
    main()
