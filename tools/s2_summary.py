#!/usr/bin/env python3
"""t_6edf5d17: summarise tools/s2_ab.sh JSONL (arms labelled ...-A-<i> ...-B-<i>)."""
import json, sys, statistics

path = sys.argv[1]
rows = [json.loads(l) for l in open(path) if l.strip()]


def arm(r):
    lab = r["label"]
    if "-A-" in lab:
        return "A"
    if "-B-" in lab:
        return "B"
    return "?"


def fmt(v):
    return "%.3f" % v if isinstance(v, float) else str(v)


groups = {}
for r in rows:
    groups.setdefault(arm(r), []).append(r)

print("runs: %d  (A=%d B=%d)  file=%s" % (len(rows), len(groups.get("A", [])),
                                           len(groups.get("B", [])), path))
metrics = [("insn_per_s_M", "guest insn/s (fixed work)"),
           ("aggregate_insn_per_s_M", "aggregate insn/s"),
           ("fps", "fps"),
           ("ms_per_frame", "ms/frame"),
           ("proc_main_thread_cpu_s", "dispatch-thread CPU s / run"),
           ("proc_cpu_s", "process CPU s / run"),
           ("proc_cores_gt10pct", "cores >10%"),
           ("system_busy_cores_avg", "system busy cores"),
           ("draw_ms", "draw_ms (FRAMESTAT)")]
for key, label in metrics:
    a = [r[key] for r in groups.get("A", []) if key in r]
    b = [r[key] for r in groups.get("B", []) if key in r]
    if not a or not b:
        continue
    ma, mb = statistics.median(a), statistics.median(b)
    delta = 100.0 * (mb / ma - 1.0) if ma else float("nan")
    print("%-28s A med=%10s (n=%d, %s..%s)  B med=%10s (n=%d, %s..%s)  %+.1f%%"
          % (label, fmt(round(ma, 3)), len(a), fmt(round(min(a), 3)), fmt(round(max(a), 3)),
             fmt(round(mb, 3)), len(b), fmt(round(min(b), 3)), fmt(round(max(b), 3)), delta))

# paired per-pair ratios
pairs = {}
for r in rows:
    idx = r["label"].rsplit("-", 1)[-1]
    pairs.setdefault(idx, {})[arm(r)] = r
print()
for idx in sorted(pairs, key=lambda x: int(x) if x.isdigit() else 0):
    p = pairs[idx]
    if "A" not in p or "B" not in p:
        continue
    a, b = p["A"], p["B"]
    parts = []
    for key in ("insn_per_s_M", "fps", "proc_main_thread_cpu_s"):
        if key in a and key in b and a[key]:
            parts.append("%s %s->%s (%+.1f%%)" % (key, fmt(a[key]), fmt(b[key]),
                                                  100.0 * (b[key] / a[key] - 1.0)))
    print("pair %s: %s" % (idx, " | ".join(parts)))

# per-thread breakdown of the dispatch thread in fps mode
for r in rows:
    if r.get("mode") == "fps":
        th = r.get("threads") or []
        if th:
            t = th[0]
            print("  %-12s hottest thread %-14s cpu=%.2fs duty=%.1f%%  cores>10%%=%s"
                  % (r["label"], t["comm"], t["cpu_s"], t["duty_pct"],
                     r.get("proc_cores_gt10pct")))
