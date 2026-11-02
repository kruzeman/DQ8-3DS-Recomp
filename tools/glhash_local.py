#!/usr/bin/env python3
"""t_d559a510: LOCALISED per-draw A/B (colour + emulated depth).

Difference from glhash_zcmp.py: a draw only counts as diverging when its
*starting* state is identical in both arms (pre == pre AND zpre == zpre).  A
difference in the incoming content means the divergence happened earlier
("cascade") and is not evidence about this draw's own evaluation.

usage: glhash_local.py <arm-a.log> <arm-b.log> [max_print]
exit 0 = no local colour divergence beyond the rounding class and no local
depth divergence; 1 otherwise.
"""
import re
import sys
from collections import Counter

LINE = re.compile(
    r"GLHASH draw=(?P<draw>\d+) tris=(?P<tris>\d+) mode=(?P<mode>-?\d+) "
    r"blend=(?P<blend>-?\d+) glb=(?P<glb>-?\d+) "
    r"src=(?P<srcr>\d+),(?P<srca>\d+) dst=(?P<dstr>\d+),(?P<dsta>\d+) "
    r"eq=(?P<eqr>\d+),(?P<eqa>\d+) a=(?P<at>-?\d+)/(?P<af>\d+)/(?P<aref>[-.\d]+) "
    r"mask=(?P<mask>[0-9A-Fa-f]+) depth=(?P<dt>-?\d+)/(?P<dw>-?\d+)/(?P<don>-?\d+) "
    r"box=(?P<box>[-.\d,]+) pre=(?P<pre>[0-9a-f]+) post=(?P<post>[0-9a-f]+) "
    r"pre_sum=(?P<psum>\d+) post_sum=(?P<qsum>\d+)"
    r"(?: zpre=(?P<zpre>[0-9a-f]+) zpost=(?P<zpost>[0-9a-f]+) "
    r"zpre_sum=(?P<zpsum>\d+) zpost_sum=(?P<zqsum>\d+))?")


def load(path):
    out = {}
    with open(path, "r", errors="replace") as f:
        for ln in f:
            m = LINE.search(ln)
            if m:
                d = m.groupdict()
                out[int(d["draw"])] = d
    return out


def cfg(d):
    return ("tris=%s src=%s,%s dst=%s,%s eq=%s,%s mask=%s depth=%s/%s/%s"
            % (d["tris"], d["srcr"], d["srca"], d["dstr"], d["dsta"],
               d["eqr"], d["eqa"], d["mask"], d["dt"], d["dw"], d["don"]))


def main():
    A, B = load(sys.argv[1]), load(sys.argv[2])
    max_print = int(sys.argv[3]) if len(sys.argv) > 3 else 6
    common = sorted(set(A) & set(B))
    if not common:
        print("no common draw indices")
        return 2
    loc_c = 0
    loc_c_big = []
    c_same = c_round = c_big = 0
    dsum_max = 0
    loc_d = 0
    loc_d_bad = []
    d_same = d_cmp = 0
    first_c = first_d = None
    cascade_pre = 0
    for n in common:
        a, b = A[n], B[n]
        if a["pre"] != b["pre"] or a.get("zpre") != b.get("zpre"):
            cascade_pre += 1
            continue
        dsum = abs(int(a["qsum"]) - int(b["qsum"]))
        dsum_max = max(dsum_max, dsum)
        if a["post"] == b["post"]:
            c_same += 1
        else:
            loc_c += 1
            if dsum <= 64:
                c_round += 1
            else:
                c_big += 1
                if len(loc_c_big) < max_print:
                    loc_c_big.append((n, a, b, dsum))
            if first_c is None:
                first_c = (n, a, b, dsum)
        if a.get("zpost") and b.get("zpost"):
            d_cmp += 1
            if a["zpost"] == b["zpost"]:
                d_same += 1
            else:
                loc_d += 1
                if first_d is None:
                    first_d = (n, a, b)
                if len(loc_d_bad) < max_print:
                    loc_d_bad.append((n, a, b))
    print("draws A=%d B=%d common=%d  (skipped as pre-diverged/cascade: %d)"
          % (len(A), len(B), len(common), cascade_pre))
    print("colour, same starting content: identical=%d  rounding<=64=%d  "
          "DIVERGENT=%d  max|dsum|=%d" % (c_same, c_round, c_big, dsum_max))
    print("depth , same starting content: compared=%d  identical=%d  DIVERGENT=%d"
          % (d_cmp, d_same, loc_d))
    if first_c:
        n, a, b, dsum = first_c
        print("\nfirst colour difference: draw %d %s  |dsum|=%d" % (n, cfg(a), dsum))
        print("  A post=%s sum=%s" % (a["post"], a["qsum"]))
        print("  B post=%s sum=%s" % (b["post"], b["qsum"]))
    if first_d:
        n, a, b = first_d
        print("\nfirst DEPTH difference: draw %d %s" % (n, cfg(a)))
        print("  box=%s zpre=%s (same in both)" % (a["box"], a["zpre"]))
        print("  A zpost=%s sum=%s" % (a["zpost"], a["zqsum"]))
        print("  B zpost=%s sum=%s" % (b["zpost"], b["zqsum"]))
    for n, a, b, dsum in loc_c_big:
        print("\ncolour divergence draw %d %s |dsum|=%d" % (n, cfg(a), dsum))
    for n, a, b in loc_d_bad:
        print("\ndepth divergence draw %d %s box=%s" % (n, cfg(a), a["box"]))
    return 0 if (c_big == 0 and loc_d == 0) else 1


if __name__ == "__main__":
    sys.exit(main())
