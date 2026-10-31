#!/usr/bin/env python3
"""t_3de68379: summary for tools/codegen_ab.sh (interleaved A/B runs).

usage: tools/codegen_ab_summary.py <ab-dir> <tagA> <tagB>

Reports fps / ms-per-frame per run, the per-arm median, and the relative
delta.  Only windows after the first TIME window are counted (the first one
contains the savestate restore and the first texture uploads).
"""
import os
import re
import statistics
import sys

out, ta, tb = sys.argv[1:4]


def runs(tag, pairs):
    vals = []
    for i in range(1, pairs + 1):
        p = os.path.join(out, "%s-%d" % (tag, i), "run.log")
        if not os.path.exists(p):
            continue
        txt = open(p, errors="replace").read()
        w = re.findall(r"TIME window=([\d.]+)s frames=(\d+)", txt)
        if len(w) < 3:
            vals.append((tag, i, None, None, "no TIME windows"))
            continue
        w = w[1:]                       # drop warmup window
        wall = sum(float(a) for a, b in w)
        fr = sum(int(b) for a, b in w)
        if wall <= 0 or fr < 50:
            vals.append((tag, i, None, None, "too few frames (%d)" % fr))
            continue
        vals.append((tag, i, fr / wall, 1000.0 * wall / fr, "ok"))
    return vals


def median_fps(vals):
    f = [v[2] for v in vals if v[2]]
    return statistics.median(f) if f else None


pairs = 0
for name in os.listdir(out):
    m = re.match(r".*-(\d+)$", name)
    if m:
        pairs = max(pairs, int(m.group(1)))

va = runs(ta, pairs)
vb = runs(tb, pairs)
print("%-14s %3s %8s %9s" % ("run", "", "fps", "ms/frame"))
for v in va + vb:
    if v[2]:
        print("%-14s %8.3f %9.1f" % (v[0] + "-" + str(v[1]), v[2], v[3]))
    else:
        print("%-14s %8s %9s   %s" % (v[0] + "-" + str(v[1]), "-", "-", v[4]))
fa, fb = median_fps(va), median_fps(vb)
print()
print("median fps  %-14s %s" % (ta, "%.3f" % fa if fa else "n/a"))
print("median fps  %-14s %s" % (tb, "%.3f" % fb if fb else "n/a"))
if fa and fb:
    print("delta       %+.2f %%  (%s vs %s)" % (100.0 * (fb - fa) / fa, tb, ta))
sys.exit(0)
