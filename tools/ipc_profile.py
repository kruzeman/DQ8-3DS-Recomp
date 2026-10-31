#!/usr/bin/env python3
"""Per-snapshot IPC activity profile of a DQ8 run.log.

Splits the log at periodic "guest snapshot" markers and counts guest IPC
commands (handle/command pairs) in each interval, printing every Nth interval.
Usage: ipc_profile.py run.log [step]
"""
import re
import sys
from collections import Counter

path = sys.argv[1]
step = int(sys.argv[2]) if len(sys.argv) > 2 else 10
log = open(path, errors="replace").read().splitlines()
snap_idx = [i for i, l in enumerate(log) if l.startswith("guest snapshot")]
print(f"snapshots: {len(snap_idx)} lines: {len(log)}")
pat = re.compile(r"handle=([0-9A-F]+) command=([0-9A-F]+)")
prev = 0
rows = []
for si in snap_idx:
    c = Counter()
    for l in log[prev:si]:
        m = pat.search(l)
        if m:
            c[(m.group(1), m.group(2))] += 1
    rows.append((si, sum(c.values()), c.most_common(3)))
    prev = si + 1
for r in rows[::step]:
    top = " ".join(f"{h}:{cmd}x{n}" for (h, cmd), n in r[2])
    print(f"line={r[0]} ipc_total={r[1]} {top}")
