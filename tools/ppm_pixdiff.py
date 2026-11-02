#!/usr/bin/env python3
"""t_d559a510: analyze a dumped pre/post draw box pair.

usage: ppm_pixdiff.py <pre.ppm> <post.ppm> [other_post.ppm]

With two post images (arm A / arm B) it reports, per pixel where the arms
differ, the destination (pre), both results, and the implied source value for
the SRC_ALPHA / ONE_MINUS_SRC_ALPHA case.
"""
import sys
from PIL import Image
import collections


def load(p):
    im = Image.open(p).convert('RGB')
    return im.width, im.height, im.tobytes()


def main():
    w, h, pre = load(sys.argv[1])
    wa, ha, pa = load(sys.argv[2])
    assert (w, h) == (wa, ha)
    if len(sys.argv) > 3:
        wb, hb, pb = load(sys.argv[3])
        assert (w, h) == (wb, hb)
        hist = collections.Counter()
        shown = 0
        for i in range(w * h):
            d = tuple(pb[i * 3 + k] - pa[i * 3 + k] for k in range(3))
            if any(d):
                ch = tuple(1 if d[k] else 0 for k in range(3))
                hist[ch] += 1
                if shown < 12:
                    print('px=%-4d,%d pre=%s A=%s B=%s  B-A=%s'
                          % (i % w, i // w,
                             tuple(pre[i * 3:i * 3 + 3]),
                             tuple(pa[i * 3:i * 3 + 3]),
                             tuple(pb[i * 3:i * 3 + 3]), d))
                    shown += 1
        tot = w * h
        print('differing_px=%d/%d (%.2f%%) channels=%s'
              % (sum(hist.values()), tot, 100.0 * sum(hist.values()) / tot,
                 dict(hist)))
        # distribution of per-channel deltas
        dd = collections.Counter()
        for i in range(tot):
            for k in range(3):
                v = pb[i * 3 + k] - pa[i * 3 + k]
                if v:
                    dd[v] += 1
        print('delta_hist(top10)=%s' % dd.most_common(10))
    else:
        n = 0
        for i in range(w * h):
            if pre[i * 3:i * 3 + 3] != pa[i * 3:i * 3 + 3]:
                if n < 12:
                    print('px=%d,%d pre=%s post=%s'
                          % (i % w, i // w, tuple(pre[i * 3:i * 3 + 3]),
                             tuple(pa[i * 3:i * 3 + 3])))
                n += 1
        print('changed_px=%d/%d' % (n, w * h))


main()
