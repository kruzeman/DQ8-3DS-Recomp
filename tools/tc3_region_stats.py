#!/usr/bin/env python3
"""t_66c89cf2: numeric region analysis of a BEFORE vs AFTER frame pair.

Answers "corruption or phase" without vision: for a fixed window, report
per-channel byte-diff percent and whether the differing pixels carry texture
pattern (high local variance) in BOTH frames (=> pattern exists in both, just
different phase/content) or is FLAT in one and patterned in the other
(=> one side lost the pattern = corruption)."""
import sys, os

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

def region_stats(da, db, w, x0, y0, x1, y1, label):
    n = 0
    bad = 0
    var_a = 0.0
    var_b = 0.0
    flat_b_bad = 0   # A patterned, B flat (B lost texture)
    flat_a_bad = 0   # B patterned, A flat (A lost texture)
    vals_a = []
    vals_b = []
    # local variance over 5x5 luminance in each
    def lum(d, x, y):
        i = (y * w + x) * 3
        return (d[i] + d[i+1] + d[i+2]) / 3.0
    for y in range(y0 + 2, y1 - 2):
        for x in range(x0 + 2, x1 - 2):
            i = (y * w + x) * 3
            d = (abs(da[i]-db[i]) + abs(da[i+1]-db[i+1]) + abs(da[i+2]-db[i+2]))
            n += 1
            if d > 12:
                bad += 1
                # local 5x5 variance of each side at this pixel
                la = [lum(da, xx, yy) for yy in range(y-2, y+3) for xx in range(x-2, x+3)]
                lb = [lum(db, xx, yy) for yy in range(y-2, y+3) for xx in range(x-2, x+3)]
                va = max(la) - min(la)
                vb = max(lb) - min(lb)
                if va > 24 and vb <= 8:
                    flat_b_bad += 1     # A has pattern, B flat
                elif vb > 24 and va <= 8:
                    flat_a_bad += 1     # B has pattern, A flat
    print('%-34s diff=%d/%d (%.1f%%)  A-patterned-B-flat=%d  B-patterned-A-flat=%d'
          % (label, bad, n, 100.0*bad/n if n else 0, flat_b_bad, flat_a_bad))

run_a = sys.argv[1]
run_b = sys.argv[2]
t = sys.argv[3]
w, h, da = fr(run_a, t)
_, _, db = fr(run_b, t)
# torso window (character), left prop window, dialogue box
region_stats(da, db, w, 150, 100, 260, 160, 'torso 150,100-260,160')
region_stats(da, db, w, 0, 20, 120, 80, 'left-prop 0,20-120,80')
region_stats(da, db, w, 20, 180, 380, 235, 'dialogue 20,180-380,235')
region_stats(da, db, w, 260, 40, 400, 120, 'right-260,40-400,120')
