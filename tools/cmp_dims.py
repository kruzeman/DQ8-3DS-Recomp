#!/usr/bin/env python3
"""Compare two PPMs and print per-row / per-col diff counts (bounded output)."""
import sys
from PIL import Image

a = Image.open(sys.argv[1]).convert("RGB")
b = Image.open(sys.argv[2]).convert("RGB")
print("size", a.size, b.size)
if a.size != b.size:
    sys.exit("size mismatch")
pa, pb = a.load(), b.load()
w, h = a.size
rows = [0] * h
cols = [0] * w
for y in range(h):
    for x in range(w):
        if pa[x, y] != pb[x, y]:
            rows[y] += 1
            cols[x] += 1
print("total diff px:", sum(rows))
print("rows(diff):", [(i, c) for i, c in enumerate(rows) if c][:100])
print("cols(diff):", [(i, c) for i, c in enumerate(cols) if c][:100])
n = int(sys.argv[3]) if len(sys.argv) > 3 else 110
print("sample rows at col set:", sys.argv[4] if len(sys.argv) > 4 else "0,4,8,12,16")
cset = [int(v) for v in (sys.argv[4] if len(sys.argv) > 4 else "0,4,8,12,16").split(",")]
for y in range(n, n + 40, 5):
    print(y, "A", [pa[x, y] for x in cset], "B", [pb[x, y] for x in cset])
