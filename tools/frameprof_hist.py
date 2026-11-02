#!/usr/bin/env python3
"""Frame-time histogram from a DQ8_FRAMEPROF=1 log (t_5a3c517a).

Reads `FRAMEPROF f=<n> t=<s> total_ms=<x> guest_ms=.. gl_ms=.. present_ms=..`
lines and reports the frame-time distribution the operator cares about:
percentiles, the count of frames over the 30 fps slot (33.3 ms) and over the
50 ms acceptance bar, plus the worst frames with their per-phase split.

usage: tools/frameprof_hist.py LOG [LOG...]
"""
import re
import sys

RE = re.compile(
    r"^FRAMEPROF f=(\d+) t=([\d.]+) total_ms=([\d.]+) guest_ms=([\d.]+) "
    r"gl_ms=([\d.]+) present_ms=([\d.]+)")


def pct(vals, p):
    if not vals:
        return 0.0
    k = (len(vals) - 1) * p / 100.0
    lo = int(k)
    hi = min(lo + 1, len(vals) - 1)
    return vals[lo] + (vals[hi] - vals[lo]) * (k - lo)


def one(path):
    rows = []
    with open(path, "r", errors="replace") as f:
        for line in f:
            m = RE.match(line)
            if m:
                rows.append((int(m.group(1)), float(m.group(2)),
                             float(m.group(3)), float(m.group(4)),
                             float(m.group(5)), float(m.group(6))))
    if not rows:
        print(f"{path}: no FRAMEPROF lines")
        return
    span = rows[-1][1] - rows[0][1]
    totals = sorted(r[2] for r in rows)
    guest = sum(r[3] for r in rows) / len(rows)
    print(f"== {path}")
    print(f"   frames={len(rows)} span={span:.1f}s fps={len(rows)/span:.2f} "
          f"guest_mean={guest:.2f}ms")
    print(f"   p50={pct(totals,50):.2f} p90={pct(totals,90):.2f} "
          f"p95={pct(totals,95):.2f} p99={pct(totals,99):.2f} "
          f"p999={pct(totals,99.9):.2f} max={totals[-1]:.2f} ms")
    over = {t: sum(1 for v in totals if v > t) for t in (16.7, 33, 50, 80, 100, 150)}
    print("   frames over: " + "  ".join(
        f">{k}ms={v} ({100.0*v/len(totals):.1f}%)" for k, v in over.items()))
    worst = sorted(rows, key=lambda r: -r[2])[:8]
    for r in worst:
        print(f"   worst f={r[0]} t={r[1]:.2f} total={r[2]:.1f} "
              f"guest={r[3]:.1f} gl={r[4]:.2f} present={r[5]:.2f}")


if __name__ == "__main__":
    for p in sys.argv[1:]:
        one(p)
