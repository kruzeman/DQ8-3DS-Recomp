#!/usr/bin/env python3
"""vspar_sum.py -- one-line performance summary of a runtime stderr log.

  fps        published frames per wall second (TEMPO swap_per_s)
  guest_ms   mean guest-phase ms per main-loop iteration (FRAMEPROF guest_ms)
  iters/frame  main-loop iterations (PDC0 scanouts) per published frame
  frame_ms   published frame duration = iters/frame * mean total_ms

Window: every FRAMEPROF line with t >= first + SKIP_S (state load and the first
frames dropped), TEMPO lines in the same window.  Same definitions as
hotreg/vl_sum.py so the numbers are comparable with the other cards.
"""
import statistics
import sys

SKIP_S = 3.0
PHASES = ["total", "guest", "gl", "present", "idle", "audio", "other"]


def kv(line):
    return dict(t.split("=", 1) for t in line.split() if "=" in t)


def main():
    if len(sys.argv) < 2:
        print("usage: vspar_sum.py <stderr> [tag]", file=sys.stderr)
        return 2
    path = sys.argv[1]
    tag = sys.argv[2] if len(sys.argv) > 2 else path.rsplit("/", 1)[-1]
    prof, tempo = [], []
    for line in open(path, errors="replace"):
        if line.startswith("FRAMEPROF f="):
            d = kv(line)
            try:
                prof.append((float(d["t"]), d))
            except (KeyError, ValueError):
                pass
        elif line.startswith("TEMPO "):
            d = kv(line)
            try:
                tempo.append((float(d["wall_s"]), d))
            except (KeyError, ValueError):
                pass
    if not prof:
        print("%s\tNO_FRAMEPROF" % tag)
        return 0
    t0 = prof[0][0]
    win = [d for t, d in prof if t - t0 >= SKIP_S]
    if len(win) < 10:
        win = [d for _, d in prof]
    n = len(win)
    el = max(float(win[-1]["t"]) - float(win[0]["t"]), 1e-9)
    tw = [d for w, d in tempo if w >= (t0 + SKIP_S)] or [d for _, d in tempo]
    swap = statistics.mean(float(d["swap_per_s"]) for d in tw) if tw else float("nan")
    pdc0 = statistics.mean(float(d["pdc0_per_s"]) for d in tw) if tw else float("nan")
    vbl = statistics.mean(float(d["vbl_per_s"]) for d in tw) if tw else float("nan")
    ipf = (pdc0 / swap) if (tw and swap > 0) else float("nan")
    means = {p: statistics.mean(float(d.get(p + "_ms", "0")) for d in win) for p in PHASES}
    frame_ms = means["total"] * ipf if ipf == ipf else float("nan")
    print("%-22s fps=%6.2f guest_ms=%6.3f gl_ms=%5.2f present_ms=%5.2f "
          "iters/frame=%5.2f frame_ms=%6.2f pdc0=%5.1f vbl=%5.1f n=%d" %
          (tag, swap, means["guest"], means["gl"], means["present"], ipf,
           frame_ms, pdc0, vbl, n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
