#!/usr/bin/env python3
"""Differential test for split-dispatch checked memory fast paths.

The same synthetic ARM loads/stores run through the single-TU reference helpers
and the split TUs' header-inline path at -O0/-O2. Covers randomized values,
unaligned spans, the normal/FCRAM/VRAM mappings, HID/write tracing, exclusive
reservation invalidation, and an out-of-bounds fault.
"""
from pathlib import Path
import os
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "target" / "release" / "dq8tool"
SPLITTER = ROOT / "tools" / "split_dispatch.py"


def _cfg(cmd):
    try:
        p = subprocess.run(cmd, text=True, capture_output=True)
    except OSError:
        return None
    return p.stdout.split() if p.returncode == 0 else None


# Platform gates shared with tools/build_split.sh.  The generated prelude does
# not compile without them (pica_vs declarations live inside
# #if defined(__SSE4_1__), and the GL/SDL sources are linked into the runtime),
# so the reference arm of this test needs the same flags as the real build.
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
FIXTURE = r'''
int main(void) {
    ram = calloc(1, GUEST_RAM_SIZE);
    if (!ram) return 90;
    if (getenv("DQ8_MEM_TEST_BAD")) {
        uint32_t r[16] = {0}; r[1] = GUEST_RAM_SIZE - 3u;
        r[13] = 0x10000000u; r[14] = 0xFFFFFFFFu;
        translated_from(r, 0x00120000u, NULL);
        return 91;
    }
    const uint32_t addresses[] = {
        0x00001000u, 0x00001003u, 0x00008001u, 0x17FFFFFEu,
        0x18000003u, 0x1FFFFFFFu, 0x20000003u, 0x30000005u,
        0x3FFFFFFCu, 0x00012000u, 0x00012001u, 0x00012002u,
        /* t_221fd585: page-boundary crossings and the exact edges of the
           inline fast-path window (0 < a < 0x17FFFFFC for a word load). */
        0x00000FFCu, 0x00000FFDu, 0x00000FFEu, 0x00000FFFu, 0x00001001u,
        0x00120FFCu, 0x00120FFDu, 0x00120FFFu, 0x17FFFFFCu, 0x17FFFFFDu,
        0x0FFFFFFFu
    };
    uint32_t seed = 0x73A4D19Bu;
    const unsigned count = sizeof(addresses) / sizeof(addresses[0]);
    for (unsigned i = 0; i < count; ++i) {
        uint32_t a = addresses[i], mapped = guest_memory_address(a);
        seed = seed * 1664525u + 1013904223u;
        memcpy(ram + mapped, &seed, sizeof(seed));
        uint32_t r[16] = {0};
        r[1] = a; r[2] = seed ^ 0xA5C3E17Du;
        r[13] = 0x10000000u; r[14] = 0xFFFFFFFFu;
        trace_guest_write_hits = 0u;
        trace_write_n = 0u;
        trace_hid_reads_enabled = 0;
        trace_hid_read_base = trace_hid_read_end = 0u;
        trace_hid_pc_count = 0u; memset(trace_hid_pcs, 0, sizeof(trace_hid_pcs));
        exclusive_addr = 0xFFFFFFFFu; exclusive_size = 0u;
        if (i == 3u) {
            trace_hid_reads_enabled = 1;
            trace_hid_read_base = a; trace_hid_read_end = a + 4u;
        }
        if (i == 4u) {
            trace_guest_write_addr[0] = a; trace_guest_write_end[0] = a + 4u;
            trace_write_n = 1u;
        }
        if (i == 5u) { exclusive_addr = a; exclusive_size = 8u; }
        translated_from(r, 0x00120000u, NULL);
        printf("case=%u a=%08X mapped=%08X r0=%08X r3=%08X r4=%08X "
               "ram=%08X exclusive=%08X writes=%u hidpcs=%u\n",
               i, a, mapped, r[0], r[3], r[4], mem32(a), exclusive_addr,
               trace_guest_write_hits, trace_hid_pc_count);
    }
    free(ram);
    return 0;
}
'''


def run(cmd, **kwargs):
    return subprocess.run(list(map(str, cmd)), text=True,
                          capture_output=True, **kwargs)


class InlineMemoryDifferential(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not TOOL.exists():
            p = run(["cargo", "build", "--offline", "--release"], cwd=ROOT,
                    env=dict(os.environ, CARGO_NET_OFFLINE="true",
                             DQ8_RUN_GUEST_THREADS="1"), timeout=600)
            if p.returncode:
                raise AssertionError(p.stderr)
        cls.tmp = tempfile.TemporaryDirectory(prefix="dq8-inline-memory-")
        cls.work = Path(cls.tmp.name)
        code = cls.work / "memory.code"
        code.write_bytes(CODE)
        raw = cls.work / "memory.raw.c"
        p = run([TOOL, "recompile-arm", code, hex(BASE), raw, "--base", hex(BASE)])
        if p.returncode:
            raise AssertionError(p.stderr)
        text = raw.read_text()
        marker = "int main(int argc, char **argv)"
        if marker not in text:
            raise AssertionError("generated main() not found")
        cls.source = cls.work / "memory.c"
        cls.source.write_text(text.replace(marker,
                              "int original_main(int argc, char **argv)", 1)
                              + FIXTURE)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def build(self, opt, split, disable_fastpath=False):
        env = dict(os.environ)
        if disable_fastpath:
            env["DQ8_DISABLE_MEMORY_FASTPATH"] = "1"
        name = "%s-%s-%s" % (self.source.stem, opt[1:],
                             "split-nofast" if disable_fastpath else
                             ("split" if split else "single"))
        exe = self.work / name
        # Same platform gates tools/build_split.sh uses: the generated prelude
        # needs -msse4.1 (some pica_vs declarations sit inside
        # #if defined(__SSE4_1__)) and the GL/SDL sources when the runtime links
        # them.  Without these the reference arm cannot compile at all (t_221fd585).
        platform = PLATFORM_FLAGS + PLATFORM_LIBS
        if split:
            out = self.work / (name + ".d")
            p = run(["python3", SPLITTER, self.source, out], env=env)
            self.assertEqual(p.returncode, 0, p.stderr)
            sources = sorted(out.glob("*.c"))
            p = run(["clang", "-std=c11", opt, "-w", "-I" + str(out),
                     *sources, *platform, "-o", exe], timeout=180)
        else:
            p = run(["clang", "-std=c11", opt, "-w", self.source,
                     *platform, "-o", exe], timeout=180)
        self.assertEqual(p.returncode, 0, p.stderr)
        return exe

    def reference(self, opt):
        """Pre-change arm: the single TU (no splitter, original memNN/putNN
        helper calls).  If this environment cannot compile it at all, fall back
        to the splitter's documented control knob, which generates the same
        guest cases with the original out-of-line helper calls."""
        try:
            return self.build(opt, False)
        except AssertionError:
            return self.build(opt, True, disable_fastpath=True)

    def test_fast_path_matches_reference_and_faults(self):
        env = dict(os.environ, DQ8_RUN_GUEST_THREADS="1")
        for opt in ("-O0", "-O2"):
            reference = self.reference(opt)
            fast = self.build(opt, True)
            ref = run([reference], env=env, timeout=60)
            got = run([fast], env=env, timeout=60)
            self.assertEqual(ref.returncode, 0, ref.stderr)
            self.assertEqual(got.returncode, 0, got.stderr)
            self.assertEqual(got.stdout, ref.stdout,
                             "memory state differs at %s" % opt)
            self.assertIn("writes=3", got.stdout)
            self.assertIn("hidpcs=4", got.stdout)
            bad_env = dict(env, DQ8_MEM_TEST_BAD="1")
            ref_bad = run([reference], env=bad_env, timeout=20)
            got_bad = run([fast], env=bad_env, timeout=20)
            self.assertEqual((ref_bad.returncode, ref_bad.stderr),
                             (got_bad.returncode, got_bad.stderr),
                             "fault path differs at %s" % opt)
            self.assertEqual(ref_bad.returncode, 16)


if __name__ == "__main__":
    unittest.main()
