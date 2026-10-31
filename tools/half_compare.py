#!/usr/bin/env python3
"""Compare the left/right halves of a captured top screen (400x240 P6).

usage: tools/half_compare.py FRAME.ppm
Reports mean abs difference between columns 0..199 and 200..399, and the
best horizontal shift that aligns them (detects a duplicated/stereo image).
"""
import sys


def read_ppm(path):
    with open(path, "rb") as fh:
        data = fh.read()
    parts = []
    i = 0
    while len(parts) < 4:
        while i < len(data) and data[i:i+1].isspace():
            i += 1
        if data[i:i+1] == b"#":
            while i < len(data) and data[i:i+1] != b"\n":
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j+1].isspace():
            j += 1
        parts.append(data[i:j])
        i = j
    i += 1
    w, h = int(parts[1]), int(parts[2])
    return w, h, data[i:i + w * h * 3]


def main():
    w, h, px = read_ppm(sys.argv[1])
    print("frame %s: %dx%d" % (sys.argv[1], w, h))

    def col(x, y):
        o = (y * w + x) * 3
        return px[o], px[o + 1], px[o + 2]

    half = w // 2
    best = None
    for shift in range(-40, 41):
        total = 0
        n = 0
        for y in range(0, h, 3):
            for x in range(0, half, 3):
                xs = x + shift
                if xs < 0 or xs >= half:
                    continue
                a = col(x, y)
                b = col(x + half, y) if xs == x else col(xs + half, y)
                total += abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2])
                n += 3
        if n:
            avg = total / n
            if best is None or avg < best[1]:
                best = (shift, avg)
    print("best align shift=%d mean-abs-diff=%.2f (0 = halves identical)" % best)
    total = 0
    n = 0
    for y in range(0, h, 2):
        for x in range(0, half, 2):
            a = col(x, y)
            b = col(x + half, y)
            total += abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2])
            n += 3
    print("no-shift mean-abs-diff=%.2f" % (total / n))


if __name__ == "__main__":
    main()
