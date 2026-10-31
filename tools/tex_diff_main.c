/* DQ8 sampler differential test driver.
 *
 * Compares OUR texture sampler (pica_sample_tex0, extracted verbatim from
 * src/main.rs by tools/test_tex_decode.py and #included below) pixel-for-pixel
 * against the LOCAL azahar-master reference decoder (tools/azahar_ref.cpp,
 * compiled from azahar-master/src/video_core/texture/*.cpp).
 *
 * Suites:
 *   1. decoder matrix: formats 0-13 x sizes {8x8,16x16,32x32,64x8,8x64,
 *      NPOT 12x10} x 2 PRNG seeds, every texel compared.  (PICA texture
 *      memory is Morton 8x8 tiled in every format; azahar defines no linear
 *      texture layout, so "linear tiling" is N/A for the tex0 contract.)
 *   2. wrap/border/edge: all 8 PICA wrap modes on both axes (plus mixed
 *      pairs), two border colors, coordinate grid k/16 covering negative,
 *      in-range, edge and out-of-range positions, on 16x16 textures for
 *      every format.  Coordinates are k/16 so u*width is exact in both the
 *      f32 driver math and azahar's f24 rasterizer math.
 *
 * Exit code 0 = every compared sample matches the azahar reference.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GUEST_RAM_SIZE 0x400000u
static uint8_t ram[GUEST_RAM_SIZE];
static uint32_t pica_regs[0x300];
static uint32_t guest_memory_address(uint32_t paddr) { return paddr; }
static int guest_memory_range(uint32_t va, uint32_t size, uint32_t *mapped) {
    if ((uint64_t)va + size > GUEST_RAM_SIZE) return 0;
    *mapped = va;
    return 1;
}

#include "sampler_extracted.c"

extern void az_lookup_texel(const uint8_t *data, uint32_t w, uint32_t h,
                            uint32_t fmt, uint32_t x, uint32_t y,
                            uint8_t out[4]);
extern void az_sample_uv(const uint8_t *data, uint32_t w, uint32_t h,
                         uint32_t fmt, uint32_t wrap_s, uint32_t wrap_t,
                         uint32_t border_rgba, float uf, float vf,
                         uint8_t out[4]);

#define TEX_BASE 0x1000u

static uint32_t rng_state;
static uint32_t rng_next(void) {
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return rng_state = x;
}

/* Azahar CalculateTileSize (texture_decode.cpp:22) — for buffer sizing only. */
static uint32_t tile_size_of(uint32_t fmt) {
    switch (fmt) {
    case 0u: return 4u * 64u;
    case 1u: return 3u * 64u;
    case 2u: case 3u: case 4u: case 5u: case 6u: return 2u * 64u;
    case 7u: case 8u: case 9u: return 64u;
    case 10u: case 11u: return 32u;
    case 12u: return 32u;
    case 13u: return 64u;
    default: return 0u;
    }
}

static void setup_tex(uint32_t w, uint32_t h, uint32_t fmt,
                      uint32_t wrap_s, uint32_t wrap_t, uint32_t border) {
    memset(pica_regs, 0, sizeof pica_regs);
    pica_regs[0x85] = (TEX_BASE / 8u) & 0x0FFFFFFFu;       /* TEXUNIT0_ADDR1 */
    pica_regs[0x82] = ((w & 0x7FFu) << 16) | (h & 0x7FFu); /* TEXUNIT0_DIM */
    pica_regs[0x8E] = fmt & 0xFu;                          /* TEXUNIT0_TYPE */
    /* TEXUNIT0_PARAM: wrap_t bits 8-10, wrap_s bits 12-14 (regs_texturing.h). */
    pica_regs[0x83] = ((wrap_s & 7u) << 12) | ((wrap_t & 7u) << 8);
    /* TEXUNIT0_BORDER_COLOR: r 0-7, g 8-15, b 16-23, a 24-31. */
    pica_regs[0x81] = border;
}

static void fill_texture(uint32_t bytes, uint32_t seed) {
    uint8_t *p = ram + TEX_BASE;
    rng_state = seed ? seed : 1u;
    for (uint32_t i = 0; i < bytes; ++i) p[i] = (uint8_t)rng_next();
}

static unsigned long samples_run, mismatches;
static unsigned long fmt_mm[14];      /* mismatches per format, decoder suite */
static unsigned long wrap_mm[14][8];  /* mismatches per format x wrap_s, wrap suite */
static unsigned shown;

static void mismatch(const char *suite, uint32_t fmt, uint32_t w, uint32_t h,
                     uint32_t ws, uint32_t wt, uint32_t border,
                     long x, long y, float u, float v,
                     const uint8_t ref[4], const uint8_t got[4], int got_ok) {
    ++mismatches;
    if (shown < 24u) {
        ++shown;
        fprintf(stderr,
                "MISMATCH %s fmt=%u %ux%u wrap=%u/%u border=%08X texel=(%ld,%ld) "
                "uv=(%.5f,%.5f) az=%02X/%02X/%02X/%02X our=%02X/%02X/%02X/%02X%s\n",
                suite, fmt, w, h, ws, wt, border, x, y, (double)u, (double)v,
                ref[0], ref[1], ref[2], ref[3], got[0], got[1], got[2], got[3],
                got_ok ? "" : " (sampler returned FAIL)");
    }
}

/* Suite 1: every texel of every format/size/seed vs az_lookup_texel. */
static void run_decoder_matrix(void) {
    static const uint32_t sizes[][2] = {
        {8, 8}, {16, 16}, {32, 32}, {64, 8}, {8, 64}, {12, 10},
    };
    static const uint32_t seeds[2] = {0xA5A5A5A5u, 0x12345678u};
    for (uint32_t fmt = 0; fmt <= 13; ++fmt) {
        shown = 0; /* cap mismatch details per format */
        for (unsigned si = 0; si < sizeof sizes / sizeof sizes[0]; ++si) {
            uint32_t w = sizes[si][0], h = sizes[si][1];
            uint32_t bytes = tile_size_of(fmt) * ((h / 8u) + 2u) * ((w / 8u) + 2u) + 4096u;
            for (unsigned ki = 0; ki < sizeof seeds / sizeof seeds[0]; ++ki) {
                fill_texture(bytes, seeds[ki]);
                setup_tex(w, h, fmt, 0, 0, 0);
                for (uint32_t y = 0; y < h; ++y) {
                    for (uint32_t x = 0; x < w; ++x) {
                        uint8_t ref[4], got[4] = {0, 0, 0, 0};
                        az_lookup_texel(ram + TEX_BASE, w, h, fmt, x, y, ref);
                        /* Our sampler flips v internally (y = h-1-(s32)(v*h)),
                           so layout row y is reached with v=(h-1-y+0.5)/h. */
                        float u = ((float)x + 0.5f) / (float)w;
                        float v = ((float)(h - 1u - y) + 0.5f) / (float)h;
                        int ok = pica_sample_tex0(u, v, &got[0], &got[1],
                                                  &got[2], &got[3]);
                        ++samples_run;
                        if (!ok || memcmp(ref, got, 4) != 0) {
                            if (fmt < 14u) ++fmt_mm[fmt];
                            mismatch("decoder", fmt, w, h, 0, 0, 0, x, y, u, v,
                                     ref, got, ok);
                        }
                    }
                }
            }
        }
    }
}

/* Suite 2: wrap modes, border colour, edge coordinates. */
static void run_wrap_matrix(void) {
    static const uint32_t w = 16, h = 16;
    static const uint32_t mode_pairs[][2] = {
        {0, 0}, {1, 1}, {2, 2}, {3, 3}, {4, 4}, {5, 5}, {6, 6}, {7, 7},
        {0, 2}, {2, 0}, {1, 2}, {2, 1}, {3, 1}, {1, 3},
    };
    static const uint32_t borders[2] = {0x80402010u, 0xFF00FF7Fu};
    /* k/16 grid: exact in f32 and f24, covers negative / edge / beyond. */
    static const int kvals[] = {-32, -31, -17, -16, -15, -9, -8, -1,
                                0,   1,   7,   8,   15,  16, 17, 31, 32, 47};
    /* Extra exact half-step edge coords (n/32 — also exact in f24). */
    static const float extra[] = {-0.5f / 16.0f, 0.5f / 16.0f,
                                  15.5f / 16.0f, 16.5f / 16.0f,
                                  -16.5f / 16.0f, 32.5f / 16.0f};
    uint32_t coords[32];
    unsigned ncoords = 0;
    for (unsigned i = 0; i < sizeof kvals / sizeof kvals[0]; ++i) {
        float f = (float)kvals[i] / 16.0f;
        uint32_t bits;
        memcpy(&bits, &f, 4);
        coords[ncoords++] = bits;
    }
    for (unsigned i = 0; i < sizeof extra / sizeof extra[0]; ++i) {
        uint32_t bits;
        memcpy(&bits, &extra[i], 4);
        coords[ncoords++] = bits;
    }

    uint32_t bytes = tile_size_of(0) * 4u * 4u + 4096u;
    for (uint32_t fmt = 0; fmt <= 13; ++fmt) {
        shown = 0; /* cap mismatch details per format */
        fill_texture(bytes, 0xC0FFEEu + fmt);
        for (unsigned mi = 0; mi < sizeof mode_pairs / sizeof mode_pairs[0]; ++mi) {
            uint32_t ws = mode_pairs[mi][0], wt = mode_pairs[mi][1];
            for (unsigned bi = 0; bi < sizeof borders / sizeof borders[0]; ++bi) {
                uint32_t border = borders[bi];
                setup_tex(w, h, fmt, ws, wt, border);
                for (unsigned ui = 0; ui < ncoords; ++ui) {
                    for (unsigned vi = 0; vi < ncoords; ++vi) {
                        float u, v;
                        memcpy(&u, &coords[ui], 4);
                        memcpy(&v, &coords[vi], 4);
                        uint8_t ref[4], got[4] = {0, 0, 0, 0};
                        az_sample_uv(ram + TEX_BASE, w, h, fmt, ws, wt, border,
                                     u, v, ref);
                        int ok = pica_sample_tex0(u, v, &got[0], &got[1],
                                                  &got[2], &got[3]);
                        ++samples_run;
                        if (!ok || memcmp(ref, got, 4) != 0) {
                            if (fmt < 14u && ws < 8u) ++wrap_mm[fmt][ws];
                            mismatch("wrap", fmt, w, h, ws, wt, border, -1, -1,
                                     u, v, ref, got, ok);
                        }
                    }
                }
            }
        }
    }
}

static unsigned long wrap_mm_total(uint32_t fmt) {
    unsigned long sum = 0;
    for (uint32_t m = 0; m < 8; ++m) sum += wrap_mm[fmt][m];
    return sum;
}

int main(int argc, char **argv) {
    int do_wrap = 1;
    if (argc > 1 && strcmp(argv[1], "--decoder") == 0) do_wrap = 0;
    run_decoder_matrix();
    if (do_wrap) run_wrap_matrix();
    for (uint32_t f = 0; f < 14; ++f)
        if (fmt_mm[f] || wrap_mm_total(f))
            printf("  fmt %2u: decoder_mm=%lu wrap_mm=%lu\n", f, fmt_mm[f],
                   wrap_mm_total(f));
    printf("tex_diff: samples=%lu mismatches=%lu\n", samples_run, mismatches);
    return mismatches ? 1 : 0;
}
