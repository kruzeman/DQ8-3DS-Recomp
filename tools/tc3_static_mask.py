#!/usr/bin/env python3
"""t_66c89cf2: static-scene-mask frame comparison (phase-exact correctness test).

Characters animate between runs (AFTER renders 36% more frames in the same
wall time), so a raw frame diff mixes animation phase with real differences.
This test builds a STATIC mask = pixels byte-identical across time WITHIN each
run (t0005 vs t0030 for BEFORE, BEFORE2 and AFTER separately, intersected),
then compares BEFORE vs AFTER ONLY on those pixels.  Static scene content
(background, props, dialogue UI — all texture-sourced) must match EXACTLY if
caching did not corrupt anything; animation-only differences are masked out."""
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

def frame(run, t):
    return readppm(os.path.join(EV, run, 'frames', t + '.ppm'))

runs_static = {}
for run in ['texcache2-BEFORE', 'texcache2-BEFORE2', 'texcache2-AFTER']:
    w, h, a = frame(run, 't0005')
    _, _, b = frame(run, 't0030')
    runs_static[run] = bytes(1 if a[i] == b[i] else 0 for i in range(len(a)))
    print('%s: static bytes t5 vs t30 = %d/%d (%.1f%%)' %
          (run, sum(runs_static[run]), len(a), 100.0*sum(runs_static[run])/len(a)))

# static mask: byte static in ALL runs (and across runs at t0005 via BEFORE/BEFORE2)
_, _, b5 = frame('texcache2-BEFORE', 't0005')
_, _, b25 = frame('texcache2-BEFORE2', 't0005')
_, _, a5 = frame('texcache2-AFTER', 't0005')
_, _, b30 = frame('texcache2-BEFORE', 't0030')
_, _, b230 = frame('texcache2-BEFORE2', 't0030')
_, _, a30 = frame('texcache2-AFTER', 't0030')

mask = bytearray(len(b5))
for i in range(len(b5)):
    mask[i] = (runs_static['texcache2-BEFORE'][i] and
               runs_static['texcache2-BEFORE2'][i] and
               runs_static['texcache2-AFTER'][i] and
               b5[i] == b25[i])
nmask = sum(mask)
print('static mask (static in all 3 runs + BEFORE/BEFORE2 agree at t5): %d/%d bytes (%.1f%%)'
      % (nmask, len(mask), 100.0*nmask/len(mask)))

def masked_diff(pa, pb, label):
    _, _, da = pa
    _, _, db = pb
    bad = 0
    tot = 0
    for i in range(len(da)):
        if not mask[i]:
            continue
        tot += 1
        if da[i] != db[i]:
            bad += 1
    print('%-42s masked_diff=%d/%d = %.4f%%  %s'
          % (label, bad, tot, 100.0*bad/tot if tot else 0,
             'PASS(exact)' if bad == 0 else 'FAIL'))

print()
masked_diff((0,0,b5), (0,0,a5), 'BEFORE vs AFTER @ t0005 (static scene)')
masked_diff((0,0,b30), (0,0,a30), 'BEFORE vs AFTER @ t0030 (static scene)')
masked_diff((0,0,b5), (0,0,b25), 'CONTROL: BEFORE vs BEFORE2 @ t0005')
masked_diff((0,0,b30), (0,0,b230), 'CONTROL: BEFORE vs BEFORE2 @ t0030')

# full-frame numbers for reference
def full(pa, pb, label):
    _, _, da = pa
    _, _, db = pb
    bad = sum(1 for i in range(len(da)) if da[i] != db[i])
    print('%-42s full_diff=%d/%d = %.2f%%' % (label, bad, len(da), 100.0*bad/len(da)))

print()
full((0,0,b5), (0,0,a5), 'full-frame BEFORE vs AFTER @ t0005')
full((0,0,b5), (0,0,b25), 'full-frame CONTROL @ t0005')
