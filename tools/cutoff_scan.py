#!/usr/bin/env python3
"""For each row in a band, find the first non-black column in GL and SW."""
import sys
from PIL import Image
a = Image.open(sys.argv[1]).convert("RGB")  # GL
b = Image.open(sys.argv[2]).convert("RGB")  # SW
w, h = a.size
pa, pb = a.load(), b.load()
def first_nb(p, y):
    for x in range(w):
        if sum(p[x, y]) > 30: return x
    return None
def last_nb(p, y):
    for x in range(w - 1, -1, -1):
        if sum(p[x, y]) > 30: return x
    return None
for y in range(145, 240, 3):
    print(f"y={y:3d}: GL first={first_nb(pa,y)} last={last_nb(pa,y)}   SW first={first_nb(pb,y)} last={last_nb(pb,y)}")
