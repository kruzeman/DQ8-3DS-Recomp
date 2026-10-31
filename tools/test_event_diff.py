#!/usr/bin/env python3
"""Guest-event differential between two DQ8 runner binaries.

Runs both binaries over the SAME guest-step budget and the SAME user-data seed
with the service traces on (DQ8_TRACE_IPC / DQ8_TRACE_GPU / DQ8_TRACE_GSP_FB)
and compares, in order:

  1. the full ordered event stream (byte-identical => identical guest-visible
     behaviour: IPC requests, FS reads, PICA register/list writes, GSP
     transfers and framebuffer switches);
  2. if that differs, the multiset of the same lines (same events, different
     interleaving);
  3. if that differs too, the multiset of *guest-request* lines only (IPC, FS,
     PICA, XFER, APT), i.e. everything the executed guest code asked for,
     independent of host-paced producers (vblank scan-out, audio ticks).

Caveat (audit PERF-02): `steps` counts main-context instructions only, so the
host-paced producers do not line up run to run; compare the guest-request
multiset for longer budgets, or stay inside the deterministic window for the
strict ordered comparison.  Run the same binary twice as a control to see what
the harness alone can vary.

usage: tools/test_event_diff.py <steps> <outdir> <seed-userdata|-> <a> <b>
"""
import collections
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NOISE = ("BENCH ", "ALSA", "host audio", "guest progress:")
GUEST = ("guest IPC ", "FS ", "native FS ", "PICA ", "XFER ", "APT ")


def run(binp, steps, outdir, seed):
    tag = Path(binp).name
    ud = outdir / f"userdata-{tag}"
    if ud.exists():
        shutil.rmtree(ud)
    if seed != "-":
        shutil.copytree(seed, ud)
    env = dict(os.environ, DQ8_HEADLESS="1", DQ8_RUN_GUEST_THREADS="1",
               DQ8_BENCH_STEPS=str(steps), DQ8_BENCH_NO_SNAPSHOT="1",
               DQ8_USER_DATA_DIR=str(ud), DQ8_TRACE_IPC="1",
               DQ8_TRACE_GPU="1", DQ8_TRACE_GSP_FB="1")
    p = subprocess.run([str(binp), "DQ8.code", "target/dq8.romfs"], cwd=ROOT,
                       env=env, capture_output=True, text=True, timeout=3600)
    lines = [ln.replace(str(ud), "<USERDATA>")
             for ln in p.stdout.splitlines() + p.stderr.splitlines()
             if not ln.startswith(NOISE)]
    (outdir / f"{tag}.events").write_text("\n".join(lines) + "\n")
    print(f"{tag:32s} rc={p.returncode} events={len(lines)}")
    return lines


def main():
    steps, outdir, seed, a, b = sys.argv[1:]
    outdir = Path(outdir).resolve()
    outdir.mkdir(parents=True, exist_ok=True)
    ea = run(Path(a).resolve(), steps, outdir, seed)
    eb = run(Path(b).resolve(), steps, outdir, seed)
    if ea == eb:
        print(f"EVENT DIFFERENTIAL: IDENTICAL ({len(ea)} lines, ordered)")
        return 0
    ca, cb = collections.Counter(ea), collections.Counter(eb)
    if ca == cb:
        print(f"EVENT DIFFERENTIAL: SAME EVENTS, DIFFERENT INTERLEAVING ({len(ea)} lines)")
        return 0
    ga = collections.Counter(x for x in ea if x.startswith(GUEST))
    gb = collections.Counter(x for x in eb if x.startswith(GUEST))
    print(f"guest-request multiset: A={sum(ga.values())} B={sum(gb.values())} "
          f"identical={ga == gb}")
    for k, v in list((ga - gb).items())[:5]:
        print("  only A", v, k[:120])
    for k, v in list((gb - ga).items())[:5]:
        print("  only B", v, k[:120])
    print("EVENT DIFFERENTIAL: DIFFERENT")
    return 1


if __name__ == "__main__":
    sys.exit(main())
