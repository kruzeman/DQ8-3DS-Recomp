#!/usr/bin/env python3
"""Compare main host-thread CPU percentages from paired audio capture logs."""
from pathlib import Path
import re
import sys


def samples(path: str):
    root = Path(path)
    timeline = (root / "timeline.log").read_text()
    match = re.search(r"^pid=(\d+)$", timeline, re.MULTILINE)
    if not match:
        raise SystemExit(f"missing PID in {root / 'timeline.log'}")
    pid = match.group(1)
    current_t = None
    sample_index = 0
    explicit_clock = False
    main = {}
    for line in (root / "thread-cpu.log").read_text().splitlines():
        fields = line.split()
        if not fields:
            continue
        if fields[0].startswith("t="):
            current_t = int(fields[0][2:])
            explicit_clock = True
            fields = fields[1:]
        if len(fields) >= 3 and fields[0] == pid:
            if not explicit_clock:
                sample_index += 1
                current_t = sample_index
            if current_t is not None:
                main[current_t] = float(fields[1])
    if not main:
        raise SystemExit(f"no main-thread ps samples for pid {pid} in {root}")
    return main


def main(path: str) -> float:
    rows = samples(path)
    return rows[max(rows)]


def elapsed_seconds(path: str) -> float:
    timeline = (Path(path) / "timeline.log").read_text()
    match = re.search(r"\belapsed_s=([0-9.]+)", timeline)
    if not match:
        raise SystemExit(f"missing elapsed_s in {Path(path) / 'timeline.log'}")
    return float(match.group(1))


def idle_window(path: str, start: int = 155, end: int = 185):
    rows = samples(path)
    before = [t for t in rows if t <= start]
    after = [t for t in rows if t >= end]
    if not before or not after:
        return None
    t0, t1 = max(before), min(after)
    value = (rows[t1] * t1 - rows[t0] * t0) / float(t1 - t0)
    return t0, t1, value


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--idle":
        result = idle_window(sys.argv[2])
        if result is None:
            raise SystemExit("idle CPU window requires samples spanning t=155..185")
        t0, t1, value = result
        label = Path(sys.argv[2]).name
        print(f"{label}_idle_dialog_t{t0}_{t1}_main_thread_cpu_pct_est={value:.2f}")
    elif len(sys.argv) == 5 and sys.argv[1] == "--idle-window":
        start, end = int(sys.argv[2]), int(sys.argv[3])
        if end <= start:
            raise SystemExit("idle CPU window end must be after start")
        result = idle_window(sys.argv[4], start, end)
        if result is None:
            raise SystemExit(f"idle CPU window requires samples spanning t={start}..{end}")
        t0, t1, value = result
        label = Path(sys.argv[4]).name
        print(f"{label}_idle_dialog_t{t0}_{t1}_main_thread_cpu_pct_est={value:.2f}")
    elif len(sys.argv) == 3:
        baseline_rows = samples(sys.argv[1])
        candidate_rows = samples(sys.argv[2])
        baseline = baseline_rows[max(baseline_rows)]
        candidate = candidate_rows[max(candidate_rows)]
        baseline_wall = elapsed_seconds(sys.argv[1])
        candidate_wall = elapsed_seconds(sys.argv[2])
        freed = baseline - candidate
        relative = freed * 100.0 / baseline if baseline else 0.0
        baseline_cpu_s = baseline * baseline_wall / 100.0
        candidate_cpu_s = candidate * candidate_wall / 100.0
        freed_cpu_s = baseline_cpu_s - candidate_cpu_s
        cpu_reduction = freed_cpu_s * 100.0 / baseline_cpu_s if baseline_cpu_s else 0.0
        print(f"baseline_guest_host_thread_cpu_pct={baseline:.2f}")
        print(f"candidate_guest_host_thread_cpu_pct={candidate:.2f}")
        print(f"main_thread_cpu_share_freed_percentage_points={freed:.2f}")
        print(f"main_thread_cpu_reduction_pct={relative:.2f}")
        print(f"baseline_guest_thread_cpu_s_est={baseline_cpu_s:.2f}")
        print(f"candidate_guest_thread_cpu_s_est={candidate_cpu_s:.2f}")
        print(f"guest_thread_cpu_s_freed_est={freed_cpu_s:.2f}")
        print(f"guest_thread_cpu_s_reduction_pct_est={cpu_reduction:.2f}")
        print(f"baseline_capture_wall_s={baseline_wall:.2f}")
        print(f"candidate_capture_wall_s={candidate_wall:.2f}")
    else:
        raise SystemExit("usage: audio_cpu_share.py <baseline-dir> <candidate-dir> | --idle <capture-dir> | --idle-window <start> <end> <capture-dir>")
