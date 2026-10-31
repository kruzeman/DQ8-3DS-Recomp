#!/usr/bin/env python3
"""Aggregate joint_measure runs into the F4/F5 before/after acceptance table.

usage: tools/analyze_joint.py <window_start> <window_end> run1 run2 ...
(run names are directories under tools/joint/)
"""
import csv
import glob
import os
import re
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
HZ = 100.0  # CLK_TCK on this host; cpu_total column cross-checks it


def load(name):
    d = os.path.join(ROOT, "tools", "joint", name)
    rows = []
    with open(os.path.join(d, "metrics.csv")) as fh:
        for r in csv.DictReader(fh):
            try:
                rows.append({k: int(v) for k, v in r.items()
                             if k not in ("top_hash", "bot_hash")} |
                            {"top_hash": r["top_hash"], "bot_hash": r["bot_hash"]})
            except (ValueError, TypeError):
                continue
    return d, rows


def analyze(name, w0, w1):
    d, rows = load(name)
    win = [r for r in rows if w0 <= r["t"] < w1 and r.get("counters_ok")]
    if not win:
        return {"name": name, "err": "no rows in window"}
    n = len(win)
    main_pct = statistics.mean(r["cpu_main"] for r in win) * 100.0 / HZ
    total_pct = statistics.mean(r["cpu_total"] for r in win)
    minstr = statistics.mean(r["main_instr_d"] + r["worker_instr_d"]
                             for r in win) / 1e6
    vb = statistics.mean(r["vblanks_d"] for r in win)
    draws = statistics.mean(r["draws_d"] for r in win)
    ticks = statistics.mean(r["dsp_ticks_d"] for r in win)
    resync = statistics.mean(r["dsp_resyncs_d"] for r in win)
    top_hashes = {r["top_hash"] for r in win}
    uniq = len(top_hashes) / float(n)
    # windowed vs headless host sleeps / parks
    host_sl = statistics.mean(r["host_sleeps_d"] for r in win)
    out = dict(name=name, rows=n, main_pct=main_pct, total_pct=total_pct,
               minstr=minstr, vblanks=vb, draws=draws, uniq_top=uniq,
               dsp_ticks=ticks, dsp_resyncs=resync, host_sleeps=host_sl)
    # whole-run extras from run.log
    logp = os.path.join(d, "run.log")
    escapes = invalid = 0
    audio = None
    hid_ts = []
    worker = None
    if os.path.exists(logp):
        with open(logp, errors="replace") as fh:
            for line in fh:
                if "translated PC escaped" in line:
                    escapes += 1
                if "SVC ArbitrateAddress: invalid handle" in line:
                    invalid += 1
                if "dsp audio metrics:" in line:
                    audio = dict(re.findall(r"(\w+)=(-?\d+)", line))
                m = re.search(r"HID buttons=\w+.*t=(\d+\.\d+)", line)
                if m:
                    hid_ts.append(float(m.group(1)))
                if line.startswith("split present worker:"):
                    worker = line.strip()
    out["escapes"] = escapes
    out["invalid_handle"] = invalid
    out["audio"] = audio
    out["worker"] = worker
    if len(hid_ts) > 2:
        gaps = [b - a for a, b in zip(hid_ts, hid_ts[1:]) if 0 < b - a < 1.0]
        if gaps:
            gaps.sort()
            out["hid_gap_p50_ms"] = statistics.median(gaps) * 1e3
            out["hid_gap_p95_ms"] = gaps[int(len(gaps) * 0.95) - 1] * 1e3
            out["hid_n"] = len(gaps)
    return out


def main():
    w0, w1 = int(sys.argv[1]), int(sys.argv[2])
    names = sys.argv[3:]
    res = [analyze(n, w0, w1) for n in names]
    cols = ["name", "main_pct", "total_pct", "minstr", "vblanks", "uniq_top",
            "draws", "dsp_ticks", "dsp_resyncs", "escapes", "invalid_handle"]
    print(" | ".join(cols))
    for r in res:
        if r.get("err"):
            print(r["name"], "ERR", r["err"])
            continue
        print(" | ".join(
            str(r[c]) if not isinstance(r[c], float) else f"{r[c]:.2f}"
            for c in cols))
        if r.get("audio"):
            print("   audio:", {k: r["audio"][k] for k in
                                ("deadline_misses", "active_audio_gaps",
                                 "sink_empty") if k in r["audio"]})
        if r.get("hid_n"):
            print("   hid publish gaps: n=%d p50=%.1fms p95=%.1fms"
                  % (r["hid_n"], r["hid_gap_p50_ms"], r["hid_gap_p95_ms"]))
        if r.get("worker"):
            print("  ", r["worker"])


if __name__ == "__main__":
    main()
