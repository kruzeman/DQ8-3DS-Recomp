#!/usr/bin/env python3
"""Rotate the runtime's portrait top-screen dump (240x400 P6) to landscape.

The top LCD framebuffer is 240x400 portrait; the runtime's capture path maps
src(x,y) -> dst(y, 239-x).  This tool applies exactly that mapping so the
result can be compared with the 400x240 reference screenshots.

usage: tools/ppm_rot.py IN.ppm OUT.ppm
"""
import sys


def read_ppm(path):
    with open(path, "rb") as fh:
        data = fh.read()
    parts = []
    i = 0
    while len(parts) < 4:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while i < len(data) and data[i:i + 1] != b"\n":
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j + 1].isspace():
            j += 1
        parts.append(data[i:j])
        i = j
    i += 1
    w, h = int(parts[1]), int(parts[2])
    return w, h, data[i:i + w * h * 3]


def main():
    w, h, px = read_ppm(sys.argv[1])
    if w == 400 and h == 240:
        out_w, out_h = w, h
        rot = False
    else:
        out_w, out_h = h, w          # 240x400 -> 400x240
        rot = True
    buf = bytearray(out_w * out_h * 3)
    for y in range(h):
        row = y * w * 3
        for x in range(w):
            s = row + x * 3
            if rot:
                dx, dy = y, 239 - x
            else:
                dx, dy = x, y
            d = (dy * out_w + dx) * 3
            buf[d:d + 3] = px[s:s + 3]
    with open(sys.argv[2], "wb") as fh:
        fh.write(b"P6\n%d %d\n255\n" % (out_w, out_h))
        fh.write(bytes(buf))
    print("wrote %s %dx%d rot=%d" % (sys.argv[2], out_w, out_h, rot))


if __name__ == "__main__":
    main()
