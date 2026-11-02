#!/usr/bin/env python3
"""t_8df70ad4: median summary + PGLFLUSH means for a set of run dirs.

usage: zz_sum.py <label>:<dir> [<label>:<dir> ...]
"""
import sys, os, re, statistics

def load(d):
    tot, gs, gl, last_t = [], [], [], 0.0
    for name in ('out.log', 'err.log'):
        p = os.path.join(d, name)
        if not os.path.exists(p):
            continue
        for ln in open(p, errors='replace'):
            m = re.match(r'FRAMEPROF f=(\d+) t=([\d.]+) total_ms=([\d.]+) '
                         r'guest_ms=([\d.]+) gl_ms=([\d.]+)', ln)
            if m:
                tot.append(float(m.group(3)))
                gs.append(float(m.group(4)))
                gl.append(float(m.group(5)))
                last_t = max(last_t, float(m.group(2)))
    p = os.path.join(d, 'err.log')
    rel = take = lock = zrb = zwb = 0.0
    nw = nz = 0
    if os.path.exists(p):
        for ln in open(p, errors='replace'):
            if not ln.startswith('PGLFLUSH'):
                continue
            f = dict(t.split('=', 1) for t in ln.split() if '=' in t)
            if 'mean[rel/take/lock]' in f:
                a = f['mean[rel/take/lock]'].split('/')
                rel += float(a[0]); take += float(a[1]); lock += float(a[2])
                nw += 1
            if 'mean[read/col/zrb/zwb]' in f:
                a = f['mean[read/col/zrb/zwb]'].split('/')
                zrb += float(a[2]); zwb += float(a[3])
                nz += 1
    rend = sorted(x for x in tot if x > 5.0)
    rg = [g for g, t in zip(gs, tot) if t > 5.0]
    rgl = [g for g, t in zip(gl, tot) if t > 5.0]
    return dict(frames=len(tot), rendered=len(rend), fps=len(rend) / last_t if last_t else 0,
                p50=statistics.median(rend) if rend else 0,
                p90=rend[int(len(rend) * 0.9)] if rend else 0,
                guest=statistics.mean(rg) if rg else 0,
                gl=statistics.mean(rgl) if rgl else 0,
                rel=rel / nw if nw else 0, take=take / nw if nw else 0,
                lock=lock / nw if nw else 0, zrb=zrb / nz if nz else 0,
                zwb=zwb / nz if nz else 0)

groups = {}
for spec in sys.argv[1:]:
    lab, d = spec.split(':', 1)
    groups.setdefault(lab, []).append(load(d))

hdr = ('%-10s %6s %6s %6s %6s %6s %6s %6s %6s %6s %6s' %
       ('group', 'runs', 'fps', 'p50', 'p90', 'guest', 'gl', 'rel', 'take',
        'lock', 'zrb/zwb'))
print(hdr)
for lab, rows in groups.items():
    def med(k):
        return statistics.median([r[k] for r in rows])
    print('%-10s %6d %6.1f %6.2f %6.2f %6.2f %6.2f %6.3f %6.3f %6.2f %4.2f/%.2f'
          % (lab, len(rows), med('fps'), med('p50'), med('p90'), med('guest'),
             med('gl'), med('rel'), med('take'), med('lock'), med('zrb'),
             med('zwb')))
