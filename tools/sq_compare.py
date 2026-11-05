#!/usr/bin/env python3
"""t_e216fc37: side-by-side of our 400x240 top screen and the Azahar
reference's top screen, both cropped to the same ground region.

usage: python3 tools/sq_compare.py <ours.ppm|png> <out.png> [x0,y0,x1,y1]
       (region in OUR 400x240 coordinates)
"""
import sys
from PIL import Image

REF_BOX = (94, 138, 814, 570)      # top 3DS screen inside azahar-reference.jpg

ours = Image.open(sys.argv[1]).convert("RGB")
out = sys.argv[2]
x0, y0, x1, y1 = (int(v) for v in (sys.argv[3] if len(sys.argv) > 3 else "0,0,400,240").split(","))

ref = Image.open("/home/hermes/DQ8/evidence/render-cape-path/azahar-reference.jpg").convert("RGB")
ref = ref.crop(REF_BOX).resize((400, 240), Image.LANCZOS)
if ref.size != ours.size:
    ref = ref.resize(ours.size, Image.LANCZOS)

a = ours.crop((x0, y0, x1, y1))
b = ref.crop((x0, y0, x1, y1))
s = 3
a = a.resize((a.width * s, a.height * s), Image.NEAREST)
b = b.resize((b.width * s, b.height * s), Image.NEAREST)
canvas = Image.new("RGB", (a.width, a.height * 2 + 6), (0, 0, 0))
canvas.paste(a, (0, 0))
canvas.paste(b, (0, a.height + 6))
canvas.save(out)
print("wrote", out, canvas.size, "(top = ours, bottom = azahar)")
