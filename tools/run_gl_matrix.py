#!/usr/bin/env python3
"""run_gl_matrix.py - frozen-window renderer matrix cells (B1/B2 measurement).

Runs the frozen idle-dialog scenario (tools/run_idle_scenario.py, anchored
runner) with renderer/capture knobs and adds GL eligibility counter reads
(pica_gl_draws_ok / pica_gl_draws_sw) taken before and after the CPU window,
so a cell answers: did GL-eligible draws grow THROUGH the window?

  tools/run_gl_matrix.py CELL --renderer sw|gl [--capture on|off] \
      [--bin target/dq8-tev] [--batch on|off] [--max-wall 200]

Cell evidence lands in target/audio-workers-evidence/CELL/ (the runner's
files) plus matrix-meta.txt (counters, renderer line, env knobs).

B2 (hardware GL idle budget) must be measured on a GPU host; CI-host
llvmpipe numbers do NOT count (see reports/PERF_IDLE_DIALOG_BGM_DECISION.md
"Target re-scope decision").
"""
import argparse
import ctypes
import os
import re
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
import run_idle_scenario as ris  # noqa: E402  (symbol/base helpers)

COUNTERS = ["pica_gl_draws_ok", "pica_gl_draws_sw", "pgl_flushes",
            "pgl_refills", "pgl_cleared", "pgl_draws"]


def read_counters(pid, binary, syms, base):
    """Read the u64 GL counters from the live process via /proc/<pid>/mem."""
    out = {}
    try:
        with open("/proc/%d/mem" % pid, "rb", buffering=0) as mem:
            for name in COUNTERS:
                entries = syms.get(name)
                if not entries:
                    continue
                mem.seek(base + entries[0][0])
                raw = mem.read(8)
                if len(raw) == 8:
                    out[name] = int.from_bytes(raw, "little")
    except OSError:
        pass
    return out


def find_game_pid(bin_abs):
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            if os.readlink("/proc/%s/exe" % pid) == bin_abs:
                return int(pid)
        except OSError:
            continue
    return None


def main():
    ap = argparse.ArgumentParser(description="renderer matrix cell runner")
    ap.add_argument("cell")
    ap.add_argument("--renderer", choices=["sw", "gl"], required=True)
    ap.add_argument("--capture", choices=["on", "off"], default="on")
    ap.add_argument("--batch", choices=["on", "off"], default="on",
                    help="DQ8_GL_BATCH (batched readback); off = per-draw")
    ap.add_argument("--bin", default="target/dq8-tev")
    ap.add_argument("--max-wall", type=int, default=200)
    ap.add_argument("--window", type=int, nargs=2, default=[160, 190])
    ap.add_argument("--capture-at", type=int, default=155)
    ap.add_argument("--counters-at", type=int, nargs=2, default=[150, 195],
                    metavar=("PRE", "POST"))
    ap.add_argument("--no-profile", action="store_true",
                    help="skip the ptrace profiler (the CPU metric is the "
                         "task-stat delta either way)")
    args = ap.parse_args()

    bin_abs = args.bin if os.path.isabs(args.bin) else os.path.join(REPO, args.bin)
    env = dict(os.environ)
    if args.renderer == "gl":
        env["DQ8_GL"] = "1"
    else:
        env.pop("DQ8_GL", None)
    if args.capture == "off":
        env["DQ8_CAPTURE_PPM"] = "0"
    if args.renderer == "gl":
        env["DQ8_GL_BATCH"] = "1" if args.batch == "on" else "0"
    env["DQ8_DUMP_TRANSFER_PREFIX"] = os.path.join(
        REPO, "target", "audio-workers-evidence", args.cell, "tr")

    syms = ris.load_data_symbols(bin_abs)  # {name: [(addr, size)]}
    pre_at, post_at = args.counters_at

    cmd = [sys.executable, os.path.join(REPO, "tools", "run_idle_scenario.py"),
           args.cell, "--bin", bin_abs, "--max-wall", str(args.max_wall),
           "--window", str(args.window[0]), str(args.window[1]),
           "--capture-at", str(args.capture_at)]
    if args.no_profile:
        cmd.append("--no-profile")
    # NOTE: the log lives OUTSIDE the evidence dir - run_idle_scenario.py
    # refuses to start when its evidence dir already exists.
    log_path = os.path.join(REPO, "target", "gl-matrix-logs", args.cell + ".log")
    os.makedirs(os.path.dirname(log_path), exist_ok=True)
    with open(log_path, "w") as hlog:
        proc = subprocess.Popen(cmd, cwd=REPO, env=env, stdout=hlog,
                                stderr=subprocess.STDOUT)
        game_pid = None
        base = 0
        snapshots = {}
        t0 = time.time()
        got_pre = got_post = False
        while proc.poll() is None:
            time.sleep(1.0)
            elapsed = time.time() - t0
            if game_pid is None:
                game_pid = find_game_pid(bin_abs)
                if game_pid:
                    base = ris.load_base(bin_abs, game_pid)
            if game_pid and not got_pre and elapsed >= pre_at:
                snapshots["pre"] = read_counters(game_pid, bin_abs, syms, base)
                got_pre = True
            if game_pid and not got_post and elapsed >= post_at:
                snapshots["post"] = read_counters(game_pid, bin_abs, syms, base)
                got_post = True
        rc = proc.wait()

    meta = os.path.join(REPO, "target", "audio-workers-evidence", args.cell,
                        "matrix-meta.txt")
    renderer_line = ""
    run_log = os.path.join(REPO, "target", "audio-workers-evidence", args.cell,
                           "run.log")
    try:
        with open(run_log) as fh:
            for line in fh:
                if "GL backend: vendor=" in line:
                    renderer_line = line.strip()
                    break
    except OSError:
        pass
    with open(meta, "w") as fh:
        fh.write("cell=%s renderer=%s capture=%s batch=%s bin=%s\n" % (
            args.cell, args.renderer, args.capture, args.batch,
            os.path.basename(bin_abs)))
        fh.write("renderer_line=%s\n" % (renderer_line or "(none)"))
        for phase in ("pre", "post"):
            snap = snapshots.get(phase, {})
            fh.write("counters_%s=%s\n" % (
                phase, " ".join("%s=%d" % (k, snap.get(k, -1))
                                for k in COUNTERS)))
        pre = snapshots.get("pre", {})
        post = snapshots.get("post", {})
        fh.write("window_delta=%s\n" % " ".join(
            "%s=%d" % (k, post.get(k, 0) - pre.get(k, 0)) for k in COUNTERS))
    print("matrix cell %s done rc=%d; meta: %s" % (args.cell, rc, meta))
    for phase in ("pre", "post"):
        print("  %s: %s" % (phase, snapshots.get(phase)))
    return rc


if __name__ == "__main__":
    sys.exit(main())
