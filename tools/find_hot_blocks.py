#!/usr/bin/env python3
"""Find the hot basic blocks inside one guest range (t_f79c2913).

The emitted C gives the static structure: a `case 0xADDR:` whose body assigns
`pc` is a block terminator; a body without a `pc` assignment falls through to
`ADDR + 4` (ARM) / `ADDR + 2` (Thumb).  Chaining fall-through cases therefore
reconstructs exactly the basic blocks the translator emitted, without needing
the guest binary.

The profile log (GUEST_PC_PROFILE_PC lines, DQ8_GUEST_PC_PROFILE=1) supplies
the dynamic weight, so a block is ranked by the instructions it actually
retires, not by its static length.

usage:
  find_hot_blocks.py --start 0x00499000 --end 0x00499FFF \
      --profile evidence/.../prof-field.stderr \
      [--split target/dq8-split-prof] [--top 10]
"""
import argparse
import os
import re

CASE_RE = re.compile(r"^case 0x([0-9A-Fa-f]+): \{ (.*) break; \}\s*$")
PC_RE = re.compile(r"^GUEST_PC_PROFILE_PC pc=([0-9A-F]+) steps=(\d+)")
PCSET_RE = re.compile(r"(?<![A-Za-z0-9_])pc\s*=\s*0x([0-9A-Fa-f]+)u?;")
SVC_RE = re.compile(r"\bsvc_[a-z_]+\(")
RET_RE = re.compile(r"(?<![A-Za-z0-9_])pc\s*=\s*r\[14\]")
IND_RE = re.compile(r"branch_indirect\(|pc = dq8_mem(8|16|32)_fast\(|"
                    r"(?<![A-Za-z0-9_])pc = r\[(?!14\])|(?<![A-Za-z0-9_])pc = \(")


def load_cases(split, start, end):
    pages = sorted({p for p in range(start >> 12, (end >> 12) + 1)})
    files = []
    for page in pages:
        f = os.path.join(split, "dq8_part_%03d.c" % (page >> 5))
        if f not in files and os.path.exists(f):
            files.append(f)
    cases = {}
    for path in files:
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
    return cases


def load_steps(paths):
    steps = {}
    for path in paths:
        for line in open(path, errors="replace"):
            m = PC_RE.match(line)
            if m:
                steps[int(m.group(1), 16)] = steps.get(int(m.group(1), 16), 0) + int(m.group(2))
    return steps


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--start", required=True)
    ap.add_argument("--end", required=True)
    ap.add_argument("--profile", required=True,
                    help="one or more profile logs, comma separated (summed)")
    ap.add_argument("--split", default="target/dq8-split-prof")
    ap.add_argument("--top", type=int, default=10)
    args = ap.parse_args()
    start, end = int(args.start, 16), int(args.end, 16)

    cases = load_cases(args.split, start, end)
    steps = load_steps(args.profile.split(","))
    total = sum(steps.get(pc, 0) for pc in cases)
    grand = sum(steps.values())
    order = sorted(cases)

    stride = 4
    if len(order) > 1 and order[1] - order[0] == 2:
        stride = 2

    blocks = []
    i = 0
    while i < len(order):
        first = order[i]
        j = i
        exits = set()
        while j < len(order):
            pc = order[j]
            body = cases[pc]
            targets = {int(t, 16) for t in PCSET_RE.findall(body)}
            nxt = pc + stride
            nonconst = bool(RET_RE.search(body) or re.search(
                r"branch_indirect\(|pc = dq8_mem(8|16|32)_fast\(|"
                r"(?<![A-Za-z0-9_])pc = r\[|(?<![A-Za-z0-9_])pc = \(", body))
            if nonconst:
                break               # return / indirect transfer ends the block
            if targets and targets != {nxt}:
                # conditional/unconditional transfer away from the fall-through
                exits |= targets - {nxt}
                break
            if j + 1 >= len(order) or order[j + 1] != nxt:
                break               # falls off the translated set
            j += 1
        members = order[i:j + 1]
        body_all = " ".join(cases[p] for p in members)
        blocks.append({
            "start": first,
            "end": members[-1] + stride,
            "insns": len(members),
            "steps": sum(steps.get(p, 0) for p in members),
            "svc": bool(SVC_RE.search(body_all)),
            "indirect": bool(IND_RE.search(body_all)),
            "ret": bool(RET_RE.search(body_all)),
            "exits": sorted(exits),
        })
        i = j + 1

    blocks.sort(key=lambda b: -b["steps"])
    print("range 0x%08X-0x%08X  mode=%s  blocks=%d  executed instructions here=%d"
          " (%.3f%% of all scenarios)" % (
              start, end, "Thumb" if stride == 2 else "ARM", len(blocks), total,
              100.0 * total / grand if grand else 0.0))
    print("| block start | block end | insns | executed | share of this range | share of all | exits | svc | indirect | ret |")
    print("|---|---|---:|---:|---:|---:|---|---|:--:|:--:|:--:|")
    for b in blocks[:args.top]:
        print("| `0x%08X` | `0x%08X` | %d | %d | %.2f%% | %.3f%% | %s | %s | %s | %s |" % (
            b["start"], b["end"], b["insns"], b["steps"],
            100.0 * b["steps"] / total if total else 0.0,
            100.0 * b["steps"] / grand if grand else 0.0,
            ", ".join("0x%08X" % e for e in b["exits"][:4]) or "-",
            "yes" if b["svc"] else "-", "yes" if b["indirect"] else "-",
            "yes" if b["ret"] else "-"))


if __name__ == "__main__":
    main()
