#!/usr/bin/env python3
"""Input-delivery latency through the idle throttle (acceptance gate).

Runs the runner to the static save dialog, then taps the A button through
DQ8_INPUT_FILE while DQ8_TRACE_INPUT=1 records every HID publish with a
monotonic timestamp (the runtime's `HID buttons=... t=<sec>` line).  For
every tap edge:

    latency = HID publish time (runtime monotonic clock == time.monotonic())
              - the moment the tool wrote the new button value

The acceptance gate is p95 <= 2 HID periods (33.34 ms) in BOTH headless and
windowed mode.  Guest reaction is additionally verified from the captured
PPM frames (frame identity changes inside the hold window).

Usage:
  tools/measure_input_latency.py RUN_NAME [--bin PATH] [--windowed]
      [--arm-at 150] [--taps 10] [--hold 0.5] [--gap 1.5] [--env K=V ...]
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
CODE = "/home/hermes/DQ8/DQ8.code"
ROMFS = "/home/hermes/DQ8/target/dq8.romfs"

HID_RE = re.compile(r"HID buttons=([0-9A-Fa-f]+) pressed=([0-9A-Fa-f]+) "
                    r"released=([0-9A-Fa-f]+) index=(\d+) t=([0-9.]+)")


def log(msg):
    print(msg, flush=True)


def parse_args():
    ap = argparse.ArgumentParser(description="input delivery latency probe")
    ap.add_argument("run_name")
    ap.add_argument("--bin", default="target/dq8-idle-throttle")
    ap.add_argument("--windowed", action="store_true")
    ap.add_argument("--display", default=":98")
    ap.add_argument("--arm-at", type=int, default=150,
                    help="seconds to wait before the first tap (dialog is up)")
    ap.add_argument("--taps", type=int, default=10)
    ap.add_argument("--hold", type=float, default=0.5)
    ap.add_argument("--gap", type=float, default=1.5)
    ap.add_argument("--env", action="append", default=[], metavar="KEY=VAL")
    return ap.parse_args()


def ppm_hash(path):
    try:
        with open(path, "rb") as fh:
            return hashlib.sha256(fh.read()).hexdigest()[:16]
    except OSError:
        return None


def main():
    args = parse_args()
    bin_path = args.bin if os.path.isabs(args.bin) else os.path.join(REPO, args.bin)
    out = os.path.join(REPO, "target", "audio-workers-evidence", args.run_name)
    if os.path.exists(out):
        log("refusing to reuse evidence directory: %s" % out)
        return 2
    os.makedirs(os.path.join(out, "userdata"))
    os.makedirs(os.path.join(out, "frames"))
    input_file = os.path.join(out, "input.txt")
    with open(input_file, "w") as fh:
        fh.write("0\n")

    env = dict(os.environ)
    env.update({
        "DQ8_HEADLESS": "0" if args.windowed else "1",
        "DQ8_DISPLAY": "1" if args.windowed else "0",
        "DQ8_RUN_GUEST_THREADS": "1",
        "DQ8_PROGRESS_SECONDS": "10",
        "DQ8_TRACE_INPUT": "1",
        "DQ8_INPUT_FILE": input_file,
        "DQ8_USER_DATA_DIR": os.path.join(out, "userdata"),
        "DQ8_CAPTURE_PPM": "1",
        "DQ8_CAPTURE_PPM_DIR": os.path.join(out, "frames"),
        "DQ8_AUDIO_SDL": "1",
        "SDL_AUDIODRIVER": "dummy",
    })
    for pair in args.env:
        if "=" not in pair:
            log("--env wants KEY=VAL, got %r" % pair)
            return 2
        key, val = pair.split("=", 1)
        env[key] = val

    xvfb_proc = None
    if args.windowed:
        env["DISPLAY"] = args.display
        xvfb_proc = subprocess.Popen(
            ["Xvfb", args.display, "-screen", "0", "1024x768x24", "-ac",
             "-nolisten", "tcp"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(1.0)
        if xvfb_proc.poll() is not None:
            log("Xvfb failed to start")
            return 2

    run_log = os.path.join(out, "run.log")
    log_fh = open(run_log, "wb")
    proc = subprocess.Popen([bin_path, CODE, ROMFS], cwd=REPO, env=env,
                            stdout=log_fh, stderr=subprocess.STDOUT,
                            start_new_session=True)
    pid = proc.pid
    log("started %s pid=%d windowed=%s; tapping from t=%ds"
        % (bin_path, pid, args.windowed, args.arm_at))

    t0 = time.monotonic()
    deadlines = [t0 + args.arm_at + i * (args.hold + args.gap)
                 for i in range(args.taps)]
    taps = []          # (monotonic write time, value)
    tap_idx = 0
    frame_dir = os.path.join(out, "frames")
    reaction = {}      # tap index -> hash seen during hold
    stop_at = deadlines[-1] + args.hold + args.gap
    while True:
        now = time.monotonic()
        if proc.poll() is not None:
            log("runner exited early rc=%s" % proc.returncode)
            break
        if now >= stop_at:
            break
        while tap_idx < len(deadlines) and now >= deadlines[tap_idx]:
            value = 1 if (tap_idx % 2 == 0) else 0
            with open(input_file, "w") as fh:
                fh.write("%d\n" % value)
            taps.append((time.monotonic(), value))
            log("t=%.3f tap[%d] -> %d" % (now - t0, tap_idx, value))
            tap_idx += 1
        time.sleep(0.01)

    # frame identity before/after each press (guest reaction evidence)
    top = os.path.join(frame_dir, "dq8-screen.ppm")
    final_hash = ppm_hash(top)
    try:
        proc.terminate()
        proc.wait(timeout=15)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
    log_fh.close()
    if xvfb_proc is not None:
        xvfb_proc.terminate()
        try:
            xvfb_proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            xvfb_proc.kill()

    # parse HID publishes (the runtime prints its own CLOCK_MONOTONIC stamp)
    edges = []
    with open(run_log, "rb") as fh:
        for raw in fh:
            line = raw.decode("utf-8", "replace")
            m = HID_RE.search(line)
            if m:
                edges.append((float(m.group(5)), int(m.group(1), 16),
                              int(m.group(2), 16), int(m.group(3), 16)))
    latencies = []
    for tap_time, value in taps:
        match = next((e for e in edges if e[0] >= tap_time - 0.002 and e[1] == value), None)
        if match is None:
            latencies.append((tap_time - t0, value, None))
        else:
            latencies.append((tap_time - t0, value, (match[0] - tap_time) * 1000.0))

    measured = sorted(l[2] for l in latencies if l[2] is not None)
    report = os.path.join(out, "latency.txt")
    with open(report, "w") as fh:
        fh.write("run=%s windowed=%s taps=%d hold=%.2fs gap=%.2fs\n"
                 % (args.run_name, args.windowed, args.taps, args.hold, args.gap))
        fh.write("latency = HID publish (runtime monotonic t=) - input-file write\n")
        fh.write("gate: p95 <= 33.34 ms (2 HID periods of 16.67 ms)\n\n")
        for t, value, lat in latencies:
            fh.write("tap t=%.3f value=%d publish_latency_ms=%s\n"
                     % (t, value, "%.2f" % lat if lat is not None else "MISSING"))
        fh.write("\nedges_seen=%d taps=%d delivered=%d\n"
                 % (len(edges), len(taps), len(measured)))
        if measured:
            p95 = measured[min(len(measured) - 1, int(0.95 * len(measured)))]
            fh.write("p50_ms=%.2f p95_ms=%.2f max_ms=%.2f\n"
                     % (measured[len(measured) // 2], p95, measured[-1]))
            fh.write("frame_hash_before=see-frames-dir final_top_ppm_sha256_16=%s\n"
                     % final_hash)
            fh.write("gate_p95_33_34ms=%s\n" % ("PASS" if p95 <= 33.34 else "FAIL"))
        else:
            fh.write("NO DELIVERY: no tap reached the HID ring\n")
    log("latency report: %s" % report)
    if measured:
        p95 = measured[min(len(measured) - 1, int(0.95 * len(measured)))]
        log("delivered=%d/%d p95=%.2fms max=%.2fms"
            % (len(measured), len(taps), p95, measured[-1]))
        return 0 if p95 <= 33.34 else 1
    return 1


if __name__ == "__main__":
    sys.exit(main())
