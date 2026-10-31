#!/usr/bin/env python3
"""Phase search: is the very dark 'black block' patch in AFTER's torso window
present in ANY frame of BEFORE (i.e. a script/pose state both runs pass
through), or only in AFTER (possible corruption)?"""
import os
EV = 'target/blackworld-evidence'

def rd(p):
    f = open(p, 'rb')
    assert f.readline().strip() == b'P6'
    l = f.readline()
    while l.startswith(b'#'):
        l = f.readline()
    w, h = map(int, l.split())
    int(f.readline())
    return w, f.read()

def darkcount(run, t, w, x0, y0, x1, y1, thr=24):
    _, d = rd(os.path.join(EV, run, 'frames', t + '.ppm'))
    n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            i = (y * w + x) * 3
            if (d[i] + d[i+1] + d[i+2]) / 3 < thr:
                n += 1
    return n

W = 400
X0, Y0, X1, Y1 = 150, 100, 260, 160
TS = ['t0005', 't0010', 't0015', 't0020', 't0025', 't0030']
for run in ['texcache2-BEFORE', 'texcache2-BEFORE2', 'texcache2-AFTER',
            'texcache2-AFTER-CHECKALL', 'texcache2-REFDEC']:
    counts = []
    for t in TS:
        try:
            counts.append('%s=%d' % (t, darkcount(run, t, W, X0, Y0, X1, Y1)))
        except FileNotFoundError:
            counts.append('%s=n/a' % t)
    print('%-28s %s' % (run, '  '.join(counts)))
