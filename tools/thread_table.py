#!/usr/bin/env python3
"""Print the Nth guest thread table (5 s progress dumps) of a run log."""
import sys

path, which = sys.argv[1], int(sys.argv[2])
tables = []
cur = []
for line in open(path, errors="replace"):
    if line.startswith("guest thread["):
        cur.append(line.rstrip())
    else:
        if cur:
            tables.append(cur)
            cur = []
if cur:
    tables.append(cur)
if not tables:
    sys.exit("no thread tables")
idx = min(which, len(tables) - 1)
print("table %d of %d" % (idx, len(tables)))
for row in tables[idx]:
    print(row)
