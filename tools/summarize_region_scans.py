#!/usr/bin/env python3
"""Summarise scan_region_exits.py JSON files into one table.

usage: summarize_region_scans.py <dir-or-json> [...]
"""
import glob
import json
import os
import sys


def main():
    files = []
    for arg in sys.argv[1:]:
        if os.path.isdir(arg):
            files += sorted(glob.glob(os.path.join(arg, "scan-*.json")))
        else:
            files.append(arg)
    print("| guest range | insns | mode | svc | indirect | ret | calls | branch | seq | exit targets |")
    print("|---|---:|---|---:|---:|---:|---:|---:|---:|---:|")
    for f in files:
        d = json.load(open(f))
        print("| `%s-%s` | %d | %s | %d | %d | %d | %d | %d | %d | %d |" % (
            d["start"], d["end"], d["instructions"], d["mode"], d["svc"],
            d["indirect"], d.get("ret", 0), d["calls"], d["branches"],
            d["sequential"], len(d["external_exit_targets"])))


if __name__ == "__main__":
    main()
