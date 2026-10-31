#!/usr/bin/env python3
"""Frame identity for the S1 rollout batch.

Runs the SAME binary through tools/frame_dump_diff.py at each budget twice —
gates off and gates on (DQ8_S1_CFG=all) — from the same prepared save seed, and
compares the presented top/bottom screen shadows by sha256.  This is the
S1c/STOP-1 frame-identity gate (2M/5M/10M/20M main steps).

usage: python3 tools/s1_frame_identity.py [binary]
"""
import hashlib
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BIN = sys.argv[1] if len(sys.argv) > 1 else "target/dq8-s1-rollout1"
SEED = "/home/hermes/DQ8/target/userdata-title-1"
BUDGETS = [2_000_000, 5_000_000, 10_000_000, 20_000_000]
TAG = Path(BIN).name
OUT = ROOT / "target" / "s1r-frames"


def sha(p: Path):
    return hashlib.sha256(p.read_bytes()).hexdigest() if p.exists() else None


def main() -> int:
    ok = True
    for steps in BUDGETS:
        per = {}
        for gate in ("off", "on"):
            out = OUT / f"{steps // 1000_000}m-{gate}"
            env = dict(os.environ)
            env.pop("DQ8_S1_CFG", None)
            if gate == "on":
                env["DQ8_S1_CFG"] = "all"
            p = subprocess.run(
                [sys.executable, str(ROOT / "tools/frame_dump_diff.py"),
                 str(steps), str(out), SEED, str(ROOT / BIN)],
                cwd=ROOT, env=env, text=True, capture_output=True)
            sys.stdout.write(p.stdout)
            if p.stderr.strip():
                sys.stdout.write(p.stderr[-800:] + "\n")
            per[gate] = (sha(out / f"{TAG}-top.rgba"),
                         sha(out / f"{TAG}-bot.rgba"))
        same = per["off"] == per["on"] and per["on"][0] is not None
        ok = ok and same
        print(f"FRAME IDENTITY {steps // 1000_000}M: "
              f"{'IDENTICAL' if same else 'DIFFERENT'}")
        print(f"    off top={per['off'][0]}")
        print(f"    on  top={per['on'][0]}")
        print(f"    off bot={per['off'][1]}")
        print(f"    on  bot={per['on'][1]}")
        sys.stdout.flush()
    print("FRAME IDENTITY ALL:", "IDENTICAL" if ok else "DIFFERENT")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
