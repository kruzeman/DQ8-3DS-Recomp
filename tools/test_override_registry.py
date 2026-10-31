#!/usr/bin/env python3
"""Strict same-state differential tests for named guest-region overrides.

The reference is the normal generated dispatcher. The candidate is the same
translation split with one manifest handler. Randomized GPR/flag/memory states
compare all architectural GPRs, NZCV/GE, FP state, exclusives, PC, and the
fixture's guest-written RAM word. The handler exercises both HANDLED and
CALL_ORIGINAL, plus the runtime DQ8_DISABLE_OVERRIDES fallback.
"""
from pathlib import Path
import json
import os
import re
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "target" / "release" / "dq8tool"
SPLITTER = ROOT / "tools" / "split_dispatch.py"
BASE = 0x00130000
CODE = struct.pack("<4I",
    0xE1A00001,  # mov   r0,r1
    0xE2522001,  # subs  r2,r2,#1 (sets NZCV)
    0xE5830000,  # str   r0,[r3]
    0xE12FFF1E,  # bx    lr
)
HANDLER = r"""
if (r[1] & 1u) return DQ8_OVERRIDE_CALL_ORIGINAL;
r[0] = r[1];
*pc_io = guest_pc + 4u;
return DQ8_OVERRIDE_HANDLED;
"""
FIXTURE = r'''
int main(void) {
    ram = calloc(1, GUEST_RAM_SIZE);
    if (!ram) return 90;
#ifdef DQ8_OVERRIDE_HANDLED
    if (getenv("DQ8_DISABLE_OVERRIDES")) dq8_overrides_enabled = 0;
#endif
    uint32_t seed = 0xD28B53A1u;
    for (unsigned i = 0; i < 128u; ++i) {
        seed = seed * 1664525u + 1013904223u;
        GuestThread ctx = {0};
        ctx.active = 1; ctx.pc = 0x00130000u; ctx.r[14] = 0xFFFFFFFFu;
        ctx.r[0] = seed ^ 0xA5A55A5Au;
        ctx.r[1] = (seed & ~1u) | (i & 1u); /* force both override and original */
        ctx.r[2] = seed ^ 0x6D2B79F5u;
        ctx.r[3] = 0x00003000u;
        ctx.n = (i >> 0) & 1; ctx.z = (i >> 1) & 1;
        ctx.c = (i >> 2) & 1; ctx.v = (i >> 3) & 1; ctx.ge = i & 15u;
        ctx.exclusive_addr = 0x3000u; ctx.exclusive_size = 4u;
        ctx.fpscr = seed ^ 0x9E3779B9u;
        for (unsigned j = 0; j < 32u; ++j) ctx.fpr[j] = seed + j * 0x1020304u;
        put32(0x3000u, seed ^ 0x12345678u);
        translated_from(ctx.r, 0x00130000u, &ctx);
        printf("STATE %u pc=%08X flags=%d%d%d%d/%X ex=%08X/%u fpscr=%08X ram=%08X",
               i, ctx.pc, ctx.n, ctx.z, ctx.c, ctx.v, ctx.ge,
               ctx.exclusive_addr, ctx.exclusive_size, ctx.fpscr, mem32(0x3000u));
        for (unsigned j = 0; j < 16u; ++j) printf(" %08X", ctx.r[j]);
        for (unsigned j = 0; j < 32u; ++j) printf(" %08X", ctx.fpr[j]);
        putchar('\n');
    }
#ifdef DQ8_OVERRIDE_HANDLED
    printf("RESULT handled=%llu call_original=%llu enabled=%d\n",
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


class OverrideRegistryDifferential(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not TOOL.exists():
            p = run(["cargo", "build", "--offline", "--release"], cwd=ROOT,
                    env=dict(os.environ, CARGO_NET_OFFLINE="true",
                             DQ8_RUN_GUEST_THREADS="1"), timeout=600)
            if p.returncode:
                raise AssertionError(p.stderr)
        cls.tmp = tempfile.TemporaryDirectory(prefix="dq8-overrides-")
        cls.work = Path(cls.tmp.name)
        code = cls.work / "override.code"
        code.write_bytes(CODE)
        raw = cls.work / "override.raw.c"
        p = run([TOOL, "recompile-arm", code, hex(BASE), raw, "--base", hex(BASE)])
        if p.returncode:
            raise AssertionError(p.stderr)
        text = raw.read_text()
        marker = "int main(int argc, char **argv)"
        if marker not in text:
            raise AssertionError("generated main() not found")
        cls.source = cls.work / "override.c"
        cls.source.write_text(text.replace(marker,
                              "int unused_main(int argc, char **argv)", 1) + FIXTURE)
        cls.registry = cls.work / "registry.json"
        cls.registry.write_text(json.dumps({
            "version": 1,
            "overrides": [{
                "name": "mov_r0_r1",
                "start": hex(BASE),
                "end": hex(BASE + 4),
                "body": HANDLER,
            }],
        }))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def build_reference(self, opt):
        exe = self.work / ("reference-" + opt[1:])
        p = run(["clang", "-std=c11", opt, "-w", self.source,
                 "-lm", "-o", exe], timeout=180)
        self.assertEqual(p.returncode, 0, p.stderr)
        return exe

    def build_override(self, opt):
        name = "override-" + opt[1:]
        out = self.work / (name + ".d")
        p = run(["python3", SPLITTER, self.source, out,
                 "--overrides", self.registry])
        self.assertEqual(p.returncode, 0, p.stderr)
        sources = sorted(out.glob("*.c"))
        exe = self.work / name
        p = run(["clang", "-std=c11", opt, "-w", "-I" + str(out),
                 *sources, "-lm", "-o", exe], timeout=180)
        self.assertEqual(p.returncode, 0, p.stderr)
        return exe

    def run_binary(self, binary, disabled=False):
        env = dict(os.environ, DQ8_RUN_GUEST_THREADS="1")
        if disabled:
            env["DQ8_DISABLE_OVERRIDES"] = "1"
        else:
            env.pop("DQ8_DISABLE_OVERRIDES", None)
        p = run([binary], env=env, timeout=60)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        states = [line for line in p.stdout.splitlines() if line.startswith("STATE ")]
        self.assertEqual(len(states), 128)
        return states, p.stdout

    def test_randomized_state_equivalence_call_original_and_disable(self):
        for opt in ("-O0", "-O2"):
            reference = self.build_reference(opt)
            override = self.build_override(opt)
            ref_states, _ = self.run_binary(reference)
            on_states, on_output = self.run_binary(override)
            off_states, off_output = self.run_binary(override, disabled=True)
            self.assertEqual(on_states, ref_states, "enabled override changed state at " + opt)
            self.assertEqual(off_states, ref_states, "disabled override changed state at " + opt)
            on = re.search(r"RESULT handled=(\d+) call_original=(\d+) enabled=(\d+)", on_output)
            off = re.search(r"RESULT handled=(\d+) call_original=(\d+) enabled=(\d+)", off_output)
            self.assertIsNotNone(on)
            self.assertGreater(int(on.group(1)), 0, "HANDLED path was not exercised")
            self.assertGreater(int(on.group(2)), 0, "CALL_ORIGINAL path was not exercised")
            self.assertEqual(off.groups(), ("0", "0", "0"), "disable flag did not select reference")


if __name__ == "__main__":
    unittest.main()
