#!/usr/bin/env python3
"""Summarize per-thread guest PCs from a run log's snapshot lines."""
import re
import sys
from collections import Counter

path = sys.argv[1]
want = sys.argv[2] if len(sys.argv) > 2 else None  # optional thread index
per = {}
main = Counter()
for line in open(path, errors="replace"):
    if line.startswith("guest snapshot"):
        m = re.search(r"main=([0-9A-F]{8})", line)
        if m:
            main[m.group(1)] += 1
    elif line.startswith("guest thread["):
        m = re.match(r"guest thread\[(\d+)\] id=\S+ active=(\d) blocked=(\d) pc=([0-9A-F]{8})", line)
        if not m:
            continue
        idx, active, blocked, pc = m.group(1), m.group(2), m.group(3), m.group(4)
        if want is not None and idx != want:
            continue
        per.setdefault(idx, Counter())[("%s%s" % (active, blocked), pc)] += 1

print("== main thread PCs ==")
for pc, n in main.most_common(12):
    print("  %s x%d" % (pc, n))
for idx in sorted(per, key=int):
    print("== thread[%s] ==" % idx)
    for (state, pc), n in per[idx].most_common(8):
        print("  %s pc=%s x%d" % (state, pc, n))
