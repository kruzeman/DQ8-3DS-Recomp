#!/usr/bin/env python3
"""Correlate DQ8_TRACE_WRITE lines with periodic snapshots: print write line
numbers and the state oscillation period estimate.

Usage: write_timeline.py run.log [addr]
"""
import re
import sys

path = sys.argv[1]
addr = sys.argv[2].lower() if len(sys.argv) > 2 else "005ccca4"
log = open(path, errors="replace").read().splitlines()
snaps = [i for i, l in enumerate(log) if l.startswith("guest snapshot")]
writes = []
for i, l in enumerate(log):
    m = re.match(r"guest write8? addr=([0-9A-Fa-f]+) pc=([0-9A-Fa-f]+) old=([0-9A-Fa-f]+) new=([0-9A-Fa-f]+)", l)
    if m and m.group(1).lower() == addr:
        writes.append((i, m.group(2), m.group(3), m.group(4)))
print(f"snapshots={len(snaps)} writes={len(writes)}")
# map write line -> snapshot ordinal (time proxy)
import bisect
def snap_of(line):
    return bisect.bisect_right(snaps, line)
prev = None
for i, (ln, pc, old, new) in enumerate(writes[:40]):
    s = snap_of(ln)
    dt = "" if prev is None else f"dsnap={s-prev[0]}"
    print(f"line={ln} snap#{s} pc={pc} {old}->{new} {dt}")
    prev = (s,)
# oscillation rate over whole run
if len(writes) > 2:
    s0, s1 = snap_of(writes[0][0]), snap_of(writes[-1][0])
    span = max(s1 - s0, 1)
    print(f"overall: {len(writes)} writes across {span} snapshots -> {span/len(writes):.2f} snapshots/write")
