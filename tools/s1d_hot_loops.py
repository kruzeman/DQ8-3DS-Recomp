#!/usr/bin/env python3
"""Find maximal equal-count PC runs (loop bodies) in a guest-PC profile.

A guest loop body executes every one of its instructions the same number of
times, so contiguous profiled PCs with (nearly) equal step counts form a
candidate straight-line region for the S1 region gate.  This ranks those runs
inside a given address window, which is how reports/S1_ROLLOUT.md picks the
next regions after the already-converted 0x001A93A8..0x001A9410.

usage: tools/s1d_hot_loops.py <stderr-log> [--start 0x...] [--end 0x...]
                              [--tol 0.01] [--min-pcs 4] [--top 15]
"""
import argparse
import re
from pathlib import Path

PC_RE = re.compile(r"^GUEST_PC_PROFILE_PC pc=([0-9A-F]{8}) steps=(\d+)$", re.M)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("log", type=Path)
    p.add_argument("--start", default="0x00100000")
    p.add_argument("--end", default="0x00600000")
    p.add_argument("--tol", type=float, default=0.01)
    p.add_argument("--min-pcs", type=int, default=4)
    p.add_argument("--top", type=int, default=15)
    args = p.parse_args()
    lo, hi = int(args.start, 16), int(args.end, 16)
    pcs = [(int(pc, 16), int(n)) for pc, n in PC_RE.findall(args.log.read_text(
        errors="replace")) if lo <= int(pc, 16) < hi]
    pcs.sort()
    if not pcs:
        raise SystemExit("no PCs in window")

    runs = []
    cur = None
    for pc, n in pcs:
        if (cur is None or pc != cur["end"]
                or abs(n - cur["count"]) > args.tol * cur["count"]):
            cur = {"start": pc, "end": pc + 4, "count": n, "pcs": 1}
            runs.append(cur)
        else:
            cur["end"] = pc + 4
            cur["pcs"] += 1
    runs = [r for r in runs if r["pcs"] >= args.min_pcs]
    window_total = sum(n for _, n in pcs)
    print(f"window=0x{lo:08X}-0x{hi:08X} pcs={len(pcs)} "
          f"window_steps={window_total:,d} candidate_runs={len(runs)}")
    print("rank  range                            pcs   count/pc   run_steps"
          "   share_of_window  share_of_profile")
    prof_total = 551_084_121  # overwritten by caller-supplied total if known
    for i, r in enumerate(sorted(runs, key=lambda r: -r["pcs"] * r["count"])[
                          :args.top], 1):
        steps = r["pcs"] * r["count"]
        print(f"{i:4d}  0x{r['start']:08X}-0x{r['end'] - 1:08X}   "
              f"{r['pcs']:5d}  {r['count']:10,d}  {steps:11,d}   "
              f"{steps * 100 / window_total:12.3f}%   "
              f"{steps * 100 / prof_total:13.3f}%")


if __name__ == "__main__":
    main()
