#!/usr/bin/env python3
"""t_66c89cf2: phase-drift-aware frame comparison.

AFTER renders more frames per wall-second than BEFORE, so same-wall-time
snapshots are in different animation phases.  Compute:
  1. control matrix: BEFORE x BEFORE2 (same binary, phase noise floor)
  2. treatment matrix: BEFORE x AFTER
and report min/median over all pairs for each.  If treatment min is at the
control level, AFTER renders the same images at some phase alignment."""
import subprocess, os, re, sys

ROOT = '/home/hermes/DQ8/worktrees/texcache2-t_66c89cf2'
CMP = os.path.join(ROOT, 'tools/compare_frames.py')
TIMES = ['t0005', 't0010', 't0015', 't0020', 't0025', 't0030']

def matrix(dir_a, dir_b, label):
    vals = []
    print('== %s ==' % label)
    print('        ' + ' '.join('%7s' % t for t in TIMES))
    for ta in TIMES:
        row = []
        for tb in TIMES:
            pa = os.path.join(ROOT, dir_a, 'frames', ta + '.ppm')
            pb = os.path.join(ROOT, dir_b, 'frames', tb + '.ppm')
            if not (os.path.exists(pa) and os.path.exists(pb)):
                row.append(None); continue
            out = subprocess.run([sys.executable, CMP, pa, pb], capture_output=True, text=True).stdout
            m = re.search(r'\(([\d.]+)% of bytes', out)
            if 'IDENTICAL' in out and not m:
                row.append(0.0)
            elif m:
                row.append(float(m.group(1)))
            else:
                row.append(None)
        print('%-7s ' % ta + ' '.join('%7s' % ('%.2f' % v if v is not None else '-') for v in row))
        vals += [v for v in row if v is not None]
    vals.sort()
    n = len(vals)
    print('n=%d min=%.3f p25=%.3f median=%.3f max=%.3f' %
          (n, vals[0], vals[n//4], vals[n//2], vals[-1]))
    return vals

A = "target/blackworld-evidence/texcache2-AFTER"
B = "target/blackworld-evidence/texcache2-AFTER-CHECKALL"
C = 'target/blackworld-evidence/texcache2-AFTER'
ctrl = matrix(A, B, 'SAME-BINARY: AFTER x AFTER-CHECKALL (code-identical, ~1% speed diff) (phase noise floor)')
treat = matrix(A, C, 'TREATMENT2: AFTER x AFTER-CHECKALL')
print()
print('control  min=%.3f median=%.3f' % (ctrl[0], ctrl[len(ctrl)//2]))
print('treatment min=%.3f median=%.3f' % (treat[0], treat[len(treat)//2]))
