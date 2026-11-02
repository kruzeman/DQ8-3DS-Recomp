#!/usr/bin/env python3
"""Classify the emitted guest instructions of one address range (t_f79c2913).

Reads the split part file(s) that hold the range (band = page >> 5) and, for
every translated case in [start, end), reports what its emitted body does:

  svc        a 3DS supervisor call (svc_* helper)              <- blocks S1b
  indirect   branch through a register / load-to-PC            <- blocks S1b
  call       direct BL (r[14] = return; pc = target)
  branch     direct B (pc = target, conditional or not)
  seq        plain dataflow / memory access

It also reports the instruction width (ARM = 4-byte stride, Thumb = 2) so
"Thumb interworking inside the region" is visible, and lists the exit targets
that leave the range.

usage: scan_region_exits.py --start 0x00160000 --end 0x00161000
                            [--split target/dq8-split] [--json out.json]
"""
import argparse
import json
import os
import re

CASE_RE = re.compile(r"^case 0x([0-9A-Fa-f]+): \{ (.*) break; \}\s*$")
SVC_RE = re.compile(r"\bsvc_[a-z_]+\(")
RET_RE = re.compile(r"(?<![A-Za-z0-9_])pc\s*=\s*r\[14\]")
IND_RE = re.compile(r"branch_indirect\(|pc = dq8_mem(8|16|32)_fast\(|"
                    r"(?<![A-Za-z0-9_])pc = r\[(?!14\])|(?<![A-Za-z0-9_])pc = \(")
CALL_RE = re.compile(r"r\[14\] = 0x([0-9A-Fa-f]+)u; pc = 0x([0-9A-Fa-f]+)u;")
BR_RE = re.compile(r"(?<!r\[14\] = )pc = 0x([0-9A-Fa-f]+)u;")
PCSET_RE = re.compile(r"(?<![A-Za-z0-9_])pc\s*=\s*0x([0-9A-Fa-f]+)u?;")


def classify(body):
    if SVC_RE.search(body):
        return "svc"
    if IND_RE.search(body):
        return "indirect"
    if RET_RE.search(body):
        return "ret"
    if CALL_RE.search(body):
        return "call"
    if PCSET_RE.search(body):
        return "branch"
    return "seq"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--start", required=True)
    ap.add_argument("--end", required=True)
    ap.add_argument("--split", default="target/dq8-split")
    ap.add_argument("--json")
    args = ap.parse_args()
    start, end = int(args.start, 16), int(args.end, 16)

    pages = sorted({p for p in range(start >> 12, (end >> 12) + 1)})
    files = []
    for page in pages:
        f = os.path.join(args.split, "dq8_part_%03d.c" % (page >> 5))
        if f not in files:
            files.append(f)

    cases = {}
    for path in files:
        if not os.path.exists(path):
            continue
        with open(path, errors="replace") as fh:
            for line in fh:
                if not line.startswith("case "):
                    continue
                m = CASE_RE.match(line.rstrip("\n"))
                if not m:
                    continue
                pc = int(m.group(1), 16)
                if start <= pc < end:
                    cases[pc] = m.group(2)

    order = sorted(cases)
    kinds = {"svc": [], "indirect": [], "ret": [], "call": [], "branch": [], "seq": []}
    exits = {}
    for pc in order:
        body = cases[pc]
        k = classify(body)
        kinds[k].append(pc)
        for m in PCSET_RE.finditer(body):
            tgt = int(m.group(1), 16)
            if not (start <= tgt < end):
                exits.setdefault(tgt, 0)
                exits[tgt] += 1

    deltas = sorted({order[i + 1] - order[i] for i in range(len(order) - 1)}) if len(order) > 1 else []
    mode = "ARM (4-byte)" if deltas and deltas[0] == 4 else (
        "Thumb (2-byte)" if deltas and deltas[0] == 2 else "mixed/unknown")

    out = {
        "start": "0x%08X" % start, "end": "0x%08X" % end,
        "instructions": len(order), "stride_deltas": deltas[:6], "mode": mode,
        "parts": [os.path.basename(f) for f in files if os.path.exists(f)],
        "svc": len(kinds["svc"]), "indirect": len(kinds["indirect"]),
        "ret": len(kinds["ret"]),
        "calls": len(kinds["call"]), "branches": len(kinds["branch"]),
        "sequential": len(kinds["seq"]),
        "external_exit_targets": {"0x%08X" % t: n for t, n in sorted(exits.items())},
        "svc_pcs": ["0x%08X" % p for p in kinds["svc"]],
        "indirect_pcs": ["0x%08X" % p for p in kinds["indirect"]],
    }
    print(json.dumps(out, indent=1))
    if args.json:
        with open(args.json, "w") as fh:
            json.dump(out, fh, indent=1)


if __name__ == "__main__":
    main()
