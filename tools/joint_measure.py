#!/usr/bin/env python3
"""Joint performance measurement harness (audit t_bb46a3d2, scenario driver).

One run simultaneously records, WITHOUT touching runtime source:
  * guest instructions/sec (perf_main_retired + perf_worker_retired deltas)
  * guest vblanks/sec (pqt_frames delta, PDC0 scan-out events)
  * PICA draw calls/sec (pica_draw_count delta)
  * unique presented-content rate (sha256 of host_shadow_top/bot at 10 Hz;
    a repeated framebuffer does NOT change the hash)  + gsp_fb_shown flips
  * per-thread host CPU from /proc/<pid>/task/<tid>/stat deltas (1 Hz)
  * a frame-pointer stack sampler (ptrace, bounded) over declared windows
    (perf is unusable: perf_event_paranoid=4)
  * memory-dumped screen PNGs at requested seconds (no guest-side capture
    writes: DQ8_CAPTURE_PPM stays unset, DQ8_BENCH_NO_SNAPSHOT=1)

Every game process is launched with DQ8_RUN_GUEST_THREADS=1 (set both here and
on the shell command line of the invoking terminal command).

Evidence: reports/perf_realtime_e8d079b/joint/<RUN_NAME>/
  metrics.csv  samples.raw  summary.txt  meta.txt  run.log  shot_tNN.png
"""
import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(REPO, "tools"))
import run_idle_scenario as ris  # noqa: E402  (importable helpers only)

CODE = "/home/hermes/DQ8/DQ8.code"
ROMFS = "/home/hermes/DQ8/target/dq8.romfs"

COUNTERS = [
    "perf_main_retired", "perf_worker_retired",
    "perf_main_dispatches", "perf_worker_dispatches",
    "pica_draw_count", "pqt_frames",
    "perf_movie_entries", "perf_y2r_conversions",
    "perf_main_sleeps", "perf_main_sleep_ns", "perf_worker_sleeps",
    "perf_worker_parks", "perf_main_wait_spins",
    "perf_host_yields", "perf_host_sleeps", "perf_host_sleep_ns",
    "perf_dsp_ticks", "perf_dsp_resyncs",
    "bench_snapshots",
]
SHADOW_TOP = ("host_shadow_top", 400 * 240 * 4)
SHADOW_BOT = ("host_shadow_bot", 320 * 240 * 4)
GSP_FB_SHOWN = "gsp_fb_shown"  # 2 x uint32
GSP_FB_VALID = "gsp_fb_valid"  # 2 x int32


def log(msg):
    print("[%s] %s" % (time.strftime("%H:%M:%S"), msg), flush=True)


class Mem:
    def __init__(self, pid):
        self.fh = open("/proc/%d/mem" % pid, "rb", buffering=0)

    def read(self, addr, size):
        try:
            self.fh.seek(addr)
            blob = self.fh.read(size)
            return blob if len(blob) == size else None
        except OSError:
            return None

    def close(self):
        try:
            self.fh.close()
        except OSError:
            pass


def png_save(path, rgb, w, h):
    from PIL import Image
    Image.frombytes("RGB", (w, h), bytes(rgb)).save(path)


def shadow_png(top, bot, path):
    """Dump both shadow buffers as one stacked PNG (top over bottom)."""
    from PIL import Image
    t = Image.frombytes("RGBA", (400, 240), bytes(top)).convert("RGB")
    b = Image.frombytes("RGBA", (320, 240), bytes(bot)).convert("RGB")
    out = Image.new("RGB", (400, 480), (0, 0, 0))
    out.paste(t, (0, 0))
    out.paste(b, (40, 240))
    out.save(path)


def parse_args():
    ap = argparse.ArgumentParser()
    ap.add_argument("run_name")
    ap.add_argument("--bin", default="target/dq8-audit-gl")
    ap.add_argument("--max-wall", type=int, default=165)
    ap.add_argument("--window", type=int, nargs=2, action="append",
                    default=[], metavar=("START", "END"),
                    help="stack-sampling window(s); repeatable")
    ap.add_argument("--press-at", type=int, default=None,
                    help="second at which to hold A for --press-ms")
    ap.add_argument("--press-ms", type=int, default=1000)
    ap.add_argument("--headless", action="store_true")
    ap.add_argument("--no-trace-time", action="store_true",
                    help="omit DQ8_TRACE_TIME so pqt_frames stays monotonic")
    ap.add_argument("--gl", action="store_true", help="DQ8_GL=1")
    ap.add_argument("--shots", default="",
                    help="comma seconds at which to dump shadow PNGs")
    ap.add_argument("--hz", type=int, default=25, help="sample rate in windows")
    return ap.parse_args()


def main():
    args = parse_args()
    bin_path = args.bin if os.path.isabs(args.bin) else os.path.join(REPO, args.bin)
    out = os.path.join(HERE, "joint", args.run_name)
    if os.path.exists(out):
        log("refusing to reuse evidence dir %s" % out)
        return 2
    os.makedirs(out)
    for req in (CODE, ROMFS, bin_path):
        if not os.path.isfile(req):
            log("missing %s" % req)
            return 2

    shots = sorted({int(s) for s in args.shots.split(",") if s.strip()})
    input_file = os.path.join(out, "input.txt")
    with open(input_file, "w") as fh:
        fh.write("0\n")

    env = dict(os.environ)
    env.pop("DQ8_TRACE_TIME", None)
    env.pop("DQ8_GL", None)
    env.pop("DQ8_HEADLESS", None)
    env.pop("DQ8_DISPLAY", None)
    env.update({
        "DQ8_RUN_GUEST_THREADS": "1",
        "DQ8_PROGRESS_SECONDS": "10",
        "DQ8_BENCH_NO_SNAPSHOT": "1",
        "DQ8_CAPTURE_STATS": "1",
        "DQ8_TRACE_INPUT": "1",
        "DQ8_INPUT_FILE": input_file,
        "DQ8_USER_DATA_DIR": os.path.join(out, "userdata"),
        "SDL_AUDIODRIVER": "dummy",
        "DQ8_AUDIO_SDL": "1",
    })
    if not args.no_trace_time:
        env["DQ8_TRACE_TIME"] = "1"
    if args.headless:
        env["DQ8_HEADLESS"] = "1"
        env["DQ8_DISPLAY"] = "0"
    if args.gl:
        env["DQ8_GL"] = "1"
    os.makedirs(env["DQ8_USER_DATA_DIR"])

    sha = hashlib.sha256(open(bin_path, "rb").read()).hexdigest()
    commit = subprocess.run(["git", "-C", REPO, "rev-parse", "HEAD"],
                            capture_output=True, text=True).stdout.strip()
    with open(os.path.join(out, "meta.txt"), "w") as fh:
        fh.write("run_name=%s\ncommit=%s\nbinary=%s\nbinary_sha256=%s\n"
                 % (args.run_name, commit, bin_path, sha))
        fh.write("code=%s\nromfs=%s\n" % (CODE, ROMFS))
        for key in sorted(env):
            if key.startswith(("DQ8_", "SDL_", "DISPLAY")):
                fh.write("%s=%s\n" % (key, env[key]))
        fh.write("windows=%s\nshots=%s\nmax_wall=%d\nhz=%d\npress_at=%s\n"
                 % (args.window, shots, args.max_wall, args.hz, args.press_at))
        fh.write("started_wall=%s\n" % time.strftime("%Y-%m-%d %H:%M:%S %z"))

    run_log = open(os.path.join(out, "run.log"), "wb")
    proc = subprocess.Popen([bin_path, CODE, ROMFS], cwd=REPO, env=env,
                            stdout=run_log, stderr=subprocess.STDOUT,
                            start_new_session=True)
    pid = proc.pid
    log("started pid=%d %s" % (pid, os.path.basename(bin_path)))

    data_syms = ris.load_data_symbols(bin_path)
    base = 0
    for _ in range(40):
        base = ris.load_base(bin_path, pid)
        if base:
            break
        time.sleep(0.25)
    mem = Mem(pid)
    addr = {}
    for name in COUNTERS + [GSP_FB_SHOWN, GSP_FB_VALID, SHADOW_TOP[0], SHADOW_BOT[0]]:
        lst = data_syms.get(name, [])
        if not lst:
            log("WARNING: symbol %s not found" % name)
            continue
        addr[name] = lst[0][0] + base

    def u64(name):
        blob = mem.read(addr[name], 8)
        return struct.unpack("<Q", blob)[0] if blob else None

    def u32pair(name):
        blob = mem.read(addr[name], 8)
        return struct.unpack("<II", blob) if blob else (None, None)

    # stack samplers: round-robin over all threads at --hz aggregate
    sym = ris.Symbolizer(bin_path, pid)
    with open(os.path.join(out, "maps.txt"), "w") as fh:
        for lo, hi, perms, off, label in sym.maps:
            fh.write("%x-%x %s %x %s\n" % (lo, hi, perms, off, label))
    samplers = {}
    hot_tids = []
    weighted_tids = []
    pause_total = [0.0]
    sample_log = open(os.path.join(out, "samples.raw"), "w")

    def sample_tick(t):
        tids = weighted_tids or list(hot_tids) or list(ris.list_tids(pid))
        if not tids:
            return
        tid = tids[int(t * 1000) % len(tids)]
        smp = samplers.get(tid)
        if smp is None:
            try:
                smp = ris.RipSampler(pid, tid, bin_path, args.hz)
            except Exception:
                return
            samplers[tid] = smp
        rec = smp.sample()
        if rec:
            ts, rip, rbp, rsp, chain = rec
            sample_log.write(json.dumps({
                "t": ts, "tid": tid, "chain": chain,
                "names": [sym.describe(x) for x in chain]}) + "\n")

    csv_fh = open(os.path.join(out, "metrics.csv"), "w")
    csv_fh.write("t,counters_ok,main_instr_d,worker_instr_d,draws_d,vblanks_d,"
                 "movie_d,y2r_d,wait_spins_d,parks_d,host_sleeps_d,main_sleeps_d,"
                 "dsp_ticks_d,dsp_resyncs_d,dispatches_d,snapshots_d,"
                 "fb_flips_d,top_hash,bot_hash,top_nz,cpu_main,cpu_other,cpu_total\n")
    proc_log = open(os.path.join(out, "proc-cpu.log"), "w")

    prev = None
    prev_sec_counts = None
    prev_hashes = (None, None)
    prev_flip = None
    last_sample_ts = [0.0]
    comm_of = {}
    start = time.time()
    last_sec = -1
    cpu_prev = {}
    done_shots = set()
    pressed = False
    press_end = 0.0
    exit_note = ""
    in_window = False
    sample_count = 0

    while True:
        now = time.time()
        el = now - start
        if el >= args.max_wall:
            exit_note = "max-wall reached"
            break
        if proc.poll() is not None:
            exit_note = "runner exited rc=%s" % proc.returncode
            break

        sec = int(el)
        if sec != last_sec:
            last_sec = sec
            cur = {n: u64(n) for n in COUNTERS}
            shown = u32pair(GSP_FB_SHOWN)
            ok = all(v is not None for v in cur.values())
            top = mem.read(addr[SHADOW_TOP[0]], SHADOW_TOP[1])
            bot = mem.read(addr[SHADOW_BOT[0]], SHADOW_BOT[1])
            th = hashlib.sha256(top).hexdigest()[:12] if top else "none"
            bh = hashlib.sha256(bot).hexdigest()[:12] if bot else "none"
            top_nz = 0
            if top:
                top_nz = sum(1 for i in range(0, len(top), 4)
                             if top[i] or top[i + 1] or top[i + 2])
            # per-thread CPU (deltas) + hot-tid list for the sampler
            cpu_now = {}
            comm_of = {}
            hot = []
            for tid in ris.list_tids(pid):
                try:
                    comm, ut, st = ris.task_stat(pid, tid)
                except OSError:
                    continue
                cpu_now[tid] = ut + st
                comm_of[tid] = comm
            cpu_main_d = cpu_other_d = 0
            wlist = []
            proc_log.write("t=%d" % sec)
            for tid in sorted(cpu_now):
                d = cpu_now[tid] - cpu_prev.get(tid, cpu_now[tid])
                if d > 0:
                    hot.append(tid)
                    wlist.extend([tid] * d)
                proc_log.write(" %d:%s:%d" % (tid, comm_of[tid], d))
                if tid == pid:
                    cpu_main_d = d
                else:
                    cpu_other_d += d
            proc_log.write("\n")
            proc_log.flush()
            hot_tids[:] = hot
            weighted_tids[:] = wlist
            cpu_prev = cpu_now
            hz = os.sysconf("SC_CLK_TCK")

            def delta(k):
                if prev_sec_counts is None or prev_sec_counts.get(k) is None or cur.get(k) is None:
                    return 0
                return cur[k] - prev_sec_counts[k]

            flips = 0
            if prev_flip is not None and shown[0] is not None:
                flips = (1 if shown[0] != prev_flip[0] else 0) + \
                        (1 if shown[1] != prev_flip[1] else 0)
            prev_flip = shown
            csv_fh.write("%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%s,%s,%d,%d,%d,%d\n" % (
                sec, 1 if ok else 0,
                delta("perf_main_retired"), delta("perf_worker_retired"),
                delta("pica_draw_count"), delta("pqt_frames"),
                delta("perf_movie_entries"), delta("perf_y2r_conversions"),
                delta("perf_main_wait_spins"), delta("perf_worker_parks"),
                delta("perf_host_sleeps"), delta("perf_main_sleeps"),
                delta("perf_dsp_ticks"), delta("perf_dsp_resyncs"),
                delta("perf_main_dispatches") + delta("perf_worker_dispatches"),
                delta("bench_snapshots"), flips, th, bh, top_nz,
                cpu_main_d, cpu_other_d,
                int((cpu_main_d + cpu_other_d) * 100 / hz)))
            csv_fh.flush()
            prev_sec_counts = cur
            if sec in shots and sec not in done_shots and top and bot:
                done_shots.add(sec)
                shadow_png(top, bot, os.path.join(out, "shot_t%03d.png" % sec))
                log("shot t=%d saved" % sec)

        # input press
        if args.press_at is not None and not pressed and el >= args.press_at:
            pressed = True
            press_end = el + args.press_ms / 1000.0
            with open(input_file, "w") as fh:
                fh.write("1\n")
            log("A press at t=%.1f" % el)
        if pressed and el >= press_end and press_end > 0:
            press_end = 0
            with open(input_file, "w") as fh:
                fh.write("0\n")
            log("A released at t=%.1f" % el)

        # sampling only inside declared windows, at exactly --hz
        want = any(a <= el < b for a, b in args.window)
        if want:
            if not in_window:
                in_window = True
                log("sampling window start t=%.1f" % el)
            if now - last_sample_ts[0] >= 1.0 / max(args.hz, 1):
                last_sample_ts[0] = now
                sample_tick(now)
                sample_count += 1
        elif in_window:
            in_window = False
            log("sampling window end t=%.1f" % el)
        time.sleep(0.002)

    log("loop done: %s samples=%d" % (exit_note, sample_count))
    try:
        proc.terminate()
        proc.wait(timeout=10)
    except Exception:
        pass
    for smp in samplers.values():
        pause_total[0] += smp.pauses
    sample_log.close()
    csv_fh.close()
    proc_log.close()
    run_log.close()
    with open(os.path.join(out, "meta.txt"), "a") as fh:
        fh.write("exit_note=%s\nsamples=%d\nptrace_pause_total_s=%.3f\n"
                 % (exit_note, sample_count, pause_total[0]))

    # ---- compact analysis -------------------------------------------------
    import csv as csvmod
    rows = list(csvmod.DictReader(open(os.path.join(out, "metrics.csv"))))
    lines = []
    lines.append("run=%s commit=%s bin_sha=%s" % (args.run_name, commit[:12], sha[:12]))
    lines.append("exit=%s samples=%d ptrace_pause_s=%.3f windows=%s"
                 % (exit_note, sample_count, pause_total[0], args.window))

    def window_rows(a, b):
        return [r for r in rows if a <= int(r["t"]) < b and int(r["counters_ok"]) == 1]

    def agg(sel, label):
        n = len(sel)
        if not n:
            lines.append("window %s: no rows" % label)
            return
        def s(k):
            return sum(int(r[k]) for r in sel)
        cpu = s("cpu_total")
        lines.append(
            "window %s (%d s): guest_Minstr/s=%.2f (main %.2f + worker %.2f) "
            "vblanks/s=%.1f draws/s=%.1f movie_d=%d y2r_d=%d "
            "fb_flips/s=%.2f distinct_top_hashes=%d cpu: main=%.1f%% other=%.1f%% total=%.1f%% "
            "wait_spins=%d parks=%d host_sleeps=%d main_sleeps=%d dsp_ticks=%d dsp_resyncs=%d snapshots=%d"
            % (label, n,
               (s("main_instr_d") + s("worker_instr_d")) / n / 1e6,
               s("main_instr_d") / n / 1e6, s("worker_instr_d") / n / 1e6,
               s("vblanks_d") / n, s("draws_d") / n,
               s("movie_d"), s("y2r_d"),
               s("fb_flips_d") / n,
               len({r["top_hash"] for r in sel}),
               s("cpu_main") / n, s("cpu_other") / n, cpu / n,
               s("wait_spins_d"), s("parks_d"), s("host_sleeps_d"),
               s("main_sleeps_d"), s("dsp_ticks_d"), s("dsp_resyncs_d"),
               s("snapshots_d")))

    agg(rows, "0-%d" % args.max_wall)
    for a, b in args.window:
        agg(window_rows(a, b), "%d-%d" % (a, b))
        agg(window_rows(max(a, b - 10), b), "last10 %d-%d" % (max(a, b - 10), b))

    # top stacks per window
    samples = []
    try:
        for ln in open(os.path.join(out, "samples.raw")):
            samples.append(json.loads(ln))
    except OSError:
        pass
    for a, b in args.window:
        sel = [s_ for s_ in samples if a <= s_["t"] - start < b]
        from collections import Counter
        leaf = Counter()
        full = Counter()
        for s_ in sel:
            ch = s_["chain"]
            if not ch:
                continue
            names = [sym.describe(rip) for rip in ch]
            leaf[names[0]] += 1
            full[" <- ".join(names)] += 1
        lines.append("--- stacks %d-%d (%d samples)" % (a, b, len(sel)))
        for name, n in leaf.most_common(20):
            lines.append("  leaf %5.1f%% %d %s" % (100.0 * n / max(len(sel), 1), n, name))
        lines.append("  top chains:")
        for ch, n in full.most_common(12):
            lines.append("  chain %4d %s" % (n, ch[:170]))
        per_tid = {}
        for s_ in sel:
            ch = s_["chain"]
            if not ch:
                continue
            per_tid.setdefault(s_["tid"], Counter())[
                " <- ".join(sym.describe(rip) for rip in ch)] += 1
        lines.append("  per-tid:")
        for tid, ctr in sorted(per_tid.items(),
                               key=lambda kv: -sum(kv[1].values()))[:6]:
            lines.append("    tid %s (%s) %d samples"
                         % (tid, comm_of.get(tid, "?"), sum(ctr.values())))
            for ch, n in ctr.most_common(4):
                lines.append("      %4d %s" % (n, ch[:160]))
    with open(os.path.join(out, "summary.txt"), "w") as fh:
        fh.write("\n".join(lines) + "\n")
    log("summary written")
    for ln in lines[:8]:
        log(ln)
    return 0


def sampler_name(chain):
    out = []
    for rip in chain:
        out.append(hex(rip) if isinstance(rip, int) else str(rip))
    return out


if __name__ == "__main__":
    sys.exit(main())
