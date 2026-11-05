#!/usr/bin/env python3
"""Aggregate DQ8_FRAMEPROF lines into render-frame cost.

usage: texfilter_cost.py LABEL LOG [min_total_ms]

Reports, over the frames that actually rendered (total_ms >= min_total_ms,
default 5), the count, the mean/median guest_ms and gl_ms, and the sum.  A
frame with no draw work is 0.003 ms and would otherwise drown the mean."""
import statistics
import sys


def main(label, path, min_total=5.0):
    guest = []
    gl = []
    total = []
    for line in open(path, errors="replace"):
        if not line.startswith("FRAMEPROF f="):
            continue
        d = dict(tok.split("=", 1) for tok in line.split() if "=" in tok)
        t = float(d["total_ms"])
        if t < min_total:
            continue
        total.append(t)
        guest.append(float(d["guest_ms"]))
        gl.append(float(d["gl_ms"]))
    if not total:
        print("%-14s no render frames" % label)
        return
    print("%-14s frames=%3d total_mean=%8.2f guest_mean=%8.2f "
          "guest_med=%8.2f gl_mean=%8.2f gl_med=%8.2f" %
          (label, len(total), statistics.mean(total),
           statistics.mean(guest), statistics.median(guest),
           statistics.mean(gl), statistics.median(gl)))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2],
         float(sys.argv[3]) if len(sys.argv) > 3 else 5.0)
