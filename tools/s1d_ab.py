#!/usr/bin/env python3
"""S1d movie-path A/B runner (measurement tooling only).

Runs the established 100M-main-step movie-path segment (tools/bench.sh budget,
seeded prepared save, headless, guest threads on) for one arm of the A/B and
records, per run:

  * the BENCH line (main/worker/aggregate retired guest instructions,
    aggregate guest instr/s, wall, scene_movie_entries, scene_y2r_conversions,
    dispatch calls) -- guest-throughput metrics, PERF-02 honest counters;
  * /usr/bin/time -v user/system/wall -- host CPU split (user vs system);
  * a /proc/<pid>/task sampler that splits host CPU seconds between the main
    host thread, guest-worker threads and single-core-pinned (audio) workers;
  * the S1_CFG reporter line when the region gate is on.

Y2R numbers are COMPLETED conversion batches, not unique displayed frames:
the runtime has no unique source-frame counter (Q0 OPT-04 labelling).

usage: tools/s1d_ab.py --arm NAME --bin PATH [--steps N] [--runs N]
                       [--gate ON|OFF] [--outdir DIR] [--code C] [--romfs R]
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


def read_tasks(pid):
    """{tid: (utime+stime ticks, allowed_cpu_list)} for live tasks."""
    out = {}
    taskdir = Path(f"/proc/{pid}/task")
    try:
        tids = list(taskdir.iterdir())
    except FileNotFoundError:
        return out
    for t in tids:
        try:
            stat = (t / "stat").read_text()
            # comm may contain spaces/parens: split on last ')'
            rest = stat.rsplit(")", 1)[1].split()
            utime = int(rest[11])
            stime = int(rest[12])
            allowed = "?"
            for line in (t / "status").read_text().splitlines():
                if line.startswith("Cpus_allowed_list:"):
                    allowed = line.split(":", 1)[1].strip()
                    break
            out[int(t.name)] = (utime + stime, allowed)
        except (FileNotFoundError, ProcessLookupError, IndexError, ValueError):
            continue
    return out


def classify(tid, allowed, pid):
    # Empirically (tools/s1d_ab.py diag): the process has exactly four host
    # threads -- the main thread, two single-core-pinned native DSP/audio
    # workers and SDL's hotplug thread.  ALL guest execution (main context
    # AND every guest worker context) runs on the MAIN host thread, so
    # "guest_worker" below is really "any other host thread".
    if tid == pid:
        return "main"
    if re.fullmatch(r"\d+", allowed) is not None:
        return "native_pinned"
    return "other"


def run_once(args, tag, outdir):
    userdata = outdir / f"userdata-{tag}"
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
    env.pop("DQ8_TRACE_Y2R", None)
    if args.gate == "ON":
        env["DQ8_S1_CFG"] = args.gate_entry

    errlog = outdir / f"{args.arm}-{tag}.err"
    tlog = outdir / f"{args.arm}-{tag}.time"
    cmd = ["/usr/bin/time", "-v", "-o", str(tlog), str(args.bin),
           args.code, args.romfs]
    t0 = time.time()
    with open(errlog, "wb") as fh:
        proc = subprocess.Popen(cmd, env=env, stdout=subprocess.DEVNULL,
                                stderr=fh, cwd=ROOT)
        pid = proc.pid
        # /usr/bin/time is the parent; the game is its child.
        game = None
        deadline = t0 + args.timeout
        while time.time() < deadline:
            try:
                kids = Path(f"/proc/{pid}/task").iterdir()
                children = Path(f"/proc/{pid}/task/{pid}/children").read_text().split()
                if children:
                    game = int(children[0])
                    break
            except (FileNotFoundError, ProcessLookupError, OSError):
                pass
            time.sleep(0.2)
        if game is None:
            proc.kill()
            raise SystemExit(f"{tag}: game child not found")
        prev = read_tasks(game)
        first_seen = dict(prev)
        while proc.poll() is None and time.time() < deadline:
            time.sleep(0.5)
            cur = read_tasks(game)
            for tid, val in cur.items():
                prev[tid] = val
                first_seen.setdefault(tid, val)
        rc = proc.wait()
    wall = time.time() - t0

    # CPU seconds per class: final - first sample (first sample is not zero,
    # so report both the delta and the absolute total).
    cls = {"main": [0, 0], "native_pinned": [0, 0], "other": [0, 0]}
    hz = os.sysconf("SC_CLK_TCK")
    for tid, (ticks, allowed) in prev.items():
        c = classify(tid, allowed, game)
        cls[c][1] += ticks / hz
        base = first_seen.get(tid, (ticks, allowed))[0]
        cls[c][0] += max(0.0, (ticks - base) / hz)

    text = errlog.read_text(errors="replace")
    bench = BENCH_RE.findall(text)
    s1 = S1_RE.findall(text)
    fields = {}
    if bench:
        for kv in bench[0].split():
            if "=" in kv:
                k, v = kv.split("=", 1)
                fields[k] = v
    time_fields = {}
    for line in tlog.read_text(errors="replace").splitlines():
        if ":" in line:
            k, v = line.split(":", 1)
            time_fields[k.strip()] = v.strip()
    rec = {
        "arm": args.arm, "tag": tag, "gate": args.gate, "rc": rc,
        "wall_s": round(wall, 3),
        "bench": fields,
        "s1_cfg": [{"entry": e, "runs": int(r), "insns": int(i)}
                   for e, r, i in s1],
        "time_user_s": float(time_fields.get("User time (seconds)", "nan")),
        "time_sys_s": float(time_fields.get("System time (seconds)", "nan")),
        "time_wall": time_fields.get("Elapsed (wall clock) time (h:mm:ss or m:ss)", ""),
        "host_cpu_s": {k: round(v[1], 2) for k, v in cls.items()},
        "host_cpu_delta_s": {k: round(v[0], 2) for k, v in cls.items()},
        "escaped": "translated PC escaped" in text,
        "errlog": str(errlog),
    }
    (outdir / f"{args.arm}-{tag}.json").write_text(json.dumps(rec, indent=1))
    return rec


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--arm", required=True)
    p.add_argument("--bin", required=True, type=Path)
    p.add_argument("--steps", type=int, default=100_000_000)
    p.add_argument("--runs", type=int, default=1)
    p.add_argument("--gate", choices=["ON", "OFF"], default="OFF")
    p.add_argument("--gate-entry", default="0x001A93A8")
    p.add_argument("--outdir", type=Path, required=True)
    p.add_argument("--code", default="DQ8.code")
    p.add_argument("--romfs", default="target/dq8.romfs")
    p.add_argument("--timeout", type=float, default=600.0)
    p.add_argument("--start", type=int, default=1)
    args = p.parse_args()
    args.outdir.mkdir(parents=True, exist_ok=True)
    for i in range(args.start, args.start + args.runs):
        rec = run_once(args, f"{i:02d}", args.outdir)
        b = rec["bench"]
        print(f"{args.arm} run{rec['tag']} rc={rec['rc']} wall={rec['wall_s']}s "
              f"agg_rate={b.get('aggregate_instr_per_sec')} "
              f"main={b.get('main_steps')} worker={b.get('worker_steps')} "
              f"y2r_batches={b.get('scene_y2r_conversions')} "
              f"movie_entries={b.get('scene_movie_entries')} "
              f"user={rec['time_user_s']} sys={rec['time_sys_s']} "
              f"cpu={rec['host_cpu_s']} s1={rec['s1_cfg']} "
              f"escaped={rec['escaped']}", flush=True)


if __name__ == "__main__":
    main()
