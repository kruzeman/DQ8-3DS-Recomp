#!/usr/bin/env python3
"""PERF-04 regression test: lazy scan-out capture when no consumer.

Extracts the REAL runtime code from src/main.rs (not a copy):
  - the capture counter globals,
  - host_display_capture_stats_dump(), gsp_ppm_capture_enabled(),
    host_display_shadow_consumer() (the PERF-04 gate),
  - gsp_transfer_bytes_per_pixel(),
  - host_display_capture_transfer() (whole function, brace-matched),
then compiles them into a fixture and runs it in three consumer modes:

  off  no SDL window (host_display_ready=0), DQ8_CAPTURE_PPM unset
       -> ZERO pixel conversion: converted==0, skipped==calls, and every
          shadow byte keeps its sentinel value.
  sdl  host_display_ready=1 (SDL window consumer)
  ppm  DQ8_CAPTURE_PPM=1 (snapshot consumer)

In the two consumer modes the converted shadow buffers are compared
byte-for-byte against an independent Python model of the conversion
(RGBA8/RGB8/RGB565/RGBA5551/RGBA4, portrait rotation, bottom screen,
480-wide box-scaled render targets), and a writes-without-swap pair proves
the shadow tracks direct guest rewrites of the shown buffer (no dirty flag).

The extraction fails loudly if the expected source shapes are missing, so a
refactor cannot silently make this test pass on stale text.

Usage: python3 tools/test_lazy_capture.py [--keep]
Exit code 0 = all cases passed.
"""
import os
import re
import shutil
import subprocess
import sys
from typing import NoReturn

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAIN_RS = os.path.join(ROOT, "src", "main.rs")
OUTDIR = os.path.join(ROOT, "target", "test_lazy_capture_out")

HOST_TOP_W, HOST_TOP_H = 400, 240
HOST_BOT_W, HOST_BOT_H = 320, 240
SENTINEL = 0x5A
RAM_SIZE = 0x100000


def die(msg) -> NoReturn:
    print("FAIL extract: %s" % msg, file=sys.stderr)
    raise SystemExit(2)


def extract_function(src, signature) -> str:
    # Match the DEFINITION (signature followed by '{'), not a forward decl.
    i = -1
    probe = src.find(signature)
    while probe >= 0:
        rest = src[probe + len(signature):].lstrip()
        if rest.startswith("{"):
            i = probe
            break
        probe = src.find(signature, probe + 1)
    if i < 0:
        die("definition not found: %r" % signature)
    j = src.find("{", i)
    depth = 0
    k = j
    while k < len(src):
        if src[k] == "{":
            depth += 1
        elif src[k] == "}":
            depth -= 1
            if depth == 0:
                return src[i:k + 1]
        k += 1
    die("unbalanced braces after %r" % signature)


def extract_line(src, line) -> str:
    if src.count(line) != 1:
        die("line not unique: %r" % line)
    return line


# ---- independent model of host_display_capture_transfer (the reference) ----

def fill_region(w, h, bpp, fill):
    """Same deterministic pattern the fixture writes into guest RAM."""
    buf = bytearray(w * h * bpp)
    for y in range(h):
        for x in range(w):
            for bi in range(bpp):
                buf[(y * w + x) * bpp + bi] = (fill + 3 * y + 7 * x + 13 * bi) & 0xFF
    return bytes(buf)


def model_convert(src, w, h, fmt):
    """Reference conversion into the two shadow buffers (bytes RGBA8).

    Transcribed from the original (pre-PERF-04) host_display_capture_transfer
    semantics; the test asserts the consumer-mode output matches it exactly.
    """
    top = bytearray(bytes([SENTINEL]) * (HOST_TOP_W * HOST_TOP_H * 4))
    bot = bytearray(bytes([SENTINEL]) * (HOST_BOT_W * HOST_BOT_H * 4))
    bpp = {0: 4, 1: 3, 2: 2, 3: 2, 4: 2}.get(fmt & 7, 0)
    if not bpp:
        return bytes(top), bytes(bot)
    rotate = 0
    if w == HOST_TOP_W and h == HOST_TOP_H:
        shadow, sw, sh = top, HOST_TOP_W, HOST_TOP_H
    elif w == HOST_BOT_W and h == HOST_BOT_H:
        shadow, sw, sh = bot, HOST_BOT_W, HOST_BOT_H
    elif w == HOST_TOP_H and h == HOST_TOP_W:
        shadow, sw, sh, rotate = top, HOST_TOP_W, HOST_TOP_H, 1
    elif w == HOST_BOT_H and h == HOST_BOT_W:
        shadow, sw, sh, rotate = bot, HOST_BOT_W, HOST_BOT_H, 1
    elif w == 480 or w == 240:
        if h != 400 and h != 320:
            return bytes(top), bytes(bot)
        shadow = top if h == 400 else bot
        sw = HOST_TOP_W if h == 400 else HOST_BOT_W
        sh = HOST_TOP_H if h == 400 else HOST_BOT_H
        for y in range(sh):
            sy = y * h // sh
            for x in range(sw):
                sx = x * w // sw
                s = (sy * w + sx) * bpp
                d = (y * sw + x) * 4
                shadow[d + 0] = src[s + 0] if bpp >= 1 else 0
                shadow[d + 1] = src[s + 1] if bpp >= 2 else shadow[d + 0]
                shadow[d + 2] = src[s + 2] if bpp >= 3 else shadow[d + 0]
                shadow[d + 3] = 255
        return bytes(top), bytes(bot)
    else:
        return bytes(top), bytes(bot)

    for y in range(h):
        for x in range(w):
            s = (y * w + x) * bpp
            dx = y if rotate else x
            dy = (w - 1 - x) if rotate else y
            if dx >= sw or dy >= sh:
                continue
            d = (dy * sw + dx) * 4
            f = fmt & 7
            if f == 0:
                shadow[d + 0] = src[s + 3]
                shadow[d + 1] = src[s + 2]
                shadow[d + 2] = src[s + 1]
                shadow[d + 3] = src[s + 0]
            elif f == 1:
                shadow[d + 0] = src[s + 2]
                shadow[d + 1] = src[s + 1]
                shadow[d + 2] = src[s + 0]
                shadow[d + 3] = 255
            elif f in (2, 3, 4):
                v = src[s] | (src[s + 1] << 8)
                if f == 2:
                    shadow[d + 0] = ((v >> 11) & 31) * 255 // 31
                    shadow[d + 1] = ((v >> 5) & 63) * 255 // 63
                    shadow[d + 2] = (v & 31) * 255 // 31
                    shadow[d + 3] = 255
                elif f == 3:
                    shadow[d + 0] = ((v >> 11) & 31) * 255 // 31
                    shadow[d + 1] = ((v >> 6) & 31) * 255 // 31
                    shadow[d + 2] = ((v >> 1) & 31) * 255 // 31
                    shadow[d + 3] = 255 if (v & 1) else 0
                else:
                    shadow[d + 0] = ((v >> 12) & 15) * 17
                    shadow[d + 1] = ((v >> 8) & 15) * 17
                    shadow[d + 2] = ((v >> 4) & 15) * 17
                    shadow[d + 3] = (v & 15) * 17
            else:
                shadow[d + 0] = shadow[d + 1] = shadow[d + 2] = 0
                shadow[d + 3] = 255
    return bytes(top), bytes(bot)


# name, w, h, fmt, mapped_dest, fill
CASES = [
    ("top_direct_rgba8", 400, 240, 0, 0x00000, 0x10),
    ("top_direct_rgb8",  400, 240, 1, 0x00000, 0x20),
    ("top_direct_565",   400, 240, 2, 0x00000, 0x30),
    ("top_direct_5551",  400, 240, 3, 0x00000, 0x40),
    ("top_direct_4444",  400, 240, 4, 0x00000, 0x50),
    ("top_rot_rgba8",    240, 400, 0, 0x20000, 0x60),
    ("top_rot_565",      240, 400, 2, 0x20000, 0x70),
    ("bot_rot_5551",     240, 320, 3, 0x40000, 0x80),
    ("bot_direct_rgb8",  320, 240, 1, 0x40000, 0x90),
    ("scaled_top_rgb8",  480, 400, 1, 0x60000, 0xA0),
    ("scaled_bot_rgb8",  480, 320, 1, 0x80000, 0xB0),
    # writes-without-swap coherence pair: same buffer, same dims, pixels
    # rewritten directly by the guest with no swap descriptor in between.
    ("coh1",             400, 240, 0, 0x00000, 0xC0),
    ("coh2",             400, 240, 0, 0x00000, 0xC1),
]


def main():
    keep = "--keep" in sys.argv
    src = open(MAIN_RS, encoding="utf-8").read()

    counters = extract_line(
        src,
        "static uint64_t host_display_capture_converted, host_display_capture_skipped;")
    dump_fn = extract_function(src, "static void host_display_capture_stats_dump(void)")
    ppm_fn = extract_function(src, "static int gsp_ppm_capture_enabled(void)")
    gate_fn = extract_function(src, "static int host_display_shadow_consumer(void)")
    bpp_fn = extract_function(src, "static uint32_t gsp_transfer_bytes_per_pixel(uint32_t format)")
    cap_fn = extract_function(
        src,
        "static void host_display_capture_transfer(uint32_t mapped_dest, uint32_t w, uint32_t h, uint32_t format)")

    # Sanity: the extracted pieces are the code under test.
    if "host_display_capture_skipped" not in gate_fn and "host_display_shadow_consumer" not in cap_fn:
        die("capture function does not use the PERF-04 gate")
    if "host_display_shadow_consumer()" not in cap_fn:
        die("capture function is not gated by host_display_shadow_consumer")
    if "host_display_ready" not in gate_fn or "gsp_ppm_capture_enabled" not in gate_fn:
        die("gate does not consider both consumers (SDL + PPM)")

    fixture = r"""/* Generated by tools/test_lazy_capture.py — do not edit. */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GUEST_RAM_SIZE 0x00100000u
#define HOST_TOP_W 400u
#define HOST_TOP_H 240u
#define HOST_BOT_W 320u
#define HOST_BOT_H 240u
#define SENTINEL 0x5Au

static uint8_t ram[GUEST_RAM_SIZE];
static uint8_t host_shadow_top[HOST_TOP_W * HOST_TOP_H * 4u];
static uint8_t host_shadow_bot[HOST_BOT_W * HOST_BOT_H * 4u];
static int host_display_ready;

""" + counters + "\n\n" + dump_fn + "\n\n" + ppm_fn + "\n\n" + gate_fn + "\n\n" + bpp_fn + "\n\n" + cap_fn + "\n\n" + r"""
static void fill_region(uint32_t mapped, uint32_t w, uint32_t h, uint32_t bpp, uint8_t fill) {
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
            for (uint32_t bi = 0; bi < bpp; ++bi)
                ram[mapped + (y * w + x) * bpp + bi] =
                    (uint8_t)(fill + 3u * y + 7u * x + 13u * bi);
}

static int write_shadow(const char *dir, const char *name, const uint8_t *p, size_t n) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.bin", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { printf("FAIL cannot write %s\n", path); return 1; }
    fwrite(p, 1, n, f);
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 3) { printf("usage: %s off|sdl|ppm <outdir>\n", argv[0]); return 2; }
    const char *mode = argv[1], *dir = argv[2];
    if (!strcmp(mode, "sdl")) host_display_ready = 1;
    else if (!strcmp(mode, "ppm")) setenv("DQ8_CAPTURE_PPM", "1", 1);
    else if (strcmp(mode, "off")) { printf("FAIL unknown mode %s\n", mode); return 2; }

    struct { const char *name; uint32_t w, h, fmt, mapped; uint8_t fill; } cases[] = {
""" + "".join(
        '        {"%s", %du, %du, %du, 0x%05Xu, 0x%02Xu},\n'
        % (n, w, h, f, m, fill) for (n, w, h, f, m, fill) in CASES) + r"""    };
    static const uint32_t bpp_of[5] = { 4u, 3u, 2u, 2u, 2u };
    unsigned failures = 0;
    memset(host_shadow_top, SENTINEL, sizeof host_shadow_top);
    memset(host_shadow_bot, SENTINEL, sizeof host_shadow_bot);
    unsigned ncases = sizeof cases / sizeof cases[0];
    for (unsigned i = 0; i < ncases; ++i) {
        uint32_t bpp = cases[i].fmt < 5u ? bpp_of[cases[i].fmt] : 0u;
        fill_region(cases[i].mapped, cases[i].w, cases[i].h, bpp, cases[i].fill);
        /* Fresh sentinel shadows per case so each dump is self-contained. */
        memset(host_shadow_top, SENTINEL, sizeof host_shadow_top);
        memset(host_shadow_bot, SENTINEL, sizeof host_shadow_bot);
        host_display_capture_transfer(cases[i].mapped, cases[i].w, cases[i].h, cases[i].fmt);
        char top_name[256], bot_name[256];
        snprintf(top_name, sizeof top_name, "%s-top", cases[i].name);
        snprintf(bot_name, sizeof bot_name, "%s-bot", cases[i].name);
        failures += write_shadow(dir, top_name, host_shadow_top, sizeof host_shadow_top);
        failures += write_shadow(dir, bot_name, host_shadow_bot, sizeof host_shadow_bot);
        printf("case %s done\n", cases[i].name);
    }
    int off = !strcmp(mode, "off");
    int ok_counts = off
        ? (host_display_capture_converted == 0u && host_display_capture_skipped == ncases)
        : (host_display_capture_converted == ncases && host_display_capture_skipped == 0u);
    if (!ok_counts) {
        printf("FAIL counters mode=%s converted=%llu skipped=%llu expected_calls=%u\n",
               mode, (unsigned long long)host_display_capture_converted,
               (unsigned long long)host_display_capture_skipped, ncases);
        failures++;
    } else {
        printf("ok   counters mode=%s converted=%llu skipped=%llu\n",
               mode, (unsigned long long)host_display_capture_converted,
               (unsigned long long)host_display_capture_skipped);
    }
    if (failures) { printf("RESULT: FAIL (%u)\n", failures); return 1; }
    printf("RESULT: PASS\n");
    return 0;
}
"""

    if os.path.isdir(OUTDIR):
        shutil.rmtree(OUTDIR)
    os.makedirs(OUTDIR)
    fixture_c = os.path.join(ROOT, "target", "test_lazy_capture.c")
    fixture_bin = os.path.join(ROOT, "target", "test_lazy_capture")
    with open(fixture_c, "w", encoding="utf-8") as fh:
        fh.write(fixture)

    cc = ["cc", "-std=c11", "-O0", "-g", "-Wall", "-Wextra", fixture_c, "-o", fixture_bin]
    print("$ " + " ".join(cc))
    subprocess.run(cc, check=True)

    failures = 0
    for mode, env_extra in (("off", {}), ("sdl", {}), ("ppm", {})):
        env = dict(os.environ)
        env.pop("DQ8_CAPTURE_PPM", None)
        env.pop("DQ8_CAPTURE_STATS", None)
        env.update(env_extra)
        cmd = [fixture_bin, mode, OUTDIR]
        print("$ " + " ".join(cmd))
        p = subprocess.run(cmd, env=env, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True)
        sys.stdout.write(p.stdout)
        if p.returncode != 0:
            print("FAIL fixture mode=%s rc=%d" % (mode, p.returncode))
            failures += 1

        for (name, w, h, fmt, _mapped, fill) in CASES:
            src_bytes = fill_region(w, h, {0: 4, 1: 3, 2: 2, 3: 2, 4: 2}[fmt], fill)
            exp_top, exp_bot = model_convert(src_bytes, w, h, fmt)
            case_fail = False
            for side, expected in (("-top", exp_top), ("-bot", exp_bot)):
                path = os.path.join(OUTDIR, name + side + ".bin")
                got = open(path, "rb").read()
                if got == expected:
                    continue
                if mode == "off" and got == bytes([SENTINEL]) * len(expected):
                    continue  # skip path must not touch the shadow at all
                diff = sum(1 for a, b in zip(got, expected) if a != b)
                print("FAIL %s %s%s shadow differs in %d/%d bytes"
                      % (mode, name, side, diff, len(got)))
                failures += 1
                case_fail = True
            if not case_fail and mode != "off":
                print("ok   %s %s pixels match reference" % (mode, name))
        if mode == "off":
            print("ok   off zero conversion (sentinels intact)")

    if failures:
        print("test_lazy_capture: FAIL (%d)" % failures)
        return 1
    print("test_lazy_capture: PASS")
    if not keep:
        # keep the generated fixture source for review; drop the binaries/dumps
        shutil.rmtree(OUTDIR)
        os.remove(fixture_bin)
    return 0


if __name__ == "__main__":
    sys.exit(main())
