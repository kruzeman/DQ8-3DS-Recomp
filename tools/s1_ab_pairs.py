#!/usr/bin/env python3
"""Interleaved gate-OFF / gate-ON movie-path A/B pairs for the S1 rollout batch.

Runs N off/on pairs of the established 100M-main-step movie-path segment
(tools/s1d_ab.py) with `off` first inside each pair, exactly like S1d's paired
design, so host-load drift cancels.  All runs use the SAME binary; only
`DQ8_S1_CFG` differs (unset vs `all`).

This is a *rollout check* (S1_ROLLOUT.md STOP-3: cumulative movie-level gain
over gate-off), NOT the movie verdict — that is R3's final A/B.

usage: python3 tools/s1_ab_pairs.py [--pairs N] [--outdir DIR] [--bin PATH]
"""
import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--pairs", type=int, default=4)
    ap.add_argument("--outdir", type=Path, default=ROOT / "target" / "s1r-ab")
    ap.add_argument("--steps", type=int, default=100_000_000)
    ap.add_argument("--bin", type=Path,
                    default=ROOT / "target" / "dq8-s1-rollout1")
    args = ap.parse_args()
    args.outdir.mkdir(parents=True, exist_ok=True)
    rc = 0
    for i in range(1, args.pairs + 1):
        for arm, gate in (("off", "OFF"), ("on", "ON")):
            cmd = [sys.executable, str(ROOT / "tools" / "s1d_ab.py"),
                   "--arm", arm, "--bin", str(args.bin), "--steps", str(args.steps),
                   "--runs", "1", "--start", str(i), "--gate", gate,
                   "--outdir", str(args.outdir)]
            if gate == "ON":
                cmd += ["--gate-entry", "all"]
            p = subprocess.run(cmd, cwd=ROOT, text=True)
            if p.returncode:
                rc = p.returncode
    return rc


if __name__ == "__main__":
    sys.exit(main())
