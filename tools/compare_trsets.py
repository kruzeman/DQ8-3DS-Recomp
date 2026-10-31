#!/usr/bin/env python3
"""compare_trsets.py A-dir B-dir — compare tr-*.ppm frame sets across runs.

Runs of the same scenario can drift in transfer count (throttle/pacing), so
this compares CONTENT SETS rather than indices: how many distinct frame
images each run produced, how many are shared, and which are unique to one
side (with example file names).
"""
import hashlib
import os
import re
import sys


def frames(d):
    out = {}
    for name in os.listdir(d):
        m = re.fullmatch(r"tr-(\d+)\.ppm", name)
        if not m:
            continue
        path = os.path.join(d, name)
        with open(path, "rb") as fh:
            h = hashlib.sha256(fh.read()).hexdigest()
        out.setdefault(h, []).append(int(m.group(1)))
    for v in out.values():
        v.sort()
    return out


a = frames(sys.argv[1])
b = frames(sys.argv[2])
ka, kb = set(a), set(b)
print("A=%s frames=%d distinct=%d" % (sys.argv[1], sum(len(v) for v in a.values()), len(a)))
print("B=%s frames=%d distinct=%d" % (sys.argv[2], sum(len(v) for v in b.values()), len(b)))
print("shared_distinct=%d  A_only=%d  B_only=%d" % (len(ka & kb), len(ka - kb), len(kb - ka)))
for tag, keys, src in (("A_only", ka - kb, a), ("B_only", kb - ka, b)):
    ex = sorted(src[k][0] for k in keys)[:12]
    print("%s example tr indices: %s" % (tag, ex))
