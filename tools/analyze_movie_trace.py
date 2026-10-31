#!/usr/bin/env python3
"""Summarize a DQ8_TRACE_MOVIE run log (task t_c6ea81c4, F3 pacing trace).

Input: run.log captured from a game run with DQ8_TRACE_MOVIE=1.
Output: per-5s MOVIE_REPORT table + inter-frame gap decomposition:
  * guest retired instructions in the gap (worker vs main)
  * parked wall time per guest thread in the gap
  * wake reasons (signal/timeout/ready) in the gap
  * fs reads + y2r conversion durations

Usage: python3 tools/analyze_movie_trace.py <run.log> [window_start_s window_end_s]
"""
import re
import sys
from collections import defaultdict


def main():
    path = sys.argv[1]
    w0 = float(sys.argv[2]) if len(sys.argv) > 2 else None
    w1 = float(sys.argv[3]) if len(sys.argv) > 3 else None
    reports, threads, events = [], [], []
    with open(path, "r", errors="replace") as fh:
        for line in fh:
            if "MOVIE_REPORT " in line:
                d = dict(re.findall(r"(\w+)=([^\s]+)", line))
                reports.append(d)
            elif "MOVIE_THREAD " in line:
                d = dict(re.findall(r"(\w+)=([^\s]+)", line))
                threads.append(d)
            elif "MOVIE_TRACE " in line:
                m = re.search(r"t_ms=(\d+)\s+(\w+)(.*)", line)
                if m:
                    events.append((int(m.group(1)), m.group(2), m.group(3).strip()))

    def in_win(t_ms):
        if w0 is None:
            return True
        t = t_ms / 1000.0
        return w0 <= t < w1

    print("== MOVIE_REPORT (5s buckets) ==")
    hdr = ("t_ms", "y2r", "entries", "fs_reads", "sleeps", "parks",
           "wake_sig", "wake_to", "wake_rdy", "main_ret", "worker_ret", "dsp")
    print("%8s %4s %4s %6s %6s %6s %7s %7s %7s %9s %9s %5s" % hdr)
    for r in reports:
        t_ms = int(r.get("t_ms", 0))
        if not in_win(t_ms):
            continue
        print("%8s %4s %4s %6s %6s %6s %7s %7s %7s %9s %9s %5s" % (
            t_ms, r.get("y2r"), r.get("entries"), r.get("fs_reads"),
            r.get("sleeps"), r.get("parks"), r.get("wake_sig"),
            r.get("wake_to"), r.get("wake_rdy"), r.get("main_retired"),
            r.get("worker_retired"), r.get("dsp_ticks")))

    print()
    print("== MOVIE_THREAD (5s buckets, retired vs parked) ==")
    print("%8s %3s %3s %10s %9s %6s %10s %6s %9s" % (
        "t_ms", "idx", "act", "retired", "blocked_ms", "parks", "wait_h", "type", "wake_in"))
    for t in threads:
        t_ms = int(t.get("t_ms", 0))
        if not in_win(t_ms):
            continue
        print("%8s %3s %3s %10s %9s %6s %10s %6s %9s" % (
            t_ms, t.get("idx"), t.get("active"), t.get("retired"),
            t.get("blocked_ms"), t.get("parks"), t.get("wait"),
            t.get("type"), t.get("wake_in_ms")))

    print()
    print("== gap decomposition between y2r_done events ==")
    y2r_ts = [e[0] for e in events if e[1] == "y2r_done"]
    if len(y2r_ts) >= 2:
        gaps = [b - a for a, b in zip(y2r_ts, y2r_ts[1:])]
        gaps_sorted = sorted(gaps)
        print("n=%d gaps_ms: p50=%d p90=%d max=%d min=%d mean=%.0f" % (
            len(gaps), gaps_sorted[len(gaps) // 2],
            gaps_sorted[int(len(gaps) * 0.9)], gaps_sorted[-1], gaps_sorted[0],
            sum(gaps) / len(gaps)))

    kinds = defaultdict(int)
    for _t, k, _rest in events:
        kinds[k] += 1
    print()
    print("== event kinds ==", dict(kinds))

    print()
    print("== first 40 events of the movie window ==")
    shown = 0
    for t, k, rest in events:
        if not in_win(t):
            continue
        print("t_ms=%-8d %-12s %s" % (t, k, rest))
        shown += 1
        if shown >= 40:
            break


if __name__ == "__main__":
    main()
