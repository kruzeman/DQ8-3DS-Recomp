#!/usr/bin/env python3
"""Pre-build scan: find candidate-region blocks that would trip the emitter's
`running`-mid-block refusal (`src/block_emit.rs`).

The dispatch lowering writes the dispatcher's `running` flag for any statement
that loads pc (the sentinel/stop check), so a block that writes r15 *before*
its last instruction is refused as "touches `running` mid-block".  This scan
reads the Block-IR dump (`dq8tool extract-blocks`) and reports, per candidate
range, the offending blocks, so a range can be split before a build attempt.

usage: python3 tools/s1_running_scan.py --blocks FILE 0xSTART:0xEND [...]
"""
import argparse
import re
from pathlib import Path

BLOCK_RE = re.compile(r"^block start=0x([0-9A-F]{8}) end=0x([0-9A-F]{8}) insns=(\d+).* exit=(\S+)")
INSN_RE = re.compile(r"^  insn addr=0x([0-9A-F]{8}) .*writes=([0-9a-z,]+)")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--blocks", required=True, type=Path)
    ap.add_argument("ranges", nargs="+", metavar="0xSTART:0xEND")
    args = ap.parse_args()

    blocks = []
    cur = None
    for line in args.blocks.read_text().splitlines():
        m = BLOCK_RE.match(line)
        if m:
            cur = {"start": int(m.group(1), 16), "end": int(m.group(2), 16),
                   "exit": m.group(4), "insns": []}
            blocks.append(cur)
            continue
        mi = INSN_RE.match(line)
        if mi and cur is not None:
            cur["insns"].append((int(mi.group(1), 16), mi.group(2).split(",")))

    for spec in args.ranges:
        lo_s, hi_s = spec.split(":")
        lo, hi = int(lo_s, 16), int(hi_s, 16)
        bad = []
        pc_blocks = []
        for b in blocks:
            if b["start"] < lo or b["end"] > hi:
                continue
            for addr, writes in b["insns"][:-1]:
                if "r15" in writes:
                    bad.append((b["start"], addr, b["exit"]))
            if any("r15" in w for _a, w in b["insns"]):
                pc_blocks.append(b["start"])
        print(f"0x{lo:08X}..0x{hi:08X} mid-block-pc-writes={len(bad)} "
              f"blocks-ending-in-pc={len(pc_blocks)}")
        for start, addr, ex in bad:
            print(f"    REFUSAL block 0x{start:08X} (exit={ex}) contains "
                  f"pc-writing insn 0x{addr:08X} -> split around it")


if __name__ == "__main__":
    main()
