#!/usr/bin/env python3
"""Per-block profile share for candidate S1 regions (R3 selection aid).

Cross-joins the Block-IR dump (`dq8tool extract-blocks`) with a Perf-G guest-PC
histogram (`GUEST_PC_PROFILE_PC pc=... steps=...` lines) and prints, for each
requested window, every block with its profiled-step share, flagging blocks
that already live inside a converted region.

usage: python3 tools/s1_block_steps.py --profile LOG --blocks FILE \
           --range 0xSTART:0xEND [--range ...] [--total 551084121]
"""
import argparse
import re
from pathlib import Path

BLOCK_RE = re.compile(r"^block start=0x([0-9A-F]{8}) end=0x([0-9A-F]{8}) insns=(\d+).* exit=(\S+)")
PC_RE = re.compile(r"^GUEST_PC_PROFILE_PC pc=([0-9A-F]{8}) steps=(\d+)$", re.M)

# regions already converted by S1c / rollout batches 1-2
CONVERTED = [
    (0x001A93A8, 0x001A9410),
    (0x001A9428, 0x001A9608),
    (0x001A96C8, 0x001A97B8),
    (0x001A9904, 0x001A9990),
    (0x001A99A8, 0x001A9A64),
    (0x00194D64, 0x001956B8),
    (0x00195D44, 0x00196A40),
]


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--profile", required=True, type=Path)
    ap.add_argument("--blocks", required=True, type=Path)
    ap.add_argument("--range", action="append", required=True,
                    metavar="0xSTART:0xEND")
    ap.add_argument("--total", type=int, default=551_084_121)
    args = ap.parse_args()

    prof = {int(p, 16): int(n)
            for p, n in PC_RE.findall(args.profile.read_text())}
    blocks = []
    for line in args.blocks.read_text().splitlines():
        m = BLOCK_RE.match(line)
        if m:
            blocks.append((int(m.group(1), 16), int(m.group(2), 16),
                           int(m.group(3)), m.group(4)))
    blocks.sort()

    for spec in args.range:
        lo_s, hi_s = spec.split(":")
        lo, hi = int(lo_s, 16), int(hi_s, 16)
        tot = sum(prof.get(pc, 0) for pc in range(lo, hi, 4))
        print(f"=== 0x{lo:08X}..0x{hi:08X} steps={tot:,} "
              f"({tot / args.total * 100:.3f}% of profile)")
        for s, e, ins, ex in blocks:
            if s < lo or e > hi:
                continue
            st = sum(prof.get(pc, 0) for pc in range(s, e, 4))
            if st == 0 and ins < 8:
                continue
            flag = "CONV" if any(cs <= s < ce for cs, ce in CONVERTED) else ""
            print(f"  0x{s:08X}..0x{e:08X} insns={ins:4d} exit={ex:14s} "
                  f"steps={st:>12,} {flag}")


if __name__ == "__main__":
    main()
