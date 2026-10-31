#!/usr/bin/env python3
"""S1 rollout preflight: does the region CFG emitter accept a candidate range?

For each candidate [entry, end) runs the recompiler with --s1-cfg/--s1-cfg-end
(starting translation at the region entry so only that function is lowered)
and reports whether the emitted native CFG made it into the generated C.

usage: python3 tools/s1_probe_regions.py 0x001A9428:0x001A9608 ...
       python3 tools/s1_probe_regions.py --default
"""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "target" / "release" / "dq8tool"

DEFAULT = [
    "0x001A9428:0x001A9608",   # rank 1 of the rollout (rank 2+3 merged), 17.0 %
    "0x001A99A8:0x001A9A64",   # rank 2 of the rollout (rank 4+7 merged), 10.2 %
    "0x001A96C8:0x001A97B8",
    "0x001A9904:0x001A998C",
    "0x001A9428:0x001A94DC",
    "0x001A94F0:0x001A9608",
    "0x001A99F8:0x001A9A64",
    "0x001A99A8:0x001A99E4",
]


def probe(entry: str, end: str) -> str:
    with tempfile.TemporaryDirectory(prefix="s1-probe-") as td:
        out = Path(td) / "probe.c"
        cmd = [str(TOOL), "recompile-arm", str(ROOT / "DQ8.code"), entry,
               str(out), "--base", "0x00100000",
               "--s1-cfg", entry, "--s1-cfg-end", end]
        p = subprocess.run(cmd, text=True, capture_output=True, timeout=600)
        tag = entry.replace("0x", "").replace("0X", "")
        if p.returncode:
            msg = (p.stderr or p.stdout).strip().splitlines()
            return "REFUSED: " + (msg[-1] if msg else f"rc={p.returncode}")
        if not out.exists():
            return "REFUSED: no output"
        text = out.read_text(errors="replace")
        if f"dq8_s1_cfg_{tag}" not in text:
            return "REFUSED: emitted CFG missing from generated C"
        lines = [l for l in text.splitlines()
                 if l.startswith("/* S1c emitted native-CFG region")]
        return "OK " + (lines[0][:160] if lines else "(cfg present)")


def main() -> int:
    args = sys.argv[1:] or ["--default"]
    if args == ["--default"]:
        args = DEFAULT
    rc = 0
    for spec in args:
        entry, end = spec.split(":")
        res = probe(entry, end)
        print(f"{entry}..{end}  {res}", flush=True)
        if not res.startswith("OK"):
            rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
