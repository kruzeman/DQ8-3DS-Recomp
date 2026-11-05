#!/usr/bin/env python3
"""Summarise DQ8_TRACE_TEXDRAW lines: per texture unit, the filter bits the
guest requests, and the draw/triangle totals.  Usage: texfilter_stats.py LOG"""
import sys
from collections import Counter, defaultdict


def parse(line):
    d = {}
    for tok in line.split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            d[k] = v
    return d


def fields(param):
    return dict(
        mag=(param >> 1) & 1,
        min_=(param >> 2) & 1,
        wrap_t=(param >> 8) & 7,
        wrap_s=(param >> 12) & 7,
        mip=(param >> 24) & 1,
        type_=(param >> 28) & 7,
    )


def main(path):
    n = 0
    tris = 0
    units = [Counter() for _ in range(3)]
    enabled = [0, 0, 0]
    combos = [Counter() for _ in range(3)]
    param_words = [Counter() for _ in range(3)]
    wraps = [Counter() for _ in range(3)]
    for line in open(path, errors="replace"):
        if "TEXDRAW" not in line:
            continue
        d = parse(line)
        if "p0" not in d:
            continue
        n += 1
        tris += int(d.get("tris", "0"))
        for u in range(3):
            if d.get("t%d" % u) != "1":
                continue
            enabled[u] += 1
            param = int(d["p%d" % u], 16)
            f = fields(param)
            units[u]["mag=%d" % f["mag"]] += 1
            units[u]["min=%d" % f["min_"]] += 1
            units[u]["mip=%d" % f["mip"]] += 1
            units[u]["type=%d" % f["type_"]] += 1
            units[u]["mag/min=%d/%d" % (f["mag"], f["min_"])] += 1
            combos[u]["%d%d%d" % (f["mag"], f["min_"], f["mip"])] += 1
            param_words[u]["%08X" % param] += 1
            wraps[u]["s=%d t=%d" % (f["wrap_s"], f["wrap_t"])] += 1
    print("draws=%d tris=%d" % (n, tris))
    for u in range(3):
        print("\nunit %d enabled in %d draws" % (u, enabled[u]))
        for k in sorted(units[u]):
            print("  %-16s %d" % (k, units[u][k]))
        print("  mag/min/mip combos: %s" % dict(combos[u]))
        print("  wrap s/t: %s" % dict(wraps[u]))
        print("  top param words: %s" % param_words[u].most_common(8))


if __name__ == "__main__":
    main(sys.argv[1])
