#!/usr/bin/env python3
"""Diff two binary PPMs (P6); report per-column and per-row mismatch counts
to localize where two renderers diverge. Usage: ppm_diff.py a.ppm b.ppm"""
import sys
from PIL import Image

a = Image.open(sys.argv[1]).convert("RGB")
b = Image.open(sys.argv[2]).convert("RGB")
if a.size != b.size:
    print("size mismatch", a.size, b.size)
    sys.exit(1)
w, h = a.size
pa, pb = a.load(), b.load()
col_diff = [0] * w
row_diff = [0] * h
total = 0
for y in range(h):
    for x in range(w):
        if pa[x, y] != pb[x, y]:
            col_diff[x] += 1
            row_diff[y] += 1
            total += 1
print(f"size {w}x{h}  total_diff_px {total} ({100.0*total/(w*h):.2f}%)")
# find first/last differing column and row
first_c = next((x for x in range(w) if col_diff[x]), None)
last_c = next((w - 1 - x for x in range(w) if col_diff[x]), None)
first_r = next((y for y in range(h) if row_diff[y]), None)
last_r = next((h - 1 - y for y in range(h) if row_diff[y]), None)
print(f"diff cols {first_c}..{last_c}   diff rows {first_r}..{last_r}")
print("col_diff (x:count) nonzero:")
print(" ".join(f"{x}:{col_diff[x]}" for x in range(w) if col_diff[x]))
