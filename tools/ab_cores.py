#!/usr/bin/env python3
"""t_03f3f620: per-core utilisation + fps / guest-throughput sampler for the
frozen-snapshot S0-vs-S1 A/B.

Runs one DQ8 binary on the savestate scene and records, alongside fps or guest
instruction throughput:

  * per-CPU busy fraction from /proc/stat deltas (system-wide, so llvmpipe
    worker threads show up on their cores),
  * per-thread CPU time of the DQ8 process (/proc/<pid>/task/*/stat) so the
    guest-dispatch thread's single-core time can be compared directly,
  * total process CPU-seconds and the effective number of busy cores.

usage:
  tools/ab_cores.py <label> <binary> <dur_s> [--steps N] [--out DIR]

  default (fps mode): FRAME_STATS+TRACE_TIME for dur_s, then TERM.
  --steps N (tp mode): fixed guest-instruction budget, run to exit or 600 s.

Prints a JSON object at the end (also written to OUT/<label>/sample.json).
"""
import json, os, re, signal, subprocess, sys, time, collections

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TICKS = os.sysconf("SC_CLK_TCK")
HZ = os.sysconf("SC_CLK_TCK")


def cpu_lines():
    out = []
    with open("/proc/stat") as f:
        for line in f:
            if line.startswith("cpu"):
                p = line.split()
                vals = [int(x) for x in p[1:]]
                idle = vals[3] + (vals[4] if len(vals) > 4 else 0)
                out.append((p[0], sum(vals), idle))
    return out


def task_stats(pid, with_cpu=False):
    res = {}
    try:
        tids = os.listdir("/proc/%d/task" % pid)
    except OSError:
        return res
    for tid in tids:
        try:
            with open("/proc/%d/task/%s/stat" % (pid, tid)) as f:
                raw = f.read()
        except OSError:
            continue
        rp = raw.rfind(")")
        comm = raw[raw.find("(") + 1:rp]
        fields = raw[rp + 2:].split()
        try:  # utime/stime are the 14th/15th fields of stat
            ticks = int(fields[11]) + int(fields[12])
        except (IndexError, ValueError):
            continue
        proc = None
        if with_cpu:  # field 39 of stat = processor (last CPU it ran on)
            try:
                proc = int(fields[36])
            except (IndexError, ValueError):
                proc = None
        res[tid] = (comm, ticks, proc)
    return res


def main():
    args = sys.argv[1:]
    if len(args) < 3:
        print(__doc__)
        return 2
    label, binary, dur = args[0], os.path.abspath(args[1]), float(args[2])
    steps, outdir = None, os.path.join(ROOT, "evidence/t_03f3f620")
    i = 3
    while i < len(args):
        if args[i] == "--steps":
            steps = int(args[i + 1]); i += 2
        elif args[i] == "--out":
            outdir = os.path.abspath(args[i + 1]); i += 2
        else:
            print("unknown arg %s" % args[i]); return 2
    od = os.path.join(outdir, label)
    os.makedirs(os.path.join(od, "userdata/archive-00000004-00000000"), exist_ok=True)
    sav = "/home/hermes/DQ8/saves/honeySave.bin"
    if os.path.exists(sav):
        subprocess.run(["cp", "-f", sav,
                        os.path.join(od, "userdata/archive-00000004-00000000/honeySave.bin")])
    with open(os.path.join(od, "input.txt"), "w") as f:
        f.write("0\n")

    env = dict(os.environ)
    env.update({"DQ8_HEADLESS": "1", "DQ8_RUN_GUEST_THREADS": "1",
                "DQ8_TRACE_INPUT": "1", "DQ8_GL": "1",
                "DQ8_USER_DATA_DIR": os.path.join(od, "userdata"),
                "DQ8_INPUT_FILE": os.path.join(od, "input.txt")})
    if steps:
        env.update({"DQ8_BENCH_STEPS": str(steps), "DQ8_BENCH_NO_SNAPSHOT": "1",
                    "DQ8_FRAME_STATS": "0", "DQ8_TRACE_TIME": "0"})
    else:
        env.update({"DQ8_FRAME_STATS": "1", "DQ8_TRACE_TIME": "1"})

    log = open(os.path.join(od, "run.log"), "w")
    t0 = time.time()
    c0 = cpu_lines()
    p = subprocess.Popen([binary, "/home/hermes/DQ8/DQ8.code",
                          "/home/hermes/DQ8/target/dq8.romfs",
                          "--load-state", "/home/hermes/DQ8/saves/dq8-state.sav"],
                         stdout=log, stderr=subprocess.STDOUT, env=env, cwd=ROOT)
    base, last, hist = {}, {}, {}
    next_th, deadline = t0 + 2.0, t0 + (1800 if steps else dur)
    while time.time() < deadline:
        if p.poll() is not None:
            break
        time.sleep(0.25)
        if time.time() >= next_th:
            s = task_stats(p.pid, with_cpu=True)
            if s:
                last = s
                if not base or len(s) > len(base):
                    base = s
                for tid, (_comm, _ticks, proc) in s.items():
                    if proc is not None:
                        hist.setdefault(tid, collections.Counter())[proc] += 1
            next_th = time.time() + 0.5
    wall = time.time() - t0
    if p.poll() is None:
        p.send_signal(signal.SIGTERM)
        try:
            p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            p.kill(); p.wait()
    wall_end = time.time() - t0
    c1 = cpu_lines()
    log.close()

    d = {}
    for (n0, t0v, i0), (n1, t1v, i1) in zip(c0, c1):
        if n0 != "cpu":
            d[n0] = (t1v - t0v, i1 - i0)
    per = {n: (100.0 * (dt - di) / dt if dt else 0.0) for n, (dt, di) in d.items()}
    tot_cpu_s = sum(dt for dt, _ in d.values()) / HZ
    per_thread, proc_per_core = [], collections.Counter()
    for tid, (comm, ticks, _proc) in last.items():
        b = base.get(tid)
        delta = (ticks - b[1]) / HZ if b else 0.0
        per_thread.append({"tid": int(tid), "comm": comm, "cpu_s": round(delta, 3),
                           "duty_pct": round(100.0 * delta / max(wall_end, 1e-9), 1)})
        h = hist.get(tid)
        if h and delta > 0:
            tot = sum(h.values())
            for cpu, cnt in h.items():
                proc_per_core[cpu] += delta * cnt / tot
    per_thread.sort(key=lambda r: -r["cpu_s"])
    cores_used = {int(k): round(v, 2) for k, v in proc_per_core.items()}

    txt = open(os.path.join(od, "run.log"), errors="replace").read()
    res = {"label": label, "binary": binary, "mode": "tp" if steps else "fps",
           "wall_s": round(wall_end, 2), "nproc": len(per),
           "per_cpu_pct": {k: round(v, 1) for k, v in sorted(per.items())},
           "cores_gt50": sum(1 for v in per.values() if v >= 50),
           "cores_gt80": sum(1 for v in per.values() if v >= 80),
           "cores_gt10": sum(1 for v in per.values() if v >= 10),
           "system_cpu_s": round(tot_cpu_s, 1),
           "system_busy_cores_avg": round(tot_cpu_s / max(wall_end, 1e-9), 2),
           "threads": per_thread[:12],
           "proc_cpu_s": round(sum(r["cpu_s"] for r in per_thread), 2),
           "proc_cpu_s_per_core": cores_used,
           "proc_cores_gt10pct": sum(1 for v in cores_used.values()
                                     if v >= 0.10 * wall_end),
           "proc_cores_gt1s": sum(1 for v in cores_used.values() if v >= 1.0),
           "proc_main_thread_cpu_s": per_thread[0]["cpu_s"] if per_thread else None}
    win = re.findall(r"TIME window=([\d.]+)s frames=(\d+)", txt)
    if win:
        fw = sum(float(a) for a, b in win); ff = sum(int(b) for a, b in win)
        res["fps"] = round(ff / fw, 3) if fw else None
        res["ms_per_frame"] = round(1000.0 * fw / ff, 1) if ff else None
        res["frames"] = ff
    b = re.findall(r"BENCH steps=(\d+) pc=\S+ wall=([\d.]+) sec instr_per_sec=([\d.]+)M", txt)
    if b:
        res["bench_steps"] = int(b[-1][0])
        res["bench_internal_wall"] = float(b[-1][1])
        res["insn_per_s_M"] = float(b[-1][2])
    ag = re.findall(r"aggregate_steps=(\d+) aggregate_instr_per_sec=([\d.]+)M", txt)
    if ag:
        res["aggregate_steps"] = int(ag[-1][0])
        res["aggregate_insn_per_s_M"] = float(ag[-1][1])
    for key, pat in (("setup_ms", r"setup_ms=([\d.]+)"),
                     ("draw_ms", r"draw_ms=([\d.]+)"),
                     ("vert_ms", r"vert_ms=([\d.]+)")):
        v = re.findall(pat, txt)
        if v:
            res[key] = round(sum(float(x) for x in v) / len(v), 1)
    with open(os.path.join(od, "sample.json"), "w") as f:
        json.dump(res, f, indent=1)
    print(json.dumps(res))
    return 0


if __name__ == "__main__":
    sys.exit(main())
