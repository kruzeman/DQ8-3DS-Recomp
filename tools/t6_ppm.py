#!/usr/bin/env python3
"""t_6b81015d: PPM helpers — convert to PNG, diff two frames, find differing boxes."""
import sys, os, glob
from PIL import Image, ImageChops

def load(p):
    return Image.open(p).convert("RGB")

def main():
    cmd = sys.argv[1]
    if cmd == "png":
        for p in sys.argv[2:]:
            im = load(p)
            out = os.path.splitext(p)[0] + ".png"
            im.resize((im.width * 2, im.height * 2), Image.NEAREST).save(out)
            print("wrote", out, im.size)
    elif cmd == "diff":
        a, b = load(sys.argv[2]), load(sys.argv[3])
        if a.size != b.size:
            print("size mismatch", a.size, b.size); return
        d = ImageChops.difference(a, b)
        bbox = d.getbbox()
        px = d.convert("L")
        hist = px.histogram()
        ndiff = sum(hist[1:])
        print("size", a.size, "bbox", bbox, "changed_px", ndiff,
              "of", a.width * a.height, "%.2f%%" % (100.0 * ndiff / (a.width * a.height)))
        print("max", max(px.getdata()), "mean", sum(i * c for i, c in enumerate(hist)) / (a.width * a.height))
        if len(sys.argv) > 4:
            amp = px.point(lambda v: 255 if v > int(sys.argv[4]) else 0)
            amp.save(sys.argv[5] if len(sys.argv) > 5 else "/tmp/diffmask.png")
            print("mask ->", sys.argv[5] if len(sys.argv) > 5 else "/tmp/diffmask.png")
    elif cmd == "rows":
        # per-row changed pixel counts between two frames
        a, b = load(sys.argv[2]), load(sys.argv[3])
        d = ImageChops.difference(a, b).convert("L")
        w, h = d.size
        data = list(d.getdata())
        for y in range(h):
            c = sum(1 for v in data[y * w:(y + 1) * w] if v > 2)
            if c:
                print("row %3d: %4d changed" % (y, c))
    else:
        print("usage: ppm_tools.py png|diff|rows ...")

if __name__ == "__main__":
    main()
