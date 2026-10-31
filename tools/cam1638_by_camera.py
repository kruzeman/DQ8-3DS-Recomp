#!/usr/bin/env python3
"""t_41f38df0: correlate count=1638 draws with the camera they got.

Reads a probe run.log and prints, per camera bucket (UNI 25-27 .w = eye), how
many count=1638 draws rendered (tris>0) vs were fully clipped, with the time
range — decides whether the cutscene camera is uniformly wrong or only during
part of the dialog.

usage: cam1638_by_camera.py <run.log>
"""
import re
import sys

path = sys.argv[1]
uni_re = re.compile(r"^UNICAM t=([0-9.]+) draw=(\d+) count=(1638) .*?M=\[[^]]*?,([+-][0-9.]+)\|[^]]*?,([+-][0-9.]+)\|[^]]*?,([+-][0-9.]+)\]")
# M is 3 rows of 4; the translation is the 4th component of each row
m_re = re.compile(r"M=\[(.*?)\]\s")
draw_re = re.compile(r"^DRAW t=([0-9.]+) \S+ draw=(\d+) count=(\d+) tris=(\d+) .*clip=(\d+) wmin=([-\d.e+]+) wmax=([-\d.e+]+)")

cam = {}      # draw id -> (t, eye tuple)
buckets = {}
with open(path, "r", errors="replace") as fh:
    for line in fh:
        if line.startswith("UNICAM") and "count=1638" in line:
            t = float(line.split("t=", 1)[1].split()[0])
            did = int(line.split("draw=", 1)[1].split()[0])
            mm = m_re.search(line)
            if mm:
                rows = mm.group(1).split("|")
                eye = tuple(float(r.split(",")[3]) for r in rows)
                cam[did] = (t, eye)
        elif line.startswith("DRAW") and "count=1638" in line:
            dm = draw_re.match(line)
            if not dm:
                continue
            did = int(dm.group(2))
            if did not in cam:
                continue
            t, eye = cam[did]
            tris = int(dm.group(4))
            key = tuple(round(v, 2) for v in eye)
            b = buckets.setdefault(key, [0, 0, t, t, [], []])
            b[0 if tris else 1] += 1
            b[2] = min(b[2], t)
            b[3] = max(b[3], t)
            (b[4] if tris else b[5]).append(tris)

print(f"count=1638 draws grouped by eye = UNI25-27.w   ({path})")
print(f"{'eye':>38}  {'render':>6} {'clip0':>6}  t-range        tris(rendered)")
for key, b in sorted(buckets.items(), key=lambda kv: -kv[1][0]):
    rendered, clipped, t0, t1, tr, tc = b
    sample = ",".join(str(x) for x in sorted(set(tr))[:6]) or "-"
    print(f"({key[0]:>10.3f},{key[1]:>9.3f},{key[2]:>10.3f})  {rendered:6d} {clipped:6d}  "
          f"{t0:7.2f}-{t1:6.2f}  {sample}")
print(f"distinct cameras seen by count=1638: {len(buckets)}")
