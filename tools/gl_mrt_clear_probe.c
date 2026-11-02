/* gl_mrt_clear_probe — t_6de47bba: decisive semantics probe for the pkg23
   root-cause candidate "BUG A" (see reports/PKG22_READBACK_FIX_REPORT.md).
 *
 * Question: with an FBO carrying ATT0=RGBA8 + ATT1=R32F (the GL backend's
 * render-target + depth-mirror pair), does glClear(GL_COLOR_BUFFER_BIT)
 * clear ONLY ATT0, or every buffer in the FBO's glDrawBuffers list?
 *
 * The GL backend's uniform-fill fast clear (pgl_refresh) runs
 * glClear(COLOR_BUFFER_BIT) right after a depth draw, while the FBO's draw
 * buffer list is still {ATT0, ATT1}.  If glClear honours the WHOLE list
 * (spec: "each color buffer currently being drawn to is cleared"), the
 * color fill also wipes the R32F depth mirror with the CLEAR COLOR (~0..1)
 * — and pgl_refresh_depth's memcmp "unchanged" fast path then never
 * restores it, so every later depth test compares integer z against a
 * wiped value and fails.
 *
 * Tests (llvmpipe is enough: Mesa front-end semantics are driver-shared):
 *   T1  drawBuffers={A0,A1}; clear A1 to 42; glClear(black); read A1.
 *       A1==0        -> clobber CONFIRMED (the bug).
 *       A1==42       -> no clobber (hypothesis dead).
 *   T2  the candidate fix: re-clear A1 to 42; drawBuffers={A0}; glClear;
 *       read A1.  A1 must stay 42 on every driver.
 *   T3  attribution detail: drawBuffers={A0,A1}; clear A1 to 42;
 *       glClearColor(0.25,...); glClear; read A1.
 *       A1==0.25     -> wiped with the clear colour's RED channel.
 *
 * Exit code 0 = probe ran to completion; the PASS/FAIL lines are the data.
 */
#include <EGL/egl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
typedef EGLDisplay (*fn_eglGetPlatformDisplayEXT)(EGLenum, void *, const EGLint *);

/* Minimal GL loader (ABI constants; same approach as src/pica_gl.c). */
#define GL_COLOR_BUFFER_BIT   0x00004000u
#define GL_FLOAT              0x1406u
#define GL_RGBA               0x1908u
#define GL_RGBA8              0x8058u
#define GL_RED                0x1903u
#define GL_R32F               0x822Eu
#define GL_TEXTURE_2D         0x0DE1u
#define GL_TEXTURE_MIN_FILTER 0x2801u
#define GL_TEXTURE_MAG_FILTER 0x2800u
#define GL_NEAREST            0x2600u
#define GL_TEXTURE_WRAP_S     0x2802u
#define GL_TEXTURE_WRAP_T     0x2803u
#define GL_CLAMP_TO_EDGE      0x812Fu
#define GL_FRAMEBUFFER        0x8D40u
#define GL_COLOR_ATTACHMENT0  0x8CE0u
#define GL_COLOR_ATTACHMENT1  0x8CE1u
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5u
#define GL_UNSIGNED_BYTE      0x1401u

#define LOAD(name) do { \
    p##name = (PFN_##name)eglGetProcAddress(#name); \
    if (!p##name) { fprintf(stderr, "missing %s\n", #name); return 1; } \
} while (0)

typedef unsigned int PGuint;
typedef int PGint;
typedef unsigned int PGenum;
typedef int PGsizei;
typedef float PGfloat;
typedef void (*PFN_glGenFramebuffers)(PGsizei, PGuint *);
typedef void (*PFN_glBindFramebuffer)(PGenum, PGuint);
typedef void (*PFN_glGenTextures)(PGsizei, PGuint *);
typedef void (*PFN_glBindTexture)(PGenum, PGuint);
typedef void (*PFN_glTexImage2D)(PGenum, PGint, PGint, PGsizei, PGsizei, PGint,
                                 PGenum, PGenum, const void *);
typedef void (*PFN_glTexParameteri)(PGenum, PGenum, PGint);
typedef void (*PFN_glFramebufferTexture2D)(PGenum, PGenum, PGenum, PGuint, PGint);
typedef PGenum (*PFN_glCheckFramebufferStatus)(PGenum);
typedef void (*PFN_glDrawBuffers)(PGsizei, const PGenum *);
typedef void (*PFN_glClearBufferfv)(PGenum, PGint, const PGfloat *);
typedef void (*PFN_glClear)(PGuint);
typedef void (*PFN_glClearColor)(PGfloat, PGfloat, PGfloat, PGfloat);
typedef void (*PFN_glColorMask)(PGuint, PGuint, PGuint, PGuint);
typedef void (*PFN_glReadPixels)(PGint, PGint, PGsizei, PGsizei, PGenum, PGenum,
                                 void *);
typedef void (*PFN_glReadBuffer)(PGenum);
typedef PGenum (*PFN_glGetError)(void);
typedef const unsigned char *(*PFN_glGetString)(PGenum);

static PFN_glGenFramebuffers pglGenFramebuffers;
static PFN_glBindFramebuffer pglBindFramebuffer;
static PFN_glGenTextures pglGenTextures;
static PFN_glBindTexture pglBindTexture;
static PFN_glTexImage2D pglTexImage2D;
static PFN_glTexParameteri pglTexParameteri;
static PFN_glFramebufferTexture2D pglFramebufferTexture2D;
static PFN_glCheckFramebufferStatus pglCheckFramebufferStatus;
static PFN_glDrawBuffers pglDrawBuffers;
static PFN_glClearBufferfv pglClearBufferfv;
static PFN_glClear pglClear;
static PFN_glClearColor pglClearColor;
static PFN_glColorMask pglColorMask;
static PFN_glReadPixels pglReadPixels;
static PFN_glReadBuffer pglReadBuffer;
static PFN_glGetError pglGetError;
static PFN_glGetString pglGetString;

static PGuint fbo, tex0, tex1;

static float read_att1(void) {
    float v = -1.0f;
    pglBindFramebuffer(GL_FRAMEBUFFER, fbo);
    pglReadBuffer(GL_COLOR_ATTACHMENT1);
    pglReadPixels(0, 0, 1, 1, GL_RED, GL_FLOAT, &v);
    pglReadBuffer(GL_COLOR_ATTACHMENT0);
    return v;
}

static void set_att1(float v) {
    PGfloat a1 = v;
    PGenum bufs[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
    pglBindFramebuffer(GL_FRAMEBUFFER, fbo);
    pglColorMask(1, 1, 1, 1);
    pglDrawBuffers(2, bufs);
    pglClearBufferfv(0x1800 /* GL_COLOR */, 1, &a1);
}

static void verdict(const char *t, int ok, const char *fmt, ...) {
    va_list ap;
    printf("%s %s: ", ok ? "PASS" : "FAIL", t);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

int main(void) {
    EGLint major = 0, minor = 0;
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLContext ctx;
    EGLSurface surf;
    EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE, 0, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_NONE
    };
    EGLint ctx_attribs[] = { EGL_CONTEXT_MAJOR_VERSION, 3,
                             EGL_CONTEXT_MINOR_VERSION, 3, EGL_NONE };
    EGLConfig cfg;
    EGLint ncfg = 0;
    fn_eglGetPlatformDisplayEXT getplat;
    PGenum status;
    float v;
    int fails = 0;

    eglBindAPI(EGL_OPENGL_API);
    getplat = (fn_eglGetPlatformDisplayEXT)
        eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (getplat)
        dpy = getplat(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (dpy == EGL_NO_DISPLAY) dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor)) {
        fprintf(stderr, "no EGL display\n");
        return 1;
    }
    if (!eglChooseConfig(dpy, cfg_attribs, &cfg, 1, &ncfg) || ncfg < 1) {
        fprintf(stderr, "no EGL config\n");
        return 1;
    }
    ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
    surf = eglCreatePbufferSurface(dpy, cfg, NULL);
    if (ctx == EGL_NO_CONTEXT ||
        !eglMakeCurrent(dpy, surf, surf, ctx)) {
        fprintf(stderr, "no current EGL context (surfaceless=%p)\n",
                (void *)eglGetError());
        return 1;
    }
    printf("renderer: see GL line below\n");

    LOAD(glGenFramebuffers); LOAD(glBindFramebuffer);
    LOAD(glGenTextures); LOAD(glBindTexture); LOAD(glTexImage2D);
    LOAD(glTexParameteri); LOAD(glFramebufferTexture2D);
    LOAD(glCheckFramebufferStatus); LOAD(glDrawBuffers);
    LOAD(glClearBufferfv); LOAD(glClear); LOAD(glClearColor);
    LOAD(glColorMask); LOAD(glReadPixels); LOAD(glReadBuffer);
    LOAD(glGetError); LOAD(glGetString);
    printf("GL renderer: %s / %s\n", pglGetString(0x1F00 /*VENDOR*/),
           pglGetString(0x1F01 /*RENDERER*/));

    pglGenFramebuffers(1, &fbo);
    pglGenTextures(1, &tex0);
    pglGenTextures(1, &tex1);
    pglBindTexture(GL_TEXTURE_2D, tex0);
    pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    pglTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 8, 8, 0, GL_RGBA,
                  GL_UNSIGNED_BYTE, NULL);
    pglBindTexture(GL_TEXTURE_2D, tex1);
    pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    pglTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, 8, 8, 0, GL_RED, GL_FLOAT, NULL);
    pglBindFramebuffer(GL_FRAMEBUFFER, fbo);
    pglFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                            tex0, 0);
    pglFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D,
                            tex1, 0);
    status = pglCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "FBO incomplete 0x%04X\n", (unsigned)status);
        return 1;
    }
    while (pglGetError() != 0) {}

    /* T1: glClear with drawBuffers={A0,A1} — does ATT1 survive? */
    set_att1(42.0f);
    {
        PGenum bufs[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
        pglDrawBuffers(2, bufs);
    }
    pglColorMask(1, 1, 1, 1);
    pglClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    pglClear(GL_COLOR_BUFFER_BIT);
    v = read_att1();
    printf("T1 glClear({A0,A1}) after A1=42: A1=%g  (0 => MRT clear clobber "
           "CONFIRMED; 42 => no clobber)\n", (double)v);
    if (v == 0.0f) fails += 0; /* informational: the bug is what we test FOR */

    /* T2: the candidate fix — pin drawBuffers to {A0} before the color clear. */
    set_att1(42.0f);
    {
        PGenum bufs[1] = { GL_COLOR_ATTACHMENT0 };
        pglDrawBuffers(1, bufs);
    }
    pglColorMask(1, 1, 1, 1);
    pglClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    pglClear(GL_COLOR_BUFFER_BIT);
    v = read_att1();
    verdict("T2 pin-drawBuffers fix keeps A1", v == 42.0f, "A1=%g (want 42)",
            (double)v);
    if (v != 42.0f) ++fails;

    /* T3: attribution — wiped with the clear colour's RED channel? */
    set_att1(42.0f);
    {
        PGenum bufs[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
        pglDrawBuffers(2, bufs);
    }
    pglColorMask(1, 1, 1, 1);
    pglClearColor(0.25f, 0.5f, 0.75f, 1.0f);
    pglClear(GL_COLOR_BUFFER_BIT);
    v = read_att1();
    printf("T3 glClear(0.25,0.5,0.75,{A0,A1}) after A1=42: A1=%g "
           "(0.25 => wiped with clear.r; 42 => untouched)\n", (double)v);

    /* T4 sanity: the ATT0 colour clear itself works through the pinned list. */
    {
        unsigned char px[4] = { 9, 9, 9, 9 };
        PGenum bufs[1] = { GL_COLOR_ATTACHMENT0 };
        pglDrawBuffers(1, bufs);
        pglClearColor(1.0f, 0.0f, 0.0f, 1.0f);
        pglClear(GL_COLOR_BUFFER_BIT);
        pglReadBuffer(GL_COLOR_ATTACHMENT0);
        pglReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        verdict("T4 pinned clear still clears A0",
                px[0] == 255 && px[1] == 0 && px[2] == 0,
                "px=%u/%u/%u/%u (want 255/0/0/*)", px[0], px[1], px[2], px[3]);
        if (!(px[0] == 255 && px[1] == 0 && px[2] == 0)) ++fails;
    }

    printf("done: %s (T1/T3 are informational)\n", fails ? "FIX FAILED"
                                                         : "FIX OK");
    return fails ? 1 : 0;
}
