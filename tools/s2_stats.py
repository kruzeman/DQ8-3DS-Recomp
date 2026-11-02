#!/usr/bin/env python3
"""t_6edf5d17: structural statistics of an emitted S2 split directory.

Counts, per generated part: straight-line blocks (LB_ labels), instructions
(T_ labels), the block-length distribution, cold entry stubs (C_ labels) and
the emitted text size — i.e. the shape of the dispatch that the S2 stages
produce, independent of any timing measurement.

usage: tools/s2_stats.py <split-dir>
"""
import collections
import os
import re
import sys

d = sys.argv[1]
lb = re.compile(r"^    LB_([0-9A-F]{8}): ;")
tlab = re.compile(r"^    T_([0-9A-F]{8}): ;")
clab = re.compile(r"^    C_([0-9A-F]{8}): ;")
per_block, all_blocks = collections.Counter(), []
total_t = total_c = 0
files = sorted(f for f in os.listdir(d) if re.match(r"dq8_part_\d+\.c$", f))
for f in files:
    cur = 0
    for line in open(os.path.join(d, f), errors="replace"):
        if lb.match(line):
            if cur:
                all_blocks.append(cur)
            cur = 0
        elif tlab.match(line):
            cur += 1
            total_t += 1
        elif clab.match(line):
            total_c += 1
    if cur:
        all_blocks.append(cur)
per_block.update(all_blocks)
all_blocks.sort()
n = len(all_blocks)
if not n:
    sys.exit("no blocks found in %s" % d)
mean = sum(all_blocks) / n
median = all_blocks[n // 2]
cum = 0
print("parts=%d  instructions(T)=%d  blocks(LB)=%d  cold stubs(C)=%d"
      % (len(files), total_t, n, total_c))
print("block length: mean=%.2f median=%d max=%d" % (mean, median, all_blocks[-1]))
for thr in (1, 2, 4, 8):
    k = sum(1 for b in all_blocks if b >= thr)
    ins = sum(b for b in all_blocks if b >= thr)
    print("  blocks >= %d insns: %6d (%5.1f%%)  covering %6.1f%% of instructions"
          % (thr, k, 100.0 * k / n, 100.0 * ins / total_t))
print("length histogram (top 8): %s"
      % ", ".join("%d:%d" % (k, per_block[k]) for k in sorted(per_block)[:8]))
