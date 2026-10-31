#!/usr/bin/env python3
"""Cluster DQ8_GUEST_PC_PROFILE_PC records into candidate conversion regions.

Perf G's tools/guest_pc_profile.py ranks individual PCs; a recompiler region
is a contiguous address RANGE, so this script groups hot PCs into runs
(gap <= --gap bytes between consecutive counted PCs) and ranks the clusters
by executed guest steps.  Used by reports/S1_ROLLOUT.md to pick the next
regions for the S1 region gate.

usage: tools/s1d_pc_clusters.py <stderr-log> [--gap 0x20] [--top N]
"""
import argparse
import re
from pathlib import Path

PC_RE = re.compile(r"^GUEST_PC_PROFILE_PC pc=([0-9A-F]{8}) steps=(\d+)$", re.M)
BEG_RE = re.compile(r"^GUEST_PC_PROFILE_BEGIN after_movie_entry=(\d+)$", re.M)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("log", type=Path)
    p.add_argument("--gap", default="0x20")
    p.add_argument("--top", type=int, default=20)
    p.add_argument("--exclude", default="",
                   help="comma-separated hex ranges to subtract "
                        "(e.g. 0x1A93A8-0x1A9410 for the already-converted "
                        "region), reported as removed share")
    args = p.parse_args()
    text = args.log.read_text(errors="replace")
    pcs = sorted((int(pc, 16), int(n)) for pc, n in PC_RE.findall(text))
    total = sum(n for _, n in pcs)
    if not pcs:
        raise SystemExit("no GUEST_PC_PROFILE_PC records")
    gap = int(args.gap, 0)

    excluded = set()
    for part in filter(None, args.exclude.split(",")):
        lo, hi = part.split("-")
        excluded.update(range(int(lo, 16), int(hi, 16)))

    clusters = []
    cur = None
    prev_pc = -1 << 30
    for pc, n in pcs:
        if cur is None or pc - prev_pc - 4 > gap:
            cur = {"start": pc, "end": pc + 4, "steps": 0, "pcs": 0}
            clusters.append(cur)
        cur["end"] = pc + 4
        cur["steps"] += n
        cur["pcs"] += 1
        prev_pc = pc

    removed = 0
    if excluded:
        removed = sum(n for pc, n in pcs if pc in excluded)
    print(f"nonzero_pcs={len(pcs)} total_steps={total} "
          f"clusters={len(clusters)} "
          f"excluded_steps={removed} "
          f"excluded_share={removed * 100 / total:.3f}%")
    print("rank  range                          pcs    steps        share   "
          "cumshare  share_excl_region")
    cum = 0
    cum_excl = removed
    for i, c in enumerate(sorted(clusters, key=lambda c: -c["steps"])[:args.top],
                          1):
        cum += c["steps"]
        ex = sum(n for pc, n in pcs
                 if c["start"] <= pc < c["end"] and pc in excluded)
        cum_excl += c["steps"] - ex
        print(f"{i:4d}  0x{c['start']:08X}-0x{c['end'] - 1:08X}      "
              f"{c['pcs']:4d}  {c['steps']:11,d}  {c['steps'] * 100 / total:7.3f}%"
              f"  {cum * 100 / total:7.3f}%   "
              f"{cum_excl * 100 / total:7.3f}% (this cluster "
              f"{(c['steps'] - ex) * 100 / total:.3f}%)")


if __name__ == "__main__":
    main()
