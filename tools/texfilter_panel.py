#!/usr/bin/env python3
"""Build a labelled A / B / |A-B| panel for two PPM frames.

usage: texfilter_panel.py A.ppm B.ppm out.png [x0,y0,x1,y1] [scale] [labelA] [labelB]

The crop rectangle is in source pixels (inclusive corners); default is the
whole frame.  A and B are upscaled with nearest-neighbour (so the source
pixels stay visible), the diff panel shows 4x the absolute difference."""
import sys

from PIL import Image, ImageChops, ImageDraw

a = Image.open(sys.argv[1]).convert("RGB")
b = Image.open(sys.argv[2]).convert("RGB")
out = sys.argv[3]
if len(sys.argv) > 4 and sys.argv[4] != "-":
    x0, y0, x1, y1 = [int(v) for v in sys.argv[4].split(",")]
    a = a.crop((x0, y0, x1 + 1, y1 + 1))
    b = b.crop((x0, y0, x1 + 1, y1 + 1))
scale = int(sys.argv[5]) if len(sys.argv) > 5 else 4
lab_a = sys.argv[6] if len(sys.argv) > 6 else "A: DQ8_TEX_FILTER=0 (nearest)"
lab_b = sys.argv[7] if len(sys.argv) > 7 else "B: DQ8_TEX_FILTER=1 (guest bits)"

diff = ImageChops.difference(a, b)
diff = diff.point(lambda v: min(255, v * 4))
w, h = a.size
pad = 18
panel = Image.new("RGB", (w * scale, (h * 3) * scale + pad * 2), (16, 16, 16))
panel.paste(a.resize((w * scale, h * scale), Image.NEAREST), (0, pad))
panel.paste(b.resize((w * scale, h * scale), Image.NEAREST), (0, pad + h * scale))
panel.paste(diff.resize((w * scale, h * scale), Image.NEAREST),
            (0, pad + 2 * h * scale))
draw = ImageDraw.Draw(panel)
draw.text((2, 2), lab_a, fill=(255, 255, 255))
draw.text((2, pad + h * scale + 2), lab_b, fill=(255, 255, 255))
draw.text((2, pad + 2 * h * scale + 2), "|A-B| x4", fill=(255, 255, 255))
panel.save(out)
print("wrote %s (%dx%d) crop=%s scale=%d" % (out, panel.width, panel.height,
                                             sys.argv[4] if len(sys.argv) > 4 else "-",
                                             scale))
