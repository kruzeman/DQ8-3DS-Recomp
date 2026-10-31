#!/usr/bin/env python3
"""Aggregate tools/s1d_ab.py JSON records into the S1d movie-path A/B table.

Runs that never reached the movie (scene_movie_entries == 0 / no Y2R batch)
are the documented flaky main-spin mode: they are listed separately and
excluded from the movie-path statistics instead of being averaged in.
"""
import argparse
import json
import statistics
from pathlib import Path

KEYS = [
    ("wall_s", "wall s"),
    ("agg_rate", "aggregate guest M/s"),
    ("main_rate", "main guest M/s"),
    ("user_s", "user s"),
    ("sys_s", "system s"),
    ("cpu_main", "CPU main-thread s"),
    ("cpu_native", "CPU native-pinned s"),
    ("agg_steps", "aggregate retired"),
    ("worker_steps", "worker retired"),
    ("y2r", "Y2R batches"),
    ("movie", "movie entries"),
    ("dispatch", "dispatch calls"),
    ("s1_share", "region insn share of aggregate"),
]


def num(s):
    """BENCH rates carry an M suffix ("11.440M"); strip it."""
    if s is None:
        return float("nan")
    return float(str(s).rstrip("M"))


def metrics(rec):
    b = rec["bench"]
    return {
        "wall_s": rec["wall_s"],
        "agg_rate": num(b.get("aggregate_instr_per_sec")),
        "main_rate": num(b.get("instr_per_sec")),
        "user_s": rec["time_user_s"],
        "sys_s": rec["time_sys_s"],
        "cpu_main": rec["host_cpu_s"].get("main", float("nan")),
        "cpu_native": rec["host_cpu_s"].get("native_pinned", float("nan")),
        "agg_steps": int(b.get("aggregate_steps", 0)),
        "worker_steps": int(b.get("worker_steps", 0)),
        "y2r": int(b.get("scene_y2r_conversions", 0)),
        "movie": int(b.get("scene_movie_entries", 0)),
        "dispatch": int(b.get("dispatch_calls", 0)),
        "s1_runs": rec["s1_cfg"][0]["runs"] if rec["s1_cfg"] else 0,
        "s1_insns": rec["s1_cfg"][0]["insns"] if rec["s1_cfg"] else 0,
        "s1_share": (rec["s1_cfg"][0]["insns"] / int(
            b.get("aggregate_steps", 1)) if rec["s1_cfg"] else 0.0),
        "rc": rec["rc"],
        "escaped": rec["escaped"],
    }


def main():
    p = argparse.ArgumentParser()
    p.add_argument("outdir", type=Path)
    p.add_argument("--markdown", action="store_true")
    p.add_argument("--paired", action="store_true",
                   help="per-tag off/on deltas (interleaved same-machine pairs)")
    args = p.parse_args()
    arms = {}
    excluded = []
    by_tag = {}
    for f in sorted(args.outdir.glob("*.json")):
        rec = json.loads(f.read_text())
        if rec["arm"].startswith("smoke"):
            continue
        m = metrics(rec)
        if m["movie"] == 0 or m["y2r"] == 0:
            excluded.append((rec["arm"], rec["tag"], m))
            continue
        arms.setdefault(rec["arm"], []).append(m)
        by_tag.setdefault(rec["tag"], {})[rec["arm"]] = m

    if args.paired:
        print("pair  off_wall   on_wall    d_wall   off_agg   on_agg    "
              "d_agg%   off_user  on_user   d_user")
        dw, da, du = [], [], []
        for tag in sorted(by_tag):
            pair = by_tag[tag]
            if "off" not in pair or "on" not in pair:
                continue
            o, n = pair["off"], pair["on"]
            dwall = n["wall_s"] - o["wall_s"]
            dagg = (n["agg_rate"] - o["agg_rate"]) / o["agg_rate"] * 100
            duser = n["user_s"] - o["user_s"]
            dw.append(dwall)
            da.append(dagg)
            du.append(duser)
            print(f"{tag}   {o['wall_s']:8.3f} {n['wall_s']:9.3f} "
                  f"{dwall:+8.3f}  {o['agg_rate']:8.3f} {n['agg_rate']:8.3f} "
                  f"{dagg:+7.2f}%  {o['user_s']:8.2f} {n['user_s']:8.2f} "
                  f"{duser:+8.2f}")
        if dw:
            print(f"median delta over {len(dw)} pairs: "
                  f"wall {statistics.median(dw):+.3f}s, "
                  f"aggregate throughput {statistics.median(da):+.2f}%, "
                  f"user {statistics.median(du):+.2f}s")
            print(f"mean   delta: wall {statistics.fmean(dw):+.3f}s, "
                  f"aggregate {statistics.fmean(da):+.2f}%, "
                  f"user {statistics.fmean(du):+.2f}s")
        return

    if args.markdown:
        print("| metric | " + " | ".join(
            f"{a} (n={len(v)})" for a, v in sorted(arms.items())) + " |")
        print("|---|" + "---|" * len(arms))
        for key, label in KEYS:
            row = [label]
            for a in sorted(arms):
                vals = [m[key] for m in arms[a]]
                med = statistics.median(vals)
                lo, hi = min(vals), max(vals)
                if key in ("agg_steps", "worker_steps", "y2r", "movie",
                           "dispatch"):
                    row.append(f"{med:,.0f} [{lo:,.0f}–{hi:,.0f}]")
                elif key == "s1_share":
                    row.append(f"{med * 100:.2f}% "
                               f"[{lo * 100:.2f}–{hi * 100:.2f}]"
                               if med else "n/a (gate off)")
                else:
                    row.append(f"{med:.3f} [{lo:.3f}–{hi:.3f}]")
            print("| " + " | ".join(row) + " |")
    else:
        for a in sorted(arms):
            print(f"== {a} n={len(arms[a])}")
            for m in arms[a]:
                print("   ", m)
    if excluded:
        print(f"\nEXCLUDED (movie path not reached, flaky main-spin mode): "
              f"{len(excluded)}")
        for a, t, m in excluded:
            print(f"   {a}-{t} wall={m['wall_s']}s worker={m['worker_steps']} "
                  f"y2r={m['y2r']} movie={m['movie']} rc={m['rc']}")


if __name__ == "__main__":
    main()
