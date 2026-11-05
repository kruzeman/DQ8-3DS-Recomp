#!/usr/bin/env python3
"""t_fdd26eba: side-by-side guest-vs-scaled montage for a presented frame.

    python3 tools/ps_montage.py <out.png> <guest.ppm> <scaled.ppm> [x y w h]

Top band: the whole 800x480 top screen, guest path (left) and scaled path
(right).  Bottom band: the same [x y w h] region of both, upscaled 2x with
NEAREST so the 2x2 blocks of the guest path and the real detail of the scaled
path are both visible at the same magnification.
"""
import sys
from PIL import Image

W, H = 800, 480


def load(path):
    im = Image.open(path).convert("RGB")
    return im.crop((0, 0, W, H))


def main():
    out, gp, sp = sys.argv[1], sys.argv[2], sys.argv[3]
    if len(sys.argv) >= 8:
        x, y, w, h = (int(v) for v in sys.argv[4:8])
    else:
        x, y, w, h = 200, 120, 400, 240
    g = load(gp)
    s = load(sp)
    zoom_g = g.crop((x, y, x + w, y + h)).resize((w * 2, h * 2), Image.NEAREST)
    zoom_s = s.crop((x, y, x + w, y + h)).resize((w * 2, h * 2), Image.NEAREST)
    canvas = Image.new("RGB", (W * 2, H + h * 2), (0, 0, 0))
    canvas.paste(g, (0, 0))
    canvas.paste(s, (W, 0))
    canvas.paste(zoom_g, (0, H))
    canvas.paste(zoom_s, (w * 2, H))
    canvas.save(out)
    print("wrote %s (%dx%d)" % (out, canvas.size[0], canvas.size[1]))


if __name__ == "__main__":
    main()
