#!/usr/bin/env python3
"""t_66c89cf2: localize the static-masked BEFORE vs AFTER difference.

Writes a heatmap where:
  red   = byte static within every run AND differing BEFORE vs AFTER (the 5352)
  green = animated (excluded from the verdict)
plus row/column concentration of the red pixels, and region clusters."""
import os

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

w, h, b5 = fr('texcache2-BEFORE', 't0005')
_, _, b30 = fr('texcache2-BEFORE', 't0030')
_, _, a5 = fr('texcache2-AFTER', 't0005')
_, _, a30 = fr('texcache2-AFTER', 't0030')
_, _, b25 = fr('texcache2-BEFORE2', 't0005')
_, _, b230 = fr('texcache2-BEFORE2', 't0030')

n = len(b5)
buf = bytearray(n)
red = []
for i in range(n):
    st = (b5[i] == b30[i]) and (a5[i] == a30[i]) and (b25[i] == b230[i]) and (b5[i] == b25[i])
    if not st:
        # animated: dim green
        buf[i] = 30 if i % 3 != 1 else 60
    elif b5[i] != a5[i]:
        buf[i] = 255
        red.append(i)
    else:
        g = b5[i] // 5
        buf[i] = g
# keep RGB triplets together: red only where all 3 bytes qualify
redpx = []
for p in range(0, n, 3):
    if all((b5[i] == b30[i]) and (a5[i] == a30[i]) and (b25[i] == b230[i])
           and (b5[i] == b25[i]) and (b5[i] != a5[i]) for i in (p, p+1, p+2)):
        redpx.append(p // 3)
    # clear half-pixel reds (channels differing individually)
for p in range(0, n, 3):
    if (p // 3) not in set(redpx):
        pass

buf2 = bytearray(n)
redset = set(redpx)
rows = {}
cols = {}
for y in range(h):
    for x in range(w):
        p = y * w + x
        i = p * 3
        static = ((b5[i] == b30[i]) and (a5[i] == a30[i]) and (b25[i] == b230[i])
                  and (b5[i] == b25[i]) and (b5[i+1] == b25[i+1]) and (b5[i+2] == b25[i+2]))
        diff3 = (b5[i] != a5[i]) or (b5[i+1] != a5[i+1]) or (b5[i+2] != a5[i+2])
        if static and diff3:
            buf2[i] = 255; buf2[i+1] = 40; buf2[i+2] = 40
            rows[y] = rows.get(y, 0) + 1
            cols[x] = cols.get(x, 0) + 1
        elif not static:
            buf2[i] = 20; buf2[i+1] = 45; buf2[i+2] = 20
        else:
            g = (b5[i] + b5[i+1] + b5[i+2]) // 8
            buf2[i] = buf2[i+1] = buf2[i+2] = g

out = '/tmp/tc3_staticdiff.ppm'
open(out, 'wb').write(b'P6\n%d %d\n255\n' % (w, h) + bytes(buf2))
nred = sum(rows.values())
print('static-differing pixels: %d (%.2f%% of frame)' % (nred, 100.0*nred/(w*h)))
print('top rows:', sorted(rows.items(), key=lambda kv: -kv[1])[:10])
print('top cols:', sorted(cols.items(), key=lambda kv: -kv[1])[:10])
# bounding box of red
if rows and cols:
    ys = sorted(rows); xs = sorted(cols)
    print('bbox: x[%d..%d] y[%d..%d]' % (xs[0], xs[-1], ys[0], ys[-1]))
# quadrant summary
q = [[0,0],[0,0]]
for y, c in rows.items():
    q[y >= h//2][x if False else 0]  # noop guard
q = {'TL':0,'TR':0,'BL':0,'BR':0}
for y, c in rows.items():
    for xcount in [c]:
        pass
for y, c in rows.items():
    pass
# simpler: sum rows into quadrants using column midpoint via recompute
qt = {'TL': 0, 'TR': 0, 'BL': 0, 'BR': 0}
for y in rows:
    for x in range(w):
        p = y*w + x
        if p in redset:
            pass
# too slow; redo per-pixel quadrant tally directly
qt = {'TL': 0, 'TR': 0, 'BL': 0, 'BR': 0}
for y in range(h):
    for x in range(w):
        p = y * w + x
        i = p * 3
        static = ((b5[i] == b30[i]) and (a5[i] == a30[i]) and (b25[i] == b230[i])
                  and (b5[i] == b25[i]) and (b5[i+1] == b25[i+1]) and (b5[i+2] == b25[i+2]))
        if static and ((b5[i] != a5[i]) or (b5[i+1] != a5[i+1]) or (b5[i+2] != a5[i+2])):
            key = ('T' if y < h//2 else 'B') + ('L' if x < w//2 else 'R')
            qt[key] += 1
print('quadrants:', qt)
print('wrote', out)
