#!/usr/bin/env python3
"""Rank guest hot regions from the guest-PC profile counters (t_f79c2913).

The counters are produced by the build switch `DQ8_GUEST_PC_PROFILE_BUILD=1`
(tools/split_dispatch.py) and printed to stderr by the runtime when
`DQ8_GUEST_PC_PROFILE=1` is set -- at a clean bench_report() exit or, more
usefully, after `DQ8_GUEST_PC_PROFILE_SECS=<s>` by the report timer.

usage:
  analyze_guest_pc_profile.py --startup target/dq8-startup.c \
      --out DIR [--json OUT.json] \
      name=path/to/profile.log [name=path/to/profile.log ...]

For every scenario it prints:
  * total retired instructions and wall time,
  * the 4 KB page (address range) share of executed instructions,
  * top ranges with their static translated instruction count and the split
    part file (dq8_part_NNN.c, band = page >> 5) the work would land in,
  * a call profile: pages/ranges entered most often (arrivals) versus the
    instructions executed inside them,
and one cross-scenario table ranking the ranges by aggregate share.
"""
import argparse
import json
import os
import re
from collections import defaultdict

BEGIN_RE = re.compile(
    r"^GUEST_PC_PROFILE_BEGIN after_movie_entry=(\d+) elapsed=([\d.]+)"
    r" total_steps=(\d+) page_entries=(\d+) pc_arrivals=(\d+)"
    r" base=([0-9A-F]+) slots=(\d+)")
PAGE_RE = re.compile(r"^GUEST_PC_PROFILE_PAGE page=([0-9A-F]+) steps=(\d+) entries=(\d+)")
PC_RE = re.compile(r"^GUEST_PC_PROFILE_PC pc=([0-9A-F]+) steps=(\d+) arrivals=(\d+)")
CASE_RE = re.compile(r"^case 0x([0-9A-Fa-f]+):")


def parse_log(path):
    out = {"begin": None, "pages": {}, "pcs": {}}
    with open(path, errors="replace") as fh:
        for line in fh:
            m = BEGIN_RE.match(line)
            if m:
                out["begin"] = {
                    "after_movie_entry": int(m.group(1)),
                    "elapsed": float(m.group(2)),
                    "total_steps": int(m.group(3)),
                    "page_entries": int(m.group(4)),
                    "pc_arrivals": int(m.group(5)),
                    "base": int(m.group(6), 16),
                    "slots": int(m.group(7)),
                }
                continue
            m = PAGE_RE.match(line)
            if m:
                out["pages"][int(m.group(1), 16)] = (int(m.group(2)), int(m.group(3)))
                continue
            m = PC_RE.match(line)
            if m:
                out["pcs"][int(m.group(1), 16)] = (int(m.group(2)), int(m.group(3)))
    return out


def static_instruction_counts(path):
    """Translated instruction count per 4 KB page, from the emitted C."""
    per_page = defaultdict(int)
    with open(path, errors="replace") as fh:
        for line in fh:
            if not line.startswith("case "):
                continue
            m = CASE_RE.match(line)
            if m:
                per_page[int(m.group(1), 16) >> 12] += 1
    return per_page


def part_file(page):
    return "dq8_part_%03d.c" % (page >> 5)


def q(x, n=2):
    return ("%%.%df" % n) % x


def analyze(name, log, static):
    total = sum(s for s, _ in log["pages"].values())
    rows = []
    for page, (steps, entries) in log["pages"].items():
        lo, hi = page << 12, (page << 12) + 0xFFF
        rows.append({
            "page": page,
            "range": "0x%08X-0x%08X" % (lo, hi),
            "steps": steps,
            "share": 100.0 * steps / total if total else 0.0,
            "entries": entries,
            "pinsns": static.get(page, 0),
            "part": part_file(page),
            "steps_per_entry": (steps / entries) if entries else 0.0,
        })
    rows.sort(key=lambda r: -r["steps"])
    return {"name": name, "total_steps": total, "begin": log["begin"], "pages": rows,
            "pcs": log["pcs"]}


def md_table(rows, limit=20):
    head = ("| # | guest range (4 KB) | insns in range | executed | share | "
            "steps/entry | part file |\n|---:|---|---:|---:|---:|---:|---|\n")
    body = []
    for i, r in enumerate(rows[:limit], 1):
        body.append("| %d | `%s` | %d | %d | %.2f%% | %s | `%s` |" % (
            i, r["range"], r["pinsns"], r["steps"], r["share"],
            q(r["steps_per_entry"], 1), r["part"]))
    return head + "\n".join(body) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--startup", default="target/dq8-startup.c")
    ap.add_argument("--out", required=True)
    ap.add_argument("--json")
    ap.add_argument("scenarios", nargs="+", help="name=path/to/profile.log")
    args = ap.parse_args()

    static = static_instruction_counts(args.startup)
    results = []
    for spec in args.scenarios:
        name, path = spec.split("=", 1)
        log = parse_log(path)
        if not log["begin"]:
            raise SystemExit("%s: no GUEST_PC_PROFILE_BEGIN in %s" % (name, path))
        results.append(analyze(name, log, static))

    os.makedirs(args.out, exist_ok=True)
    os.makedirs(args.out, exist_ok=True)
    lines = ["# Hot-region ranking (raw counters)\n"]
    for res in results:
        b = res["begin"]
        lines.append("## scenario %s\n" % res["name"])
        lines.append("total_steps=%d elapsed=%s s  page_entries=%d  pc_arrivals=%d  "
                     "after_movie_entry=%d\n" %
                     (b["total_steps"], b["elapsed"], b["page_entries"],
                      b["pc_arrivals"], b["after_movie_entry"]))
        lines.append("instr/s = %.2fM\n" % (b["total_steps"] / b["elapsed"] / 1e6))
        lines.append("\n### top 20 ranges by executed instructions\n")
        lines.append(md_table(res["pages"], 20))
        by_entry = sorted(res["pages"], key=lambda r: -r["entries"])
        lines.append("\n### top 10 ranges by ARRIVALS (entries into the range)\n")
        lines.append(md_table(by_entry, 10))
        lines.append("\n### all ranges (share >= 0.10%%)\n")
        lines.append(md_table([r for r in res["pages"] if r["share"] >= 0.10], 200))

    # cross-scenario aggregate
    agg = defaultdict(lambda: {"steps": 0, "entries": 0, "perscen": {}})
    per_tot = {r["name"]: r["total_steps"] for r in results}
    for res in results:
        for r in res["pages"]:
            a = agg[r["page"]]
            a["steps"] += r["steps"]
            a["entries"] += r["entries"]
            a["pinsns"] = r["pinsns"]
            a["part"] = r["part"]
            a["range"] = r["range"]
            a["perscen"][res["name"]] = r["share"]
    grand = sum(v["steps"] for v in agg.values())
    names = [r["name"] for r in results]
    rows = []
    for page, a in agg.items():
        rows.append({
            "page": page, "range": a["range"], "pinsns": a["pinsns"],
            "steps": a["steps"], "share": 100.0 * a["steps"] / grand if grand else 0.0,
            "entries": a["entries"],
            "steps_per_entry": (a["steps"] / a["entries"]) if a["entries"] else 0.0,
            "part": a["part"], "perscen": a["perscen"],
        })
    rows.sort(key=lambda r: -r["steps"])
    lines.append("\n## aggregate over all scenarios (total %d executed instructions)\n" % grand)
    head = ("| # | guest range (4 KB) | insns in range | executed | share | "
            + " | ".join(names) + " | steps/entry | part file |\n")
    head += "|---:|---|---:|---:|---:|" + "---:|" * len(names) + "---:|---|\n"
    body = []
    for i, r in enumerate(rows[:20], 1):
        body.append("| %d | `%s` | %d | %d | %.2f%% | %s | %s | `%s` |" % (
            i, r["range"], r["pinsns"], r["steps"], r["share"],
            " | ".join("%.2f%%" % r["perscen"].get(n, 0.0) for n in names),
            q(r["steps_per_entry"], 1), r["part"]))
    lines.append(head + "\n".join(body) + "\n")

    text = "\n".join(lines)
    with open(os.path.join(args.out, "RANKING.md"), "w") as fh:
        fh.write(text)
    if args.json:
        with open(args.json, "w") as fh:
            json.dump({"scenarios": [{k: v for k, v in r.items() if k != "pcs"}
                                     for r in results],
                       "aggregate": rows}, fh, indent=1)
    print(text)


if __name__ == "__main__":
    main()
