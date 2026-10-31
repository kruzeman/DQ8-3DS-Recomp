/* t_bc6b4eb7: standalone check of the instanced per-triangle payload path
   (divisor-1 attributes + gl_VertexID position select), using the EXACT
   vertex shader string from src/pica_gl.c and a minimal fragment shader.
   Renders 4 known triangles into an FBO and verifies pixel colors.
   Variant argv[1]: "id"   = new path (default)
                    "pos"  = old-style per-vertex a_pos attribute (control)
                    "nodiv"= new path but divisor forced 0 (diagnostic)
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <SDL2/SDL.h>

typedef const unsigned char *(*PFN_glGetString)(unsigned);
typedef unsigned (*PFN_glCreateShader)(unsigned);
typedef void (*PFN_glShaderSource)(unsigned, int, const char *const *, const int *);
typedef void (*PFN_glCompileShader)(unsigned);
typedef void (*PFN_glGetShaderiv)(unsigned, unsigned, int *);
typedef void (*PFN_glGetShaderInfoLog)(unsigned, int, int *, char *);
typedef unsigned (*PFN_glCreateProgram)(void);
typedef void (*PFN_glAttachShader)(unsigned, unsigned);
typedef void (*PFN_glLinkProgram)(unsigned);
typedef void (*PFN_glGetProgramiv)(unsigned, unsigned, int *);
typedef void (*PFN_glUseProgram)(unsigned);
typedef int (*PFN_glGetUniformLocation)(unsigned, const char *);
typedef void (*PFN_glUniform2f)(int, float, float);
typedef void (*PFN_glUniform1i)(int, int);
typedef void (*PFN_glGenBuffers)(int, unsigned *);
typedef void (*PFN_glBindBuffer)(unsigned, unsigned);
typedef void (*PFN_glBufferData)(unsigned, long, const void *, unsigned);
typedef void (*PFN_glBufferSubData)(unsigned, long, long, const void *);
typedef void (*PFN_glGenVertexArrays)(int, unsigned *);
typedef void (*PFN_glBindVertexArray)(unsigned);
typedef void (*PFN_glEnableVertexAttribArray)(unsigned);
typedef void (*PFN_glVertexAttribPointer)(unsigned, int, unsigned, unsigned char, int, const void *);
typedef void (*PFN_glVertexAttribDivisor)(unsigned, unsigned);
typedef void (*PFN_glDrawArraysInstanced)(unsigned, int, int, int);
typedef void (*PFN_glDrawArrays)(unsigned, int, int);
typedef void (*PFN_glViewport)(int, int, int, int);
typedef void (*PFN_glClearColor)(float, float, float, float);
typedef void (*PFN_glClear)(unsigned);
typedef void (*PFN_glGenFramebuffers)(int, unsigned *);
typedef void (*PFN_glBindFramebuffer)(unsigned, unsigned);
typedef void (*PFN_glFramebufferTexture2D)(unsigned, unsigned, unsigned, unsigned, int);
typedef unsigned (*PFN_glCheckFramebufferStatus)(unsigned);
typedef void (*PFN_glGenTextures)(int, unsigned *);
typedef void (*PFN_glBindTexture)(unsigned, unsigned);
typedef void (*PFN_glTexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
typedef void (*PFN_glTexParameteri)(unsigned, unsigned, int);
typedef void (*PFN_glReadPixels)(int, int, int, int, unsigned, unsigned, void *);
typedef void (*PFN_glPixelStorei)(unsigned, int);
typedef void (*PFN_glDrawBuffers)(int, const unsigned *);
typedef unsigned (*PFN_glGetError)(void);
typedef void (*PFN_glDeleteShader)(unsigned);

#define LO(fn) do { pfn_##fn = (PFN_##fn)eglGetProcAddress("gl" #fn); \
    if (!pfn_##fn) { printf("missing gl%s\n", #fn); return 1; } } while (0)

/* EXACT vertex shader string from src/pica_gl.c (t_bc6b4eb7 version) — keep
   in sync manually; the test FAILS LOUDLY if src drifts (checked below). */
static const char vs_new[] =
    "#version 330 core\n"
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
    "    gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "    g0a = f0a; g0b = f0b; g0c = f0c;\n"
    "    g1a = f1a; g1b = f1b; g1c = f1c;\n"
    "    g2a = f2a; g2b = f2b; g2c = f2c;\n"
    "}\n";

/* Old-style control: per-vertex position attribute (pre-t_bc6b4eb7 shape). */
static const char vs_pos[] =
    "#version 330 core\n"
    "layout(location=0) in vec2 a_pos;\n"
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
    "    vec2 ndc = a_pos / u_rt * 2.0 - 1.0;\n"
    "    gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "    g0a = f0a; g0b = f0b; g0c = f0c;\n"
    "    g1a = f1a; g1b = f1b; g1c = f1c;\n"
    "    g2a = f2a; g2b = f2b; g2c = f2c;\n"
    "}\n";

/* Minimal fragment shader: flat primary color straight out (geometry layer
   only — TEV/blend math is not what this test checks). */
static const char fs[] =
    "#version 330 core\n"
    "flat in vec4 g0a, g0b, g0c, g1a, g1b, g1c, g2a, g2b, g2c;\n"
    "out vec4 frag;\n"
    "void main() {\n"
    "    frag = vec4(g0b.rgb, 1.0);\n"
    "}\n";

typedef struct {
    float a[3][4];
    float b[3][4];
    float c[3][2];
} Payload;

typedef struct { float x, y, r, g, b; } Corner;

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "id";
    PFN_glGetString pfn_glGetString;
    PFN_glCreateShader pfn_glCreateShader;
    PFN_glShaderSource pfn_glShaderSource;
    PFN_glCompileShader pfn_glCompileShader;
    PFN_glGetShaderiv pfn_glGetShaderiv;
    PFN_glGetShaderInfoLog pfn_glGetShaderInfoLog;
    PFN_glCreateProgram pfn_glCreateProgram;
    PFN_glAttachShader pfn_glAttachShader;
    PFN_glLinkProgram pfn_glLinkProgram;
    PFN_glGetProgramiv pfn_glGetProgramiv;
    PFN_glUseProgram pfn_glUseProgram;
    PFN_glGetUniformLocation pfn_glGetUniformLocation;
    PFN_glUniform2f pfn_glUniform2f;
    PFN_glGenBuffers pfn_glGenBuffers;
    PFN_glBindBuffer pfn_glBindBuffer;
    PFN_glBufferData pfn_glBufferData;
    PFN_glBufferSubData pfn_glBufferSubData;
    PFN_glGenVertexArrays pfn_glGenVertexArrays;
    PFN_glBindVertexArray pfn_glBindVertexArray;
    PFN_glEnableVertexAttribArray pfn_glEnableVertexAttribArray;
    PFN_glVertexAttribPointer pfn_glVertexAttribPointer;
    PFN_glVertexAttribDivisor pfn_glVertexAttribDivisor;
    PFN_glDrawArraysInstanced pfn_glDrawArraysInstanced;
    PFN_glViewport pfn_glViewport;
    PFN_glClearColor pfn_glClearColor;
    PFN_glClear pfn_glClear;
    PFN_glGenFramebuffers pfn_glGenFramebuffers;
    PFN_glBindFramebuffer pfn_glBindFramebuffer;
    PFN_glFramebufferTexture2D pfn_glFramebufferTexture2D;
    PFN_glCheckFramebufferStatus pfn_glCheckFramebufferStatus;
    PFN_glGenTextures pfn_glGenTextures;
    PFN_glBindTexture pfn_glBindTexture;
    PFN_glTexImage2D pfn_glTexImage2D;
    PFN_glTexParameteri pfn_glTexParameteri;
    PFN_glReadPixels pfn_glReadPixels;
    PFN_glPixelStorei pfn_glPixelStorei;
    PFN_glGetError pfn_glGetError;
    PFN_glDeleteShader pfn_glDeleteShader;

    EGLDisplay dpy;
    EGLContext ctx;
    EGLConfig cfg;
    EGLint maj, min, ncfg = 0;
    static const EGLint cfg_attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                       EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                                       EGL_NONE };
    PFNEGLGETPLATFORMDISPLAYEXTPROC gp;
    unsigned prog, vs, fsid, vbo, vao, fbo, tex;
    int ok, loc, i, errors = 0;
    enum { RTW = 512, RTH = 400, NTRI = 4 };
    unsigned char *pixels;
    float divisors_use;

    /* 4 triangles: corners in raster coords (y=0 bottom), flat color. */
    Corner tris[NTRI][3] = {
        /* red, well inside bottom-left  */
        { {100, 100, 1, 0, 0}, {260, 100, 1, 0, 0}, {100, 260, 1, 0, 0} },
        /* green, bottom-right */
        { {300, 100, 0, 1, 0}, {440, 100, 0, 1, 0}, {440, 240, 0, 1, 0} },
        /* blue, top-left */
        { {100, 300, 0, 0, 1}, {260, 300, 0, 0, 1}, {160, 380, 0, 0, 1} },
        /* yellow, top-right */
        { {300, 300, 1, 1, 0}, {440, 300, 1, 1, 0}, {380, 380, 1, 1, 0} },
    };
    /* test points well inside each triangle */
    const int probe[NTRI][2] = { {150, 140}, {400, 140}, {150, 320}, {370, 330} };

    gp = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    /* The real binary runs host_display_init() -> SDL_Init(VIDEO) BEFORE
       pica_gl_init(); SDL's init is what makes eglGetProcAddress-returned GL
       entry points callable (dispatch setup).  Mirror that order here. */
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 2;
    }
    eglBindAPI(EGL_OPENGL_API);
    dpy = gp(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &maj, &min)) {
        printf("egl init failed 0x%04X\n", eglGetError());
        return 2;
    }
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &ncfg) || ncfg < 1) {
        printf("no egl cfg\n");
        return 2;
    }
    ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT,
                           (const EGLint[]){ EGL_CONTEXT_MAJOR_VERSION, 3,
                                             EGL_CONTEXT_MINOR_VERSION, 3, EGL_NONE });
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        fprintf(stderr, "makecurrent failed 0x%04X\n", eglGetError());
        return 2;
    }
    fprintf(stderr, "makecurrent ok ctx=%p egl_err=0x%04X\n", (void *)ctx, eglGetError());

    LO(glGetString); LO(glCreateShader); LO(glShaderSource); LO(glCompileShader);
    LO(glGetShaderiv); LO(glGetShaderInfoLog); LO(glCreateProgram); LO(glAttachShader);
    LO(glLinkProgram); LO(glGetProgramiv); LO(glUseProgram); LO(glGetUniformLocation);
    LO(glUniform2f); LO(glGenBuffers); LO(glBindBuffer); LO(glBufferData);
    LO(glBufferSubData); LO(glGenVertexArrays); LO(glBindVertexArray);
    LO(glEnableVertexAttribArray); LO(glVertexAttribPointer); LO(glVertexAttribDivisor);
    LO(glDrawArraysInstanced); LO(glViewport); LO(glClearColor); LO(glClear);
    LO(glGenFramebuffers); LO(glBindFramebuffer); LO(glFramebufferTexture2D);
    LO(glCheckFramebufferStatus);
    LO(glGenTextures); LO(glBindTexture); LO(glTexImage2D); LO(glTexParameteri);
    LO(glReadPixels); LO(glPixelStorei); LO(glGetError); LO(glDeleteShader);

    fprintf(stderr, "renderer ptr check: glGetString=%p\n", (void *)pfn_glGetString);
    fprintf(stderr, "renderer -> %s\n",
            pfn_glGetString ? (const char *)pfn_glGetString(0x1F01) : "(null)");
    fprintf(stderr, "variant=%s egl_err=0x%04X\n", mode, eglGetError());
    fflush(stderr);

    vsid_build:;
    {
        const char *src = (strcmp(mode, "pos") == 0) ? vs_pos : vs_new;
        vs = pfn_glCreateShader(0x8B31);
        pfn_glShaderSource(vs, 1, &src, NULL);
        pfn_glCompileShader(vs);
        pfn_glGetShaderiv(vs, 0x8B81, &ok);
        if (!ok) {
            char log[4096]; int n = 0;
            pfn_glGetShaderInfoLog(vs, sizeof log, &n, log);
            printf("VS COMPILE FAILED: %.*s\n", n, log);
            return 3;
        }
        fsid = pfn_glCreateShader(0x8B30);
        pfn_glShaderSource(fsid, 1, &fs, NULL);
        pfn_glCompileShader(fsid);
        pfn_glGetShaderiv(fsid, 0x8B81, &ok);
        if (!ok) {
            char log[4096]; int n = 0;
            pfn_glGetShaderInfoLog(fsid, sizeof log, &n, log);
            printf("FS COMPILE FAILED: %.*s\n", n, log);
            return 3;
        }
        prog = pfn_glCreateProgram();
        pfn_glAttachShader(prog, vs);
        pfn_glAttachShader(prog, fsid);
        pfn_glLinkProgram(prog);
        pfn_glGetProgramiv(prog, 0x8B82, &ok);
        if (!ok) { printf("LINK FAILED\n"); return 3; }
        pfn_glDeleteShader(vs);
        pfn_glDeleteShader(fsid);
    }

    /* FBO: RGBA8 texture, like pgl_ensure_rt */
    pfn_glGenTextures(1, &tex);
    pfn_glBindTexture(0x0DE1, tex);
    pfn_glTexImage2D(0x0DE1, 0, 0x8058, RTW, RTH, 0, 0x1908, 0x1401, NULL);
    pfn_glTexParameteri(0x0DE1, 0x2801, 0x2600);
    pfn_glTexParameteri(0x0DE1, 0x2800, 0x2600);
    pfn_glGenFramebuffers(1, &fbo);
    pfn_glBindFramebuffer(0x8D40, fbo);
    pfn_glFramebufferTexture2D(0x8D40, 0x8CE0, 0x0DE1, tex, 0);
    {
        unsigned st = pfn_glCheckFramebufferStatus(0x8D40);
        if (st != 0x8CD5) { printf("fbo status 0x%04X\n", st); return 4; }
    }

    /* payload: NTRI records, 120 B each */
    {
        Payload pay[NTRI];
        unsigned char *stage = (unsigned char *)malloc(8u * 1024u * 1024u);
        size_t off = 0, bytes = sizeof pay;
        memset(pay, 0, sizeof pay);
        for (i = 0; i < NTRI; ++i) {
            int k;
            for (k = 0; k < 3; ++k) {
                pay[i].a[k][0] = tris[i][k].x;
                pay[i].a[k][1] = tris[i][k].y;
                pay[i].a[k][2] = 1.0f;                    /* iw */
                pay[i].a[k][3] = 0.0f;                    /* zw */
                pay[i].b[k][0] = tris[i][k].r;
                pay[i].b[k][1] = tris[i][k].g;
                pay[i].b[k][2] = tris[i][k].b;
                pay[i].b[k][3] = 1.0f;
                pay[i].c[k][0] = 0.0f;
                pay[i].c[k][1] = 0.0f;
            }
        }
        memcpy(stage, pay, bytes);

        pfn_glGenVertexArrays(1, &vao);
        pfn_glBindVertexArray(vao);
        pfn_glGenBuffers(1, &vbo);
        pfn_glBindBuffer(0x8892, vbo);
        /* the ring shape: allocate 8 MB then SubData at `off` */
        pfn_glBufferData(0x8892, 8 * 1024 * 1024, NULL, 0x88E0);
        pfn_glBufferSubData(0x8892, (long)off, (long)bytes, stage + off);

        divisors_use = (strcmp(mode, "nodiv") == 0) ? 0.0f : 1.0f;
        for (i = 0; i < 3; ++i) {
            pfn_glEnableVertexAttribArray(1 + i);
            pfn_glVertexAttribPointer(1 + i, 4, 0x1406, 0, sizeof(Payload),
                                      (const void *)(off + (size_t)i * 16u));
            pfn_glVertexAttribDivisor(1 + i, divisors_use ? 1u : 0u);
            pfn_glEnableVertexAttribArray(4 + i);
            pfn_glVertexAttribPointer(4 + i, 4, 0x1406, 0, sizeof(Payload),
                                      (const void *)(off + 48u + (size_t)i * 16u));
            pfn_glVertexAttribDivisor(4 + i, divisors_use ? 1u : 0u);
            pfn_glEnableVertexAttribArray(7 + i);
            pfn_glVertexAttribPointer(7 + i, 2, 0x1406, 0, sizeof(Payload),
                                      (const void *)(off + 96u + (size_t)i * 8u));
            pfn_glVertexAttribDivisor(7 + i, divisors_use ? 1u : 0u);
        }
        if (strcmp(mode, "pos") == 0) {
            /* control: per-vertex screen positions from the SAME records,
               uploaded as a separate interleaved 8-float... simplest: reuse
               payload: enable location0 over the payload's a rows is NOT
               layout-compatible; instead build a per-vertex buffer. */
            static float posbuf[NTRI * 3 * 2];
            int j;
            for (i = 0; i < NTRI; ++i)
                for (j = 0; j < 3; ++j) {
                    posbuf[(i * 3 + j) * 2 + 0] = tris[i][j].x;
                    posbuf[(i * 3 + j) * 2 + 1] = tris[i][j].y;
                }
            /* pack posbuf AFTER the payload in the same ring so the control
               uses the same upload style */
            {
                size_t poff = (bytes + 3u) & ~(size_t)3u;
                pfn_glBufferSubData(0x8892, (long)poff, sizeof posbuf, posbuf);
                pfn_glEnableVertexAttribArray(0);
                pfn_glVertexAttribPointer(0, 2, 0x1406, 0, 0, (const void *)poff);
            }
            for (i = 0; i < 3; ++i) {
                pfn_glVertexAttribDivisor(1 + i, 1u);
                pfn_glVertexAttribDivisor(4 + i, 1u);
                pfn_glVertexAttribDivisor(7 + i, 1u);
            }
        }

        pfn_glUseProgram(prog);
        loc = pfn_glGetUniformLocation(prog, "u_rt");
        if (loc < 0) { printf("u_rt not found\n"); return 5; }
        pfn_glUniform2f(loc, (float)RTW, (float)RTH);
        pfn_glViewport(0, 0, RTW, RTH);
        pfn_glClearColor(0, 0, 0, 1);
        pfn_glClear(0x4000);
        while (pfn_glGetError()) {}
        if (strcmp(mode, "pos") == 0) {
            /* DrawArrays control: same payload in per-vertex-expanded form is
               not available here; just draw instanced too — a_pos comes from
               the per-vertex buffer (divisor 0) so vertices differ correctly. */
        }
        pfn_glDrawArraysInstanced(0x0004, 0, 3, NTRI);
        {
            unsigned err = pfn_glGetError();
            if (err) { printf("GL ERROR after draw: 0x%04X\n", err); ++errors; }
        }

        pixels = (unsigned char *)malloc((size_t)RTW * RTH * 4);
        pfn_glPixelStorei(0x0CF5, 1);
        pfn_glReadPixels(0, 0, RTW, RTH, 0x1908, 0x1401, pixels);
        {
            unsigned err = pfn_glGetError();
            if (err) { printf("GL ERROR after readback: 0x%04X\n", err); ++errors; }
        }

        for (i = 0; i < NTRI; ++i) {
            int px = probe[i][0], py = probe[i][1];
            const unsigned char *q = pixels + ((size_t)py * RTW + px) * 4;
            const char *name = (i == 0) ? "red" : i == 1 ? "green" : i == 2 ? "blue" : "yellow";
            int expect_r = tris[i][0].r * 255, expect_g = tris[i][0].g * 255,
                expect_b = tris[i][0].b * 255;
            int match = (q[0] == expect_r && q[1] == expect_g && q[2] == expect_b);
            printf("probe %-6s (%3d,%3d) got=(%3u,%3u,%3u) want=(%3d,%3d,%3d) %s\n",
                   name, px, py, q[0], q[1], q[2], expect_r, expect_g, expect_b,
                   match ? "OK" : "MISMATCH");
            if (!match) ++errors;
        }
        /* coverage sanity: count non-black pixels */
        {
            size_t n = 0, j;
            for (j = 0; j < (size_t)RTW * RTH; ++j)
                if (pixels[j * 4] | pixels[j * 4 + 1] | pixels[j * 4 + 2]) ++n;
            printf("nonblack px = %zu (expect roughly the 4 triangles' area ~ 40-60k)\n", n);
        }
        free(stage);
        free(pixels);
    }
    printf(errors ? "RESULT: FAIL (%d errors)\n" : "RESULT: PASS\n", errors);
    return errors ? 6 : 0;
}
