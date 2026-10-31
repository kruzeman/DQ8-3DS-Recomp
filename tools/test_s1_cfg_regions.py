#!/usr/bin/env python3
"""Per-region S1 rollout differential + isolated-cost fixture (batch 1).

For every converted region this builds the recompiler's own interpreter for the
function that contains the region TWICE — once as the plain dispatch path
(reference) and once with `--s1-cfg/--s1-cfg-end` (the native CFG) — and runs
128 randomized same-state cases through all three arms, requiring bit-identical
results for (reference == gate-off == gate-on): all GPRs, NZCV/GE, FPR/FPSCR,
the exclusive monitor, pc, retired steps, dispatch count and a 64 KiB guest-RAM
hash.  That is the S1c gate (`tools/test_s1_cfg_region.py`) applied per region,
so the batch has a per-region answer instead of a build-level one.

States come from `init_case()`: every region in this batch is the same
movie-decoder butterfly family (byte LUT at r8, byte stream at lr, block table
at r11, trip counter in r12/ip, return frame on the stack whose `ldmia ... pc`
pop carries the 0xFFFFFFFF stop sentinel), so one fixture covers them all.  All
inputs are seeded per case, so the 128 cases are 128 different states.

It also reports the isolated region-pass cost (median of five 50k-call runs) —
the same >= 2x go/no-go number S1c used, per region.

usage: DQ8_RUN_GUEST_THREADS=1 python3 tools/test_s1_cfg_regions.py [-v] [entry ...]
"""
from pathlib import Path
import os
import re
import statistics
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "target" / "release" / "dq8tool"
BENCH_CALLS = 50000

# entry, exclusive end, static guest instructions of one region visit
REGIONS = [
    (0x001A94F4, 0x001A9608, 69),   # rank 2 body (rollout #1)
    (0x001A99FC, 0x001A9A64, 26),   # rank 4 body (rollout #2)
    (0x001A96CC, 0x001A97B8, 59),   # rank 8 body (rollout #3)
    (0x001A9904, 0x001A9990, 35),   # rank 9 (rollout #4, self-looping)
]

TEMPLATE = r'''
#define ENTRY __ENTRY__u
static void init_case(GuestThread *ctx, uint32_t seed, int clear_ram) {
    if (clear_ram) {
        memset(ram, 0, 0x10000u);
        memset(ram + 0x02000000u, 0, 0x400u);
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->active = 1;
    ctx->pc = ENTRY;
    uint32_t s = seed;
    if (clear_ram) {
    /* r11 block table: the dispatch-side `ldmia r11!,{...}` between two
       region visits loads r0..r7 from here, so the WORDS must stay small —
       they feed the byte-LUT index below (a full 32-bit word would make the
       LUT address wander off the guest RAM image). */
    for (unsigned j = 0; j < 64u; ++j) {
        s = s * 1664525u + 1013904223u;
        put32(0x00003000u + j * 4u, (s & 0xFFu) * 0x01010101u);
    }
    /* lr byte stream: 0x1400 bytes covers 8 iterations x 0x200 of lr. */
    for (unsigned j = 0; j < 0x1400u / 4u; ++j) {
        s = s * 1664525u + 1013904223u;
        put32(0x00006000u + j * 4u, s);
    }
    /* r8 byte LUT; the region indexes it with byte + small arithmetic. */
    for (unsigned j = 0; j < 0x400u / 4u; ++j) {
        s = s * 1664525u + 1013904223u;
        put32(0x02000000u + j * 4u, s);
    }
    /* Return frame: `ldmia sp!,{r0,r1,r2,r3,r10,r11,ip,pc}` pops pc from
       sp+0x1C; 0xFFFFFFFF is the interpreter's stop sentinel. */
    for (unsigned j = 0; j < 8u; ++j) {
        s = s * 1664525u + 1013904223u;
        put32(0x00009000u + j * 4u, s);
    }
    put32(0x00009000u + 0x1Cu, 0xFFFFFFFFu);
    }
    for (unsigned j = 0; j < 8u; ++j) {
        s = s * 1664525u + 1013904223u;
        ctx->r[j] = s & 0xFFu;            /* butterfly inputs are bytes */
    }
    ctx->r[8] = 0x02000000u;              /* byte LUT */
    ctx->r[9] = (s >> 3) & 0xFFu;
    ctx->r[10] = (s >> 5) & 0xFFu;
    ctx->r[11] = 0x00003000u;             /* block table (outside the region) */
    ctx->r[12] = 1u + ((s >> 7) % 6u);    /* ip trip counter, 1..6 */
    ctx->r[13] = 0x00009000u;             /* stack / return frame */
    ctx->r[14] = 0x00006000u;             /* byte stream */
    ctx->n = (s >> 0) & 1u; ctx->z = (s >> 1) & 1u;
    ctx->c = (s >> 2) & 1u; ctx->v = (s >> 3) & 1u;
    ctx->ge = (s >> 4) & 15u;
    ctx->fpscr = s ^ 0x9E3779B9u;
    fpscr = ctx->fpscr;
    for (unsigned j = 0; j < 32u; ++j)
        fpr[j] = ctx->fpr[j] = s + j * 0x1020304u;
    exclusive_addr = 0x21B8u + (seed & 0xFFu) * 4u;
    exclusive_size = 4u;
}

/* Table + stream + stack live below 64 KiB; the LUT is read-only. */
static uint64_t fixture_hash(void) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned i = 0; i < 0x10000u; ++i) { h ^= ram[i]; h *= 1099511628211ull; }
    return h;
}

static void run_state(unsigned i, uint32_t seed) {
    GuestThread ctx;
    bench_step_limit = 200000u;           /* runaway guard, one pass is << this */
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
    /* Diagnostic only: with the hook on the dispatcher never visits the
       region's interior PCs, so previous/current-PC tracking legitimately
       differs.  Excluded from the STATE comparison. */
    printf("DIAG %u prev=%08X curr=%08X\n", i, previous_guest_pc, current_guest_pc);
}

/* One full region pass per call: every case is re-initialised (the region
   consumes r11/lr/ip).  The guest RAM image is set up ONCE per timed run —
   filling 1.6k words per call would be counted in BOTH arms and drag the
   isolated region cost towards 1.0x. */
static uint64_t time_region(unsigned count) {
    bench_step_limit = 200000u;
    GuestThread ctx;
    struct timespec begin, end;
    init_case(&ctx, 0x5A17C0DEu, 1);      /* one full RAM setup */
    clock_gettime(CLOCK_MONOTONIC, &begin);
    for (unsigned i = 0; i < count; ++i) {
        init_case(&ctx, 0x5A17C0DEu, 0);  /* registers only */
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
            dq8_s1_cfg_enabled___HEX__ =
                ((uint32_t)strtoull(s1_text, NULL, 0) == DQ8_S1_CFG_ENTRY);
        if (dq8_s1_cfg_enabled___HEX__) atexit(dq8_s1_cfg_report___HEX__);
        printf("GATE entry=0x%08X enabled=%d\n", DQ8_S1_CFG_ENTRY,
               dq8_s1_cfg_enabled___HEX__);
    }
#endif
    uint32_t seed = 0xD28B53A1u;
    for (unsigned i = 0; i < 128u; ++i) {
        seed = seed * 1664525u + 1013904223u;
        run_state(i, seed);
    }
    for (unsigned rep = 0; rep < 5u; ++rep) {
        uint64_t ns = time_region(50000u);
        printf("REGION_BENCH repeat=%u calls=50000 ns=%llu\n", rep,
               (unsigned long long)ns);
    }
    free(ram);
    return 0;
}
'''


def run(cmd, **kwargs):
    return subprocess.run(list(map(str, cmd)), text=True, capture_output=True,
                          **kwargs)


class RegionTest(unittest.TestCase):
    """One test per converted region (methods generated below)."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="dq8-s1-regions-")

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()


def _build_case(entry, end, insns):
    def test(self):
        _check_region(self, entry, end, insns)
    return test


def _check_region(self, entry, end, insns):
    hex_entry = f"{entry:08X}"
    work = Path(self.tmp.name) / f"r{hex_entry}"
    work.mkdir(parents=True, exist_ok=True)
    exes = {}
    for tag, cfg in (("reference", False), ("s1", True)):
        raw = work / f"{tag}.raw.c"
        cmd = [TOOL, "recompile-arm", ROOT / "DQ8.code", hex(entry), raw,
               "--base", "0x00100000"]
        if cfg:
            cmd += ["--s1-cfg", hex(entry), "--s1-cfg-end", hex(end)]
        p = run(cmd, timeout=600)
        if p.returncode:
            self.fail(f"recompile-arm {tag} failed: {p.stderr[-2000:]}")
        text = raw.read_text()
        if cfg:
            if f"dq8_s1_cfg_{hex_entry}" not in text:
                self.fail("S1 build lacks the emitted region CFG")
        elif "dq8_s1_cfg" in text:
            self.fail("reference build contains S1c code")
        marker = "int main(int argc, char **argv)"
        if marker not in text:
            self.fail("generated main() not found")
        fixture = (TEMPLATE.replace("__ENTRY__", f"0x{hex_entry}")
                           .replace("__HEX__", hex_entry))
        source = work / f"{tag}_fixture.c"
        source.write_text(text.replace(marker,
                          "int unused_main(int argc, char **argv)", 1) + fixture)
        for opt in ("-O0", "-O2"):
            exe = work / f"{tag}{opt}"
            defines = ([f"-DDQ8_S1_CFG_ENTRY=0x{hex_entry}u"] if cfg else [])
            p = run(["clang", "-std=c11", opt, "-w", *defines, str(source),
                     "-lm", "-o", exe], timeout=600)
            if p.returncode:
                self.fail(f"clang {tag}{opt}: {p.stderr[-2000:]}")
            exes[(tag, opt)] = exe

    def states(exe, gate_on=False):
        env = dict(os.environ, DQ8_RUN_GUEST_THREADS="1")
        env.pop("DQ8_INPUT_FILE", None)
        if gate_on:
            env["DQ8_S1_CFG"] = f"0x{entry:X}"
        else:
            env.pop("DQ8_S1_CFG", None)
        p = run([exe], env=env, timeout=600)
        self.assertEqual(p.returncode, 0, p.stdout[-4000:] + p.stderr[-4000:])
        lines = p.stdout.splitlines()
        st = [l for l in lines if l.startswith("STATE ")]
        self.assertEqual(len(st), 128, p.stdout[-2000:])
        timings = [int(ns) for ns in
                   re.findall(r"REGION_BENCH repeat=\d+ calls=\d+ ns=(\d+)",
                              p.stdout)]
        self.assertEqual(len(timings), 5, p.stdout[-2000:])
        m = re.search(r"S1_CFG entry=0x[0-9A-F]+ runs=(\d+) insns=(\d+)",
                      p.stderr)
        return st, timings, (int(m.group(1)), int(m.group(2))) if m else (0, 0), p.stdout

    for opt in ("-O0", "-O2"):
        ref_states, ref_t, _, _ = states(exes[("reference", opt)])
        off_states, _, _, off_out = states(exes[("s1", opt)])
        self.assertIn("enabled=0", off_out)
        on_states, on_t, (runs, insn_total), on_out = states(
            exes[("s1", opt)], gate_on=True)
        self.assertIn("enabled=1", on_out)
        self.assertEqual(off_states, ref_states,
                         f"gate-off dispatch diverged from reference at {opt}")
        self.assertEqual(on_states, ref_states,
                         f"region CFG diverged from dispatch at {opt}")
        # The hook must actually have fired, and every entry must have
        # retired at least the region's static instruction count.
        self.assertGreater(runs, 0, f"region hook never fired at {opt}")
        self.assertGreaterEqual(insn_total, runs * insns,
                                f"insns={insn_total} < runs*{insns} at {opt}")
        self.assertGreaterEqual(runs, 50000, f"too few region entries at {opt}")
        r = statistics.median(ref_t)
        n = statistics.median(on_t)
        print(f"S1R_STATE_RESULT entry=0x{hex_entry} opt={opt} states=128 "
              f"bit-exact (reference == gate-off == gate-on) runs={runs} "
              f"insns={insn_total} static={insns}")
        print(f"S1R_COST_RESULT entry=0x{hex_entry} opt={opt} "
              f"reference_ns={r} cfg_ns={n} speedup={r / n:.3f}x "
              f"median_of_5 calls={BENCH_CALLS} insns_per_entry>={insns}")


def _make_tests():
    for entry, end, insns in REGIONS:
        name = f"test_region_{entry:08X}_is_bit_exact"
        setattr(RegionTest, name, _build_case(entry, end, insns))


if __name__ == "__main__":
    only = [int(a, 16) for a in sys.argv[1:] if a.startswith("0x")]
    if only:
        REGIONS[:] = [r for r in REGIONS if r[0] in only]
        sys.argv = [sys.argv[0]]
    if not TOOL.exists():
        b = run(["cargo", "build", "--offline", "--release"], cwd=ROOT,
                env=dict(os.environ, CARGO_NET_OFFLINE="true"), timeout=900)
        if b.returncode:
            raise SystemExit(b.stderr)
    _make_tests()
    unittest.main(verbosity=2)
