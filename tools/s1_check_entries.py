#!/usr/bin/env python3
"""Check S1 candidate region entries against the Block-IR dump (R3 preflight).

For each requested range prints whether a block starts exactly at the entry,
whether the range's blocks are already inside a converted region, the block /
instruction counts and the profiled-step share, so a build attempt is never
spent on a range the emitter's structural checks will reject.

usage: python3 tools/s1_check_entries.py --blocks FILE --profile LOG \
           0xSTART:0xEND [...]
"""
import argparse
import re
from pathlib import Path

BLOCK_RE = re.compile(r"^block start=0x([0-9A-F]{8}) end=0x([0-9A-F]{8}) insns=(\d+).* exit=(\S+)")
PC_RE = re.compile(r"^GUEST_PC_PROFILE_PC pc=([0-9A-F]{8}) steps=(\d+)$", re.M)
CONVERTED = [
    (0x001A93A8, 0x001A9410), (0x001A9428, 0x001A9608),
    (0x001A96C8, 0x001A97B8), (0x001A9904, 0x001A9990),
    (0x001A99A8, 0x001A9A64), (0x00194D64, 0x001956B8),
    (0x00195D44, 0x00196A40),
]


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--blocks", required=True, type=Path)
    ap.add_argument("--profile", type=Path)
    ap.add_argument("--total", type=int, default=551_084_121)
    ap.add_argument("ranges", nargs="+", metavar="0xSTART:0xEND")
    args = ap.parse_args()

    prof = {}
    if args.profile:
        prof = {int(p, 16): int(n)
                for p, n in PC_RE.findall(args.profile.read_text())}
    blocks = []
    for line in args.blocks.read_text().splitlines():
        m = BLOCK_RE.match(line)
        if m:
            blocks.append((int(m.group(1), 16), int(m.group(2), 16),
                           int(m.group(3)), m.group(4)))
    blocks.sort()

    for spec in args.ranges:
        lo_s, hi_s = spec.split(":")
        lo, hi = int(lo_s, 16), int(hi_s, 16)
        inside = [b for b in blocks if b[0] >= lo and b[1] <= hi]
        starts_at_entry = any(b[0] == lo for b in blocks)
        overlap = [b for b in blocks
                   if b[0] < hi and b[1] > lo
                   and any(cs <= b[0] < ce for cs, ce in CONVERTED)]
        steps = (sum(prof.get(pc, 0) for pc in range(lo, hi, 4)) if prof else 0)
        insns = sum(b[2] for b in inside)
        share = steps / args.total * 100 if prof else 0.0
        print(f"0x{lo:08X}..0x{hi:08X} entry_block_start="
              f"{'YES' if starts_at_entry else 'NO'} blocks={len(inside)} "
              f"insns={insns} steps={steps:,} ({share:.3f}%) "
              f"overlap_converted={len(overlap)}")
        if not starts_at_entry:
            near = [b for b in blocks if abs(b[0] - lo) <= 0x40]
            for b in near[:4]:
                print(f"    nearest block start 0x{b[0]:08X} "
                      f"(delta {b[0] - lo:+d})")


if __name__ == "__main__":
    main()
