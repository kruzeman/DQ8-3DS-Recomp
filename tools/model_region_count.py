#!/usr/bin/env python3
"""Before/after pixel-count table for the character-model region (t_664244f9).

The acceptance evidence for the character fix needs a quantitative statement
that the model region gained rendered model pixels (not just "it looks
different"), so this reports, for a fixed crop around where the model stands
(default: centre of the 400x240 top screen, above the dialog box):

  - "lit" pixels: luma >= --lit threshold (default 40/255) -- kills the dark
    forest background without hiding a lit model
  - "colored" pixels: max(rgb)-min(rgb) >= 12 -- skin/clothes saturation,
    again vs the mostly desaturated dark forest
  - changed pixels between before/after with |delta| luma >= 8, and their
    bounding box

Usage:
  tools/model_region_count.py before.ppm after.ppm [x0,y0,x1,y1]
"""
import sys

def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise SystemExit("%s: not a P6 PPM" % path)
    # header: P6\n<w> <h>\n<max>\n
    parts = []
    i = 2
    while len(parts) < 3:
        while data[i : i + 1].isspace():
            i += 1
        if data[i : i + 1] == b"#":
            while data[i : i + 1] != b"\n":
                i += 1
            continue
        j = i
        while not data[j : j + 1].isspace():
            j += 1
        parts.append(int(data[i:j]))
        i = j
    i += 1  # single whitespace after maxval
    w, h, maxval = parts
    assert maxval == 255, maxval
    px = data[i : i + w * h * 3]
    assert len(px) == w * h * 3, (len(px), w * h * 3)
    return w, h, px

def stats(path, box, lit_th=40, sat_th=12):
    w, h, px = read_ppm(path)
    x0, y0, x1, y1 = box
    x0 = max(0, min(w - 1, x0)); x1 = max(0, min(w, x1))
    y0 = max(0, min(h - 1, y0)); y1 = max(0, min(h, y1))
    lit = colored = 0
    lum = {}
    for y in range(y0, y1):
        for x in range(x0, x1):
            o = (y * w + x) * 3
            r, g, b = px[o], px[o + 1], px[o + 2]
            l = (r * 299 + g * 587 + b * 114) // 1000
            lum[(x, y)] = l
            if l >= lit_th:
                lit += 1
            if max(r, g, b) - min(r, g, b) >= sat_th:
                colored += 1
    return w, h, lit, colored, lum, px

def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    box = (140, 60, 260, 180)
    if len(sys.argv) > 3:
        box = tuple(int(v) for v in sys.argv[3].split(","))
    wa, ha, lita, cola, luma, pxa = stats(sys.argv[1], box)
    wb, hb, litb, colb, lumb, pxb = stats(sys.argv[2], box)
    assert (wa, ha) == (wb, hb)
    changed = 0
    bx = [1 << 30, 1 << 30, -1, -1]
    for (x, y), la in luma.items():
        lb = lumb[(x, y)]
        if abs(la - lb) >= 8:
            changed += 1
            bx[0] = min(bx[0], x); bx[1] = min(bx[1], y)
            bx[2] = max(bx[2], x); bx[3] = max(bx[3], y)
    region = "%d,%d..%d,%d" % box
    print("region %s (%dx%d screen)" % (region, wa, ha))
    print("%-10s %10s %10s" % ("metric", "before", "after"))
    print("%-10s %10d %10d" % ("lit", lita, litb))
    print("%-10s %10d %10d" % ("colored", cola, colb))
    print("%-10s %10s %10d" % ("changed", "-", changed))
    if bx[2] >= 0:
        print("change bbox %d,%d..%d,%d" % tuple(bx))

if __name__ == "__main__":
    main()
