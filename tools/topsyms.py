#!/usr/bin/env python3
"""Top symbols from joint samples.raw for one tid/comm within a time window."""
import json
import sys
from collections import Counter

path, w0, w1 = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
tid_filter = sys.argv[4] if len(sys.argv) > 4 else None
records = []
with open(path) as fh:
    for line in fh:
        try:
            records.append(json.loads(line))
        except ValueError:
            continue
origin = records[0]["t"] if records else 0.0
frames = Counter()
n = 0
for r in records:
    rel = r["t"] - origin
    if not (w0 <= rel < w1):
        continue
    if tid_filter and str(r.get("tid")) != tid_filter:
        continue
    n += 1
    # attribute weight along the chain, preferring leaf-ish runtime frames
    for nm in r.get("names", []):
        if nm.startswith(("??", "0x")):
            continue
        frames[nm.split("+")[0].split(" [")[0]] += 1
print("samples:", n)
for name, c in frames.most_common(25):
    print(f"{c:6d}  {100.0 * c / max(n, 1):5.1f}%  {name}")
