#!/usr/bin/env python3
"""Non-black frame heuristic over a frames dir (PPM/PNG): black frames
compress tiny / stay small; report sizes so a human can spot the dialog."""
import glob
import os
import sys

d = sys.argv[1]
files = sorted(glob.glob(os.path.join(d, '*')))
print('frames:', len(files))
sizes = [(os.path.basename(f), os.path.getsize(f)) for f in files]
big = [x for x in sizes if x[1] > 20000]
print('files > 20 KB (likely non-black):', len(big))
for name, sz in big[:12]:
    print('  ', name, sz)
if sizes:
    print('smallest:', min(sizes, key=lambda x: x[1]))
    print('largest:', max(sizes, key=lambda x: x[1]))
