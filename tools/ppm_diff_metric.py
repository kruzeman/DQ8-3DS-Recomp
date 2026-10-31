#!/usr/bin/env python3
"""Pixel-level difference metric between two PPM frames (sw/gl differential)."""
import re
import sys


def load(p):
    data = open(p, "rb").read()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", data)
    w, h = int(m.group(1)), int(m.group(2))
    return w, h, data[m.end():]


a_w, a_h, a = load(sys.argv[1])
b_w, b_h, b = load(sys.argv[2])
if (a_w, a_h) != (b_w, b_h):
    print("DIM MISMATCH", (a_w, a_h), (b_w, b_h))
    sys.exit(2)
n = a_w * a_h
bad = 0
maxd = 0
first = []
for i in range(n):
    d = max(abs(a[3 * i] - b[3 * i]), abs(a[3 * i + 1] - b[3 * i + 1]),
            abs(a[3 * i + 2] - b[3 * i + 2]))
    if d:
        bad += 1
        if d > maxd:
            maxd = d
        if len(first) < 8:
            first.append((i % a_w, i // a_w, d))
print("%s vs %s: %dx%d differing=%d/%d (%.3f%%) maxdelta=%d first=%s" %
      (sys.argv[1], sys.argv[2], a_w, a_h, bad, n, 100.0 * bad / n, maxd, first))
