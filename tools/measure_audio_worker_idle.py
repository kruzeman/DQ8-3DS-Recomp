#!/usr/bin/env python3
"""Audio-worker idle overhead: per-thread CPU **and wakeup rate**, both sinks.

Measurement only.  This script never changes the runtime: it launches the same
frozen idle scenario as ``tools/run_idle_scenario.py`` (fresh userdata, no
input, headless, guest threads ON, PPM capture ON, t=155 screenshot) and adds

  * per-thread voluntary / involuntary context-switch deltas over the window,
    i.e. the wakeup rate of every host thread, and
  * ``/proc/<pid>/task/<tid>/schedstat`` deltas -> time-on-cpu and timeslices,
    i.e. the average run slice per wakeup, and
  * optionally a bounded ptrace RIP profile of the two *audio worker* threads
    over the window, which splits worker CPU between the empty-queue poll loop
    (``dsp_sleep_brief`` = nanosleep(100us) + SPSC try_pop) and per-frame
    silence processing (dsp_process_music / dsp_process_sfx / dsp_audio_out_push).

The scenario helpers (ptrace sampler, DSP counters, /proc parsing) are reused
from ``run_idle_scenario`` so the numbers stay comparable with the frozen
``idle-after-c-{audible,muted}`` baselines.

Evidence: target/audio-workers-evidence/<RUN_NAME>/
  worker-idle-<w0>-<w1>.txt     CPU% + wakeups/s + timeslices/s per thread
  worker-profile-<w0>-<w1>.txt  per-worker leaf/call-chain histogram (opt.)
  proc-idle.log                 raw per-second utime/stime/ctxt/schedstat
  dsp-metrics-<w0>-<w1>.txt     DSP frames/sources + guest PC at window ends
  t155-top.png / t155-bottom.png, meta.txt, timeline.log, run.log

Usage:
  tools/measure_audio_worker_idle.py RUN_NAME [--variant audible|muted]
        [--window 160 190] [--worker-hz 25] [--no-worker-profile]
"""
import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import run_idle_scenario as scen  # noqa: E402  (frozen scenario helpers)


# --------------------------------------------------------------------------- #
# /proc readers that run_idle_scenario does not have
# --------------------------------------------------------------------------- #
def read_ctxt(pid, tid):
    """(voluntary, involuntary) context-switch counters, cumulative."""
    try:
        with open("/proc/%d/task/%d/status" % (pid, tid)) as fh:
            vol = nonvol = None
            for line in fh:
                if line.startswith("voluntary_ctxt_switches:"):
                    vol = int(line.split()[1])
                elif line.startswith("nonvoluntary_ctxt_switches:"):
                    nonvol = int(line.split()[1])
                    break
        return vol or 0, nonvol or 0
    except (OSError, ValueError):
        return 0, 0


def read_schedstat(pid, tid):
    """(run_ns, wait_ns, timeslices), cumulative; zeros if unavailable."""
    try:
        with open("/proc/%d/task/%d/schedstat" % (pid, tid)) as fh:
            parts = fh.read().split()
        return (int(parts[0]), int(parts[1]), int(parts[2]) if len(parts) > 2 else 0)
    except (OSError, ValueError, IndexError):
        return 0, 0, 0


def task_state(pid, tid):
    """Single-char task state from /proc/<pid>/task/<tid>/stat ('R','S',...)."""
    try:
        with open("/proc/%d/task/%d/stat" % (pid, tid), "rb") as fh:
            data = fh.read().decode("utf-8", "replace")
        close = data.rindex(")")
        return data[close + 2:close + 3]
    except (OSError, ValueError):
        return "?"


# DSP counters on top of run_idle_scenario's set: sink/deadline quality of the
# window, read straight out of process memory at both window endpoints.
EXTRA_METRICS = ["dsp_frames_out", "dsp_sources_started", "dsp_sources_finished",
                 "dsp_audio_deadline_misses", "dsp_audio_sink_empty",
                 "dsp_audio_content_gaps", "dsp_audio_sink_checks",
                 "dsp_audio_sink_drops", "dsp_audio_latency_count",
                 "current_guest_pc", "main_guest_pc"]


def snapshot_metrics_extra(pid, binary, data_syms, base):
    """Like scen.snapshot_metrics but with EXTRA_METRICS (same nm/process-mem path)."""
    out = {}
    try:
        with open("/proc/%d/mem" % pid, "rb", buffering=0) as mem:
            for name in EXTRA_METRICS:
                addrs = data_syms.get(name, [])
                for addr, size in addrs:
                    key = name if len(addrs) == 1 else "%s@0x%x" % (name, addr)
                    try:
                        mem.seek(addr + base)
                        blob = mem.read(8)
                    except OSError as exc:
                        out[key + "_read_error"] = "%s @0x%x" % (exc, addr + base)
                        continue
                    if len(blob) < 8 and not name.endswith("pc"):
                        out[key + "_read_error"] = "short read @0x%x" % (addr + base)
                        continue
                    if name.endswith("pc"):
                        out[key] = struct.unpack("<I", blob[:4])[0]
                    else:
                        out[key] = struct.unpack("<Q", blob[:8])[0] if len(blob) >= 8 else blob[0]
    except OSError as exc:
        out["error"] = str(exc)
    return out


# --------------------------------------------------------------------------- #
def parse_args():
    ap = argparse.ArgumentParser(description="audio-worker idle CPU + wakeup harness")
    ap.add_argument("run_name")
    ap.add_argument("--variant", choices=["audible", "muted"], default=None)
    ap.add_argument("--bin", default="target/dq8-audio-workers")
    ap.add_argument("--capture-at", type=int, default=155)
    ap.add_argument("--window", type=int, nargs=2, default=[160, 190], metavar=("START", "END"))
    ap.add_argument("--max-wall", type=int, default=192)
    ap.add_argument("--hz", type=int, default=25, help="main-thread profile rate (as in run_idle_scenario)")
    ap.add_argument("--worker-hz", type=int, default=25, help="audio-worker profile rate")
    ap.add_argument("--worker-sample-all", action="store_true",
                    help="sample the workers whatever their state (default: only while "
                         "runnable, so sleeping threads are never paused and the histogram "
                         "is conditional on where the CPU time actually goes)")
    ap.add_argument("--no-worker-profile", action="store_true",
                    help="skip worker ptrace sampling (use for the unbiased CPU/wakeup run)")
    ap.add_argument("--no-main-profile", action="store_true")
    ap.add_argument("--no-metrics", action="store_true")
    return ap.parse_args()


def worker_role(aff):
    if aff == "3":
        return "audio worker (music, core 3)"
    if aff == "4":
        return "audio worker (sfx/mixer, core 4)"
    return None


def main():
    args = parse_args()
    variant = args.variant or ("muted" if args.run_name.endswith("-muted") else "audible")
    bin_path = args.bin if os.path.isabs(args.bin) else os.path.join(REPO, args.bin)
    out = os.path.join(REPO, "target", "audio-workers-evidence", args.run_name)
    win0, win1 = args.window
    if not (0 < args.capture_at < win0 < win1 <= args.max_wall - 2):
        print("bad timing: need 0 < capture-at < window-start < window-end <= max-wall-2")
        return 2
    if os.path.exists(out):
        print("refusing to reuse evidence directory: %s" % out)
        return 2
    if not (os.path.isfile(scen.CODE) and os.path.isfile(scen.ROMFS)):
        print("missing read-only DQ8 inputs (%s / %s)" % (scen.CODE, scen.ROMFS))
        return 2
    if not os.path.isfile(bin_path):
        print("missing runner binary %s (build with tools/build_split.sh)" % bin_path)
        return 2

    os.makedirs(os.path.join(out, "userdata"))
    os.makedirs(os.path.join(out, "frames"))
    input_file = os.path.join(out, "input.txt")
    with open(input_file, "w") as fh:
        fh.write("0\n")
    run_log = os.path.join(out, "run.log")
    timeline = open(os.path.join(out, "timeline.log"), "w")
    proc_log = open(os.path.join(out, "proc-idle.log"), "w")

    try:
        commit = subprocess.run(["git", "-C", REPO, "rev-parse", "--short", "HEAD"],
                                capture_output=True, text=True).stdout.strip() or "unknown"
    except OSError:
        commit = "unknown"
    sha = hashlib.sha256(open(bin_path, "rb").read()).hexdigest()

    env = dict(os.environ)
    env.update({
        "DQ8_HEADLESS": "1",
        "DQ8_DISPLAY": "0",
        "DQ8_RUN_GUEST_THREADS": "1",
        "DQ8_BENCH_NO_SNAPSHOT": "0",
        "DQ8_PROGRESS_SECONDS": "10",
        "DQ8_TRACE_INPUT": "1",
        "DQ8_INPUT_FILE": input_file,
        "DQ8_USER_DATA_DIR": os.path.join(out, "userdata"),
        "DQ8_CAPTURE_PPM": "1",
        "DQ8_CAPTURE_PPM_DIR": os.path.join(out, "frames"),
        "SDL_AUDIODRIVER": "dummy",
    })
    env["DQ8_AUDIO_SDL"] = "1" if variant == "audible" else "0"

    profile_workers = not args.no_worker_profile
    with open(os.path.join(out, "meta.txt"), "w") as fh:
        fh.write("run_name=%s\nvariant=%s\ncommit=%s\nbinary=%s\nbinary_sha256=%s\n"
                 % (args.run_name, variant, commit, bin_path, sha))
        fh.write("harness=tools/measure_audio_worker_idle.py\n")
        for key in sorted(env):
            if key.startswith(("DQ8_", "SDL_")):
                fh.write("%s=%s\n" % (key, env[key]))
        fh.write("window=%d..%d\ncapture_at=%d\nmax_wall=%d\nmain_profile_hz=%d\n"
                 "worker_profile=%s\nworker_profile_hz=%d\nstarted_wall=%s\n"
                 % (win0, win1, args.capture_at, args.max_wall, args.hz,
                    "on" if profile_workers else "off", args.worker_hz,
                    time.strftime("%Y-%m-%d %H:%M:%S %z")))

    timeline.write("label=%s variant=%s start=%s window=%d..%d capture_at=%d\n"
                   % (args.run_name, variant, time.time(), win0, win1, args.capture_at))
    log_fh = open(run_log, "wb")
    proc = subprocess.Popen([bin_path, scen.CODE, scen.ROMFS], cwd=REPO, env=env,
                            stdout=log_fh, stderr=subprocess.STDOUT, start_new_session=True)
    pid = proc.pid
    timeline.write("pid=%s\n" % pid)
    timeline.flush()
    print("started %s pid=%d variant=%s" % (bin_path, pid, variant), flush=True)

    data_syms = scen.load_data_symbols(bin_path)
    metric_base = scen.load_base(bin_path, pid) if not args.no_metrics else 0

    main_sampler = None
    time.sleep(0.5)
    if not args.no_main_profile and pid in scen.list_tids(pid):
        main_sampler = scen.RipSampler(pid, pid, bin_path, args.hz)
        print("main-thread RIP sampler ready (%d symbols)" % len(main_sampler.sym.syms), flush=True)

    # worker samplers are created lazily once the audio workers exist
    worker_samplers = {}          # tid -> RipSampler
    worker_due = {}               # tid -> next sample time
    worker_paused = 0.0
    worker_samples = 0
    worker_fail = 0
    worker_checks = {}            # tid -> on-cpu observations taken
    worker_skipped = {}           # tid -> ticks where the worker was not runnable

    start = time.time()
    last_sec = -1
    samples = {}                  # sec -> tid -> (utime, stime)
    ctxts = {}                    # sec -> tid -> (vol, nonvol)
    stats = {}                    # sec -> tid -> (run_ns, wait_ns, slices)
    stamps = {}
    allowed = {}
    comm_of = {}
    capture_done = False
    metric_snaps = {}
    profile_on = False
    exit_note = ""
    interval = 1.0 / max(args.hz, args.worker_hz if profile_workers else 1)
    next_tick = start
    main_due = start           # main sampler keeps its own --hz cadence

    while True:
        now = time.time()
        elapsed = now - start
        if elapsed >= args.max_wall:
            exit_note = "max-wall reached"
            break
        if proc.poll() is not None:
            exit_note = "runner exited early rc=%s" % proc.returncode
            break
        sec = int(elapsed)
        if sec != last_sec and sec >= 0:
            last_sec = sec
            tids = scen.list_tids(pid)
            row, cx, ss = {}, {}, {}
            for tid in tids:
                try:
                    comm, ut, st = scen.task_stat(pid, tid)
                except OSError:
                    continue
                row[tid] = (ut, st)
                comm_of[tid] = comm
                if tid not in allowed:
                    allowed[tid] = scen.cpu_allowed(pid, tid)
                cx[tid] = read_ctxt(pid, tid)
                ss[tid] = read_schedstat(pid, tid)
            samples[sec] = row
            ctxts[sec] = cx
            stats[sec] = ss
            stamps[sec] = now
            proc_log.write("t=%d" % sec)
            for tid in sorted(row):
                proc_log.write(" %d:%d:%d:%d:%d:%d:%d" % (
                    tid, row[tid][0], row[tid][1], cx[tid][0], cx[tid][1],
                    ss[tid][0] // 1000000, ss[tid][2]))
            proc_log.write("\n")
            proc_log.flush()

            if sec == args.capture_at and not capture_done:
                capture_done = True
                for src_name, dst_name in (("dq8-screen.ppm", "t%d-top.png" % sec),
                                           ("dq8-screen-bottom.ppm", "t%d-bottom.png" % sec)):
                    src = os.path.join(out, "frames", src_name)
                    dst = os.path.join(out, dst_name)
                    if os.path.exists(src):
                        shutil.copyfile(src, dst[:-4] + ".ppm")
                        try:
                            from PIL import Image
                            with Image.open(dst[:-4] + ".ppm") as im:
                                im.save(dst)
                            os.remove(dst[:-4] + ".ppm")
                        except Exception as exc:
                            timeline.write("t=%s png_convert_failed=%s\n" % (sec, exc))
                        timeline.write("t=%d captured=%s -> %s\n" % (sec, src_name, dst_name))
                    else:
                        timeline.write("t=%d missing=%s\n" % (sec, src_name))
                timeline.flush()

        if sec == win0 and not metric_snaps.get("win0") and not args.no_metrics:
            metric_snaps["win0"] = snapshot_metrics_extra(pid, bin_path, data_syms, metric_base)
            metric_snaps["win0_t"] = time.time()
        if sec == win1 and not metric_snaps.get("win1") and not args.no_metrics:
            metric_snaps["win1"] = snapshot_metrics_extra(pid, bin_path, data_syms, metric_base)
            metric_snaps["win1_t"] = time.time()

        # ---- main-thread profile (same cadence as the frozen harness) ----- #
        if main_sampler is not None:
            if win0 <= elapsed < win1:
                if not profile_on:
                    profile_on = True
                    timeline.write("t=%d main profile start\n" % int(elapsed))
                    timeline.flush()
                if time.time() >= main_due:
                    main_due = time.time() + 1.0 / max(args.hz, 1)
                    main_sampler.sample()
            elif profile_on and elapsed >= win1:
                profile_on = False
                timeline.write("t=%d main profile stop samples=%d\n"
                               % (int(elapsed), len(main_sampler.samples)))
                timeline.flush()

        # ---- audio-worker profile (separate cadence, own TIDs) ------------ #
        if profile_workers and win0 <= elapsed < win1:
            for tid in scen.list_tids(pid):
                if tid in worker_samplers or tid == pid:
                    continue
                aff = allowed.get(tid) or scen.cpu_allowed(pid, tid)
                if worker_role(aff):
                    worker_samplers[tid] = scen.RipSampler(pid, tid, bin_path, args.worker_hz)
                    worker_due[tid] = time.time()
                    timeline.write("t=%d worker sampler ready tid=%d affinity=%s\n"
                                   % (int(elapsed), tid, aff))
                    timeline.flush()
            for tid, sampler in list(worker_samplers.items()):
                if tid not in scen.list_tids(pid):
                    continue
                if time.time() >= worker_due.get(tid, 0):
                    worker_due[tid] = time.time() + 1.0 / max(args.worker_hz, 1)
                    if not args.worker_sample_all:
                        # Only interrupt a worker that is actually on-cpu: sleeping
                        # threads are never paused (no timing perturbation) and the
                        # histogram is conditional on where the worker CPU goes.
                        state = task_state(pid, tid)
                        if state != "R":
                            worker_skipped[tid] = worker_skipped.get(tid, 0) + 1
                            continue
                        worker_checks[tid] = worker_checks.get(tid, 0) + 1
                    before = sampler.pauses
                    rec = sampler.sample()
                    worker_paused += sampler.pauses - before
                    if rec is None:
                        worker_fail += 1
                    else:
                        worker_samples += 1

        next_tick += interval
        delay = next_tick - time.time()
        if delay > 0:
            time.sleep(delay)
        else:
            next_tick = time.time()

    # ------------------------------------------------------------------ #
    rc = None
    if proc.poll() is None:
        timeline.write("sample complete; sending TERM at t=%.0f\n" % (time.time() - start))
        timeline.flush()
        try:
            proc.terminate()
        except OSError:
            pass
        try:
            rc = proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()
            rc = proc.wait()
    else:
        rc = proc.returncode
    if main_sampler is not None:
        main_sampler.close()
    for sampler in worker_samplers.values():
        sampler.close()
    timeline.write("exit=%s end=%s elapsed_s=%.0f note=%s\n"
                   % (rc, time.time(), time.time() - start, exit_note))
    timeline.close()
    proc_log.close()
    log_fh.close()

    # ------------------------------------------------------------------ #
    # window report
    # ------------------------------------------------------------------ #
    w0, w1 = win0, win1
    report = os.path.join(out, "worker-idle-%d-%d.txt" % (w0, w1))
    if w0 not in samples or w1 not in samples:
        with open(report, "w") as fh:
            fh.write("INCOMPLETE: missing samples t=%d or t=%d (%s)\n" % (w0, w1, exit_note))
        print("INCOMPLETE run: missing window samples (%s)" % exit_note)
        return 1

    wall_s = stamps[w1] - stamps[w0]
    rows = []
    for tid in sorted(set(samples[w0]) | set(samples[w1])):
        u0, s0 = samples[w0].get(tid, (0, 0))
        u1, s1 = samples[w1].get(tid, (0, 0))
        ticks = (u1 - u0) + (s1 - s0)
        pct = 100.0 * ticks / (scen.HZ * wall_s)
        v0, n0 = ctxts[w0].get(tid, (0, 0))
        v1, n1 = ctxts[w1].get(tid, (0, 0))
        vol, nonvol = (v1 - v0), (n1 - n0)
        r0, w_0, sl0 = stats[w0].get(tid, (0, 0, 0))
        r1, w_1, sl1 = stats[w1].get(tid, (0, 0, 0))
        run_ms, wait_ms, slices = (r1 - r0) / 1e6, (w_1 - w_0) / 1e6, (sl1 - sl0)
        aff = allowed.get(tid, "?")
        if tid == pid:
            role = "main host thread (guest emulation)"
        else:
            role = worker_role(aff) or ("other thread (affinity %s)" % aff)
        rows.append(dict(tid=tid, comm=comm_of.get(tid, "?"), aff=aff, pct=pct, ticks=ticks,
                         vol=vol, nonvol=nonvol, wps=vol / wall_s, nps=nonvol / wall_s,
                         run_ms=run_ms, wait_ms=wait_ms, slices=slices,
                         sps=slices / wall_s, role=role))

    main_pct = next((r["pct"] for r in rows if r["tid"] == pid), 0.0)
    workers = [r for r in rows if r["role"].startswith("audio worker")]
    worker_pct = sum(r["pct"] for r in workers)

    with open(report, "w") as fh:
        fh.write("scenario=%s variant=%s commit=%s binary_sha256=%s\n"
                 % (args.run_name, variant, commit, sha[:16]))
        fh.write("harness=tools/measure_audio_worker_idle.py (measurement only; runtime unchanged)\n")
        fh.write("method=/proc/<pid>/task/<tid>/stat utime+stime DELTA + "
                 "/status ctxt-switch DELTA + /schedstat DELTA\n")
        fh.write("window=t%d (%.3f) .. t%d (%.3f)  wall_s=%.3f  hz=%d\n"
                 % (w0, stamps[w0], w1, stamps[w1], wall_s, scen.HZ))
        fh.write("shutdown=rc=%s (%s)\n" % (rc, exit_note))
        fh.write("input=none (fresh userdata, input file held at 0)\n")
        fh.write("sdl_sink=%s\n" % ("enabled" if variant == "audible" else "disabled/muted"))
        fh.write("worker_ptrace_profile=%s (%d samples, %d failed, pause %.1f ms)\n\n"
                 % ("on" if profile_workers else "off", worker_samples, worker_fail,
                    worker_paused * 1000.0))
        fh.write("%-5s %-7s %-16s %8s %9s %10s %9s %9s %10s %9s %11s  %s\n"
                 % ("", "tid", "comm", "cpu%", "ticks", "vol/s", "invol/s",
                    "timesl", "timesl/s", "run_ms", "run_slice_us", "role"))
        for r in rows:
            prefix = "MAIN" if r["tid"] == pid else "AUD" if r["role"].startswith("audio") else ""
            slice_us = (r["run_ms"] * 1000.0 / r["slices"]) if r["slices"] else 0.0
            fh.write("%-5s %-7d %-16s %7.2f%% %9d %10.1f %9.1f %9d %10.1f %9.1f %10.1f  %s\n"
                     % (prefix, r["tid"], r["comm"], r["pct"], r["ticks"], r["wps"], r["nps"],
                        r["slices"], r["sps"], r["run_ms"], slice_us, r["role"]))
        fh.write("\nmain_guest_host_thread_cpu_pct=%.2f\n" % main_pct)
        fh.write("worker_total_cpu_pct=%.2f\n" % worker_pct)
        for r in workers:
            fh.write("worker_tid_%d_cpu_pct=%.2f\n" % (r["tid"], r["pct"]))
            fh.write("worker_tid_%d_wakeups_per_s=%.1f (voluntary %.1f + involuntary %.1f)\n"
                     % (r["tid"], r["wps"] + r["nps"], r["wps"], r["nps"]))
            fh.write("worker_tid_%d_timeslices_per_s=%.1f avg_run_slice_us=%.1f\n"
                     % (r["tid"], r["sps"],
                        (r["run_ms"] * 1000.0 / r["slices"]) if r["slices"] else 0.0))
            if r["wps"] + r["nps"] > 0:
                fh.write("worker_tid_%d_cpu_us_per_wakeup=%.3f\n"
                         % (r["tid"], r["pct"] * 10000.0 / (r["wps"] + r["nps"])))
        if workers:
            tot_w = sum(r["wps"] + r["nps"] for r in workers)
            fh.write("worker_total_wakeups_per_s=%.1f\n" % tot_w)
            if tot_w:
                fh.write("worker_total_cpu_us_per_wakeup=%.3f\n" % (worker_pct * 10000.0 / tot_w))
    print("window t%d..%d: main=%.2f%% workers=%.2f%% wakeups/s=%s -> %s"
          % (w0, w1, main_pct, worker_pct,
             ["%.1f" % (r["wps"] + r["nps"]) for r in workers], report), flush=True)

    # ---- DSP endpoint metrics ------------------------------------------- #
    if metric_snaps:
        with open(os.path.join(out, "dsp-metrics-%d-%d.txt" % (w0, w1)), "w") as fh:
            fh.write("read from process memory (no runtime change, no trace overhead)\n")
            for tag in ("win0", "win1"):
                if tag not in metric_snaps:
                    continue
                fh.write("t=%d wall=%.3f\n" % (w0 if tag == "win0" else w1,
                                               metric_snaps[tag + "_t"]))
                for key in sorted(metric_snaps[tag]):
                    val = metric_snaps[tag][key]
                    if isinstance(val, int) and key.endswith("pc"):
                        fh.write("  %s=0x%08x\n" % (key, val))
                    elif isinstance(val, int):
                        fh.write("  %s=%d\n" % (key, val))
                    else:
                        fh.write("  %s=%s\n" % (key, val))
        print("metrics -> dsp-metrics-%d-%d.txt" % (w0, w1), flush=True)

    # ---- worker profiles ------------------------------------------------- #
    if worker_samplers:
        prof = os.path.join(out, "worker-profile-%d-%d.txt" % (w0, w1))
        with open(prof, "w") as fh:
            fh.write("audio worker threads profile, window t%d..%d\n" % (w0, w1))
            fh.write("sampler=ptrace SEIZE/INTERRUPT RIP at %d Hz per worker\n" % args.worker_hz)
            fh.write("selection=%s\n" % (
                "on-cpu only (state=='R' at the tick; sleeping workers are not paused)"
                if not args.worker_sample_all else "any state"))
            fh.write("total_worker_samples=%d failed=%d pause_ms=%.1f\n"
                     % (worker_samples, worker_fail, worker_paused * 1000.0))
            for tid in sorted(worker_samplers):
                fh.write("tid=%d ticks=%d on_cpu=%d not_runnable=%d on_cpu_ratio=%.4f\n"
                         % (tid, worker_checks.get(tid, 0) + worker_skipped.get(tid, 0),
                            worker_checks.get(tid, 0), worker_skipped.get(tid, 0),
                            (worker_checks.get(tid, 0) /
                             max(worker_checks.get(tid, 0) + worker_skipped.get(tid, 0), 1))))
            fh.write("NOTE: pausing a worker removes its CPU while stopped, so this run's "
                     "worker CPU% is biased low; use the -nop profile run for CPU/wakeup numbers.\n")
            for tid, sampler in sorted(worker_samplers.items()):
                n = len(sampler.samples)
                fh.write("\n== tid=%d %s affinity=%s samples=%d ==\n"
                         % (tid, comm_of.get(tid, "?"), allowed.get(tid, "?"), n))
                if not n:
                    continue
                leaf, chain = {}, {}
                for t, rip, rbp, rsp, ch in sampler.samples:
                    d = sampler.sym.describe
                    leaf[d(rip)] = leaf.get(d(rip), 0) + 1
                    key = " <- ".join(d(a) for a in ch) if ch else d(rip) + " <- [no frame chain]"
                    chain[key] = chain.get(key, 0) + 1
                fh.write("-- leaf histogram --\n")
                for name, cnt in sorted(leaf.items(), key=lambda kv: -kv[1]):
                    fh.write("  %6.2f%% %5d  %s\n" % (100.0 * cnt / n, cnt, name))
                fh.write("-- call chains, top 25 --\n")
                for name, cnt in sorted(chain.items(), key=lambda kv: -kv[1])[:25]:
                    fh.write("  %6.2f%% %5d  %s\n" % (100.0 * cnt / n, cnt, name))
        raw = os.path.join(out, "worker-profile-%d-%d.raw" % (w0, w1))
        with open(raw, "w") as fh:
            fh.write("tid t rip rbp rsp chain\n")
            for tid, sampler in sorted(worker_samplers.items()):
                for t, rip, rbp, rsp, ch in sampler.samples:
                    fh.write("%d %.6f 0x%x 0x%x 0x%x %s\n"
                             % (tid, t, rip, rbp, rsp, ",".join("0x%x" % a for a in ch)))
        print("worker profile -> %s" % prof, flush=True)

    print("done: %s (exit=%s, %s)" % (out, rc, exit_note), flush=True)
    return 0 if capture_done and os.path.getsize(report) > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
