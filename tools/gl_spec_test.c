/* gl_spec_test — t_5ddc492f differential test for the specialised fragment
 * shaders of src/pica_gl.c.
 *
 * For each draw state in the table below the SAME draw is submitted twice in
 * ONE process: once with the generated per-shape shader (DQ8_GL_SPEC=1) and
 * once with the generic shader (DQ8_GL_SPEC=0), and the two render-target
 * read-backs are compared byte for byte.
 *
 * The comparison is valid because every case starts with a full-target
 * PRIMING draw (depth write on, depth test ALWAYS, no blend) that overwrites
 * the whole colour target and the whole depth target with identical values in
 * both passes.  Whatever the second draw reads (destination colour for blend,
 * old z for the depth test) is therefore identical in both passes, so any
 * pixel difference is the shader's.
 *
 * Usage: tools/run_gl_spec_test.sh            (headless EGL)
 * Exit 0 = every case matched; 1 = mismatch(es); 2 = setup failure.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../src/pica_gl.c"

/* Defined by src/pica_vs.c in the runtime; stubbed here. */
static void pica_stage_stats_dump(void) {}

#define RT_W 64
#define RT_H 64

static unsigned char fb[RT_W * RT_H * 4];
static unsigned char cap_a[RT_W * RT_H * 4];
static unsigned char cap_b[RT_W * RT_H * 4];

static int host_rt_get(void *user, int x, int y, unsigned char rgba[4]) {
    (void)user;
    if (x < 0 || y < 0 || x >= RT_W || y >= RT_H) return 0;
    memcpy(rgba, fb + ((size_t)y * RT_W + x) * 4, 4);
    return 1;
}
static void host_rt_put(void *user, int x, int y, const unsigned char rgba[4]) {
    (void)user;
    if (x < 0 || y < 0 || x >= RT_W || y >= RT_H) return;
    memcpy(fb + ((size_t)y * RT_W + x) * 4, rgba, 4);
}
static const PicaGlHost host = { host_rt_get, host_rt_put, NULL, NULL, NULL,
                                 NULL, NULL, NULL, NULL };

static void set_vertex(PicaGlVertex *v, float x, float y, float zw, float r,
                       float g, float b, float a, float u, float vv) {
    v->x = x; v->y = y; v->iw = 1.0f; v->zw = zw;
    v->color[0] = r; v->color[1] = g; v->color[2] = b; v->color[3] = a;
    v->u = u; v->v = vv;
}
static void quad(PicaGlVertex *t, float x0, float y0, float x1, float y1,
                 float zw, float r, float g, float b, float a) {
    set_vertex(&t[0], x0, y0, zw, r, g, b, a, 0, 0);
    set_vertex(&t[1], x1, y0, zw, r, g, b, a, 1, 0);
    set_vertex(&t[2], x1, y1, zw, r, g, b, a, 1, 1);
    set_vertex(&t[3], x0, y0, zw, r, g, b, a, 0, 0);
    set_vertex(&t[4], x1, y1, zw, r, g, b, a, 1, 1);
    set_vertex(&t[5], x0, y1, zw, r, g, b, a, 0, 1);
}

static const unsigned char test_tex[2 * 2 * 4] = {
    255, 0, 0, 255,   0, 255, 0, 255,     /* GL row 0 (v~0): red, green */
    0, 0, 255, 255,   255, 255, 255, 255  /* GL row 1 (v~1): blue, white */
};

/* All six stages pass `previous` through, scale 1: the PICA reset state. */
static void tev_reset(PicaGlState *st) {
    int s, i;
    memset(&st->tev, 0, sizeof st->tev);
    for (s = 0; s < 6; ++s) {
        for (i = 0; i < 3; ++i) {
            st->tev.src_c[s * 3 + i] = 15;
            st->tev.src_a[s * 3 + i] = 15;
        }
        st->tev.scale_c[s] = 1.0f;
        st->tev.scale_a[s] = 1.0f;
    }
}
static void st_base(PicaGlState *st, int mode) {
    memset(st, 0, sizeof *st);
    st->mode = mode;
    st->rt_w = RT_W;
    st->rt_h = RT_H;
    st->color_mask = 15;
    st->tex_rgba = test_tex;
    st->tex_w = 2;
    st->tex_h = 2;
    st->wrap_s = st->wrap_t = 0;
    tev_reset(st);
}
/* stage s: colour = op(src_a, src_b, src_c) with per-operand modifiers */
static void tev_stage(PicaGlState *st, int s, int cop, int aop,
                      int sa, int ma, int sb, int mb, int sc, int mc) {
    st->tev.src_c[s * 3 + 0] = (unsigned)sa;
    st->tev.mod_c[s * 3 + 0] = (unsigned)ma;
    st->tev.src_c[s * 3 + 1] = (unsigned)sb;
    st->tev.mod_c[s * 3 + 1] = (unsigned)mb;
    st->tev.src_c[s * 3 + 2] = (unsigned)sc;
    st->tev.mod_c[s * 3 + 2] = (unsigned)mc;
    st->tev.src_a[s * 3 + 0] = (unsigned)sa;
    st->tev.mod_a[s * 3 + 0] = (unsigned)(ma & 7);
    st->tev.src_a[s * 3 + 1] = (unsigned)sb;
    st->tev.mod_a[s * 3 + 1] = (unsigned)(mb & 7);
    st->tev.src_a[s * 3 + 2] = (unsigned)sc;
    st->tev.mod_a[s * 3 + 2] = (unsigned)(mc & 7);
    st->tev.cop[s] = (unsigned)cop;
    st->tev.aop[s] = (unsigned)aop;
}

/* ---- cases ------------------------------------------------------------- */
enum { GEOM_ONE, GEOM_TWO_DEPTH };
typedef struct {
    const char *name;
    int geom;
    int fallback;   /* 1 = this shape must NOT be specialised (generic only) */
    void (*setup)(PicaGlState *st);
} Case;

static void c_modulate(PicaGlState *st) {
    st_base(st, PICA_GL_TEV);
    st->tev.tex_en = 1;
    tev_stage(st, 0, 1, 1, 0, 0, 3, 0, 15, 0);   /* prim * tex */
}
static void c_two_stage_const(PicaGlState *st) {
    st_base(st, PICA_GL_TEV);
    st->tev.tex_en = 1;
    tev_stage(st, 0, 1, 1, 0, 0, 3, 0, 15, 0);   /* prim * tex */
    tev_stage(st, 1, 2, 2, 15, 0, 14, 0, 15, 0); /* prev + const */
    st->tev.const_c[1][0] = 0.25f;
    st->tev.const_c[1][1] = 0.125f;
    st->tev.const_c[1][2] = 0.0625f;
    st->tev.const_c[1][3] = 0.5f;
}
static void c_ops_8_9(PicaGlState *st) {
    st_base(st, PICA_GL_TEV);
    st->tev.tex_en = 1;
    tev_stage(st, 0, 8, 1, 0, 0, 3, 0, 14, 0);   /* prim*tex + const */
    tev_stage(st, 1, 9, 2, 15, 3, 3, 0, 14, 0);  /* min(prev+tex,1)*const */
    st->tev.const_c[0][0] = 0.2f; st->tev.const_c[0][1] = 0.3f;
    st->tev.const_c[0][2] = 0.4f; st->tev.const_c[0][3] = 1.0f;
    st->tev.const_c[1][0] = 0.75f; st->tev.const_c[1][1] = 0.65f;
    st->tev.const_c[1][2] = 0.5f;  st->tev.const_c[1][3] = 1.0f;
}
static void c_dot(PicaGlState *st) {
    st_base(st, PICA_GL_TEV);
    st->tev.tex_en = 1;
    tev_stage(st, 0, 6, 1, 0, 0, 3, 0, 14, 0);   /* dot(prim, tex) */
    tev_stage(st, 1, 7, 0, 15, 2, 3, 5, 14, 0);  /* dot + alpha from x */
}
static void c_scale(PicaGlState *st) {
    st_base(st, PICA_GL_TEV);
    st->tev.tex_en = 1;
    tev_stage(st, 0, 1, 1, 0, 0, 3, 0, 15, 0);
    st->tev.scale_c[0] = 2.0f;
    st->tev.scale_a[0] = 4.0f;
    tev_stage(st, 1, 5, 5, 15, 0, 14, 0, 15, 0);  /* prev - const */
    st->tev.const_c[1][0] = 0.1f;
}
static void c_buffer(PicaGlState *st) {
    st_base(st, PICA_GL_TEV);
    st->tev.tex_en = 1;
    tev_stage(st, 0, 1, 1, 0, 0, 3, 0, 15, 0);   /* prim * tex */
    st->tev.buf_upd_c[0] = 1;
    st->tev.buf_upd_a[0] = 1;
    tev_stage(st, 1, 4, 2, 13, 0, 15, 0, 14, 0); /* mix(buf, prev, const) */
    st->tev.const_c[1][0] = 0.3f;
    st->tev.buf_init[0] = 0.5f; st->tev.buf_init[1] = 0.25f;
    st->tev.buf_init[2] = 0.125f; st->tev.buf_init[3] = 1.0f;
}
static void c_mods(PicaGlState *st) {
    st_base(st, PICA_GL_TEV);
    st->tev.tex_en = 1;
    tev_stage(st, 0, 1, 1, 0, 5, 3, 3, 15, 0);   /* (1-prim) * tex */
    tev_stage(st, 1, 2, 2, 15, 1, 3, 6, 14, 0);  /* -prev + tex.g + const */
    st->tev.const_c[1][0] = 0.05f; st->tev.const_c[1][1] = 0.1f;
    st->tev.const_c[1][2] = 0.15f; st->tev.const_c[1][3] = 1.0f;
}
static void c_solid_blend(PicaGlState *st) {
    st_base(st, PICA_GL_SOLID);
    st->blend = 1;
    st->blend_src_rgb = 6; st->blend_dst_rgb = 7;   /* src_alpha / 1-src */
    st->blend_src_a = 6;   st->blend_dst_a = 7;
    st->blend_eq_rgb = 0;  st->blend_eq_a = 0;
}
static void c_blend_const_min(PicaGlState *st) {
    st_base(st, PICA_GL_TEV);
    st->tev.tex_en = 1;
    tev_stage(st, 0, 1, 1, 0, 0, 3, 0, 15, 0);
    st->blend = 1;
    st->blend_src_rgb = 10; st->blend_dst_rgb = 11; /* const / 1-const */
    st->blend_src_a = 12;   st->blend_dst_a = 13;
    st->blend_eq_rgb = 3;   st->blend_eq_a = 4;     /* min / max */
    st->blend_const[0] = 0.75f; st->blend_const[1] = 0.5f;
    st->blend_const[2] = 0.25f; st->blend_const[3] = 0.5f;
}
static void c_blend_sub(PicaGlState *st) {
    st_base(st, PICA_GL_TEV);
    st->tev.tex_en = 1;
    tev_stage(st, 0, 2, 2, 0, 0, 3, 0, 15, 0);   /* prim + tex */
    st->blend = 1;
    st->blend_src_rgb = 2; st->blend_dst_rgb = 4; /* src / dest */
    st->blend_src_a = 1;   st->blend_dst_a = 0;
    st->blend_eq_rgb = 1;  st->blend_eq_a = 0;    /* subtract */
}
static void c_alpha_ge(PicaGlState *st) {
    st_base(st, PICA_GL_SOLID);
    st->alpha_test = 1;
    st->alpha_func = 7;
    st->alpha_ref = 128.0f;
}
static void c_alpha_lt(PicaGlState *st) {
    st_base(st, PICA_GL_TEV);
    st->tev.tex_en = 1;
    tev_stage(st, 0, 1, 1, 0, 0, 3, 0, 15, 0);
    st->alpha_test = 1;
    st->alpha_func = 4;
    st->alpha_ref = 140.0f;
}
/* depth: far quad first (2nd draw covers it with a nearer one) */
static void c_depth_less(PicaGlState *st) {
    st_base(st, PICA_GL_TEV);
    st->tev.tex_en = 1;
    tev_stage(st, 0, 1, 1, 0, 0, 3, 0, 15, 0);
    st->depth_on = 1;
    st->depth_test = 1;
    st->depth_write = 1;
    st->depth_func = 4;        /* LESS */
    st->depth_zbuf = 1;
    st->depth_scale = 1.0f;
    st->depth_offset = 0.0f;
    st->depth_zmax = 65535.0f;
    st->depth_bpp = 2;
}
static void c_depth_wbuf_noztest(PicaGlState *st) {
    st_base(st, PICA_GL_SOLID);
    st->depth_on = 1;
    st->depth_test = 0;        /* write-only: oldz must not matter */
    st->depth_write = 1;
    st->depth_zbuf = 0;        /* w-buffering: depth /= iw */
    st->depth_scale = 0.5f;
    st->depth_offset = 0.25f;
    st->depth_zmax = 16777215.0f;
    st->depth_bpp = 3;
}
static void c_tev_no_tex(PicaGlState *st) {
    st_base(st, PICA_GL_TEV);
    st->tev.tex_en = 0;        /* src 3 must read as vec4(0) */
    tev_stage(st, 0, 2, 2, 0, 0, 3, 0, 15, 0);
    st->alpha_test = 1;
    st->alpha_func = 1;        /* always pass */
}

static const Case cases[] = {
    { "tev modulate (1 stage)",        GEOM_ONE,       0, c_modulate },
    { "tev 2 stage + const",           GEOM_ONE,       0, c_two_stage_const },
    { "tev ops 8/9",                   GEOM_ONE,       0, c_ops_8_9 },
    { "tev dot 6/7",                   GEOM_ONE,       0, c_dot },
    { "tev scale 2/4",                 GEOM_ONE,       0, c_scale },
    { "tev buffer + mix",              GEOM_ONE,       0, c_buffer },
    { "tev operand modifiers",         GEOM_ONE,       0, c_mods },
    { "solid shader blend",            GEOM_ONE,       0, c_solid_blend },
    { "blend const + min/max",         GEOM_ONE,       0, c_blend_const_min },
    { "blend subtract",                GEOM_ONE,       0, c_blend_sub },
    { "alpha test >=",                 GEOM_ONE,       0, c_alpha_ge },
    { "alpha test <",                  GEOM_ONE,       0, c_alpha_lt },
    { "tev no texture unit",           GEOM_ONE,       0, c_tev_no_tex },
    { "depth LESS, far then near",     GEOM_TWO_DEPTH, 0, c_depth_less },
    { "depth w-buffer write only",     GEOM_TWO_DEPTH, 0, c_depth_wbuf_noztest },
};


/* Submit one case's geometry.  GEOM_TWO_DEPTH draws a full-target priming
   quad (depth write, test ALWAYS) and then a smaller quad at a different
   depth, so the second draw's old z is defined identically in both passes. */
static int submit(const Case *c, PicaGlState *st) {
    PicaGlVertex t[12];
    int ok = 1;
    if (c->geom == GEOM_ONE) {
        quad(t, 0, 0, RT_W, RT_H, 0.5f, 1.0f, 1.0f, 1.0f, 1.0f);
        ok = pica_gl_draw(t, 6, st, &host);
    } else {
        PicaGlState prim = *st;
        prim.blend = 0;
        prim.alpha_test = 0;
        prim.depth_test = 1;
        prim.depth_func = 1;                       /* ALWAYS */
        prim.depth_write = 1;
        prim.depth_zbuf = 1;
        prim.depth_scale = 1.0f;
        prim.depth_offset = 0.0f;
        quad(t, 0, 0, RT_W, RT_H, 0.75f, 0.45f, 0.55f, 0.65f, 1.0f);
        ok = pica_gl_draw(t, 6, &prim, &host) && ok;
        quad(t, 4, 4, RT_W - 4, RT_H - 4, 0.25f, 1.0f, 0.5f, 0.25f, 1.0f);
        ok = pica_gl_draw(t, 6, st, &host) && ok;
    }
    return ok;
}

int main(void) {
    int ci, fails = 0, checks = 0;
    setenv("DQ8_GL", "1", 0);
    if (!pica_gl_init()) {
        fprintf(stderr, "pica_gl_init failed (no EGL/GL)\n");
        return 2;
    }
    fprintf(stderr, "GL spec test: renderer = %s\n", pica_gl_renderer());
    if (getenv("DQ8_GL_SPEC_DEBUG")) pgl_spec_dbg = 1;
    for (ci = 0; ci < (int)(sizeof cases / sizeof cases[0]); ++ci) {
        const Case *c = &cases[ci];
        PicaGlState st;
        int pass, nspec_before = pgl_nspec;
        for (pass = 0; pass < 2; ++pass) {
            int y, x;
            /* identical starting host image for both passes */
            for (y = 0; y < RT_H; ++y)
                for (x = 0; x < RT_W; ++x) {
                    unsigned char *p = fb + ((size_t)y * RT_W + x) * 4;
                    p[0] = 10; p[1] = 20; p[2] = 30; p[3] = 255;
                }
            memset(&st, 0, sizeof st);
            c->setup(&st);
            pica_gl_spec_set(pass == 0 ? 1 : 0);
            if (!submit(c, &st)) {
                fprintf(stderr, "FAIL %-28s: draw rejected (spec=%d)\n",
                        c->name, pass == 0);
                return 1;
            }
            memcpy(pass == 0 ? cap_a : cap_b, fb, sizeof fb);
        }
        {
            int nbad = 0, fx = -1, fy = -1;
            int i;
            for (i = 0; i < RT_W * RT_H; ++i) {
                if (memcmp(cap_a + i * 4, cap_b + i * 4, 4) != 0) {
                    if (!nbad) { fx = i % RT_W; fy = i / RT_W; }
                    ++nbad;
                }
            }
            ++checks;
            if (c->fallback && pgl_nspec != nspec_before) {
                ++fails;
                fprintf(stderr, "FAIL %-28s expected the generic fallback, "
                        "but a specialised program was generated\n", c->name);
            }
            if (nbad) {
                int i0 = ((size_t)fy * RT_W + fx) * 4;
                ++fails;
                fprintf(stderr, "FAIL %-28s %d px differ (first %d,%d "
                        "spec=%u,%u,%u,%u generic=%u,%u,%u,%u)\n",
                        c->name, nbad, fx, fy,
                        cap_a[i0], cap_a[i0 + 1], cap_a[i0 + 2], cap_a[i0 + 3],
                        cap_b[i0], cap_b[i0 + 1], cap_b[i0 + 2], cap_b[i0 + 3]);
            } else {
                fprintf(stderr, "ok   %-28s identical (%d px)\n", c->name,
                        RT_W * RT_H);
            }
        }
    }
    pica_gl_shutdown();
    fprintf(stderr, "GL spec test: %d cases, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
