#!/usr/bin/env python3
"""S1b differential and isolated-cost fixture for the emitted movie block.

Modelled on the Perf-G movie-kernel fixture (reports/HYBRID_KERNELS.md,
tools/test_movie_kernel_override.py on perf/hybrid-kernels): the recompiler's
own generated interpreter for the movie region (entry 0x001A93A8) is compiled
twice — once as the plain dispatch path (reference) and once with
`--s1-block 0x001A93B4` (the S1b emitter's straight-line block) — and both
run 128 randomized same-state cases.  Every compared field (all GPRs, NZCV/GE,
FPR/FPSCR, exclusive monitor, pc, retired steps, dispatch count, guest-RAM
hash) must be bit-identical between:

  * the reference dispatch path,
  * the S1 build with the DQ8_S1_BLOCK gate unset/off,
  * the S1 build with DQ8_S1_BLOCK=0x001A93B4 (block hook on).

The per-instruction previous/current-PC diagnostics legitimately differ (the
hook skips the block's interior PCs); they are printed on a separate DIAG
line and excluded from the bit-exact comparison.

The fixture also reports the isolated block-pass cost: bench_step_limit is
pinned to the block length so each translated_from() call retires exactly one
pass of the 11-instruction block, once through per-instruction dispatch and
once through the emitted block, median of five 200k-call runs.

usage: DQ8_RUN_GUEST_THREADS=1 python3 tools/test_s1_block_emit.py [-v]
"""
from pathlib import Path
import os
import re
import statistics
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "target" / "release" / "dq8tool"
REGION_ENTRY = 0x001A93A8
BLOCK_ENTRY = 0x001A93B4
BLOCK_LEN = 11

FIXTURE = r'''
#define ENTRY 0x001A93B4u
#define BLOCK_LEN 11u
static void init_case(GuestThread *ctx, uint32_t seed, int clear_ram) {
    if (clear_ram) memset(ram, 0, 0x8000u);
    memset(ctx, 0, sizeof(*ctx));
    ctx->active = 1;
    ctx->pc = ENTRY;
    ctx->r[0] = 0x2000u;
    ctx->r[1] = seed ^ 0xDEADBEEFu;
    ctx->r[2] = 0x00010000u + (seed & 0xFFFFu);
    ctx->r[3] = seed & 0x01FFFFFFu; /* marker guard is false; one table item exits */
    ctx->r[11] = 0x3000u;
    ctx->r[12] = 0x4000u;
    ctx->r[13] = 0x5000u;
    ctx->r[14] = 0xFFFFFFFFu;
    ctx->n = (seed >> 0) & 1u; ctx->z = (seed >> 1) & 1u;
    ctx->c = (seed >> 2) & 1u; ctx->v = (seed >> 3) & 1u;
    ctx->ge = (seed >> 4) & 15u;
    ctx->fpscr = seed ^ 0x9E3779B9u;
    fpscr = ctx->fpscr;
    for (unsigned j = 0; j < 32u; ++j)
        fpr[j] = ctx->fpr[j] = seed + j * 0x1020304u;
    uint32_t table_index = ctx->r[3] >> 20;
    for (unsigned j = 0; j < 64u; ++j) {
        uint32_t x = seed + j * 0x45D9F3Bu;
        uint16_t entry = (uint16_t)((x & 0x7FF0u) | 0x8000u | (x & 15u));
        put16(ctx->r[11] + j * 2u, entry);
    }
    /* The selected metadata entry has a terminating top bit, while its
       remaining fields vary across the randomized same-state cases. */
    put16(ctx->r[11] + table_index * 2u,
          (uint16_t)(((seed & 0x7FF0u) | 0x8000u) | (seed & 15u)));
    put32(ctx->r[12], seed ^ 0xA5C35A7Du);
    put32(ctx->r[13], 0xFFFFFFFFu);
    uint32_t out_index = mem32(ctx->r[12]) & 0xFFu;
    exclusive_addr = (seed & 1u) ? 0x21B8u + out_index * 4u : 0x3000u;
    exclusive_size = 4u;
}

static uint64_t fixture_hash(void) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned i = 0; i < 0x8000u; ++i) { h ^= ram[i]; h *= 1099511628211ull; }
    return h;
}

static void run_state(unsigned i, uint32_t seed) {
    GuestThread ctx;
    init_case(&ctx, seed, 1);
    main_guest_regs = NULL; main_guest_pc = 0;
    previous_guest_pc = 0; current_guest_pc = 0;
    translated_from(ctx.r, ENTRY, &ctx);
    printf("STATE %u pc=%08X flags=%d%d%d%d/%X ex=%08X/%u fpscr=%08X hash=%016llX",
           i, ctx.pc, ctx.n, ctx.z, ctx.c, ctx.v, ctx.ge,
           exclusive_addr, exclusive_size, fpscr,
           (unsigned long long)fixture_hash());
    for (unsigned j = 0; j < 16u; ++j) printf(" %08X", ctx.r[j]);
    for (unsigned j = 0; j < 32u; ++j) printf(" %08X", fpr[j]);
    printf(" retired=%llu dispatches=%llu\n",
           (unsigned long long)ctx.retired_steps,
           (unsigned long long)ctx.dispatch_calls);
    /* Diagnostic only: with the hook on, the dispatcher never visits the
       block's interior PCs, so previous/current-PC tracking legitimately
       differs from per-instruction dispatch.  Excluded from the STATE
       comparison. */
    printf("DIAG %u prev=%08X curr=%08X\n", i, previous_guest_pc, current_guest_pc);
}

/* Exactly one pass of the 11-instruction block per call: the loop stops at
   bench_step_limit == BLOCK_LEN, which both arms reach with identical steps
   accounting (dispatch: 11 iterations; hook: 1 iteration + steps += 10), and
   neither arm ever executes the block's call target.  State is initialized
   once outside the timed loop; register/RAM drift across calls is identical
   in both arms (that is what the bit-exactness test proves), and only the
   restart pc is rewritten per call. */
static uint64_t time_block(unsigned count) {
    bench_step_limit = BLOCK_LEN;
    GuestThread ctx;
    init_case(&ctx, 0x5A17C0DEu, 1);
    struct timespec begin, end;
    clock_gettime(CLOCK_MONOTONIC, &begin);
    for (unsigned i = 0; i < count; ++i) {
        ctx.pc = ENTRY;
        main_guest_regs = NULL;
        previous_guest_pc = 0; current_guest_pc = 0;
        translated_from(ctx.r, ENTRY, &ctx);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    return (uint64_t)(end.tv_sec - begin.tv_sec) * 1000000000ull +
           (uint64_t)(end.tv_nsec - begin.tv_nsec);
}

int main(void) {
    ram = calloc(1, GUEST_RAM_SIZE);
    if (!ram) return 90;
#ifdef DQ8_S1_BLOCK_ENTRY
    /* The real runtime gate lives in the generated main(), which the fixture
       renames away; replicate the same env-gate semantics here. */
    {
        const char *s1_text = getenv("DQ8_S1_BLOCK");
        if (s1_text && *s1_text)
            dq8_s1_block_enabled =
                ((uint32_t)strtoull(s1_text, NULL, 0) == DQ8_S1_BLOCK_ENTRY);
        if (dq8_s1_block_enabled) atexit(dq8_s1_block_report);
        printf("GATE entry=0x%08X enabled=%d\n", DQ8_S1_BLOCK_ENTRY,
               dq8_s1_block_enabled);
    }
#endif
    uint32_t seed = 0xD28B53A1u;
    for (unsigned i = 0; i < 128u; ++i) {
        seed = seed * 1664525u + 1013904223u;
        run_state(i, seed);
    }
    for (unsigned rep = 0; rep < 5u; ++rep) {
        uint64_t ns = time_block(200000u);
        printf("REGION_BENCH repeat=%u calls=200000 ns=%llu\n",
               rep, (unsigned long long)ns);
    }
    free(ram);
    return 0;
}
'''


def run(cmd, **kwargs):
    return subprocess.run(list(map(str, cmd)), text=True,
                          capture_output=True, **kwargs)


class S1BlockEmitTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not TOOL.exists():
            build = run(["cargo", "build", "--offline", "--release"], cwd=ROOT,
                        env=dict(os.environ, CARGO_NET_OFFLINE="true",
                                 DQ8_RUN_GUEST_THREADS="1"), timeout=600)
            if build.returncode:
                raise AssertionError(build.stderr)
        cls.tmp = tempfile.TemporaryDirectory(prefix="dq8-s1-emit-")
        cls.work = Path(cls.tmp.name)
        cls.exes = {}
        for tag, s1 in (("reference", False), ("s1", True)):
            raw = cls.work / f"{tag}.raw.c"
            cmd = [TOOL, "recompile-arm", ROOT / "DQ8.code",
                   hex(REGION_ENTRY), raw, "--base", "0x00100000"]
            if s1:
                cmd += ["--s1-block", hex(BLOCK_ENTRY)]
            translated = run(cmd, timeout=300)
            if translated.returncode:
                raise AssertionError(translated.stderr)
            text = raw.read_text()
            if s1:
                if "dq8_s1_block_001A93B4" not in text:
                    raise AssertionError("S1 build lacks the emitted block")
            elif "dq8_s1_block" in text:
                raise AssertionError("reference build contains S1 code")
            marker = "int main(int argc, char **argv)"
            if marker not in text:
                raise AssertionError("generated main() not found")
            source = cls.work / f"{tag}_fixture.c"
            source.write_text(text.replace(marker,
                              "int unused_main(int argc, char **argv)", 1) + FIXTURE)
            for opt in ("-O0", "-O2"):
                exe = cls.work / f"{tag}{opt}"
                defines = ([f"-DDQ8_S1_BLOCK_ENTRY=0x{BLOCK_ENTRY:08X}u"]
                           if s1 else [])
                p = run(["clang", "-std=c11", opt, "-w", *defines,
                         str(source), "-lm", "-o", exe], timeout=300)
                if p.returncode:
                    raise AssertionError(p.stderr)
                cls.exes[(tag, opt)] = exe

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_states(self, exe, gate_on=False):
        env = dict(os.environ, DQ8_RUN_GUEST_THREADS="1")
        env.pop("DQ8_INPUT_FILE", None)
        if gate_on:
            env["DQ8_S1_BLOCK"] = f"0x{BLOCK_ENTRY:X}"
        else:
            env.pop("DQ8_S1_BLOCK", None)
        p = run([exe], env=env, timeout=300)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        lines = p.stdout.splitlines()
        states = [l for l in lines if l.startswith("STATE ")]
        diags = [l for l in lines if l.startswith("DIAG ")]
        self.assertEqual(len(states), 128, p.stdout + p.stderr)
        self.assertEqual(len(diags), 128, p.stdout + p.stderr)
        timings = [int(ns) for ns in
                   re.findall(r"REGION_BENCH repeat=\d+ calls=\d+ ns=(\d+)", p.stdout)]
        self.assertEqual(len(timings), 5, p.stdout)
        runs = re.search(r"S1_BLOCK entry=0x[0-9A-F]+ runs=(\d+)", p.stderr)
        return states, timings, (int(runs.group(1)) if runs else 0), p.stdout

    def test_emitted_block_is_bit_exact_on_randomized_states(self):
        results = {}
        for opt in ("-O0", "-O2"):
            ref_states, _, _, _ = self.run_states(self.exes[("reference", opt)])
            off_states, _, _, off_out = self.run_states(self.exes[("s1", opt)])
            self.assertIn("GATE entry=0x001A93B4 enabled=0", off_out)
            on_states, timings, runs, on_out = self.run_states(
                self.exes[("s1", opt)], gate_on=True)
            self.assertIn("GATE entry=0x001A93B4 enabled=1", on_out)
            self.assertEqual(off_states, ref_states,
                             "gate-off dispatch diverged from reference at " + opt)
            self.assertEqual(on_states, ref_states,
                             "emitted block diverged from dispatch at " + opt)
            self.assertGreater(runs, 0, "block hook never fired at " + opt)
            results[opt] = (timings, runs)
        for opt in ("-O0", "-O2"):
            print(f"S1_STATE_RESULT opt={opt} states=128 bit-exact "
                  f"(reference == gate-off == gate-on)")

    def test_isolated_block_pass_cost(self):
        summary = []
        for opt in ("-O0", "-O2"):
            _, ref_ns, _, _ = self.run_states(self.exes[("reference", opt)])
            _, off_ns, _, _ = self.run_states(self.exes[("s1", opt)])
            _, on_ns, runs, _ = self.run_states(self.exes[("s1", opt)], gate_on=True)
            r = statistics.median(ref_ns)
            o = statistics.median(off_ns)
            n = statistics.median(on_ns)
            summary.append((opt, r, o, n))
            print(f"REGION_RESULT opt={opt} reference_ns={r} gate_off_ns={o} "
                  f"emitted_ns={n} speedup={r / n:.3f}x median_of_5 calls=200000 "
                  f"block=one 11-insn pass per call")
        for opt, r, o, n in summary:
            self.assertGreater(r, 0)
            self.assertGreater(n, 0)


if __name__ == "__main__":
    unittest.main()
