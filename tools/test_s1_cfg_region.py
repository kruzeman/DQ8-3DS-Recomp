#!/usr/bin/env python3
"""S1c differential and region-cost fixture for the emitted movie-region CFG.

Modelled on the S1b fixture (tools/test_s1_block_emit.py), itself modelled on
the Perf-G movie-kernel fixture (reports/HYBRID_KERNELS.md): the recompiler's
own generated interpreter for the movie region (entry 0x001A93A8) is compiled
twice — once as the plain dispatch path (reference) and once with
`--s1-cfg 0x001A93A8 --s1-cfg-end 0x001A9410` (the S1c region CFG) — and both
run 128 randomized same-state cases.  Every compared field (all GPRs, NZCV/GE,
FPR/FPSCR, exclusive monitor, pc, retired steps, dispatch count, guest-RAM
hash) must be bit-identical between:

  * the reference dispatch path,
  * the S1 build with the DQ8_S1_CFG gate unset/off,
  * the S1 build with DQ8_S1_CFG=0x001A93A8 (region hook on).

Each case executes ONE full region pass: entry 0x001A93A8, six loop
iterations over the packed-field table (indices 1,2,4,8,16,32 — the entry at
index 32 carries the terminating top bit), then the `ldr pc,[sp],#4` tail
return whose sentinel address 0xFFFFFFFF stops the interpreter.  That is 151
guest instructions per pass, of which the loop back-edge B2->B0 (a direct
`goto` in the CFG, a dispatcher round-trip in the reference) is taken 5 times.

The fixture also reports the isolated region-pass cost: bench_step_limit is
pinned above one pass so each translated_from() call retires exactly one pass,
once through per-instruction dispatch and once through the region CFG, median
of five 50k-call runs.  This is the S1c go/no-go measurement (>= 2x).

usage: DQ8_RUN_GUEST_THREADS=1 python3 tools/test_s1_cfg_region.py [-v]
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
REGION_END = 0x001A9410
# Guest instructions per region pass (see module doc): 6 * (3 + 11 + 11) + 1.
PASS_INSNS = 151
BENCH_CALLS = 50000

FIXTURE = r'''
#define ENTRY 0x001A93A8u
static void init_case(GuestThread *ctx, uint32_t seed, int clear_ram) {
    if (clear_ram) memset(ram, 0, 0x8000u);
    memset(ctx, 0, sizeof(*ctx));
    ctx->active = 1;
    ctx->pc = ENTRY;
    ctx->r[0] = 0x2000u;                    /* object base; out = r0+0x1B8 */
    ctx->r[2] = 0x00010000u + (seed & 0xFFFFu); /* subs r2,r5 never overflows */
    /* Packed-field cursor: low 20 bits clear, table index 1.  Every table
       entry carries low nibble 1, so each loop iteration shifts the cursor
       left by one and the index walks 1,2,4,8,16,32. */
    ctx->r[3] = 1u << 20;
    ctx->r[11] = 0x3000u;                   /* metadata table base */
    ctx->r[12] = 0x4000u;                   /* stream cursor */
    ctx->r[13] = 0x5000u;                   /* stack; return sentinel slot */
    ctx->r[14] = 0xFFFFFFFFu;
    ctx->n = (seed >> 0) & 1u; ctx->z = (seed >> 1) & 1u;
    ctx->c = (seed >> 2) & 1u; ctx->v = (seed >> 3) & 1u;
    ctx->ge = (seed >> 4) & 15u;
    ctx->fpscr = seed ^ 0x9E3779B9u;
    fpscr = ctx->fpscr;
    for (unsigned j = 0; j < 32u; ++j)
        fpr[j] = ctx->fpr[j] = seed + j * 0x1020304u;
    for (unsigned j = 0; j < 64u; ++j) {
        /* low nibble 1 (index step), varying bits 4..14, terminating top bit
           ONLY at index 32 so the loop closes after six iterations */
        uint16_t entry = (uint16_t)(((seed >> (j % 16u)) & 0x7FF0u) | 1u);
        if (j == 32u) entry = (uint16_t)(entry | 0x8000u);
        put16(ctx->r[11] + j * 2u, entry);
    }
    for (unsigned j = 0; j < 64u; ++j)
        put32(ctx->r[12] + j * 4u, seed ^ (0xA5C35A7Du + j * 0x10203u));
    put32(ctx->r[13], 0xFFFFFFFFu);         /* tail return stops the pass */
    exclusive_addr = 0x21B8u + (seed & 0xFFu) * 4u;
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
       region's interior PCs, so previous/current-PC tracking legitimately
       differs from per-instruction dispatch.  Excluded from the STATE
       comparison. */
    printf("DIAG %u prev=%08X curr=%08X\n", i, previous_guest_pc, current_guest_pc);
}

/* Exactly one full region pass per call: bench_step_limit never truncates a
   pass (151 instructions), and the pass ends with the tail return's sentinel
   address, which clears running.  The case state is reset at the start of
   EVERY timed call (without the 32 KB RAM clear): the region consumes its
   cursor registers (r3 shifts by r5 each iteration, r13 post-increments), so
   a once-initialized state would drift into a different — and for this table
   non-terminating — path after the first pass.  The reset costs the same in
   both arms and keeps every call a genuine entry-to-exit region pass. */
static uint64_t time_region(unsigned count) {
    bench_step_limit = 100000u;
    GuestThread ctx;
    struct timespec begin, end;
    clock_gettime(CLOCK_MONOTONIC, &begin);
    for (unsigned i = 0; i < count; ++i) {
        init_case(&ctx, 0x5A17C0DEu, 0);
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
#ifdef DQ8_S1_CFG_ENTRY
    /* The real runtime gate lives in the generated main(), which the fixture
       renames away; replicate the same env-gate semantics here. */
    {
        const char *s1_text = getenv("DQ8_S1_CFG");
        if (s1_text && *s1_text)
            dq8_s1_cfg_enabled_001A93A8 =
                ((uint32_t)strtoull(s1_text, NULL, 0) == DQ8_S1_CFG_ENTRY);
        if (dq8_s1_cfg_enabled_001A93A8) atexit(dq8_s1_cfg_report_001A93A8);
        printf("GATE entry=0x%08X enabled=%d\n", DQ8_S1_CFG_ENTRY,
               dq8_s1_cfg_enabled_001A93A8);
    }
#endif
    uint32_t seed = 0xD28B53A1u;
    for (unsigned i = 0; i < 128u; ++i) {
        seed = seed * 1664525u + 1013904223u;
        run_state(i, seed);
    }
    for (unsigned rep = 0; rep < 5u; ++rep) {
        uint64_t ns = time_region(50000u);
        printf("REGION_BENCH repeat=%u calls=50000 ns=%llu\n",
               rep, (unsigned long long)ns);
    }
    free(ram);
    return 0;
}
'''


def run(cmd, **kwargs):
    return subprocess.run(list(map(str, cmd)), text=True,
                          capture_output=True, **kwargs)


class S1CfgRegionTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not TOOL.exists():
            build = run(["cargo", "build", "--offline", "--release"], cwd=ROOT,
                        env=dict(os.environ, CARGO_NET_OFFLINE="true",
                                 DQ8_RUN_GUEST_THREADS="1"), timeout=600)
            if build.returncode:
                raise AssertionError(build.stderr)
        cls.tmp = tempfile.TemporaryDirectory(prefix="dq8-s1-cfg-")
        cls.work = Path(cls.tmp.name)
        cls.exes = {}
        for tag, cfg in (("reference", False), ("s1", True)):
            raw = cls.work / f"{tag}.raw.c"
            cmd = [TOOL, "recompile-arm", ROOT / "DQ8.code",
                   hex(REGION_ENTRY), raw, "--base", "0x00100000"]
            if cfg:
                cmd += ["--s1-cfg", hex(REGION_ENTRY),
                        "--s1-cfg-end", hex(REGION_END)]
            translated = run(cmd, timeout=300)
            if translated.returncode:
                raise AssertionError(translated.stderr)
            text = raw.read_text()
            if cfg:
                if "dq8_s1_cfg_001A93A8" not in text:
                    raise AssertionError("S1 build lacks the emitted region CFG")
                if "if (z) goto L_001A93A8;" not in text:
                    raise AssertionError("region CFG lacks the loop back-edge")
            elif "dq8_s1_cfg" in text:
                raise AssertionError("reference build contains S1c code")
            marker = "int main(int argc, char **argv)"
            if marker not in text:
                raise AssertionError("generated main() not found")
            source = cls.work / f"{tag}_fixture.c"
            source.write_text(text.replace(marker,
                              "int unused_main(int argc, char **argv)", 1) + FIXTURE)
            for opt in ("-O0", "-O2"):
                exe = cls.work / f"{tag}{opt}"
                defines = ([f"-DDQ8_S1_CFG_ENTRY=0x{REGION_ENTRY:08X}u"]
                           if cfg else [])
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
            env["DQ8_S1_CFG"] = f"0x{REGION_ENTRY:X}"
        else:
            env.pop("DQ8_S1_CFG", None)
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
        runs = re.search(r"S1_CFG entry=0x[0-9A-F]+ runs=(\d+) insns=(\d+)",
                         p.stderr)
        counters = (int(runs.group(1)), int(runs.group(2))) if runs else (0, 0)
        return states, timings, counters, p.stdout

    def test_region_cfg_is_bit_exact_on_randomized_states(self):
        for opt in ("-O0", "-O2"):
            ref_states, _, _, _ = self.run_states(self.exes[("reference", opt)])
            off_states, _, _, off_out = self.run_states(self.exes[("s1", opt)])
            self.assertIn("GATE entry=0x001A93A8 enabled=0", off_out)
            on_states, timings, (runs, insns), on_out = self.run_states(
                self.exes[("s1", opt)], gate_on=True)
            self.assertIn("GATE entry=0x001A93A8 enabled=1", on_out)
            self.assertEqual(off_states, ref_states,
                             "gate-off dispatch diverged from reference at " + opt)
            self.assertEqual(on_states, ref_states,
                             "region CFG diverged from dispatch at " + opt)
            self.assertGreater(runs, 0, "region hook never fired at " + opt)
            # One hook run per region pass: the 128 state passes plus the 5 x
            # 50000 timed passes in the same process, exactly PASS_INSNS guest
            # instructions each — the exec credits keep step accounting exact.
            total_passes = 128 + 5 * BENCH_CALLS
            self.assertEqual(runs, total_passes, opt)
            self.assertEqual(insns, total_passes * PASS_INSNS, opt)
            print(f"S1C_STATE_RESULT opt={opt} states=128 bit-exact "
                  f"(reference == gate-off == gate-on) runs={runs} insns={insns}")

    def test_region_pass_cost(self):
        summary = []
        for opt in ("-O0", "-O2"):
            _, ref_ns, _, _ = self.run_states(self.exes[("reference", opt)])
            _, off_ns, _, _ = self.run_states(self.exes[("s1", opt)])
            _, on_ns, _, _ = self.run_states(self.exes[("s1", opt)], gate_on=True)
            r = statistics.median(ref_ns)
            o = statistics.median(off_ns)
            n = statistics.median(on_ns)
            summary.append((opt, r, o, n))
            print(f"S1C_REGION_RESULT opt={opt} reference_ns={r} gate_off_ns={o} "
                  f"cfg_ns={n} speedup={r / n:.3f}x median_of_5 calls={BENCH_CALLS} "
                  f"pass={PASS_INSNS} insns (6 loop iterations, 5 back-edges)")
        for opt, r, o, n in summary:
            self.assertGreater(r, 0)
            self.assertGreater(n, 0)


if __name__ == "__main__":
    unittest.main()
