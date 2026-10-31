#!/usr/bin/env python3
"""Find rows where a given column differs, and locate the non-black extent per image."""
import sys
from PIL import Image
a = Image.open(sys.argv[1]).convert("RGB")
b = Image.open(sys.argv[2]).convert("RGB")
w, h = a.size
pa, pb = a.load(), b.load()
col = int(sys.argv[3]) if len(sys.argv) > 3 else 0
rows = [y for y in range(h) if pa[col, y] != pb[col, y]]
print(f"col {col}: differing rows {rows[:5]}..{rows[-5:] if rows else []} (n={len(rows)})")
for y in rows[:6]:
    print(f"  row {y}: GL={pa[col,y]} SW={pb[col,y]}")
# For a horizontal band, find non-black extent in each image
def extent(p, y):
    xs = [x for x in range(w) if sum(p[x, y]) > 30]
    return (xs[0], xs[-1]) if xs else None
for y in range(90, 160, 6):
    print(f"  y={y}: GL extent={extent(pa,y)}  SW extent={extent(pb,y)}")
