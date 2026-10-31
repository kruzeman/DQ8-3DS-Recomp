#!/usr/bin/env python3
"""Compare sw/gl transfer-frame dumps numerically (run_sw_gl_diff.sh output)."""
import hashlib
import os
import re
import sys


def frames(d):
    fs = {}
    for f in os.listdir(os.path.join(out, d)):
        m = re.match(r"tr-(\d+)\.ppm$", f)
        if m:
            fs[int(m.group(1))] = f
    return fs


out = sys.argv[1] if len(sys.argv) > 1 else "target/sw-gl-diff"
sw, gl = frames("sw"), frames("gl")
common = sorted(set(sw) & set(gl))
bad = []
for i in common:
    a = hashlib.sha256(open(os.path.join(out, "sw", sw[i]), "rb").read()).hexdigest()
    b = hashlib.sha256(open(os.path.join(out, "gl", gl[i]), "rb").read()).hexdigest()
    if a != b:
        bad.append(i)
print("sw=%d gl=%d common=%d identical=%d differing=%d" %
      (len(sw), len(gl), len(common), len(common) - len(bad), len(bad)))
print("differing indices:", bad[:30])
sys.exit(0 if not bad and len(common) > 100 else 1)
