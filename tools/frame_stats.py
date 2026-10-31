#!/usr/bin/env python3
"""Summarize PPM frame captures: for each frame print time label, max byte and
nonzero byte count, so fades/black frames are distinguishable from content."""
import sys, glob, os

d = sys.argv[1]
sel = sys.argv[2] if len(sys.argv) > 2 else "top"  # top|bottom|both
files = sorted(glob.glob(os.path.join(d, "frames", "t*.ppm")))
for f in files:
    base = os.path.basename(f)
    if sel == "top" and ".bottom" in base:
        continue
    if sel == "bottom" and ".bottom" not in base:
        continue
    data = open(f, "rb").read()
    # header: P6\nW H\n255\n
    parts = data.split(b"\n", 3)
    payload = parts[3] if len(parts) > 3 else b""
    nz = sum(1 for b in payload if b)
    mx = max(payload) if payload else 0
    print(f"{base} len={len(payload)} max={mx} nonzero={nz}")
