#!/usr/bin/env python3
"""Dispatch-throughput benchmark for single-TU vs split part dispatch.

Compiles one synthetic guest (a 5M-iteration SUBS/BNE loop, 10M guest
instructions at 0x00100000) three ways and reports wall-clock time:

  single  - the original one-translation-unit interpreter
  split   - tools/split_dispatch.py with CHAIN_BATCH as shipped
  naive   - the same splitter with CHAIN_BATCH=1 (pre-chaining behaviour,
            kept here as the reference that block-chaining must beat)

Usage:  python3 tools/bench_split_dispatch.py <workdir> [repeats]
Prints one line per variant:  <name> ns=<best> mins=<M insn/s>
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def arm_loop_code(iterations: int) -> bytes:
    """Build r0 = iterations from rotated-immediate MOV/ORR, then loop:
    loop: SUBS r0, r0, #1; BNE loop; BX lr.

    MOVW/MOVT deliberately avoided: the recompiler's decoder at this SHA does
    not special-case them (they alias the TST/CMP immediate encoding space),
    so the counter is built from plain data-processing immediates instead.
    Any 32-bit value decomposes into its four bytes, each encodable as an
    imm8 rotated to an 8-bit-aligned position.
    """
    words = []
    first = True
    for i in range(4):
        byte = (iterations >> (8 * i)) & 0xFF
        if not byte:
            continue
        if first:
            words.append(0xE3A00000 | byte)          # MOV r0, #byte
            first = False
        else:
            rot = (16 - 4 * i) & 0xF                 # ror amount = 32-8i
            words.append(0xE3800000 | (rot << 8) | byte)  # ORR r0, r0, #(byte ror 2*rot)
    assert not first, "iterations must be non-zero"
    subs = 0xE2500001                       # SUBS r0, r0, #1
    bne = 0x1A000000 | ((-3) & 0xFFFFFF)    # back 3 instructions (pc+8 bias)
    bx = 0xE12FFF1E
    return b"".join(w.to_bytes(4, "little") for w in (*words, subs, bne, bx))


FIXTURE = r"""
int main(void) {
    ram = calloc(1, GUEST_RAM_SIZE);
    uint32_t r[16] = {0};
    r[13] = 0x10000000u; r[14] = 0xFFFFFFFFu;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    translated_from(r, 0x00100000u, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ns = (long)(t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);
    printf("ns=%ld r0=%u\n", ns, r[0]);
    return 0;
}
"""

ITERATIONS = 5_000_000
GUEST_INSNS = 2 + ITERATIONS * 2 + 1


def run(cmd, **kw):
    return subprocess.run(cmd, text=True, capture_output=True, **kw)


def build_single(src: Path, out: Path, opt="-O2"):
    p = run(["clang", "-std=c11", opt, "-w", str(src), "-lm", "-o", str(out)])
    p.check_returncode()


def build_split(src: Path, outdir: Path, exe: Path, opt="-O2", batch=None):
    script = ROOT / "tools" / "split_dispatch.py"
    if batch is not None:
        text = script.read_text()
        text2, n = re.subn(r"^CHAIN_BATCH = \d+$", "CHAIN_BATCH = %d" % batch,
                           text, count=1, flags=re.M)
        assert n == 1, "CHAIN_BATCH not found"
        tmp = outdir.parent / "split_dispatch_naive.py"
        tmp.write_text(text2)
        script = tmp
    p = run(["python3", str(script), str(src), str(outdir)])
    p.check_returncode()
    sources = sorted(outdir.glob("*.c"))
    p = run(["clang", "-std=c11", opt, "-w", "-I" + str(outdir),
             *map(str, sources), "-lm", "-o", str(exe)])
    p.check_returncode()


def measure(exe: Path, repeats: int) -> int:
    best = None
    env = dict(os.environ, DQ8_RUN_GUEST_THREADS="0")
    for _ in range(repeats):
        p = run([str(exe)], env=env, timeout=300)
        p.check_returncode()
        ns = int(re.search(r"ns=(\d+)", p.stdout).group(1))
        best = ns if best is None else min(best, ns)
    return best


def main():
    work = Path(sys.argv[1] if len(sys.argv) > 1 else tempfile.mkdtemp(prefix="dq8-bench-"))
    repeats = int(sys.argv[2]) if len(sys.argv) > 2 else 3
    work.mkdir(parents=True, exist_ok=True)

    code = work / "loop.code"
    code.write_bytes(arm_loop_code(ITERATIONS))
    single_src = work / "single.c"
    tool = ROOT / "target" / "release" / "dq8tool"
    p = run([str(tool), "recompile-arm", str(code), "0x00100000", str(single_src),
             "--base", "0x00100000"])
    p.check_returncode()

    text = single_src.read_text()
    marker = "int main(int argc, char **argv)"
    assert marker in text, "generated main() not found"
    single_src.write_text(text.replace(marker, "int original_main(int argc, char **argv)", 1)
                          .rstrip() + "\n" + FIXTURE)

    results = {"guest_insns": GUEST_INSNS}
    build_single(single_src, work / "single")
    results["single"] = measure(work / "single", repeats)

    build_split(single_src, work / "split", work / "split256")
    results["split"] = measure(work / "split256", repeats)

    build_split(single_src, work / "naive", work / "naive1", batch=1)
    results["naive"] = measure(work / "naive1", repeats)

    for name in ("single", "split", "naive"):
        ns = results[name]
        mins = GUEST_INSNS / ns  # M insn/s (ns per instruction => M/s)
        print("%-8s ns=%d  %.1f M insn/s" % (name, ns, mins))
    print("speedup split/naive = %.1fx   split/single = %.2fx"
          % (results["naive"] / results["split"], results["split"] / results["single"]))
    (work / "bench.json").write_text(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
