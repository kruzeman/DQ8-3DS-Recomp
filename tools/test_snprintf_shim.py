#!/usr/bin/env python3
"""Regression tests for the guest snprintf shim in src/main.rs.

The shim lives inside the emitted runtime (the big raw string in src/main.rs),
so the test extracts `guest_memory_cstr` + `guest_host_snprintf` verbatim,
compiles them against tiny guest-RAM stubs and checks that printf flags, width
and precision survive - above all the dialog's `TextBox_%02d_%02d` zero padding.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
START = "static int guest_memory_cstr(uint32_t va, char *out, unsigned cap) {"
# The extraction stops at the A05 boundary predicate: everything from here
# (dq8_pc_needs_boundary through translated_from) is dispatch-boundary
# machinery that depends on runtime globals the shim test does not stub.
END = "static int dq8_pc_needs_boundary("

header = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define GUEST_RAM_SIZE 4096u
static uint8_t ram[GUEST_RAM_SIZE];

static int guest_memory_range(uint32_t va, uint64_t n, uint32_t *mapped) {
    if (n > GUEST_RAM_SIZE || va > GUEST_RAM_SIZE - n) return 0;
    *mapped = va;
    return 1;
}
static uint32_t mem32(uint32_t a) {
    uint32_t v = 0;
    assert(a + 4u <= GUEST_RAM_SIZE);
    memcpy(&v, ram + a, 4);
    return v;
}
static void put_word(uint32_t a, uint32_t v) { memcpy(ram + a, &v, 4); }
static void put_str(uint32_t a, const char *s) { memcpy(ram + a, s, strlen(s) + 1); }
'''

checks = r'''
/* Format `fmt_text` with the guest word arguments in `args` and return the
   shim's character count, leaving the NUL-terminated result in `buf`. */
static int run_fmt(const char *fmt_text, const uint32_t *args, unsigned nargs,
                   char *buf, unsigned bufsize) {
    uint32_t r[16];
    assert(bufsize >= 64);
    memset(r, 0, sizeof r);
    memset(ram + 0x200, 0xEE, 64);
    put_str(0x40, fmt_text);
    for (unsigned i = 0; i < nargs; i++) put_word(0x300 + i * 4u, args[i]);
    r[0] = 0x200;   /* dest */
    r[1] = 64;      /* size */
    r[2] = 64;      /* maxlen */
    r[3] = 0x40;    /* format */
    r[13] = 0x300;  /* sp: variadic argument words */
    int n = guest_host_snprintf(r);
    memcpy(buf, ram + 0x200, 64);
    buf[63] = 0;
    return n;
}

static void expect(const char *fmt_text, const uint32_t *args, unsigned nargs,
                   const char *want) {
    char buf[64];
    int n = run_fmt(fmt_text, args, nargs, buf, sizeof buf);
    if (n != (int)strlen(want) || strcmp(buf, want) != 0) {
        fprintf(stderr, "snprintf shim: fmt=\"%s\" -> \"%s\" (n=%d), want \"%s\" (n=%d)\n",
                fmt_text, buf, n, want, (int)strlen(want));
        assert(0 && "snprintf shim output mismatch");
    }
}

int main(void) {
    /* --- zero padding: the save dialog's child-lookup names ------------- */
    { uint32_t a[] = {0, 0};
      expect("TextBox_%02d_%02d", a, 2, "TextBox_00_00"); }
    { uint32_t a[] = {4, 13};
      expect("TextBox_%02d_%02d", a, 2, "TextBox_04_13"); }
    { uint32_t a[] = {5, 0};
      expect("SelectText_%02d", a, 1, "SelectText_05"); }
    { uint32_t a[] = {7};
      expect("%02d", a, 1, "07"); }
    { uint32_t a[] = {42};
      expect("%02d", a, 1, "42"); }
    { uint32_t a[] = {(uint32_t)-1};
      expect("%02d", a, 1, "-1"); }

    /* --- existing formatting must be unchanged -------------------------- */
    { uint32_t a[] = {5};
      expect("%d", a, 1, "5"); }
    { uint32_t a[] = {(uint32_t)-5};
      expect("%i", a, 1, "-5"); }
    { uint32_t a[] = {10};
      expect("%u", a, 1, "10"); }
    { uint32_t a[] = {0x1a, 0x1a, 10};
      expect("%x %X %u", a, 3, "1a 1A 10"); }
    { uint32_t a[] = {0xab};
      expect("%08x", a, 1, "000000ab"); }
    { uint32_t a[] = {0x1234};
      expect("%p", a, 1, "00001234"); }
    { uint32_t a[] = {'Q'};
      expect("[%c]", a, 1, "[Q]"); }
    { uint32_t a[] = {0};
      expect("%%", a, 0, "%"); }
    { uint32_t a[] = {0};
      expect("%q", a, 0, "%q"); }
    expect("plain text", NULL, 0, "plain text");

    /* string arguments, including the resource-path case from README.md */
    put_str(0x380, "anim");
    put_str(0x390, "walk");
    { uint32_t a[] = {0x380, 0x390};
      expect("%s/%s.bflan", a, 2, "anim/walk.bflan"); }
    { uint32_t a[] = {0x380};
      expect("%s.bflan", a, 1, "anim.bflan"); }

    /* --- width / precision beyond zero padding -------------------------- */
    { uint32_t a[] = {5};
      expect("%5d|", a, 1, "    5|"); }
    { uint32_t a[] = {5};
      expect("%-5d|", a, 1, "5    |"); }
    { uint32_t a[] = {5};
      expect("%.3d", a, 1, "005"); }

    /* --- bounds: an oversized spec must not overflow piece[] or out[] --- */
    { uint32_t a[] = {5};
      char buf[64];
      int n = run_fmt("%1000d", a, 1, buf, sizeof buf);
      assert(n == 63);
      for (int i = 0; i < 63; i++) assert(buf[i] == ' ');
      assert(buf[63] == 0); }

    /* long string argument: dest capacity still wins */
    char longstr[400];
    memset(longstr, 'a', sizeof longstr - 1);
    longstr[sizeof longstr - 1] = 0;
    put_str(0x380, longstr);   /* guest_memory_cstr caps at 255 chars */
    { uint32_t a[] = {0x380};
      char buf[64];
      int n = run_fmt("%s", a, 1, buf, sizeof buf);
      assert(n == 63);
      for (int i = 0; i < 63; i++) assert(buf[i] == 'a'); }

    return 0;
}
'''


def runtime_source() -> str:
    text = (ROOT / "src" / "main.rs").read_text()
    start = text.index(START)
    end = text.index(END, start)
    return text[start:end]


class SnprintfShimTest(unittest.TestCase):
    def test_zero_padding_and_existing_formatting(self):
        clang = shutil.which("clang")
        self.assertIsNotNone(clang, "clang is required for the shim test")
        runtime = runtime_source()
        self.assertIn("guest_host_snprintf", runtime)
        with tempfile.TemporaryDirectory(prefix="dq8-snprintf-") as d:
            src = Path(d) / "test.c"
            exe = Path(d) / "test"
            src.write_text(header + runtime + checks)
            subprocess.run(
                [clang, "-std=c11", "-O1", "-fsanitize=undefined,address",
                 str(src), "-lm", "-o", str(exe)],
                check=True,
            )
            env = dict(os.environ)
            env["ASAN_OPTIONS"] = env.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
            subprocess.run([str(exe)], check=True, timeout=10, env=env)


if __name__ == "__main__":
    unittest.main()
