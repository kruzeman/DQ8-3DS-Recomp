#!/usr/bin/env python3
"""Bisect the FN_EMIT list for the battle-scene guest memory fault (t_e1547d93).

  tools/fnhook_fault_bisect.py --bin target/dq8-fnhook --state <battle.sav> \
      --full <list with all emitted entries> --base tools/fn_emit_top300.txt

Every test enables a subset of the emitted functions with DQ8_FN_EMIT and runs
the battle savestate for a few seconds; the fault surfaces at frame ~90 as
`guest memory fault: pc=0x00137B98 address=0x44000010`.

The base list (the 300 functions of dq8-b35) is enabled in every test: it is
known not to fault, so a single culprit in the delta is found by halving.
"""
import argparse
import os
import re
import subprocess
import sys

FAULT = "guest memory fault"


def load(path):
    out = []
    for line in open(path):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        f = line.split()
        if len(f) >= 2 and f[0].startswith("0x"):
            out.append(f[0].upper().replace("0X", "0x"))
    return out


def run(binary, state, pcs, secs, tag):
    env = dict(os.environ)
    env.update(DQ8_HEADLESS="1", DQ8_GL="1", DQ8_RUN_GUEST_THREADS="1", DQ8_VSPAR="4",
               DQ8_SPLIT_GL="0", DQ8_STACK_NO_RELOCATE="1", DQ8_GL_DEPTH_ATTACH="1",
               DQ8_FN_EMIT=",".join(pcs),
               DQ8_USER_DATA_DIR=os.path.abspath("target/fnhook-fault-ud"))
    err = "target/fnhook-fault/%s.err" % tag
    os.makedirs(os.path.dirname(err), exist_ok=True)
    with open(err, "wb") as fh:
        subprocess.run(["timeout", "-k", "5", str(secs), binary, "--load-state", state,
                        "DQ8.code", "dq8.romfs"], env=env, stdout=subprocess.DEVNULL,
                       stderr=fh)
    txt = open(err, "rb").read().decode("utf-8", "replace")
    faulted = FAULT in txt
    m = re.search(r"guest memory fault: pc=0x([0-9A-Fa-f]+) address=0x([0-9A-Fa-f]+)", txt)
    frames = len(re.findall(r"^FRAMEPROF f=", txt, re.M))
    print("  %-16s n=%5d faulted=%-5s frames=%4d %s"
          % (tag, len(pcs), faulted, frames, (m.group(0) if m else "")), flush=True)
    return faulted


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default="target/dq8-fnhook")
    ap.add_argument("--state", default="/home/hermes/DQ8/evidence/hot-regions-20261102/state-battle.sav")
    ap.add_argument("--full", default="tools/fn_emit_pcrel.txt")
    ap.add_argument("--base", default="tools/fn_emit_top300.txt")
    ap.add_argument("--secs", type=int, default=8)
    a = ap.parse_args()

    base = load(a.base)
    full = load(a.full)
    delta = [p for p in full if p not in set(base)]
    print("base=%d full=%d delta=%d" % (len(base), len(full), len(delta)))

    if not run(a.bin, a.state, full, a.secs, "full"):
        print("NO FAULT with the full list -> nothing to bisect")
        return 0
    if not run(a.bin, a.state, base, a.secs, "base"):
        print("base list faults too -> the culprit is in the base, bisecting it instead")
        delta, base = base, []
    else:
        run(a.bin, a.state, base + delta, a.secs, "base+delta")

    level = 0
    while len(delta) > 2:
        level += 1
        h = len(delta) // 2
        left, right = delta[:h], delta[h:]
        lf = run(a.bin, a.state, base + left, a.secs, "L%d-left" % level)
        rf = run(a.bin, a.state, base + right, a.secs, "L%d-right" % level)
        if lf and not rf:
            delta = left
        elif rf and not lf:
            delta = right
        elif lf and rf:
            print("  both halves fault; following the left half (a second culprit may exist)")
            delta = left
        else:
            print("  neither half faults alone -> interaction; stopping")
            break
    print("candidate set (%d):" % len(delta), delta)
    for p in list(delta):
        run(a.bin, a.state, base + [p], a.secs, "solo-" + p)
    return 0


if __name__ == "__main__":
    sys.exit(main())
