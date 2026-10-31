/* gl_backend_test — reproducible test for the OpenGL backend (src/pica_gl.c).
 *
 * Builds and runs WITHOUT the game: it drives the same pica_gl_draw() entry
 * point the guest pipeline uses, with test geometry and a synthetic PICA-like
 * draw state, then verifies the read-back render-target pixels.
 *
 * Modes:
 *   --headless (default)  EGL surfaceless context; no display needed.
 *   --window [seconds]    SDL2 window + GL context (requires a display); shows
 *                         the rendered frame so the result is visible.
 *
 * The GL_VENDOR / GL_RENDERER / GL_VERSION strings are printed at startup:
 * "llvmpipe"/"softpipe" means a software rasterizer (no GPU), anything else
 * (NVIDIA/AMD/Intel/Apple) means the draw ran on real GPU hardware.
 *
 * Exit code 0 = all pixel checks passed.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "../src/pica_gl.c"

/* Defined by src/pica_vs.c in the runtime; stubbed here (this test builds
   pica_gl.c standalone). */
static void pica_stage_stats_dump(void) {}

#ifdef GL_TEST_ENABLE_SDL
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#endif

#define RT_W 480
#define RT_H 400

static unsigned char fb[RT_W * RT_H * 4]; /* host render target, row 0 = y 0 (bottom) */

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
static const PicaGlHost host = { host_rt_get, host_rt_put, NULL };

static int failures;

static void check_px(const char *what, int x, int y, int r, int g, int b, int a,
                     int tol) {
    const unsigned char *p = fb + ((size_t)y * RT_W + x) * 4;
    int bad = abs((int)p[0] - r) > tol || abs((int)p[1] - g) > tol ||
              abs((int)p[2] - b) > tol || abs((int)p[3] - a) > tol;
    if (bad) {
        ++failures;
        fprintf(stderr, "FAIL %-28s (%3d,%3d) got %3d,%3d,%3d,%3d want %3d,%3d,%3d,%3d\n",
                what, x, y, p[0], p[1], p[2], p[3], r, g, b, a);
    } else {
        fprintf(stderr, "ok   %-28s (%3d,%3d) = %3d,%3d,%3d,%3d\n",
                what, x, y, p[0], p[1], p[2], p[3]);
    }
}

static void set_vertex(PicaGlVertex *v, float x, float y, float r, float g,
                       float b, float a, float u, float vv) {
    v->x = x; v->y = y; v->iw = 1.0f;
    v->color[0] = r; v->color[1] = g; v->color[2] = b; v->color[3] = a;
    v->u = u; v->v = vv;
}

/* Two-triangle quad: (x0,y0)-(x1,y1), uv (0,0) bottom-left .. (1,1) top-right. */
static void quad(PicaGlVertex *t, float x0, float y0, float x1, float y1,
                 float r, float g, float b, float a) {
    set_vertex(&t[0], x0, y0, r, g, b, a, 0, 0);
    set_vertex(&t[1], x1, y0, r, g, b, a, 1, 0);
    set_vertex(&t[2], x1, y1, r, g, b, a, 1, 1);
    set_vertex(&t[3], x0, y0, r, g, b, a, 0, 0);
    set_vertex(&t[4], x1, y1, r, g, b, a, 1, 1);
    set_vertex(&t[5], x0, y1, r, g, b, a, 0, 1);
}

static void write_ppm(const char *path) {
    FILE *f = fopen(path, "wb");
    int y, x;
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return; }
    fprintf(f, "P6\n%d %d\n255\n", RT_W, RT_H);
    for (y = RT_H - 1; y >= 0; --y) /* PPM rows are top-down */
        for (x = 0; x < RT_W; ++x) {
            const unsigned char *p = fb + ((size_t)y * RT_W + x) * 4;
            fwrite(p, 1, 3, f); /* RGB only */
        }
    fclose(f);
}

/* --- window presentation reuses the GL entry points loaded by pica_gl.c --- */

int main(int argc, char **argv) {
    int window_mode = 0, window_seconds = 8;
    const char *ppm_path = "target/gl_test_frame.ppm";
    PicaGlState st;
    PicaGlVertex tris[6];
    unsigned char tex[2 * 2 * 4];
    int i;

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--window")) {
            window_mode = 1;
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
                window_seconds = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--headless")) {
            window_mode = 0;
        } else if (!strcmp(argv[i], "--ppm") && i + 1 < argc) {
            ppm_path = argv[++i];
        } else {
            fprintf(stderr, "usage: %s [--headless|--window [seconds]] [--ppm path]\n",
                    argv[0]);
            return 2;
        }
    }

    memset(fb, 0, sizeof fb);
    /* background (10,20,30,255) everywhere */
    for (i = 0; i < RT_W * RT_H; ++i) {
        fb[i * 4 + 0] = 10; fb[i * 4 + 1] = 20; fb[i * 4 + 2] = 30;
        fb[i * 4 + 3] = 255;
    }

#ifdef GL_TEST_ENABLE_SDL
    SDL_Window *win = NULL;
    SDL_GLContext glctx = NULL;
    if (window_mode) {
        if (SDL_Init(SDL_INIT_VIDEO) != 0) {
            fprintf(stderr, "SDL_Init: %s (use --headless)\n", SDL_GetError());
            return 2;
        }
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        win = SDL_CreateWindow("PICA GL backend test", SDL_WINDOWPOS_CENTERED,
                               SDL_WINDOWPOS_CENTERED, RT_W * 2, RT_H * 2,
                               SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
        if (!win || !(glctx = SDL_GL_CreateContext(win))) {
            fprintf(stderr, "SDL GL context failed: %s (use --headless)\n",
                    SDL_GetError());
            return 2;
        }
        if (!pica_gl_init_external((void *(*)(const char *))SDL_GL_GetProcAddress)) {
            fprintf(stderr, "pica_gl_init_external failed\n");
            return 2;
        }
    } else
#endif
    {
        if (!window_mode) {
            setenv("DQ8_GL", "1", 0);
            if (!pica_gl_init()) {
                fprintf(stderr, "pica_gl_init failed (no EGL/GL available)\n");
                return 2;
            }
        }
    }
    if (!pica_gl_active()) {
        fprintf(stderr, "GL backend not active\n");
        return 2;
    }
    fprintf(stderr, "GL test: renderer = %s\n", pica_gl_renderer());
    if (strstr(pica_gl_renderer(), "llvmpipe") ||
        strstr(pica_gl_renderer(), "softpipe") ||
        strstr(pica_gl_renderer(), "swrast"))
        fprintf(stderr, "GL test: NOTE: software GL rasterizer here; "
                "on a GPU machine this draw is hardware-accelerated\n");

    /* ---- Test 1: solid-color quad (PICA_GL_SOLID), no blend ---- */
    memset(&st, 0, sizeof st);
    st.mode = PICA_GL_SOLID;
    st.rt_w = RT_W; st.rt_h = RT_H;
    st.color_mask = 15;
    quad(tris, 20, 20, 120, 120, 200 / 255.0f, 100 / 255.0f, 50 / 255.0f, 1.0f);
    if (!pica_gl_draw(tris, 6, &st, &host)) {
        fprintf(stderr, "FAIL solid draw did not run\n");
        return 1;
    }
    check_px("solid interior", 70, 70, 200, 100, 50, 255, 1);
    check_px("solid interior 2", 21, 21, 200, 100, 50, 255, 1);
    check_px("outside untouched", 130, 70, 10, 20, 30, 255, 0);

    /* ---- Test 2: textured quad (PICA_GL_TEV, modulate chain) ---- */
    /* GL row 0 (v~0) = red,green ; row 1 (v~1) = blue,white */
    memcpy(tex, (unsigned char[16]){
        255, 0, 0, 255,    0, 255, 0, 255,
        0, 0, 255, 255,    255, 255, 255, 255 }, 16);
    memset(&st, 0, sizeof st);
    st.mode = PICA_GL_TEV;
    st.rt_w = RT_W; st.rt_h = RT_H;
    st.color_mask = 15;
    st.tex_rgba = tex; st.tex_w = 2; st.tex_h = 2;
    /* TEV chain = primary * texture (stage 0), pass-through stages 1..5 */
    st.tev.tex_en = 1;
    st.tev.src_c[0] = 0; st.tev.src_c[1] = 3;
    st.tev.src_a[0] = 0; st.tev.src_a[1] = 3;
    st.tev.cop[0] = 1; st.tev.aop[0] = 1;
    for (int s = 1; s < 6; s++) {
        st.tev.src_c[s * 3] = 15;
        st.tev.src_a[s * 3] = 15;
    }
    for (int s = 0; s < 6; s++) {
        st.tev.scale_c[s] = 1.0f;
        st.tev.scale_a[s] = 1.0f;
    }
    quad(tris, 140, 20, 172, 52, 1, 1, 1, 1);
    if (!pica_gl_draw(tris, 6, &st, &host)) {
        fprintf(stderr, "FAIL textured draw did not run\n");
        return 1;
    }
    check_px("tex modulate lower-left", 148, 28, 255, 0, 0, 255, 2);
    check_px("tex modulate lower-right", 164, 28, 0, 255, 0, 255, 2);
    check_px("tex modulate upper-left", 148, 44, 0, 0, 255, 255, 2);
    check_px("tex modulate upper-right", 164, 44, 255, 255, 255, 255, 2);

    /* ---- Test 3: alpha blending (src_alpha, 1-src_alpha) over background ---- */
    {
        float sa = 128 / 255.0f;
        int er, eg, eb, ea;
        memset(&st, 0, sizeof st);
        st.mode = PICA_GL_SOLID;
        st.rt_w = RT_W; st.rt_h = RT_H;
        st.color_mask = 15;
        st.blend = 1;
        st.blend_src_rgb = 6; st.blend_dst_rgb = 7; /* SRC_ALPHA, 1-SRC_ALPHA */
        st.blend_src_a = 6; st.blend_dst_a = 7;
        st.blend_eq_rgb = 0; st.blend_eq_a = 0; /* add */
        quad(tris, 200, 20, 300, 120, 1, 0, 0, sa);
        if (!pica_gl_draw(tris, 6, &st, &host)) {
            fprintf(stderr, "FAIL blend draw did not run\n");
            return 1;
        }
        er = (int)(255.0f * (1.0f * sa + (10 / 255.0f) * (1 - sa)) + 0.5f);
        eg = (int)(255.0f * (0.0f * sa + (20 / 255.0f) * (1 - sa)) + 0.5f);
        eb = (int)(255.0f * (0.0f * sa + (30 / 255.0f) * (1 - sa)) + 0.5f);
        ea = (int)(255.0f * (sa * sa + 1.0f * (1 - sa)) + 0.5f);
        check_px("blend src_alpha over bg", 250, 70, er, eg, eb, ea, 2);
    }

    /* ---- Test 4: alpha test (func 7 = a >= ref) ---- */
    memset(&st, 0, sizeof st);
    st.mode = PICA_GL_SOLID;
    st.rt_w = RT_W; st.rt_h = RT_H;
    st.color_mask = 15;
    st.alpha_test = 1; st.alpha_func = 7; st.alpha_ref = 128;
    quad(tris, 320, 20, 360, 60, 1 / 255.0f, 2 / 255.0f, 3 / 255.0f, 200 / 255.0f);
    if (!pica_gl_draw(tris, 6, &st, &host)) {
        fprintf(stderr, "FAIL alpha-test draw did not run\n");
        return 1;
    }
    check_px("alpha test pass (200>=128)", 340, 40, 1, 2, 3, 200, 1);
    st.alpha_ref = 250;
    quad(tris, 380, 20, 420, 60, 1 / 255.0f, 2 / 255.0f, 3 / 255.0f, 200 / 255.0f);
    if (!pica_gl_draw(tris, 6, &st, &host)) {
        fprintf(stderr, "FAIL alpha-test-2 draw did not run\n");
        return 1;
    }
    check_px("alpha test reject (200<250)", 400, 40, 10, 20, 30, 255, 0);

    write_ppm(ppm_path);
    fprintf(stderr, "GL test: wrote %s (%d failed checks)\n", ppm_path, failures);

#ifdef GL_TEST_ENABLE_SDL
    if (window_mode && win) {
        /* Present the host framebuffer in the window (fullscreen triangle). */
        const char *pvs =
            "#version 330 core\nout vec2 v_uv;\nvoid main(){\n"
            " vec2 p=vec2(float((gl_VertexID<<1)&2),float(gl_VertexID&2));\n"
            " v_uv=p; gl_Position=vec4(p*2.0-1.0,0.0,1.0);}\n";
        const char *pfs =
            "#version 330 core\nin vec2 v_uv;uniform sampler2D u_tex;"
            "out vec4 frag;\nvoid main(){frag=texture(u_tex,v_uv);}\n";
        PGuint prog = 0, texid = 0;
        {
            PGuint vs = pgl_compile(PGGL_VERTEX_SHADER, pvs);
            PGuint fs = pgl_compile(PGGL_FRAGMENT_SHADER, pfs);
            prog = pgl_glCreateProgram();
            pgl_glAttachShader(prog, vs);
            pgl_glAttachShader(prog, fs);
            pgl_glLinkProgram(prog);
            pgl_glDeleteShader(vs);
            pgl_glDeleteShader(fs);
            pgl_glGenTextures(1, &texid);
            pgl_glBindTexture(PGGL_TEXTURE_2D, texid);
            pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_MIN_FILTER, PGGL_NEAREST);
            pgl_glTexParameteri(PGGL_TEXTURE_2D, PGGL_TEXTURE_MAG_FILTER, PGGL_NEAREST);
            pgl_glTexImage2D(PGGL_TEXTURE_2D, 0, PGGL_RGBA8, RT_W, RT_H, 0,
                             PGGL_RGBA, PGGL_UNSIGNED_BYTE, fb);
            pgl_glUseProgram(prog);
            pgl_glUniform1i(pgl_glGetUniformLocation(prog, "u_tex"), 0);
        }
        {
            int done = 0;
            Uint32 start = SDL_GetTicks();
            while (!done) {
                SDL_Event ev;
                while (SDL_PollEvent(&ev))
                    if (ev.type == SDL_QUIT ||
                        (ev.type == SDL_KEYDOWN &&
                         (ev.key.keysym.sym == SDLK_ESCAPE || ev.key.keysym.sym == SDLK_q)))
                        done = 1;
                pgl_glViewport(0, 0, RT_W * 2, RT_H * 2);
                pgl_glClearColor(0, 0, 0, 1);
                pgl_glClear(PGGL_COLOR_BUFFER_BIT);
                pgl_glDrawArrays(PGGL_TRIANGLES, 0, 3);
                SDL_GL_SwapWindow(win);
                if (window_seconds > 0 &&
                    SDL_GetTicks() - start > (Uint32)window_seconds * 1000u)
                    done = 1;
            }
        }
    }
#endif
    pica_gl_shutdown();
    if (failures) {
        fprintf(stderr, "GL test: FAILED (%d checks)\n", failures);
        return 1;
    }
    fprintf(stderr, "GL test: PASS (draws=%llu)\n", pica_gl_draw_count());
    return 0;
}
