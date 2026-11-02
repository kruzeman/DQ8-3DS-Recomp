#!/usr/bin/env python3
"""S1b prototype A/B runner: gate off vs gate on, same binary pair, same scene.

Runs each arm N times with an isolated userdata dir, fixed BENCH step budget,
and records per run: wall, aggregate instr/s, FRAMEPROF guest_ms distribution,
and the S1_CFG per-region run/insn counters (gate-on arm).
"""
import argparse, json, os, re, statistics, subprocess, sys, time
from pathlib import Path

FRAME_RE = re.compile(r"(?m)^FRAMEPROF f=(\d+) t=([\d.]+) total_ms=([\d.]+) guest_ms=([\d.]+) gl_ms=([\d.]+)")
BENCH_RE = re.compile(r"(?m)^BENCH steps=(\d+) .*wall=([\d.]+) sec instr_per_sec=([\d.]+)M .*aggregate_steps=(\d+) aggregate_instr_per_sec=([\d.]+)M")
S1_RE = re.compile(r"(?m)^S1_CFG entry=0x([0-9A-F]{8}) runs=(\d+) insns=(\d+)$")
SC_RE = re.compile(r"(?m)^S1_SELFCHECK entry=0x([0-9A-F]{8}) ok=(\d+) bad=(\d+)$")


def run_one(binary, state, scene, outdir, steps, gate, selfcheck, extra_env=None):
    outdir.mkdir(parents=True, exist_ok=True)
    ud = outdir / "ud"
    env = dict(os.environ)
    env.update({
        "DQ8_HEADLESS": "1", "DQ8_DISPLAY": "0", "DQ8_RUN_GUEST_THREADS": "1",
        "DQ8_GL": "1", "DQ8_FRAMEPROF": "1", "DQ8_BENCH_STEPS": str(steps),
        "DQ8_USER_DATA_DIR": str(ud),
    })
    if gate:
        env["DQ8_S1_CFG"] = "all"
    if selfcheck:
        env["DQ8_S1_SELFCHECK"] = "1"
    if extra_env:
        env.update(extra_env)
    t0 = time.time()
    p = subprocess.run(["timeout", "-s", "KILL", "900", str(Path(binary).resolve()),
                        "--load-state", str(state),
                        "/home/hermes/DQ8/worktrees/allfix/DQ8.code",
                        "/home/hermes/DQ8/target/dq8.romfs"],
                       cwd=str(outdir), env=env, capture_output=True, text=True)
    wall = time.time() - t0
    (outdir / "err.log").write_text(p.stderr)
    err = p.stderr
    frames = [(float(m.group(4)), float(m.group(3))) for m in
              (FRAME_RE.match(l) for l in err.splitlines()) if m]
    guest = [g for g, t in frames if t > 0]
    active = [g for g, t in frames if t >= 1.0]
    bench = BENCH_RE.search(err)
    s1 = S1_RE.findall(err)
    sc = SC_RE.findall(err)
    return {
        "returncode": p.returncode,
        "wall_s": wall,
        "bench": bench.groups() if bench else None,
        "frames": len(frames),
        "guest_mean_ms": statistics.mean(guest) if guest else None,
        "guest_p50_ms": statistics.median(guest) if guest else None,
        "guest_p90_ms": (sorted(guest)[int(0.9 * (len(guest) - 1))] if guest else None),
        "active_frames": len(active),
        "active_mean_ms": statistics.mean(active) if active else None,
        "active_p90_ms": (sorted(active)[int(0.9 * (len(active) - 1))] if active else None),
        "guest_total_ms": sum(guest),
        "s1_regions": len(s1),
        "s1_runs": sum(int(r) for _, r, _ in s1),
        "s1_insns": sum(int(i) for _, _, i in s1),
        "selfcheck_ok": sum(int(o) for _, o, _ in sc),
        "selfcheck_bad": sum(int(b) for _, _, b in sc),
        "selfcheck_regions": len(sc),
        "escapes": err.count("escaped to"),
        "memfaults": err.count("GUEST MEMORY FAULT"),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--off", required=True)
    ap.add_argument("--on", required=True)
    ap.add_argument("--scene", required=True)
    ap.add_argument("--state", required=True)
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--steps", type=int, default=60000000)
    ap.add_argument("--outdir", required=True)
    ap.add_argument("--selfcheck", action="store_true")
    ap.add_argument("--extra", default="")
    a = ap.parse_args()
    out = Path(a.outdir); out.mkdir(parents=True, exist_ok=True)
    extra = dict(kv.split("=", 1) for kv in a.extra.split(",") if kv)
    res = {"scene": a.scene, "steps": a.steps, "off": [], "on": []}
    for i in range(a.runs):
        for arm in ("off", "on"):
            d = out / f"{arm}-{i}"
            r = run_one(Path(a.off if arm == "off" else a.on), Path(a.state),
                        a.scene, d, a.steps, arm == "on", a.selfcheck, extra)
            r["arm"] = arm; r["run"] = i
            res[arm].append(r)
            print(json.dumps(r))
    (out / "result.json").write_text(json.dumps(res, indent=1))
    for arm in ("off", "on"):
        rs = res[arm]
        gm = [r["guest_mean_ms"] for r in rs if r["guest_mean_ms"]]
        p90 = [r["guest_p90_ms"] for r in rs if r["guest_p90_ms"]]
        ips = [float(r["bench"][4]) for r in rs if r["bench"]]
        am = [r["active_mean_ms"] for r in rs if r["active_mean_ms"]]
        ap90 = [r["active_p90_ms"] for r in rs if r["active_p90_ms"]]
        ns = [r["guest_total_ms"] * 1e6 / float(r["bench"][3]) for r in rs if r["bench"]]
        print(f"SUMMARY {arm} frames_active_mean={statistics.mean(am) if am else 0:.3f}ms "
              f"active_p90={statistics.mean(ap90) if ap90 else 0:.3f}ms "
              f"ns_per_guest_insn={statistics.mean(ns) if ns else 0:.3f} "
              f"aggregate_Mips={statistics.mean(ips) if ips else 0:.3f} "
              f"wall={statistics.mean([r['wall_s'] for r in rs]):.1f}s "
              f"s1_insns={[r['s1_insns'] for r in rs]}")


if __name__ == "__main__":
    main()
