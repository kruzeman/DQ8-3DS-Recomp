#!/usr/bin/env python3
"""Isolated access-path timing for the checked-memory fast path (t_221fd585).

Builds the same 7-instruction guest stub (ldr/str/ldrh/strh/ldrb/strb at one
address) through the splitter three ways and times a tight loop of
translated_from() calls, so the cost of the helper call can be measured without
the game, the rasteriser or the host scheduler in the way:

  nofast     splitter control knob DQ8_DISABLE_MEMORY_FASTPATH=1
             -> every access is `call mem32` (pre-11cf64c behaviour)
  b24        splitter from the baseline commit ca2786e
             -> access is `call dq8_mem32_fast`, which calls
                dq8_mem_identity_span out of line (the shipped dq8-b24 path)
  inline     current splitter
             -> the compare and the load are in the guest case, no call

usage:
  tools/mem_fastpath_bench.py [--baseline-ref ca2786e] [--reps 5] [--iters 200000]
"""
from __future__ import annotations

import argparse
import os
import statistics
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "target" / "release" / "dq8tool"
SPLITTER = ROOT / "tools" / "split_dispatch.py"
BASE = 0x00120000
CODE = struct.pack("<7I",
                   0xE5910000,  # ldr  r0,[r1]
                   0xE5812000,  # str  r2,[r1]
                   0xE1D130B0,  # ldrh r3,[r1]
                   0xE1C120B0,  # strh r2,[r1]
                   0xE5D14000,  # ldrb r4,[r1]
                   0xE5C12000,  # strb r2,[r1]
                   0xE12FFF1E,  # bx lr
                   )


def _cfg(cmd):
    try:
        p = subprocess.run(cmd, text=True, capture_output=True)
    except OSError:
        return None
    return p.stdout.split() if p.returncode == 0 else None


PLATFORM_FLAGS = ["-msse4.1"]
PLATFORM_LIBS = ["-lm"]
_sdl = _cfg(["sdl2-config", "--cflags"])
if _sdl is not None:
    PLATFORM_FLAGS = ["-DDQ8_ENABLE_SDL"] + _sdl + PLATFORM_FLAGS
    PLATFORM_LIBS += _cfg(["sdl2-config", "--libs"]) or []
_egl = _cfg(["pkg-config", "--cflags", "egl"])
if _egl is not None:
    PLATFORM_FLAGS = (["-DDQ8_ENABLE_GL", "-DDQ8_HAVE_X11_PRESENT"]
                      + _egl + PLATFORM_FLAGS)
    PLATFORM_LIBS += _cfg(["pkg-config", "--libs", "egl"]) or []
    PLATFORM_LIBS += _cfg(["pkg-config", "--libs", "x11", "xext"]) or []

FIXTURE = r'''
int main(void) {
    ram = calloc(1, GUEST_RAM_SIZE);
    if (!ram) return 90;
    uint32_t r[16] = {0};
    r[1] = 0x00200000u;          /* the address the 6 accesses hit */
    r[2] = 0x11223344u;
    r[13] = 0x10000000u;
    r[14] = 0xFFFFFFFFu;
    const char *e = getenv("DQ8_MEM_BENCH");
    unsigned long n = e ? strtoul(e, NULL, 0) : 200000ul;
    if (!n) n = 200000ul;
    unsigned long warm = n / 10ul + 1ul;
    for (unsigned long i = 0; i < warm; ++i)
        translated_from(r, 0x00120000u, NULL);
    struct timespec t0, t1;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t0);
    for (unsigned long i = 0; i < n; ++i)
        translated_from(r, 0x00120000u, NULL);
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t1);
    double ns = (double)(t1.tv_sec - t0.tv_sec) * 1e9 +
                (double)(t1.tv_nsec - t0.tv_nsec);
    printf("MEMBENCH n=%lu ns_total=%.0f ns_per_call=%.2f r0=%08X r3=%08X "
           "r4=%08X ram=%08X\n", n, ns, ns / (double)n,
           r[0], r[3], r[4], mem32(0x00200000u));
    free(ram);
    return 0;
}
'''


def run(cmd, **kw):
    return subprocess.run([str(c) for c in cmd], text=True,
                          capture_output=True, **kw)


def build(work: Path, source: Path, name: str, splitter: Path, opt: str,
          disable_fastpath: bool) -> Path:
    env = dict(os.environ)
    if disable_fastpath:
        env["DQ8_DISABLE_MEMORY_FASTPATH"] = "1"
    out = work / (name + ".d")
    p = run(["python3", splitter, source, out], env=env)
    if p.returncode:
        raise SystemExit("split failed (%s): %s" % (name, p.stderr))
    exe = work / name
    p = run(["clang", "-std=c11", opt, "-w", "-I" + str(out),
             *sorted(out.glob("*.c")), *PLATFORM_FLAGS, *PLATFORM_LIBS,
             "-o", exe], timeout=300)
    if p.returncode:
        raise SystemExit("compile failed (%s): %s" % (name, p.stderr))
    return exe


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--baseline-ref", default="ca2786e")
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--iters", type=int, default=200000)
    ap.add_argument("--opt", default="-O0")
    args = ap.parse_args()

    if not TOOL.exists():
        raise SystemExit("build the tool first: cargo build --release")

    with tempfile.TemporaryDirectory(prefix="dq8-mem-bench-") as tmp:
        work = Path(tmp)
        baseline_splitter = work / "split_dispatch_baseline.py"
        p = run(["git", "show",
                 "%s:tools/split_dispatch.py" % args.baseline_ref], cwd=ROOT)
        if p.returncode:
            raise SystemExit("cannot read %s:tools/split_dispatch.py: %s"
                             % (args.baseline_ref, p.stderr))
        baseline_splitter.write_text(p.stdout)
        if "dq8_mem32_fast" not in p.stdout:
            raise SystemExit("baseline %s has no fast-path helpers"
                             % args.baseline_ref)

        code = work / "memory.code"
        code.write_bytes(CODE)
        raw = work / "memory.raw.c"
        p = run([TOOL, "recompile-arm", code, hex(BASE), raw, "--base", hex(BASE)])
        if p.returncode:
            raise SystemExit(p.stderr)
        text = raw.read_text()
        marker = "int main(int argc, char **argv)"
        if marker not in text:
            raise SystemExit("generated main() not found")
        source = work / "memory.c"
        source.write_text(text.replace(marker,
                                       "int original_main(int argc, char **argv)", 1)
                          + FIXTURE)

        arms = {
            "nofast": build(work, source, "mem-nofast", SPLITTER, args.opt, True),
            "b24": build(work, source, "mem-b24", baseline_splitter, args.opt, False),
            "inline": build(work, source, "mem-inline", SPLITTER, args.opt, False),
        }

        env = dict(os.environ, DQ8_RUN_GUEST_THREADS="1",
                   DQ8_MEM_BENCH=str(args.iters))
        results = {k: [] for k in arms}
        # interleave the arms so host drift hits every arm equally
        for _ in range(args.reps):
            for name, exe in arms.items():
                p = run([exe], env=env, timeout=600)
                if p.returncode:
                    raise SystemExit("%s failed: %s" % (name, p.stderr))
                for line in p.stdout.splitlines():
                    if line.startswith("MEMBENCH"):
                        nspc = float(line.split("ns_per_call=")[1].split()[0])
                        results[name].append(nspc)
        print("iters=%d reps=%d opt=%s baseline=%s"
              % (args.iters, args.reps, args.opt, args.baseline_ref))
        med = {}
        for name in ("nofast", "b24", "inline"):
            v = results[name]
            med[name] = statistics.median(v)
            print("  %-7s ns/call samples=%s median=%.2f"
                  % (name, " ".join("%.1f" % x for x in v), med[name]))
        print("  inline vs b24:    %+.1f %%  (%.2f -> %.2f ns)"
              % (100.0 * (med["inline"] - med["b24"]) / med["b24"],
                 med["b24"], med["inline"]))
        print("  inline vs nofast: %+.1f %%  (%.2f -> %.2f ns)"
              % (100.0 * (med["inline"] - med["nofast"]) / med["nofast"],
                 med["nofast"], med["inline"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
