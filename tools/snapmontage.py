#!/usr/bin/env python3
"""Build a labelled contact sheet from t####.ppm snapshots (t_993e6fb6).

usage: snapmontage.py <indir> <out.png> [cols] [thumbsize] [every]

Picks every Nth snapshot so the sheet covers the whole run, tiles them with the
tick number burned in, so one vision call can classify a 10-minute capture.
"""
import glob
import os
import sys
from PIL import Image, ImageDraw

ind = sys.argv[1]
out = sys.argv[2]
cols = int(sys.argv[3]) if len(sys.argv) > 3 else 6
thumb = int(sys.argv[4]) if len(sys.argv) > 4 else 200
every = int(sys.argv[5]) if len(sys.argv) > 5 else 20

files = sorted(glob.glob(os.path.join(ind, 't[0-9][0-9][0-9][0-9].ppm')))
if not files:
    files = sorted(glob.glob(os.path.join(ind, 'f_*.ppm')))
sel = files[::every]
if not sel:
    raise SystemExit('no snapshots in ' + ind)
w, h = Image.open(sel[0]).size
th = int(round(thumb * h / w))
rowsh = (len(sel) + cols - 1) // cols
sheet = Image.new('RGB', (cols * thumb, rowsh * (th + 12)), (32, 32, 32))
dr = ImageDraw.Draw(sheet)
for i, f in enumerate(sel):
    im = Image.open(f).convert('RGB').resize((thumb, th))
    x = (i % cols) * thumb
    y = (i // cols) * (th + 12)
    sheet.paste(im, (x, y + 12))
    dr.text((x + 2, y + 1), os.path.basename(f)[1:5], fill=(255, 255, 0))
sheet.save(out)
print('%s: %d frames (%dx%d tiles) -> %s' % (ind, len(sel), thumb, th, out))
