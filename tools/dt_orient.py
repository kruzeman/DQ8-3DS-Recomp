"""t_dd830a74: which geometric transform relates the 4x composite to the 1x one?

Compares box4(x4 capture) against x1 under identity / mirror-x / mirror-y /
180deg and reports mean |d| for each, so the orientation of the scaled-sample
path is measured, not guessed.

usage: dt_orient.py <x1.ppm> <x4.ppm>
"""
import sys
from PIL import Image


def load(p):
    data = open(p, 'rb').read()
    parts = data.split(b'\n', 3)
    w, h = map(int, parts[1].split())
    return Image.frombytes('RGB', (w, h), parts[3][:w * h * 3])


a = load(sys.argv[1])
b = load(sys.argv[2])
w, h = a.size
box = b.resize((w, h), Image.BOX)
pa = a.load()
cands = {
    'identity': box,
    'mirror-x': box.transpose(Image.FLIP_LEFT_RIGHT),
    'mirror-y': box.transpose(Image.FLIP_TOP_BOTTOM),
    'rot-180': box.transpose(Image.ROTATE_180),
}
for name, im in cands.items():
    pb = im.load()
    s = 0
    for y in range(h):
        for x in range(w):
            r, q = pa[x, y], pb[x, y]
            s += abs(r[0] - q[0]) + abs(r[1] - q[1]) + abs(r[2] - q[2])
    print('%-9s mean|d| = %.2f' % (name, s / (3.0 * w * h)))
