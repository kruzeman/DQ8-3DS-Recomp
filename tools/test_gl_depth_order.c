/* test_gl_depth_order — per-fragment depth ORDERING differential (software vs
 * OpenGL backend) for t_a22413f1.
 *
 * Why: the GL backend emulates the PICA depth buffer inside a COLOR attachment
 * (R32F, COLOR_ATTACHMENT1) and reads the "old" depth of a fragment from either
 *   (a) a per-draw copy of that attachment into a separate texture
 *       (DQ8_GL_FBFETCH=0, the copy path), or
 *   (b) imageLoad() of the SAME attachment the draw is writing
 *       (DQ8_GL_FBFETCH=1, the default on llvmpipe/radeonsi).
 * Both give a fragment the depth value from BEFORE the draw started, so two
 * triangles of ONE draw never depth-test against each other: whichever triangle
 * is rasterised LAST wins the pixel even when it is FARTHER away.  The software
 * rasterizer reads the depth buffer per pixel, so the NEARER triangle wins.
 *
 * A character model is exactly that case: one PICA draw call carrying hundreds
 * of overlapping triangles (arm over torso, clothing over body).  Result:
 * interior patches of the model render with the wrong surface — the operator's
 * "holes in GL" frames (evidence/holes-op-20261101).
 *
 * The test drives both rasterizers with the same triangles/state and asserts
 * the near-over-far order in BOTH submission orders and in one/two batches.
 *
 * Build/run: tools/run_renderer_tests.sh (or the cc line below)
 *   cc -std=c11 -O2 -DDQ8_ENABLE_GL tools/test_gl_depth_order.c -o \
 *      target/test_gl_depth_order $(pkg-config --cflags --libs egl) -lm
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
#include <unistd.h>

/* The backend under test.  -DPICA_GL_SRC='"..."' compiles this same test
   against another copy of pica_gl.c (used to show the PRE-fix source failing
   the cases below, so the test is known to be discriminating). */
#ifndef PICA_GL_SRC
#define PICA_GL_SRC "../src/pica_gl.c"
#endif
#include PICA_GL_SRC

/* ---- stubs required by src/pica_vs.c (same shape as the runtime) ---- */
static uint32_t pica_regs[0x400], vs_program[4096], vs_swizzle[4096];
static unsigned vs_program_size, vs_swizzle_size;
static float vs_uniform_f[96][4], vs_default_attrib[16][4];
static unsigned pica_rt_w = 32, pica_rt_h = 32, pica_rt_format, pica_rt_addr;
static unsigned long long pica_draw_count;
static unsigned pica_depth_bits = 24u;
static unsigned long long pqt_frames;
static unsigned char ram[4096 + 32 * 32 * 4];
#define DEPTH_ADDR 4096u
static unsigned char *const depthmem = ram + DEPTH_ADDR;
static unsigned char fb[32 * 32 * 4]; /* the "render target" pixels */
#define GUEST_RAM_SIZE ((uint32_t)sizeof ram)
static int guest_memory_range(uint32_t a, unsigned n, uint32_t *m) {
    *m = a;
    return (uint64_t)a + n <= sizeof ram;
}
static uint32_t guest_memory_address(uint32_t pa) { return pa; }
static unsigned pica_color_bpp(uint32_t fmt) { (void)fmt; return 4u; }
static uint32_t pica_rt_off(int x, int y) {
    return ((uint32_t)y * 32u + (uint32_t)x) * 4u;
}
static unsigned pica_depth_allow_write = 1u;
static int pica_depth_bound = 1;
static float pica_f24_to_f32(uint32_t raw); /* defined in pica_vs.c */
static uint32_t pica_depth_addr = DEPTH_ADDR;
static unsigned pica_depth_bpp = 3u;
static int pica_depth_range_ok(void) {
    uint32_t mapped;
    return pica_depth_addr && guest_memory_range(pica_depth_addr,
           (uint64_t)pica_rt_w * pica_rt_h * pica_depth_bpp, &mapped);
}
/* Morton tiling, identical to the runtime's pica_depth_off. */
static uint32_t pica_depth_off(int x, int y) {
    static const uint32_t xlut[8] = {0x00,0x01,0x04,0x05,0x10,0x11,0x14,0x15};
    static const uint32_t ylut[8] = {0x00,0x02,0x08,0x0a,0x20,0x22,0x28,0x2a};
    uint32_t px = (uint32_t)x, py = (uint32_t)y;
    uint32_t i = xlut[px & 7u] + ylut[py & 7u];
    uint32_t row = (py >> 3) * (pica_rt_w >> 3) * 64u + (px & ~7u) * 8u;
    return (row + i) * pica_depth_bpp;
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
    (void)u; (void)v;
    *r = *g = *b = 128; *a = 255;
    return 1;
}
static void pica_bind_framebuffer(void) {}
static void pica_bind_depth(void) {}
static float dq8_layer_t(void) { return 0.0f; }
static void pica_bb_add(int x, int y) { (void)x; (void)y; }
static unsigned pica_db_x0, pica_db_x1, pica_db_y0, pica_db_y1;
static void pica_db_add(int x, int y) { (void)x; (void)y; }
static void pica_db_reset(void) {}
static void pica_log_draw(int early) { (void)early; }
static void pica_rect_log(int x, int y, const uint8_t src[4], const uint8_t res[4]) {
    (void)x; (void)y; (void)src; (void)res;
}
/* THE SOFTWARE REFERENCE DEPTH TEST: byte-for-byte the runtime's
   pica_depth_test_write (dq8_main.c prelude), so the software column of this
   test is the game's own semantics, not a reimplementation. */
static int pica_depth_test_write(int x, int y, float depth) {
    uint32_t mask = pica_regs[0x107];
    int test_enable = mask & 1u;
    int write_enable = (mask & (1u << 12)) && pica_depth_allow_write;
    if (!pica_depth_bound || !pica_depth_range_ok())
        return 1;
    uint32_t mapped;
    guest_memory_range(pica_depth_addr,
                       (uint64_t)pica_rt_w * pica_rt_h * pica_depth_bpp, &mapped);
    int my = (int)pica_rt_h - 1 - y;
    if (x < 0 || my < 0 || (uint32_t)x >= pica_rt_w || (uint32_t)my >= pica_rt_h)
        return 1;
    uint32_t off = pica_depth_off(x, my);
    uint8_t *p = ram + mapped + off;
    float d = depth < 0.0f ? 0.0f : (depth > 1.0f ? 1.0f : depth);
    uint32_t zmax = (pica_depth_bits == 16u) ? 0xFFFFu : 0xFFFFFFu;
    uint32_t z = (uint32_t)(d * (float)zmax);
    if (test_enable) {
        uint32_t ref;
        if (pica_depth_bpp == 2u) { uint16_t v; memcpy(&v, p, 2); ref = v; }
        else ref = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
        unsigned func = (mask >> 4) & 7u;
        int pass;
        switch (func) {
        case 0u: pass = 0; break;
        case 1u: pass = 1; break;
        case 2u: pass = z == ref; break;
        case 3u: pass = z != ref; break;
        case 4u: pass = z < ref; break;
        case 5u: pass = z <= ref; break;
        case 6u: pass = z > ref; break;
        default: pass = z >= ref; break;
        }
        if (!pass) return 0;
    }
    if (write_enable) {
        p[0] = (uint8_t)(z & 0xFFu);
        p[1] = (uint8_t)((z >> 8) & 0xFFu);
        if (pica_depth_bpp >= 3u) p[2] = (uint8_t)((z >> 16) & 0xFFu);
    }
    return 1;
}
static float pica_fragment_depth(float zw, float iw, float scale, float offset) {
    (void)iw; (void)scale; (void)offset;
    return zw;
}
static float pica_draw_prim_a = 1.0f, pica_draw_tev_a = 1.0f;
static int pica_rect_src_needed(void) { return 0; }
static void pica_dump_current_cmdlist(unsigned count) { (void)count; }

#include "../src/frame_prof.c"
#include "../src/pica_vs.c"

/* ---- GL host callbacks (depth is authoritative in guest RAM) ---- */
static int host_rt_get(void *user, int x, int y, unsigned char rgba[4]) {
    (void)user;
    if (x < 0 || y < 0 || x >= 32 || y >= 32) return 0;
    memcpy(rgba, fb + ((size_t)y * 32 + x) * 4, 4);
    return 1;
}
static void host_rt_put(void *user, int x, int y, const unsigned char rgba[4]) {
    (void)user;
    if (x < 0 || y < 0 || x >= 32 || y >= 32) return;
    memcpy(fb + ((size_t)y * 32 + x) * 4, rgba, 4);
}
static int host_depth_get(void *user, int x, int y, unsigned *z) {
    (void)user;
    int my = 32 - 1 - y;
    const uint8_t *p = depthmem + pica_depth_off(x, my);
    *z = p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16);
    return 1;
}
static void host_depth_put(void *user, int x, int y, unsigned z) {
    (void)user;
    int my = 32 - 1 - y;
    uint8_t *p = depthmem + pica_depth_off(x, my);
    p[0] = (uint8_t)(z & 0xFFu);
    p[1] = (uint8_t)((z >> 8) & 0xFFu);
    p[2] = (uint8_t)((z >> 16) & 0xFFu);
}
static int host_depth_raw(void *user, const unsigned char **base, unsigned *len) {
    (void)user;
    *base = depthmem;
    *len = 32u * 32u * 3u;
    return 1;
}
static unsigned host_depth_px_off(void *user, int x, int y) {
    (void)user;
    return pica_depth_off(x, 32 - 1 - y);
}
static const PicaGlHost host = {
    host_rt_get, host_rt_put, NULL, NULL, NULL,
    host_depth_get, host_depth_put, host_depth_raw, host_depth_px_off
};

static int failures;
#define CHECK(cond, ...) do { \
    if (cond) printf("ok   " __VA_ARGS__); \
    else { printf("FAIL " __VA_ARGS__); failures++; } \
    printf("\n"); \
} while (0)

static PicaVertex vertex(float x, float y, float z, float r, float g, float b) {
    PicaVertex v;
    memset(&v, 0, sizeof v);
    v.x = x; v.y = y; v.iw = 1.0f; v.zw = z;
    v.color[0] = r; v.color[1] = g; v.color[2] = b; v.color[3] = 1.0f;
    v.u = v.v = 0.5f;
    v.cz = z; v.cw = 1.0f;
    return v;
}

static void reset_buffers(void) {
    memset(fb, 0, sizeof fb);
    for (unsigned i = 0; i < 32u * 32u; ++i) {
        uint8_t *p = depthmem + i * 3u;
        p[0] = p[1] = p[2] = 0xFFu; /* far plane */
    }
}

static void setup_state(void) {
    memset(pica_regs, 0, sizeof pica_regs);
    pica_regs[0x107] = 1u | (4u << 4) | (1u << 12) | 0xf00u; /* test LESS, write, mask */
    pica_regs[0x4D] = 0x003F0000u;  /* f24 1.0: depth = zw * 1 + 0 */
    pica_regs[0x4E] = 0;
    pica_regs[0x6D] = 1u;           /* Z-buffering: no /w */
    pica_regs[0x80] = 0u;           /* TEV stage 0: solid / no texture */
    pica_regs[0x100] = 0u;          /* no blend */
    pica_regs[0x104] = 0u;          /* no alpha test */
}

/* Two overlapping triangles inside a 32x32 target.
   NEAR: zw=0.25 over x in [2,20], RED.   FAR: zw=0.75 over x in [6,28], BLUE.
   Overlap x in [6,20]: the NEAR triangle must win in every submission order. */
static PicaVertex near_a(void) { return vertex(2, 2, 0.25f, 1, 0, 0); }
static PicaVertex near_b(void) { return vertex(20, 2, 0.25f, 1, 0, 0); }
static PicaVertex near_c(void) { return vertex(2, 28, 0.25f, 1, 0, 0); }
/* right-half quad of the near triangle, to cover the overlap box solidly */
static PicaVertex near_d(void) { return vertex(20, 2, 0.25f, 1, 0, 0); }
static PicaVertex near_e(void) { return vertex(20, 28, 0.25f, 1, 0, 0); }
static PicaVertex near_f(void) { return vertex(2, 28, 0.25f, 1, 0, 0); }
static PicaVertex far_a(void) { return vertex(6, 2, 0.75f, 0, 0, 1); }
static PicaVertex far_b(void) { return vertex(28, 2, 0.75f, 0, 0, 1); }
static PicaVertex far_c(void) { return vertex(6, 28, 0.75f, 0, 0, 1); }
static PicaVertex far_d(void) { return vertex(28, 2, 0.75f, 0, 0, 1); }
static PicaVertex far_e(void) { return vertex(28, 28, 0.75f, 0, 0, 1); }
static PicaVertex far_f(void) { return vertex(6, 28, 0.75f, 0, 0, 1); }

/* sample a pixel in fb coordinates (row 0 = raster y 0) */
static void px(int x, int y, unsigned char out[4]) {
    memcpy(out, fb + ((size_t)y * 32 + x) * 4, 4);
}

/* Run <order> (0 = near first, 1 = far first) in one or two batches on both
   paths and require the near surface in the overlap box. */
static void order_case(int far_first, int separate_batches) {
    unsigned char sw[32 * 32 * 4];
    unsigned char swz[32 * 32 * 3];
    char name[128];

    /* --- software reference --- */
    reset_buffers();
    setup_state();
    pica_draw_state_build();
    if (far_first) {
        pica_raster_triangle(far_a(), far_b(), far_c());
        if (separate_batches) pica_draw_state_build();
        pica_raster_triangle(far_d(), far_e(), far_f());
        if (separate_batches) pica_draw_state_build();
        pica_raster_triangle(near_a(), near_b(), near_c());
        pica_raster_triangle(near_d(), near_e(), near_f());
    } else {
        pica_raster_triangle(near_a(), near_b(), near_c());
        pica_raster_triangle(near_d(), near_e(), near_f());
        if (separate_batches) pica_draw_state_build();
        pica_raster_triangle(far_a(), far_b(), far_c());
        if (separate_batches) pica_draw_state_build();
        pica_raster_triangle(far_d(), far_e(), far_f());
    }
    memcpy(sw, fb, sizeof sw);
    memcpy(swz, depthmem, sizeof swz);

    /* --- GL backend --- */
    reset_buffers();
    setup_state();
    if (!pica_gl_prepare_draw()) {
        CHECK(0, "GL state accepted (depth path)");
        return;
    }
    if (far_first) {
        pica_queue_triangle(far_a(), far_b(), far_c());
        pica_queue_triangle(far_d(), far_e(), far_f());
        if (separate_batches) { pica_flush_triangles(); pica_gl_flush_guest_rt(); }
        if (!pica_gl_prepare_draw()) { CHECK(0, "GL state accepted (2)"); return; }
        pica_queue_triangle(near_a(), near_b(), near_c());
        pica_queue_triangle(near_d(), near_e(), near_f());
    } else {
        pica_queue_triangle(near_a(), near_b(), near_c());
        pica_queue_triangle(near_d(), near_e(), near_f());
        if (separate_batches) { pica_flush_triangles(); pica_gl_flush_guest_rt(); }
        if (!pica_gl_prepare_draw()) { CHECK(0, "GL state accepted (2)"); return; }
        pica_queue_triangle(far_a(), far_b(), far_c());
        pica_queue_triangle(far_d(), far_e(), far_f());
    }
    pica_flush_triangles();
    pica_gl_flush_guest_rt();

    snprintf(name, sizeof name, "depth order: %s first, %s batch",
             far_first ? "far" : "near", separate_batches ? "2 draws" : "1 draw");

    /* Reference pixels: overlap (10,15) must be NEAR (red) in software. */
    {
        unsigned char s[4], g[4];
        memcpy(s, sw + ((size_t)15 * 32 + 10) * 4, 4);
        px(10, 15, g);
        CHECK(s[0] > 200 && s[2] < 60,
              "%s: software reference overlap = NEAR (r=%u g=%u b=%u)", name,
              s[0], s[1], s[2]);
        CHECK(g[0] == s[0] && g[1] == s[1] && g[2] == s[2],
              "%s: GL overlap = software NEAR surface (GL %u,%u,%u / SW %u,%u,%u)",
              name, g[0], g[1], g[2], s[0], s[1], s[2]);
    }
    /* Whole-image differential (interior only: skip the 1px edge rule band). */
    {
        int maxd = 0, n = 0, bad = 0;
        for (int y = 3; y < 27; ++y) {
            for (int x = 3; x < 27; ++x) {
                const unsigned char *s = sw + ((size_t)y * 32 + x) * 4;
                const unsigned char *g = fb + ((size_t)y * 32 + x) * 4;
                for (int k = 0; k < 4; ++k) {
                    int d = abs((int)s[k] - (int)g[k]);
                    if (d > maxd) maxd = d;
                    if (d > 1) { bad++; break; }
                }
                ++n;
            }
        }
        CHECK(bad == 0, "%s: interior differential (%d px, %d differ, max delta %d)",
              name, n, bad, maxd);
    }
    /* The guest-visible depth bytes must match the software rasterizer's too:
       later draws (and the guest itself) test against them. */
    {
        int zb = 0, shown = 0;
        for (size_t i = 0; i < sizeof swz; ++i) if (swz[i] != depthmem[i]) zb++;
        if (zb && getenv("GLDIFF_DUMP")) {
            for (int y = 0; y < 32; ++y)
                for (int x = 0; x < 32; ++x) {
                    int my = 32 - 1 - y;
                    unsigned off = pica_depth_off(x, my);
                    const unsigned char *s = swz + off, *g = depthmem + off;
                    if ((s[0] != g[0] || s[1] != g[1] || s[2] != g[2]) && shown < 12) {
                        printf("  zdiff x=%d y=%d my=%d sw=%02X%02X%02X gl=%02X%02X%02X\n",
                               x, y, my, s[2], s[1], s[0], g[2], g[1], g[0]);
                        shown++;
                    }
                }
        }
        CHECK(zb == 0, "%s: guest depth write-back identical (%d/%zu bytes differ)",
              name, zb, sizeof swz);
    }
}

/* ---- model case: ONE draw, many overlapping triangles ------------------
   The operator's artifact is a CHARACTER model, i.e. one PICA draw call that
   carries the whole surface as a single triangle list: arm over torso, clothing
   over body, all in one batch, submitted in mesh order (not depth order).  This
   case builds exactly that shape of draw: MODEL_N quads, each covering the
   middle of the target at a distinct depth, submitted in a permuted order, all
   inside ONE batch.  Every interior pixel is covered by every quad, so the
   winner must be the NEAREST quad regardless of submission order.

   With the pre-fix backend every fragment compares against the depth as it was
   BEFORE the draw, so the LAST submitted quad wins instead — large interior
   patches of the "model" show the wrong surface, which is the reported defect. */
#define MODEL_N 24
static PicaVertex model_v(int q, int corner) {
    /* quad q: a 20x18 box offset by (q%3)*2,(q%4)*2; depth by index */
    static const int cx[6] = {0, 1, 0, 1, 1, 0};
    static const int cy[6] = {0, 0, 1, 0, 1, 1};
    float x0 = 6.0f + (float)(q % 3) * 2.0f;
    float y0 = 6.0f + (float)(q % 4) * 2.0f;
    float x1 = x0 + 20.0f, y1 = y0 + 16.0f;
    float x = cx[corner] ? x1 : x0;
    float y = cy[corner] ? y1 : y0;
    float z = 0.10f + 0.03f * (float)q;  /* distinct depth per quad */
    float col = (float)((q * 10) % 200 + 40) / 255.0f;
    return vertex(x, y, z, col, col * 0.5f, 1.0f - col);
}
/* Submission order: mesh order is NOT depth order (like a real model). */
static int model_perm(int i, int reverse) {
    int p = (i * 7) % MODEL_N;   /* 7 and 24 are coprime -> a permutation */
    return reverse ? (MODEL_N - 1 - p) : p;
}

static void dump_ppm(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n32 32\n255\n");
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x)
            fwrite(fb + ((size_t)y * 32 + x) * 4, 1, 3, f);
    fclose(f);
}

static void model_case(int reverse) {
    unsigned char sw[32 * 32 * 4];
    unsigned char swz[32 * 32 * 3];
    static const int tri[6] = {0, 1, 2, 1, 4, 2};

    /* --- software reference: one draw, all triangles --- */
    reset_buffers();
    setup_state();
    pica_draw_state_build();
    for (int i = 0; i < MODEL_N; ++i) {
        int q = model_perm(i, reverse);
        for (int t = 0; t < 6; t += 3)
            pica_raster_triangle(model_v(q, tri[t]), model_v(q, tri[t + 1]),
                                 model_v(q, tri[t + 2]));
    }
    memcpy(sw, fb, sizeof sw);
    memcpy(swz, depthmem, sizeof swz);

    /* --- GL backend: the same triangles in the SAME one draw --- */
    reset_buffers();
    setup_state();
    if (!pica_gl_prepare_draw()) { CHECK(0, "model: GL state accepted"); return; }
    for (int i = 0; i < MODEL_N; ++i) {
        int q = model_perm(i, reverse);
        for (int t = 0; t < 6; t += 3)
            pica_queue_triangle(model_v(q, tri[t]), model_v(q, tri[t + 1]),
                                model_v(q, tri[t + 2]));
    }
    pica_flush_triangles();
    pica_gl_flush_guest_rt();

    /* HOLES_DUMP=<prefix> writes the two images (32x32 PPM) for the report. */
    {
        const char *pfx = getenv("HOLES_DUMP");
        if (pfx && *pfx) {
            char a[512], b[512];
            snprintf(a, sizeof a, "%s_sw.ppm", pfx);
            snprintf(b, sizeof b, "%s_gl.ppm", pfx);
            FILE *f = fopen(a, "wb");
            if (f) {
                fprintf(f, "P6\n32 32\n255\n");
                for (int y = 0; y < 32; ++y)
                    for (int x = 0; x < 32; ++x)
                        fwrite(sw + ((size_t)y * 32 + x) * 4, 1, 3, f);
                fclose(f);
            }
            dump_ppm(b);
        }
    }

    /* Quad q is at depth 0.10+0.03q, so quad 0 is nearest and must own every
       pixel it covers.  Both rasterizers must produce the SAME image. */
    {
        int bad = 0, maxd = 0, n = 0;
        for (int y = 9; y < 20; ++y) {
            for (int x = 9; x < 22; ++x) {
                const unsigned char *s = sw + ((size_t)y * 32 + x) * 4;
                const unsigned char *g = fb + ((size_t)y * 32 + x) * 4;
                for (int k = 0; k < 4; ++k) {
                    int d = abs((int)s[k] - (int)g[k]);
                    if (d > maxd) maxd = d;
                    if (d > 1) { bad++; break; }
                }
                ++n;
            }
        }
        int zdiff = 0, zdiff_big = 0;
        for (size_t i = 0; i < sizeof swz; ++i) {
            int d = abs((int)swz[i] - (int)depthmem[i]);
            if (d) zdiff++;
            if (d > 1) zdiff_big++;
        }
        CHECK(bad == 0,
              "model (one draw, %d overlapping quads, %s order): %d/%d interior px "
              "differ (max delta %d)", MODEL_N, reverse ? "reverse mesh" : "mesh",
              bad, n, maxd);
        CHECK(zdiff_big == 0, "model (one draw, %d quads, %s order): guest depth "
              "write-back within 1 LSB (%d/%zu bytes differ, %d by more than 1)",
              MODEL_N, reverse ? "reverse mesh" : "mesh", zdiff, sizeof swz, zdiff_big);
    }
}

int main(void) {
    setenv("DQ8_GL", "1", 1);
    /* t_a22413f1 run3: the HARDWARE depth path is opt-in in the delivered
       binary; this test is its theory check, so it asks for it unless the
       caller overrides (DQ8_GL_DEPTH_ATTACH=0 then exercises the in-shader
       fallback, i.e. the pre-fix behaviour). */
    if (!getenv("DQ8_GL_DEPTH_ATTACH"))
        setenv("DQ8_GL_DEPTH_ATTACH", "1", 1);
    if (!pica_gl_init()) {
        printf("SKIP: GL backend unavailable (EGL init failed)\n");
        return 2;
    }
    printf("GL renderer: %s\n", pica_gl_renderer());
    printf("depth path: %s (DQ8_GL_DEPTH_ATTACH=%s)\n",
           pgl_depth_hw ? "HARDWARE D32F attachment + gl_FragDepth"
                        : "IN-SHADER emulation (ATT1 mirror)",
           getenv("DQ8_GL_DEPTH_ATTACH"));
    printf("framebuffer fetch (imageLoad depth source): %s\n",
           pgl_fbfetch ? "on" : "off (per-draw copies)");
    CHECK(fabsf(pica_f24_to_f32(0x003F0000u) - 1.0f) < 1e-6f,
          "depth scale register decodes to 1.0 (got %g)",
          (double)pica_f24_to_f32(0x003F0000u));

    order_case(0, 0); /* near first, one draw */
    order_case(1, 0); /* far first, one draw: THE operator case */
    order_case(0, 1); /* separate draws */
    order_case(1, 1);
    model_case(0);    /* ONE draw, 24 overlapping quads, mesh order */
    model_case(1);    /* ONE draw, same quads, reversed submission order */

    pica_gl_shutdown();
    if (failures) {
        printf("RESULT: %d FAILED\n", failures);
        return 1;
    }
    printf("RESULT: PASS\n");
    return 0;
}
