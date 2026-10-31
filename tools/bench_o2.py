#!/usr/bin/env python3
"""Fixed-wall-time throughput probe for a built DQ8 split runner (O0 vs O2).

Runs the given binary headless for a fixed duration with periodic guest
progress dumps (DQ8_PROGRESS_SECONDS -> a `presentation state: pica_draws=N`
line every 5 s).  The opening cinematic renders steadily and needs no input
during the measured window, so the number of PICA draws reached in fixed wall
time is a direct throughput proxy: a faster build renders more draws in the
same seconds.

Every run starts from a brand-new empty DQ8_USER_DATA_DIR so both builds take
the identical save-creation path (fresh save), keeping the comparison
apples-to-apples.  DQ8_RUN_GUEST_THREADS follows the project rule (1).

Usage:  python3 tools/bench_o2.py <binary> <label> [repeats=3] [duration=30]
Env:    (none required)
Prints one JSON line per run plus a SUMMARY line with mean final draws and
draws/sec.  Exit 0 always (this is a probe, not a pass/fail gate).
"""
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DRAW_RE = re.compile(r"pica_draws=(\d+)")


def one_run(binary: Path, duration: int) -> dict:
    ud = tempfile.mkdtemp(prefix="dq8bench_ud_")
    env = dict(os.environ)
    env.update(
        DQ8_HEADLESS="1",
        DQ8_PROGRESS_SECONDS="1",
        DQ8_RUN_GUEST_THREADS="1",
        DQ8_USER_DATA_DIR=ud,
    )
    cmd = ["timeout", "-s", "KILL", str(duration),
           str(binary), str(ROOT / "DQ8.code"), str(ROOT / "target" / "dq8.romfs")]
    p = subprocess.run(cmd, env=env, stdout=subprocess.DEVNULL,
                       stderr=subprocess.PIPE, text=True)
    draws = [int(m) for m in DRAW_RE.findall(p.stderr)]
    final = draws[-1] if draws else 0
    # Dumps fire every 5 s; the last may be truncated by the KILL.
    elapsed = max(len(draws) * 5, 1)
    return {
        "rc": p.returncode,
        "snapshots": len(draws),
        "draws_seq": draws,
        "draws_final": final,
        "elapsed_s": elapsed,
        "draws_per_s": round(final / elapsed, 2),
    }


def main() -> int:
    binary = Path(sys.argv[1])
    label = sys.argv[2] if len(sys.argv) > 2 else "run"
    repeats = int(sys.argv[3]) if len(sys.argv) > 3 else 3
    duration = int(sys.argv[4]) if len(sys.argv) > 4 else 30
    if not binary.exists():
        print(f"missing binary {binary}", file=sys.stderr)
        return 2
    runs = []
    for i in range(repeats):
        r = one_run(binary, duration)
        r["label"] = label
        r["rep"] = i
        runs.append(r)
        print(json.dumps(r))
    finals = [r["draws_final"] for r in runs]
    rates = [r["draws_per_s"] for r in runs]
    summary = {
        "label": label,
        "binary": str(binary),
        "repeats": repeats,
        "duration_s": duration,
        "draws_final_mean": round(sum(finals) / len(finals), 1),
        "draws_per_s_mean": round(sum(rates) / len(rates), 2),
        "draws_final_all": finals,
    }
    print("SUMMARY " + json.dumps(summary))
    return 0


if __name__ == "__main__":
    sys.exit(main())
