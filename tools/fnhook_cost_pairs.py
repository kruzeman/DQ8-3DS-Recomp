#!/usr/bin/env python3
"""Per-repetition ns/entry for the fnhook_cost arms, plus the amplified
(16x) probe costs.  Every arm runs back to back inside one repetition, so the
paired difference against mode 0 (or against the null-call control) removes
slow drift; the sd over repetitions is the honest error bar.

  mode 0  no check                       mode 1  new check (inline, 1x)
  mode 6  bitmap test x16 inline         -> (mode6 - mode0)/16 = bitmap test
  mode 7  page-check call x16            -> (mode7 - mode9)/16 = page check
  mode 8  flat-check call x16            -> (mode8 - mode9)/16 = flat check
  mode 9  null call x16 (control)
"""
import glob
import os
import re
import statistics
import sys

out = sys.argv[1] if len(sys.argv) > 1 else "target/fnhook-cost"
N = 16
data = {}
for path in sorted(glob.glob(os.path.join(out, "m*-*.err"))):
    m = re.match(r"m(\d+)-(\d+)\.err", os.path.basename(path))
    if not m:
        continue
    mode, rep = int(m.group(1)), int(m.group(2))
    gsum = 0.0
    frames = 0
    entries = None
    for line in open(path, errors="replace"):
        if line.startswith("FRAMEPROF "):
            g = re.search(r"guest_ms=([0-9.]+)", line)
            t = re.search(r"total_ms=([0-9.]+)", line)
            if g and t and float(t.group(1)) > 2.0:
                gsum += float(g.group(1))
                frames += 1
        elif line.startswith("FN_HOOK "):
            entries = int(re.search(r"entries=(\d+)", line).group(1))
    if frames and entries:
        data[(mode, rep)] = gsum / frames * 1e6 / (entries / frames)

reps = sorted({r for (_, r) in data})
modes = sorted({m for (m, _) in data})
print("ns per dispatch entry (whole run: dispatch + check)")
print("  rep  " + "".join("   mode%-2d" % m for m in modes))
for r in reps:
    print("  %3d  " % r + "".join("%9.2f" % data[(m, r)] if (m, r) in data else "        -" for m in modes))


def paired(a, b, scale=1.0):
    d = [data[(a, r)] - data[(b, r)] for r in reps if (a, r) in data and (b, r) in data]
    if len(d) < 2:
        return None
    return statistics.mean(d) / scale, statistics.stdev(d) / scale, d


print("\npaired differences, ns/entry (mean, sd over %d reps)" % len(reps))
for m in modes:
    if m == 0:
        continue
    res = paired(m, 0)
    if res:
        print("  mode %-2d - mode 0: %+9.2f  sd %7.2f  per-rep %s"
              % (m, res[0], res[1], ["%+.1f" % x for x in res[2]]))

print("\namplified probes (16 repetitions per entry)")
res = paired(6, 0, N)
if res:
    print("  bitmap test inline:      %+7.3f ns/check  (sd %.3f)  per-rep %s"
          % (res[0], res[1], ["%+.2f" % (x / N) for x in res[2]]))
res = paired(7, 9, N)
if res:
    print("  page-grouped switch:     %+7.3f ns/check  (sd %.3f)  per-rep %s"
          % (res[0], res[1], ["%+.2f" % (x / N) for x in res[2]]))
res = paired(8, 9, N)
if res:
    print("  flat switch:             %+7.3f ns/check  (sd %.3f)  per-rep %s"
          % (res[0], res[1], ["%+.2f" % (x / N) for x in res[2]]))
