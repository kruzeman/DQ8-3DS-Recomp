/* test_pica_gl_diff — software/OpenGL pixel differential and texture-cache
 * collision test for the audit findings A14 and A15.
 *
 * A14: GL texture-modulate must saturate the primary color before the
 *      texture multiply exactly like the software rasterizer
 *      (pica_sat on the interpolated primary color, then TEV).
 * A15: the GL texture upload cache must not treat a 32-bit FNV hash as proof
 *      of equal texture bytes (a real collision pair is exercised).
 *
 * The test drives BOTH paths with the same triangles and draw state and
 * compares interior pixels of the 32x32 render target byte-for-byte
 * (tolerance 1 LSB for fixed-point rounding), asserting the draw actually
 * ran on the GL backend (no silent software fallback).
 *
 * Build/run: tools/run_renderer_tests.sh  (needs EGL; headless surfaceless)
 * Exit code 0 = all checks passed.
 */
#define DQ8_ENABLE_GL
#define _POSIX_C_SOURCE 199309L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <time.h>

#include "../src/pica_gl.c"

/* ---- stubs required by src/pica_vs.c (same shape as the runtime) ---- */
static uint32_t pica_regs[0x400], vs_program[4096], vs_swizzle[4096];
static unsigned vs_program_size, vs_swizzle_size;
static float vs_uniform_f[96][4], vs_default_attrib[16][4];
static unsigned pica_rt_w = 32, pica_rt_h = 32, pica_rt_format, pica_rt_addr;
static unsigned long long pica_draw_count;
static unsigned pica_depth_bits = 24u;
static unsigned long long pqt_frames;
/* Guest RAM carries BOTH the 32x32 linear "render target" (first 4096 bytes,
   fb alias below) and the 8x8 texture source (bytes 4096..4352), because the
   staging cache's dirty-check reads the texture from `ram` exactly like the
   runtime's pica_sample_tex0 does. */
static unsigned char ram[4096 + 256];
#define fb ram /* the "guest" color buffer IS the raw region the GL backend
                  compares against (test layout is linear, not Morton-tiled) */
static unsigned char *const tex = ram + 4096; /* texture source region, also
                                                 seen by the dirty-check */
#define GUEST_RAM_SIZE ((uint32_t)sizeof ram)
static int guest_memory_range(uint32_t a, unsigned n, uint32_t *m) {
    *m = a;
    return a + n <= sizeof ram;
}
static uint32_t guest_memory_address(uint32_t pa) { return pa; }
static unsigned pica_color_bpp(uint32_t fmt) { (void)fmt; return 4u; }
static uint32_t pica_rt_off(int x, int y) {
    return ((uint32_t)y * 32u + (uint32_t)x) * 4u; /* linear test layout */
}
static unsigned pica_depth_allow_write = 1u;
static int pica_depth_bound;
static int pica_depth_range_ok(void) { return 1; }
static uint32_t pica_depth_addr;
static unsigned pica_depth_bpp = 3u;
static uint32_t pica_depth_off(int x, int y) {
    return ((uint32_t)y * 32u + (uint32_t)x) * pica_depth_bpp; /* linear */
}
static int pica_get_pixel(uint32_t addr, unsigned w, unsigned h, unsigned fmt,
                          unsigned x, unsigned y,
                          uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *a) {
    (void)addr; (void)fmt;
    if (x >= w || y >= h) return 0;
    const uint8_t *p = fb + ((size_t)y * w + x) * 4;
    *r = p[0]; *g = p[1]; *b = p[2]; *a = p[3];
    return 1;
}
static void pica_put_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (x < 0 || x >= 32 || y < 0 || y >= 32) return;
    uint8_t *p = fb + ((size_t)y * 32 + x) * 4;
    p[0] = r; p[1] = g; p[2] = b; p[3] = a;
}
static int pica_sample_tex0(float u, float v,
                            uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *a) {
    int x = (int)fminf(7, fmaxf(0, u * 8)), y = (int)fminf(7, fmaxf(0, v * 8));
    const uint8_t *p = tex + ((size_t)y * 8 + x) * 4;
    *r = p[0]; *g = p[1]; *b = p[2]; *a = p[3];
    return 1;
}
static void pica_bind_framebuffer(void) {}
static void pica_bind_depth(void) {}
static int setenv(const char *name, const char *value, int overwrite);
/* diagnostics / depth stubs: logging only, depth always passes and never
   writes (the differential cases keep depth off) */
static float dq8_layer_t(void) { return 0.0f; }
static void pica_bb_add(int x, int y) { (void)x; (void)y; }
static unsigned pica_db_x0, pica_db_x1, pica_db_y0, pica_db_y1;
static void pica_db_add(int x, int y) { (void)x; (void)y; }
static void pica_db_reset(void) {}
static void pica_log_draw(int early) { (void)early; }
static void pica_rect_log(int x, int y, const uint8_t src[4], const uint8_t res[4]) {
    (void)x; (void)y; (void)src; (void)res;
}
static int pica_depth_test_write(int x, int y, float depth) {
    (void)x; (void)y; (void)depth;
    return 1;
}
static float pica_fragment_depth(float zw, float iw, float scale, float offset) {
    (void)iw; (void)scale; (void)offset;
    return zw;
}
static float pica_draw_prim_a = 1.0f, pica_draw_tev_a = 1.0f;
static int pica_rect_src_needed(void) { return 0; }

#include "../src/pica_vs.c"

/* ---- harness ---- */
static int failures;
static int diff_strict; /* require byte-identical output (0 LSB tolerance) */
#define CHECK(cond, ...) do { \
    if (cond) printf("ok   " __VA_ARGS__); \
    else { printf("FAIL " __VA_ARGS__); failures++; } \
    printf("\n"); \
} while (0)

static void init_tev(void) {
    /* TEV stage 0 = pure modulate (primary * texture, RGB + alpha),
       stages 1..5 = pass-through: exactly the GL TEX_MODULATE contract. */
    unsigned bases[] = { 0xc0, 0xc8, 0xd0, 0xd8, 0xf0, 0xf8 };
    pica_regs[0x80] = 1;
    pica_regs[0x82] = (8 << 16) | 8;
    pica_regs[0x85] = 512u; /* texture base: tex = ram + 4096 (see stubs) */
    pica_regs[0x107] = 0xf00;
    pica_regs[0xc0] = 0x00300030;
    pica_regs[0xc2] = 0x00010001;
    for (int i = 1; i < 6; i++) pica_regs[bases[i]] = 0x000f000f;
    memset(tex, 128, 8 * 8 * 4);
    for (int i = 0; i < 64; i++) tex[i * 4 + 3] = 255;
}

static PicaVertex vertex(float x, float y, float r, float g, float b, float a) {
    PicaVertex v = { .x = x, .y = y, .iw = 1, .color = { r, g, b, a },
                     .u = 0.5f, .v = 0.5f };
    return v;
}

static void clear_fb(void) {
    for (unsigned i = 0; i < 32 * 32; ++i) {
        fb[i * 4 + 0] = 10; fb[i * 4 + 1] = 20; fb[i * 4 + 2] = 30; fb[i * 4 + 3] = 255;
    }
}

/* Run the same triangle on both paths and compare interior pixels.
   Interior = at least 2 px from the viewport border and from the
   hypotenuse of the (0,0)-(32,0)-(0,32) test triangle. */
static void diff_case(const char *name, PicaVertex a, PicaVertex b, PicaVertex c) {
    unsigned char sw[32 * 32 * 4], glsnap[32 * 32 * 4];
    unsigned long long ok_before = pica_gl_draws_ok, sw_before = pica_gl_draws_sw;

    clear_fb();
    pica_raster_triangle(a, b, c);
    memcpy(sw, fb, sizeof sw);

    clear_fb();
    if (!pica_gl_prepare_draw()) {
        CHECK(0, "%s: GL draw state accepted", name);
        return;
    }
    pica_queue_triangle(a, b, c);
    pica_flush_triangles();
    pica_gl_flush_guest_rt(); /* deferred write-back, as a transfer would */
    memcpy(glsnap, fb, sizeof glsnap);

    if (getenv("GLDIFF_DUMP")) {
        /* '#'=written (differs from bg), '.'=bg, rows top-down in fb order */
        printf("dump %s:\n   software                gl\n", name);
        for (int y = 0; y < 32; ++y) {
            for (int k = 0; k < 2; ++k) {
                const unsigned char *s = k ? glsnap : sw;
                for (int x = 0; x < 32; ++x) {
                    const unsigned char *p = s + ((size_t)y * 32 + x) * 4;
                    putchar((p[0] != 10 || p[1] != 20 || p[2] != 30) ? '#' : '.');
                }
                putchar(k ? '\n' : ' ');
            }
        }
        for (int y = 2; y < 25; y += 7) {
            const unsigned char *s = sw + ((size_t)y * 32 + 6) * 4;
            const unsigned char *g = glsnap + ((size_t)y * 32 + 6) * 4;
            printf("  sample (6,%d): sw=%3d,%3d,%3d,%3d gl=%3d,%3d,%3d,%3d\n", y,
                   s[0], s[1], s[2], s[3], g[0], g[1], g[2], g[3]);
        }
    }

    if (pica_gl_draws_ok != ok_before + 1u) {
        CHECK(0, "%s: ran on GL backend (ok=%llu sw=%llu)", name,
              (unsigned long long)(pica_gl_draws_ok - ok_before),
              (unsigned long long)(pica_gl_draws_sw - sw_before));
        return;
    }
    int max_delta = 0;
    int compared = 0;
    for (int y = 2; y < 30; ++y) {
        for (int x = 2; x < 30; ++x) {
            /* Clear of all three triangle edges in fb coordinates: the
               hypotenuse runs along x==y and rasterization fill rules differ
               by one boundary pixel there (pre-existing GL/software property,
               not part of the A14 saturation contract). */
            if (x + y > 26 || x >= y - 1) continue;
            const unsigned char *s = sw + ((size_t)y * 32 + x) * 4;
            const unsigned char *g = glsnap + ((size_t)y * 32 + x) * 4;
            for (int k = 0; k < 4; ++k) {
                int d = abs((int)s[k] - (int)g[k]);
                if (d > max_delta) max_delta = d;
            }
            compared++;
        }
    }
    CHECK(max_delta <= (diff_strict ? 0 : 1) && compared > 100,
          "%s: software/GL interior differential (%d px, max delta %d%s)",
          name, compared, max_delta, diff_strict ? ", strict" : "");
}

static void modulate_saturation_cases(void) {
    PicaVertex a, b, c;

    /* Primary components > 1: software saturates BEFORE the multiply
       (2 * 128/255 saturated first = 128), a final-result clamp would
       give 255 (the A14 repro). */
    a = vertex(0, 0, 2, 2, 2, 2); b = vertex(32, 0, 2, 2, 2, 2);
    c = vertex(0, 32, 2, 2, 2, 2);
    diff_case("modulate primary>1 (2,2,2,2)", a, b, c);
    CHECK(1, "    reference: software texel=128 => interior=128,128,128,255");

    /* Primary components < 0: saturation to 0 before multiply. */
    a = vertex(0, 0, -1, -1, -1, -1); b = vertex(32, 0, -1, -1, -1, -1);
    c = vertex(0, 32, -1, -1, -1, -1);
    diff_case("modulate primary<0 (-1,-1,-1,-1)", a, b, c);

    /* In-range control. */
    a = vertex(0, 0, 0.5f, 0.25f, 0.75f, 1); b = vertex(32, 0, 0.5f, 0.25f, 0.75f, 1);
    c = vertex(0, 32, 0.5f, 0.25f, 0.75f, 1);
    diff_case("modulate primary in [0,1]", a, b, c);

    /* Interpolated gradient crossing the saturation boundary. */
    a = vertex(0, 0, 2, 0, 0, 1); b = vertex(32, 0, 0, 2, 0, 1);
    c = vertex(0, 32, 0, 0, 2, 1);
    diff_case("modulate gradient crossing 1.0", a, b, c);
}

static void modulate_alpha_test_case(void) {
    /* alpha-test EQUAL 255 with primary alpha=2: software saturates the
       interpolated alpha first (2 -> 1 -> *tex.a -> 255, passes), an
       unsaturated GL path computes a=510 and wrongly rejects. */
    pica_regs[0x104] = 1u | (2u << 4) | (255u << 8);
    PicaVertex a = vertex(0, 0, 0.5f, 0.5f, 0.5f, 2);
    PicaVertex b = vertex(32, 0, 0.5f, 0.5f, 0.5f, 2);
    PicaVertex c = vertex(0, 32, 0.5f, 0.5f, 0.5f, 2);
    diff_case("modulate + alpha-test equal255, primary a=2", a, b, c);
    pica_regs[0x104] = 0;
}

static void modulate_blend_case(void) {
    /* src_alpha / one_minus_src_alpha blend over the background with
       primary alpha=2 (saturation boundary again). */
    pica_regs[0x100] = 256u;
    pica_regs[0x101] = 0x54540000u; /* add: src_alpha, 1-src_alpha (rgb+a) */
    PicaVertex a = vertex(0, 0, 2, 0.5f, 0.25f, 2);
    PicaVertex b = vertex(32, 0, 2, 0.5f, 0.25f, 2);
    PicaVertex c = vertex(0, 32, 2, 0.5f, 0.25f, 2);
    diff_case("modulate + src_alpha blend, primary>1", a, b, c);
    pica_regs[0x100] = 0;
    pica_regs[0x101] = 0;
}

/* ---- A15: texture upload cache collision safety ---- */
static void texture_cache_cases(void) {
    /* The documented collision pair: two 8x8 RGBA images that differ in the
       first 8 bytes and hash to the SAME 32-bit FNV value. */
    const unsigned char aa[8] = { 137, 137, 95, 52, 91, 83, 159, 236 };
    const unsigned char bb[8] = { 17, 47, 91, 179, 81, 121, 28, 92 };

    PicaVertex a = vertex(0, 0, 1, 1, 1, 1), b = vertex(32, 0, 1, 1, 1, 1),
               c = vertex(0, 32, 1, 1, 1, 1);
    a.u = a.v = b.u = b.v = c.u = c.v = 0.0625f; /* sample texel (0,0) only */

    /* Draw A (also the "unchanged repeat" baseline).  u=v=0.0625 makes every
       pixel sample texel (0,0); (10,10) is a safe interior pixel. */
    memcpy(tex, aa, 8);
    CHECK(pica_gl_prepare_draw(), "cache: prepare A accepted");
    unsigned hash_a = pica_gl_st.tex_hash;
    unsigned long long up_before = pgl_tex_uploads;
    clear_fb();
    pica_queue_triangle(a, b, c);
    pica_flush_triangles();
    pica_gl_flush_guest_rt();
    unsigned char pixel_a = fb[(16 * 32 + 6) * 4];
    CHECK(pgl_tex_uploads == up_before + 1u, "cache: first upload happens (uploads=%llu)",
          (unsigned long long)(pgl_tex_uploads - up_before));

    /* Unchanged image drawn again: no re-upload, same pixel. */
    CHECK(pica_gl_prepare_draw(), "cache: prepare A repeat accepted");
    up_before = pgl_tex_uploads;
    clear_fb();
    pica_queue_triangle(a, b, c);
    pica_flush_triangles();
    pica_gl_flush_guest_rt();
    CHECK(pgl_tex_uploads == up_before, "cache: unchanged image skips upload (delta=%llu)",
          (unsigned long long)(pgl_tex_uploads - up_before));
    CHECK(pica_stage_hits >= 1u,
          "cache: staging dirty-check hit on unchanged texture (hits=%llu)",
          pica_stage_hits);
    CHECK(fb[(16 * 32 + 6) * 4] == pixel_a, "cache: unchanged image pixel stable (%u)",
          fb[(16 * 32 + 6) * 4]);
    CHECK(pixel_a == 137u, "cache: image A red = 137 (got %u)", pixel_a);

    /* Software reference for image B. */
    memcpy(tex, bb, 8);
    clear_fb();
    pica_raster_triangle(a, b, c);
    unsigned char pixel_b_sw = fb[(16 * 32 + 6) * 4];

    /* B in the SAME staging allocation: same pointer, same dimensions,
       SAME HASH — the pre-fix cache skipped the upload here (A15). */
    CHECK(pica_gl_prepare_draw(), "cache: prepare B accepted");
    unsigned hash_b = pica_gl_st.tex_hash;
    CHECK(hash_a == 0x9fa6dad9u && hash_b == hash_a,
          "cache: collision pair reproduces FNV 9fa6dad9 (A=%08X B=%08X)",
          hash_a, hash_b);
    {
        /* The multi-slot upload cache must hold a resident slot for the
           staged pointer (pointer test alone is still unsafe — see A15). */
        size_t si; int held = 0;
        for (si = 0; si < PGL_UP_SLOTS; ++si)
            if (pgl_up[si].used && pgl_up[si].ptr == pica_gl_st.tex_rgba) held = 1;
        CHECK(held, "cache: staging pointer resident in upload cache "
              "(pointer test alone is unsafe)");
    }
    up_before = pgl_tex_uploads;
    clear_fb();
    pica_queue_triangle(a, b, c);
    pica_flush_triangles();
    pica_gl_flush_guest_rt();
    unsigned char pixel_b_gl = fb[(16 * 32 + 6) * 4];
    CHECK(pgl_tex_uploads == up_before + 1u,
          "cache: colliding content change re-uploads (delta=%llu)",
          (unsigned long long)(pgl_tex_uploads - up_before));
    CHECK(pica_stage_misses >= 2u,
          "cache: raw-byte dirty-check re-decodes on changed texture (misses=%llu)",
          pica_stage_misses);
    CHECK(pixel_b_sw == 17u, "cache: software image B red = 17 (got %u)", pixel_b_sw);
    CHECK(pixel_b_gl == pixel_b_sw,
          "cache: GL shows image B after colliding change (GL=%u SW=%u)",
          pixel_b_gl, pixel_b_sw);
}

/* ---- full-TEV-chain differential (software pica_tev vs GL fragment
   shader).  Randomized TEV register configurations cover every combiner
   op/modifier/scale shape pica_tev supports (replace/mul/add/addsub/lerp/
   sub/dot3/mad/lerp-min, all source and modifier selects, buffer updates,
   constants), plus randomized alpha-test, blend and color-mask states.
   Vertex colors are constant per triangle so the comparison isolates the
   fragment math (interpolation is covered by the modulate cases above).
   Strict mode: byte-identical output required. ---- */
static uint32_t rng_state = 0x12345678u;
static uint32_t rnd(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

static void random_tev_regs(void) {
    static const unsigned bases[6] = { 0xc0, 0xc8, 0xd0, 0xd8, 0xf0, 0xf8 };
    static const unsigned srcs[5] = { 0, 3, 13, 14, 15 }; /* prim/tex/buf/cst/prev */
    static const unsigned cops[11] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    for (int s = 0; s < 6; s++) {
        uint32_t sources = 0, mods = 0;
        for (int i = 0; i < 3; i++) {
            sources |= srcs[rnd() % 5] << (4 * i);
            sources |= srcs[rnd() % 5] << (16 + 4 * i);
            mods |= (rnd() % 16) << (4 * i);
            mods |= (rnd() % 8) << (12 + 4 * i);
        }
        pica_regs[bases[s]] = sources;
        pica_regs[bases[s] + 1] = mods;
        pica_regs[bases[s] + 2] =
            (cops[rnd() % 11] << 16) | cops[rnd() % 11]; /* aop | cop */
        pica_regs[bases[s] + 3] = rnd();                 /* constant color */
        pica_regs[bases[s] + 4] = ((rnd() % 4) << 16) | (rnd() % 4); /* scales */
    }
    pica_regs[0xfd] = rnd();            /* TEV buffer initial color */
    pica_regs[0xe0] = rnd() & 0xFF00u;  /* buffer update flags (stages 0..3) */
    pica_regs[0x80] = 1;
    pica_regs[0x82] = (8 << 16) | 8;
    pica_regs[0x85] = 512u; /* texture base: tex = ram + 4096 (see stubs) */
    pica_regs[0x83] = 0;                /* clamp-to-edge wrap */
    /* alpha test on/off + random func/ref */
    pica_regs[0x104] = (rnd() % 2)
        ? (1u | ((1u + rnd() % 7u) << 4) | ((rnd() % 256u) << 8)) : 0u;
    /* blend on/off + random equations/factors/constant */
    pica_regs[0x100] = (rnd() % 2) ? 256u : 0u;
    pica_regs[0x101] = (rnd() % 8) | ((rnd() % 8) << 8) |
                       ((rnd() % 16) << 16) | ((rnd() % 16) << 20) |
                       ((rnd() % 16) << 24) | ((rnd() % 16) << 28);
    pica_regs[0x103] = rnd();
    /* color mask: usually full, sometimes partial */
    pica_regs[0x107] = 0xf00u ^ (((rnd() % 2) ? 0u : (rnd() % 15u + 1u)) << 8);
}

static void tev_random_cases(void) {
    for (int t = 0; t < 24; t++) {
        char name[64];
        PicaVertex a, b, c;
        float col[4];
        random_tev_regs();
        for (int i = 0; i < 8 * 8 * 4; i++) tex[i] = (unsigned char)rnd();
        for (int k = 0; k < 4; k++) col[k] = ((int)(rnd() % 300) - 50) / 100.0f;
        a = vertex(0, 0, col[0], col[1], col[2], col[3]);
        b = vertex(32, 0, col[0], col[1], col[2], col[3]);
        c = vertex(0, 32, col[0], col[1], col[2], col[3]);
        a.u = a.v = b.u = b.v = c.u = c.v = (float)((rnd() % 64) + 1) / 65.0f;
        snprintf(name, sizeof name, "TEV random #%02d", t);
        diff_strict = 1;
        diff_case(name, a, b, c);
        diff_strict = 0;
    }
}

/* Deterministic hand-built chains exercising buffer updates and multi-stage
   previous/constant chains explicitly (the dialog-style shape: several
   stages chaining previous + texture). */
static void tev_chain_cases(void) {
    unsigned bases[] = { 0xc0, 0xc8, 0xd0, 0xd8, 0xf0, 0xf8 };
    PicaVertex a, b, c;

    /* stage0: prev = primary*texture, stage1: prev = prev + constant
       (scale x2), stage2..5 pass-through. */
    for (int i = 0; i < 6; i++) {
        pica_regs[bases[i]] = 0x000f000f;
        pica_regs[bases[i] + 1] = 0;
        pica_regs[bases[i] + 2] = 0;
        pica_regs[bases[i] + 3] = 0;
        pica_regs[bases[i] + 4] = 0;
    }
    pica_regs[0xc0] = 0x00300030;   /* color0=prim, color1=tex; alpha0=prim.a, alpha1=tex.a */
    pica_regs[0xc2] = 0x00010001;   /* mul, mul */
    pica_regs[0xc8] = 0x0e0f000f;   /* c0=prev, c1=const; a0=prev */
    pica_regs[0xc9] = 0x00000000;
    pica_regs[0xcb] = 0x80402010u;  /* constant color */
    pica_regs[0xca] = 0x00020002;   /* add, add */
    pica_regs[0xcc] = 0x00020000;   /* scale x2 (rgb), x1 (a) */
    pica_regs[0x80] = 1;
    pica_regs[0x82] = (8 << 16) | 8;
    pica_regs[0x83] = 0;
    pica_regs[0xe0] = 0x0100u;      /* stage0 writes pending rgb */
    pica_regs[0xfd] = 0x20304050u;
    memset(tex, 96, sizeof tex);
    for (int i = 0; i < 64; i++) tex[i * 4 + 3] = 200;
    a = vertex(0, 0, 0.8f, 0.6f, 0.4f, 0.9f);
    b = vertex(32, 0, 0.8f, 0.6f, 0.4f, 0.9f);
    c = vertex(0, 32, 0.8f, 0.6f, 0.4f, 0.9f);
    diff_strict = 1;
    diff_case("TEV chain mul + add-const x2 + buffer", a, b, c);

    /* dot3 stage feeding alpha from the dot (cop==7). */
    pica_regs[0xc0] = 0x00300030;
    pica_regs[0xc2] = 0x00010007;   /* dot3 with alpha = dot result */
    pica_regs[0xc8] = 0x000f000f;
    pica_regs[0xca] = 0x00000000;
    pica_regs[0xe0] = 0;
    diff_case("TEV dot3 (cop=7) alpha=rgb", a, b, c);
    diff_strict = 0;
}

int main(void) {
    setenv("DQ8_GL", "1", 1);
    if (!pica_gl_init()) {
        printf("SKIP: GL backend unavailable (EGL init failed)\n");
        return 2;
    }
    init_tev();

    modulate_saturation_cases();
    modulate_alpha_test_case();
    modulate_blend_case();
    texture_cache_cases(); /* before the randomized cases: pins the A15
                              collision-pair hash over the known texture */
    tev_chain_cases();
    tev_random_cases();

    pica_gl_shutdown();
    if (failures) {
        printf("RESULT: %d FAILED\n", failures);
        return 1;
    }
    printf("RESULT: PASS\n");
    return 0;
}
