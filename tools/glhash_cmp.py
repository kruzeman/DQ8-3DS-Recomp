#!/usr/bin/env python3
"""t_d559a510: align two DQ8_GL_DRAWHASH logs and report blend divergence.

usage: glhash_cmp.py <logA> <logB> [max_report]

Each GLHASH line carries draw index, the pre/post box hashes and the byte sums.
The arms are aligned by draw index; a line pair with equal pre but different
post means the draw's evaluation differed.  The byte-sum delta estimates the
magnitude, so the first *significant* divergence is the one to dump.
"""
import re
import sys

PAT = re.compile(
    r'GLHASH draw=(\d+) tris=(\d+) mode=(\d+) blend=(\d+) glb=(\d+) '
    r'src=(\d+),(\d+) dst=(\d+),(\d+) eq=(\d+),(\d+) a=(\d+)/(\d+)/(\S+) '
    r'mask=(\w+) depth=(\d+)/(\d+)/(\d+) box=(-?\d+),(-?\d+)\.\.(-?\d+),(-?\d+) '
    r'pre=([0-9a-f]+) post=([0-9a-f]+) pre_sum=(\d+) post_sum=(\d+)')


def load(path):
    out = {}
    for line in open(path, errors='replace'):
        m = PAT.search(line)
        if m:
            g = m.groups()
            out[int(g[0])] = {
                'tris': g[1], 'mode': g[2], 'blend': g[3], 'glb': g[4],
                'src': g[5] + ',' + g[6], 'dst': g[7] + ',' + g[8],
                'eq': g[9] + ',' + g[10], 'alpha': '%s/%s/%s' % (g[11], g[12], g[13]),
                'mask': g[14], 'depth': '%s/%s/%s' % (g[15], g[16], g[17]),
                'box': ','.join(g[18:22]),
                'pre': g[22], 'post': g[23],
                'pre_sum': int(g[24]), 'post_sum': int(g[25]),
            }
    return out


def main():
    a = load(sys.argv[1])
    b = load(sys.argv[2])
    lim = int(sys.argv[3]) if len(sys.argv) > 3 else 20
    keys = sorted(set(a) & set(b))
    n_diff = 0
    n_pre_diff = 0
    shown = 0
    for k in keys:
        x, y = a[k], b[k]
        if x['pre'] != y['pre']:
            n_pre_diff += 1
            if n_pre_diff <= 3:
                print('draw=%d PRE differs (upstream state diverged): %016s vs %016s'
                      % (k, x['pre'], y['pre']))
            continue
        if x['post'] != y['post']:
            n_diff += 1
            d = y['post_sum'] - x['post_sum']
            if shown < lim:
                print('draw=%d post differs dsum=%+d src=%s dst=%s eq=%s mask=%s '
                      'alpha=%s depth=%s box=%s tris=%s mode=%s'
                      % (k, d, x['src'], x['dst'], x['eq'], x['mask'],
                         x['alpha'], x['depth'], x['box'], x['tris'], x['mode']))
                shown += 1
    print('common_draws=%d pre_diverged=%d post_diverged=%d' % (len(keys), n_pre_diff, n_diff))


main()
