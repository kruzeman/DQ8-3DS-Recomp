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
#include <stdarg.h>
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
    float x, y, iw, zw, color[4], u, v, u1, v1, cx, cy, cz, cw;
} PicaGlVertex; /* layout-compatible with the software path's PicaVertex */

/* VBO payload (t_bc6b4eb7): ONE record per TRIANGLE, read by all 3 rendered
   vertices through divisor-1 attributes (instanced draw), so every FLAT
   input of the fragment shader receives the full triangle exactly like the
   old layout where each vertex carried a 3x-expanded copy (38 floats =
   152 B/vertex -> 456 B/triangle).  Only the varyings the shader consumes
   are kept: gNc carries (u, v) for texture unit 0 and (u1, v1) for unit 1 —
   the old vec4 carried two always-zero padding floats.  144 B/triangle,
   written once, uploaded once. */
typedef struct {
    float a[3][4]; /* gNa: (x, y, iw, zw) of triangle vertex 0/1/2 */
    float b[3][4]; /* gNb: color[4] of triangle vertex 0/1/2 */
    float c[3][4]; /* gNc: (u, v, u1, v1) of triangle vertex 0/1/2 */
} PicaGlTriPayload;
_Static_assert(sizeof(PicaGlTriPayload) == 144, "payload layout");
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
    /* t_dd830a74 item 2: texture unit 0 is the CURRENT render target sampled
       at the internal scale.  tex_rgba is NULL and the draw binds
       pgl_snap_tex (a full-resolution copy of the scaled backing) instead of
       the staged native image; normalised UVs need no change. */
    int tex_is_rt;
    /* t_c043d7f7: guest texture filter bits (PARAM register 0x83):
       bit1 mag_filter, bit2 min_filter, bit24 mip_filter (0=nearest,
       1=linear) and the LOD word (0x84: bias bits 0-12, max_level 16-19,
       min_level 24-27).  The GL path applies mag/min to the sampler; mip
       levels are not uploaded (see pgl_tex_filter_apply), so a mip_filter
       request degrades to bilinear instead of sampling an incomplete
       texture (which would be black). */
    unsigned mag_filter, min_filter, mip_filter;
    unsigned lod_word;
    /* t_e216fc37: texture unit 1 (TEV source 4).  Only filled in when the
       TEV chain of this draw actually reads source 4 AND unit 1 is enabled
       (GPUREG 0x80 bit 1); the staged image comes from pica_vs.c's unit-1
       staging cache, which keys on the unit-1 registers
       0x91/0x92/0x93/0x95/0x96.  tex1_gen/tex1_slot are the same
       (slot,generation) identity the unit-0 upload cache uses. */
    int tex1_en;
    const unsigned char *tex1_rgba;
    int tex1_w, tex1_h;
    unsigned tex1_hash, tex1_slot;
    unsigned long long tex1_gen;
    unsigned wrap_s1, wrap_t1;
    unsigned mag_filter1, min_filter1, mip_filter1;
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
#include <unistd.h>
#include <sys/syscall.h>   /* FREEZE WATCHDOG (t_4371e6ea): hold owner tid */

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
#define PGGL_READ_FRAMEBUFFER     0x8CA8u
#define PGGL_DRAW_FRAMEBUFFER     0x8CA9u
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
#define PGGL_TEXTURE3             0x84C3u
#define PGGL_TEXTURE_MIN_FILTER   0x2801u
#define PGGL_TEXTURE_MAG_FILTER   0x2800u
#define PGGL_NEAREST              0x2600u
#define PGGL_LINEAR               0x2601u
#define PGGL_TEXTURE_WRAP_S       0x2802u
#define PGGL_TEXTURE_WRAP_T       0x2803u
#define PGGL_CLAMP_TO_EDGE        0x812Fu
#define PGGL_REPEAT               0x2901u
#define PGGL_MIRRORED_REPEAT      0x8370u
/* t_6b81015d: glMemoryBarrier bits (GL 4.2/ARB_shader_image_load_store). */
#define PGGL_TEXTURE_FETCH_BARRIER_BIT        0x00000008u
#define PGGL_SHADER_IMAGE_ACCESS_BARRIER_BIT  0x00000020u
#define PGGL_FRAMEBUFFER_BARRIER_BIT          0x00000400u
#define PGGL_ARRAY_BUFFER         0x8892u
#define PGGL_DYNAMIC_DRAW         0x88E8u
#define PGGL_VERTEX_SHADER        0x8B31u
#define PGGL_FRAGMENT_SHADER      0x8B30u
#define PGGL_COMPILE_STATUS       0x8B81u
#define PGGL_LINK_STATUS          0x8B82u
#define PGGL_FRAMEBUFFER          0x8D40u
#define PGGL_COLOR_ATTACHMENT0    0x8CE0u
#define PGGL_COLOR_ATTACHMENT1    0x8CE1u
/* t_a22413f1: real depth attachment.  The emulated PICA depth still lives in
   ATT1 (R32F, raw integer z) because that is what the guest write-back reads,
   but the depth TEST/WRITE itself is done by the rasterizer against this
   attachment, so fragments of one draw depth-test each other in raster order
   (the software path's semantics) instead of against a pre-draw snapshot. */
#define PGGL_DEPTH_ATTACHMENT     0x8D00u
#define PGGL_DEPTH_COMPONENT      0x1902u
#define PGGL_DEPTH_COMPONENT32F   0x8CACu
#define PGGL_DEPTH                0x1801u
#define PGGL_DEPTH_TEST           0x0B71u
#define PGGL_NEVER                0x0200u
#define PGGL_LESS                 0x0201u
#define PGGL_EQUAL                0x0202u
#define PGGL_LEQUAL               0x0203u
#define PGGL_GREATER              0x0204u
#define PGGL_NOTEQUAL             0x0205u
#define PGGL_GEQUAL               0x0206u
#define PGGL_ALWAYS               0x0207u
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
#define PGGL_SRC_ALPHA_SATURATE        0x0308u /* t_d559a510: PICA factor 14 */
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
typedef void (*PFN_glBlitFramebuffer)(PGint, PGint, PGint, PGint, PGint, PGint, PGint, PGint, unsigned, PGenum);
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
/* t_6b81015d: texture/framebuffer barriers for the framebuffer-fetch path. */
typedef void (*PFN_glTextureBarrier)(void);
typedef void (*PFN_glMemoryBarrier)(PGuint);
/* t_d559a510: per-draw-buffer blend state (GL 4.0 glBlendFunci/glBlendEquationi).
   Required by the fixed-function blend path because the depth buffer is an
   EMULATED depth buffer living in COLOR_ATTACHMENT1: GL_BLEND applies the
   same fixed-function blend to EVERY enabled draw buffer, so a blend draw
   would blend frag_z against the old depth instead of replacing it. */
typedef void (*PFN_glBlendFunci)(PGuint, PGenum, PGenum);
typedef void (*PFN_glBlendEquationi)(PGuint, PGenum);
/* t_993e6fb6: per-draw-buffer colour mask (GL 3.0 core, soft-loaded).  The
   PICA colour mask must gate ONLY the colour attachment: the emulated depth
   buffer lives in COLOR_ATTACHMENT1 and the software path writes depth even
   for colour-masked (depth-only) draws. */
typedef void (*PFN_glColorMaski)(PGuint, PGboolean, PGboolean, PGboolean,
                                 PGboolean);
/* t_a22413f1: hardware depth test/write state (core GL, must exist). */
typedef void (*PFN_glDepthFunc)(PGenum);
typedef void (*PFN_glDepthMask)(PGboolean);

#define PGL_FN(name) static PFN_##name pgl_##name
PGL_FN(glGetString);
PGL_FN(glGetStringi);      /* t_8c05fac8: extension query (soft-loaded) */
PGL_FN(glBindImageTexture); /* t_8c05fac8: framebuffer fetch (soft-loaded) */
PGL_FN(glTextureBarrier);  /* t_6b81015d: soft-loaded */
PGL_FN(glMemoryBarrier);   /* t_6b81015d: soft-loaded */
PGL_FN(glBlendFunci);      /* t_d559a510: soft-loaded (depth attachment) */
PGL_FN(glBlendEquationi);  /* t_d559a510: soft-loaded (depth attachment) */
PGL_FN(glColorMaski);      /* t_993e6fb6: soft-loaded (depth attachment) */
PGL_FN(glDepthFunc);       /* t_a22413f1: soft-loaded (real depth attachment) */
PGL_FN(glDepthMask);       /* t_a22413f1: soft-loaded (real depth attachment) */
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
PGL_FN(glBlitFramebuffer);   /* t_dd830a74: soft-loaded, scaled-sample snapshot */
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
             pgl_dst_tex, pgl_z_tex, pgl_zdst_tex, pgl_dtex;
static int pgl_depth_ok = 1;     /* t_a22413f1: glDepthFunc/glDepthMask loaded */
static int pgl_dt_w, pgl_dt_h;   /* attached depth texture size */
/* Applied hardware depth state (avoid redundant GL calls per draw). */
static int pgl_depth_test_applied = -1;   /* -1 unknown, 0 off, 1 on */
static unsigned pgl_depth_func_applied;   /* GL enum, valid when 1 */
static int pgl_depth_mask_applied = -1;
static int pgl_rt_w, pgl_rt_h;
/* ---- t_ab8159de: INTERNAL RENDER SCALE (DQ8_RENDER_SCALE=1..4) -----------
   The render target, its depth mirror and the real depth attachment are
   allocated at pgl_rt_w*f x pgl_rt_h*f and the fragment shader rasterises f*f
   samples per guest pixel; the write-back box-averages them back to one
   native texel per guest pixel.  pgl_rt_w/pgl_rt_h therefore keep their old
   meaning — GUEST pixels — and every guest-space box (draw bounds, refresh
   blocks, write-back boxes, pgl_shadow dims) is unchanged.  Everything that
   is handed to GL (texture sizes, viewport, read-back windows, FBO copy
   rectangles) goes through pgl_tgt_w/h and PGL_S() and is multiplied.
   At scale 1 every expression below collapses to the previous code. */
static int pgl_scale = 1;          /* DQ8_RENDER_SCALE: MAXIMUM internal scale, 1..4 */
static int pgl_cur_scale = 1;      /* scale the attached FBO was actually built at */
static int pgl_scale_init_done;
static int pgl_tgt_w, pgl_tgt_h;   /* GL target dims = guest * pgl_cur_scale */
static int pgl_rt_scale;           /* scale of the first ever FBO build (-1: none) */
#define PGL_S(v) ((v) * pgl_cur_scale)
static int pgl_scale_value(void) {
    if (!pgl_scale_init_done) {
        const char *e;
        pgl_scale_init_done = 1;
        e = getenv("DQ8_RENDER_SCALE");
        if (e && e[0]) {
            int v = atoi(e);
            if (v >= 1 && v <= 4) pgl_scale = v;
            else fprintf(stderr, "GL backend: DQ8_RENDER_SCALE=%s out of range "
                                 "(1..4) — using 1\n", e);
        }
    }
    return pgl_scale;
}

/* ---- t_dd830a74: AZAHAR SURFACE MODEL (DQ8_RENDER_SCALE_SAMPLED) ---------
   Azahar backs every surface at `res_scale` and keeps a native copy only for
   the guest-memory boundary (gl_texture_runtime.cpp:354-380, 436-505).  The
   consequence is that "the guest samples this target" is NOT a reason to
   render it native: the sample is taken from the scaled backing, and the only
   downsample in the whole pipeline is the download into guest RAM.

   This switch reproduces that rule.  Default ON (=1).  DQ8_RENDER_SCALE_SAMPLED=0
   restores the stage-A behaviour byte for byte (a target the guest samples is
   demoted to native for the rest of the run), which is the A/B reference. */
static int pgl_scale_sampled_gate = -1;
static int pgl_scale_sampled(void) {
    if (pgl_scale_sampled_gate < 0) {
        const char *e = getenv("DQ8_RENDER_SCALE_SAMPLED");
        pgl_scale_sampled_gate = (e && e[0] == '0' && !e[1]) ? 0 : 1;
    }
    return pgl_scale_sampled_gate;
}

/* ================= STAGE A TARGET GATING (t_05281b89) =====================
   A target is rendered at the internal scale only while NOTHING needs its
   pixels at native resolution inside the episode.  The one consumer that
   forces a target native in this stage is the guest sampling the target's
   memory as a TEXTURE (fb-fetch): pica_vs.c's staging path detects the
   overlap (pica_gl_pending() + region test at the texture staging sites),
   publishes the episode and then reads the texels from guest memory.  From
   that moment the same target must keep rendering natively, otherwise a
   later draw would blend into supersampled content the guest never saw —
   exactly the "no mixing" rule.

   Everything else the guest does with a target is a PUBLISH, and a publish
   is the down-sample point of this stage: the write-back hands guest memory
   one box-filtered native texel per guest pixel, so scan-out, display
   transfers, DMA, Y2R and host captures all keep operating on native-size
   data by construction.  A publish does not, by itself, take the target out
   of the scaled set — scale + down-sample reproduces the native image up to
   the filter, which is exactly what stage A promises.

   The marks are keyed on the guest target DIMENSIONS, not the address: two
   targets with the same size in one frame are unusual, and marking both
   native is the conservative direction (never incorrect). */
#define PGL_NATIVE_MAX 8
static struct { int w, h; } pgl_native_tgt[PGL_NATIVE_MAX];
static int pgl_native_n;
static unsigned long long pgl_gate_marks;   /* targets forced native, cumulative */
static unsigned long long pgl_texread_events; /* guest texture reads of a target */
static unsigned long long pgl_sampled_kept;   /* reads served without demoting */
/* t_dd830a74 item 2: unit-0 samples of a target served from the scaled
   backing (pica_gl_sample_scaled_target) and the cases that fell back to the
   native staged image. */
static unsigned long long pgl_snap_taken, pgl_snap_fallback;
/* t_fdd26eba item A: display transfers served at the internal scale, and the
   ones that had to keep the native path (with the reason counters). */
static unsigned long long pgl_tr_taken, pgl_tr_fallback, pgl_tr_ns;
static unsigned long long pgl_tr_fallback_mem, pgl_tr_fallback_gate;
/* t_fdd26eba item A: the identity of the target whose (scaled) content the
   attachment still holds AFTER the last publish.  The display transfer runs
   after the guest already consumed the frame (the episode is flushed), so the
   transfer has to be served from the *last published* episode, not only from
   a pending one: the attachment keeps that content until another draw touches
   it, which pgl_draws detects. */
static int pgl_cap_valid;
static uint32_t pgl_cap_addr;
static int pgl_cap_w, pgl_cap_h, pgl_cap_f;
static unsigned long long pgl_cap_draws, pgl_cap_flushes;
static int pgl_keep_marks;          /* the next flush is a guest texture read */
static unsigned long long pgl_flushseq;       /* publishes (flushes) so far */

/* Guest-space coverage accounting (stage A item 5).  A coarse cell grid over
   the frame's LARGEST target — in DQ8's field frame that is the 512x400
   presentation target every 3D+UI draw renders into, so the grid answers
   "how much of the top screen was drawn at the scaled size".  Cells are 8x8
   guest pixels: bit0 = touched by a scaled draw, bit1 = by a native one.
   Bounding boxes, not exact rasterisation — the GL path never sees the
   covered pixels on the CPU — so a draw's whole box counts as covered
   (documented in the report).  The grid is NOT cleared during the run: each
   cell keeps its LAST touch, so the report answers "of the pixels the final
   frame shows, how many were produced by a scaled draw". */
#define PGL_COV_CELL 8
#define PGL_COV_MAXW 128
static unsigned char *pgl_cov;
static int pgl_cov_w, pgl_cov_h;    /* cells across/down */
static int pgl_cov_gw, pgl_cov_gh;  /* guest dims the grid describes */
static unsigned long long pgl_cov_draws_scaled, pgl_cov_draws_native;
static unsigned long long pgl_cov_px_scaled, pgl_cov_px_native;
/* Per-target draw tally: which target rendered at which scale, and how many
   draws it took.  This is the "share of draws" half of the coverage answer. */
#define PGL_TGTSTAT_MAX 8
static struct { int w, h, scale; unsigned long long draws, native; } pgl_tgtstat[PGL_TGTSTAT_MAX];
static int pgl_tgtstat_n;

static void pgl_tgtstat_add(int w, int h, int scale) {
    int i;
    for (i = 0; i < pgl_tgtstat_n; ++i)
        if (pgl_tgtstat[i].w == w && pgl_tgtstat[i].h == h) break;
    if (i == pgl_tgtstat_n) {
        if (pgl_tgtstat_n >= PGL_TGTSTAT_MAX) return;
        pgl_tgtstat[i].w = w;
        pgl_tgtstat[i].h = h;
        pgl_tgtstat[i].draws = 0;
        ++pgl_tgtstat_n;
    }
    pgl_tgtstat[i].scale = scale;
    ++pgl_tgtstat[i].draws;
    if (scale <= 1) ++pgl_tgtstat[i].native;
}

static int pgl_target_native(int w, int h) {
    int i;
    for (i = 0; i < pgl_native_n; ++i)
        if (pgl_native_tgt[i].w == w && pgl_native_tgt[i].h == h) return 1;
    return 0;
}

/* Called by pica_vs.c right before it publishes the episode to sample the
   target as a texture: this target must render natively from now on.  The
   mark is STICKY FOR THE RUN (conservative by design: once the guest has
   depended on a target's native pixels, that size never renders scaled again
   in this session; a run that wants the scale back is restarted). */
void pica_gl_note_target_sampled(unsigned w, unsigned h) {
    if (pgl_scale_value() <= 1) return;
    ++pgl_texread_events;
    /* The flush that follows this call must not clear the mark: it is the
       publish the guest is about to sample from. */
    pgl_keep_marks = 1;
    if (pgl_scale_sampled()) {
        /* t_dd830a74 (Azahar surface model): the surface KEEPS its scaled
           backing.  The guest's sample is served either from the scaled
           texture (pica_gl_snapshot_target / pgl_snap_tex, item 2) or, when
           that path does not apply, from the box-filtered download the caller
           is about to perform (item 3).  Either way the target is never
           demoted to native, so a "sampled => NATIVE from now on" line can
           never be printed again. */
        ++pgl_sampled_kept;
        if (pgl_texread_events <= 8u || (pgl_texread_events % 100u) == 0u)
            fprintf(stderr, "GL backend: render scale: guest texture read #%llu "
                            "of the current target %ux%u (after %llu publishes) "
                            "— target STAYS x%d, sampled from the scaled backing\n",
                    pgl_texread_events, w, h, pgl_flushseq, pgl_scale_value());
        return;
    }
    if (pgl_texread_events <= 8u || (pgl_texread_events % 100u) == 0u)
        fprintf(stderr, "GL backend: render scale: guest texture read #%llu of "
                        "the current target %ux%u (after %llu publishes)\n",
                pgl_texread_events, w, h, pgl_flushseq);
    if (pgl_target_native((int)w, (int)h)) return;
    if (pgl_native_n < PGL_NATIVE_MAX) {
        pgl_native_tgt[pgl_native_n].w = (int)w;
        pgl_native_tgt[pgl_native_n].h = (int)h;
        ++pgl_native_n;
        ++pgl_gate_marks;
        fprintf(stderr, "GL backend: render scale: target %ux%u is sampled as a "
                        "texture by the guest — rendering it NATIVE from now on "
                        "(mark #%d, stage B territory)\n",
                w, h, pgl_native_n);
    }
}

/* The scale the FBO for a guest target of this size must be built at.
   t_dd830a74: with the Azahar surface model the answer is ALWAYS f — a target
   is never demoted because the guest samples it.  The stage-A demotion stays
   reachable through DQ8_RENDER_SCALE_SAMPLED=0 for A/B runs only. */
static int pgl_scale_for(int w, int h) {
    int f = pgl_scale_value();
    if (f <= 1) return 1;
    if (pgl_scale_sampled()) return f;
    return pgl_target_native(w, h) ? 1 : f;
}

/* One line per SURFACE (guest w,h) the first time it is created with a scaled
   backing, so a log shows what is scaled without one line per FBO rebuild. */
#define PGL_SURF_MAX 12
static struct { int w, h, f; } pgl_surf_seen[PGL_SURF_MAX];
static int pgl_surf_seen_n;
static void pgl_surface_created_scaled(int w, int h, int f) {
    int i;
    if (f <= 1) return;
    for (i = 0; i < pgl_surf_seen_n; ++i)
        if (pgl_surf_seen[i].w == w && pgl_surf_seen[i].h == h &&
            pgl_surf_seen[i].f == f)
            return;
    if (pgl_surf_seen_n >= PGL_SURF_MAX) return;
    pgl_surf_seen[pgl_surf_seen_n].w = w;
    pgl_surf_seen[pgl_surf_seen_n].h = h;
    pgl_surf_seen[pgl_surf_seen_n].f = f;
    ++pgl_surf_seen_n;
    fprintf(stderr, "GL backend: render scale: surface %dx%d created scaled "
                    "x%d (%dx%d colour backing + R32F depth mirror)\n",
            w, h, f, w * f, h * f);
}

/* Point the coverage grid at a target of these guest dims (only if it is at
   least as large as the one it describes — the presentation target wins). */
static void pgl_cov_track(int gw, int gh) {
    int cw, ch;
    if (gw <= 0 || gh <= 0) return;
    if (pgl_cov && gw * gh < pgl_cov_gw * pgl_cov_gh) return;
    cw = (gw + PGL_COV_CELL - 1) / PGL_COV_CELL;
    ch = (gh + PGL_COV_CELL - 1) / PGL_COV_CELL;
    if (cw == pgl_cov_w && ch == pgl_cov_h && gw == pgl_cov_gw && gh == pgl_cov_gh)
        return;                     /* same target: keep the tallies */
    if (cw != pgl_cov_w || ch != pgl_cov_h) {
        unsigned char *n = (unsigned char *)realloc(pgl_cov,
                                                    (size_t)cw * (size_t)ch);
        if (!n) return;
        pgl_cov = n;
        pgl_cov_w = cw;
        pgl_cov_h = ch;
    }
    pgl_cov_gw = gw;
    pgl_cov_gh = gh;
    memset(pgl_cov, 0, (size_t)cw * (size_t)ch);
}

static void pgl_cov_mark(int gw, int gh, int x0, int y0, int x1, int y1,
                         int scaled) {
    unsigned char kind = scaled ? 1u : 2u;
    int cx, cy;
    if (!pgl_cov || pgl_cov_w <= 0 || pgl_cov_h <= 0) return;
    if (gw != pgl_cov_gw || gh != pgl_cov_gh) return;   /* smaller target */
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > pgl_cov_w * PGL_COV_CELL - 1) x1 = pgl_cov_w * PGL_COV_CELL - 1;
    if (y1 > pgl_cov_h * PGL_COV_CELL - 1) y1 = pgl_cov_h * PGL_COV_CELL - 1;
    if (x1 < x0 || y1 < y0) return;
    /* LAST touch wins: the reported share is "of the pixels the final frame
       shows, how many were produced by a scaled draw". */
    for (cy = y0 / PGL_COV_CELL; cy <= y1 / PGL_COV_CELL; ++cy)
        for (cx = x0 / PGL_COV_CELL; cx <= x1 / PGL_COV_CELL; ++cx)
            pgl_cov[cy * pgl_cov_w + cx] = kind;
}

static void pgl_cov_report(void) {
    unsigned long long s = 0, nv = 0, t = 0;
    int i, n;
    if (!pgl_cov) return;
    n = pgl_cov_w * pgl_cov_h;
    for (i = 0; i < n; ++i) {
        if (pgl_cov[i] == 1u) ++s;
        else if (pgl_cov[i] == 2u) ++nv;
        if (pgl_cov[i]) ++t;
    }
    fprintf(stderr, "GL backend: render scale coverage (target %dx%d guest, last "
                    "touch over the run): %llu/%llu 8x8 cells last drawn at the "
                    "scaled size (%llu.%01llu%%), %llu last drawn native "
                    "(%llu.%01llu%%), %llu untouched\n",
            pgl_cov_gw, pgl_cov_gh, s, (unsigned long long)n,
            n ? s * 1000ull / (unsigned long long)n / 10ull : 0ull,
            n ? s * 1000ull / (unsigned long long)n % 10ull : 0ull,
            nv,
            n ? nv * 1000ull / (unsigned long long)n / 10ull : 0ull,
            n ? nv * 1000ull / (unsigned long long)n % 10ull : 0ull,
            (unsigned long long)(n - t));
    fprintf(stderr, "GL backend: render scale coverage (draws): scaled %llu, "
                    "native %llu (%.2f%% of %llu scaled); box pixels scaled "
                    "%llu, native %llu; targets forced native %d (marks %llu)\n",
            pgl_cov_draws_scaled, pgl_cov_draws_native,
            (pgl_cov_draws_scaled + pgl_cov_draws_native)
                ? 100.0 * (double)pgl_cov_draws_scaled /
                  (double)(pgl_cov_draws_scaled + pgl_cov_draws_native) : 100.0,
            pgl_cov_draws_scaled + pgl_cov_draws_native,
            pgl_cov_px_scaled, pgl_cov_px_native, pgl_native_n, pgl_gate_marks);
    fprintf(stderr, "GL backend: render scale coverage (gating): guest texture "
                    "reads of a render target %llu, targets forced native %d, "
                    "reads served with the target kept scaled %llu (model=%s)\n",
            pgl_texread_events, pgl_native_n, pgl_sampled_kept,
            pgl_scale_sampled() ? "azahar-scaled" : "stage-A-demote");
    fprintf(stderr, "GL backend: render scale coverage (scaled samples): "
                    "unit-0 samples of the current target served from the "
                    "scaled backing %llu, fallbacks to the native staged image "
                    "%llu\n", pgl_snap_taken, pgl_snap_fallback);
    if (getenv("DQ8_RS_COVMAP")) {
        /* Which part of the target was last drawn at the scaled size:
           '#' scaled, '.' native, ' ' untouched.  One char per 8x8 cell. */
        int cy;
        fprintf(stderr, "GL backend: render scale coverage map (%dx%d cells, "
                        "'#'=scaled, '.'=native, ' '=untouched):\n",
                pgl_cov_w, pgl_cov_h);
        for (cy = 0; cy < pgl_cov_h; ++cy) {
            char line[PGL_COV_MAXW + 3];
            int cx, n2 = pgl_cov_w > PGL_COV_MAXW ? PGL_COV_MAXW : pgl_cov_w;
            for (cx = 0; cx < n2; ++cx)
                line[cx] = pgl_cov[cy * pgl_cov_w + cx] == 1u ? '#'
                         : pgl_cov[cy * pgl_cov_w + cx] == 2u ? '.' : ' ';
            line[n2] = 0;
            fprintf(stderr, "GLCOVMAP %s\n", line);
        }
    }
    {
        int i;
        for (i = 0; i < pgl_tgtstat_n; ++i)
            fprintf(stderr, "GL backend: render scale coverage (target %dx%d): "
                            "%llu draws, %llu of them at x%d, %llu native\n",
                    pgl_tgtstat[i].w, pgl_tgtstat[i].h,
                    pgl_tgtstat[i].draws,
                    (unsigned long long)(pgl_tgtstat[i].draws - pgl_tgtstat[i].native),
                    pgl_tgtstat[i].scale, pgl_tgtstat[i].native);
    }
}
static PGint pgl_u_rt, pgl_u_mode, pgl_u_tex, pgl_u_alpha_test,
             pgl_u_alpha_func, pgl_u_alpha_ref, pgl_u_tex_en, pgl_u_dst,
             pgl_u_tex1, pgl_u_tex1_en,
             pgl_u_dst_off, pgl_u_zdst, pgl_u_dst_used, pgl_u_zdst_used,
             pgl_u_dstimg, pgl_u_zimg, pgl_u_iscale;
static PGint pgl_u_src_c, pgl_u_mod_c, pgl_u_src_a, pgl_u_mod_a, pgl_u_cop,
             pgl_u_aop, pgl_u_scale_c, pgl_u_scale_a, pgl_u_const_c,
             pgl_u_buf_init, pgl_u_buf_upd_c, pgl_u_buf_upd_a;
/* t_1ae971f3: per-ELEMENT locations of u_const_c[].  A specialised shader
   declares the array [6] but references only the TEV const indices its own
   combiner uses, so the driver reports just those elements as active and
   glUniform4f(base + k) for an inactive element is GL_INVALID_OPERATION
   (observed on radeonsi: const_c=2, elements 1..5 invalid → one error per
   draw).  Looking the elements up individually is the only portable way to
   learn which ones may be written. */
static PGint pgl_u_const_c_e[6];
static PGint pgl_u_blend_en, pgl_u_eq_rgb, pgl_u_eq_a, pgl_u_f_src_rgb,
             pgl_u_f_dst_rgb, pgl_u_f_src_a, pgl_u_f_dst_a, pgl_u_blend_const;
static PGint pgl_u_depth_on, pgl_u_depth_hw, pgl_u_depth_test, pgl_u_depth_write,
             pgl_u_depth_func, pgl_u_depth_zbuf, pgl_u_depth_scale,
             pgl_u_depth_offset, pgl_u_depth_zmax;
static unsigned long long pgl_draws;
/* t_993e6fb6: colour-only clear accounting (see pgl_clear_color_only). */
static unsigned long long pgl_clear_pins, pgl_clear_would_clobber;
/* t_993e6fb6: depth draws whose PICA colour mask was not 0xF: the SW path
   still writes depth for them, so they used to lose their depth-mirror write
   under the global glColorMask. */
static unsigned long long pgl_draws_cmask_depth;
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

/* t_e216fc37: staged-image upload cache for PICA TEXTURE UNIT 1 (TEV source 4).
   One GL texture object per unit-1 staging slot (pica_vs.c's
   pica_gl_stage_texture1), keyed on the same (slot, generation) identity: a
   generation bump means the staged bytes changed and the image is re-uploaded
   exactly once.  Unit 1 is sampled from GL texture unit 3 (0/1/2 are the
   staged image, the blend source and the depth source). */
#define PGL_TEX1_SLOTS 4
typedef struct {
    PGuint tex;
    const unsigned char *ptr;
    int w, h;
    unsigned long long gen;   /* staging-slot generation of the uploaded image */
    int used;
} PglTex1Slot;
static PglTex1Slot pgl_tex1[PGL_TEX1_SLOTS];
static unsigned long long pgl_tex1_uploads;

/* PICA wrap code -> GL sampler wrap enum (codes 0/2/3; anything else falls
   back to CLAMP_TO_EDGE, which is what the unit-0 path does too). */
static PGenum pgl_wrap_enum(unsigned w) {
    if (w == 2u) return PGGL_REPEAT;
    if (w == 3u) return PGGL_MIRRORED_REPEAT;
    return PGGL_CLAMP_TO_EDGE;
}

/* t_c043d7f7: DQ8_TEX_FILTER (default on) gates texture filtering in BOTH
   backends, so the before/after A/B uses one binary.  The GL sampler takes
   the guest's mag/min bits.  Only level 0 is staged, so the min filter is
   never a mipmap enum: a mip_filter=linear request (param bit24; 6 of 1442
   draws in the reference scene) degrades to plain bilinear rather than
   leaving the texture incomplete (which would sample black). */
static int pgl_tex_filter_on = -1;
static int pgl_tex_filter_enabled(void) {
    if (pgl_tex_filter_on < 0) {
        const char *e = getenv("DQ8_TEX_FILTER");
        pgl_tex_filter_on = !(e && e[0] == '0');
    }
    return pgl_tex_filter_on;
}
static void pgl_tex_filter_apply(const PicaGlState *st, int unit1) {
    PGint mag, minf;
    if (!pgl_tex_filter_enabled()) {
        mag = PGGL_NEAREST;
        minf = PGGL_NEAREST;
    } else if (unit1) {
        mag = st->mag_filter1 ? PGGL_LINEAR : PGGL_NEAREST;
        minf = st->min_filter1 ? PGGL_LINEAR : PGGL_NEAREST;
    } else {
        mag = st->mag_filter ? PGGL_LINEAR : PGGL_NEAREST;
        minf = st->min_filter ? PGGL_LINEAR : PGGL_NEAREST;
    }
    pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_MAG_FILTER, mag);
    pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_MIN_FILTER, minf);
}

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
/* FREEZE WATCHDOG (t_4371e6ea): which host thread owns the active hold.  A
   freeze while the hold is set means our GL context was left current while
   the owner blocked inside a GL/EGL call; the dump prints this tid so the
   backtrace can be matched to it.  Plain long writes, read only for display. */
static long pgl_hold_owner_tid;
static int pica_gl_hold_state(const void **owner, unsigned long long *releases,
                              unsigned long long *mid) {
    if (owner) *owner = pgl_hold_active ? pgl_thread_id : NULL;
    if (releases) *releases = pgl_hold_releases;
    if (mid) *mid = pgl_hold_mid;
    return pgl_hold_active;
}
static long pica_gl_hold_tid(void) { return pgl_hold_owner_tid; }

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
static unsigned char *pgl_zshadow;      /* guest-thread owned under the split */
/* t_a22413f1 run3: DEPTH PATH SELECTION (the operator's Radeon rendered
   almost entirely black with the hardware depth attachment, so it must be
   opt-in and self-validating, never the default):
     pgl_depth_hw = 0  in-shader emulation against the emulated ATT1 mirror
                       (the pre-fix behaviour: playable, may show holes on
                       drivers that return pre-draw contents for a read of an
                       attachment the same draw writes)
     pgl_depth_hw = 1  real DEPTH_COMPONENT32F attachment + gl_FragDepth
   DQ8_GL_DEPTH_ATTACH=1 asks for the hardware path; it is only kept when the
   driver passes the startup self-test (small offscreen FBO with a known
   gl_FragDepth sequence) and the main FBO comes back COMPLETE. */
static int pgl_depth_hw_env = 0;   /* DQ8_GL_DEPTH_ATTACH as parsed */
static int pgl_depth_hw = 0;       /* ACTIVE mode (0 in-shader, 1 hardware) */
static int pgl_depth_hw_applied = -1; /* mode the shader/GL state reflects */
static int pgl_depth_selftest;     /* 0 not run, 1 pass, -1 fail */
static const char *pgl_depth_off_why = "default (DQ8_GL_DEPTH_ATTACH=0)";
static unsigned pgl_depth_fallback_count;
/* t_f8f2c1da: how many times the depth path changed at run time (0 for a
   normal run: the mode is decided once, in pgl_setup, before any program). */
static unsigned pgl_depth_mode_changes;
static int pgl_black_frames;       /* consecutive near-black full frames */
static int pgl_hw_bright_frames;    /* a lit full frame seen in HARDWARE mode */
static int pgl_depth_autofallback;  /* DQ8_GL_DEPTH_AUTOFALLBACK (default ON) */
/* ---- t_599a43f7: FAIL-SAFE FRAME GUARD ----------------------------------
   The operator's Radeon 780M (RDNA3) presented an almost entirely black 3D
   frame with the D32F depth attachment while our RX 580 (Polaris) renders it
   correctly, and the previous defence could be disarmed: it counted a frame
   "lit" when >0.5% of the sampled pixels were non-black, and the operator's
   frame has a small green patch in a corner — so the guard latched "the run is
   live", stopped watching, and the black screen stayed on screen for the whole
   run.  Two changes make a lasting black screen impossible:
     1. LIT means a PLAUSIBLE SCENE (default >5% of the sampled pixels), not
        "one patch"; DQ8_GL_FRAME_GUARD_LIT adjusts the threshold.
     2. A dark frame is only accepted as the game's own fade when the DEPTH
        SURFACE shows structure, i.e. some fragment actually wrote depth during
        that frame.  A flat depth surface (all-far after a far clear, or all-
        near if the clear left 0.0 and every fragment failed GL_LESS) combined
        with a dark colour attachment and depth-tested draws is the hardware
        depth path not working on this driver -> fall back to the in-shader
        path instead of presenting black.  That discriminator does not depend
        on brightness thresholds or on guessing driver behaviour. */
static unsigned pgl_frame_depth_draws; /* depth-tested draws since last flush */
static unsigned pgl_frame_no;          /* full-frame flushes evaluated */
static unsigned pgl_dark_frames;       /* dark frames with a FLAT depth surface */
static unsigned pgl_dark_draws;        /* depth-tested draws over those frames */
static int      pgl_frame_guard_lit = -1; /* parsed threshold, percent */
static unsigned pgl_frame_guard_probes;   /* depth readbacks spent */
static int      pgl_frame_guard_last;     /* last verdict (diag) */
static unsigned pgl_lit_best_pmille;      /* brightest sampled frame, 0..1000 */
static int      pgl_guard_sample_flat = -1; /* 1 flat far, -1 flat near, 0 struct */
static int      pgl_frame_guard_off = -1;   /* DQ8_GL_FRAME_GUARD=0 (A/B only) */
/* DQ8_GL_DEPTH_INJECT=near  clear the D32F attachment to 0.0 after every
   depth clear, i.e. exactly the state in which every fragment fails GL_LESS
   and the 3D scene comes out black while the UI/green patch survives.  It is
   a fault injector for the frame guard (it makes the operator's symptom
   reproducible on any GPU, including llvmpipe) and nothing else. */
static int pgl_zinject = -1;
static int pgl_zinject_near(void) {
    if (pgl_zinject < 0) {
        const char *e = getenv("DQ8_GL_DEPTH_INJECT");
        pgl_zinject = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return pgl_zinject;
}
static unsigned pgl_errs_logged;   /* always-on GL error lines emitted */
static unsigned pgl_depth_fbo_builds;
/* t_8510204b: candidate fixes for the battle-scene corruption.  Both are
   DEFAULT OFF until a deterministic A/B can prove them (see the status report
   tools/../t_8510204b_STATUS.md): DQ8_GL_ZFIX=1 enables
     (a) clearing a new depth attachment to PICA's 24-bit FAR (0xFFFFFF/2^24)
         instead of GL 1.0 — 1.0 leaves one ulp of headroom so a far plane
         polygon that PICA's integer compare rejects can pass GL_LESS;
     (b) invalidating the depth-shadow fast path whenever a write-back skipped
         pixels (the shadow then no longer describes the attachment). */
static int pgl_zfix = -1;
static int pgl_zfix_on(void) {
    if (pgl_zfix < 0) {
        const char *e = getenv("DQ8_GL_ZFIX");
        pgl_zfix = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return pgl_zfix;
}
/* t_d8e5ab8c (RX 580): the D32F attachment is cleared with
   glClearBufferfv(GL_DEPTH, ...), which the spec makes conditional on the
   DEPTH WRITE MASK — unlike glClear(GL_DEPTH_BUFFER_BIT) on the default
   framebuffer, and unlike the colour attachment's mask which the code already
   forces to 1.  The previous draw of a frame routinely leaves
   glDepthMask(0) behind (PICA depth-write-off draws and the in-shader mode's
   unconditional glDepthMask(0)), so the frame's depth clear silently cleared
   NOTHING: the attachment kept the previous frame's (or the allocator's)
   contents, every fragment failed the depth test and the frame came out
   black — the exact radeonsi symptom.  Force the mask on around the clear;
   DQ8_GL_DEPTH_CLEARMASK=0 restores the old behaviour for A/B. */
static int pgl_depth_clearmask_fix = 1;   /* DQ8_GL_DEPTH_CLEARMASK=0 -> off */
/* t_8510204b causality knobs (diagnosis only, default = shipped behaviour):
   DQ8_GL_ZWB=0      skip the depth write-back from ATT1 into guest memory
                     (isolates "guest-visible depth memory content" as a cause)
   DQ8_GL_HW_ATT1ALL=1  in HARDWARE mode write ATT1 for every depth draw even
                     when PICA's depth write is off (isolates the ATT1 masking) */
static int pgl_zwb_on = -1;   /* t_8df70ad4: -1 unresolved, 1 = write depth
                                 back, 0 = DQ8_GL_ZWB=0 skips it */
static int pgl_zwb(void) {
    if (pgl_zwb_on < 0) {
        const char *e = getenv("DQ8_GL_ZWB");
        pgl_zwb_on = !(e && e[0] == '0');   /* only "0" disables */
    }
    return pgl_zwb_on;
}
static int pgl_hw_att1all = 0;
static unsigned pgl_depth_probe_left;     /* DQ8_GL_DEPTH_PROBE=<n flushes> */
static unsigned pgl_depth_probe_n;
static int pgl_diag_gate = -1;            /* DQ8_GL_DIAG (t_275eb521: cached) */
static unsigned pgl_depth_syncs;   /* times the D32F attachment was cleared/uploaded */
static unsigned pgl_zshadow_len;
static unsigned pgl_zshadow_w, pgl_zshadow_h, pgl_zshadow_bpp;
static int pgl_zforce_full;
static int pgl_zdirty; /* depth draws since the last write-back */
/* t_7d18d1b6: a DEPTH-WRITING draw happened since the last write-back.  When
   only depth TESTING ran, the depth attachment still equals what guest memory
   already holds (the refresh uploaded it), so the depth resolve can be skipped
   entirely — it would write the identical bytes.  pgl_zwrote is set by
   depth-writing draws (both the synchronous and the split draw path) and by
   every depth attachment clear, which are the only operations that can move a
   depth pixel away from guest memory. */
static int pgl_zwrote;
/* DQ8_WB_ZSKIP=0 restores "resolve depth at every flush that ran a depth
   test" (the pre-t_7d18d1b6 behaviour), for A/B of the skip rule. */
static int pgl_zskip_env = -1;
static int pgl_zskip_on(void) {
    if (pgl_zskip_env < 0) {
        const char *e = getenv("DQ8_WB_ZSKIP");
        pgl_zskip_env = (e && e[0] == '0') ? 0 : 1;
    }
    return pgl_zskip_env;
}
static unsigned long long pgl_zrefills, pgl_zflushes;
static int pgl_drawn_x0, pgl_drawn_y0, pgl_drawn_x1, pgl_drawn_y1; /* GL y */
static int pgl_rect_trace; /* DQ8_GL_RECT_TRACE=1 */

/* t_599a43f7: forward decls for the fail-safe (the helper below is the only
   depth clear in the backend and verifies itself). */
static void pgl_depth_fallback(const char *why);
static unsigned pgl_drain_errors(const char *where, PGenum *first);
static unsigned pgl_dclear_checks, pgl_dclear_bad;
static int pgl_selftest_active;   /* inside a depth self-test: no injection, no
                                     verification (the test sets its own state) */

/* t_599a43f7: THE one and only way this backend clears the D32F depth
   attachment.  glClearBufferfv on a depth attachment is gated by the depth
   WRITE MASK (t_d8e5ab8c); a draw with PICA depth-write-off leaves
   glDepthMask(0) behind, and then the frame's depth clear silently clears
   NOTHING — the attachment keeps stale contents, every fragment fails GL_LESS
   and the 3D frame comes out black.  The synchronous path was fixed for that;
   the DQ8_SPLIT_GL worker's depth clear (pgl_split_apply_refresh) was not, which
   IS the operator's black screen: DQ8_SPLIT_GL=1 + DQ8_GL_DEPTH_ATTACH=1 gave a
   98.9% dark frame while DQ8_SPLIT_GL=0 rendered the same scene correctly.
   Every clear now goes through this helper so the two threads cannot drift.
   DQ8_GL_DEPTH_CLEARMASK=0 restores the pre-fix behaviour for A/B.
   The helper also VERIFIES the clear on the real context (one-pixel depth
   read-back at the screen centre: the first 8 clears of a run, then every
   120th).  A stale value there means the clear did not take effect, which is
   the single failure that made the operator's frame black — so it falls back
   to the in-shader path immediately instead of presenting black. */
static void pgl_depth_attachment_clear(float v) {
    PGfloat d[4];
    if (!pgl_depth_hw) return;
    if (pgl_depth_clearmask_fix) {
        pgl_glDepthMask(1);
        pgl_depth_mask_applied = -1;   /* next draw re-applies PICA's mask */
    }
    if (!pgl_selftest_active && pgl_zinject_near()) v = 0.0f;  /* DQ8_GL_DEPTH_INJECT=near */
    d[0] = d[1] = d[2] = d[3] = (PGfloat)v;
    pgl_glClearBufferfv(PGGL_DEPTH, 0, d);
    pgl_zwrote = 1;   /* t_7d18d1b6: the attachment no longer matches guest RAM */
    /* ---- verify (cheap, rare) ---- */
    if (pgl_selftest_active == 0 && pgl_depth_clearmask_fix && pgl_rt_w >= 4 && pgl_rt_h >= 4 &&
        (pgl_dclear_checks < 8u || (pgl_dclear_checks % 120u) == 0u) &&
        pgl_glCheckFramebufferStatus(PGGL_FRAMEBUFFER) == PGGL_FRAMEBUFFER_COMPLETE) {
        float got = -1.0f;
        ++pgl_dclear_checks;
        pgl_glReadPixels(pgl_tgt_w / 2, pgl_tgt_h / 2, 1, 1, PGGL_DEPTH_COMPONENT,
                         PGGL_FLOAT, &got);
        if (pgl_drain_errors("dc-clear-check", NULL) == 0 &&
            (got < v - 1e-3f || got > v + 1e-3f)) {
            ++pgl_dclear_bad;
            fprintf(stderr, "GL backend: DEPTH CLEAR did not take effect at "
                            "(%d,%d): cleared to %.8f, reads %.8f (check #%u) — "
                            "the depth attachment is stale, every fragment will "
                            "fail the depth test\n",
                    pgl_tgt_w / 2, pgl_tgt_h / 2, (double)v, (double)got,
                    pgl_dclear_checks);
            pgl_depth_fallback("the depth attachment clear did not take effect");
        }
    }
}
/* ---- t_8510204b: per-frame depth timeline (DQ8_GL_DEPTHLOG=1) ------------
   The battle scene rendered wrong with the hardware D32F depth path while the
   in-shader ATT1 mirror was clean.  This trace answers, for the frame that is
   on screen: which draws were issued, with which PICA depth state, against
   which depth surface, and every place the real depth attachment was cleared
   or re-uploaded mid-frame (a clear/upload between draws of one frame
   destroys the depth written by earlier draws, so later geometry passes the
   test and paints over the scene = the operator's wedges/bands).  Bounded
   ring; dumped at every flush so the last DFRAME block describes the last
   frame.  Diagnostic only, off by default. */
#define PGL_DLOG_MAX 400
typedef struct {
    unsigned long long draw;
    unsigned rt_w, rt_h;
    unsigned depth_addr, bpp;
    int test, write, func, zbuf, on;
    float scale, off, zmax;
    int box[4];
    int syncs_since;      /* depth attachment rebuilds/clears since prev draw */
} PglDLogEntry;
static PglDLogEntry pgl_dlog[PGL_DLOG_MAX];
static unsigned pgl_dlog_n;       /* entries since the last flush */
static int pgl_dlog_on = -1;      /* DQ8_GL_DEPTHLOG parsed on first use */
static unsigned pgl_dlog_syncs;   /* depth attachment clears/uploads this frame */
static unsigned long long pgl_dlog_draws; /* draws since the last flush */
/* ---- t_d559a510: per-draw A/B instrumentation for blend semantics -------
   DQ8_GL_DRAWHASH=1 reads the draw's box back from COLOR_ATTACHMENT0 before
   and after the draw and logs the full pipeline state plus an FNV-1a hash of
   both reads.  Two runs that differ ONLY in where the blend is evaluated (GL
   fixed-function blending vs the shader's software blend) then produce the
   same draw sequence: equal `pre` with different `post` on the first
   diverging draw means the blend evaluation itself differs, and the state on
   that line names the PICA blend configuration responsible.  Diagnostic only:
   forces two readbacks per draw (slow), off by default. */
static int pgl_drawhash_on = -1;
static long long pgl_dh_dump_idx = -2;   /* DQ8_GL_DRAWHASH_DUMP=<draw> */
static unsigned char *pgl_dh_buf, *pgl_dh_pre_saved;
static size_t pgl_dh_cap, pgl_dh_pre_len;
static unsigned long long pgl_dh_asum, pgl_dh_pre_asum;
static void pgl_dh_write_ppm(const char *suffix, const unsigned char *rgba,
                             unsigned w, unsigned h) {
    char path[256];
    const char *dir = getenv("DQ8_GL_DRAWHASH_DIR");
    const char *tag = getenv("DQ8_GL_DRAWHASH_TAG");
    FILE *f;
    unsigned i;
    if (!dir || !*dir) dir = "/tmp";
    if (!tag) tag = "arm";
    snprintf(path, sizeof path, "%s/dh_%s_%s.ppm", dir, tag, suffix);
    f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (i = 0; i < w * h; ++i) fwrite(rgba + i * 4u, 1, 3, f);
    fclose(f);
    fprintf(stderr, "GLHASHDUMP %s %ux%u\n", path, w, h);
}
static unsigned long long pgl_dh_hash(const unsigned char *p, size_t n) {
    unsigned long long h = 1469598103934665603ull;
    size_t i;
    for (i = 0; i < n; ++i) {
        h ^= (unsigned long long)p[i];
        h *= 1099511628211ull;
    }
    return h;
}
static int pgl_dh_box(const int box[4], unsigned long long *out,
                      unsigned long long *sum) {
    unsigned w = (unsigned)(box[2] - box[0] + 1), h = (unsigned)(box[3] - box[1] + 1);
    size_t need = (size_t)w * h * 4u;
    if (!pgl_dh_buf || pgl_dh_cap < need) {
        unsigned char *grown = (unsigned char *)realloc(pgl_dh_buf, need);
        if (!grown) { pgl_dh_cap = 0; return 0; }
        pgl_dh_buf = grown;
        pgl_dh_cap = need;
    }
    pgl_glBindFramebuffer(PGGL_FRAMEBUFFER, pgl_fbo);
    pgl_glReadBuffer(PGGL_COLOR_ATTACHMENT0);
    pgl_glReadPixels(box[0], box[1], (PGsizei)w, (PGsizei)h, PGGL_RGBA,
                     PGGL_UNSIGNED_BYTE, pgl_dh_buf);
    *out = pgl_dh_hash(pgl_dh_buf, need);
    if (sum) {
        unsigned long long s = 0;
        size_t i;
        for (i = 0; i < need; ++i) s += pgl_dh_buf[i];
        *sum = s;
    }
    {
        /* t_d559a510: the RGBA box hash covers alpha too; this splits it out so
           a divergence can be attributed to colour vs alpha (the PPM dumps only
           carry RGB). */
        unsigned long long a = 0;
        size_t i;
        for (i = 0; i < (size_t)w * h; ++i) a += pgl_dh_buf[i * 4u + 3u];
        pgl_dh_asum = a;
    }
    return 1;
}
static int pgl_zclear_on = 1; /* DQ8_GL_ZCLEAR=0: legacy no-op clear (reference) */
/* t_d559a510: depth-attachment readback for the per-draw A/B.  The emulated
   depth buffer is COLOR_ATTACHMENT1 (R32F, integer z values); the GL-blend
   path must leave it bit-identical to the shader-blend path. */
static unsigned char *pgl_dh_zbuf;
static size_t pgl_dh_zcap;
static int pgl_dh_zbox(const int box[4], unsigned long long *out,
                       unsigned long long *sum) {
    unsigned w = (unsigned)(box[2] - box[0] + 1);
    unsigned h = (unsigned)(box[3] - box[1] + 1);
    size_t need = (size_t)w * h * 4u;
    if (!pgl_dh_zbuf || pgl_dh_zcap < need) {
        unsigned char *grown = (unsigned char *)realloc(pgl_dh_zbuf, need);
        if (!grown) { pgl_dh_zcap = 0; return 0; }
        pgl_dh_zbuf = grown;
        pgl_dh_zcap = need;
    }
    pgl_glBindFramebuffer(PGGL_FRAMEBUFFER, pgl_fbo);
    pgl_glReadBuffer(PGGL_COLOR_ATTACHMENT1);
    pgl_glReadPixels(box[0], box[1], (PGsizei)w, (PGsizei)h, PGGL_RED,
                     PGGL_FLOAT, pgl_dh_zbuf);
    pgl_glReadBuffer(PGGL_COLOR_ATTACHMENT0);
    *out = pgl_dh_hash(pgl_dh_zbuf, need);
    if (sum) {
        const float *fp = (const float *)pgl_dh_zbuf;
        double s = 0.0;
        size_t i, n = (size_t)w * h;
        for (i = 0; i < n; ++i) s += (double)fp[i];
        *sum = (unsigned long long)(s + 0.5);
    }
    return 1;
}
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
/* t_2d934e23: split of pgl_ph_setup_ns (the per-draw CPU cost before the
   bounding box is even known): ens = drain_errors + object/FBO checks,
   ref = pgl_refresh/pgl_refresh_depth (guest-memory diff against the FBO
   shadow).  Also splits the deferred read-back: read = glReadPixels (+ error
   drain), wb = pgl_wb_color pixel write-back into guest RAM. */
static unsigned long long pgl_ph_ens_ns, pgl_ph_ref_ns, pgl_ph_ref_from;
/* t_2d934e23: refresh fast-path hit counts (per report window). */
static unsigned long long pgl_ph_ref_eq, pgl_ph_zref_eq, pgl_ph_ref_all;
static unsigned long long pgl_ph_ref_uniform, pgl_ph_ref_blocks, pgl_ph_box_px;
static unsigned long long pgl_fl_n, pgl_fl_read_ns, pgl_fl_wb_ns, pgl_fl_px;
/* t_8df70ad4: depth half of the same window — number of flushes that had a
   pending depth write-back (pgl_fl_zn), the ones skipped by DQ8_GL_ZWB=0
   (pgl_fl_zskip) and the ATT1 read-back / guest-store times. */
static unsigned long long pgl_fl_zn, pgl_fl_zskip, pgl_fl_zrb_ns, pgl_fl_zwb_ns;
/* t_7d18d1b6: colour vs depth split of the deferred write-back, plus the
   per-flush counters (pgl_fl_nc = flushes, pgl_fl_nz = flushes that needed a
   depth resolve). */
static unsigned long long pgl_fl_nz, pgl_fl_nc, pgl_fl_crb_ns, pgl_fl_cwb_ns;
/* t_7d18d1b6 diagnostics: who closed the deferred episode (DQ8_WB_TRACE=1). */
static int pgl_fl_reason;
static int pgl_wb_trace(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("DQ8_WB_TRACE");
        on = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return on;
}
static double pgl_fl_last;
/* t_2d934e23: flush wall-clock split (release / context take / read-back). */
static int pgl_tprof_on = -1;
static unsigned long long pgl_tp_n, pgl_tp_rel_ns, pgl_tp_take_ns, pgl_tp_lock_ns,
                          pgl_tp_pend0, pgl_tp_takefail;
/* t_2d934e23: DQ8_XFER_FAST=0 forces the reference (per-pixel) write-back and
   the reference display-transfer conversion loop — one knob, both A/B paths. */
static int pgl_wb_fast_on = -1;
static int pgl_wb_verify;
static unsigned long long pgl_wb_fast;

/* t_dd830a74 item 3: the ONE downsample of the whole pipeline is the download
   into guest RAM (pica_vs.c pica_gl_flush_guest_rt -> pica_gl_flush: GL
   submit + glReadPixels of the f x f block + box filter + guest write).  It is
   inside gl_ms, so these counters exist to report it on its own instead of
   assuming a share.  Accumulated by the flush, printed once at shutdown. */
static unsigned long long pgl_dl_ns, pgl_dl_calls;

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
/* t_6b81015d: framebuffer-fetch feedback-loop ordering (see pgl_dst_barrier). */
static int pgl_rt_wrote;          /* RT written since the last barrier */
/* t_2d934e23: boxes of the RT writes since the last barrier.  The fb-fetch
   surface is read SCREEN-ALIGNED (imageLoad(u_dstimg, ivec2(gl_FragCoord.xy))),
   so a draw can only observe prior writes that overlap its own box.  Each
   rect is a conservative superset of written pixels (the draw's clamped bbox)
   and the list holds every unbarriered draw's rect (capped; on overflow two
   rects are merged, which only ever makes the test more conservative), so
   skipping the barrier when no rect intersects cannot change a single output
   pixel. */
#define PGL_WRECT_MAX 8
static int pgl_wrect[PGL_WRECT_MAX][4];
static int pgl_nwrect;
static void pgl_wrect_add(int x0, int y0, int x1, int y1) {
    if (pgl_nwrect < PGL_WRECT_MAX) {
        int *r = pgl_wrect[pgl_nwrect++];
        r[0] = x0; r[1] = y0; r[2] = x1; r[3] = y1;
        return;
    }
    if (pgl_nwrect >= 2) {   /* merge the two oldest into one */
        int *a = pgl_wrect[0], *b = pgl_wrect[1];
        int i, *last;
        if (b[0] < a[0]) a[0] = b[0];
        if (b[1] < a[1]) a[1] = b[1];
        if (b[2] > a[2]) a[2] = b[2];
        if (b[3] > a[3]) a[3] = b[3];
        last = pgl_wrect[--pgl_nwrect];
        for (i = 0; i < 4; ++i) b[i] = last[i];
        pgl_wrect_add(x0, y0, x1, y1);
    } else {
        int *a = pgl_wrect[0];
        if (x0 < a[0]) a[0] = x0;
        if (y0 < a[1]) a[1] = y0;
        if (x1 > a[2]) a[2] = x1;
        if (y1 > a[3]) a[3] = y1;
    }
}
static int pgl_wrect_hit(int x0, int y0, int x1, int y1) {
    int i;
    for (i = 0; i < pgl_nwrect; ++i) {
        const int *r = pgl_wrect[i];
        if (!(x1 < r[0] || x0 > r[2] || y1 < r[1] || y0 > r[3])) return 1;
    }
    return 0;
}
static unsigned long long pgl_texbar_skip;
static unsigned long long pgl_texbarriers;
static int pgl_texbarrier_on = -1;
static int pgl_blend_gl_on;       /* DQ8_GL_BLENDS=0 disables (default on) */
static int pgl_perbuf_blend;      /* glBlendFunci/glBlendEquationi present */
static int pgl_blend_depth_guard = 1; /* DQ8_GL_BLENDDEPTH=0 skips the guard
                                         (A/B: reproduces the pre-fix bug) */
static unsigned long long pgl_blend_gl_depthfix; /* blend draws drawn with the
                                                    depth attachment replaced */
/* t_d559a510: the GL blend state (draw buffer 0 factors + the per-buffer
   REPLACE on the depth attachment) is PERSISTENT context state: it only has to
   be re-applied when the PICA blend configuration changes.  Applying it per
   draw costs two indexed blend calls per draw, and llvmpipe re-validates its
   rasteriser variant on every per-buffer blend change (~1.2 ms/draw measured at
   the operator's menu) — that is the whole saving of the fixed-function path.
   pgl_blend_key holds the last applied PICA configuration;
   pgl_blend_state_sets counts real applications (GLPHASE glb_sets). */
static int pgl_blend_applied = -1;   /* -1 = nothing applied yet */
static unsigned pgl_blend_key[6];
static PGfloat pgl_blend_key_const[4];
static int pgl_blend_guard_armed;    /* COLOR_ATTACHMENT1 = replace is set */
static unsigned long long pgl_blend_state_sets;
static int pgl_blend_enabled = -1;   /* last glEnable/glDisable(GL_BLEND) */
/* t_8c05fac8: framebuffer fetch.  1 = the fragment shader imageLoads the
   render-target textures directly (GL_ARB_shader_image_load_store, no
   per-draw FBO read => no llvmpipe queue drain); 0 = legacy per-draw
   glCopyTexSubImage2D path.  Decided once in pgl_setup: t_9651f10a DEFAULT
   ON for the known-good renderer allowlist (llvmpipe/softpipe/swrast +
   radeonsi), opt-in elsewhere, DQ8_GL_FBFETCH=0 forces the copies. */
static int pgl_fbfetch;
/* t_cb8723ae: guest-thread enqueue accounting for the GL worker split
   (GLPHASE enq_ms).  Atomic because the worker's pgl_ph_report reads/resets
   them while the guest thread bumps them (stats only; relaxed). */
static _Atomic unsigned long long pgl_enq_n, pgl_enq_ns, pgl_enq_full_waits;
static unsigned long long pgl_split_draw_fail; /* worker: draw_locked returned 0 */
static double pgl_ph_last;
/* t_5ddc492f: fragment-shader specialisation state reported by
   pgl_ph_report (the cache itself is defined next to pgl_build_program). */
static int pgl_nspec;
static unsigned long long pgl_spec_draws, pgl_spec_gen_draws, pgl_spec_fail;
static unsigned long long pgl_verify_n, pgl_verify_cmism, pgl_verify_zmism;
static void pgl_shape_report(void);
static void pgl_spec_report(void);

/* t_6f9e3fed: why the split path did or did not re-sync the FBO at enqueue
   (the counters pgl_refr_needed's gate is judged by).  need = capture ran the
   diff, skip = the guest had not touched the render target, ff = need because
   pgl_force_full was set, gd = need because a guest store hit the RT range. */
static unsigned long long pgl_scap_need, pgl_scap_skip, pgl_scap_ff, pgl_scap_gd;

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
            "enq_n=%llu enq_ms=%.1f fullwait_ms=%.1f split_fail=%llu "
            "texbar=%llu tbskip=%llu glb_depth=%llu glb_sets=%llu "
            "clear_pins=%llu clear_clobber=%llu cmask_depth=%llu "
            "ens_ms=%.1f ref_ms=%.1f refeq=%llu zrefeq=%llu refuni=%llu refblk=%llu "
            "scap=%llu/%llu scapff=%llu scapgd=%llu "
            "boxpx=%llu wbfast=%llu spec=%llu gen=%llu schfail=%llu nvar=%d "
            "vn=%llu vcmis=%llu vzmis=%llu\n", pgl_ph_n,
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
            pgl_split_draw_fail, pgl_texbarriers, pgl_texbar_skip,
            pgl_blend_gl_depthfix,
            pgl_blend_state_sets, pgl_clear_pins, pgl_clear_would_clobber,
            pgl_draws_cmask_depth,
            (double)pgl_ph_ens_ns / 1e6, (double)pgl_ph_ref_ns / 1e6,
            pgl_ph_ref_eq, pgl_ph_zref_eq, pgl_ph_ref_uniform, pgl_ph_ref_blocks,
            pgl_scap_need, pgl_scap_skip, pgl_scap_ff, pgl_scap_gd,
            pgl_ph_box_px / (pgl_ph_n ? pgl_ph_n : 1u), pgl_wb_fast,
            pgl_spec_draws, pgl_spec_gen_draws, pgl_spec_fail, pgl_nspec,
            pgl_verify_n, pgl_verify_cmism, pgl_verify_zmism);
    pgl_spec_draws = pgl_spec_gen_draws = pgl_spec_fail = 0;
    pgl_verify_n = pgl_verify_cmism = pgl_verify_zmism = 0;
    pgl_shape_report();
    pgl_spec_report();
    pgl_ph_n = pgl_ph_setup_ns = pgl_ph_copy_ns = pgl_ph_draw_ns = 0;
    pgl_ph_ens_ns = pgl_ph_ref_ns = 0;
    pgl_ph_ref_eq = pgl_ph_zref_eq = 0;
    pgl_ph_ref_uniform = pgl_ph_ref_blocks = pgl_ph_box_px = 0;
    pgl_wb_fast = 0;
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

/* t_8510204b: DEPTH-ONLY invalidation.  The depth mirror (ATT1) and the real
   D32F attachment are refreshed from guest depth memory only when the guest
   bytes differ from pgl_zshadow; pgl_zshadow is declared "in sync" after every
   write-back.  But pgl_wb_depth deliberately SKIPS pixels the guest rewrote
   after the draw ("later guest write wins"), and those pixels leave the
   attachment holding the rendered value while the shadow claims the guest
   value.  A later guest fill/clear of the depth region then compares equal to
   the shadow and skips the refresh, so the attachment keeps the PREVIOUS
   frame's depth: every fragment of the new frame fails the test there and the
   colour attachment shows the clear colour - the black diagonal bands and the
   sky wedge through the terrain holes seen in battle (field never triggers a
   mid-frame guest depth write, which is why the same build looked right
   there).  Raising the force-full flag when a write-back skips a pixel keeps
   the shadow honest: the next refresh re-uploads the region the guest owns,
   exactly as the guest-write case already did. */
static void pgl_request_zresync(void) {
    if (pgl_split_on) {
        atomic_store_explicit(&pgl_split_resync, 1, memory_order_release);
    } else {
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
        /* t_a22413f1 run3: a diagnostic binary cannot be gated on
           DQ8_GL_DEBUG (the operator's log must show the failures without a
           rebuild), so the first 40 errors are always logged. */
        if (!pgl_debug && pgl_errs_logged < 40u) {
            ++pgl_errs_logged;
            fprintf(stderr, "GL ERROR: %s: err=0x%04X %s (draw #%llu, "
                            "depth=%s)\n",
                    stage, (unsigned)e, pgl_err_name(e),
                    (unsigned long long)pgl_draws,
                    pgl_depth_hw ? "HARDWARE" : "IN-SHADER");
        }
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
    "uniform sampler2D u_tex1;\n"
    "uniform int u_tex1_en;\n"
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
    "uniform float u_iscale;\n"      /* t_ab8159de: 1/internal render scale */
    "uniform int u_alpha_test;\n"
    "uniform int u_alpha_func;\n"
    "uniform float u_alpha_ref;\n"
    /* Depth test/write (mirrors pica_fragment_depth/pica_depth_test_write). */
    "uniform int u_depth_on;\n"
    "uniform int u_depth_hw;\n"
    "/* t_a22413f1 run3: 1 = hardware depth attachment path. */\n"
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
    "vec2 sat(vec2 v) { return clamp(v, 0.0, 1.0); }\n"
    "vec3 sat(vec3 v) { return clamp(v, 0.0, 1.0); }\n"
    "vec4 sat(vec4 v) { return clamp(v, 0.0, 1.0); }\n"
    "vec4 sel_src(int s, vec4 prim, vec4 tex, vec4 tex1, vec4 prev, vec4 buf, vec4 cst) {\n"
    "    if (s == 0) return prim;\n"
    "    if (s == 3) return tex;\n"
    "    if (s == 4) return tex1;\n"
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
    "vec4 run_tev(vec4 prim, vec4 tex, vec4 tex1) {\n"
    "    vec4 previous = prim;\n"
    "    vec4 buffer = u_buf_init;\n"
    "    vec4 pending = buffer;\n"
    "    for (int s = 0; s < 6; ++s) {\n"
    "        vec4 cst = u_const_c[s];\n"
    "        vec3 col[3];\n"
    "        float alpha[3];\n"
    "        for (int i = 0; i < 3; ++i) {\n"
    "            vec4 v = sel_src(u_src_c[s*3+i], prim, tex, tex1, previous, buffer, cst);\n"
    "            col[i] = mod_color(u_mod_c[s*3+i], v);\n"
    "            v = sel_src(u_src_a[s*3+i], prim, tex, tex1, previous, buffer, cst);\n"
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
    "    vec2 pxs = gl_FragCoord.xy;\n"   /* supersampled target coords */
    "    vec2 px = pxs * u_iscale;\n"     /* guest-space subpixel coords */ /* (x+0.5, y+0.5), same as software */
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
    /* t_e216fc37: texture unit 1 (TEV source 4) uses texcoord1, same
       perspective-correct weights as texcoord0. */
    "    float tu1 = w0 * g0c[2] + w1 * g1c[2] + w2 * g2c[2];\n"
    "    float tv1 = w0 * g0c[3] + w1 * g1c[3] + w2 * g2c[3];\n"
    "    vec4 texel1 = (u_tex1_en != 0) ? texture(u_tex1, vec2(tu1, tv1)) : vec4(0.0);\n"
    "    vec4 source = (u_mode == 1) ? run_tev(prim, texel, texel1) : clamp(g0b, 0.0, 1.0);\n"
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
       failed test writes nothing at all).
       t_a22413f1: the TEST and the WRITE are done by the rasterizer against
       the real GL depth attachment (gl_FragDepth = zf/2^24, an exact power-of-
       two scaling, so the float ordering IS the integer z ordering and equal
       zf compare exactly equal).  The shader therefore no longer reads "old"
       depth from the emulated R32F mirror: a per-draw snapshot of that mirror
       (or a spec-undefined imageLoad of it) made two triangles of ONE draw
       test against the pre-draw depth instead of each other, so the last-
       rasterised triangle won even when farther — the "holes"/wrong-surface
       patches on a character model (one draw, hundreds of overlapping
       triangles).  attr frag_z still carries the raw integer z for the guest
       write-back path; it is colour-masked off when depth writing is off. */
    "    gl_FragDepth = gl_FragCoord.z;\n"
    "    if (u_depth_on != 0) {\n"
    "        float depth = zw * u_depth_scale + u_depth_offset;\n"
    "        if (u_depth_zbuf == 0 && iw > 0.0) depth /= iw;\n"
    "        depth = sat(depth);\n"
    "        float zf = floor(depth * u_depth_zmax);\n"
    "        frag_z = zf;\n"
    "        if (u_depth_hw != 0) {\n"
    "            gl_FragDepth = clamp(zf * (1.0 / 16777216.0), 0.0, 1.0);\n"
    "        } else {\n"
    "            float oldz;\n"
    "            if (u_zdst_used != 0) {\n"
    "#if DQ8_FBFETCH\n"
    "                oldz = imageLoad(u_zimg, ivec2(pxs)).r;\n"
    "#else\n"
    "                oldz = texelFetch(u_zdst, ivec2(pxs) - ivec2(u_dst_off), 0).r;\n"
    "#endif\n"
    "            } else { oldz = 0.0; }\n"
    "            if (u_depth_test != 0) {\n"
    "                bool zpass;\n"
    "                if (u_depth_func == 0) zpass = false;\n"
    "                else if (u_depth_func == 1) zpass = true;\n"
    "                else if (u_depth_func == 2) zpass = (zf == oldz);\n"
    "                else if (u_depth_func == 3) zpass = (zf != oldz);\n"
    "                else if (u_depth_func == 4) zpass = (zf < oldz);\n"
    "                else if (u_depth_func == 5) zpass = (zf <= oldz);\n"
    "                else if (u_depth_func == 6) zpass = (zf > oldz);\n"
    "                else zpass = (zf >= oldz);\n"
    "                if (!zpass) discard;\n"
    "            }\n"
    "            frag_z = (u_depth_write != 0) ? zf : oldz;\n"
    "        }\n"
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
    "        vec4 dest = (u_dst_used != 0) ? imageLoad(u_dstimg, ivec2(pxs)) : vec4(0.0);\n"
    "#else\n"
    "        vec4 dest = (u_dst_used != 0) ? texelFetch(u_dst, ivec2(pxs) - ivec2(u_dst_off), 0) : vec4(0.0);\n"
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
    pgl_glBlitFramebuffer = (PFN_glBlitFramebuffer)getproc("glBlitFramebuffer");
    if (!pgl_glBlitFramebuffer)
        fprintf(stderr, "GL backend: glBlitFramebuffer unavailable — a sampled "
                        "target falls back to the native staged image\n");
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
    /* t_6b81015d: ordering primitive for the live-image reads above. */
    pgl_glTextureBarrier = (PFN_glTextureBarrier)getproc("glTextureBarrier");
    if (!pgl_glTextureBarrier)
        pgl_glTextureBarrier =
            (PFN_glTextureBarrier)getproc("glTextureBarrierNV");
    pgl_glMemoryBarrier = (PFN_glMemoryBarrier)getproc("glMemoryBarrier");
    if (!pgl_glMemoryBarrier)
        pgl_glMemoryBarrier = (PFN_glMemoryBarrier)getproc("glMemoryBarrierEXT");
    /* t_d559a510: per-draw-buffer blend state (GL 4.0 core, or
       ARB_draw_buffers_blend as glBlendFuncSeparateiARB).  Soft-loaded: when
       absent the fixed-function blend path refuses depth draws, which keeps
       the emulated depth buffer (COLOR_ATTACHMENT1) out of the blend. */
    pgl_glBlendFunci = (PFN_glBlendFunci)getproc("glBlendFunci");
    if (!pgl_glBlendFunci)
        pgl_glBlendFunci = (PFN_glBlendFunci)getproc("glBlendFuncSeparatei");
    pgl_glBlendEquationi = (PFN_glBlendEquationi)getproc("glBlendEquationi");
    if (!pgl_glBlendEquationi)
        pgl_glBlendEquationi = (PFN_glBlendEquationi)getproc("glBlendEquationSeparatei");
    pgl_perbuf_blend = (pgl_glBlendFunci && pgl_glBlendEquationi) ? 1 : 0;
    /* t_993e6fb6: per-buffer colour mask.  Without it the PICA colour mask is
       applied to COLOR_ATTACHMENT1 too, so a colour-masked (depth-only) draw
       silently drops its depth-mirror write and every later depth test reads
       stale z. */
    pgl_glColorMaski = (PFN_glColorMaski)getproc("glColorMaski");
    if (!pgl_glColorMaski)
        pgl_glColorMaski = (PFN_glColorMaski)getproc("glColorMaskIndexedEXT");
    /* t_a22413f1: the hardware depth test/write state.  Core GL 1.0; if a
       loader cannot resolve them the backend refuses depth draws (see
       pica_gl_depth_ok) instead of silently rendering without a depth test. */
    pgl_glDepthFunc = (PFN_glDepthFunc)getproc("glDepthFunc");
    pgl_glDepthMask = (PFN_glDepthMask)getproc("glDepthMask");
    pgl_depth_ok = (pgl_glDepthFunc && pgl_glDepthMask) ? 1 : 0;
    if (!pgl_depth_ok)
        fprintf(stderr, "GL backend: glDepthFunc/glDepthMask unavailable -> "
                        "depth draws stay on the software rasterizer\n");
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

/* ---- t_5ddc492f: fragment-shader specialisation -------------------------
   The generic fragment shader (pgl_fs_src) evaluates the WHOLE PICA pipeline
   from uniforms for every fragment: the 6-stage TEV combiner chain (18 source
   selectors, 18 modifiers and 12 combine ops as dynamic selector chains), the
   depth probe (an imageLoad of the R32F target) and the shader blend (another
   imageLoad of the RGBA8 target).  llvmpipe JITs that as straight-line code
   and charges it per covered pixel; measured on the savestate scene it is
   63-65 % of wall (~1.8 ms per draw of a ~34 k-px box, GLPHASE sub_ms).

   A scene only uses a handful of state shapes, so the backend now GENERATES a
   fragment shader per shape - TEV stages unrolled with source selectors,
   modifiers, operators and shifts baked as literals; no destination/depth
   image read when the draw never consumes it; alpha test, depth compare and
   blend factors baked - and caches the linked programs.  Entry 0 is always
   the generic program and every failure path (compile error, cache full,
   DQ8_GL_SPEC=0) falls back to it, so correctness cannot regress.  The
   generated code performs the SAME float operations in the SAME order as
   pgl_fs_src / pica_tev() (src/pica_vs.c).

   Knobs:
     DQ8_GL_SPEC=0         disable (generic program only) - A/B reference
     DQ8_GL_SPEC_MAX=n     cache size, includes the generic entry (40)
     DQ8_GL_SPEC_DEBUG=1   dump every generated source to stderr
     DQ8_GL_SPEC_VERIFY=n  in-process A/B: re-render up to n draws that do not
                           read the target with the generic program and compare
                           the colour/depth box hashes (pgl_spec_verify)
     DQ8_GL_SHAPEHIST=1    report the per-feature draw histogram (GLPHASE)
*/
typedef struct { char *p; size_t n, cap; } PglSb;

static void pgl_sb_add(PglSb *b, const char *s) {
    size_t l = strlen(s);
    if (b->n + l + 1u > b->cap) {
        size_t nc = b->cap ? b->cap : 8192u;
        char *np;
        while (nc < b->n + l + 1u) nc *= 2u;
        np = (char *)realloc(b->p, nc);
        if (!np) { free(b->p); b->p = NULL; b->n = b->cap = 0; return; }
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->n, s, l + 1u);
    b->n += l;
}
static void pgl_sb_f(PglSb *b, const char *fmt, ...) {
    char tmp[1024];
    va_list ap;
    int k;
    va_start(ap, fmt);
    k = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (k > 0) pgl_sb_add(b, tmp);
}
/* Print a float as a GLSL literal that round-trips to the same float (9
   significant digits is exactly that guarantee) with an explicit fraction, so
   1 prints as "1.0" and never as an int literal. */
static void pgl_ffl(float v, char *out, size_t n) {
    snprintf(out, n, "%.9g", (double)v);
    if (!strpbrk(out, ".eE")) {
        size_t l = strlen(out);
        if (l + 3u < n) { out[l] = '.'; out[l + 1u] = '0'; out[l + 2u] = 0; }
    }
}
/* Operand source, as pica_tev_source() selects it. */
static void pgl_src_str(char *dst, size_t n, int src, int s) {
    switch (src) {
    case 0:  snprintf(dst, n, "prim"); break;
    case 3:  snprintf(dst, n, "texel"); break;
    case 4:  snprintf(dst, n, "texel1"); break;
    case 13: snprintf(dst, n, "buffer"); break;
    case 14: snprintf(dst, n, "u_const_c[%d]", s); break;
    case 15: snprintf(dst, n, "previous"); break;
    default: snprintf(dst, n, "vec4(0.0)"); break;
    }
}
/* Colour modifier (mod_color): (m & 1) negates, the rest picks the channel. */
static void pgl_emit_cmod(PglSb *b, int m, int src, int s) {
    char ex[128], base[192];
    pgl_src_str(ex, sizeof ex, src, s);
    if (m < 2)       snprintf(base, sizeof base, "(%s).rgb", ex);
    else if (m < 4)  snprintf(base, sizeof base, "vec3((%s).a)", ex);
    else if (m < 6)  snprintf(base, sizeof base, "vec3((%s).r)", ex);
    else if (m < 10) snprintf(base, sizeof base, "vec3((%s).g)", ex);
    else             snprintf(base, sizeof base, "vec3((%s).b)", ex);
    if (m & 1) pgl_sb_f(b, "(vec3(1.0) - %s)", base);
    else       pgl_sb_add(b, base);
}
/* Alpha modifier (mod_alpha). */
static void pgl_emit_amod(PglSb *b, int m, int src, int s) {
    char ex[128], base[192];
    const char *ch = (m < 2) ? "a" : (m < 4) ? "r" : (m < 6) ? "g" : "b";
    pgl_src_str(ex, sizeof ex, src, s);
    snprintf(base, sizeof base, "(%s).%s", ex, ch);
    if (m & 1) pgl_sb_f(b, "(1.0 - %s)", base);
    else       pgl_sb_add(b, base);
}
/* tev_op as an inline expression over three scalar expressions. */
static void pgl_emit_op(PglSb *b, int op, const char *a, const char *bb,
                        const char *c) {
    switch (op) {
    case 0: pgl_sb_add(b, a); break;
    case 1: pgl_sb_f(b, "(%s * %s)", a, bb); break;
    case 2: pgl_sb_f(b, "(%s + %s)", a, bb); break;
    case 3: pgl_sb_f(b, "(%s + %s - 0.5)", a, bb); break;
    case 4: pgl_sb_f(b, "(%s * %s + %s * (1.0 - %s))", a, c, bb, c); break;
    case 5: pgl_sb_f(b, "(%s - %s)", a, bb); break;
    case 8: pgl_sb_f(b, "(%s * %s + %s)", a, bb, c); break;
    case 9: pgl_sb_f(b, "(min(%s + %s, 1.0) * %s)", a, bb, c); break;
    default: pgl_sb_add(b, "0.0"); break;
    }
}
/* blend_factor as an inline scalar expression for channel k (0..3). */
static void pgl_emit_bf(PglSb *b, int f, int k) {
    switch (f) {
    case 0: pgl_sb_add(b, "0.0"); break;
    case 1: pgl_sb_add(b, "1.0"); break;
    case 2: pgl_sb_f(b, "source[%d]", k); break;
    case 3: pgl_sb_f(b, "(1.0 - source[%d])", k); break;
    case 4: pgl_sb_f(b, "dest[%d]", k); break;
    case 5: pgl_sb_f(b, "(1.0 - dest[%d])", k); break;
    case 6: pgl_sb_add(b, "source[3]"); break;
    case 7: pgl_sb_add(b, "(1.0 - source[3])"); break;
    case 8: pgl_sb_add(b, "dest[3]"); break;
    case 9: pgl_sb_add(b, "(1.0 - dest[3])"); break;
    case 10: pgl_sb_f(b, "u_blend_const[%d]", k); break;
    case 11: pgl_sb_f(b, "(1.0 - u_blend_const[%d])", k); break;
    case 12: pgl_sb_add(b, "u_blend_const[3]"); break;
    case 13: pgl_sb_add(b, "(1.0 - u_blend_const[3])"); break;
    case 14:
        if (k == 3) pgl_sb_add(b, "1.0");
        else        pgl_sb_add(b, "min(source[3], 1.0 - dest[3])");
        break;
    default: pgl_sb_add(b, "0.0"); break;
    }
}

/* EVERY state field hashed here must be baked by pgl_spec_src(); everything
   else stays a uniform and is passed bit-exactly. */
static unsigned long long pgl_key_u(unsigned long long h, unsigned v) {
    h ^= (unsigned long long)v;
    return h * 1099511628211ull;
}
static unsigned long long pgl_key_f(unsigned long long h, float f) {
    unsigned u;
    memcpy(&u, &f, sizeof u);
    return pgl_key_u(h, u);
}
static unsigned long long pgl_spec_key(const PicaGlState *st, int gl_blend,
                                       int dst_used, int zdst_used) {
    unsigned long long h = 1469598103934665603ull;
    int j, s;
    h = pgl_key_u(h, (unsigned)st->mode);
    h = pgl_key_u(h, (st->mode == PICA_GL_TEV && st->tev.tex_en) ? 1u : 0u);
    if (st->mode == PICA_GL_TEV) {
        for (j = 0; j < 18; ++j) {
            h = pgl_key_u(h, st->tev.src_c[j]);
            h = pgl_key_u(h, st->tev.mod_c[j]);
            h = pgl_key_u(h, st->tev.src_a[j]);
            h = pgl_key_u(h, st->tev.mod_a[j]);
        }
        for (s = 0; s < 6; ++s) {
            h = pgl_key_u(h, st->tev.cop[s] & 15u);
            h = pgl_key_u(h, st->tev.aop[s] & 15u);
            h = pgl_key_u(h, st->tev.buf_upd_c[s] ? 1u : 0u);
            h = pgl_key_u(h, st->tev.buf_upd_a[s] ? 1u : 0u);
            h = pgl_key_f(h, st->tev.scale_c[s]);
            h = pgl_key_f(h, st->tev.scale_a[s]);
        }
    }
    h = pgl_key_u(h, st->alpha_test ? (1u + (st->alpha_func & 7u)) : 0u);
    if (st->depth_on) {
        h = pgl_key_u(h, 1u + (st->depth_test ? 8u : 0u) +
                         (st->depth_write ? 16u : 0u) +
                         (st->depth_zbuf ? 32u : 0u) +
                         (st->depth_func & 7u));
    } else {
        h = pgl_key_u(h, 0u);
    }
    h = pgl_key_u(h, (!gl_blend && st->blend) ? 1u : 0u);
    if (!gl_blend && st->blend) {
        h = pgl_key_u(h, st->blend_src_rgb & 15u);
        h = pgl_key_u(h, st->blend_dst_rgb & 15u);
        h = pgl_key_u(h, st->blend_src_a & 15u);
        h = pgl_key_u(h, st->blend_dst_a & 15u);
        h = pgl_key_u(h, st->blend_eq_rgb & 7u);
        h = pgl_key_u(h, st->blend_eq_a & 7u);
    }
    h = pgl_key_u(h, dst_used ? 1u : 0u);
    h = pgl_key_u(h, zdst_used ? 1u : 0u);
    h = pgl_key_u(h, pgl_fbfetch ? 1u : 0u);
    /* t_d8e5ab8c x t_5ddc492f: the depth path is baked into the generated
       source, so the two variants must never share a cache entry. */
    h = pgl_key_u(h, pgl_depth_hw ? 1u : 0u);
    /* t_ab8159de: u_iscale is a uniform (1/scale), so the generated source does
       NOT depend on the scale and one program serves every target. */
    return h ? h : 1ull;   /* 0 is reserved for the generic program */
}

/* Build the specialised fragment shader source for a matching state. */
static char *pgl_spec_src(const PicaGlState *st, int gl_blend, int dst_used,
                          int zdst_used, size_t *out_len) {
    PglSb sb;
    const PicaTev *t = &st->tev;
    int tev = (st->mode == PICA_GL_TEV);
    int tex_en = tev && t->tex_en;
    int blend_en = (!gl_blend && st->blend) ? 1 : 0;
    int need_texel = 0, need_buffer = 0, need_const = 0, need_bconst = 0;
    int need_tex1 = 0;
    int depth_on = st->depth_on;
    int use_dst = blend_en && dst_used;
    int use_zdst = depth_on && zdst_used;
    int s, i, k;
    char num[64], num2[64];

    memset(&sb, 0, sizeof sb);
    if (tev) {
        for (i = 0; i < 18; ++i) {
            unsigned sc = t->src_c[i] & 15u, sa = t->src_a[i] & 15u;
            if (sc == 3u || sa == 3u) need_texel = 1;
            if (sc == 4u || sa == 4u) need_tex1 = 1;
            if (sc == 13u || sa == 13u) need_buffer = 1;
            if (sc == 14u || sa == 14u) need_const = 1;
        }
    }
    if (blend_en) {
        const unsigned bf[4] = { st->blend_src_rgb & 15u, st->blend_dst_rgb & 15u,
                                 st->blend_src_a & 15u, st->blend_dst_a & 15u };
        for (i = 0; i < 4; ++i)
            if (bf[i] >= 10u && bf[i] <= 13u) need_bconst = 1;
    }
    (void)num;

    pgl_sb_add(&sb, "#version 330 core\n");
    if (pgl_fbfetch && (use_dst || use_zdst))
        pgl_sb_add(&sb, "#extension GL_ARB_shader_image_load_store : require\n");
    pgl_sb_add(&sb, "flat in vec4 g0a, g0b, g0c, g1a, g1b, g1c, g2a, g2b, g2c;\n");
    pgl_sb_add(&sb, "uniform float u_iscale;\n");
    if (tex_en) pgl_sb_add(&sb, "uniform sampler2D u_tex;\n");
    if (need_tex1)
        pgl_sb_add(&sb, "uniform sampler2D u_tex1;\nuniform int u_tex1_en;\n");
    if (pgl_fbfetch) {
        if (use_dst)
            pgl_sb_add(&sb, "layout(rgba8) uniform readonly image2D u_dstimg;\n");
        if (use_zdst)
            pgl_sb_add(&sb, "layout(r32f) uniform readonly image2D u_zimg;\n");
    } else {
        if (use_dst) pgl_sb_add(&sb, "uniform sampler2D u_dst;\n");
        if (use_zdst) pgl_sb_add(&sb, "uniform sampler2D u_zdst;\n");
        if (use_dst || use_zdst) pgl_sb_add(&sb, "uniform vec2 u_dst_off;\n");
    }
    if (need_const) pgl_sb_add(&sb, "uniform vec4 u_const_c[6];\n");
    if (need_buffer) pgl_sb_add(&sb, "uniform vec4 u_buf_init;\n");
    if (st->alpha_test && (st->alpha_func & 7u) != 1u)
        pgl_sb_add(&sb, "uniform float u_alpha_ref;\n");
    if (depth_on)
        pgl_sb_add(&sb, "uniform float u_depth_scale, u_depth_offset, u_depth_zmax;\n");
    if (need_bconst) pgl_sb_add(&sb, "uniform vec4 u_blend_const;\n");
    pgl_sb_add(&sb, "out vec4 frag;\nout float frag_z;\n");
    pgl_sb_add(&sb, "float sat(float x) { return min(1.0, max(0.0, x)); }\n");
    /* TEV expressions can reduce a whole vector in one call; GLSL has no
       implicit component-wise overload, so provide the vector forms too. */
    pgl_sb_add(&sb, "vec2 sat(vec2 v) { return clamp(v, 0.0, 1.0); }\n"
                    "vec3 sat(vec3 v) { return clamp(v, 0.0, 1.0); }\n"
                    "vec4 sat(vec4 v) { return clamp(v, 0.0, 1.0); }\n");
    pgl_sb_add(&sb, "float edge_canon(vec2 u, vec2 v, vec2 p) {\n"
                    "    bool flip = (v.x < u.x) || (v.x == u.x && v.y < u.y);\n"
                    "    vec2 a = flip ? v : u;\n"
                    "    vec2 b = flip ? u : v;\n"
                    "    float e = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);\n"
                    "    return flip ? -e : e;\n"
                    "}\n");
    pgl_sb_add(&sb, "void main() {\n");
    pgl_sb_add(&sb, "    vec2 pxs = gl_FragCoord.xy;\n");
    pgl_sb_add(&sb, "    vec2 px = pxs * u_iscale;\n");
    /* identical interpolation to the generic shader */
    pgl_sb_add(&sb, "    float e0 = edge_canon(g1a.xy, g2a.xy, px);\n"
                    "    float e1 = edge_canon(g2a.xy, g0a.xy, px);\n"
                    "    float e2 = edge_canon(g0a.xy, g1a.xy, px);\n"
                    "    float w0 = e0 * g0a.z, w1 = e1 * g1a.z, w2 = e2 * g2a.z;\n"
                    "    float sum = w0 + w1 + w2;\n"
                    "    if (abs(sum) < 1e-20) discard;\n"
                    "    w0 /= sum; w1 /= sum; w2 /= sum;\n"
                    "    float lsum = e0 + e1 + e2;\n"
                    "    float zw = (lsum != 0.0) ? (e0 * g0a.w + e1 * g1a.w + e2 * g2a.w) / lsum : g0a.w;\n"
                    "    float iw = (lsum != 0.0) ? (e0 * g0a.z + e1 * g1a.z + e2 * g2a.z) / lsum : g0a.z;\n");
    if (tev) {
        pgl_sb_add(&sb, "    vec4 prim;\n");
        for (k = 0; k < 4; ++k)
            pgl_sb_f(&sb, "    prim[%d] = sat(w0 * g0b[%d] + w1 * g1b[%d] + w2 * g2b[%d]);\n",
                     k, k, k, k);
        if (tex_en) {
            pgl_sb_add(&sb, "    float tu = w0 * g0c[0] + w1 * g1c[0] + w2 * g2c[0];\n"
                            "    float tv = w0 * g0c[1] + w1 * g1c[1] + w2 * g2c[1];\n"
                            "    vec4 texel = texture(u_tex, vec2(tu, tv));\n");
        } else if (need_texel) {
            pgl_sb_add(&sb, "    vec4 texel = vec4(0.0);\n");
        }
        if (need_tex1)
            pgl_sb_add(&sb, "    float tu1 = w0 * g0c[2] + w1 * g1c[2] + w2 * g2c[2];\n"
                            "    float tv1 = w0 * g0c[3] + w1 * g1c[3] + w2 * g2c[3];\n"
                            "    vec4 texel1 = (u_tex1_en != 0) ? texture(u_tex1, vec2(tu1, tv1)) : vec4(0.0);\n");
        if (need_buffer)
            pgl_sb_add(&sb, "    vec4 buffer = u_buf_init;\n"
                            "    vec4 pending = buffer;\n");
        pgl_sb_add(&sb, "    vec4 previous = prim;\n");
        for (s = 0; s < 6; ++s) {
            int cop = (int)(t->cop[s] & 15u), aop = (int)(t->aop[s] & 15u);
            for (i = 0; i < 3; ++i) {
                pgl_sb_f(&sb, "    vec3 c%d_%d = ", s, i);
                pgl_emit_cmod(&sb, (int)(t->mod_c[s * 3 + i] & 15u),
                              (int)(t->src_c[s * 3 + i] & 15u), s);
                pgl_sb_add(&sb, ";\n");
                pgl_sb_f(&sb, "    float a%d_%d = ", s, i);
                pgl_emit_amod(&sb, (int)(t->mod_a[s * 3 + i] & 7u),
                              (int)(t->src_a[s * 3 + i] & 15u), s);
                pgl_sb_add(&sb, ";\n");
            }
            if (cop == 6 || cop == 7) {
                pgl_sb_f(&sb, "    float d%d = 0.0;\n", s);
                pgl_sb_f(&sb, "    d%d += (c%d_0.x - 0.5) * (c%d_1.x - 0.5) * 4.0;\n", s, s, s);
                pgl_sb_f(&sb, "    d%d += (c%d_0.y - 0.5) * (c%d_1.y - 0.5) * 4.0;\n", s, s, s);
                pgl_sb_f(&sb, "    d%d += (c%d_0.z - 0.5) * (c%d_1.z - 0.5) * 4.0;\n", s, s, s);
                pgl_sb_f(&sb, "    vec3 n%d = vec3(sat(d%d));\n", s, s);
                if (cop == 7) {
                    /* reference: next[3] = next[0] for cop 7 only */
                    pgl_sb_f(&sb, "    float na%d = n%d.x;\n", s, s);
                } else {
                    char aa[32], ab[32], ac[32];
                    snprintf(aa, sizeof aa, "a%d_0", s);
                    snprintf(ab, sizeof ab, "a%d_1", s);
                    snprintf(ac, sizeof ac, "a%d_2", s);
                    pgl_sb_f(&sb, "    float na%d = sat(", s);
                    pgl_emit_op(&sb, aop, aa, ab, ac);
                    pgl_sb_add(&sb, ");\n");
                }
            } else {
                pgl_sb_f(&sb, "    vec3 n%d = vec3(\n", s);
                for (k = 0; k < 3; ++k) {
                    const char *cch = (k == 0) ? "x" : (k == 1) ? "y" : "z";
                    char ea[32], eb[32], ec[32];
                    snprintf(ea, sizeof ea, "c%d_0.%s", s, cch);
                    snprintf(eb, sizeof eb, "c%d_1.%s", s, cch);
                    snprintf(ec, sizeof ec, "c%d_2.%s", s, cch);
                    pgl_sb_add(&sb, "        sat(");
                    pgl_emit_op(&sb, cop, ea, eb, ec);
                    pgl_sb_add(&sb, k < 2 ? "),\n" : "));\n");
                }
                {
                    char aa[32], ab[32], ac[32];
                    snprintf(aa, sizeof aa, "a%d_0", s);
                    snprintf(ab, sizeof ab, "a%d_1", s);
                    snprintf(ac, sizeof ac, "a%d_2", s);
                    pgl_sb_f(&sb, "    float na%d = sat(", s);
                    pgl_emit_op(&sb, aop, aa, ab, ac);
                    pgl_sb_add(&sb, ");\n");
                }
            }
            if (t->scale_c[s] != 1.0f) {
                pgl_ffl(t->scale_c[s], num, sizeof num);
                pgl_sb_f(&sb, "    n%d = sat(n%d * %s);\n", s, s, num);
            }
            if (t->scale_a[s] != 1.0f) {
                pgl_ffl(t->scale_a[s], num2, sizeof num2);
                pgl_sb_f(&sb, "    na%d = sat(na%d * %s);\n", s, s, num2);
            }
            if (need_buffer) pgl_sb_add(&sb, "    buffer = pending;\n");
            if (need_buffer && s < 4) {
                if (t->buf_upd_c[s]) pgl_sb_f(&sb, "    pending.rgb = n%d.rgb;\n", s);
                if (t->buf_upd_a[s]) pgl_sb_f(&sb, "    pending.a = na%d;\n", s);
            }
            pgl_sb_f(&sb, "    previous = vec4(n%d, na%d);\n", s, s);
        }
        pgl_sb_add(&sb, "    vec4 source = previous;\n");
    } else {
        pgl_sb_add(&sb, "    vec4 source = clamp(g0b, 0.0, 1.0);\n");
    }
    /* alpha test */
    if (st->alpha_test) {
        int f = (int)(st->alpha_func & 7u);
        if (f != 1) {
            pgl_sb_add(&sb, "    {\n        float a = floor(source[3] * 255.0 + 0.5);\n"
                            "        if (!(");
            switch (f) {
            case 2: pgl_sb_add(&sb, "a == u_alpha_ref"); break;
            case 3: pgl_sb_add(&sb, "a != u_alpha_ref"); break;
            case 4: pgl_sb_add(&sb, "a < u_alpha_ref"); break;
            case 5: pgl_sb_add(&sb, "a <= u_alpha_ref"); break;
            case 6: pgl_sb_add(&sb, "a > u_alpha_ref"); break;
            case 7: pgl_sb_add(&sb, "a >= u_alpha_ref"); break;
            default: pgl_sb_add(&sb, "false"); break;
            }
            pgl_sb_add(&sb, ")) discard;\n    }\n");
        }
    }
    /* depth */
    if (depth_on) {
        pgl_sb_add(&sb, "    float depth = zw * u_depth_scale + u_depth_offset;\n");
        if (!st->depth_zbuf)
            pgl_sb_add(&sb, "    if (iw > 0.0) depth /= iw;\n");
        pgl_sb_add(&sb, "    depth = sat(depth);\n"
                        "    float zf = floor(depth * u_depth_zmax);\n");
        if (pgl_depth_hw) {
            /* t_d8e5ab8c x t_5ddc492f: HARDWARE depth (the default).  The
               rasterizer tests and writes the real DEPTH_COMPONENT32F
               attachment, so the generated shader only has to hand it the
               integer z scaled by an exact power of two (float ordering ==
               integer ordering; equal zf compare exactly equal) and keep
               frag_z for the guest ATT1 mirror.  No oldz read, no per-fragment
               depth comparison — that in-shader comparison against a per-draw
               snapshot was exactly what produced the character "holes".
               DQ8_GL_DEPTH_ATTACH=0 selects the in-shader A/B below. */
            pgl_sb_add(&sb, "    gl_FragDepth = clamp(zf * (1.0 / 16777216.0), 0.0, 1.0);\n"
                            "    frag_z = zf;\n");
        } else {
        if (use_zdst) {
            if (pgl_fbfetch)
                pgl_sb_add(&sb, "    float oldz = imageLoad(u_zimg, ivec2(pxs)).r;\n");
            else
                pgl_sb_add(&sb, "    float oldz = texelFetch(u_zdst, ivec2(pxs) - ivec2(u_dst_off), 0).r;\n");
        } else {
            pgl_sb_add(&sb, "    float oldz = 0.0;\n");
        }
        if (st->depth_test) {
            int f = (int)(st->depth_func & 7u);
            if (f == 0) {
                pgl_sb_add(&sb, "    discard;\n");
            } else if (f != 1) {
                switch (f) {
                case 2: pgl_sb_add(&sb, "    if (!(zf == oldz)) discard;\n"); break;
                case 3: pgl_sb_add(&sb, "    if (!(zf != oldz)) discard;\n"); break;
                case 4: pgl_sb_add(&sb, "    if (!(zf < oldz)) discard;\n"); break;
                case 5: pgl_sb_add(&sb, "    if (!(zf <= oldz)) discard;\n"); break;
                case 6: pgl_sb_add(&sb, "    if (!(zf > oldz)) discard;\n"); break;
                default: pgl_sb_add(&sb, "    if (!(zf >= oldz)) discard;\n"); break;
                }
            }
        }
        pgl_sb_add(&sb, st->depth_write ? "    frag_z = zf;\n" : "    frag_z = oldz;\n");
        }
    } else {
        pgl_sb_add(&sb, "    frag_z = 0.0;\n");
    }
    /* blend */
    if (blend_en) {
        if (dst_used) {
            if (pgl_fbfetch)
                pgl_sb_add(&sb, "    vec4 dest = imageLoad(u_dstimg, ivec2(pxs));\n");
            else
                pgl_sb_add(&sb, "    vec4 dest = texelFetch(u_dst, ivec2(pxs) - ivec2(u_dst_off), 0);\n");
        } else {
            pgl_sb_add(&sb, "    vec4 dest = vec4(0.0);\n");
        }
        pgl_sb_add(&sb, "    vec4 value = source;\n");
        for (k = 0; k < 4; ++k) {
            int eq = (k == 3) ? (int)(st->blend_eq_a & 7u)
                              : (int)(st->blend_eq_rgb & 7u);
            int fs = (k == 3) ? (int)(st->blend_src_a & 15u)
                              : (int)(st->blend_src_rgb & 15u);
            int fd = (k == 3) ? (int)(st->blend_dst_a & 15u)
                              : (int)(st->blend_dst_rgb & 15u);
            pgl_sb_add(&sb, "    {\n        float fs = ");
            pgl_emit_bf(&sb, fs, k);
            pgl_sb_add(&sb, ";\n        float fd = ");
            pgl_emit_bf(&sb, fd, k);
            pgl_sb_f(&sb, ";\n        float sv = source[%d] * fs;\n"
                          "        float dv = dest[%d] * fd;\n", k, k);
            switch (eq) {
            case 0: pgl_sb_f(&sb, "        value[%d] = sv + dv;\n", k); break;
            case 1: pgl_sb_f(&sb, "        value[%d] = sv - dv;\n", k); break;
            case 2: pgl_sb_f(&sb, "        value[%d] = dv - sv;\n", k); break;
            case 3: pgl_sb_f(&sb, "        value[%d] = min(source[%d], dest[%d]);\n",
                             k, k, k); break;
            default: pgl_sb_f(&sb, "        value[%d] = max(source[%d], dest[%d]);\n",
                              k, k, k); break;
            }
            pgl_sb_add(&sb, "    }\n");
        }
    } else {
        pgl_sb_add(&sb, "    vec4 value = source;\n");
    }
    pgl_sb_add(&sb, "    frag = vec4(floor(sat(value[0]) * 255.0 + 0.5),\n"
                    "                floor(sat(value[1]) * 255.0 + 0.5),\n"
                    "                floor(sat(value[2]) * 255.0 + 0.5),\n"
                    "                floor(sat(value[3]) * 255.0 + 0.5)) / 255.0;\n"
                    "}\n");
    if (!sb.p) return NULL;
    if (out_len) *out_len = sb.n;
    return sb.p;
}

/* ---- program cache ------------------------------------------------------ */
typedef struct {
    PGint rt, mode, tex, alpha_test, alpha_func, alpha_ref, tex_en, dst,
          tex1, tex1_en,
          dst_off, zdst, dst_used, zdst_used, dstimg, zimg,
          src_c, mod_c, src_a, mod_a, cop, aop, scale_c, scale_a, const_c,
          const_c_e[6],
          buf_init, buf_upd_c, buf_upd_a,
          blend_en, eq_rgb, eq_a, f_src_rgb, f_dst_rgb, f_src_a, f_dst_a,
          blend_const,
          depth_on, depth_test, depth_write, depth_func, depth_zbuf,
          depth_scale, depth_offset, depth_zmax, depth_hw, iscale;
} PglULoc;

#define PGL_SPEC_MAX 40
typedef struct {
    unsigned long long key;
    PGuint prog;
    PglULoc u;
    unsigned long long draws, px, ns;
    /* t_f8f2c1da: the depth path BAKED into this program's generated source
       (pgl_spec_src branches on pgl_depth_hw).  -1 = entry 0, the generic
       program, which selects the path at run time through u_depth_hw and is
       valid in both modes.  Compared against the live mode on every use by the
       DQ8_GL_SPEC_MODECHECK tripwire so a stale variant can never be served
       silently again. */
    int depth_hw;
} PglProgSpec;

static PglProgSpec pgl_spec[PGL_SPEC_MAX];
/* pgl_nspec, pgl_spec_draws/gen_draws/fail and the verify counters are
   declared above pgl_ph_report, which reports them. */
static int pgl_spec_on = -1;          /* -1 = not decided, 0/1 = knob */
static int pgl_spec_max = PGL_SPEC_MAX;
static int pgl_spec_dbg = -1;
/* t_f8f2c1da: tripwire state.  modecheck_n counts every use of a program whose
   baked depth path disagrees with the live one (must stay 0).  The knob only
   controls logging: -1 unparsed, 1 = log each violation, 2 = also abort. */
static int pgl_spec_modecheck = -1;
static unsigned long long pgl_spec_modecheck_n;
static int pgl_depth_mode_ready;      /* set once the path is decided for good */
static void pgl_spec_modecheck_use(const PglProgSpec *e, const char *where);
static PGuint pgl_prog_cur;
static unsigned long long pgl_spec_compiles;
static int pgl_verify_on = -1, pgl_verify_limit;
/* program/side of the draw currently being submitted */
static PglProgSpec *pgl_cur_prog;
static int pgl_cur_dst_used, pgl_cur_zdst_used;

static void pgl_query_locs(PGuint prog, PglULoc *l) {
#define PGL_LOC(f, name) l->f = pgl_glGetUniformLocation(prog, name)
    PGL_LOC(rt, "u_rt");
    PGL_LOC(mode, "u_mode");
    PGL_LOC(tex, "u_tex");
    PGL_LOC(alpha_test, "u_alpha_test");
    PGL_LOC(alpha_func, "u_alpha_func");
    PGL_LOC(alpha_ref, "u_alpha_ref");
    PGL_LOC(tex_en, "u_tex_en");
    PGL_LOC(tex1, "u_tex1");
    PGL_LOC(tex1_en, "u_tex1_en");
    PGL_LOC(dst, "u_dst");
    PGL_LOC(dst_off, "u_dst_off");
    PGL_LOC(iscale, "u_iscale");
    PGL_LOC(dst_used, "u_dst_used");
    PGL_LOC(zdst_used, "u_zdst_used");
    PGL_LOC(src_c, "u_src_c");
    PGL_LOC(mod_c, "u_mod_c");
    PGL_LOC(src_a, "u_src_a");
    PGL_LOC(mod_a, "u_mod_a");
    PGL_LOC(cop, "u_cop");
    PGL_LOC(aop, "u_aop");
    PGL_LOC(scale_c, "u_scale_c");
    PGL_LOC(scale_a, "u_scale_a");
    PGL_LOC(const_c, "u_const_c");
    {
        int ci;
        for (ci = 0; ci < 6; ++ci) {
            char nm[32];
            snprintf(nm, sizeof nm, "u_const_c[%d]", ci);
            l->const_c_e[ci] = pgl_glGetUniformLocation(prog, nm);
        }
    }
    PGL_LOC(buf_init, "u_buf_init");
    PGL_LOC(buf_upd_c, "u_buf_upd_c");
    PGL_LOC(buf_upd_a, "u_buf_upd_a");
    PGL_LOC(blend_en, "u_blend_en");
    PGL_LOC(eq_rgb, "u_eq_rgb");
    PGL_LOC(eq_a, "u_eq_a");
    PGL_LOC(f_src_rgb, "u_f_src_rgb");
    PGL_LOC(f_dst_rgb, "u_f_dst_rgb");
    PGL_LOC(f_src_a, "u_f_src_a");
    PGL_LOC(f_dst_a, "u_f_dst_a");
    PGL_LOC(blend_const, "u_blend_const");
    PGL_LOC(zdst, "u_zdst");
    PGL_LOC(depth_on, "u_depth_on");
    PGL_LOC(depth_test, "u_depth_test");
    PGL_LOC(depth_write, "u_depth_write");
    PGL_LOC(depth_func, "u_depth_func");
    PGL_LOC(depth_zbuf, "u_depth_zbuf");
    PGL_LOC(depth_scale, "u_depth_scale");
    PGL_LOC(depth_offset, "u_depth_offset");
    PGL_LOC(depth_zmax, "u_depth_zmax");
    /* t_a22413f1 run3: MUST be looked up — a location of 0 is a real
       location, and glUniform1i(0, ...) then overwrites whatever uniform
       sits at 0 (it made the first hardware-depth build render a black
       frame).  Kept for the generic program (which still branches on it). */
    PGL_LOC(depth_hw, "u_depth_hw");
    PGL_LOC(dstimg, "u_dstimg");
    PGL_LOC(zimg, "u_zimg");
#undef PGL_LOC
    /* t_8c05fac8: image-unit assignment is program state, set once. */
    if (l->dstimg >= 0 || l->zimg >= 0) {
        pgl_glUseProgram(prog);
        if (l->dstimg >= 0) pgl_glUniform1i(l->dstimg, 0);
        if (l->zimg >= 0) pgl_glUniform1i(l->zimg, 1);
    }
}

/* Make entry e's program current and publish its uniform locations to the
   pgl_u_* globals the uniform upload uses. */
static void pgl_prog_use(PglProgSpec *e) {
    const PglULoc *l = &e->u;
    /* Uniform values live per program: when the draw switches to another
       specialised variant (or back to the generic one) the depth-mode
       uniform of THAT program is whatever it was last set to, or 0 for a
       newly linked program, so the "already applied" cache must be
       invalidated on every program change. */
    if (pgl_prog_cur != e->prog) pgl_depth_hw_applied = -1;
    pgl_prog_cur = e->prog;
    pgl_glUseProgram(e->prog);
    pgl_u_rt = l->rt; pgl_u_mode = l->mode; pgl_u_tex = l->tex;
    pgl_u_alpha_test = l->alpha_test; pgl_u_alpha_func = l->alpha_func;
    pgl_u_alpha_ref = l->alpha_ref; pgl_u_tex_en = l->tex_en;
    pgl_u_tex1 = l->tex1; pgl_u_tex1_en = l->tex1_en;
    pgl_u_dst = l->dst; pgl_u_dst_off = l->dst_off; pgl_u_zdst = l->zdst;
    pgl_u_iscale = l->iscale;
    pgl_u_dst_used = l->dst_used; pgl_u_zdst_used = l->zdst_used;
    pgl_u_dstimg = l->dstimg; pgl_u_zimg = l->zimg;
    pgl_u_src_c = l->src_c; pgl_u_mod_c = l->mod_c;
    pgl_u_src_a = l->src_a; pgl_u_mod_a = l->mod_a;
    pgl_u_cop = l->cop; pgl_u_aop = l->aop;
    pgl_u_scale_c = l->scale_c; pgl_u_scale_a = l->scale_a;
    pgl_u_const_c = l->const_c; pgl_u_buf_init = l->buf_init;
    memcpy(pgl_u_const_c_e, l->const_c_e, sizeof pgl_u_const_c_e);
    pgl_u_buf_upd_c = l->buf_upd_c; pgl_u_buf_upd_a = l->buf_upd_a;
    pgl_u_blend_en = l->blend_en; pgl_u_eq_rgb = l->eq_rgb;
    pgl_u_eq_a = l->eq_a; pgl_u_f_src_rgb = l->f_src_rgb;
    pgl_u_f_dst_rgb = l->f_dst_rgb; pgl_u_f_src_a = l->f_src_a;
    pgl_u_f_dst_a = l->f_dst_a; pgl_u_blend_const = l->blend_const;
    pgl_u_depth_on = l->depth_on; pgl_u_depth_test = l->depth_test;
    pgl_u_depth_write = l->depth_write; pgl_u_depth_func = l->depth_func;
    pgl_u_depth_zbuf = l->depth_zbuf; pgl_u_depth_scale = l->depth_scale;
    pgl_u_depth_offset = l->depth_offset; pgl_u_depth_zmax = l->depth_zmax;
    pgl_u_depth_hw = l->depth_hw;
}

/* Compile+link one generated fragment shader (vertex shader is shared). */
static PGuint pgl_spec_link(const char *fs_src, unsigned long long key) {
    PGuint vs, fs, prog;
    PGint ok = 0;
    vs = pgl_compile(PGGL_VERTEX_SHADER, pgl_vs_src);
    fs = pgl_compile(PGGL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) {
        if (vs) pgl_glDeleteShader(vs);
        if (fs) pgl_glDeleteShader(fs);
        return 0;
    }
    prog = pgl_glCreateProgram();
    pgl_glAttachShader(prog, vs);
    pgl_glAttachShader(prog, fs);
    pgl_glLinkProgram(prog);
    pgl_glGetProgramiv(prog, PGGL_LINK_STATUS, &ok);
    pgl_glDeleteShader(vs);
    pgl_glDeleteShader(fs);
    if (!ok) {
        char log[2048];
        PGsizei n = 0;
        memset(log, 0, sizeof log);
        pgl_glGetProgramInfoLog(prog, sizeof log, &n, log);
        fprintf(stderr, "GL backend: specialised program link failed "
                "(key=%016llx):\n%.*s\n", key, (int)n, log);
        return 0;
    }
    ++pgl_spec_compiles;
    return prog;
}

/* Shapes the generator must NOT specialise yet.  Only a TEV stage that READS
   the buffer (operand source 13) is refused: the reference pica_tev()
   implements the buffer stage (source 13 + the reg 0xE0 pending updates) and
   the generated code follows it, but the long-validated GENERIC shader
   disagrees with both on buffer states (tools/gl_spec_test.c case "tev buffer
   + mix": the generic result equals u_buf_init, i.e. it does not apply the
   pending update).  The reg 0xE0 update flags ALONE are harmless - with
   nothing reading the buffer the updates are dead code - and the savestate
   scene sets them, so they must not disable the specialisation.
   Nothing in the DQ8 scene reads the buffer. */
/* Return the program for this draw: the specialised entry, or the generic
   fallback (index 0) when specialisation is off / the cache is full / the
   shape is unsupported / the generated source does not compile. */
static PglProgSpec *pgl_spec_select(const PicaGlState *st, int gl_blend,
                                    int dst_used, int zdst_used) {
    unsigned long long key;
    char *src;
    size_t len = 0;
    PGuint prog;
    int i;
    if (pgl_spec_on < 0) {
        const char *e = getenv("DQ8_GL_SPEC");
        const char *m = getenv("DQ8_GL_SPEC_MAX");
        const char *d = getenv("DQ8_GL_SPEC_DEBUG");
        pgl_spec_on = (e && e[0] == '0') ? 0 : 1;
        if (m && m[0]) {
            int v = atoi(m);
            if (v >= 1 && v <= PGL_SPEC_MAX) pgl_spec_max = v;
        }
        pgl_spec_dbg = (d && d[0] && d[0] != '0') ? 1 : 0;
        fprintf(stderr, "GL backend: fragment-shader specialisation %s "
                "(cache %d, generic fallback always available)\n",
                pgl_spec_on ? "ON" : "OFF (DQ8_GL_SPEC=0)", pgl_spec_max);
    }
    if (!pgl_spec_on || pgl_nspec <= 0) {
        ++pgl_spec_gen_draws;
        return &pgl_spec[0];
    }
    key = pgl_spec_key(st, gl_blend, dst_used, zdst_used);
    for (i = 1; i < pgl_nspec; ++i)
        if (pgl_spec[i].key == key) {
            /* t_f8f2c1da: the key already carries the depth path, so this must
               never fire — that is exactly why it is checked. */
            pgl_spec_modecheck_use(&pgl_spec[i], "select/cache-hit");
            return &pgl_spec[i];
        }
    if (pgl_nspec >= pgl_spec_max) {
        ++pgl_spec_gen_draws;
        return &pgl_spec[0];
    }
    /* t_f8f2c1da: a specialised program bakes the depth path, so the path MUST
       already be decided (pgl_setup, before the first draw).  A generation
       before that point is the same class of bug as a stale hit. */
    if (!pgl_depth_mode_ready) {
        ++pgl_spec_modecheck_n;
        fprintf(stderr, "GL SPEC MODECHECK VIOLATION (generate): specialised "
                        "program generated before the depth path was decided "
                        "(draw=%llu)\n", (unsigned long long)pgl_draws);
    }
    src = pgl_spec_src(st, gl_blend, dst_used, zdst_used, &len);
    if (!src) {
        ++pgl_spec_gen_draws;
        ++pgl_spec_fail;
        return &pgl_spec[0];
    }
    if (pgl_spec_dbg)
        fprintf(stderr, "GL backend: SPEC src key=%016llx mode=%d tex=%d "
                "blend=%d depth=%d dst=%d zdst=%d len=%zu\n----\n%s----\n",
                key, st->mode, st->tev.tex_en, (!gl_blend && st->blend),
                st->depth_on, dst_used, zdst_used, len, src);
    prog = pgl_spec_link(src, key);
    free(src);
    if (!prog) {
        ++pgl_spec_gen_draws;
        ++pgl_spec_fail;
        return &pgl_spec[0];
    }
    i = pgl_nspec++;
    pgl_spec[i].key = key;
    pgl_spec[i].prog = prog;
    pgl_spec[i].draws = pgl_spec[i].px = pgl_spec[i].ns = 0;
    pgl_spec[i].depth_hw = pgl_depth_hw;   /* baked by pgl_spec_src */
    memset(&pgl_spec[i].u, 0, sizeof pgl_spec[i].u);
    pgl_query_locs(prog, &pgl_spec[i].u);
    fprintf(stderr, "GL backend: specialised shader #%d (key=%016llx "
            "mode=%d tex=%d blend=%d glblend=%d depth=%d/%d/%d alpha=%d "
            "dst=%d zdst=%d)\n",
            i, key, st->mode, st->tev.tex_en, st->blend, gl_blend,
            st->depth_on, st->depth_test, st->depth_write, st->alpha_test,
            dst_used, zdst_used);
    return &pgl_spec[i];
}

/* t_d8e5ab8c x t_5ddc492f: drop every specialised variant.  The generated
   source bakes the depth path (hardware gl_FragDepth vs the in-shader
   comparison), so when pgl_depth_fallback flips the mode at run time the
   cached variants are stale by construction.  Programs are leaked, not
   deleted (glDeleteProgram is not loaded; the rebuild path does the same). */
static void pgl_spec_reset(const char *why) {
    int i, n = pgl_nspec;
    for (i = 1; i < n; ++i) {
        pgl_spec[i].prog = 0;
        pgl_spec[i].key = 0;
        pgl_spec[i].draws = pgl_spec[i].px = pgl_spec[i].ns = 0;
        pgl_spec[i].depth_hw = -1;
        memset(&pgl_spec[i].u, 0, sizeof pgl_spec[i].u);
    }
    if (n > 1)
        fprintf(stderr, "GL backend: specialised cache reset (%s) — %d "
                        "variant(s) dropped\n", why, n - 1);
    pgl_nspec = 1;
    pgl_prog_cur = 0;         /* force pgl_prog_use() to re-establish state */
    pgl_depth_hw_applied = -1;
}

/* t_f8f2c1da: the depth path is a PROCESS-WIDE mode that the generated shader
   source depends on, so it may only be changed through this function: the
   specialised cache is invalidated in the same step.  Every writer used to do
   that by hand (and the replay harness had to remember pgl_spec_reset() twice),
   which is exactly how a stale variant could be served. */
static void pgl_depth_hw_set(int hw, const char *why) {
    int prev = pgl_depth_hw;
    hw = hw ? 1 : 0;
    pgl_depth_hw = hw;
    pgl_depth_hw_applied = -1;
    if (prev == hw) return;
    /* Before pgl_depth_mode_ready nothing can have been generated yet: the
       whole point is that the path is chosen ONCE, in pgl_setup, before the
       first draw and therefore before the first specialised program. */
    if (!pgl_depth_mode_ready) return;
    ++pgl_depth_mode_changes;
    fprintf(stderr, "GL backend: depth path %d -> %d (%s)\n",
            prev, pgl_depth_hw, why ? why : "?");
    pgl_spec_reset(why ? why : "depth path changed");
    /* Guest depth memory may hold values written under the other path, and the
       attachment/mirror contents are only meaningful for the old mode. */
    if (!pgl_split_cur) pgl_zforce_full = 1;
}

/* t_f8f2c1da: tripwire.  Called for every draw that uses a SPECIALISED program
   and for every cache hit, so a program generated under the other depth path
   can never be used silently.  The counter is unconditional (one compare); the
   environment knob only decides how loud it is. */
static void pgl_spec_modecheck_use(const PglProgSpec *e, const char *where) {
    /* Entry 0 is the generic program: it selects the path at run time through
       u_depth_hw, so it is valid in both modes (its depth_hw is -1, but it is
       also recognised by address because a static array starts zeroed). */
    if (!e || e == &pgl_spec[0] || e->depth_hw < 0) return;
    if (e->depth_hw == pgl_depth_hw) return;
    ++pgl_spec_modecheck_n;
    if (pgl_spec_modecheck < 0) {
        const char *m = getenv("DQ8_GL_SPEC_MODECHECK");
        pgl_spec_modecheck = (m && m[0]) ? atoi(m) : 0;
    }
    if (pgl_spec_modecheck > 0) {
        fprintf(stderr, "GL SPEC MODECHECK VIOLATION (%s): program key=%016llx "
                        "baked_depth_hw=%d live=%d draw=%llu #%llu\n",
                where, e->key, e->depth_hw, pgl_depth_hw,
                (unsigned long long)pgl_draws, pgl_spec_modecheck_n);
        if (pgl_spec_modecheck >= 2) {
            fflush(stderr);
            abort();
        }
    }
}

/* Upload the draw's uniforms into the CURRENT program (pgl_prog_cur).  A
   specialised program only declares the uniforms its shape needs; every
   location is checked because the array packing in front of a glUniform* call
   with location -1 is not free. */
static void pgl_set_uniforms(const PicaGlState *st, int gl_blend,
                             int dst_used, int zdst_used, PGint bx0, PGint by0) {
    if (pgl_spec_dbg > 0)
        fprintf(stderr, "SPECDBG prog=%u loc_const=%d loc_bufupd_c=%d "
                "val_bufupd0=%u loc_blend_const=%d loc_tex=%d\n",
                pgl_prog_cur, pgl_u_const_c, pgl_u_buf_upd_c,
                st->mode == PICA_GL_TEV ? st->tev.buf_upd_c[0] : 999u,
                pgl_u_blend_const, pgl_u_tex);
    pgl_glUseProgram(pgl_prog_cur);
    if (pgl_u_rt >= 0)
        pgl_glUniform2f(pgl_u_rt, (PGfloat)st->rt_w, (PGfloat)st->rt_h);
    if (pgl_u_mode >= 0)
        pgl_glUniform1i(pgl_u_mode, st->mode == PICA_GL_TEV ? 1 : 0);
    if (pgl_u_alpha_test >= 0)
        pgl_glUniform1i(pgl_u_alpha_test, st->alpha_test ? 1 : 0);
    if (pgl_u_alpha_func >= 0)
        pgl_glUniform1i(pgl_u_alpha_func, (PGint)st->alpha_func);
    if (pgl_u_alpha_ref >= 0)
        pgl_glUniform1f(pgl_u_alpha_ref, st->alpha_ref);
    if (pgl_u_iscale >= 0)
        pgl_glUniform1f(pgl_u_iscale, 1.0f / (PGfloat)pgl_cur_scale);
    if (pgl_u_dst_off >= 0)
        /* t_ab8159de: the u_dst/u_zdst copies are SUPERSAMPLED copies of the
           draw box, so their origin is the box origin in target texels. */
        pgl_glUniform2f(pgl_u_dst_off, (PGfloat)PGL_S(bx0), (PGfloat)PGL_S(by0));
    if (pgl_u_dst >= 0) pgl_glUniform1i(pgl_u_dst, 1);
    if (pgl_u_zdst >= 0) pgl_glUniform1i(pgl_u_zdst, 2);
    if (pgl_u_dst_used >= 0)
        pgl_glUniform1i(pgl_u_dst_used, dst_used ? 1 : 0);
    if (pgl_u_zdst_used >= 0)
        pgl_glUniform1i(pgl_u_zdst_used, zdst_used ? 1 : 0);
    if (pgl_u_depth_on >= 0)
        pgl_glUniform1i(pgl_u_depth_on, st->depth_on ? 1 : 0);
    /* t_d8e5ab8c x t_5ddc492f: the GENERIC program (cache entry 0) still picks
       the depth path at run time through u_depth_hw; the specialised variants
       bake the choice into their generated source, so they do not declare the
       uniform at all (location -1).  The applied value is per-program state —
       pgl_prog_use() invalidates the cache on every program change — and
       glUniform1i is never called with location 0 by accident (the location is
       looked up in pgl_query_locs, never assumed). */
    if (pgl_u_depth_hw >= 0 && pgl_depth_hw_applied != pgl_depth_hw) {
        pgl_glUniform1i(pgl_u_depth_hw, pgl_depth_hw ? 1 : 0);
        pgl_depth_hw_applied = pgl_depth_hw;
    }
    if (pgl_u_depth_test >= 0)
        pgl_glUniform1i(pgl_u_depth_test, st->depth_test ? 1 : 0);
    if (pgl_u_depth_write >= 0)
        pgl_glUniform1i(pgl_u_depth_write, st->depth_write ? 1 : 0);
    if (pgl_u_depth_func >= 0)
        pgl_glUniform1i(pgl_u_depth_func, (PGint)st->depth_func);
    if (pgl_u_depth_zbuf >= 0)
        pgl_glUniform1i(pgl_u_depth_zbuf, st->depth_zbuf ? 1 : 0);
    if (pgl_u_depth_scale >= 0)
        pgl_glUniform1f(pgl_u_depth_scale, st->depth_scale);
    if (pgl_u_depth_offset >= 0)
        pgl_glUniform1f(pgl_u_depth_offset, st->depth_offset);
    if (pgl_u_depth_zmax >= 0)
        pgl_glUniform1f(pgl_u_depth_zmax, st->depth_zmax);
    if (pgl_u_blend_en >= 0)
        pgl_glUniform1i(pgl_u_blend_en, (!gl_blend && st->blend) ? 1 : 0);
    if (pgl_u_eq_rgb >= 0)
        pgl_glUniform1i(pgl_u_eq_rgb, (PGint)st->blend_eq_rgb);
    if (pgl_u_eq_a >= 0)
        pgl_glUniform1i(pgl_u_eq_a, (PGint)st->blend_eq_a);
    if (pgl_u_f_src_rgb >= 0)
        pgl_glUniform1i(pgl_u_f_src_rgb, (PGint)st->blend_src_rgb);
    if (pgl_u_f_dst_rgb >= 0)
        pgl_glUniform1i(pgl_u_f_dst_rgb, (PGint)st->blend_dst_rgb);
    if (pgl_u_f_src_a >= 0)
        pgl_glUniform1i(pgl_u_f_src_a, (PGint)st->blend_src_a);
    if (pgl_u_f_dst_a >= 0)
        pgl_glUniform1i(pgl_u_f_dst_a, (PGint)st->blend_dst_a);
    if (pgl_u_blend_const >= 0)
        pgl_glUniform4f(pgl_u_blend_const, st->blend_const[0], st->blend_const[1],
                        st->blend_const[2], st->blend_const[3]);
    if (st->mode == PICA_GL_TEV) {
        PGint iv[18];
        unsigned s, j;
        if (pgl_u_src_c >= 0) {
            for (j = 0; j < 18u; ++j) iv[j] = (PGint)st->tev.src_c[j];
            pgl_glUniform1iv(pgl_u_src_c, 18, iv);
        }
        if (pgl_u_mod_c >= 0) {
            for (j = 0; j < 18u; ++j) iv[j] = (PGint)st->tev.mod_c[j];
            pgl_glUniform1iv(pgl_u_mod_c, 18, iv);
        }
        if (pgl_u_src_a >= 0) {
            for (j = 0; j < 18u; ++j) iv[j] = (PGint)st->tev.src_a[j];
            pgl_glUniform1iv(pgl_u_src_a, 18, iv);
        }
        if (pgl_u_mod_a >= 0) {
            for (j = 0; j < 18u; ++j) iv[j] = (PGint)st->tev.mod_a[j];
            pgl_glUniform1iv(pgl_u_mod_a, 18, iv);
        }
        if (pgl_u_cop >= 0) {
            for (j = 0; j < 6u; ++j) iv[j] = (PGint)st->tev.cop[j];
            pgl_glUniform1iv(pgl_u_cop, 6, iv);
        }
        if (pgl_u_aop >= 0) {
            for (j = 0; j < 6u; ++j) iv[j] = (PGint)st->tev.aop[j];
            pgl_glUniform1iv(pgl_u_aop, 6, iv);
        }
        if (pgl_u_buf_upd_c >= 0) {
            for (j = 0; j < 6u; ++j) iv[j] = (PGint)st->tev.buf_upd_c[j];
            pgl_glUniform1iv(pgl_u_buf_upd_c, 6, iv);
        }
        if (pgl_u_buf_upd_a >= 0) {
            for (j = 0; j < 6u; ++j) iv[j] = (PGint)st->tev.buf_upd_a[j];
            pgl_glUniform1iv(pgl_u_buf_upd_a, 6, iv);
        }
        if (pgl_u_scale_c >= 0) pgl_glUniform1fv(pgl_u_scale_c, 6, st->tev.scale_c);
        if (pgl_u_scale_a >= 0) pgl_glUniform1fv(pgl_u_scale_a, 6, st->tev.scale_a);
        if (pgl_u_const_c >= 0)
            for (s = 0; s < 6u; ++s)
                if (pgl_u_const_c_e[s] >= 0)
                    pgl_glUniform4f(pgl_u_const_c_e[s], st->tev.const_c[s][0],
                                    st->tev.const_c[s][1], st->tev.const_c[s][2],
                                    st->tev.const_c[s][3]);
        if (pgl_u_buf_init >= 0)
            pgl_glUniform4f(pgl_u_buf_init, st->tev.buf_init[0],
                            st->tev.buf_init[1], st->tev.buf_init[2],
                            st->tev.buf_init[3]);
        if (pgl_u_tex_en >= 0)
            pgl_glUniform1i(pgl_u_tex_en, st->tev.tex_en ? 1 : 0);
    } else if (pgl_u_tex_en >= 0) {
        pgl_glUniform1i(pgl_u_tex_en, 0);
    }
    if (pgl_u_tex >= 0) pgl_glUniform1i(pgl_u_tex, 0);
    /* t_e216fc37: texture unit 1 lives on GL texture unit 3 (0/1/2 are the
       staged image / blend source / depth source). */
    if (pgl_u_tex1 >= 0) pgl_glUniform1i(pgl_u_tex1, 3);
    if (pgl_u_tex1_en >= 0) pgl_glUniform1i(pgl_u_tex1_en, st->tex1_en ? 1 : 0);
}

/* ---- t_5ddc492f shape histogram (baseline classification) ----------------
   Independent of the program cache, so DQ8_GL_SHAPEHIST=1 also classifies the
   draws when the specialisation is off (the A/B reference run). */
typedef struct {
    unsigned mask, draws;
    unsigned long long px, ns;
} PglShape;
#define PGL_SHAPE_MAX 128
static PglShape pgl_shape[PGL_SHAPE_MAX];
static int pgl_shape_n;
static int pgl_shape_on = -1;

static unsigned pgl_shape_mask(const PicaGlState *st, int gl_blend,
                               int dst_used, int zdst_used) {
    unsigned m = 0, nact = 0;
    int s, i;
    if (st->mode == PICA_GL_TEV) {
        m |= 1u;
        if (st->tev.tex_en) m |= 2u;
        for (s = 0; s < 6; ++s) {
            if ((st->tev.cop[s] & 15u) != 0u || (st->tev.aop[s] & 15u) != 0u) ++nact;
            for (i = 0; i < 3; ++i) {
                if ((st->tev.src_c[s * 3 + i] & 15u) == 3u ||
                    (st->tev.src_a[s * 3 + i] & 15u) == 3u) m |= 2u;
            }
        }
    }
    if (st->blend) m |= gl_blend ? 8u : 4u;
    if (st->depth_on) {
        m |= 16u;
        if (st->depth_test) m |= 32u;
        if (st->depth_write) m |= 64u;
    }
    if (st->alpha_test) m |= 128u;
    if (dst_used) m |= 256u;
    if (zdst_used) m |= 512u;
    return m | (nact << 10);
}
static void pgl_shape_add(unsigned mask, unsigned long long px,
                          unsigned long long ns) {
    int i;
    if (pgl_shape_on < 0) {
        const char *e = getenv("DQ8_GL_SHAPEHIST");
        pgl_shape_on = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    if (!pgl_shape_on) return;
    for (i = 0; i < pgl_shape_n; ++i)
        if (pgl_shape[i].mask == mask) {
            ++pgl_shape[i].draws;
            pgl_shape[i].px += px;
            pgl_shape[i].ns += ns;
            return;
        }
    if (pgl_shape_n >= PGL_SHAPE_MAX) return;
    pgl_shape[pgl_shape_n].mask = mask;
    pgl_shape[pgl_shape_n].draws = 1;
    pgl_shape[pgl_shape_n].px = px;
    pgl_shape[pgl_shape_n].ns = ns;
    ++pgl_shape_n;
}
static void pgl_shape_str(unsigned k, char *out, size_t n) {
    snprintf(out, n, "%s%s%s%s%s%s%s%s%s%s st%u",
             (k & 1u) ? "tev" : "solid",
             (k & 2u) ? "+tex" : "",
             (k & 4u) ? "+blend" : "",
             (k & 8u) ? "+glblend" : "",
             (k & 16u) ? "+depth" : "",
             (k & 32u) ? "+dtest" : "",
             (k & 64u) ? "+dwrite" : "",
             (k & 128u) ? "+alpha" : "",
             (k & 256u) ? "+dst" : "",
             (k & 512u) ? "+zdst" : "",
             (k >> 10) & 7u);
}
static void pgl_shape_report(void) {
    int i;
    char desc[96];
    for (i = 0; i < pgl_shape_n; ++i) {
        if (!pgl_shape[i].draws) continue;
        pgl_shape_str(pgl_shape[i].mask, desc, sizeof desc);
        fprintf(stderr, "GLSHAPE %-52s draws=%u px=%llu ms=%.1f\n", desc,
                pgl_shape[i].draws, pgl_shape[i].px,
                (double)pgl_shape[i].ns / 1e6);
        pgl_shape[i].draws = 0;
        pgl_shape[i].px = 0;
        pgl_shape[i].ns = 0;
    }
}
static void pgl_spec_report(void) {
    int i;
    for (i = 1; i < pgl_nspec; ++i) {
        if (!pgl_spec[i].draws) continue;
        fprintf(stderr, "GLSPEC var=%d key=%016llx draws=%llu px_per=%llu "
                "sub_ms=%.1f us_per_draw=%.0f\n", i, pgl_spec[i].key,
                pgl_spec[i].draws,
                pgl_spec[i].px / (pgl_spec[i].draws ? pgl_spec[i].draws : 1u),
                (double)pgl_spec[i].ns / 1e6,
                (double)pgl_spec[i].ns / 1e3 /
                    (double)(pgl_spec[i].draws ? pgl_spec[i].draws : 1u));
        pgl_spec[i].draws = pgl_spec[i].px = pgl_spec[i].ns = 0;
    }
}

/* ---- t_5ddc492f in-process A/B of one draw -------------------------------
   For a draw that never reads the target (no blend, no destination/depth
   image read) the framebuffer write is a pure function of the flat payload,
   so re-rendering the SAME triangles with the generic program must produce
   the same pixels.  Rare enough to run with a per-draw glReadPixels. */
static void pgl_spec_verify(const PicaGlState *st, PglProgSpec *cur,
                            int gl_blend, int dst_used, int zdst_used,
                            int bx0, int by0, int bx1, int by1,
                            unsigned ntri) {
    int box[4];
    unsigned long long h1 = 0, h2 = 0, s1 = 0, s2 = 0, z1 = 0, z2 = 0, zs1 = 0, zs2 = 0;
    if (pgl_verify_on < 0) {
        const char *e = getenv("DQ8_GL_SPEC_VERIFY");
        pgl_verify_on = (e && e[0] && e[0] != '0') ? 1 : 0;
        pgl_verify_limit = (e && e[0]) ? atoi(e) : 0;
        if (pgl_verify_limit <= 0) pgl_verify_limit = pgl_verify_on ? 200 : 0;
    }
    if (!pgl_verify_on || (int)pgl_verify_n >= pgl_verify_limit) return;
    if (pgl_nspec < 2 || pgl_nspec <= 0 || cur == &pgl_spec[0]) return;
    if (gl_blend || st->blend) return;
    if (dst_used || zdst_used) return;
    if (!ntri || bx0 > bx1 || by0 > by1) return;
    box[0] = bx0; box[1] = by0; box[2] = bx1; box[3] = by1;
    if (!pgl_dh_box(box, &h1, &s1)) return;
    if (st->depth_on && !pgl_dh_zbox(box, &z1, &zs1)) return;
    pgl_prog_use(&pgl_spec[0]);
    pgl_set_uniforms(st, gl_blend, dst_used, zdst_used, (PGint)bx0, (PGint)by0);
    pgl_glDrawArraysInstanced(PGGL_TRIANGLES, 0, 3, (PGsizei)ntri);
    if (!pgl_dh_box(box, &h2, &s2)) return;
    if (st->depth_on && !pgl_dh_zbox(box, &z2, &zs2)) return;
    ++pgl_verify_n;
    if (h1 != h2) {
        ++pgl_verify_cmism;
        if (pgl_verify_cmism <= 10)
            fprintf(stderr, "GLSPECVERIFY MISMATCH draw#%llu colour "
                    "spec=%016llx/%llu generic=%016llx/%llu box=%d,%d..%d,%d\n",
                    (unsigned long long)pgl_draws, h1, s1, h2, s2,
                    bx0, by0, bx1, by1);
    }
    if (st->depth_on && z1 != z2) {
        ++pgl_verify_zmism;
        if (pgl_verify_zmism <= 10)
            fprintf(stderr, "GLSPECVERIFY MISMATCH draw#%llu depth "
                    "spec=%016llx/%llu generic=%016llx/%llu\n",
                    (unsigned long long)pgl_draws, z1, zs1, z2, zs2);
    }
    /* the FBO now holds the generic result, which just compared equal */
    pgl_prog_use(cur);
    pgl_set_uniforms(st, gl_blend, dst_used, zdst_used, (PGint)bx0, (PGint)by0);
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
    /* t_d559a510: a rebuilt program may come with a fresh context; never trust
       the cached blend state across it. */
    pgl_blend_applied = -1;
    pgl_blend_enabled = -1;
    pgl_blend_guard_armed = 0;
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
    /* t_5ddc492f: cache entry 0 is always the generic shader and the fallback
       for every shape the specialised cache cannot serve. */
    memset(&pgl_spec[0].u, 0, sizeof pgl_spec[0].u);
    pgl_query_locs(pgl_program, &pgl_spec[0].u);
    pgl_spec[0].prog = pgl_program;
    pgl_spec[0].key = 0;
    pgl_spec[0].draws = pgl_spec[0].px = pgl_spec[0].ns = 0;
    if (pgl_nspec < 1) pgl_nspec = 1;
    pgl_prog_cur = pgl_program;
    pgl_depth_hw_applied = -1;   /* fresh program: uniform 0 is unknown */
    return 1;
}

/* forward declaration: defined after pgl_ensure_rt */
static void pgl_depth_fallback(const char *why);

/* t_a22413f1 run3: DEPTH PATH SELF-TEST — runs once per context, on the
   operator's own driver, because the failure that made this necessary
   (D32F depth attachment -> almost fully black frame on radeonsi) cannot be
   reproduced on this host (llvmpipe only).  It exercises exactly the GL
   features the HARDWARE depth path depends on:
     - a 4x4 FBO whose depth attachment is DEPTH_COMPONENT32F is COMPLETE;
     - a fragment shader writing gl_FragDepth actually controls the test;
     - the depth test rejects a farther fragment of the SAME draw;
     - the depth attachment reads back (the guest write-back mirrors it).
   One full-screen triangle is drawn three times with a constant gl_FragDepth:
     0.25 -> must pass (depth cleared to 1.0, GL_LESS)
     0.75 -> must be REJECTED (the colour must still show 0.25)
     0.20 -> must pass
   The colour channel carries the depth the shader wrote, so a driver that
   ignores gl_FragDepth (or a depth attachment that is silently disabled)
   turns step 2 or 3 red here instead of presenting the operator a black
   screen.  Returns 1 = pass, -1 = fail (caller falls back to IN-SHADER). */
static int pgl_depth_selftest_run(void) {
    static const char vs_src[] =
        "#version 330 core\n"
        "layout(location = 0) in vec2 a_pos;\n"
        "void main() { gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
    static const char fs_src[] =
        "#version 330 core\n"
        "uniform float u_z;\n"
        "out vec4 o_colour;\n"
        "void main() { gl_FragDepth = u_z; o_colour = vec4(u_z, 0.0, 0.0, 1.0); }\n";
    PGuint fbo = 0, ctex = 0, dtex = 0, vao = 0, vbo = 0, vs = 0, fs = 0, prog = 0;
    PGenum status, buf0 = PGGL_COLOR_ATTACHMENT0;
    float verts[6] = { -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f };
    float zclear = 1.0f, zval;
    unsigned char px[64];
    float dz[16];
    char log[512];
    PGsizei n = 0;
    PGint loc, ok_prog = 0;
    int r0 = -1, r1 = -1, r2 = -1, d0 = -1, res = -1;

    memset(px, 0, sizeof px);
    memset(dz, 0, sizeof dz);
    memset(log, 0, sizeof log);

    /* ---- shaders ---- */
    vs = pgl_glCreateShader(PGGL_VERTEX_SHADER);
    if (!vs) goto done;
    {   const char *src_one = vs_src;
        pgl_glShaderSource(vs, 1, &src_one, NULL);
    }
    pgl_glCompileShader(vs);
    pgl_glGetShaderiv(vs, PGGL_COMPILE_STATUS, &ok_prog);
    if (!ok_prog) {
        pgl_glGetShaderInfoLog(vs, (PGsizei)sizeof log, &n, log);
        fprintf(stderr, "GL backend: depth self-test: vertex shader failed: %.*s\n", (int)n, log);
        goto done;
    }
    fs = pgl_glCreateShader(PGGL_FRAGMENT_SHADER);
    if (!fs) goto done;
    {   const char *src_one = fs_src;
        pgl_glShaderSource(fs, 1, &src_one, NULL);
    }
    pgl_glCompileShader(fs);
    pgl_glGetShaderiv(fs, PGGL_COMPILE_STATUS, &ok_prog);
    if (!ok_prog) {
        pgl_glGetShaderInfoLog(fs, (PGsizei)sizeof log, &n, log);
        fprintf(stderr, "GL backend: depth self-test: fragment shader failed: %.*s\n", (int)n, log);
        goto done;
    }
    prog = pgl_glCreateProgram();
    if (!prog) goto done;
    pgl_glAttachShader(prog, vs);
    pgl_glAttachShader(prog, fs);
    pgl_glLinkProgram(prog);
    pgl_glGetProgramiv(prog, PGGL_LINK_STATUS, &ok_prog);
    if (!ok_prog) {
        pgl_glGetProgramInfoLog(prog, (PGsizei)sizeof log, &n, log);
        fprintf(stderr, "GL backend: depth self-test: link failed: %.*s\n", (int)n, log);
        goto done;
    }
    loc = pgl_glGetUniformLocation(prog, "u_z");

    /* ---- geometry ---- */
    pgl_glGenVertexArrays(1, &vao);
    pgl_glGenBuffers(1, &vbo);
    pgl_glBindVertexArray(vao);
    pgl_glBindBuffer(PGGL_ARRAY_BUFFER, vbo);
    pgl_glBufferData(PGGL_ARRAY_BUFFER, (PGsizei)sizeof verts, verts, 0x88E4u /* GL_STATIC_DRAW */);
    pgl_glEnableVertexAttribArray(0);
    pgl_glVertexAttribPointer(0, 2, PGGL_FLOAT, 0 /* GL_FALSE */, 0, NULL);

    /* ---- 4x4 colour + D32F depth FBO ---- */
    pgl_glGenFramebuffers(1, &fbo);
    pgl_glGenTextures(1, &ctex);
    pgl_glGenTextures(1, &dtex);
    pgl_glBindTexture(PGGL_TEXTURE_2D, ctex);
    pgl_glTexImage2D(PGGL_TEXTURE_2D, 0, PGGL_RGBA8, 4, 4, 0, PGGL_RGBA,
                     PGGL_UNSIGNED_BYTE, NULL);
    pgl_glBindTexture(PGGL_TEXTURE_2D, dtex);
    pgl_glTexImage2D(PGGL_TEXTURE_2D, 0, PGGL_DEPTH_COMPONENT32F, 4, 4, 0,
                     PGGL_DEPTH_COMPONENT, PGGL_FLOAT, NULL);
    pgl_glBindFramebuffer(PGGL_FRAMEBUFFER, fbo);
    pgl_glFramebufferTexture2D(PGGL_FRAMEBUFFER, PGGL_COLOR_ATTACHMENT0,
                               PGGL_TEXTURE_2D, ctex, 0);
    pgl_glFramebufferTexture2D(PGGL_FRAMEBUFFER, PGGL_DEPTH_ATTACHMENT,
                               PGGL_TEXTURE_2D, dtex, 0);
    status = pgl_glCheckFramebufferStatus(PGGL_FRAMEBUFFER);
    if (status != PGGL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "GL backend: depth self-test: FAIL: 4x4 RGBA8 + "
                        "DEPTH_COMPONENT32F FBO incomplete (0x%04X, complete=0x%04X)\n",
                (unsigned)status, (unsigned)PGGL_FRAMEBUFFER_COMPLETE);
        goto done;
    }

    /* ---- draw the known sequence ---- */
    pgl_glViewport(0, 0, 4, 4);
    pgl_glDrawBuffers(1, &buf0);
    pgl_glColorMask(1, 1, 1, 1);
    pgl_glDisable(PGGL_BLEND);
    pgl_glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    pgl_glClear(PGGL_COLOR_BUFFER_BIT);
    pgl_glDepthMask(1);
    pgl_glClearBufferfv(PGGL_DEPTH, 0, &zclear);
    pgl_glEnable(PGGL_DEPTH_TEST);
    pgl_glDepthFunc(PGGL_LESS);
    pgl_glUseProgram(prog);

    zval = 0.25f; pgl_glUniform1f(loc, zval);
    pgl_glDrawArrays(PGGL_TRIANGLES, 0, 3);
    pgl_glReadBuffer(PGGL_COLOR_ATTACHMENT0);
    pgl_glReadPixels(0, 0, 4, 4, PGGL_RGBA, PGGL_UNSIGNED_BYTE, px);
    r0 = (int)px[0];

    zval = 0.75f; pgl_glUniform1f(loc, zval);
    pgl_glDrawArrays(PGGL_TRIANGLES, 0, 3);
    pgl_glReadPixels(0, 0, 4, 4, PGGL_RGBA, PGGL_UNSIGNED_BYTE, px);
    r1 = (int)px[0];

    zval = 0.20f; pgl_glUniform1f(loc, zval);
    pgl_glDrawArrays(PGGL_TRIANGLES, 0, 3);
    pgl_glReadPixels(0, 0, 4, 4, PGGL_RGBA, PGGL_UNSIGNED_BYTE, px);
    r2 = (int)px[0];
    pgl_glReadPixels(0, 0, 4, 4, PGGL_DEPTH_COMPONENT, PGGL_FLOAT, dz);
    d0 = (int)(dz[0] * 1000.0f + 0.5f);

    /* 0.25 -> 64, 0.75 rejected, 0.20 -> 51, depth read back ~0.200 */
    if (r0 < 63 || r0 > 65) {
        fprintf(stderr, "GL backend: depth self-test: FAIL: first depth draw "
                        "did not pass (colour %d, expected 64)\n", r0);
        goto done;
    }
    if (r1 != r0) {
        fprintf(stderr, "GL backend: depth self-test: FAIL: the farther "
                        "fragment (z=0.75) was NOT rejected by the depth test "
                        "(colour %d, expected %d)\n", r1, r0);
        goto done;
    }
    if (r2 < 50 || r2 > 52) {
        fprintf(stderr, "GL backend: depth self-test: FAIL: the nearer "
                        "fragment (z=0.20) did not pass (colour %d, expected 51)\n", r2);
        goto done;
    }
    if (d0 < 195 || d0 > 205) {
        fprintf(stderr, "GL backend: depth self-test: FAIL: depth attachment "
                        "read-back = %d/1000, expected ~200\n", d0);
        goto done;
    }
    fprintf(stderr, "GL backend: depth self-test: PASS (FBO complete, "
                    "gl_FragDepth 0.25 pass / 0.75 rejected / 0.20 pass, "
                    "depth read-back %d/1000)\n", d0);
    res = 1;

done:
    if (prog) pgl_glUseProgram(0);
    if (vbo) pgl_glDeleteBuffers(1, &vbo);
    if (vao) pgl_glDeleteVertexArrays(1, &vao);
    if (fbo) pgl_glDeleteFramebuffers(1, &fbo);
    if (ctex) pgl_glDeleteTextures(1, &ctex);
    if (dtex) pgl_glDeleteTextures(1, &dtex);
    if (vs) pgl_glDeleteShader(vs);
    if (fs) pgl_glDeleteShader(fs);
    /* the program object is leaked on purpose: glDeleteProgram is not loaded
       and this runs exactly once per context */
    pgl_glBindFramebuffer(PGGL_FRAMEBUFFER, 0);
    pgl_glDisable(PGGL_DEPTH_TEST);
    if (res != 1 && pgl_errs_logged < 40u) {
        PGenum e = 0;
        if (pgl_drain_errors("depth-selftest", &e) != 0) {
            ++pgl_errs_logged;
            fprintf(stderr, "GL backend: depth self-test: first GL error "
                            "0x%04X %s\n", (unsigned)e, pgl_err_name(e));
        }
    } else {
        pgl_drain_errors("depth-selftest", NULL);
    }
    return res;
}

/* t_599a43f7: DEPTH SELF-TEST, PART 2 — the PRODUCTION pipeline.
   pgl_depth_selftest_run() above proves the driver can write gl_FragDepth and
   test against a D32F attachment, in a one-colour-attachment 4x4 FBO, with the
   depth write mask forced on.  It PASSED on the operator's Radeon while the real
   path presented a black screen, so it does not cover what actually broke:
     - the production FBO layout: COLOR_ATTACHMENT0 RGBA8 + COLOR_ATTACHMENT1
       R32F (the guest depth mirror) + DEPTH_ATTACHMENT D32F;
     - the production ORDER inside a frame: ... draws leave glDepthMask(0)
       behind, then the frame's depth clear happens.  glClearBufferfv on a
       depth attachment is gated by that mask, so a clear written without
       forcing the mask clears NOTHING and every later fragment fails GL_LESS —
       exactly the DQ8_SPLIT_GL black frame (the worker-thread clear had lost
       the t_d8e5ab8c fix);
     - the mirror/depth read-back the guest write-back depends on.
   Every clear here goes through pgl_depth_attachment_clear(), the same helper
   the runtime uses, so a regression in either thread shows up at startup.
   Returns 1 = pass, -1 = fail (caller falls back to IN-SHADER).  Each stage
   prints PASS/FAIL on its own line: on a driver we cannot test locally, that
   line names the broken feature. */
static int pgl_depth_selftest_pipeline(void) {
    static const char vs_src[] =
        "#version 330 core\n"
        "layout(location = 0) in vec2 a_pos;\n"
        "layout(location = 1) in float a_z;\n"
        "layout(location = 2) in vec3 a_col;\n"
        "flat out float v_z;\n"
        "flat out vec3 v_col;\n"
        "void main() { v_z = a_z; v_col = a_col;\n"
        "              gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
    static const char fs_src[] =
        "#version 330 core\n"
        "flat in float v_z;\n"
        "flat in vec3 v_col;\n"
        "layout(location = 0) out vec4 o_col;\n"
        "layout(location = 1) out vec4 o_z;\n"
        "void main() { gl_FragDepth = clamp(v_z, 0.0, 1.0);\n"
        "              o_col = vec4(v_col, 1.0);\n"
        "              o_z = vec4(v_z * 16777216.0, 0.0, 0.0, 1.0); }\n";
    /* t_ab8159de: the depth self-test runs at the INTERNAL scale (a scaled
       target is where the depth path has to work, so 4x must be exercised at
       4x, not at 16x16). */
    const int N = 16 * pgl_scale;
    PGuint fbo = 0, ctex = 0, ztex = 0, dtex = 0, vao = 0, vbo = 0, vs = 0, fs = 0, prog = 0;
    PGenum status, bufs[2] = { PGGL_COLOR_ATTACHMENT0, PGGL_COLOR_ATTACHMENT1 };
    float v[18];              /* 3 verts x (x,y,z,r,g,b) — unused placeholder */
    (void)v;
    unsigned char px[4 * 4 * 4];
    float rd[4];
    float zclear = 1.0f;
    char log[512];
    PGsizei n = 0;
    PGint loc_a = -1, loc_z = -1, loc_c = -1, ok = 0;
    int order = -1, clearmask = -1, mirror = -1, zread = -1, reject = -1;
    int res = -1;

    pgl_selftest_active = 1;
    memset(log, 0, sizeof log);
    memset(px, 0, sizeof px);
    memset(rd, 0, sizeof rd);

    /* ---- program ---- */
    vs = pgl_glCreateShader(PGGL_VERTEX_SHADER);
    if (!vs) goto done;
    { const char *s = vs_src; pgl_glShaderSource(vs, 1, &s, NULL); }
    pgl_glCompileShader(vs);
    pgl_glGetShaderiv(vs, PGGL_COMPILE_STATUS, &ok);
    if (!ok) {
        pgl_glGetShaderInfoLog(vs, (PGsizei)sizeof log, &n, log);
        fprintf(stderr, "GL backend: depth self-test[2]: vertex shader failed: %.*s\n",
                (int)n, log);
        goto done;
    }
    fs = pgl_glCreateShader(PGGL_FRAGMENT_SHADER);
    if (!fs) goto done;
    { const char *s = fs_src; pgl_glShaderSource(fs, 1, &s, NULL); }
    pgl_glCompileShader(fs);
    pgl_glGetShaderiv(fs, PGGL_COMPILE_STATUS, &ok);
    if (!ok) {
        pgl_glGetShaderInfoLog(fs, (PGsizei)sizeof log, &n, log);
        fprintf(stderr, "GL backend: depth self-test[2]: fragment shader failed: %.*s\n",
                (int)n, log);
        goto done;
    }
    prog = pgl_glCreateProgram();
    pgl_glAttachShader(prog, vs);
    pgl_glAttachShader(prog, fs);
    pgl_glLinkProgram(prog);
    pgl_glGetProgramiv(prog, PGGL_LINK_STATUS, &ok);
    if (!ok) {
        pgl_glGetProgramInfoLog(prog, (PGsizei)sizeof log, &n, log);
        fprintf(stderr, "GL backend: depth self-test[2]: link failed: %.*s\n",
                (int)n, log);
        goto done;
    }
    loc_a = 0; loc_z = 1; loc_c = 2;   /* layout(location=..) in vs_src */

    /* ---- PRODUCTION FBO LAYOUT ---- */
    pgl_glGenFramebuffers(1, &fbo);
    pgl_glGenTextures(1, &ctex);
    pgl_glGenTextures(1, &ztex);
    pgl_glGenTextures(1, &dtex);
    pgl_glBindTexture(PGGL_TEXTURE_2D, ctex);
    pgl_glTexImage2D(PGGL_TEXTURE_2D, 0, PGGL_RGBA8, N, N, 0, PGGL_RGBA,
                     PGGL_UNSIGNED_BYTE, NULL);
    pgl_glBindTexture(PGGL_TEXTURE_2D, ztex);
    pgl_glTexImage2D(PGGL_TEXTURE_2D, 0, PGGL_R32F, N, N, 0, PGGL_RED,
                     PGGL_FLOAT, NULL);
    pgl_glBindTexture(PGGL_TEXTURE_2D, dtex);
    pgl_glTexImage2D(PGGL_TEXTURE_2D, 0, PGGL_DEPTH_COMPONENT32F, N, N, 0,
                     PGGL_DEPTH_COMPONENT, PGGL_FLOAT, NULL);
    pgl_glBindFramebuffer(PGGL_FRAMEBUFFER, fbo);
    pgl_glFramebufferTexture2D(PGGL_FRAMEBUFFER, PGGL_COLOR_ATTACHMENT0,
                               PGGL_TEXTURE_2D, ctex, 0);
    pgl_glFramebufferTexture2D(PGGL_FRAMEBUFFER, PGGL_COLOR_ATTACHMENT1,
                               PGGL_TEXTURE_2D, ztex, 0);
    pgl_glFramebufferTexture2D(PGGL_FRAMEBUFFER, PGGL_DEPTH_ATTACHMENT,
                               PGGL_TEXTURE_2D, dtex, 0);
    pgl_glDrawBuffers(2, bufs);
    status = pgl_glCheckFramebufferStatus(PGGL_FRAMEBUFFER);
    pgl_glViewport(0, 0, N, N);
    pgl_glDisable(PGGL_BLEND);
    pgl_glColorMask(1, 1, 1, 1);
    pgl_glEnable(PGGL_DEPTH_TEST);
    pgl_glDepthFunc(PGGL_LESS);
    {
        int layout_ok = (status == PGGL_FRAMEBUFFER_COMPLETE);
        fprintf(stderr, "GL backend: depth self-test[2] layout: %s "
                        "(RGBA8+R32F+D32F status=0x%04X complete=0x%04X)\n",
                layout_ok ? "PASS" : "FAIL", (unsigned)status,
                (unsigned)PGGL_FRAMEBUFFER_COMPLETE);
        if (!layout_ok) goto done;
    }

    /* ---- geometry: full-screen triangles ---- */
    pgl_glGenVertexArrays(1, &vao);
    pgl_glGenBuffers(1, &vbo);
    pgl_glBindVertexArray(vao);
    pgl_glBindBuffer(PGGL_ARRAY_BUFFER, vbo);
    pgl_glBufferData(PGGL_ARRAY_BUFFER, (PGsizei)(36 * sizeof(float)), NULL, 0x88E8u /* GL_DYNAMIC_DRAW */);
    if (loc_a >= 0) {
        pgl_glEnableVertexAttribArray((PGuint)loc_a);
        pgl_glVertexAttribPointer((PGuint)loc_a, 2, PGGL_FLOAT, 0, 6 * (PGint)sizeof(float), (const void *)0);
    }
    if (loc_z >= 0) {
        pgl_glEnableVertexAttribArray((PGuint)loc_z);
        pgl_glVertexAttribPointer((PGuint)loc_z, 1, PGGL_FLOAT, 0, 6 * (PGint)sizeof(float),
                                  (const void *)(2 * sizeof(float)));
    }
    if (loc_c >= 0) {
        pgl_glEnableVertexAttribArray((PGuint)loc_c);
        pgl_glVertexAttribPointer((PGuint)loc_c, 3, PGGL_FLOAT, 0, 6 * (PGint)sizeof(float),
                                  (const void *)(3 * sizeof(float)));
    }
    pgl_glUseProgram(prog);

    /* ---- stage A: clear, then TWO triangles of ONE draw, near one second ---- */
    pgl_glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    pgl_glColorMask(1, 1, 1, 1);
    pgl_glClear(PGGL_COLOR_BUFFER_BIT);
    pgl_depth_attachment_clear(zclear);      /* the production helper */
    pgl_glDepthMask(1);
    pgl_depth_mask_applied = -1;
    {
        /* tri 0: z=0.75 red (far)   tri 1: z=0.25 green (near), ONE draw */
        static const float quad[36] = {
            -1, -1, 0.75f, 1, 0, 0,   3, -1, 0.75f, 1, 0, 0,   -1,  3, 0.75f, 1, 0, 0,
            -1, -1, 0.25f, 0, 1, 0,   3, -1, 0.25f, 0, 1, 0,   -1,  3, 0.25f, 0, 1, 0
        };
        pgl_glBufferSubData(PGGL_ARRAY_BUFFER, 0, (PGsizei)sizeof quad, quad);
        pgl_glDrawArrays(PGGL_TRIANGLES, 0, 6);
    }
    pgl_glReadBuffer(PGGL_COLOR_ATTACHMENT0);
    pgl_glReadPixels(N / 2, N / 2, 1, 1, PGGL_RGBA, PGGL_UNSIGNED_BYTE, px);
    order = (px[0] < 8 && px[1] > 247 && px[2] < 8) ? 1 : 0;
    fprintf(stderr, "GL backend: depth self-test[2] same-draw order: %s "
                    "(near triangle of ONE draw must win: %u,%u,%u)\n",
            order == 1 ? "PASS" : "FAIL", px[0], px[1], px[2]);

    /* ---- stage B: THE REGRESSION — depth clear after a depth-write-off draw ----
       PICA leaves glDepthMask(0) behind all the time; the frame's depth clear
       must still clear.  pgl_depth_attachment_clear() is what forces the mask,
       so this stage fails if that safety is ever dropped from either thread. */
    pgl_glDepthMask(0);              /* exactly what a depth-write-off draw leaves */
    pgl_depth_mask_applied = 0;
    pgl_depth_attachment_clear(zclear);
    {
        const float tris[18] = { -1, -1, 0.9f, 0, 0, 1,   3, -1, 0.9f, 0, 0, 1,
                                 -1,  3, 0.9f, 0, 0, 1 };
        pgl_glBufferSubData(PGGL_ARRAY_BUFFER, 0, (PGsizei)sizeof tris, tris);
        pgl_glDepthMask(1);          /* what the draw path does for a write-on draw */
        pgl_depth_mask_applied = -1;
        pgl_glDrawArrays(PGGL_TRIANGLES, 0, 3);
    }
    pgl_glReadBuffer(PGGL_COLOR_ATTACHMENT0);
    pgl_glReadPixels(N / 2, N / 2, 1, 1, PGGL_RGBA, PGGL_UNSIGNED_BYTE, px);
    clearmask = (px[0] < 8 && px[1] < 8 && px[2] > 247) ? 1 : 0;
    if (clearmask == 1)
        fprintf(stderr, "GL backend: depth self-test[2] clear-after-mask0: PASS "
                        "(the depth clear after a depth-write-off draw is not a "
                        "no-op: %u,%u,%u)\n", px[0], px[1], px[2]);
    else
        fprintf(stderr, "GL backend: depth self-test[2] clear-after-mask0: FAIL "
                        "(%u,%u,%u — the depth clear was gated by the write mask: "
                        "stale depth, every fragment fails, BLACK FRAME)\n",
                px[0], px[1], px[2]);

    /* ---- stage C: the mirror and the attachment carry the same depth ---- */
    pgl_glReadBuffer(PGGL_COLOR_ATTACHMENT1);
    pgl_glReadPixels(N / 2, N / 2, 1, 1, PGGL_RED, PGGL_FLOAT, rd);
    mirror = (rd[0] > 0.9f * 16777216.0f - 64.0f &&
              rd[0] < 0.9f * 16777216.0f + 64.0f) ? 1 : 0;
    fprintf(stderr, "GL backend: depth self-test[2] att1 mirror: %s "
                    "(R32F read-back %.0f, expected %.0f)\n",
            mirror == 1 ? "PASS" : "FAIL", (double)rd[0], 0.9 * 16777216.0);
    memset(rd, 0, sizeof rd);
    pgl_glReadPixels(N / 2, N / 2, 1, 1, PGGL_DEPTH_COMPONENT, PGGL_FLOAT, rd);
    zread = (rd[0] > 0.899f && rd[0] < 0.901f) ? 1 : 0;
    fprintf(stderr, "GL backend: depth self-test[2] D32F read-back: %s "
                    "(%.6f, expected 0.9)\n", zread == 1 ? "PASS" : "FAIL",
            (double)rd[0]);

    /* ---- stage D: a farther fragment must be rejected ---- */
    {
        const float tris[18] = { -1, -1, 0.95f, 1, 1, 0,   3, -1, 0.95f, 1, 1, 0,
                                 -1,  3, 0.95f, 1, 1, 0 };
        pgl_glBufferSubData(PGGL_ARRAY_BUFFER, 0, (PGsizei)sizeof tris, tris);
        pgl_glDrawArrays(PGGL_TRIANGLES, 0, 3);
    }
    pgl_glReadBuffer(PGGL_COLOR_ATTACHMENT0);
    pgl_glReadPixels(N / 2, N / 2, 1, 1, PGGL_RGBA, PGGL_UNSIGNED_BYTE, px);
    reject = (px[2] > 247 && px[0] < 8) ? 1 : 0;
    fprintf(stderr, "GL backend: depth self-test[2] farther rejected: %s "
                    "(%u,%u,%u — must still be the blue pass)\n",
            reject == 1 ? "PASS" : "FAIL", px[0], px[1], px[2]);

    if (order == 1 && clearmask == 1 && mirror == 1 && zread == 1 && reject == 1) {
        fprintf(stderr, "GL backend: depth self-test[2]: PASS (production FBO "
                        "layout, same-draw order, clear-after-mask0, mirror and "
                        "D32F read-back, farther-rejected)\n");
        res = 1;
    } else {
        fprintf(stderr, "GL backend: depth self-test[2]: FAIL "
                        "(order=%d clearmask=%d mirror=%d zread=%d reject=%d)\n",
                order, clearmask, mirror, zread, reject);
    }

done:
    pgl_selftest_active = 0;
    if (prog) pgl_glUseProgram(0);
    if (vbo) pgl_glDeleteBuffers(1, &vbo);
    if (vao) pgl_glDeleteVertexArrays(1, &vao);
    if (fbo) pgl_glDeleteFramebuffers(1, &fbo);
    if (ctex) pgl_glDeleteTextures(1, &ctex);
    if (ztex) pgl_glDeleteTextures(1, &ztex);
    if (dtex) pgl_glDeleteTextures(1, &dtex);
    if (vs) pgl_glDeleteShader(vs);
    if (fs) pgl_glDeleteShader(fs);
    pgl_glBindFramebuffer(PGGL_FRAMEBUFFER, 0);
    pgl_glDisable(PGGL_DEPTH_TEST);
    pgl_glDepthMask(1);
    pgl_drain_errors("depth-selftest2", NULL);
    if (res != 1) {
        /* Any stray GL error is part of the evidence on the operator's driver. */
        fprintf(stderr, "GL backend: depth self-test[2]: result=%d\n", res);
    }
    return res;
}

static int pgl_setup(PicaGlGetProc getproc) {
    if (!pgl_load(getproc)) return 0;
    /* t_ab8159de: internal render scale (DQ8_RENDER_SCALE) — read before
       anything sizes a target, so the first FBO is built at the right size. */
    pgl_scale_value();
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
    /* t_ab8159de/t_05281b89: what the internal scale costs, printed once,
       together with the actual target sizes it produces. */
    if (pgl_scale > 1)
        fprintf(stderr, "GL backend: internal render scale x%d (DQ8_RENDER_SCALE) "
                "— 512x400 -> %dx%d, 400x240 -> %dx%d, 320x240 -> %dx%d, "
                "128x128 -> %dx%d targets; %.1f MB per 512x400 target (RGBA8 "
                "colour + R32F z mirror + D32F depth), %.1f MB at native; "
                "sampled-target model = %s (DQ8_RENDER_SCALE_SAMPLED)\n",
                pgl_scale, 512 * pgl_scale, 400 * pgl_scale,
                400 * pgl_scale, 240 * pgl_scale,
                320 * pgl_scale, 240 * pgl_scale,
                128 * pgl_scale, 128 * pgl_scale,
                3.0 * 512.0 * 400.0 * (double)pgl_scale * (double)pgl_scale /
                    1048576.0,
                3.0 * 512.0 * 400.0 / 1048576.0,
                pgl_scale_sampled() ? "scaled (Azahar)" : "stage-A demote");
    else
        fprintf(stderr, "GL backend: internal render scale x1 (DQ8_RENDER_SCALE "
                        "unset) — native targets, guest framebuffer passes "
                        "through unchanged\n");
    /* ---- t_8c05fac8: framebuffer-fetch mode decision (context is current).
       DQ8_GL_FBFETCH=0 forces the legacy per-draw copy path; DQ8_GL_DSTALWAYS=1
       (legacy reference mode) also forces copies.  The extension + entry
       points must both exist on this context (GL 3.3 core exposes ARB exts
       independently of the context version). */
    {
        const char *ef = getenv("DQ8_GL_FBFETCH");
        const char *ea = getenv("DQ8_GL_DSTALWAYS");
        /* t_9651f10a: framebuffer fetch is DEFAULT ON for every driver whose
           live-image reads are verified pixel-clean: the software rasterisers
           used by this machine (llvmpipe, softpipe, swrast — t_8c05fac8,
           t_6b81015d, t_d559a510) and the operator's radeonsi/Mesa Radeon
           (verified with the pgl_dst_barrier texture barriers that make a read
           of the live render target well-defined, GL 4.5 §9.3).  The safety net
           for everything else stays, but as a *known-good allowlist* rather
           than a blanket off: a renderer outside the list (nVidia, Intel,
           ...) is untested, and an unordered destination read on the UI's
           translucent draws is exactly the show-through artefact t_6b81015d
           fixed.  Such a driver keeps the per-draw copy path unless the
           operator opts in with DQ8_GL_FBFETCH=1; DQ8_GL_FBFETCH=0 forces the
           copies on any driver (A/B). */
        static const char *const pgl_fbfetch_ok[] = {
            "llvmpipe",   /* verified: t_8c05fac8, t_6b81015d, t_d559a510 */
            "softpipe",   /* verified: t_8c05fac8 */
            "swrast",     /* verified: t_8c05fac8 */
            "radeonsi",   /* verified: operator Radeon (RX 6xxx), t_6b81015d */
            NULL
        };
        int verified = 0, fbi;
        if (pgl_renderer[0])
            for (fbi = 0; pgl_fbfetch_ok[fbi]; ++fbi)
                if (strstr(pgl_renderer, pgl_fbfetch_ok[fbi])) {
                    verified = 1;
                    break;
                }
        int have_env = ef && ef[0];
        int want = have_env ? (ef[0] != '0') : verified;
        if (!have_env)
            fprintf(stderr, "GL backend: renderer %s is %s the verified list "
                    "(llvmpipe, softpipe, swrast, radeonsi) -> framebuffer fetch "
                    "%s by default (set DQ8_GL_FBFETCH=1 to force the live-image "
                    "path, =0 to force per-draw copies)\n",
                    pgl_renderer[0] ? pgl_renderer : "(unknown)",
                    verified ? "on" : "not on", verified ? "on" : "off");
        int legacy = (ea && ea[0] && ea[0] != '0');
        int have_ext = 0;
        if (want && !legacy) {
            /* t_d8e5ab8c: glGetString(GL_EXTENSIONS) is GL_INVALID_ENUM in a
               core profile — that was the "setup-program: err=0x0500
               GL_INVALID_ENUM" line every run logged.  glGetStringi is the
               core-profile query; only fall back to the legacy string on a
               context that has no indexed query (GL < 3.0). */
            if (pgl_glGetStringi) {
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
            } else {
                const char *s = (const char *)pgl_glGetString(PGGL_EXT_STR);
                if (s) have_ext = strstr(s, "GL_ARB_shader_image_load_store") != NULL;
            }
            if (have_ext && !pgl_glBindImageTexture) have_ext = 0;
            /* t_6b81015d: the live-image reads need an ordering primitive.
               Reading texels written by an earlier draw without a barrier is
               undefined (GL 4.5 §9.3), so a context that exposes image
               load/store but no barrier keeps the well-defined per-draw copy
               path instead of risking driver-dependent panel corruption. */
            if (have_ext && !pgl_glTextureBarrier && !pgl_glMemoryBarrier) {
                have_ext = 0;
                fprintf(stderr, "GL backend: image load/store without "
                        "glTextureBarrier/glMemoryBarrier -> framebuffer "
                        "fetch off (per-draw FBO copies)\n");
            }
            if (!have_ext)
                fprintf(stderr, "GL backend: image load/store unavailable -> "
                        "framebuffer fetch off (per-draw FBO copies)\n");
        }
        pgl_fbfetch = want && !legacy && have_ext;
    }
    fprintf(stderr, "GL backend: framebuffer fetch (imageLoad) = %s\n",
            pgl_fbfetch ? "on" : "off (per-draw FBO copies)");
    /* ---- t_a22413f1 run3 / t_d8e5ab8c: DEPTH PATH SELECTION -------------
       The D32F depth attachment + gl_FragDepth is the CORRECT path (the PICA
       depth test runs per fragment in raster order, so overlapping triangles
       of ONE draw test against each other) and since t_d8e5ab8c it renders
       pixel-identical to the software rasterizer on the operator's Radeon
       (DX 580/570, radeonsi).  It is the DEFAULT; it is still gated on the
       per-driver self-test below and on the main FBO coming back COMPLETE, and
       a failing driver falls back to IN-SHADER automatically with one startup
       line.  DQ8_GL_DEPTH_ATTACH=0 forces the in-shader path (the pre-fix
       behaviour, kept for A/B). */
    {
        const char *e = getenv("DQ8_GL_DEPTH_ATTACH");
        const char *af = getenv("DQ8_GL_DEPTH_AUTOFALLBACK");
        /* t_599a43f7: the automatic switch to the in-shader path is ON by
           default.  A black 3D frame is never an acceptable outcome, and the
           in-shader path always draws the scene; the only cost of a needless
           fallback is the model holes coming back.  DQ8_GL_DEPTH_AUTOFALLBACK=0
           restores the opt-in behaviour for A/B. */
        pgl_depth_autofallback = !(af && af[0] == '0' && !af[1]);
        pgl_depth_hw_env = (e && e[0]) ? (e[0] != '0') : 1;
        pgl_depth_hw = 0;                      /* before the first set */
        pgl_depth_hw_set(pgl_depth_hw_env,
                         "startup: DQ8_GL_DEPTH_ATTACH / default");
        {
            const char *cm = getenv("DQ8_GL_DEPTH_CLEARMASK");
            const char *pr = getenv("DQ8_GL_DEPTH_PROBE");
            pgl_depth_clearmask_fix = (cm && cm[0] == '0') ? 0 : 1;
            pgl_depth_probe_left = (pr && pr[0] && pr[0] != '0')
                                   ? (unsigned)atoi(pr) : 0u;
        }
        if (pgl_depth_hw && !pgl_depth_ok) {
            pgl_depth_hw_set(0, "glDepthFunc/glDepthMask unavailable");
            pgl_depth_off_why = "glDepthFunc/glDepthMask unavailable";
        } else if (!pgl_depth_hw) {
            pgl_depth_off_why = pgl_depth_hw_env
                ? "requested but disabled"
                : "DQ8_GL_DEPTH_ATTACH=0 (in-shader path forced)";
        }
    }
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
    pgl_glGenTextures(1, &pgl_dtex);   /* t_a22413f1: real depth attachment */
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
            /* t_9651f10a: fixed-function blending is DEFAULT ON
               (t_77ac52ce removed the per-draw u_dst copy and its pipeline
               drain; t_d559a510 made the per-buffer depth REPLACE guard keep
               the emulated depth attachment out of the fixed-function blend).
               The PICA blend configurations DQ8 issues (factors 0..14/eq 0..4)
               have exact GL equivalents and the pixel result matches the
               shader (software) blend to <=1 LSB of RGBA8 rounding, so the
               gate is an OPT-OUT: any explicit DQ8_GL_BLENDS=0 restores the
               shader blend path (A/B). */
            const char *bl = getenv("DQ8_GL_BLENDS");
            pgl_blend_gl_on = !(bl && bl[0] == '0');
            fprintf(stderr, "GL backend: fixed-function blending %s "
                    "(renderer: %s; DQ8_GL_BLENDS=0 for the shader blend path)\n",
                    pgl_blend_gl_on ? "ON (default)" : "off (DQ8_GL_BLENDS=0)",
                    pgl_renderer[0] ? pgl_renderer : "unknown");
            if (pgl_blend_gl_on)
                fprintf(stderr, "GL backend: fixed-function blend depth guard "
                        "= %s\n", pgl_perbuf_blend
                        ? "per-buffer blend state (COLOR_ATTACHMENT1 = replace)"
                        : "off (no glBlendFunci/glBlendEquationi -> shader "
                          "blend path: the depth attachment must not be "
                          "blended)");
            {
                const char *dg = getenv("DQ8_GL_BLENDDEPTH");
                if (dg && dg[0] == '0' && !dg[1]) {
                    pgl_blend_depth_guard = 0;
                    fprintf(stderr, "GL backend: DQ8_GL_BLENDDEPTH=0 — the "
                            "depth attachment is blended again (A/B of the "
                            "pre-fix bug)\n");
                }
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
    /* t_a22413f1 run3: validate the hardware depth path on THIS driver before
       any game draw uses it. */
    if (pgl_depth_hw) {
        /* t_599a43f7: BOTH stages.  Stage 1 is the driver-capability check in a
           minimal FBO; stage 2 is the PRODUCTION pipeline (three-attachment FBO,
           depth clear after a depth-write-off draw, mirror + D32F read-back,
           same-draw ordering).  Stage 1 passed on the operator's Radeon while
           the real path was black, so stage 2 is what makes the gate real. */
        int st1 = pgl_depth_selftest_run();
        int st2 = pgl_depth_selftest_pipeline();
        pgl_depth_selftest = (st1 == 1 && st2 == 1) ? 1 : -1;
        /* DQ8_GL_DEPTH_FORCEFAIL=1: exercise the automatic fallback on a
           healthy driver (the path the operator relies on if the D32F
           attachment ever breaks again). */
        {
            const char *ff = getenv("DQ8_GL_DEPTH_FORCEFAIL");
            if (ff && ff[0] && ff[0] != '0') pgl_depth_selftest = -1;
        }
        if (pgl_depth_selftest != 1)
            pgl_depth_fallback(st1 == 1
                ? "production-depth self-test failed on this driver"
                : "depth self-test failed on this driver");
    }
    fprintf(stderr, "GL backend: depth path = %s%s (DQ8_GL_DEPTH_ATTACH=%d)%s\n",
            pgl_depth_hw ? "HARDWARE D32F depth attachment"
                         : "IN-SHADER emulation (ATT1 mirror)",
            pgl_depth_hw ? " + gl_FragDepth" : "",
            pgl_depth_hw_env,
            pgl_depth_hw ? "" : "");
    if (!pgl_depth_hw)
        fprintf(stderr, "GL backend: depth path reason = %s \n",
                pgl_depth_off_why ? pgl_depth_off_why : "?");
    {   /* t_599a43f7: ONE line that answers the operator's report without a
           second round trip.  Printed on every driver, every run. */
        const char *ver = (const char *)pgl_glGetString(PGGL_VERSION);
        if (pgl_frame_guard_off < 0) {
            const char *g = getenv("DQ8_GL_FRAME_GUARD");
            pgl_frame_guard_off = (g && g[0] == '0' && !g[1]) ? 1 : 0;
        }
        fprintf(stderr, "GL backend: DEPTH DIAG renderer=\"%s\" gl_version=\"%s\" "
                        "fbo_layout=%s att0=RGBA8 att1=R32F depth=%s "
                        "hw_requested=%d hw_active=%d selftest=%d "
                        "fbo_builds=%u clearmask_fix=%d guard=%s lit_threshold=%d%% "
                        "autofallback=%d split_gl=%d zdepth_inject=%d\n",
                pgl_renderer[0] ? pgl_renderer : "?",
                ver ? ver : "?",
                pgl_depth_hw ? "RGBA8+R32F+D32F" : "RGBA8+R32F",
                pgl_depth_hw ? "D32F" : "none",
                pgl_depth_hw_env, pgl_depth_hw, pgl_depth_selftest,
                pgl_depth_fbo_builds, pgl_depth_clearmask_fix,
                pgl_frame_guard_off ? "off (DQ8_GL_FRAME_GUARD=0)" : "armed",
                pgl_frame_guard_lit > 0 ? pgl_frame_guard_lit : 5,
                pgl_depth_autofallback, pgl_split_on, pgl_zinject_near());
    }
    /* Fresh context: every cached GL state we track is unknown. */
    pgl_depth_test_applied = -1;
    pgl_depth_mask_applied = -1;
    pgl_depth_hw_applied = -1;
    /* t_f8f2c1da: the depth path is now FINAL for this process.  From here on
       any change goes through pgl_depth_hw_set(), which drops the specialised
       cache in the same step, and pgl_spec_select() may generate programs. */
    pgl_depth_mode_ready = 1;
    pgl_spec[0].depth_hw = -1;   /* generic program: no baked path */
    {
        const char *m = getenv("DQ8_GL_SPEC_MODECHECK");
        if (m && m[0])
            fprintf(stderr, "GL backend: SPEC MODECHECK armed (%d)%s\n", atoi(m),
                    atoi(m) >= 2 ? " — violation aborts" : "");
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
    if (!env || !env[0] || env[0] == '0') {
        /* t_ab8159de: the software rasteriser has no supersampled path — say so
           once instead of letting the knob look like it did something. */
        int f = pgl_scale_value();
        if (f > 1)
            fprintf(stderr, "software rasterizer: DQ8_RENDER_SCALE=%d ignored — "
                            "the software path always renders at the native "
                            "guest resolution (DQ8_GL=1 enables the internal "
                            "render scale)\n", f);
        return 0;
    }
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
        const char *batch = getenv("DQ8_GL_BATCH");
        if (batch && batch[0] == '0') {
            /* DQ8_GL_BATCH=0 restores per-draw write-back semantics; the
               split path is batched by construction — do not silently mix. */
            fprintf(stderr, "GL backend: DQ8_GL_BATCH=0 needs the synchronous "
                    "path; ignoring DQ8_SPLIT_GL\n");
        } else if (pgl_split_start()) {
            atexit(pica_gl_shutdown);
            return pgl_ready;
        } else {
            fprintf(stderr, "GL backend: worker-thread split failed to start; "
                    "continuing on the synchronous path\n");
        }
    }
    if (!pgl_bringup()) return 0;
    atexit(pica_gl_shutdown);
    return 1;
}

static int pica_gl_active(void) { return pgl_ready; }
static unsigned long long pica_gl_draw_count(void) { return pgl_draws; }
static const char *pica_gl_renderer(void) { return pgl_renderer; }
/* t_5ddc492f test hook (tools/gl_spec_test.c): force the fragment-shader
   specialisation on/off so one process can A/B the same draw state.  The
   game never calls it (the env knob decides). */
static void pica_gl_spec_set(int on) { pgl_spec_on = on ? 1 : 0; }

static void pica_gl_shutdown(void) {
    unsigned i;
    /* t_05281b89: stage A coverage — how much of the scene this scale
       actually buys.  Printed for every scale (at 1x it reports 0 scaled). */
    pgl_cov_report();
    /* t_f8f2c1da: the stale-specialised-program tripwire.  A clean run prints
       violations=0 and (outside the replay harness) depth_mode_changes=0. */
    fprintf(stderr, "GL SPEC MODECHECK: violations=%llu depth_mode_changes=%u "
            "baked_variants=%d depth_path=%s\n",
            pgl_spec_modecheck_n, pgl_depth_mode_changes,
            pgl_nspec > 1 ? pgl_nspec - 1 : 0,
            pgl_depth_hw ? "hardware-d32f" : "in-shader");
    if (pgl_clear_pins || pgl_clear_would_clobber)
        fprintf(stderr, "GL clear pin: colour-only clears pinned=%llu "
                "would_clobber_att1=%llu cmask_depth_draws=%llu\n",
                pgl_clear_pins, pgl_clear_would_clobber, pgl_draws_cmask_depth);
    /* t_599a43f7: the black-frame guard's verdict for this run.  A run that
       needed a fallback says so here with the reason; depth_fallbacks follows. */
    fprintf(stderr, "GL DEPTH GUARD: frames=%u best_lit=%u.%u%% dark_flat=%u "
            "dark_draws=%u armed=%d probes=%u clear_checks=%u clear_bad=%u "
            "fallbacks=%u reason=\"%s\" depth_path=%s\n",
            pgl_frame_no, pgl_lit_best_pmille / 10u, pgl_lit_best_pmille % 10u,
            pgl_dark_frames, pgl_dark_draws, pgl_hw_bright_frames,
            pgl_frame_guard_probes, pgl_dclear_checks, pgl_dclear_bad,
            pgl_depth_fallback_count,
            pgl_depth_hw ? "-" : (pgl_depth_off_why ? pgl_depth_off_why : "?"),
            pgl_depth_hw ? "hardware-d32f" : "in-shader");
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
    /* t_dd830a74 item 3/4: the download boundary, measured on its own. */
    fprintf(stderr, "GL backend: download boundary (scaled -> native guest "
            "bytes, the only downsample in the pipeline): %llu flushes, "
            "%.1f ms total, %.0f us each%s\n",
            pgl_dl_calls, (double)pgl_dl_ns / 1e6,
            pgl_dl_calls ? (double)pgl_dl_ns / 1e3 / (double)pgl_dl_calls : 0.0,
            pgl_dl_calls ? "" : " (no readback this run)");
    /* t_fdd26eba item A: the display transfer at the internal scale. */
    fprintf(stderr, "GL backend: display transfer at scale: %llu taken "
            "(%.1f ms total, %.2f ms each), %llu fell back to native "
            "(%llu gate, %llu allocation/GL error)\n",
            pgl_tr_taken, (double)pgl_tr_ns / 1e6,
            pgl_tr_taken ? (double)pgl_tr_ns / 1e6 / (double)pgl_tr_taken : 0.0,
            pgl_tr_fallback, pgl_tr_fallback_gate, pgl_tr_fallback_mem);
    fprintf(stderr, "GL context hold: releases=%llu mid-hold retakes=%llu\n",
            pgl_hold_releases, pgl_hold_mid);
    fprintf(stderr, "GL texture uploads: count=%llu bytes=%llu "
            "cache_hits=%llu cache_misses=%llu\n",
            pgl_tex_uploads, pgl_tex_upload_bytes, pgl_tex_hits, pgl_tex_misses);
    fprintf(stderr, "GL texture uploads detail: generation_hits=%llu "
            "subimage_updates=%llu slots=%d\n",
            pgl_tex_gen_hits, pgl_tex_subuploads, PGL_UP_SLOTS);
    fprintf(stderr, "GL texture uploads unit1: count=%llu slots=%d\n",
            pgl_tex1_uploads, PGL_TEX1_SLOTS);
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
        pgl_hold_owner_tid = (long)syscall(SYS_gettid);
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
    pgl_hold_owner_tid = 0;
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
    if (!pgl_glIsProgram(pgl_program)) {
        int si;
        pgl_program = 0;
        lost |= 1u;
        /* t_5ddc492f: the specialised programs went with the same context
           loss; drop them and let the draw path regenerate on demand. */
        for (si = 1; si < pgl_nspec; ++si) pgl_spec[si].key = 0;
        pgl_nspec = 1;
        pgl_prog_cur = 0;
    }
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
        /* t_e216fc37: same for the unit-1 staged textures. */
        for (i = 0; i < PGL_TEX1_SLOTS; ++i) {
            if (!pgl_tex1[i].used || !pgl_tex1[i].tex ||
                pgl_glIsTexture(pgl_tex1[i].tex))
                continue;
            pgl_tex1[i].tex = 0;
            pgl_tex1[i].used = 0;
            pgl_tex1[i].ptr = NULL;
            pgl_tex1[i].gen = 0;
            lost |= 4096u;
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
    if ((used & 512u) && !pgl_glIsTexture(pgl_dtex)) {
        pgl_glGenTextures(1, &pgl_dtex);
        lost |= 512u;
    }
    if (lost & (8u | 16u | 128u | 512u)) {
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

/* t_a22413f1 run3: leave the hardware depth path at runtime.  The operator's
   Radeon rendered an almost fully black screen with the D32F depth attachment;
   nothing here can be debugged from llvmpipe, so the backend must never be able
   to present that state: on a failed self-test, an incomplete FBO or a run of
   black frames the depth attachment is detached and the shader resumes the
   in-shader test (the pre-fix behaviour, which at least draws the scene). */
static void pgl_depth_fallback(const char *why) {
    if (!pgl_depth_hw) return;
    pgl_depth_hw_set(0, why);      /* invalidates the specialised cache */
    pgl_depth_off_why = why;
    ++pgl_depth_fallback_count;
    if (pgl_fbo != 0 && pgl_rt_tex != 0) {
        PGenum st;
        pgl_glBindFramebuffer(PGGL_FRAMEBUFFER, pgl_fbo);
        pgl_obj_used |= 8u;
        pgl_glFramebufferTexture2D(PGGL_FRAMEBUFFER, PGGL_DEPTH_ATTACHMENT,
                                   PGGL_TEXTURE_2D, 0, 0);
        st = pgl_glCheckFramebufferStatus(PGGL_FRAMEBUFFER);
        fprintf(stderr, "GL backend: DEPTH PATH FALLBACK -> IN-SHADER "
                        "(%s); FBO status 0x%04X (complete=0x%04X)\n",
                why, (unsigned)st, (unsigned)PGGL_FRAMEBUFFER_COMPLETE);
    } else {
        fprintf(stderr, "GL backend: DEPTH PATH FALLBACK -> IN-SHADER (%s)\n", why);
    }
    /* The applied state is unknown now: re-apply on the next draw. */
    pgl_depth_test_applied = -1;
    pgl_depth_mask_applied = -1;
    pgl_depth_hw_applied = -1;
    /* The specialised shaders baked the hardware depth path into their source,
       so they cannot serve draws any more: pgl_depth_hw_set() already reset the
       cache (t_f8f2c1da — one code path, no way to forget it). */
    /* Guest depth memory may hold values written under the hardware path. */
    if (!pgl_split_cur) pgl_zforce_full = 1;
    pgl_drain_errors("depth-fallback", NULL);
}

/* Ensure FBO color attachment matches the render-target size.
   t_05281b89: the attachment is sized guest * pgl_cur_scale, where the scale
   is decided PER TARGET by pgl_scale_for() (stage A gating).  A target whose
   scale changes is re-specified and its guest content is force-re-uploaded:
   the flip only ever happens right after the episode was published, so guest
   memory holds exactly the native pixels the guest is about to sample. */
static int pgl_ensure_rt(int w, int h) {
    int f;
    if (pgl_fbo == 0 || w <= 0 || h <= 0) return 0;
    f = pgl_scale_for(w, h);
    if (pgl_rt_tex == 0 || pgl_rt_w != w || pgl_rt_h != h ||
        pgl_rt_scale != f) {
        PGenum fb_status;
        int scale_moved = (pgl_rt_scale != f && pgl_rt_tex != 0);
        pgl_rt_w = w;
        pgl_rt_h = h;
        pgl_rt_scale = f;
        pgl_cur_scale = f;
        pgl_tgt_w = PGL_S(w);
        pgl_tgt_h = PGL_S(h);
        pgl_cov_track(w, h);
        /* A scale change resizes the attachments: their content is gone, so
           the next refresh must re-upload the whole target from guest memory
           (which is native and current, because every publish downsamples). */
        if (scale_moved) {
            if (pgl_split_cur) pgl_request_resync();
            else { pgl_force_full = 1; pgl_zforce_full = 1; }
        }
        if (pgl_debug && scale_moved)
            fprintf(stderr, "GL backend: render scale: target %dx%d now x%d "
                            "(%dx%d texels), re-uploading from guest memory\n",
                    w, h, f, pgl_tgt_w, pgl_tgt_h);
        /* t_dd830a74: one line per SURFACE the first time it is built scaled
           (never one per FBO rebuild). */
        pgl_surface_created_scaled(w, h, f);
        pgl_glBindTexture(PGGL_TEXTURE_2D, pgl_rt_tex);
        pgl_obj_used |= 16u;
        pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_MIN_FILTER, PGGL_NEAREST);
        pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_MAG_FILTER, PGGL_NEAREST);
        pgl_glTexImage2D(PGGL_TEXTURE_2D, 0, PGGL_RGBA8, pgl_tgt_w, pgl_tgt_h, 0,
                         PGGL_RGBA, PGGL_UNSIGNED_BYTE, NULL);
        /* Depth attachment: R32F storing integer z values (exact).  This is the
           GUEST-VISIBLE depth mirror (t_a22413f1: it is no longer the source of
           the depth test — the real depth attachment below does that — but it
           stays the authoritative value carrier for the write-back because it
           is exactly what the software path stores). */
        pgl_glBindTexture(PGGL_TEXTURE_2D, pgl_z_tex);
        pgl_obj_used |= 128u;
        pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_MIN_FILTER, PGGL_NEAREST);
        pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_MAG_FILTER, PGGL_NEAREST);
        pgl_glTexImage2D(PGGL_TEXTURE_2D, 0, PGGL_R32F, pgl_tgt_w, pgl_tgt_h, 0,
                         PGGL_RED, PGGL_FLOAT, NULL);
        /* t_a22413f1: real depth attachment the rasterizer tests/writes, so
           fragments of ONE draw depth-test each other in raster order.  Only
           built in HARDWARE mode (run3: opt-in, off by default). */
        if (pgl_depth_hw) {
            pgl_glBindTexture(PGGL_TEXTURE_2D, pgl_dtex);
            pgl_obj_used |= 512u;
            pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_MIN_FILTER, PGGL_NEAREST);
            pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_MAG_FILTER, PGGL_NEAREST);
            pgl_glTexImage2D(PGGL_TEXTURE_2D, 0, PGGL_DEPTH_COMPONENT32F, pgl_tgt_w,
                             pgl_tgt_h, 0, PGGL_DEPTH_COMPONENT, PGGL_FLOAT, NULL);
            pgl_dt_w = pgl_tgt_w;
            pgl_dt_h = pgl_tgt_h;
        }
        pgl_glBindFramebuffer(PGGL_FRAMEBUFFER, pgl_fbo);
        pgl_obj_used |= 8u;
        pgl_glFramebufferTexture2D(PGGL_FRAMEBUFFER, PGGL_COLOR_ATTACHMENT0,
                                   PGGL_TEXTURE_2D, pgl_rt_tex, 0);
        pgl_glFramebufferTexture2D(PGGL_FRAMEBUFFER, PGGL_COLOR_ATTACHMENT1,
                                   PGGL_TEXTURE_2D, pgl_z_tex, 0);
        pgl_glFramebufferTexture2D(PGGL_FRAMEBUFFER, PGGL_DEPTH_ATTACHMENT,
                                   PGGL_TEXTURE_2D,
                                   pgl_depth_hw ? pgl_dtex : 0, 0);
        /* New depth attachment content is undefined: force a full re-upload. */
        pgl_depth_test_applied = -1;
        pgl_depth_mask_applied = -1;
        /* Under the split the GUEST thread mirrors the re-spec force flags
           (it owns pgl_zshadow); pgl_zforce_full is guest state there. */
        if (!pgl_split_cur) pgl_zforce_full = 1;
        fb_status = pgl_glCheckFramebufferStatus(PGGL_FRAMEBUFFER);
        /* t_a22413f1 run3: ALWAYS report the build (a diagnostic run needs the
           status of the working case, not only the failure). */
        ++pgl_depth_fbo_builds;
        fprintf(stderr, "GL backend: FBO build #%u %dx%d (guest %dx%d scale x%d) "
                        "depth_attach=%s "
                        "status=0x%04X (complete=0x%04X) draw=%llu\n",
                pgl_depth_fbo_builds, pgl_tgt_w, pgl_tgt_h, w, h, pgl_cur_scale,
                pgl_depth_hw ? "D32F" : "none",
                (unsigned)fb_status, (unsigned)PGGL_FRAMEBUFFER_COMPLETE,
                (unsigned long long)pgl_draws);
        if (fb_status != PGGL_FRAMEBUFFER_COMPLETE) {
            if (pgl_depth_hw) {
                /* Never run a black scene: drop the depth attachment and
                   retry the same build with the in-shader path. */
                pgl_depth_fallback("incomplete FBO with the D32F depth attachment");
                pgl_glFramebufferTexture2D(PGGL_FRAMEBUFFER, PGGL_DEPTH_ATTACHMENT,
                                           PGGL_TEXTURE_2D, 0, 0);
                fb_status = pgl_glCheckFramebufferStatus(PGGL_FRAMEBUFFER);
                fprintf(stderr, "GL backend: FBO re-check without depth "
                                "attachment: 0x%04X (complete=0x%04X)\n",
                        (unsigned)fb_status,
                        (unsigned)PGGL_FRAMEBUFFER_COMPLETE);
            }
            if (fb_status != PGGL_FRAMEBUFFER_COMPLETE) {
                fprintf(stderr, "GL backend: FBO incomplete at %dx%d (0x%04X)\n",
                        w, h, (unsigned)fb_status);
                return 0;
            }
        }
        if (pgl_debug)
            fprintf(stderr, "GL debug: FBO %u rt_tex %u %dx%d complete\n",
                    pgl_fbo, pgl_rt_tex, w, h);
        /* t_d8e5ab8c: a newly attached depth attachment has UNDEFINED content
           (in practice zeros on radeonsi), and the guest's own depth region is
           only uploaded once it writes it.  A draw in between would test
           against 0.0 and lose every fragment - the first frame of a run went
           black.  Start from "far", exactly like a cleared PICA depth buffer. */
        if (pgl_depth_hw) {
            /* t_8510204b: clear to PICA's FAR value, not GL's 1.0.  The guest
               clears its depth region to 0xFFFFFF, and a PICA fragment whose
               integer z is 0xFFFFFF must FAIL less/lequal against it (the
               in-shader path compares integers, so 0xFFFFFF < 0xFFFFFF is
               false).  Clearing the attachment to 1.0 left ONE ulp of headroom
               (0xFFFFFF/2^24 = 0.99999994 < 1.0), so every far-plane polygon —
               the sky/backdrop quads that cover the screen in battle, and the
               black occluders that write far depth — passed the hardware test
               and painted over the scene, while the in-shader path rejected
               them.  Clearing to the same 24-bit far keeps the two paths
               identical. */
            float fv = pgl_zfix_on() ? (float)(16777215.0 / 16777216.0) : 1.0f;
            pgl_depth_attachment_clear(fv);   /* t_599a43f7: one clear path */
            pgl_drain_errors("depth-init-clear", NULL);
        }
    }
    pgl_glBindFramebuffer(PGGL_FRAMEBUFFER, pgl_fbo);
    pgl_obj_used |= 8u;
    return 1;
}

/* ---- t_dd830a74 item 2: SAMPLE THE SCALED BACKING ------------------------
   The guest's post-process pass samples the very target the 3D pass rendered
   into.  Azahar samples the scaled texture directly (normalised UVs, no
   downsample, gl_texture_runtime.cpp's textures[1]); a GL shader cannot sample
   the texture it is rendering into (feedback loop, GL 4.5 §9.3), so this
   backend first copies the scaled backing into pgl_snap_tex and the draw binds
   THAT for unit 0.  The copy is at f x f: nothing is downsampled, so the
   composite is built from supersampled content.

   The guest-memory publish still happens at the call site (item 3), so every
   other consumer (display transfer, DMA, Y2R, savestate) is unchanged.
   Returns 1 when unit 0 must be bound to pgl_snap_tex, 0 to fall back to the
   native staged path. */
static PGuint pgl_snap_tex;
static PGuint pgl_snap_fbo;   /* destination of the snapshot blit */
static int pgl_snap_w, pgl_snap_h;

/* ---- t_dd830a74 item 2 (round 3): ONE PLACEMENT TRANSFORM FOR BOTH PATHS --
   The native path does not "place" anything: pica_vs.c's
   pica_gl_stage_texture decodes the WHOLE DECLARED texture out of guest
   memory, so a target that the guest samples through a larger declared
   texture (DQ8: the 512x400 field target, declared 512x512 at the same base
   address) lands at rows [decl_h - tgt_h, decl_h) of the staged image with the
   target's FBO row k at row k of that image.  That is MEASURED, not assumed —
   tools/dt_stage.py against the GL target reports "identity, yoff=112,
   mean|d| 3.1", i.e. no x flip, no y flip, offset = decl_h - tgt_h.

   The snapshot must reproduce exactly that at scale.  So there is one
   transform, expressed in NATIVE units here and multiplied by f at the single
   call site (glBlitFramebuffer), and the log line below prints both vectors so
   the sharing is visible rather than claimed.  Round 2's copy had a y flip
   here (`dst_lo..dst_hi` reversed); that flip is the whole difference between
   the snapshot path and the native one. */
static void pgl_snap_place_native(int decl_w, int decl_h, int tgt_nw, int tgt_nh,
                                  int *xoff, int *yoff, int *w, int *h)
{
    (void)decl_w;
    *xoff = 0;                       /* target column 0 = texture column 0 */
    *yoff = decl_h - tgt_nh;         /* bottom-aligned inside the declared height */
    if (*yoff < 0) *yoff = 0;
    *w = tgt_nw;
    *h = tgt_nh;
}

/* Printed once: the same transform at 1x and at f.  The f x row is exactly f
   times the native row (offsets and extents), which is the property the
   composite's window depends on. */
static int pgl_snap_place_logged;

/* Diagnosis only (DQ8_RS_SNAP_DUMP=<dir>, default off): write the first four
   snapshots AND the target they were blitted from as raw RGBA, plus a dims
   line, so the x/y convention of the copy is MEASURED instead of guessed
   (tools/dt_snap.py).  Both files come from glReadPixels, so they share one
   row order (GL: row 0 = bottom); only the blit's own transform is left. */
static void pgl_snap_diag(int tw, int th) {
    const char *dd = getenv("DQ8_RS_SNAP_DUMP");
    static unsigned n;
    char path[768];
    FILE *f;
    unsigned char *buf;
    unsigned long long len;
    if (!dd || !dd[0] || n >= 8u) return;
    ++n;
    len = (unsigned long long)pgl_snap_w * (unsigned long long)pgl_snap_h * 4ull;
    buf = (unsigned char *)malloc((size_t)len ? (size_t)len : 1u);
    if (buf) {
        pgl_glBindFramebuffer(PGGL_READ_FRAMEBUFFER, pgl_snap_fbo);
        pgl_glReadPixels(0, 0, pgl_snap_w, pgl_snap_h, PGGL_RGBA,
                         PGGL_UNSIGNED_BYTE, buf);
        snprintf(path, sizeof path, "%s/snap_%u.raw", dd, n);
        f = fopen(path, "wb");
        if (f) { fwrite(buf, 1, (size_t)len, f); fclose(f); }
        free(buf);
    }
    len = (unsigned long long)pgl_tgt_w * (unsigned long long)pgl_tgt_h * 4ull;
    buf = (unsigned char *)malloc((size_t)len ? (size_t)len : 1u);
    if (buf) {
        pgl_glBindFramebuffer(PGGL_READ_FRAMEBUFFER, pgl_fbo);
        pgl_glReadBuffer(PGGL_COLOR_ATTACHMENT0);
        pgl_glReadPixels(0, 0, pgl_tgt_w, pgl_tgt_h, PGGL_RGBA,
                         PGGL_UNSIGNED_BYTE, buf);
        snprintf(path, sizeof path, "%s/fbo_%u.raw", dd, n);
        f = fopen(path, "wb");
        if (f) { fwrite(buf, 1, (size_t)len, f); fclose(f); }
        free(buf);
    }
    snprintf(path, sizeof path, "%s/dims_%u.txt", dd, n);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "snap %d %d tgt %d %d declared %dx%d scale %d\n",
                pgl_snap_w, pgl_snap_h, pgl_tgt_w, pgl_tgt_h, tw, th,
                pgl_cur_scale);
        fclose(f);
    }
    pgl_glBindFramebuffer(PGGL_FRAMEBUFFER, pgl_fbo);
    fprintf(stderr, "GL diag: snapshot %u (%dx%d) + target (%dx%d) written to %s\n",
            n, pgl_snap_w, pgl_snap_h, pgl_tgt_w, pgl_tgt_h, dd);
}

/* Diagnosis only (DQ8_RS_SNAP_DUMP=<dir>): the same measurement for the
   NATIVE path — dump the staged RGBA image the draw is about to upload for a
   draw that samples the current target, plus the target as GL sees it, so
   "staged image == target" can be compared instead of assumed.  Armed by the
   alias site in pica_vs.c (pgl_diag_stage_want). */
static int pgl_diag_stage_want;

/* Diagnosis only: the target as GL leaves it right AFTER the draw that
   sampled it — the "composite output" side of the input/output pair. */
static int pgl_diag_post_want;

static void pgl_diag_post_dump(void) {
    const char *dd = getenv("DQ8_RS_SNAP_DUMP");
    static unsigned n;
    char path[768];
    FILE *f;
    unsigned char *buf;
    unsigned long long len;
    if (!dd || !dd[0] || n >= 8u) return;
    ++n;
    len = (unsigned long long)pgl_tgt_w * (unsigned long long)pgl_tgt_h * 4ull;
    buf = (unsigned char *)malloc((size_t)len ? (size_t)len : 1u);
    if (!buf) return;
    pgl_glBindFramebuffer(PGGL_READ_FRAMEBUFFER, pgl_fbo);
    pgl_glReadBuffer(PGGL_COLOR_ATTACHMENT0);
    pgl_glReadPixels(0, 0, pgl_tgt_w, pgl_tgt_h, PGGL_RGBA,
                     PGGL_UNSIGNED_BYTE, buf);
    snprintf(path, sizeof path, "%s/post_%u.raw", dd, n);
    f = fopen(path, "wb");
    if (f) { fwrite(buf, 1, (size_t)len, f); fclose(f); }
    free(buf);
    pgl_glBindFramebuffer(PGGL_FRAMEBUFFER, pgl_fbo);
    fprintf(stderr, "GL diag: post-draw target %u (%dx%d) written to %s\n",
            n, pgl_tgt_w, pgl_tgt_h, dd);
}


static void pgl_diag_stage_dump(const unsigned char *rgba, int w, int h) {
    const char *dd = getenv("DQ8_RS_SNAP_DUMP");
    static unsigned n;
    char path[768];
    FILE *f;
    unsigned char *buf;
    unsigned long long len;
    if (!dd || !dd[0] || !rgba || n >= 8u) return;
    ++n;
    snprintf(path, sizeof path, "%s/staged_%u.raw", dd, n);
    f = fopen(path, "wb");
    if (f) { fwrite(rgba, 1, (size_t)w * (size_t)h * 4u, f); fclose(f); }
    len = (unsigned long long)pgl_tgt_w * (unsigned long long)pgl_tgt_h * 4ull;
    buf = (unsigned char *)malloc((size_t)len ? (size_t)len : 1u);
    if (buf) {
        pgl_glBindFramebuffer(PGGL_READ_FRAMEBUFFER, pgl_fbo);
        pgl_glReadBuffer(PGGL_COLOR_ATTACHMENT0);
        pgl_glReadPixels(0, 0, pgl_tgt_w, pgl_tgt_h, PGGL_RGBA,
                         PGGL_UNSIGNED_BYTE, buf);
        snprintf(path, sizeof path, "%s/sfbo_%u.raw", dd, n);
        f = fopen(path, "wb");
        if (f) { fwrite(buf, 1, (size_t)len, f); fclose(f); }
        free(buf);
    }
    snprintf(path, sizeof path, "%s/sdims_%u.txt", dd, n);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "staged %d %d tgt %d %d scale %d\n",
                w, h, pgl_tgt_w, pgl_tgt_h, pgl_cur_scale);
        fclose(f);
    }
    pgl_glBindFramebuffer(PGGL_FRAMEBUFFER, pgl_fbo);
    fprintf(stderr, "GL diag: staged image %u (%dx%d) + target (%dx%d) written to %s\n",
            n, w, h, pgl_tgt_w, pgl_tgt_h, dd);
}

int pica_gl_sample_scaled_target(unsigned tw, unsigned th, unsigned tfmt,
                                 unsigned taddr) {
    static int on = -1;
    if (on < 0) {
        /* t_dd830a74 item 2: DEFAULT ON since round 3.  The placement fix
           below made the composite pixel-identical to the 1x reference
           (tools/dt_orient.py: identity mean|d| 1.65, the stage-A AA residual,
           against 16.11 for mirror-x), so sampling the scaled backing is the
           normal path.  DQ8_RS_SAMPLE_SCALED=0 restores the stage-A behaviour
           (sample the native downloaded image) for A/B runs. */
        const char *e = getenv("DQ8_RS_SAMPLE_SCALED");
        on = (e && e[0] == '0' && !e[1]) ? 0 : 1;
        if (on) fprintf(stderr, "GL backend: render scale: a render target the "
                       "guest samples is served from its scaled backing "
                       "(Azahar textures[1]; DQ8_RS_SAMPLE_SCALED=0 disables)\n");
        else fprintf(stderr, "GL backend: render scale: scaled sample DISABLED "
                        "(DQ8_RS_SAMPLE_SCALED=0) — targets are still scaled, "
                        "samples use the native download\n");
    }
    if (!on) return 0;
    if (!pgl_ready) return 0;
    if (!pgl_scale_sampled() || pgl_cur_scale <= 1) return 0;
    /* DQ8_SPLIT_GL runs the GL calls on a worker context; the snapshot would
       have to be created there.  Until that is plumbed through, the split
       keeps the native staged path (item 3's download). */
    if (pgl_split_on) { ++pgl_snap_fallback; return 0; }
    /* Exact alias (the sampled texture IS the current target), plus the common
       PICA shape where the guest declares a texture at least as large as the
       target it was rendered into (DQ8: the 512x400 field target is sampled as
       a 512x512 texture at the same address).  The sampled texture keeps its
       DECLARED size — the UVs are unchanged — while the content inside the
       target's own rect comes from the scaled backing.  Texture rows past the
       target's height are not defined by the guest render either; the snapshot
       leaves them untouched (documented in reports/RENDER_SCALE_AZAHAR.md). */
    if ((int)tw < pgl_rt_w || (int)th < pgl_rt_h ||
        taddr != pica_rt_addr || tfmt != pica_rt_format ||
        pica_rt_format != 0u /* RGBA8 */ || pgl_rt_tex == 0) {
        ++pgl_snap_fallback;
        if (pgl_snap_fallback <= 8u)
            fprintf(stderr, "GL backend: render scale: scaled sample not taken "
                            "(tex %ux%u fmt=%u addr=%08X vs target %dx%d fmt=%u "
                            "addr=%08X tex=%u scaled=%d)\n",
                    tw, th, tfmt, taddr, pgl_rt_w, pgl_rt_h, pica_rt_format,
                    pica_rt_addr, (unsigned)pgl_rt_tex, pgl_cur_scale);
        return 0;
    }
    if (!pgl_ensure_rt(pgl_rt_w, pgl_rt_h)) {
        ++pgl_snap_fallback;
        fprintf(stderr, "GL backend: render scale: scaled sample fallback (ensure_rt)\n");
        return 0;
    }
    if (pgl_cur_scale <= 1) {
        ++pgl_snap_fallback;
        fprintf(stderr, "GL backend: render scale: scaled sample fallback (cur_scale=%d)\n", pgl_cur_scale);
        return 0;
    }
    if (!pgl_snap_tex || !pgl_glIsTexture(pgl_snap_tex)) {
        pgl_snap_tex = 0;
        pgl_glGenTextures(1, &pgl_snap_tex);
        pgl_snap_w = pgl_snap_h = 0;
        if (!pgl_snap_tex) {
            ++pgl_snap_fallback;
            fprintf(stderr, "GL backend: render scale: scaled sample fallback (glGenTextures)\n");
            return 0;
        }
    }
    /* The FBO's read buffer is ATT0 (the colour attachment) — same source the
       write-back reads. */
    pgl_glBindFramebuffer(PGGL_FRAMEBUFFER, pgl_fbo);
    pgl_obj_used |= 8u;
    pgl_glReadBuffer(PGGL_COLOR_ATTACHMENT0);
    /* The guest's PICA sampler flips t (pica_vs.c: ty = h-1-wrap(t)), so the
       staged image the native path would build has memory row m at staged row
       (texture_h - 1 - m): the target's rect lands in the LAST tgt_h rows of
       the declared texture, mirrored vertically.  DQ8's post-process confirms
       it — it samples v in [0.21875 .. 1.0] = exactly rows (th-tgt_h)..th-1 of
       a 512x512 texture over a 512x400 target.  Reproduce that placement with
       one GPU blit: scaled f x f, no downsample. */
    if (pgl_snap_w != (int)tw * pgl_cur_scale ||
        pgl_snap_h != (int)th * pgl_cur_scale) {
        pgl_glActiveTexture(PGGL_TEXTURE0);
        pgl_glBindTexture(PGGL_TEXTURE_2D, pgl_snap_tex);
        pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_MIN_FILTER, PGGL_NEAREST);
        pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_MAG_FILTER, PGGL_NEAREST);
        pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_WRAP_S, PGGL_CLAMP_TO_EDGE);
        pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_WRAP_T, PGGL_CLAMP_TO_EDGE);
        pgl_glTexImage2D(PGGL_TEXTURE_2D, 0, PGGL_RGBA8, (PGsizei)tw * pgl_cur_scale,
                         (PGsizei)th * pgl_cur_scale, 0, PGGL_RGBA,
                         PGGL_UNSIGNED_BYTE, NULL);
        pgl_snap_w = (int)tw * pgl_cur_scale;
        pgl_snap_h = (int)th * pgl_cur_scale;
    }
    if (!pgl_glBlitFramebuffer || !pgl_snap_tex) { ++pgl_snap_fallback; return 0; }
    if (!pgl_snap_fbo || !pgl_glIsFramebuffer(pgl_snap_fbo)) {
        pgl_snap_fbo = 0;
        pgl_glGenFramebuffers(1, &pgl_snap_fbo);
        if (!pgl_snap_fbo) { ++pgl_snap_fallback; return 0; }
        pgl_glBindFramebuffer(PGGL_FRAMEBUFFER, pgl_snap_fbo);
        pgl_glFramebufferTexture2D(PGGL_FRAMEBUFFER, PGGL_COLOR_ATTACHMENT0,
                                   PGGL_TEXTURE_2D, pgl_snap_tex, 0);
        {   PGenum att = PGGL_COLOR_ATTACHMENT0;
            pgl_glDrawBuffers(1, &att); }
    } else {
        pgl_glBindFramebuffer(PGGL_FRAMEBUFFER, pgl_snap_fbo);
        pgl_glFramebufferTexture2D(PGGL_FRAMEBUFFER, PGGL_COLOR_ATTACHMENT0,
                                   PGGL_TEXTURE_2D, pgl_snap_tex, 0);
    }
    {
        /* ONE transform, native units, multiplied by f — the same placement the
           native stager produces (pgl_snap_place_native).  No flip: measurement
           B of round 2 (tools/dt_stage.py) says the native image holds the
           target identity + yoff=112, so a row-reversed copy is what made the
           composite differ. */
        int xo_n, yo_n, wn, hn, f = pgl_cur_scale;
        pgl_snap_place_native((int)tw, (int)th, pgl_tgt_w / f, pgl_tgt_h / f,
                              &xo_n, &yo_n, &wn, &hn);
        if (!pgl_snap_place_logged) {
            pgl_snap_place_logged = 1;
            fprintf(stderr,
                    "GL backend: render scale: target-sample placement — "
                    "declared %dx%d, target %dx%d: native off=(%d,%d) extent "
                    "%dx%d flip=(0,0); x%d off=(%d,%d) extent %dx%d = native*%d "
                    "(shared stager transform: bottom-aligned, no flip)\n",
                    (int)tw, (int)th, pgl_tgt_w / f, pgl_tgt_h / f,
                    xo_n, yo_n, wn, hn, f,
                    xo_n * f, yo_n * f, wn * f, hn * f, f);
        }
        /* Source = the live target, GL rows 0..th_s-1 (GL row j = guest
           framebuffer row j).  Destination = the same rows at f x, offset and
           extent multiplied by f, so normalised guest UVs sample the same
           content at every scale. */
        pgl_glBindFramebuffer(PGGL_READ_FRAMEBUFFER, pgl_fbo);
        pgl_glReadBuffer(PGGL_COLOR_ATTACHMENT0);
        pgl_glBindFramebuffer(PGGL_DRAW_FRAMEBUFFER, pgl_snap_fbo);
        pgl_glBlitFramebuffer(0, 0, wn * f, hn * f,
                              xo_n * f, yo_n * f, (xo_n + wn) * f,
                              (yo_n + hn) * f,
                              PGGL_COLOR_BUFFER_BIT, PGGL_NEAREST);
        pgl_obj_used |= 8u;
        pgl_glBindFramebuffer(PGGL_FRAMEBUFFER, pgl_fbo);
    }
    if (pgl_drain_errors("scaled-sample-snapshot", NULL) != 0) {
        ++pgl_snap_fallback;
        fprintf(stderr, "GL backend: render scale: scaled sample fallback (GL error during snapshot %dx%d)\n",
                pgl_tgt_w, pgl_tgt_h);
        return 0;
    }
    ++pgl_snap_taken;
    if (pgl_snap_taken <= 8u) pgl_snap_diag((int)tw, (int)th);
    /* t_dd830a74 control experiment (DQ8_RS_SNAP_ONLY=1): take the snapshot
       but still hand the draw the native staged image, so "the copy's side
       effects" and "the bound texture" can be told apart. */
    {
        const char *so = getenv("DQ8_RS_SNAP_ONLY");
        if (so && so[0] == '1' && !so[1]) {
            if (pgl_snap_taken <= 8u)
                fprintf(stderr, "GL backend: render scale: snapshot #%llu taken, "
                                "draw keeps the native staged image "
                                "(DQ8_RS_SNAP_ONLY)\n",
                        pgl_snap_taken);
            return 0;
        }
    }
    if (pgl_snap_taken <= 8u)
        fprintf(stderr, "GL backend: render scale: target %ux%u sampled as a "
                        "texture at x%d — bound the %dx%d scaled backing "
                        "(snapshot #%llu, no downsample)\n",
                tw, th, pgl_cur_scale, pgl_tgt_w, pgl_tgt_h, pgl_snap_taken);
    return 1;
}

/* ============ t_fdd26eba item A: DISPLAY TRANSFER INHERITS f ==============

   Azahar `rasterizer_cache/rasterizer_cache.h:289-352`
   (AccelerateDisplayTransfer) never gives the guest's display transfer a
   native-sized destination: the destination surface INHERITS the source's
   res_scale (`dst_params.res_scale = src_surface.res_scale`, line 324), its
   DECLARED width/height stay the guest's logical ones (halved by the scaling
   mode, lines 303-307), and the transfer itself is a GPU BlitTextures from the
   SCALED source rect into the SCALED destination rect (lines 343-347).  The
   destination therefore holds `declared * f` texels of real detail; the only
   thing that is ever downsampled is the download into guest memory.

   This function produces that scaled destination for one transfer:
     source   the attached colour backing of the current render target
              (GL row j == guest memory row pgl_tgt_h-1-j at scale 1, which is
              pgl_tgt_h-1-j*f .. in scaled texels), cropped to the guest's
              source rect (0,0,crop_w,crop_h) and taken at scale;
     blit     glBlitFramebuffer, GL_LINEAR on an axis whose rect shrinks
              (exactly Azahar's CopyTextures filter: a 2:1 axis averages the
              two scaled texels a destination texel covers);
     readback the scaled destination in GUEST MEMORY ROW ORDER (row 0 = the
              destination's first memory row), RGBA8, caller frees it.

   The guest's own bytes are NOT produced here: they still come from the
   download boundary (t_dd830a74 item 3, box-filtered), so every guest reader
   (DMA, Y2R, savestate, scan-out capture) keeps seeing native data.

   Returns 1 on success; every gate that cannot serve the transfer returns 0
   and is counted in pgl_tr_fallback together with the reason, so a run reports
   how many transfers kept the native-only path and why. */
static PGuint pgl_tr_tex, pgl_tr_fbo;
static int pgl_tr_w, pgl_tr_h;

static int pgl_tr_no(const char *why) {
    ++pgl_tr_fallback;
    ++pgl_tr_fallback_gate;
    if (pgl_tr_fallback <= 8u)
        fprintf(stderr, "GL backend: render scale: display transfer stays "
                        "NATIVE — %s (fallback #%llu)\n", why, pgl_tr_fallback);
    return 0;
}

static int pica_gl_transfer_scaled_capture(unsigned src_addr,
                                           unsigned src_w, unsigned src_h,
                                           unsigned crop_w, unsigned crop_h,
                                           unsigned dst_w, unsigned dst_h,
                                           int vflip, unsigned h_scale,
                                           unsigned v_scale,
                                           unsigned char **out, unsigned *ow,
                                           unsigned *oh, int *oscale) {
    const int f = pgl_cur_scale;
    int dw, dh, sy0, ratio, neary;
    unsigned long long t0 = 0;
    unsigned char *raw = NULL, *img = NULL;
    PglSavedCtx prev;
    unsigned y;
    if (out) *out = NULL;
    if (!out || !ow || !oh || !oscale) return 0;
    if (!pgl_ready) return pgl_tr_no("GL backend not active");
    if (pgl_split_on)
        return pgl_tr_no("DQ8_SPLIT_GL runs GL on the worker context");
    if (f <= 1) return pgl_tr_no("internal scale is 1 (nothing to inherit)");
    if (pgl_rt_tex == 0) return pgl_tr_no("no colour attachment yet");
    if (pgl_pending) {
        if (src_addr != pica_rt_addr || src_w != (unsigned)pgl_rt_w ||
            src_h != (unsigned)pgl_rt_h)
            return pgl_tr_no("the pending episode is a different target");
    } else {
        /* Published already: the attachment still holds the last episode's
           content, but only while nothing has drawn into it since. */
        if (!pgl_cap_valid || src_addr != pgl_cap_addr ||
            src_w != (unsigned)pgl_cap_w || src_h != (unsigned)pgl_cap_h ||
            f != pgl_cap_f || pgl_draws != pgl_cap_draws ||
            pgl_rt_w != pgl_cap_w || pgl_rt_h != pgl_cap_h)
            return pgl_tr_no("the attachment no longer holds the published "
                             "target's content");
    }
    if (!crop_w || !crop_h || crop_w > (unsigned)pgl_rt_w ||
        crop_h > (unsigned)pgl_rt_h)
        return pgl_tr_no("transfer crop lies outside the render target");
    if (dst_w != (crop_w >> h_scale) || dst_h != (crop_h >> v_scale))
        return pgl_tr_no("destination is not crop >> scaling mode");
    dw = (int)dst_w * f;
    dh = (int)dst_h * f;
    if (dw <= 0 || dh <= 0 || dw > 4096 || dh > 4096)
        return pgl_tr_no("scaled destination out of range");
    ratio = (int)(crop_w / dst_w);          /* 1 or 2 (scaling mode 0..2) */
    neary = (int)(crop_h / dst_h);
    if (ratio != (int)(1u << h_scale) || neary != (int)(1u << v_scale))
        return pgl_tr_no("scaling ratio is not 2^mode");
    if (!pgl_glBlitFramebuffer) return pgl_tr_no("glBlitFramebuffer unavailable");
    t0 = pica_pqt_now_ns();
    memset(&prev, 0, sizeof prev);
    if (!pgl_context_take(&prev))
        return pgl_tr_no("GL context could not be made current");
    if (!pgl_tr_tex || !pgl_glIsTexture(pgl_tr_tex)) {
        pgl_tr_tex = 0;
        pgl_glGenTextures(1, &pgl_tr_tex);
        pgl_tr_w = pgl_tr_h = 0;
    }
    if (!pgl_tr_fbo || !pgl_glIsFramebuffer(pgl_tr_fbo)) {
        pgl_tr_fbo = 0;
        pgl_glGenFramebuffers(1, &pgl_tr_fbo);
    }
    if (!pgl_tr_tex || !pgl_tr_fbo) {
        pgl_context_put(&prev);
        return pgl_tr_no("staging texture/framebuffer allocation failed");
    }
    pgl_glActiveTexture(PGGL_TEXTURE0);
    pgl_glBindTexture(PGGL_TEXTURE_2D, pgl_tr_tex);
    pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_MIN_FILTER, PGGL_NEAREST);
    pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_MAG_FILTER, PGGL_NEAREST);
    pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_WRAP_S, PGGL_CLAMP_TO_EDGE);
    pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_WRAP_T, PGGL_CLAMP_TO_EDGE);
    if (pgl_tr_w != dw || pgl_tr_h != dh) {
        pgl_glTexImage2D(PGGL_TEXTURE_2D, 0, PGGL_RGBA8, (PGsizei)dw,
                         (PGsizei)dh, 0, PGGL_RGBA, PGGL_UNSIGNED_BYTE, NULL);
        pgl_tr_w = dw;
        pgl_tr_h = dh;
    }
    pgl_glBindFramebuffer(PGGL_FRAMEBUFFER, pgl_tr_fbo);
    pgl_glFramebufferTexture2D(PGGL_FRAMEBUFFER, PGGL_COLOR_ATTACHMENT0,
                               PGGL_TEXTURE_2D, pgl_tr_tex, 0);
    {   PGenum att = PGGL_COLOR_ATTACHMENT0;
        pgl_glDrawBuffers(1, &att); }
    /* Guest memory rows [0,crop_h) of the target are the TOP crop_h*f GL rows
       of the attachment (GL row j == raster row j, memory row m == raster row
       rt_h-1-m), so the source rect starts at pgl_tgt_h - crop_h*f. */
    sy0 = pgl_tgt_h - (int)crop_h * f;
    pgl_glBindFramebuffer(PGGL_READ_FRAMEBUFFER, pgl_fbo);
    pgl_obj_used |= 8u;
    pgl_glReadBuffer(PGGL_COLOR_ATTACHMENT0);
    pgl_glBindFramebuffer(PGGL_DRAW_FRAMEBUFFER, pgl_tr_fbo);
    pgl_glBlitFramebuffer(0, sy0, (PGint)(crop_w * (unsigned)f),
                          sy0 + (int)crop_h * f, 0, 0, dw, dh,
                          PGGL_COLOR_BUFFER_BIT,
                          (ratio > 1 || neary > 1) ? PGGL_LINEAR : PGGL_NEAREST);
    pgl_glBindFramebuffer(PGGL_READ_FRAMEBUFFER, pgl_tr_fbo);
    pgl_glReadBuffer(PGGL_COLOR_ATTACHMENT0);
    raw = (unsigned char *)malloc((size_t)dw * (size_t)dh * 4u);
    if (raw)
        pgl_glReadPixels(0, 0, (PGsizei)dw, (PGsizei)dh, PGGL_RGBA,
                         PGGL_UNSIGNED_BYTE, raw);
    pgl_glBindFramebuffer(PGGL_FRAMEBUFFER, pgl_fbo);
    pgl_obj_used |= 8u;
    if (!raw || pgl_drain_errors("transfer-scaled-capture", NULL) != 0) {
        free(raw);
        pgl_context_put(&prev);
        pgl_tr_ns += pica_pqt_now_ns() - t0;
        ++pgl_tr_fallback;
        ++pgl_tr_fallback_mem;
        return 0;
    }
    pgl_context_put(&prev);
    pgl_tr_ns += pica_pqt_now_ns() - t0;
    /* glReadPixels returns bottom-up, i.e. destination memory row dh-1 first;
       reverse into memory order, and reverse the whole thing when the guest
       asked for flip_vertically (the native loop reads
       sy = input_h-1-(y<<v_scale), a mirror of the unflipped box average). */
    img = (unsigned char *)malloc((size_t)dw * (size_t)dh * 4u);
    if (!img) {
        free(raw);
        ++pgl_tr_fallback;
        ++pgl_tr_fallback_mem;
        return 0;
    }
    for (y = 0; y < (unsigned)dh; ++y) {
        const unsigned char *srcrow = vflip ? raw + (size_t)y * dw * 4u
                                            : raw + (size_t)(dh - 1 - (int)y) * dw * 4u;
        memcpy(img + (size_t)y * dw * 4u, srcrow, (size_t)dw * 4u);
    }
    free(raw);
    ++pgl_tr_taken;
    *out = img;
    *ow = (unsigned)dw;
    *oh = (unsigned)dh;
    *oscale = f;
    if (pgl_tr_taken <= 4u)
        fprintf(stderr, "GL backend: render scale: display transfer at x%d — "
                        "source %ux%u crop %ux%u -> scaled destination %dx%d "
                        "(GL_LINEAR %d:%d), #%llu\n",
                f, src_w, src_h, crop_w, crop_h, dw, dh, ratio, neary,
                pgl_tr_taken);
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
static int pgl_factor_gl(unsigned f, int is_src, PGenum *out) {
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
    case 14:
        /* t_d559a510: SRC_ALPHA_SATURATE.  pica_blend_factor(14) returns
           min(sa, 1-da) for RGB and 1.0 for alpha; GL_SRC_ALPHA_SATURATE is
           defined as exactly (min(As,1-Ad), min(As,1-Ad), min(As,1-Ad), 1),
           so it matches term for term — but ONLY as a source factor.  As a
           destination factor GL has no equivalent (the GL spec only allows it
           in the source slot), so those draws stay on the shader path. */
        if (!is_src) return 0;
        *out = PGGL_SRC_ALPHA_SATURATE;
        break;
    default: return 0; /* 15 (unused) */
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
    /* t_d559a510: a depth draw writes the emulated depth buffer in
       COLOR_ATTACHMENT1.  Fixed-function blending would blend frag_z against
       the old depth (GL applies the blend to every enabled draw buffer), which
       corrupts the depth test for every later draw.  With per-buffer blend
       state the depth buffer is switched back to a plain replace; without those
       entry points the draw must not take the fixed-function path at all.
       The shader writes frag_z to COLOR_ATTACHMENT1 on EVERY draw (0.0 when the
       depth buffer is off), so the replace is needed whenever blending is on,
       not only for depth draws. */
    if (pgl_blend_depth_guard && !pgl_perbuf_blend) return 0;
    if (!pgl_factor_gl(st->blend_src_rgb, 1, &f[0])) return 0;
    if (!pgl_factor_gl(st->blend_dst_rgb, 0, &f[1])) return 0;
    if (!pgl_factor_gl(st->blend_src_a, 1, &f[2])) return 0;
    if (!pgl_factor_gl(st->blend_dst_a, 0, &f[3])) return 0;
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

/* t_6b81015d: feedback-loop ordering for the framebuffer-fetch path.
   GL 4.5 core spec §9.3 ("Feedback Loops Between Textures and the
   Framebuffer"): a texel may be read and written by the SAME shader
   invocation (the panel's per-fragment read-modify-write is explicitly
   allowed), but "if a texel has been written, then in order to safely read
   the result a texel fetch must be in a subsequent draw call separated by
   void TextureBarrier(void)" — otherwise the read values are UNDEFINED.
   With DQ8_GL_FBFETCH the shader imageLoads the live render target, so every
   overlapping UI draw (panel fill, then its border/glyph quads) reads texels
   written by an earlier draw.  Unbarriered that is undefined: llvmpipe and
   softpipe rasterise strictly in order and return the right values, while a
   tiled/async driver may return pre-draw contents — which shows up as the
   background bleeding through the translucent panel in patches, with
   partial/stale UI fragments inside it, flipping frame to frame.  One barrier
   per dest-reading draw (only when a draw has written since the last one) is
   the minimum the spec requires.  DQ8_GL_TEXBARRIER=0 restores the
   unbarriered behaviour as an A/B. */
static void pgl_dst_barrier(int x0, int y0, int x1, int y1) {
    static int mode = -1;   /* 1 = texture barrier + box skip, 2 = image
                               barrier + box skip, 4 = texture barrier, no skip,
                               0 = no barrier at all */
    if (pgl_texbarrier_on < 0) {
        /* t_2d934e23: DEFAULT depends on the driver class.  The barrier only
           exists to make a read of the live render target well-defined
           (GL 4.5 §9.3): a tiled/async driver may return pre-draw contents.
           llvmpipe/softpipe/swrast rasterise strictly IN ORDER (verified in
           t_6b81015d: they return the right values unbarriered), so on those
           renderers the barrier buys nothing and costs a full pipeline flush
           per reading draw -- measured 3.03 -> 1.75 ms/draw of glDrawArrays
           time (llvmpipe), i.e. ~40% of the GL drain, because a flush also
           stops llvmpipe from batching consecutive draws.  Hardware drivers
           keep the barrier.  DQ8_GL_TEXBARRIER=1/2/4 force it (=4 also
           disables the box skip), =0 disables it everywhere (A/B). */
        const char *e = getenv("DQ8_GL_TEXBARRIER");
        int soft_inorder = pgl_renderer[0] &&
            (strstr(pgl_renderer, "llvmpipe") || strstr(pgl_renderer, "softpipe") ||
             strstr(pgl_renderer, "swrast"));
        if (e && e[0] == '0')      { pgl_texbarrier_on = 0; mode = 0; }
        else if (e && e[0] == '2') { pgl_texbarrier_on = 1; mode = 2; }
        else if (e && e[0] == '4') { pgl_texbarrier_on = 1; mode = 4; }
        else if (e && e[0] == '1') { pgl_texbarrier_on = 1; mode = 1; }
        else {
            pgl_texbarrier_on = soft_inorder ? 0 : 1;
            mode = 1;
            fprintf(stderr, "GL backend: fb-fetch ordering barrier %s (renderer %s: "
                    "%s rasterisation; DQ8_GL_TEXBARRIER=1/2/4 force it, =0 off)\n",
                    pgl_texbarrier_on ? "on" : "off",
                    pgl_renderer[0] ? pgl_renderer : "(unknown)",
                    soft_inorder ? "in-order software" : "hardware/tiled");
        }
    }
    if (!pgl_texbarrier_on || !pgl_rt_wrote) return;
    /* t_2d934e23: disjoint boxes -> this draw cannot observe the pending
       writes (screen-aligned fetch, conservative write rects), so the pipeline
       flush is not needed.  DQ8_GL_TEXBARRIER=4 forces the barrier for every
       reading draw (A/B of this skip). */
    if (mode != 4 && !pgl_wrect_hit(x0, y0, x1, y1)) {
        ++pgl_texbar_skip;
        return;
    }
    /* t_2d934e23: the fb-fetch surface is read through an IMAGE unit
       (imageLoad), so glMemoryBarrier(SHADER_IMAGE_ACCESS_BARRIER_BIT|...) is
       the spec primitive for it; glTextureBarrier orders texture FETCHES and
       makes llvmpipe wait for every rasteriser thread (~1.3 ms/draw measured).
       DQ8_GL_TEXBARRIER=2 selects the image barrier, =1 (default) the texture
       barrier, =0 no barrier at all. */
    if (mode == 1 && pgl_glTextureBarrier) {
        pgl_glTextureBarrier();
    } else if (pgl_glMemoryBarrier) {
        pgl_glMemoryBarrier(PGGL_TEXTURE_FETCH_BARRIER_BIT |
                            PGGL_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                            PGGL_FRAMEBUFFER_BARRIER_BIT);
    } else if (pgl_glTextureBarrier) {
        pgl_glTextureBarrier();
    } else {
        return; /* no ordering primitive: fb-fetch is already disabled */
    }
    pgl_rt_wrote = 0;
    pgl_nwrect = 0;
    ++pgl_texbarriers;
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

/* t_993e6fb6: colour-only clears must never touch COLOR_ATTACHMENT1.
   glClear(GL_COLOR_BUFFER_BIT) clears EVERY buffer in the FBO's draw-buffer
   list, and after any depth draw that list is {COLOR_ATTACHMENT0,
   COLOR_ATTACHMENT1} — ATT1 being the emulated PICA depth mirror (R32F).
   The uniform-fill fast clear below therefore wiped the depth mirror with the
   FILL COLOUR while pgl_zshadow still reported it in sync (its memcmp fast
   path then never restored it), so every later depth test compared integer z
   (0..16777215) against a ~0..1 value and failed: the front faces of 3D
   models drop out and the interior/back faces show through (in the extreme,
   the scene goes black).  The software rasterizer has no such path, which is
   why the SW backend is perfect.  Pin the draw-buffer list to ATT0 around the
   colour clear (probe tools/gl_mrt_clear_probe.c T2/T4); the next draw
   re-establishes its own list (glDrawBuffers(depth_on ? 2 : 1, ...)).
   The counters make the interleaving visible in the operator's log. */
static void pgl_clear_color_only(PGfloat r, PGfloat g, PGfloat b, PGfloat a) {
    PGenum one[1] = { PGGL_COLOR_ATTACHMENT0 };
    if (pgl_glGetIntegerv && pgl_glDrawBuffers) {
        PGint db1 = 0;
        /* GL_DRAW_BUFFER1 = 0x8826 (0x8830 is DRAW_BUFFER11 and always
           GL_NONE here, which would silently hide the interception). */
        pgl_glGetIntegerv(0x8826 /* GL_DRAW_BUFFER1 */, &db1);
        if (db1 != 0) ++pgl_clear_would_clobber; /* ATT1 was in the list */
        pgl_glColorMask(1, 1, 1, 1);
        pgl_glDrawBuffers(1, one);
    } else {
        /* Cannot pin: the clear WILL wipe the depth mirror.  Force the next
           depth refresh to re-upload from guest memory instead of trusting
           the (now stale) shadow comparison. */
        pgl_zforce_full = 1;
    }
    pgl_glClearColor(r, g, b, a);
    pgl_glClear(PGGL_COLOR_BUFFER_BIT);
    ++pgl_clear_pins;
}

/* t_993e6fb6: the PICA colour mask gates the COLOUR attachment only.
   COLOR_ATTACHMENT1 is the emulated depth buffer, and the software path
   (pica_depth_test_write_d) writes depth for colour-masked depth-only draws.
   A plain glColorMask() applies to every enabled draw buffer, so a
   colour-masked depth prepass silently dropped its depth-mirror write and
   every later depth test compared against stale z — front faces dropped out
   and the model interior showed through (GL only; SW has no such coupling).
   glColorMaski splits them. */
static void pgl_apply_color_mask(const PicaGlState *st) {
    PGboolean r = (st->color_mask >> 0) & 1, g = (st->color_mask >> 1) & 1,
              b = (st->color_mask >> 2) & 1, a = (st->color_mask >> 3) & 1;
    if (pgl_glColorMaski) {
        pgl_glColorMaski(0, r, g, b, a);
        /* t_a22413f1: ATT1 carries the raw integer z for the guest write-back.
           In HARDWARE mode the real depth test already killed failing
           fragments, so ATT1 is written exactly when the PICA depth mask allows
           it (never re-writing the previous z, which needed the undefined
           read-back).  In IN-SHADER mode (run3: the pre-fix behaviour, and the
           default) the shader itself stores oldz when the depth mask is off,
           so ATT1 must stay writable for every depth draw. */
        if (st->depth_on && (pgl_depth_hw ? (st->depth_write || pgl_hw_att1all) : 1))
            pgl_glColorMaski(1, 1, 1, 1, 1);
        else
            pgl_glColorMaski(1, 0, 0, 0, 0);
        return;
    }
    pgl_glColorMask(r, g, b, a);
}

/* t_a22413f1: hardware depth state for one draw.  PICA compare codes are
   0=never 1=always 2=equal 3=notequal 4=less 5=lequal 6=greater 7=gequal. */
static unsigned pgl_gl_depth_func(unsigned pica_func) {
    switch (pica_func) {
    case 0u: return PGGL_NEVER;
    case 1u: return PGGL_ALWAYS;
    case 2u: return PGGL_EQUAL;
    case 3u: return PGGL_NOTEQUAL;
    case 4u: return PGGL_LESS;
    case 5u: return PGGL_LEQUAL;
    case 6u: return PGGL_GREATER;
    default: return PGGL_GEQUAL;
    }
}

static void pgl_apply_depth_state(const PicaGlState *st) {
    int on, mask;
    unsigned func;
    if (!pgl_glDepthFunc || !pgl_glDepthMask) return;
    if (!pgl_depth_hw) {
        /* t_a22413f1 run3: IN-SHADER mode — the fragment shader does the
           compare/discard against the emulated ATT1 mirror, exactly like the
           pre-fix backend, so the GL depth test must not also run. */
        if (pgl_depth_test_applied != 0) {
            pgl_glDisable(PGGL_DEPTH_TEST);
            pgl_depth_test_applied = 0;
        }
        if (pgl_depth_mask_applied != 0) {
            pgl_glDepthMask(0);
            pgl_depth_mask_applied = 0;
        }
        return;
    }
    if (!st->depth_on) {
        /* No depth buffer bound: no test, no write (gateway for the PICA
           "unbound depth" case, which the software path also passes). */
        if (pgl_depth_test_applied != 0) {
            pgl_glDisable(PGGL_DEPTH_TEST);
            pgl_depth_test_applied = 0;
        }
        if (pgl_depth_mask_applied != 0) {
            pgl_glDepthMask(0);
            pgl_depth_mask_applied = 0;
        }
        return;
    }
    on = (st->depth_test || st->depth_write) ? 1 : 0;
    func = st->depth_test ? pgl_gl_depth_func(st->depth_func) : PGGL_ALWAYS;
    mask = st->depth_write ? 1 : 0;
    if (pgl_depth_test_applied != on) {
        if (on) pgl_glEnable(PGGL_DEPTH_TEST);
        else pgl_glDisable(PGGL_DEPTH_TEST);
        pgl_depth_test_applied = on;
    }
    if (on && pgl_depth_func_applied != func) {
        pgl_glDepthFunc((PGenum)func);
        pgl_depth_func_applied = func;
    }
    if (pgl_depth_mask_applied != mask) {
        pgl_glDepthMask((PGboolean)mask);
        pgl_depth_mask_applied = mask;
    }
}

/* Bring the FBO back in sync with guest memory after guest-side writes.
   `raw`/`len` are the render target's raw guest bytes (host->rt_raw).
   Fast paths: unchanged region (single memcmp), uniform raw pattern (GSP
   fills / memsets -> glClear or residue-class colors), otherwise a per-8x8-
   Morton-block diff uploaded as sub-rectangles.  The Morton color buffer is
   laid out as 64-pixel (8x8) blocks in block-row-major order, so a block is
   one contiguous raw range (see pica_rt_off in the runtime). */
/* ---- t_477973bb: refresh once per PICA list, not once per draw -----------
   Every draw re-synced the FBO against guest memory with two full-surface
   memcmp()s (colour 384 KB + depth).  On the RX 580 that is 36.8 us per draw,
   ref_ms=256 ms/s = 27 % of the main thread (measured with GLPHASE on
   dq8-b18) and 99.6 % of those compares find the buffer UNCHANGED
   (refeq=6933/6963 per second).
   The compare is only needed to catch GUEST-SIDE writes to the render target.
   Between two draws of one submitted PICA command list the guest CPU cannot
   run: the list is parsed synchronously on the submitting thread, so the
   guest bytes are frozen for the whole list.  One refresh per list is
   therefore equivalent, and a new list (pica_list_no, bumped by
   pica_process_command_list) re-arms the compare — the very same signal the
   batching design already uses.  A render-target switch (different raw
   pointer/length), a rebuilt shadow or pgl_force_full also re-arm it.
   DQ8_REFRESH_PER_DRAW=1 restores the old per-draw compare exactly. */
static int pgl_refr_per_draw = -1;
static const unsigned char *pgl_shadow_src;
static const unsigned char *pgl_zshadow_src;
/* Guest-write watch (t_477973bb): the ranges the render target and its depth
   mirror occupy in guest RAM.  Every guest store (put8/16/32 in the runtime)
   and every GSP fill reports here, so a refresh is only needed when the guest
   actually touched those bytes — the memcmp only has to prove the common
   "nothing changed" case, and this proves it without reading 576 KB. */
static unsigned char *pgl_watch_lo, *pgl_watch_hi;
static unsigned char *pgl_zwatch_lo, *pgl_zwatch_hi;
static int pgl_guest_dirty;
static int pgl_dirty_on = -1;   /* DQ8_REFRESH_DIRTY=0 -> legacy per-draw */

void pica_gl_note_guest_write(const void *p, unsigned len) {
    const unsigned char *c = (const unsigned char *)p;
    if (pgl_guest_dirty || !len) return;
    if (pgl_watch_lo && c < pgl_watch_hi && c + len > pgl_watch_lo) {
        pgl_guest_dirty = 1;
        return;
    }
    if (pgl_zwatch_lo && c < pgl_zwatch_hi && c + len > pgl_zwatch_lo)
        pgl_guest_dirty = 1;
}

/* 1 = refresh this draw, 0 = the shadow already reflects guest memory,
   -1 = host access failed. */
static int pgl_refr_needed(const PicaGlState *st, const PicaGlHost *host,
                           const unsigned char **raw, unsigned *len) {
    if (pgl_refr_per_draw < 0) {
        const char *e = getenv("DQ8_REFRESH_PER_DRAW");
        const char *d = getenv("DQ8_REFRESH_DIRTY");
        pgl_refr_per_draw = (e && e[0] && e[0] != '0') ? 1 : 0;
        pgl_dirty_on = (d && d[0] == '0') ? 0 : 1;
    }
    if (!host->rt_raw(host->user, raw, len)) return -1;
    if (*raw && *len) {
        pgl_watch_lo = (unsigned char *)*raw;
        pgl_watch_hi = pgl_watch_lo + *len;
    } else {
        pgl_watch_lo = pgl_watch_hi = NULL;
    }
    if (st->depth_on && host->depth_raw) {
        const unsigned char *zr = NULL;
        unsigned zl = 0;
        if (!host->depth_raw(host->user, &zr, &zl)) return -1;
        pgl_zwatch_lo = (unsigned char *)zr;
        pgl_zwatch_hi = zr ? (unsigned char *)zr + zl : NULL;
    } else {
        pgl_zwatch_lo = pgl_zwatch_hi = NULL;
    }
    if (pgl_refr_per_draw || !pgl_dirty_on) return 1;
    if (pgl_force_full || !pgl_shadow) return 1;
    if (pgl_shadow_src != *raw || pgl_shadow_len != *len) return 1;
    if (pgl_shadow_w != st->rt_w || pgl_shadow_h != st->rt_h) return 1;
    if (pgl_shadow_bpp != (st->rt_bpp ? st->rt_bpp : 4u)) return 1;
    if (pgl_guest_dirty) return 1;
    if (st->depth_on && host->depth_raw) {
        if (pgl_zforce_full || !pgl_zshadow) return 1;
        if (pgl_zshadow_src != pgl_zwatch_lo ||
            pgl_zshadow_len != (unsigned)(pgl_zwatch_hi - pgl_zwatch_lo))
            return 1;
    }
    return 0;
}


/* ================= t_ab8159de: INTERNAL RENDER SCALE =======================
   Everything the guest hands this backend is NATIVE resolution data, and
   everything the guest consumes (framebuffer, depth, display transfers,
   captures) must stay native too.  Only the GL render targets grow:

     guest pixel (x,y)  ->  target texels [x*f .. x*f+f-1] x [y*f .. y*f+f-1]

   * guest -> target uploads replicate each native texel f times in x and y
     (nearest).  The write-back box average of an untouched f x f block is
     therefore exactly the original texel, so guest content round-trips
     bit-exactly; anything rasterised on top gets f x f coverage samples.
   * the fragment shader computes its barycentrics from
     px = gl_FragCoord.xy / f, i.e. at the f x f sub-pixel positions inside the
     guest pixel, and every render-target texel fetch uses the supersampled
     coordinate gl_FragCoord.xy (safe: the target IS f x f bigger), while the
     u_dst/u_zdst copies are supersampled copies of the draw box.
   * the write-back reads the f x f box and box-averages it back down.

   At f == 1 every helper below is the identity and the code is byte-for-byte
   the previous behaviour (the upscale returns the caller's pointer). */
static unsigned char *pgl_up_rgba;
static size_t pgl_up_rgba_cap;
static float *pgl_up_z;
static size_t pgl_up_z_cap;

static const unsigned char *pgl_upscale_rgba(const unsigned char *rgba,
                                             int gw, int gh) {
    const int f = pgl_cur_scale;
    size_t stride, need;
    unsigned char *dst;
    int y, x, d, k;
    if (!rgba || f <= 1) return rgba;              /* scale 1: no copy at all */
    stride = (size_t)gw * (size_t)f * 4u;
    need = stride * (size_t)gh * (size_t)f;
    if (pgl_up_rgba_cap < need) {
        unsigned char *n = (unsigned char *)realloc(pgl_up_rgba, need);
        if (!n) return NULL;
        pgl_up_rgba = n;
        pgl_up_rgba_cap = need;
    }
    dst = pgl_up_rgba;
    for (y = 0; y < gh; ++y) {
        unsigned char *row = dst + (size_t)y * (size_t)f * stride;
        const unsigned char *src = rgba + (size_t)y * (size_t)gw * 4u;
        for (x = 0; x < gw; ++x) {
            const unsigned char *sp = src + (size_t)x * 4u;
            unsigned char *dp = row + (size_t)x * (size_t)f * 4u;
            for (d = 0; d < f; ++d) {
                for (k = 0; k < 4; ++k) dp[k] = sp[k];
                dp += 4;
            }
        }
        for (d = 1; d < f; ++d)
            memcpy(row + (size_t)d * stride, row, stride);
    }
    return dst;
}

static const float *pgl_upscale_z(const float *z, int gw, int gh) {
    const int f = pgl_cur_scale;
    size_t stride, need;
    float *dst;
    int y, x, d;
    if (!z || f <= 1) return z;
    stride = (size_t)gw * (size_t)f;
    need = stride * (size_t)gh * (size_t)f;
    if (pgl_up_z_cap < need) {
        float *n = (float *)realloc(pgl_up_z, need * sizeof(float));
        if (!n) return NULL;
        pgl_up_z = n;
        pgl_up_z_cap = need;
    }
    dst = pgl_up_z;
    for (y = 0; y < gh; ++y) {
        float *row = dst + (size_t)y * (size_t)f * stride;
        const float *src = z + (size_t)y * (size_t)gw;
        for (x = 0; x < gw; ++x) {
            float v = src[x];
            float *dp = row + (size_t)x * (size_t)f;
            for (d = 0; d < f; ++d) dp[d] = v;
        }
        for (d = 1; d < f; ++d)
            memcpy(row + (size_t)d * stride, row, stride * sizeof(float));
    }
    return dst;
}

/* Upload a native RGBA8 box at guest (gx,gy) into the colour attachment. */
static int pgl_upload_rgba(int gx, int gy, int gw, int gh,
                           const unsigned char *rgba) {
    const unsigned char *src;
    if (gw <= 0 || gh <= 0 || !rgba) return 0;
    src = pgl_upscale_rgba(rgba, gw, gh);
    if (!src) return 0;
    pgl_glBindTexture(PGGL_TEXTURE_2D, pgl_rt_tex);
    pgl_obj_used |= 16u;
    pgl_glTexSubImage2D(PGGL_TEXTURE_2D, 0, PGL_S(gx), PGL_S(gy),
                        PGL_S(gw), PGL_S(gh), PGGL_RGBA, PGGL_UNSIGNED_BYTE,
                        src);
    return 1;
}

/* (Re)specify the whole colour attachment from a native RGBA8 image
   (NULL = allocate only, contents undefined). */
static int pgl_specify_rgba(int gw, int gh, const unsigned char *rgba) {
    const unsigned char *src = pgl_upscale_rgba(rgba, gw, gh);
    if (rgba && !src) return 0;
    pgl_glBindTexture(PGGL_TEXTURE_2D, pgl_rt_tex);
    pgl_obj_used |= 16u;
    pgl_glTexImage2D(PGGL_TEXTURE_2D, 0, PGGL_RGBA8, PGL_S(gw), PGL_S(gh), 0,
                     PGGL_RGBA, PGGL_UNSIGNED_BYTE, src);
    return 1;
}

/* (Re)specify the integer-z mirror (R32F) from a native float image. */
static int pgl_specify_r32f(int gw, int gh, const float *z_native) {
    const float *src = pgl_upscale_z(z_native, gw, gh);
    if (z_native && !src) return 0;
    pgl_glBindTexture(PGGL_TEXTURE_2D, pgl_z_tex);
    pgl_obj_used |= 128u;
    pgl_glTexImage2D(PGGL_TEXTURE_2D, 0, PGGL_R32F, PGL_S(gw), PGL_S(gh), 0,
                     PGGL_RED, PGGL_FLOAT, src);
    return 1;
}

/* (Re)specify the real depth attachment (D32F) from a native float image. */
static int pgl_specify_d32f(int gw, int gh, const float *z_native) {
    const float *src = pgl_upscale_z(z_native, gw, gh);
    if (z_native && !src) return 0;
    pgl_glBindTexture(PGGL_TEXTURE_2D, pgl_dtex);
    pgl_obj_used |= 512u;
    pgl_glTexImage2D(PGGL_TEXTURE_2D, 0, PGGL_DEPTH_COMPONENT32F,
                     PGL_S(gw), PGL_S(gh), 0, PGGL_DEPTH_COMPONENT, PGGL_FLOAT,
                     src);
    return 1;
}

/* ---- write-back read-back: supersampled box -> native texels -------------
   Colour and alpha are BOX FILTERED: each native texel is the rounded mean of
   its f x f target texels (sum + n/2) / n, per channel, alpha included.  That
   is what makes an edge that splits the pixel become a proportional blend,
   and it is the identity at f == 1.
   Depth is NOT averaged (a mean of z values is a depth no fragment ever had,
   and it would break the integer compare in the in-shader path): the value of
   the sub-sample nearest the NATIVE pixel centre is taken (ties go to the
   higher sub-sample).  That keeps "the frontmost surface at this pixel" —
   the same answer a native render gives — while the colour keeps the extra
   coverage information. */
static unsigned char *pgl_rb_color_native(int x0, int y0, int x1, int y1) {
    const int f = pgl_cur_scale;
    unsigned w = (unsigned)(x1 - x0 + 1), h = (unsigned)(y1 - y0 + 1);
    unsigned char *out = (unsigned char *)malloc((size_t)w * (size_t)h * 4u);
    unsigned x, y, sx, sy, n = (unsigned)(f * f);
    if (!out) return NULL;
    if (f <= 1) {
        pgl_glReadPixels(x0, y0, (PGsizei)w, (PGsizei)h, PGGL_RGBA,
                         PGGL_UNSIGNED_BYTE, out);
        return out;
    }
    {
        unsigned sw = w * (unsigned)f, sh = h * (unsigned)f;
        unsigned char *big = (unsigned char *)malloc((size_t)sw * sh * 4u);
        if (!big) { free(out); return NULL; }
        pgl_glReadPixels(x0 * f, y0 * f, (PGsizei)sw, (PGsizei)sh, PGGL_RGBA,
                         PGGL_UNSIGNED_BYTE, big);
        for (y = 0; y < h; ++y)
            for (x = 0; x < w; ++x) {
                unsigned acc[4] = { 0, 0, 0, 0 };
                int k;
                for (sy = 0; sy < (unsigned)f; ++sy) {
                    const unsigned char *row = big +
                        (((size_t)(y * f + sy) * sw + (size_t)x * f) * 4u);
                    for (sx = 0; sx < (unsigned)f; ++sx, row += 4u)
                        for (k = 0; k < 4; ++k) acc[k] += row[k];
                }
                for (k = 0; k < 4; ++k)
                    out[((size_t)y * w + x) * 4u + (size_t)k] =
                        (unsigned char)((acc[k] + n / 2u) / n);
            }
        free(big);
    }
    return out;
}

static float *pgl_rb_depth_native(int x0, int y0, int x1, int y1) {
    const int f = pgl_cur_scale;
    unsigned w = (unsigned)(x1 - x0 + 1), h = (unsigned)(y1 - y0 + 1);
    float *out = (float *)malloc((size_t)w * (size_t)h * sizeof(float));
    if (!out) return NULL;
    if (f <= 1) {
        pgl_glReadPixels(x0, y0, (PGsizei)w, (PGsizei)h, PGGL_RED,
                         PGGL_FLOAT, out);
        return out;
    }
    {
        unsigned sw = w * (unsigned)f, sh = h * (unsigned)f;
        float *big = (float *)malloc((size_t)sw * sh * sizeof(float));
        unsigned x, y;
        int k = f / 2;              /* sub-sample nearest the native centre */
        if (!big) { free(out); return NULL; }
        pgl_glReadPixels(x0 * f, y0 * f, (PGsizei)sw, (PGsizei)sh, PGGL_RED,
                         PGGL_FLOAT, big);
        for (y = 0; y < h; ++y)
            for (x = 0; x < w; ++x)
                out[(size_t)y * w + x] =
                    big[((size_t)(y * f + (unsigned)k)) * sw +
                        (size_t)(x * f + (unsigned)k)];
        free(big);
    }
    return out;
}

static int pgl_refresh(const PicaGlState *st, const PicaGlHost *host,
                       const unsigned char *raw, unsigned len) {
    unsigned char *pixels;
    unsigned bs, blocks_x, nblocks, bi, x, y;
    int all = pgl_force_full;
    if (!all && memcmp(pgl_shadow, raw, len) == 0) { ++pgl_ph_ref_eq; return 1; }
    ++pgl_refills;
    /* 1) uniform raw pattern: the whole region is a repeating 3- or 4-byte
          pattern (what gsp_memory_fill / memset write).  A pixel's decoded
          color then depends only on (px_off % L), i.e. at most L colors;
          one color becomes an O(1) glClear. */
    {
        unsigned L = 0, i;
        /* t_2d934e23: a uniform raw pattern of period 3 or 4 is exactly
           memcmp(raw, raw + i, len - i) == 0.  The old per-byte
           `raw[j] != raw[j % i]` loop did a modulo per byte — two full passes
           over the 819 KB render target (~2-3 ms) on every fill-induced
           refresh; memcmp is vectorised and costs ~50 us.  Identical
           predicate, identical L. */
        for (i = 4; i >= 3; --i) {
            if (len < i) continue;
            if (memcmp(raw, raw + i, len - i) == 0) { L = i; break; }
        }
        if (L) ++pgl_ph_ref_uniform;
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
                pgl_clear_color_only(col[0][0] / 255.0f, col[0][1] / 255.0f,
                                     col[0][2] / 255.0f, col[0][3] / 255.0f);
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
                pgl_specify_rgba((int)st->rt_w, (int)st->rt_h, pixels);
                free(pixels);
                pgl_drain_errors("rt-upload", NULL);
                memcpy(pgl_shadow, raw, len);
                pgl_force_full = 0;
                return 1;
            }
        }
    }
    /* 2) generic: changed 8x8 Morton blocks -> sub-rectangle uploads. */
    ++pgl_ph_ref_blocks;
    blocks_x = st->rt_w / 8u;
    bs = 64u * (st->rt_bpp ? st->rt_bpp : 4u);
    if (blocks_x && (st->rt_w % 8u) == 0 && (st->rt_h % 8u) == 0 &&
        (size_t)bs * blocks_x * (st->rt_h / 8u) <= (size_t)len && st->rt_bpp == 4u) {
        /* t_2d934e23: RGBA8 guest buffer (fmt 0 = A,B,G,R bytes).  One
           glTexSubImage2D per horizontal run of changed blocks in a block
           row instead of one per 8x8 block (a full-screen diff was up to
           3200 driver calls), and the pixels are decoded straight out of the
           raw guest bytes (pica_get_pixel's tiling formula) instead of
           64 rt_get() indirect calls -- each of which re-ran
           guest_memory_range -- per block.  DQ8_REF_VERIFY=1 re-runs the old
           path into a second buffer and byte-compares. */
        static const unsigned xl[8] = {0u, 1u, 4u, 5u, 16u, 17u, 20u, 21u};
        static const unsigned yl[8] = {0u, 2u, 8u, 10u, 32u, 34u, 40u, 42u};
        unsigned by, run_bx, run_n, buffers = 0;
        unsigned char *buf = (unsigned char *)malloc((size_t)blocks_x * 8u * 8u * 4u);
        static int refver = -1;
        if (!buf) { free(pixels); return 0; }
        if (refver < 0) {
            const char *e = getenv("DQ8_REF_VERIFY");
            refver = (e && e[0] && e[0] != '0') ? 1 : 0;
        }
        for (by = 0; by < st->rt_h / 8u; ++by) {
            int y0 = (int)st->rt_h - 1 - (int)(by * 8u + 7u);
            for (run_bx = 0; run_bx < blocks_x;) {
                unsigned bi = by * blocks_x + run_bx;
                if (!all && memcmp(pgl_shadow + (size_t)bi * bs,
                                   raw + (size_t)bi * bs, bs) == 0) {
                    ++run_bx;
                    continue;
                }
                for (run_n = 0; run_bx + run_n < blocks_x; ++run_n) {
                    unsigned b = by * blocks_x + run_bx + run_n;
                    if (!all && memcmp(pgl_shadow + (size_t)b * bs,
                                       raw + (size_t)b * bs, bs) == 0)
                        break;
                }
                {
                    unsigned row, blk, ci;
                    for (row = 0; row < 8u; ++row) {
                        unsigned yi = 7u - row;   /* guest row inside block */
                        unsigned yo = yl[yi];
                        for (blk = 0; blk < run_n; ++blk) {
                            const unsigned char *srcb =
                                raw + (size_t)(by * blocks_x + run_bx + blk) * bs;
                            unsigned char *drow = buf + (size_t)blk * 32u +
                                                  (size_t)row * run_n * 32u;
                            for (ci = 0; ci < 8u; ++ci) {
                                const unsigned char *sp = srcb + (yo + xl[ci]) * 4u;
                                unsigned char *dp = drow + (size_t)ci * 4u;
                                dp[0] = sp[3]; dp[1] = sp[2];
                                dp[2] = sp[1]; dp[3] = sp[0];
                            }
                        }
                    }
                    pgl_upload_rgba((int)(run_bx * 8u), y0,
                                    (int)(run_n * 8u), 8, buf);
                    ++buffers;
                    if (refver && buffers <= 64u) {
                        unsigned char *ref = (unsigned char *)malloc(
                            (size_t)run_n * 8u * 8u * 4u);
                        if (ref) {
                            unsigned r2, b2, c2, diffs = 0;
                            for (r2 = 0; r2 < 8u; ++r2)
                                for (b2 = 0; b2 < run_n; ++b2)
                                    for (c2 = 0; c2 < 8u; ++c2) {
                                        unsigned char rgba[4];
                                        unsigned gx = (run_bx + b2) * 8u + c2;
                                        if (!host->rt_get(host->user, (int)gx,
                                                          y0 + (int)r2, rgba))
                                            memset(rgba, 0, 4);
                                        memcpy(ref + ((size_t)b2 * 8u + c2) * 4u +
                                               (size_t)r2 * run_n * 32u, rgba, 4);
                                    }
                            for (r2 = 0; r2 < (unsigned)run_n * 8u * 8u * 4u; ++r2)
                                if (ref[r2] != buf[r2]) ++diffs;
                            fprintf(stderr, "REFVERIFY n=%u run=%u@%u,%u "
                                    "bytes=%u diff=%u %s\n", buffers, run_n,
                                    run_bx, by, run_n * 8u * 8u * 4u, diffs,
                                    diffs ? "MISMATCH" : "IDENTICAL");
                            free(ref);
                        }
                    }
                }
                run_bx += run_n;
            }
        }
        free(buf);
        free(pixels);
        pgl_drain_errors("rt-subupload", NULL);
        memcpy(pgl_shadow, raw, len);
        pgl_force_full = 0;
        return 1;
    }
    pixels = (unsigned char *)malloc(64u * 4u);
    if (!pixels) return 0;
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
            pgl_upload_rgba((int)(bx * 8u), y0, 8, 8, pixels);
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
        pgl_specify_rgba((int)st->rt_w, (int)st->rt_h, img);
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
    if (pgl_dlog_on < 0) {
        const char *e = getenv("DQ8_GL_DEPTHLOG");
        pgl_dlog_on = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    /* t_8510204b diagnosis: DQ8_GL_ZFORCE=1 distrusts the shadow fast path and
       re-uploads the guest depth region into both the mirror and the D32F
       attachment on every depth refresh.  If the battle artifacts disappear
       with it, the cause is a shadow/attachment desync, not the depth test. */
    {
        static int zf = -1;
        if (zf < 0) {
            const char *e = getenv("DQ8_GL_ZFORCE");
            zf = (e && e[0] && e[0] != '0') ? 1 : 0;
        }
        if (zf) pgl_zforce_full = 1;
    }
    if (!pgl_zforce_full && pgl_zshadow && pgl_zshadow_len == len &&
        memcmp(pgl_zshadow, raw, len) == 0) {
        ++pgl_ph_zref_eq;
        if (pgl_dlog_on)
            fprintf(stderr, "ZSYNC skip draw=%llu len=%u force=%d (shadow "
                            "equals guest: attachment NOT refreshed)\n",
                    (unsigned long long)pgl_draws, len, pgl_zforce_full);
        return 1;
    }
    ++pgl_zrefills;
    if (pgl_dlog_on) ++pgl_dlog_syncs;
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
                /* t_a22413f1: the real depth attachment must be cleared with
                   the same value, scaled to the GL depth range (HARDWARE mode
                   only — in IN-SHADER mode there is no depth attachment). */
                if (pgl_depth_hw) {
                    int mask_before = pgl_depth_mask_applied;
                    /* t_599a43f7: single clear path — forces the depth write
                       mask on (t_d8e5ab8c) so the clear cannot be a no-op. */
                    pgl_depth_attachment_clear(
                        (float)z0 * (1.0f / 16777216.0f));
                    if (pgl_depth_probe_left)
                        fprintf(stderr, "GL DEPTHCLEAR z0=%u (%.8f) "
                                "mask_before=%d clearmask_fix=%d\n",
                                z0, (double)((float)z0 * (1.0f / 16777216.0f)),
                                mask_before, pgl_depth_clearmask_fix);
                }
                ok = pgl_drain_errors("z-clear", NULL) == 0;
            }
            if (pgl_dlog_on)
                fprintf(stderr, "ZSYNC clear draw=%llu rt=%ux%u z0=%u bpp=%u "
                        "force=%d ok=%d\n",
                        (unsigned long long)pgl_draws, st->rt_w, st->rt_h,
                        z0, bpp, pgl_zforce_full, ok);
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
                if (pgl_depth_hw) ++pgl_depth_syncs;
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
        if (!pgl_specify_r32f((int)st->rt_w, (int)st->rt_h, img)) {
            free(img);
            return 0;
        }
        /* t_a22413f1: the same values, scaled into [0,1] with an exact
           power-of-two factor, become the real depth attachment the
           rasterizer tests against (zf ordering == float ordering). */
        if (pgl_depth_hw) {
            size_t n = (size_t)st->rt_w * st->rt_h, k;
            for (k = 0; k < n; ++k) img[k] *= (1.0f / 16777216.0f);
            if (!pgl_specify_d32f((int)st->rt_w, (int)st->rt_h, img)) {
                free(img);
                return 0;
            }
        }
        free(img);
        pgl_drain_errors("z-upload", NULL);
        if (pgl_dlog_on)
            fprintf(stderr, "ZSYNC upload draw=%llu rt=%ux%u bpp=%u force=%d\n",
                    (unsigned long long)pgl_draws, st->rt_w, st->rt_h, bpp,
                    pgl_zforce_full);
        if (pgl_depth_hw) ++pgl_depth_syncs;
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
/* t_275eb521: DQ8_GL_DUMP_DIR is read from per-flush write-back code (two
   lookups on every flush); resolve the string once per process. */
static const char *pgl_env_dump_dir(void) {
    static const char *v;
    static int init;
    if (!init) { init = 1; v = getenv("DQ8_GL_DUMP_DIR"); }
    return v;
}
static void pgl_dump_snap(const char *name, const void *data, size_t bytes) {
    const char *dd = pgl_env_dump_dir();
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
    const char *dd = pgl_env_dump_dir();
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
/* t_f8f2c1da: the ZDESYNC figures printed below describe ATT1 (the R32F
   integer-z MIRROR), never the DEPTH_COMPONENT32F attachment the rasterizer
   really tests against.  Two rounds of investigation read that number as "the
   depth buffer is entirely far", so the log now names the surface it measured
   and, for the first few events, measures the REAL depth attachment of the
   same box as well. */
static int pgl_zattach_stats(int x0, int y0, int x1, int y1,
                             double *far_pct, double *min_z, double *max_z) {
    int w = x1 - x0 + 1, h = y1 - y0 + 1, i, n;
    float *z;
    int nfar = 0;
    double mn = 1.0, mx = 0.0;
    if (w <= 0 || h <= 0 || pgl_fbo == 0 || pgl_dt_w == 0) return 0;
    n = w * h;
    z = (float *)malloc((size_t)n * sizeof(float));
    if (!z) return 0;
    pgl_glBindFramebuffer(PGGL_FRAMEBUFFER, pgl_fbo);
    pgl_obj_used |= 8u;
    pgl_glReadBuffer(PGGL_COLOR_ATTACHMENT0);
    pgl_glReadPixels(x0, y0, (PGsizei)w, (PGsizei)h, PGGL_DEPTH_COMPONENT,
                     PGGL_FLOAT, z);
    pgl_glReadBuffer(PGGL_COLOR_ATTACHMENT0);
    pgl_drain_errors("zattach-stats", NULL);
    for (i = 0; i < n; ++i) {
        double v = (double)z[i];
        if (v * 16777216.0 >= 16777215.0) ++nfar;
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    free(z);
    *far_pct = 100.0 * (double)nfar / (double)(n ? n : 1);
    *min_z = mn * 16777216.0;      /* report in PICA integer z units */
    *max_z = mx * 16777216.0;
    return 1;
}

/* ---- t_7d18d1b6: block/parallel DEPTH write-back ------------------------
   The generic loop below resolves host->depth_px_off + host->depth_put per
   pixel (two indirect calls + Morton math + a per-pixel shadow memcmp), which
   is exactly the shape the colour write-back had before t_2d934e23.  The
   depth buffer is Morton-tiled in 8x8 blocks with the same tile layout as the
   colour buffer (pica_depth_off == pica_rt_off, only the pixel size differs),
   so the same treatment applies:

     * one pgl_zwb_preclean() pass decides whether ANY pixel of the box has
       been overwritten by the guest since the shadow snapshot (a block that
       still matches its shadow bytes cannot contain a guest write),
     * if none has (the common case), every pixel of the box is written, so the
       band worker resolves depth_offset ONCE per 8x8 block and writes the
       block's 64 pixels as tight 2/3-byte stores, in parallel over block rows
       on the vertex pool,
     * if any pixel HAS been overwritten, the untouched generic loop runs (it
       owns the per-pixel "guest write wins" rule and the GL ZDESYNC report).

   The fast path is therefore byte-identical to the generic one: it only runs
   when every pixel would take the generic loop's write branch, and it
   reproduces its z rounding (double + 0.5), far-plane clamp and 2/3-byte
   store exactly.  DQ8_WB_ZBLK=0 restores the generic path (A/B + kill
   switch); DQ8_WB_ZPAR=0 keeps the block path but writes it serially. */
/* Parallel pool entry point, defined in pica_vs.c (same TU, emitted later). */
static int pgl_par_run(void (*fn)(void *, unsigned, unsigned), void *ctx,
                       unsigned lo, unsigned hi);

typedef struct {
    const PicaGlHost *host;
    const unsigned char *raw;
    const float *zbuf;
    int x0, y0, x1, y1;
    unsigned w, len, gw, gh, bpp;
    unsigned bad;
} PglWbZBand;

static void pgl_wb_depth_band(void *vctx, unsigned by0, unsigned by1) {
    PglWbZBand *c = (PglWbZBand *)vctx;
    static const unsigned xl[8] = {0u, 1u, 4u, 5u, 16u, 17u, 20u, 21u};
    static const unsigned yl[8] = {0u, 2u, 8u, 10u, 32u, 34u, 40u, 42u};
    const unsigned bpp = c->bpp;
    const unsigned blkbytes = 64u * bpp;
    const float zmaxv = (bpp == 2u) ? 65535.0f : 16777215.0f;
    unsigned by;
    int bx;
    for (by = by0; by < by1; ++by) {
        for (bx = c->x0 >> 3; bx <= (c->x1 >> 3); ++bx) {
            int gx0 = bx << 3, gy0 = (int)(by << 3);
            unsigned boff;
            int gy_lo, gy_hi, gx_lo, gx_hi, gy, gx;
            /* depth_px_off flips GL y to guest y; with gh % 8 == 0 and gy0
               aligned, GL row gy0+7 is guest row (gh-1-gy0-7), the block's
               first (index 0) pixel, i.e. the block start. */
            boff = c->host->depth_px_off(c->host->user, gx0, gy0 + 7);
            if ((size_t)boff + blkbytes > c->len) { c->bad = 1; return; }
            gy_lo = gy0 < c->y0 ? c->y0 : gy0;
            gy_hi = gy0 + 7 > c->y1 ? c->y1 : gy0 + 7;
            gx_lo = gx0 < c->x0 ? c->x0 : gx0;
            gx_hi = gx0 + 7 > c->x1 ? c->x1 : gx0 + 7;
            for (gy = gy_lo; gy <= gy_hi; ++gy) {
                unsigned yidx = yl[(c->gh - 1u - (unsigned)gy) & 7u];
                const float *src = c->zbuf + (size_t)(gy - c->y0) * c->w +
                                   (size_t)(gx_lo - c->x0);
                for (gx = gx_lo; gx <= gx_hi; ++gx, ++src) {
                    /* Inside one 8x8 block the pixels are NOT contiguous: the
                       byte offset of (gx,gy) within the block is
                       (yl[guest_row&7] + xl[gx&7]) * bpp, exactly what
                       pica_depth_off computes.  Walking `dst += bpp` (the
                       first version of this band) writes every pixel onto its
                       left neighbour's slot — caught by DQ8_WB_VERIFY, which
                       byte-compares this band against the reference loop. */
                    unsigned char *dst = (unsigned char *)c->raw + boff +
                                         (yidx + xl[(unsigned)gx & 7u]) * bpp;
                    float v = *src;
                    unsigned zv;
                    if (!(v >= 0.0f)) {
                        zv = 0u;
                    } else if ((double)v >= (double)zmaxv) {
                        zv = (unsigned)zmaxv;
                    } else {
                        zv = (unsigned)((double)v + 0.5);
                    }
                    dst[0] = (unsigned char)(zv & 0xFFu);
                    dst[1] = (unsigned char)((zv >> 8) & 0xFFu);
                    if (bpp >= 3u) dst[2] = (unsigned char)((zv >> 16) & 0xFFu);
                }
            }
        }
    }
}

/* 1 = no pixel of the box carries a guest write since the shadow snapshot
   (the fast path contract), 0 = fall back to the generic loop. */
static int pgl_zwb_preclean(const PicaGlHost *host, const unsigned char *raw,
                            unsigned len, unsigned bpp, unsigned gw,
                            unsigned gh, int x0, int y0, int x1, int y1) {
    unsigned by, blkbytes = 64u * bpp;
    for (by = (unsigned)(y0 >> 3); by <= (unsigned)(y1 >> 3); ++by) {
        int bx;
        for (bx = x0 >> 3; bx <= (x1 >> 3); ++bx) {
            unsigned boff = host->depth_px_off(host->user, bx << 3,
                                               (int)(by << 3) + 7);
            if ((size_t)boff + blkbytes > len) return 0;
            if (memcmp(raw + boff, pgl_zshadow + boff, blkbytes) != 0)
                return 0;
        }
    }
    (void)gw; (void)gh;
    return 1;
}

static int pgl_wb_depth_blocks(const PicaGlHost *host, const unsigned char *raw,
                               unsigned len, unsigned bpp, const float *zbuf,
                               unsigned w, int x0, int y0, int x1, int y1) {
    static int par_on = -1;
    PglWbZBand c;
    unsigned by0, by1;
    if (par_on < 0) {
        const char *e = getenv("DQ8_WB_ZPAR");
        par_on = (e && e[0] == '0') ? 0 : 1;
    }
    memset(&c, 0, sizeof c);
    c.host = host; c.raw = raw; c.zbuf = zbuf;
    c.x0 = x0; c.y0 = y0; c.x1 = x1; c.y1 = y1;
    c.w = w; c.len = len; c.bpp = bpp;
    c.gw = pgl_zshadow_w; c.gh = pgl_zshadow_h;
    by0 = (unsigned)(y0 >> 3);
    by1 = (unsigned)(y1 >> 3) + 1u;
    if (par_on) pgl_par_run(pgl_wb_depth_band, &c, by0, by1);
    else pgl_wb_depth_band(&c, by0, by1);
    return c.bad ? 0 : 1;
}

/* Write-back core for one depth read-back box (GUEST thread): the same skip
   rule as color — a guest-side write after the last sync wins. */
static void pgl_wb_depth_generic(const PicaGlHost *host, int have_raw,
                         const float *buf, unsigned w, unsigned h,
                         int x0, int y0, int x1, int y1) {
    unsigned x, y, bpp = pgl_zshadow_bpp, len = 0;
    const unsigned char *raw = NULL;
    unsigned long long skipped = 0;   /* t_8510204b: skip-desync witness */
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
                       on the next sync. Clamp instead (NaN/negative -> 0).
                       t_a22413f1: the rounding must be done in DOUBLE.  In
                       float32 every odd z >= 2^23 (= 0x800000) has no
                       representable +0.5 (spacing is 1.0), so `(unsigned)
                       (v + 0.5f)` silently rounded it UP by one and wrote a
                       +1-biased depth for the whole far half of the range —
                       a 1-LSB depth mismatch against the software path on
                       every GL depth draw. */
                    if (!(v >= 0.0f)) {
                        zv = 0u;
                        ++nclamp;
                    } else if ((double)v >= (double)zmaxv) {
                        zv = (unsigned)zmaxv;
                        ++nclamp;
                    } else {
                        zv = (unsigned)((double)v + 0.5);
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
        skipped = nskip;
        if (skipped) {
            static unsigned zdesync_n;
            ++zdesync_n;
            if (zdesync_n <= 6 || (zdesync_n % 500u) == 0u) {
                unsigned k, smp = 0;
                unsigned long long nfar_g = 0, nfar_a = 0, npx = (unsigned long long)w * h;
                for (k = 0; k < npx; ++k) {
                    float v = buf[k];
                    if (v >= 0.9999f) ++nfar_a;
                }
                if (raw && bpp && host->depth_px_off) {
                    for (k = 0; k < npx; ++k) {
                        int gx = x0 + (int)(k % w), gy = y0 + (int)(k / w);
                        unsigned off = host->depth_px_off(host->user, gx, gy);
                        unsigned z;
                        if ((size_t)off + bpp > len) continue;
                        z = raw[off] | ((unsigned)raw[off + 1] << 8) |
                            ((bpp > 2u) ? ((unsigned)raw[off + 2] << 16) : 0u);
                        if (z >= (bpp == 2u ? 65535u : 16777215u)) ++nfar_g;
                    }
                }
                fprintf(stderr, "GL ZDESYNC #%u: skip=%llu write=%llu of %llu "
                        "pixels; guest-z far=%.1f%% att1-mirror far=%.1f%% "
                        "box=%d,%d..%d,%d\n",
                        zdesync_n, nskip, nwrite, npx,
                        100.0 * (double)nfar_g / (double)(npx ? npx : 1u),
                        100.0 * (double)nfar_a / (double)(npx ? npx : 1u),
                        x0, y0, x1, y1);
                /* t_f8f2c1da: the two percentages above are GUEST memory and
                   the ATT1 R32F mirror — never the DEPTH_COMPONENT32F
                   attachment the rasterizer tests against.  State the real
                   attachment too (first 3 events only: one extra read-back). */
                if (pgl_depth_hw && zdesync_n <= 3u) {
                    double zfar = 0.0, zmin = 0.0, zmax = 0.0;
                    if (pgl_zattach_stats(x0, y0, x1, y1, &zfar, &zmin, &zmax))
                        fprintf(stderr, "GL ZATTACH #%u: D32F far=%.1f%% "
                                "z=[%.0f..%.0f] of 16777216 box=%d,%d..%d,%d\n",
                                zdesync_n, zfar, zmin, zmax, x0, y0, x1, y1);
                }
                /* four sample pixel pairs: what guest memory holds vs what the
                   ATT1 R32F mirror holds, both as integer z (NOT the D32F
                   depth attachment — see GL ZATTACH above) */
                for (k = 0; k < 4u && smp < npx; ++k) {
                    unsigned xi = (unsigned)(((unsigned long long)(k + 1) * npx) / 5u);
                    int gx = x0 + (int)(xi % w), gy = y0 + (int)(xi / w);
                    unsigned off = host->depth_px_off
                                   ? host->depth_px_off(host->user, gx, gy) : 0u;
                    unsigned gz = 0xFFFFFFFFu;
                    if (raw && off + bpp <= len)
                        gz = raw[off] | ((unsigned)raw[off + 1] << 8) |
                             ((bpp > 2u) ? ((unsigned)raw[off + 2] << 16) : 0u);
                    fprintf(stderr, "  px %d,%d guest_z=%u att1_z=%.1f\n",
                            gx, gy, gz, (double)buf[xi]);
                    ++smp;
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
    /* t_8510204b: skipped pixels mean the attachment no longer matches the
       shadow — never let the next depth refresh trust the memcmp fast path. */
    if (skipped && pgl_zfix_on())
        pgl_request_zresync();
    pgl_dump_snap("z_guest_last.raw", raw, len);
    ++pgl_zflushes;
}

/* t_7d18d1b6: depth write-back entry point.  Fast path (block/parallel) when
   the box is whole 8x8 blocks inside the depth surface, the guest has not
   touched it since the shadow snapshot, and nothing diagnostic forces the
   reference loop; the untouched generic loop otherwise. */
static int pgl_wbz_on = -1;
static int pgl_zwb_verify = -1;   /* DQ8_WB_VERIFY=1: run BOTH depth paths */
static unsigned long long pgl_wbz_fast, pgl_wbz_gen;
static void pgl_wb_depth(const PicaGlHost *host, int have_raw,
                         const float *buf, unsigned w, unsigned h,
                         int x0, int y0, int x1, int y1) {
    const unsigned char *raw = NULL;
    unsigned len = 0, bpp = pgl_zshadow_bpp;
    unsigned char *pre = NULL, *gen = NULL;
    int tried = 0;
    if (pgl_wbz_on < 0) {
        const char *e = getenv("DQ8_WB_ZBLK");
        pgl_wbz_on = (e && e[0] == '0') ? 0 : 1;
        e = getenv("DQ8_WB_VERIFY");
        pgl_zwb_verify = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    if (pgl_wbz_on && !pgl_rect_trace && pgl_zshadow && pgl_zshadow_len &&
        have_raw && host->depth_raw && host->depth_px_off &&
        (pgl_zshadow_w & 7u) == 0u && (pgl_zshadow_h & 7u) == 0u &&
        (bpp == 2u || bpp == 4u) &&
        x0 >= 0 && y0 >= 0 && x1 >= x0 && y1 >= y0 &&
        x1 < (int)pgl_zshadow_w && y1 < (int)pgl_zshadow_h &&
        host->depth_raw(host->user, &raw, &len) && raw &&
        len == pgl_zshadow_len &&
        pgl_zwb_preclean(host, raw, len, bpp, w, h, x0, y0, x1, y1)) {
        int did = 0;
        if (pgl_zwb_verify) {
            /* Same in-process identity test the colour path has used since
               t_2d934e23 (DQ8_WB_VERIFY=1): snapshot guest depth BEFORE any
               write-back, run the reference per-pixel loop, keep its bytes,
               restore the pre state, then let the block path produce its own
               result from the identical input and compare the WHOLE depth
               surface.  A cross-run A/B cannot prove this: two runs of the
               same binary diverge within a few frames. */
            pre = (unsigned char *)malloc(len ? len : 1u);
            gen = (unsigned char *)malloc(len ? len : 1u);
            if (!pre || !gen) { free(pre); free(gen); pre = gen = NULL; }
            else {
                unsigned long long tg;
                memcpy(pre, raw, len);
                tg = pica_pqt_now_ns();
                pgl_wb_depth_generic(host, have_raw, buf, w, h, x0, y0, x1, y1);
                tg = pica_pqt_now_ns() - tg;
                memcpy(gen, raw, len);
                memcpy((unsigned char *)raw, pre, len);
                did = pgl_wb_depth_blocks(host, raw, len, bpp, buf, w, x0, y0, x1, y1);
                tried = 1;
                {
                    unsigned i, diffs = 0, first = 0;
                    if (did)
                        for (i = 0; i < len; ++i)
                            if (raw[i] != gen[i]) {
                                if (!diffs) first = i;
                                ++diffs;
                            }
                    fprintf(stderr, "ZWBVERIFY len=%u bpp=%u box=%d,%d..%d,%d "
                            "diff_bytes=%u first=%u generic_ms=%.3f %s\n",
                            len, bpp, x0, y0, x1, y1, diffs, first,
                            (double)tg / 1e6,
                            !did ? "DECLINED (generic kept)"
                                 : (diffs ? "MISMATCH" : "IDENTICAL"));
                }
                if (!did) memcpy((unsigned char *)raw, gen, len);
                free(pre); free(gen);
            }
        }
        if (!did && !tried)
            did = pgl_wb_depth_blocks(host, raw, len, bpp, buf, w,
                                      x0, y0, x1, y1);
        if (did) {
            ++pgl_wbz_fast;
            memcpy(pgl_zshadow, raw, len);
            pgl_dump_snap("z_guest_last.raw", raw, len);
            ++pgl_zflushes;
            return;
        }
    }
    ++pgl_wbz_gen;
    pgl_wb_depth_generic(host, have_raw, buf, w, h, x0, y0, x1, y1);
}

/* Depth read-back (GL thread): attachment 1 -> caller-owned float box, or
   NULL on failure (the FBO content is then marked untrustworthy). */
static float *pgl_rb_depth(int x0, int y0, int x1, int y1) {
    unsigned w = (unsigned)(x1 - x0 + 1), h = (unsigned)(y1 - y0 + 1);
    float *buf;
    pgl_glBindFramebuffer(PGGL_FRAMEBUFFER, pgl_fbo);
    pgl_obj_used |= 8u;
    pgl_glReadBuffer(PGGL_COLOR_ATTACHMENT1);
    /* t_ab8159de: reads the f x f attachment box and reduces it to the value
       of the sub-sample nearest each native pixel centre. */
    buf = pgl_rb_depth_native(x0, y0, x1, y1);
    pgl_glReadBuffer(PGGL_COLOR_ATTACHMENT0);
    if (!buf) { pgl_request_resync(); return NULL; }
    if (pgl_drain_errors("z-readback", NULL) != 0) {
        free(buf);
        pgl_request_resync();
        return NULL;
    }
    pgl_dump_snap("z_fbo_last.raw", buf, (size_t)w * h * sizeof(float));
    pgl_dump_box("z_fbo_last.box", x0, y0, x1, y1);
    return buf;
}

/* t_2d934e23: block-wise write-back fast path for the scan-out case — guest
   target is a TGBA8 (4-byte) Morton-tiled buffer whose dims are multiples of
   8, which is what the 3DS colour buffer always is.  The generic loop below
   resolves host->rt_px_off + host->rt_put PER PIXEL (two indirect calls, the
   Morton math and a 4-byte guest-shadow memcmp each): measured ~8 ms for one
   498x400 box.  Here the block's guest offset is resolved once per 8x8 block
   and its 64 pixels are written as indexes inside one contiguous 256-byte
   run; the "did the guest overwrite this pixel since the shadow snapshot"
   test is hoisted to a single 256-byte block compare (a block that still
   matches cannot contain a guest write, and every pixel in it is written
   exactly as the generic loop would).  Returns 0 when the geometry/layout is
   outside the fast-path contract so the caller keeps the generic path.

   t_5a3c517a: the block rows are independent (disjoint 256-byte runs, read
   the same shadow), so the box is written as a parallel job over block rows
   on the vertex pool (DQ8_WB_PAR=0 restores the serial loop). */
typedef struct {
    const PicaGlHost *host;
    const unsigned char *raw;
    const unsigned char *pixels;
    int x0, y0, x1, y1;
    unsigned w, len, gw, gh;
    unsigned bad;                 /* contract violation -> caller falls back */
} PglWbBand;

/* Parallel pool entry point, defined in pica_vs.c (same TU, emitted later). */
static int pgl_par_run(void (*fn)(void *, unsigned, unsigned), void *ctx,
                       unsigned lo, unsigned hi);

static void pgl_wb_color_band(void *vctx, unsigned by0, unsigned by1) {
    PglWbBand *c = (PglWbBand *)vctx;
    static const unsigned xl[8] = {0u, 1u, 4u, 5u, 16u, 17u, 20u, 21u};
    static const unsigned yl[8] = {0u, 2u, 8u, 10u, 32u, 34u, 40u, 42u};
    unsigned by;
    int bx;
    for (by = by0; by < by1; ++by) {
        for (bx = c->x0 >> 3; bx <= (c->x1 >> 3); ++bx) {
            int gx0 = bx << 3, gy0 = (int)(by << 3);
            unsigned boff;
            int gy_lo, gy_hi, gx_lo, gx_hi, gy, gx, clean;
            /* rt_px_off flips GL y to guest y; with gh % 8 == 0 and gy0
               aligned, GL row gy0+7 is guest row (gh-1-gy0-7), the block's
               first (index 0) pixel, i.e. the block start. */
            boff = c->host->rt_px_off(c->host->user, gx0, gy0 + 7);
            if ((size_t)boff + 256u > c->len) { c->bad = 1; return; }
            gy_lo = gy0 < c->y0 ? c->y0 : gy0;
            gy_hi = gy0 + 7 > c->y1 ? c->y1 : gy0 + 7;
            gx_lo = gx0 < c->x0 ? c->x0 : gx0;
            gx_hi = gx0 + 7 > c->x1 ? c->x1 : gx0 + 7;
            clean = memcmp(c->raw + boff, pgl_shadow + boff, 256u) == 0;
            if (clean) {
                for (gy = gy_lo; gy <= gy_hi; ++gy) {
                    unsigned yidx = yl[(c->gh - 1u - (unsigned)gy) & 7u];
                    const unsigned char *src =
                        c->pixels + ((size_t)(gy - c->y0) * c->w +
                                     (size_t)(gx_lo - c->x0)) * 4u;
                    for (gx = gx_lo; gx <= gx_hi; ++gx, src += 4u) {
                        /* glReadPixels gives R,G,B,A; the guest colour buffer
                           is A,B,G,R (pica_put_pixel's store order). */
                        unsigned char *dp = (unsigned char *)c->raw + boff +
                                            (yidx + xl[(unsigned)gx & 7u]) * 4u;
                        dp[0] = src[3]; dp[1] = src[2];
                        dp[2] = src[1]; dp[3] = src[0];
                    }
                }
            } else {
                for (gy = gy_lo; gy <= gy_hi; ++gy) {
                    unsigned yidx = yl[(c->gh - 1u - (unsigned)gy) & 7u];
                    for (gx = gx_lo; gx <= gx_hi; ++gx) {
                        unsigned didx = boff + (yidx + xl[(unsigned)gx & 7u]) * 4u;
                        if (memcmp(c->raw + didx, pgl_shadow + didx, 4u) == 0) {
                            const unsigned char *src =
                                c->pixels + ((size_t)(gy - c->y0) * c->w +
                                             (size_t)(gx - c->x0)) * 4u;
                            unsigned char *dp = (unsigned char *)c->raw + didx;
                            dp[0] = src[3]; dp[1] = src[2];
                            dp[2] = src[1]; dp[3] = src[0];
                        }
                    }
                }
            }
        }
    }
}

static int pgl_wb_color_blocks(const PicaGlHost *host, const unsigned char *raw,
                               const unsigned char *pixels,
                               int x0, int y0, int x1, int y1, unsigned w,
                               unsigned len) {
    static int par_on = -1;
    PglWbBand c;
    unsigned by0, by1;
    if ((pgl_shadow_w & 7u) || (pgl_shadow_h & 7u)) return 0;   /* need whole 8x8 blocks */
    if (x0 < 0 || y0 < 0 || x1 >= (int)pgl_shadow_w || y1 >= (int)pgl_shadow_h) return 0;
    if (par_on < 0) {
        const char *e = getenv("DQ8_WB_PAR");
        par_on = (e && e[0] == '0') ? 0 : 1;
    }
    memset(&c, 0, sizeof c);
    c.host = host; c.raw = raw; c.pixels = pixels;
    c.x0 = x0; c.y0 = y0; c.x1 = x1; c.y1 = y1;
    c.w = w; c.len = len; c.gw = pgl_shadow_w; c.gh = pgl_shadow_h;
    by0 = (unsigned)(y0 >> 3);
    by1 = (unsigned)(y1 >> 3) + 1u;
    if (par_on) pgl_par_run(pgl_wb_color_band, &c, by0, by1);
    else pgl_wb_color_band(&c, by0, by1);
    return c.bad ? 0 : 1;
}
/* t_2d934e23: byte-identity probe for the write-back (DQ8_RT_HASH=1).  After
   every colour write-back the guest render-target bytes are hashed and
   printed, so two runs (DQ8_XFER_FAST=1 fast paths vs =0 reference) can be
   compared hash-sequence by hash-sequence. */
static int pgl_rthash_on = -1;
static unsigned long long pgl_rthash_n;
static void pgl_rthash(const unsigned char *raw, unsigned len) {
    unsigned long long h = 1469598103934665603ull;
    unsigned i;
    if (pgl_rthash_on < 0) {
        const char *e = getenv("DQ8_RT_HASH");
        pgl_rthash_on = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    if (!pgl_rthash_on || !raw) return;
    for (i = 0; i < len; ++i) { h ^= raw[i]; h *= 1099511628211ull; }
    fprintf(stderr, "RTHASH n=%llu len=%u h=%016llx\n",
            ++pgl_rthash_n, len, h);
}/* t_d8e5ab8c: DQ8_GL_DEPTH_PROBE=<n> reports, for the first n flushes, what
   the real DEPTH_COMPONENT32F attachment contains compared with the emulated
   ATT1 mirror that the guest write-back reads.  A hardware depth path that
   renders black must show up here as "depth is 1.0/0.0 everywhere" or as a
   systematic mismatch against ATT1; a correct frame shows the two agreeing
   within a few LSB.  Diagnostic only (extra readback per flush). */
static void pgl_depth_probe(const char *tag, int x0, int y0, int x1, int y1) {
    unsigned w, h, i, n, ones = 0, zeros = 0, mid = 0, mism = 0;
    float dmin = 1e30f, dmax = -1e30f;
    double dsum = 0.0;
    float *dbuf, *zbuf;
    if (!pgl_depth_probe_left || !pgl_depth_hw) return;
    if (x0 > x1 || y0 > y1 || pgl_rt_w <= 0 || pgl_rt_h <= 0) return;
    if (x1 >= (int)pgl_rt_w) x1 = (int)pgl_rt_w - 1;
    if (y1 >= (int)pgl_rt_h) y1 = (int)pgl_rt_h - 1;
    w = (unsigned)(x1 - x0 + 1);
    h = (unsigned)(y1 - y0 + 1);
    n = w * h;
    if (!n) return;
    dbuf = (float *)malloc((size_t)n * sizeof(float));
    zbuf = (float *)malloc((size_t)n * sizeof(float));
    if (!dbuf || !zbuf) { free(dbuf); free(zbuf); return; }
    pgl_glBindFramebuffer(PGGL_FRAMEBUFFER, pgl_fbo);
    pgl_obj_used |= 8u;
    pgl_glReadBuffer(PGGL_COLOR_ATTACHMENT0);
    pgl_glReadPixels(x0, y0, (PGsizei)w, (PGsizei)h, PGGL_DEPTH_COMPONENT,
                     PGGL_FLOAT, dbuf);
    pgl_glReadBuffer(PGGL_COLOR_ATTACHMENT1);
    pgl_glReadPixels(x0, y0, (PGsizei)w, (PGsizei)h, PGGL_RED, PGGL_FLOAT,
                     zbuf);
    pgl_glReadBuffer(PGGL_COLOR_ATTACHMENT0);
    --pgl_depth_probe_left;
    ++pgl_depth_probe_n;
    for (i = 0; i < n; ++i) {
        double d = (double)dbuf[i];
        double z = (double)zbuf[i];
        if (dbuf[i] < dmin) dmin = dbuf[i];
        if (dbuf[i] > dmax) dmax = dbuf[i];
        dsum += d;
        if (!(dbuf[i] > 0.0f)) ++zeros;
        else if (dbuf[i] >= 0.99999994f) ++ones;
        else ++mid;
        /* |depth*2^24 - att1_z| > 16 integer z units */
        if (d * 16777216.0 - z > 16.0 || z - d * 16777216.0 > 16.0) ++mism;
    }
    fprintf(stderr, "GL DEPTHPROBE %s #%u box=%d,%d..%d,%d pixels=%u "
            "depth[min=%.9f max=%.9f mean=%.9f] one=%.1f%% zero=%.1f%% "
            "mid=%.1f%% att1_mismatch(>16z)=%.1f%%\n",
            tag, pgl_depth_probe_n, x0, y0, x1, y1, n,
            (double)dmin, (double)dmax, dsum / (double)n,
            100.0 * (double)ones / (double)n, 100.0 * (double)zeros / (double)n,
            100.0 * (double)mid / (double)n, 100.0 * (double)mism / (double)n);
    free(dbuf);
    free(zbuf);
    pgl_drain_errors("depth-probe", NULL);
}
/* Write the FBO's not-yet-written-back content into guest memory.  Only the
   drawn box is read back; a pixel whose raw guest bytes changed since the
   FBO was last synchronized with guest memory (a later guest-side write)
   keeps the guest value — the guest write is newer and wins.  The shadow is
   then re-synchronized with guest memory.  Runs on OUR context (the public
   wrapper pica_gl_save/restore is pica_gl_flush). */
/* Reference per-pixel write-back loop (the pre-t_2d934e23 code, verbatim, as
   a function so the fast path and the verifier can both call it). */
static void pgl_wb_color_generic(const PicaGlHost *host,
                                 const unsigned char *raw, unsigned len,
                                 unsigned bpp, const unsigned char *pixels,
                                 int x0, int y0, int x1, int y1) {
    unsigned x, y;
    unsigned w = (unsigned)(x1 - x0 + 1), h = (unsigned)(y1 - y0 + 1);
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
/* Write-back core for one color read-back box (GUEST thread). */
static void pgl_wb_color(const PicaGlHost *host, int have_raw,
                         const unsigned char *pixels,
                         int x0, int y0, int x1, int y1) {
    unsigned len = 0, bpp = pgl_shadow_bpp;
    unsigned w = (unsigned)(x1 - x0 + 1);
    const unsigned char *raw = NULL;
    if (have_raw && host->rt_raw && !host->rt_raw(host->user, &raw, &len))
        raw = NULL;
    if (raw && (!pgl_shadow || len != pgl_shadow_len)) raw = NULL;
    if (pgl_wb_fast_on < 0) {
        /* t_2d934e23: ON by default; DQ8_WB_FAST=0 selects the reference
           per-pixel loop (A/B).  DQ8_WB_VERIFY=1 runs BOTH in-process and
           byte-compares (see below). */
        const char *e = getenv("DQ8_WB_FAST");
        pgl_wb_fast_on = (e && e[0] == '0') ? 0 : 1;
        e = getenv("DQ8_WB_VERIFY");
        pgl_wb_verify = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    /* t_2d934e23: the scan-out case — DQ8_XFER_FAST=0 forces the generic loop
       (A/B reference for the byte-identity check).  DQ8_WB_VERIFY=1 runs BOTH
       paths inside one process and compares the resulting guest bytes, which
       is the only valid identity test: the scene is not deterministic across
       runs (two runs of the SAME binary diverge by the 3rd write-back). */
    if (raw && bpp == 4u && pgl_wb_fast_on && !pgl_rect_trace) {
        unsigned char *pre = NULL, *gen = NULL;
        unsigned long long t_gen = 0, t_fast = 0;
        int did;
        if (pgl_wb_verify) {
            /* Snapshot BEFORE any write-back, run the reference loop, keep its
               result, restore the pre state, then let the fast path produce
               its own result from the identical input.  (Snapshotting after
               the fast path — the first version of this probe — compares the
               fast result against a generic run over already-written bytes.) */
            pre = (unsigned char *)malloc(len ? len : 1u);
            gen = (unsigned char *)malloc(len ? len : 1u);
            if (!pre || !gen) { free(pre); free(gen); pre = gen = NULL; }
            else {
                memcpy(pre, raw, len);
                t_gen = pica_pqt_now_ns();
                pgl_wb_color_generic(host, raw, len, bpp, pixels, x0, y0, x1, y1);
                t_gen = pica_pqt_now_ns() - t_gen;
                memcpy(gen, raw, len);
                memcpy((unsigned char *)raw, pre, len);
            }
        }
        t_fast = pica_pqt_now_ns();
        did = pgl_wb_color_blocks(host, raw, pixels, x0, y0, x1, y1, w, len);
        t_fast = pica_pqt_now_ns() - t_fast;
        if (gen) {
            unsigned i, diffs = 0, first = 0;
            if (did) {
                for (i = 0; i < len; ++i)
                    if (raw[i] != gen[i]) { if (!diffs) first = i; ++diffs; }
            } else {
                memcpy((unsigned char *)raw, gen, len); /* fast path declined */
                diffs = 0xFFFFFFFFu;
            }
            fprintf(stderr, "WBVERIFY n=%llu len=%u box=%d,%d..%d,%d "
                    "diff_bytes=%u first=%u generic_ms=%.3f fast_ms=%.3f %s\n",
                    (unsigned long long)pgl_rthash_n, len, x0, y0, x1, y1,
                    diffs, first, (double)t_gen / 1e6, (double)t_fast / 1e6,
                    diffs ? "MISMATCH" : "IDENTICAL");
            free(pre); free(gen);
        }
        if (did) {
            ++pgl_wb_fast;
            memcpy(pgl_shadow, raw, len);
            pgl_dump_snap("color_guest_last.raw", raw, len);
            pgl_rthash(raw, len);
            return;
        }
    }
    pgl_wb_color_generic(host, raw, len, bpp, pixels, x0, y0, x1, y1);
    /* rt_put wrote into guest memory through `raw`; snapshot the new state. */
    if (raw) memcpy(pgl_shadow, raw, len);
    pgl_dump_snap("color_guest_last.raw", raw, len);
    pgl_rthash(raw, len);
}

/* t_a22413f1 run3: the operator's Radeon presented an almost fully black
   frame while the D32F depth path was active.  That state must never be the
   lasting one: if a run in HARDWARE mode has not yet produced a single lit
   full frame and three of them come back near-black, drop to the in-shader
   path.  A run that has already drawn something is never overridden (a game
   fade to black must not be mistaken for a driver failure). */
/* t_8510204b: dump the last full-frame flush's depth state for cross-mode
   A/B.  In HARDWARE mode the rasterizer tests the D32F attachment while the
   guest mirror ATT1 keeps the integer z; in IN-SHADER mode ATT1 *is* the test
   surface.  Comparing the two ATT1 images across modes therefore separates
   "the depth VALUES written differ" from "the TEST decision differs".
   Raw w*h float files are overwritten, so the files on disk describe the last
   (largest) flush.  DQ8_GL_DEPTH_DUMP=<dir>.  Diagnostic only. */
static int pgl_depth_dump_on = -1;
static unsigned pgl_depth_dumps;
static void pgl_depth_dump(int x0, int y0, int x1, int y1) {
    const char *dir;
    unsigned w, h, x, y, nmask = 0;
    float *zbuf;
    char path[512];
    FILE *f;
    if (pgl_depth_dump_on < 0) {
        const char *e = getenv("DQ8_GL_DEPTH_DUMP");
        pgl_depth_dump_on = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    if (!pgl_depth_dump_on || pgl_fbo == 0 || pgl_rt_w < 8 || pgl_rt_h < 8) return;
    /* t_05281b89: the dump is written in NATIVE guest pixels; a supersampled
       attachment would silently produce f x f times the rows.  Skip with one
       notice instead of writing a file nobody can compare. */
    if (pgl_cur_scale > 1) {
        static int once;
        if (!once) {
            once = 1;
            fprintf(stderr, "GL backend: depth dump skipped — the target is "
                            "rendered at x%d (DQ8_RENDER_SCALE), dump needs "
                            "native pixels\n", pgl_cur_scale);
        }
        return;
    }
    dir = getenv("DQ8_GL_DEPTH_DUMP");
    if (!dir || !dir[0]) return;
    w = (unsigned)pgl_rt_w;
    h = (unsigned)pgl_rt_h;
    /* only the full-frame flush describes the presented frame */
    if ((unsigned)(x1 - x0 + 1) * (unsigned)(y1 - y0 + 1) < w * h * 4u / 5u) return;
    zbuf = (float *)malloc((size_t)w * h * sizeof(float));
    if (!zbuf) return;
    pgl_glBindFramebuffer(PGGL_FRAMEBUFFER, pgl_fbo);
    pgl_obj_used |= 8u;
    pgl_glReadBuffer(PGGL_COLOR_ATTACHMENT1);
    pgl_glReadPixels(0, 0, (PGsizei)w, (PGsizei)h, PGGL_RED, PGGL_FLOAT, zbuf);
    ++pgl_depth_dumps;
    snprintf(path, sizeof path, "%s/att1.f32", dir);
    f = fopen(path, "wb");
    if (f) { fwrite(zbuf, sizeof(float), (size_t)w * h, f); fclose(f); }
    for (y = 0; y < h; ++y)
        for (x = 0; x < w; ++x) {
            unsigned xi = y * w + x;
            if (!(zbuf[xi] > 0.0f)) ++nmask;
        }
    if (pgl_depth_hw) {
        memset(zbuf, 0, (size_t)w * h * sizeof(float));
        pgl_glReadBuffer(PGGL_COLOR_ATTACHMENT0);
        pgl_glReadPixels(0, 0, (PGsizei)w, (PGsizei)h, PGGL_DEPTH_COMPONENT,
                         PGGL_FLOAT, zbuf);
        snprintf(path, sizeof path, "%s/depth.f32", dir);
        f = fopen(path, "wb");
        if (f) { fwrite(zbuf, sizeof(float), (size_t)w * h, f); fclose(f); }
    }
    pgl_glReadBuffer(PGGL_COLOR_ATTACHMENT0);
    free(zbuf);
    fprintf(stderr, "GL DEPTHDUMP #%u %ux%d hw=%d box=%d,%d..%d,%d z<=0=%.2f%%\n",
            pgl_depth_dumps, w, h, pgl_depth_hw, x0, y0, x1, y1,
            100.0 * (double)nmask / ((double)w * (double)h));
    pgl_drain_errors("depth-dump", NULL);
}

/* t_599a43f7: does the D32F attachment show ANY structure over this box?
   Returns 1 = uniformly far (>=0.99), -1 = uniformly near (<=0.01),
   0 = structured (some fragment wrote a depth), -2 = unavailable/error.
   Only called on a dark frame with depth-tested draws in HARDWARE mode, at
   most a few times per run, so the read-back cost is irrelevant. */
static int pgl_depth_box_flat(int x0, int y0, int x1, int y1) {
    unsigned w, h, i, n, mn_i, mx_i;
    float *zb;
    int res;
    if (!pgl_depth_hw || !pgl_dtex) return -2;
    w = (unsigned)(x1 - x0 + 1);
    h = (unsigned)(y1 - y0 + 1);
    if (!w || !h || w * h > 4u * 1024u * 1024u) return -2;
    zb = (float *)malloc((size_t)w * h * sizeof(float));
    if (!zb) return -2;
    pgl_glBindFramebuffer(PGGL_FRAMEBUFFER, pgl_fbo);
    pgl_obj_used |= 8u;
    pgl_glPixelStorei(PGGL_PACK_ALIGNMENT, 1);
    pgl_glReadPixels(x0, y0, (PGsizei)w, (PGsizei)h, PGGL_DEPTH_COMPONENT,
                     PGGL_FLOAT, zb);
    if (pgl_drain_errors("depth-guard", NULL) != 0) {
        free(zb);
        return -2;
    }
    ++pgl_frame_guard_probes;
    n = w * h;
    /* NaN-safe scan: min and max of the surface. */
    mn_i = 0; mx_i = 0;
    for (i = 1; i < n; ++i) {
        if (zb[i] < zb[mn_i]) mn_i = i;
        if (zb[i] > zb[mx_i]) mx_i = i;
    }
    if (zb[mx_i] - zb[mn_i] < 1e-4f) {
        res = (zb[mn_i] > 0.99f) ? 1 : ((zb[mx_i] < 0.01f) ? -1 : 0);
    } else {
        res = 0;
    }
    if (pgl_depth_probe_left || dq8_env_on("DQ8_GL_DIAG", &pgl_diag_gate))
        fprintf(stderr, "GL DEPTHGUARD box=%d,%d..%d,%d min=%.6f max=%.6f -> %s\n",
                x0, y0, x1, y1, (double)zb[mn_i], (double)zb[mx_i],
                res == 1 ? "flat far" : res == -1 ? "flat near"
                : res == 0 ? "structured" : "unknown");
    free(zb);
    return res;
}

/* t_a22413f1 run3 (rewritten in t_599a43f7): the operator's Radeon presented
   an almost fully black frame while the D32F depth path was active.  That state
   must never be the lasting one.  Verdicts, in order:
     - a plausible scene (>5% of the sampled pixels lit) arms the guard for the
       rest of the run: a game fade to black after that is never overridden;
     - a dark frame whose DEPTH SURFACE is flat while depth-tested draws were
       issued in hardware mode means the hardware depth path is not doing its
       job on this driver (all-far: no fragment passed; all-near: the clear left
       0.0 and every fragment failed GL_LESS).  Two in a row -> fall back;
     - a dark frame with a structured depth surface is the game's own fade (the
       geometry did write depth) -> keep the path, keep watching;
     - nothing plausible after 180 full frames at all -> fall back anyway: an
       unusable depth path costs holes, a black screen costs the run.
   DQ8_GL_DEPTH_AUTOFALLBACK=0 disables the automatic switch (A/B of the pre-fix
   behaviour); the 180-frame floor still applies so a black screen cannot
   survive an unattended run. */
static void pgl_frame_guard(const unsigned char *px, unsigned w, unsigned h) {
    unsigned i, n, sampled, nonblack = 0, pmille, lit;
    int flat;
    if (pgl_frame_guard_off < 0) {
        const char *g = getenv("DQ8_GL_FRAME_GUARD");
        pgl_frame_guard_off = (g && g[0] == '0' && !g[1]);
    }
    if (pgl_frame_guard_off) return;
    if (!pgl_depth_hw || !px || !w || !h) return;
    if (!pgl_depth_syncs) return;   /* depth attachment has no defined content */
    if ((unsigned long long)w * h * 2ull < (unsigned long long)pgl_rt_w * pgl_rt_h)
        return;                        /* partial box: not a whole frame */
    if (pgl_frame_guard_lit < 0) {
        const char *e = getenv("DQ8_GL_FRAME_GUARD_LIT");
        pgl_frame_guard_lit = (e && e[0]) ? atoi(e) : 5;
        if (pgl_frame_guard_lit < 1) pgl_frame_guard_lit = 1;
        if (pgl_frame_guard_lit > 100) pgl_frame_guard_lit = 100;
    }
    n = w * h;
    for (i = 0; i < n; i += 7) {
        const unsigned char *p = px + (size_t)i * 4u;
        if (p[0] | p[1] | p[2]) ++nonblack;
    }
    sampled = (n + 6u) / 7u;
    if (!sampled) return;
    ++pgl_frame_no;
    pmille = (unsigned)((unsigned long long)nonblack * 1000ull / sampled);
    if (pmille > pgl_lit_best_pmille) pgl_lit_best_pmille = pmille;
    lit = (pmille >= (unsigned)pgl_frame_guard_lit * 10u) ? 1u : 0u;
    if (lit) {
        if (!pgl_hw_bright_frames)
            fprintf(stderr, "GL backend: depth path HARDWARE: plausible scene at "
                            "full frame #%u (%u.%u%% lit)\n",
                    pgl_frame_no, pmille / 10u, pmille % 10u);
        pgl_hw_bright_frames = 1;
        pgl_black_frames = 0;
        pgl_dark_frames = 0;
        pgl_dark_draws = 0;
        pgl_frame_guard_last = 1;
        return;
    }
    /* A frame that never asked the rasterizer for depth work (a 2-D UI screen,
       a loading screen, the movie player) says nothing about the depth path:
       do not judge it.  This is what keeps the guard from firing during boot. */
    if (pgl_frame_depth_draws < 8u) {
        pgl_frame_guard_last = 3;
        return;
    }
    ++pgl_black_frames;
    pgl_dark_draws += pgl_frame_depth_draws;
    flat = (pgl_frame_guard_probes < 24u)
         ? pgl_depth_box_flat(0, 0, (int)pgl_tgt_w - 1, (int)pgl_tgt_h - 1) : -2;
    pgl_guard_sample_flat = flat;
    if (flat == 1 || flat == -1) ++pgl_dark_frames;
    else if (flat == 0) pgl_dark_frames = 0;  /* something wrote depth */
    pgl_frame_guard_last = (flat == 1 || flat == -1) ? 4 : (flat == 0 ? 5 : 6);
    if (pgl_black_frames == 1)
        fprintf(stderr, "GL backend: WARNING: dark frame #%u with the D32F depth "
                        "attachment (%u.%u%% lit, %u depth-tested draws, depth "
                        "surface %s)\n",
                pgl_frame_no, pmille / 10u, pmille % 10u, pgl_frame_depth_draws,
                flat == 1 ? "flat FAR" : flat == -1 ? "flat NEAR (nothing passed "
                           "the depth test)" : flat == 0 ? "structured (the game's "
                           "own fade)" : "unreadable");
    /* Decision rules.  A *wrong* fallback costs the model holes; a black screen
       costs the run, so the balance is deliberately one-sided.
       DQ8_GL_DEPTH_AUTOFALLBACK=0 turns the automatic action off entirely (the
       A/B mode: warnings only, the broken path stays on for measurement). */
    if (!pgl_depth_autofallback) return;
    if (pgl_dark_frames >= 2) {
        pgl_depth_fallback(flat == -1
            ? "two dark frames with an all-near D32F depth surface (every "
              "fragment rejected by the depth test)"
            : "two dark frames with an all-far D32F depth surface (no fragment "
              "wrote depth)");
        return;
    }
    if (pgl_dark_draws >= 1200u) {
        /* The stale-depth class: the attachment keeps the previous frame, so the
           surface is NOT flat — but 1200+ depth-tested draws without a single
           lit pixel is not a fade. */
        pgl_depth_fallback("1200+ depth-tested draws with an almost black frame "
                           "and no flat-surface explanation (stale depth?)");
        return;
    }
    if (pgl_black_frames >= 60)
        pgl_depth_fallback("60 consecutive almost-black 3D frames");
}


/* ===== t_05a3190d: deterministic draw-stream replay A/B =====================
   The guest advances on a WALL-CLOCK vblank, not on guest instructions, so two
   runs of this binary at the same instruction budget render different frames
   (22% of pixels differ between two runs of the SAME config).  Every cross-run
   verdict about the hardware depth path was therefore noise.

   This harness records ONE frame's draw stream — the screen-space vertices the
   guest pipeline produced, the PICA state of each draw, and a copy of the
   staged texture bytes — and then re-renders that exact stream twice inside
   this process, from the same guest-memory snapshot:

     pass A: pgl_depth_hw = 1   (real D32F depth attachment, GL depth test)
     pass B: pgl_depth_hw = 0   (in-shader compare/discard against ATT1)

   The draw stream is fixed, so the two renders are deterministic and their
   difference is exactly the depth path's contribution.  Each draw's bounding
   box is read back after the draw in both passes, so the FIRST draw whose
   output differs is reported together with its full PICA depth state, blend
   state and vertex extent.

   Knobs (default off, no behaviour change to normal runs):
     DQ8_GL_REPLAY=<flush#>   record the frame that ends at flush #<flush#>
                              and replay it (needs the synchronous path)
     DQ8_GL_REPLAY_DIR=<dir>  write a.ppm / b.ppm / diff.ppm there
   ======================================================================== */
#define PGL_REP_MAX_DRAWS   20000u
#define PGL_REP_MAX_TEXBYTE (192u * 1024u * 1024u)
typedef struct {
    unsigned nverts;
    PicaGlVertex *verts;
    PicaGlState st;
    int tex_idx;
    int bx0, by0, bx1, by1;
} PglRepDraw;
static struct {
    int on;                      /* DQ8_GL_REPLAY=<flush#> parsed */
    int state;                   /* 0 idle, 1 recording, 2 replayed */
    unsigned long long flush;
    PglRepDraw *d;
    unsigned long long n, cap;
    unsigned char **tex;         /* unique staged texture images */
    const unsigned char **tex_src;
    unsigned *tex_len;
    unsigned ntex, tcap;
    unsigned long long tex_bytes;
    unsigned char *rt_snap, *z_snap;  /* frame-start guest bytes (const) */
    unsigned char *rt_out, *z_out;    /* replay-private copies (mutable) */
    unsigned rt_len, z_len, rt_w, rt_h, depth_bpp;
    PicaGlHost host;             /* copy of the first recorded draw's host */
    int have_host;
    unsigned long long nmixed;   /* draws skipped: other render target */
    unsigned want_w, want_h;     /* DQ8_GL_REPLAY_RT filter (0 = any) */
    /* t_f8f2c1da: frame-selection criterion.  A battle's 3-D frame is only
       interesting when it has depth-tested draws; the flushed 400x240
       message-box frames have a dozen 2-D draws and reproduce nothing.
       DQ8_GL_REPLAY_MINDT=<n> accepts the first recorded frame with at least
       n depth-tested draws (0 = accept any frame). */
    unsigned want_dt, ndepth_test;
} pgl_rep;
#define PGL_REP_TEX(i) (pgl_rep.tex[(i)])

static void pgl_rep_bbox(const PicaGlVertex *verts, unsigned nverts,
                         const PicaGlState *st, int *bx0, int *by0,
                         int *bx1, int *by1) {
    float minx, maxx, miny, maxy;
    unsigned i;
    if (!nverts) { *bx0 = *by0 = *bx1 = *by1 = -1; return; }
    minx = maxx = verts[0].x;
    miny = maxy = verts[0].y;
    for (i = 1; i < nverts; ++i) {
        if (verts[i].x < minx) minx = verts[i].x;
        if (verts[i].x > maxx) maxx = verts[i].x;
        if (verts[i].y < miny) miny = verts[i].y;
        if (verts[i].y > maxy) maxy = verts[i].y;
    }
    *bx0 = (int)floorf(minx) - 1;
    *by0 = (int)floorf(miny) - 1;
    *bx1 = (int)ceilf(maxx) + 1;
    *by1 = (int)ceilf(maxy) + 1;
    if (*bx0 < 0) *bx0 = 0;
    if (*by0 < 0) *by0 = 0;
    if (*bx1 > (int)st->rt_w - 1) *bx1 = (int)st->rt_w - 1;
    if (*by1 > (int)st->rt_h - 1) *by1 = (int)st->rt_h - 1;
    if (*bx0 > *bx1 || *by0 > *by1) { *bx0 = *by0 = *bx1 = *by1 = -1; }
}

static void pgl_rep_record(const PicaGlVertex *verts, unsigned nverts,
                           const PicaGlState *st, const PicaGlHost *host) {
    PglRepDraw *r;
    if (!host || !nverts || !st->rt_w || !st->rt_h) return;
    if (pgl_rep.n >= PGL_REP_MAX_DRAWS) {
        pgl_rep.state = 2;
        fprintf(stderr, "GL REPLAY: frame exceeds %u draws, recording stopped\n",
                (unsigned)PGL_REP_MAX_DRAWS);
        return;
    }
    /* Frame-start snapshot: the guest's RT/depth bytes as they are BEFORE the
       first draw of the frame.  Both replay passes start from exactly these. */
    if (!pgl_rep.have_host) {
        const unsigned char *raw = NULL;
        unsigned len = 0;
        pgl_rep.host = *host;
        pgl_rep.have_host = 1;
        pgl_rep.rt_w = st->rt_w;
        pgl_rep.rt_h = st->rt_h;
        pgl_rep.depth_bpp = st->depth_bpp;
        if (host->rt_raw && host->rt_px_off &&
            host->rt_raw(host->user, &raw, &len) && raw && len) {
            pgl_rep.rt_snap = (unsigned char *)malloc(len);
            pgl_rep.rt_out = (unsigned char *)malloc(len);
            if (pgl_rep.rt_snap && pgl_rep.rt_out) {
                memcpy(pgl_rep.rt_snap, raw, len);
                memcpy(pgl_rep.rt_out, raw, len);
                pgl_rep.rt_len = len;
            } else {
                free(pgl_rep.rt_snap); free(pgl_rep.rt_out);
                pgl_rep.rt_snap = pgl_rep.rt_out = NULL;
            }
        }
        raw = NULL; len = 0;
        if (st->depth_on && host->depth_raw &&
            host->depth_raw(host->user, &raw, &len) && raw && len) {
            pgl_rep.z_snap = (unsigned char *)malloc(len);
            pgl_rep.z_out = (unsigned char *)malloc(len);
            if (pgl_rep.z_snap && pgl_rep.z_out) {
                memcpy(pgl_rep.z_snap, raw, len);
                memcpy(pgl_rep.z_out, raw, len);
                pgl_rep.z_len = len;
            } else {
                free(pgl_rep.z_snap); free(pgl_rep.z_out);
                pgl_rep.z_snap = pgl_rep.z_out = NULL;
            }
        }
        fprintf(stderr, "GL REPLAY: recording flush #%llu rt=%ux%u bpp=%u "
                        "rt_bytes=%u z_bytes=%u\n",
                pgl_rep.flush, st->rt_w, st->rt_h, st->depth_bpp,
                pgl_rep.rt_len, pgl_rep.z_len);
        if (!pgl_rep.rt_len) {
            pgl_rep.state = 2;
            fprintf(stderr, "GL REPLAY: no raw render-target access, aborted\n");
            return;
        }
    }
    /* A frame can mix two render targets (512x400 menu + 482x400 3D view); the
       replay drives ONE FBO from ONE snapshot, so only draws into the snapshot
       target take part. */
    if (st->rt_w != pgl_rep.rt_w || st->rt_h != pgl_rep.rt_h) {
        if (!pgl_rep.nmixed)
            fprintf(stderr, "GL REPLAY: skipping draws into other targets "
                            "(first: %ux%u, replay target %ux%u)\n",
                    st->rt_w, st->rt_h, pgl_rep.rt_w, pgl_rep.rt_h);
        ++pgl_rep.nmixed;
        return;
    }
    if (pgl_rep.n == pgl_rep.cap) {
        unsigned long long nc = pgl_rep.cap ? pgl_rep.cap * 2ull : 256ull;
        PglRepDraw *nd = (PglRepDraw *)realloc(pgl_rep.d,
                                              (size_t)nc * sizeof *nd);
        if (!nd) { pgl_rep.state = 2; return; }
        pgl_rep.d = nd;
        pgl_rep.cap = nc;
    }
    r = &pgl_rep.d[pgl_rep.n];
    memset(r, 0, sizeof *r);
    r->verts = (PicaGlVertex *)malloc((size_t)nverts * sizeof *verts);
    if (!r->verts) { pgl_rep.state = 2; return; }
    memcpy(r->verts, verts, (size_t)nverts * sizeof *verts);
    r->nverts = nverts;
    r->st = *st;
    if (st->depth_on && st->depth_test) ++pgl_rep.ndepth_test;
    r->tex_idx = -1;
    if (st->tex_rgba && st->tex_w > 0 && st->tex_h > 0) {
        unsigned k, texlen = (unsigned)st->tex_w * (unsigned)st->tex_h * 4u;
        for (k = 0; k < pgl_rep.ntex; ++k) {
            if (pgl_rep.tex_src[k] == st->tex_rgba &&
                pgl_rep.tex_len[k] == texlen) { r->tex_idx = (int)k; break; }
        }
        if (r->tex_idx < 0 &&
            pgl_rep.tex_bytes + texlen <= (unsigned long long)PGL_REP_MAX_TEXBYTE) {
            if (pgl_rep.ntex == pgl_rep.tcap) {
                unsigned nc = pgl_rep.tcap ? pgl_rep.tcap * 2u : 32u;
                unsigned char **nt = (unsigned char **)realloc(pgl_rep.tex,
                                        (size_t)nc * sizeof *nt);
                const unsigned char **ns = (const unsigned char **)realloc(
                                        pgl_rep.tex_src, (size_t)nc * sizeof *ns);
                unsigned *nl = (unsigned *)realloc(pgl_rep.tex_len,
                                        (size_t)nc * sizeof *nl);
                if (nt) pgl_rep.tex = nt;
                if (ns) pgl_rep.tex_src = ns;
                if (nl) pgl_rep.tex_len = nl;
                if (!nt || !ns || !nl) { pgl_rep.state = 2; return; }
                pgl_rep.tcap = nc;
            }
            pgl_rep.tex[pgl_rep.ntex] = (unsigned char *)malloc(texlen);
            if (pgl_rep.tex[pgl_rep.ntex]) {
                memcpy(pgl_rep.tex[pgl_rep.ntex], st->tex_rgba, texlen);
                pgl_rep.tex_src[pgl_rep.ntex] = st->tex_rgba;
                pgl_rep.tex_len[pgl_rep.ntex] = texlen;
                pgl_rep.tex_bytes += texlen;
                r->tex_idx = (int)pgl_rep.ntex;
                ++pgl_rep.ntex;
            }
        }
    }
    pgl_rep_bbox(verts, nverts, st, &r->bx0, &r->by0, &r->bx1, &r->by1);
    ++pgl_rep.n;
}

/* ---- replay-private host: guest memory is never touched ------------------ */
static int pgl_rep_h_rt_raw(void *u, const unsigned char **base, unsigned *len) {
    (void)u;
    if (!pgl_rep.rt_out) return 0;
    *base = pgl_rep.rt_out;
    *len = pgl_rep.rt_len;
    return 1;
}
static int pgl_rep_h_z_raw(void *u, const unsigned char **base, unsigned *len) {
    (void)u;
    if (!pgl_rep.z_out) return 0;
    *base = pgl_rep.z_out;
    *len = pgl_rep.z_len;
    return 1;
}
static int pgl_rep_h_rt_get(void *u, int x, int y, unsigned char rgba[4]) {
    (void)u;
    if (!pgl_rep.rt_out || x < 0 || y < 0 ||
        (unsigned)x >= pgl_rep.rt_w || (unsigned)y >= pgl_rep.rt_h) return 0;
    memcpy(rgba, pgl_rep.rt_out +
           ((size_t)y * pgl_rep.rt_w + (size_t)x) * 4u, 4);
    return 1;
}
static void pgl_rep_h_rt_put(void *u, int x, int y, const unsigned char rgba[4]) {
    (void)u;
    if (!pgl_rep.rt_out || x < 0 || y < 0 ||
        (unsigned)x >= pgl_rep.rt_w || (unsigned)y >= pgl_rep.rt_h) return;
    memcpy(pgl_rep.rt_out + ((size_t)y * pgl_rep.rt_w + (size_t)x) * 4u, rgba, 4);
}
static int pgl_rep_h_z_get(void *u, int x, int y, unsigned *z) {
    (void)u;
    if (!pgl_rep.z_out || x < 0 || y < 0 ||
        (unsigned)x >= pgl_rep.rt_w || (unsigned)y >= pgl_rep.rt_h) return 0;
    *z = ((unsigned *)pgl_rep.z_out)[(size_t)y * pgl_rep.rt_w + (size_t)x];
    return 1;
}
static void pgl_rep_h_z_put(void *u, int x, int y, unsigned z) {
    (void)u;
    if (!pgl_rep.z_out || x < 0 || y < 0 ||
        (unsigned)x >= pgl_rep.rt_w || (unsigned)y >= pgl_rep.rt_h) return;
    ((unsigned *)pgl_rep.z_out)[(size_t)y * pgl_rep.rt_w + (size_t)x] = z;
}
static void pgl_rep_make_host(PicaGlHost *h) {
    memset(h, 0, sizeof *h);
    *h = pgl_rep.host;                /* rt_px_off/depth_px_off + user kept */
    h->rt_raw = pgl_rep_h_rt_raw;
    h->rt_get = pgl_rep_h_rt_get;
    h->rt_put = pgl_rep_h_rt_put;
    h->depth_raw = pgl_rep_h_z_raw;
    h->depth_get = pgl_rep_h_z_get;
    h->depth_put = pgl_rep_h_z_put;
}
static unsigned char *pgl_rep_rb(int x0, int y0, int x1, int y1) {
    unsigned w = (unsigned)(x1 - x0 + 1), h = (unsigned)(y1 - y0 + 1);
    unsigned char *buf;
    if (x0 > x1 || y0 > y1) return NULL;
    buf = (unsigned char *)malloc((size_t)w * h * 4u);
    if (!buf) return NULL;
    pgl_glBindFramebuffer(PGGL_FRAMEBUFFER, pgl_fbo);
    pgl_obj_used |= 8u;
    pgl_glReadBuffer(PGGL_COLOR_ATTACHMENT0);
    pgl_glReadPixels(x0, y0, (PGsizei)w, (PGsizei)h, PGGL_RGBA,
                     PGGL_UNSIGNED_BYTE, buf);
    return buf;
}

/* Re-render the recorded stream once under one depth path.  `boxes[i]` (when
   non-NULL on entry) receives the colour read-back of draw i's bounding box. */
static void pgl_rep_pass(int hw, unsigned char **boxes, unsigned char *full) {
    PicaGlHost h;
    unsigned long long i;
    int s_depth_hw, s_test, s_mask, s_hwapplied, s_blend_app, s_blend_en,
        s_guard, s_force, s_zforce, s_pending, s_zdirty, s_zwrote, s_dlog;
    unsigned s_fapplied, s_shlen, s_zshlen, s_shw, s_shh, s_shbpp, s_zshw,
             s_zshh, s_zshbpp;
    unsigned long long s_draws, s_dlogn, s_dlogdraws;
    int s_dx0, s_dy0, s_dx1, s_dy1;

    s_depth_hw = pgl_depth_hw;
    s_test = pgl_depth_test_applied;
    s_fapplied = pgl_depth_func_applied;
    s_mask = pgl_depth_mask_applied;
    s_hwapplied = pgl_depth_hw_applied;
    s_blend_app = pgl_blend_applied;
    s_blend_en = pgl_blend_enabled;
    s_guard = pgl_blend_guard_armed;
    s_force = pgl_force_full;
    s_zforce = pgl_zforce_full;
    s_pending = pgl_pending;
    s_zdirty = pgl_zdirty;
    s_zwrote = pgl_zwrote;
    s_dlog = pgl_dlog_n;
    s_shlen = pgl_shadow_len; s_shw = pgl_shadow_w; s_shh = pgl_shadow_h;
    s_shbpp = pgl_shadow_bpp;
    s_zshlen = pgl_zshadow_len; s_zshw = pgl_zshadow_w; s_zshh = pgl_zshadow_h;
    s_zshbpp = pgl_zshadow_bpp;
    s_draws = pgl_draws;
    s_dlogn = pgl_dlog_n; s_dlogdraws = pgl_dlog_draws;
    s_dx0 = pgl_drawn_x0; s_dy0 = pgl_drawn_y0;
    s_dx1 = pgl_drawn_x1; s_dy1 = pgl_drawn_y1;

    /* The mode switch goes through the setter BELOW (after the shadow state is
       re-pointed at the replay target): it drops the specialised cache, which
       is mandatory because the generated source bakes the path. */
    pgl_depth_test_applied = -1;
    pgl_depth_func_applied = 0;
    pgl_depth_mask_applied = -1;
    pgl_depth_hw_applied = -1;
    pgl_blend_applied = 0;
    pgl_blend_enabled = 0;
    pgl_blend_guard_armed = 0;
    pgl_dlog_on = 0;                  /* keep the live timeline clean */
    pgl_rect_trace = 0;
    pgl_pending = 0;
    pgl_zdirty = 0;
    pgl_zwrote = 0;
    pgl_force_full = 1;
    pgl_zforce_full = 1;
    pgl_shadow_len = 0;
    pgl_zshadow_len = 0;
    /* The shadow DIMENSIONS must already describe the replay target, otherwise
       draw_locked sees a "render target changed" and closes an episode with an
       internal flush in the middle of the replay. */
    pgl_shadow_w = pgl_rep.rt_w;
    pgl_shadow_h = pgl_rep.rt_h;
    pgl_shadow_bpp = (pgl_rep.n && pgl_rep.d[0].st.rt_bpp)
                     ? pgl_rep.d[0].st.rt_bpp : 4u;
    pgl_zshadow_w = pgl_rep.rt_w;
    pgl_zshadow_h = pgl_rep.rt_h;
    pgl_zshadow_bpp = pgl_rep.depth_bpp ? pgl_rep.depth_bpp : 2u;
    /* The specialised fragment shaders BAKE pgl_depth_hw into their source
       (pgl_spec_src); pgl_depth_hw_set() drops the cache in the same step, so
       the other mode's programs can never be reused for the A/B pass. */
    pgl_depth_hw_set(hw, "replay: depth path changed for the A/B pass");
    pgl_reset_drawn();
    if (pgl_rep.rt_out && pgl_rep.rt_snap)
        memcpy(pgl_rep.rt_out, pgl_rep.rt_snap, pgl_rep.rt_len);
    if (pgl_rep.z_out && pgl_rep.z_snap)
        memcpy(pgl_rep.z_out, pgl_rep.z_snap, pgl_rep.z_len);
    pgl_rep_make_host(&h);
    pgl_ensure_rt((int)pgl_rep.rt_w, (int)pgl_rep.rt_h);

    for (i = 0; i < pgl_rep.n; ++i) {
        const PglRepDraw *r = &pgl_rep.d[i];
        PicaGlState st = r->st;
        st.tex_rgba = (r->tex_idx >= 0) ? PGL_REP_TEX(r->tex_idx) : NULL;
        pica_gl_draw_locked(r->verts, r->nverts, &st, &h);
        if (boxes && r->bx0 >= 0)
            boxes[i] = pgl_rep_rb(r->bx0, r->by0, r->bx1, r->by1);
    }
    if (full) {
        unsigned char *f = pgl_rep_rb(0, 0, (int)pgl_rep.rt_w - 1,
                                      (int)pgl_rep.rt_h - 1);
        if (f) memcpy(full, f, (size_t)pgl_rep.rt_w * pgl_rep.rt_h * 4u);
        free(f);
    }

    pgl_depth_hw_set(s_depth_hw, "replay: restore the live depth path");
    pgl_depth_test_applied = s_test;
    pgl_depth_func_applied = s_fapplied;
    pgl_depth_mask_applied = s_mask;
    pgl_depth_hw_applied = s_hwapplied;
    pgl_blend_applied = s_blend_app;
    pgl_blend_enabled = s_blend_en;
    pgl_blend_guard_armed = s_guard;
    pgl_force_full = s_force;
    pgl_zforce_full = s_zforce;
    pgl_pending = s_pending;
    pgl_zdirty = s_zdirty;
    pgl_zwrote = s_zwrote;
    pgl_shadow_len = s_shlen; pgl_shadow_w = s_shw; pgl_shadow_h = s_shh;
    pgl_shadow_bpp = s_shbpp;
    pgl_zshadow_len = s_zshlen; pgl_zshadow_w = s_zshw; pgl_zshadow_h = s_zshh;
    pgl_zshadow_bpp = s_zshbpp;
    pgl_draws = s_draws;
    pgl_dlog_n = s_dlogn; pgl_dlog_draws = s_dlogdraws;
    pgl_dlog_on = s_dlog;
    pgl_drawn_x0 = s_dx0; pgl_drawn_y0 = s_dy0;
    pgl_drawn_x1 = s_dx1; pgl_drawn_y1 = s_dy1;
    if (s_force) pgl_force_full = 1;   /* the live frame's next refresh must
                                          not trust a shadow the replay
                                          re-pointed at its own snapshot */
    if (s_zforce) pgl_zforce_full = 1;
    /* ...and the live path's specialised programs must be rebuilt for the
       restored depth mode — pgl_depth_hw_set() did that above. */
}

static void pgl_rep_reset_stream(void) {
    unsigned i;
    for (i = 0; i < pgl_rep.ntex; ++i) free(pgl_rep.tex[i]);
    pgl_rep.ntex = 0;
    pgl_rep.tex_bytes = 0;
    for (i = 0; i < pgl_rep.n; ++i) free(pgl_rep.d[i].verts);
    pgl_rep.n = 0;
    pgl_rep.have_host = 0;
    pgl_rep.nmixed = 0;
    pgl_rep.ndepth_test = 0;
    free(pgl_rep.rt_snap); free(pgl_rep.z_snap);
    free(pgl_rep.rt_out); free(pgl_rep.z_out);
    pgl_rep.rt_snap = pgl_rep.z_snap = pgl_rep.rt_out = pgl_rep.z_out = NULL;
    pgl_rep.rt_len = pgl_rep.z_len = 0;
}

static int pgl_rep_env = -1;   /* -1 = DQ8_GL_REPLAY not parsed yet */
static void pgl_rep_setup(void) {
    const char *e;
    const char *dir;
    const char *rt;
    if (pgl_rep_env >= 0) return;
    pgl_rep_env = 0;
    e = getenv("DQ8_GL_REPLAY");
    if (!e || !e[0] || e[0] == '0') return;
    pgl_rep.flush = strtoull(e, NULL, 10);
    if (!pgl_rep.flush) return;
    dir = getenv("DQ8_GL_REPLAY_DIR");
    rt = getenv("DQ8_GL_REPLAY_RT");   /* "482x400": flush box to replay */
    if (rt && rt[0]) {
        unsigned ww = 0, hh = 0;
        if (sscanf(rt, "%ux%u", &ww, &hh) == 2) {
            pgl_rep.want_w = ww;
            pgl_rep.want_h = hh;
        }
    }
    {
        const char *md = getenv("DQ8_GL_REPLAY_MINDT");
        if (md && md[0]) pgl_rep.want_dt = (unsigned)atoi(md);
    }
    pgl_rep.on = 1;
    pgl_rep.state = 0;
    pgl_rep_env = 1;
    fprintf(stderr, "GL REPLAY: armed from flush #%llu rt=%ux%u dir=%s\n",
            pgl_rep.flush, pgl_rep.want_w, pgl_rep.want_h,
            (dir && dir[0]) ? dir : "(none)");
}

static void pgl_rep_write_ppm(const char *path, const unsigned char *px,
                              unsigned w, unsigned h) {
    FILE *f = fopen(path, "wb");
    unsigned y, x;
    if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (y = h; y-- > 0;)                 /* GL rows are bottom-up */
        for (x = 0; x < w; ++x)
            fwrite(px + ((size_t)y * w + x) * 4u, 1, 3, f);
    fclose(f);
}

/* Compare the two passes and report the first divergent draw. */
static void pgl_rep_run(void) {
    unsigned n = (unsigned)pgl_rep.n;
    unsigned w = (unsigned)pgl_rep.rt_w, h = (unsigned)pgl_rep.rt_h;
    unsigned char **boxa = NULL, **boxb = NULL;
    unsigned char *fa = NULL, *fb = NULL, *diff = NULL;
    unsigned i, x, y;
    unsigned long long ndiff = 0, first = 0xFFFFFFFFFFFFFFFFull;
    unsigned ndiv = 0;
    const char *dir = getenv("DQ8_GL_REPLAY_DIR");
    if (!pgl_fbo || !w || !h) return;
    boxa = (unsigned char **)calloc(n ? n : 1u, sizeof *boxa);
    boxb = (unsigned char **)calloc(n ? n : 1u, sizeof *boxb);
    fa = (unsigned char *)malloc((size_t)w * h * 4u);
    fb = (unsigned char *)malloc((size_t)w * h * 4u);
    diff = (unsigned char *)malloc((size_t)w * h * 4u);
    if (!boxa || !boxb || !fa || !fb || !diff) goto done;
    /* Recording MUST be off from here on: the replay calls the draw path,
       whose record hook would otherwise append the replayed draws to the
       stream being replayed (and outrun the box arrays). */
    pgl_rep.state = 2;
    fprintf(stderr, "GL REPLAY: replaying %u draws, rt=%ux%u, "
                    "textures=%u (%llu bytes)\n", n, w, h, pgl_rep.ntex,
            pgl_rep.tex_bytes);
    pgl_rep_pass(1, boxa, fa);            /* hardware D32F depth */
    pgl_rep_pass(0, boxb, fb);            /* in-shader ATT1 depth */
    /* full-frame difference */
    for (i = 0; i < w * h; ++i) {
        const unsigned char *a = fa + (size_t)i * 4u;
        const unsigned char *b = fb + (size_t)i * 4u;
        int d = 0;
        if (a[0] != b[0] || a[1] != b[1] || a[2] != b[2] || a[3] != b[3]) d = 1;
        if (d) ++ndiff;
        diff[(size_t)i * 4u + 0] = d ? 255 : (unsigned char)(a[0] / 3);
        diff[(size_t)i * 4u + 1] = d ? 0 : (unsigned char)(a[1] / 3);
        diff[(size_t)i * 4u + 2] = d ? 0 : (unsigned char)(a[2] / 3);
        diff[(size_t)i * 4u + 3] = 255;
    }
    fprintf(stderr, "GL REPLAY: full-frame hw-vs-shader differing pixels: "
                    "%llu of %u (%.2f%%)\n", ndiff, w * h,
            100.0 * (double)ndiff / (double)(w * h));
    /* per-draw: the first draw whose own bounding box already differs */
    for (i = 0; i < n; ++i) {
        const PglRepDraw *r = &pgl_rep.d[i];
        unsigned bw, k;
        unsigned long long npx = 0;
        if (!boxa[i] || !boxb[i] || r->bx0 < 0) continue;
        bw = (unsigned)(r->bx1 - r->bx0 + 1);
        for (k = 0; k < (unsigned)(r->bx1 - r->bx0 + 1) *
                        (unsigned)(r->by1 - r->by0 + 1); ++k) {
            const unsigned char *a = boxa[i] + (size_t)k * 4u;
            const unsigned char *b = boxb[i] + (size_t)k * 4u;
            if (a[0] != b[0] || a[1] != b[1] || a[2] != b[2] || a[3] != b[3])
                ++npx;
        }
        (void)bw;
        if (npx) {
            if (first == 0xFFFFFFFFFFFFFFFFull) first = i;
            if (ndiv < 24u) {
                fprintf(stderr, "GL REPLAY DIV draw=%u nverts=%u box=%d,%d..%d,%d "
                        "diffpx=%llu depth(on=%d test=%d write=%d func=%u zbuf=%d "
                        "scale=%.9g off=%.9g zmax=%.0f bpp=%u) mode=%d blend=%d "
                        "tex=%d alpha_test=%d cmask=%X\n",
                        i, r->nverts, r->bx0, r->by0, r->bx1, r->by1, npx,
                        r->st.depth_on, r->st.depth_test, r->st.depth_write,
                        r->st.depth_func, r->st.depth_zbuf,
                        (double)r->st.depth_scale, (double)r->st.depth_offset,
                        (double)r->st.depth_zmax, r->st.depth_bpp,
                        r->st.mode, r->st.blend, r->st.tev.tex_en,
                        r->st.alpha_test, r->st.color_mask);
            }
            ++ndiv;
        }
    }
    if (first != 0xFFFFFFFFFFFFFFFFull && first < n) {
        const PglRepDraw *r = &pgl_rep.d[first];
        fprintf(stderr, "GL REPLAY: FIRST DIVERGENT DRAW = %llu (of %u); "
                        "divergent draws=%u; v0=(%.2f,%.2f,iw=%.4g,zw=%.4g) "
                        "col0=(%.3f,%.3f,%.3f,%.3f)\n",
                (unsigned long long)first, n, ndiv, r->verts[0].x, r->verts[0].y,
                (double)r->verts[0].iw, (double)r->verts[0].zw,
                (double)r->verts[0].color[0], (double)r->verts[0].color[1],
                (double)r->verts[0].color[2], (double)r->verts[0].color[3]);
    } else {
        fprintf(stderr, "GL REPLAY: NO per-draw divergence found (hw and "
                        "in-shader paths agree pixel-for-pixel on every draw "
                        "box)%s\n", ndiff ? " — but the FULL FRAME differs: the "
                        "difference is outside the draw boxes (refresh/clear/"
                        "write-back path)" : "");
    }
    if (dir && dir[0]) {
        char path[768];
        snprintf(path, sizeof path, "%s/a.ppm", dir);
        pgl_rep_write_ppm(path, fa, w, h);
        snprintf(path, sizeof path, "%s/b.ppm", dir);
        pgl_rep_write_ppm(path, fb, w, h);
        snprintf(path, sizeof path, "%s/diff.ppm", dir);
        pgl_rep_write_ppm(path, diff, w, h);
    }
    /* the live frame's next refresh must re-sync from guest memory */
    pgl_force_full = 1;
    pgl_zforce_full = 1;
done:
    if (boxa) for (i = 0; i < n; ++i) free(boxa[i]);
    if (boxb) for (i = 0; i < n; ++i) free(boxb[i]);
    free(boxa); free(boxb);
    free(fa); free(fb); free(diff);
    pgl_rep_reset_stream();
    pgl_rep.state = 3;   /* replay done: stop recording */
}

static void pica_gl_flush_locked(const PicaGlHost *host, int have_raw) {
    unsigned char *pixels;
    float *zbuf = NULL;
    int need_z = 0;
    unsigned w, h;
    int x0 = pgl_drawn_x0, y0 = pgl_drawn_y0, x1 = pgl_drawn_x1, y1 = pgl_drawn_y1;
    if (!pgl_pending) return;
    pgl_pending = 0;
    if (pgl_tprof_on)
        fprintf(stderr, "PGLLK enter box=%d,%d..%d,%d rt=%dx%d\n",
                pgl_drawn_x0, pgl_drawn_y0, pgl_drawn_x1, pgl_drawn_y1,
                pgl_rt_w, pgl_rt_h);
    if (x0 > x1 || y0 > y1) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)pgl_rt_w - 1) x1 = (int)pgl_rt_w - 1;
    if (y1 > (int)pgl_rt_h - 1) y1 = (int)pgl_rt_h - 1;
    if (x0 > x1 || y0 > y1) return;
    w = (unsigned)(x1 - x0 + 1);
    h = (unsigned)(y1 - y0 + 1);
    if (pgl_wb_trace())
        fprintf(stderr, "WBTRACE #%llu why=%d box=%d,%d..%d,%d px=%u "
                "zdirty=%d rt=%ux%u\n",
                (unsigned long long)(pgl_flushes + 1u), pgl_fl_reason,
                x0, y0, x1, y1, w * h, pgl_zdirty, pgl_rt_w, pgl_rt_h);
    pixels = (unsigned char *)malloc((size_t)w * h * 4u);
    if (!pixels) return;
    {   /* t_2d934e23: read-back vs write-back split, ~2 s window */
        unsigned long long fl_t = pica_pqt_now_ns();
    pgl_glBindFramebuffer(PGGL_FRAMEBUFFER, pgl_fbo);
    pgl_obj_used |= 8u;
    pgl_glReadBuffer(PGGL_COLOR_ATTACHMENT0);
    {   /* t_ab8159de: read the f x f box and box-average it down to one texel
           per guest pixel (identity at scale 1). */
        unsigned char *red = pgl_rb_color_native(x0, y0, x1, y1);
        if (red) { memcpy(pixels, red, (size_t)w * h * 4u); free(red); }
    }
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
    /* t_7d18d1b6 diagnostic (DQ8_WB_TRACE=1, first 8 flushes): how much of the
       colour read-back is the GPU draining the frame vs. the transfer itself?
       A second identical glReadPixels right after the first measures the
       quiesced cost. */
    if (pgl_wb_trace() && pgl_fl_n < 8u) {
        static unsigned char *rb2;
        static size_t rb2_cap;
        size_t need = (size_t)w * h * 4u;
        if (rb2_cap < need) {
            unsigned char *n = (unsigned char *)realloc(rb2, need);
            if (n) { rb2 = n; rb2_cap = need; }
        }
        if (rb2) {
            unsigned long long t2 = pica_pqt_now_ns();
            pgl_glReadPixels(x0, y0, (PGsizei)w, (PGsizei)h, PGGL_RGBA,
                             PGGL_UNSIGNED_BYTE, rb2);
            pgl_drain_errors("readback-2", NULL);
            fprintf(stderr, "PGLRBTEST #%llu px=%u first_ms=%.3f second_ms=%.3f\n",
                    (unsigned long long)pgl_fl_n, w * h,
                    (double)(t2 - fl_t) / 1e6,
                    (double)(pica_pqt_now_ns() - t2) / 1e6);
        }
    }
    /* t_7d18d1b6: decide ONCE whether this flush has depth work.  A flush
       whose depth attachment only ran depth TESTS still equals guest memory
       (the refresh uploaded it), so the depth resolve is skipped; only a
       depth-WRITING draw or a depth clear invalidates those bytes. */
    need_z = pgl_zdirty && (pgl_zwrote || !pgl_zskip_on());
    pgl_zdirty = 0;
    pgl_zwrote = 0;
    if (need_z) ++pgl_fl_nz;
    pgl_frame_guard(pixels, w, h);
    pgl_frame_depth_draws = 0;
    /* t_05a3190d: optional per-frame PPM dump, used to locate the flush number
       of a battle frame for DQ8_GL_REPLAY (find a corrupted frame, replay it).
       DQ8_GL_REPLAY_FRAMES=<dir>; only whole-target flushes from an origin of
       0,0 (the presented frames) are written. */
    if (pgl_rep_env < 0) pgl_rep_setup();
    if (pgl_rep.on && pgl_rep.state == 0 && x0 == 0 && y0 == 0 &&
        (pgl_flushes % 5u) == 0u &&
        (unsigned long long)w * (unsigned long long)h * 4ull >=
        (unsigned long long)pgl_rt_w * (unsigned long long)pgl_rt_h) {
        const char *dd = getenv("DQ8_GL_REPLAY_FRAMES");
        if (dd && dd[0] && dd[0] != '0') {
            char path[768];
            snprintf(path, sizeof path, "%s/f%05llu.ppm", dd,
                     (unsigned long long)(pgl_flushes + 1u));
            pgl_rep_write_ppm(path, pixels, w, h);
        }
    }
    pgl_depth_probe("flush", x0, y0, x1, y1);
    pgl_depth_dump(x0, y0, x1, y1);
    /* t_8510204b: dump the depth timeline of the frame that just finished. */
    if (pgl_dlog_on && pgl_dlog_n) {
        unsigned k, n = pgl_dlog_n < PGL_DLOG_MAX ? pgl_dlog_n : PGL_DLOG_MAX;
        unsigned first = (pgl_dlog_n > PGL_DLOG_MAX) ? (pgl_dlog_n - PGL_DLOG_MAX) : 0u;
        fprintf(stderr, "DFRAME flush=%llu rt=%ux%u box=%d,%d..%d,%d draws=%llu "
                "keeping=%u of %u\n",
                (unsigned long long)(pgl_flushes + 1u), pgl_rt_w, pgl_rt_h,
                x0, y0, x1, y1, pgl_dlog_draws, n, pgl_dlog_n);
        for (k = 0; k < n; ++k) {
            const PglDLogEntry *e = &pgl_dlog[(first + k) % PGL_DLOG_MAX];
            fprintf(stderr, "DDRAW draw=%llu rt=%ux%u zaddr=%08X bpp=%u "
                    "on=%d t=%d w=%d f=%u zb=%d scale=%.9g off=%.9g zmax=%.0f "
                    "sync=%d box=%d,%d..%d,%d\n",
                    e->draw, e->rt_w, e->rt_h, e->depth_addr, e->bpp,
                    e->on, e->test, e->write, (unsigned)e->func, e->zbuf,
                    (double)e->scale, (double)e->off, (double)e->zmax,
                    e->syncs_since, e->box[0], e->box[1], e->box[2], e->box[3]);
        }
        pgl_dlog_n = 0;
        pgl_dlog_draws = 0;
    }
    pgl_dump_snap("color_fbo_last.raw", pixels, (size_t)w * h * 4u);
    pgl_dump_box("color_fbo_last.box", x0, y0, x1, y1);
    {
        unsigned long long fl_t1 = pica_pqt_now_ns();
        pgl_fl_read_ns += fl_t1 - fl_t;
        pgl_fl_crb_ns += fl_t1 - fl_t;
        pgl_wb_color(host, have_raw, pixels, x0, y0, x1, y1);
        pgl_fl_wb_ns += pica_pqt_now_ns() - fl_t1;
        pgl_fl_cwb_ns += pica_pqt_now_ns() - fl_t1;
        ++pgl_fl_n; pgl_fl_px += (unsigned long long)w * h;
    }
    }   /* end t_2d934e23 flush timing block */
    free(pixels);
    ++pgl_fl_nc;
    if (need_z && pgl_zwb()) {
        unsigned long long zt0 = pica_pqt_now_ns();
        ++pgl_fl_zn;
        zbuf = pgl_rb_depth(x0, y0, x1, y1);
        pgl_fl_zrb_ns += pica_pqt_now_ns() - zt0;
    } else if (need_z) {
        /* DQ8_GL_ZWB=0: the depth resolve is skipped on purpose so its cost
           can be A/B'd; the ATT1/D32F attachment keeps the frame's depth and
           the shadow snapshot is untouched, so the next refresh does not
           re-upload (correct as long as nothing outside this backend reads
           the guest depth buffer). */
        ++pgl_fl_zn; ++pgl_fl_zskip;
    }
    if (zbuf) {
        unsigned long long zt0 = pica_pqt_now_ns();
        pgl_wb_depth(host, have_raw, zbuf, w, h, x0, y0, x1, y1);
        pgl_fl_zwb_ns += pica_pqt_now_ns() - zt0;
        free(zbuf);
    }
    ++pgl_flushes;
    /* t_05a3190d: deterministic draw-stream replay A/B orchestration.  The
       frame that ENDS at flush #N is the one recorded: recording is enabled
       when flush #N-1 completes, and the replay runs at the end of flush #N. */
    if (pgl_rep_env < 0) pgl_rep_setup();
    if (pgl_rep.on && !pgl_split_on) {
        if (pgl_rep.state == 1) {
            /* The frame that just ended is a candidate: replay it when its
               render target / draw shape matches what we asked for, otherwise
               drop it and keep recording the next frame. */
            int ok_rt = !pgl_rep.want_w ||
                        (w == pgl_rep.want_w && h == pgl_rep.want_h);
            int ok_dt = !pgl_rep.want_dt ||
                        pgl_rep.ndepth_test >= pgl_rep.want_dt;
            if (ok_rt && ok_dt && pgl_rep.n) {
                pgl_rep_run();
            } else {
                if (pgl_rep.n)
                    fprintf(stderr, "GL REPLAY: flush #%llu rejected "
                                    "(box=%dx%d depth-tested draws=%u, want "
                                    "%ux%u and >=%u) — recording the next "
                                    "frame\n",
                            (unsigned long long)(pgl_flushes + 1ull), w, h,
                            pgl_rep.ndepth_test, pgl_rep.want_w,
                            pgl_rep.want_h, pgl_rep.want_dt);
                pgl_rep_reset_stream();
            }
        } else if (pgl_rep.state == 0 && pgl_flushes + 1ull >= pgl_rep.flush) {
            pgl_rep.state = 1;      /* start recording the next frame */
            pgl_rep_reset_stream();
        }
    }
    if (pgl_debug && (pgl_flushes <= 8u || (pgl_flushes % 200u) == 0u))
        fprintf(stderr, "GL debug: flush #%llu box=%d,%d..%d,%d rt=%ux%u\n",
                (unsigned long long)pgl_flushes, x0, y0, x1, y1, pgl_rt_w, pgl_rt_h);
    pgl_reset_drawn();
    /* t_fdd26eba item A: remember whose scaled content the attachment now
       holds, so a display transfer that follows this publish can inherit the
       scale (the guest publishes through the transfer AFTER it consumed the
       frame, i.e. after the flush). */
    pgl_cap_valid = 1;
    pgl_cap_addr = pica_rt_addr;
    pgl_cap_w = (int)pgl_rt_w;
    pgl_cap_h = (int)pgl_rt_h;
    pgl_cap_f = pgl_cur_scale;
    pgl_cap_draws = pgl_draws;
    pgl_cap_flushes = pgl_flushes;
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
    /* t_05281b89 stage A: a publish is the frame's content boundary — the
       target the guest just consumed (sampled as a texture) has seen its
       native pixels, so the next render into it may be scaled again.  The
       flush that a texture read triggers is exempt (pgl_keep_marks). */
    if (!pgl_keep_marks && pgl_native_n) pgl_native_n = 0;
    pgl_keep_marks = 0;
    ++pgl_flushseq;
    /* t_2d934e23: where does the deferred write-back's wall time go?  The
       EGL context is dropped at the start of every flush and re-taken below
       (pgl_context_take -> eglMakeCurrent), so measure release / take /
       read-back separately.  Gated by DQ8_XFERPROF=1. */
    if (pgl_tprof_on < 0) {
        const char *e = getenv("DQ8_XFERPROF");
        pgl_tprof_on = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    {
    unsigned long long tp0 = pica_pqt_now_ns(), tp1, tp2;
    /* Flush is the guest-visible-pixel boundary: any consumer past this point
       (GSP transfer/capture/scan-out/DMA, software draws, SDL present) may
       run foreign GL calls, so drop the context hold here. */
    pica_gl_context_release();
    tp1 = pica_pqt_now_ns();
    if (!pgl_ready || !pgl_pending || !host) { pgl_tp_pend0 += pgl_tprof_on; return; }
    if (pgl_split_on) { pgl_split_flush(host); return; }
    memset(&prev, 0, sizeof prev);
    if (!pgl_context_take(&prev)) { ++pgl_tp_takefail; return; }
    tp2 = pica_pqt_now_ns();
    pica_gl_flush_locked(host, host->rt_raw && host->rt_px_off);
    {
        unsigned long long tp3 = pica_pqt_now_ns();
        pgl_context_put(&prev);
        if (pgl_tprof_on) {
            ++pgl_tp_n;
            pgl_tp_rel_ns += tp1 - tp0;
            pgl_tp_take_ns += tp2 - tp1;
            pgl_tp_lock_ns += tp3 - tp2;
            if ((pgl_tp_n % 16u) == 0u) {
                /* t_8df70ad4: ONE PGLFLUSH line, every field accumulated by
                   the run that prints it.  t_2d934e23's release/take/lock
                   window is unchanged; the write-back half (color read-back /
                   color store / depth ATT1 read-back / depth store, plus how
                   many depth write-backs DQ8_GL_ZWB=0 skipped) is the same
                   window, so DQ8_GL_ZWB=0 vs =1 differ on the same line.  The
                   old second printer printed a counter it zeroed every flush
                   (so it never fired) and a box area labelled `px`. */
                double fn = (double)(pgl_fl_n ? pgl_fl_n : 1u);
                double zn = (double)(pgl_fl_zn ? pgl_fl_zn : 1u);
                fprintf(stderr, "PGLFLUSH n=%llu pend0=%llu takefail=%llu "
                        "release_ms=%.2f take_ms=%.2f locked_ms=%.2f "
                        "mean[rel/take/lock]=%.2f/%.2f/%.2f ms "
                        "wb_n=%llu z_n=%llu zskip=%llu zwb=%d "
                        "mean[read/col/zrb/zwb]=%.2f/%.2f/%.2f/%.2f ms\n",
                        (unsigned long long)pgl_tp_n,
                        (unsigned long long)pgl_tp_pend0,
                        (unsigned long long)pgl_tp_takefail,
                        (double)pgl_tp_rel_ns / 1e6, (double)pgl_tp_take_ns / 1e6,
                        (double)pgl_tp_lock_ns / 1e6,
                        (double)pgl_tp_rel_ns / (double)pgl_tp_n / 1e6,
                        (double)pgl_tp_take_ns / (double)pgl_tp_n / 1e6,
                        (double)pgl_tp_lock_ns / (double)pgl_tp_n / 1e6,
                        (unsigned long long)pgl_fl_n,
                        (unsigned long long)pgl_fl_zn,
                        (unsigned long long)pgl_fl_zskip,
                        pgl_zwb(),
                        (double)pgl_fl_read_ns / fn / 1e6,
                        (double)pgl_fl_wb_ns / fn / 1e6,
                        (double)pgl_fl_zrb_ns / zn / 1e6,
                        (double)pgl_fl_zwb_ns / zn / 1e6);
                /* t_7d18d1b6: colour vs depth split of the same window
                   (mean ms/flush; nz/nc = flushes that carried depth/colour
                   work; zfast/zgen = depth write-backs on the block path vs
                   the reference loop). */
                fprintf(stderr, "PGLWBSPLIT n=%llu mean_ms[c_rb/c_wb/z_rb/z_wb]="
                        "%.3f/%.3f/%.3f/%.3f nz=%llu nc=%llu zfast=%llu "
                        "zgen=%llu\n",
                        (unsigned long long)pgl_tp_n,
                        (double)pgl_fl_crb_ns / (double)pgl_tp_n / 1e6,
                        (double)pgl_fl_cwb_ns / (double)pgl_tp_n / 1e6,
                        (double)pgl_fl_zrb_ns / (double)pgl_tp_n / 1e6,
                        (double)pgl_fl_zwb_ns / (double)pgl_tp_n / 1e6,
                        (unsigned long long)pgl_fl_nz,
                        (unsigned long long)pgl_fl_nc,
                        (unsigned long long)pgl_wbz_fast,
                        (unsigned long long)pgl_wbz_gen);
                pgl_tp_n = pgl_tp_rel_ns = pgl_tp_take_ns = pgl_tp_lock_ns = 0;
                pgl_tp_pend0 = pgl_tp_takefail = 0;
                pgl_fl_n = pgl_fl_read_ns = pgl_fl_wb_ns = pgl_fl_px = 0;
                pgl_fl_zn = pgl_fl_zskip = pgl_fl_zrb_ns = pgl_fl_zwb_ns = 0;
                pgl_fl_crb_ns = pgl_fl_cwb_ns = 0;
                pgl_fl_nz = pgl_fl_nc = 0;
                pgl_wbz_fast = pgl_wbz_gen = 0;
            }
        }
    }
    }
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
            pay->c[k][2] = v->u1;
            pay->c[k][3] = v->v1;
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
    int dh_ok = 0;
    unsigned long long dh_pre = 0, dh_post = 0;
    unsigned long long dh_pre_sum = 0, dh_post_sum = 0;
    int dh_zok = 0;
    unsigned long long dh_zpre = 0, dh_zpost = 0, dh_zpre_sum = 0, dh_zpost_sum = 0;
    unsigned long long ph;
    if (!pgl_ready || !ntri || !host || !host->rt_get || !host->rt_put) return 0;
    if (!st->rt_w || !st->rt_h) return 0;
    if (pgl_dlog_on < 0) {
        const char *e = getenv("DQ8_GL_DEPTHLOG");
        pgl_dlog_on = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    if (st->mode == PICA_GL_TEV && st->tev.tex_en && !st->tex_is_rt &&
        (!st->tex_rgba || st->tex_w <= 0 || st->tex_h <= 0)) return 0;
    /* t_05a3190d: deterministic draw-stream replay A/B — record this draw. */
    if (pgl_rep_env < 0) pgl_rep_setup();
    if (pgl_rep.state == 1) pgl_rep_record(verts, nverts, st, host);
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
    {   /* t_2d934e23: ens/ref split of the per-draw CPU setup */
        unsigned long long ph_ens0 = pica_pqt_now_ns();
        unsigned long long ph_e;
        /* A foreign GL user sharing this thread may have deleted our objects
           between draws; rebuild before using them (no-op in the normal case). */
        if (!pgl_ensure_objects()) return 0;
        /* A pending episode must be written back before the FBO is re-sized to
           a different render target (its content would be lost).  Under the
           split the GUEST thread owns pgl_pending/shadow dims and closes the
           episode at enqueue (blocking flush) before the size can change, so
           the worker never evaluates this condition. */
        if (!pgl_split_cur && pgl_pending &&
            (pgl_shadow_w != st->rt_w || pgl_shadow_h != st->rt_h))
            pica_gl_flush_locked(host, 1);
        if (!pgl_ensure_rt((int)st->rt_w, (int)st->rt_h)) return 0;
        ph_e = pica_pqt_now_ns();
        pgl_ph_ens_ns += ph_e - ph_ens0; pgl_ph_ref_from = ph_e;
    }
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
        int pgl_need = pgl_refr_needed(st, host, &raw, &len);
        if (pgl_need < 0) return 0;
        if (pgl_need) {
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
        {
            uint64_t pf_r0 = pf_trap_armed() ? pica_pqt_now_ns() : 0;
            if (!pgl_refresh(st, host, raw, len)) return 0;
            if (pf_r0) pf_shim(PF_SH_REFRESH, pica_pqt_now_ns() - pf_r0);
        }
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
            pgl_zshadow_src = zraw;
        }
        pgl_guest_dirty = 0;
        pgl_shadow_src = raw;
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

    pgl_ph_ref_ns += pica_pqt_now_ns() - pgl_ph_ref_from;
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
        pgl_ph_box_px += (unsigned long long)bw * bh;
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
    /* t_ab8159de: the supersampled target.  The vertex shader keeps mapping
       guest coordinates through u_rt (guest dims), so guest pixel p lands at
       target texel p*f — the mapping is exact for every scale. */
    pgl_glViewport(0, 0, (PGsizei)pgl_tgt_w, (PGsizei)pgl_tgt_h);
    /* t_993e6fb6: colour mask on ATT0 only; ATT1 (depth mirror) follows
       depth_on, never the PICA colour mask. */
    pgl_apply_color_mask(st);
    if (st->depth_on && st->color_mask != 0xFu) ++pgl_draws_cmask_depth;
    /* t_a22413f1: hardware depth test/write state for this draw. */
    pgl_apply_depth_state(st);
    /* t_8510204b: record the draw's depth timeline entry. */
    if (pgl_dlog_on) {
        PglDLogEntry *e = &pgl_dlog[pgl_dlog_n % PGL_DLOG_MAX];
        e->draw = (unsigned long long)pgl_draws;
        e->rt_w = st->rt_w; e->rt_h = st->rt_h;
        e->depth_addr = pica_depth_addr;
        e->bpp = st->depth_bpp;
        e->test = st->depth_test; e->write = st->depth_write;
        e->func = (int)st->depth_func; e->zbuf = st->depth_zbuf;
        e->on = st->depth_on;
        e->scale = st->depth_scale; e->off = st->depth_offset;
        e->zmax = st->depth_zmax;
        e->box[0] = bx0; e->box[1] = by0; e->box[2] = bx1; e->box[3] = by1;
        e->syncs_since = (int)pgl_dlog_syncs;
        pgl_dlog_syncs = 0;
        ++pgl_dlog_n;
        ++pgl_dlog_draws;
    }
    if (pgl_blend_enabled != 0) {   /* shader blend: fixed-function off */
        pgl_glDisable(PGGL_BLEND);
        pgl_blend_enabled = 0;
    }
    if (gl_blend) {
        /* t_77ac52ce: expressible PICA blend -> fixed-function GL blending.
           No u_dst copy, no per-fragment blend math, and no pipeline drain
           from reading the render target back into a texture.
           t_d559a510: the state is PERSISTENT, so it is only (re)applied when
           the PICA configuration changes — llvmpipe revalidates the rasteriser
           variant on every per-buffer blend call. */
        unsigned key[6];
        int same, k, need;
        key[0] = st->blend_src_rgb; key[1] = st->blend_dst_rgb;
        key[2] = st->blend_src_a;   key[3] = st->blend_dst_a;
        key[4] = st->blend_eq_rgb;  key[5] = st->blend_eq_a;
        same = (pgl_blend_applied == 1);
        for (k = 0; k < 6 && same; ++k)
            if (pgl_blend_key[k] != key[k]) same = 0;
        for (k = 0; k < 4 && same; ++k)
            if (pgl_blend_key_const[k] != st->blend_const[k]) same = 0;
        /* The depth guard only has to be re-established after a non-indexed
           blend call (those re-arm every draw buffer) — once it is in place,
           draws that do not change the colour blend state need no GL call. */
        need = !same || (pgl_blend_depth_guard && !pgl_blend_guard_armed);
        if (need) {
            if (!same) {
                pgl_glBlendFuncSeparate(bl_f[0], bl_f[1], bl_f[2], bl_f[3]);
                pgl_glBlendEquationSeparate(bl_eq[0], bl_eq[1]);
                pgl_glBlendColor(st->blend_const[0], st->blend_const[1],
                                 st->blend_const[2], st->blend_const[3]);
                pgl_blend_guard_armed = 0;  /* non-indexed call re-armed all */
            }
            if (pgl_blend_depth_guard) {
                /* t_d559a510: COLOR_ATTACHMENT1 is the emulated depth buffer,
                   not a colour target — it must be REPLACED with frag_z, never
                   blended against the old depth.  Per-buffer blend state:
                   index 1 gets (ONE, ZERO, ADD), which is exactly a replace.
                   (GL 4.5 §17.3.3: blend state is per draw buffer, but the
                   non-indexed glBlendFuncSeparate/glBlendEquationSeparate are
                   defined for EVERY draw buffer, so this override has to be
                   re-established after them.) */
                if (pgl_glBlendFunci)
                    pgl_glBlendFunci(1u, PGGL_ONE, PGGL_ZERO);
                if (pgl_glBlendEquationi)
                    pgl_glBlendEquationi(1u, PGGL_FUNC_ADD);
                pgl_blend_guard_armed = 1;
            }
            for (k = 0; k < 6; ++k) pgl_blend_key[k] = key[k];
            for (k = 0; k < 4; ++k) pgl_blend_key_const[k] = st->blend_const[k];
            pgl_blend_applied = 1;
            ++pgl_blend_state_sets;
        }
        if (pgl_blend_depth_guard) ++pgl_blend_gl_depthfix;
        if (pgl_blend_enabled != 1) {
            pgl_glEnable(PGGL_BLEND);
            pgl_blend_enabled = 1;
        }
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
        /* t_a22413f1 run3: in HARDWARE mode the real depth attachment does the
           test/write, so no per-draw depth-source copy is needed.  IN-SHADER
           mode (the default since the operator's Radeon regression) is the
           pre-fix behaviour and needs the copy exactly when the shader reads
           the old z: the test runs, or the depth write is off. */
        zdst_need = pgl_depth_hw ? 0
                                 : (st->depth_on && (st->depth_test || !st->depth_write));
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
        if (pgl_dst_w != pgl_tgt_w || pgl_dst_h != pgl_tgt_h) {
            /* t_ab8159de: same-size-as-the-target copy; the shader samples it
               at the supersampled coordinate minus the scaled box origin. */
            pgl_glTexImage2D(PGGL_TEXTURE_2D, 0, PGGL_RGBA8, pgl_tgt_w,
                             pgl_tgt_h, 0, PGGL_RGBA, PGGL_UNSIGNED_BYTE,
                             NULL);
            pgl_dst_w = pgl_tgt_w;
            pgl_dst_h = pgl_tgt_h;
            pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_MIN_FILTER, PGGL_NEAREST);
            pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_MAG_FILTER, PGGL_NEAREST);
            pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_WRAP_S, PGGL_CLAMP_TO_EDGE);
            pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_WRAP_T, PGGL_CLAMP_TO_EDGE);
        }
        pgl_glCopyTexSubImage2D(PGGL_TEXTURE_2D, 0, 0, 0,
                                (PGint)PGL_S(bx0), (PGint)PGL_S(by0),
                                (PGsizei)PGL_S(bw), (PGsizei)PGL_S(bh));
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
            if (pgl_zdst_w != pgl_tgt_w || pgl_zdst_h != pgl_tgt_h) {
                pgl_glTexImage2D(PGGL_TEXTURE_2D, 0, PGGL_R32F, pgl_tgt_w,
                                 pgl_tgt_h, 0, PGGL_RED, PGGL_FLOAT, NULL);
                pgl_zdst_w = pgl_tgt_w;
                pgl_zdst_h = pgl_tgt_h;
                pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_MIN_FILTER, PGGL_NEAREST);
                pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_MAG_FILTER, PGGL_NEAREST);
            }
            pgl_glReadBuffer(PGGL_COLOR_ATTACHMENT1);
            pgl_glCopyTexSubImage2D(PGGL_TEXTURE_2D, 0, 0, 0,
                                    (PGint)PGL_S(bx0), (PGint)PGL_S(by0),
                                    (PGsizei)PGL_S(bw), (PGsizei)PGL_S(bh));
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
    /* t_5ddc492f: pick the fragment-shader variant for this draw's shape and
       upload the uniforms into it.  dst_used/zdst_used are exactly the
       expressions the uniform block used below. */
    {
        int dst_used, zdst_used;
        if (pgl_fbfetch) {
            dst_used = (dst_read && !(pgl_skip_mask & PGL_SK_DST)) ? 1 : 0;
            zdst_used = (zdst_need && !(pgl_skip_mask & PGL_SK_ZDST)) ? 1 : 0;
        } else {
            dst_used = do_dst ? 1 : 0;
            zdst_used = do_zdst ? 1 : 0;
        }
        pgl_cur_prog = pgl_spec_select(st, gl_blend, dst_used, zdst_used);
        pgl_cur_dst_used = dst_used;
        pgl_cur_zdst_used = zdst_used;
        /* t_f8f2c1da: second half of the tripwire — the program about to be
           used must have been generated for the CURRENT depth path. */
        pgl_spec_modecheck_use(pgl_cur_prog, "draw/use");
        pgl_prog_use(pgl_cur_prog);
        pgl_set_uniforms(st, gl_blend, dst_used, zdst_used, (PGint)bx0,
                         (PGint)by0);    }
    pgl_drain_errors("use-program", NULL);
    if (st->mode == PICA_GL_TEV && st->tev.tex_en && st->tex_is_rt &&
        pgl_snap_tex) {
        /* t_dd830a74 item 2: unit 0 is the current render target sampled at the
           internal scale — bind the full-resolution snapshot (pica_gl.c
           pica_gl_sample_scaled_target) instead of a staged native image.
           Normalised UVs are unchanged, so the guest's texture coordinates
           need no scaling; the wrap/filter state is the guest's, exactly like
           the staged path. */
        pgl_glActiveTexture(PGGL_TEXTURE0);
        pgl_glBindTexture(PGGL_TEXTURE_2D, pgl_snap_tex);
        pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_WRAP_S,
                            (PGint)pgl_wrap_enum(st->wrap_s));
        pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_WRAP_T,
                            (PGint)pgl_wrap_enum(st->wrap_t));
        pgl_tex_filter_apply(st, 0);
        pgl_glUniform1i(pgl_u_tex, 0);
        pgl_diag_post_want = 1;   /* diagnosis only; 0 unless DQ8_RS_SNAP_DUMP */
        if (pgl_debug) {
            float u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f;
            unsigned k;
            for (k = 0; k < nverts; ++k) {
                if (verts[k].u < u0) u0 = verts[k].u;
                if (verts[k].u > u1) u1 = verts[k].u;
                if (verts[k].v < v0) v0 = verts[k].v;
                if (verts[k].v > v1) v1 = verts[k].v;
            }
            fprintf(stderr, "GL debug: target-sample draw #%llu uv=[%.4f..%.4f]x"
                            "[%.4f..%.4f] tex=%ux%u snap=%dx%d target=%ux%u\n",
                    (unsigned long long)pgl_draws, u0, u1, v0, v1,
                    st->tex_w, st->tex_h, pgl_snap_w, pgl_snap_h,
                    pgl_tgt_w, pgl_tgt_h);
            for (k = 0; k < nverts && k < 6u; ++k)
                fprintf(stderr, "GL debug:   v%u xy=(%.1f,%.1f) uv=(%.4f,%.4f)\n",
                        k, verts[k].x, verts[k].y, verts[k].u, verts[k].v);
        }
    } else if (st->mode == PICA_GL_TEV && st->tev.tex_en) {
        if (pgl_diag_stage_want) {
            pgl_diag_stage_want = 0;
            pgl_diag_stage_dump(st->tex_rgba, st->tex_w, st->tex_h);
            pgl_diag_post_want = 1;
        }
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
        /* t_c043d7f7: filter state is per draw (the upload cache is keyed on
           the image, not on the sampler bits), so it is applied on every
           bind, after the wrap parameters. */
        pgl_tex_filter_apply(st, 0);
        pgl_glUniform1i(pgl_u_tex, 0);
    }
    /* t_e216fc37: upload + bind PICA texture unit 1 when this draw's TEV chain
       reads it (pica_gl_st.tex1_* is filled by the unit-1 staging cache). */
    if (st->mode == PICA_GL_TEV && st->tex1_en && st->tex1_rgba &&
        st->tex1_w > 0 && st->tex1_h > 0) {
        unsigned si = (st->tex1_slot < (unsigned)PGL_TEX1_SLOTS)
                          ? st->tex1_slot : 0u;
        PglTex1Slot *s1 = &pgl_tex1[si];
        if (!s1->tex) pgl_glGenTextures(1, &s1->tex);
        s1->used = 1;
        pgl_glActiveTexture(PGGL_TEXTURE3);
        pgl_glBindTexture(PGGL_TEXTURE_2D, s1->tex);
        pgl_obj_used |= 4096u;
        if (s1->gen != st->tex1_gen || s1->ptr != st->tex1_rgba ||
            s1->w != st->tex1_w || s1->h != st->tex1_h) {
            pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_MIN_FILTER, PGGL_NEAREST);
            pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_MAG_FILTER, PGGL_NEAREST);
            if (s1->gen && s1->w == st->tex1_w && s1->h == st->tex1_h &&
                s1->ptr == st->tex1_rgba) {
                pgl_glTexSubImage2D(PGGL_TEXTURE_2D, 0, 0, 0, st->tex1_w,
                                    st->tex1_h, PGGL_RGBA, PGGL_UNSIGNED_BYTE,
                                    st->tex1_rgba);
            } else {
                pgl_glTexImage2D(PGGL_TEXTURE_2D, 0, PGGL_RGBA8, st->tex1_w,
                                 st->tex1_h, 0, PGGL_RGBA, PGGL_UNSIGNED_BYTE,
                                 st->tex1_rgba);
            }
            s1->ptr = st->tex1_rgba;
            s1->w = st->tex1_w;
            s1->h = st->tex1_h;
            s1->gen = st->tex1_gen;
            ++pgl_tex1_uploads;
        }
        pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_WRAP_S,
                            (PGint)pgl_wrap_enum(st->wrap_s1));
        pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_WRAP_T,
                            (PGint)pgl_wrap_enum(st->wrap_t1));
        pgl_tex_filter_apply(st, 1);
        pgl_glActiveTexture(PGGL_TEXTURE0);
        if (pgl_u_tex1 >= 0) pgl_glUniform1i(pgl_u_tex1, 3);
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
            pgl_glVertexAttribPointer(3 + 3u * i, 4, PGGL_FLOAT, 0,
                                      sizeof(PicaGlTriPayload),
                                      (const void *)(base + 96u + (size_t)i * 16u));
            pgl_glVertexAttribDivisor(3 + 3u * i, 1);
        }
        ph_t4 = pica_pqt_now_ns(); /* t_8c05fac8: before the draw submit */
        /* t_6b81015d: make earlier draws' RT writes visible to this draw's
           imageLoad of the destination/depth (GL 4.5 §9.3).  Only needed when
           the shader actually consumes the live image and only once per run of
           writing draws. */
        if (pgl_fbfetch && (dst_read || zdst_need))
            pgl_dst_barrier(bx0, by0, bx1, by1);
        if (pgl_drawhash_on < 0) {
            const char *e = getenv("DQ8_GL_DRAWHASH");
            pgl_drawhash_on = (e && e[0] && e[0] != '0') ? 1 : 0;
        }
        if (pgl_drawhash_on && bw && bh) {
            int dhbox[4];
            dhbox[0] = bx0; dhbox[1] = by0; dhbox[2] = bx1; dhbox[3] = by1;
            dh_ok = pgl_dh_box(dhbox, &dh_pre, &dh_pre_sum);
            pgl_dh_pre_asum = pgl_dh_asum;
            if (st->depth_on)
                dh_zok = pgl_dh_zbox(dhbox, &dh_zpre, &dh_zpre_sum);
            if (pgl_dh_dump_idx == -2) {
                const char *e = getenv("DQ8_GL_DRAWHASH_DUMP");
                pgl_dh_dump_idx = (e && *e) ? strtoll(e, NULL, 0) : -1;
            }
            if (dh_ok && pgl_dh_dump_idx >= 0 &&
                pgl_dh_dump_idx == (long long)pgl_draws) {
                pgl_dh_pre_len = (size_t)bw * bh * 4u;
                pgl_dh_pre_saved = (unsigned char *)realloc(pgl_dh_pre_saved,
                                                            pgl_dh_pre_len);
                if (pgl_dh_pre_saved)
                    memcpy(pgl_dh_pre_saved, pgl_dh_buf, pgl_dh_pre_len);
            }
        }
        if (!(pgl_skip_mask & PGL_SK_GEO)) {
            /* t_150574cd face culling: NO glEnable(GL_CULL_FACE) here, by
               design.  This backend is not a fixed-function triangle
               rasteriser: every instance is ONE PICA triangle whose vertices
               are placed at their real screen positions (VS: ndc = p/u_rt*2-1)
               and the fragment shader does the PICA edge tests itself, so a
               GL front-face test would be a second, differently-defined
               winding test on the same geometry.  The guest's GPUREG 0x40
               mode is instead applied once, in pica_vs.c pica_queue_triangle()
               (pica_cull_keep()), on the triangles both backends consume, so
               `ntri` here is already the guest's kept-face set and the
               hardware and software paths cannot disagree. */
            pgl_glDrawArraysInstanced(PGGL_TRIANGLES, 0, 3, (PGsizei)ntri);
            if (pgl_diag_post_want) { pgl_diag_post_want = 0; pgl_diag_post_dump(); }
            /* t_fa78ea1f: per-draw render-target census (DQ8_GL_DRAWDUMP).
               Reads the whole attachment back after each draw of the window
               and prints the dark share of the image plus a 4x4 grid, so a
               frame that loses the UI can be attributed to the exact draw
               (or shows that the draws all ran and something else erased
               them).  Diagnostic only; llvmpipe/radeonsi cost ~1-3 ms/draw. */
            {
                static int dd_on = -1;
                static long long dd_lo, dd_hi;
                if (dd_on < 0) {
                    const char *e = getenv("DQ8_GL_DRAWDUMP");
                    dd_on = (e && e[0] && e[0] != '0') ? 1 : 0;
                    dd_lo = 0; dd_hi = 1LL << 50;
                    if (dd_on && e[0] >= '0' && e[0] <= '9') {
                        dd_lo = strtoll(e, NULL, 0);
                        dd_hi = strchr(e, '-') ? strtoll(strchr(e, '-') + 1, NULL, 0)
                                               : dd_lo;
                    }
                }
                if (dd_on && (long long)pgl_draws >= dd_lo &&
                    (long long)pgl_draws <= dd_hi && st->rt_w && st->rt_h) {
                    size_t dd_len = (size_t)st->rt_w * st->rt_h * 4u;
                    unsigned char *dd_buf = (unsigned char *)malloc(dd_len);
                    if (dd_buf) {
                        unsigned dd_gx, dd_gy, dd_n = 0, dd_dark = 0;
                        unsigned dd_g[16];
                        pgl_glReadBuffer(PGGL_COLOR_ATTACHMENT0);
                        pgl_glReadPixels(0, 0, (PGsizei)st->rt_w, (PGsizei)st->rt_h,
                                         PGGL_RGBA, PGGL_UNSIGNED_BYTE, dd_buf);
                        memset(dd_g, 0, sizeof dd_g);
                        for (dd_gy = 0; dd_gy < st->rt_h; dd_gy += 8u)
                            for (dd_gx = 0; dd_gx < st->rt_w; dd_gx += 8u) {
                                const unsigned char *q =
                                    dd_buf + ((size_t)dd_gy * st->rt_w + dd_gx) * 4u;
                                unsigned m = q[0] > q[1] ? (q[0] > q[2] ? q[0] : q[2])
                                                         : (q[1] > q[2] ? q[1] : q[2]);
                                unsigned cell = ((dd_gy * 4u) / st->rt_h) * 4u +
                                                ((dd_gx * 4u) / st->rt_w);
                                ++dd_n;
                                if (m <= 16u) { ++dd_dark; ++dd_g[cell]; }
                            }
                        fprintf(stderr, "DDUMP n=%llu dark=%.1f g=",
                                (unsigned long long)pgl_draws,
                                dd_n ? 100.0 * (double)dd_dark / (double)dd_n : -1.0);
                        for (dd_gx = 0; dd_gx < 16u; ++dd_gx)
                            fprintf(stderr, "%s%u", dd_gx ? "," : "", dd_g[dd_gx]);
                        fputc(10, stderr);
                        free(dd_buf);
                    }
                }
            }
            /* t_8510204b: per-draw probe pixels.  For a pixel inside a
               disputed region this prints, for every draw that changes its
               colour or depth, the value BEFORE/AFTER and the PICA depth state
               the draw ran with — the attribution needed to explain why the
               hardware path keeps geometry the in-shader path rejects.
               DQ8_GL_PROBE_PX="x,y[;x,y]" (GL/render-target coords),
               DQ8_GL_PROBE_DRAW="lo-hi" restricts the window. */
            {
                static int ppr_n = -1;
                static int ppr_px[8][2], ppr_lo, ppr_hi;
                static unsigned char ppr_col[8][4];
                static float ppr_z[8];
                static float ppr_az[8];   /* t_8510204b: previous ATT1 z */
                static int ppr_init;
                if (ppr_n < 0) {
                    const char *e = getenv("DQ8_GL_PROBE_PX");
                    const char *w2 = getenv("DQ8_GL_PROBE_DRAW");
                    ppr_n = 0;
                    ppr_lo = ppr_hi = -1;
                    if (w2 && w2[0]) {
                        ppr_lo = (int)strtol(w2, NULL, 0);
                        ppr_hi = (strchr(w2, '-') != NULL)
                                 ? (int)strtol(strchr(w2, '-') + 1, NULL, 0)
                                 : ppr_lo;
                    }
                    while (e && *e && ppr_n < 8) {
                        int x = (int)strtol(e, NULL, 0);
                        char *c = strchr(e, ',');
                        int y = c ? (int)strtol(c + 1, NULL, 0) : 0;
                        ppr_px[ppr_n][0] = x; ppr_px[ppr_n][1] = y;
                        ++ppr_n;
                        c = c ? strchr(c + 1, ';') : NULL;
                        e = c ? c + 1 : NULL;
                    }
                }
                if (ppr_n > 0 &&
                    (long long)pgl_draws >= (long long)ppr_lo &&
                    (long long)pgl_draws <= (long long)ppr_hi) {
                    int k;
                    for (k = 0; k < ppr_n; ++k) {
                        unsigned char cb[4] = {0, 0, 0, 0}, c2[4] = {0, 0, 0, 0};
                        float z2 = 0.0f, az = 0.0f;
                        pgl_glReadBuffer(PGGL_COLOR_ATTACHMENT0);
                        pgl_glReadPixels(ppr_px[k][0], ppr_px[k][1], 1, 1,
                                         PGGL_RGBA, PGGL_UNSIGNED_BYTE, cb);
                        if (pgl_depth_hw) {
                            pgl_glReadPixels(ppr_px[k][0], ppr_px[k][1], 1, 1,
                                             PGGL_DEPTH_COMPONENT, PGGL_FLOAT, &z2);
                            pgl_glReadBuffer(PGGL_COLOR_ATTACHMENT1);
                            pgl_glReadPixels(ppr_px[k][0], ppr_px[k][1], 1, 1,
                                             PGGL_RED, PGGL_FLOAT, &az);
                            pgl_glReadBuffer(PGGL_COLOR_ATTACHMENT0);
                            /* t_8510204b: the two depth surfaces must agree.
                               A divergence means the D32F attachment the
                               rasterizer tests is not the depth the guest
                               mirror holds (the shadow/resync disagreement). */
                            if ((double)z2 * 16777216.0 - (double)az > 16.0 ||
                                (double)az - (double)z2 * 16777216.0 > 16.0)
                                fprintf(stderr, "GL ZDIVERGE d=%llu px=%d,%d "
                                        "attach_z=%.1f att1_z=%.1f col=%u,%u,%u\n",
                                        (unsigned long long)pgl_draws,
                                        ppr_px[k][0], ppr_px[k][1],
                                        (double)z2 * 16777216.0, (double)az,
                                        cb[0], cb[1], cb[2]);
                        }
                        c2[0] = cb[0]; c2[1] = cb[1]; c2[2] = cb[2]; c2[3] = cb[3];
                        if (!ppr_init ||
                            memcmp(c2, ppr_col[k], 4) != 0 || z2 != ppr_z[k]) {
                            /* t_8510204b: zprev/znew + the draw's PICA compare
                               test whether the rasterizer's keep/reject
                               decision is consistent with PICA's integer
                               compare on the very same numbers.  A "passed"
                               colour change with !(znew <cmp> zprev) is the
                               hardware path accepting a fragment the PICA
                               compare rejects. */
                            fprintf(stderr,
                                "GL PRB d=%llu px=%d,%d col=%u,%u,%u,%u "
                                "z=%.9f zprev=%.1f znew=%.1f on=%d test=%d "
                                "wr=%d fn=%d zb=%d sc=%g off=%g zmax=%g "
                                "box=%d,%d..%d,%d bpp=%u\n",
                                (unsigned long long)pgl_draws, ppr_px[k][0],
                                ppr_px[k][1], cb[0], cb[1], cb[2], cb[3],
                                (double)z2, (double)ppr_az[k], (double)az,
                                st->depth_on, st->depth_test,
                                st->depth_write, (int)st->depth_func,
                                st->depth_zbuf, (double)st->depth_scale,
                                (double)st->depth_offset, (double)st->depth_zmax,
                                bx0, by0, bx1, by1, st->depth_bpp);
                            memcpy(ppr_col[k], c2, 4);
                            ppr_z[k] = z2;
                        }
                        ppr_az[k] = az;
                    }
                    ppr_init = 1;
                }
            }
            /* t_5ddc492f: DQ8_GL_SPEC_VERIFY=n - re-render this draw with the
               generic program and compare the box hashes. */
            pgl_spec_verify(st, pgl_cur_prog, gl_blend, pgl_cur_dst_used,
                            pgl_cur_zdst_used, bx0, by0, bx1, by1, ntri);
        }
        if (ntri) {
            pgl_rt_wrote = 1;
            pgl_wrect_add(bx0, by0, bx1, by1);
        }
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
    if (dh_ok) {
        int dhbox[4];
        dhbox[0] = bx0; dhbox[1] = by0; dhbox[2] = bx1; dhbox[3] = by1;
        if (pgl_dh_box(dhbox, &dh_post, &dh_post_sum)) {
            if (dh_zok)
                dh_zok = pgl_dh_zbox(dhbox, &dh_zpost, &dh_zpost_sum);
            fprintf(stderr, "GLHASH draw=%llu tris=%u mode=%d blend=%d glb=%d "
                    "src=%u,%u dst=%u,%u eq=%u,%u a=%d/%u/%.0f mask=%X "
                    "depth=%d/%d/%d box=%d,%d..%d,%d pre=%016llx post=%016llx "
                    "pre_sum=%llu post_sum=%llu zpre=%016llx zpost=%016llx "
                    "zpre_sum=%llu zpost_sum=%llu asum=%llu/%llu\n",
                    (unsigned long long)pgl_draws, ntri, st->mode, st->blend,
                    gl_blend, st->blend_src_rgb, st->blend_src_a,
                    st->blend_dst_rgb, st->blend_dst_a, st->blend_eq_rgb,
                    st->blend_eq_a, st->alpha_test, st->alpha_func,
                    (double)st->alpha_ref, st->color_mask, st->depth_test,
                    st->depth_write, st->depth_on, bx0, by0, bx1, by1,
                    dh_pre, dh_post, dh_pre_sum, dh_post_sum,
                    dh_zpre, dh_zpost, dh_zpre_sum, dh_zpost_sum,
                    pgl_dh_pre_asum, pgl_dh_asum);
            if (pgl_dh_dump_idx >= 0 &&
                pgl_dh_dump_idx == (long long)pgl_draws && pgl_dh_pre_saved) {
                unsigned dw = (unsigned)(bx1 - bx0 + 1), dh = (unsigned)(by1 - by0 + 1);
                pgl_dh_write_ppm("pre", pgl_dh_pre_saved, dw, dh);
                pgl_dh_write_ppm("post", pgl_dh_buf, dw, dh);
            }
        }
    }
    ph = pica_pqt_now_ns();
    ++pgl_ph_n;
    pgl_ph_setup_ns += ph_t1 - ph_t0;
    pgl_ph_copy_ns += ph_t2 - ph_t1;
    pgl_ph_draw_ns += ph - ph_t2;
    pgl_ph_u_ns += ph_t3 - ph_t2;
    pgl_ph_vbo_ns += ph_t4 - ph_t3;
    pgl_ph_sub_ns += ph - ph_t4;
    /* t_5ddc492f: per-shape histogram + per-variant timing (GLPHASE) */
    {
        unsigned long long dns = ph - ph_t4;
        unsigned long long px = (unsigned long long)bw * bh;
        pgl_shape_add(pgl_shape_mask(st, gl_blend, pgl_cur_dst_used,
                                     pgl_cur_zdst_used), px, dns);
        if (pgl_cur_prog) {
            ++pgl_cur_prog->draws;
            pgl_cur_prog->px += px;
            pgl_cur_prog->ns += dns;
            ++pgl_spec_draws;
        }
    }
    pgl_ph_report();
    if (st->depth_on && !pgl_split_cur) {
        pgl_zdirty = 1;
        if (st->depth_write) pgl_zwrote = 1;
    }

    /* t_05281b89: stage A coverage accounting (see pgl_cov_mark). */
    pgl_tgtstat_add((int)st->rt_w, (int)st->rt_h, pgl_cur_scale);
    pgl_cov_mark((int)st->rt_w, (int)st->rt_h, bx0, by0, bx1, by1,
                 pgl_cur_scale > 1);
    if (pgl_cur_scale > 1) {
        ++pgl_cov_draws_scaled;
        if (bw && bh) pgl_cov_px_scaled += (unsigned long long)bw * bh;
    } else {
        ++pgl_cov_draws_native;
        if (bw && bh) pgl_cov_px_native += (unsigned long long)bw * bh;
    }

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
    /* t_599a43f7: per-frame evidence that the hardware depth path was asked to
       do per-fragment depth work (the frame guard needs it to tell the game's
       own fade-to-black from a depth path that rejects every fragment). */
    if (pgl_depth_hw && st->depth_on && st->depth_test && ntri)
        ++pgl_frame_depth_draws;
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
   queue: clear colour, changed 8x8 block RGBA bytes, or a full decode.

   t_6f9e3fed: the capture now uses the SAME guest-write gate as the
   synchronous path (t_477973bb, pgl_refr_needed) instead of memcmp'ing the
   whole colour surface on every enqueue.  Measured on the field scene, the
   ungated split path paid ref_ms 134.9/s versus 33.2/s for the gated
   synchronous path — i.e. the async path was paying for the very compare the
   synchronous path had just removed, which is most of why DQ8_SPLIT_GL=1 did
   not pay off.  pgl_refr_needed() also arms the guest-write watch ranges
   (pgl_watch_lo/hi, pgl_zwatch_lo/hi), so a guest store into the render
   target still raises pgl_guest_dirty and forces the compare next enqueue:
   the gate removes the compare, not the detection.
   DQ8_REFRESH_DIRTY=0 restores the per-enqueue compare for both paths. */
static int pgl_split_capture_refresh(PglSplitOp *op, const PicaGlState *st,
                                     const PicaGlHost *host) {
    const unsigned char *raw = NULL;
    unsigned len = 0, bpp = st->rt_bpp ? st->rt_bpp : 4u;
    int all, need;
    need = pgl_refr_needed(st, host, &raw, &len);
    if (need < 0) return 0;
    op->ref_kind = PGL_REF_NONE;
    op->zref_kind = PGL_ZREF_NONE;
    if (!need) {              /* guest did not touch the RT: nothing to sync */
        ++pgl_scap_skip;
        pgl_guest_dirty = 0;
        return 1;
    }
    ++pgl_scap_need;
    if (pgl_force_full) ++pgl_scap_ff;
    if (pgl_guest_dirty) ++pgl_scap_gd;
    all = pgl_force_full;
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
    pgl_shadow_src = raw;   /* t_6f9e3fed: the gate above needs this */
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
        pgl_zshadow_src = zraw;   /* t_6f9e3fed: gate state for the z region */
    }
    pgl_guest_dirty = 0;   /* the guest bytes above have been consumed */
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
        pgl_clear_color_only(op->ref_col[0] / 255.0f, op->ref_col[1] / 255.0f,
                             op->ref_col[2] / 255.0f, op->ref_col[3] / 255.0f);
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
            pgl_upload_rgba((int)(bx * 8u), y0, 8, 8,
                            op->ref_blob + (size_t)i * 256u);
        }
        pgl_drain_errors("rt-subupload", NULL);
        break;
    }
    case PGL_REF_FULL:
        pgl_specify_rgba((int)st->rt_w, (int)st->rt_h, op->ref_full);
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
            /* t_a22413f1: real depth attachment, same value scaled.
               t_599a43f7 — ROOT CAUSE of the operator's black screen: this
               worker-thread twin of the z-clear used to call glClearBufferfv
               directly, WITHOUT forcing the depth write mask.  The previous
               draw leaves glDepthMask(0) behind (PICA depth-write-off), and
               glClearBufferfv on a depth attachment is gated by that mask, so
               the frame's depth clear cleared NOTHING: the attachment kept the
               stale depth, every fragment failed GL_LESS and the 3D frame came
               out ~99% dark with DQ8_SPLIT_GL=1 + DQ8_GL_DEPTH_ATTACH=1, while
               the same build with DQ8_SPLIT_GL=0 rendered fine.  Both threads
               now go through pgl_depth_attachment_clear(). */
            if (pgl_depth_hw) {
                pgl_depth_attachment_clear(
                    (float)op->zref_z * (1.0f / 16777216.0f));
                ++pgl_depth_syncs;   /* the frame guard needs this on the worker */
            }
            if (pgl_drain_errors("z-clear", NULL) != 0)
                pgl_request_resync(); /* guest re-sends a full z refresh */
        }
        break;
    case PGL_ZREF_FULL:
        pgl_specify_r32f((int)st->rt_w, (int)st->rt_h, op->zref_full);
        /* t_a22413f1: mirror into the real depth attachment (scaled). */
        if (pgl_depth_hw && op->zref_full) {
            size_t n = (size_t)st->rt_w * st->rt_h, k;
            float *scaled = (float *)malloc(n * sizeof(float));
            if (!scaled) { pgl_request_resync(); break; }
            for (k = 0; k < n; ++k) scaled[k] = op->zref_full[k] * (1.0f / 16777216.0f);
            pgl_specify_d32f((int)st->rt_w, (int)st->rt_h, scaled);
            free(scaled);
            ++pgl_depth_syncs;
        }
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
    /* pgl_draws is incremented by draw_locked itself (worker-side). */
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
    {   /* t_ab8159de: scaled read + box average (identity at scale 1). */
        unsigned char *red = pgl_rb_color_native(x0, y0, x1, y1);
        if (red) { memcpy(op->fl_px, red, (size_t)w * h * 4u); free(red); }
    }
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
    /* t_599a43f7: the frame guard must exist on the SPLIT path too.  It used
       to be called only from the synchronous flush, so the operator — who runs
       DQ8_SPLIT_GL=1 — had no black-screen defence at all.  Runs on the worker
       (the thread that owns the rendering context). */
    pgl_frame_guard(op->fl_px, w, h);
    pgl_frame_depth_draws = 0;
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
    if (st->mode == PICA_GL_TEV && st->tev.tex_en && !st->tex_is_rt &&
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
    if (st->depth_on) {
        pgl_zdirty = 1;
        if (st->depth_write) pgl_zwrote = 1;
    }
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
    op->fl_have_depth = pgl_zdirty && (pgl_zwrote || !pgl_zskip_on());
    pgl_zdirty = 0; /* mirror pica_gl_flush_depth_locked's clear */
    pgl_zwrote = 0;
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
    if (op->fl_z) {
        /* t_8df70ad4: DQ8_GL_ZWB=0 gates the split path too, so the knob is
           not silently inert under DQ8_SPLIT_GL=1.  Same semantics as the
           batched path: skip the guest store, keep the attachment. */
        ++pgl_fl_zn;
        if (pgl_zwb())
            pgl_wb_depth(host, have_raw, op->fl_z, w, h, x0, y0, x1, y1);
        else
            ++pgl_fl_zskip;
    }
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
/* FREEZE WATCHDOG (t_4371e6ea): no GL backend compiled in, no hold. */
static int pica_gl_hold_state(const void **owner, unsigned long long *releases,
                              unsigned long long *mid) {
    if (owner) *owner = NULL;
    if (releases) *releases = 0;
    if (mid) *mid = 0;
    return 0;
}
static long pica_gl_hold_tid(void) { return 0; }
static int pica_gl_active(void) { return 0; }
/* t_cb8723ae: staging-decode sync hook (no GL build = no queue). */
static void pica_gl_split_sync(void) {}
static unsigned long long pgl_tex_uploads, pgl_tex_upload_bytes,
    pgl_tex_hits, pgl_tex_misses; /* zeroed stubs for the shared stats line */
static unsigned long long pica_gl_draw_count(void) { return 0; }
static const char *pica_gl_renderer(void) { return "not compiled in"; }
static void pica_gl_shutdown(void) {}
static int pica_gl_pending(void) { return 0; }
/* t_fdd26eba: no GL backend, so a display transfer never inherits a scale. */
static int pica_gl_transfer_scaled_capture(unsigned src_addr, unsigned src_w,
                                           unsigned src_h, unsigned crop_w,
                                           unsigned crop_h, unsigned dst_w,
                                           unsigned dst_h, int vflip,
                                           unsigned h_scale, unsigned v_scale,
                                           unsigned char **out, unsigned *ow,
                                           unsigned *oh, int *oscale) {
    (void)src_addr; (void)src_w; (void)src_h; (void)crop_w; (void)crop_h;
    (void)dst_w; (void)dst_h; (void)vflip; (void)h_scale; (void)v_scale;
    if (out) *out = NULL;
    (void)ow; (void)oh; (void)oscale;
    return 0;
}
static unsigned long long pgl_tr_taken, pgl_tr_fallback, pgl_tr_ns,
    pgl_tr_fallback_mem, pgl_tr_fallback_gate;
static void pica_gl_flush(const PicaGlHost *host) { (void)host; }
static unsigned pica_gl_draw(const PicaGlVertex *verts, unsigned nverts,
                             const PicaGlState *st, const PicaGlHost *host) {
    (void)verts; (void)nverts; (void)st; (void)host;
    return 0;
}

#endif /* DQ8_ENABLE_GL */

#endif /* PICA_GL_C */
