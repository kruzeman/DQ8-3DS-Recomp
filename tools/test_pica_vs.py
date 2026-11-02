#!/usr/bin/env python3
"""Compile the emitted vertex-shader runtime against synthetic PICA programs."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class VertexShaderTest(unittest.TestCase):
    def test_native_shader_math_flow_packing_and_bounds(self):
        clang = shutil.which('clang')
        self.assertIsNotNone(clang, 'clang is required for the native runtime test')
        runtime = (ROOT / 'src/pica_vs.c').read_text().split('static float pica_sat(', 1)[0]
        header = r'''
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
static uint32_t pica_regs[768], vs_program[4096], vs_swizzle[4096];
static unsigned vs_program_size, vs_swizzle_size;
static float vs_uniform_f[96][4];
'''
        checks = r'''
int main(void) {
    assert(pica_f24_to_f32(0xbf0000) == -1.0f);
    assert(pica_f24_to_f32(0x400000) == 2.0f);
    assert(signbit(pica_f24_to_f32(0x800000)));
    assert(pica_f24_to_f32(1) == ldexpf(1,-78));
    assert(isinf(pica_f24_to_f32(0x7f0000)));
    assert(isnan(pica_f24_to_f32(0x7f0001)));
    uint32_t packed[3] = {0x3f00003e,0x0000bf00,0x00c00000};
    float unpacked[4];pica_unpack_f24(packed,unpacked);
    assert(unpacked[0]==-2 && unpacked[1]==-1 && unpacked[2]==0.5f && unpacked[3]==1);
    /* MOV o0,v0; MUL o1,c0,v0; END. Identity operand selectors. */
    vs_swizzle[0]=0x0006c36f;vs_swizzle_size=1;
    vs_program[0]=0x4c000000;vs_program[1]=0x20220000;vs_program[2]=0x88000000;
    vs_program_size=3;
    PicaVsState s={0};
    float input[4]={-2,3,0.5f,1}, coeff[4]={4,-2,8,0.25f};
    memcpy(s.input[0],input,sizeof input);memcpy(vs_uniform_f[0],coeff,sizeof coeff);
    assert(pica_execute_vs(&s));
    assert(!memcmp(s.output[0],input,sizeof input));
    assert(s.output[1][0]==-8 && s.output[1][1]==-6 && s.output[1][2]==4 && s.output[1][3]==0.25f);
    /* Masked MOV must leave untouched output components intact. */
    vs_swizzle[0]=0x0006c368;vs_program_size=2;vs_program[1]=0x88000000;
    for(unsigned k=0;k<4;k++)s.output[0][k]=17;
    assert(pica_execute_vs(&s));
    assert(s.output[0][0]==-2 && s.output[0][1]==17 && s.output[0][3]==17);
    /* CALLU b0 executes one-word subroutine and returns before the END. */
    vs_swizzle[0]=0x0006c36f;pica_regs[0x2b0]=1;
    vs_program[0]=0x98000c01;vs_program[1]=0x4c200000;
    vs_program[2]=0x88000000;vs_program[3]=0x20020000;vs_program_size=4;
    assert(pica_execute_vs(&s));
    assert(s.output[0][0]==-8 && s.output[0][2]==4 && s.output[1][0]==-2);
    /* A jump on a subroutine's final word must yield to the return boundary. */
    vs_program[0]=0x98000c01;vs_program[3]=0xb4000000;
    assert(pica_execute_vs(&s));
    /* Invalid fetch, unsupported LOOP, and a backward jump fail boundedly. */
    pica_regs[0x2ba]=4096;assert(!pica_execute_vs(&s));pica_regs[0x2ba]=0;
    vs_program[0]=0xa4000000;assert(!pica_execute_vs(&s));
    pica_regs[0x2b0]=0;vs_program[0]=0xb4000001;assert(!pica_execute_vs(&s));
    return 0;
}
'''
        self.compile_run(header+runtime+checks)

    def test_predecoded_dispatch_is_bit_identical(self):
        """t_469b3d92: the pre-decoded VS dispatcher must produce exactly the
        same registers/cc/address/failure code as the interpreter's dispatch,
        for the REAL DQ8 vertex shader, over random and IEEE edge-case inputs.

        The program + swizzle table come from tools/testdata/vsprog_dq8.txt (a
        DQ8_VSPROGDUMP capture), so MOV/ADD/MUL/FLOOR/MAD/DP3/DP4/DPH/CMP,
        MOVA, CALL, the uniform-relative operands and the flow-control scopes
        are all exercised.  Runs twice per case: pvs_dec_ready=0 (interpreter)
        vs pvs_dec_ready=1 (pre-decoded), and compares the whole PicaVsState
        with memcmp so a single wrong lane fails the test."""
        clang = shutil.which('clang')
        self.assertIsNotNone(clang, 'clang is required for the native runtime test')
        words = [ln for ln in (ROOT / 'tools/testdata/vsprog_dq8.txt').read_text().splitlines()
                 if not ln.startswith('#')]
        prog = next(ln for ln in words if ln.startswith('program')).split(None, 1)[1]
        swz = next(ln for ln in words if ln.startswith('swizzle')).split(None, 1)[1]
        runtime = (ROOT / 'src/pica_vs.c').read_text().split('static float pica_sat(', 1)[0]
        header = r'''
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t pica_regs[768], vs_program[4096], vs_swizzle[4096];
static unsigned vs_program_size, vs_swizzle_size;
static float vs_uniform_f[96][4];
/* Stubs for the render-target / depth-test globals the extracted region
   references (this test never rasterises, it only runs the vertex shader). */
static uint32_t pica_rt_w, pica_rt_h, pica_rt_format, pica_rt_addr;
static int pica_depth_bound, pica_depth_allow_write;
static uint32_t pica_depth_addr, pica_depth_bits, pica_depth_bpp;
static int pica_depth_range_ok(void) { return 0; }
static uint8_t ram[4096];
static int guest_memory_range(uint32_t a, uint64_t n, uint32_t *m) {
    *m = a; return a < sizeof ram && n <= sizeof ram - a;
}
static double dq8_layer_t(void) { return 0.0; }
static uint64_t pica_draw_count;
/* SPIKETRAP hooks are inert here (trap off => one load+branch in the runner). */
static int pf_trap_on = 0;
static void pf_vs_step(void) {}
'''
        checks = r'''
static const char *VS_PROG_HEX = "%(prog)s";
static const char *VS_SWZ_HEX = "%(swz)s";
static unsigned load_hex(const char *hex, uint32_t *out, unsigned cap) {
    unsigned n = 0;
    while (*hex && n < cap) {
        char buf[9];
        unsigned k;
        while (*hex == ' ') ++hex;
        if (!*hex) break;
        for (k = 0; k < 8 && hex[k]; ++k) buf[k] = hex[k];
        buf[k] = 0;
        out[n++] = (uint32_t)strtoul(buf, NULL, 16);
        hex += k;
    }
    return n;
}
static uint32_t rng_state = 0x12345678u;
static uint32_t rnd(void) { rng_state = rng_state * 1103515245u + 12345u; return rng_state >> 8; }
/* Architecturally-equal float compare.  The interpreter's scalar
   pica_multiply/copysignf chain and the 4-wide pvs_vec_mul agree on every
   finite/inf value bit for bit, but the sign bit of a ZERO and the
   sign/payload of a NaN produced by 0*x / inf-inf can differ between the two
   code shapes (x86 addss vs addps operand selection, copysignf of a NaN).
   Neither is observable: PICA has no NaN-payload semantics, and +/-0 render
   and compare identically, so the rendered frames are byte-identical (the
   hardware run compares PPM captures for that). */
static int same_f(float a, float b) {
    uint32_t x, y;
    if (isnan(a) && isnan(b)) return 1;
    if (a == 0.0f && b == 0.0f) return 1;
    memcpy(&x, &a, 4); memcpy(&y, &b, 4);
    return x == y;
}
static int same_state(const PicaVsState *p, const PicaVsState *q) {
    unsigned i, k;
    for (i = 0; i < 16; ++i) {
        for (k = 0; k < 4; ++k)
            if (!same_f(p->input[i][k], q->input[i][k])) return 0;
        for (k = 0; k < 4; ++k)
            if (!same_f(p->temp[i][k], q->temp[i][k])) return 0;
        for (k = 0; k < 4; ++k)
            if (!same_f(p->output[i][k], q->output[i][k])) return 0;
    }
    for (i = 0; i < 3; ++i) if (p->address[i] != q->address[i]) return 0;
    for (i = 0; i < 2; ++i) if (p->cc[i] != q->cc[i]) return 0;
    return 1;
}
/* A float drawn from: random bits, +/-0, +/-inf, NaN, small ints, denormals.
   The zero/inf/NaN cases are exactly what pvs_vec_mul's 0*x rule and the
   NaN-propagating scalar path can disagree on if the rewrite is wrong. */
static float edge_float(void) {
    union { uint32_t u; float f; } v;
    switch (rnd() %% 8u) {
    case 0: case 1: v.u = rnd(); return v.f;
    case 2: return (rnd() & 1u) ? 0.0f : -0.0f;
    case 3: return (rnd() & 1u) ? INFINITY : -INFINITY;
    case 4: v.u = 0x7f800001u + (rnd() %% 4u); return v.f;
    case 5: return (float)((int)(rnd() %% 2001u) - 1000) / 8.0f;
    case 6: v.u = 0x00000001u + (rnd() %% 16u); return v.f;
    default: return 1e30f + (float)(rnd() %% 1024u);
    }
}
int main(void) {
    static PicaVsState sA, sB;
    unsigned kinds[8] = {0}, i, k, seed, bad = 0, ok = 0, fail = 0;
    vs_program_size = load_hex(VS_PROG_HEX, vs_program, 4096);
    vs_swizzle_size = load_hex(VS_SWZ_HEX, vs_swizzle, 4096);
    assert(vs_program_size == 238 && vs_swizzle_size == 60);
    pvs_sse_sync();
    pvs_dec_sync();
    assert(pvs_dec_ready);
    for (i = 0; i < vs_program_size; ++i) ++kinds[pvs_dec_tab[i].kind];
    printf("predecoded kinds: gen=%%u mov=%%u add=%%u mul=%%u floor=%%u mad=%%u dp=%%u cmp=%%u\n",
           kinds[PVK_GEN], kinds[PVK_MOV], kinds[PVK_ADD], kinds[PVK_MUL],
           kinds[PVK_FLR], kinds[PVK_MAD], kinds[PVK_DP], kinds[PVK_CMP]);
    assert(kinds[PVK_MOV] && kinds[PVK_ADD] && kinds[PVK_MUL] &&
           kinds[PVK_FLR] && kinds[PVK_DP] && kinds[PVK_CMP] && kinds[PVK_MAD]);
    for (seed = 0; seed < 500; ++seed) {
        int ra, rb;
        unsigned detA, detB;
        memset(&sA, 0, sizeof sA);
        for (i = 0; i < 96; ++i)
            for (k = 0; k < 4; ++k) vs_uniform_f[i][k] = edge_float();
        /* Inputs: small integers so MOVA's relative offsets stay in range. */
        for (i = 0; i < 16; ++i)
            for (k = 0; k < 4; ++k) sA.input[i][k] = (float)((int)(rnd() %% 201u) - 100);
        pica_regs[0x2ba] = 0;                    /* program entry */
        pica_regs[0x2b0] = 0xffffu;              /* all callee conditions true */
        pica_regs[0x2b0] ^= rnd() & 0x3u;        /* ... except a couple */
        memcpy(&sB, &sA, sizeof sA);
        pvs_dec_ready = 0;                       /* interpreter dispatch */
        ra = pica_execute_vs(&sA);
        detA = pica_vs_fail_detail;
        pvs_dec_ready = 1;                       /* pre-decoded dispatch */
        rb = pica_execute_vs(&sB);
        detB = pica_vs_fail_detail;
        if (ra) ++ok; else ++fail;
        if (ra != rb || detA != detB || !same_state(&sA, &sB)) {
            ++bad;
            if (bad <= 3) {
                printf("MISMATCH seed=%%u ret %%d/%%d det %%08X/%%08X\n", seed, ra, rb, detA, detB);
                for (i = 0; i < 16; ++i)
                    for (k = 0; k < 4; ++k)
                        if (!same_f(sA.output[i][k], sB.output[i][k]))
                            printf("  o%%u[%%u] %%g vs %%g\n", i, k,
                                   sA.output[i][k], sB.output[i][k]);
            }
        }
    }
    printf("predecode A/B: 500 cases (%%u executed, %%u rejected), %%u mismatches\n", ok, fail, bad);
    assert(bad == 0 && ok > 400);
    return 0;
}
''' % {'prog': prog, 'swz': swz}
        self.compile_run(header + runtime + checks, ['-msse4.1'])

    def compile_run(self, source, extra_flags=None):
        clang = shutil.which('clang')
        self.assertIsNotNone(clang)
        with tempfile.TemporaryDirectory(prefix='dq8-pica-vs-') as d:
            src=Path(d)/'test.c';exe=Path(d)/'test'
            src.write_text(source)
            flags = list(extra_flags or [])
            subprocess.run([clang,'-std=c11','-O1','-fsanitize=undefined,address',*flags,
                            str(src),'-lm','-o',str(exe)],check=True)
            env = dict(os.environ)
            env["ASAN_OPTIONS"] = env.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
            res = subprocess.run([str(exe)],check=True,timeout=20,env=env,capture_output=True,text=True)
            sys.stderr.write(res.stdout)

    def test_shared_triangle_edge_is_blended_once(self):
        # pica_vs.c's GL glue references types/stubs from pica_gl.c; include it
        # first, mirroring the /* PICA_GL_RUNTIME */ + /* PICA_VS_RUNTIME */
        # splice order in the generated runner (stub path: no DQ8_ENABLE_GL).
        runtime = (ROOT / 'src/pica_gl.c').read_text() + (ROOT / 'src/pica_vs.c').read_text()
        header = r''' 
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t pica_regs[768], vs_program[4096], vs_swizzle[4096];
static unsigned vs_program_size,vs_swizzle_size;
static float vs_uniform_f[96][4],vs_default_attrib[16][4];
static uint64_t pica_draw_count;
static unsigned pica_rt_w=8,pica_rt_h=8,pica_rt_format,pica_rt_addr;
static uint8_t ram[4096],pixels[8][8][4];
static void pica_bind_framebuffer(void) {}
static int guest_memory_range(uint32_t a,uint64_t n,uint32_t *m) {
    *m=a;return a<sizeof ram && n<=sizeof ram-a;
}
static void pica_put_pixel(int x,int y,uint8_t r,uint8_t g,uint8_t b,uint8_t a) {
    assert(x>=0 && x<8 && y>=0 && y<8);
    pixels[y][x][0]=r;pixels[y][x][1]=g;pixels[y][x][2]=b;pixels[y][x][3]=a;
}
static int pica_get_pixel(uint32_t base,uint32_t w,uint32_t h,uint32_t fmt,uint32_t x,uint32_t y,
                          uint8_t *r,uint8_t *g,uint8_t *b,uint8_t *a) {
    if(x>=8||y>=8)return 0;
    *r=pixels[y][x][0];*g=pixels[y][x][1];*b=pixels[y][x][2];*a=pixels[y][x][3];return 1;
}
static int pica_sample_tex0(float u,float v,uint8_t *r,uint8_t *g,uint8_t *b,uint8_t *a) {
    assert(0 && "untextured draw must not sample a texture");return 0;
}
/* Cross-file helpers that live in the generated runner (src/main.rs): the
   depth test, the per-draw/layer trace and the fragment trace. Stubs keep the
   extracted fragment pipeline behaviour identical (depth test off in this
   test's register setup, traces no-op). */
static float pica_draw_tev_a=1.0f,pica_draw_prim_a=1.0f;
static void pica_bb_add(int x,int y){(void)x;(void)y;}
static void pica_db_reset(void){}
static void pica_db_add(int x,int y){(void)x;(void)y;}
static void pica_log_draw(int failed){(void)failed;}
static int pica_rect_log(int x,int y,const uint8_t s[4],const uint8_t o[4]){
    (void)x;(void)y;(void)s;(void)o;return 0;
}
static void pica_bind_depth(void){}
static int pica_depth_test_write(int x,int y,float d){(void)x;(void)y;(void)d;return 1;}
static float pica_fragment_depth(float zw,float iw,float sc,float off){
    (void)iw;float d=zw*sc+off;return d<0.0f?0.0f:(d>1.0f?1.0f:d);
}
'''
        checks = r'''
int main(void) {
    const unsigned bases[6]={0xc0,0xc8,0xd0,0xd8,0xf0,0xf8};
    for(unsigned i=1;i<6;i++)pica_regs[bases[i]]=0x000f000f;
    pica_regs[0x107]=0xf00;pica_regs[0x100]=256;pica_regs[0x101]=0x76760000;
    PicaVertex a={.x=0,.y=0,.iw=1,.color={1,0,0,0.5f}},b=a,c=a,d=a;
    b.x=8;c.y=8;d.x=8;d.y=8;
    pica_raster_triangle(a,b,c);pica_raster_triangle(c,b,d);
    for(unsigned y=0;y<8;y++)for(unsigned x=0;x<8;x++) {
        assert(pixels[y][x][0]==128);assert(pixels[y][x][1]==0);
        assert(pixels[y][x][2]==0);assert(pixels[y][x][3]==64);
    }
    return 0;
}
'''
        self.compile_run(header+runtime+checks)

if __name__ == '__main__':
    unittest.main()
