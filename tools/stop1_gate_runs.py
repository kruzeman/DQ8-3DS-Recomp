#!/usr/bin/env python3
"""STOP-1 bounded gate-ON/OFF run series (t_270c19b4).

Runs the established 100M-main-step movie-path segment N times with a hard
per-run timeout (tools/s1d_ab.py has no post-deadline kill, so a stalling run
would hang the series), records rc/wall and the acceptance-relevant counters:

  * BENCH line: scene_movie_entries, scene_y2r_conversions, aggregate rate;
  * escape diagnostic ("translated PC escaped");
  * SVC ArbitrateAddress invalid-handle diagnostic;
  * S1_CFG reporter line (region runs/insns).

usage: python3 tools/stop1_gate_runs.py --bin PATH --runs N --outdir DIR
           [--gate ON|OFF] [--steps N] [--timeout S] [--env K=V ...]
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SEED = Path("/home/hermes/DQ8/target/userdata-title-1")
BENCH_RE = re.compile(r"^BENCH (.*)$", re.M)
S1_RE = re.compile(r"^S1_CFG entry=(\S+) runs=(\d+) insns=(\d+)$", re.M)


def run_once(args, tag):
    userdata = args.outdir / f"userdata-{tag}"
    if userdata.exists():
        shutil.rmtree(userdata)
    shutil.copytree(SEED, userdata)
    env = dict(os.environ)
    env.update({
        "DQ8_HEADLESS": "1",
        "DQ8_RUN_GUEST_THREADS": "1",
        "DQ8_BENCH_STEPS": str(args.steps),
        "DQ8_BENCH_NO_SNAPSHOT": "1",
        "DQ8_USER_DATA_DIR": str(userdata),
    })
    env.pop("DQ8_S1_CFG", None)
    env.pop("DQ8_S1_CFG_END", None)
    if args.gate == "ON":
        env["DQ8_S1_CFG"] = args.gate_entry
    for kv in args.env:
        k, _, v = kv.partition("=")
        env[k] = v

    errlog = args.outdir / f"{args.arm}-{tag}.err"
    cmd = [str(args.bin), args.code, args.romfs]
    t0 = time.time()
    rec = {"tag": tag, "arm": args.arm, "gate": args.gate}
    try:
        with open(errlog, "wb") as fh:
            proc = subprocess.Popen(cmd, env=env, stdout=subprocess.DEVNULL,
                                    stderr=fh, cwd=ROOT, start_new_session=True)
            try:
                rc = proc.wait(timeout=args.timeout)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, 9)
                rc = proc.wait()
                rec["timeout"] = True
    except Exception as exc:  # noqa: BLE001
        rec["error"] = repr(exc)
        rc = -1
    wall = time.time() - t0
    text = errlog.read_text(errors="replace")
    fields = {}
    bench = BENCH_RE.findall(text)
    if bench:
        for kv in bench[0].split():
            if "=" in kv:
                k, v = kv.split("=", 1)
                fields[k] = v
    s1 = S1_RE.findall(text)
    rec.update({
        "rc": rc, "wall_s": round(wall, 2),
        "steps": fields.get("steps"),
        "movie_entries": fields.get("scene_movie_entries"),
        "y2r_batches": fields.get("scene_y2r_conversions"),
        "agg_rate": fields.get("aggregate_instr_per_sec"),
        "escaped": "translated PC escaped" in text,
        "bad_handle": "SVC ArbitrateAddress: invalid handle" in text,
        "s1": [{"entry": e, "runs": int(r), "insns": int(i)} for e, r, i in s1],
        "err": str(errlog),
    })
    bad = [l for l in text.splitlines()
           if l.startswith("SVC ArbitrateAddress") or l.startswith("translated PC escaped")]
    if bad:
        rec["diagnostic"] = bad[0]
    (args.outdir / f"{args.arm}-{tag}.json").write_text(json.dumps(rec, indent=1))
    return rec


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--bin", required=True, type=Path)
    p.add_argument("--arm", required=True)
    p.add_argument("--runs", type=int, default=1)
    p.add_argument("--start", type=int, default=1)
    p.add_argument("--outdir", required=True, type=Path)
    p.add_argument("--gate", choices=["ON", "OFF"], default="ON")
    p.add_argument("--gate-entry", default="0x001A93A8")
    p.add_argument("--steps", type=int, default=100_000_000)
    p.add_argument("--timeout", type=float, default=300.0)
    p.add_argument("--env", action="append", default=[])
    p.add_argument("--code", default="DQ8.code")
    p.add_argument("--romfs", default="target/dq8.romfs")
    args = p.parse_args()
    args.outdir.mkdir(parents=True, exist_ok=True)
    ok = 0
    for i in range(args.start, args.start + args.runs):
        rec = run_once(args, f"{i:02d}")
        good = (rec["rc"] == 0 and not rec.get("timeout")
                and rec["movie_entries"] and int(rec["movie_entries"]) > 0
                and not rec["escaped"] and not rec["bad_handle"])
        ok += bool(good)
        print(f"{args.arm} {rec['tag']} rc={rec['rc']} wall={rec['wall_s']}s "
              f"movie={rec['movie_entries']} y2r={rec['y2r_batches']} "
              f"agg={rec['agg_rate']} s1={rec['s1']} timeout={rec.get('timeout', False)} "
              f"escaped={rec['escaped']} bad_handle={rec['bad_handle']} "
              f"GOOD={good}", flush=True)
        if rec.get("diagnostic"):
            print("  DIAG: " + rec["diagnostic"], flush=True)
    print(f"SUMMARY {args.arm}: {ok}/{args.runs} GOOD", flush=True)


if __name__ == "__main__":
    main()
