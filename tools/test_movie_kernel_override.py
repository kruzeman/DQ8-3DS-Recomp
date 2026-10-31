#!/usr/bin/env python3
"""Differential and bounded timing fixture for the profile-selected movie loop."""
from pathlib import Path
import os
import re
import statistics
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "target" / "release" / "dq8tool"
SPLITTER = ROOT / "tools" / "split_dispatch.py"
REGISTRY = ROOT / "tools" / "overrides" / "registry.json"
ENTRY = 0x001A93A8

FIXTURE = r'''
#define ENTRY 0x001A93A8u
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
    printf(" retired=%llu dispatches=%llu prev=%08X curr=%08X\n",
           (unsigned long long)ctx.retired_steps,
           (unsigned long long)ctx.dispatch_calls,
           previous_guest_pc, current_guest_pc);
}

static uint64_t time_calls(unsigned count) {
    memset(ram, 0, 0x8000u);
    for (unsigned j = 0; j < 64u; ++j) put16(0x3000u + j * 2u, 0x8001u);
    put32(0x4000u, 0x12345678u);
    put32(0x5000u, 0xFFFFFFFFu);
    struct timespec begin, end;
    clock_gettime(CLOCK_MONOTONIC, &begin);
    for (unsigned i = 0; i < count; ++i) {
        GuestThread ctx = {0};
        ctx.active = 1; ctx.pc = ENTRY;
        ctx.r[0] = 0x2000u; ctx.r[2] = 0x10000u; ctx.r[3] = 0x40u;
        ctx.r[11] = 0x3000u; ctx.r[12] = 0x4000u;
        ctx.r[13] = 0x5000u; ctx.r[14] = 0xFFFFFFFFu;
        main_guest_regs = NULL;
        exclusive_addr = 0x3000u; exclusive_size = 4u;
        translated_from(ctx.r, ENTRY, &ctx);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    return (uint64_t)(end.tv_sec - begin.tv_sec) * 1000000000ull +
           (uint64_t)(end.tv_nsec - begin.tv_nsec);
}

int main(void) {
    ram = calloc(1, GUEST_RAM_SIZE);
    if (!ram) return 90;
#ifdef DQ8_OVERRIDE_HANDLED
    if (getenv("DQ8_DISABLE_OVERRIDES")) dq8_overrides_enabled = 0;
    /* Explicitly exercise the handler's CALL_ORIGINAL result without running
       the sentinel path: the guest's top marker is exactly 3 here. */
    {
        uint32_t r[16] = {0}, pc = ENTRY;
        int n = 0, z = 0, c = 0, v = 0, ge = 0, running = 1;
        uint64_t steps = 1; unsigned budget = 256;
        r[3] = 3u << 25;
        int status = dq8_override_dispatch(ENTRY, r, &pc, &n, &z, &c, &v,
                                           &ge, &running, &steps, &budget);
        printf("FALLBACK status=%d pc=%08X steps=%llu budget=%u\n", status,
               pc, (unsigned long long)steps, budget);
    }
#endif
    uint32_t seed = 0xD28B53A1u;
    for (unsigned i = 0; i < 128u; ++i) {
        seed = seed * 1664525u + 1013904223u;
        run_state(i, seed);
    }
    for (unsigned rep = 0; rep < 5u; ++rep) {
        uint64_t ns = time_calls(200000u);
        printf("REGION_BENCH repeat=%u calls=200000 ns=%llu\n",
               rep, (unsigned long long)ns);
    }
#ifdef DQ8_OVERRIDE_HANDLED
    printf("OVERRIDE handled=%llu call_original=%llu enabled=%d\n",
           (unsigned long long)dq8_override_hits,
           (unsigned long long)dq8_override_original_fallbacks,
           dq8_overrides_enabled);
#endif
    free(ram);
    return 0;
}
'''


def run(cmd, **kwargs):
    return subprocess.run(list(map(str, cmd)), text=True,
                         capture_output=True, **kwargs)


class MovieKernelOverrideTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not TOOL.exists():
            build = run(["cargo", "build", "--offline", "--release"], cwd=ROOT,
                        env=dict(os.environ, CARGO_NET_OFFLINE="true",
                                 DQ8_RUN_GUEST_THREADS="1"), timeout=600)
            if build.returncode:
                raise AssertionError(build.stderr)
        cls.tmp = tempfile.TemporaryDirectory(prefix="dq8-movie-kernel-")
        cls.work = Path(cls.tmp.name)
        raw = cls.work / "movie.raw.c"
        translated = run([TOOL, "recompile-arm", ROOT / "DQ8.code",
                          hex(ENTRY), raw, "--base", "0x00100000"], timeout=300)
        if translated.returncode:
            raise AssertionError(translated.stderr)
        text = raw.read_text()
        marker = "int main(int argc, char **argv)"
        if marker not in text:
            raise AssertionError("generated main() not found")
        cls.source = cls.work / "movie_fixture.c"
        cls.source.write_text(text.replace(marker,
                              "int unused_main(int argc, char **argv)", 1) + FIXTURE)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def build_reference(self, opt):
        out = self.work / ("reference-" + opt[1:] + ".d")
        p = run(["python3", SPLITTER, self.source, out], env=dict(
                os.environ, DQ8_RUN_GUEST_THREADS="1"), timeout=180)
        self.assertEqual(p.returncode, 0, p.stderr)
        exe = self.work / ("reference-" + opt[1:])
        p = run(["clang", "-std=c11", opt, "-w", "-I" + str(out),
                 *sorted(map(str, out.glob("*.c"))), "-lm", "-o", exe], timeout=240)
        self.assertEqual(p.returncode, 0, p.stderr)
        return exe

    def build_override(self, opt):
        out = self.work / ("override-" + opt[1:] + ".d")
        p = run(["python3", SPLITTER, self.source, out,
                 "--overrides", REGISTRY], env=dict(os.environ,
                 DQ8_RUN_GUEST_THREADS="1"), timeout=180)
        self.assertEqual(p.returncode, 0, p.stderr)
        exe = self.work / ("override-" + opt[1:])
        p = run(["clang", "-std=c11", opt, "-w", "-I" + str(out),
                 *sorted(map(str, out.glob("*.c"))), "-lm", "-o", exe], timeout=240)
        self.assertEqual(p.returncode, 0, p.stderr)
        return exe

    def run_binary(self, exe, disabled=False):
        env = dict(os.environ, DQ8_RUN_GUEST_THREADS="1")
        env.pop("DQ8_INPUT_FILE", None)
        if disabled:
            env["DQ8_DISABLE_OVERRIDES"] = "1"
        else:
            env.pop("DQ8_DISABLE_OVERRIDES", None)
        p = run([exe], env=env, timeout=120)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        states = [line for line in p.stdout.splitlines() if line.startswith("STATE ")]
        self.assertEqual(len(states), 128, p.stdout + p.stderr)
        timings = [int(ns) for ns in re.findall(r"REGION_BENCH repeat=\d+ calls=\d+ ns=(\d+)", p.stdout)]
        self.assertEqual(len(timings), 5, p.stdout)
        return states, p.stdout, int(statistics.median(timings))

    def test_profiled_packed_loop_is_bit_exact_and_measurable(self):
        for opt in ("-O0", "-O2"):
            reference = self.build_reference(opt)
            override = self.build_override(opt)
            ref_states, _ref_output, ref_ns = self.run_binary(reference)
            on_states, on_output, on_ns = self.run_binary(override)
            off_states, off_output, _off_ns = self.run_binary(override, disabled=True)
            self.assertEqual(on_states, ref_states, "optimized region diverged at " + opt)
            self.assertEqual(off_states, ref_states, "disabled region diverged at " + opt)
            result = re.search(r"OVERRIDE handled=(\d+) call_original=(\d+) enabled=(\d+)", on_output)
            self.assertIsNotNone(result, on_output)
            self.assertGreater(int(result.group(1)), 0, "override never handled the hot loop")
            self.assertGreater(int(result.group(2)), 0, "CALL_ORIGINAL fallback was not exercised")
            disabled = re.search(r"OVERRIDE handled=(\d+) call_original=(\d+) enabled=(\d+)", off_output)
            self.assertIsNotNone(disabled, off_output)
            self.assertEqual(disabled.groups(), ("0", "0", "0"))
            print(f"REGION_RESULT opt={opt} reference_ns={ref_ns} override_ns={on_ns} "
                  f"speedup={ref_ns / on_ns:.3f}x median_of_5 calls=200000")


if __name__ == "__main__":
    unittest.main()
