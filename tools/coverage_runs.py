#!/usr/bin/env python3
"""Dynamic coverage summary: executed-instruction split per scene.

Parses the stderr of bounded BENCH-mode runs (tools/coverage_run_scene.sh):
the `BENCH ...` reporter line (aggregate guest retired instructions) and the
per-region `S1_CFG entry=... runs=... insns=...` lines (instructions retired
inside native-CFG regions while the gate was on), and reports per run/scene:

  * native-CFG (S1) share of retired guest instructions,
  * translated-dispatch share (everything else the guest retired - the
    per-instruction generated C path),
  * abort events (escaped PC / unsupported instruction / memory fault) -
    there is no interpreter fallback in this runtime, a bad PC exits,
  * native-side host counters (dispatch calls, completed Y2R conversion
    batches, DSP/audio ticks) - host work, NOT guest instructions, reported
    separately so the two are never mixed.

usage: python3 tools/coverage_runs.py target/coverage-runs/<scene> [...]
"""
import re
import sys
from pathlib import Path

BENCH_RE = re.compile(r"^BENCH (.*)$", re.M)
S1_RE = re.compile(r"^S1_CFG entry=(\S+) runs=(\d+) insns=(\d+)$", re.M)
ABORT_RES = {
    "escaped_pc": re.compile(r"^translated PC escaped", re.M),
    "unsupported": re.compile(r"^unsupported guest instruction", re.M),
    "mem_fault": re.compile(r"^guest memory fault", re.M),
    "invalid_handle": re.compile(r"invalid handle", re.M),
}


def parse(run_dir: Path):
    err = run_dir / "run.err"
    if not err.exists():
        return {"name": run_dir.name, "missing": True}
    text = err.read_text(errors="replace")
    fields = {}
    bench = BENCH_RE.findall(text)
    if bench:
        for kv in bench[0].split():
            if "=" in kv:
                k, v = kv.split("=", 1)
                fields[k] = v
    regions = {e: (int(r), int(i)) for e, r, i in S1_RE.findall(text)}
    agg = int(fields.get("aggregate_steps", 0) or 0)
    s1_insns = sum(i for _r, i in regions.values())
    return {
        "name": run_dir.name,
        "missing": False,
        "rc_line": bool(bench),
        "wall": float(fields.get("wall", 0) or 0),
        "agg": agg,
        "main": int(fields.get("main_steps", 0) or 0),
        "worker": int(fields.get("worker_steps", 0) or 0),
        "dispatch_calls": int(fields.get("dispatch_calls", 0) or 0),
        "movie_entries": int(fields.get("scene_movie_entries", 0) or 0),
        "y2r": int(fields.get("scene_y2r_conversions", 0) or 0),
        "audio_ticks": int(fields.get("audio_ticks", 0) or 0),
        "s1_insns": s1_insns,
        "s1_regions": len(regions),
        "escapes": {k: len(rx.findall(text)) for k, rx in ABORT_RES.items()},
    }


def main() -> int:
    rows = []
    for arg in sys.argv[1:]:
        p = Path(arg)
        if (p / "run.err").exists():
            rows.append(parse(p))
            continue
        children = [d for d in sorted(p.glob("*")) if d.is_dir()
                    and (d / "run.err").exists()]
        if children:
            rows.extend(parse(d) for d in children)
        else:
            rows.append(parse(p))
    if not rows:
        print("no run dirs")
        return 2
    hdr = (f"{'run':<12} {'wall s':>7} {'aggregate':>14} {'S1 cfg insns':>13} "
           f"{'S1%':>6} {'dispatch%':>9} {'movie':>6} {'y2r':>5} "
           f"{'dispatches':>11} {'aborts':>22}")
    print(hdr)
    for r in rows:
        if r.get("missing"):
            print(f"{r['name']:<12} MISSING run.err")
            continue
        if not r["rc_line"]:
            aborted = ",".join(f"{k}x{v}" for k, v in r["escapes"].items() if v)
            print(f"{r['name']:<12} NO BENCH LINE (abort/timeout)"
                  f"  [{aborted or 'killed/timeout'}]")
            continue
        agg = r["agg"] or 1
        s1p = r["s1_insns"] * 100.0 / agg
        disp = (agg - r["s1_insns"]) * 100.0 / agg
        aborted = ",".join(f"{k}x{v}" for k, v in r["escapes"].items() if v) or "-"
        print(f"{r['name']:<12} {r['wall']:>7.1f} {r['agg']:>14,} "
              f"{r['s1_insns']:>13,} {s1p:>5.2f}% {disp:>8.2f}% "
              f"{r['movie_entries']:>6} {r['y2r']:>5} "
              f"{r['dispatch_calls']:>11,} {aborted:>22}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
