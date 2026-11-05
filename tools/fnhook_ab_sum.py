#!/usr/bin/env python3
"""Summarise tools/fnhook_ab.sh: per-arm guest_ms range and the b35 vs new2800
comparison.  Prints min/mean/max over the interleaved runs, so the reader can
see whether the ranges are disjoint."""
import statistics
import sys
from collections import defaultdict

tsv = sys.argv[1] if len(sys.argv) > 1 else "target/fnhook-ab/runs.tsv"
rows = defaultdict(list)
frames = defaultdict(list)
for line in open(tsv):
    parts = line.rstrip("\n").split("\t")
    if len(parts) < 5:
        continue
    scene, tag, rep, mean = parts[0], parts[1], parts[2], parts[3]
    try:
        v = float(mean)
    except ValueError:
        continue
    rows[(scene, tag)].append(v)
    frames[(scene, tag)].append(parts[4])

order = ["b41", "b35off", "off", "b35", "new300", "new2800"]
for scene in ("field", "battle"):
    print("\n%s: guest_ms mean per frame, %d interleaved runs" %
          (scene, max(len(v) for (s, t), v in rows.items() if s == scene) if rows else 0))
    print("  arm      runs     min     mean      max   ranges-disjoint-vs-b35")
    for tag in order:
        v = rows.get((scene, tag))
        if not v:
            continue
        b35 = rows.get((scene, "b35"), [])
        note = ""
        if tag != "b35" and b35:
            note = "yes" if max(v) < min(b35) else ("worse" if min(v) > max(b35) else "overlap")
        print("  %-8s %4d  %6.3f  %6.3f  %6.3f   %s" %
              (tag, len(v), min(v), statistics.mean(v), max(v), note))
    b35 = rows.get((scene, "b35"))
    n28 = rows.get((scene, "new2800"))
    if b35 and n28:
        print("  b35 -> new2800: %+.1f %% (%+.3f ms)" % (
            100.0 * (statistics.mean(n28) - statistics.mean(b35)) / statistics.mean(b35),
            statistics.mean(n28) - statistics.mean(b35)))
