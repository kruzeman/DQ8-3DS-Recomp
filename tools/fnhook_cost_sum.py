#!/usr/bin/env python3
"""Summarise tools/fnhook_cost.sh arms: ns per dispatch entry, measured.

ns/entry is (guest_ms per frame * 1e6) / (FN_HOOK entries per frame): the two
counters come from the same run, and guest_ms is the PF_GUEST wall-clock zone
accumulated per frame.  Modes 0/1/3/4/5 retire the same instruction stream, so
their entry counts are directly comparable; mode 2 dispatches bodies and has
fewer entries per frame.
"""
import glob
import os
import re
import statistics
import sys

out = sys.argv[1] if len(sys.argv) > 1 else "target/fnhook-cost"
NAME = {0: "off (no check)", 1: "bitmap+hash check", 2: "bitmap+hash+dispatch",
        3: "page-grouped check", 4: "flat switch check", 5: "null noinline call"}


def arm(path):
    frames = 0
    gsum = 0.0
    entries = None
    dispatched = None
    probe_hits = None
    bench = {}
    for line in open(path, errors="replace"):
        if line.startswith("FRAMEPROF "):
            m = re.search(r"guest_ms=([0-9.]+)", line)
            t = re.search(r"total_ms=([0-9.]+)", line)
            if m and t and float(t.group(1)) > 2.0:
                gsum += float(m.group(1))
                frames += 1
        elif line.startswith("FN_HOOK "):
            kv = dict(re.findall(r"(\w+)=(\d+)", line))
            entries = int(kv["entries"])
            dispatched = int(kv.get("dispatched", 0))
            probe_hits = int(kv.get("probe_hits", 0))
        elif line.startswith("BENCH "):
            bench = dict(re.findall(r"(\w+)=([0-9.]+)", line))
    if not frames or entries is None:
        return None
    gpf = gsum / frames
    return dict(frames=frames, guest_ms=gpf, entries=entries,
                entries_per_frame=entries / frames,
                ns_per_entry=gpf * 1e6 / (entries / frames),
                dispatched=dispatched, probe_hits=probe_hits,
                wall=float(bench.get("wall", 0.0)))


rows = {}
for path in sorted(glob.glob(os.path.join(out, "m*-*.err"))):
    base = os.path.basename(path)
    m = re.match(r"m(\d+)-(\d+)\.err", base)
    if not m:
        continue
    rows.setdefault(int(m.group(1)), []).append(arm(path))

print("arm                                   reps  entries/frame     guest_ms/frame      ns/entry")
for mode in (0, 1, 3, 4, 5, 2):
    if mode not in rows:
        continue
    vals = [r for r in rows[mode] if r]
    if not vals:
        continue
    e = statistics.mean(v["entries_per_frame"] for v in vals)
    g = statistics.mean(v["guest_ms"] for v in vals)
    n = statistics.mean(v["ns_per_entry"] for v in vals)
    rng = ""
    if len(vals) > 1:
        ns = [v["ns_per_entry"] for v in vals]
        rng = "  [%.3f..%.3f]" % (min(ns), max(ns))
    print("mode %d %-28s %4d %14.1f %16.3f %10.3f%s"
          % (mode, NAME[mode], len(vals), e, g, n, rng))

if 0 in rows and 1 in rows:
    a = statistics.mean(v["ns_per_entry"] for v in rows[0] if v)
    b = statistics.mean(v["ns_per_entry"] for v in rows[1] if v)
    print("\nnew check, ns/entry = %.3f  (mode 1 - mode 0)" % (b - a))
if 5 in rows:
    nul = statistics.mean(v["ns_per_entry"] for v in rows[5] if v)
    base = statistics.mean(v["ns_per_entry"] for v in rows[0] if v) if 0 in rows else 0.0
    print("noinline call overhead, ns/entry = %.3f  (mode 5 - mode 0)" % (nul - base))
    for mode in (3, 4):
        if mode in rows:
            v = statistics.mean(x["ns_per_entry"] for x in rows[mode] if x)
            print("%s: %.3f ns/entry, minus call overhead = %.3f"
                  % (NAME[mode], v, v - nul))
if 2 in rows:
    v = [x for x in rows[2] if x]
    print("production arm: entries/frame=%.0f dispatched/frame=%.0f"
          % (statistics.mean(x["entries_per_frame"] for x in v),
             statistics.mean(x["dispatched"] / x["frames"] for x in v)))
