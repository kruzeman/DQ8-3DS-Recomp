#!/usr/bin/env python3
"""Bisect the battle fault of the 2 800-function list on the production binary.

The compiled hook covers every list entry; DQ8_FN_EMIT=<comma pcs> only gates
which bodies are called, so a subset run needs no rebuild.  The fault fires
~1.3 s into the battle state, so a non-faulting arm is cut off with timeout.

usage: fnhook_fault_bisect2.py [--list tools/fn_emit_pcrel.txt] [--secs 5]
"""
import os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "target/dq8-fnhook")
STATE = "/home/hermes/DQ8/evidence/hot-regions-20261102/state-battle.sav"
LIST = os.path.join(ROOT, "tools/fn_emit_pcrel.txt")
SECS = "5"
BOOT = dict(DQ8_HEADLESS="1", DQ8_GL="1", DQ8_RUN_GUEST_THREADS="1", DQ8_VSPAR="4",
            DQ8_SPLIT_GL="0", DQ8_STACK_NO_RELOCATE="1", DQ8_GL_DEPTH_ATTACH="1",
            DQ8_FRAMEPROF="1")
faults = 0


def entries(path):
    out = []
    for line in open(path):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        f = line.split()
        if len(f) >= 2 and f[0].startswith("0x"):
            out.append(f[0])
    return out


def faults_with(pcs):
    global faults
    faults += 1
    env = dict(os.environ)
    env.update(BOOT)
    env["DQ8_FN_EMIT"] = ",".join(pcs) if pcs else ""
    with tempfile.TemporaryDirectory() as td:
        env["DQ8_USER_DATA_DIR"] = td
        p = subprocess.run(["timeout", "-k", "2", SECS, BIN, "--load-state", STATE,
                            "DQ8.code", "dq8.romfs"], cwd=ROOT, env=env,
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    err = p.stderr.decode("utf-8", "replace")
    hit = "guest memory fault: pc=0x00137B98" in err
    n = len(pcs)
    print("  run %2d n=%4d fault=%s rc=%d" % (faults, n, hit, p.returncode), flush=True)
    return hit


def main():
    allp = entries(LIST)
    print("list=%d" % len(allp), flush=True)
    if not faults_with(allp):
        print("full list does not fault -> nothing to bisect"); return
    cur = allp
    # invariant: faults_with(cur) is True
    while len(cur) > 1:
        mid = len(cur) // 2
        a, b = cur[:mid], cur[mid:]
        if faults_with(a):
            cur = a
        elif faults_with(b):
            cur = b
        else:
            print("  neither half faults at n=%d -> interaction" % len(cur))
            # find a minimal pair: test a against progressively larger b
            for k in (1, 2, 4, 8, 16, 32, 64):
                if faults_with(a + b[:k]):
                    cur = a + b[:k]
                    break
            else:
                print("  no fault with a+b[:64]; stopping"); break
            continue
    print("culprit: %s" % (" ".join(cur) if cur else "(none)"))


main()
