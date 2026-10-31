/* pica_gl.c — experimental OpenGL rasterization backend for PICA draws.
 *
 * Self-contained: only libc + EGL (for its own context) are required. GL entry
 * points are loaded at runtime through eglGetProcAddress or a caller-provided
 * loader, so no GL headers or link-time GL symbols are needed.
 *
 * Design (first hardware-draw slice):
 *  - The guest pipeline (attribute decode + uploaded vertex program) still runs
 *    on the CPU and produces screen-space PicaGlVertex triangles, exactly the
 *    same vertices the software rasterizer consumes.
 *  - This backend rasterizes those triangles on the GPU into an RGBA8 FBO the
 *    size of the PICA render target, blending against the target's current
 *    contents, then reads the result back and hands it to the host through
 *    rt_get/rt_put so guest memory stays authoritative for display transfers.
 *
 * Limitations (documented, by design for this slice):
 *  - Texture decode (tiling/formats) stays CPU-side; the staged RGBA8 image is
 *    sampled on the GPU.
 *  - The TEV combiner chain is not translated; callers only submit draws whose
 *    fragment math the backend can express (solid or texture-modulate).
 *  - Depth/stencil are not implemented.
 */
#ifndef PICA_GL_C
#define PICA_GL_C

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <math.h>
#include <time.h> /* pica_pqt_now_ns: CLOCK_MONOTONIC (was template-provided) */

typedef struct {
    /* MUST stay field-for-field identical to the software path's PicaVertex
       (pica_vs.c): pica_flush_triangles() copies triangles into this struct
       with a plain memcpy of sizeof(PicaGlVertex) bytes.  zw is never read by
       the GL backend (there is no depth buffer yet), but DROPPING it here
       silently shifted every later field by one float: color[] received
       (zw,r,g,b), u received the vertex alpha and v received u -- so every
       TEXTURE_MODULATE draw sampled the staged texture at (u=prim_alpha, v=u),
       e.g. the intro movie's quads landed on u=1.0 (black texture padding)
       and rendered black.  Keep the field; a _Static_assert in pica_vs.c
       guards the layout. */
    float x, y, iw, zw, color[4], u, v, cx, cy, cz, cw;
} PicaGlVertex; /* layout-compatible with the software path's PicaVertex */

/* VBO payload (t_bc6b4eb7): ONE record per TRIANGLE, read by all 3 rendered
   vertices through divisor-1 attributes (instanced draw), so every FLAT
   input of the fragment shader receives the full triangle exactly like the
   old layout where each vertex carried a 3x-expanded copy (38 floats =
   152 B/vertex -> 456 B/triangle).  Only the varyings the shader consumes
   are kept: gNc needs (u, v) only — the old vec4 carried two always-zero
   padding floats.  120 B/triangle, written once, uploaded once. */
typedef struct {
    float a[3][4]; /* gNa: (x, y, iw, zw) of triangle vertex 0/1/2 */
    float b[3][4]; /* gNb: color[4] of triangle vertex 0/1/2 */
    float c[3][2]; /* gNc: (u, v) of triangle vertex 0/1/2 */
} PicaGlTriPayload;
_Static_assert(sizeof(PicaGlTriPayload) == 120, "payload layout");
_Static_assert(offsetof(PicaGlTriPayload, b) == 48 &&
               offsetof(PicaGlTriPayload, c) == 96, "payload field offsets");

enum { PICA_GL_SOLID = 0, PICA_GL_TEV = 1 };

/* PICA TEV combiner chain, mirroring pica_tev() in src/pica_vs.c exactly
   (same registers, same operand/modifier decoding).  The fragment shader
   evaluates this chain with the same float expression order, so the GL math
   matches the software rasterizer bit-for-bit on IEEE GPUs. */
typedef struct {
    unsigned src_c[18], mod_c[18]; /* 3 color operands x 6 stages */
    unsigned src_a[18], mod_a[18]; /* 3 alpha operands x 6 stages */
    unsigned cop[6], aop[6];       /* color/alpha combine ops 0..15 */
    float scale_c[6], scale_a[6];  /* post-op shift: 1, 2 or 4 */
    unsigned buf_upd_c[6], buf_upd_a[6]; /* buffer update flags (reg 0xE0) */
    float const_c[6][4];           /* per-stage constant color (reg base+3) */
    float buf_init[4];             /* TEV buffer initial color (reg 0xFD) */
    int tex_en;                    /* texture unit 0 enabled (reg 0x80 bit0) */
} PicaTev;

typedef struct {
    int mode;                    /* PICA_GL_SOLID | PICA_GL_TEV */
    const unsigned char *tex_rgba; /* texture0, row 0 = GL v=0 */
    int tex_w, tex_h;
    int blend;                   /* 0 = replace, 1 = factors below */
    /* PICA blend codes exactly as pica_write_fragment() consumes them
       (factor 0..15, equation 0..7), NOT GL enums: the fragment shader
       reproduces the software blend math bit-exactly. */
    unsigned blend_src_rgb, blend_dst_rgb, blend_src_a, blend_dst_a;
    unsigned blend_eq_rgb, blend_eq_a;
    float blend_const[4];
    int alpha_test;              /* 0 = off, 1 = on */
    unsigned alpha_func;         /* 0..7, software codes (1=always) */
    float alpha_ref;             /* 0..255 */
    unsigned char color_mask;    /* bit0=R .. bit3=A write enable */
    unsigned rt_w, rt_h;
    unsigned rt_bpp;             /* guest bytes per color pixel (2..4) */
    unsigned tex_hash;           /* FNV-1a of tex_rgba[0..tex_w*tex_h*4) */
    /* t_66c89cf2: staging-slot identity of tex_rgba.  tex_gen is bumped by
       the staging cache on every decode of that slot, so (slot,gen) is exact
       identity of the decoded bytes and the GL upload is skipped without a
       byte comparison.  tex_gen == 0 means "no staging identity" (cache
       disabled / legacy path). */
    unsigned tex_slot;
    unsigned long long tex_gen;
    /* PICA wrap codes for texture0 (0=clamp-edge, 1=clamp-border, 2=repeat,
       3=mirror; reg 0x83 bits 12..14 = s, 8..10 = t).  Modes 0/2/3 map onto
       the GL sampler (CLAMP_TO_EDGE / REPEAT / MIRRORED_REPEAT) 1:1 for the
       NEAREST-filtered staged texture, so u,v may leave [0,1] there
       (t_77ac52ce); mode 1 (border colour) still runs in software. */
    unsigned wrap_s, wrap_t;
    PicaTev tev;                 /* PICA_GL_TEV */
    /* Depth test/write (mirrors pica_depth_test_write/pica_fragment_depth):
       the fragment shader computes z/w interpolation exactly like the
       software rasterizer (canonical shared edges) and tests/writes an
       R32F depth buffer storing integer z values (exact). */
    int depth_on;                /* depth buffer bound and test or write on */
    int depth_test, depth_write;
    unsigned depth_func;         /* 0..7 software compare codes */
    int depth_zbuf;              /* 1 = z-buffering, 0 = w-buffering */
    float depth_scale, depth_offset;
    float depth_zmax;            /* 65535.0 (16-bit) or 16777215.0 (24-bit) */
    unsigned depth_bpp;          /* guest bytes per depth value (2 or 3) */
} PicaGlState;

typedef struct {
    /* Read current render-target pixel (x, y with y=0 at the BOTTOM, GL
       raster orientation). Return 0 to abort the GL draw. */
    int (*rt_get)(void *user, int x, int y, unsigned char rgba[4]);
    /* Write final pixel, same y orientation as rt_get. */
    void (*rt_put)(void *user, int x, int y, const unsigned char rgba[4]);
    void *user;
    /* Batched-readback support (see docs/ogl-backend.md): raw guest bytes of
       the whole render target and the byte offset of each pixel's first byte
       (same x/y orientation as rt_get/rt_put).  Both present = the backend
       may keep the render target in its FBO across draws and defer the
       write-back to guest memory until a consumer needs it.  Both NULL =
       per-draw upload/readback (the pre-batching behaviour). */
    int (*rt_raw)(void *user, const unsigned char **base, unsigned *len);
    unsigned (*rt_px_off)(void *user, int x, int y);
    /* Depth buffer access, same conventions as above (z = raw integer depth
       value 0..0xFFFF / 0xFFFFFF).  When depth_* are present and
       PicaGlState.depth_on is set, depth test/write runs on the GPU. */
    int (*depth_get)(void *user, int x, int y, unsigned *z);
    void (*depth_put)(void *user, int x, int y, unsigned z);
    int (*depth_raw)(void *user, const unsigned char **base, unsigned *len);
    unsigned (*depth_px_off)(void *user, int x, int y);
} PicaGlHost;

/* DQ8_TRACE_TIME phase accounting (t_399eab58): wall time in each stage of
   a GL round-trip draw, aggregated here and reported by the runtime's
   pica_pqt_report (pica_vs.c).  Defined unconditionally so a non-GL build
   still links; only pica_gl_draw_locked (GL build) fills them. */
static unsigned long long pgl_t_n;
static unsigned long long pgl_t_read_ns, pgl_t_upload_ns, pgl_t_draw_ns,
                          pgl_t_readback_ns, pgl_t_write_ns;
static unsigned long long pica_pqt_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ull +
           (unsigned long long)ts.tv_nsec;
}

#ifdef DQ8_ENABLE_GL

#include <EGL/egl.h>
/* t_cb8723ae: GL worker thread split (DQ8_SPLIT_GL=1).  In the spliced
   runtime TU pthread/stdatomic/time are already included by the template,
   but pica_gl.c also compiles standalone (tools/run_gl_test.sh). */
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

/* Mesa's surfaceless platform; not in the core egl.h on all distros. */
#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif
typedef EGLDisplay (*pgl_eglGetPlatformDisplayEXT_fn)(EGLenum, void *, const EGLint *);

/* ---- minimal GL loader (values are fixed ABI constants) ---- */
typedef unsigned int PGuint;
typedef int PGint;
typedef unsigned int PGenum;
typedef int PGsizei;
typedef float PGfloat;
typedef unsigned char PGboolean;
typedef char PGchar;
typedef long PGsizeiptr;
typedef unsigned char PGubyte;

#define PGGL_COLOR_BUFFER_BIT     0x00004000u
#define PGGL_TRIANGLES            0x0004u
#define PGGL_UNSIGNED_BYTE        0x1401u
#define PGGL_FLOAT                0x1406u
#define PGGL_COLOR                0x1800u
#define PGGL_RGBA                 0x1908u
#define PGGL_RGBA8                0x8058u
#define PGGL_TEXTURE_2D           0x0DE1u
#define PGGL_TEXTURE0             0x84C0u
#define PGGL_TEXTURE1             0x84C1u
#define PGGL_TEXTURE2             0x84C2u
#define PGGL_TEXTURE_MIN_FILTER   0x2801u
#define PGGL_TEXTURE_MAG_FILTER   0x2800u
#define PGGL_NEAREST              0x2600u
#define PGGL_TEXTURE_WRAP_S       0x2802u
#define PGGL_TEXTURE_WRAP_T       0x2803u
#define PGGL_CLAMP_TO_EDGE        0x812Fu
#define PGGL_REPEAT               0x2901u
#define PGGL_MIRRORED_REPEAT      0x8370u
#define PGGL_ARRAY_BUFFER         0x8892u
#define PGGL_DYNAMIC_DRAW         0x88E8u
#define PGGL_VERTEX_SHADER        0x8B31u
#define PGGL_FRAGMENT_SHADER      0x8B30u
#define PGGL_COMPILE_STATUS       0x8B81u
#define PGGL_LINK_STATUS          0x8B82u
#define PGGL_FRAMEBUFFER          0x8D40u
#define PGGL_COLOR_ATTACHMENT0    0x8CE0u
#define PGGL_COLOR_ATTACHMENT1    0x8CE1u
#define PGGL_FRAMEBUFFER_COMPLETE 0x8CD5u
#define PGGL_NONE                 0u
#define PGGL_RED                  0x1903u
#define PGGL_R32F                 0x822Eu
#define PGGL_READ_ONLY            0x88B8u  /* t_8c05fac8 */
#define PGGL_NUM_EXTENSIONS       0x821Du  /* t_8c05fac8 */
#define PGGL_EXT_STR              0x1F03u  /* GL_EXTENSIONS */
#define PGGL_BLEND                0x0BE2u
#define PGGL_PACK_ALIGNMENT       0x0D05u
#define PGGL_UNPACK_ALIGNMENT     0x0CF5u
#define PGGL_VENDOR               0x1F00u
#define PGGL_RENDERER             0x1F01u
#define PGGL_VERSION              0x1F02u
#define PGGL_GLSL_VERSION         0x8B8Cu
#define PGGL_READ_BUFFER          0x0C02u
#define PGGL_CONTEXT_PROFILE_MASK 0x9126u
#define PGGL_CONTEXT_CORE_PROFILE_MASK 0x00000001u
/* Fixed-function blending (t_77ac52ce): PICA factor/equation codes map 1:1
   onto these for every factor except 14 (src-alpha-sat). */
#define PGGL_ZERO                      0x0000u
#define PGGL_ONE                       0x0001u
#define PGGL_SRC_COLOR                 0x0300u
#define PGGL_ONE_MINUS_SRC_COLOR       0x0301u
#define PGGL_SRC_ALPHA                 0x0302u
#define PGGL_ONE_MINUS_SRC_ALPHA       0x0303u
#define PGGL_DST_ALPHA                 0x0304u
#define PGGL_ONE_MINUS_DST_ALPHA       0x0305u
#define PGGL_DST_COLOR                 0x0306u
#define PGGL_ONE_MINUS_DST_COLOR       0x0307u
#define PGGL_CONSTANT_COLOR            0x8001u
#define PGGL_ONE_MINUS_CONSTANT_COLOR  0x8002u
#define PGGL_CONSTANT_ALPHA            0x8003u
#define PGGL_ONE_MINUS_CONSTANT_ALPHA  0x8004u
#define PGGL_FUNC_ADD                  0x8006u
#define PGGL_MIN                       0x8007u
#define PGGL_MAX                       0x8008u
#define PGGL_FUNC_SUBTRACT             0x800Au
#define PGGL_FUNC_REVERSE_SUBTRACT     0x800Bu

typedef const PGubyte *(*PFN_glGetString)(PGenum);
typedef PGuint (*PFN_glCreateShader)(PGenum);
typedef void (*PFN_glShaderSource)(PGuint, PGsizei, const PGchar *const *, const PGint *);
typedef void (*PFN_glCompileShader)(PGuint);
typedef void (*PFN_glGetShaderiv)(PGuint, PGenum, PGint *);
typedef void (*PFN_glGetShaderInfoLog)(PGuint, PGsizei, PGsizei *, PGchar *);
typedef PGuint (*PFN_glCreateProgram)(void);
typedef void (*PFN_glAttachShader)(PGuint, PGuint);
typedef void (*PFN_glLinkProgram)(PGuint);
typedef void (*PFN_glGetProgramiv)(PGuint, PGenum, PGint *);
typedef void (*PFN_glGetProgramInfoLog)(PGuint, PGsizei, PGsizei *, PGchar *);
typedef void (*PFN_glDeleteShader)(PGuint);
typedef void (*PFN_glUseProgram)(PGuint);
typedef PGint (*PFN_glGetUniformLocation)(PGuint, const PGchar *);
typedef void (*PFN_glUniform1i)(PGint, PGint);
typedef void (*PFN_glUniform1iv)(PGint, PGsizei, const PGint *);
typedef void (*PFN_glUniform1f)(PGint, PGfloat);
typedef void (*PFN_glUniform1fv)(PGint, PGsizei, const PGfloat *);
typedef void (*PFN_glUniform2f)(PGint, PGfloat, PGfloat);
typedef void (*PFN_glUniform4f)(PGint, PGfloat, PGfloat, PGfloat, PGfloat);
typedef void (*PFN_glGenBuffers)(PGsizei, PGuint *);
typedef void (*PFN_glBindBuffer)(PGenum, PGuint);
typedef void (*PFN_glBufferData)(PGenum, PGsizeiptr, const void *, PGenum);
typedef void (*PFN_glDeleteBuffers)(PGsizei, const PGuint *);
typedef void (*PFN_glGenVertexArrays)(PGsizei, PGuint *);
typedef void (*PFN_glBindVertexArray)(PGuint);
typedef void (*PFN_glDeleteVertexArrays)(PGsizei, const PGuint *);
typedef void (*PFN_glEnableVertexAttribArray)(PGuint);
typedef void (*PFN_glVertexAttribPointer)(PGuint, PGint, PGenum, PGboolean, PGsizei, const void *);
typedef void (*PFN_glVertexAttribDivisor)(PGuint, PGuint);
typedef void (*PFN_glDrawArraysInstanced)(PGenum, PGint, PGsizei, PGsizei);
typedef void (*PFN_glBufferSubData)(PGenum, PGsizeiptr, PGsizeiptr, const void *);
typedef void (*PFN_glGetBufferSubData)(PGenum, PGsizeiptr, PGsizeiptr, void *);
typedef void (*PFN_glGetVertexAttribiv)(PGuint, PGenum, PGint *);
typedef void (*PFN_glGenTextures)(PGsizei, PGuint *);
typedef void (*PFN_glBindTexture)(PGenum, PGuint);
typedef void (*PFN_glTexImage2D)(PGenum, PGint, PGint, PGsizei, PGsizei, PGint, PGenum, PGenum, const void *);
typedef void (*PFN_glTexSubImage2D)(PGenum, PGint, PGint, PGint, PGsizei, PGsizei, PGenum, PGenum, const void *);
typedef void (*PFN_glCopyTexSubImage2D)(PGenum, PGint, PGint, PGint, PGint, PGint, PGsizei, PGsizei);
typedef void (*PFN_glTexParameteri)(PGenum, PGenum, PGint);
typedef void (*PFN_glActiveTexture)(PGenum);
typedef void (*PFN_glDeleteTextures)(PGsizei, const PGuint *);
typedef void (*PFN_glGenFramebuffers)(PGsizei, PGuint *);
typedef void (*PFN_glBindFramebuffer)(PGenum, PGuint);
typedef void (*PFN_glFramebufferTexture2D)(PGenum, PGenum, PGenum, PGuint, PGint);
typedef PGenum (*PFN_glCheckFramebufferStatus)(PGenum);
typedef void (*PFN_glDeleteFramebuffers)(PGsizei, const PGuint *);
typedef void (*PFN_glViewport)(PGint, PGint, PGsizei, PGsizei);
typedef void (*PFN_glClearColor)(PGfloat, PGfloat, PGfloat, PGfloat);
typedef void (*PFN_glClear)(PGenum);
typedef void (*PFN_glEnable)(PGenum);
typedef void (*PFN_glDisable)(PGenum);
typedef void (*PFN_glBlendFuncSeparate)(PGenum, PGenum, PGenum, PGenum);
typedef void (*PFN_glBlendEquationSeparate)(PGenum, PGenum);
typedef void (*PFN_glBlendColor)(PGfloat, PGfloat, PGfloat, PGfloat);
typedef void (*PFN_glColorMask)(PGboolean, PGboolean, PGboolean, PGboolean);
typedef void (*PFN_glDrawArrays)(PGenum, PGint, PGsizei);
typedef void (*PFN_glDrawBuffers)(PGsizei, const PGenum *);
typedef void (*PFN_glClearBufferfv)(PGenum, PGint, const PGfloat *);
typedef void (*PFN_glReadPixels)(PGint, PGint, PGsizei, PGsizei, PGenum, PGenum, void *);
typedef void (*PFN_glPixelStorei)(PGenum, PGint);
typedef void (*PFN_glReadBuffer)(PGenum);
typedef void (*PFN_glGetIntegerv)(PGenum, PGint *);
typedef PGenum (*PFN_glGetError)(void);
typedef PGboolean (*PFN_glIsTexture)(PGuint);
typedef PGboolean (*PFN_glIsFramebuffer)(PGuint);
typedef PGboolean (*PFN_glIsProgram)(PGuint);
typedef PGboolean (*PFN_glIsVertexArray)(PGuint);
typedef PGboolean (*PFN_glIsBuffer)(PGuint);
typedef void (*PFN_glBindImageTexture)(PGuint, PGuint, PGint, PGboolean, PGint,
                                       PGenum, PGenum);
typedef const PGubyte *(*PFN_glGetStringi)(PGenum, PGuint);

#define PGL_FN(name) static PFN_##name pgl_##name
PGL_FN(glGetString);
PGL_FN(glGetStringi);      /* t_8c05fac8: extension query (soft-loaded) */
PGL_FN(glBindImageTexture); /* t_8c05fac8: framebuffer fetch (soft-loaded) */
PGL_FN(glCreateShader);
PGL_FN(glShaderSource);
PGL_FN(glCompileShader);
PGL_FN(glGetShaderiv);
PGL_FN(glGetShaderInfoLog);
PGL_FN(glCreateProgram);
PGL_FN(glAttachShader);
PGL_FN(glLinkProgram);
PGL_FN(glGetProgramiv);
PGL_FN(glGetProgramInfoLog);
PGL_FN(glDeleteShader);
PGL_FN(glUseProgram);
PGL_FN(glGetUniformLocation);
PGL_FN(glUniform1i);
PGL_FN(glUniform1iv);
PGL_FN(glUniform1f);
PGL_FN(glUniform1fv);
PGL_FN(glUniform2f);
PGL_FN(glUniform4f);
PGL_FN(glGenBuffers);
PGL_FN(glBindBuffer);
PGL_FN(glBufferData);
PGL_FN(glBufferSubData);
PGL_FN(glGetBufferSubData);
PGL_FN(glGetVertexAttribiv);
PGL_FN(glDeleteBuffers);
PGL_FN(glGenVertexArrays);
PGL_FN(glBindVertexArray);
PGL_FN(glDeleteVertexArrays);
PGL_FN(glEnableVertexAttribArray);
PGL_FN(glVertexAttribPointer);
PGL_FN(glVertexAttribDivisor);
PGL_FN(glGenTextures);
PGL_FN(glBindTexture);
PGL_FN(glTexImage2D);
PGL_FN(glTexSubImage2D);
PGL_FN(glCopyTexSubImage2D);
PGL_FN(glTexParameteri);
PGL_FN(glActiveTexture);
PGL_FN(glDeleteTextures);
PGL_FN(glGenFramebuffers);
PGL_FN(glBindFramebuffer);
PGL_FN(glFramebufferTexture2D);
PGL_FN(glCheckFramebufferStatus);
PGL_FN(glDeleteFramebuffers);
PGL_FN(glViewport);
PGL_FN(glClearColor);
PGL_FN(glClear);
PGL_FN(glEnable);
PGL_FN(glDisable);
PGL_FN(glBlendFuncSeparate);
PGL_FN(glBlendEquationSeparate);
PGL_FN(glBlendColor);
PGL_FN(glColorMask);
PGL_FN(glDrawArrays);
PGL_FN(glDrawArraysInstanced);
PGL_FN(glDrawBuffers);
PGL_FN(glClearBufferfv);
PGL_FN(glReadPixels);
PGL_FN(glPixelStorei);
PGL_FN(glReadBuffer);
PGL_FN(glGetIntegerv);
PGL_FN(glGetError);
PGL_FN(glIsTexture);
PGL_FN(glIsFramebuffer);
PGL_FN(glIsProgram);
PGL_FN(glIsVertexArray);
PGL_FN(glIsBuffer);

#undef PGL_FN

typedef void *(*PicaGlGetProc)(const char *name);

static void pica_gl_shutdown(void);

/* Saved thread-current EGL state for context isolation (see pgl_context_take). */
typedef struct {
    EGLDisplay dpy;
    EGLSurface draw, read;
    EGLContext ctx;
} PglSavedCtx;
static void pgl_context_save(PglSavedCtx *s);
static int pgl_context_take(PglSavedCtx *s);
static void pgl_context_put(const PglSavedCtx *s);
static void pica_gl_context_release(void);
static void pica_stage_stats_dump(void); /* pica_vs.c staging-cache counters */

static int pgl_ready;
static int pgl_tried;
static int pgl_external_ctx;
static EGLDisplay pgl_dpy = EGL_NO_DISPLAY;
static EGLContext pgl_ctx = EGL_NO_CONTEXT;
static PGuint pgl_program, pgl_vao, pgl_vbo, pgl_fbo, pgl_rt_tex,
             pgl_dst_tex, pgl_z_tex, pgl_zdst_tex;
static int pgl_rt_w, pgl_rt_h;
static PGint pgl_u_rt, pgl_u_mode, pgl_u_tex, pgl_u_alpha_test,
             pgl_u_alpha_func, pgl_u_alpha_ref, pgl_u_tex_en, pgl_u_dst,
             pgl_u_dst_off, pgl_u_zdst, pgl_u_dst_used, pgl_u_zdst_used,
             pgl_u_dstimg, pgl_u_zimg;
static PGint pgl_u_src_c, pgl_u_mod_c, pgl_u_src_a, pgl_u_mod_a, pgl_u_cop,
             pgl_u_aop, pgl_u_scale_c, pgl_u_scale_a, pgl_u_const_c,
             pgl_u_buf_init, pgl_u_buf_upd_c, pgl_u_buf_upd_a;
static PGint pgl_u_blend_en, pgl_u_eq_rgb, pgl_u_eq_a, pgl_u_f_src_rgb,
             pgl_u_f_dst_rgb, pgl_u_f_src_a, pgl_u_f_dst_a, pgl_u_blend_const;
static PGint pgl_u_depth_on, pgl_u_depth_test, pgl_u_depth_write,
             pgl_u_depth_func, pgl_u_depth_zbuf, pgl_u_depth_scale,
             pgl_u_depth_offset, pgl_u_depth_zmax;
static unsigned long long pgl_draws;
static unsigned long long pgl_rebuilds;   /* vanished-object repairs */
static unsigned long long pgl_displaces;  /* foreign context was current */
static unsigned pgl_obj_used; /* 1=program 2=vao 4=vbo 8=fbo 16=rt_tex 32=stage_tex
                                 64=dst_tex (objects are only glIs*-checkable
                                 once bound) */
/* ---- t_bc6b4eb7: pre-allocated streaming geometry ring -------------------
   One fixed-size GL VBO + one client staging buffer.  Each draw packs its
   triangles into the ring and uploads with a single glBufferSubData (rewind =
   glBufferData orphan + cursor 0) instead of a per-draw malloc / glBufferData
   orphan / free.  8 MB covers any realistic draw (the 65536-vertex input cap
   is ~2.6 MB of payload even without clip fan-out); an oversized draw falls
   back to the one-off path below, which stays correct. */
#define PGL_VBO_CAP (8u * 1024u * 1024u)
static unsigned char *pgl_vbo_stage; /* client staging, PGL_VBO_CAP bytes */
static size_t pgl_vbo_off;           /* ring write cursor */
static size_t pgl_vbo_alloc;         /* GL store currently sized PGL_VBO_CAP */
static char pgl_renderer[256];
static int pgl_debug;                 /* DQ8_GL_DEBUG=1 */
static EGLSurface pgl_surf = EGL_NO_SURFACE; /* pbuffer fallback surface */
static const char *pgl_dpy_source = "default-display";
static const char *pgl_ctx_path = "none";     /* surfaceless | pbuffer */
static __thread char pgl_tls_marker;  /* address identifies the OS thread */
static const void *pgl_thread_id;     /* &pgl_tls_marker at context init */
/* ---- staged-texture upload cache (multi-slot, t_86b8f1f9; deepened
   t_66c89cf2) ------------------------------------------------------------
   One GL texture object per STAGING SLOT.  The staging cache in pica_vs.c
   owns one stable decoded image per distinct texture content and bumps its
   generation counter on every (re)decode, so "same slot id + same generation"
   is exact identity of the decoded bytes: the upload is skipped with no
   per-draw memcmp and no duplicate byte copy (the A15 hash-collision rule is
   satisfied by construction — a hit means no decode happened at all).
   Draws that bypass the staging cache (DQ8_GL_TEXCACHE=0 or a failed stage)
   fall back to the legacy pointer/hash/byte-compare path. */
/* t_66c89cf2: 96 -> 256 — must cover PICA_STAGE_SLOTS (256) so every
   resident staging slot keeps ITS GL texture object for process lifetime
   (slot-indexed identity, no LRU churn on the GL side). */
#define PGL_UP_SLOTS 256
typedef struct {
    PGuint tex;
    const unsigned char *ptr;
    int w, h;
    unsigned hash;
    unsigned long long gen;   /* staging-slot generation of the uploaded image */
    unsigned char *copy;   /* exact bytes of the last upload (legacy path) */
    size_t bytes;
    unsigned long long lru;
    int used;
} PglUpSlot;
static PglUpSlot pgl_up[PGL_UP_SLOTS];
static unsigned long long pgl_up_lru;
static unsigned long long pgl_tex_uploads;
static unsigned long long pgl_tex_upload_bytes;
static unsigned long long pgl_tex_hits, pgl_tex_misses;
static unsigned long long pgl_tex_gen_hits;  /* (slot,gen) identity hits */
static unsigned long long pgl_tex_subuploads; /* same-size re-spec via SubImage */

/* ---- context hold (t_86b8f1f9) ------------------------------------------
   The pre-change wrapper displaced the foreign (SDL) context around EVERY
   draw — 2 eglMakeCurrent per hardware draw (displacements == draws).  When
   DQ8_GL_CTX_HOLD!=0 (default) our context stays current across a contiguous
   burst of GL work and the foreign context is restored only at the release
   points (pica_gl_flush — every consumer of guest-visible pixels, i.e. GSP
   transfer/capture/scan-out/DMA/software draws — and the SDL present path),
   so a frame pays ~2 MakeCurrent instead of 2 per draw.  DQ8_GL_CTX_HOLD=0
   restores the per-draw save/restore exactly. */
static int pgl_hold_on = 1;
static int pgl_hold_active;
static PglSavedCtx pgl_held;
static unsigned long long pgl_hold_releases, pgl_hold_mid;

/* ---- batched readback state (see docs/ogl-backend.md) ------------------
   The FBO holds the render target across draws ("episode").  pgl_shadow is
   a copy of the raw guest bytes the FBO currently reflects; guest-side
   writes are detected by comparing the two before each draw (and localized
   to changed 8x8 Morton blocks / a uniform pattern fast path).  The
   write-back to guest memory is deferred until a consumer needs it
   (pica_gl_flush).  DQ8_GL_BATCH=0 forces a flush after every draw, i.e.
   the pre-batching per-draw behaviour. */
static int pgl_batch_on;
static unsigned char *pgl_shadow;
static unsigned pgl_shadow_len;
static unsigned pgl_shadow_w, pgl_shadow_h, pgl_shadow_bpp;
static int pgl_pending; /* FBO content not yet written back to guest memory */
static int pgl_force_full; /* FBO was rebuilt: re-upload everything */
static int pgl_dst_w, pgl_dst_h; /* u_dst blend-source texture storage */
static int pgl_zdst_w, pgl_zdst_h;
/* depth-region shadow (raw guest bytes), same sync design as the color
   region but coarser: memcmp fast path, uniform-z clear fast path, full
   re-upload otherwise (fills dominate depth-region writes). */
static unsigned char *pgl_zshadow;
static unsigned pgl_zshadow_len;
static unsigned pgl_zshadow_w, pgl_zshadow_h, pgl_zshadow_bpp;
static int pgl_zforce_full;
static int pgl_zdirty; /* depth draws since the last write-back */
static unsigned long long pgl_zrefills, pgl_zflushes;
static int pgl_drawn_x0, pgl_drawn_y0, pgl_drawn_x1, pgl_drawn_y1; /* GL y */
static int pgl_rect_trace; /* DQ8_GL_RECT_TRACE=1 */
static int pgl_zclear_on = 1; /* DQ8_GL_ZCLEAR=0: legacy no-op clear (reference) */
/* ---- DQ8_GL_SKIP diagnostic knobs (t_77ac52ce profiling) ----
   Comma/space separated list of stages to SKIP, used only to attribute the
   per-draw wall time to a stage (visual output is wrong on purpose):
     nodst  - skip the per-draw u_dst blend-source copy
     nozdst - skip the per-draw u_zdst depth-source copy
     nogeo  - skip the copies AND glDrawArrays (measures pure per-draw CPU
              + sync overhead with no rasterization at all) */
#define PGL_SK_DST  1
#define PGL_SK_ZDST 2
#define PGL_SK_GEO  4
static int pgl_skip_mask;
/* Phase clock: per-draw time split into refresh/state, dst copies, raster. */
static unsigned long long pgl_ph_n, pgl_ph_setup_ns, pgl_ph_copy_ns,
                          pgl_ph_draw_ns;
/* t_8c05fac8: dst vs zdst copy split + copy counts (which side drains). */
static unsigned long long pgl_ph_dst_ns, pgl_ph_zdst_ns;
/* t_8c05fac8 round 2: draw_ms sub-split (uniforms+tex / vbo+attribs / submit). */
static unsigned long long pgl_ph_u_ns, pgl_ph_vbo_ns, pgl_ph_sub_ns;
static unsigned long long pgl_dst_copies, pgl_zdst_copies;
/* Depth draw shape: how many depth draws actually READ u_zdst in the
   shader (oldz feeds the depth test and frag_z when depth_write==0). */
static unsigned long long pgl_d_on, pgl_d_test, pgl_d_oldz_used;
/* t_8c05fac8: what does the u_dst blend math actually consume?
     ident  - fs=1 fd=0 eq=ADD on all 4 channels -> value==source exactly
              (u_blend_en can be 0, no u_dst read needed)
     nodest - fd=0 on all channels and eq in {ADD,SUB,RSUB} per channel ->
              dest mathematically irrelevant to every channel (still may
              scale source), no u_dst read needed
     read   - dest actually consumed: must copy the draw box. */
static unsigned long long pgl_b_ident, pgl_b_nodest_only, pgl_b_read;
static unsigned long long pgl_blend_gl_draws, pgl_blend_sw_draws;
static int pgl_blend_gl_on;       /* opt-in: DQ8_GL_BLENDS=1 */
/* t_8c05fac8: framebuffer fetch.  1 = the fragment shader imageLoads the
   render-target textures directly (GL_ARB_shader_image_load_store, no
   per-draw FBO read => no llvmpipe queue drain); 0 = legacy per-draw
   glCopyTexSubImage2D path.  Decided once in pgl_setup from
   DQ8_GL_FBFETCH (default on) + extension presence + DQ8_GL_DSTALWAYS. */
static int pgl_fbfetch;
/* t_cb8723ae: guest-thread enqueue accounting for the GL worker split
   (GLPHASE enq_ms).  Atomic because the worker's pgl_ph_report reads/resets
   them while the guest thread bumps them (stats only; relaxed). */
static _Atomic unsigned long long pgl_enq_n, pgl_enq_ns, pgl_enq_full_waits;
static unsigned long long pgl_split_draw_fail; /* worker: draw_locked returned 0 */
static double pgl_ph_last;
static void pgl_ph_report(void) {
    double now = (double)pica_pqt_now_ns() / 1e9;
    if (pgl_ph_last == 0.0) { pgl_ph_last = now; return; }
    if (now - pgl_ph_last < 2.0 || !pgl_ph_n) return;
    fprintf(stderr, "GLPHASE draws=%llu setup_ms=%.1f copy_ms=%.1f draw_ms=%.1f "
            "mean_us=%.0f skip=0x%X blend_gl=%llu blend_sw=%llu "
            "dst_ms=%.1f zdst_ms=%.1f dst_n=%llu zdst_n=%llu "
            "d_on=%llu d_test=%llu d_oldz=%llu "
            "b_read=%llu b_ident=%llu b_nodst=%llu "
            "u_ms=%.1f vbo_ms=%.1f sub_ms=%.1f "
            "enq_n=%llu enq_ms=%.1f fullwait_ms=%.1f split_fail=%llu\n", pgl_ph_n,
            (double)pgl_ph_setup_ns / 1e6, (double)pgl_ph_copy_ns / 1e6,
            (double)pgl_ph_draw_ns / 1e6,
            (double)(pgl_ph_setup_ns + pgl_ph_copy_ns + pgl_ph_draw_ns) /
                (double)pgl_ph_n / 1e3, pgl_skip_mask,
            pgl_blend_gl_draws, pgl_blend_sw_draws,
            (double)pgl_ph_dst_ns / 1e6, (double)pgl_ph_zdst_ns / 1e6,
            pgl_dst_copies, pgl_zdst_copies,
            pgl_d_on, pgl_d_test, pgl_d_oldz_used,
            pgl_b_read, pgl_b_ident, pgl_b_nodest_only,
            (double)pgl_ph_u_ns / 1e6, (double)pgl_ph_vbo_ns / 1e6,
            (double)pgl_ph_sub_ns / 1e6,
            (unsigned long long)atomic_load_explicit(&pgl_enq_n, memory_order_relaxed),
            (double)atomic_load_explicit(&pgl_enq_ns, memory_order_relaxed) / 1e6,
            (double)atomic_load_explicit(&pgl_enq_full_waits, memory_order_relaxed) / 1e6,
            pgl_split_draw_fail);
    pgl_ph_n = pgl_ph_setup_ns = pgl_ph_copy_ns = pgl_ph_draw_ns = 0;
    pgl_ph_dst_ns = pgl_ph_zdst_ns = 0;
    pgl_ph_u_ns = pgl_ph_vbo_ns = pgl_ph_sub_ns = 0;
    atomic_store_explicit(&pgl_enq_n, 0, memory_order_relaxed);
    atomic_store_explicit(&pgl_enq_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&pgl_enq_full_waits, 0, memory_order_relaxed);
    pgl_dst_copies = pgl_zdst_copies = 0;
    pgl_d_on = pgl_d_test = pgl_d_oldz_used = 0;
    pgl_b_read = pgl_b_ident = pgl_b_nodest_only = 0;
    pgl_ph_last = now;
}
static unsigned long long pgl_flushes;  /* deferred write-backs */
static unsigned long long pgl_refills;  /* guest-side refresh passes */
static unsigned long long pgl_cleared;  /* uniform-fill fast clears */

static const void *pgl_tid(void) { return (const void *)&pgl_tls_marker; }

/* ---- t_cb8723ae: GL worker thread split (DQ8_SPLIT_GL=1) ----------------
   llvmpipe serializes every draw whose program has images bound to the active
   FBO attachments (feedback: sub_ms 478 ms / 2 s block, bind-vs-unbound
   isolation 487 vs 79 ms) — submits block the guest thread.  With the split
   the guest thread only prepares and enqueues; a dedicated worker thread owns
   the EGL context, the FBO, the upload cache and every glDrawArrays, so the
   feedback stall hides behind the queue.  Soundness (settled in t_8c05fac8):

   - push-time refresh: the pgl_refresh MEMCMP against the GUEST bytes runs
     on the guest thread at enqueue; only the changed 8x8 block bytes /
     uniform clear-color / full decode cross the queue (uploads are rare:
     0 refresh events in 48 traced frames).  The worker never touches guest
     memory: pica_vs.c's staging-decode path calls pica_gl_split_sync() (a
     queue drain) before overwriting any slot's staged bytes, and the flush
     write-back (host->rt_put) runs back on the guest thread after done_seq.
   - texture identity: the op carries (slot,gen,ptr,w,h); the worker's
     upload cache revalidates it exactly as the synchronous path does.
   - pica_gl_flush = FLUSH op + condvar wait on done_seq; the worker only
     reads the FBO back into owned buffers.
   - DQ8_SPLIT_GL=0 (default) keeps the synchronous path byte-for-byte. */
typedef struct PglSplitOp PglSplitOp;
static int pgl_split_on;        /* split active (set on the guest thread at init) */
static PglSplitOp *pgl_split_cur; /* worker-only: op being processed */
static _Atomic int pgl_split_resync; /* worker -> guest: FBO needs full refresh */
static unsigned long long pgl_split_flush_ops; /* FLUSH ops read back (worker) */
static unsigned pica_gl_draw_split(const PicaGlVertex *verts, unsigned nverts,
                                   const PicaGlState *st, const PicaGlHost *host);
static void pgl_split_flush(const PicaGlHost *host);
static void pgl_split_stop(void);
static int pgl_split_start(void);
static int pgl_split_apply_refresh(const PicaGlState *st);
static void pica_gl_shutdown(void);

/* The FBO content became untrustworthy (read-back error, lost objects).
   Synchronous path: set the force-full flags directly (same thread owns the
   shadows).  Split path: the GL worker only raises the atomic; the guest
   thread folds it into its own force-full flags at the next enqueue. */
static void pgl_request_resync(void) {
    if (pgl_split_on) {
        atomic_store_explicit(&pgl_split_resync, 1, memory_order_release);
    } else {
        pgl_force_full = 1;
        pgl_zforce_full = 1;
    }
}

static const char *pgl_err_name(PGenum e) {
    switch (e) {
    case 0x0500u: return "GL_INVALID_ENUM";
    case 0x0501u: return "GL_INVALID_VALUE";
    case 0x0502u: return "GL_INVALID_OPERATION";
    case 0x0505u: return "GL_OUT_OF_MEMORY";
    case 0x0506u: return "GL_INVALID_FRAMEBUFFER_OPERATION";
    case 0x0507u: return "GL_STACK_OVERFLOW";
    case 0x0508u: return "GL_STACK_UNDERFLOW";
    default:      return "unknown";
    }
}

/* Drain the context error flag so no stale error can be misattributed to a
   later stage (glGetError returns one flag per call). Always drains — the
   call is a local state read, no driver round-trip; logs each exact GLenum
   with its stage when DQ8_GL_DEBUG=1. Returns the number of errors drained;
   *first_err (optional) receives the first code seen. */
static unsigned pgl_drain_errors(const char *stage, PGenum *first_err) {
    unsigned count = 0;
    PGenum e;
    while ((e = pgl_glGetError()) != 0) {
        if (count == 0 && first_err) *first_err = e;
        if (pgl_debug)
            fprintf(stderr, "GL debug: %s: err=0x%04X %s (draw #%llu)\n",
                    stage, (unsigned)e, pgl_err_name(e),
                    (unsigned long long)pgl_draws);
        if (++count >= 64u) break; /* pathological loop guard */
    }
    return count;
}

static const char pgl_vs_src[] =
    "#version 330 core\n"
    /* t_bc6b4eb7: instanced per-triangle payload.  Each of the 3 vertices of
       an instance reads the SAME 120-byte record (divisor 1), so gNa/gNb/gNc
       — read FLAT by the fragment shader — are bit-identical for all 3
       vertices exactly like the old 3x-expanded layout.  Only this corner's
       screen position varies: it comes from the same record via gl_VertexID
       (0/1/2), which replaces the old a_pos attribute. */
    "layout(location=1) in vec4 f0a;\n"
    "layout(location=2) in vec4 f0b;\n"
    "layout(location=3) in vec4 f0c;\n"
    "layout(location=4) in vec4 f1a;\n"
    "layout(location=5) in vec4 f1b;\n"
    "layout(location=6) in vec4 f1c;\n"
    "layout(location=7) in vec4 f2a;\n"
    "layout(location=8) in vec4 f2b;\n"
    "layout(location=9) in vec4 f2c;\n"
    "uniform vec2 u_rt;\n"
    "flat out vec4 g0a, g0b, g0c, g1a, g1b, g1c, g2a, g2b, g2c;\n"
    "void main() {\n"
    "    vec2 p = (gl_VertexID == 0) ? f0a.xy\n"
    "           : (gl_VertexID == 1) ? f1a.xy : f2a.xy;\n"
    "    vec2 ndc = p / u_rt * 2.0 - 1.0;\n"
    "    gl_Position = vec4(ndc, 0.0, 1.0);\n" /* screen-space rasterization */
    "    g0a = f0a; g0b = f0b; g0c = f0c;\n"
    "    g1a = f1a; g1b = f1b; g1c = f1c;\n"
    "    g2a = f2a; g2b = f2b; g2c = f2c;\n"
    "}\n";

static const char pgl_fs_src[] =
    "#version 330 core\n"
    "flat in vec4 g0a, g0b, g0c, g1a, g1b, g1c, g2a, g2b, g2c;\n"
    "uniform sampler2D u_tex;\n"
    "uniform sampler2D u_dst;\n"
    "uniform sampler2D u_zdst;\n"
    "uniform int u_dst_used;\n"
    "uniform int u_zdst_used;\n"
    /* t_8c05fac8 framebuffer fetch: the RT textures bound as read-only
       images (image units 0/1, set once after link). */
    "#if DQ8_FBFETCH\n"
    "layout(rgba8) uniform readonly image2D u_dstimg;\n"
    "layout(r32f) uniform readonly image2D u_zimg;\n"
    "#endif\n"
    "uniform int u_mode;\n"        /* 0 = passthrough (solid), 1 = full TEV */
    "uniform int u_tex_en;\n"
    "uniform vec2 u_dst_off;\n"
    "uniform int u_alpha_test;\n"
    "uniform int u_alpha_func;\n"
    "uniform float u_alpha_ref;\n"
    /* Depth test/write (mirrors pica_fragment_depth/pica_depth_test_write). */
    "uniform int u_depth_on;\n"
    "uniform int u_depth_test;\n"
    "uniform int u_depth_write;\n"
    "uniform int u_depth_func;\n"
    "uniform int u_depth_zbuf;\n"
    "uniform float u_depth_scale;\n"
    "uniform float u_depth_offset;\n"
    "uniform float u_depth_zmax;\n"
    /* Full PICA TEV chain (mirrors pica_tev in src/pica_vs.c exactly). */
    "uniform int u_src_c[18];\n"
    "uniform int u_mod_c[18];\n"
    "uniform int u_src_a[18];\n"
    "uniform int u_mod_a[18];\n"
    "uniform int u_cop[6];\n"
    "uniform int u_aop[6];\n"
    "uniform float u_scale_c[6];\n"
    "uniform float u_scale_a[6];\n"
    "uniform vec4 u_const_c[6];\n"
    "uniform vec4 u_buf_init;\n"
    "uniform int u_buf_upd_c[6];\n"
    "uniform int u_buf_upd_a[6];\n"
    /* Blend (mirrors pica_write_fragment/pica_blend_factor exactly). */
    "uniform int u_blend_en;\n"
    "uniform int u_eq_rgb;\n"
    "uniform int u_eq_a;\n"
    "uniform int u_f_src_rgb;\n"
    "uniform int u_f_dst_rgb;\n"
    "uniform int u_f_src_a;\n"
    "uniform int u_f_dst_a;\n"
    "uniform vec4 u_blend_const;\n"
    "out vec4 frag;\n"
    "out float frag_z;\n"
    "float sat(float x) { return min(1.0, max(0.0, x)); }\n"
    "vec4 sel_src(int s, vec4 prim, vec4 tex, vec4 prev, vec4 buf, vec4 cst) {\n"
    "    if (s == 0) return prim;\n"
    "    if (s == 3) return tex;\n"
    "    if (s == 13) return buf;\n"
    "    if (s == 14) return cst;\n"
    "    if (s == 15) return prev;\n"
    "    return vec4(0.0);\n"
    "}\n"
    "vec3 mod_color(int m, vec4 v) {\n"
    "    float x;\n"
    "    if (m < 2) return (m & 1) != 0 ? vec3(1.0) - v.rgb : v.rgb;\n"
    "    x = (m < 4) ? v[3] : (m < 6) ? v[0] : (m < 10) ? v[1] : v[2];\n"
    "    return vec3((m & 1) != 0 ? 1.0 - x : x);\n"
    "}\n"
    "float mod_alpha(int m, vec4 v) {\n"
    "    int ch = (m < 2) ? 3 : (m / 2 - 1);\n"
    "    float x = v[ch];\n"
    "    return ((m & 1) != 0) ? 1.0 - x : x;\n"
    "}\n"
    "float tev_op(int op, float a, float b, float c) {\n"
    "    if (op == 0) return a;\n"
    "    if (op == 1) return a * b;\n"
    "    if (op == 2) return a + b;\n"
    "    if (op == 3) return a + b - 0.5;\n"
    "    if (op == 4) return a * c + b * (1.0 - c);\n"
    "    if (op == 5) return a - b;\n"
    "    if (op == 8) return a * b + c;\n"
    "    if (op == 9) return min(a + b, 1.0) * c;\n"
    "    return 0.0;\n"
    "}\n"
    "vec4 run_tev(vec4 prim, vec4 tex) {\n"
    "    vec4 previous = prim;\n"
    "    vec4 buffer = u_buf_init;\n"
    "    vec4 pending = buffer;\n"
    "    for (int s = 0; s < 6; ++s) {\n"
    "        vec4 cst = u_const_c[s];\n"
    "        vec3 col[3];\n"
    "        float alpha[3];\n"
    "        for (int i = 0; i < 3; ++i) {\n"
    "            vec4 v = sel_src(u_src_c[s*3+i], prim, tex, previous, buffer, cst);\n"
    "            col[i] = mod_color(u_mod_c[s*3+i], v);\n"
    "            v = sel_src(u_src_a[s*3+i], prim, tex, previous, buffer, cst);\n"
    "            alpha[i] = mod_alpha(u_mod_a[s*3+i], v);\n"
    "        }\n"
    "        int cop = u_cop[s];\n"
    "        vec3 next3;\n"
    "        float nexta;\n"
    "        if (cop == 6 || cop == 7) {\n"
    "            float d = 0.0;\n"
    "            for (int k = 0; k < 3; ++k) d += (col[0][k] - 0.5) * (col[1][k] - 0.5) * 4.0;\n"
    "            next3 = vec3(sat(d));\n"
    "        } else {\n"
    "            for (int k = 0; k < 3; ++k) next3[k] = sat(tev_op(cop, col[0][k], col[1][k], col[2][k]));\n"
    "        }\n"
    "        nexta = (cop == 7) ? next3[0] : sat(tev_op(u_aop[s], alpha[0], alpha[1], alpha[2]));\n"
    "        vec4 next = vec4(next3, nexta);\n"
    "        for (int k = 0; k < 3; ++k) next[k] = sat(next[k] * u_scale_c[s]);\n"
    "        next[3] = sat(next[3] * u_scale_a[s]);\n"
    "        buffer = pending;\n"
    "        if (s < 4) {\n"
    "            if (u_buf_upd_c[s] != 0) pending.rgb = next.rgb;\n"
    "            if (u_buf_upd_a[s] != 0) pending[3] = next[3];\n"
    "        }\n"
    "        previous = next;\n"
    "    }\n"
    "    return previous;\n"
    "}\n"
    "float blend_factor(int f, int k, vec4 s, vec4 d, vec4 c) {\n"
    "    if (f == 0) return 0.0;\n"
    "    if (f == 1) return 1.0;\n"
    "    if (f == 2) return s[k];\n"
    "    if (f == 3) return 1.0 - s[k];\n"
    "    if (f == 4) return d[k];\n"
    "    if (f == 5) return 1.0 - d[k];\n"
    "    if (f == 6) return s[3];\n"
    "    if (f == 7) return 1.0 - s[3];\n"
    "    if (f == 8) return d[3];\n"
    "    if (f == 9) return 1.0 - d[3];\n"
    "    if (f == 10) return c[k];\n"
    "    if (f == 11) return 1.0 - c[k];\n"
    "    if (f == 12) return c[3];\n"
    "    if (f == 13) return 1.0 - c[3];\n"
    "    if (f == 14) return k == 3 ? 1.0 : min(s[3], 1.0 - d[3]);\n"
    "    return 0.0;\n"
    "}\n"
    "float edge_canon(vec2 u, vec2 v, vec2 p) {\n"
    "    bool flip = (v.x < u.x) || (v.x == u.x && v.y < u.y);\n"
    "    vec2 a = flip ? v : u;\n"
    "    vec2 b = flip ? u : v;\n"
    "    float e = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);\n"
    "    return flip ? -e : e;\n"
    "}\n"
    "void main() {\n"
    "    vec2 px = gl_FragCoord.xy;\n" /* (x+0.5, y+0.5), same as software */
    /* Interpolation with the software rasterizer's exact expressions
       (pica_raster_triangle: canonical shared edges, perspective-correct
       color/uv weights, LINEAR z/w and 1/w for depth). */
    "    float e0 = edge_canon(g1a.xy, g2a.xy, px);\n"
    "    float e1 = edge_canon(g2a.xy, g0a.xy, px);\n"
    "    float e2 = edge_canon(g0a.xy, g1a.xy, px);\n"
    "    float w0 = e0 * g0a.z, w1 = e1 * g1a.z, w2 = e2 * g2a.z;\n"
    "    float sum = w0 + w1 + w2;\n"
    "    if (abs(sum) < 1e-20) discard;\n"
    "    w0 /= sum; w1 /= sum; w2 /= sum;\n"
    "    float lsum = e0 + e1 + e2;\n"
    "    float zw = (lsum != 0.0) ? (e0 * g0a.w + e1 * g1a.w + e2 * g2a.w) / lsum : g0a.w;\n"
    "    float iw = (lsum != 0.0) ? (e0 * g0a.z + e1 * g1a.z + e2 * g2a.z) / lsum : g0a.z;\n"
    /* Primary color saturates to [0,1] BEFORE the TEV chain, exactly like
       the software path (pica_vs.c raster loop: pica_sat on the
       interpolated primary color, then TEV).  Clamping only the final
       result is NOT equivalent (A14). */
    "    vec4 prim;\n"
    "    prim[0] = sat(w0 * g0b[0] + w1 * g1b[0] + w2 * g2b[0]);\n"
    "    prim[1] = sat(w0 * g0b[1] + w1 * g1b[1] + w2 * g2b[1]);\n"
    "    prim[2] = sat(w0 * g0b[2] + w1 * g1b[2] + w2 * g2b[2]);\n"
    "    prim[3] = sat(w0 * g0b[3] + w1 * g1b[3] + w2 * g2b[3]);\n"
    "    float tu = w0 * g0c[0] + w1 * g1c[0] + w2 * g2c[0];\n"
    "    float tv = w0 * g0c[1] + w1 * g1c[1] + w2 * g2c[1];\n"
    "    vec4 texel = (u_tex_en != 0) ? texture(u_tex, vec2(tu, tv)) : vec4(0.0);\n"
    "    vec4 source = (u_mode == 1) ? run_tev(prim, texel) : clamp(g0b, 0.0, 1.0);\n"
    "    if (u_alpha_test != 0) {\n"
    "        float a = floor(source[3] * 255.0 + 0.5);\n"
    "        bool pass = false;\n"
    "        if (u_alpha_func == 1) pass = true;\n"
    "        else if (u_alpha_func == 2) pass = (a == u_alpha_ref);\n"
    "        else if (u_alpha_func == 3) pass = (a != u_alpha_ref);\n"
    "        else if (u_alpha_func == 4) pass = (a < u_alpha_ref);\n"
    "        else if (u_alpha_func == 5) pass = (a <= u_alpha_ref);\n"
    "        else if (u_alpha_func == 6) pass = (a > u_alpha_ref);\n"
    "        else if (u_alpha_func == 7) pass = (a >= u_alpha_ref);\n"
    "        if (!pass) discard;\n"
    "    }\n"
    /* Depth test/write (pica_write_fragment runs it before blending; a
       failed test writes nothing at all).  z = trunc(depth * zmax) is kept
       as an exact integer-valued float in the R32F buffer. */
    "    if (u_depth_on != 0) {\n"
    "        float depth = zw * u_depth_scale + u_depth_offset;\n"
    "        if (u_depth_zbuf == 0 && iw > 0.0) depth /= iw;\n"
    "        depth = sat(depth);\n"
    "        float zf = floor(depth * u_depth_zmax);\n"
    "#if DQ8_FBFETCH\n"
    "        float oldz = (u_zdst_used != 0) ? imageLoad(u_zimg, ivec2(px)).r : 0.0;\n"
    "#else\n"
    "        float oldz = (u_zdst_used != 0) ? texelFetch(u_zdst, ivec2(px) - ivec2(u_dst_off), 0).r : 0.0;\n"
    "#endif\n"
    "        if (u_depth_test != 0) {\n"
    "            bool pass;\n"
    "            if (u_depth_func == 0) pass = false;\n"
    "            else if (u_depth_func == 1) pass = true;\n"
    "            else if (u_depth_func == 2) pass = (zf == oldz);\n"
    "            else if (u_depth_func == 3) pass = (zf != oldz);\n"
    "            else if (u_depth_func == 4) pass = (zf < oldz);\n"
    "            else if (u_depth_func == 5) pass = (zf <= oldz);\n"
    "            else if (u_depth_func == 6) pass = (zf > oldz);\n"
    "            else pass = (zf >= oldz);\n"
    "            if (!pass) discard;\n"
    "        }\n"
    "        frag_z = (u_depth_write != 0) ? zf : oldz;\n"
    "    } else {\n"
    "        frag_z = 0.0;\n"
    "    }\n"
    /* Blend against the current render-target contents (copied into u_dst
       per draw box) with the software path's exact float math; the result is
       quantized like pica_write_fragment (floor(sat(v)*255+0.5)).  Fixed-
       function GL blending is NOT used: it blends already-quantized source
       values and can differ from the software path at the 1-LSB level. */
    "    vec4 value = source;\n"
    "    if (u_blend_en != 0) {\n"
    "#if DQ8_FBFETCH\n"
    "        vec4 dest = (u_dst_used != 0) ? imageLoad(u_dstimg, ivec2(px)) : vec4(0.0);\n"
    "#else\n"
    "        vec4 dest = (u_dst_used != 0) ? texelFetch(u_dst, ivec2(px) - ivec2(u_dst_off), 0) : vec4(0.0);\n"
    "#endif\n"
    "        for (int k = 0; k < 4; ++k) {\n"
    "            int eq = (k == 3) ? u_eq_a : u_eq_rgb;\n"
    "            float fs = blend_factor((k == 3) ? u_f_src_a : u_f_src_rgb, k, source, dest, u_blend_const);\n"
    "            float fd = blend_factor((k == 3) ? u_f_dst_a : u_f_dst_rgb, k, source, dest, u_blend_const);\n"
    "            float sv = source[k] * fs;\n"
    "            float dv = dest[k] * fd;\n"
    "            if (eq == 0) value[k] = sv + dv;\n"
    "            else if (eq == 1) value[k] = sv - dv;\n"
    "            else if (eq == 2) value[k] = dv - sv;\n"
    "            else if (eq == 3) value[k] = min(source[k], dest[k]);\n"
    "            else value[k] = max(source[k], dest[k]);\n"
    "        }\n"
    "    }\n"
    "    frag = vec4(floor(sat(value[0]) * 255.0 + 0.5),\n"
    "                floor(sat(value[1]) * 255.0 + 0.5),\n"
    "                floor(sat(value[2]) * 255.0 + 0.5),\n"
    "                floor(sat(value[3]) * 255.0 + 0.5)) / 255.0;\n"
    "}\n";

static int pgl_load(PicaGlGetProc getproc) {
#define PGL_LOAD(name) \
    do { pgl_##name = (PFN_##name)getproc(#name); \
         if (!pgl_##name) { fprintf(stderr, "GL backend: missing %s\n", #name); return 0; } } while (0)
    PGL_LOAD(glGetString);
    PGL_LOAD(glCreateShader);
    PGL_LOAD(glShaderSource);
    PGL_LOAD(glCompileShader);
    PGL_LOAD(glGetShaderiv);
    PGL_LOAD(glGetShaderInfoLog);
    PGL_LOAD(glCreateProgram);
    PGL_LOAD(glAttachShader);
    PGL_LOAD(glLinkProgram);
    PGL_LOAD(glGetProgramiv);
    PGL_LOAD(glGetProgramInfoLog);
    PGL_LOAD(glDeleteShader);
    PGL_LOAD(glUseProgram);
    PGL_LOAD(glGetUniformLocation);
    PGL_LOAD(glUniform1i);
    PGL_LOAD(glUniform1iv);
    PGL_LOAD(glUniform1f);
    PGL_LOAD(glUniform1fv);
    PGL_LOAD(glUniform2f);
    PGL_LOAD(glUniform4f);
    PGL_LOAD(glGenBuffers);
    PGL_LOAD(glBindBuffer);
    PGL_LOAD(glBufferData);
    PGL_LOAD(glBufferSubData);
    /* optional (diagnostic dump only): NULL simply disables the dump */
    pgl_glGetBufferSubData = (PFN_glGetBufferSubData)getproc("glGetBufferSubData");
    pgl_glGetVertexAttribiv = (PFN_glGetVertexAttribiv)getproc("glGetVertexAttribiv");
    PGL_LOAD(glDeleteBuffers);
    PGL_LOAD(glGenVertexArrays);
    PGL_LOAD(glBindVertexArray);
    PGL_LOAD(glDeleteVertexArrays);
    PGL_LOAD(glEnableVertexAttribArray);
    PGL_LOAD(glVertexAttribPointer);
    PGL_LOAD(glVertexAttribDivisor);
    PGL_LOAD(glGenTextures);
    PGL_LOAD(glBindTexture);
    PGL_LOAD(glTexImage2D);
    PGL_LOAD(glTexSubImage2D);
    PGL_LOAD(glCopyTexSubImage2D);
    PGL_LOAD(glTexParameteri);
    PGL_LOAD(glActiveTexture);
    PGL_LOAD(glDeleteTextures);
    PGL_LOAD(glGenFramebuffers);
    PGL_LOAD(glBindFramebuffer);
    PGL_LOAD(glFramebufferTexture2D);
    PGL_LOAD(glCheckFramebufferStatus);
    PGL_LOAD(glDeleteFramebuffers);
    PGL_LOAD(glViewport);
    PGL_LOAD(glClearColor);
    PGL_LOAD(glClear);
    PGL_LOAD(glEnable);
    PGL_LOAD(glDisable);
    PGL_LOAD(glBlendFuncSeparate);
    PGL_LOAD(glBlendEquationSeparate);
    PGL_LOAD(glBlendColor);
    PGL_LOAD(glColorMask);
    PGL_LOAD(glDrawArrays);
    PGL_LOAD(glDrawArraysInstanced);
    PGL_LOAD(glDrawBuffers);
    PGL_LOAD(glClearBufferfv);
    PGL_LOAD(glReadPixels);
    PGL_LOAD(glPixelStorei);
    PGL_LOAD(glReadBuffer);
    PGL_LOAD(glGetIntegerv);
    PGL_LOAD(glGetError);
    PGL_LOAD(glIsTexture);
    PGL_LOAD(glIsFramebuffer);
    PGL_LOAD(glIsProgram);
    PGL_LOAD(glIsVertexArray);
    PGL_LOAD(glIsBuffer);
#undef PGL_LOAD
    /* t_8c05fac8 (soft): optional GL 4.2 entry points for framebuffer fetch;
       absence keeps the per-draw copy path, never fails the backend. */
    pgl_glGetStringi = (PFN_glGetStringi)getproc("glGetStringi");
    pgl_glBindImageTexture =
        (PFN_glBindImageTexture)getproc("glBindImageTexture");
    return 1;
}

static PGuint pgl_compile(PGenum type, const char *src) {
    PGuint sh = pgl_glCreateShader(type);
    PGint ok = 0;
    if (!sh) {
        fprintf(stderr, "GL backend: glCreateShader(%u) failed err=0x%04X\n",
                (unsigned)type, (unsigned)pgl_glGetError());
        return 0;
    }
    pgl_glShaderSource(sh, 1, &src, NULL);
    pgl_glCompileShader(sh);
    pgl_glGetShaderiv(sh, PGGL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        PGsizei n = -1;
        memset(log, 0, sizeof log);
        pgl_glGetShaderInfoLog(sh, sizeof log, &n, log);
        fprintf(stderr, "GL backend: shader(type=%u) compile failed "
                "(loglen=%d err=0x%04X):\n%s\n",
                (unsigned)type, (int)n, (unsigned)pgl_glGetError(), log);
        return 0;
    }
    return sh;
}

/* (Re)build the shader program and refresh uniform locations.  Called at
   init, and again from pgl_ensure_objects() when another GL user on this
   thread (SDL's window machinery with an OPENGL-capable driver) has deleted
   our program while its context was current. */
static int pgl_build_program(void) {
    PGuint vs, fs;
    PGint ok = 0;
    char *fs_buf = NULL;
    const char *fs_src = pgl_fs_src;
    if (pgl_program && pgl_glIsProgram(pgl_program)) return 1;
    /* t_8c05fac8: inject the framebuffer-fetch preamble right after the
       #version line.  The extension directive must precede other
       non-preprocessor tokens; DQ8_FBFETCH gates the imageLoad/texelFetch
       variants in the source.  On allocation failure fall back to the copy
       path (define 0 keeps the shader compiling). */
    {
        const char *nl = strchr(pgl_fs_src, '\n');
        size_t head = nl ? (size_t)(nl - pgl_fs_src + 1) : 0;
        const char *pre = pgl_fbfetch
            ? "#extension GL_ARB_shader_image_load_store : require\n"
              "#define DQ8_FBFETCH 1\n"
            : "#define DQ8_FBFETCH 0\n";
        size_t pre_len = strlen(pre), tail_len = strlen(pgl_fs_src + head);
        fs_buf = (char *)malloc(head + pre_len + tail_len + 1);
        if (fs_buf) {
            memcpy(fs_buf, pgl_fs_src, head);
            memcpy(fs_buf + head, pre, pre_len);
            memcpy(fs_buf + head + pre_len, pgl_fs_src + head, tail_len + 1);
            fs_src = fs_buf;
        } else if (pgl_fbfetch) {
            pgl_fbfetch = 0;
            fprintf(stderr, "GL backend: fs preamble alloc failed -> "
                    "framebuffer fetch off\n");
        }
    }
    vs = pgl_compile(PGGL_VERTEX_SHADER, pgl_vs_src);
    fs = pgl_compile(PGGL_FRAGMENT_SHADER, fs_src);
    free(fs_buf);
    if (!vs || !fs) return 0;
    pgl_program = pgl_glCreateProgram();
    pgl_glAttachShader(pgl_program, vs);
    pgl_glAttachShader(pgl_program, fs);
    pgl_glLinkProgram(pgl_program);
    pgl_glGetProgramiv(pgl_program, PGGL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        PGsizei n = 0;
        memset(log, 0, sizeof log);
        pgl_glGetProgramInfoLog(pgl_program, sizeof log, &n, log);
        fprintf(stderr, "GL backend: program link failed:\n%.*s\n", (int)n, log);
        return 0;
    }
    if (pgl_debug)
        fprintf(stderr, "GL debug: program %u linked ok\n", pgl_program);
    pgl_obj_used |= 1u;
    pgl_glDeleteShader(vs);
    pgl_glDeleteShader(fs);
    pgl_u_rt = pgl_glGetUniformLocation(pgl_program, "u_rt");
    pgl_u_mode = pgl_glGetUniformLocation(pgl_program, "u_mode");
    pgl_u_tex = pgl_glGetUniformLocation(pgl_program, "u_tex");
    pgl_u_alpha_test = pgl_glGetUniformLocation(pgl_program, "u_alpha_test");
    pgl_u_alpha_func = pgl_glGetUniformLocation(pgl_program, "u_alpha_func");
    pgl_u_alpha_ref = pgl_glGetUniformLocation(pgl_program, "u_alpha_ref");
    pgl_u_tex_en = pgl_glGetUniformLocation(pgl_program, "u_tex_en");
    pgl_u_dst = pgl_glGetUniformLocation(pgl_program, "u_dst");
    pgl_u_dst_off = pgl_glGetUniformLocation(pgl_program, "u_dst_off");
    pgl_u_dst_used = pgl_glGetUniformLocation(pgl_program, "u_dst_used");
    pgl_u_zdst_used = pgl_glGetUniformLocation(pgl_program, "u_zdst_used");
    pgl_u_src_c = pgl_glGetUniformLocation(pgl_program, "u_src_c");
    pgl_u_mod_c = pgl_glGetUniformLocation(pgl_program, "u_mod_c");
    pgl_u_src_a = pgl_glGetUniformLocation(pgl_program, "u_src_a");
    pgl_u_mod_a = pgl_glGetUniformLocation(pgl_program, "u_mod_a");
    pgl_u_cop = pgl_glGetUniformLocation(pgl_program, "u_cop");
    pgl_u_aop = pgl_glGetUniformLocation(pgl_program, "u_aop");
    pgl_u_scale_c = pgl_glGetUniformLocation(pgl_program, "u_scale_c");
    pgl_u_scale_a = pgl_glGetUniformLocation(pgl_program, "u_scale_a");
    pgl_u_const_c = pgl_glGetUniformLocation(pgl_program, "u_const_c");
    pgl_u_buf_init = pgl_glGetUniformLocation(pgl_program, "u_buf_init");
    pgl_u_buf_upd_c = pgl_glGetUniformLocation(pgl_program, "u_buf_upd_c");
    pgl_u_buf_upd_a = pgl_glGetUniformLocation(pgl_program, "u_buf_upd_a");
    pgl_u_blend_en = pgl_glGetUniformLocation(pgl_program, "u_blend_en");
    pgl_u_eq_rgb = pgl_glGetUniformLocation(pgl_program, "u_eq_rgb");
    pgl_u_eq_a = pgl_glGetUniformLocation(pgl_program, "u_eq_a");
    pgl_u_f_src_rgb = pgl_glGetUniformLocation(pgl_program, "u_f_src_rgb");
    pgl_u_f_dst_rgb = pgl_glGetUniformLocation(pgl_program, "u_f_dst_rgb");
    pgl_u_f_src_a = pgl_glGetUniformLocation(pgl_program, "u_f_src_a");
    pgl_u_f_dst_a = pgl_glGetUniformLocation(pgl_program, "u_f_dst_a");
    pgl_u_blend_const = pgl_glGetUniformLocation(pgl_program, "u_blend_const");
    pgl_u_zdst = pgl_glGetUniformLocation(pgl_program, "u_zdst");
    pgl_u_depth_on = pgl_glGetUniformLocation(pgl_program, "u_depth_on");
    pgl_u_depth_test = pgl_glGetUniformLocation(pgl_program, "u_depth_test");
    pgl_u_depth_write = pgl_glGetUniformLocation(pgl_program, "u_depth_write");
    pgl_u_depth_func = pgl_glGetUniformLocation(pgl_program, "u_depth_func");
    pgl_u_depth_zbuf = pgl_glGetUniformLocation(pgl_program, "u_depth_zbuf");
    pgl_u_depth_scale = pgl_glGetUniformLocation(pgl_program, "u_depth_scale");
    pgl_u_depth_offset = pgl_glGetUniformLocation(pgl_program, "u_depth_offset");
    pgl_u_depth_zmax = pgl_glGetUniformLocation(pgl_program, "u_depth_zmax");
    pgl_u_dst_used = pgl_glGetUniformLocation(pgl_program, "u_dst_used");
    pgl_u_zdst_used = pgl_glGetUniformLocation(pgl_program, "u_zdst_used");
    pgl_u_dstimg = pgl_glGetUniformLocation(pgl_program, "u_dstimg");
    pgl_u_zimg = pgl_glGetUniformLocation(pgl_program, "u_zimg");
    if (pgl_fbfetch) {
        /* t_8c05fac8: image-unit assignment is program state, set once. */
        pgl_glUseProgram(pgl_program);
        pgl_glUniform1i(pgl_u_dstimg, 0);
        pgl_glUniform1i(pgl_u_zimg, 1);
    }
    return 1;
}

static int pgl_setup(PicaGlGetProc getproc) {
    if (!pgl_load(getproc)) return 0;
    {
        const char *r = (const char *)pgl_glGetString(PGGL_RENDERER);
        const char *v = (const char *)pgl_glGetString(PGGL_VERSION);
        const char *ven = (const char *)pgl_glGetString(PGGL_VENDOR);
        snprintf(pgl_renderer, sizeof pgl_renderer, "%s", r ? r : "?");
        fprintf(stderr, "GL backend: vendor=%s renderer=%s version=%s\n",
                ven ? ven : "?", r ? r : "?", v ? v : "?");
        if (pgl_debug) {
            PGint profile = 0;
            pgl_glGetIntegerv(PGGL_CONTEXT_PROFILE_MASK, &profile);
            fprintf(stderr, "GL debug: glsl=%s profile_mask=0x%X (%s) "
                    "init_thread=%p\n",
                    (const char *)pgl_glGetString(PGGL_GLSL_VERSION),
                    (unsigned)profile,
                    (profile & PGGL_CONTEXT_CORE_PROFILE_MASK) ? "core"
                                                              : "non-core",
                    pgl_thread_id);
        }
        if (r && (strstr(r, "llvmpipe") || strstr(r, "softpipe") || strstr(r, "swrast")))
            fprintf(stderr, "GL backend: WARNING: software GL rasterizer; "
                    "no GPU acceleration on this machine\n");
    }
    /* ---- t_8c05fac8: framebuffer-fetch mode decision (context is current).
       DQ8_GL_FBFETCH=0 forces the legacy per-draw copy path; DQ8_GL_DSTALWAYS=1
       (legacy reference mode) also forces copies.  The extension + entry
       points must both exist on this context (GL 3.3 core exposes ARB exts
       independently of the context version). */
    {
        const char *ef = getenv("DQ8_GL_FBFETCH");
        const char *ea = getenv("DQ8_GL_DSTALWAYS");
        int want = (!ef || !ef[0] || ef[0] != '0');
        int legacy = (ea && ea[0] && ea[0] != '0');
        int have_ext = 0;
        if (want && !legacy) {
            const char *s = (const char *)pgl_glGetString(PGGL_EXT_STR);
            if (s) have_ext = strstr(s, "GL_ARB_shader_image_load_store") != NULL;
            else if (pgl_glGetStringi) {
                PGint n = 0, i;
                pgl_glGetIntegerv(PGGL_NUM_EXTENSIONS, &n);
                for (i = 0; i < n; ++i) {
                    const char *e2 = (const char *)
                        pgl_glGetStringi(PGGL_EXT_STR, (PGuint)i);
                    if (e2 && strstr(e2, "GL_ARB_shader_image_load_store")) {
                        have_ext = 1;
                        break;
                    }
                }
            }
            if (have_ext && !pgl_glBindImageTexture) have_ext = 0;
            if (!have_ext)
                fprintf(stderr, "GL backend: image load/store unavailable -> "
                        "framebuffer fetch off (per-draw FBO copies)\n");
        }
        pgl_fbfetch = want && !legacy && have_ext;
    }
    fprintf(stderr, "GL backend: framebuffer fetch (imageLoad) = %s\n",
            pgl_fbfetch ? "on" : "off (per-draw FBO copies)");
    if (!pgl_build_program()) return 0;
    pgl_drain_errors("setup-program", NULL);
    pgl_glGenVertexArrays(1, &pgl_vao);
    pgl_glGenBuffers(1, &pgl_vbo);
    /* t_bc6b4eb7: one process-lifetime staging buffer for the geometry ring */
    if (!pgl_vbo_stage) pgl_vbo_stage = (unsigned char *)malloc(PGL_VBO_CAP);
    if (!pgl_vbo_stage) {
        fprintf(stderr, "GL backend: geometry staging alloc (%u B) failed\n",
                (unsigned)PGL_VBO_CAP);
        return 0;
    }
    pgl_glGenFramebuffers(1, &pgl_fbo);
    pgl_glGenTextures(1, &pgl_rt_tex);
    pgl_glGenTextures(1, &pgl_dst_tex);
    pgl_glGenTextures(1, &pgl_z_tex);
    pgl_glGenTextures(1, &pgl_zdst_tex);
    pgl_glPixelStorei(PGGL_PACK_ALIGNMENT, 1);
    pgl_glPixelStorei(PGGL_UNPACK_ALIGNMENT, 1);
    pgl_drain_errors("setup-objects", NULL);
    {
        /* Batched readback is on by default; DQ8_GL_BATCH=0 restores the
           per-draw upload/readback behaviour for differential testing. */
        const char *batch = getenv("DQ8_GL_BATCH");
        pgl_batch_on = !(batch && batch[0] == '0');
        /* Context hold is on by default; DQ8_GL_CTX_HOLD=0 restores the
           per-draw foreign-context displacement behaviour exactly. */
        const char *hold = getenv("DQ8_GL_CTX_HOLD");
        pgl_hold_on = !(hold && hold[0] == '0');
        {
            /* t_77ac52ce: fixed-function blending removes the per-draw u_dst
               copy and its pipeline drain — a big win on a real GPU, neutral
               on llvmpipe (the drain moves to the next sync).  Off by
               default: it is a second pixel path with 1-LSB rounding
               differences.  DQ8_GL_BLENDS=1 opts in. */
            const char *bl = getenv("DQ8_GL_BLENDS");
            if (bl && bl[0] == '1' && !bl[1]) {
                pgl_blend_gl_on = 1;
                fprintf(stderr, "GL backend: fixed-function blending ON "
                        "(no per-draw u_dst copy for mappable PICA blends)\n");
            }
        }
        {
            const char *sk = getenv("DQ8_GL_SKIP");
            if (sk && sk[0]) {
                if (strstr(sk, "nodst")) pgl_skip_mask |= PGL_SK_DST;
                if (strstr(sk, "nozdst")) pgl_skip_mask |= PGL_SK_ZDST;
                if (strstr(sk, "nogeo"))
                    pgl_skip_mask |= PGL_SK_GEO | PGL_SK_DST | PGL_SK_ZDST;
                fprintf(stderr, "GL backend: DQ8_GL_SKIP=%s (mask=0x%X, "
                        "diagnostic — output is wrong on purpose)\n",
                        sk, pgl_skip_mask);
            }
        }
    }
    pgl_ready = 1;
    return 1;
}

/* Caller-owned GL context (e.g. an SDL window's context). */
static int pica_gl_init_external(void *(*getproc)(const char *)) {
    if (pgl_tried) return pgl_ready;
    pgl_tried = 1;
    pgl_external_ctx = 1;
    return pgl_setup((PicaGlGetProc)getproc);
}

static void *pgl_egl_getproc(const char *name) {
    return (void *)eglGetProcAddress(name);
}

/* Try to create a 3.3 core context on `dpy` and make it current — surfaceless
   if the extension is present, else a 1x1 pbuffer.  On success fills the
   pgl_ctx/pgl_surf/pgl_ctx_path/pgl_dpy globals and returns 1.  On failure
   destroys anything it created, resets those globals, and returns 0 so the
   caller can try another display. */
static int pgl_ctx_on_display(EGLDisplay dpy, const char *source) {
    const char *exts;
    EGLConfig cfg;
    EGLint n = 0;
    int have_surfaceless, cfg_pbuffer = 0, made_current = 0;
    static const EGLint cfg_pb_attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_NONE
    };
    static const EGLint cfg_any_attribs[] = {
        EGL_SURFACE_TYPE, 0, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE
    };
    static const EGLint ctx_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_NONE
    };
    static const EGLint pbuffer_attribs[] = {
        EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE
    };
    pgl_ctx = EGL_NO_CONTEXT;
    pgl_surf = EGL_NO_SURFACE;
    exts = eglQueryString(dpy, EGL_EXTENSIONS);
    have_surfaceless = exts && strstr(exts, "EGL_KHR_surfaceless_context") != NULL;
    /* Prefer a pbuffer-capable config so the fallback below always exists. */
    if (eglChooseConfig(dpy, cfg_pb_attribs, &cfg, 1, &n) && n >= 1)
        cfg_pbuffer = 1;
    else if (!eglChooseConfig(dpy, cfg_any_attribs, &cfg, 1, &n) || n < 1) {
        if (pgl_debug)
            fprintf(stderr, "GL debug: %s: no EGL config\n", source);
        return 0;
    }
    pgl_ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
    if (pgl_ctx == EGL_NO_CONTEXT) {
        if (pgl_debug)
            fprintf(stderr, "GL debug: %s: eglCreateContext failed 0x%04X\n",
                    source, eglGetError());
        return 0;
    }
    if (have_surfaceless &&
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, pgl_ctx)) {
        made_current = 1;
        pgl_ctx_path = "surfaceless";
    }
    if (!made_current) {
        if (pgl_debug) {
            if (!have_surfaceless)
                fprintf(stderr, "GL debug: %s: no surfaceless ext; trying pbuffer\n",
                        source);
            else
                fprintf(stderr, "GL debug: %s: surfaceless makecurrent failed "
                        "0x%04X; trying pbuffer\n", source, eglGetError());
        }
        if (cfg_pbuffer) {
            pgl_surf = eglCreatePbufferSurface(dpy, cfg, pbuffer_attribs);
            if (pgl_surf != EGL_NO_SURFACE &&
                eglMakeCurrent(dpy, pgl_surf, pgl_surf, pgl_ctx)) {
                made_current = 1;
                pgl_ctx_path = "pbuffer";
            }
        }
    }
    if (!made_current) {
        if (pgl_ctx != EGL_NO_CONTEXT) {
            eglDestroyContext(dpy, pgl_ctx);
            pgl_ctx = EGL_NO_CONTEXT;
        }
        if (pgl_surf != EGL_NO_SURFACE) {
            eglDestroySurface(dpy, pgl_surf);
            pgl_surf = EGL_NO_SURFACE;
        }
        return 0;
    }
    pgl_dpy = dpy;
    pgl_dpy_source = source;
    return 1;
}

/* Create the EGL display + 3.3 context on the CURRENT thread, make it
   current and run pgl_setup (shaders/objects).  Shared by the synchronous
   path (guest thread) and the split worker thread — an EGL context is made
   current on exactly one thread, and under the split that thread is the
   worker from here on. */
static int pgl_bringup(void) {
    EGLint major = 0, minor = 0;
    PglSavedCtx prev;
    pgl_eglGetPlatformDisplayEXT_fn getplat;
    EGLDisplay sdpy;
    memset(&prev, 0, sizeof prev);
    pgl_context_save(&prev); /* whatever SDL made current on this thread */
    eglBindAPI(EGL_OPENGL_API);
    /* 1) surfaceless-mesa platform display (window-system independent). */
    getplat = (pgl_eglGetPlatformDisplayEXT_fn)
        eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (getplat) {
        sdpy = getplat(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
        if (sdpy != EGL_NO_DISPLAY && eglInitialize(sdpy, &major, &minor)) {
            if (pgl_ctx_on_display(sdpy, "surfaceless-mesa")) goto setup;
        }
    }
    /* 2) default display (real desktop GLX) as a fallback. */
    pgl_dpy = EGL_NO_DISPLAY;
    pgl_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (pgl_dpy != EGL_NO_DISPLAY && eglInitialize(pgl_dpy, &major, &minor)) {
        if (pgl_ctx_on_display(pgl_dpy, "default-display")) goto setup;
        if (pgl_debug)
            fprintf(stderr, "GL debug: default-display context failed\n");
    }
    pgl_dpy = EGL_NO_DISPLAY;
    fprintf(stderr, "GL backend: could not create a current GL context on any "
            "display (set DQ8_GL=0 to silence)\n");
    return 0;
setup:
    fprintf(stderr, "GL backend: EGL %d.%d display=%s context-current=%s\n",
            (int)major, (int)minor, pgl_dpy_source, pgl_ctx_path);
    if (!pgl_setup(pgl_egl_getproc)) {
        pgl_context_put(&prev); /* never leave our context stuck on the thread */
        return 0;
    }
    pgl_context_put(&prev); /* hand the thread back to SDL's GL user */
    return 1;
}

/* Headless-safe context: prefer the EGL surfaceless-mesa platform display,
   which is NOT backed by the window system.  An X-backed (GLX/EGL-on-X11)
   context shares X-server GL state with SDL's renderer, so SDL's per-frame
   GLX MakeCurrent displaces ours and every later GL call (incl. readback)
   runs on the wrong/no context — the "readback failed" symptom.  Using the
   surfaceless platform keeps the GL context independent of X/SDL.  Fall back
   to the default display (real desktop GLX) only if surfaceless is absent or
   its context cannot be made current.  Each display tries surfaceless first,
   then a 1x1 pbuffer. */
static int pica_gl_init(void) {
    const char *env;
    if (pgl_tried) return pgl_ready;
    pgl_tried = 1;
    env = getenv("DQ8_GL");
    if (!env || !env[0] || env[0] == '0') return 0;
    env = getenv("DQ8_GL_DEBUG");
    pgl_debug = env && env[0] && env[0] != '0';
    env = getenv("DQ8_GL_RECT_TRACE");
    pgl_rect_trace = env && env[0] && env[0] != '0';
    env = getenv("DQ8_GL_ZCLEAR");
    pgl_zclear_on = !env || !env[0] || env[0] != '0';
    pgl_thread_id = pgl_tid();
    /* t_cb8723ae: DQ8_SPLIT_GL=1 moves GL execution (draws, uploads,
       read-backs, the EGL context itself) onto a dedicated worker thread.
       Only for OUR context — a caller-owned context (pica_gl_init_external)
       stays with its owner.  Failure falls back to the synchronous path. */
    env = getenv("DQ8_SPLIT_GL");
    if (env && env[0] && env[0] != '0' && !pgl_external_ctx) {
        if (pgl_split_start()) {
            atexit(pica_gl_shutdown);
            return pgl_ready;
        }
        fprintf(stderr, "GL backend: worker-thread split failed to start; "
                "continuing on the synchronous path\n");
    }
    if (!pgl_bringup()) return 0;
    atexit(pica_gl_shutdown);
    return 1;
}

static int pica_gl_active(void) { return pgl_ready; }
static unsigned long long pica_gl_draw_count(void) { return pgl_draws; }
static const char *pica_gl_renderer(void) { return pgl_renderer; }

static void pica_gl_shutdown(void) {
    unsigned i;
    if (!pgl_ready) return;
    pica_gl_context_release(); /* hand the thread back before teardown */
    pgl_ready = 0;
    if (pgl_split_on) {
        /* Guest-side teardown: stop and JOIN the worker (it drains the
           queue, then destroys the GL context on its own thread), then free
           the guest-owned shadows.  Counters are stable after the join. */
        pgl_split_stop();
        fprintf(stderr, "GL backend: split shutdown after %llu hardware draws "
                "(%llu deferred readbacks, %llu guest refreshes; "
                "split flushes=%llu)\n",
                pgl_draws, pgl_flushes, pgl_refills, pgl_split_flush_ops);
        fprintf(stderr, "GL texture uploads: count=%llu bytes=%llu "
                "cache_hits=%llu cache_misses=%llu\n",
                pgl_tex_uploads, pgl_tex_upload_bytes, pgl_tex_hits, pgl_tex_misses);
        fprintf(stderr, "GL texture uploads detail: generation_hits=%llu "
                "subimage_updates=%llu slots=%d\n",
                pgl_tex_gen_hits, pgl_tex_subuploads, PGL_UP_SLOTS);
        pica_stage_stats_dump();
        free(pgl_shadow);
        pgl_shadow = NULL;
        pgl_shadow_len = 0;
        free(pgl_zshadow);
        pgl_zshadow = NULL;
        pgl_zshadow_len = 0;
        pgl_pending = 0;
        return;
    }
    fprintf(stderr, "GL backend: shutdown after %llu hardware draws "
            "(batched: %llu deferred readbacks, %llu guest refreshes, "
            "%llu uniform clears; foreign-context displacements=%llu, "
            "object rebuilds=%llu)\n",
            pgl_draws, pgl_flushes, pgl_refills, pgl_cleared,
            pgl_displaces, pgl_rebuilds);
    fprintf(stderr, "GL context hold: releases=%llu mid-hold retakes=%llu\n",
            pgl_hold_releases, pgl_hold_mid);
    fprintf(stderr, "GL texture uploads: count=%llu bytes=%llu "
            "cache_hits=%llu cache_misses=%llu\n",
            pgl_tex_uploads, pgl_tex_upload_bytes, pgl_tex_hits, pgl_tex_misses);
    fprintf(stderr, "GL texture uploads detail: generation_hits=%llu "
            "subimage_updates=%llu slots=%d\n",
            pgl_tex_gen_hits, pgl_tex_subuploads, PGL_UP_SLOTS);
    pica_stage_stats_dump();
    for (i = 0; i < PGL_UP_SLOTS; ++i) {
        free(pgl_up[i].copy);
        pgl_up[i].copy = NULL;
        pgl_up[i].bytes = 0;
        pgl_up[i].used = 0;
    }
    free(pgl_shadow);
    pgl_shadow = NULL;
    pgl_shadow_len = 0;
    pgl_pending = 0;
    if (!pgl_external_ctx && pgl_dpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(pgl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (pgl_ctx != EGL_NO_CONTEXT) eglDestroyContext(pgl_dpy, pgl_ctx);
        if (pgl_surf != EGL_NO_SURFACE) eglDestroySurface(pgl_dpy, pgl_surf);
        eglTerminate(pgl_dpy);
    }
}

/* ---- context isolation -------------------------------------------------
   Between draws, other GL users on this thread (SDL's window machinery on
   an OPENGL-capable driver: x11/wayland/offscreen all initialise Mesa and
   set SDL_WINDOW_OPENGL, offscreen even creates its own EGL context) issue
   GL calls against WHATEVER CONTEXT IS CURRENT.  When ours is left current
   their object ids collide with ours and they corrupt our state — observed
   on llvmpipe: VAO/VBO/FBO/stage-texture deleted before draw #1
   ("rebuilt GL objects lost to a foreign GL user"), then a driver SIGSEGV
   inside glTexImage2D; on a GPU the same contamination yields a wiped
   render target (black readback -> rt_put paints the GSP framebuffer
   black).  So our context is current ONLY while our own GL code runs:
   save the previous context, bind ours, draw, restore.  Foreign GL then
   runs on its own context (or none) and can never touch ours. ---- */
static void pgl_context_save(PglSavedCtx *s) {
    s->dpy = eglGetCurrentDisplay();
    s->draw = eglGetCurrentSurface(EGL_DRAW);
    s->read = eglGetCurrentSurface(EGL_READ);
    s->ctx = eglGetCurrentContext();
}

/* Bind our context, remembering what was current.  Returns 0 (and leaves the
   previous context alone) when ours cannot be made current.
   With the context hold active (default), the first take of a burst remembers
   the foreign context and keeps OURS current until pica_gl_context_release();
   the caller's pgl_context_put then becomes a no-op, so a burst of draws pays
   one displacement instead of one per draw. */
static int pgl_context_take(PglSavedCtx *s) {
    pgl_context_save(s);
    if (pgl_external_ctx || s->ctx == pgl_ctx) return 1;
    if (!eglMakeCurrent(pgl_dpy, pgl_surf, pgl_surf, pgl_ctx)) {
        fprintf(stderr, "GL backend: eglMakeCurrent failed 0x%04X\n", eglGetError());
        return 0;
    }
    if (pgl_hold_on) {
        if (pgl_hold_active) {
            /* A foreign user displaced us mid-hold and we just displaced it
               back; the newest foreign context is what release must restore. */
            ++pgl_hold_mid;
            if (pgl_debug)
                fprintf(stderr, "GL debug: mid-hold foreign displacement by "
                        "context %p (retake #%llu)\n", (void *)s->ctx,
                        (unsigned long long)pgl_hold_mid);
        }
        pgl_held = *s;
        pgl_hold_active = 1;
        s->ctx = pgl_ctx; /* caller's put() is a no-op: ours stays current */
    }
    return 1;
}

/* Restore the context that was current before pgl_context_take(); when there
   was none, release ours so foreign GL calls hit "no current context"
   (no-op/INVALID_OPERATION) instead of our objects. */
static void pgl_context_put(const PglSavedCtx *s) {
    if (pgl_external_ctx || s->ctx == pgl_ctx) return;
    if (s->ctx == EGL_NO_CONTEXT) {
        eglMakeCurrent(pgl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    } else if (!eglMakeCurrent(s->dpy, s->draw, s->read, s->ctx)) {
        /* Cannot restore the foreign context: at worst release ours. */
        eglMakeCurrent(pgl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
}

/* Release point for the context hold: restore the foreign context that was
   current before the burst of GL work.  Called from pica_gl_flush (every
   consumer of guest-visible pixels — GSP transfer/capture/scan-out/DMA and
   software draws) and from the SDL present path, so no foreign GL call can
   run against our objects.  No-op when nothing is held or when called from
   a thread other than the GL thread (MakeCurrent is per-thread). */
static void pica_gl_context_release(void) {
    if (!pgl_hold_active) return;
    if (pgl_tid() != pgl_thread_id) return;
    pgl_hold_active = 0;
    pgl_context_put(&pgl_held);
    ++pgl_hold_releases;
}

/* Verify our GL objects still exist.  glIsTexture/glIsBuffer/glIsVAO/
   glIsFramebuffer report FALSE for a glGen'd name until its first bind, so
   each object is only validated after WE have bound it (pgl_obj_used marks
   that).  A genuinely vanished object means some other GL user on this thread
   deleted it in our context; rebuild it and force the render-target texture
   + FBO to be re-specified/re-attached. */
static int pgl_ensure_objects(void) {
    unsigned lost = 0, used = pgl_obj_used, i;
    if (!pgl_glIsProgram(pgl_program)) { pgl_program = 0; lost |= 1u; }
    if ((lost & 1u) && !pgl_build_program()) return 0;
    if ((used & 2u) && !pgl_glIsVertexArray(pgl_vao)) { pgl_glGenVertexArrays(1, &pgl_vao); lost |= 2u; }
    if ((used & 4u) && !pgl_glIsBuffer(pgl_vbo)) { pgl_glGenBuffers(1, &pgl_vbo); lost |= 4u; }
    if ((used & 8u) && !pgl_glIsFramebuffer(pgl_fbo)) { pgl_glGenFramebuffers(1, &pgl_fbo); lost |= 8u; }
    if ((used & 16u) && !pgl_glIsTexture(pgl_rt_tex)) { pgl_glGenTextures(1, &pgl_rt_tex); lost |= 16u; }
    if ((used & 32u)) {
        /* Stage-texture objects live in the multi-slot upload cache; a slot
           whose texture vanished is simply marked unused so the next draw
           re-uploads into a fresh object. */
        for (i = 0; i < PGL_UP_SLOTS; ++i) {
            if (!pgl_up[i].used || !pgl_up[i].tex || pgl_glIsTexture(pgl_up[i].tex))
                continue;
            pgl_up[i].tex = 0;
            pgl_up[i].used = 0;
            pgl_up[i].ptr = NULL;
            lost |= 32u;
        }
    }
    if ((used & 64u) && !pgl_glIsTexture(pgl_dst_tex)) {
        pgl_glGenTextures(1, &pgl_dst_tex);
        lost |= 64u;
    }
    if ((used & 128u) && !pgl_glIsTexture(pgl_z_tex)) {
        pgl_glGenTextures(1, &pgl_z_tex);
        lost |= 128u;
    }
    if ((used & 256u) && !pgl_glIsTexture(pgl_zdst_tex)) {
        pgl_glGenTextures(1, &pgl_zdst_tex);
        lost |= 256u;
    }
    if (lost & (8u | 16u | 128u)) {
        pgl_rt_w = pgl_rt_h = 0; /* re-specify + re-attach */
        /* FBO content lost with the objects: the guest thread owns the
           shadows under the split, so route through the resync atomic. */
        if (pgl_split_cur) pgl_request_resync();
        else { pgl_force_full = 1; pgl_zforce_full = 1; }
    }
    if (lost) {
        if (lost & 4u) { /* VBO regenerated: the ring cursor/store are void */
            pgl_vbo_alloc = 0;
            pgl_vbo_off = 0;
        }
        ++pgl_rebuilds;
        fprintf(stderr, "GL backend: rebuilt vanished GL objects mask=0x%X "
                        "(rebuild #%llu, draw #%llu)\n",
                lost, (unsigned long long)pgl_rebuilds,
                (unsigned long long)pgl_draws);
        pgl_drain_errors("object-rebuild", NULL);
    }
    return 1;
}

/* Ensure FBO color attachment matches the render-target size. */
static int pgl_ensure_rt(int w, int h) {
    if (pgl_fbo == 0 || w <= 0 || h <= 0) return 0;
    if (pgl_rt_tex == 0 || pgl_rt_w != w || pgl_rt_h != h) {
        PGenum fb_status;
        pgl_rt_w = w;
        pgl_rt_h = h;
        pgl_glBindTexture(PGGL_TEXTURE_2D, pgl_rt_tex);
        pgl_obj_used |= 16u;
        pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_MIN_FILTER, PGGL_NEAREST);
        pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_MAG_FILTER, PGGL_NEAREST);
        pgl_glTexImage2D(PGGL_TEXTURE_2D, 0, PGGL_RGBA8, w, h, 0, PGGL_RGBA,
                         PGGL_UNSIGNED_BYTE, NULL);
        /* Depth attachment: R32F storing integer z values (exact). */
        pgl_glBindTexture(PGGL_TEXTURE_2D, pgl_z_tex);
        pgl_obj_used |= 128u;
        pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_MIN_FILTER, PGGL_NEAREST);
        pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_MAG_FILTER, PGGL_NEAREST);
        pgl_glTexImage2D(PGGL_TEXTURE_2D, 0, PGGL_R32F, w, h, 0, PGGL_RED,
                         PGGL_FLOAT, NULL);
        pgl_glBindFramebuffer(PGGL_FRAMEBUFFER, pgl_fbo);
        pgl_obj_used |= 8u;
        pgl_glFramebufferTexture2D(PGGL_FRAMEBUFFER, PGGL_COLOR_ATTACHMENT0,
                                   PGGL_TEXTURE_2D, pgl_rt_tex, 0);
        pgl_glFramebufferTexture2D(PGGL_FRAMEBUFFER, PGGL_COLOR_ATTACHMENT1,
                                   PGGL_TEXTURE_2D, pgl_z_tex, 0);
        /* Under the split the GUEST thread mirrors the re-spec force flags
           (it owns pgl_zshadow); pgl_zforce_full is guest state there. */
        if (!pgl_split_cur) pgl_zforce_full = 1;
        fb_status = pgl_glCheckFramebufferStatus(PGGL_FRAMEBUFFER);
        if (fb_status != PGGL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "GL backend: FBO incomplete at %dx%d (0x%04X)\n",
                    w, h, (unsigned)fb_status);
            return 0;
        }
        if (pgl_debug)
            fprintf(stderr, "GL debug: FBO %u rt_tex %u %dx%d complete\n",
                    pgl_fbo, pgl_rt_tex, w, h);
    }
    pgl_glBindFramebuffer(PGGL_FRAMEBUFFER, pgl_fbo);
    pgl_obj_used |= 8u;
    return 1;
}

/* Draw triangles (3 consecutive PicaGlVertex per triangle) into the host
   render target. Returns the number of triangles drawn, or 0 when the draw
   cannot run on GL (caller then falls back to the software rasterizer).
   Public entry: isolates our context from any other GL user on this thread
   (context save/restore around the whole draw). */
/* ---- Fixed-function blending fast path (t_77ac52ce) ----------------------
   The per-draw u_dst copy exists because the fragment shader reproduces
   pica_write_fragment's blend math by hand, and a shader cannot sample the
   texture it renders into.  Measured cost on llvmpipe: ~880 us per draw
   (~21 % of the whole frame).  Every PICA blend factor except 14
   (src-alpha-sat) and every equation 0..4 has an exact GL equivalent, and GL
   blends against the same 8-bit destination the u_dst copy would have
   supplied, so the result differs at most by the final 8-bit rounding. */
static int pgl_factor_gl(unsigned f, PGenum *out) {
    switch (f) {
    case 0:  *out = PGGL_ZERO; break;
    case 1:  *out = PGGL_ONE; break;
    case 2:  *out = PGGL_SRC_COLOR; break;
    case 3:  *out = PGGL_ONE_MINUS_SRC_COLOR; break;
    case 4:  *out = PGGL_DST_COLOR; break;
    case 5:  *out = PGGL_ONE_MINUS_DST_COLOR; break;
    case 6:  *out = PGGL_SRC_ALPHA; break;
    case 7:  *out = PGGL_ONE_MINUS_SRC_ALPHA; break;
    case 8:  *out = PGGL_DST_ALPHA; break;
    case 9:  *out = PGGL_ONE_MINUS_DST_ALPHA; break;
    case 10: *out = PGGL_CONSTANT_COLOR; break;
    case 11: *out = PGGL_ONE_MINUS_CONSTANT_COLOR; break;
    case 12: *out = PGGL_CONSTANT_ALPHA; break;
    case 13: *out = PGGL_ONE_MINUS_CONSTANT_ALPHA; break;
    default: return 0; /* 14 = src-alpha-sat (min(sa, 1-da)): no GL equivalent */
    }
    return 1;
}
static int pgl_equation_gl(unsigned eq, PGenum *out) {
    switch (eq) {
    case 0: *out = PGGL_FUNC_ADD; break;
    case 1: *out = PGGL_FUNC_SUBTRACT; break;
    case 2: *out = PGGL_FUNC_REVERSE_SUBTRACT; break;
    case 3: *out = PGGL_MIN; break;
    case 4: *out = PGGL_MAX; break;
    default: return 0; /* 5..7 have no GL blend-equation equivalent */
    }
    return 1;
}
/* Fill the GL blend state for `st`; returns 1 when the whole PICA blend
   configuration is expressible as fixed-function GL blending. */
static int pgl_blend_state(const PicaGlState *st, PGenum f[4], PGenum eq[2]) {
    if (!pgl_blend_gl_on || !st->blend) return 0;
    if (!pgl_factor_gl(st->blend_src_rgb, &f[0])) return 0;
    if (!pgl_factor_gl(st->blend_dst_rgb, &f[1])) return 0;
    if (!pgl_factor_gl(st->blend_src_a, &f[2])) return 0;
    if (!pgl_factor_gl(st->blend_dst_a, &f[3])) return 0;
    if (!pgl_equation_gl(st->blend_eq_rgb, &eq[0])) return 0;
    if (!pgl_equation_gl(st->blend_eq_a, &eq[1])) return 0;
    return 1;
}

/* ---- t_8c05fac8: does this draw's blend math actually CONSUME dest? ----
   Every channel computes value[k] from (source[k], dest[k]):
     eq 0 ADD:      sv + dest[k]*fd
     eq 1 SUB:      sv - dest[k]*fd
     eq 2 RSUB:     dest[k]*fd - sv
     eq 3/4 MIN/MAX: op(source[k], dest[k])   <- raw dest, fd ignored
   dest is mathematically irrelevant iff for EVERY channel fd==0 AND
   eq in {0,1,2} (then dest[k]*0 == 0 exactly: dest is RGBA8-finite in
   u_dst).  Such draws skip the per-draw FBO copy entirely; the shader
   takes its dest from u_dst_used=0 (=vec4(0)) which yields the identical
   quantized byte floor(sat(v)*255+0.5) as multiplying the real dest by
   0 (only -0.0 vs +0.0 can differ pre-quantization; both quantize to 0). */
static int pgl_blend_dst_read(const PicaGlState *st) {
    unsigned ch;
    if (!st->blend) return 0;
    for (ch = 0; ch < 4u; ++ch) {
        unsigned f = (ch == 3u) ? st->blend_dst_a : st->blend_dst_rgb;
        unsigned eq = (ch == 3u) ? st->blend_eq_a : st->blend_eq_rgb;
        if (f != 0u || eq >= 3u) return 1;
    }
    return 0;
}

static unsigned pica_gl_draw_locked(const PicaGlVertex *verts, unsigned nverts,
                                    const PicaGlState *st, const PicaGlHost *host);

static unsigned pica_gl_draw(const PicaGlVertex *verts, unsigned nverts,
                             const PicaGlState *st, const PicaGlHost *host) {
    PglSavedCtx prev;
    unsigned r;
    if (!pgl_ready) return 0;
    if (pgl_split_on) return pica_gl_draw_split(verts, nverts, st, host);
    memset(&prev, 0, sizeof prev);
    if (!pgl_context_take(&prev)) return 0;
    if (prev.ctx != EGL_NO_CONTEXT && prev.ctx != pgl_ctx) {
        ++pgl_displaces;
        if (pgl_debug && pgl_draws < 8u)
            fprintf(stderr, "GL debug: foreign context %p was current before "
                    "draw #%llu; saving/restoring it (displace #%llu)\n",
                    (void *)prev.ctx, (unsigned long long)(pgl_draws + 1u),
                    (unsigned long long)pgl_displaces);
    }
    r = pica_gl_draw_locked(verts, nverts, st, host);
    pgl_context_put(&prev);
    return r;
}

static void pgl_reset_drawn(void) {
    pgl_drawn_x0 = pgl_drawn_y0 = 1 << 30;
    pgl_drawn_x1 = pgl_drawn_y1 = -1;
}

/* Bring the FBO back in sync with guest memory after guest-side writes.
   `raw`/`len` are the render target's raw guest bytes (host->rt_raw).
   Fast paths: unchanged region (single memcmp), uniform raw pattern (GSP
   fills / memsets -> glClear or residue-class colors), otherwise a per-8x8-
   Morton-block diff uploaded as sub-rectangles.  The Morton color buffer is
   laid out as 64-pixel (8x8) blocks in block-row-major order, so a block is
   one contiguous raw range (see pica_rt_off in the runtime). */
static int pgl_refresh(const PicaGlState *st, const PicaGlHost *host,
                       const unsigned char *raw, unsigned len) {
    unsigned char *pixels;
    unsigned bs, blocks_x, nblocks, bi, x, y;
    int all = pgl_force_full;
    if (!all && memcmp(pgl_shadow, raw, len) == 0) return 1;
    ++pgl_refills;
    /* 1) uniform raw pattern: the whole region is a repeating 3- or 4-byte
          pattern (what gsp_memory_fill / memset write).  A pixel's decoded
          color then depends only on (px_off % L), i.e. at most L colors;
          one color becomes an O(1) glClear. */
    {
        unsigned L = 0, i;
        for (i = 4; i >= 3; --i) {
            unsigned j;
            if (len < i) continue;
            for (j = 0; j < len; ++j)
                if (raw[j] != raw[j % i]) break;
            if (j == len) { L = i; break; }
        }
        if (L) {
            unsigned char col[4][4], have[4] = {0, 0, 0, 0};
            int ok = 1, uniform = 1;
            unsigned r;
            for (i = 0; i < 16u; ++i) {
                unsigned char rgba[4];
                int px = (int)(i & 7u), py = (int)(i >> 3);
                if (!host->rt_get(host->user, px, py, rgba)) { ok = 0; break; }
                r = host->rt_px_off(host->user, px, py) % L;
                if (!have[r]) { memcpy(col[r], rgba, 4); have[r] = 1; }
            }
            for (r = 0; ok && r < L; ++r) {
                if (!have[r]) { ok = 0; break; }
                if (r && memcmp(col[r], col[0], 4)) uniform = 0;
            }
            if (ok && uniform) {
                pgl_glBindFramebuffer(PGGL_FRAMEBUFFER, pgl_fbo);
                pgl_obj_used |= 8u;
                pgl_glColorMask(1, 1, 1, 1);
                pgl_glClearColor(col[0][0] / 255.0f, col[0][1] / 255.0f,
                                 col[0][2] / 255.0f, col[0][3] / 255.0f);
                pgl_glClear(PGGL_COLOR_BUFFER_BIT);
                ++pgl_cleared;
                memcpy(pgl_shadow, raw, len);
                pgl_force_full = 0;
                pgl_drain_errors("rt-clear", NULL);
                return 1;
            }
            if (ok) {
                pixels = (unsigned char *)malloc((size_t)st->rt_w * st->rt_h * 4u);
                if (!pixels) return 0;
                for (y = 0; y < st->rt_h; ++y)
                    for (x = 0; x < st->rt_w; ++x) {
                        r = host->rt_px_off(host->user, (int)x, (int)y) % L;
                        memcpy(pixels + ((size_t)y * st->rt_w + x) * 4u, col[r], 4);
                    }
                pgl_glBindTexture(PGGL_TEXTURE_2D, pgl_rt_tex);
                pgl_obj_used |= 16u;
                pgl_glTexImage2D(PGGL_TEXTURE_2D, 0, PGGL_RGBA8, (PGsizei)st->rt_w,
                                 (PGsizei)st->rt_h, 0, PGGL_RGBA, PGGL_UNSIGNED_BYTE,
                                 pixels);
                free(pixels);
                pgl_drain_errors("rt-upload", NULL);
                memcpy(pgl_shadow, raw, len);
                pgl_force_full = 0;
                return 1;
            }
        }
    }
    /* 2) generic: changed 8x8 Morton blocks -> sub-rectangle uploads. */
    pixels = (unsigned char *)malloc(64u * 4u);
    if (!pixels) return 0;
    blocks_x = st->rt_w / 8u;
    bs = 64u * (st->rt_bpp ? st->rt_bpp : 4u);
    if (blocks_x && (st->rt_w % 8u) == 0 && (st->rt_h % 8u) == 0 &&
        (size_t)bs * blocks_x * (st->rt_h / 8u) <= (size_t)len) {
        nblocks = blocks_x * (st->rt_h / 8u);
        {
            static int clog; /* rect-trace: guest->FBO uploads over PENDING GL content */
            for (bi = 0; bi < nblocks; ++bi) {
                unsigned bx = bi % blocks_x, by = bi / blocks_x;
                unsigned row, ci;
                int y0;
                if (!all &&
                    memcmp(pgl_shadow + (size_t)bi * bs, raw + (size_t)bi * bs, bs) == 0)
                    continue;
                if (pgl_rect_trace && pgl_pending && clog < 40) {
                    fprintf(stderr, "GLRECT refresh-over-pending block=%u,%u "
                            "guest_rows=%u..%u (gl_y=%d..%d) draw#%llu\n",
                            bx, by, by * 8u, by * 8u + 7u,
                            (int)st->rt_h - 1 - (int)(by * 8u + 7u),
                            (int)st->rt_h - 1 - (int)(by * 8u),
                            (unsigned long long)pgl_draws);
                    ++clog;
                }
            /* block (bx,by) = top-down rows [by*8, by*8+8); GL rows are
               bottom-up, so the rectangle's lowest GL row is rt_h-1-(by*8+7). */
            y0 = (int)st->rt_h - 1 - (int)(by * 8u + 7u);
            for (row = 0; row < 8u; ++row)
                for (ci = 0; ci < 8u; ++ci) {
                    unsigned char rgba[4];
                    if (!host->rt_get(host->user, (int)(bx * 8u + ci), y0 + (int)row,
                                      rgba))
                        memset(rgba, 0, 4);
                    memcpy(pixels + ((size_t)row * 8u + ci) * 4u, rgba, 4);
                }
            pgl_glBindTexture(PGGL_TEXTURE_2D, pgl_rt_tex);
            pgl_obj_used |= 16u;
            pgl_glTexSubImage2D(PGGL_TEXTURE_2D, 0, (PGint)(bx * 8u), (PGint)y0,
                                8, 8, PGGL_RGBA, PGGL_UNSIGNED_BYTE, pixels);
            }
        }
        pgl_drain_errors("rt-subupload", NULL);
    } else {
        unsigned char *img = (unsigned char *)malloc((size_t)st->rt_w * st->rt_h * 4u);
        if (!img) { free(pixels); return 0; }
        for (y = 0; y < st->rt_h; ++y)
            for (x = 0; x < st->rt_w; ++x) {
                unsigned char rgba[4];
                if (!host->rt_get(host->user, (int)x, (int)y, rgba))
                    memset(rgba, 0, 4);
                memcpy(img + ((size_t)y * st->rt_w + x) * 4u, rgba, 4);
            }
        pgl_glBindTexture(PGGL_TEXTURE_2D, pgl_rt_tex);
        pgl_obj_used |= 16u;
        pgl_glTexImage2D(PGGL_TEXTURE_2D, 0, PGGL_RGBA8, (PGsizei)st->rt_w,
                         (PGsizei)st->rt_h, 0, PGGL_RGBA, PGGL_UNSIGNED_BYTE, img);
        free(img);
        pgl_drain_errors("rt-upload", NULL);
    }
    free(pixels);
    memcpy(pgl_shadow, raw, len);
    pgl_force_full = 0;
    return 1;
}

/* Depth-region sync (coarser than color: memcmp fast path, uniform-z clear
   fast path for fills, full re-upload otherwise). */
static int pgl_refresh_depth(const PicaGlState *st, const PicaGlHost *host,
                             const unsigned char *raw, unsigned len) {
    unsigned bpp = st->depth_bpp ? st->depth_bpp : 2u, x, y;
    if (!pgl_zforce_full && pgl_zshadow && pgl_zshadow_len == len &&
        memcmp(pgl_zshadow, raw, len) == 0)
        return 1;
    ++pgl_zrefills;
    {   /* uniform z (GSP fills / memsets): one glClearBufferfv */
        unsigned z0 = raw[0] | ((unsigned)raw[1] << 8) |
                      (bpp > 2u ? ((unsigned)raw[2] << 16) : 0u);
        unsigned i;
        int uniform = 1;
        for (i = 0; i + bpp <= len; i += bpp) {
            unsigned z = raw[i] | ((unsigned)raw[i + 1] << 8) |
                         (bpp > 2u ? ((unsigned)raw[i + 2] << 16) : 0u);
            if (z != z0) { uniform = 0; break; }
        }
        if (uniform) {
            int ok = !pgl_zclear_on; /* DQ8_GL_ZCLEAR=0: legacy no-op (reference) */
            if (pgl_zclear_on) {
                PGfloat v[4];
                PGenum zbufs[2] = { PGGL_COLOR_ATTACHMENT0,
                                    PGGL_COLOR_ATTACHMENT1 };
                v[0] = (PGfloat)z0; v[1] = 0.0f; v[2] = 0.0f; v[3] = 1.0f;
                pgl_glBindFramebuffer(PGGL_FRAMEBUFFER, pgl_fbo);
                pgl_obj_used |= 8u;
                /* Attachment 1 (the R32F depth mirror) is a COLOR attachment,
                   so glClearBufferfv needs GL_COLOR plus the draw-buffer INDEX
                   of attachment 1 — GL_FLOAT as the buffer enum is
                   GL_INVALID_ENUM and silently cleared nothing while the
                   shadow below was still marked in sync (stale depth ->
                   failed depth tests in the top raster rows -> left-clipped
                   dialog).  The buffer must also be in the draw-buffer list
                   and writable: set both explicitly; the next draw
                   re-establishes its own color mask / draw buffers. */
                pgl_glColorMask(1, 1, 1, 1);
                pgl_glDrawBuffers(2, zbufs);
                pgl_glClearBufferfv(PGGL_COLOR, 1, v);
                ok = pgl_drain_errors("z-clear", NULL) == 0;
            }
            if (ok) {
                if (pgl_zshadow_len != len) {
                    unsigned char *grown =
                        (unsigned char *)realloc(pgl_zshadow, len ? len : 1u);
                    if (!grown) return 0;
                    pgl_zshadow = grown;
                    pgl_zshadow_len = len;
                }
                memcpy(pgl_zshadow, raw, len);
                pgl_zforce_full = 0;
                ++pgl_zflushes; /* count clear fast-path wins separately below */
                --pgl_zflushes;
                return 1;
            }
            /* Rejected by the driver: fall through to the full upload so the
               shadow is only marked synced once the attachment really holds
               the guest's depth. */
        }
    }
    {   /* full re-upload */
        float *img = (float *)malloc((size_t)st->rt_w * st->rt_h * sizeof(float));
        if (!img) return 0;
        for (y = 0; y < st->rt_h; ++y)
            for (x = 0; x < st->rt_w; ++x) {
                unsigned z = 0;
                if (host->depth_get && !host->depth_get(host->user, (int)x, (int)y, &z))
                    z = 0;
                img[(size_t)y * st->rt_w + x] = (float)z;
            }
        pgl_glBindTexture(PGGL_TEXTURE_2D, pgl_z_tex);
        pgl_obj_used |= 128u;
        pgl_glTexImage2D(PGGL_TEXTURE_2D, 0, PGGL_R32F, (PGsizei)st->rt_w,
                         (PGsizei)st->rt_h, 0, PGGL_RED, PGGL_FLOAT, img);
        free(img);
        pgl_drain_errors("z-upload", NULL);
    }
    if (pgl_zshadow_len != len) {
        unsigned char *grown =
            (unsigned char *)realloc(pgl_zshadow, len ? len : 1u);
        if (!grown) return 0;
        pgl_zshadow = grown;
        pgl_zshadow_len = len;
    }
    memcpy(pgl_zshadow, raw, len);
    pgl_zforce_full = 0;
    return 1;
}

/* DQ8_GL_DUMP_DIR (diagnosis only, off by default): overwrite snapshot files
   on every flush with the FBO read-back, the depth read-back and the guest
   render-target/depth bytes, so a run's LAST flush describes the state that
   reaches the captured frame. */
static void pgl_dump_snap(const char *name, const void *data, size_t bytes) {
    const char *dd = getenv("DQ8_GL_DUMP_DIR");
    char path[768];
    FILE *f;
    if (!dd || !dd[0] || !data) return;
    snprintf(path, sizeof path, "%s/%s", dd, name);
    f = fopen(path, "wb");
    if (!f) return;
    fwrite(data, 1, bytes, f);
    fclose(f);
}
static void pgl_dump_box(const char *name, int x0, int y0, int x1, int y1) {
    const char *dd = getenv("DQ8_GL_DUMP_DIR");
    char path[768];
    FILE *f;
    if (!dd || !dd[0]) return;
    snprintf(path, sizeof path, "%s/%s", dd, name);
    f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%d %d %d %d %u %u\n", x0, y0, x1, y1, pgl_rt_w, pgl_rt_h);
    fclose(f);
}

/* ---- write-back cores (t_cb8723ae GL thread split) ----------------------
   pica_gl_flush_locked used to interleave the GL read-back (GL thread) with
   the guest-memory write-back (host->rt_put/depth_put write guest RAM, so it
   must run on the guest thread).  The split path needs them on different
   threads, so the flush is factored into read-back helpers (GL thread) and
   write-back cores (guest thread); the synchronous path calls them
   back-to-back and is behaviourally unchanged. */

/* Write-back core for one depth read-back box (GUEST thread): the same skip
   rule as color — a guest-side write after the last sync wins. */
static void pgl_wb_depth(const PicaGlHost *host, int have_raw,
                         const float *buf, unsigned w, unsigned h,
                         int x0, int y0, int x1, int y1) {
    unsigned x, y, bpp = pgl_zshadow_bpp, len = 0;
    const unsigned char *raw = NULL;
    if (have_raw && host->depth_raw && !host->depth_raw(host->user, &raw, &len))
        raw = NULL;
    if (raw && (!pgl_zshadow || len != pgl_zshadow_len)) raw = NULL;
    {
        unsigned long long nwrite = 0, nzero = 0, nskip = 0, nclamp = 0;
        int sx0 = 1 << 30, sy0 = 1 << 30, sx1 = -1, sy1 = -1;
        int zx0 = 1 << 30, zy0 = 1 << 30, zx1 = -1, zy1 = -1;
        for (y = 0; y < h; ++y) {
            for (x = 0; x < w; ++x) {
                int gx = x0 + (int)x, gy = y0 + (int)y;
                float v = buf[(size_t)y * w + x];
                if (raw && bpp && host->depth_px_off) {
                    unsigned off = host->depth_px_off(host->user, gx, gy);
                    if ((size_t)off + bpp <= len &&
                        memcmp(raw + off, pgl_zshadow + off, bpp) != 0) {
                        ++nskip; /* later guest write wins */
                        if (gx < sx0) sx0 = gx;
                        if (gy < sy0) sy0 = gy;
                        if (gx > sx1) sx1 = gx;
                        if (gy > sy1) sy1 = gy;
                        continue;
                    }
                }
                if (host->depth_put) {
                    unsigned zv;
                    float zmaxv = (bpp == 2u) ? 65535.0f : 16777215.0f;
                    /* The attachment holds integer z, but the far plane
                       (0xFFFFFF) cannot absorb the +0.5 rounding: 16777215.0f
                       + 0.5f rounds to 16777216.0f (24-bit mantissa), whose
                       3-byte store writes 00 00 00 — turning the far plane
                       into depth 0 in guest memory and failing LEQUAL there
                       on the next sync. Clamp instead (NaN/negative -> 0). */
                    if (!(v >= 0.0f)) {
                        zv = 0u;
                        ++nclamp;
                    } else if (v >= zmaxv) {
                        zv = (unsigned)zmaxv;
                        ++nclamp;
                    } else {
                        zv = (unsigned)(v + 0.5f);
                    }
                    if (zv == 0u) {
                        ++nzero;
                        if (gx < zx0) zx0 = gx;
                        if (gy < zy0) zy0 = gy;
                        if (gx > zx1) zx1 = gx;
                        if (gy > zy1) zy1 = gy;
                    }
                    host->depth_put(host->user, gx, gy, zv);
                    ++nwrite;
                }
            }
        }
        if (pgl_rect_trace) {
            unsigned eoff = (host->depth_px_off && raw) ?
                            host->depth_px_off(host->user, x1, y0) : 0u;
            fprintf(stderr, "ZFLUSH#%llu wrote=%llu zero=%llu skipped=%llu "
                    "clamp=%llu skipbox=%d,%d..%d,%d zerobox=%d,%d..%d,%d "
                    "box=%d,%d..%d,%d "
                    "edge(v=%g zv=%u raw=%02X%02X%02X sh=%02X%02X%02X have_raw=%d)\n",
                    (unsigned long long)pgl_zflushes, nwrite, nzero, nskip,
                    nclamp,
                    sx0, sy0, sx1, sy1, zx0, zy0, zx1, zy1, x0, y0, x1, y1,
                    (double)buf[(size_t)(h - 1) * w + (w - 1)],
                    (unsigned)(buf[(size_t)(h - 1) * w + (w - 1)] + 0.5f),
                    (raw && eoff + 3 < len) ? raw[eoff] : 0xFFu,
                    (raw && eoff + 3 < len) ? raw[eoff + 1] : 0xFFu,
                    (raw && eoff + 3 < len) ? raw[eoff + 2] : 0xFFu,
                    (pgl_zshadow && eoff + 3 < pgl_zshadow_len) ? pgl_zshadow[eoff] : 0xEEu,
                    (pgl_zshadow && eoff + 3 < pgl_zshadow_len) ? pgl_zshadow[eoff + 1] : 0xEEu,
                    (pgl_zshadow && eoff + 3 < pgl_zshadow_len) ? pgl_zshadow[eoff + 2] : 0xEEu,
                    raw != NULL);
        }
    }
    if (raw) memcpy(pgl_zshadow, raw, len);
    pgl_dump_snap("z_guest_last.raw", raw, len);
    ++pgl_zflushes;
}

/* Depth read-back (GL thread): attachment 1 -> caller-owned float box, or
   NULL on failure (the FBO content is then marked untrustworthy). */
static float *pgl_rb_depth(int x0, int y0, int x1, int y1) {
    unsigned w = (unsigned)(x1 - x0 + 1), h = (unsigned)(y1 - y0 + 1);
    float *buf = (float *)malloc((size_t)w * h * sizeof(float));
    if (!buf) return NULL;
    pgl_glBindFramebuffer(PGGL_FRAMEBUFFER, pgl_fbo);
    pgl_obj_used |= 8u;
    pgl_glReadBuffer(PGGL_COLOR_ATTACHMENT1);
    pgl_glReadPixels(x0, y0, (PGsizei)w, (PGsizei)h, PGGL_RED, PGGL_FLOAT, buf);
    pgl_glReadBuffer(PGGL_COLOR_ATTACHMENT0);
    if (pgl_drain_errors("z-readback", NULL) != 0) {
        free(buf);
        pgl_request_resync();
        return NULL;
    }
    pgl_dump_snap("z_fbo_last.raw", buf, (size_t)w * h * sizeof(float));
    pgl_dump_box("z_fbo_last.box", x0, y0, x1, y1);
    return buf;
}

/* Write the FBO's not-yet-written-back content into guest memory.  Only the
   drawn box is read back; a pixel whose raw guest bytes changed since the
   FBO was last synchronized with guest memory (a later guest-side write)
   keeps the guest value — the guest write is newer and wins.  The shadow is
   then re-synchronized with guest memory.  Runs on OUR context (the public
   wrapper pica_gl_save/restore is pica_gl_flush). */
/* Write-back core for one color read-back box (GUEST thread). */
static void pgl_wb_color(const PicaGlHost *host, int have_raw,
                         const unsigned char *pixels,
                         int x0, int y0, int x1, int y1) {
    unsigned x, y, len = 0, bpp = pgl_shadow_bpp;
    unsigned w = (unsigned)(x1 - x0 + 1), h = (unsigned)(y1 - y0 + 1);
    const unsigned char *raw = NULL;
    if (have_raw && host->rt_raw && !host->rt_raw(host->user, &raw, &len))
        raw = NULL;
    if (raw && (!pgl_shadow || len != pgl_shadow_len)) raw = NULL;
    {
        unsigned long long nwrite = 0, nskip = 0;
        int sx0 = 1 << 30, sy0 = 1 << 30, sx1 = -1, sy1 = -1;
        for (y = 0; y < h; ++y) {
            for (x = 0; x < w; ++x) {
                const unsigned char *px = pixels + ((size_t)y * w + x) * 4u;
                int gx = x0 + (int)x, gy = y0 + (int)y;
                if (raw && bpp) {
                    unsigned off = host->rt_px_off(host->user, gx, gy);
                    if ((size_t)off + bpp <= len &&
                        memcmp(raw + off, pgl_shadow + off, bpp) != 0) {
                        ++nskip; /* guest wrote this pixel later: guest value wins */
                        if (gx < sx0) sx0 = gx;
                        if (gy < sy0) sy0 = gy;
                        if (gx > sx1) sx1 = gx;
                        if (gy > sy1) sy1 = gy;
                        continue;
                    }
                }
                ++nwrite;
                host->rt_put(host->user, gx, gy, px);
            }
        }
        if (pgl_rect_trace && (nskip || nwrite < (unsigned long long)w * h))
            fprintf(stderr, "GLRECT flush#%llu wrote=%llu skipped=%llu "
                    "skipbox=%d,%d..%d,%d box=%d,%d..%d,%d\n",
                    (unsigned long long)pgl_flushes, nwrite, nskip,
                    sx0, sy0, sx1, sy1, x0, y0, x1, y1);
    }
    /* rt_put wrote into guest memory through `raw`; snapshot the new state. */
    if (raw) memcpy(pgl_shadow, raw, len);
    pgl_dump_snap("color_guest_last.raw", raw, len);
}

static void pica_gl_flush_locked(const PicaGlHost *host, int have_raw) {
    unsigned char *pixels;
    unsigned w, h;
    int x0 = pgl_drawn_x0, y0 = pgl_drawn_y0, x1 = pgl_drawn_x1, y1 = pgl_drawn_y1;
    if (!pgl_pending) return;
    pgl_pending = 0;
    if (x0 > x1 || y0 > y1) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)pgl_rt_w - 1) x1 = (int)pgl_rt_w - 1;
    if (y1 > (int)pgl_rt_h - 1) y1 = (int)pgl_rt_h - 1;
    if (x0 > x1 || y0 > y1) return;
    w = (unsigned)(x1 - x0 + 1);
    h = (unsigned)(y1 - y0 + 1);
    pixels = (unsigned char *)malloc((size_t)w * h * 4u);
    if (!pixels) return;
    pgl_glBindFramebuffer(PGGL_FRAMEBUFFER, pgl_fbo);
    pgl_obj_used |= 8u;
    pgl_glReadBuffer(PGGL_COLOR_ATTACHMENT0);
    pgl_glReadPixels(x0, y0, (PGsizei)w, (PGsizei)h, PGGL_RGBA,
                     PGGL_UNSIGNED_BYTE, pixels);
    {
        PGenum rb_err = 0;
        if (pgl_drain_errors("readback", &rb_err) != 0) {
            fprintf(stderr, "GL backend: batched readback failed: err=0x%04X %s "
                    "(rt=%ux%u box=%d,%d..%d,%d)\n", (unsigned)rb_err,
                    pgl_err_name(rb_err), pgl_rt_w, pgl_rt_h, x0, y0, x1, y1);
            free(pixels);
            pgl_request_resync(); /* FBO content cannot be trusted as written back */
            pgl_reset_drawn();
            return;
        }
    }
    pgl_dump_snap("color_fbo_last.raw", pixels, (size_t)w * h * 4u);
    pgl_dump_box("color_fbo_last.box", x0, y0, x1, y1);
    pgl_wb_color(host, have_raw, pixels, x0, y0, x1, y1);
    free(pixels);
    if (pgl_zdirty) {
        float *zbuf;
        pgl_zdirty = 0;
        zbuf = pgl_rb_depth(x0, y0, x1, y1);
        if (zbuf) {
            pgl_wb_depth(host, have_raw, zbuf, w, h, x0, y0, x1, y1);
            free(zbuf);
        }
    }
    ++pgl_flushes;
    if (pgl_debug && (pgl_flushes <= 8u || (pgl_flushes % 200u) == 0u))
        fprintf(stderr, "GL debug: flush #%llu box=%d,%d..%d,%d rt=%ux%u\n",
                (unsigned long long)pgl_flushes, x0, y0, x1, y1, pgl_rt_w, pgl_rt_h);
    pgl_reset_drawn();
    if (pgl_rect_trace)
        fprintf(stderr, "GLRECT flush#%llu box=%d,%d..%d,%d rt=%ux%u\n",
                (unsigned long long)pgl_flushes, x0, y0, x1, y1, pgl_rt_w, pgl_rt_h);
}

static int pica_gl_pending(void) { return pgl_ready && pgl_pending; }

/* Write back any deferred render-target content (context-isolated).  Call
   before anything reads the render target from guest memory (GSP display
   transfer, capture, scan-out) or before a software-path draw. */
static void pica_gl_flush(const PicaGlHost *host) {
    PglSavedCtx prev;
    /* Flush is the guest-visible-pixel boundary: any consumer past this point
       (GSP transfer/capture/scan-out/DMA, software draws, SDL present) may
       run foreign GL calls, so drop the context hold here. */
    pica_gl_context_release();
    if (!pgl_ready || !pgl_pending || !host) return;
    if (pgl_split_on) { pgl_split_flush(host); return; }
    memset(&prev, 0, sizeof prev);
    if (!pgl_context_take(&prev)) return;
    pica_gl_flush_locked(host, host->rt_raw && host->rt_px_off);
    pgl_context_put(&prev);
}

/* t_bc6b4eb7: pack triangles into the instanced payload layout — one
   PicaGlTriPayload per triangle, written exactly once (the old path wrote a
   3x-expanded 152 B record per output vertex). */
static void pgl_pack_tri(const PicaGlVertex *verts, unsigned nverts,
                         unsigned char *dst) {
    PicaGlTriPayload *pay = (PicaGlTriPayload *)dst;
    unsigned t, k;
    for (t = 0; t + 3u <= nverts; t += 3u, ++pay) {
        for (k = 0; k < 3u; ++k) {
            const PicaGlVertex *v = &verts[t + k];
            float *a = pay->a[k];
            a[0] = v->x;
            a[1] = v->y;
            a[2] = v->iw;
            a[3] = v->zw;
            memcpy(pay->b[k], v->color, sizeof pay->b[k]);
            pay->c[k][0] = v->u;
            pay->c[k][1] = v->v;
        }
    }
}

static unsigned pica_gl_draw_locked(const PicaGlVertex *verts, unsigned nverts,
                                    const PicaGlState *st, const PicaGlHost *host) {
    unsigned char *pixels;
    unsigned ntri = nverts / 3u;
    unsigned x, y, i;
    int batch, bx0, by0, bx1, by1;
    int dst_read, zdst_need, do_dst, do_zdst;
    unsigned bw = 0, bh = 0;
    float minx, maxx, miny, maxy;
    unsigned long long ph_t0 = pica_pqt_now_ns(), ph_t1 = 0, ph_t2 = 0,
                       ph_t3 = 0, ph_t4 = 0;
    PGenum bl_f[4], bl_eq[2];
    int gl_blend;
    unsigned long long ph;
    if (!pgl_ready || !ntri || !host || !host->rt_get || !host->rt_put) return 0;
    if (!st->rt_w || !st->rt_h) return 0;
    if (st->mode == PICA_GL_TEV && st->tev.tex_en &&
        (!st->tex_rgba || st->tex_w <= 0 || st->tex_h <= 0)) return 0;
    batch = pgl_batch_on && host->rt_raw && host->rt_px_off;
    /* The wrapper (pica_gl_draw) made OUR context current and saved SDL's;
       everything below runs only on our context, and pica_gl_draw restores
       the previous one before returning, so no foreign GL call can ever
       execute against our objects. */
    if (pgl_debug && pgl_tid() != pgl_thread_id)
        fprintf(stderr, "GL debug: draw #%llu from thread %p (init thread %p)\n",
                (unsigned long long)pgl_draws, pgl_tid(), pgl_thread_id);
    /* DQ8_GL_DEBUG: vertex screen extent + primary alpha/colour of the first
       vertex — a degenerate/translated quad (all verts outside the RT) draws
       nothing while still counting as a successful glDrawArrays. */
    if (pgl_debug && (pgl_draws < 12u || (pgl_draws % 500u) == 0u)) {
        float x0 = verts[0].x, x1 = verts[0].x, y0 = verts[0].y, y1 = verts[0].y;
        float u0 = verts[0].u, u1 = verts[0].u, v0 = verts[0].v, v1 = verts[0].v;
        unsigned vi;
        for (vi = 1; vi < nverts; ++vi) {
            if (verts[vi].x < x0) x0 = verts[vi].x;
            if (verts[vi].x > x1) x1 = verts[vi].x;
            if (verts[vi].y < y0) y0 = verts[vi].y;
            if (verts[vi].y > y1) y1 = verts[vi].y;
            if (verts[vi].u < u0) u0 = verts[vi].u;
            if (verts[vi].u > u1) u1 = verts[vi].u;
            if (verts[vi].v < v0) v0 = verts[vi].v;
            if (verts[vi].v > v1) v1 = verts[vi].v;
        }
        fprintf(stderr, "GL debug: verts #%llu n=%u x=[%.1f..%.1f] y=[%.1f..%.1f] "
                "uv=[%.3f..%.3f]x[%.3f..%.3f] col0=(%.3f,%.3f,%.3f,%.3f)\n",
                (unsigned long long)pgl_draws, nverts, x0, x1, y0, y1,
                u0, u1, v0, v1,
                verts[0].color[0], verts[0].color[1], verts[0].color[2],
                verts[0].color[3]);
        /* CPU sample of the STAGED texture at the quad's UV centre with GL
           NEAREST semantics — distinguishes "uv lands on black padding" from
           "GL samples/uploads wrongly" when the readback shows no RGB. */
        if (st->mode == 1 && st->tex_rgba && st->tex_w > 0 && st->tex_h > 0) {
            float cu = (u0 + u1) * 0.5f, cv = (v0 + v1) * 0.5f;
            int sx = (int)(cu * st->tex_w), sy = (int)(cv * st->tex_h);
            const unsigned char *tp;
            if (sx < 0) sx = 0;
            if (sx >= st->tex_w) sx = st->tex_w - 1;
            if (sy < 0) sy = 0;
            if (sy >= st->tex_h) sy = st->tex_h - 1;
            tp = st->tex_rgba + ((size_t)sy * st->tex_w + sx) * 4u;
            fprintf(stderr, "GL debug: staged-sample #%llu uv=(%.3f,%.3f) "
                    "texel=(%d,%d/%dx%d) rgba=%u/%u/%u/%u\n",
                    (unsigned long long)pgl_draws, cu, cv, sx, sy,
                    st->tex_w, st->tex_h, tp[0], tp[1], tp[2], tp[3]);
        }
    }
    /* Drain errors left by earlier calls (other subsystems, prior stages) so
       the readback check below can only ever attribute readback's own errors. */
    pgl_drain_errors("stale@entry", NULL);
    /* A foreign GL user sharing this thread may have deleted our objects
       between draws; rebuild before using them (no-op in the normal case). */
    if (!pgl_ensure_objects()) return 0;
    /* A pending episode must be written back before the FBO is re-sized to a
       different render target (its content would be lost).  Under the split
       the GUEST thread owns pgl_pending/shadow dims and closes the episode
       at enqueue (blocking flush) before the size can change, so the worker
       never evaluates this condition. */
    if (!pgl_split_cur && pgl_pending &&
        (pgl_shadow_w != st->rt_w || pgl_shadow_h != st->rt_h))
        pica_gl_flush_locked(host, 1);
    if (!pgl_ensure_rt((int)st->rt_w, (int)st->rt_h)) return 0;
    /* t_8c05fac8: keep the read-only image units pointed at the current RT
       textures (first use, resize re-spec, or an object rebuild that changed
       the names).  State call only — never a queue drain. */
    if (pgl_fbfetch && pgl_glBindImageTexture) {
        static PGuint img_rt = 0, img_z = 0;
        static int img_w = 0, img_h = 0, no_bind = -1;
        if (no_bind < 0) { /* t_8c05fac8 diag: DQ8_GL_NOIMGBIND=1 */
            const char *e = getenv("DQ8_GL_NOIMGBIND");
            no_bind = (e && e[0] && e[0] != '0') ? 1 : 0;
        }
        if (!no_bind &&
            (img_rt != pgl_rt_tex || img_z != pgl_z_tex ||
             img_w != pgl_rt_w || img_h != pgl_rt_h)) {
            pgl_glBindImageTexture(0u, pgl_rt_tex, 0, 0, 0,
                                   PGGL_READ_ONLY, PGGL_RGBA8);
            pgl_glBindImageTexture(1u, pgl_z_tex, 0, 0, 0,
                                   PGGL_READ_ONLY, PGGL_R32F);
            img_rt = pgl_rt_tex;
            img_z = pgl_z_tex;
            img_w = pgl_rt_w;
            img_h = pgl_rt_h;
        }
    }
    gl_blend = pgl_blend_state(st, bl_f, bl_eq);
    if (gl_blend) ++pgl_blend_gl_draws;
    else if (st->blend) ++pgl_blend_sw_draws;

    if (pgl_split_cur) {
        /* Split path: the guest thread captured the refresh diff at enqueue
           (push-time MEMCMP against the guest bytes); apply it here without
           ever touching guest memory. */
        if (!pgl_split_apply_refresh(st)) return 0;
    } else if (batch) {
        const unsigned char *raw = NULL;
        unsigned len = 0;
        if (!host->rt_raw(host->user, &raw, &len)) return 0;
        if (!pgl_shadow || pgl_shadow_len != len || pgl_shadow_w != st->rt_w ||
            pgl_shadow_h != st->rt_h || pgl_shadow_bpp != (st->rt_bpp ? st->rt_bpp : 4u)) {
            unsigned char *grown =
                (unsigned char *)realloc(pgl_shadow, len ? len : 1u);
            if (!grown) return 0;
            pgl_shadow = grown;
            pgl_shadow_len = len;
            pgl_shadow_w = st->rt_w;
            pgl_shadow_h = st->rt_h;
            pgl_shadow_bpp = st->rt_bpp ? st->rt_bpp : 4u;
            memcpy(pgl_shadow, raw, len);
            pgl_force_full = 1; /* FBO content unknown for this target */
        }
        if (!pgl_refresh(st, host, raw, len)) return 0;
        if (st->depth_on && host->depth_raw) {
            const unsigned char *zraw = NULL;
            unsigned zlen = 0;
            if (!host->depth_raw(host->user, &zraw, &zlen)) return 0;
            if (!pgl_zshadow || pgl_zshadow_len != zlen ||
                pgl_zshadow_w != st->rt_w || pgl_zshadow_h != st->rt_h ||
                pgl_zshadow_bpp != st->depth_bpp) {
                unsigned char *grown =
                    (unsigned char *)realloc(pgl_zshadow, zlen ? zlen : 1u);
                if (!grown) return 0;
                pgl_zshadow = grown;
                pgl_zshadow_len = zlen;
                pgl_zshadow_w = st->rt_w;
                pgl_zshadow_h = st->rt_h;
                pgl_zshadow_bpp = st->depth_bpp ? st->depth_bpp : 2u;
                memcpy(pgl_zshadow, zraw, zlen);
                pgl_zforce_full = 1;
            }
            if (!pgl_refresh_depth(st, host, zraw, zlen)) return 0;
        }
    } else {
        /* Host without raw access: upload the render target from guest
           memory before every draw (the pre-batching behaviour). */
        pixels = (unsigned char *)malloc((size_t)st->rt_w * st->rt_h * 4u);
        if (!pixels) return 0;
        for (y = 0; y < st->rt_h; ++y) {
            for (x = 0; x < st->rt_w; ++x) {
                unsigned char rgba[4];
                if (!host->rt_get(host->user, (int)x, (int)y, rgba)) {
                    free(pixels);
                    return 0;
                }
                memcpy(pixels + ((size_t)y * st->rt_w + x) * 4u, rgba, 4);
            }
        }
        pgl_glBindTexture(PGGL_TEXTURE_2D, pgl_rt_tex);
        pgl_obj_used |= 16u;
        pgl_glTexImage2D(PGGL_TEXTURE_2D, 0, PGGL_RGBA8, (PGsizei)st->rt_w,
                         (PGsizei)st->rt_h, 0, PGGL_RGBA, PGGL_UNSIGNED_BYTE, pixels);
        free(pixels);
        pgl_drain_errors("rt-upload", NULL);
    }

    ph_t1 = pica_pqt_now_ns();
    /* Draw bounding box (raster coordinates, y=0 bottom): bounds the u_dst
       blend copy and the deferred read-back, both at least as large as the
       triangle coverage. */
    minx = maxx = verts[0].x;
    miny = maxy = verts[0].y;
    for (i = 1; i < nverts; ++i) {
        if (verts[i].x < minx) minx = verts[i].x;
        if (verts[i].x > maxx) maxx = verts[i].x;
        if (verts[i].y < miny) miny = verts[i].y;
        if (verts[i].y > maxy) maxy = verts[i].y;
    }
    bx0 = (int)floorf(minx) - 1;
    by0 = (int)floorf(miny) - 1;
    bx1 = (int)ceilf(maxx) + 1;
    by1 = (int)ceilf(maxy) + 1;
    if (bx0 < 0) bx0 = 0;
    if (by0 < 0) by0 = 0;
    if (bx1 > (int)st->rt_w - 1) bx1 = (int)st->rt_w - 1;
    if (by1 > (int)st->rt_h - 1) by1 = (int)st->rt_h - 1;
    if (bx0 <= bx1 && by0 <= by1) {
        bw = (unsigned)(bx1 - bx0 + 1);
        bh = (unsigned)(by1 - by0 + 1);
    }
    if (pgl_rect_trace) {
        fprintf(stderr, "GLRECT draw#%llu n=%u rt=%ux%u v0=(%.2f,%.2f) "
                "min=(%.2f,%.2f) max=(%.2f,%.2f) box=%d,%d..%d,%d bw=%u bh=%u "
                "blend=%d depth=%d mode=%d\n",
                (unsigned long long)pgl_draws, nverts, st->rt_w, st->rt_h,
                verts[0].x, verts[0].y, minx, miny, maxx, maxy,
                bx0, by0, bx1, by1, bw, bh, st->blend, st->depth_on, st->mode);
    }

    pgl_glBindFramebuffer(PGGL_FRAMEBUFFER, pgl_fbo);
    pgl_obj_used |= 8u;
    pgl_glViewport(0, 0, (PGsizei)st->rt_w, (PGsizei)st->rt_h);
    pgl_glColorMask((st->color_mask >> 0) & 1, (st->color_mask >> 1) & 1,
                    (st->color_mask >> 2) & 1, (st->color_mask >> 3) & 1);
    pgl_glDisable(PGGL_BLEND); /* blending runs in the fragment shader */
    if (gl_blend) {
        /* t_77ac52ce: expressible PICA blend -> fixed-function GL blending.
           No u_dst copy, no per-fragment blend math, and no pipeline drain
           from reading the render target back into a texture. */
        pgl_glEnable(PGGL_BLEND);
        pgl_glBlendFuncSeparate(bl_f[0], bl_f[1], bl_f[2], bl_f[3]);
        pgl_glBlendEquationSeparate(bl_eq[0], bl_eq[1]);
        pgl_glBlendColor(st->blend_const[0], st->blend_const[1],
                         st->blend_const[2], st->blend_const[3]);
    }
    /* Blend destination: a fragment shader cannot sample the texture it
       renders into, so the draw box's current FBO contents are copied into
       the u_dst texture first. */
    /* ---- t_8c05fac8: only read the FBO back when the shader CONSUMES it.
       dst_read  - blend math consumes dest (some fd!=0 or eq in {MIN,MAX});
                   skipped draws take dest = vec4(0) in the shader, which is
                   bit-identical because fd==0 makes dest enter the math as
                   exactly +0.0 anyway (u_dst is RGBA8-finite).
       zdst_need - oldz is read (depth test runs, or depth writes are off so
                   frag_z keeps oldz).
       DQ8_GL_DSTALWAYS=1 restores the legacy always-copy behaviour
       (A/B frame-diff reference).  The DQ8_GL_SKIP=nodst/nozdst diagnostic
       knobs keep priority (deliberately wrong output). */
    {
        static int dst_always = -1;
        if (dst_always < 0) {
            const char *e = getenv("DQ8_GL_DSTALWAYS");
            dst_always = (e && e[0] && e[0] != '0') ? 1 : 0;
        }
        dst_read = pgl_blend_dst_read(st);
        zdst_need = st->depth_on && (st->depth_test || !st->depth_write);
        /* t_8c05fac8: under framebuffer fetch NO per-draw FBO read happens
           (the shader imageLoads live content); the copy path stays exactly
           as before (DQ8_GL_DSTALWAYS forces it via pgl_fbfetch=0). */
        do_dst = !pgl_fbfetch && bw && bh && !gl_blend &&
                 !(pgl_skip_mask & PGL_SK_DST) && (dst_read || dst_always);
        do_zdst = !pgl_fbfetch && bw && bh && st->depth_on &&
                  !(pgl_skip_mask & PGL_SK_ZDST) &&
                  (zdst_need || dst_always);
    }
    if (st->depth_on) {
        ++pgl_d_on;
        if (st->depth_test) ++pgl_d_test;
        /* oldz is fetched whenever depth_on and is READ by the shader when
           the depth test runs or when frag_z keeps the old value. */
        if (st->depth_test || !st->depth_write) ++pgl_d_oldz_used;
    }
    if (st->blend) {
        if (dst_read) ++pgl_b_read;
        else if (st->blend_src_rgb == 1 && st->blend_src_a == 1 &&
                 st->blend_eq_rgb == 0 && st->blend_eq_a == 0) ++pgl_b_ident;
        else ++pgl_b_nodest_only;
    }
    {   unsigned long long ct0 = pica_pqt_now_ns();
    if (do_dst) {
        pgl_glActiveTexture(PGGL_TEXTURE1);
        pgl_glBindTexture(PGGL_TEXTURE_2D, pgl_dst_tex);
        pgl_obj_used |= 64u;
        if (pgl_dst_w != (int)st->rt_w || pgl_dst_h != (int)st->rt_h) {
            pgl_glTexImage2D(PGGL_TEXTURE_2D, 0, PGGL_RGBA8, (PGsizei)st->rt_w,
                             (PGsizei)st->rt_h, 0, PGGL_RGBA, PGGL_UNSIGNED_BYTE,
                             NULL);
            pgl_dst_w = (int)st->rt_w;
            pgl_dst_h = (int)st->rt_h;
            pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_MIN_FILTER, PGGL_NEAREST);
            pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_MAG_FILTER, PGGL_NEAREST);
            pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_WRAP_S, PGGL_CLAMP_TO_EDGE);
            pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_WRAP_T, PGGL_CLAMP_TO_EDGE);
        }
        pgl_glCopyTexSubImage2D(PGGL_TEXTURE_2D, 0, 0, 0, (PGint)bx0, (PGint)by0,
                                (PGsizei)bw, (PGsizei)bh);
        ++pgl_dst_copies;
    }
    pgl_ph_dst_ns += pica_pqt_now_ns() - ct0;
    ct0 = pica_pqt_now_ns();
    if (do_zdst) {
        {
            /* depth blend/test source: copy from attachment 1 */
            pgl_glActiveTexture(PGGL_TEXTURE2);
            pgl_glBindTexture(PGGL_TEXTURE_2D, pgl_zdst_tex);
            pgl_obj_used |= 256u;
            if (pgl_zdst_w != (int)st->rt_w || pgl_zdst_h != (int)st->rt_h) {
                pgl_glTexImage2D(PGGL_TEXTURE_2D, 0, PGGL_R32F, (PGsizei)st->rt_w,
                                 (PGsizei)st->rt_h, 0, PGGL_RED, PGGL_FLOAT,
                                 NULL);
                pgl_zdst_w = (int)st->rt_w;
                pgl_zdst_h = (int)st->rt_h;
                pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_MIN_FILTER, PGGL_NEAREST);
                pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_MAG_FILTER, PGGL_NEAREST);
            }
            pgl_glReadBuffer(PGGL_COLOR_ATTACHMENT1);
            pgl_glCopyTexSubImage2D(PGGL_TEXTURE_2D, 0, 0, 0, (PGint)bx0,
                                    (PGint)by0, (PGsizei)bw, (PGsizei)bh);
            pgl_glReadBuffer(PGGL_COLOR_ATTACHMENT0);
            ++pgl_zdst_copies;
        }
    }
    pgl_ph_zdst_ns += pica_pqt_now_ns() - ct0;
    } /* end dst/zdst copy timing block (t_8c05fac8) */
    ph_t2 = pica_pqt_now_ns();
    {
        /* MRT slot 1 carries the depth value only for depth draws */
        PGenum bufs[2] = { PGGL_COLOR_ATTACHMENT0, PGGL_COLOR_ATTACHMENT1 };
        pgl_glDrawBuffers(st->depth_on ? 2 : 1, bufs);
    }
    pgl_glUseProgram(pgl_program);
    pgl_glUniform2f(pgl_u_rt, (PGfloat)st->rt_w, (PGfloat)st->rt_h);
    pgl_glUniform1i(pgl_u_mode, st->mode == PICA_GL_TEV ? 1 : 0);
    pgl_glUniform1i(pgl_u_alpha_test, st->alpha_test ? 1 : 0);
    pgl_glUniform1i(pgl_u_alpha_func, (PGint)st->alpha_func);
    pgl_glUniform1f(pgl_u_alpha_ref, st->alpha_ref);
    pgl_glUniform2f(pgl_u_dst_off, (PGfloat)bx0, (PGfloat)by0);
    pgl_glUniform1i(pgl_u_dst, 1);
    pgl_glUniform1i(pgl_u_zdst, 2);
    /* t_8c05fac8: 1 = the draw may read the value: under framebuffer fetch
       that is "the blend/depth math consumes it" (imageLoad is always live);
       under the copy path "the per-draw FBO copy for this box really ran".
       0 = neutral value (vec4(0)/oldz 0.0) instead of stale texels. */
    if (pgl_fbfetch) {
        pgl_glUniform1i(pgl_u_dst_used,
                        dst_read && !(pgl_skip_mask & PGL_SK_DST) ? 1 : 0);
        pgl_glUniform1i(pgl_u_zdst_used,
                        zdst_need && !(pgl_skip_mask & PGL_SK_ZDST) ? 1 : 0);
    } else {
        pgl_glUniform1i(pgl_u_dst_used, do_dst ? 1 : 0);
        pgl_glUniform1i(pgl_u_zdst_used, do_zdst ? 1 : 0);
    }
    pgl_glUniform1i(pgl_u_depth_on, st->depth_on ? 1 : 0);
    pgl_glUniform1i(pgl_u_depth_test, st->depth_test ? 1 : 0);
    pgl_glUniform1i(pgl_u_depth_write, st->depth_write ? 1 : 0);
    pgl_glUniform1i(pgl_u_depth_func, (PGint)st->depth_func);
    pgl_glUniform1i(pgl_u_depth_zbuf, st->depth_zbuf ? 1 : 0);
    pgl_glUniform1f(pgl_u_depth_scale, st->depth_scale);
    pgl_glUniform1f(pgl_u_depth_offset, st->depth_offset);
    pgl_glUniform1f(pgl_u_depth_zmax, st->depth_zmax);
    pgl_glUniform1i(pgl_u_blend_en, (!gl_blend && st->blend) ? 1 : 0);
    pgl_glUniform1i(pgl_u_eq_rgb, (PGint)st->blend_eq_rgb);
    pgl_glUniform1i(pgl_u_eq_a, (PGint)st->blend_eq_a);
    pgl_glUniform1i(pgl_u_f_src_rgb, (PGint)st->blend_src_rgb);
    pgl_glUniform1i(pgl_u_f_dst_rgb, (PGint)st->blend_dst_rgb);
    pgl_glUniform1i(pgl_u_f_src_a, (PGint)st->blend_src_a);
    pgl_glUniform1i(pgl_u_f_dst_a, (PGint)st->blend_dst_a);
    pgl_glUniform4f(pgl_u_blend_const, st->blend_const[0], st->blend_const[1],
                    st->blend_const[2], st->blend_const[3]);
    if (st->mode == PICA_GL_TEV) {
        PGint iv[18];
        unsigned s, j;
        for (j = 0; j < 18u; ++j) iv[j] = (PGint)st->tev.src_c[j];
        pgl_glUniform1iv(pgl_u_src_c, 18, iv);
        for (j = 0; j < 18u; ++j) iv[j] = (PGint)st->tev.mod_c[j];
        pgl_glUniform1iv(pgl_u_mod_c, 18, iv);
        for (j = 0; j < 18u; ++j) iv[j] = (PGint)st->tev.src_a[j];
        pgl_glUniform1iv(pgl_u_src_a, 18, iv);
        for (j = 0; j < 18u; ++j) iv[j] = (PGint)st->tev.mod_a[j];
        pgl_glUniform1iv(pgl_u_mod_a, 18, iv);
        for (j = 0; j < 6u; ++j) iv[j] = (PGint)st->tev.cop[j];
        pgl_glUniform1iv(pgl_u_cop, 6, iv);
        for (j = 0; j < 6u; ++j) iv[j] = (PGint)st->tev.aop[j];
        pgl_glUniform1iv(pgl_u_aop, 6, iv);
        for (j = 0; j < 6u; ++j) iv[j] = (PGint)st->tev.buf_upd_c[j];
        pgl_glUniform1iv(pgl_u_buf_upd_c, 6, iv);
        for (j = 0; j < 6u; ++j) iv[j] = (PGint)st->tev.buf_upd_a[j];
        pgl_glUniform1iv(pgl_u_buf_upd_a, 6, iv);
        pgl_glUniform1fv(pgl_u_scale_c, 6, st->tev.scale_c);
        pgl_glUniform1fv(pgl_u_scale_a, 6, st->tev.scale_a);
        for (s = 0; s < 6u; ++s)
            pgl_glUniform4f(pgl_u_const_c + (PGint)s, st->tev.const_c[s][0],
                            st->tev.const_c[s][1], st->tev.const_c[s][2],
                            st->tev.const_c[s][3]);
        pgl_glUniform4f(pgl_u_buf_init, st->tev.buf_init[0], st->tev.buf_init[1],
                        st->tev.buf_init[2], st->tev.buf_init[3]);
        pgl_glUniform1i(pgl_u_tex_en, st->tev.tex_en ? 1 : 0);
    } else {
        pgl_glUniform1i(pgl_u_tex_en, 0);
    }
    pgl_drain_errors("use-program", NULL);
    if (st->mode == PICA_GL_TEV && st->tev.tex_en) {
        /* Upload cache.  Two identities are recognised:
           (a) t_66c89cf2 primary path — the staging cache handed us a slot id
               plus a generation counter, both bumped only when that slot is
               re-decoded.  Same (slot,gen) therefore means the decoded bytes
               are literally the same buffer contents: skip the upload, no
               memcmp, no copy.
           (b) legacy path (no staging identity: DQ8_GL_TEXCACHE=0 or a stage
               bypass) — pointer/size/hash are a FAST REJECT and the stored
               copy of the last upload must compare equal (the A15 rule: a
               32-bit hash match alone never skips an upload). */
        size_t tex_bytes = (size_t)st->tex_w * (size_t)st->tex_h * 4u;
        PglUpSlot *slot = NULL;
        size_t si;
        if (st->tex_gen && st->tex_slot < PGL_UP_SLOTS) {
            PglUpSlot *c = &pgl_up[st->tex_slot];
            if (c->used && c->gen == st->tex_gen && c->ptr == st->tex_rgba &&
                c->w == st->tex_w && c->h == st->tex_h) {
                slot = c;
                ++pgl_tex_hits;
                ++pgl_tex_gen_hits;
                c->lru = ++pgl_up_lru;
            }
        }
        if (!slot)
        for (si = 0; si < PGL_UP_SLOTS; ++si) {
            PglUpSlot *c = &pgl_up[si];
            if (!st->tex_gen && c->used && c->ptr == st->tex_rgba &&
                c->w == st->tex_w && c->h == st->tex_h &&
                c->hash == st->tex_hash && c->copy != NULL &&
                c->bytes == tex_bytes &&
                memcmp(st->tex_rgba, c->copy, tex_bytes) == 0) { slot = c; break; }
        }
        if (slot) {
            slot->lru = ++pgl_up_lru;
        } else {
            ++pgl_tex_misses;
            if (st->tex_gen && st->tex_slot < PGL_UP_SLOTS) {
                /* slot-indexed: THIS staging slot's GL object is reused for
                   as long as the process lives (no LRU churn for textures
                   that stay resident). */
                slot = &pgl_up[st->tex_slot];
            } else {
                slot = &pgl_up[0];
                for (si = 0; si < PGL_UP_SLOTS; ++si) {
                    if (!pgl_up[si].used) { slot = &pgl_up[si]; break; }
                    if (pgl_up[si].lru < slot->lru) slot = &pgl_up[si];
                }
            }
            if (!slot->tex) pgl_glGenTextures(1, &slot->tex);
            pgl_glActiveTexture(PGGL_TEXTURE0);
            pgl_glBindTexture(PGGL_TEXTURE_2D, slot->tex);
            pgl_obj_used |= 32u;
            pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_MIN_FILTER, PGGL_NEAREST);
            pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_MAG_FILTER, PGGL_NEAREST);
            pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_WRAP_S, PGGL_CLAMP_TO_EDGE);
            pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_WRAP_T, PGGL_CLAMP_TO_EDGE);
            if (slot->used && slot->w == st->tex_w && slot->h == st->tex_h &&
                slot->ptr == st->tex_rgba) {
                /* Same immutable storage, new decoded content: a sub-image
                   update avoids a driver-side re-spec of the texture object. */
                pgl_glTexSubImage2D(PGGL_TEXTURE_2D, 0, 0, 0, st->tex_w, st->tex_h,
                                    PGGL_RGBA, PGGL_UNSIGNED_BYTE, st->tex_rgba);
                ++pgl_tex_subuploads;
            } else {
                pgl_glTexImage2D(PGGL_TEXTURE_2D, 0, PGGL_RGBA8, st->tex_w,
                                 st->tex_h, 0, PGGL_RGBA, PGGL_UNSIGNED_BYTE,
                                 st->tex_rgba);
            }
            slot->ptr = st->tex_rgba;
            slot->w = st->tex_w;
            slot->h = st->tex_h;
            slot->hash = st->tex_hash;
            slot->gen = st->tex_gen;
            slot->used = 1;
            slot->lru = ++pgl_up_lru;
            /* Keep the exact bytes for the next skip decision on the LEGACY
               path only — the generation path never needs them (and dropping
               them frees a second full copy of every resident texture).  If
               the copy cannot be stored the legacy cache stays empty and the
               next draw simply re-uploads (correct, just slower). */
            if (st->tex_gen) {
                if (slot->copy) {
                    free(slot->copy);
                    slot->copy = NULL;
                    slot->bytes = 0;
                }
            } else {
                if (slot->bytes != tex_bytes) {
                    unsigned char *grown = (unsigned char *)realloc(
                        slot->copy, tex_bytes ? tex_bytes : 1u);
                    if (grown) {
                        slot->copy = grown;
                        slot->bytes = tex_bytes;
                    } else {
                        free(slot->copy);
                        slot->copy = NULL;
                        slot->bytes = 0;
                    }
                }
                if (slot->copy) memcpy(slot->copy, st->tex_rgba, tex_bytes);
            }
            ++pgl_tex_uploads; /* real upload count (also without debug) */
            pgl_tex_upload_bytes += tex_bytes;
            if (pgl_debug)
                fprintf(stderr, "GL debug: stage tex upload #%llu %dx%d ptr=%p "
                        "hash=%08X gen=%llu\n", (unsigned long long)pgl_tex_uploads,
                        st->tex_w, st->tex_h, (const void *)st->tex_rgba,
                        (unsigned)st->tex_hash,
                        (unsigned long long)st->tex_gen);
        }
        pgl_glActiveTexture(PGGL_TEXTURE0);
        pgl_glBindTexture(PGGL_TEXTURE_2D, slot->tex);
        pgl_obj_used |= 32u;
        /* t_77ac52ce: texture0 wrap modes follow the PICA sampler so u,v may
           leave [0,1] (repeat/mirror) instead of forcing a software fallback.
           The staged image + NEAREST filtering make REPEAT/MIRRORED_REPEAT
           reproduce pica_sample_tex0's wrap for these modes; the upload cache
           is keyed on the image, not the wrap, so set it per draw. */
        {
            static const struct { unsigned pica, gl; } wrap_map[] = {
                { 0u, PGGL_CLAMP_TO_EDGE }, { 2u, PGGL_REPEAT },
                { 3u, PGGL_MIRRORED_REPEAT }
            };
            unsigned s, ws = st->wrap_s, wt = st->wrap_t, i;
            for (i = 0; i < sizeof wrap_map / sizeof wrap_map[0]; ++i) {
                if (wrap_map[i].pica == ws) { s = wrap_map[i].gl; break; }
            }
            if (i == sizeof wrap_map / sizeof wrap_map[0]) s = PGGL_CLAMP_TO_EDGE;
            pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_WRAP_S, (PGint)s);
            for (i = 0; i < sizeof wrap_map / sizeof wrap_map[0]; ++i) {
                if (wrap_map[i].pica == wt) { s = wrap_map[i].gl; break; }
            }
            if (i == sizeof wrap_map / sizeof wrap_map[0]) s = PGGL_CLAMP_TO_EDGE;
            pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_WRAP_T, (PGint)s);
        }
        pgl_glUniform1i(pgl_u_tex, 0);
    }
    pgl_drain_errors("tex-upload", NULL);
    ph_t3 = pica_pqt_now_ns(); /* t_8c05fac8: uniforms+tex done */
    pgl_glBindVertexArray(pgl_vao);
    pgl_obj_used |= 2u;
    pgl_glBindBuffer(PGGL_ARRAY_BUFFER, pgl_vbo);
    pgl_obj_used |= 4u;
    {
        /* t_bc6b4eb7: pack ONE 120-byte record per triangle directly into the
           pre-allocated streaming ring (no per-draw malloc/free) and upload it
           with a single glBufferSubData — not a per-draw glBufferData orphan.
           All 3 instanced vertices read the same record (divisor 1), so the
           FLAT payload the fragment shader consumes is bit-identical to the
           old 3x-expanded layout while the upload drops from 456 B to 120 B
           per triangle (2.5 MB -> ~0.65 MB/frame). */
        size_t bytes = (size_t)ntri * sizeof(PicaGlTriPayload);
        size_t base;
        if (bytes > PGL_VBO_CAP) {
            /* Rare oversized draw: one-off store (pre-ring behaviour). */
            unsigned char *tmp = (unsigned char *)malloc(bytes);
            if (!tmp) return 0;
            pgl_pack_tri(verts, nverts, tmp);
            pgl_glBufferData(PGGL_ARRAY_BUFFER, (PGsizeiptr)bytes, tmp,
                             PGGL_DYNAMIC_DRAW);
            free(tmp);
            base = 0;
            pgl_vbo_off = 0;
            pgl_vbo_alloc = 0; /* store sized `bytes`; reallocate before reuse */
        } else {
            if (pgl_vbo_alloc != PGL_VBO_CAP) {
                pgl_glBufferData(PGGL_ARRAY_BUFFER, (PGsizeiptr)PGL_VBO_CAP,
                                 NULL, PGGL_DYNAMIC_DRAW);
                pgl_vbo_alloc = PGL_VBO_CAP;
                pgl_vbo_off = 0;
            } else if (pgl_vbo_off + bytes > PGL_VBO_CAP) {
                /* Rewind: orphan the store (pending draws keep the old data)
                   and restart the cursor — one call, no stall. */
                pgl_glBufferData(PGGL_ARRAY_BUFFER, (PGsizeiptr)PGL_VBO_CAP,
                                 NULL, PGGL_DYNAMIC_DRAW);
                pgl_vbo_off = 0;
            }
            pgl_pack_tri(verts, nverts, pgl_vbo_stage + pgl_vbo_off);
            pgl_glBufferSubData(PGGL_ARRAY_BUFFER, (PGsizeiptr)pgl_vbo_off,
                                (PGsizeiptr)bytes, pgl_vbo_stage + pgl_vbo_off);
            base = pgl_vbo_off;
            pgl_vbo_off += bytes;
        }
        /* Location order follows the SHADER's vertex-interleaved layout:
           loc 1,2,3 = f{0}a,f{0}b,f{0}c = rows a[0]/b[0]/c[0];
           loc 4,5,6 = f{1}a,b,c = a[1]/b[1]/c[1];
           loc 7,8,9 = f{2}a,b,c = a[2]/b[2]/c[2].  (Field-grouped binding
           here would hand f1a the COLOR row and f2a the UV row — the
           convergence-streak artifact seen before this fix.) */
        for (i = 0; i < 3u; ++i) {
            pgl_glEnableVertexAttribArray(1 + 3u * i);
            pgl_glVertexAttribPointer(1 + 3u * i, 4, PGGL_FLOAT, 0,
                                      sizeof(PicaGlTriPayload),
                                      (const void *)(base + (size_t)i * 16u));
            pgl_glVertexAttribDivisor(1 + 3u * i, 1);
            pgl_glEnableVertexAttribArray(2 + 3u * i);
            pgl_glVertexAttribPointer(2 + 3u * i, 4, PGGL_FLOAT, 0,
                                      sizeof(PicaGlTriPayload),
                                      (const void *)(base + 48u + (size_t)i * 16u));
            pgl_glVertexAttribDivisor(2 + 3u * i, 1);
            pgl_glEnableVertexAttribArray(3 + 3u * i);
            pgl_glVertexAttribPointer(3 + 3u * i, 2, PGGL_FLOAT, 0,
                                      sizeof(PicaGlTriPayload),
                                      (const void *)(base + 96u + (size_t)i * 8u));
            pgl_glVertexAttribDivisor(3 + 3u * i, 1);
        }
        ph_t4 = pica_pqt_now_ns(); /* t_8c05fac8: before the draw submit */
        if (!(pgl_skip_mask & PGL_SK_GEO))
            pgl_glDrawArraysInstanced(PGGL_TRIANGLES, 0, 3, (PGsizei)ntri);
        {
            /* Diagnostic (t_bc6b4eb7): what does GL actually hold for the
               first draws — divisor state + the bytes on the GL side vs what
               the pack wrote to staging.  Enabled with DQ8_GEOM_DUMP=1. */
            static int geom_dump = -1;
            if (geom_dump < 0) {
                const char *e = getenv("DQ8_GEOM_DUMP");
                geom_dump = (e && e[0] && e[0] != '0') ? 1 : 0;
            }
            if (geom_dump && pgl_draws < 5u && pgl_glGetBufferSubData) {
                PGint d1 = -99, d4 = -99, d7 = -99;
                unsigned char glb[240];
                size_t rd = bytes < sizeof glb ? bytes : sizeof glb;
                unsigned j;
                pgl_glGetVertexAttribiv(1, 0x88FE, &d1);
                pgl_glGetVertexAttribiv(4, 0x88FE, &d4);
                pgl_glGetVertexAttribiv(7, 0x88FE, &d7);
                memset(glb, 0xEE, sizeof glb);
                pgl_glGetBufferSubData(PGGL_ARRAY_BUFFER, (PGsizeiptr)base,
                                       (PGsizeiptr)rd, glb);
                fprintf(stderr,
                        "GEOMDUMP draw=%llu ntri=%u base=%zu bytes=%zu "
                        "div1=%d div4=%d div7=%d err=0x%04X eq=%d\n",
                        (unsigned long long)pgl_draws, ntri, base, bytes,
                        d1, d4, d7, pgl_glGetError(),
                        memcmp(glb, pgl_vbo_stage + base, rd) == 0);
                fprintf(stderr, "GEOMDUMP rec0:");
                for (j = 0; j < 30u && j * 4u < rd; ++j)
                    fprintf(stderr, " %.8g", ((const float *)glb)[j]);
                fprintf(stderr, "\n");
            }
        }
    }
    pgl_drain_errors("draw-arrays", NULL);
    ph = pica_pqt_now_ns();
    ++pgl_ph_n;
    pgl_ph_setup_ns += ph_t1 - ph_t0;
    pgl_ph_copy_ns += ph_t2 - ph_t1;
    pgl_ph_draw_ns += ph - ph_t2;
    pgl_ph_u_ns += ph_t3 - ph_t2;
    pgl_ph_vbo_ns += ph_t4 - ph_t3;
    pgl_ph_sub_ns += ph - ph_t4;
    pgl_ph_report();
    if (st->depth_on && !pgl_split_cur) pgl_zdirty = 1;

    if (bw && bh && !pgl_split_cur) {
        if (bx0 < pgl_drawn_x0) pgl_drawn_x0 = bx0;
        if (by0 < pgl_drawn_y0) pgl_drawn_y0 = by0;
        if (bx1 > pgl_drawn_x1) pgl_drawn_x1 = bx1;
        if (by1 > pgl_drawn_y1) pgl_drawn_y1 = by1;
    }
    if (!pgl_split_cur) pgl_pending = 1;
    if (!batch && !pgl_split_cur) {
        /* Host without raw access (or DQ8_GL_BATCH=0 handled by the caller
           via pica_gl_flush): write back immediately, pre-batching
           behaviour. */
        pica_gl_flush_locked(host, 0);
    }
    ++pgl_draws;
    return ntri;
}

/* ===================== t_cb8723ae: GL worker thread =====================
   The queue holds HEAP ops; ownership transfers guest -> worker at push and
   (for FLUSH read-back buffers) back at done_seq.  No pointer into guest RAM
   ever crosses the queue: the refresh diff is captured at enqueue and the
   texture bytes are stable because pica_vs.c drains the queue (see
   pica_gl_split_sync) before overwriting a staging slot. */

#define PGL_SPLIT_QUEUE_CAP 1024u
enum { PGL_OP_DRAW = 1, PGL_OP_FLUSH = 2 };
enum { PGL_REF_NONE = 0, PGL_REF_CLEAR = 1, PGL_REF_BLOCKS = 2, PGL_REF_FULL = 3 };
enum { PGL_ZREF_NONE = 0, PGL_ZREF_CLEAR = 1, PGL_ZREF_FULL = 2 };

struct PglSplitOp {
    unsigned long long seq;
    int kind;                       /* PGL_OP_DRAW | PGL_OP_FLUSH */
    /* DRAW: owned copies (freed by the WORKER after processing) */
    PicaGlVertex *verts;
    unsigned nverts;
    PicaGlState st;                 /* by value; tex_rgba = staging pointer */
    /* color refresh capture */
    int ref_kind;
    unsigned char ref_col[4];       /* PGL_REF_CLEAR */
    unsigned ref_nblocks;           /* PGL_REF_BLOCKS */
    unsigned *ref_blocks;           /* changed block indices, ascending */
    unsigned char *ref_blob;        /* 64*4 B RGBA per changed block, in order */
    unsigned char *ref_full;        /* PGL_REF_FULL: rt_w*rt_h*4 RGBA */
    /* depth refresh capture */
    int zref_kind;
    unsigned zref_z;                /* PGL_ZREF_CLEAR */
    float *zref_full;               /* PGL_ZREF_FULL: rt_w*rt_h floats */
    /* FLUSH (owned by the GUEST until write-back: the worker fills the
       read-back buffers, publishes done_seq and never touches the op again) */
    int fl_box[4];                  /* x0, y0, x1, y1 (GL orientation) */
    int fl_have_depth;
    unsigned char *fl_px;           /* color read-back, w*h*4 */
    float *fl_z;                    /* depth read-back, w*h floats */
    int fl_rb_failed;
};

static pthread_mutex_t pgl_sq_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pgl_sq_notempty = PTHREAD_COND_INITIALIZER;
static pthread_cond_t pgl_sq_notfull = PTHREAD_COND_INITIALIZER;
static pthread_cond_t pgl_sq_done = PTHREAD_COND_INITIALIZER;
static PglSplitOp *pgl_sq[PGL_SPLIT_QUEUE_CAP];
static unsigned pgl_sq_head, pgl_sq_tail, pgl_sq_count;
static unsigned long long pgl_sq_pushed, pgl_sq_done_seq;
static int pgl_sq_stop;
static pthread_t pgl_split_thread;
static _Atomic int pgl_split_ready;   /* 1 = up, -1 = bring-up failed */

/* Worker-side dummy host: draw_locked's split path must never call a guest
   callback; any accidental call is loud and aborts the draw. */
static int pgl_split_dummy_rt_get(void *u, int x, int y, unsigned char rgba[4]) {
    (void)u; (void)x; (void)y;
    if (rgba) rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0;
    fprintf(stderr, "GL backend: SPLIT BUG: rt_get on the worker thread\n");
    return 0;
}
static void pgl_split_dummy_rt_put(void *u, int x, int y,
                                   const unsigned char rgba[4]) {
    (void)u; (void)x; (void)y; (void)rgba;
    fprintf(stderr, "GL backend: SPLIT BUG: rt_put on the worker thread\n");
}
static int pgl_split_dummy_rt_raw(void *u, const unsigned char **b, unsigned *l) {
    (void)u; (void)b; (void)l;
    fprintf(stderr, "GL backend: SPLIT BUG: rt_raw on the worker thread\n");
    return 0;
}
static unsigned pgl_split_dummy_px_off(void *u, int x, int y) {
    (void)u; (void)x; (void)y;
    return 0;
}
static const PicaGlHost pgl_split_host = {
    pgl_split_dummy_rt_get, pgl_split_dummy_rt_put, NULL,
    pgl_split_dummy_rt_raw, pgl_split_dummy_px_off,
    NULL, NULL, NULL, NULL
};

static void pgl_split_op_free(PglSplitOp *op) {
    if (!op) return;
    free(op->verts);
    free(op->ref_blocks);
    free(op->ref_blob);
    free(op->ref_full);
    free(op->zref_full);
    free(op->fl_px);
    free(op->fl_z);
    free(op);
}

/* Guest producer: push (blocking only when the queue is full — a submitted
   draw may never be dropped). */
static void pgl_split_push(PglSplitOp *op) {
    pthread_mutex_lock(&pgl_sq_mu);
    while (pgl_sq_count == PGL_SPLIT_QUEUE_CAP) {
        unsigned long long t0 = pica_pqt_now_ns();
        pthread_cond_wait(&pgl_sq_notfull, &pgl_sq_mu);
        atomic_fetch_add_explicit(&pgl_enq_full_waits,
                                  pica_pqt_now_ns() - t0, memory_order_relaxed);
    }
    op->seq = ++pgl_sq_pushed;
    pgl_sq[pgl_sq_head] = op;
    pgl_sq_head = (pgl_sq_head + 1u) % PGL_SPLIT_QUEUE_CAP;
    ++pgl_sq_count;
    pthread_cond_signal(&pgl_sq_notempty);
    pthread_mutex_unlock(&pgl_sq_mu);
}

/* Worker: publish completion.  Called only after the op's GL work AND any
   use of its buffers are finished; FLUSH read-back buffers stay allocated
   for the guest, which frees the op after the write-back. */
static void pgl_split_publish_done(PglSplitOp *op) {
    pthread_mutex_lock(&pgl_sq_mu);
    if (op->seq > pgl_sq_done_seq) pgl_sq_done_seq = op->seq;
    pthread_cond_broadcast(&pgl_sq_done);
    pthread_mutex_unlock(&pgl_sq_mu);
}

/* Guest thread: block until every submitted op has completed (flush barrier,
   staging-slot sync, shutdown drain). */
static void pgl_split_wait_drain(void) {
    unsigned long long want;
    pthread_mutex_lock(&pgl_sq_mu);
    want = pgl_sq_pushed;
    while (pgl_sq_done_seq < want)
        pthread_cond_wait(&pgl_sq_done, &pgl_sq_mu);
    pthread_mutex_unlock(&pgl_sq_mu);
}

/* pica_vs.c staging-decode hook: before overwriting any slot's staged bytes
   (the buffer the worker may still read for a queued draw), drain the queue
   so no queued/in-flight op can reference those bytes.  Decode misses are
   rare (~0.4/frame in the savestate scene), so a full drain is cheap. */
static void pica_gl_split_sync(void) {
    if (!pgl_split_on) return;
    pgl_split_wait_drain();
}

/* ---- push-time refresh capture (GUEST thread) ---------------------------
   Mirrors pgl_refresh/pgl_refresh_depth's decision tree, but every guest
   memory read happens HERE (at draw time) and only the result crosses the
   queue: clear colour, changed 8x8 block RGBA bytes, or a full decode. */
static int pgl_split_capture_refresh(PglSplitOp *op, const PicaGlState *st,
                                     const PicaGlHost *host) {
    const unsigned char *raw = NULL;
    unsigned len = 0, bpp = st->rt_bpp ? st->rt_bpp : 4u;
    int all = pgl_force_full;
    if (!host->rt_raw(host->user, &raw, &len)) return 0;
    if (!pgl_shadow || pgl_shadow_len != len || pgl_shadow_w != (int)st->rt_w ||
        pgl_shadow_h != (int)st->rt_h || pgl_shadow_bpp != bpp) {
        unsigned char *grown = (unsigned char *)realloc(pgl_shadow, len ? len : 1u);
        if (!grown) return 0;
        pgl_shadow = grown;
        pgl_shadow_len = len;
        pgl_shadow_w = (int)st->rt_w;
        pgl_shadow_h = (int)st->rt_h;
        pgl_shadow_bpp = bpp;
        memcpy(pgl_shadow, raw, len);
        pgl_force_full = 1; /* FBO content unknown for this target */
        all = 1;
    }
    op->ref_kind = PGL_REF_NONE;
    if (all || memcmp(pgl_shadow, raw, len) != 0) {
        int done = 0;
        unsigned L = 0, i;
        ++pgl_refills;
        for (i = 4; i >= 3; --i) {
            unsigned j;
            if (len < i) continue;
            for (j = 0; j < len; ++j)
                if (raw[j] != raw[j % i]) break;
            if (j == len) { L = i; break; }
        }
        if (L) {   /* uniform raw pattern: at most L colors -> one clear */
            unsigned char col[4][4], have[4] = {0, 0, 0, 0};
            int ok = 1, uniform = 1;
            unsigned r;
            for (i = 0; i < 16u; ++i) {
                unsigned char rgba[4];
                int px = (int)(i & 7u), py = (int)(i >> 3);
                if (!host->rt_get(host->user, px, py, rgba)) { ok = 0; break; }
                r = host->rt_px_off(host->user, px, py) % L;
                if (!have[r]) { memcpy(col[r], rgba, 4); have[r] = 1; }
            }
            for (r = 0; ok && r < L; ++r) {
                if (!have[r]) { ok = 0; break; }
                if (r && memcmp(col[r], col[0], 4)) uniform = 0;
            }
            if (ok && uniform) {
                memcpy(op->ref_col, col[0], 4);
                op->ref_kind = PGL_REF_CLEAR;
                ++pgl_cleared;
                done = 1;
            } else if (ok) {
                /* residue-class pattern: decode fully (rare) */
                unsigned x, y;
                op->ref_full = (unsigned char *)malloc((size_t)st->rt_w * st->rt_h * 4u);
                if (!op->ref_full) return 0;
                for (y = 0; y < st->rt_h; ++y)
                    for (x = 0; x < st->rt_w; ++x) {
                        r = host->rt_px_off(host->user, (int)x, (int)y) % L;
                        memcpy(op->ref_full + ((size_t)y * st->rt_w + x) * 4u,
                               col[r], 4);
                    }
                op->ref_kind = PGL_REF_FULL;
                done = 1;
            }
        }
        if (!done) {
            unsigned blocks_x = st->rt_w / 8u, bs = 64u * bpp;
            if (blocks_x && (st->rt_w % 8u) == 0 && (st->rt_h % 8u) == 0 &&
                (size_t)bs * blocks_x * (st->rt_h / 8u) <= (size_t)len) {
                unsigned nblocks = blocks_x * (st->rt_h / 8u), bi;
                op->ref_blocks = (unsigned *)malloc((size_t)nblocks * sizeof(unsigned));
                op->ref_blob = (unsigned char *)malloc((size_t)nblocks * 256u);
                if (!op->ref_blocks || !op->ref_blob) return 0;
                for (bi = 0; bi < nblocks; ++bi) {
                    unsigned bx = bi % blocks_x, by = bi / blocks_x;
                    unsigned row, ci;
                    int y0;
                    unsigned char *dst;
                    if (!all &&
                        memcmp(pgl_shadow + (size_t)bi * bs, raw + (size_t)bi * bs, bs) == 0)
                        continue;
                    /* block (bx,by) = top-down rows [by*8, by*8+8); GL rows
                       are bottom-up (same y0 math as pgl_refresh). */
                    y0 = (int)st->rt_h - 1 - (int)(by * 8u + 7u);
                    dst = op->ref_blob + (size_t)op->ref_nblocks * 256u;
                    for (row = 0; row < 8u; ++row)
                        for (ci = 0; ci < 8u; ++ci) {
                            unsigned char rgba[4];
                            if (!host->rt_get(host->user, (int)(bx * 8u + ci),
                                              y0 + (int)row, rgba))
                                memset(rgba, 0, 4);
                            memcpy(dst + ((size_t)row * 8u + ci) * 4u, rgba, 4);
                        }
                    op->ref_blocks[op->ref_nblocks++] = bi;
                }
                op->ref_kind = PGL_REF_BLOCKS;
                done = 1;
            } else {
                unsigned x, y;
                op->ref_full = (unsigned char *)malloc((size_t)st->rt_w * st->rt_h * 4u);
                if (!op->ref_full) return 0;
                for (y = 0; y < st->rt_h; ++y)
                    for (x = 0; x < st->rt_w; ++x) {
                        unsigned char rgba[4];
                        if (!host->rt_get(host->user, (int)x, (int)y, rgba))
                            memset(rgba, 0, 4);
                        memcpy(op->ref_full + ((size_t)y * st->rt_w + x) * 4u,
                               rgba, 4);
                    }
                op->ref_kind = PGL_REF_FULL;
                done = 1;
            }
        }
        memcpy(pgl_shadow, raw, len);
        pgl_force_full = 0;
    }
    /* ---- depth region (same decision tree as pgl_refresh_depth) ---- */
    op->zref_kind = PGL_ZREF_NONE;
    if (st->depth_on && host->depth_raw) {
        const unsigned char *zraw = NULL;
        unsigned zlen = 0, zbpp = st->depth_bpp ? st->depth_bpp : 2u;
        if (!host->depth_raw(host->user, &zraw, &zlen)) return 0;
        if (!pgl_zshadow || pgl_zshadow_len != zlen ||
            pgl_zshadow_w != (int)st->rt_w || pgl_zshadow_h != (int)st->rt_h ||
            pgl_zshadow_bpp != zbpp) {
            unsigned char *grown = (unsigned char *)realloc(pgl_zshadow, zlen ? zlen : 1u);
            if (!grown) return 0;
            pgl_zshadow = grown;
            pgl_zshadow_len = zlen;
            pgl_zshadow_w = (int)st->rt_w;
            pgl_zshadow_h = (int)st->rt_h;
            pgl_zshadow_bpp = zbpp;
            memcpy(pgl_zshadow, zraw, zlen);
            pgl_zforce_full = 1;
        }
        if (pgl_zforce_full || memcmp(pgl_zshadow, zraw, zlen) != 0) {
            unsigned z0 = zraw[0] | ((unsigned)zraw[1] << 8) |
                          (zbpp > 2u ? ((unsigned)zraw[2] << 16) : 0u);
            unsigned i;
            int uniform = 1;
            ++pgl_zrefills;
            for (i = 0; i + zbpp <= zlen; i += zbpp) {
                unsigned z = zraw[i] | ((unsigned)zraw[i + 1] << 8) |
                             (zbpp > 2u ? ((unsigned)zraw[i + 2] << 16) : 0u);
                if (z != z0) { uniform = 0; break; }
            }
            if (uniform) {
                op->zref_kind = PGL_ZREF_CLEAR;
                op->zref_z = z0;
            } else {
                unsigned x, y;
                op->zref_full = (float *)malloc((size_t)st->rt_w * st->rt_h * sizeof(float));
                if (!op->zref_full) return 0;
                for (y = 0; y < st->rt_h; ++y)
                    for (x = 0; x < st->rt_w; ++x) {
                        unsigned z = 0;
                        if (host->depth_get &&
                            !host->depth_get(host->user, (int)x, (int)y, &z))
                            z = 0;
                        op->zref_full[(size_t)y * st->rt_w + x] = (float)z;
                    }
                op->zref_kind = PGL_ZREF_FULL;
            }
            memcpy(pgl_zshadow, zraw, zlen);
            pgl_zforce_full = 0;
        }
    }
    return 1;
}

/* Apply the captured refresh on the WORKER (GL thread only). */
static int pgl_split_apply_refresh(const PicaGlState *st) {
    PglSplitOp *op = pgl_split_cur;
    if (!op) return 0;
    switch (op->ref_kind) {
    case PGL_REF_CLEAR:
        pgl_glBindFramebuffer(PGGL_FRAMEBUFFER, pgl_fbo);
        pgl_obj_used |= 8u;
        pgl_glColorMask(1, 1, 1, 1);
        pgl_glClearColor(op->ref_col[0] / 255.0f, op->ref_col[1] / 255.0f,
                         op->ref_col[2] / 255.0f, op->ref_col[3] / 255.0f);
        pgl_glClear(PGGL_COLOR_BUFFER_BIT);
        pgl_drain_errors("rt-clear", NULL);
        break;
    case PGL_REF_BLOCKS: {
        unsigned i, blocks_x = st->rt_w / 8u;
        if (!blocks_x) return 0;
        pgl_glBindTexture(PGGL_TEXTURE_2D, pgl_rt_tex);
        pgl_obj_used |= 16u;
        for (i = 0; i < op->ref_nblocks; ++i) {
            unsigned bi = op->ref_blocks[i];
            unsigned bx = bi % blocks_x, by = bi / blocks_x;
            int y0 = (int)st->rt_h - 1 - (int)(by * 8u + 7u);
            pgl_glTexSubImage2D(PGGL_TEXTURE_2D, 0, (PGint)(bx * 8u), (PGint)y0,
                                8, 8, PGGL_RGBA, PGGL_UNSIGNED_BYTE,
                                op->ref_blob + (size_t)i * 256u);
        }
        pgl_drain_errors("rt-subupload", NULL);
        break;
    }
    case PGL_REF_FULL:
        pgl_glBindTexture(PGGL_TEXTURE_2D, pgl_rt_tex);
        pgl_obj_used |= 16u;
        pgl_glTexImage2D(PGGL_TEXTURE_2D, 0, PGGL_RGBA8, (PGsizei)st->rt_w,
                         (PGsizei)st->rt_h, 0, PGGL_RGBA, PGGL_UNSIGNED_BYTE,
                         op->ref_full);
        pgl_drain_errors("rt-upload", NULL);
        break;
    }
    switch (op->zref_kind) {
    case PGL_ZREF_CLEAR:
        /* DQ8_GL_ZCLEAR=0 keeps the legacy no-op clear (reference mode). */
        if (pgl_zclear_on) {
            PGfloat v[4];
            PGenum zbufs[2] = { PGGL_COLOR_ATTACHMENT0, PGGL_COLOR_ATTACHMENT1 };
            v[0] = (PGfloat)op->zref_z; v[1] = 0.0f; v[2] = 0.0f; v[3] = 1.0f;
            pgl_glBindFramebuffer(PGGL_FRAMEBUFFER, pgl_fbo);
            pgl_obj_used |= 8u;
            pgl_glColorMask(1, 1, 1, 1);
            pgl_glDrawBuffers(2, zbufs);
            pgl_glClearBufferfv(PGGL_COLOR, 1, v);
            if (pgl_drain_errors("z-clear", NULL) != 0)
                pgl_request_resync(); /* guest re-sends a full z refresh */
        }
        break;
    case PGL_ZREF_FULL:
        pgl_glBindTexture(PGGL_TEXTURE_2D, pgl_z_tex);
        pgl_obj_used |= 128u;
        pgl_glTexImage2D(PGGL_TEXTURE_2D, 0, PGGL_R32F, (PGsizei)st->rt_w,
                         (PGsizei)st->rt_h, 0, PGGL_RED, PGGL_FLOAT,
                         op->zref_full);
        pgl_drain_errors("z-upload", NULL);
        break;
    }
    return 1;
}

/* Worker: execute one DRAW op through the existing draw_locked body (with
   pgl_split_cur set, its guest-memory sections are replaced by the captured
   refresh above). */
static void pgl_split_do_draw(PglSplitOp *op) {
    pgl_split_cur = op;
    if (!pica_gl_draw_locked(op->verts, op->nverts, &op->st, &pgl_split_host))
        ++pgl_split_draw_fail;
    pgl_split_cur = NULL;
    ++pgl_draws;
}

/* Worker: execute one FLUSH op = read the drawn box back into owned buffers.
   The guest thread writes them into memory after done_seq. */
static void pgl_split_do_flush(PglSplitOp *op) {
    int x0 = op->fl_box[0], y0 = op->fl_box[1];
    int x1 = op->fl_box[2], y1 = op->fl_box[3];
    unsigned w = (unsigned)(x1 - x0 + 1), h = (unsigned)(y1 - y0 + 1);
    op->fl_px = (unsigned char *)malloc((size_t)w * h * 4u);
    if (!op->fl_px) { op->fl_rb_failed = 1; return; }
    pgl_glBindFramebuffer(PGGL_FRAMEBUFFER, pgl_fbo);
    pgl_obj_used |= 8u;
    pgl_glReadBuffer(PGGL_COLOR_ATTACHMENT0);
    pgl_glReadPixels(x0, y0, (PGsizei)w, (PGsizei)h, PGGL_RGBA,
                     PGGL_UNSIGNED_BYTE, op->fl_px);
    {
        PGenum rb_err = 0;
        if (pgl_drain_errors("readback", &rb_err) != 0) {
            fprintf(stderr, "GL backend: split readback failed: err=0x%04X %s "
                    "(rt=%ux%u box=%d,%d..%d,%d)\n", (unsigned)rb_err,
                    pgl_err_name(rb_err), pgl_rt_w, pgl_rt_h, x0, y0, x1, y1);
            free(op->fl_px);
            op->fl_px = NULL;
            op->fl_rb_failed = 1;
            pgl_request_resync();
            return;
        }
    }
    pgl_dump_snap("color_fbo_last.raw", op->fl_px, (size_t)w * h * 4u);
    pgl_dump_box("color_fbo_last.box", x0, y0, x1, y1);
    if (op->fl_have_depth) {
        op->fl_z = pgl_rb_depth(x0, y0, x1, y1);
        if (!op->fl_z) op->fl_rb_failed = 1;
    }
    ++pgl_split_flush_ops;
}

/* Worker thread body. */
static void *pgl_split_main(void *arg) {
    (void)arg;
    pgl_thread_id = pgl_tid();
    if (!pgl_bringup()) {
        atomic_store_explicit(&pgl_split_ready, -1, memory_order_release);
        return NULL;
    }
    /* pgl_bringup restores whatever was current before it (the synchronous
       wrapper rebinds per draw via pgl_context_take; the worker does not).
       Keep OUR context current on this thread for the worker's lifetime —
       without this every GL call fails with "no context" (glCreateShader
       returns 0 with err=0x0000 and every draw is dropped). */
    if (!pgl_external_ctx &&
        !eglMakeCurrent(pgl_dpy, pgl_surf, pgl_surf, pgl_ctx)) {
        fprintf(stderr, "GL backend: worker eglMakeCurrent failed 0x%04X\n",
                eglGetError());
        atomic_store_explicit(&pgl_split_ready, -1, memory_order_release);
        return NULL;
    }
    atomic_store_explicit(&pgl_split_ready, 1, memory_order_release);
    for (;;) {
        PglSplitOp *op = NULL;
        int stop_now = 0;
        pthread_mutex_lock(&pgl_sq_mu);
        while (!pgl_sq_count && !pgl_sq_stop)
            pthread_cond_wait(&pgl_sq_notempty, &pgl_sq_mu);
        if (pgl_sq_count) {
            op = pgl_sq[pgl_sq_tail];
            pgl_sq[pgl_sq_tail] = NULL;
            pgl_sq_tail = (pgl_sq_tail + 1u) % PGL_SPLIT_QUEUE_CAP;
            --pgl_sq_count;
            pthread_cond_signal(&pgl_sq_notfull);
        } else {
            stop_now = pgl_sq_stop; /* empty and stopping */
        }
        pthread_mutex_unlock(&pgl_sq_mu);
        if (!op) { if (stop_now) break; continue; }
        if (op->kind == PGL_OP_DRAW) {
            pgl_split_do_draw(op);
            pgl_split_publish_done(op);
            pgl_split_op_free(op); /* DRAW ops are worker-owned */
        } else {
            pgl_split_do_flush(op);
            pgl_split_publish_done(op);
            /* FLUSH ops carry the read-back for the guest: it frees them. */
        }
    }
    /* Teardown on our own thread (the context is current here). */
    {
        unsigned i;
        for (i = 0; i < PGL_UP_SLOTS; ++i) {
            free(pgl_up[i].copy);
            pgl_up[i].copy = NULL;
            pgl_up[i].bytes = 0;
            pgl_up[i].used = 0;
        }
        free(pgl_vbo_stage);
        pgl_vbo_stage = NULL;
        if (!pgl_external_ctx && pgl_dpy != EGL_NO_DISPLAY) {
            eglMakeCurrent(pgl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (pgl_ctx != EGL_NO_CONTEXT) eglDestroyContext(pgl_dpy, pgl_ctx);
            if (pgl_surf != EGL_NO_SURFACE) eglDestroySurface(pgl_dpy, pgl_surf);
            eglTerminate(pgl_dpy);
            pgl_dpy = EGL_NO_DISPLAY;
        }
    }
    return NULL;
}

static int pgl_split_start(void) {
    unsigned i;
    /* Queue init is static (PTHREAD_*_INITIALIZER); just reset cursors. */
    pgl_sq_head = pgl_sq_tail = pgl_sq_count = 0;
    pgl_sq_pushed = pgl_sq_done_seq = 0;
    pgl_sq_stop = 0;
    atomic_store_explicit(&pgl_split_ready, 0, memory_order_relaxed);
    pgl_split_on = 1; /* set BEFORE the thread runs so every guarded branch
                         on the worker behaves as split */
    if (pthread_create(&pgl_split_thread, NULL, pgl_split_main, NULL) != 0) {
        pgl_split_on = 0;
        return 0;
    }
    for (i = 0; i < 5000u; ++i) {
        int r = atomic_load_explicit(&pgl_split_ready, memory_order_acquire);
        if (r > 0) {
            fprintf(stderr, "GL backend: worker thread ready (DQ8_SPLIT_GL=1)\n");
            return 1;
        }
        if (r < 0) {
            pthread_join(pgl_split_thread, NULL);
            pgl_split_on = 0;
            return 0;
        }
        {
            struct timespec ts = { 0, 1000000L };
            nanosleep(&ts, NULL);
        }
    }
    fprintf(stderr, "GL backend: worker thread init timeout\n");
    pthread_join(pgl_split_thread, NULL);
    pgl_split_on = 0;
    return 0;
}

/* Guest thread: enqueue one draw. */
static unsigned pica_gl_draw_split(const PicaGlVertex *verts, unsigned nverts,
                                   const PicaGlState *st, const PicaGlHost *host) {
    PglSplitOp *op;
    unsigned long long t0, t1;
    unsigned ntri = nverts / 3u, i;
    float minx, maxx, miny, maxy;
    int bx0, by0, bx1, by1;
    /* Same validation as pica_gl_draw_locked's early-outs (a rejected draw
       must still fall back to the software rasterizer). */
    if (!ntri || !st || !host || !host->rt_get || !host->rt_put) return 0;
    if (!st->rt_w || !st->rt_h) return 0;
    if (st->mode == PICA_GL_TEV && st->tev.tex_en &&
        (!st->tex_rgba || st->tex_w <= 0 || st->tex_h <= 0)) return 0;
    /* The split needs raw batched access for push-time refresh + write-back. */
    if (!host->rt_raw || !host->rt_px_off) return 0;
    t0 = pica_pqt_now_ns();
    /* Fold a worker-raised resync request into the guest-owned force flags. */
    if (atomic_exchange_explicit(&pgl_split_resync, 0, memory_order_acq_rel)) {
        pgl_force_full = 1;
        pgl_zforce_full = 1;
    }
    /* Close the episode before an RT resize (the synchronous path does this
       inline in draw_locked; here it blocks on the flush barrier). */
    if (pgl_pending && (pgl_shadow_w != (int)st->rt_w ||
                        pgl_shadow_h != (int)st->rt_h))
        pica_gl_flush(host);
    op = (PglSplitOp *)calloc(1, sizeof *op);
    if (!op) return 0;
    op->kind = PGL_OP_DRAW;
    op->st = *st;
    op->nverts = nverts;
    op->verts = (PicaGlVertex *)malloc((size_t)nverts * sizeof(PicaGlVertex));
    if (!op->verts) { pgl_split_op_free(op); return 0; }
    memcpy(op->verts, verts, (size_t)nverts * sizeof(PicaGlVertex));
    if (!pgl_split_capture_refresh(op, st, host)) {
        pgl_split_op_free(op);
        return 0;
    }
    /* Drawn box / zdirty / pending are GUEST state under the split (the
       worker must never touch them).  Same bbox math as draw_locked. */
    minx = maxx = verts[0].x;
    miny = maxy = verts[0].y;
    for (i = 1; i < nverts; ++i) {
        if (verts[i].x < minx) minx = verts[i].x;
        if (verts[i].x > maxx) maxx = verts[i].x;
        if (verts[i].y < miny) miny = verts[i].y;
        if (verts[i].y > maxy) maxy = verts[i].y;
    }
    bx0 = (int)floorf(minx) - 1;
    by0 = (int)floorf(miny) - 1;
    bx1 = (int)ceilf(maxx) + 1;
    by1 = (int)ceilf(maxy) + 1;
    if (bx0 < 0) bx0 = 0;
    if (by0 < 0) by0 = 0;
    if (bx1 > (int)st->rt_w - 1) bx1 = (int)st->rt_w - 1;
    if (by1 > (int)st->rt_h - 1) by1 = (int)st->rt_h - 1;
    if (bx0 <= bx1 && by0 <= by1) {
        if (bx0 < pgl_drawn_x0) pgl_drawn_x0 = bx0;
        if (by0 < pgl_drawn_y0) pgl_drawn_y0 = by0;
        if (bx1 > pgl_drawn_x1) pgl_drawn_x1 = bx1;
        if (by1 > pgl_drawn_y1) pgl_drawn_y1 = by1;
    }
    if (st->depth_on) pgl_zdirty = 1;
    pgl_pending = 1;
    pgl_split_push(op);
    t1 = pica_pqt_now_ns();
    atomic_fetch_add_explicit(&pgl_enq_n, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&pgl_enq_ns, t1 - t0, memory_order_relaxed);
    return ntri;
}

/* Guest thread: the flush barrier. */
static void pgl_split_flush(const PicaGlHost *host) {
    PglSplitOp *op;
    int have_raw = host->rt_raw && host->rt_px_off;
    int x0 = pgl_drawn_x0, y0 = pgl_drawn_y0, x1 = pgl_drawn_x1, y1 = pgl_drawn_y1;
    /* Clamp against the GUEST-owned episode dims (pgl_shadow_*): pgl_rt_w/h
       are worker state and may still describe the previous target while the
       worker has not processed this episode's first draw yet. */
    int rw = pgl_shadow_w, rh = pgl_shadow_h;
    unsigned w, h;
    pgl_pending = 0; /* mirror pica_gl_flush_locked */
    if (x0 > x1 || y0 > y1) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > rw - 1) x1 = rw - 1;
    if (y1 > rh - 1) y1 = rh - 1;
    if (x0 > x1 || y0 > y1) return;
    w = (unsigned)(x1 - x0 + 1);
    h = (unsigned)(y1 - y0 + 1);
    op = (PglSplitOp *)calloc(1, sizeof *op);
    if (!op) { pgl_force_full = 1; pgl_reset_drawn(); return; }
    op->kind = PGL_OP_FLUSH;
    op->fl_box[0] = x0; op->fl_box[1] = y0;
    op->fl_box[2] = x1; op->fl_box[3] = y1;
    op->fl_have_depth = pgl_zdirty;
    pgl_zdirty = 0; /* mirror pica_gl_flush_depth_locked's clear */
    pgl_split_push(op);
    {   /* condvar barrier: block until this op's read-back is done */
        pthread_mutex_lock(&pgl_sq_mu);
        while (pgl_sq_done_seq < op->seq)
            pthread_cond_wait(&pgl_sq_done, &pgl_sq_mu);
        pthread_mutex_unlock(&pgl_sq_mu);
    }
    if (op->fl_rb_failed) {
        pgl_force_full = 1; /* resync already requested by the worker */
        pgl_reset_drawn();
        pgl_split_op_free(op);
        return;
    }
    pgl_wb_color(host, have_raw, op->fl_px, x0, y0, x1, y1);
    if (op->fl_z) pgl_wb_depth(host, have_raw, op->fl_z, w, h, x0, y0, x1, y1);
    ++pgl_flushes;
    if (pgl_debug && (pgl_flushes <= 8u || (pgl_flushes % 200u) == 0u))
        fprintf(stderr, "GL debug: split flush #%llu box=%d,%d..%d,%d rt=%dx%d\n",
                (unsigned long long)pgl_flushes, x0, y0, x1, y1, rw, rh);
    pgl_reset_drawn();
    if (pgl_rect_trace)
        fprintf(stderr, "GLRECT split flush#%llu box=%d,%d..%d,%d rt=%dx%d\n",
                (unsigned long long)pgl_flushes, x0, y0, x1, y1, rw, rh);
    pgl_split_op_free(op);
}

/* Guest thread: shutdown — request stop, join (the worker drains the queue
   first and then destroys the GL context on its own thread). */
static void pgl_split_stop(void) {
    pthread_mutex_lock(&pgl_sq_mu);
    pgl_sq_stop = 1;
    pthread_cond_broadcast(&pgl_sq_notempty);
    pthread_mutex_unlock(&pgl_sq_mu);
    pthread_join(pgl_split_thread, NULL);
}

#else /* !DQ8_ENABLE_GL */

static int pica_gl_init(void) {
    const char *env = getenv("DQ8_GL");
    if (env && env[0] && env[0] != '0')
        fprintf(stderr, "GL backend: not compiled in; rebuild with DQ8_ENABLE_GL "
                "(software rasterizer remains active)\n");
    return 0;
}
static int pica_gl_init_external(void *(*getproc)(const char *)) {
    (void)getproc;
    return 0;
}
static void pica_gl_context_release(void) {}
static int pica_gl_active(void) { return 0; }
/* t_cb8723ae: staging-decode sync hook (no GL build = no queue). */
static void pica_gl_split_sync(void) {}
static unsigned long long pgl_tex_uploads, pgl_tex_upload_bytes,
    pgl_tex_hits, pgl_tex_misses; /* zeroed stubs for the shared stats line */
static unsigned long long pica_gl_draw_count(void) { return 0; }
static const char *pica_gl_renderer(void) { return "not compiled in"; }
static void pica_gl_shutdown(void) {}
static int pica_gl_pending(void) { return 0; }
static void pica_gl_flush(const PicaGlHost *host) { (void)host; }
static unsigned pica_gl_draw(const PicaGlVertex *verts, unsigned nverts,
                             const PicaGlState *st, const PicaGlHost *host) {
    (void)verts; (void)nverts; (void)st; (void)host;
    return 0;
}

#endif /* DQ8_ENABLE_GL */

#endif /* PICA_GL_C */
