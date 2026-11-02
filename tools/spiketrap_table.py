#!/usr/bin/env python3
"""Aggregate SPIKETRAP lines from a DQ8_SPIKETRAP=<ms> run into a cause table.

usage: tools/spiketrap_table.py LOG [LOG...]
"""
import re
import sys

KEYS = ["total_ms", "guest", "gl", "present", "vert", "attr", "vsexec",
        "refresh", "wb", "xfer_flush", "triflush", "stage", "gldraw"]


def main(paths):
    for path in paths:
        rows = []
        vs = []
        for line in open(path, errors="replace"):
            if not line.startswith("SPIKETRAP f="):
                continue
            d = dict(re.findall(r"(\w+)=([0-9.]+)", line))
            rows.append(d)
            m = re.search(r"vs_steps=(\d+)", line)
            if m:
                vs.append(int(m.group(1)))
        print(f"== {path}: {len(rows)} spike frames")
        if not rows:
            continue
        print(f"   {'bucket':12s} {'n':>5s} {'mean_ms':>8s} {'med_ms':>7s} {'max_ms':>7s}")
        for k in KEYS:
            v = [float(r[k]) for r in rows if k in r]
            if not v:
                continue
            v.sort()
            print(f"   {k:12s} {len(v):5d} {sum(v)/len(v):8.1f} "
                  f"{v[len(v)//2]:7.1f} {v[-1]:7.1f}")
        if vs:
            verts = [float(r["vsexec"].split("/")[1]) if "/" in r.get("vsexec", "")
                     else 0.0 for r in rows]
            print(f"   VS microcode steps per spike frame: "
                  f"mean={sum(vs)/len(vs):.0f}")
            if sum(verts):
                print(f"   vertices per spike frame (vsexec calls): "
                      f"mean={sum(verts)/len(verts):.0f}")
                print(f"   steps per vertex: {(sum(vs)/len(vs))/(sum(verts)/len(verts)):.0f}")


if __name__ == "__main__":
    main(sys.argv[1:])
