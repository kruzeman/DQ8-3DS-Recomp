#!/usr/bin/env python3
"""Bucket HIDREAD trace lines by phase and report read rate + top PCs.

Usage: hidread_profile.py run.log
Phases are derived from the trace's own t= seconds: pre (<60), post-skip
fade (60..150), black (>150).
"""
import re
import sys
from collections import Counter

log = open(sys.argv[1], errors="replace").read().splitlines()
pat = re.compile(r"HIDREAD t=(\d+) th=(\d+) pc=([0-9A-Fa-f]+) addr=([0-9A-Fa-f]+) off=([0-9A-Fa-f]+)")
phases = {"pre(<60)": Counter(), "fade(60-150)": Counter(), "black(>150)": Counter()}
counts = Counter()
tmax = 0
for l in log:
    m = pat.search(l)
    if not m:
        continue
    t = int(m.group(1))
    tmax = max(tmax, t)
    pc = m.group(3).upper()
    off = m.group(5).upper()
    key = f"pc={pc} off={off}"
    if t < 60:
        phases["pre(<60)"][key] += 1
    elif t <= 150:
        phases["fade(60-150)"][key] += 1
    else:
        phases["black(>150)"][key] += 1
    counts[t] += 1
print(f"trace tmax={tmax}s total_reads={sum(counts.values())}")
for name, c in phases.items():
    total = sum(c.values())
    print(f"== {name}: {total} reads; top PCs:")
    for k, n in c.most_common(6):
        print(f"   {k} x{n}")
print("reads per second (t: count):")
for t in sorted(counts):
    print(f"  {t}:{counts[t]}", end="")
print()
