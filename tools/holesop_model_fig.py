#!/usr/bin/env python3
"""t_a22413f1: compose the model-case figure (SW | PRE-fix GL | fixed GL).

usage: holesop_model_fig.py <out.png> <sw.ppm> <gl_a.ppm> <gl_b.ppm>
Upscaled with nearest-neighbour so every pixel is visible.
"""
import sys
from PIL import Image

out = sys.argv[1]
ims = [Image.open(p).convert("RGB") for p in sys.argv[2:5]]
scale = 8
w, h = ims[0].size
gap = 4
canvas = Image.new("RGB", (w * len(ims) * scale + gap * (len(ims) - 1), h * scale), (0, 0, 0))
for i, im in enumerate(ims):
    big = im.resize((w * scale, h * scale), Image.NEAREST)
    canvas.paste(big, (i * (w * scale + gap), 0))
canvas.save(out)
print("wrote %s  %s" % (out, canvas.size))
for p in sys.argv[2:5]:
    im = Image.open(p).convert("RGB")
    cols = sorted(set(im.getdata()))
    print("  %s: %d distinct colours, centre=%s" % (p, len(cols), im.getpixel((16, 16))))
