#!/usr/bin/env python3
"""Summarise the t_c043d7f7 cost matrix logs (logs/cost_GL_FILTER_REP.err).

For each arm (GL x filter) and each rep, sum the FRAMEPROF frames that actually
rendered (total_ms >= 5, i.e. frames carrying draw work) and report:
  frames, sum(guest_ms), sum(gl_ms), mean(guest_ms/frame), mean(gl_ms/frame).
Then the per-arm MEDIAN over the 3 interleaved reps.  Prints a markdown table
ready for reports/TEXTURE_FILTER.md.
"""
import glob
import os
import statistics

ARMS = [(0, 0), (0, 1), (1, 0), (1, 1)]
ROWS = {}


def load(path):
    g = gl = tot = 0.0
    n = 0
    for line in open(path, errors="replace"):
        if not line.startswith("FRAMEPROF f="):
            continue
        d = dict(tok.split("=", 1) for tok in line.split() if "=" in tok)
        t = float(d["total_ms"])
        if t < 5.0:
            continue
        n += 1
        tot += t
        g += float(d["guest_ms"])
        gl += float(d["gl_ms"])
    return n, g, gl, tot


for gl, f in ARMS:
    key = "GL=%d TEX_FILTER=%d" % (gl, f)
    rows = []
    for rep in (1, 2, 3):
        path = "logs/cost_%d_%d_%d.err" % (gl, f, rep)
        if os.path.exists(path):
            rows.append(load(path))
    if not rows:
        continue
    n = [r[0] for r in rows]
    gm = [r[1] / r[0] for r in rows if r[0]]
    lm = [r[2] / r[0] for r in rows if r[0]]
    tm = [r[3] / r[0] for r in rows if r[0]]
    ROWS[key] = (statistics.median(n), statistics.median(gm),
                 statistics.median(lm), statistics.median(tm))

print("| arm | render frames | guest_ms/frame | gl_ms/frame | total_ms/frame |")
print("|---|---|---|---|---|")
for key, (n, gm, lm, tm) in ROWS.items():
    print("| %s | %.0f | %.2f | %.2f | %.2f |" % (key, n, gm, lm, tm))

print("\nsame-binary deltas (median over 3 interleaved reps):")
for gl in (0, 1):
    a = ROWS.get("GL=%d TEX_FILTER=0" % gl)
    b = ROWS.get("GL=%d TEX_FILTER=1" % gl)
    if a and b:
        print("GL=%d: guest_ms/frame %+.1f%%, gl_ms/frame %+.1f%%, "
              "total_ms/frame %+.1f%%" %
              (gl, 100 * (b[1] / a[1] - 1), 100 * (b[2] / a[2] - 1) if a[2] else 0,
               100 * (b[3] / a[3] - 1)))
