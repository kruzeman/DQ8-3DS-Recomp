/* tools/present_kernel_verify.c -- pixel-identity test for the composed
   present kernel (t_5286064e).
 *
 * The kernel under test is NOT copied here: tools/present_kernel_test.py
 * extracts it verbatim from src/main.rs (the runtime template) into
 * present_kernel_extracted.c, so this test always validates the shipped code
 * and cannot drift from it.  That includes host_present_scaler_mode(), so the
 * real DQ8_PRESENT_SCALER knob selects the kernel: the driver script runs this
 * binary once with the knob unset (SSE2) and once with =2 (scalar).
 *
 * For every case it renders a 400x240 and a 320x240 source into an 800x960
 * destination twice:
 *   reference = SDL_FillRect + 2x SDL_BlitScaled              (frozen path)
 *   candidate = host_present_fill_black_rect + host_present_scale2 (composed)
 * and compares the destination images byte for byte, for both destination
 * formats the presenter accepts and for opaque, random-alpha and 0/255-alpha
 * sources.
 *
 * exit 0 = every case byte-identical, 1 = a mismatch, 2 = setup failure.
 */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "present_kernel_extracted.c"

#define TW 400
#define TH 240
#define BW 320
#define BH 240
#define W (TW * 2)
#define H 960

static unsigned char top[TW * TH * 4], bot[BW * BH * 4];

static unsigned long long fnv(const unsigned char *p, size_t n) {
    unsigned long long h = 14695981039346656037ull;
    while (n--) { h ^= *p++; h *= 1099511628211ull; }
    return h;
}
static unsigned rng_state;
static unsigned rng(void) {
    rng_state = rng_state * 1103515245u + 12345u;
    return (rng_state >> 8) & 0x00FFFFFFu;
}
static void gen(int alpha_mode) {
    for (int y = 0; y < TH; ++y)
        for (int x = 0; x < TW; ++x) {
            unsigned char *p = top + ((size_t)y * TW + x) * 4;
            unsigned r = rng();
            p[0] = (unsigned char)r; p[1] = (unsigned char)(r >> 8); p[2] = (unsigned char)(r >> 16);
            p[3] = alpha_mode == 0 ? 255
                 : alpha_mode == 1 ? (unsigned char)rng()
                 : (unsigned char)(rng() & 1 ? 255 : rng());
        }
    for (int y = 0; y < BH; ++y)
        for (int x = 0; x < BW; ++x) {
            unsigned char *p = bot + ((size_t)y * BW + x) * 4;
            unsigned r = rng();
            p[0] = (unsigned char)r; p[1] = (unsigned char)(r >> 8); p[2] = (unsigned char)(r >> 16);
            p[3] = alpha_mode == 0 ? 255
                 : alpha_mode == 1 ? (unsigned char)rng()
                 : (unsigned char)(rng() & 1 ? 255 : rng());
        }
}

/* destination formats: RGB888 (B,G,R,0x00 in memory) = HP_KIND_BGRX,
   ABGR8888 (R,G,B,A in memory) = HP_KIND_RGBA_A.  These are the two the
   runtime's dst-kind check accepts for a window/SHM surface. */
static int run_case(int kind, int alpha_mode) {
    Uint32 fmt = kind == HP_KIND_BGRX ? SDL_PIXELFORMAT_RGB888
                                      : SDL_PIXELFORMAT_ABGR8888;
    const char *label = alpha_mode == 0 ? "opaque"
                     : alpha_mode == 1 ? "random-alpha" : "0/255-alpha";
    rng_state = 12345u + (unsigned)kind * 7u + (unsigned)alpha_mode;
    gen(alpha_mode);
    SDL_Surface *st = SDL_CreateRGBSurfaceFrom(top, TW, TH, 32, TW * 4,
        0x000000FFu, 0x0000FF00u, 0x00FF0000u, 0xFF000000u);
    SDL_Surface *sb = SDL_CreateRGBSurfaceFrom(bot, BW, BH, 32, BW * 4,
        0x000000FFu, 0x0000FF00u, 0x00FF0000u, 0xFF000000u);
    SDL_Surface *ref = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, fmt);
    SDL_Surface *got = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, fmt);
    if (!st || !sb || !ref || !got) {
        printf("  %-13s SURFACE FAIL: %s\n", label, SDL_GetError());
        return 1;
    }
    SDL_FillRect(ref, NULL, SDL_MapRGB(ref->format, 0, 0, 0));
    SDL_Rect rtop = { 0, 0, TW * 2, TH * 2 };
    SDL_Rect rbot = { (TW - BW) / 2 * 2, TH * 2, BW * 2, BH * 2 };
    SDL_BlitScaled(st, NULL, ref, &rtop);
    SDL_BlitScaled(sb, NULL, ref, &rbot);

    host_present_fill_black_rect((uint8_t *)got->pixels, got->pitch, kind, 0, 0, W, H);
    int tl = host_present_scale2((uint8_t *)got->pixels, got->pitch, kind, top, TW, TH, 0, 0);
    tl |= host_present_scale2((uint8_t *)got->pixels, got->pitch, kind, bot, BW, BH,
                              (TW - BW) / 2 * 2, TH * 2);

    size_t diffs = 0, first = 0;
    for (int y = 0; y < H; ++y) {
        const uint8_t *a = (const uint8_t *)ref->pixels + (size_t)y * ref->pitch;
        const uint8_t *b = (const uint8_t *)got->pixels + (size_t)y * got->pitch;
        for (int i = 0; i < W * 4; ++i)
            if (a[i] != b[i]) { if (!diffs) first = (size_t)y * (W * 4) + (size_t)i; ++diffs; }
    }
    printf("  %-13s dst=%-6s ref=%016llx got=%016llx diff_bytes=%zu%s\n",
           label, kind == HP_KIND_BGRX ? "BGRX" : "RGBA_A",
           fnv((const unsigned char *)ref->pixels, (size_t)ref->pitch * H),
           fnv((const unsigned char *)got->pixels, (size_t)got->pitch * H),
           diffs, diffs ? "  <-- MISMATCH" : "  IDENTICAL");
    if (diffs) {
        size_t y = first / (W * 4), i = first % (W * 4);
        printf("     first@(%zu,%zu,byte%zu) ref=%02X got=%02X\n",
               i / 4, y, i % 4,
               ((const uint8_t *)ref->pixels)[y * (size_t)ref->pitch + i],
               ((const uint8_t *)got->pixels)[y * (size_t)got->pitch + i]);
    }
    /* The translucent-source flag drives the in-place incremental composition
       (it forces a black base for that region), so it must be exact. */
    if ((alpha_mode == 0 && tl) || (alpha_mode != 0 && !tl)) {
        printf("     translucent flag WRONG for alpha_mode=%d (tl=%d)\n", alpha_mode, tl);
        ++diffs;
    }
    SDL_FreeSurface(st); SDL_FreeSurface(sb); SDL_FreeSurface(ref); SDL_FreeSurface(got);
    return diffs ? 1 : 0;
}

static Uint64 now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (Uint64)ts.tv_sec * 1000000000ull + (Uint64)ts.tv_nsec;
}

/* Microbenchmark of the per-present work only (no window, no X11): the frozen
   FillRect + 2x SDL_BlitScaled against the composed fill + 2x kernel, over the
   same real-sized surfaces.  "both" recomposes both screens, "one" only the
   top (what the per-screen serials usually do). */
static int bench(void) {
    rng_state = 999;
    gen(0);
    SDL_Surface *st = SDL_CreateRGBSurfaceFrom(top, TW, TH, 32, TW * 4,
        0x000000FFu, 0x0000FF00u, 0x00FF0000u, 0xFF000000u);
    SDL_Surface *sb = SDL_CreateRGBSurfaceFrom(bot, BW, BH, 32, BW * 4,
        0x000000FFu, 0x0000FF00u, 0x00FF0000u, 0xFF000000u);
    SDL_Surface *dst = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, SDL_PIXELFORMAT_RGB888);
    int kind = HP_KIND_BGRX;
    int iters = 300;
    SDL_Rect rtop = { 0, 0, TW * 2, TH * 2 };
    SDL_Rect rbot = { (TW - BW) / 2 * 2, TH * 2, BW * 2, BH * 2 };
    Uint64 t0;
    /* warm up (first-touch page faults, CPU frequency ramp) */
    for (int i = 0; i < 20; ++i) {
        SDL_FillRect(dst, NULL, SDL_MapRGB(dst->format, 0, 0, 0));
        SDL_BlitScaled(st, NULL, dst, &rtop);
        SDL_BlitScaled(sb, NULL, dst, &rbot);
    }
    t0 = now_ns();
    for (int i = 0; i < iters; ++i) {
        SDL_FillRect(dst, NULL, SDL_MapRGB(dst->format, 0, 0, 0));
        SDL_BlitScaled(st, NULL, dst, &rtop);
        SDL_BlitScaled(sb, NULL, dst, &rbot);
    }
    double frozen = (double)(now_ns() - t0) / iters / 1e6;
    t0 = now_ns();
    for (int i = 0; i < iters; ++i) {
        host_present_fill_black_rect((uint8_t *)dst->pixels, dst->pitch, kind, 0, 0, W, H);
        host_present_scale2((uint8_t *)dst->pixels, dst->pitch, kind, top, TW, TH, 0, 0);
        host_present_scale2((uint8_t *)dst->pixels, dst->pitch, kind, bot, BW, BH,
                            (TW - BW) / 2 * 2, TH * 2);
    }
    double both = (double)(now_ns() - t0) / iters / 1e6;
    t0 = now_ns();
    for (int i = 0; i < iters; ++i) {
        host_present_scale2((uint8_t *)dst->pixels, dst->pitch, kind, top, TW, TH, 0, 0);
    }
    double one = (double)(now_ns() - t0) / iters / 1e6;
    printf("BENCH kernel=%s  frozen(fill+2xBlitScaled)=%.3f ms  "
           "composed(clear+both)=%.3f ms  composed(top only)=%.3f ms  "
           "speedup=%.1fx\n",
           host_present_scaler_mode() == 2 ? "scalar" : "sse2",
           frozen, both, one, frozen / both);
    SDL_FreeSurface(st); SDL_FreeSurface(sb); SDL_FreeSurface(dst);
    return 0;
}

int main(int argc, char **argv) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL init: %s\n", SDL_GetError()); return 2; }
    if (argc > 1 && strcmp(argv[1], "require-dummy") == 0 &&
        strcmp(SDL_GetCurrentVideoDriver(), "dummy") != 0) {
        fprintf(stderr, "expected the dummy video driver, got %s\n", SDL_GetCurrentVideoDriver());
        return 2;
    }
    if (argc > 1 && strcmp(argv[1], "bench") == 0) { bench(); SDL_Quit(); return 0; }
    printf("SDL %d.%d.%d driver=%s kernel=%s\n", SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
           SDL_PATCHLEVEL, SDL_GetCurrentVideoDriver(),
           host_present_scaler_mode() == 2 ? "scalar" : "sse2");
    int fails = 0, cases = 0;
    for (int kind = HP_KIND_RGBA_A; kind <= HP_KIND_BGRX; ++kind) {
        for (int alpha = 0; alpha < 3; ++alpha) {
            fails += run_case(kind, alpha);
            ++cases;
        }
    }
    printf("RESULT: %d/%d cases byte-identical%s\n", cases - fails, cases,
           fails ? "  -- FAIL" : "");
    SDL_Quit();
    return fails ? 1 : 0;
}
