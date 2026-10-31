#!/usr/bin/env python3
"""Crop a region from BEFORE and AFTER frames side by side, scaled up, for
visual inspection. usage: tc3_crop_pair.py x0 y0 x1 y1 scale out.png"""
import sys, os, zlib, struct

ROOT = '/home/hermes/DQ8/worktrees/texcache2-t_66c89cf2'
EV = os.path.join(ROOT, 'target/blackworld-evidence')

def readppm(p):
    f = open(p, 'rb')
    assert f.readline().strip() == b'P6', p
    l = f.readline()
    while l.startswith(b'#'):
        l = f.readline()
    w, h = map(int, l.split())
    int(f.readline())
    return w, h, f.read()

def fr(run, t):
    return readppm(os.path.join(EV, run, 'frames', t + '.ppm'))

def writepng(path, w, h, rgb):
    raw = b''.join(b'\x00' + rgb[y*w*3:(y+1)*w*3] for y in range(h))
    def chunk(tag, data):
        c = tag + data
        return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)
    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(raw, 6))
           + chunk(b'IEND', b''))
    open(path, 'wb').write(png)

x0, y0, x1, y1 = map(int, sys.argv[1:5])
scale = int(sys.argv[5])
out = sys.argv[6]
t = sys.argv[7] if len(sys.argv) > 7 else 't0015'
run_a = sys.argv[8] if len(sys.argv) > 8 else 'texcache2-BEFORE'
run_b = sys.argv[9] if len(sys.argv) > 9 else 'texcache2-AFTER'

w, h, da = fr(run_a, t)
_, _, db = fr(run_b, t)
cw, ch = x1 - x0, y1 - y0
gap = 4
W = cw * scale * 2 + gap
H = ch * scale
outbuf = bytearray(W * H * 3)
for y in range(H):
    sy = y0 + y // scale
    for x in range(W):
        if x < cw * scale:
            sx = x0 + x // scale
            i = (sy * w + sx) * 3
            o = (y * W + x) * 3
            outbuf[o:o+3] = da[i:i+3]
        elif x < cw * scale + gap:
            outbuf[(y*W+x)*3:(y*W+x)*3+3] = b'\x00\xff\x00'
        else:
            sx = x0 + (x - cw * scale - gap) // scale
            i = (sy * w + sx) * 3
            o = (y * W + x) * 3
            outbuf[o:o+3] = db[i:i+3]
writepng(out, W, H, bytes(outbuf))
print('wrote %s  (left=BEFORE right=AFTER, %s, region %d,%d..%d,%d x%d)'
      % (out, t, x0, y0, x1, y1, scale))
