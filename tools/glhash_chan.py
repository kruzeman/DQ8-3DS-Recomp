#!/usr/bin/env python3
"""t_d559a510: attribute per-draw box differences to RGB vs alpha.

usage: glhash_chan.py <arm-a.log> <arm-b.log> [max_print]

For every draw whose *starting* content is identical in both arms, compares
pre_sum/post_sum (all four channels) and the alpha sums, and reports how much of
the byte-sum delta is alpha-only.
"""
import re
import sys

LINE = re.compile(
    r"GLHASH draw=(?P<draw>\d+).*?box=(?P<box>[-.\d,]+) pre=(?P<pre>[0-9a-f]+) "
    r"post=(?P<post>[0-9a-f]+) pre_sum=(?P<psum>\d+) post_sum=(?P<qsum>\d+)"
    r".*?zpre=(?P<zpre>[0-9a-f]+) zpost=(?P<zpost>[0-9a-f]+)"
    r".*?asum=(?P<apsum>\d+)/(?P<aqsum>\d+)")


def load(p):
    d = {}
    for ln in open(p, errors="replace"):
        m = LINE.search(ln)
        if m:
            g = m.groupdict()
            d[int(g["draw"])] = g
    return d


def main():
    A, B = load(sys.argv[1]), load(sys.argv[2])
    lim = int(sys.argv[3]) if len(sys.argv) > 3 else 6
    common = sorted(set(A) & set(B))
    rgb_only = 0
    alpha_only = 0
    both = 0
    shown = 0
    for n in common:
        a, b = A[n], B[n]
        if a["pre"] != b["pre"] or a["apsum"] != b["apsum"]:
            continue                     # not comparable (cascade)
        dtot = abs(int(a["qsum"]) - int(b["qsum"]))
        if not dtot:
            continue
        da = abs(int(a["aqsum"]) - int(b["aqsum"]))
        drgb = abs(int(a["qsum"]) - int(b["qsum"]))
        if da == 0:
            rgb_only += 1
            kind = "RGB"
        elif da == dtot:
            alpha_only += 1
            kind = "ALPHA-only"
        else:
            both += 1
            kind = "RGB+alpha"
        if shown < lim:
            print("draw %5d %-10s |dsum|=%7d  alpha delta=%7d  box=%s"
                  % (n, kind, dtot, da, a["box"]))
            shown += 1
    print("\ncomparable draws with a difference: alpha-only=%d  rgb-only=%d  "
          "rgb+alpha=%d" % (alpha_only, rgb_only, both))


main()
