#!/usr/bin/env python3
# Track the +0x2860 sequence counters of both DSP regions over dump snapshots.
# Usage: python3 tools/dsp_seq_track.py <prefix>
import glob, struct, sys

files = sorted(glob.glob(sys.argv[1] + "*.bin"))
print("file      R0seq  R1seq  pick")
for i, f in enumerate(files):
    with open(f, "rb") as fh:
        d = fh.read()
    s0 = struct.unpack_from("<H", d, 0x50000 + 0x2860)[0]
    s1 = struct.unpack_from("<H", d, 0x70000 + 0x2860)[0]
    if i % 4 == 0 or i == len(files) - 1:
        pick = "R0" if (s0 - s1) % 0x10000 < 0x8000 and s0 != s1 else ("R1" if s0 != s1 else "-")
        print("%s  0x%04X 0x%04X  %s" % (f.split("/")[-1], s0, s1, pick))
