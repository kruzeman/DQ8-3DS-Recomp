"""t_05281b89 stage A: where does the scaled frame differ from the native one?

Prints an ASCII map of mean |d| per 8x8 pixel block of the presented frame,
so the report can state exactly which part of the screen the internal render
scale changed (and confirm the part it did not).  Also prints the overall
share of pixels that differ by any amount.

usage: rsa_diffmap.py <x1.ppm> <xf.ppm>
"""
import sys


def load(p):
    data = open(p, 'rb').read()
    parts = data.split(b'\n', 3)
    assert parts[0].strip() == b'P6', p
    w, h = map(int, parts[1].split())
    return w, h, parts[3][:w * h * 3]


def main():
    w, h, a = load(sys.argv[1])
    w2, h2, b = load(sys.argv[2])
    assert (w, h) == (w2, h2)
    C = 8
    nz = 0
    tot = 0
    print('frame %dx%d, block grid %dx%d (mean |d| per 8x8 block, '
          "'0'=identical, '1'..'3'=small, '4'..'9'=large)" % (w, h, w // C, h // C))
    for by in range(0, h, C):
        line = ''
        for bx in range(0, w, C):
            s = 0
            n = 0
            for y in range(by, min(by + C, h)):
                for x in range(bx, min(bx + C, w)):
                    i = (y * w + x) * 3
                    d = (abs(a[i] - b[i]) + abs(a[i + 1] - b[i + 1]) +
                         abs(a[i + 2] - b[i + 2]))
                    s += d
                    n += 1
                    tot += 1
                    if d:
                        nz += 1
            m = s / float(n)
            line += '0' if m < 0.5 else str(min(9, int(m)))
        print('DMAP ' + line)
    print('pixels differing at all: %d/%d (%.2f%%), scaled/native args: %s %s'
          % (nz, tot, 100.0 * nz / tot, sys.argv[1], sys.argv[2]))


main()
