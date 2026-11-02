#!/usr/bin/env python3
"""Panel-region brightness statistics for the adventure-log menu frames.

usage: menu_stats.py <img> <x0> <y0> <x1> <y1>
prints mean/min/max luminance and a coarse 8x8-cell map of mean luminance.
"""
import sys
from PIL import Image

def lum(px):
    r, g, b = px
    return 0.299 * r + 0.587 * g + 0.114 * b

def main():
    p = sys.argv[1]
    x0, y0, x1, y1 = (int(v) for v in sys.argv[2:6])
    im = Image.open(p).convert("RGB")
    reg = im.crop((x0, y0, x1, y1))
    w, h = reg.size
    d = list(reg.getdata())
    vals = [lum(px) for px in d]
    mean = sum(vals) / len(vals)
    vals_s = sorted(vals)
    p50 = vals_s[len(vals_s) // 2]
    p90 = vals_s[int(len(vals_s) * 0.9)]
    p99 = vals_s[int(len(vals_s) * 0.99)]
    print("%s region=(%d,%d)-(%d,%d) mean=%.1f p50=%.1f p90=%.1f p99=%.1f max=%.1f"
          % (p.split("/")[-1], x0, y0, x1, y1, mean, p50, p90, p99, vals_s[-1]))
    # 12x6 cell map
    cx, cy = 12, 6
    print("cell map (mean lum):")
    for j in range(cy):
        row = []
        for i in range(cx):
            ax0, ay0 = i * w // cx, j * h // cy
            ax1, ay1 = (i + 1) * w // cx, (j + 1) * h // cy
            s = n = 0
            for y in range(ay0, ay1):
                base = y * w
                for x in range(ax0, ax1):
                    s += lum(d[base + x]); n += 1
            row.append("%5.1f" % (s / n if n else 0))
        print("  " + " ".join(row))

if __name__ == "__main__":
    main()
