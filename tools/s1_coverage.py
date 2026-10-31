#!/usr/bin/env python3
"""Retired-instruction coverage of the converted S1 regions.

Parses each run's stderr: the `BENCH ... aggregate_steps=...` line and every
`S1_CFG entry=0x... runs=N insns=N` reporter line, then reports

  * each region's share of retired guest instructions on the movie path
    (insns / aggregate_steps), and
  * the total converted share, plus the rank-1 (S1c) region alone as the
    "before" figure measured in the SAME runs with the same method.

usage: python3 tools/s1_coverage.py target/s1r-100m/*.err
"""
import re
import statistics
import sys
from pathlib import Path

BENCH_RE = re.compile(r"^BENCH (.*)$", re.M)
S1_RE = re.compile(r"^S1_CFG entry=(\S+) runs=(\d+) insns=(\d+)$", re.M)
RANK1 = "0x001A93A8"


def parse(path: Path):
    text = path.read_text(errors="replace")
    bench = BENCH_RE.findall(text)
    fields = {}
    if bench:
        for kv in bench[0].split():
            if "=" in kv:
                k, v = kv.split("=", 1)
                fields[k] = v
    regions = {e: int(i) for e, _r, i in S1_RE.findall(text)}
    agg = int(fields["aggregate_steps"]) if "aggregate_steps" in fields else 0
    return {
        "name": path.name,
        "rc_line": bool(bench),
        "movie": int(fields.get("scene_movie_entries", 0) or 0),
        "agg": agg,
        "regions": regions,
        "sum": sum(regions.values()),
        "escaped": "translated PC escaped" in text,
        "fault": "guest memory fault" in text,
    }


def main() -> int:
    rows = [parse(Path(p)) for p in sys.argv[1:]]
    if not rows:
        print("no input")
        return 2
    print(f"{'run':<18} {'aggregate':>13} {'rank1':>10} {'all':>10} "
          f"{'rank1%':>8} {'all%':>7} {'movie':>6}")
    r1, alls = [], []
    for r in rows:
        if not r["rc_line"]:
            print(f"{r['name']:<18} NO BENCH LINE (abort/timeout)")
            continue
        p1 = r["regions"].get(RANK1, 0)
        pa = r["sum"]
        s1p = p1 / r["agg"] * 100 if r["agg"] else 0
        sap = pa / r["agg"] * 100 if r["agg"] else 0
        r1.append(s1p)
        alls.append(sap)
        print(f"{r['name']:<18} {r['agg']:>13,} {p1:>10,} {pa:>10,} "
              f"{s1p:>7.2f}% {sap:>6.2f}% {r['movie']:>6}")
    if alls:
        print(f"\nvalid runs: {len(alls)}/{len(rows)}")
        print(f"COVERAGE rank1 (S1c region, 'before') : "
              f"median {statistics.median(r1):.2f}%  "
              f"[{min(r1):.2f}-{max(r1):.2f}]")
        print(f"COVERAGE all converted regions ('after'): "
              f"median {statistics.median(alls):.2f}%  "
              f"[{min(alls):.2f}-{max(alls):.2f}]")
        per = {}
        for r in rows:
            if not r["rc_line"]:
                continue
            for e, i in r["regions"].items():
                per.setdefault(e, []).append(i / r["agg"] * 100 if r["agg"] else 0)
        print("\nper-region share of retired guest instructions (median):")
        for e in sorted(per, key=lambda k: -statistics.median(per[k])):
            print(f"  {e}  {statistics.median(per[e]):6.2f}%  "
                  f"n={len(per[e])}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
