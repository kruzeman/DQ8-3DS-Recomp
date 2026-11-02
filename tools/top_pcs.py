#!/usr/bin/env python3
"""Top guest PCs from a guest-PC profile log (t_f79c2913).

usage: top_pcs.py <profile.log> [n]

Prints two lists:
  * top PCs by executed instructions (heavy instructions / loop bodies)
  * top PCs by non-sequential ARRIVALS (branch/call targets = block entries),
    with the average instructions executed per arrival, which separates a hot
    caller (many arrivals, few instructions) from a heavy callee body.
"""
import re
import sys
from collections import defaultdict

PC_RE = re.compile(r"^GUEST_PC_PROFILE_PC pc=([0-9A-F]+) steps=(\d+) arrivals=(\d+)")


def main():
    path = sys.argv[1]
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 20
    pcs = {}
    for line in open(path, errors="replace"):
        m = PC_RE.match(line)
        if m:
            pcs[int(m.group(1), 16)] = (int(m.group(2)), int(m.group(3)))
    total = sum(s for s, _ in pcs.values())
    print("scenario %s: %d distinct PCs, %d executed instructions" % (path, len(pcs), total))
    print("\ntop %d by executed instructions" % n)
    print("| pc | page | steps | share | arrivals | steps/arrival |")
    print("|---|---|---:|---:|---:|---:|")
    for pc, (steps, arr) in sorted(pcs.items(), key=lambda kv: -kv[1][0])[:n]:
        print("| `0x%08X` | `0x%08X` | %d | %.2f%% | %d | %s |" % (
            pc, pc & ~0xFFF, steps, 100.0 * steps / total, arr,
            ("%.1f" % (steps / arr)) if arr else "-"))
    print("\ntop %d by non-sequential arrivals (block entries)" % n)
    print("| pc | page | arrivals | steps | steps/arrival |")
    print("|---|---|---:|---:|---:|")
    for pc, (steps, arr) in sorted(pcs.items(), key=lambda kv: -kv[1][1])[:n]:
        print("| `0x%08X` | `0x%08X` | %d | %d | %s |" % (
            pc, pc & ~0xFFF, arr, steps,
            ("%.1f" % (steps / arr)) if arr else "-"))


if __name__ == "__main__":
    main()
