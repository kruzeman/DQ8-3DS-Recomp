#!/usr/bin/env python3
"""Rank S1 rollout candidate regions: maximal convertible block runs x guest-PC profile.

A candidate region is a maximal run of consecutive Block-IR blocks whose exits
are ALL control-flow exits (branch/call/return/indirect).  The S1c region
emitter refuses a block whose exit is a fall-through (no terminator), so such
runs are exactly the ranges it can chain without changing its refusal rules.

Each candidate is scored with the guest-PC histogram (Perf G,
`GUEST_PC_PROFILE_PC pc=... steps=...` lines) as its share of profiled steps.

usage: python3 tools/s1_rank_candidates.py --profile LOG --blocks B1 [B2 ...]
                                            [--top 25] [--exclude ALREADY]
"""
import argparse
import re
from pathlib import Path

BLOCK_RE = re.compile(
    r"^block start=0x([0-9A-F]{8}) end=0x([0-9A-F]{8}) insns=(\d+).* exit=(\S+)")
PC_RE = re.compile(r"^GUEST_PC_PROFILE_PC pc=([0-9A-F]{8}) steps=(\d+)$", re.M)
CONTROL = {"branch", "call", "return", "indirect"}


def load_profile(path: Path):
    return {int(pc, 16): int(n) for pc, n in PC_RE.findall(path.read_text())}


def load_blocks(paths):
    blocks = []
    for p in paths:
        for line in Path(p).read_text().splitlines():
            m = BLOCK_RE.match(line)
            if m:
                blocks.append({
                    "start": int(m.group(1), 16),
                    "end": int(m.group(2), 16),
                    "insns": int(m.group(3)),
                    "exit": m.group(4),
                })
    blocks.sort(key=lambda b: b["start"])
    # drop overlaps (same range extracted twice)
    out, last_end = [], -1
    for b in blocks:
        if b["start"] >= last_end:
            out.append(b)
            last_end = b["end"]
    return out


def candidates(blocks):
    runs, cur = [], []
    for b in blocks:
        if b["exit"] in CONTROL:
            cur.append(b)
        else:
            if len(cur) >= 1:
                runs.append(cur)
            cur = []
    if cur:
        runs.append(cur)
    return runs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--profile", required=True, type=Path)
    ap.add_argument("--blocks", required=True, nargs="+")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--window", default="0x00100000,0x00600000")
    args = ap.parse_args()

    prof = load_profile(args.profile)
    lo, hi = (int(x, 16) for x in args.window.split(","))
    total = sum(v for k, v in prof.items() if lo <= k < hi)
    blocks = load_blocks(args.blocks)
    rows = []
    for run in candidates(blocks):
        start, end = run[0]["start"], run[-1]["end"]
        if end <= lo or start >= hi:
            continue
        steps = sum(prof.get(pc, 0) for pc in range(start, end, 4))
        insns = sum(b["insns"] for b in run)
        rows.append((steps, start, end, len(run), insns))
    rows.sort(reverse=True)
    print(f"profiled window steps={total:,}  blocks={len(blocks)}  "
          f"candidate runs={len(rows)}")
    print(f"{'rank':>4}  {'range':<28} {'blocks':>6} {'insns':>6} "
          f"{'steps':>14} {'share':>8}")
    cum = 0
    for i, (steps, start, end, nb, insns) in enumerate(rows[:args.top], 1):
        cum += steps
        print(f"{i:>4}  0x{start:08X}..0x{end:08X}    {nb:>6} {insns:>6} "
              f"{steps:>14,} {steps/total*100:>7.3f}%")
    print(f"top-{min(args.top, len(rows))} cumulative share "
          f"{cum/total*100:.3f}%")


if __name__ == "__main__":
    main()
