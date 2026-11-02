#!/usr/bin/env python3
"""t_8df70ad4: compare two GLHASH (DQ8_GL_DRAWHASH=1) logs draw by draw.

usage: zz_dhcmp.py <a.err.log> <b.err.log>
Reports how many draws the two runs have, the first draw whose colour or depth
state (pre/post hashes) differ, and how the divergence is distributed.
"""
import sys

FIELDS = ('mode', 'depth', 'box', 'pre', 'post', 'zpre', 'zpost',
          'pre_sum', 'post_sum', 'zpre_sum', 'zpost_sum')


def parse(path):
    out = []
    for ln in open(path, errors='replace'):
        if not ln.startswith('GLHASH draw='):
            continue
        f = {}
        for tok in ln.split():
            if '=' in tok:
                k, v = tok.split('=', 1)
                f[k] = v
        out.append(f)
    return out


a = parse(sys.argv[1])
b = parse(sys.argv[2])
n = min(len(a), len(b))
print('%s: %d draws   %s: %d draws' % (sys.argv[1].split('/')[-2], len(a),
                                       sys.argv[2].split('/')[-2], len(b)))
first_col = first_z = None
ncol = nz = 0
for i in range(n):
    x, y = a[i], b[i]
    if any(x.get(k) != y.get(k) for k in ('pre', 'post', 'pre_sum', 'post_sum')):
        ncol += 1
        if first_col is None:
            first_col = i
    if x.get('zpre') != y.get('zpre') or x.get('zpost') != y.get('zpost'):
        nz += 1
        if first_z is None:
            first_z = i
print('colour divergences: %d/%d (first at draw index %s)' % (ncol, n, first_col))
print('depth  divergences: %d/%d (first at draw index %s)' % (nz, n, first_z))
for lbl, idx in (('first colour', first_col), ('first depth', first_z)):
    if idx is not None:
        x, y = a[idx], b[idx]
        keys = ('draw', 'box', 'depth', 'pre', 'post', 'zpre', 'zpost')
        print('  %s @%d:' % (lbl, idx))
        print('    A ' + ' '.join('%s=%s' % (k, x.get(k)) for k in keys))
        print('    B ' + ' '.join('%s=%s' % (k, y.get(k)) for k in keys))
