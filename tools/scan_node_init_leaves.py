#!/usr/bin/env python3
"""Scan DQ8.code for the 6-word ARM node-init leaf family
   (add r1,r0,#4; mov r2,#0; str r1,[r0,#4]; str r2,[r0]; str r1,[r0,#8]; bx lr)
   and report which instances are missing from recomp-roots.txt."""
import sys

PAT = bytes.fromhex("041080e20020a0e3041080e5002080e5081080e51eff2fe1")
BASE = 0x00100000

data = open("/home/hermes/DQ8/DQ8.code", "rb").read()
roots = set()
for line in open("/home/hermes/DQ8/recomp-roots.txt"):
    s = line.strip()
    if s and not s.startswith("#"):
        roots.add(int(s, 16))

hits = []
start = 0
while True:
    i = data.find(PAT, start)
    if i < 0:
        break
    hits.append(BASE + i)
    start = i + 4

missing = [h for h in hits if h not in roots]
print("total instances: %d" % len(hits))
print("unrooted: %d" % len(missing))
for h in missing:
    print("  0x%08X" % h)
with open("/tmp/missing_roots.txt", "w") as fh:
    for h in missing:
        fh.write("0x%08X\n" % h)
sys.exit(0)
