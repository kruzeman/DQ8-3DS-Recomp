#!/usr/bin/env python3
"""Bucket GPU activity (PICA draws, display transfers, GSP triggers) between
periodic snapshot markers of a DQ8 run.log, one row per snapshot interval.

Usage: gpu_profile.py run.log [step]
"""
import re
import sys

path = sys.argv[1]
step = int(sys.argv[2]) if len(sys.argv) > 2 else 5
log = open(path, errors="replace").read().splitlines()
snap_idx = [i for i, l in enumerate(log) if l.startswith("guest snapshot")]
print(f"snapshots: {len(snap_idx)} lines: {len(log)}")
prev = 0
rows = []
for n, si in enumerate(snap_idx):
    draws = transfers = triggers = lists = 0
    for l in log[prev:si]:
        if "PICA draw trigger" in l:
            draws += 1
        elif "display-transfer" in l:
            transfers += 1
        elif "GSP TriggerCmdReqQueue" in l:
            triggers += 1
        elif "PICA list" in l and "dump" not in l:
            lists += 1
    rows.append((n, draws, transfers, triggers, lists))
    prev = si + 1
for r in rows[::step]:
    print(f"snap#{r[0]:3d} draws={r[1]:5d} transfers={r[2]:5d} triggers={r[3]:5d} lists={r[4]:5d}")
