/* Bounded PICA vertex-shader executor. Instructions and operands come from
   guest command uploads; no resource-name or screen-layout substitutions. */
/* DQ8_TRACE_TIME flags (defined once here; used by pica_write_fragment and
   the pqt_account aggregator near the end of this file). */
static int pqt_on = -1;
/* Set when any fragment in the current draw took the read-modify-write blend
   path (pica_write_fragment's non-fast branch: get_pixel + blend + put_pixel
   per pixel).  This is the true "semi-transparent" signal — the blend unit can
   be enabled while an opaque (alpha=1) draw still takes the cheap put-only
   fast path, so the blend register alone over-counts translucent work. */
static int pqt_slow;
/* t_275eb521: env gates for knobs read from the per-draw / per-flush paths
   (getenv walks `environ` on every call; dq8_env_on() is defined in the core
   runtime and resolves each one once per process). */
static int pvs_flush_trace_gate = -1; /* DQ8_GL_FLUSH_TRACE */
static int pvs_texlog_gate = -1;      /* DQ8_GL_TEXLOG */
static int pvs_gl_debug_gate = -1;    /* DQ8_GL_DEBUG */
/* Forward decls for helpers defined later in this file (used by the per-draw
   snapshot below). */
static float pica_f24_to_f32(uint32_t raw);
static int pica_px_find(int x, int y);
/* ---- DQ8_TRACE_VSDUMP per-instruction trace (t_664244f9): state declared and
   the tiny helpers DEFINED here, before pica_execute_vs, because
   tools/test_pica_vs.py compiles this file only up to the pica_sat helper
   and would otherwise lose the definitions.  Full dump writer (registers,
   uniforms, program/swizzle tables, per-vertex state) lives near
   pica_load_vertex.  Inert without DQ8_TRACE_VSDUMP. */
#include <stdio.h>
#include <stdlib.h>
static FILE *pvs_f;
/* t_e1773c51: per-thread.  pica_load_vertex() assigns it per vertex (line
   ~2570) and pica_vs_finish()/pica_execute_vs() read it; the vertex pool runs
   those on 8 threads at once, so a single global word was written by every
   worker for every vertex (a data race, benign output but UB and it can flip
   another worker's trace decision when DQ8_TRACE_VSDUMP is armed). */
static __thread unsigned pvs_trace;
static unsigned pvs_proj_trace; /* current vertex was instruction-traced: print VSPROJ */
static void pvs_flow(unsigned pc, uint32_t w, unsigned next) {
    if (!pvs_f || !pvs_trace) return;
    fprintf(pvs_f, "VSF pc=%u w=%08X next=%u\n", pc, w, next);
}
static void pvs_ins(unsigned pc, uint32_t w, const float a[4], const float b[4],
                    const float c[4], const float v[4], unsigned dst, unsigned mask) {
    unsigned k;
    if (!pvs_f || !pvs_trace) return;
    fprintf(pvs_f, "VSI pc=%u w=%08X", pc, w);
    if (a) { fprintf(pvs_f, " a="); for (k = 0; k < 4; k++) fprintf(pvs_f, "%s%+.9g", k ? "," : "", a[k]); }
    if (b) { fprintf(pvs_f, " b="); for (k = 0; k < 4; k++) fprintf(pvs_f, "%s%+.9g", k ? "," : "", b[k]); }
    if (c) { fprintf(pvs_f, " c="); for (k = 0; k < 4; k++) fprintf(pvs_f, "%s%+.9g", k ? "," : "", c[k]); }
    if (v) { fprintf(pvs_f, " v="); for (k = 0; k < 4; k++) fprintf(pvs_f, "%s%+.9g", k ? "," : "", v[k]); }
    fprintf(pvs_f, " dst=%u mask=%X\n", dst, mask);
}
/* ---- Per-draw fragment-state snapshot + alpha-blend fast path (t_568f6976)
   The translucent RMW path re-validated and re-decoded, for EVERY fragment,
   things that are constant for the whole draw: the render-target and
   depth-buffer guest ranges (guest_memory_range in get/put/test), the Morton
   offset math, the f24 depth scale/offset registers, and the blend config
   (8 pica_blend_factor dispatches per pixel).  Every one of those global
   reads also sits behind the per-pixel ram[] stores, so the optimiser cannot
   hoist any of it even at -O2 — that is the measured ~0.45 us/px.

   pica_draw_state_build() snapshots the draw's state once, after
   bind_framebuffer/bind_depth and before the first fragment; the fast path
   then runs on cached values with BIT-IDENTICAL float math (the same
   expressions in the same order as the old factor/blend code).  The depth
   test/write semantics are not changed at all (UI layering depends on them —
   see the pica_bind_depth comment in the generated runtime); they are only
   re-expressed on a mapping resolved once per draw.

   Fast-path scope: RGBA8 render targets with the game's UI blend config
   0x76760000 (SRC_ALPHA / ONE_MINUS_SRC_ALPHA for color and alpha, both ADD)
   and a full color mask — that is the same cfg the old put-only opaque fast
   path special-cases, so it is the config every translucent UI window uses.
   Any other blend config keeps the original generic path byte-for-byte.

   DQ8_BLEND_FAST=0 skips the snapshot entirely (read once at first draw),
   restoring the pre-task per-pixel path byte-for-byte; the same switch is
   the A/B reference for tools/test_pica_blend_diff.py and the scene runs. */
static int pica_blend_fast_on = -1;
/* Coverage counters (DQ8_TRACE_TIME reports): RMW fragments that took the
   fast path vs the generic fallback. */
static unsigned long long pqt_fast_px, pqt_gen_px;
typedef struct {
    int built;            /* snapshot taken for the current draw */
    uint8_t *rt;          /* ram + mapped RT base (RGBA8 only, else NULL) */
    uint8_t *depth;       /* ram + mapped depth base (NULL = old code
                             pass-through: unbound or unmappable depth) */
    unsigned depth_bpp;   /* cached pica_depth_bpp */
    uint32_t depth_zmax;
    float depth_scale, depth_offset;   /* f24-decoded regs 0x4D / 0x4E */
    int depth_z_mode1;    /* pica_regs[0x6D] & 1 (Z-buffering vs W) */
    int depth_test, depth_write, depth_func;
    uint32_t rt_w, rt_h;  /* cached dimensions for the inner loop */
    int alpha_on, alpha_func; unsigned alpha_ref;   /* reg 0x104 decode */
    int opaque_cfg;       /* mask==0xf00 && blend on && cfg==0x76760000 */
    int fast;             /* specialized blend path armed (see above) */
} PicaDrawState;
static PicaDrawState pds;
/* Morton color/depth index LUTs (same tables the per-pixel code uses). */
static const uint32_t pica_morton_xl[8] = {0x00,0x01,0x04,0x05,0x10,0x11,0x14,0x15};
static const uint32_t pica_morton_yl[8] = {0x00,0x02,0x08,0x0a,0x20,0x22,0x28,0x2a};
/* byte/255 as the exact float the old dest[k]=byte/255.0f produced (both are
   the correctly-rounded IEEE single for the same value). */
static float pica_unit255[256];
static void pica_draw_state_build(void) {
    uint32_t mapped;
    memset(&pds, 0, sizeof pds);
    if (pica_blend_fast_on < 0) {
        const char *e = getenv("DQ8_BLEND_FAST");
        pica_blend_fast_on = !(e && e[0] == '0');
    }
    if (!pica_blend_fast_on) return;
    {
        static int unit_init;
        if (!unit_init) {
            unsigned i;
            for (i = 0; i < 256; i++) pica_unit255[i] = (float)i / 255.0f;
            unit_init = 1;
        }
    }
    pds.rt_w = pica_rt_w;
    pds.rt_h = pica_rt_h;
    /* Depth snapshot: mirrors pica_depth_range_ok / pica_depth_test_write's
       setup exactly; a NULL base reproduces the old pass-through return. */
    if (pica_depth_bound && pica_depth_range_ok() &&
        guest_memory_range(pica_depth_addr,
                           (uint64_t)pica_rt_w * pica_rt_h * pica_depth_bpp,
                           &mapped)) {
        uint32_t mask = pica_regs[0x107];
        pds.depth = ram + mapped;
        pds.depth_bpp = pica_depth_bpp;
        pds.depth_zmax = (pica_depth_bits == 16u) ? 0xFFFFu : 0xFFFFFFu;
        pds.depth_test = mask & 1u;
        pds.depth_write = (mask & (1u << 12)) && pica_depth_allow_write;
        pds.depth_func = (mask >> 4) & 7u;
    }
    pds.depth_scale = pica_f24_to_f32(pica_regs[0x4D]);
    pds.depth_offset = pica_f24_to_f32(pica_regs[0x4E]);
    pds.depth_z_mode1 = pica_regs[0x6D] & 1u;
    {   /* alpha test + opaque-cfg decode (per-draw, was per-fragment) */
        uint32_t t = pica_regs[0x104];
        pds.alpha_on = t & 1u;
        pds.alpha_func = (t >> 4) & 7u;
        pds.alpha_ref = (t >> 8) & 255u;
        pds.opaque_cfg = ((pica_regs[0x107] & 0xf00u) == 0xf00u) &&
                         (pica_regs[0x100] & 256u) &&
                         pica_regs[0x101] == 0x76760000u;
    }
    /* Color RT snapshot: the specialized blend path is RGBA8-only. */
    if (pica_rt_format == 0u &&
        guest_memory_range(pica_rt_addr,
                           (uint64_t)pica_rt_w * pica_rt_h * 4u, &mapped)) {
        pds.rt = ram + mapped;
        pds.fast = ((pica_regs[0x107] & 0xf00u) == 0xf00u) &&
                   (pica_regs[0x100] & 256u) &&
                   pica_regs[0x101] == 0x76760000u;
    }
    pds.built = 1;
}
/* Depth value for a fragment on the snapshotted state: identical expression
   to pica_fragment_depth (zw*scale+offset, then /w unless Z-buffering). */
static float pica_fragment_depth_d(float zw, float iw_interp) {
    float depth = zw * pds.depth_scale + pds.depth_offset;
    if (!pds.depth_z_mode1 && iw_interp > 0.0f)
        depth /= iw_interp;
    return depth < 0.0f ? 0.0f : (depth > 1.0f ? 1.0f : depth);
}
static uint32_t pica_depth_off_d(int x, int y) {
    uint32_t px = (uint32_t)x, py = (uint32_t)y;
    uint32_t i = pica_morton_xl[px & 7u] + pica_morton_yl[py & 7u];
    uint32_t row = (py >> 3) * (pds.rt_w >> 3) * 64u + (px & ~7u) * 8u;
    return (row + i) * pds.depth_bpp;
}
/* Depth test+write on the snapshotted state: identical semantics to
   pica_depth_test_write (same clamps, truncation, compare funcs, byte
   layout), minus the per-fragment range revalidation. */
static int pica_depth_test_write_d(int x, int y, float depth) {
    int my;
    uint8_t *p;
    float d;
    uint32_t z;
    if (!pds.depth) return 1;
    my = (int)pds.rt_h - 1 - y;
    if (x < 0 || my < 0 || (uint32_t)x >= pds.rt_w || (uint32_t)my >= pds.rt_h) return 1;
    p = pds.depth + pica_depth_off_d(x, my);
    d = depth < 0.0f ? 0.0f : (depth > 1.0f ? 1.0f : depth);
    z = (uint32_t)(d * (float)pds.depth_zmax);     /* Azahar truncates */
    if (pds.depth_test) {
        uint32_t ref;
        int pass;
        if (pds.depth_bpp == 2u) { uint16_t v; memcpy(&v, p, 2); ref = v; }
        else ref = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
        switch (pds.depth_func) {
        case 0u: pass = 0; break;
        case 1u: pass = 1; break;
        case 2u: pass = z == ref; break;
        case 3u: pass = z != ref; break;
        case 4u: pass = z < ref; break;
        case 5u: pass = z <= ref; break;
        case 6u: pass = z > ref; break;
        default: pass = z >= ref; break;
        }
        if (!pass) {
            int pi = pica_px_find(x, y);
            if (pi >= 0)
                fprintf(stderr, "PXF t=%.2f draw=%llu px=%d,%d z=%06X ref=%06X f=%u\n",
                        dq8_layer_t(), (unsigned long long)pica_draw_count, x, y,
                        z, ref, pds.depth_func);
            return 0;
        }
    }
    if (pds.depth_write) {
        p[0] = (uint8_t)(z & 0xFFu);
        p[1] = (uint8_t)((z >> 8) & 0xFFu);
        if (pds.depth_bpp >= 3u) p[2] = (uint8_t)((z >> 16) & 0xFFu);
    }
    return 1;
}
static float pica_f24_to_f32(uint32_t raw) {
    uint32_t sign = (raw & 0x800000u) << 8, exp = (raw >> 16) & 127u;
    uint32_t mant = raw & 65535u, bits;
    if (!exp) {
        float f = ldexpf((float)mant, -78);
        return sign ? -f : f;
    }
    bits = sign | ((exp == 127u ? 255u : exp + 64u) << 23) | (mant << 7);
    float f; memcpy(&f, &bits, 4); return f;
}
static void pica_unpack_f24(const uint32_t q[3], float v[4]) {
    v[0] = pica_f24_to_f32(q[2] & 0xffffffu);
    v[1] = pica_f24_to_f32(((q[1] & 65535u) << 8) | (q[2] >> 24));
    v[2] = pica_f24_to_f32(((q[0] & 255u) << 16) | (q[1] >> 16));
    v[3] = pica_f24_to_f32(q[0] >> 8);
}
typedef struct {
    float input[16][4], temp[16][4], output[16][4];
    int address[3], cc[2];
} PicaVsState;
static float pica_multiply(float a, float b) {
    return a == 0.0f || b == 0.0f ? copysignf(0.0f, a*b) : a*b;
}
/* ---- 4-wide vertex-shader core (t_5a3c517a) -----------------------------
   The interpreter below runs ~97 microcode instructions per vertex, and the
   measured cost was ~75 cycles per instruction: almost all of that was
   pica_vs_source()'s per-component loop (4 shift+mask+load+sign-select+store
   per operand, 2-3 operands per instruction).  The DQ8 vertex shader uses a
   swizzle on virtually every operand (399/394 operands: only 0.5 % are the
   identity permute) but the swizzles are simple permutes of one register, so
   one SSSE3 pshufb with a precomputed mask replaces the whole loop, and the
   arithmetic itself is one mul/add on all four lanes.  With -msse4.1 the
   runtime TU gets:
     source  = xor(pshufb(load(reg), perm_mask), sign_mask)
     add/mul/mov/mad = one vector op
     write   = blend by the 4-bit write mask (plain store when mask==0xf)
   pica_multiply()'s zero special case is preserved exactly in vector form
   (pvs_vec_mul) so results stay bit-identical to the scalar interpreter. */
#if defined(__SSE4_1__)
#include <emmintrin.h>
#include <tmmintrin.h>
#include <smmintrin.h>
#include <immintrin.h>   /* t_e1773c51: AVX2 8-lane batch */
#define PVS_SSE 1
typedef struct { __m128i perm; __m128 sign; } PvsSwzVec;
static PvsSwzVec pvs_swzv[256 * 3];       /* [desc][operand] */
static unsigned char pvs_swzv_ready[256];
static __m128 pvs_wmask[16];              /* write-mask lanes */
static __m128 pvs_ones4;
static const float pvs_ones_f[4] = {1, 1, 1, 1};
static int pvs_sse_init_done;
static void pvs_sse_build_swz(void) {
    unsigned d, o, k, x;
    for (d = 0; d < 256; ++d) {
        uint32_t sw = d < vs_swizzle_size ? vs_swizzle[d] : 0;
        for (o = 0; o < 3; ++o) {
            static const unsigned shifts[3] = {11, 20, 29}, signs[3] = {4, 13, 22};
            signed char idx[16];
            float sg[4];
            int neg = (int)((sw >> signs[o]) & 1u);
            for (k = 0; k < 4; ++k) {
                unsigned sel = (sw >> (shifts[o] - 2 * k)) & 3u;
                for (x = 0; x < 4; ++x) idx[4 * k + x] = (signed char)(sel * 4 + x);
                sg[k] = neg ? -0.0f : 0.0f;   /* sign-bit XOR mask */
            }
            memcpy(&pvs_swzv[d * 3 + o].perm, idx, 16);
            memcpy(&pvs_swzv[d * 3 + o].sign, sg, 16);
        }
        pvs_swzv_ready[d] = 1;
    }
}
/* The swizzle table is uploaded by the guest, so the mask table has to follow
   it.  The fingerprint is taken once per DRAW (60 words), not per vertex. */
static uint64_t pvs_swz_fp;
static unsigned pvs_swz_fp_n;
static void pvs_sse_sync(void) {
    uint64_t h = 1469598103934665603ULL;
    unsigned i, x;
    for (i = 0; i < vs_swizzle_size; ++i) {
        h ^= vs_swizzle[i];
        h *= 1099511628211ULL;
    }
    if (pvs_sse_init_done && h == pvs_swz_fp && vs_swizzle_size == pvs_swz_fp_n) return;
    pvs_swz_fp = h;
    pvs_swz_fp_n = vs_swizzle_size;
    if (!pvs_sse_init_done) {
        pvs_sse_init_done = 1;
        for (x = 0; x < 16; ++x) {
            /* FULL-WIDTH lanes (0xFFFFFFFF), not -1.0f: the write-mask blend
               is a bitwise select, and -1.0f is 0x80000000, i.e. only the
               sign bit.  With the float form every masked store kept the OLD
               magnitude and took the new sign (found by the t_469b3d92
               pre-decode A/B test: MOV/ADD/MUL/MAD with mask != f produced
               old-magnitude garbage).  Unmasked stores (mask == f) never
               touch this table. */
            uint32_t m[4];
            m[0] = (x >> 3) & 1 ? 0xFFFFFFFFu : 0u;
            m[1] = (x >> 2) & 1 ? 0xFFFFFFFFu : 0u;
            m[2] = (x >> 1) & 1 ? 0xFFFFFFFFu : 0u;
            m[3] = x & 1 ? 0xFFFFFFFFu : 0u;
            memcpy(&pvs_wmask[x], m, 16);
        }
        memcpy(&pvs_ones4, pvs_ones_f, 16);
    }
    pvs_sse_build_swz();
}
static inline const float *pvs_vec_src(const PicaVsState *s, unsigned n, unsigned rel) {
    if (n < 16) return s->input[n];
    if (n < 32) return s->temp[n - 16];
    {
        unsigned i = n - 32;
        if (rel) {
            int off = s->address[rel - 1];
            if (off < -128 || off > 127) off = 0;
            i = (i + off) & 127;
        }
        return i < 96 ? vs_uniform_f[i] : pvs_ones_f;
    }
}
static inline __m128 pvs_vec_apply(const PicaVsState *s, unsigned n, unsigned rel,
                                   const PvsSwzVec *w) {
    __m128 v = _mm_loadu_ps(pvs_vec_src(s, n, rel));
    return _mm_xor_ps(_mm_castsi128_ps(_mm_shuffle_epi8(_mm_castps_si128(v), w->perm)),
                      w->sign);
}
static inline __m128 pvs_vec_opnd(const PicaVsState *s, unsigned n, unsigned rel,
                                  unsigned desc, unsigned operand) {
    return pvs_vec_apply(s, n, rel, &pvs_swzv[desc * 3 + operand]);
}
/* pica_multiply(a,b) for four lanes: 0*x would be NaN for inf/NaN x, the
   hardware (and the scalar path) produce a signed zero instead. */
static inline __m128 pvs_vec_mul(__m128 a, __m128 b) {
    __m128 zero = _mm_setzero_ps();
    __m128 r = _mm_mul_ps(a, b);
    __m128 z = _mm_or_ps(_mm_cmpeq_ps(a, zero), _mm_cmpeq_ps(b, zero));
    __m128 s = _mm_and_ps(r, _mm_set1_ps(-0.0f));
    return _mm_or_ps(_mm_andnot_ps(z, r), _mm_and_ps(z, s));
}
/* ---- pre-decoded VS dispatch (t_469b3d92) --------------------------------
   After the 4-wide core the remaining cost of pica_execute_vs is DISPATCH: for
   every one of the ~97 microcode instructions a vertex executes, the loop
   re-derives op, the mad/inv operand layout, the destination register, the
   swizzle descriptor, its table entry and hence the write mask - a dozen
   dependent ALU ops and 3-4 unpredictable branches per instruction, all of it
   a pure function of the program word (which the guest uploads once and the
   draw loop then re-uses for the whole draw).

   So decode the program ONCE per (program, swizzle table) generation into
   pvs_dec_tab[pc] and let the interpreter switch on a pre-classified kind
   instead.  Only the ops that the DQ8 vertex shader actually executes in
   volume get a kind: MOV, ADD, MUL, FLOOR, MAD (op>=0x30), DP3/DP4/DPH/0x18
   and CMP.  Everything else - flow control, MOVA, transcendentals, MIN/MAX
   and any opcode the PICA supports but this decoder does not classify - keeps
   PVK_GEN and runs the original interpreter path unchanged, so behaviour,
   including every failure detail code, is identical.

   The decoded entry carries the operand register numbers/rel-index/segments
   exactly as the scalar path computes them, plus the swizzle-mask pointer, so
   the fast path is one load, one pshufb, one xor and the arithmetic.  The
   float math is the SAME intrinsics in the SAME order as before (pvs_vec_mul
   keeps pica_multiply's zero rule; DP accumulates p[0]+p[1]+p[2](+p[3]) in
   scalar order starting from 0.0f, so the result is bit-identical). */
/* t_6f9e3fed: extended pre-decode.  DQ8_VSDEC_HIST (see pvs_ghist_*) showed
   the field vertex shader still took the generic interpreter for 36.2 % of all
   executed microcode instructions, and that 73 % of those were flow control
   (0x21/0x22, 0x24..0x28, 0x2C/0x2D) with the rest RCP/RSQ (0x0E/0x0F),
   MOVA (0x12) and MIN/MAX/SLT/SGE (0x0C/0x0D/0x09/0x0A/0x1A/0x1B) — i.e. the
   hot ops were paying the full op re-derivation (op extract, mad/inv layout,
   four-way if chain, target/n shifts) per instruction on top of their work.
   Classify them once per program upload instead:
     * flow kinds carry the decoded target/n/condition index, so the dispatch
       becomes one kind compare plus the condition evaluation and the same
       calls[]/branches[] scope bookkeeping;
     * the arithmetic kinds reuse the existing non-mad operand decode and only
       add the operation, with the SAME scalar float expressions (fminf/fmaxf,
       1.0f/a[0], 1.0f/sqrtf(a[0]), a>=b -> 1.0f) so results stay
       bit-identical to the interpreter, which still handles every opcode that
       is not classified here. */
enum { PVK_GEN = 0, PVK_MOV, PVK_ADD, PVK_MUL, PVK_FLR, PVK_MAD, PVK_DP, PVK_CMP,
       PVK_RCP, PVK_RSQ, PVK_MIN, PVK_MAX, PVK_SLT, PVK_SGE, PVK_MOVA,
       PVK_END, PVK_JMP, PVK_CALL, PVK_CALLZ, PVK_CALLNZ, PVK_JMPC, PVK_JMPZ,
       PVK_IFU, PVK_IFC };
typedef struct {
    uint8_t  kind;
    uint8_t  dst, msk;
    uint8_t  o_a, o_b, o_c;       /* source register numbers */
    uint8_t  r_a, r_b, r_c;       /* relative-addressing selector (0 = none) */
    uint16_t sw_a, sw_b, sw_c;    /* &pvs_swzv[desc*3 + operand] */
    uint8_t  dpn, dph;            /* PVK_DP: lanes (3/4), + b[3] */
    uint8_t  cmp0, cmp1;          /* PVK_CMP compare selectors */
    uint16_t tgt;                 /* flow: decoded branch target */
    uint8_t  n;                   /* flow: call/branch scope length */
    uint8_t  idx;                 /* flow (bool predicated): (w>>22)&15 */
} PvsDec;
static PvsDec *pvs_dec_tab;
static unsigned pvs_dec_cap;
static int pvs_dec_ready, pvs_dec_on = -1;   /* DQ8_VSPREDEC=0 restores dispatch */
static uint64_t pvs_dec_hash;
static void pvs_dec_build(unsigned n) {
    unsigned pc;
    if (pvs_dec_cap == 0 || n > pvs_dec_cap) {
        unsigned cap = (n ? n : 1u) + 64u;
        PvsDec *t = (PvsDec *)realloc(pvs_dec_tab, cap * sizeof *t);
        if (!t) { pvs_dec_tab = NULL; pvs_dec_cap = 0; return; }
        pvs_dec_tab = t;
        pvs_dec_cap = cap;
    }
    for (pc = 0; pc < n; ++pc) {
        uint32_t w = vs_program[pc];
        unsigned op = w >> 26, desc, rel;
        int mad = op >= 0x30, inv;
        PvsDec *d = &pvs_dec_tab[pc];
        memset(d, 0, sizeof *d);
        inv = (op >= 0x18 && op <= 0x1b) || (op >= 0x30 && op < 0x38);
        /* t_6f9e3fed: flow control first — these have no swizzle descriptor,
           so they must be classified before the desc check below. */
        if (!mad && (op == 0x22 || op == 0x21 || (op >= 0x24 && op <= 0x28) ||
                     op == 0x2c || op == 0x2d)) {
            d->idx = (uint8_t)((w >> 22) & 15u);
            if (op == 0x24 || op == 0x25 || op == 0x26 ||
                op == 0x27 || op == 0x28) {
                d->tgt = (uint16_t)((w >> 10) & 4095u);
                d->n = (uint8_t)(w & 255u);
            } else if (op == 0x2c || op == 0x2d) {
                d->tgt = (uint16_t)((w >> 10) & 4095u);
            }
            if (op == 0x22) d->kind = PVK_END;
            else if (op == 0x21) d->kind = PVK_JMP;
            else if (op == 0x24) d->kind = PVK_CALL;     /* unconditional */
            else if (op == 0x25) d->kind = PVK_CALLZ;    /* cc condition */
            else if (op == 0x26) d->kind = PVK_CALLNZ;   /* bool register */
            else if (op == 0x27) d->kind = PVK_JMPC;     /* bool register */
            else if (op == 0x28) d->kind = PVK_JMPZ;     /* cc condition */
            else if (op == 0x2c) d->kind = PVK_IFU;      /* cc condition */
            else d->kind = PVK_IFC;                      /* bool register */
            continue;
        }
        if (!mad && !(op == 0x00 || op == 0x01 || op == 0x02 || op == 0x03 ||
                      op == 0x08 || op == 0x0b || op == 0x13 || op == 0x18 ||
                      op == 0x09 || op == 0x0a || op == 0x0c || op == 0x0d ||
                      op == 0x0e || op == 0x0f || op == 0x12 ||
                      op == 0x1a || op == 0x1b ||
                      op == 0x2e || op == 0x2f))
            continue;                        /* PVK_GEN: original interpreter */
        desc = w & (uint32_t)(mad ? 31 : 127);
        if (desc >= vs_swizzle_size) continue;  /* GEN reports detail 2 */
        rel = (w >> (mad ? 22 : 19)) & 3u;
        d->msk = (uint8_t)(vs_swizzle[desc] & 15u);
        if (mad) {
            d->o_a = (uint8_t)((w >> 17) & 31u); d->r_a = 0;
            d->o_b = (uint8_t)(inv ? ((w >> 12) & 31u) : ((w >> 10) & 127u));
            d->r_b = (uint8_t)(inv ? 0u : rel);
            d->o_c = (uint8_t)(inv ? ((w >> 5) & 127u) : ((w >> 5) & 31u));
            d->r_c = (uint8_t)(inv ? rel : 0u);
            d->dst = (uint8_t)((w >> 24) & 31u);
            d->kind = PVK_MAD;
        } else {
            d->o_a = (uint8_t)(inv ? ((w >> 14) & 31u) : ((w >> 12) & 127u));
            d->r_a = (uint8_t)(inv ? 0u : rel);
            d->o_b = (uint8_t)(inv ? ((w >> 7) & 127u) : ((w >> 7) & 31u));
            d->r_b = (uint8_t)(inv ? rel : 0u);
            d->dst = (uint8_t)((w >> 21) & 31u);
            if (op == 0x13) d->kind = PVK_MOV;
            else if (op == 0x00) d->kind = PVK_ADD;
            else if (op == 0x08) d->kind = PVK_MUL;
            else if (op == 0x0b) d->kind = PVK_FLR;
            else if (op == 0x0c) d->kind = PVK_MAX;   /* fmaxf(a,b) */
            else if (op == 0x0d) d->kind = PVK_MIN;   /* fminf(a,b) */
            else if (op == 0x0e) d->kind = PVK_RCP;   /* 1.0f/a[0] */
            else if (op == 0x0f) d->kind = PVK_RSQ;   /* 1.0f/sqrtf(a[0]) */
            else if (op == 0x12) d->kind = PVK_MOVA;  /* address update */
            else if (op == 0x09 || op == 0x1a) d->kind = PVK_SGE;
            else if (op == 0x0a || op == 0x1b) d->kind = PVK_SLT;
            else if (op == 0x01 || op == 0x02 || op == 0x03 || op == 0x18) {
                d->kind = PVK_DP;
                d->dpn = (uint8_t)(op == 0x02 ? 4 : 3);
                d->dph = (uint8_t)((op == 0x03 || op == 0x18) ? 1 : 0);
            } else {
                d->kind = PVK_CMP;
                d->cmp0 = (uint8_t)((w >> 24) & 7u);
                d->cmp1 = (uint8_t)((w >> 21) & 7u);
            }
        }
        d->sw_a = (uint16_t)(desc * 3u);
        d->sw_b = (uint16_t)(desc * 3u + 1u);
        d->sw_c = (uint16_t)(desc * 3u + 2u);
    }
    {
        static int dec_stats = -1;
        static unsigned shown;
        if (dec_stats < 0) {
            const char *e = getenv("DQ8_VSDEC_STATS");
            dec_stats = (e && e[0] && e[0] != '0') ? 1 : 0;
        }
        if (dec_stats && shown < 8u) {
            unsigned h[32] = {0}, k;
            for (k = 0; k < n; ++k) h[pvs_dec_tab[k].kind & 31u]++;
            ++shown;
            fprintf(stderr, "VSDEC size=%u", n);
            for (k = 0; k < 32u; ++k)
                if (h[k]) fprintf(stderr, " k%u=%u", k, h[k]);
            fprintf(stderr, "\n");
        }
    }
}
/* t_6f9e3fed: which opcodes still fall back to the generic interpreter, and
   what share of the executed microcode that is.  DQ8_VSDEC_HIST=1 turns the
   counters on (one increment per executed instruction); the periodic
   DRAWPHASE window prints and resets them.  Profiling only, no effect when
   unset. */
static unsigned long long pvs_ghist[64], pvs_ghist_gen, pvs_ghist_all;
static int pvs_ghist_on = -1;
static void pvs_ghist_init(void) {
    const char *e;
    if (pvs_ghist_on >= 0) return;
    e = getenv("DQ8_VSDEC_HIST");
    pvs_ghist_on = (e && *e && *e != '0') ? 1 : 0;
}
static void pvs_ghist_report(void) {
    unsigned k;
    fprintf(stderr, "VSHIST all=%llu generic=%llu (%.1f%%)",
            pvs_ghist_all, pvs_ghist_gen,
            pvs_ghist_all ? 100.0 * (double)pvs_ghist_gen / (double)pvs_ghist_all
                          : 0.0);
    for (k = 0; k < 64u; ++k)
        if (pvs_ghist[k]) fprintf(stderr, " op%02X=%llu", k, pvs_ghist[k]);
    fprintf(stderr, "\n");
    memset(pvs_ghist, 0, sizeof pvs_ghist);
    pvs_ghist_gen = pvs_ghist_all = 0;
}
/* Re-decode when the guest uploads a different program or swizzle table.  Runs
   once per draw on the main thread, before the vertex pool is published, so
   the workers always see a fully built table. */
static void pvs_dec_sync(void) {
    uint64_t h = 1469598103934665603ULL;
    unsigned i, n = vs_program_size;
    if (pvs_dec_on < 0) {
        const char *e = getenv("DQ8_VSPREDEC");
        pvs_dec_on = (e && e[0] == '0') ? 0 : 1;
    }
    if (!pvs_dec_on) { pvs_dec_ready = 0; return; }
    for (i = 0; i < n; ++i) { h ^= vs_program[i]; h *= 1099511628211ULL; }
    h ^= pvs_swz_fp; h *= 1099511628211ULL;
    h ^= (uint64_t)n; h *= 1099511628211ULL;
    h ^= (uint64_t)vs_swizzle_size;
    if (pvs_dec_ready && h == pvs_dec_hash) return;
    pvs_dec_build(n);
    if (pvs_dec_cap) {
        pvs_dec_hash = h;
        pvs_dec_ready = 1;
    }
}
/* ---- prepared per-program execution plan (t_fdc40e6f) --------------------
   pvs_dec_tab (t_469b3d92) already removes the *bitfield decode* from the
   per-vertex loop, but every one of the ~97 executed microcode instructions
   still re-derives, at run time:
     * where each operand lives  -- pvs_vec_src() branches three ways
     (state input / state temp / uniform block) and then re-checks relative
     addressing in the uniform case;
     * where the destination lives -- a `<16 ? output : temp` select;
     * which swizzle vector applies -- an index multiply off the descriptor;
     * whether instrumentation is armed -- pvs_ghist_on, pf_trap_on,
     pvs_dec_ready and pvs_trace were re-read FROM GLOBALS per instruction.
   perf annotate on the operator's city state put 35 % of pica_load_vertex's
   samples on the single `cmpb pvs_dec_ready` at the top of that loop.

   The plan below is built ONCE per (program words, swizzle table, input map
   0x2BB/0x2BC, output mask 0x2BD, OUTMAP_TOTAL 0x4F, maps 0x50..0x56) and
   cached, so a draw whose program repeats costs nothing to prepare.  Each
   entry carries, fully resolved:
     * the operand address: an ABSOLUTE pointer for uniform / constant-ones
     operands (their storage never moves) and a BYTE OFFSET for state
     registers (selected at run time with a mask, not a branch);
     * the swizzle vector index, the write mask, the destination byte offset
       into the flat 48-register float block (input|temp|output are adjacent);
     * for flow control, the decoded target / length / bool index.
   Everything the plan cannot represent (unclassified opcode, descriptor out
   of the swizzle table, relative uniform addressing, tracing) keeps PVO_GEN
   and runs the existing interpreter body for that pc, with a per-occurrence
   counter -- the plan never silently changes a result.

   DQ8_VSPLAN=0 restores the pre-task path byte for byte (A/B reference). */
enum {
    PVO_GEN = 0, PVO_MOV, PVO_ADD, PVO_MUL, PVO_FLR, PVO_MAD, PVO_DP, PVO_CMP,
    PVO_RCP, PVO_RSQ, PVO_MIN, PVO_MAX, PVO_SLT, PVO_SGE, PVO_MOVA,
    PVO_END, PVO_JMP, PVO_CALL, PVO_CALLZ, PVO_CALLNZ, PVO_JMPC, PVO_JMPZ,
    PVO_IFU, PVO_IFC
};
typedef struct {
    uintptr_t a, b, c;        /* absolute operand pointer, or state byte offset */
    uint16_t sw_a, sw_b, sw_c;/* index into pvs_swzv[] */
    uint16_t dst;             /* byte offset into the 48-register float block */
    uint16_t tgt;             /* flow: decoded branch/call target */
    int16_t  na, nb, nc;      /* operand register numbers (relative path only) */
    uint8_t  op, msk, flags;  /* flags bit0/1/2: operand a/b/c is state-relative */
    uint8_t  rel_a, rel_b, rel_c;
    uint8_t  n, idx, dpn, dph, cmp0, cmp1;
} PvsPlan;
/* t_45ee02dd: 64 slots, not 8.  The city and battle states upload EIGHT
   distinct programs, and every mesh format change (0x2BD output mask,
   0x4F/0x50..0x56 attribute maps) produces a new key for the same program,
   so a 64-field state had > 8 live keys: `skipped_draws` was 26 814 of
   45 000 draws on the field state, i.e. 60 % of the draws silently fell back
   to the interpreter.  A full table still falls back (never evicts). */
#define PVSP_MAX 64
typedef struct {
    uint64_t key;
    unsigned n, gen;          /* program length, PVO_GEN entries */
    unsigned long long verts; /* vertices executed with this plan */
    PvsPlan *tab;
    unsigned nsc;             /* output-map scatter entries */
    struct { uint16_t off; uint8_t sem; } sc[40];
} PvsProg;
static PvsProg pvs_progs[PVSP_MAX];
static int pvs_nprog;
static int pvs_plan_on = -1;              /* DQ8_VSPLAN=0 restores the old path */
static int pvs_stats_on = -1;             /* DQ8_VSPLAN_STATS=1 */
static const PvsPlan *pvs_plan_cur;       /* plan for the draw being executed */
static const PvsProg *pvs_prog_cur;
/* t_e1773c51: the pool participant's plan snapshot.  pvs_plan_cur /
   pvs_prog_cur above are published by the main thread once per draw; a worker
   that is still executing the draw it was woken for must not read whatever
   the main thread has published since.  pvs_body_run() copies the job's plan
   pointer into these per-thread slots for the duration of the job, and every
   reader below goes through the accessors.  Outside a job (serial path,
   DQ8_VSPAR_VERIFY reference pass, tracing) the NULL TLS falls through to the
   globals, so the single-threaded behaviour is unchanged. */
static __thread const PvsPlan *pvs_tls_plan;
static __thread const PvsProg *pvs_tls_prog;
static inline const PvsPlan *pvs_plan_now(void) {
    return pvs_tls_plan ? pvs_tls_plan : pvs_plan_cur;
}
static inline const PvsProg *pvs_prog_now(void) {
    return pvs_tls_prog ? pvs_tls_prog : pvs_prog_cur;
}
static unsigned long long pvs_plan_build_n, pvs_plan_hit_n, pvs_gen_exec,
                          pvs_plan_used, pvs_plan_skipped;
static unsigned long long pvs_plan_hit_ns, pvs_plan_build_ns, pvs_plan_build_max_ns;
static unsigned pvs_plan_progs_seen;
static int pvs_vs4_on = -1;               /* DQ8_VS4=0 disables the 4-lane batch */
/* t_e1773c51: the batch counters used to be process-global words incremented by
   every pool participant on every batch (8 threads writing the same cache line
   per 4 vertices: lost updates AND false sharing).  Each participant now counts
   into its own slot and the report sums the registered slots. */
typedef struct {
    unsigned long long batches, verts, bail_div, bail_plan, scalar,
                       bail_rel, bail_gen, bail_gate, bail_pc;
    /* t_e1773c51 STEP 2: the 8-lane AVX2 batch keeps its own tallies so the
       bail rate of the wider batch can be compared with the 4-lane one. */
    unsigned long long b8_batches, b8_verts, b8_div, b8_plan, b8_rel,
                       b8_gen, b8_pc, b8_gate, b8_fallback;
} Pvs4Cnt;
static __thread Pvs4Cnt pvs4_tls;
#define PVS4_CNT_MAX 16
static Pvs4Cnt *pvs4_cnt_slots[PVS4_CNT_MAX];
static volatile unsigned pvs4_cnt_n;
static void pvs4_cnt_register(void) {
    static __thread int reg;
    unsigned i;
    if (reg) return;
    reg = 1;
    i = __atomic_fetch_add(&pvs4_cnt_n, 1u, __ATOMIC_ACQ_REL);
    if (i < PVS4_CNT_MAX)
        __atomic_store_n(&pvs4_cnt_slots[i], &pvs4_tls, __ATOMIC_RELEASE);
}
static void pvs4_cnt_total(Pvs4Cnt *t) {
    unsigned i, n = __atomic_load_n(&pvs4_cnt_n, __ATOMIC_ACQUIRE);
    memset(t, 0, sizeof *t);
    if (n > PVS4_CNT_MAX) n = PVS4_CNT_MAX;
    for (i = 0; i < n; ++i) {
        Pvs4Cnt *s = __atomic_load_n(&pvs4_cnt_slots[i], __ATOMIC_ACQUIRE);
        if (!s) continue;
        t->batches += s->batches; t->verts += s->verts;
        t->bail_div += s->bail_div; t->bail_plan += s->bail_plan;
        t->scalar += s->scalar; t->bail_rel += s->bail_rel;
        t->bail_gen += s->bail_gen; t->bail_gate += s->bail_gate;
        t->bail_pc += s->bail_pc;
        t->b8_batches += s->b8_batches; t->b8_verts += s->b8_verts;
        t->b8_div += s->b8_div; t->b8_plan += s->b8_plan;
        t->b8_rel += s->b8_rel; t->b8_gen += s->b8_gen;
        t->b8_pc += s->b8_pc; t->b8_gate += s->b8_gate;
        t->b8_fallback += s->b8_fallback;
    }
}
static inline int pvs_vs4_gate(void) {
    if (pvs_vs4_on < 0) {
        /* t_45ee02dd: default ON.  The batch is bit-identical to the scalar
           plan path on 8M-step deterministic runs of city/battle/field and on
           `DQ8_VSPAR_VERIFY=1` over 13-20 M vertices per scene, which is the
           condition this card set for enabling it.  DQ8_VS4=0 is the A/B
           reference. */
        const char *e = getenv("DQ8_VS4");
        pvs_vs4_on = (e && e[0] == '0' && !e[1]) ? 0 : 1;
        pvs4_cnt_register();
    }
    return pvs_vs4_on;
}
static int pvs4_norel = -1;   /* DQ8_VS4_NOREL=1: diagnostic, bail on relative */
static inline int pvs4_norel_gate(void) {
    if (pvs4_norel < 0) {
        const char *e = getenv("DQ8_VS4_NOREL");
        pvs4_norel = (e && e[0] == '1') ? 1 : 0;
    }
    return pvs4_norel;
}
static int pvs4_bm = -1;      /* DQ8_VS4_BAIL=<mask>: diagnostic per-opclass bail */
static inline int pvs4_bailmask_gate(void) {
    if (pvs4_bm < 0) {
        const char *e = getenv("DQ8_VS4_BAIL");
        pvs4_bm = (e && *e) ? (int)strtol(e, NULL, 0) : 0;
    }
    return pvs4_bm;
}
static inline unsigned pvs4_bailcls(unsigned o) {
    switch (o) {
    case PVO_CALLZ: case PVO_CALLNZ: case PVO_JMPC: case PVO_JMPZ:
    case PVO_IFU: case PVO_IFC: return 1u;      /* conditional flow */
    case PVO_MOVA: return 2u;
    case PVO_DP: return 4u;
    case PVO_MIN: case PVO_MAX: case PVO_SLT: case PVO_SGE: return 8u;
    case PVO_RCP: case PVO_RSQ: return 16u;
    case PVO_CMP: return 32u;
    case PVO_MAD: return 64u;
    default: return 0u;
    }
}
/* Operand resolution: state register n (0..31) is 16*n bytes into the state's
   flat float block; 32..127 are uniform slots (or the constant 1,1,1,1 block
   the interpreter returns beyond slot 95).  A relative index (MOVA) stays on
   the interpreter's own source helper. */
static void pvs_plan_opnd(PvsPlan *p, unsigned slot, unsigned n, unsigned rel) {
    uintptr_t addr;
    if (n < 32u) {
        addr = (uintptr_t)n * 16u;
        p->flags |= (uint8_t)(1u << slot);
    } else {
        unsigned i = n - 32u;
        addr = (i < 96u) ? (uintptr_t)(const void *)&vs_uniform_f[i][0]
                         : (uintptr_t)(const void *)pvs_ones_f;
    }
    switch (slot) {
    case 0: p->a = addr; p->na = (int16_t)n; p->rel_a = (uint8_t)rel; break;
    case 1: p->b = addr; p->nb = (int16_t)n; p->rel_b = (uint8_t)rel; break;
    default: p->c = addr; p->nc = (int16_t)n; p->rel_c = (uint8_t)rel; break;
    }
}
static int pvs_plan_build_one(PvsPlan *p, uint32_t w, unsigned pc) {
    unsigned op = w >> 26, desc, rel, dst;
    int mad = op >= 0x30, inv;
    (void)pc;
    memset(p, 0, sizeof *p);
    inv = (op >= 0x18 && op <= 0x1b) || (op >= 0x30 && op < 0x38);
    /* Flow control carries no swizzle descriptor: classify it first. */
    if (!mad && (op == 0x22 || op == 0x21 || (op >= 0x24 && op <= 0x28) ||
                 op == 0x2c || op == 0x2d)) {
        p->idx = (uint8_t)((w >> 22) & 15u);
        if (op >= 0x24 && op <= 0x28) {
            p->tgt = (uint16_t)((w >> 10) & 4095u);
            p->n = (uint8_t)(w & 255u);
        } else if (op == 0x2c || op == 0x2d) {
            p->tgt = (uint16_t)((w >> 10) & 4095u);
        }
        p->op = op == 0x22 ? PVO_END : op == 0x21 ? PVO_JMP :
                op == 0x24 ? PVO_CALL : op == 0x25 ? PVO_CALLZ :
                op == 0x26 ? PVO_CALLNZ : op == 0x27 ? PVO_JMPC :
                op == 0x28 ? PVO_JMPZ : op == 0x2c ? PVO_IFU : PVO_IFC;
        return 0;
    }
    if (!mad && !(op == 0x00 || op == 0x01 || op == 0x02 || op == 0x03 ||
                  op == 0x08 || op == 0x0b || op == 0x13 || op == 0x18 ||
                  op == 0x09 || op == 0x0a || op == 0x0c || op == 0x0d ||
                  op == 0x0e || op == 0x0f || op == 0x12 ||
                  op == 0x1a || op == 0x1b || op == 0x2e || op == 0x2f))
        return 1;                          /* PVO_GEN */
    desc = w & (uint32_t)(mad ? 31 : 127);
    if (desc >= vs_swizzle_size) return 1; /* GEN reports detail 2 */
    rel = (w >> (mad ? 22 : 19)) & 3u;
    p->msk = (uint8_t)(vs_swizzle[desc] & 15u);
    p->sw_a = (uint16_t)(desc * 3u);
    p->sw_b = (uint16_t)(desc * 3u + 1u);
    p->sw_c = (uint16_t)(desc * 3u + 2u);
    if (mad) {
        pvs_plan_opnd(p, 0, (w >> 17) & 31u, 0);
        pvs_plan_opnd(p, 1, inv ? ((w >> 12) & 31u) : ((w >> 10) & 127u),
                      inv ? 0u : rel);
        pvs_plan_opnd(p, 2, inv ? ((w >> 5) & 127u) : ((w >> 5) & 31u),
                      inv ? rel : 0u);
        dst = (w >> 24) & 31u;
        p->op = PVO_MAD;
    } else {
        pvs_plan_opnd(p, 0, inv ? ((w >> 14) & 31u) : ((w >> 12) & 127u),
                      inv ? 0u : rel);
        pvs_plan_opnd(p, 1, inv ? ((w >> 7) & 127u) : ((w >> 7) & 31u),
                      inv ? rel : 0u);
        dst = (w >> 21) & 31u;
        if (op == 0x01 || op == 0x02 || op == 0x03 || op == 0x18) {
            p->op = PVO_DP;
            p->dpn = (uint8_t)(op == 0x02 ? 4 : 3);
            p->dph = (uint8_t)((op == 0x03 || op == 0x18) ? 1 : 0);
        } else if (op == 0x2e || op == 0x2f) {
            p->op = PVO_CMP;
            p->cmp0 = (uint8_t)((w >> 24) & 7u);
            p->cmp1 = (uint8_t)((w >> 21) & 7u);
            return 0;                      /* no destination write */
        } else {
            p->op = op == 0x13 ? PVO_MOV : op == 0x00 ? PVO_ADD :
                    op == 0x08 ? PVO_MUL : op == 0x0b ? PVO_FLR :
                    op == 0x0c ? PVO_MAX : op == 0x0d ? PVO_MIN :
                    op == 0x0e ? PVO_RCP : op == 0x0f ? PVO_RSQ :
                    op == 0x12 ? PVO_MOVA :
                    (op == 0x09 || op == 0x1a) ? PVO_SGE : PVO_SLT;
        }
    }
    /* input[16] | temp[16] | output[16] are adjacent in PicaVsState:
       dst < 16 selects output[dst] (offset 512 + 16*dst), else temp[dst-16]
       (offset 16*dst). */
    p->dst = (uint16_t)(dst < 16u ? 512u + dst * 16u : dst * 16u);
    return 0;
}
static uint64_t pvs_plan_key(unsigned n) {
    uint64_t h = 1469598103934665603ULL;
    unsigned i;
    for (i = 0; i < n; ++i) { h ^= vs_program[i]; h *= 1099511628211ULL; }
    h ^= pvs_swz_fp; h *= 1099511628211ULL;
    h ^= (uint64_t)n; h *= 1099511628211ULL;
    h ^= (uint64_t)vs_swizzle_size; h *= 1099511628211ULL;
    /* Everything else that changes the program's semantics: the input-register
       mapping (0x2BB/0x2BC), the output mask (0x2BD), OUTMAP_TOTAL (0x4F) and
       the packed attribute maps (0x50..0x56).  A mid-program guest write to
       any of these changes the key, so the next draw builds a new entry. */
    for (i = 0x2bbu; i <= 0x2bdu; ++i) { h ^= pica_regs[i]; h *= 1099511628211ULL; }
    h ^= pica_regs[0x4fu]; h *= 1099511628211ULL;
    for (i = 0; i < 7u; ++i) { h ^= pica_regs[0x50 + i]; h *= 1099511628211ULL; }
    return h ? h : 1u;
}
/* Output-map scatter (also folded into the plan): the old per-vertex loop
   walked the mask/maps with bitfield decoding and wrote every semantic id,
   but pica_load_vertex only ever reads ids 0..15 (clip position, colour, two
   uv pairs and the isfinite checks).  Build the (destination float, semantic)
   pairs once per draw, in the same order, so the per-vertex cost is one copy
   per mapped lane instead of a decode per lane, and only ids that are read
   are copied at all. */
static unsigned pvs_plan_scatter(PvsProg *g) {
    unsigned packed = 0, mask = pica_regs[0x2bd] & 65535u,
             attribs = pica_regs[0x4f] & 7u, reg, k, ns = 0;
    for (reg = 0; reg < 16u && packed < attribs && packed < 7u; ++reg) {
        if (!(mask & (1u << reg))) continue;
        for (k = 0; k < 4u; ++k) {
            unsigned id = (pica_regs[0x50 + packed] >> (8 * k)) & 31u;
            if (id >= 16u || ns >= sizeof g->sc / sizeof g->sc[0]) continue;
            g->sc[ns].off = (uint16_t)(512u + reg * 16u + k * 4u);
            g->sc[ns].sem = (uint8_t)id;
            ++ns;
        }
        ++packed;
    }
    g->nsc = ns;
    return ns;
}
/* Build (or find) the plan for the program uploaded right now.  Runs once per
   draw on the main thread, before the vertex pool is published. */
static void pvs_plan_sync(void) {
    unsigned n = vs_program_size, i;
    uint64_t key, t0, tb = 0;
    PvsProg *g = NULL;
    int hit = 0;
    if (pvs_plan_on < 0) {
        const char *e = getenv("DQ8_VSPLAN");
        pvs_plan_on = (e && e[0] == '0' && !e[1]) ? 0 : 1;
    }
    if (pvs_stats_on < 0) {
        const char *e = getenv("DQ8_VSPLAN_STATS");
        pvs_stats_on = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    pvs_plan_cur = NULL;
    pvs_prog_cur = NULL;
    if (!pvs_plan_on || !n) { pvs_plan_skipped++; return; }
    /* The cost of a cache HIT is what the draw pays instead of a build: the key
       hash over the program words (n FNV steps) plus one table compare.  It is
       timed and reported (VSPLAN hit_us) so "a hit is free" is a measurement,
       not a claim. */
    t0 = pvs_stats_on ? pica_pqt_now_ns() : 0;
    key = pvs_plan_key(n);
    for (i = 0; i < (unsigned)pvs_nprog; ++i)
        if (pvs_progs[i].key == key) { g = &pvs_progs[i]; hit = 1; break; }
    if (hit) ++pvs_plan_hit_n;
    if (!g) {
        unsigned gen = 0;
        PvsPlan *tab;
        if (pvs_stats_on) tb = pica_pqt_now_ns();
        if (pvs_nprog >= PVSP_MAX) {
            /* Never evict: a rebuild costs more than it saves and the
               interpreter is the reference path.  Counted, so a table that is
               too small shows up in the report instead of in the frame rate. */
            pvs_plan_skipped++;
            return;
        }
        g = &pvs_progs[pvs_nprog];
        tab = (PvsPlan *)malloc((size_t)n * sizeof *tab);
        if (!tab) { pvs_plan_skipped++; return; }
        for (i = 0; i < n; ++i) gen += (unsigned)pvs_plan_build_one(&tab[i], vs_program[i], i);
        g->key = key;
        g->n = n;
        g->gen = gen;
        g->tab = tab;
        g->verts = 0;
        /* The output-map scatter is a pure function of registers that are part
           of the key (0x2BD mask, 0x4F count, 0x50..0x56 maps), so it is built
           with the plan: a hit does no per-draw decode at all. */
        pvs_plan_scatter(g);
        ++pvs_nprog;
        ++pvs_plan_build_n;
        ++pvs_plan_progs_seen;
        if (pvs_stats_on) {
            uint64_t d = pica_pqt_now_ns() - tb;
            if (d > pvs_plan_build_max_ns) pvs_plan_build_max_ns = d;
            fprintf(stderr, "VSPLAN build prog=%d n=%u generic=%u key=%016llX\n",
                    pvs_nprog - 1, n, gen, (unsigned long long)key);
        }
    }
    pvs_plan_cur = g->tab;
    pvs_prog_cur = g;
    {   /* per-program vertex accounting: draw granularity on the main thread,
           so the shared counters are never written from the pool */
        unsigned cnt = pica_regs[0x228] & 0xffffu;
        g->verts += cnt;
        pvs_plan_used += cnt;
    }
    if (t0) {
        uint64_t d = pica_pqt_now_ns() - t0;
        if (hit) pvs_plan_hit_ns += d;
        else pvs_plan_build_ns += d;
    }
}
static void pvs_plan_report(void) {
    if (pvs_stats_on)
        fprintf(stderr, "VSPLAN programs=%d builds=%llu hits=%llu verts=%llu "
                        "generic_exec=%llu skipped_draws=%llu hit_rate=%.4f%% "
                        "hit_us=%.4f build_us=%.2f build_max_us=%.1f\n",
                pvs_nprog, pvs_plan_build_n, pvs_plan_hit_n, pvs_plan_used,
                pvs_gen_exec, pvs_plan_skipped,
                (pvs_plan_hit_n + pvs_plan_build_n)
                    ? 100.0 * (double)pvs_plan_hit_n /
                      (double)(pvs_plan_hit_n + pvs_plan_build_n) : 0.0,
                pvs_plan_hit_n ? (double)pvs_plan_hit_ns / 1e3 / (double)pvs_plan_hit_n : 0.0,
                pvs_plan_build_n ? (double)pvs_plan_build_ns / 1e3 / (double)pvs_plan_build_n : 0.0,
                (double)pvs_plan_build_max_ns / 1e3);
    if (pvs_stats_on) {
        Pvs4Cnt t;
        pvs4_cnt_total(&t);
        fprintf(stderr, "VS4 batches=%llu verts=%llu bail_div=%llu gate=%llu gen=%llu "
                        "rel=%llu pc=%llu stack=%llu fallback=%llu\n",
                t.batches, t.verts, t.bail_div, t.bail_gate, t.bail_gen,
                t.bail_rel, t.bail_pc, t.bail_plan, t.scalar);
        fprintf(stderr, "VS8 batches=%llu verts=%llu bail_div=%llu gate=%llu gen=%llu "
                        "rel=%llu pc=%llu stack=%llu fallback=%llu rate=%.2f%%\n",
                t.b8_batches, t.b8_verts, t.b8_div, t.b8_gate, t.b8_gen,
                t.b8_rel, t.b8_pc, t.b8_plan, t.b8_fallback,
                (t.b8_batches + t.b8_fallback)
                    ? 100.0 * (double)t.b8_fallback /
                      (double)(t.b8_batches + t.b8_fallback) : 0.0);
    }
}
#endif /* __SSE4_1__ */
static void pica_vs_source(PicaVsState *s, unsigned n, unsigned rel,
                           uint32_t sw, unsigned operand, float v[4]) {
    static const unsigned shifts[3] = {11,20,29}, signs[3] = {4,13,22};
    static const float ones[4] = {1,1,1,1};
    const float *src;
    if (n < 16) src = s->input[n];
    else if (n < 32) src = s->temp[n-16];
    else {
        unsigned i = n-32;
        if (rel) {
            int off = s->address[rel-1];
            if (off < -128 || off > 127) off = 0;
            i = (i + off) & 127;
        }
        src = i < 96 ? vs_uniform_f[i] : ones;
    }
    for (unsigned k=0;k<4;k++) {
        v[k] = src[(sw >> (shifts[operand]-2*k)) & 3];
        if ((sw >> signs[operand]) & 1) v[k] = -v[k];
    }
}
static int pica_vs_condition(const PicaVsState *s, uint32_t w) {
    int x = s->cc[0] == ((w >> 25) & 1), y = s->cc[1] == ((w >> 24) & 1);
    switch ((w >> 22) & 3) {case 0:return x||y;case 1:return x&&y;case 2:return x;default:return y;}
}
static int pica_vs_compare(float a, float b, unsigned op) {
    switch(op) {case 0:return a==b;case 1:return a!=b;case 2:return a<b;
    case 3:return a<=b;case 4:return a>b;case 5:return a>=b;default:return 1;}
}
/* t_0b95f1a7 black-3D forensics: why pica_load_vertex rejects a vertex and how
   many triangles each draw queued.  Reason codes (pica_vs_fail_reason):
   0=ok 1=count skip 2=max_input_attrib>=12 3=attribute not loaded
   4=guest-memory range 5=VS execution 6=w<=0 7=nonfinite/huge position.
   VS-exec detail codes (pica_vs_fail_detail>>24): 1=pc out of program
   2=swizzle desc out of table 3=call-stack overflow 4=branch-stack overflow
   5=unknown opcode 6=step limit. */
/* t_968a1fa9: PER-THREAD verdict.  The parallel vertex pool (DQ8_VSPAR) calls
   pica_load_vertex from up to PVS_PAR_MAX+1 threads at once.  With one
   process-global pair, worker A's failure reason could be zeroed by worker B's
   *entry* (every call starts with fail_reason=0) or overwritten by B's own
   failure before A's caller read it, so a failed vertex was recorded as valid
   and the uninitialised output slot was queued as geometry (the stretched
   polygon).  The serial path is unchanged: it writes and reads on one thread. */
static __thread unsigned pica_vs_fail_reason, pica_vs_fail_detail;
static unsigned long long pica_vs_fail_totals[8];
static unsigned pica_draw_log_count, pica_draw_log_tris;
/* Per-draw clip/w forensics (756d24b): also referenced from the standalone
   test TU (tools/test_pica_gl_diff.c includes this file without the runtime
   amalgamation, so the tentative definitions must live here too). */
static unsigned pica_draw_log_clip;
static float pica_draw_log_wmin, pica_draw_log_wmax;
static unsigned pica_draw_log_nfdrop; /* rsn=7 nonfinite drops this draw (t_074669f7) */
static unsigned pica_draw_log_arej;   /* alpha-test rejections this draw (t_074669f7) */
static unsigned pica_draw_log_drej;   /* depth-test rejections this draw (t_074669f7) */
static unsigned pica_draw_log_frw;    /* fragments written this draw (t_074669f7) */
/* t_0bf6793d (DIAGNOSTIC): triangle winding tally for the current draw, so a
   face-culling defect can be seen directly: fcw = signed area < 0 (the order
   the rasterizer would have to reverse), fccw = signed area > 0. */
static unsigned pica_draw_log_fcw, pica_draw_log_fccw;
/* t_150574cd: face-culling accounting (triangles the GPUREG 0x40 filter kept
   and dropped).  Defined here, in the same amalgamation TU as the runtime, so
   the DQ8_TRACE_TEXDRAW / DQ8_TRACE_CULL dumps can print them. */
static unsigned long long pica_cull_kept_tris, pica_cull_dropped_tris;
static unsigned pica_thist[6]; /* triangle centroid buckets over sx (t_074669f7) */
/* DQ8_TRACE_CLIPSTAT (t_da76454a): per-draw clip classification.  Where does a
   draw's geometry actually go?  Counts primitives that are fully inside the
   w plane (queued unclipped), fully behind (dropped) and straddling (S-H
   clipped), and records the UNCLAMPED screen-space bbox of the two queued
   groups separately, so "the ground vanished" can be told apart from "the
   ground was never in front of the near plane". */
static unsigned clst_fastin, clst_dropall, clst_straddle, clst_kept0, clst_keptn;
static unsigned clst_fan, clst_fan_rt, clst_fan_off;
static unsigned clst_tri_nodesign, clst_tri_onscreen, clst_tri_dump;
static int clst_dumping;
static unsigned clst_nv_front, clst_nv_ground, clst_nv_infrustum;
static float clst_ndx0, clst_ndx1, clst_ndy0, clst_ndy1;
static int clst_on = -1;                 /* DQ8_TRACE_CLIPSTAT gate */
static int pica_clst(void);
static unsigned long long pica_cov_px;   /* pixels passing the edge tests (t_da76454a) */
static float clst_wmin_all, clst_wmax_all, clst_wmin_kept;
static float clst_fx0, clst_fx1, clst_fy0, clst_fy1;   /* fast-path fans */
static float clst_cx0, clst_cx1, clst_cy0, clst_cy1;   /* clipped fans */
static void pica_clst_reset(void) {
    clst_fastin = clst_dropall = clst_straddle = clst_kept0 = clst_keptn = 0;
    clst_fan = clst_fan_rt = clst_fan_off = 0;
    clst_tri_nodesign = clst_tri_onscreen = clst_tri_dump = 0;
    clst_dumping = 0;
    clst_nv_front = clst_nv_ground = clst_nv_infrustum = 0;
    clst_ndx0 = clst_ndy0 = 1e30f; clst_ndx1 = clst_ndy1 = -1e30f;
    clst_wmin_all = 1e30f; clst_wmax_all = -1e30f; clst_wmin_kept = 1e30f;
    clst_fx0 = clst_fy0 = clst_cx0 = clst_cy0 = 1e30f;
    clst_fx1 = clst_fy1 = clst_cx1 = clst_cy1 = -1e30f;
}
/* t_fdc40e6f: one operand load from the prepared plan.  `addr` is either an
   absolute pointer (uniform / constant-ones operand) or a byte offset from the
   vertex state, and `smask` is all-ones for the latter, so the base select is
   an AND+ADD instead of a branch. */
static inline __m128 pvs_plan_ld(uintptr_t addr, const PvsSwzVec *w) {
    __m128 v = _mm_loadu_ps((const float *)addr);
    return _mm_xor_ps(_mm_castsi128_ps(_mm_shuffle_epi8(_mm_castps_si128(v), w->perm)),
                      w->sign);
}
static int pica_execute_vs(PicaVsState *s) {
    struct {unsigned end, ret;} calls[16], branches[16];
    unsigned nc=0, nb=0, pc=pica_regs[0x2ba] & 65535, boundary=UINT32_MAX;
    /* t_fdc40e6f: these four gates used to be re-read FROM GLOBALS for every
       microcode instruction.  None of them can change during one vertex
       (pvs_plan_cur / pvs_dec_ready are published by the main thread before
       the pool job, pvs_trace is set per vertex in pica_load_vertex and the
       two instrumentation switches are resolved once per process), so read
       them once here.  `plan` is NULL while tracing, which keeps the trace
       path on the interpreter that emits it. */
    const int ghist = pvs_ghist_on > 0;
    const int ptrap = pf_trap_on > 0;
    const PvsPlan *plan = pvs_trace ? NULL : pvs_plan_now();
    for (unsigned step=0;step<16384;step++) {
        /* Scope ends compare the preceding instruction's sequential successor,
           so a branch on the last instruction cannot escape a call/if scope. */
        unsigned call_boundary=boundary;
        for (unsigned pops=0;pops<4 && nc && call_boundary==calls[nc-1].end;pops++) {
            --nc;
            if(pops<3)pc=call_boundary=calls[nc].ret;
        }
        if(nb && boundary==branches[nb-1].end)pc=branches[--nb].ret;
        if (pc>=vs_program_size) {pica_vs_fail_detail=0x01000000u|(pc&0xFFFFFFu);return 0;}
        boundary=pc+1;
        if (ghist) ++pvs_ghist_all;
        if (ptrap) pf_vs_step();
        if (plan) {
            /* t_fdc40e6f: prepared-plan dispatch.  Every operand address,
               swizzle vector, destination offset, write mask and flow target
               was resolved once per program (pvs_plan_sync); the per-vertex
               loop does no bitfield decode and no register lookup. */
            const PvsPlan *p = &plan[pc];
            unsigned o = p->op;
            if (o) {
                if (o >= PVO_END) {
                    uint32_t w = vs_program[pc];
                    unsigned nx = pc + 1;
                    if (o == PVO_END) { pvs_flow(pc, w, nx); return 1; }
                    if (o == PVO_JMP) { pvs_flow(pc, w, nx); pc = nx; continue; }
                    if (o <= PVO_JMPZ) {          /* 0x24..0x28 */
                        unsigned target = p->tgt, nn = p->n;
                        int yes;
                        if (o == PVO_CALL) yes = 1;
                        else if (o == PVO_CALLZ || o == PVO_JMPZ)
                            yes = pica_vs_condition(s, w);
                        else
                            yes = (int)((pica_regs[0x2b0] >> p->idx) & 1);
                        if (o <= PVO_CALLNZ) {    /* call type: 0x24..0x26 */
                            if (yes) {
                                if (nc == 16) { pica_vs_fail_detail = 0x03000000u; return 0; }
                                calls[nc].end = target + nn;
                                calls[nc++].ret = nx;
                                nx = target;
                            }
                        } else if (yes) {         /* branch type: 0x27/0x28 */
                            if (nb == 16) { pica_vs_fail_detail = 0x04000000u; return 0; }
                            branches[nb].end = target;
                            branches[nb++].ret = target + nn;
                        } else nx = target;
                        pvs_flow(pc, w, nx);
                        pc = nx;
                        continue;
                    }
                    {   /* 0x2C / 0x2D */
                        int yes = (o == PVO_IFU)
                            ? pica_vs_condition(s, w)
                            : (((pica_regs[0x2b0] >> p->idx) & 1) == !(w & 1));
                        pvs_flow(pc, w, yes ? p->tgt : nx);
                        pc = yes ? p->tgt : nx;
                        continue;
                    }
                }
                {
                    const uintptr_t SS = (uintptr_t)s;
                    uintptr_t ma = (uintptr_t)0 - (uintptr_t)(p->flags & 1u);
                    uintptr_t mb = (uintptr_t)0 - (uintptr_t)((p->flags >> 1) & 1u);
                    uintptr_t mc = (uintptr_t)0 - (uintptr_t)((p->flags >> 2) & 1u);
                    const PvsSwzVec *wa = &pvs_swzv[p->sw_a];
                    const PvsSwzVec *wb = &pvs_swzv[p->sw_b];
                    __m128 va, vb, vr;
                    if (o == PVO_MOVA) {
                        /* 0x12: no destination write, only address registers. */
                        float al[4];
                        unsigned k2;
                        va = p->rel_a ? pvs_vec_apply(s, p->na, p->rel_a, wa)
                                      : pvs_plan_ld(p->a + (SS & ma), wa);
                        _mm_storeu_ps(al, va);
                        for (k2 = 0; k2 < 2; k2++)
                            if ((p->msk & (8 >> k2)) && isfinite(al[k2]) &&
                                al[k2] >= -2147483648.0f && al[k2] < 2147483648.0f)
                                s->address[k2] = (int)al[k2];
                        pc = pc + 1u;
                        continue;
                    }
                    if (o == PVO_CMP) {
                        float al[4], bl[4];
                        va = p->rel_a ? pvs_vec_apply(s, p->na, p->rel_a, wa)
                                      : pvs_plan_ld(p->a + (SS & ma), wa);
                        vb = p->rel_b ? pvs_vec_apply(s, p->nb, p->rel_b, wb)
                                      : pvs_plan_ld(p->b + (SS & mb), wb);
                        _mm_storeu_ps(al, va);
                        _mm_storeu_ps(bl, vb);
                        s->cc[0] = pica_vs_compare(al[0], bl[0], p->cmp0);
                        s->cc[1] = pica_vs_compare(al[1], bl[1], p->cmp1);
                        pc = pc + 1u;
                        continue;
                    }
                    va = p->rel_a ? pvs_vec_apply(s, p->na, p->rel_a, wa)
                                  : pvs_plan_ld(p->a + (SS & ma), wa);
                    vb = p->rel_b ? pvs_vec_apply(s, p->nb, p->rel_b, wb)
                                  : pvs_plan_ld(p->b + (SS & mb), wb);
                    switch (o) {
                    case PVO_MOV:
                        vr = va;
                        break;
                    case PVO_ADD:
                        vr = _mm_add_ps(va, vb);
                        break;
                    case PVO_MUL:
                        vr = pvs_vec_mul(va, vb);
                        break;
                    case PVO_FLR:
                        vr = _mm_floor_ps(va);
                        break;
                    case PVO_RCP: {
                        /* same scalar expression as the interpreter, so the
                           rounding is identical */
                        float al[4];
                        _mm_storeu_ps(al, va);
                        vr = _mm_set1_ps(1.0f / al[0]);
                        break;
                    }
                    case PVO_RSQ: {
                        float al[4];
                        _mm_storeu_ps(al, va);
                        vr = _mm_set1_ps(1.0f / sqrtf(al[0]));
                        break;
                    }
                    case PVO_MIN:
                    case PVO_MAX: {
                        float al[4], bl[4];
                        unsigned k2;
                        _mm_storeu_ps(al, va);
                        _mm_storeu_ps(bl, vb);
                        for (k2 = 0; k2 < 4; k2++)
                            al[k2] = (o == PVO_MIN) ? fminf(al[k2], bl[k2])
                                                    : fmaxf(al[k2], bl[k2]);
                        vr = _mm_loadu_ps(al);
                        break;
                    }
                    case PVO_SLT:
                    case PVO_SGE: {
                        float al[4], bl[4];
                        unsigned k2;
                        _mm_storeu_ps(al, va);
                        _mm_storeu_ps(bl, vb);
                        for (k2 = 0; k2 < 4; k2++)
                            al[k2] = (o == PVO_SLT) ? (al[k2] < bl[k2])
                                                    : (al[k2] >= bl[k2]);
                        vr = _mm_loadu_ps(al);
                        break;
                    }
                    case PVO_MAD: {
                        __m128 vc = p->rel_c ? pvs_vec_apply(s, p->nc, p->rel_c,
                                                             &pvs_swzv[p->sw_c])
                                             : pvs_plan_ld(p->c + (SS & mc),
                                                           &pvs_swzv[p->sw_c]);
                        vr = _mm_add_ps(pvs_vec_mul(va, vb), vc);
                        break;
                    }
                    default: {          /* PVO_DP: 0 + p0 + p1 + p2 [+ p3] [+ b3] */
                        float p4[4], dot = 0.0f;
                        _mm_storeu_ps(p4, pvs_vec_mul(va, vb));
                        dot += p4[0]; dot += p4[1]; dot += p4[2];
                        if (p->dpn == 4) dot += p4[3];
                        if (p->dph) { float bl[4]; _mm_storeu_ps(bl, vb); dot += bl[3]; }
                        vr = _mm_set1_ps(dot);
                        break;
                    }
                    }
                    {
                        float *dp = (float *)((char *)s + p->dst);
                        if (p->msk == 15u) _mm_storeu_ps(dp, vr);
                        else {
                            __m128 vm = pvs_wmask[p->msk];
                            __m128 old = _mm_loadu_ps(dp);
                            _mm_storeu_ps(dp, _mm_or_ps(_mm_and_ps(vm, vr),
                                                        _mm_andnot_ps(vm, old)));
                        }
                    }
                    pc = pc + 1u;
                    continue;
                }
            }
        } else if (pvs_dec_ready && !pvs_trace) {
            /* Pre-decoded dispatch (t_469b3d92): every field this instruction
               needs was classified once per program upload. */
            const PvsDec *d = &pvs_dec_tab[pc];
            unsigned kind = d->kind;
            if (kind) {
                float *dp = d->dst < 16 ? s->output[d->dst] : s->temp[d->dst - 16];
                const PvsSwzVec *wa = &pvs_swzv[d->sw_a];
                const PvsSwzVec *wb = &pvs_swzv[d->sw_b];
                __m128 vr;
                if (kind >= PVK_END) {
                    /* t_6f9e3fed: pre-decoded flow control (0x21/0x22,
                       0x24..0x28, 0x2C/0x2D).  Target/length/predicate index
                       were decoded once per program upload; the condition
                       evaluation and the calls[]/branches[] scope bookkeeping
                       are the interpreter's own, unchanged. */
                    uint32_t w = vs_program[pc];
                    unsigned nx = pc + 1;
                    if (kind == PVK_END) { pvs_flow(pc, w, nx); return 1; }
                    if (kind == PVK_JMP) { pvs_flow(pc, w, nx); pc = nx; continue; }
                    if (kind <= PVK_JMPZ) {          /* 0x24..0x28 */
                        unsigned target = d->tgt, nn = d->n;
                        int yes;
                        if (kind == PVK_CALL) yes = 1;
                        else if (kind == PVK_CALLZ || kind == PVK_JMPZ)
                            yes = pica_vs_condition(s, w);
                        else
                            yes = (pica_regs[0x2b0] >> d->idx) & 1;
                        if (kind <= PVK_CALLNZ) {    /* call type: 0x24..0x26 */
                            if (yes) {
                                if (nc == 16) { pica_vs_fail_detail = 0x03000000u; return 0; }
                                calls[nc].end = target + nn;
                                calls[nc++].ret = nx;
                                nx = target;
                            }
                        } else if (yes) {            /* branch type: 0x27/0x28 */
                            if (nb == 16) { pica_vs_fail_detail = 0x04000000u; return 0; }
                            branches[nb].end = target;
                            branches[nb++].ret = target + nn;
                        } else nx = target;
                        pvs_flow(pc, w, nx);
                        pc = nx;
                        continue;
                    }
                    {   /* 0x2C / 0x2D */
                        int yes = (kind == PVK_IFU)
                            ? pica_vs_condition(s, w)
                            : (((pica_regs[0x2b0] >> d->idx) & 1) == !(w & 1));
                        pvs_flow(pc, w, yes ? d->tgt : nx);
                        pc = yes ? d->tgt : nx;
                        continue;
                    }
                }
                if (kind == PVK_MOVA) {
                    /* 0x12: no destination write, only address registers. */
                    float al[4];
                    unsigned k2;
                    _mm_storeu_ps(al, pvs_vec_apply(s, d->o_a, d->r_a, wa));
                    for (k2 = 0; k2 < 2; k2++)
                        if ((d->msk & (8 >> k2)) && isfinite(al[k2]) &&
                            al[k2] >= -2147483648.0f && al[k2] < 2147483648.0f)
                            s->address[k2] = (int)al[k2];
                    pc = pc + 1u;
                    continue;
                }
                if (kind == PVK_CMP) {
                    float al[4], bl[4];
                    _mm_storeu_ps(al, pvs_vec_apply(s, d->o_a, d->r_a, wa));
                    _mm_storeu_ps(bl, pvs_vec_apply(s, d->o_b, d->r_b, wb));
                    s->cc[0] = pica_vs_compare(al[0], bl[0], d->cmp0);
                    s->cc[1] = pica_vs_compare(al[1], bl[1], d->cmp1);
                    pc = pc + 1u;
                    continue;
                }
                switch (kind) {
                case PVK_MOV:
                    vr = pvs_vec_apply(s, d->o_a, d->r_a, wa);
                    break;
                case PVK_ADD:
                    vr = _mm_add_ps(pvs_vec_apply(s, d->o_a, d->r_a, wa),
                                    pvs_vec_apply(s, d->o_b, d->r_b, wb));
                    break;
                case PVK_MUL:
                    vr = pvs_vec_mul(pvs_vec_apply(s, d->o_a, d->r_a, wa),
                                     pvs_vec_apply(s, d->o_b, d->r_b, wb));
                    break;
                case PVK_FLR:
                    vr = _mm_floor_ps(pvs_vec_apply(s, d->o_a, d->r_a, wa));
                    break;
                case PVK_RCP: {
                    /* 0x0E: 1.0f/a[0] replicated — same scalar expression as
                       the interpreter, so the rounding is identical. */
                    float al[4];
                    _mm_storeu_ps(al, pvs_vec_apply(s, d->o_a, d->r_a, wa));
                    vr = _mm_set1_ps(1.0f / al[0]);
                    break;
                }
                case PVK_RSQ: {
                    /* 0x0F: 1.0f/sqrtf(a[0]) replicated. */
                    float al[4];
                    _mm_storeu_ps(al, pvs_vec_apply(s, d->o_a, d->r_a, wa));
                    vr = _mm_set1_ps(1.0f / sqrtf(al[0]));
                    break;
                }
                case PVK_MIN:
                case PVK_MAX: {
                    float al[4], bl[4];
                    unsigned k2;
                    _mm_storeu_ps(al, pvs_vec_apply(s, d->o_a, d->r_a, wa));
                    _mm_storeu_ps(bl, pvs_vec_apply(s, d->o_b, d->r_b, wb));
                    for (k2 = 0; k2 < 4; k2++)
                        al[k2] = (kind == PVK_MIN) ? fminf(al[k2], bl[k2])
                                                   : fmaxf(al[k2], bl[k2]);
                    vr = _mm_loadu_ps(al);
                    break;
                }
                case PVK_SLT:
                case PVK_SGE: {
                    /* 0x09/0x1A (>=) and 0x0A/0x1B (<): 1.0f / 0.0f lanes. */
                    float al[4], bl[4];
                    unsigned k2;
                    _mm_storeu_ps(al, pvs_vec_apply(s, d->o_a, d->r_a, wa));
                    _mm_storeu_ps(bl, pvs_vec_apply(s, d->o_b, d->r_b, wb));
                    for (k2 = 0; k2 < 4; k2++)
                        al[k2] = (kind == PVK_SLT) ? (al[k2] < bl[k2])
                                                   : (al[k2] >= bl[k2]);
                    vr = _mm_loadu_ps(al);
                    break;
                }
                case PVK_MAD: {
                    __m128 m = pvs_vec_mul(pvs_vec_apply(s, d->o_a, d->r_a, wa),
                                           pvs_vec_apply(s, d->o_b, d->r_b, wb));
                    vr = _mm_add_ps(m, pvs_vec_apply(s, d->o_c, d->r_c,
                                                     &pvs_swzv[d->sw_c]));
                    break;
                }
                default: {          /* PVK_DP: 0 + p0 + p1 + p2 [+ p3] [+ b3] */
                    __m128 va = pvs_vec_apply(s, d->o_a, d->r_a, wa);
                    __m128 vb = pvs_vec_apply(s, d->o_b, d->r_b, wb);
                    float p[4], dot = 0.0f;
                    _mm_storeu_ps(p, pvs_vec_mul(va, vb));
                    dot += p[0]; dot += p[1]; dot += p[2];
                    if (d->dpn == 4) dot += p[3];
                    if (d->dph) { float bl[4]; _mm_storeu_ps(bl, vb); dot += bl[3]; }
                    vr = _mm_set1_ps(dot);
                    break;
                }
                }
                if (d->msk == 15u) _mm_storeu_ps(dp, vr);
                else {
                    __m128 vm = pvs_wmask[d->msk];
                    __m128 old = _mm_loadu_ps(dp);
                    _mm_storeu_ps(dp, _mm_or_ps(_mm_and_ps(vm, vr),
                                                _mm_andnot_ps(vm, old)));
                }
                pc = pc + 1u;
                continue;
            }
        }
        uint32_t w=vs_program[pc]; unsigned op=w>>26, next=pc+1;
        if (pvs_ghist_on > 0) { ++pvs_ghist[op & 63u]; ++pvs_ghist_gen; }
        if (op==0x22) {pvs_flow(pc,w,next);return 1;}
        if (op==0x21) {pvs_flow(pc,w,next);pc=next;continue;}
        if (op>=0x24 && op<=0x28) {
            unsigned target=(w>>10)&4095, n=w&255;
            int yes=(op==0x24) || ((op==0x25 || op==0x28) ? pica_vs_condition(s,w) :
                         ((pica_regs[0x2b0] >> ((w>>22)&15)) & 1));
            if (op<=0x26) {
                if (yes) {if(nc==16){pica_vs_fail_detail=0x03000000u;return 0;}calls[nc].end=target+n;calls[nc++].ret=next;next=target;}
            } else if (yes) {
                if(nb==16){pica_vs_fail_detail=0x04000000u;return 0;}branches[nb].end=target;branches[nb++].ret=target+n;
            } else next=target;
            pvs_flow(pc,w,next);
            pc=next;continue;
        }
        if (op==0x2c || op==0x2d) {
            int yes=op==0x2c?pica_vs_condition(s,w):
                (((pica_regs[0x2b0]>>((w>>22)&15))&1)==!(w&1));
            pvs_flow(pc,w,yes?((w>>10)&4095):next);
            pc=yes?((w>>10)&4095):next;continue;
        }
        int mad=op>=0x30, inv=(op>=0x18 && op<=0x1b)||(op>=0x30 && op<0x38);
        unsigned desc=w&(mad?31:127), dst, rel=(w>>(mad?22:19))&3;
        if(desc>=vs_swizzle_size){pica_vs_fail_detail=0x02000000u|(desc&0xFFFFFFu);return 0;}
        uint32_t sw=vs_swizzle[desc]; float a[4],b[4],c[4],v[4];
#if defined(PVS_SSE)
        /* 4-wide fast path for the ops that dominate the executed stream
           (MOV 0x13, ADD 0x00, MUL 0x08, FLOOR 0x0B, MAD 0x30..0x3F).
           Everything with a side effect on cc/address (DP, CMP, MOVA),
           the transcendental ops and the trace path stay on the scalar
           interpreter below, which remains the reference implementation. */
        if (!pvs_trace) {
            __m128 vr;
            int have = 0;
            unsigned o_a = 0, o_b = 0, o_c = 0, r_a = 0, r_b = 0, r_c = 0;
            if (mad) {
                o_a = (w >> 17) & 31;                        r_a = 0;
                o_b = inv ? ((w >> 12) & 31) : ((w >> 10) & 127);
                r_b = inv ? 0u : rel;
                o_c = inv ? ((w >> 5) & 127) : ((w >> 5) & 31);
                r_c = inv ? rel : 0u;
            } else {
                o_a = inv ? ((w >> 14) & 31) : ((w >> 12) & 127);
                r_a = inv ? 0u : rel;
                o_b = inv ? ((w >> 7) & 127) : ((w >> 7) & 31);
                r_b = inv ? rel : 0u;
            }
            if (mad) {
                vr = _mm_add_ps(pvs_vec_mul(pvs_vec_opnd(s, o_a, r_a, desc, 0),
                                            pvs_vec_opnd(s, o_b, r_b, desc, 1)),
                                pvs_vec_opnd(s, o_c, r_c, desc, 2));
                dst = (w >> 24) & 31;
                have = 1;
            } else if (op == 0x13 || op == 0x00 || op == 0x08 || op == 0x0B) {
                __m128 va = pvs_vec_opnd(s, o_a, r_a, desc, 0);
                __m128 vb = pvs_vec_opnd(s, o_b, r_b, desc, 1);
                if (op == 0x13) vr = va;
                else if (op == 0x00) vr = _mm_add_ps(va, vb);
                else if (op == 0x08) vr = pvs_vec_mul(va, vb);
                else vr = _mm_floor_ps(va);
                dst = (w >> 21) & 31;
                have = 1;
            }
            if (have) {
                float *dp = dst < 16 ? s->output[dst] : s->temp[dst - 16];
                unsigned msk = sw & 15u;
                if (msk == 15u) _mm_storeu_ps(dp, vr);
                else {
                    __m128 vm = pvs_wmask[msk];
                    __m128 old = _mm_loadu_ps(dp);
                    _mm_storeu_ps(dp, _mm_or_ps(_mm_and_ps(vm, vr),
                                                _mm_andnot_ps(vm, old)));
                }
                pc = next;
                continue;
            }
        }
#endif
        if(mad) {
            pica_vs_source(s,(w>>17)&31,0,sw,0,a);
            pica_vs_source(s,inv?((w>>12)&31):((w>>10)&127),inv?0:rel,sw,1,b);
            pica_vs_source(s,inv?((w>>5)&127):((w>>5)&31),inv?rel:0,sw,2,c);
            for(unsigned k=0;k<4;k++)v[k]=pica_multiply(a[k],b[k])+c[k];
            dst=(w>>24)&31;
        } else {
            pica_vs_source(s,inv?((w>>14)&31):((w>>12)&127),inv?0:rel,sw,0,a);
            pica_vs_source(s,inv?((w>>7)&127):((w>>7)&31),inv?rel:0,sw,1,b);
            dst=(w>>21)&31;
            if(op==0x2e || op==0x2f) {
                s->cc[0]=pica_vs_compare(a[0],b[0],(w>>24)&7);
                s->cc[1]=pica_vs_compare(a[1],b[1],(w>>21)&7);
                if(pvs_f&&pvs_trace)fprintf(pvs_f,"VSCC pc=%u w=%08X cc=%d%d\n",pc,w,s->cc[0],s->cc[1]);
                pc=next;continue;
            }
            if(op==0x12) {
                for(unsigned k=0;k<2;k++)if((sw&(8>>k)) && isfinite(a[k]) && a[k]>=-2147483648.0f && a[k]<2147483648.0f)
                    s->address[k]=(int)a[k];
                pvs_ins(pc,w,a,NULL,NULL,NULL,0,sw&15);
                pc=next;continue;
            }
            if(op==1 || op==2 || op==3 || op==0x18) {
                float dot=0;
                for(unsigned k=0;k<(op==2?4u:3u);k++)dot+=pica_multiply(a[k],b[k]);
                if(op==3 || op==0x18)dot+=b[3];
                for(unsigned k=0;k<4;k++)v[k]=dot;
            } else for(unsigned k=0;k<4;k++)switch(op) {
                case 0:v[k]=a[k]+b[k];break;
                case 5:v[k]=exp2f(a[0]);break;case 6:v[k]=log2f(a[0]);break;
                case 8:v[k]=pica_multiply(a[k],b[k]);break;
                case 9:case 0x1a:v[k]=a[k]>=b[k];break;
                case 10:case 0x1b:v[k]=a[k]<b[k];break;
                case 11:v[k]=floorf(a[k]);break;case 12:v[k]=fmaxf(a[k],b[k]);break;
                case 13:v[k]=fminf(a[k],b[k]);break;case 14:v[k]=1.0f/a[0];break;
                case 15:v[k]=1.0f/sqrtf(a[0]);break;case 0x13:v[k]=a[k];break;
                default:pica_vs_fail_detail=0x05000000u|(op&0xFFFFFFu);return 0;
            }
        }
        float *d=dst<16?s->output[dst]:s->temp[dst-16];
        for(unsigned k=0;k<4;k++)if(sw&(8>>k))d[k]=v[k];
        pvs_ins(pc,w,a,b,mad?c:NULL,v,dst,sw&15);
        pc=next;
    }
    pica_vs_fail_detail=0x06000000u; /* step limit: no END within 16384 steps */
    return 0;
}

static float pica_sat(float x) {return fminf(1.0f,fmaxf(0.0f,x));}
static void pica_rgba(uint32_t packed,float c[4]) {
    for(unsigned k=0;k<4;k++)c[k]=((packed>>(8*k))&255)/255.0f;
}
static void pica_tev_source(unsigned source,const float primary[4],const float texture[4],
                            const float texture1[4],const float previous[4],
                            const float buffer[4],const float constant[4],float v[4]) {
    /* TEV source codes (Azahar regs_texturing.h TevStageConfig::Source):
       0  primary color (the interpolated vertex color here; this runtime has
          no fragment lighting, so 1 primary-fragment and 2 secondary-fragment
          color stay 0 as they always have),
       3/4/5 texture0/1/2, 13 previous buffer, 14 constant, 15 previous stage.
       t_e216fc37: source 4 (texture1) used to fall through to 0.0 — the ground
       "shadow/decal" draws cross-fade texture0 and texture1 by the vertex alpha
       (stage 0: sources=03300043 mods=00000200 ops=00010004), so a bound unit-1
       texture was multiplied in as black and the whole quad rendered black. */
    const float *p = source==0 ? primary : source==3 ? texture : source==4 ? texture1
                   : source==13 ? buffer : source==14 ? constant : source==15 ? previous : NULL;
    for(unsigned k=0;k<4;k++)v[k]=p?p[k]:0.0f;
}
static float pica_tev_op(unsigned op,float a,float b,float c) {
    switch(op) {case 0:return a;case 1:return a*b;case 2:return a+b;case 3:return a+b-0.5f;
    case 4:return a*c+b*(1-c);case 5:return a-b;case 8:return a*b+c;case 9:return fminf(a+b,1)*c;default:return 0;}
}
static void pica_tev(const float primary[4],const float texture[4],const float texture1[4],float out[4]) {
    static const unsigned bases[6]={0xc0,0xc8,0xd0,0xd8,0xf0,0xf8};
    float previous[4],buffer[4],pending[4];
    memcpy(previous,primary,sizeof previous);pica_rgba(pica_regs[0xfd],buffer);memcpy(pending,buffer,sizeof buffer);
    for(unsigned stage=0;stage<6;stage++) {
        unsigned base=bases[stage];uint32_t sources=pica_regs[base],mods=pica_regs[base+1],ops=pica_regs[base+2];
        float constant[4],col[3][4],alpha[3];pica_rgba(pica_regs[base+3],constant);
        for(unsigned i=0;i<3;i++) {
            float v[4];pica_tev_source((sources>>(4*i))&15,primary,texture,texture1,previous,buffer,constant,v);
            unsigned m=(mods>>(4*i))&15;
            for(unsigned k=0;k<3;k++) {
                unsigned channel=m<2?k:m<4?3:m<6?0:m<10?1:2;
                col[i][k]=(m&1)?1-v[channel]:v[channel];
            }
            pica_tev_source((sources>>(16+4*i))&15,primary,texture,texture1,previous,buffer,constant,v);
            m=(mods>>(12+4*i))&7;unsigned channel=m<2?3:(m/2-1);
            alpha[i]=(m&1)?1-v[channel]:v[channel];
        }
        unsigned cop=ops&15,aop=(ops>>16)&15,cs=pica_regs[base+4]&3,as=(pica_regs[base+4]>>16)&3;
        float next[4];
        if(cop==6 || cop==7) {
            float dot=0;for(unsigned k=0;k<3;k++)dot+=(col[0][k]-0.5f)*(col[1][k]-0.5f)*4;
            for(unsigned k=0;k<3;k++)next[k]=pica_sat(dot);
        } else for(unsigned k=0;k<3;k++)next[k]=pica_sat(pica_tev_op(cop,col[0][k],col[1][k],col[2][k]));
        next[3]=cop==7?next[0]:pica_sat(pica_tev_op(aop,alpha[0],alpha[1],alpha[2]));
        for(unsigned k=0;k<4;k++)next[k]=pica_sat(next[k]*(1u<<((k==3?as:cs)%3)));
        memcpy(buffer,pending,sizeof buffer);
        if(stage<4) {
            if(pica_regs[0xe0]&(1u<<(8+stage)))memcpy(pending,next,3*sizeof(float));
            if(pica_regs[0xe0]&(1u<<(12+stage)))pending[3]=next[3];
        }
        memcpy(previous,next,sizeof previous);
    }
    memcpy(out,previous,sizeof previous);
}
static float pica_blend_factor(unsigned f,unsigned k,const float s[4],const float d[4],const float c[4]) {
    switch(f) {case 0:return 0;case 1:return 1;case 2:return s[k];case 3:return 1-s[k];
    case 4:return d[k];case 5:return 1-d[k];case 6:return s[3];case 7:return 1-s[3];
    case 8:return d[3];case 9:return 1-d[3];case 10:return c[k];case 11:return 1-c[k];
    case 12:return c[3];case 13:return 1-c[3];case 14:return k==3?1:fminf(s[3],1-d[3]);default:return 0;}
}
/* ---- DQ8_TRACE_PX pixel probe (diagonal-line artifact diagnosis) ---------
   Env DQ8_TRACE_PX=line places probes on the observed artifact line
   (raster y = x-40 +/-6 at six x positions) plus two off-line controls;
   any other value is a space-separated list of "x,y" raster pixel pairs.
   Per draw we report: PXW (fragment written), PXA (alpha-test rejected),
   PXF (depth-test rejected), PXB (bbox covered / written / nearest own
   triangle edge distance me in px — a seam-owning draw has me ~= 0 on the
   line while not writing it). */
static int pica_px_n;
static int pica_px_x[96], pica_px_y[96];
static int pica_px_verbose[96];
static uint8_t pica_px_wr[96];            /* probes written this draw */
static float pica_px_me[96];              /* min perpendicular distance (px) from probe to this draw's triangle edges */
static uint8_t pica_px_map[512 * 400 / 8];
static void pica_px_init(void) {
    static int done;
    if (done) return;
    done = 1;
    const char *e = getenv("DQ8_TRACE_PX");
    if (!e || !e[0]) return;
    if (!strcmp(e, "line")) {
        static const int xs[6] = {100, 160, 220, 280, 340, 400};
        for (int k = 0; k < 6; k++)
            for (int d = -6; d <= 6; d++) {
                int x = xs[k], y = xs[k] - 40 + d;
                if (x < 0 || x > 511 || y < 0 || y > 399) continue;
                pica_px_x[pica_px_n] = x; pica_px_y[pica_px_n] = y;
                pica_px_verbose[pica_px_n] = (d == 0);
                pica_px_n++;
            }
        pica_px_x[pica_px_n] = 100; pica_px_y[pica_px_n] = 20;   /* off-line controls */
        pica_px_verbose[pica_px_n] = 1; pica_px_n++;
        pica_px_x[pica_px_n] = 280; pica_px_y[pica_px_n] = 100;
        pica_px_verbose[pica_px_n] = 1; pica_px_n++;
    } else {
        while (*e && pica_px_n < 96) {
            char *end;
            long x = strtol(e, &end, 10);
            if (end == e || *end != ',') break;
            long y = strtol(end + 1, &end, 10);
            pica_px_x[pica_px_n] = (int)x; pica_px_y[pica_px_n] = (int)y;
            pica_px_verbose[pica_px_n] = 1;
            pica_px_n++;
            e = end;
            while (*e == ' ' || *e == ';' || *e == ',') e++;
        }
    }
    for (int i = 0; i < pica_px_n; i++) {
        int x = pica_px_x[i], y = pica_px_y[i];
        if (x < 0 || x >= 512 || y < 0 || y >= 400) continue;
        unsigned b = (unsigned)(y * 512 + x);
        pica_px_map[b >> 3] |= (uint8_t)(1u << (b & 7u));
    }
}
static int pica_px_find(int x, int y) {
    pica_px_init();
    if (x < 0 || x >= 512 || y < 0 || y >= 400) return -1;
    unsigned b = (unsigned)(y * 512 + x);
    if (!((pica_px_map[b >> 3] >> (b & 7)) & 1u)) return -1;
    for (int i = 0; i < pica_px_n; i++)
        if (pica_px_x[i] == x && pica_px_y[i] == y) return i;
    return -1;
}
static void pica_px_log_write(int x, int y, const uint8_t c[4]) {
    int i = pica_px_find(x, y);
    if (i < 0) return;
    pica_px_wr[i] = 1;
    if (!pica_px_verbose[i] && (int)c[0] + (int)c[1] + (int)c[2] > 150) return;
    fprintf(stderr, "PXW t=%.2f draw=%llu px=%d,%d rgb=%u,%u,%u a=%u\n",
            dq8_layer_t(), (unsigned long long)pica_draw_count, x, y,
            c[0], c[1], c[2], c[3]);
}
static void pica_px_report(void) {
    pica_px_init();
    if (!pica_px_n) return;
    for (int i = 0; i < pica_px_n; i++) {
        int cov = pica_px_x[i] >= (int)pica_db_x0 && pica_px_x[i] <= (int)pica_db_x1 &&
                  pica_px_y[i] >= (int)pica_db_y0 && pica_px_y[i] <= (int)pica_db_y1;
        int wr = pica_px_wr[i];
        if (!cov && !wr) continue;
        if (wr && pica_px_me[i] >= 3.0f) continue;   /* routine interior write */
        fprintf(stderr, "PXB t=%.2f draw=%llu px=%d,%d cov=%d wr=%d me=%.2f\n",
                dq8_layer_t(), (unsigned long long)pica_draw_count,
                pica_px_x[i], pica_px_y[i], cov, wr, pica_px_me[i]);
    }
}
static void pica_write_fragment(int x,int y,const float source[4],float zw,float iwv) {
    float dest[4],constant[4];
    /* Alpha test: the setup (enable/func/ref) is constant per-draw, decoded
       into the snapshot (pds) at draw start; only the source alpha varies. */
    if(pds.built ? pds.alpha_on : (pica_regs[0x104]&1)) {
        unsigned aref = pds.built ? pds.alpha_ref : ((pica_regs[0x104]>>8)&255);
        unsigned f = pds.built ? pds.alpha_func : ((pica_regs[0x104]>>4)&7);
        float a=floorf(source[3]*255+0.5f),ref=(float)aref;
        int pass=f==1 || (f==2 && a==ref)||(f==3 && a!=ref)||(f==4 && a<ref)||
                 (f==5 && a<=ref)||(f==6 && a>ref)||(f==7 && a>=ref);
        if(!pass){
            ++pica_draw_log_arej; /* t_074669f7: count alpha-test rejections per draw */
            int pi=pica_px_find(x,y);
            if(pi>=0)fprintf(stderr,"PXA t=%.2f draw=%llu px=%d,%d aref=%u a=%.3f\n",
                dq8_layer_t(),(unsigned long long)pica_draw_count,x,y,aref,source[3]);
            return;
        }
    }
    /* Depth test first (Azahar DoDepthStencilTest): a pass also updates the
       depth buffer when depth writing is enabled for this draw.  With the
       per-draw snapshot (pds, t_568f6976) the same test/write runs on the
       mapping resolved at draw start — bit-identical semantics, no
       per-fragment range revalidation. */
    {
        float depth = pds.built ? pica_fragment_depth_d(zw,iwv)
                    : pica_fragment_depth(zw,iwv,pica_f24_to_f32(pica_regs[0x4D]),
                                          pica_f24_to_f32(pica_regs[0x4E]));
        int depth_pass = pds.built ? pica_depth_test_write_d(x,y,depth)
                                   : pica_depth_test_write(x,y,depth);
        if(!depth_pass){ ++pica_draw_log_drej; return; }
    }
    ++pica_draw_log_frw; /* fragment passed alpha+depth, reaches write */
    /* Raster coordinates are bottom-up, tiled color memory is top-down. */
    int my=(int)pica_rt_h-1-y;
    int opaque=(pds.built ? pds.opaque_cfg :
        ((pica_regs[0x107]&0xf00)==0xf00 &&
         (pica_regs[0x100]&256) && pica_regs[0x101]==0x76760000u)) && source[3]==1.0f;
    /* The put-only fast path below writes these bytes as the pixel color;
       the RMW paths only feed them to the DQ8_TRACE_PX/RECT diagnostics, so
       the quantization is skipped when neither is active. */
    int need_src = opaque || pica_px_n || pica_rect_src_needed();
    uint8_t srcc[4]={0,0,0,0};
    if(need_src)
        for(unsigned k=0;k<4;k++) srcc[k]=(uint8_t)floorf(pica_sat(source[k])*255+0.5f);
    if (opaque) {
        pica_px_log_write(x,y,srcc);
        pica_put_pixel(x,my,srcc[0],srcc[1],srcc[2],255);
        srcc[3]=255;
        pica_rect_log(x,my,srcc,srcc);
        return;
    }
    if (pqt_on) pqt_slow = 1;   /* translucent / RMW path (DQ8_TRACE_TIME) */
    /* Blend fast path (t_568f6976): the game's UI blend config 0x76760000 on
       an RGBA8 target with a full color mask.  Same float expressions in the
       same order as the generic loop below (factor 6 = source alpha, factor
       7 = 1 - source alpha, eq ADD, no masked channels); the per-draw
       snapshot replaces the per-fragment Morton/guest-range/blend-factor
       work.  The float->uint8 cast truncates toward zero, identical to the
       generic path's floorf() for the post-saturation positive range.  Any
       other blend config keeps the generic path. */
    if (pds.fast) {
        uint32_t off = (((uint32_t)my >> 3) * (pds.rt_w >> 3) + ((uint32_t)x >> 3)) * 64u
                       + pica_morton_xl[(uint32_t)x & 7u] + pica_morton_yl[(uint32_t)my & 7u];
        uint8_t *p = pds.rt + off * 4u;   /* RGBA8 stored A,B,G,R */
        float sa = source[3], isa = 1.0f - sa;
        uint8_t result[4];
        dest[0]=pica_unit255[p[3]];dest[1]=pica_unit255[p[2]];
        dest[2]=pica_unit255[p[1]];dest[3]=pica_unit255[p[0]];
        for(unsigned k=0;k<4;k++) {
            float s=source[k]*sa, d=dest[k]*isa;
            result[k]=(uint8_t)(pica_sat(s+d)*255.0f+0.5f);
        }
        if (pqt_on) ++pqt_fast_px;
        pica_px_log_write(x,y,result);
        p[0]=result[3];p[1]=result[2];p[2]=result[1];p[3]=result[0];
        if(need_src) pica_rect_log(x,my,srcc,result);
        return;
    }
    if (pqt_on) ++pqt_gen_px;
    uint8_t r=0,g=0,b=0,a=0;
    pica_get_pixel(pica_rt_addr,pica_rt_w,pica_rt_h,pica_rt_format,x,my,&r,&g,&b,&a);
    dest[0]=r/255.0f;dest[1]=g/255.0f;dest[2]=b/255.0f;dest[3]=a/255.0f;
    pica_rgba(pica_regs[0x103],constant);
    uint8_t result[4];
    for(unsigned k=0;k<4;k++) {
        float value=source[k];
        if(pica_regs[0x100]&256) {
            uint32_t cfg=pica_regs[0x101];unsigned shift=k==3?24:16,eq=(cfg>>(k==3?8:0))&7;
            float s=source[k]*pica_blend_factor((cfg>>shift)&15,k,source,dest,constant);
            float d=dest[k]*pica_blend_factor((cfg>>(shift+4))&15,k,source,dest,constant);
            value=eq==0?s+d:eq==1?s-d:eq==2?d-s:eq==3?fminf(source[k],dest[k]):fmaxf(source[k],dest[k]);
        }
        if(!(pica_regs[0x107]&(1u<<(8+k))))value=dest[k];
        result[k]=(uint8_t)floorf(pica_sat(value)*255+0.5f);
    }
    pica_px_log_write(x,y,result);
    pica_put_pixel(x,my,result[0],result[1],result[2],result[3]);
    pica_rect_log(x,my,srcc,result);
}
/* ---- DQ8_TRACE_FRAGPX=x,y[;x,y...] per-fragment probe (t_e216fc37) -------
   Prints the full per-fragment inputs and outputs (interpolated primary,
   interpolated uv, the tex0 sample, the TEV result) for the listed RASTER
   pixels, for every draw, with no per-draw limit.  DQ8_TRACE_LAYER's FRAG line
   stops after 6 fragments per draw and only below x<240, so it cannot be
   aimed at a pixel; this one can.  Inert (one cached getenv) when unset. */
static int pica_fragpx_n;
static int pica_fragpx_x[16], pica_fragpx_y[16];
static unsigned pica_fragpx_printed;
static void pica_fragpx_init(void) {
    static int done;
    const char *e;
    if (done) return;
    done = 1;
    e = getenv("DQ8_TRACE_FRAGPX");
    while (e && *e && pica_fragpx_n < 16) {
        char *end;
        long x = strtol(e, &end, 10);
        if (end == e || *end != ',') break;
        long y = strtol(end + 1, &end, 10);
        pica_fragpx_x[pica_fragpx_n] = (int)x;
        pica_fragpx_y[pica_fragpx_n] = (int)y;
        pica_fragpx_n++;
        e = end;
        while (*e == ' ' || *e == ';' || *e == ',') e++;
    }
}
static int pica_fragpx_hit(int x, int y) {
    pica_fragpx_init();
    for (int i = 0; i < pica_fragpx_n; i++)
        if (pica_fragpx_x[i] == x && pica_fragpx_y[i] == y) return 1;
    return 0;
}
/* Does any TEV stage read texture unit 1 / 2 (source code 4 / 5)?  The answer
   is constant per draw, so pica_fragment() caches it and skips the second
   sampler call for the (majority of) draws that never touch unit 1. */
static int pica_tev_reads_texunit(unsigned unit) {
    static const unsigned bases[6] = {0xc0,0xc8,0xd0,0xd8,0xf0,0xf8};
    unsigned code = unit == 1u ? 4u : 5u;
    for (unsigned s = 0; s < 6; ++s) {
        static const unsigned nib[6] = {0,1,2,4,5,6}; /* color1-3, alpha1-3 */
        uint32_t sources = pica_regs[bases[s]];
        for (unsigned i = 0; i < 6; ++i)
            if (((sources >> (4u * nib[i])) & 15u) == code) return 1;
    }
    return 0;
}
/* DQ8_TEV_TEX1 (default on): sample texture unit 1 for TEV source 4.  =0
   restores the pre-fix behaviour (source 4 reads 0.0), i.e. the black ground
   quad, so the A/B runs can use ONE binary and cannot differ by build. */
static int pica_tev_tex1_gate = -1;
static int pica_tev_tex1_on(void) {
    if (pica_tev_tex1_gate < 0) {
        const char *e = getenv("DQ8_TEV_TEX1");
        pica_tev_tex1_gate = !(e && e[0] == '0');
    }
    return pica_tev_tex1_gate;
}
/* DQ8_GL_TEV_TEX1 (default OFF): also make the GL backend REFUSE draws whose
   TEV chain reads texture unit 1/2 and rasterize them in software.  Off by
   default: the software draw does not survive the deferred GL episode
   write-back yet (the ground then shows the sky colour), so the GL renderer
   keeps its previous output instead of trading one artifact for another.
   t_e216fc37 §5. */
static int pica_gl_tev_tex1_gate = -1;
static int pica_gl_tev_tex1_on(void) {
    if (pica_gl_tev_tex1_gate < 0) {
        const char *e = getenv("DQ8_GL_TEV_TEX1");
        pica_gl_tev_tex1_gate = (e && e[0] && e[0] != '0');
    }
    return pica_gl_tev_tex1_gate;
}
static int pica_fragpx_in(int lx, int ly, int hx, int hy) {
    pica_fragpx_init();
    for (int i = 0; i < pica_fragpx_n; i++)
        if (pica_fragpx_x[i] >= lx && pica_fragpx_x[i] <= hx &&
            pica_fragpx_y[i] >= ly && pica_fragpx_y[i] <= hy) return 1;
    return 0;
}
static void pica_fragment(int x,int y,const float primary[4],float u,float v,
                          float u1,float v1,float zw,float iwv) {
    float texture[4]={0,0,0,0},texture1[4]={0,0,0,0},source[4],dest[4],constant[4];
    uint8_t tr=0,tg=0,tb=0,ta=0;
    uint8_t t1r=0,t1g=0,t1b=0,t1a=0;
    static uint64_t frag_u1_draw = ~0ull;
    static int frag_u1_ref;
    if((pica_regs[0x80]&1) && pica_sample_tex0(u,v,&tr,&tg,&tb,&ta)) {
        texture[0]=tr/255.0f;texture[1]=tg/255.0f;texture[2]=tb/255.0f;texture[3]=ta/255.0f;
    }
    /* Texture unit 1 (TEV source 4, sampled with texcoord1).  Only when the
       unit is enabled AND some stage actually reads it: the second sampler
       call is not free and almost no draw uses it. */
    if (frag_u1_draw != pica_draw_count) {
        frag_u1_draw = pica_draw_count;
        frag_u1_ref = pica_tev_reads_texunit(1u);
    }
    if (frag_u1_ref && pica_tev_tex1_on() && (pica_regs[0x80]&2u) &&
        pica_sample_tex1(u1,v1,&t1r,&t1g,&t1b,&t1a)) {
        texture1[0]=t1r/255.0f;texture1[1]=t1g/255.0f;
        texture1[2]=t1b/255.0f;texture1[3]=t1a/255.0f;
    }
    pica_tev(primary,texture,texture1,source);
    pica_draw_tev_a=source[3];
    if (pica_fragpx_hit(x,y) && pica_fragpx_printed < 400u) {
        ++pica_fragpx_printed;
        fprintf(stderr, "FRAGPX t=%.2f draw=%llu px=%d,%d uv=%.6f,%.6f uv1=%.6f,%.6f "
                "tex=%u,%u,%u,%u tex1=%u,%u,%u,%u "
                "pri=%.4f,%.4f,%.4f,%.4f tev=%.4f,%.4f,%.4f,%.4f\n",
                dq8_layer_t(), (unsigned long long)pica_draw_count, x, y, u, v, u1, v1,
                tr, tg, tb, ta, t1r, t1g, t1b, t1a,
                primary[0], primary[1], primary[2], primary[3],
                source[0], source[1], source[2], source[3]);
    }
    { /* t_074669f7: trace first few fragments per draw to diagnose terrain invisibility */
        static unsigned frag_trace_draw, frag_trace_n;
        static int frag_trace_on = -1; /* t_bc6b4eb7: getenv out of the
                                          per-fragment loop (cache once) */
        if (frag_trace_draw != pica_draw_count) { frag_trace_draw = pica_draw_count; frag_trace_n = 0; }
        if (frag_trace_on < 0) {
            const char *e = getenv("DQ8_TRACE_LAYER");
            frag_trace_on = (e && e[0] && e[0] != '0') ? 1 : 0;
        }
        if (frag_trace_on && frag_trace_n < 6u && x < 240) {
            fprintf(stderr, "FRAG t=%.2f draw=%llu px=%d,%d uv=%.4f,%.4f tex=%u,%u,%u,%u pri=%.2f,%.2f,%.2f,%.2f tev=%.2f,%.2f,%.2f,%.2f\n",
                    dq8_layer_t(), (unsigned long long)pica_draw_count, x, y, u, v,
                    tr, tg, tb, ta,
                    primary[0], primary[1], primary[2], primary[3],
                    source[0], source[1], source[2], source[3]);
            ++frag_trace_n;
        }
    }
    pica_write_fragment(x,y,source,zw,iwv);
}
typedef struct {float x,y,iw,zw,color[4],u,v,u1,v1,cx,cy,cz,cw;} PicaVertex;
/* pica_flush_triangles() memcpy's sizeof(PicaGlVertex) bytes out of a
   PicaVertex, so the two structs MUST stay layout-identical field for field.
   They did not (PicaGlVertex lacked zw) which shifted color/u/v by one float
   and made every GL TEXTURE_MODULATE draw sample the wrong texels (black
   intro movie).  Guard against the field list ever drifting again. */
_Static_assert(sizeof(PicaGlVertex) == sizeof(PicaVertex),
               "PicaGlVertex must stay layout-identical to PicaVertex");
_Static_assert(offsetof(PicaGlVertex, iw) == offsetof(PicaVertex, iw) &&
               offsetof(PicaGlVertex, color) == offsetof(PicaVertex, color) &&
               offsetof(PicaGlVertex, u) == offsetof(PicaVertex, u) &&
               offsetof(PicaGlVertex, v) == offsetof(PicaVertex, v) &&
               offsetof(PicaGlVertex, u1) == offsetof(PicaVertex, u1) &&
               offsetof(PicaGlVertex, v1) == offsetof(PicaVertex, v1),
               "PicaGlVertex field offsets must match PicaVertex");
static float pica_edge(const PicaVertex *a,const PicaVertex *b,float x,float y) {
    return (b->x-a->x)*(y-a->y)-(b->y-a->y)*(x-a->x);
}
/* Canonical shared-edge evaluation: a quad's two triangles evaluate the SAME
   edge with reversed argument order, and the two orderings are NOT exact
   float negations of each other (different intermediate roundings). When the
   seam passes exactly through pixel centres the sign noise then makes BOTH
   triangles reject the pixel (1px hole -> the diagonal artifact line) or both
   accept it (double blend). Normalising the vertex order gives both triangles
   one bit-identical expression; the caller's (u,v) order only flips the sign,
   which IEEE negation does exactly, so the two tests can never disagree. */
static float pica_edge_canon(const PicaVertex *u,const PicaVertex *v,float x,float y) {
    int flip = (v->x < u->x) || (v->x == u->x && v->y < u->y);
    const PicaVertex *a = flip ? v : u;
    const PicaVertex *b = flip ? u : v;
    float e = (b->x-a->x)*(y-a->y)-(b->y-a->y)*(x-a->x);
    return flip ? -e : e;
}
static int pica_edge_in(float e,const PicaVertex *a,const PicaVertex *b) {
    return e>0 || (e==0 && (b->y>a->y || (b->y==a->y && b->x<a->x)));
}
/* ---- PICA face culling: GPUREG_FACECULLING_CONFIG (register 0x40) -------
   t_150574cd.  The guest selects which screen-space winding the rasterizer
   keeps; neither backend read this register before, so every triangle was
   rewound by pica_raster_triangle() and drawn, including the back faces the
   hardware drops (the cape's inner/outline shell painted over the front one).

   Register layout (Azahar RasterizerRegs, first field of the block that also
   holds viewport_size_x at 0x41, so the register index is the raw command
   index): bits 0-1 = cull mode
       0 KeepAll 1 KeepClockWise 2 KeepCounterClockWise 3 KeepAll2.

   Reference semantics (azahar-master renderer_software/sw_rasterizer.cpp:246):
       KeepAll/KeepAll2     -> keep both sides (normalised to the positive
                               winding, hence the rewind),
       KeepClockWise        -> reverse and draw, i.e. keep area <  0,
       KeepCounterClockWise -> cull `SignedArea(v0,v1,v2) <= 0`, keep area > 0.
   `area` here is pica_edge(&a,&b,c), bit-for-bit the value the rasteriser's
   own rewind test uses, and the screen coordinates are produced by the same
   viewport transform Azahar uses (v = (c/w + 1)*halfsize + corner, y down),
   so Azahar's SignedArea convention applies unchanged.

   Applied in pica_queue_triangle(), i.e. on every triangle that actually
   reaches a backend: the clip stage fans straddling primitives into it too,
   exactly like Azahar (clipping in AddTriangle, culling in ProcessTriangle).
   Both backends therefore receive an identical triangle set - the software
   rasteriser directly, the GL backend (which rasterises these same triangles
   in its fragment shader) through pica_tri_buf.

   Knobs: DQ8_CULL=0 disables the filter (pre-change behaviour, for A/B);
   DQ8_CULL_FLIP=1 inverts the winding test - a diagnostic used to prove the
   sign against a reference capture, not a shipping option. */
static int pica_cull_gate = -1, pica_cull_flip;
static int pica_cull_on(void) {
    if (pica_cull_gate < 0) {
        const char *e = getenv("DQ8_CULL");
        const char *f = getenv("DQ8_CULL_FLIP");
        pica_cull_gate = !(e && e[0] == '0');
        pica_cull_flip = (f && f[0] && f[0] != '0');
    }
    return pica_cull_gate;
}
/* 1 = this triangle is drawn, 0 = the guest's cull mode drops it. */
static int pica_cull_keep(float area) {
    unsigned mode;
    int ccw;
    if (!pica_cull_on()) return 1;
    mode = pica_regs[0x40] & 3u;
    if (mode != 1u && mode != 2u) return 1;   /* KeepAll / KeepAll2 */
    ccw = pica_cull_flip ? (area < 0.0f) : (area > 0.0f);
    return (mode == 2u) ? ccw : !ccw;
}
static void pica_raster_triangle(PicaVertex a,PicaVertex b,PicaVertex c) {
    float area=pica_edge(&a,&b,c.x,c.y);
    if(!isfinite(area) || fabsf(area)<1e-7f)return;
    if(area<0) {PicaVertex t=b;b=c;c=t;area=-area;}
    int lx=(int)fmaxf(0,floorf(fminf(a.x,fminf(b.x,c.x))));
    int ly=(int)fmaxf(0,floorf(fminf(a.y,fminf(b.y,c.y))));
    int hx=(int)fminf(pica_rt_w-1,ceilf(fmaxf(a.x,fmaxf(b.x,c.x))));
    int hy=(int)fminf(pica_rt_h-1,ceilf(fmaxf(a.y,fmaxf(b.y,c.y))));
    if (clst_on > 0 && pica_draw_log_count >= 1000u) {
        if (lx > hx || ly > hy) ++clst_tri_nodesign;
        else ++clst_tri_onscreen;
        if (clst_tri_dump < 6u) {
            ++clst_tri_dump;
            fprintf(stderr, "TRI a=%+.3f,%+.3f b=%+.3f,%+.3f c=%+.3f,%+.3f area=%+.6g "
                    "bbox=%d,%d..%d,%d\n",
                    a.x,a.y,b.x,b.y,c.x,c.y,area,lx,ly,hx,hy);
        }
    }
    pica_bb_add(lx,ly); pica_bb_add(hx,hy);
    pica_db_add(lx,ly); pica_db_add(hx,hy);
    if (pica_fragpx_in(lx,ly,hx,hy)) {
        static uint64_t fv_draw;
        static unsigned fv_n, fv_total;
        if (fv_draw != pica_draw_count) { fv_draw = pica_draw_count; fv_n = 0; }
        if (fv_n < 8u && fv_total < 200u) {
            ++fv_n; ++fv_total;
            fprintf(stderr, "FRAGVTX t=%.2f draw=%llu area=%.4f "
                    "a=(%.2f,%.2f u=%.6f,%.6f c=%.3f,%.3f,%.3f,%.3f) "
                    "b=(%.2f,%.2f u=%.6f,%.6f c=%.3f,%.3f,%.3f,%.3f) "
                    "c=(%.2f,%.2f u=%.6f,%.6f c=%.3f,%.3f,%.3f,%.3f) "
                    "iw=%.6g,%.6g,%.6g\n",
                    dq8_layer_t(), (unsigned long long)pica_draw_count, area,
                    a.x,a.y,a.u,a.v,a.color[0],a.color[1],a.color[2],a.color[3],
                    b.x,b.y,b.u,b.v,b.color[0],b.color[1],b.color[2],b.color[3],
                    c.x,c.y,c.u,c.v,c.color[0],c.color[1],c.color[2],c.color[3],
                    a.iw,b.iw,c.iw);
        }
    }
    /* DQ8_TRACE_PX: nearest distance from each probe to this triangle's edge
       lines — a probe sitting ON a triangle edge (seam candidate) gets ~0. */
    pica_px_init();
    for (int k = 0; k < pica_px_n; k++) {
        int pxp = pica_px_x[k], pyp = pica_px_y[k];
        float fx, fy, e0, e1, e2, l0, l1, l2, d, d0, d1, d2;
        if (pxp < lx || pxp > hx || pyp < ly || pyp > hy) continue;
        fx = pxp + 0.5f; fy = pyp + 0.5f;
        e0 = pica_edge(&b,&c,fx,fy); e1 = pica_edge(&c,&a,fx,fy); e2 = pica_edge(&a,&b,fx,fy);
        l0 = sqrtf((b.x-c.x)*(b.x-c.x)+(b.y-c.y)*(b.y-c.y));
        l1 = sqrtf((c.x-a.x)*(c.x-a.x)+(c.y-a.y)*(c.y-a.y));
        l2 = sqrtf((a.x-b.x)*(a.x-b.x)+(a.y-b.y)*(a.y-b.y));
        d0 = fabsf(e0) / (l0 > 1.0f ? l0 : 1.0f);
        d1 = fabsf(e1) / (l1 > 1.0f ? l1 : 1.0f);
        d2 = fabsf(e2) / (l2 > 1.0f ? l2 : 1.0f);
        d = d0 < d1 ? d0 : d1; if (d2 < d) d = d2;
        if (d < pica_px_me[k]) pica_px_me[k] = d;
    }
    int constant_color=!(pica_regs[0x80]&7) &&
        !memcmp(a.color,b.color,sizeof a.color) && !memcmp(a.color,c.color,sizeof a.color);
    float solid[4];
    if(constant_color) {const float zero[4]={0};pica_tev(a.color,zero,zero,solid);pica_draw_tev_a=solid[3];}
    for(int y=ly;y<=hy;y++)for(int x=lx;x<=hx;x++) {
        float e0=pica_edge_canon(&b,&c,x+0.5f,y+0.5f),e1=pica_edge_canon(&c,&a,x+0.5f,y+0.5f),e2=pica_edge_canon(&a,&b,x+0.5f,y+0.5f);
        if(!pica_edge_in(e0,&b,&c)||!pica_edge_in(e1,&c,&a)||!pica_edge_in(e2,&a,&b))continue;
        ++pica_cov_px;
        if(constant_color) {pica_write_fragment(x,y,solid,a.zw,a.iw);continue;}
        float w0=e0*a.iw,w1=e1*b.iw,w2=e2*c.iw,sum=w0+w1+w2;
        if(fabsf(sum)<1e-20f)continue;
        w0/=sum;w1/=sum;w2/=sum;float color[4];
        for(unsigned k=0;k<4;k++)color[k]=pica_sat(w0*a.color[k]+w1*b.color[k]+w2*c.color[k]);
        /* Depth interpolates z/w LINEARLY in screen space (Azahar), unlike the
           perspective-correct colour/uv weights above. */
        { float lsum=e0+e1+e2;
          float zwl=lsum!=0.0f?(e0*a.zw+e1*b.zw+e2*c.zw)/lsum:a.zw;
          float iwl=lsum!=0.0f?(e0*a.iw+e1*b.iw+e2*c.iw)/lsum:a.iw;
          pica_fragment(x,y,color,w0*a.u+w1*b.u+w2*c.u,w0*a.v+w1*b.v+w2*c.v,
                        w0*a.u1+w1*b.u1+w2*c.u1,w0*a.v1+w1*b.v1+w2*c.v1,zwl,iwl); }
    }
}
/* ---- DQ8_TRACE_VSDUMP: full VS-state + per-instruction trace (t_664244f9
   POSITION_W negated-garbage forensics).  Env DQ8_TRACE_VSDUMP=<path> opens
   the dump file; draws whose vertex count matches DQ8_TRACE_VSDRAW (default
   1638) are captured -- first DQ8_TRACE_VSDMAX draws (default 12), with a full
   instruction trace for the first DQ8_TRACE_VSDVERT vertices (default 2) of
   each.  Each captured draw records: the VS-relevant registers, all 96 float
   uniforms (+default attributes), the whole program and swizzle tables, then
   per traced vertex: attribute fetch table, input registers, per-instruction
   src/dst values, output registers and the mapped semantics.  Inert without
   the env var (no file I/O, no behaviour change). */
static unsigned pvs_draws, pvs_vert, pvs_max_draws = 12, pvs_max_vert = 2;
static unsigned pvs_filter_count = 1638, pvs_live_saved, pvs_max_live = 2, pvs_dead_only;
static char pvs_filter_list[64] = "1638";
static char pvs_path[512];
static int pvs_file_made;
static int pvs_prog_dump = -1;   /* DQ8_VSPROGDUMP=1: dump the VS program once */
static void pvs_prog_dump_once(void) {
    unsigned i;
    if (pvs_prog_dump < 0)
        pvs_prog_dump = getenv("DQ8_VSPROGDUMP") ? 6 : 0;
    if (!pvs_prog_dump) return;
    --pvs_prog_dump;
    fprintf(stderr, "VSPROGSZ=%u SWZSZ=%u draw=%llu\n", vs_program_size,
            vs_swizzle_size, (unsigned long long)pica_draw_count);
    for (i = 0; i < vs_program_size; i++)
        fprintf(stderr, "VSPROG %u %08X\n", i, vs_program[i]);
    for (i = 0; i < vs_swizzle_size; i++)
        fprintf(stderr, "VSSWZ %u %08X\n", i, vs_swizzle[i]);
}

/* t_275eb521: DQ8_TRACE_VSDUMP is consulted once per draw; resolve the string
   once per process (it cannot change at run time). */
static const char *pvs_trace_path_cached(void) {
    static const char *path;
    static int init;
    if (!init) { init = 1; path = getenv("DQ8_TRACE_VSDUMP"); }
    return path;
}
static void pvs_open_for_draw(unsigned count) {
    const char *path;
    if (pvs_f) return;
    path = pvs_trace_path_cached();
    if (!path || !*path) return;
    {   const char *e = getenv("DQ8_TRACE_VSDRAW");
        if (e && *e) {
            snprintf(pvs_filter_list, sizeof pvs_filter_list, "%s", e);
        } }
    {   const char *e = getenv("DQ8_TRACE_VSDMAX");
        if (e && *e) pvs_max_draws = (unsigned)strtoul(e, NULL, 0); }
    {   const char *e = getenv("DQ8_TRACE_VSDVERT");
        if (e && *e) pvs_max_vert = (unsigned)strtoul(e, NULL, 0); }
    {   const char *e = getenv("DQ8_TRACE_VSDDEAD");
        pvs_dead_only = (e && *e && *e != '0') ? 1u : 0u; }
    {   const char *e = getenv("DQ8_TRACE_VSDLIVE");
        if (e && *e) pvs_max_live = (unsigned)strtoul(e, NULL, 0); }
    {   /* DQ8_TRACE_VSDRAW may list several batch sizes (comma-separated). */
        int match = 0;
        const char *mp = pvs_filter_list;
        while (*mp) {
            char *end;
            unsigned v = (unsigned)strtoul(mp, &end, 0);
            if (end == mp) break;
            if (v == count) { match = 1; break; }
            mp = end;
            while (*mp == ',') ++mp;
        }
        if (!match || pvs_draws >= pvs_max_draws) return;
    }
    {   /* Time window on dq8_layer_t() (same clock as the DRAW lines): the
           character batch is drawn every frame in many scenes, so without a
           window the capture budget fills before the target scene. */
        double t = (double)dq8_layer_t();
        const char *e = getenv("DQ8_TRACE_VSDT0");
        if (e && *e && t < strtod(e, NULL)) return;
        e = getenv("DQ8_TRACE_VSDT1");
        if (e && *e && t > strtod(e, NULL)) return;
    }
    snprintf(pvs_path, sizeof pvs_path, "%s", path);
    if (pvs_dead_only) {
        /* Buffered capture: the draw is only appended to the dump file if it
           turns out dead (every vertex clipped) or within the live-keep
           budget -- the batch draws every frame, so unconditional capture
           burns the budget on live draws before the target scene state. */
        pvs_f = tmpfile();
    } else {
        pvs_f = fopen(pvs_path, pvs_file_made ? "a" : "w");
    }
    if (!pvs_f) return;
    if (!pvs_dead_only) { ++pvs_draws; pvs_file_made = 1; }
    pvs_vert = 0;
    fprintf(pvs_f, "VSDRAW draw=%llu t=%.2f count=%u entry=%04X\n",
            (unsigned long long)pica_draw_count, dq8_layer_t(), count,
            pica_regs[0x2ba] & 0xFFFFu);
    fprintf(pvs_f, "VSREGS");
    {   unsigned i;
        for (i = 0x200; i <= 0x2BF; i++) fprintf(pvs_f, " %03X=%08X", i, pica_regs[i]);
        for (i = 0x2BD; i <= 0x2BD; i++) fprintf(pvs_f, " %03X=%08X", i, pica_regs[i]);
        for (i = 0x4F; i <= 0x56; i++) fprintf(pvs_f, " %03X=%08X", i, pica_regs[i]);
        for (i = 0x2B0; i <= 0x2BC; i++) fprintf(pvs_f, " %03X=%08X", i, pica_regs[i]); }
    fprintf(pvs_f, "\n");
    {   unsigned i, k;
        for (i = 0; i < 96; i++) {
            fprintf(pvs_f, "VSUNI %u", i);
            for (k = 0; k < 4; k++) {
                uint32_t raw; memcpy(&raw, &vs_uniform_f[i][k], 4);
                fprintf(pvs_f, " %08X=%+.9g", raw, vs_uniform_f[i][k]);
            }
            fprintf(pvs_f, "\n");
        }
        for (i = 0; i < 16; i++) {
            fprintf(pvs_f, "VSDEF %u", i);
            for (k = 0; k < 4; k++) {
                uint32_t raw; memcpy(&raw, &vs_default_attrib[i][k], 4);
                fprintf(pvs_f, " %08X=%+.9g", raw, vs_default_attrib[i][k]);
            }
            fprintf(pvs_f, "\n");
        } }
    {   unsigned i;
        for (i = 0; i < vs_program_size; i++)
            fprintf(pvs_f, "VSPROG %u %08X\n", i, vs_program[i]);
        for (i = 0; i < vs_swizzle_size; i++)
            fprintf(pvs_f, "VSSWZ %u %08X\n", i, vs_swizzle[i]); }
}
/* ---- t_41f38df0 cutscene-camera forensics.
   DQ8_TRACE_UNIDRAW=<count>[,<count>...]  log the camera/model uniforms
   (UNI 3-5 view rows, UNI 25-27 model/bone-0 rows, UNI 90-92 normal rows,
   UNI 85) of every draw whose vertex count matches, once per draw and capped,
   so the camera the character batch gets can be diffed against the camera the
   sibling batches of the SAME frame get (they rasterize; if they share the
   camera the camera is fine and the model transform is the defect).
   DQ8_DUMP_PICA_COUNT=<count> additionally arms a one-shot dump of the next
   command list that carries such a draw (pica_process_command_list).
   DQ8_TRACE_UNIDRAW_MAX=<n>  raise the UNICAM line budget (default 400; the
   cutscene starts at t~436 and 400 lines were spent by t~400).
   DQ8_TRACE_VBUF=<count>      fingerprint the vertex buffer such a draw reads
   (pvs_vbuf_trace): decides stale-mesh vs wrong-camera and prints the VA for
   a follow-up DQ8_TRACE_WRITE run. */
static unsigned pvs_unidraw[8], pvs_nunidraw;
static int pvs_unidraw_init = -1, pvs_unidraw_on;
static unsigned pvs_unidraw_hits;
/* UNICAM line budget (was a hard 400).  bw-cam3/bw-cam4 tripped it at t~400,
   before the cutscene starts (t~436), so the dialog-frame sibling cameras were
   never logged.  DQ8_TRACE_UNIDRAW_MAX raises it for a targeted run. */
static unsigned pvs_unidraw_cap(void) {
    static unsigned cap = 400u, cap_init;
    if (!cap_init) {
        const char *m = getenv("DQ8_TRACE_UNIDRAW_MAX");
        if (m && *m) cap = (unsigned)strtoul(m, NULL, 0);
        cap_init = 1;
    }
    return cap;
}
static int pvs_unidraw_match(unsigned count) {
    if (pvs_unidraw_init < 0) {
        const char *e = getenv("DQ8_TRACE_UNIDRAW");
        pvs_unidraw_init = 1;
        if (e && *e) {
            char buf[256]; char *save = NULL;
            snprintf(buf, sizeof buf, "%s", e);
            for (char *p = strtok_r(buf, ",", &save);
                 p && pvs_nunidraw < sizeof pvs_unidraw / sizeof pvs_unidraw[0];
                 p = strtok_r(NULL, ",", &save))
                pvs_unidraw[pvs_nunidraw++] = (unsigned)strtoul(p, NULL, 0);
        }
        pvs_unidraw_on = pvs_nunidraw != 0;
        {   const char *d = getenv("DQ8_DUMP_PICA_COUNT");
            if (d && *d) pvs_unidraw_on = 1; }
    }
    if (!pvs_unidraw_on || pvs_unidraw_hits >= pvs_unidraw_cap()) return 0;
    for (unsigned i = 0; i < pvs_nunidraw; ++i)
        if (pvs_unidraw[i] == count) return 1;
    return 0;
}
/* t_41f38df0 DQ8_TRACE_VBUF=<count>: fingerprint the vertex buffer the probed
   draw reads (same address arithmetic pica_load_vertex uses: base from 0x200,
   attr0 offset from 0x203, stride from 0x205, index base from 0x22A).
   DECIDES the P4 discriminator without a second run: if the hash tracks the
   camera (world state) the mesh is camera-relative and the cutscene draw runs
   on a stale buffer; if it never changes the mesh is camera-independent and
   the camera value itself is the defect.  Logs the VA too, so a following
   DQ8_TRACE_WRITE run can watch exactly this range. */
static void pvs_vbuf_trace(unsigned count) {
    static int on = -1;
    static unsigned want;
    if (on < 0) {
        const char *e = getenv("DQ8_TRACE_VBUF");
        on = (e && *e) ? 1 : 0;
        if (on) want = (unsigned)strtoul(e, NULL, 0);
    }
    if (!on || want != count) return;
    uint32_t base = ((pica_regs[0x200] >> 1) & 0x0FFFFFFFu) * 16u;
    uint32_t off = pica_regs[0x203] & 0x0FFFFFFFu;
    uint32_t idx0 = pica_regs[0x22a];
    unsigned stride = (pica_regs[0x205] >> 16) & 0xFFu;
    uint64_t start = (uint64_t)base + off + (uint64_t)idx0 * stride;
    uint64_t len = (uint64_t)count * stride;
    uint32_t mapped;
    if (!stride || start > UINT32_MAX || len > 0x400000u ||
        !guest_memory_range((uint32_t)start, (uint32_t)len, &mapped)) {
        fprintf(stderr, "VBUF t=%.2f draw=%llu count=%u FAIL va=%llX len=%llu "
                "base=%08X off=%08X idx0=%u stride=%u\n",
                dq8_layer_t(), (unsigned long long)pica_draw_count, count,
                (unsigned long long)start, (unsigned long long)len,
                base, off, idx0, stride);
        return;
    }
    uint32_t h = 2166136261u;
    for (uint64_t i = 0; i < len; i++) { h ^= ram[mapped + i]; h *= 16777619u; }
    fprintf(stderr, "VBUF t=%.2f draw=%llu count=%u hash=%08X va=%08X len=%llu "
            "base=%08X off=%08X idx0=%u stride=%u head=%02X%02X%02X%02X%02X%02X%02X%02X\n",
            dq8_layer_t(), (unsigned long long)pica_draw_count, count, h,
            (uint32_t)start, (unsigned long long)len, base, off, idx0, stride,
            ram[mapped], ram[mapped + 1], ram[mapped + 2], ram[mapped + 3],
            ram[mapped + 4], ram[mapped + 5], ram[mapped + 6], ram[mapped + 7]);
}
/* t_41f38df0 DQ8_FIND_FLOATS=<f32,f32,...> + DQ8_FIND_ON_COUNT=<count>:
   at the probed draw, scan guest RAM for each float32 bit pattern and print
   every VA holding it (one shot).  Locates the cutscene camera data without a
   guess: the game's camera struct, the GX/PICA command buffer copy and any
   intermediate all show up, and the VA is then handed to DQ8_TRACE_WRITE to
   catch the guest PC/LR that stores the camera. */
static void pvs_find_floats(unsigned count) {
    static int on = -1;
    static unsigned want, npat, fired, uni_mode;
    static uint32_t pat[8];
    if (on < 0) {
        on = 0;
        const char *c = getenv("DQ8_FIND_ON_COUNT"), *e = getenv("DQ8_FIND_FLOATS");
        if (c && *c) {
            want = (unsigned)strtoul(c, NULL, 0);
            if (e && !strcmp(e, "UNI")) {
                /* patterns are taken at FIRE time, not here: this function is
                   called for every draw, so the first call happens long before
                   the cutscene uploads its uniforms (bw-cam7 captured the
                   boot-time zeros/0.5 and scanned for those). */
                uni_mode = 1;
                npat = 6;
            } else if (e && *e) {
                char buf[512]; char *save = NULL;
                snprintf(buf, sizeof buf, "%s", e);
                for (char *p = strtok_r(buf, ",", &save); p && npat < 8u;
                     p = strtok_r(NULL, ",", &save)) {
                    float f = (float)strtod(p, NULL);
                    uint32_t b; memcpy(&b, &f, 4);
                    pat[npat++] = b;
                }
            }
            on = npat != 0;
        }
    }
    if (!on || want != count || fired) return;
    /* only the CLIPPED instance (same gate as the CMDBIN dump): the world-state
       count=1638 draw fires first and would pin the wrong camera. */
    if (pica_draw_log_tris || pica_draw_log_clip < (int)count) return;
    fired = 1;
    if (uni_mode) {
        /* exact bits of the uniforms live at this draw: eye (UNI 25-27 .w)
           and the view translation (UNI 3-5 .w) — no decimal round-trip risk,
           works whatever the run's camera values are. */
        static const unsigned idx[6] = { 25, 26, 27, 3, 4, 5 };
        npat = 0;
        for (unsigned k = 0; k < 6u; ++k) {
            uint32_t b;
            memcpy(&b, &vs_uniform_f[idx[k]][3], 4);
            pat[npat++] = b;
        }
    }
    /* drop degenerate patterns (0/1/-1 appear millions of times and would only
       produce noise — the eye is never such a value, but a view component can be) */
    {   unsigned w = 0;
        for (unsigned k = 0; k < npat; ++k) {
            if (pat[k] == 0x00000000u || pat[k] == 0x80000000u ||
                pat[k] == 0x3F800000u || pat[k] == 0xBF800000u) continue;
            pat[w++] = pat[k];
        }
        npat = w;
    }
    fprintf(stderr, "FIND start t=%.2f draw=%llu count=%u patterns=%u\n",
            dq8_layer_t(), (unsigned long long)pica_draw_count, count, npat);
    /* host-offset windows that guest VAs can map into: code/heap, LINEAR+FCRAM
       alias, VRAM.  guest_memory_address() is identity except those aliases. */
    static const struct { uint32_t lo, hi; } win[3] = {
        { 0x00100000u, 0x10000000u }, { 0x14000000u, 0x1C000000u },
        { 0x1F000000u, 0x1F600000u },
    };
    for (unsigned k = 0; k < npat; ++k) {
        unsigned shown = 0;
        for (unsigned w = 0; w < 3u && shown < 24u; ++w) {
            for (uint32_t o = win[w].lo; o + 4u <= win[w].hi && shown < 24u; o += 4u) {
                uint32_t v;
                memcpy(&v, ram + o, 4);
                if (v != pat[k]) continue;
                /* invert guest_memory_address(): LINEAR alias or FCRAM VA. */
                uint32_t va = o;
                if (o >= 0x14000000u && o < 0x1C000000u)
                    va = 0x20000000u + (o - 0x14000000u);
                fprintf(stderr, "FIND hit pat=%u bits=%08X host=%08X va=%08X va_alias=%08X\n",
                        k, pat[k], o, va, o);
                ++shown;
            }
        }
        if (!shown)
            fprintf(stderr, "FIND hit pat=%u bits=%08X none\n", k, pat[k]);
    }
    fprintf(stderr, "FIND done\n");
}
static void pica_uni_cam_trace(unsigned count) {
    /* Dump gate lives BEFORE pvs_unidraw_match: that matcher has a global hit
       cap (bw-cam3 hit it by t~404, the cutscene only starts at t~588), which
       silently disarmed the command-list capture for the draw we care about. */
    {   static int probe_count = -1;
        if (probe_count < 0) {
            const char *d = getenv("DQ8_DUMP_PICA_COUNT");
            probe_count = (d && *d) ? (int)strtoul(d, NULL, 0) : 0;
        }
        if (probe_count && (unsigned)probe_count == count)
            pica_dump_current_cmdlist(count);
    }
    pvs_vbuf_trace(count);
    pvs_find_floats(count);
    if (!pvs_unidraw_match(count)) return;
    ++pvs_unidraw_hits;
    fprintf(stderr,
        "UNICAM t=%.2f draw=%llu count=%u "
        "V=[%+.6f,%+.6f,%+.6f,%+.6f|%+.6f,%+.6f,%+.6f,%+.6f|%+.6f,%+.6f,%+.6f,%+.6f] "
        "M=[%+.6f,%+.6f,%+.6f,%+.6f|%+.6f,%+.6f,%+.6f,%+.6f|%+.6f,%+.6f,%+.6f,%+.6f] "
        "N=[%+.6f,%+.6f,%+.6f|%+.6f,%+.6f,%+.6f] A85=[%+.6f,%+.6f,%+.6f]\n",
        dq8_layer_t(), (unsigned long long)pica_draw_count, count,
        vs_uniform_f[3][0], vs_uniform_f[3][1], vs_uniform_f[3][2], vs_uniform_f[3][3],
        vs_uniform_f[4][0], vs_uniform_f[4][1], vs_uniform_f[4][2], vs_uniform_f[4][3],
        vs_uniform_f[5][0], vs_uniform_f[5][1], vs_uniform_f[5][2], vs_uniform_f[5][3],
        vs_uniform_f[25][0], vs_uniform_f[25][1], vs_uniform_f[25][2], vs_uniform_f[25][3],
        vs_uniform_f[26][0], vs_uniform_f[26][1], vs_uniform_f[26][2], vs_uniform_f[26][3],
        vs_uniform_f[27][0], vs_uniform_f[27][1], vs_uniform_f[27][2], vs_uniform_f[27][3],
        vs_uniform_f[90][0], vs_uniform_f[90][1], vs_uniform_f[90][2],
        vs_uniform_f[91][0], vs_uniform_f[91][1], vs_uniform_f[91][2],
        vs_uniform_f[92][0], vs_uniform_f[92][1], vs_uniform_f[92][2],
        vs_uniform_f[85][0], vs_uniform_f[85][1], vs_uniform_f[85][2]);
    {   const char *d = getenv("DQ8_DUMP_PICA_COUNT");
        if (d && *d && (unsigned)strtoul(d, NULL, 0) == count)
            pica_dump_current_cmdlist(count); }
}

/* Clip plane: primitives are cut at w = PICA_W_EPSILON (hardware clips at the
   eye plane; azahar's sw_rasterizer clips vs w=EPSILON, t_0b95f1a7). */
#define PICA_W_EPS 1e-6f
/* t_5a3c517a: the 12 attribute loaders' offset/stride/format table is derived
   from PICA registers 0x201..0x224, which cannot change inside a draw.  It used
   to be recomputed for EVERY vertex (12 format decodes + a 12x12 loader walk,
   ~0.2-0.3 us of the 2 us per-vertex budget).  Build it once per draw and hand
   the vertex loop a pointer.  DQ8_VSFAST=0 restores the per-vertex build. */
typedef struct {uint32_t off,stride,n,bytes,type;int valid;} PicaVsAttr;
static PicaVsAttr pica_vs_attr[12];
static int pica_vs_attr_ready;
static int pica_vsfast = -1;
static void pica_vs_attr_fill(PicaVsAttr *attr) {
    unsigned i, loader, j;
    memset(attr, 0, 12 * sizeof *attr);
    for (i = 0; i < 12; i++) {
        unsigned fmt = (pica_regs[i < 8 ? 0x201 : 0x202] >> (4 * (i < 8 ? i : i - 8))) & 15;
        attr[i].type = fmt & 3;
        attr[i].n = (fmt >> 2) + 1;
        attr[i].bytes = (fmt & 3) == 3 ? 4 : (fmt & 3) == 2 ? 2 : 1;
    }
    for (loader = 0; loader < 12; loader++) {
        uint32_t off = pica_regs[0x203 + 3 * loader] & 0xfffffff;
        uint32_t w1 = pica_regs[0x204 + 3 * loader];
        uint32_t w2 = pica_regs[0x205 + 3 * loader];
        unsigned local = 0;
        for (j = 0; j < (w2 >> 28) && j < 12; j++) {
            unsigned c = ((j < 8 ? w1 : w2) >> (4 * (j < 8 ? j : j - 8))) & 15;
            unsigned size;
            if (c >= 12) { local = (local + 3) & ~3u; local += (c - 11) * 4; continue; }
            size = attr[c].bytes; local = (local + size - 1) & ~(size - 1);
            attr[c].off = off + local;
            attr[c].stride = (w2 >> 16) & 255;
            attr[c].valid = 1;
            local += size * attr[c].n;
        }
    }
}
/* t_968a1fa9: build the table exactly once per draw, on ONE thread.  The pool
   used to reach this lazily from its first vertex on every participant, so
   concurrent memsets/fills of the same global array let a reader consume a
   half-written entry (offset/stride 0 => the wrong guest address => garbage
   vertex).  pvs_par_begin() calls this on the main thread before it publishes
   the job; the job_gen release store publishes the finished table. */
static void pica_vs_attr_ensure(void) {
    if (!pica_vs_attr_ready) { pica_vs_attr_fill(pica_vs_attr); pica_vs_attr_ready = 1; }
}
/* t_fdc40e6f: the output-map scatter, the finite checks and the projection
   of pica_load_vertex, split out so the 4-lane batch can finish four
   vertices with exactly this code (semantics unchanged). */
static int pica_vs_finish(PicaVsState *s, PicaVertex *v) {
    float semantic[32];unsigned k;
    for(k=0;k<32;k++)semantic[k]=1.0f;
    if (pvs_plan_now() && pvs_prog_now()) {
        /* t_fdc40e6f: the output mask/map decode is done once per draw
           (pvs_plan_scatter), so this is a copy per mapped lane.  pica_load_vertex
           reads ids 0..15 only (clip position, colour, two uv pairs, and the
           isfinite checks on 0..3), so ids >= 16 are not copied at all. */
        const PvsProg *pg = pvs_prog_now();
        for (k = 0; k < pg->nsc; ++k)
            semantic[pg->sc[k].sem] =
                *(const float *)((const char *)s + pg->sc[k].off);
    } else {
        unsigned packed=0,mask=pica_regs[0x2bd]&65535u,attribs=pica_regs[0x4f]&7u,reg;
        for(reg=0;reg<16 && packed<attribs && packed<7u;reg++) {
            if(!(mask&(1u<<reg)))continue;
            for(k=0;k<4;k++) {
                unsigned id=(pica_regs[0x50+packed]>>(8*k))&31;
                if(id<31)semantic[id]=s->output[reg][k];
            }
            packed++;
        }
    }
    if (pvs_f && pvs_trace) {
        unsigned r, k;
        for (r = 0; r < 16; r++) {
            fprintf(pvs_f, "VSOUT %u", r);
            for (k = 0; k < 4; k++) fprintf(pvs_f, " %+.9g", s->output[r][k]);
            fprintf(pvs_f, "\n");
        }
        fprintf(pvs_f, "VSSEM");
        for (k = 0; k < 8; k++) fprintf(pvs_f, " %u=%+.9g", k, semantic[k]);
        fprintf(pvs_f, "\n");
        pvs_proj_trace = 1;
        ++pvs_vert;
        pvs_trace = 0;
    }
    if(!isfinite(semantic[0])||!isfinite(semantic[1])||!isfinite(semantic[2])||!isfinite(semantic[3])){pica_vs_fail_reason=7;memcpy(&pica_vs_fail_detail,&semantic[3],4);return 0;}
    v->cx=semantic[0];v->cy=semantic[1];v->cz=semantic[2];v->cw=semantic[3];
    memcpy(v->color,semantic+8,sizeof v->color);v->u=semantic[12];v->v=semantic[13];
    v->u1=semantic[14];v->v1=semantic[15];
    if(semantic[3]<=PICA_W_EPS) {
        /* Clip-invalid position (w at/behind the eye plane): keep the
           clip-space position and let the primitive clipper cut the triangle
           at the w=EPSILON plane like the hardware (t_0b95f1a7).  The screen
           fields are derived after clipping. */
        /* t_e1773c51: this runs on every participant for every clipped vertex,
           so it was a cross-worker RMW on one global word (lost counts, and a
           cache line bouncing between 8 cores).  Relaxed add: the value is a
           diagnostic total, the ordering does not matter. */
        __atomic_add_fetch(&pica_vs_fail_totals[6], 1ull, __ATOMIC_RELAXED);
        if (pvs_f && pvs_proj_trace) {
            pvs_proj_trace = 0;
            fprintf(pvs_f, "VSPROJ clipped hw=%+.9g hh=%+.9g xoff=%u yoff=%u\n",
                    pica_f24_to_f32(pica_regs[0x41]), pica_f24_to_f32(pica_regs[0x43]),
                    pica_regs[0x68] & 1023u, (pica_regs[0x68] >> 16) & 1023u);
        }
        v->x=v->y=v->zw=v->iw=0.0f;
        return 1;
    }
    if (pica_clst() && pica_draw_log_count >= 1000u && semantic[3] > PICA_W_EPS) {
        float ndx = semantic[0] / semantic[3], ndy = semantic[1] / semantic[3];
        ++clst_nv_front;
        if (ndx < clst_ndx0) clst_ndx0 = ndx;
        if (ndx > clst_ndx1) clst_ndx1 = ndx;
        if (ndy < clst_ndy0) clst_ndy0 = ndy;
        if (ndy > clst_ndy1) clst_ndy1 = ndy;
        if (ndx >= -1.0f && ndx <= 0.0f) ++clst_nv_ground;
        if (ndx >= -1.0f && ndx <= 1.0f && ndy >= -1.0f && ndy <= 1.0f)
            ++clst_nv_infrustum;
    }
    v->iw=1/semantic[3];
    v->zw=semantic[2]*v->iw;   /* z/w: the value the depth test interpolates */
    float hw=pica_f24_to_f32(pica_regs[0x41]),hh=pica_f24_to_f32(pica_regs[0x43]);
    v->x=(semantic[0]*v->iw+1)*hw+(pica_regs[0x68]&1023);
    v->y=(semantic[1]*v->iw+1)*hh+((pica_regs[0x68]>>16)&1023);
    if (pvs_f && pvs_proj_trace) {
        pvs_proj_trace = 0;
        fprintf(pvs_f, "VSPROJ sx=%+.9g sy=%+.9g hw=%+.9g hh=%+.9g xoff=%u yoff=%u "
                       "rtw=%u rth=%u\n",
                v->x, v->y, hw, hh,
                pica_regs[0x68] & 1023u, (pica_regs[0x68] >> 16) & 1023u,
                pica_rt_w, pica_rt_h);
    }
    if(!isfinite(v->x)||!isfinite(v->y)){pica_vs_fail_reason=7;return 0;}
    /* Far off-screen magnitudes are legal (they are clipped later); the old
       >1e7 reject silently dropped whole near-plane primitives.  Clamp the
       outliers so the software edge math stays in float range. */
    if(v->x>1e6f)v->x=1e6f;else if(v->x<-1e6f)v->x=-1e6f;
    if(v->y>1e6f)v->y=1e6f;else if(v->y<-1e6f)v->y=-1e6f;
    return 1;
}

/* t_fdc40e6f: the attribute-fetch half of pica_load_vertex, split out so four
   vertices can be fetched and then executed together.  Same checks, same fail
   codes, same order as the original in-line code. */
static int pica_vs_fetch(uint32_t base, uint32_t index, PicaVsAttr *attr,
                         PicaVsState *s) {
    unsigned max = (pica_regs[0x2b9] & 15);
    if (max >= 12) { pica_vs_fail_reason = 2; pica_vs_fail_detail = max; return 0; }
    for (unsigned i = 0; i <= max; i++) {
        float value[4] = {0, 0, 0, 1};
        if (pica_regs[0x202] & (1u << (16 + i))) memcpy(value, vs_default_attrib[i], sizeof value);
        else {
            if (!attr[i].valid) { pica_vs_fail_reason = 3; pica_vs_fail_detail = i; return 0; }
            uint64_t address = (uint64_t)base + attr[i].off + (uint64_t)index * attr[i].stride;
            uint32_t mapped;
            if (address > UINT32_MAX ||
                !guest_memory_range((uint32_t)address, attr[i].bytes * attr[i].n, &mapped)) {
                pica_vs_fail_reason = 4; pica_vs_fail_detail = i; return 0;
            }
            for (unsigned k = 0; k < attr[i].n; k++) {
                const uint8_t *p = ram + mapped + k * attr[i].bytes;
                if (attr[i].type == 3) memcpy(&value[k], p, 4);
                else if (attr[i].type == 2) { int16_t n; memcpy(&n, p, 2); value[k] = n; }
                else value[k] = attr[i].type == 1 ? (float)*p : (float)(int8_t)*p;
            }
        }
        unsigned reg = (pica_regs[i < 8 ? 0x2bb : 0x2bc] >> (4 * (i < 8 ? i : i - 8))) & 15;
        memcpy(s->input[reg], value, sizeof value);
    }
    return 1;
}

static int pica_load_vertex(uint32_t base,uint32_t index,PicaVertex *v) {
    PicaVsAttr local_attr[12], *attr;
    uint64_t pf_a0 = pf_trap_armed() ? pica_pqt_now_ns() : 0;
    PicaVsState s;
    pica_vs_fail_reason=0;pica_vs_fail_detail=0;
    if (pica_vsfast < 0) {
        const char *e = getenv("DQ8_VSFAST");
        pica_vsfast = (e && e[0] == '0' && !e[1]) ? 0 : 1;
    }
    if (pica_vsfast) {
        pica_vs_attr_ensure();   /* t_968a1fa9: one builder, not five racers */
        attr = pica_vs_attr;
    } else {
        pica_vs_attr_fill(local_attr);
        attr = local_attr;
    }
    memset(&s, 0, sizeof s);
    if (!pica_vs_fetch(base, index, attr, &s)) return 0;
    pvs_trace = (pvs_f && pvs_vert < pvs_max_vert) ? 1u : 0u;
    if (pvs_f && pvs_trace) {
        unsigned i, k;
        fprintf(pvs_f, "VSVERTEX idx=%u base=%08X\n", index, base);
        fprintf(pvs_f, "VSATTRCFG");
        for (i = 0; i < 12; i++)
            fprintf(pvs_f, " %u:o=%08X,s=%u,n=%u,b=%u,t=%u,v=%u", i,
                    attr[i].off, attr[i].stride, attr[i].n, attr[i].bytes,
                    attr[i].type, attr[i].valid);
        fprintf(pvs_f, "\n");
        for (i = 0; i < 16; i++) {
            fprintf(pvs_f, "VSIN %u", i);
            for (k = 0; k < 4; k++) fprintf(pvs_f, " %+.9g", s.input[i][k]);
            fprintf(pvs_f, "\n");
        }
    }
    if (pf_a0) pf_shim(PF_SH_ATTR, pica_pqt_now_ns() - pf_a0);
    if(!pica_execute_vs(&s)){pica_vs_fail_reason=5;return 0;}
    if (pf_a0) pf_shim(PF_SH_VSEXEC, pica_pqt_now_ns() - pf_a0);
    return pica_vs_finish(&s, v);
}

/* ---- t_fdc40e6f step 2: four vertices per pass over the plan -------------
   The VS is independent per vertex, so four consecutive vertices run through
   the plan TOGETHER: one pass over the instruction list, with component c of
   the four vertices packed into one __m128 lane vector.  The flat register
   block is 48 registers (input 0..15, temp 16..31, output 32..47 -- the plan
   already addresses registers by byte offset >> 4), each with 4 component
   vectors.

   Divergence: the four lanes share one pc, so every flow instruction is
   evaluated for all four; if they disagree the batch is abandoned BEFORE
   anything is written back and the caller re-runs those four vertices on the
   scalar plan path.  The same fallback covers PVO_GEN, relative uniform
   addressing, stack overflow, a pc out of range and the instrumentation
   gates -- the batch never silently changes a result.

   DQ8_VS4=0 disables it; counters in the VSPLAN report. */
#define PVS4_NREG 48
typedef struct { __m128 c[PVS4_NREG][4]; } PvsSoa;
static inline int pvs4_cond(const int cc[4][2], unsigned lane, uint32_t w) {
    int x = cc[lane][0] == (int)((w >> 25) & 1), y = cc[lane][1] == (int)((w >> 24) & 1);
    switch ((w >> 22) & 3) { case 0: return x || y; case 1: return x && y; case 2: return x; default: return y; }
}
/* Operand fetch for four lanes.  `addr` is the plan's operand word: a byte
   offset into the vertex state (flag set) or an absolute uniform pointer.
   The swizzle is applied per component (its pshufb perm bytes are the source
   component index times 4), so no cross-lane shuffle is needed. */
static inline void pvs4_swz(const PvsSoa *o, uintptr_t addr, unsigned flag,
                            unsigned rel, int nreg, unsigned swi,
                            const int addrv[4][3], __m128 out[4]) {
    const PvsSwzVec *w = &pvs_swzv[swi];
    unsigned char pm[16];
    unsigned c, lane;
    /* Negate-SWIZZLE ("sign").  pvs_swzv[].sign holds -0.0f for a negated
       operand -- exactly the mask the scalar pvs_vec_apply() xors in -- so it
       is xored in directly.  The previous code rebuilt the mask from the float
       with a cast, and `(unsigned)-0.0f == 0`, so every negated swizzle in a
       batched program was silently dropped: a-b became |a|+|b|.  That is the
       whole 792-mismatch field defect (uv/uv1 wrong, clip position right, only
       on the programs that use a negate swizzle AND got batched). */
    _mm_storeu_si128((__m128i *)pm, w->perm);
    if (rel && nreg >= 32) {
        /* Relative uniform addressing (MOVA): the bank index is per vertex.
           Same arithmetic as pvs_vec_src -> pvs_vec_apply (unsigned wrap, the
           [-128,127] clamp and the 96-slot/ones boundary), evaluated per lane
           and transposed into component vectors -- bit-identical to the scalar
           helper by construction.  `nreg < 32` means the plan named a state
           register (input/temp), and the scalar helper ignores the rel bits for
           those (pvs_vec_src tests n<16 / n<32 before it looks at rel), so they
           are ignored here too. */
        float t[4][4];
        for (lane = 0; lane < 4; ++lane) {
            int off = addrv[lane][rel - 1];
            unsigned ix = (unsigned)(nreg - 32);
            const float *src;
            __m128 v;
            float tv[4];
            if (off < -128 || off > 127) off = 0;
            ix = (ix + (unsigned)off) & 127u;
            src = ix < 96u ? vs_uniform_f[ix] : pvs_ones_f;
            v = _mm_loadu_ps(src);
            v = _mm_xor_ps(_mm_castsi128_ps(_mm_shuffle_epi8(_mm_castps_si128(v), w->perm)),
                           w->sign);
            _mm_storeu_ps(tv, v);
            for (c = 0; c < 4; ++c) t[c][lane] = tv[c];
        }
        for (c = 0; c < 4; ++c) out[c] = _mm_loadu_ps(t[c]);
    } else if (flag) {
        const __m128 *rb = o->c[(unsigned)(addr >> 4)];
        for (c = 0; c < 4; ++c)
            out[c] = _mm_xor_ps(rb[pm[c * 4] >> 2], w->sign);
    } else {
        const float *u = (const float *)addr;
        for (c = 0; c < 4; ++c)
            out[c] = _mm_xor_ps(_mm_set1_ps(u[pm[c * 4] >> 2]), w->sign);
    }
}
static int pica_execute_vs4(PicaVsState *const st[4]) {
    struct { unsigned end, ret; } calls[16], branches[16];
    unsigned nc = 0, nb = 0, pc = pica_regs[0x2ba] & 65535, boundary = UINT32_MAX;
    const PvsPlan *plan;
    PvsSoa soa;
    PvsSoa *o = &soa;
    int cc[4][2], addr[4][3];
    unsigned lane, c, r, step;
    int ended = 0;
    plan = pvs_plan_now();
    if (!pvs_vs4_gate() || !plan || pvs_trace || pvs_ghist_on > 0 || pf_trap_on > 0) {
        ++pvs4_tls.bail_gate;
        return 0;
    }
    for (lane = 0; lane < 4; ++lane) {
        cc[lane][0] = st[lane]->cc[0];
        cc[lane][1] = st[lane]->cc[1];
        addr[lane][0] = st[lane]->address[0];
        addr[lane][1] = st[lane]->address[1];
        addr[lane][2] = st[lane]->address[2];
    }
    for (r = 0; r < 16; ++r)
        for (c = 0; c < 4; ++c)
            o->c[r][c] = _mm_set_ps(st[3]->input[r][c], st[2]->input[r][c],
                                    st[1]->input[r][c], st[0]->input[r][c]);
    for (r = 16; r < PVS4_NREG; ++r)
        for (c = 0; c < 4; ++c) o->c[r][c] = _mm_setzero_ps();
    for (step = 0; step < 16384; step++) {
        unsigned call_boundary = boundary;
        for (unsigned pops = 0; pops < 4 && nc && call_boundary == calls[nc-1].end; pops++) {
            --nc;
            if (pops < 3) pc = call_boundary = calls[nc].ret;
        }
        if (nb && boundary == branches[nb-1].end) pc = branches[--nb].ret;
        if (pc >= vs_program_size) { ++pvs4_tls.bail_pc; return 0; }
        boundary = pc + 1;
        {
            const PvsPlan *p = &plan[pc];
            unsigned o4 = p->op;
            if (!o4) { ++pvs4_tls.bail_gen; return 0; }
            if ((p->rel_a || p->rel_b || p->rel_c) && pvs4_norel_gate()) { ++pvs4_tls.bail_rel; return 0; }
            if (pvs4_bailmask_gate() && (pvs4_bailmask_gate() & pvs4_bailcls(o4))) {
                ++pvs4_tls.bail_rel;
                return 0;
            }
            if (o4 >= PVO_END) {
                uint32_t w = vs_program[pc];
                unsigned nx = pc + 1;
                if (o4 == PVO_END) { ended = 1; break; }
                if (o4 == PVO_JMP) { pc = nx; continue; }
                if (o4 <= PVO_JMPZ) {
                    unsigned target = p->tgt, nn = p->n;
                    int yes[4], y0;
                    if (o4 == PVO_CALL) { yes[0]=yes[1]=yes[2]=yes[3]=1; }
                    else if (o4 == PVO_CALLZ || o4 == PVO_JMPZ)
                        for (lane = 0; lane < 4; ++lane) yes[lane] = pvs4_cond(cc, lane, w);
                    else { int u = (int)((pica_regs[0x2b0] >> p->idx) & 1);
                           yes[0]=yes[1]=yes[2]=yes[3]=u; }
                    y0 = yes[0];
                    if (yes[1] != y0 || yes[2] != y0 || yes[3] != y0) { ++pvs4_tls.bail_div; return 0; }
                    if (o4 <= PVO_CALLNZ) {
                        if (y0) {
                            if (nc == 16) { ++pvs4_tls.bail_plan; return 0; }
                            calls[nc].end = target + nn;
                            calls[nc++].ret = nx;
                            nx = target;
                        }
                    } else if (y0) {
                        if (nb == 16) { ++pvs4_tls.bail_plan; return 0; }
                        branches[nb].end = target;
                        branches[nb++].ret = target + nn;
                    } else nx = target;
                    pc = nx;
                    continue;
                }
                {   /* 0x2C / 0x2D */
                    int yes[4], y0;
                    if (o4 == PVO_IFU)
                        for (lane = 0; lane < 4; ++lane) yes[lane] = pvs4_cond(cc, lane, w);
                    else { int u = (int)(((pica_regs[0x2b0] >> p->idx) & 1) == !(w & 1));
                           yes[0]=yes[1]=yes[2]=yes[3]=u; }
                    y0 = yes[0];
                    if (yes[1] != y0 || yes[2] != y0 || yes[3] != y0) { ++pvs4_tls.bail_div; return 0; }
                    pc = y0 ? p->tgt : nx;
                    continue;
                }
            }
            {
                unsigned fa = p->flags & 1u, fb = (p->flags >> 1) & 1u, fc = (p->flags >> 2) & 1u;
                __m128 va[4], vb[4], vr[4], vc[4];
                pvs4_swz(o, p->a, fa, p->rel_a, p->na, p->sw_a, addr, va);
                if (o4 == PVO_MOVA) {
                    unsigned k2;
                    for (k2 = 0; k2 < 2; ++k2) {
                        float al[4];
                        if (!(p->msk & (8 >> k2))) continue;
                        _mm_storeu_ps(al, va[k2]);
                        for (lane = 0; lane < 4; ++lane)
                            if (isfinite(al[lane]) && al[lane] >= -2147483648.0f &&
                                al[lane] < 2147483648.0f)
                                addr[lane][k2] = (int)al[lane];
                    }
                    pc = pc + 1u;
                    continue;
                }
                pvs4_swz(o, p->b, fb, p->rel_b, p->nb, p->sw_b, addr, vb);
                if (o4 == PVO_CMP) {
                    float al[4], bl[4];
                    _mm_storeu_ps(al, va[0]); _mm_storeu_ps(bl, vb[0]);
                    for (lane = 0; lane < 4; ++lane)
                        cc[lane][0] = pica_vs_compare(al[lane], bl[lane], p->cmp0);
                    _mm_storeu_ps(al, va[1]); _mm_storeu_ps(bl, vb[1]);
                    for (lane = 0; lane < 4; ++lane)
                        cc[lane][1] = pica_vs_compare(al[lane], bl[lane], p->cmp1);
                    pc = pc + 1u;
                    continue;
                }
                switch (o4) {
                case PVO_MOV: for (c = 0; c < 4; ++c) vr[c] = va[c]; break;
                case PVO_ADD: for (c = 0; c < 4; ++c) vr[c] = _mm_add_ps(va[c], vb[c]); break;
                case PVO_MUL: for (c = 0; c < 4; ++c) vr[c] = pvs_vec_mul(va[c], vb[c]); break;
                case PVO_FLR: for (c = 0; c < 4; ++c) vr[c] = _mm_floor_ps(va[c]); break;
                case PVO_RCP: {
                    float al[4];
                    _mm_storeu_ps(al, va[0]);
                    vr[0] = vr[1] = vr[2] = vr[3] =
                        _mm_set_ps(1.0f / al[3], 1.0f / al[2], 1.0f / al[1], 1.0f / al[0]);
                    break;
                }
                case PVO_RSQ: {
                    float al[4];
                    _mm_storeu_ps(al, va[0]);
                    vr[0] = vr[1] = vr[2] = vr[3] =
                        _mm_set_ps(1.0f / sqrtf(al[3]), 1.0f / sqrtf(al[2]),
                                   1.0f / sqrtf(al[1]), 1.0f / sqrtf(al[0]));
                    break;
                }
                case PVO_MIN:
                case PVO_MAX: {
                    float al[4], bl[4];
                    for (c = 0; c < 4; ++c) {
                        _mm_storeu_ps(al, va[c]); _mm_storeu_ps(bl, vb[c]);
                        for (lane = 0; lane < 4; ++lane)
                            al[lane] = (o4 == PVO_MIN) ? fminf(al[lane], bl[lane])
                                                       : fmaxf(al[lane], bl[lane]);
                        vr[c] = _mm_loadu_ps(al);
                    }
                    break;
                }
                case PVO_SLT:
                case PVO_SGE: {
                    for (c = 0; c < 4; ++c) {
                        __m128 cmp = (o4 == PVO_SLT) ? _mm_cmplt_ps(va[c], vb[c])
                                                     : _mm_cmpge_ps(va[c], vb[c]);
                        vr[c] = _mm_and_ps(cmp, _mm_set1_ps(1.0f));
                    }
                    break;
                }
                case PVO_MAD: {
                    pvs4_swz(o, p->c, fc, p->rel_c, p->nc, p->sw_c, addr, vc);
                    for (c = 0; c < 4; ++c) vr[c] = _mm_add_ps(pvs_vec_mul(va[c], vb[c]), vc[c]);
                    break;
                }
                default: {   /* PVO_DP: 0 + p0 + p1 + p2 [+ p3] [+ b3], per lane */
                    __m128 pv[4], dot;
                    for (c = 0; c < 4; ++c) pv[c] = pvs_vec_mul(va[c], vb[c]);
                    dot = _mm_add_ps(_mm_setzero_ps(), pv[0]);
                    dot = _mm_add_ps(dot, pv[1]);
                    dot = _mm_add_ps(dot, pv[2]);
                    if (p->dpn == 4) dot = _mm_add_ps(dot, pv[3]);
                    if (p->dph) dot = _mm_add_ps(dot, vb[3]);
                    vr[0] = vr[1] = vr[2] = vr[3] = dot;
                    break;
                }
                }
                {
                    unsigned dreg = (unsigned)(p->dst >> 4);
                    for (c = 0; c < 4; ++c)
                        if (p->msk & (8u >> c)) o->c[dreg][c] = vr[c];
                }
                pc = pc + 1u;
                continue;
            }
        }
    }
    if (!ended) { ++pvs4_tls.bail_plan; return 0; }
    /* END reached with all four lanes on the same path: publish the output
       registers and the per-vertex cc/address state. */
    for (c = 0; c < 4; ++c) {
        float t[4];
        for (r = 32; r < PVS4_NREG; ++r) {
            _mm_storeu_ps(t, o->c[r][c]);
            for (lane = 0; lane < 4; ++lane) st[lane]->output[r - 32][c] = t[lane];
        }
    }
    for (lane = 0; lane < 4; ++lane) {
        st[lane]->cc[0] = cc[lane][0];
        st[lane]->cc[1] = cc[lane][1];
        st[lane]->address[0] = addr[lane][0];
        st[lane]->address[1] = addr[lane][1];
        st[lane]->address[2] = addr[lane][2];
    }
    ++pvs4_tls.batches;
    pvs4_tls.verts += 4;
    return 1;
}

/* ---- t_e1773c51 STEP 2: eight vertices per pass over the plan (AVX2) -----
   The 4-lane batch (t_fdc40e6f) runs four vertices through one pass over the
   prepared plan with one __m128 per component.  This is the same executor
   widened to eight lanes with one __m256 per component: same plan, same
   operand form (state byte offset / absolute uniform pointer + swizzle index
   + destination offset + write mask), same fallback contract.

   Divergence: the eight lanes share one pc, so every flow instruction is
   evaluated for all eight; if they disagree the batch is abandoned BEFORE
   anything is written back and the caller re-runs those vertices on the
   4-lane path (which itself falls back to the scalar reference).  PVO_GEN,
   relative addressing, stack overflow, pc out of range and the
   instrumentation gates bail the same way, so the batch can never change a
   result -- DQ8_VSPAR_VERIFY proves that per vertex.

   DQ8_VS8=0 disables it; enabled only when the CPU reports AVX2.  The bail
   counters are reported next to the 4-lane ones (VS8 line, VSPLAN_STATS). */
typedef struct { __m256 c[PVS4_NREG][4]; } PvsSoa8;
static int pvs_vs8_on = -1;
static int pvs_vs8_gate(void) {
    if (pvs_vs8_on < 0) {
        const char *e = getenv("DQ8_VS8");
        int ok = __builtin_cpu_supports("avx2") ? 1 : 0;
        pvs_vs8_on = (e && e[0] == '0' && !e[1]) ? 0 : ok;
        if (pvs_vs8_on) pvs4_cnt_register();
    }
    return pvs_vs8_on;
}
__attribute__((target("avx2")))
static void pvs8_swz(const PvsSoa8 *o, uintptr_t addr, unsigned flag,
                     unsigned rel, int nreg, unsigned swi,
                     const int addrv[8][3], __m256 out[4]) {
    const PvsSwzVec *w = &pvs_swzv[swi];
    unsigned char pm[16];
    __m128 sgn = _mm_loadu_ps((const float *)&w->sign);
    __m256 sign = _mm256_insertf128_ps(_mm256_castps128_ps256(sgn), sgn, 1);
    unsigned c, lane;
    _mm_storeu_si128((__m128i *)pm, w->perm);
    if (rel && nreg >= 32) {
        /* Relative uniform addressing (MOVA): the bank index is per vertex,
           so this is the scalar helper's arithmetic evaluated per lane and
           transposed into component vectors -- bit-identical by construction
           (same clamp, same 96-slot/ones boundary). */
        float t[4][8];
        for (lane = 0; lane < 8; ++lane) {
            int off = addrv[lane][rel - 1];
            unsigned ix = (unsigned)(nreg - 32);
            const float *src;
            __m128 v;
            float tv[4];
            if (off < -128 || off > 127) off = 0;
            ix = (ix + (unsigned)off) & 127u;
            src = ix < 96u ? vs_uniform_f[ix] : pvs_ones_f;
            v = _mm_loadu_ps(src);
            v = _mm_xor_ps(_mm_castsi128_ps(_mm_shuffle_epi8(_mm_castps_si128(v), w->perm)),
                           w->sign);
            _mm_storeu_ps(tv, v);
            for (c = 0; c < 4; ++c) t[c][lane] = tv[c];
        }
        for (c = 0; c < 4; ++c) out[c] = _mm256_loadu_ps(t[c]);
    } else if (flag) {
        const __m256 *rb = o->c[(unsigned)(addr >> 4)];
        for (c = 0; c < 4; ++c)
            out[c] = _mm256_xor_ps(rb[pm[c * 4] >> 2], sign);
    } else {
        const float *u = (const float *)addr;
        for (c = 0; c < 4; ++c)
            out[c] = _mm256_xor_ps(_mm256_set1_ps(u[pm[c * 4] >> 2]), sign);
    }
}
__attribute__((target("avx2")))
static inline __m256 pvs8_mul(__m256 a, __m256 b) {
    /* pvs_vec_mul for eight lanes: 0 * anything is a signed zero, exactly like
       the scalar helper (the sign of the zero operand is preserved). */
    __m256 zero = _mm256_setzero_ps();
    __m256 r = _mm256_mul_ps(a, b);
    __m256 z = _mm256_or_ps(_mm256_cmp_ps(a, zero, _CMP_EQ_OQ),
                            _mm256_cmp_ps(b, zero, _CMP_EQ_OQ));
    __m256 s = _mm256_and_ps(r, _mm256_set1_ps(-0.0f));
    return _mm256_or_ps(_mm256_andnot_ps(z, r), _mm256_and_ps(z, s));
}
static inline int pvs8_cond(const int cc[8][2], unsigned lane, uint32_t w) {
    int x = cc[lane][0] == (int)((w >> 25) & 1), y = cc[lane][1] == (int)((w >> 24) & 1);
    switch ((w >> 22) & 3) { case 0: return x || y; case 1: return x && y; case 2: return x; default: return y; }
}
__attribute__((target("avx2")))
static int pica_execute_vs8(PicaVsState *const st[8]) {
    struct { unsigned end, ret; } calls[16], branches[16];
    unsigned nc = 0, nb = 0, pc = pica_regs[0x2ba] & 65535, boundary = UINT32_MAX;
    const PvsPlan *plan;
    PvsSoa8 soa;
    PvsSoa8 *o = &soa;
    int cc[8][2], addr[8][3];
    unsigned lane, c, r, step;
    int ended = 0;
    plan = pvs_plan_now();
    if (!pvs_vs8_gate() || !pvs_vs4_gate() || !plan || pvs_trace ||
        pvs_ghist_on > 0 || pf_trap_on > 0) {
        ++pvs4_tls.b8_gate;
        return 0;
    }
    for (lane = 0; lane < 8; ++lane) {
        cc[lane][0] = st[lane]->cc[0];
        cc[lane][1] = st[lane]->cc[1];
        addr[lane][0] = st[lane]->address[0];
        addr[lane][1] = st[lane]->address[1];
        addr[lane][2] = st[lane]->address[2];
    }
    for (r = 0; r < 16; ++r)
        for (c = 0; c < 4; ++c)
            o->c[r][c] = _mm256_set_ps(st[7]->input[r][c], st[6]->input[r][c],
                                       st[5]->input[r][c], st[4]->input[r][c],
                                       st[3]->input[r][c], st[2]->input[r][c],
                                       st[1]->input[r][c], st[0]->input[r][c]);
    for (r = 16; r < PVS4_NREG; ++r)
        for (c = 0; c < 4; ++c) o->c[r][c] = _mm256_setzero_ps();
    for (step = 0; step < 16384; step++) {
        unsigned call_boundary = boundary;
        for (unsigned pops = 0; pops < 4 && nc && call_boundary == calls[nc-1].end; pops++) {
            --nc;
            if (pops < 3) pc = call_boundary = calls[nc].ret;
        }
        if (nb && boundary == branches[nb-1].end) pc = branches[--nb].ret;
        if (pc >= vs_program_size) { ++pvs4_tls.b8_pc; return 0; }
        boundary = pc + 1;
        {
            const PvsPlan *p = &plan[pc];
            unsigned o8 = p->op;
            if (!o8) { ++pvs4_tls.b8_gen; return 0; }
            if ((p->rel_a || p->rel_b || p->rel_c) && pvs4_norel_gate()) { ++pvs4_tls.b8_rel; return 0; }
            if (pvs4_bailmask_gate() && (pvs4_bailmask_gate() & pvs4_bailcls(o8))) {
                ++pvs4_tls.b8_rel;
                return 0;
            }
            if (o8 >= PVO_END) {
                uint32_t w = vs_program[pc];
                unsigned nx = pc + 1;
                if (o8 == PVO_END) { ended = 1; break; }
                if (o8 == PVO_JMP) { pc = nx; continue; }
                if (o8 <= PVO_JMPZ) {
                    unsigned target = p->tgt, nn = p->n;
                    int yes[8], y0;
                    if (o8 == PVO_CALL) { for (lane = 0; lane < 8; ++lane) yes[lane] = 1; }
                    else if (o8 == PVO_CALLZ || o8 == PVO_JMPZ)
                        for (lane = 0; lane < 8; ++lane) yes[lane] = pvs8_cond(cc, lane, w);
                    else { int u = (int)((pica_regs[0x2b0] >> p->idx) & 1);
                           for (lane = 0; lane < 8; ++lane) yes[lane] = u; }
                    y0 = yes[0];
                    for (lane = 1; lane < 8; ++lane)
                        if (yes[lane] != y0) { ++pvs4_tls.b8_div; return 0; }
                    if (o8 <= PVO_CALLNZ) {
                        if (y0) {
                            if (nc == 16) { ++pvs4_tls.b8_plan; return 0; }
                            calls[nc].end = target + nn;
                            calls[nc++].ret = nx;
                            nx = target;
                        }
                    } else if (y0) {
                        if (nb == 16) { ++pvs4_tls.b8_plan; return 0; }
                        branches[nb].end = target;
                        branches[nb++].ret = target + nn;
                    } else nx = target;
                    pc = nx;
                    continue;
                }
                {   /* 0x2C / 0x2D */
                    int yes[8], y0;
                    if (o8 == PVO_IFU)
                        for (lane = 0; lane < 8; ++lane) yes[lane] = pvs8_cond(cc, lane, w);
                    else { int u = (int)(((pica_regs[0x2b0] >> p->idx) & 1) == !(w & 1));
                           for (lane = 0; lane < 8; ++lane) yes[lane] = u; }
                    y0 = yes[0];
                    for (lane = 1; lane < 8; ++lane)
                        if (yes[lane] != y0) { ++pvs4_tls.b8_div; return 0; }
                    pc = y0 ? p->tgt : nx;
                    continue;
                }
            }
            {
                unsigned fa = p->flags & 1u, fb = (p->flags >> 1) & 1u, fc = (p->flags >> 2) & 1u;
                __m256 va[4], vb[4], vr[4], vc[4];
                pvs8_swz(o, p->a, fa, p->rel_a, p->na, p->sw_a, addr, va);
                if (o8 == PVO_MOVA) {
                    unsigned k2;
                    for (k2 = 0; k2 < 2; ++k2) {
                        float al[8];
                        if (!(p->msk & (8 >> k2))) continue;
                        _mm256_storeu_ps(al, va[k2]);
                        for (lane = 0; lane < 8; ++lane)
                            if (isfinite(al[lane]) && al[lane] >= -2147483648.0f &&
                                al[lane] < 2147483648.0f)
                                addr[lane][k2] = (int)al[lane];
                    }
                    pc = pc + 1u;
                    continue;
                }
                pvs8_swz(o, p->b, fb, p->rel_b, p->nb, p->sw_b, addr, vb);
                if (o8 == PVO_CMP) {
                    float al[8], bl[8];
                    _mm256_storeu_ps(al, va[0]); _mm256_storeu_ps(bl, vb[0]);
                    for (lane = 0; lane < 8; ++lane)
                        cc[lane][0] = pica_vs_compare(al[lane], bl[lane], p->cmp0);
                    _mm256_storeu_ps(al, va[1]); _mm256_storeu_ps(bl, vb[1]);
                    for (lane = 0; lane < 8; ++lane)
                        cc[lane][1] = pica_vs_compare(al[lane], bl[lane], p->cmp1);
                    pc = pc + 1u;
                    continue;
                }
                switch (o8) {
                case PVO_MOV: for (c = 0; c < 4; ++c) vr[c] = va[c]; break;
                case PVO_ADD: for (c = 0; c < 4; ++c) vr[c] = _mm256_add_ps(va[c], vb[c]); break;
                case PVO_MUL: for (c = 0; c < 4; ++c) vr[c] = pvs8_mul(va[c], vb[c]); break;
                case PVO_FLR: for (c = 0; c < 4; ++c) vr[c] = _mm256_floor_ps(va[c]); break;
                case PVO_RCP: {
                    float al[8];
                    _mm256_storeu_ps(al, va[0]);
                    vr[0] = vr[1] = vr[2] = vr[3] =
                        _mm256_set_ps(1.0f / al[7], 1.0f / al[6], 1.0f / al[5], 1.0f / al[4],
                                      1.0f / al[3], 1.0f / al[2], 1.0f / al[1], 1.0f / al[0]);
                    break;
                }
                case PVO_RSQ: {
                    float al[8];
                    _mm256_storeu_ps(al, va[0]);
                    vr[0] = vr[1] = vr[2] = vr[3] =
                        _mm256_set_ps(1.0f / sqrtf(al[7]), 1.0f / sqrtf(al[6]),
                                      1.0f / sqrtf(al[5]), 1.0f / sqrtf(al[4]),
                                      1.0f / sqrtf(al[3]), 1.0f / sqrtf(al[2]),
                                      1.0f / sqrtf(al[1]), 1.0f / sqrtf(al[0]));
                    break;
                }
                case PVO_MIN:
                case PVO_MAX: {
                    float al[8], bl[8];
                    for (c = 0; c < 4; ++c) {
                        _mm256_storeu_ps(al, va[c]); _mm256_storeu_ps(bl, vb[c]);
                        for (lane = 0; lane < 8; ++lane)
                            al[lane] = (o8 == PVO_MIN) ? fminf(al[lane], bl[lane])
                                                       : fmaxf(al[lane], bl[lane]);
                        vr[c] = _mm256_loadu_ps(al);
                    }
                    break;
                }
                case PVO_SLT:
                case PVO_SGE: {
                    for (c = 0; c < 4; ++c) {
                        __m256 cmp = (o8 == PVO_SLT)
                            ? _mm256_cmp_ps(va[c], vb[c], _CMP_LT_OQ)
                            : _mm256_cmp_ps(va[c], vb[c], _CMP_GE_OQ);
                        vr[c] = _mm256_and_ps(cmp, _mm256_set1_ps(1.0f));
                    }
                    break;
                }
                case PVO_MAD: {
                    pvs8_swz(o, p->c, fc, p->rel_c, p->nc, p->sw_c, addr, vc);
                    for (c = 0; c < 4; ++c) vr[c] = _mm256_add_ps(pvs8_mul(va[c], vb[c]), vc[c]);
                    break;
                }
                default: {   /* PVO_DP: 0 + p0 + p1 + p2 [+ p3] [+ b3], per lane */
                    __m256 pv[4], dot;
                    for (c = 0; c < 4; ++c) pv[c] = pvs8_mul(va[c], vb[c]);
                    dot = _mm256_add_ps(_mm256_setzero_ps(), pv[0]);
                    dot = _mm256_add_ps(dot, pv[1]);
                    dot = _mm256_add_ps(dot, pv[2]);
                    if (p->dpn == 4) dot = _mm256_add_ps(dot, pv[3]);
                    if (p->dph) dot = _mm256_add_ps(dot, vb[3]);
                    vr[0] = vr[1] = vr[2] = vr[3] = dot;
                    break;
                }
                }
                {
                    unsigned dreg = (unsigned)(p->dst >> 4);
                    for (c = 0; c < 4; ++c)
                        if (p->msk & (8u >> c)) o->c[dreg][c] = vr[c];
                }
                pc = pc + 1u;
                continue;
            }
        }
    }
    if (!ended) { ++pvs4_tls.b8_plan; return 0; }
    for (c = 0; c < 4; ++c) {
        float t[8];
        for (r = 32; r < PVS4_NREG; ++r) {
            _mm256_storeu_ps(t, o->c[r][c]);
            for (lane = 0; lane < 8; ++lane) st[lane]->output[r - 32][c] = t[lane];
        }
    }
    for (lane = 0; lane < 8; ++lane) {
        st[lane]->cc[0] = cc[lane][0];
        st[lane]->cc[1] = cc[lane][1];
        st[lane]->address[0] = addr[lane][0];
        st[lane]->address[1] = addr[lane][1];
        st[lane]->address[2] = addr[lane][2];
    }
    ++pvs4_tls.b8_batches;
    pvs4_tls.b8_verts += 8;
    return 1;
}
/* Eight vertices with the same draw state, eight lanes at a time.  Returns -1
   when the batch declined (the caller then runs them on the 4-lane path, which
   falls back to the scalar reference), else a bitmask of the vertices that
   finished (bit j clear = v[j] untouched, reason[j]/detail[j] set exactly as
   the scalar path would have). */
__attribute__((target("avx2")))
static int pica_load_vertex8(uint32_t base, const uint32_t index[8],
                             PicaVertex *const v[8], uint8_t reason[8],
                             uint32_t detail[8]) {
    PicaVsAttr local_attr[12], *attr;
    PicaVsState st[8];
    PicaVsState *sp[8];
    uint64_t pf_a0 = pf_trap_armed() ? pica_pqt_now_ns() : 0;
    unsigned j, mask = 0;
    if (pica_vsfast < 0) {
        const char *e = getenv("DQ8_VSFAST");
        pica_vsfast = (e && e[0] == '0' && !e[1]) ? 0 : 1;
    }
    if (pica_vsfast) { pica_vs_attr_ensure(); attr = pica_vs_attr; }
    else { pica_vs_attr_fill(local_attr); attr = local_attr; }
    for (j = 0; j < 8; ++j) {
        pica_vs_fail_reason = 0; pica_vs_fail_detail = 0;
        memset(&st[j], 0, sizeof st[j]);
        if (!pica_vs_fetch(base, index[j], attr, &st[j])) return -1;
        sp[j] = &st[j];
    }
    if (pf_a0) pf_shim(PF_SH_ATTR, pica_pqt_now_ns() - pf_a0);
    if (!pica_execute_vs8(sp)) return -1;
    if (pf_a0) pf_shim(PF_SH_VSEXEC, pica_pqt_now_ns() - pf_a0);
    for (j = 0; j < 8; ++j) {
        pica_vs_fail_reason = 0; pica_vs_fail_detail = 0;
        if (pica_vs_finish(&st[j], v[j])) { mask |= 1u << j; reason[j] = 0; detail[j] = 0; }
        else { reason[j] = (uint8_t)pica_vs_fail_reason; detail[j] = (uint32_t)pica_vs_fail_detail; }
    }
    return (int)mask;
}

/* t_fdc40e6f step 2: four vertices with the same draw state, executed four
   lanes at a time.  Returns -1 when the batch declined (the caller then runs
   the four vertices through the scalar path, so every failure and every
   unrepresentable instruction still ends up on the reference code), else a
   bitmask of the vertices that finished (bit j clear = v[j] untouched, with
   reason[j]/detail[j] set exactly as the scalar path would have). */
static int pica_load_vertex4(uint32_t base, const uint32_t index[4],
                             PicaVertex *const v[4], uint8_t reason[4],
                             uint32_t detail[4]) {
    PicaVsAttr local_attr[12], *attr;
    PicaVsState st[4];
    PicaVsState *sp[4];
    uint64_t pf_a0 = pf_trap_armed() ? pica_pqt_now_ns() : 0;
    unsigned j, mask = 0;
    if (pica_vsfast < 0) {
        const char *e = getenv("DQ8_VSFAST");
        pica_vsfast = (e && e[0] == '0' && !e[1]) ? 0 : 1;
    }
    if (pica_vsfast) { pica_vs_attr_ensure(); attr = pica_vs_attr; }
    else { pica_vs_attr_fill(local_attr); attr = local_attr; }
    for (j = 0; j < 4; ++j) {
        pica_vs_fail_reason = 0; pica_vs_fail_detail = 0;
        memset(&st[j], 0, sizeof st[j]);
        if (!pica_vs_fetch(base, index[j], attr, &st[j])) return -1;
        sp[j] = &st[j];
    }
    if (pf_a0) pf_shim(PF_SH_ATTR, pica_pqt_now_ns() - pf_a0);
    if (!pica_execute_vs4(sp)) return -1;
    if (pf_a0) pf_shim(PF_SH_VSEXEC, pica_pqt_now_ns() - pf_a0);
    for (j = 0; j < 4; ++j) {
        pica_vs_fail_reason = 0; pica_vs_fail_detail = 0;
        if (pica_vs_finish(&st[j], v[j])) { mask |= 1u << j; reason[j] = 0; detail[j] = 0; }
        else { reason[j] = (uint8_t)pica_vs_fail_reason; detail[j] = pica_vs_fail_detail; }
    }
    return (int)mask;
}

/* ---- OpenGL backend glue (see src/pica_gl.c). Buffers the draw's decoded
   triangles, then rasterizes them on the GPU when the draw state is
   expressible there; otherwise the software rasterizer runs unchanged. ---- */
static PicaGlState pica_gl_st;
static int pica_gl_draw_prepared;
/* t_77ac52ce: WHY a draw left the GL path (DQ8_TRACE_SWFALL=1).  The GL
   backend is all-or-nothing per draw, so a single unsupported state bit sends
   the whole (possibly full-screen) draw to the software rasterizer.  These
   counters name the refusing condition instead of guessing. */
enum {
    SWF_GL_INACTIVE = 1, SWF_DEPTH_UNMAPPABLE, SWF_TEX_STAGE_FAIL,
    SWF_MODE_UNSUPPORTED, SWF_NOT_TEV, SWF_SOLID_NONCONST, SWF_UV_OUTSIDE,
    SWF_TEV_TEX1, SWF_GL_DRAW_FAIL, SWF_REASONS
};
static const char *const pica_swfall_name[SWF_REASONS] = {
    "?", "gl-inactive", "depth-unmappable", "tex-stage-fail",
    "mode-unsupported", "mode-not-tev", "solid-nonconst-color", "uv-outside-0..1",
    "tev-reads-texunit1", "gl-draw-fail"
};
static unsigned long long pica_swfall_count[SWF_REASONS];
static unsigned long long pica_swfall_tris[SWF_REASONS];
static int pica_swfall_on = -1;
static void pica_swfall_report(void);
static void pica_swfall(unsigned reason, unsigned nverts) {
    if (reason <= 0 || reason >= SWF_REASONS) return;
    ++pica_swfall_count[reason];
    pica_swfall_tris[reason] += nverts / 3u;
    if (pica_swfall_on < 0) {
        const char *e = getenv("DQ8_TRACE_SWFALL");
        pica_swfall_on = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    if (!pica_swfall_on) return;
    if (pica_swfall_count[reason] <= 6u ||
        (pica_swfall_count[reason] % 200u) == 0u) {
        fprintf(stderr, "SWFALL %s tris=%u n=%llu rt=%ux%u mode=%d tex=%dx%d "
                "blend=%d depth=%d\n", pica_swfall_name[reason], nverts / 3u,
                pica_swfall_count[reason], pica_gl_st.rt_w, pica_gl_st.rt_h,
                (int)pica_gl_st.mode, pica_gl_st.tex_w, pica_gl_st.tex_h,
                (int)pica_gl_st.blend, (int)pica_gl_st.depth_on);
        if ((pica_swfall_count[reason] % 200u) == 0u) pica_swfall_report();
    }
}
static void pica_swfall_report(void) {
    unsigned i;
    if (pica_swfall_on <= 0) return;
    fprintf(stderr, "SWFALL totals:");
    for (i = 1; i < SWF_REASONS; ++i)
        if (pica_swfall_count[i])
            fprintf(stderr, " %s=%llu/%llutris", pica_swfall_name[i],
                    pica_swfall_count[i], pica_swfall_tris[i]);
    fprintf(stderr, "\n");
}
static PicaVertex *pica_tri_buf;
static uint32_t pica_tri_verts, pica_tri_cap;
static uint64_t pica_gl_draws_ok, pica_gl_draws_sw;
/* Per-mode triangle totals for the FRAMESTAT redundancy instrumentation
   (t_86b8f1f9); cumulative, read by the scan-out frame-stats hook. */
static unsigned long long pica_tris_gl_total, pica_tris_sw_total;

static int pica_gl_rt_get(void *user, int x, int y, unsigned char rgba[4]) {
    uint8_t r, g, b, a;
    (void)user;
    if (!pica_get_pixel(pica_rt_addr, pica_rt_w, pica_rt_h, pica_rt_format,
                        (uint32_t)x, (uint32_t)(pica_rt_h - 1u - (uint32_t)y),
                        &r, &g, &b, &a))
        return 0;
    rgba[0] = r; rgba[1] = g; rgba[2] = b; rgba[3] = a;
    return 1;
}
static void pica_gl_rt_put(void *user, int x, int y, const unsigned char rgba[4]) {
    (void)user;
    pica_put_pixel(x, (int)pica_rt_h - 1 - y, rgba[0], rgba[1], rgba[2], rgba[3]);
}
static int pica_gl_rt_raw(void *user, const unsigned char **base, unsigned *len) {
    uint32_t mapped;
    (void)user;
    if (!guest_memory_range(pica_rt_addr, pica_rt_w * pica_rt_h * 4u, &mapped))
        return 0;
    *base = ram + mapped;
    *len = pica_rt_w * pica_rt_h * 4u;
    return 1;
}
static unsigned pica_gl_rt_px_off(void *user, int x, int y) {
    (void)user;
    /* raster y is bottom-up here; the guest color buffer is top-down */
    return pica_rt_off(x, (int)pica_rt_h - 1 - y);
}
/* ---- depth buffer access (same y convention as rt_*) ---- */
static int pica_gl_depth_get(void *user, int x, int y, unsigned *z) {
    uint32_t mapped;
    int my = (int)pica_rt_h - 1 - y;
    (void)user;
    if (!pica_depth_addr || x < 0 || my < 0 ||
        (uint32_t)x >= pica_rt_w || (uint32_t)my >= pica_rt_h)
        return 0;
    if (!guest_memory_range(pica_depth_addr,
                            (uint64_t)pica_rt_w * pica_rt_h * pica_depth_bpp,
                            &mapped))
        return 0;
    const uint8_t *p = ram + mapped + pica_depth_off(x, my);
    *z = p[0] | ((unsigned)p[1] << 8) |
         (pica_depth_bpp >= 3u ? ((unsigned)p[2] << 16) : 0u);
    return 1;
}
static void pica_gl_depth_put(void *user, int x, int y, unsigned z) {
    uint32_t mapped;
    int my = (int)pica_rt_h - 1 - y;
    (void)user;
    if (!pica_depth_addr || x < 0 || my < 0 ||
        (uint32_t)x >= pica_rt_w || (uint32_t)my >= pica_rt_h)
        return;
    if (!guest_memory_range(pica_depth_addr,
                            (uint64_t)pica_rt_w * pica_rt_h * pica_depth_bpp,
                            &mapped))
        return;
    uint8_t *p = ram + mapped + pica_depth_off(x, my);
    p[0] = (uint8_t)(z & 0xFFu);
    p[1] = (uint8_t)((z >> 8) & 0xFFu);
    if (pica_depth_bpp >= 3u) p[2] = (uint8_t)((z >> 16) & 0xFFu);
}
static int pica_gl_depth_raw(void *user, const unsigned char **base,
                             unsigned *len) {
    uint32_t mapped;
    (void)user;
    if (!pica_depth_addr) return 0;
    if (!guest_memory_range(pica_depth_addr,
                            (uint64_t)pica_rt_w * pica_rt_h * pica_depth_bpp,
                            &mapped))
        return 0;
    *base = ram + mapped;
    *len = pica_rt_w * pica_rt_h * pica_depth_bpp;
    return 1;
}
static unsigned pica_gl_depth_px_off(void *user, int x, int y) {
    (void)user;
    return pica_depth_off(x, (int)pica_rt_h - 1 - y);
}
static const PicaGlHost pica_gl_host = {
    pica_gl_rt_get, pica_gl_rt_put, NULL, pica_gl_rt_raw, pica_gl_rt_px_off,
    pica_gl_depth_get, pica_gl_depth_put, pica_gl_depth_raw,
    pica_gl_depth_px_off
};

/* Batched-readback episode identity.  A deferred write-back must land in the
   render target the FBO content belongs to, even when the guest switched
   render targets since the draw, and pica_put_pixel/pica_get_pixel resolve
   the target through the pica_rt_* globals.  pica_gl_flush_guest_rt
   therefore points those globals at the episode's target around the flush
   (the whole GSP/draw path is single-threaded, so the swap is safe). */
static uint32_t pica_gl_ep_addr, pica_gl_ep_fmt, pica_gl_ep_depth_addr;
static unsigned pica_gl_ep_w, pica_gl_ep_h, pica_gl_ep_depth_bpp;
static void pica_gl_capture_ep(void) {
    pica_gl_ep_addr = pica_rt_addr;
    pica_gl_ep_w = pica_rt_w;
    pica_gl_ep_h = pica_rt_h;
    pica_gl_ep_fmt = pica_rt_format;
    pica_gl_ep_depth_addr = pica_depth_addr;
    pica_gl_ep_depth_bpp = pica_depth_bpp;
}
/* Write back deferred GL render-target content into guest memory.  Call
   before anything reads or overwrites guest pixels outside the GL backend:
   GSP display transfer, memory fill, DMA, Y2R output, capture/scan-out, and
   the software rasterizer path.  No-op when nothing is pending. */
static void pica_gl_flush_guest_rt(void) {
    uint32_t a, f, da;
    unsigned w, h, db;
    uint64_t pgl_dl_t0;
    if (!pica_gl_pending()) return;
    pgl_dl_t0 = pica_pqt_now_ns();   /* t_dd830a74: download-boundary cost */
    a = pica_rt_addr; w = pica_rt_w; h = pica_rt_h; f = pica_rt_format;
    da = pica_depth_addr; db = pica_depth_bpp;
    PF_ENTER(PF_GL); /* FRAMEPROF: deferred write-back = GL submit + worker wait */
    { uint64_t pf_w0 = pf_trap_armed() ? pica_pqt_now_ns() : 0;
    if (dq8_env_on("DQ8_GL_FLUSH_TRACE", &pvs_flush_trace_gate))
        fprintf(stderr, "FLUSHSITE rt=%08X %ux%u fmt=%u draws=%llu\n",
                pica_gl_ep_addr, pica_gl_ep_w, pica_gl_ep_h, pica_gl_ep_fmt,
                (unsigned long long)pica_gl_draws_ok);
    pica_rt_addr = pica_gl_ep_addr;
    pica_rt_w = pica_gl_ep_w;
    pica_rt_h = pica_gl_ep_h;
    pica_rt_format = pica_gl_ep_fmt;
    pica_depth_addr = pica_gl_ep_depth_addr;
    pica_depth_bpp = pica_gl_ep_depth_bpp;
    pica_gl_flush(&pica_gl_host);
    pica_rt_addr = a; pica_rt_w = w; pica_rt_h = h; pica_rt_format = f;
    pica_depth_addr = da; pica_depth_bpp = db;
    PF_EXIT();
    if (pf_w0) pf_shim(PF_SH_WB, pica_pqt_now_ns() - pf_w0);
    pgl_dl_ns += pica_pqt_now_ns() - pgl_dl_t0;
    ++pgl_dl_calls; }
}

/* Depth testing/writing is not implemented in the GL backend; a draw that
   uses it stays on the software rasterizer (exactly the software result).
   Without this gate the GL path would silently skip the depth test. */
static int pica_gl_depth_active(void) {
    uint32_t mask = pica_regs[0x107];
    if (!pica_depth_bound || !pica_depth_range_ok()) return 0;
    return (mask & 1u) || ((mask & (1u << 12)) && pica_depth_allow_write) ? 1 : 0;
}

/* Capture the TEV combiner chain exactly as pica_tev() consumes it. */
static void pica_gl_build_tev(PicaTev *t) {
    static const unsigned bases[6] = {0xc0,0xc8,0xd0,0xd8,0xf0,0xf8};
    unsigned s, i;
    for (s = 0; s < 6; ++s) {
        unsigned base = bases[s];
        uint32_t sources = pica_regs[base], mods = pica_regs[base+1],
                 ops = pica_regs[base+2];
        for (i = 0; i < 3; ++i) {
            t->src_c[s*3+i] = (sources >> (4*i)) & 15u;
            t->mod_c[s*3+i] = (mods >> (4*i)) & 15u;
            t->src_a[s*3+i] = (sources >> (16+4*i)) & 15u;
            t->mod_a[s*3+i] = (mods >> (12+4*i)) & 7u;
        }
        t->cop[s] = ops & 15u;
        t->aop[s] = (ops >> 16) & 15u;
        {
            unsigned cs = pica_regs[base+4] & 3u, as = (pica_regs[base+4] >> 16) & 3u;
            t->scale_c[s] = (float)(1u << (cs % 3u));
            t->scale_a[s] = (float)(1u << (as % 3u));
        }
        pica_rgba(pica_regs[base+3], t->const_c[s]);
        t->buf_upd_c[s] = (s < 4 && (pica_regs[0xe0] & (1u << (8+s)))) ? 1u : 0u;
        t->buf_upd_a[s] = (s < 4 && (pica_regs[0xe0] & (1u << (12+s)))) ? 1u : 0u;
    }
    pica_rgba(pica_regs[0xfd], t->buf_init);
    t->tex_en = (pica_regs[0x80] & 1u) ? 1 : 0;
    if (!t->tex_en) {
        /* No texture unit: clear the staging identity so a later draw can
           never believe a stale (slot,gen) pair belongs to this image. */
        pica_gl_st.tex_slot = ~0u;
        pica_gl_st.tex_gen = 0;
    }
}

/* Raw Morton-tile size per texture format (mirrors pica_sample_tex0's
   CalculateTileSize switch; used only to bound the dirty-check snapshot). */
static size_t pica_stage_raw_len(uint32_t w, uint32_t h, uint32_t fmt) {
    uint32_t tile_size;
    switch (fmt) {
    case 0u: tile_size = 4u * 64u; break;                 /* RGBA8 */
    case 1u: tile_size = 3u * 64u; break;                 /* RGB8 */
    case 2u: case 3u: case 4u: case 5u: case 6u: tile_size = 2u * 64u; break;
    case 7u: case 8u: tile_size = 64u; break;             /* I8, A8 */
    case 9u: tile_size = 64u; break;                      /* IA4 */
    case 10u: case 11u: tile_size = 32u; break;           /* I4, A4 */
    case 12u: tile_size = 32u; break;                     /* ETC1 */
    case 13u: tile_size = 64u; break;                     /* ETC1A4 */
    default: return 0;
    }
    /* Superset of every tile pica_sample_tex0 can index (its index math uses
       w>>3 columns and w/h are not required to be multiples of 8). */
    return (size_t)(((w + 7u) >> 3) * ((h + 7u) >> 3)) * (size_t)tile_size;
}

/* ---- texture staging cache (t_86b8f1f9) ---------------------------------
   The staging loop below walks EVERY texel of texture0 through
   pica_sample_tex0 (256x1024 = 262144 sampler calls + 1 MB of hashing per
   draw), which dominated the name-entry/StreetPass dialog's GL CPU cost.  The
   staged image is fully determined by the raw guest-VRAM bytes of the
   texture's Morton tiles plus the sampler configuration (base/dims/format/
   wrap/border), so the first staging of a texture snapshots those raw bytes
   and a later draw with the same configuration reuses the staged image when
   the raw bytes still compare EQUAL (exact dirty-check against guest VRAM —
   no hash shortcuts).  Multiple slots so alternating atlases (bg sheet vs
   glyph sheet) both stay cached.  DQ8_GL_TEXCACHE=0 forces a re-decode on
   every draw (pre-change behaviour) for differential testing. */
/* t_66c89cf2: 24 slots (was 8) — the savestate scene keeps more distinct
   texture base addresses live than 8, so LRU eviction re-decoded textures
   that were still in use.  Byte-budgeted (see PICA_STAGE_BUDGET) so a wide
   cache cannot balloon memory when big atlases are resident.
   t_77ac52ce: 64 slots — STAGEPHASE showed 425 evictions vs 1249 hits in
   14 s with 24 slots (25 % of staging calls re-decoded a texture that was
   evicted, ~82 ms/s of decode).
   t_66c89cf2: 256 slots — on the savestate scene 64 slots still evicted a
   still-live key 1453 times in 30 s (miss_evict≈miss_new: cache perpetually
   full).  Resident cost at the measured working set (avg 41k texels/texture)
   is ~60-100 MB, well inside the byte budget below. */
#define PICA_STAGE_SLOTS 256
#ifdef DQ8_ENABLE_GL
/* A staging slot maps 1:1 onto a GL upload slot (pica_gl.c's PGL_UP_SLOTS,
   declared earlier in the spliced runtime TU), so the staging cache must not
   outgrow the GL slot space. */
_Static_assert(PICA_STAGE_SLOTS <= PGL_UP_SLOTS,
               "PICA_STAGE_SLOTS must fit the GL upload slot space");
#endif
#define PICA_STAGE_BUDGET_DEFAULT (256u * 1024u * 1024u) /* t_77ac52ce: 64 slots */
typedef struct {
    int used;
    uint32_t paddr_field, mapped, w, h, fmt, wrap_s, wrap_t, border;
    unsigned char *raw;
    size_t raw_len;
    unsigned char *staged;
    size_t staged_len;
    unsigned hash;
    unsigned long long lru;
    unsigned long long gen;   /* bumped on every (re)decode of this slot */
} PicaStageSlot;
static PicaStageSlot pica_stage_slots[PICA_STAGE_SLOTS];
static unsigned long long pica_stage_lru_tick;
static int pica_stage_cache_on = -1;
static int pica_stage_key_legacy = -1; /* DQ8_GL_TEXKEY=legacy: old wrap/border key */
static size_t pica_stage_budget = PICA_STAGE_BUDGET_DEFAULT;
static unsigned long long pica_stage_calls, pica_stage_hits, pica_stage_misses,
    pica_stage_texels_decoded, pica_stage_raw_snap_bytes, pica_stage_cmp_bytes;
/* t_66c89cf2 attribution counters (why a miss happened, and what it cost). */
static unsigned long long pica_stage_miss_dirty, pica_stage_miss_new,
    pica_stage_miss_evict, pica_stage_keyonly_hits, pica_stage_decode_ns,
    pica_stage_cmp_ns;
/* t_66c89cf2 round-1: persistent decoded-texture deepening.
   content_hits  — level-3 reuse: a resident slot's raw snapshot is
                   byte-identical to this key's raw bytes, so the decoded
                   image is reused with no decode and no GL re-upload.
   key_first     — decode of a key never decoded before this run.
   key_resee     — decode of a key that WAS decoded before but had no
                   resident slot (LRU eviction; 0 with a big enough cache).
   fast/slow texels — split of decoded texels by decoder path.
   check_ok/fail — DQ8_GL_TEXDECODE proof runs (fast vs reference byte-equal). */
static unsigned long long pica_stage_content_hits, pica_stage_key_first,
    pica_stage_key_resee, pica_stage_fast_texels, pica_stage_slow_texels,
    pica_tex_checked, pica_tex_check_fail;
static int pica_tex_fast_mode = -1; /* -1=probe env; 0=reference; 1=fast */
static int pica_tex_check_n = -1;   /* auto-check first N textures (-1=4) */
static int pica_tex_check_all;      /* DQ8_GL_TEXDECODE=check: verify every decode */
static int pica_stage_content_on = -1; /* DQ8_GL_TEXCONTENT=0 disables level 3 */
static int pica_stage_texstats_on = -1; /* cached DQ8_GL_TEXSTATS */
/* Key census: exact set of {addr_field,mapped,w,h,fmt} keys ever decoded, so
   a re-decode can be split into "first decode of this key" (unavoidable —
   one decode per distinct texture) vs "re-decode after LRU eviction" (cache
   too small).  Open addressing, full-key compare (no hash shortcuts);
   stops inserting once the table is dense — the census then degrades to an
   undercount of resee, never a wrong answer for first. */
#define PICA_STAGE_CENSUS 8192
typedef struct { unsigned long long a, b; } PicaCensusKey; /* a: addr|mapped, b: w|h|fmt (a==0 empty: addr_field!=0 here) */
static PicaCensusKey pica_stage_census[PICA_STAGE_CENSUS];
static unsigned pica_stage_census_n;
/* Returns 1 if this key was decoded before (resee), 0 if it is new (inserted
   unless the table is dense — then it stays unrecorded and future re-decodes
   count as first; resee is undercounted, never overcounted). */
static int pica_stage_census_seen(uint32_t addr_field, uint32_t mapped,
                                  uint32_t w, uint32_t h, uint32_t fmt) {
    PicaCensusKey k;
    unsigned i, probes = 0;
    k.a = ((unsigned long long)addr_field << 32) | mapped;
    k.b = ((unsigned long long)w << 32) | ((unsigned long long)h << 16) | fmt;
    i = (unsigned)(((k.a * 0x9E3779B97F4A7C15ull) ^ k.b) >> 52) &
        (PICA_STAGE_CENSUS - 1u);
    for (;;) {
        PicaCensusKey *e = &pica_stage_census[i];
        if (e->a == k.a && e->b == k.b) return 1;
        if (e->a == 0) {
            if (pica_stage_census_n < (PICA_STAGE_CENSUS / 8u) * 7u) {
                *e = k;
                ++pica_stage_census_n;
            }
            return 0;
        }
        i = (i + 1u) & (PICA_STAGE_CENSUS - 1u);
        if (++probes >= PICA_STAGE_CENSUS) return 0;
    }
}

/* Content-reuse alias: a key K whose bytes were found byte-identical to a
   resident slot S never gets its own slot (no decode, no upload), so the
   O(slots) content scan would repeat for K on every staging call.  The alias
   records K -> S and makes the repeat an O(1) probe; the probe re-validates
   with an EXACT memcmp of S->raw against K's current bytes (A15 rule: the
   alias only narrows — bytes must still compare equal, and S may have been
   re-decoded or evicted for another key since). */
#define PICA_STAGE_ALIAS 4096 /* power of two */
typedef struct {
    unsigned long long a, b; /* a: addr|mapped (0 = empty), b: w|h|fmt */
    unsigned slot;
} PicaAliasEnt;
static PicaAliasEnt pica_stage_alias[PICA_STAGE_ALIAS];
static unsigned long long pica_stage_alias_hits;

/* End-of-run texture staging summary (called from the GL shutdown line). */
static void pica_stage_stats_dump(void) {
    fprintf(stderr,
            "GL texture stage: calls=%llu cache_hits=%llu cache_misses=%llu "
            "texels_decoded=%llu raw_bytes_compared=%llu "
            "raw_bytes_snapshotted=%llu\n",
            pica_stage_calls, pica_stage_hits, pica_stage_misses,
            pica_stage_texels_decoded, pica_stage_cmp_bytes,
            pica_stage_raw_snap_bytes);
    fprintf(stderr,
            "GL texture stage reasons: miss_dirty=%llu miss_new_key=%llu "
            "miss_evicted=%llu legacy_key_only_hits=%llu "
            "decode_ms=%.1f cmp_ms=%.1f slots=%d\n",
            pica_stage_miss_dirty, pica_stage_miss_new, pica_stage_miss_evict,
            pica_stage_keyonly_hits, (double)pica_stage_decode_ns / 1e6,
            (double)pica_stage_cmp_ns / 1e6, PICA_STAGE_SLOTS);
    /* t_66c89cf2: persistent-cache proof line.  content_hits = reuse of an
       already-decoded image under a different key (no decode, no upload);
       key_first = decodes of keys never decoded this run (the floor);
       key_resee = decodes that were evicted-then-needed again (should be 0
       once slots cover the working set); fast/slow = decoder split;
       check_* = DQ8_GL_TEXDECODE byte-verification runs (fail must be 0). */
    fprintf(stderr,
            "GL texture stage deep: content_hits=%llu key_first=%llu "
            "key_resee=%llu census_keys=%u fast_texels=%llu slow_texels=%llu "
            "decode_checked=%llu decode_mismatch=%llu decoder=%s\n",
            pica_stage_content_hits, pica_stage_key_first,
            pica_stage_key_resee, pica_stage_census_n,
            pica_stage_fast_texels, pica_stage_slow_texels,
            pica_tex_checked, pica_tex_check_fail,
            pica_tex_fast_mode == 0 ? "reference"
                : (pica_tex_check_all ? "fast+check-all"
                   : (pica_tex_check_n > 0 ? "fast+check-first" : "fast")));
}

/* Periodic (every 500 staging calls) full counter line — the measurement
   channel for run_state_probe runs, which SIGTERM the process and therefore
   never reach pica_stage_stats_dump(). */
static void pica_stage_texstat_print(void) {
    fprintf(stderr,
            "GL texstat stage: calls=%llu hits=%llu misses=%llu "
            "texels_decoded=%llu cmp_bytes=%llu snap_bytes=%llu "
            "miss_dirty=%llu miss_new=%llu miss_evict=%llu keyonly=%llu "
            "decode_ms=%.1f cmp_ms=%.1f "
            "content=%llu alias=%llu first=%llu resee=%llu census=%u "
            "fast_tex=%llu slow_tex=%llu chk=%llu mismatch=%llu\n",
            pica_stage_calls, pica_stage_hits, pica_stage_misses,
            pica_stage_texels_decoded, pica_stage_cmp_bytes,
            pica_stage_raw_snap_bytes, pica_stage_miss_dirty,
            pica_stage_miss_new, pica_stage_miss_evict,
            pica_stage_keyonly_hits, (double)pica_stage_decode_ns / 1e6,
            (double)pica_stage_cmp_ns / 1e6,
            pica_stage_content_hits, pica_stage_alias_hits,
            pica_stage_key_first, pica_stage_key_resee,
            pica_stage_census_n, pica_stage_fast_texels,
            pica_stage_slow_texels, pica_tex_checked, pica_tex_check_fail);
}

/* ---- t_66c89cf2: hoisted texel decoder for the staging loop --------------
   pica_sample_tex0() costs ~57 ns/texel (measured: 60.6 M texels / 3.47 s in
   the savestate scene) because EVERY call re-reads the PICA registers,
   re-derives the guest mapping (guest_memory_address + guest_memory_range),
   re-runs the wrap/border branch and repeats the format switch.  The staging
   loop only ever samples TEXEL CENTRES of ONE fixed texture configuration:
       s = (int)((c+0.5f)/w*w) = c          (in [0,w-1] -> every wrap mode is
       t = (int)((r+0.5f)/h*h) = r           the identity for in-range coords)
       y = h-1-t
   so the decode reduces to the Morton-tile lookup with the per-texture
   invariants (base pointer, tile size, bytes/texel, tile-row stride) hoisted
   out of the loop.  The arithmetic below is the SAME as pica_sample_tex0's
   (same xlut/ylut, same per-format expressions, same truncations).
   Formats 12/13 (ETC1/ETC1A4) keep the reference sampler: their subtile math
   is long and they are rare in this scene.
   DQ8_GL_TEXDECODE=check runs BOTH decoders for the first N textures and
   requires the staged images to compare byte-identical (proof, not a guess);
   any mismatch permanently disables the fast path and is reported. */
typedef struct {
    const unsigned char *base;   /* ram + mapped of the texture base */
    uint32_t w, h, tile_size, bpp, tiles_x, fmt_;
} PicaTexFast;

static int pica_tex_fast_init(PicaTexFast *fx, const unsigned char *base,
                              uint32_t w, uint32_t h, uint32_t fmt) {
    switch (fmt) {
    case 0u: fx->tile_size = 4u * 64u; break;                 /* RGBA8 */
    case 1u: fx->tile_size = 3u * 64u; break;                 /* RGB8 */
    case 2u: case 3u: case 4u: case 5u: case 6u:
        fx->tile_size = 2u * 64u; break;                      /* 16-bit */
    case 7u: case 8u: case 9u: fx->tile_size = 64u; break;     /* I8/A8/IA4 */
    case 10u: case 11u: fx->tile_size = 32u; break;            /* I4/A4 */
    default: return 0;                                        /* ETC1 etc. */
    }
    fx->bpp = (fmt == 0u) ? 4u : (fmt == 1u) ? 3u : (fmt <= 6u) ? 2u : 1u;
    fx->base = base;
    fx->w = w;
    fx->h = h;
    fx->tiles_x = w >> 3;   /* same truncation pica_sample_tex0 uses */
    fx->fmt_ = fmt;
    return 1;
}

/* Decode one texel at in-tile-source coordinates (x, y) — y already flipped. */
static inline void pica_tex_fast_texel(const PicaTexFast *fx, uint32_t x,
                                       uint32_t y, uint8_t *out) {
    static const uint32_t xlut[8] = {0x00,0x01,0x04,0x05,0x10,0x11,0x14,0x15};
    static const uint32_t ylut[8] = {0x00,0x02,0x08,0x0a,0x20,0x22,0x28,0x2a};
    uint32_t tile = ((y >> 3) * fx->tiles_x + (x >> 3)) * fx->tile_size;
    uint32_t morton = xlut[x & 7u] + ylut[y & 7u];
    const uint8_t *p;
    if (fx->fmt_ == 10u || fx->fmt_ == 11u) {
        /* nibble formats: morton index picks the nibble inside the byte */
        const uint8_t *n = fx->base + tile + morton / 2u;
        uint32_t nib = (morton & 1u) ? ((*n & 0xF0u) >> 4) : (*n & 0x0Fu);
        uint8_t v8 = (uint8_t)((nib << 4) | nib);
        if (fx->fmt_ == 10u) { out[0] = out[1] = out[2] = v8; out[3] = 255u; }
        else { out[0] = out[1] = out[2] = 0u; out[3] = v8; }
        return;
    }
    p = fx->base + tile + morton * fx->bpp;
    switch (fx->fmt_) {
    case 0u: out[0] = p[3]; out[1] = p[2]; out[2] = p[1]; out[3] = p[0]; break;
    case 1u: out[0] = p[2]; out[1] = p[1]; out[2] = p[0]; out[3] = 255u; break;
    case 2u: { /* RGB5A1 */
        uint16_t v16; memcpy(&v16, p, 2);
        uint32_t r5 = (v16 >> 11) & 31u, g5 = (v16 >> 6) & 31u, b5 = (v16 >> 1) & 31u;
        out[0] = (uint8_t)((r5 << 3) | (r5 >> 2));
        out[1] = (uint8_t)((g5 << 3) | (g5 >> 2));
        out[2] = (uint8_t)((b5 << 3) | (b5 >> 2));
        out[3] = (v16 & 1u) ? 255u : 0u;
        break;
    }
    case 3u: { /* RGB565 */
        uint16_t v16; memcpy(&v16, p, 2);
        uint32_t r5 = (v16 >> 11) & 31u, g6 = (v16 >> 5) & 63u, b5 = v16 & 31u;
        out[0] = (uint8_t)((r5 << 3) | (r5 >> 2));
        out[1] = (uint8_t)((g6 << 2) | (g6 >> 4));
        out[2] = (uint8_t)((b5 << 3) | (b5 >> 2));
        out[3] = 255u;
        break;
    }
    case 4u: { /* RGBA4 */
        uint16_t v16; memcpy(&v16, p, 2);
        uint32_t r4 = (v16 >> 12) & 15u, g4 = (v16 >> 8) & 15u,
                 b4 = (v16 >> 4) & 15u, a4 = v16 & 15u;
        out[0] = (uint8_t)((r4 << 4) | r4);
        out[1] = (uint8_t)((g4 << 4) | g4);
        out[2] = (uint8_t)((b4 << 4) | b4);
        out[3] = (uint8_t)((a4 << 4) | a4);
        break;
    }
    case 5u: out[0] = out[1] = out[2] = p[1]; out[3] = p[0]; break; /* IA8 */
    case 6u: out[0] = p[1]; out[1] = p[0]; out[2] = 0u; out[3] = 255u; break;
    case 7u: out[0] = out[1] = out[2] = p[0]; out[3] = 255u; break;
    case 8u: out[0] = out[1] = out[2] = 0u; out[3] = p[0]; break;
    default: { /* 9: IA4 */
        uint32_t i4 = (*p >> 4) & 15u, a4 = *p & 15u;
        out[0] = out[1] = out[2] = (uint8_t)((i4 << 4) | i4);
        out[3] = (uint8_t)((a4 << 4) | a4);
        break;
    }
    }
}

/* Stage texture0 into a linear RGBA8 image for GL upload. Row r holds what the
   software sampler returns for v=(r+0.5)/h so GL_NEAREST reproduces it. */
static int pica_gl_stage_texture(void) {
    static unsigned char *stage;
    static size_t stage_cap;
    uint32_t w = (pica_regs[0x82] >> 16) & 0x7FFu;
    uint32_t h = pica_regs[0x82] & 0x7FFu;
    uint32_t addr_field = pica_regs[0x85] & 0x0FFFFFFFu;
    uint32_t fmt = pica_regs[0x8E] & 0xFu;
    uint32_t wrap_s = (pica_regs[0x83] >> 12) & 7u;
    uint32_t wrap_t = (pica_regs[0x83] >> 8) & 7u;
    uint32_t border = pica_regs[0x81];
    uint32_t va, mapped = 0;
    size_t need, raw_len;
    PicaStageSlot *slot;
    unsigned i, r, c;
    uint32_t hash = 2166136261u; /* FNV-1a, lets the GL backend skip re-upload */
    if (pica_stage_cache_on < 0) {
        const char *e = getenv("DQ8_GL_TEXCACHE");
        pica_stage_cache_on = (!e || !e[0] || e[0] != '0');
    }
    if (pica_stage_key_legacy < 0) {
        /* t_66c89cf2: the sampler configuration (wrap_s/wrap_t/border) cannot
           change the staged image: the decode samples TEXEL CENTRES
           ((c+0.5)/w), whose integer s,t always land inside [0,w)x[0,h), so
           pica_sample_tex0 never takes its border or wrap branch.  The legacy
           key included those three fields, so the same atlas drawn with a
           different sampler state was re-decoded and re-uploaded.  Default:
           key on the CONTENT ({addr,w,h,fmt}) only.  DQ8_GL_TEXKEY=legacy
           reproduces the old key for differential measurement. */
        const char *e = getenv("DQ8_GL_TEXKEY");
        pica_stage_key_legacy = (e && e[0] == 'l');
        {
            const char *m = getenv("DQ8_GL_TEXMEM_MB");
            if (m && m[0]) {
                unsigned long mb = strtoul(m, NULL, 10);
                if (mb >= 8u && mb <= 2048u)
                    pica_stage_budget = (size_t)mb * 1024u * 1024u;
            }
        }
    }
    if (pica_tex_fast_mode < 0) {
        /* t_66c89cf2 decoder selection (see PicaTexFast above):
           default      — fast decoder, byte-check the first 4 staged textures
                          against pica_sample_tex0 (proof on every run).
           =0 / off     — reference decoder only (pre-change behaviour).
           =check       — fast decoder, byte-check EVERY staged texture.
           =fast        — fast decoder, no checking (max speed). */
        const char *e = getenv("DQ8_GL_TEXDECODE");
        if (!e || !e[0]) { pica_tex_fast_mode = 1; pica_tex_check_n = 4; }
        else if (e[0] == '0' || e[0] == 'o') pica_tex_fast_mode = 0;
        else if (e[0] == 'c') { pica_tex_fast_mode = 1; pica_tex_check_all = 1; }
        else { pica_tex_fast_mode = 1; pica_tex_check_n = 0; }
        {
            const char *c2 = getenv("DQ8_GL_TEXCONTENT");
            pica_stage_content_on = (!c2 || !c2[0] || c2[0] != '0');
        }
        {
            const char *t2 = getenv("DQ8_GL_TEXSTATS");
            pica_stage_texstats_on = (t2 && t2[0]);
        }
    }
    ++pica_stage_calls;
    /* Periodic counter line on EVERY 500th call (hit or miss) — the probe
       harness SIGTERMs the run, so the end-of-run dump never prints. */
    if (pica_stage_texstats_on && (pica_stage_calls % 500u) == 0u)
        pica_stage_texstat_print();
    if (!w || !h || w > 1024u || h > 1024u || !addr_field) return 0;
    /* The texture can alias the render target: deferred GL content must
       land in guest memory before the staging reads texels from it. */
    if (pica_gl_pending()) {
        uint32_t tf = pica_regs[0x85] & 0x0FFFFFFFu;
        if (tf) {
            uint32_t tva = guest_memory_address(tf * 8u);
            uint64_t ta = (uint64_t)tva,
                     tb = ta + (uint64_t)w * h * 4u,
                     ra = (uint64_t)pica_rt_addr,
                     rb = ra + (uint64_t)pica_rt_w * pica_rt_h * 4u;
            if (ta < rb && ra < tb) {
                /* t_05281b89: the guest is sampling the CURRENT render target
                   as a texture.  t_dd830a74 item 2: with the Azahar surface
                   model the target keeps its scaled backing and unit 0 is
                   bound to a full-resolution copy of it (normalised UVs need
                   no change).  The publish still happens (guest memory current,
                   item 3's download), it is just no longer the sample source. */
                int scaled = 0;
                pica_gl_note_target_sampled(pica_rt_w, pica_rt_h);
                scaled = pica_gl_sample_scaled_target(w, h, fmt, tva);
                pgl_diag_stage_want = (scaled == 0);
                pica_gl_flush_guest_rt();
                if (scaled) {
                    pica_gl_st.tex_is_rt = 1;
                    pica_gl_st.tex_w = (int)w;
                    pica_gl_st.tex_h = (int)h;
                    pica_gl_st.tex_rgba = NULL;   /* nothing to upload */
                    pica_gl_st.tex_slot = ~0u;
                    pica_gl_st.tex_gen = 0;
                    pica_gl_st.tex_hash = 0;
                    return 1;
                }
            }
        }
    }
    raw_len = pica_stage_raw_len(w, h, fmt);
    if (!raw_len) return 0;
    va = guest_memory_address(addr_field * 8u);
    if (!guest_memory_range(va, 4u, &mapped)) return 0;
    if ((uint64_t)mapped + raw_len > GUEST_RAM_SIZE) return 0;
    /* Cache lookup: identical CONTENT key + raw Morton tiles unchanged
       (memcmp of the exact source bytes pica_sample_tex0 would read). */
    if (pica_stage_cache_on) {
        for (i = 0; i < PICA_STAGE_SLOTS; ++i) {
            PicaStageSlot *s = &pica_stage_slots[i];
            unsigned long long c0;
            if (!s->used || s->paddr_field != addr_field || s->mapped != mapped ||
                s->w != w || s->h != h || s->fmt != fmt ||
                s->raw_len != raw_len)
                continue;
            if (pica_stage_key_legacy &&
                (s->wrap_s != wrap_s || s->wrap_t != wrap_t ||
                 s->border != border))
                continue;
            c0 = pica_pqt_now_ns();
            if (memcmp(s->raw, ram + mapped, raw_len) == 0) {
                pica_stage_cmp_ns += pica_pqt_now_ns() - c0;
                pica_stage_cmp_bytes += raw_len;
                s->lru = ++pica_stage_lru_tick;
                ++pica_stage_hits;
                if (s->wrap_s != wrap_s || s->wrap_t != wrap_t ||
                    s->border != border)
                    ++pica_stage_keyonly_hits;
                /* Remember the sampler state that last used this image (for
                   the diagnostic only; the image itself does not depend on
                   it). */
                s->wrap_s = wrap_s; s->wrap_t = wrap_t; s->border = border;
                pica_gl_st.tex_rgba = s->staged;
                pica_gl_st.tex_w = (int)w;
                pica_gl_st.tex_h = (int)h;
                pica_gl_st.tex_hash = s->hash;
                pica_gl_st.tex_slot = (unsigned)i;
                pica_gl_st.tex_gen = s->gen;
                return 1;
            }
        }
    }
    /* t_66c89cf2 level 3: content-addressed reuse.  The key above is
       address-based; a miss here means NO resident slot owns this key — but
       the decoded image is a pure function of the raw Morton bytes plus
       {w,h,fmt} (texel centres never take the wrap/border branch), so a
       resident slot whose SNAPSHOT equals these bytes already has exactly
       the image this key would decode to.  Reuse it: no decode, no upload
       (we hand back that slot's (slot,gen) identity, which the GL upload
       cache already has).  Two probes:
         alias  — O(1) table for keys previously served by content reuse;
                  re-validated by an EXACT memcmp (alias only narrows, A15).
         scan   — first sighting: O(slots) memcmp, gated on w/h/fmt/raw_len. */
    if (pica_stage_cache_on && pica_stage_content_on) {
        PicaStageSlot *hit = NULL;
        unsigned long long cc0 = pica_pqt_now_ns();
        unsigned long long a = ((unsigned long long)addr_field << 32) | mapped,
                           b = ((unsigned long long)w << 32) |
                               ((unsigned long long)h << 16) | fmt;
        unsigned ai = (unsigned)(((a * 0x9E3779B97F4A7C15ull) ^ b) >> 48) &
                      (PICA_STAGE_ALIAS - 1u);
        PicaAliasEnt *e = &pica_stage_alias[ai];
        if (e->a == a && e->b == b && e->slot < PICA_STAGE_SLOTS) {
            PicaStageSlot *s = &pica_stage_slots[e->slot];
            if (s->used && s->w == w && s->h == h && s->fmt == fmt &&
                s->raw_len == raw_len &&
                memcmp(s->raw, ram + mapped, raw_len) == 0) {
                hit = s;
                ++pica_stage_alias_hits;
            } else {
                e->a = 0; /* stale: slot evicted/re-keyed or content changed */
            }
        }
        if (!hit) {
            for (i = 0; i < PICA_STAGE_SLOTS; ++i) {
                PicaStageSlot *s = &pica_stage_slots[i];
                if (!s->used || s->w != w || s->h != h || s->fmt != fmt ||
                    s->raw_len != raw_len)
                    continue;
                if (memcmp(s->raw, ram + mapped, raw_len) == 0) { hit = s; break; }
            }
            if (hit) {
                e->a = a; e->b = b;
                e->slot = (unsigned)(hit - pica_stage_slots);
            }
        }
        if (hit) {
            hit->lru = ++pica_stage_lru_tick;
            hit->wrap_s = wrap_s; hit->wrap_t = wrap_t; hit->border = border;
            ++pica_stage_hits;
            ++pica_stage_content_hits;
            /* cmp_ns/bytes = the probe memcmps themselves (alias + scan) */
            pica_stage_cmp_ns += pica_pqt_now_ns() - cc0;
            pica_stage_cmp_bytes += raw_len;
            pica_gl_st.tex_rgba = hit->staged;
            pica_gl_st.tex_w = (int)w;
            pica_gl_st.tex_h = (int)h;
            pica_gl_st.tex_hash = hit->hash;
            pica_gl_st.tex_slot = (unsigned)(hit - pica_stage_slots);
            pica_gl_st.tex_gen = hit->gen;
            return 1;
        }
    }
    ++pica_stage_misses;
    /* Decode path.  A dirty miss on a CONTENT key that already owns a slot
       re-decodes into THAT slot (stable staging pointer and GL object per
       distinct texture), so alternating content at one base address never
       churns buffers.  New keys take an unused slot, then the least-recently
       -used one; an eviction is counted (a still-live texture was pushed out
       = cache too small). */
    slot = NULL;
    for (i = 0; i < PICA_STAGE_SLOTS; ++i) {
        PicaStageSlot *s = &pica_stage_slots[i];
        if (s->used && s->paddr_field == addr_field && s->mapped == mapped &&
            s->w == w && s->h == h && s->fmt == fmt) {
            slot = s;
            break;
        }
    }
    if (slot) {
        ++pica_stage_miss_dirty;
    } else {
        ++pica_stage_miss_new;
        /* t_66c89cf2: split a decode into "first decode of this key" (the
           unavoidable floor — one per distinct texture) vs "re-decode after
           LRU eviction" (would be 0 with enough slots). */
        if (pica_stage_census_seen(addr_field, mapped, w, h, fmt))
            ++pica_stage_key_resee;
        else
            ++pica_stage_key_first;
        slot = &pica_stage_slots[0];
        for (i = 0; i < PICA_STAGE_SLOTS; ++i) {
            if (!pica_stage_slots[i].used) { slot = &pica_stage_slots[i]; break; }
            if (pica_stage_slots[i].lru < slot->lru) slot = &pica_stage_slots[i];
        }
        if (slot->used) ++pica_stage_miss_evict;
    }
    /* t_cb8723ae GL thread split: the worker may still read this slot's
       staged bytes for a queued draw (identity = (slot,gen,ptr)); drain the
       GL queue before the re-decode overwrites/reallocs them.  Decode misses
       are rare (~0.4/frame), and the hook is a no-op without DQ8_SPLIT_GL. */
    pica_gl_split_sync();
    need = (size_t)w * h * 4u;
    if (slot->staged_len < need ||
        (slot->raw_len + slot->staged_len + need + raw_len > pica_stage_budget &&
         slot->staged_len > need)) {
        unsigned char *grown =
            (unsigned char *)realloc(slot->staged, need ? need : 1u);
        if (!grown) return 0;
        slot->staged = grown;
        slot->staged_len = need;
    }
    if (slot->raw_len != raw_len) {
        unsigned char *grown =
            (unsigned char *)realloc(slot->raw, raw_len ? raw_len : 1u);
        if (!grown) return 0;
        slot->raw = grown;
        slot->raw_len = raw_len;
    }
    stage = slot->staged;
    stage_cap = slot->staged_len; /* legacy name for the debug block below */
    {
        unsigned long long d0 = pica_pqt_now_ns();
        PicaTexFast fx;
        int fast = 0, check = 0;
        /* t_66c89cf2: hoisted decoder.  pica_tex_fast_init returns 0 for
           formats it does not implement (ETC1/ETC1A4, out of range) and for
           DQ8_GL_TEXDECODE=0 — those decode through pica_sample_tex0 exactly
           as before.  check mode decodes BOTH ways and byte-compares the
           images; any mismatch disables the fast path permanently (the
           reference bytes are kept) so a bug degrades to the old speed,
           never to wrong pixels. */
        if (pica_tex_fast_mode != 0 &&
            pica_tex_fast_init(&fx, ram + mapped, w, h, fmt)) {
            fast = 1;
            check = pica_tex_check_all ||
                    (pica_tex_check_n > 0 &&
                     pica_tex_checked < (unsigned long long)pica_tex_check_n);
        }
        if (fast) {
            for (r = 0; r < h; ++r) {
                /* y = h-1-t with t = r: texel centres, wrap identity,
                   border never fires (see PicaTexFast). */
                uint32_t y = h - 1u - r;
                for (c = 0; c < w; ++c) {
                    uint8_t out[4];
                    unsigned char *p;
                    pica_tex_fast_texel(&fx, (uint32_t)c, y, out);
                    p = stage + ((size_t)r * w + c) * 4u;
                    p[0] = out[0]; p[1] = out[1]; p[2] = out[2]; p[3] = out[3];
                    hash = (hash ^ out[0]) * 16777619u;
                    hash = (hash ^ out[1]) * 16777619u;
                    hash = (hash ^ out[2]) * 16777619u;
                    hash = (hash ^ out[3]) * 16777619u;
                }
            }
            pica_stage_fast_texels += (unsigned long long)w * h;
            if (check) {
                static unsigned char *chk;
                static size_t chk_cap;
                size_t need2 = (size_t)w * h * 4u;
                int ok = 1;
                if (chk_cap < need2) {
                    unsigned char *grown =
                        (unsigned char *)realloc(chk, need2 ? need2 : 1u);
                    if (!grown) ok = 0; else { chk = grown; chk_cap = need2; }
                }
                if (ok) {
                    for (r = 0; r < h && ok; ++r)
                        for (c = 0; c < w; ++c) {
                            uint8_t rr, gg, bb, aa;
                            unsigned char *q;
                            if (!pica_sample_tex0_exact(((float)c + 0.5f) / (float)w,
                                                  ((float)r + 0.5f) / (float)h,
                                                  &rr, &gg, &bb, &aa)) { ok = 0; break; }
                            q = chk + ((size_t)r * w + c) * 4u;
                            q[0] = rr; q[1] = gg; q[2] = bb; q[3] = aa;
                        }
                }
                if (ok) {
                    ++pica_tex_checked;
                    if (memcmp(chk, stage, need2) != 0) {
                        size_t qi;
                        ++pica_tex_check_fail;
                        fprintf(stderr,
                                "GL texdecode CHECK FAIL: %ux%u fmt=%u addr=%08X "
                                "differs from pica_sample_tex0 — fast decoder "
                                "disabled, reference bytes kept\n",
                                w, h, fmt, addr_field);
                        pica_tex_fast_mode = 0;
                        memcpy(stage, chk, need2);
                        hash = 2166136261u;
                        for (qi = 0; qi < need2; ++qi)
                            hash = (hash ^ stage[qi]) * 16777619u;
                    }
                }
            }
        } else {
            for (r = 0; r < h; ++r) {
                for (c = 0; c < w; ++c) {
                    uint8_t rr, gg, bb, aa;
                    unsigned char *p;
                    if (!pica_sample_tex0_exact(((float)c + 0.5f) / (float)w,
                                          ((float)r + 0.5f) / (float)h,
                                          &rr, &gg, &bb, &aa))
                        return 0;
                    p = stage + ((size_t)r * w + c) * 4u;
                    p[0] = rr; p[1] = gg; p[2] = bb; p[3] = aa;
                    hash = (hash ^ rr) * 16777619u;
                    hash = (hash ^ gg) * 16777619u;
                    hash = (hash ^ bb) * 16777619u;
                    hash = (hash ^ aa) * 16777619u;
                }
            }
            pica_stage_slow_texels += (unsigned long long)w * h;
        }
        pica_stage_decode_ns += pica_pqt_now_ns() - d0;
    }
    pica_stage_texels_decoded += (unsigned long long)w * h;
    pica_stage_raw_snap_bytes += raw_len;
    memcpy(slot->raw, ram + mapped, raw_len);
    slot->used = 1;
    slot->paddr_field = addr_field;
    slot->mapped = mapped;
    slot->w = w;
    slot->h = h;
    slot->fmt = fmt;
    slot->wrap_s = wrap_s;
    slot->wrap_t = wrap_t;
    slot->border = border;
    slot->hash = hash ? hash : 1u; /* 0 reserved for "unset" */
    slot->lru = ++pica_stage_lru_tick;
    /* New decoded generation: the GL backend re-uploads exactly once per
       generation of this staging slot (t_66c89cf2). */
    ++slot->gen;
    pica_gl_st.tex_rgba = stage;
    pica_gl_st.tex_w = (int)w;
    pica_gl_st.tex_h = (int)h;
    pica_gl_st.tex_hash = hash ? hash : 1u; /* 0 reserved for "unset" */
    pica_gl_st.tex_slot = (unsigned)(slot - pica_stage_slots);
    pica_gl_st.tex_gen = slot->gen;
    /* Periodic line moved to the top of this function so it also fires on
       hit-only stretches (the probe harness SIGTERMs, no end-of-run dump). */
    /* DQ8_GL_DEBUG: is the staged movie image itself black?  Count RGB and
       alpha nonzero bytes separately (a fill-only RT shows 1 byte/px alpha).
       Timestamped + optional PPM dump (DQ8_GL_DUMP_DIR) so the stage content
       can be correlated with the black-screen window frame by frame. */
    if (dq8_env_on("DQ8_GL_DEBUG", &pvs_gl_debug_gate)) {
        static unsigned long long stage_n;
        unsigned long long rgb_nz = 0, a_nz = 0, i, n = (unsigned long long)w * h;
        for (i = 0; i < n; ++i) {
            if (stage[i*4] | stage[i*4+1] | stage[i*4+2]) ++rgb_nz;
            if (stage[i*4+3]) ++a_nz;
        }
        fprintf(stderr, "GL debug: stage#%llu t=%.2f %ux%u rgb_nz=%llu/%llu "
                "a_nz=%llu first_px=%u/%u/%u/%u\n",
                stage_n, dq8_layer_t(), w, h, rgb_nz, n, a_nz,
                stage[0], stage[1], stage[2], stage[3]);
        {
            const char *dd = getenv("DQ8_GL_DUMP_DIR");
            static unsigned dumps;
            if (dd && dd[0] && dumps < 8u && stage_n >= 10u) {
                char p[512];
                FILE *f;
                snprintf(p, sizeof p, "%s/stage_%03llu.ppm", dd, stage_n);
                f = fopen(p, "wb");
                if (f) {
                    fprintf(f, "P6\n%u %u\n255\n", w, h);
                    for (i = 0; i < n; ++i)
                        fwrite(stage + i * 4u, 1, 3, f);
                    fclose(f);
                    ++dumps;
                }
            }
        }
        ++stage_n;
    }
    return 1;
}


/* ---- t_e216fc37: staging for PICA TEXTURE UNIT 1 (TEV source 4) --------
   The unit-0 cache above is hard-wired to the unit-0 register block
   (0x81/0x82/0x83/0x85/0x8E) and to its fast decoder / content-addressed
   alias table / budget accounting.  Unit 1 has its own registers
   (0x91/0x92/0x93/0x95/0x96), so it gets a small independent slot cache that
   decodes through the reference sampler pica_sample_tex_unit(1, ...): exact
   for every texture format by construction (no second fast path that could
   disagree).  Draws whose TEV chain reads source 4 are rare (a handful of
   ground decals per frame) and a slot is re-decoded only when the raw Morton
   bytes of its source image change, so the reference-speed decode is paid
   once per distinct image, not per draw.

   Identity for the GL upload cache is (slot, generation), exactly like
   unit 0: a generation bump means the staged bytes changed. */
#define PICA_STAGE1_SLOTS 4
typedef struct {
    int used;
    uint32_t paddr_field, mapped, w, h, fmt;
    size_t raw_len;
    unsigned char *raw;      /* source Morton bytes (dirty check) */
    unsigned char *staged;   /* decoded RGBA8, row 0 = GL v=0 */
    size_t staged_len;
    uint32_t hash;
    unsigned long long gen;
    unsigned long long lru;
} PicaStage1Slot;
static PicaStage1Slot pica_stage1_slots[PICA_STAGE1_SLOTS];
static unsigned long long pica_stage1_lru_tick;
static unsigned long long pica_stage1_decodes, pica_stage1_hits;

static int pica_gl_stage_texture1(void) {
    uint32_t w = (pica_regs[0x92] >> 16) & 0x7FFu;
    uint32_t h = pica_regs[0x92] & 0x7FFu;
    uint32_t addr_field = pica_regs[0x95] & 0x0FFFFFFFu;
    uint32_t fmt = pica_regs[0x96] & 0xFu;
    uint32_t wrap_s = (pica_regs[0x93] >> 12) & 7u;
    uint32_t wrap_t = (pica_regs[0x93] >> 8) & 7u;
    size_t need, raw_len;
    uint32_t va, mapped = 0;
    uint32_t hash = 2166136261u;
    PicaStage1Slot *slot = NULL;
    unsigned i, r, c;
    if (!w || !h || w > 1024u || h > 1024u || !addr_field) return 0;
    /* The image can alias the render target: deferred GL content must land in
       guest memory before the staging reads texels from it. */
    if (pica_gl_pending()) {
        uint32_t tva = guest_memory_address(addr_field * 8u);
        uint64_t ta = (uint64_t)tva,
                 tb = ta + (uint64_t)w * h * 4u,
                 ra = (uint64_t)pica_rt_addr,
                 rb = ra + (uint64_t)pica_rt_w * pica_rt_h * 4u;
        if (ta < rb && ra < tb) {
            /* t_05281b89: unit-1 staging sampling the current target. */
            pica_gl_note_target_sampled(pica_rt_w, pica_rt_h);
            pica_gl_flush_guest_rt();
        }
    }
    raw_len = pica_stage_raw_len(w, h, fmt);
    if (!raw_len) return 0;
    va = guest_memory_address(addr_field * 8u);
    if (!guest_memory_range(va, 4u, &mapped)) return 0;
    if ((uint64_t)mapped + raw_len > GUEST_RAM_SIZE) return 0;
    /* Hit: same key AND unchanged source bytes. */
    for (i = 0; i < PICA_STAGE1_SLOTS; ++i) {
        PicaStage1Slot *s = &pica_stage1_slots[i];
        if (!s->used || s->paddr_field != addr_field || s->mapped != mapped ||
            s->w != w || s->h != h || s->fmt != fmt || s->raw_len != raw_len)
            continue;
        if (memcmp(s->raw, ram + mapped, raw_len) == 0) {
            slot = s;
            ++pica_stage1_hits;
            slot->lru = ++pica_stage1_lru_tick;
            break;
        }
    }
    if (!slot) {
        /* A dirty re-decode of the same key keeps its slot (stable staging
           pointer and GL object); a new key takes a free slot, else the LRU
           one. */
        for (i = 0; i < PICA_STAGE1_SLOTS; ++i) {
            PicaStage1Slot *s = &pica_stage1_slots[i];
            if (s->used && s->paddr_field == addr_field &&
                s->mapped == mapped && s->w == w && s->h == h && s->fmt == fmt) {
                slot = s;
                break;
            }
        }
        if (!slot) {
            slot = &pica_stage1_slots[0];
            for (i = 0; i < PICA_STAGE1_SLOTS; ++i) {
                if (!pica_stage1_slots[i].used) { slot = &pica_stage1_slots[i]; break; }
                if (pica_stage1_slots[i].lru < slot->lru) slot = &pica_stage1_slots[i];
            }
        }
        /* A queued GL draw may still read this slot's staged bytes. */
        pica_gl_split_sync();
        need = (size_t)w * h * 4u;
        if (slot->staged_len < need) {
            unsigned char *grown = (unsigned char *)realloc(slot->staged, need);
            if (!grown) return 0;
            slot->staged = grown;
            slot->staged_len = need;
        }
        if (slot->raw_len != raw_len) {
            unsigned char *grown = (unsigned char *)realloc(slot->raw, raw_len);
            if (!grown) return 0;
            slot->raw = grown;
            slot->raw_len = raw_len;
        }
        for (r = 0; r < h; ++r) {
            for (c = 0; c < w; ++c) {
                uint8_t rr = 0, gg = 0, bb = 0, aa = 0;
                unsigned char *p = slot->staged + ((size_t)r * w + c) * 4u;
                if (!pica_sample_tex_unit_exact(1u, ((float)c + 0.5f) / (float)w,
                                          ((float)r + 0.5f) / (float)h,
                                          &rr, &gg, &bb, &aa))
                    return 0;
                p[0] = rr; p[1] = gg; p[2] = bb; p[3] = aa;
                hash = (hash ^ rr) * 16777619u;
                hash = (hash ^ gg) * 16777619u;
                hash = (hash ^ bb) * 16777619u;
                hash = (hash ^ aa) * 16777619u;
            }
        }
        memcpy(slot->raw, ram + mapped, raw_len);
        slot->used = 1;
        slot->paddr_field = addr_field;
        slot->mapped = mapped;
        slot->w = w; slot->h = h; slot->fmt = fmt;
        slot->hash = hash ? hash : 1u;
        ++slot->gen;
        ++pica_stage1_decodes;
        slot->lru = ++pica_stage1_lru_tick;
    }
    pica_gl_st.tex1_rgba = slot->staged;
    pica_gl_st.tex1_w = (int)w;
    pica_gl_st.tex1_h = (int)h;
    pica_gl_st.tex1_hash = slot->hash;
    pica_gl_st.tex1_slot = (unsigned)(slot - pica_stage1_slots);
    pica_gl_st.tex1_gen = slot->gen;
    pica_gl_st.wrap_s1 = wrap_s;
    pica_gl_st.wrap_t1 = wrap_t;
    pica_gl_st.mag_filter1 = (pica_regs[0x93] >> 1) & 1u;
    pica_gl_st.min_filter1 = (pica_regs[0x93] >> 2) & 1u;
    pica_gl_st.mip_filter1 = (pica_regs[0x93] >> 24) & 1u;
    return 1;
}

/* Set when texture0's wrap modes can be reproduced by the GL sampler
   (0=clamp-to-edge, 2=repeat, 3=mirror; t_77ac52ce).  Only mode 1
   (clamp-to-border) still forces the software sampler. */
static int pica_gl_tex_wrap_ok;

static int pica_gl_prepare_draw(void) {
    pica_gl_draw_prepared = 0;
    if (!pica_gl_active()) { pica_swfall(SWF_GL_INACTIVE, 0); return 0; }
    /* t_c69e34a1: close the deferred GL episode BEFORE the guest renders into a
       different render target.  The batched backend keeps ONE FBO and keys it
       by target size, and the write-back that a target switch triggers inside
       pica_gl_draw resolves the guest buffer through the pica_rt_* and
       pica_depth_* globals — which pica_bind_framebuffer() has already pointed
       at the NEW target.
       The finished episode is then written into the wrong buffer while its own
       target keeps only the guest's clear: the town-map screen renders the
       world into 1F070800 and interleaves four small offscreen targets, so the
       3D frame was flushed into the next target and the top screen came out
       black (DQ8_GL_BATCH=0, which flushes per draw with the pointers still
       correct, rendered it perfectly).  pica_gl_flush_guest_rt() points
       pica_rt_* back at the episode's own target around the flush, so closing
       the episode here is correct by construction — and it also preserves the
       target's pixels across the pgl_ensure_rt() re-allocation the switch
       causes. */
    if (pica_gl_pending() &&
        (pica_rt_addr != pica_gl_ep_addr || pica_rt_w != pica_gl_ep_w ||
         pica_rt_h != pica_gl_ep_h || pica_rt_format != pica_gl_ep_fmt ||
         pica_depth_addr != pica_gl_ep_depth_addr ||
         pica_depth_bpp != pica_gl_ep_depth_bpp)) {
        /* t_7d18d1b6 (diagnostic, DQ8_WB_TRACE=1): WHO closes the episode and
           which field of the target identity moved.  A flush for a benign
           field (same colour target, only the depth binding moved) is the F3
           flush-count target. */
        if (pgl_wb_trace()) {
            fprintf(stderr, "FLUSHWHY switch ep=(%08X %ux%u fmt=%u z=%08X bpp=%u) "
                    "new=(%08X %ux%u fmt=%u z=%08X bpp=%u) d=%d%d%d%d%d%d\n",
                    pica_gl_ep_addr, pica_gl_ep_w, pica_gl_ep_h, pica_gl_ep_fmt,
                    pica_gl_ep_depth_addr, pica_gl_ep_depth_bpp,
                    pica_rt_addr, pica_rt_w, pica_rt_h, pica_rt_format,
                    pica_depth_addr, pica_depth_bpp,
                    pica_rt_addr != pica_gl_ep_addr, pica_rt_w != pica_gl_ep_w,
                    pica_rt_h != pica_gl_ep_h, pica_rt_format != pica_gl_ep_fmt,
                    pica_depth_addr != pica_gl_ep_depth_addr,
                    pica_depth_bpp != pica_gl_ep_depth_bpp);
            pgl_fl_reason = 1;
        }
        pica_gl_flush_guest_rt();
        pgl_fl_reason = 0;
    }
    /* Depth test/write runs on the GPU too (R32F integer-exact z); the only
       requirement is a bound, mappable depth buffer. */
    if (pica_gl_depth_active() && !pica_gl_host.depth_raw) {
        pica_swfall(SWF_DEPTH_UNMAPPABLE, 0);
        return 0;
    }
    memset(&pica_gl_st, 0, sizeof pica_gl_st);
    pica_gl_st.rt_w = pica_rt_w;
    pica_gl_st.rt_h = pica_rt_h;
    pica_gl_st.rt_bpp = pica_color_bpp(pica_rt_format);
    if (pica_gl_depth_active()) {
        uint32_t mask = pica_regs[0x107];
        pica_gl_st.depth_on = 1;
        pica_gl_st.depth_test = (mask & 1u) ? 1 : 0;
        pica_gl_st.depth_write =
            ((mask & (1u << 12)) && pica_depth_allow_write) ? 1 : 0;
        pica_gl_st.depth_func = (mask >> 4) & 7u;
        pica_gl_st.depth_zbuf = (pica_regs[0x6D] & 1u) ? 1 : 0;
        pica_gl_st.depth_scale = pica_f24_to_f32(pica_regs[0x4D]);
        pica_gl_st.depth_offset = pica_f24_to_f32(pica_regs[0x4E]);
        pica_gl_st.depth_bpp = pica_depth_bpp;
        pica_gl_st.depth_zmax =
            (pica_depth_bpp == 2u) ? 65535.0f : 16777215.0f;
    }
    pica_gl_st.color_mask = (unsigned char)((pica_regs[0x107] >> 8) & 15u);
    if (pica_regs[0x104] & 1u) {
        pica_gl_st.alpha_test = 1;
        pica_gl_st.alpha_func = (pica_regs[0x104] >> 4) & 7u;
        pica_gl_st.alpha_ref = (float)((pica_regs[0x104] >> 8) & 255u);
    }
    if (pica_regs[0x100] & 256u) {
        /* PICA factor/equation codes go to the fragment shader unchanged;
           it reproduces pica_write_fragment's blend math exactly (all
           factors 0..15 and equations 0..7 are expressible). */
        uint32_t cfg = pica_regs[0x101];
        pica_gl_st.blend = 1;
        pica_gl_st.blend_eq_rgb = cfg & 7u;
        pica_gl_st.blend_eq_a = (cfg >> 8) & 7u;
        pica_gl_st.blend_src_rgb = (cfg >> 16) & 15u;
        pica_gl_st.blend_dst_rgb = (cfg >> 20) & 15u;
        pica_gl_st.blend_src_a = (cfg >> 24) & 15u;
        pica_gl_st.blend_dst_a = (cfg >> 28) & 15u;
        pica_rgba(pica_regs[0x103], pica_gl_st.blend_const);
    }
    if ((pica_regs[0x80] & 7u) == 0u) {
        /* TEV stage 0 config carries no texture/combiner bits.  The software
           rasterizer still runs the full 6-stage pica_tev() chain on the
           INTERPOLATED primary colour (pica_fragment), so evaluate exactly
           that here too (t_77ac52ce).  The old CPU-side "SOLID" shortcut
           flat-shaded one vertex colour per triangle and therefore had to
           refuse every draw whose vertex colours differed, which pushed whole
           full-screen passes back to the software rasterizer. */
        pica_gl_build_tev(&pica_gl_st.tev);
        pica_gl_tex_wrap_ok = 1; /* tev.tex_en is 0 here */
        pica_gl_st.mode = PICA_GL_TEV;
    } else if ((pica_regs[0x80] & 7u) != 0u) {
        /* Any TEV chain: the fragment shader evaluates the 6 stages exactly
           like pica_tev (see pgl_fs_src). */
        unsigned ws, wt;
        pica_gl_build_tev(&pica_gl_st.tev);
        /* t_e216fc37: the GL fragment shader translates the whole TEV chain
           but can only sample TEXTURE UNIT 0 (one staged image, one sampler,
           one set of wrap registers).  A chain that reads unit 1/2 (TEV
           source 4/5) is refused in pica_flush_triangles() so the draw is
           rasterized by the software path (which samples both units); the
           refusal is NOT done here because a prepare-time refusal leaves the
           draw ordered against the deferred GL episode differently and the
           ground quad then disappeared behind the sky pass. */
        ws = (pica_regs[0x83] >> 12) & 7u;
        wt = (pica_regs[0x83] >> 8) & 7u;
        pica_gl_st.wrap_s = ws;
        pica_gl_st.wrap_t = wt;
        /* t_c043d7f7: the guest's mag/min/mip filter bits, applied to the GL
           sampler per draw (the upload cache is keyed on image bytes, not on
           filter state, so these are set on every bind, not only on upload). */
        pica_gl_st.mag_filter = (pica_regs[0x83] >> 1) & 1u;
        pica_gl_st.min_filter = (pica_regs[0x83] >> 2) & 1u;
        pica_gl_st.mip_filter = (pica_regs[0x83] >> 24) & 1u;
        pica_gl_st.lod_word = pica_regs[0x84];
        /* PICA wrap codes 0/2/3 are exactly what the GL sampler does with the
           NEAREST-filtered staged texture (the shader's uniform sampling lands
           on identical texels), so u,v are free to leave [0,1]. */
        pica_gl_tex_wrap_ok = (ws == 0u || ws == 2u || ws == 3u) &&
                              (wt == 0u || wt == 2u || wt == 3u);
        if (pica_gl_st.tev.tex_en && !pica_gl_stage_texture()) {
            pica_swfall(SWF_TEX_STAGE_FAIL, 0);
            return 0;
        }
        /* t_e216fc37: TEXTURE UNIT 1 (TEV source 4).  Staged from the unit-1
           register block (0x91/0x92/0x93/0x95/0x96) and sampled with
           texcoord1 by the GL fragment shader (GL texture unit 3).  Only when
           the unit is enabled AND this draw's chain actually reads source 4,
           so draws that never touch it pay one register test. */
        pica_gl_st.tex1_en = 0;
        if ((pica_regs[0x80] & 2u) && pica_tev_reads_texunit(1u) &&
            pica_tev_tex1_on()) {
            unsigned ws1 = (pica_regs[0x93] >> 12) & 7u;
            unsigned wt1 = (pica_regs[0x93] >> 8) & 7u;
            if ((ws1 == 0u || ws1 == 2u || ws1 == 3u) &&
                (wt1 == 0u || wt1 == 2u || wt1 == 3u) &&
                pica_gl_stage_texture1()) {
                pica_gl_st.tex1_en = 1;
            } else {
                /* A sampler mode the staged-image GL sampling cannot
                   reproduce (border colour / repeat-2 / repeat-3) or a
                   staging failure: keep this draw on the software
                   rasterizer, which samples unit 1 through
                   pica_sample_tex_unit(1, ...) exactly. */
                pica_swfall(SWF_TEV_TEX1, 0);
                return 0;
            }
        }
        /* t_fa78ea1f: staged-texture census for the UI-overlay investigation.
           Prints the texture identity plus how much of the staged image is
           black / transparent, so a draw whose texture content is empty can be
           told apart from a draw that never happened. */
        if (pica_gl_st.tev.tex_en && pica_gl_st.tex_rgba &&
            dq8_env_on("DQ8_GL_TEXLOG", &pvs_texlog_gate)) {
            static unsigned long long texlog_n;
            unsigned long long tk, tot = (unsigned long long)pica_gl_st.tex_w *
                                         (unsigned long long)pica_gl_st.tex_h, n = 0, darkn = 0, an = 0;
            unsigned long long rsum = 0;
            for (tk = 0; tk < tot; tk += 4u) {
                const unsigned char *t = pica_gl_st.tex_rgba + tk * 4u;
                unsigned m = t[0] > t[1] ? (t[0] > t[2] ? t[0] : t[2])
                                         : (t[1] > t[2] ? t[1] : t[2]);
                ++n;
                if (m <= 16u) ++darkn;
                if (t[3]) ++an;
                rsum += m;
            }
            fprintf(stderr, "TEXLOG #%llu addr=%08X %dx%d fmt=%u slot=%u gen=%u "
                    "dark=%.1f%% alpha_nz=%.1f%% meanmax=%.1f\n",
                    ++texlog_n, (unsigned)((pica_regs[0x85] & 0x0FFFFFFFu) * 8u),
                    pica_gl_st.tex_w, pica_gl_st.tex_h, (unsigned)(pica_regs[0x8E] & 0xFu),
                    pica_gl_st.tex_slot, pica_gl_st.tex_gen,
                    n ? 100.0 * (double)darkn / (double)n : -1.0,
                    n ? 100.0 * (double)an / (double)n : -1.0,
                    n ? (double)rsum / (double)n : -1.0);
        }
        pica_gl_st.mode = PICA_GL_TEV;
    } else {
        pica_swfall(SWF_MODE_UNSUPPORTED, 0);
        return 0;
    }
    pica_gl_draw_prepared = 1;
    return 1;
}

/* ---- F1: redundant-raster elision cache (task t_4545f58b) -----------------
   The guest re-issues identical PICA draws for screens whose content does not
   change (save dialog, logo holds): the PERF audit (finding F1) measured
   ~140 draws/s of software raster work for 0.32 unique content frames/s.
   This cache skips the software rasterization of a draw whose RESULT is
   already in the render target, while leaving all guest-visible state-machine
   behaviour untouched (command lists, register writes, transfers, IRQs,
   vblanks all keep running at the same rate).

   Correctness model (no heuristics):
       result(draw) = F(inputs, before-content of every buffer it touches)
   with inputs = PICA register file + VS program/swizzle/uniforms/default
   attributes + the post-VS triangle batch + tex0 raw bytes.  At flush time
   every draw computes, FRESH each time:
       sig     = hash(inputs)
       region  = clamped union of the triangles' raster bboxes
                 (a superset of every pixel the draw can read or write -
                 identical clamp formulas to pica_raster_triangle)
       before  = hash(color + depth bytes over region)
   A cache entry stores (sig, region, before) plus the sparse net pixel delta
   the draw applied (raw color + depth bytes).  A hit means: same inputs and
   bit-identical buffer content over the whole read/write footprint, so
   re-running the draw would apply the identical delta again; applying the
   stored delta is bit-identical to re-rasterizing (the software rasterizer is
   deterministic).  Because everything is re-hashed on every draw, CPU-side
   stores, GSP memory fills and display transfers into textures or render
   targets can never invalidate a stale hash.

   Only the software path is elided: a draw handed to the GL backend bypasses
   the cache entirely (no lookup, no store), so GL behaviour is unchanged.

   DQ8_ELIDE_RASTER=0  disables the cache (byte-for-byte pre-F1 behaviour).
   DQ8_ELIDE_TRACE=1   prints periodic hit/miss/delta counters on stderr. */
#define ELIDE_HMIX(h, w) do { (h) = ((h) ^ (uint64_t)(w)) * 0x100000001B3ull; } while (0)
#define ELIDE_SLOTS 48
#define ELIDE_MAX_BYTES (64ull * 1024ull * 1024ull)
typedef struct {
    uint16_t x, y;        /* raster coordinates */
    uint8_t c[4];         /* raw color bytes at this pixel (layout-dependent) */
    uint8_t d[4];         /* raw depth bytes at this pixel */
    uint8_t have_c, have_d;
} ElidePx;
typedef struct {
    uint64_t sig, before;
    uint32_t x0, y0, x1, y1;
    uint64_t tick;
    ElidePx *px;
    uint32_t npx;
    int live;
} ElideEntry;
static ElideEntry elide_tab[ELIDE_SLOTS];
static uint64_t elide_tick;
/* Data symbols (read by the joint harness via /proc/<pid>/mem). */
static uint64_t elide_hits, elide_misses, elide_replay_px, elide_captured_px;
static uint64_t elide_live_bytes, elide_uncacheable;
static uint64_t elide_verify_checks, elide_verify_mismatches;
static void elide_trace_report(void) {
    static double last;
    static int armed = -1;
    double now;
    if (armed < 0) armed = getenv("DQ8_ELIDE_TRACE") != NULL;
    if (!armed) return;
    now = dq8_layer_t();
    if (last == 0.0) { last = now; return; }
    if (now - last < 2.0) return;
    fprintf(stderr, "ELIDE t=%.1f hits=%llu misses=%llu replay_px=%llu "
            "captured_px=%llu live_bytes=%llu uncacheable=%llu verify=%llu/%llu\n",
            now, (unsigned long long)elide_hits, (unsigned long long)elide_misses,
            (unsigned long long)elide_replay_px, (unsigned long long)elide_captured_px,
            (unsigned long long)elide_live_bytes, (unsigned long long)elide_uncacheable,
            (unsigned long long)elide_verify_checks,
            (unsigned long long)elide_verify_mismatches);
    last = now;
}
static int elide_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("DQ8_ELIDE_RASTER");
        on = (e && e[0] == '0' && !e[1]) ? 0 : 1;
        /* Per-fragment/write traces must observe every write and the exact
           per-draw extents; disable elision while any of them is armed. */
        if (on) {
            static const char *const kill[] = {
                "DQ8_TRACE_PX", "DQ8_TRACE_RECT", "DQ8_TRACE_LAYER",
                "DQ8_ZWRITE0_TRACE", NULL
            };
            unsigned i;
            for (i = 0; kill[i]; i++) {
                const char *v = getenv(kill[i]);
                if (v && v[0]) { on = 0; break; }
            }
        }
    }
    return on;
}
static uint64_t elide_hash_bytes(uint64_t h, const uint8_t *p, size_t n) {
    while (n >= 8u) {
        uint64_t w;
        memcpy(&w, p, 8);
        ELIDE_HMIX(h, w);
        p += 8;
        n -= 8;
    }
    while (n--) ELIDE_HMIX(h, *p++);
    return h;
}
/* Per-draw scratch: snapshot buffers are grow-only and reused. */
typedef struct {
    uint64_t sig, before;
    uint32_t x0, y0, x1, y1;
    uint32_t mapped_c, mapped_d, cbpp, dbpp;
    int depth_on, cacheable;
} ElideCur;
static ElideCur elide_cur;
static uint8_t *elide_c_before, *elide_d_before;
static size_t elide_c_cap, elide_d_cap;
static int elide_bufs(size_t px) {
    size_t need = px * 4u;
    uint8_t *g;
    if (need > elide_c_cap) {
        g = (uint8_t *)realloc(elide_c_before, need);
        if (!g) return 0;
        elide_c_before = g;
        elide_c_cap = need;
    }
    if (need > elide_d_cap) {
        g = (uint8_t *)realloc(elide_d_before, need);
        if (!g) return 0;
        elide_d_before = g;
        elide_d_cap = need;
    }
    return 1;
}
/* tex0 raw-byte span: every pica_sample_tex0 format reads inside the Morton
   tile array ((w+7)/8) x ((h+7)/8) tiles of tile_size bytes. */
static int elide_tex0_span(uint32_t *va_out, uint32_t *span_out) {
    uint32_t addr_field = pica_regs[0x85] & 0x0FFFFFFFu;
    uint32_t w = (pica_regs[0x82] >> 16) & 0x7FFu, h = pica_regs[0x82] & 0x7FFu;
    uint32_t fmt = pica_regs[0x8E] & 0xFu, tile_size, span;
    switch (fmt) {
    case 0u: tile_size = 256u; break;
    case 1u: tile_size = 192u; break;
    case 2u: case 3u: case 4u: case 5u: case 6u: tile_size = 128u; break;
    case 7u: case 8u: case 9u: tile_size = 64u; break;
    case 10u: case 11u: case 12u: tile_size = 32u; break;
    case 13u: tile_size = 64u; break;
    default: return 0;
    }
    if (!addr_field || !w || !h) return 0;
    span = ((w + 7u) / 8u) * ((h + 7u) / 8u) * tile_size;
    *va_out = guest_memory_address(addr_field * 8u);
    *span_out = span;
    return 1;
}
static uint64_t elide_sig(const PicaVertex *tri, uint32_t n) {
    uint64_t h = 14695981039346656037ull;
    h = elide_hash_bytes(h, (const uint8_t *)pica_regs, sizeof pica_regs);
    h = elide_hash_bytes(h, (const uint8_t *)vs_program, (size_t)vs_program_size * 4u);
    h = elide_hash_bytes(h, (const uint8_t *)vs_swizzle, (size_t)vs_swizzle_size * 4u);
    h = elide_hash_bytes(h, (const uint8_t *)vs_uniform_f, sizeof vs_uniform_f);
    h = elide_hash_bytes(h, (const uint8_t *)vs_default_attrib, sizeof vs_default_attrib);
    h = elide_hash_bytes(h, (const uint8_t *)tri, (size_t)n * sizeof(PicaVertex));
    /* Framebuffer/depth globals: pica_bind_framebuffer keeps the previous
       size when a dim register parse fails, so these are NOT pure functions
       of the register file - fold them in explicitly. */
    {
        uint64_t w[8] = { pica_rt_addr, ((uint64_t)pica_rt_w << 32) | pica_rt_h,
                          pica_rt_format, pica_depth_addr,
                          ((uint64_t)pica_depth_bpp << 32) | pica_depth_bits,
                          pica_depth_allow_write, (uint64_t)pica_depth_bound, 0 };
        h = elide_hash_bytes(h, (const uint8_t *)w, sizeof w);
    }
    if (pica_regs[0x80] & 1u) {
        uint32_t va, span, mapped;
        if (!elide_tex0_span(&va, &span)) return 0;   /* uncacheable sampler config */
        if (!guest_memory_range(va, span, &mapped)) return 0;
        h = elide_hash_bytes(h, ram + mapped, span);
    }
    return h;
}
/* Clamped raster bbox union - mirrors pica_raster_triangle's lx/ly/hx/hy
   formulas exactly, so the region covers every attempted fragment (all color
   and depth reads/writes happen only there). */
static int elide_region(const PicaVertex *tri, uint32_t n, ElideCur *cur) {
    float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
    uint32_t i;
    int lx, ly, hx, hy, any = 0;
    for (i = 0; i < n; i++) {
        float x = tri[i].x, y = tri[i].y;
        if (!isfinite(x) || !isfinite(y)) continue;
        if (x < minx) minx = x;
        if (x > maxx) maxx = x;
        if (y < miny) miny = y;
        if (y > maxy) maxy = y;
        any = 1;
    }
    if (!any) return 0;
    lx = (int)fmaxf(0, floorf(minx));
    ly = (int)fmaxf(0, floorf(miny));
    hx = (int)fminf((float)pica_rt_w - 1.0f, ceilf(maxx));
    hy = (int)fminf((float)pica_rt_h - 1.0f, ceilf(maxy));
    if (lx > hx || ly > hy) return 0;
    cur->x0 = (uint32_t)lx; cur->y0 = (uint32_t)ly;
    cur->x1 = (uint32_t)hx; cur->y1 = (uint32_t)hy;
    return 1;
}
/* Hash the color+depth bytes of the region (read-only pass; Morton offsets). */
static uint64_t elide_hash_region(ElideCur *cur) {
    uint64_t h = 14695981039346656037ull;
    uint8_t *cbase = ram + cur->mapped_c, *dbase = ram + cur->mapped_d;
    uint32_t x, y, cb = cur->cbpp, db = cur->dbpp;
    for (y = cur->y0; y <= cur->y1; y++) {
        uint32_t my = pica_rt_h - 1u - y;
        for (x = cur->x0; x <= cur->x1; x++) {
            uint64_t w = 0;
            memcpy(&w, cbase + pica_rt_off(x, my), cb);
            ELIDE_HMIX(h, w);
            if (cur->depth_on) {
                w = 0;
                memcpy(&w, dbase + pica_depth_off(x, my), db);
                ELIDE_HMIX(h, w);
            }
        }
    }
    return h;
}
static void elide_copy_region(ElideCur *cur) {
    uint8_t *cbase = ram + cur->mapped_c, *dbase = ram + cur->mapped_d;
    uint32_t x, y, i = 0, cb = cur->cbpp, db = cur->dbpp;
    for (y = cur->y0; y <= cur->y1; y++) {
        uint32_t my = pica_rt_h - 1u - y;
        for (x = cur->x0; x <= cur->x1; x++, i++) {
            memcpy(elide_c_before + (size_t)i * 4u, cbase + pica_rt_off(x, my), cb);
            if (cur->depth_on)
                memcpy(elide_d_before + (size_t)i * 4u, dbase + pica_depth_off(x, my), db);
        }
    }
}
static int elide_lookup(const ElideCur *cur) {
    unsigned i;
    for (i = 0; i < ELIDE_SLOTS; i++) {
        ElideEntry *e = &elide_tab[i];
        if (e->live && e->sig == cur->sig && e->before == cur->before &&
            e->x0 == cur->x0 && e->y0 == cur->y0 &&
            e->x1 == cur->x1 && e->y1 == cur->y1) {
            e->tick = ++elide_tick;
            return (int)i;
        }
    }
    return -1;
}
static void elide_apply(const ElideEntry *e) {
    uint8_t *cbase = ram + elide_cur.mapped_c, *dbase = ram + elide_cur.mapped_d;
    uint32_t i;
    for (i = 0; i < e->npx; i++) {
        const ElidePx *p = &e->px[i];
        uint32_t my = pica_rt_h - 1u - p->y;
        if (p->have_c) memcpy(cbase + pica_rt_off(p->x, my), p->c, elide_cur.cbpp);
        if (p->have_d && elide_cur.depth_on)
            memcpy(dbase + pica_depth_off(p->x, my), p->d, elide_cur.dbpp);
    }
    elide_replay_px += e->npx;
}
static void elide_evict(uint64_t need) {
    while (elide_live_bytes + need > ELIDE_MAX_BYTES) {
        unsigned i, worst = ELIDE_SLOTS;
        uint64_t oldest = ~0ull;
        for (i = 0; i < ELIDE_SLOTS; i++)
            if (elide_tab[i].live && elide_tab[i].tick < oldest) {
                oldest = elide_tab[i].tick;
                worst = i;
            }
        if (worst == ELIDE_SLOTS) return;
        elide_live_bytes -= (uint64_t)elide_tab[worst].npx * sizeof(ElidePx);
        free(elide_tab[worst].px);
        memset(&elide_tab[worst], 0, sizeof elide_tab[worst]);
    }
}
static void elide_store(void) {
    /* Diff the current region against the pre-raster snapshot. */
    uint8_t *cbase = ram + elide_cur.mapped_c, *dbase = ram + elide_cur.mapped_d;
    uint32_t x, y, i = 0, cb = elide_cur.cbpp, db = elide_cur.dbpp;
    size_t maxpx = (size_t)(elide_cur.x1 - elide_cur.x0 + 1u) *
                   (size_t)(elide_cur.y1 - elide_cur.y0 + 1u);
    ElidePx *px = (ElidePx *)malloc(maxpx * sizeof(ElidePx));
    uint32_t npx = 0;
    int slot = -1;
    unsigned s;
    if (!px) { ++elide_uncacheable; return; }
    for (y = elide_cur.y0; y <= elide_cur.y1; y++) {
        uint32_t my = pica_rt_h - 1u - y;
        for (x = elide_cur.x0; x <= elide_cur.x1; x++, i++) {
            const uint8_t *cp = cbase + pica_rt_off(x, my);
            const uint8_t *dp = elide_cur.depth_on ? dbase + pica_depth_off(x, my) : NULL;
            if (memcmp(cp, elide_c_before + (size_t)i * 4u, cb) == 0 &&
                (!elide_cur.depth_on ||
                 memcmp(dp, elide_d_before + (size_t)i * 4u, db) == 0))
                continue;
            px[npx].x = (uint16_t)x;
            px[npx].y = (uint16_t)y;
            memset(px[npx].c, 0, 4);
            memset(px[npx].d, 0, 4);
            memcpy(px[npx].c, cp, cb);
            px[npx].have_c = 1;
            px[npx].have_d = 0;
            if (elide_cur.depth_on) {
                memcpy(px[npx].d, dp, db);
                px[npx].have_d = 1;
            }
            npx++;
        }
    }
    elide_captured_px += npx;
    if (!npx) { free(px); px = NULL; }
    /* Replace a matching (stale) entry first, else take a free slot. */
    for (s = 0; s < ELIDE_SLOTS; s++)
        if (elide_tab[s].live && elide_tab[s].sig == elide_cur.sig &&
            elide_tab[s].x0 == elide_cur.x0 && elide_tab[s].y0 == elide_cur.y0 &&
            elide_tab[s].x1 == elide_cur.x1 && elide_tab[s].y1 == elide_cur.y1) {
            slot = (int)s;
            break;
        }
    if (slot < 0)
        for (s = 0; s < ELIDE_SLOTS; s++)
            if (!elide_tab[s].live) { slot = (int)s; break; }
    if (slot < 0) {
        uint64_t oldest = ~0ull;
        for (s = 0; s < ELIDE_SLOTS; s++)
            if (elide_tab[s].tick < oldest) { oldest = elide_tab[s].tick; slot = (int)s; }
    }
    {
        ElideEntry *e = &elide_tab[slot];
        if (e->live) {
            elide_live_bytes -= (uint64_t)e->npx * sizeof(ElidePx);
            free(e->px);
            /* Retire the slot BEFORE evicting: elide_evict may otherwise
               pick this very (still-live, oldest-tick) entry and free the
               just-freed pixel array again (double free). */
            e->px = NULL;
            e->npx = 0;
            e->live = 0;
        }
        elide_evict((uint64_t)npx * sizeof(ElidePx));
        e->sig = elide_cur.sig;
        e->before = elide_cur.before;
        e->x0 = elide_cur.x0; e->y0 = elide_cur.y0;
        e->x1 = elide_cur.x1; e->y1 = elide_cur.y1;
        e->px = px;
        e->npx = npx;
        e->tick = ++elide_tick;
        e->live = 1;
        elide_live_bytes += (uint64_t)npx * sizeof(ElidePx);
    }
}
/* Classify the draw and compute everything needed for lookup or capture.
   Returns 1 when the cache can serve (or record) this draw. */
static int elide_prepare(const PicaVertex *tri, uint32_t n) {
    uint64_t sig;
    size_t px;
    elide_cur.cacheable = 0;
    if (!n || !pica_rt_w || !pica_rt_h) return 0;
    if (!elide_region(tri, n, &elide_cur)) return 0;
    sig = elide_sig(tri, n);
    if (!sig) { ++elide_uncacheable; return 0; }
    elide_cur.sig = sig;
    elide_cur.cbpp = pica_color_bpp(pica_rt_format);
    elide_cur.dbpp = pica_depth_bpp;
    if (!guest_memory_range(pica_rt_addr,
                            (uint64_t)pica_rt_w * pica_rt_h * 4u,
                            &elide_cur.mapped_c)) {
        ++elide_uncacheable;
        return 0;
    }
    elide_cur.depth_on = pica_depth_bound && pica_depth_range_ok();
    if (elide_cur.depth_on &&
        !guest_memory_range(pica_depth_addr,
                            (uint64_t)pica_rt_w * pica_rt_h * pica_depth_bpp,
                            &elide_cur.mapped_d))
        elide_cur.depth_on = 0;
    px = (size_t)(elide_cur.x1 - elide_cur.x0 + 1u) *
         (size_t)(elide_cur.y1 - elide_cur.y0 + 1u);
    if (!elide_bufs(px)) { ++elide_uncacheable; return 0; }
    elide_cur.before = elide_hash_region(&elide_cur);
    elide_cur.cacheable = 1;
    return 1;
}
/* DQ8_ELIDE_VERIFY=1 self-check: on every cache hit the draw is ALSO
   re-rasterized from the pre-hit buffer state and compared byte-for-byte
   against the replayed result.  Same-run ground truth for the hit path
   (no cross-run timing/blink-phase confounds).  Counters are the
   elide_verify_checks/_mismatches data symbols declared above. */
static uint8_t *elide_v_raster, *elide_v_before;
static size_t elide_v_raster_cap, elide_v_before_cap;
static int elide_verify_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("DQ8_ELIDE_VERIFY");
        on = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return on;
}
static void elide_verify_hit(const PicaVertex *tri, uint32_t n, const ElideEntry *e) {
    ElideCur *cur = &elide_cur;
    size_t px = (size_t)(cur->x1 - cur->x0 + 1u) * (size_t)(cur->y1 - cur->y0 + 1u);
    size_t need = px * 4u;
    uint8_t *cbase = ram + cur->mapped_c, *dbase = ram + cur->mapped_d;
    uint32_t x, y, i = 0;
    uint64_t mismatch = 0;
    size_t doff = need;   /* depth plane lives past the color plane */
    if (need * 2u > elide_v_raster_cap) {
        uint8_t *g = (uint8_t *)realloc(elide_v_raster, need * 2u);
        if (!g) return;
        elide_v_raster = g;
        elide_v_raster_cap = need * 2u;
    }
    if (need * 2u > elide_v_before_cap) {
        uint8_t *g = (uint8_t *)realloc(elide_v_before, need * 2u);
        if (!g) return;
        elide_v_before = g;
        elide_v_before_cap = need * 2u;
    }
    /* 1. preserve the pre-hit region (B) */
    for (y = cur->y0; y <= cur->y1; y++) {
        uint32_t my = pica_rt_h - 1u - y;
        for (x = cur->x0; x <= cur->x1; x++, i++) {
            memcpy(elide_v_before + (size_t)i * 4u, cbase + pica_rt_off(x, my), cur->cbpp);
            if (cur->depth_on)
                memcpy(elide_v_before + doff + (size_t)i * 4u, dbase + pica_depth_off(x, my), cur->dbpp);
        }
    }
    /* 2. rasterize the draw for real (from B) and capture the region */
    for (i = 0; i < n; i += 3u)
        pica_raster_triangle(tri[i], tri[i + 1], tri[i + 2]);
    i = 0;
    for (y = cur->y0; y <= cur->y1; y++) {
        uint32_t my = pica_rt_h - 1u - y;
        for (x = cur->x0; x <= cur->x1; x++, i++) {
            memcpy(elide_v_raster + (size_t)i * 4u, cbase + pica_rt_off(x, my), cur->cbpp);
            if (cur->depth_on)
                memcpy(elide_v_raster + doff + (size_t)i * 4u, dbase + pica_depth_off(x, my), cur->dbpp);
        }
    }
    /* 3. restore B, then replay the cached delta */
    i = 0;
    for (y = cur->y0; y <= cur->y1; y++) {
        uint32_t my = pica_rt_h - 1u - y;
        for (x = cur->x0; x <= cur->x1; x++, i++) {
            memcpy(cbase + pica_rt_off(x, my), elide_v_before + (size_t)i * 4u, cur->cbpp);
            if (cur->depth_on)
                memcpy(dbase + pica_depth_off(x, my), elide_v_before + doff + (size_t)i * 4u, cur->dbpp);
        }
    }
    elide_apply(e);
    /* 4. compare the replayed region against the re-rasterized one */
    i = 0;
    for (y = cur->y0; y <= cur->y1; y++) {
        uint32_t my = pica_rt_h - 1u - y;
        for (x = cur->x0; x <= cur->x1; x++, i++) {
            if (memcmp(cbase + pica_rt_off(x, my), elide_v_raster + (size_t)i * 4u, cur->cbpp))
                ++mismatch;
            if (cur->depth_on &&
                memcmp(dbase + pica_depth_off(x, my), elide_v_raster + doff + (size_t)i * 4u, cur->dbpp))
                ++mismatch;
        }
    }
    ++elide_verify_checks;
    if (mismatch) {
        ++elide_verify_mismatches;
        if (elide_verify_mismatches <= 8u)
            fprintf(stderr, "ELIDE VERIFY MISMATCH draw=%llu region=%u,%u..%u,%u "
                    "pixels=%llu sig=%016llX before=%016llX\n",
                    (unsigned long long)pica_draw_count, cur->x0, cur->y0,
                    cur->x1, cur->y1, (unsigned long long)mismatch,
                    (unsigned long long)cur->sig, (unsigned long long)cur->before);
    }
}
static void pica_flush_triangles(void);

static void pica_queue_triangle(PicaVertex a, PicaVertex b, PicaVertex c) {
    float area;
    ++pica_draw_log_tris;
    /* t_150574cd: face culling.  The winding tally is taken on every triangle
       the draw submits (pre-filter), so DQ8_TRACE_TEXDRAW still reports the
       guest's own cw/ccw mix; the filter below then drops the back faces the
       guest's GPUREG 0x40 mode culls. */
    area = pica_edge(&a, &b, c.x, c.y);
    if (area > 0.0f) ++pica_draw_log_fccw; else ++pica_draw_log_fcw;
    if (!pica_cull_keep(area)) { ++pica_cull_dropped_tris; return; }
    ++pica_cull_kept_tris;
    if (pica_clst() && pica_draw_log_count >= 1000u) {
        static unsigned seen[8]; static int nseen;
        int k;
        if (!clst_dumping) {
            int dup = 0;
            for (k = 0; k < nseen; k++) if (seen[k] == pica_draw_log_count) dup = 1;
            if (!dup && nseen < 8) {
                seen[nseen++] = pica_draw_log_count;
                clst_dumping = 1;
                fprintf(stderr, "TRIDUMP begin count=%u rt=%ux%u\n",
                        pica_draw_log_count, pica_rt_w, pica_rt_h);
            }
        }
        if (clst_dumping)
            fprintf(stderr, "T %u %+.3f %+.3f %+.3f %+.3f %+.3f %+.3f\n",
                    pica_draw_log_count, a.x, a.y, b.x, b.y, c.x, c.y);
    }
    {   /* t_074669f7: per-draw triangle centroid histogram over sx — where do
           this draw's primitives actually land?  Bucket width 120 = quarter of
           the 480-wide viewport. */
        float scx = (a.x + b.x + c.x) * (1.0f / 3.0f);
        int sb = scx < 0.0f ? 0 : scx < 120.0f ? 1 : scx < 240.0f ? 2 :
                 scx < 360.0f ? 3 : scx < 480.0f ? 4 : 5;
        ++pica_thist[sb];
    }
    if (pica_tri_verts + 3u > pica_tri_cap) {
        uint32_t cap = pica_tri_cap ? pica_tri_cap * 2u : 3072u;
        PicaVertex *grown;
        while (cap < pica_tri_verts + 3u) cap *= 2u;
        grown = (PicaVertex *)realloc(pica_tri_buf,
                                      (size_t)cap * sizeof(PicaVertex));
        if (!grown) {
            pica_flush_triangles();
            pica_raster_triangle(a, b, c);
            return;
        }
        pica_tri_buf = grown;
        pica_tri_cap = cap;
    }
    pica_tri_buf[pica_tri_verts++] = a;
    pica_tri_buf[pica_tri_verts++] = b;
    pica_tri_buf[pica_tri_verts++] = c;
}

/* ---- Primitive clipping (t_0b95f1a7 black-3D near-field fix) ----
   Vertices with w<=PICA_W_EPS carry their clip-space position; a triangle
   referencing any of them is Sutherland-Hodgman-clipped against the w plane
   and the four NDC side planes (x=+-w, y=+-w), which is the visible part of
   the hardware's 7-plane clip: the side planes map exactly to the viewport
   rect (v->x=(cx/w+1)*hw+xoff), so both GL and the software rasterizer only
   ever see bounded screen coordinates.  Fully-outside triangles are dropped,
   everything else is re-derived and queued as 1..2 triangles. */
static void pica_vertex_project(PicaVertex *v) {
    float hw=pica_f24_to_f32(pica_regs[0x41]),hh=pica_f24_to_f32(pica_regs[0x43]);
    v->iw=1.0f/v->cw;
    v->zw=v->cz*v->iw;
    v->x=(v->cx*v->iw+1)*hw+(pica_regs[0x68]&1023);
    v->y=(v->cy*v->iw+1)*hh+((pica_regs[0x68]>>16)&1023);
    if(v->x>1e6f)v->x=1e6f;else if(v->x<-1e6f)v->x=-1e6f;
    if(v->y>1e6f)v->y=1e6f;else if(v->y<-1e6f)v->y=-1e6f;
}
static PicaVertex pica_vertex_lerp(const PicaVertex *a,const PicaVertex *b,float t) {
    PicaVertex o;int k;
    o.cx=a->cx+(b->cx-a->cx)*t;o.cy=a->cy+(b->cy-a->cy)*t;
    o.cz=a->cz+(b->cz-a->cz)*t;o.cw=a->cw+(b->cw-a->cw)*t;
    for(k=0;k<4;k++)o.color[k]=a->color[k]+(b->color[k]-a->color[k])*t;
    o.u=a->u+(b->u-a->u)*t;o.v=a->v+(b->v-a->v)*t;
    o.u1=a->u1+(b->u1-a->u1)*t;o.v1=a->v1+(b->v1-a->v1)*t;
    return o;
}
/* plane: 0 = w>=EPS, 1 = cx<=cw, 2 = cx>=-cw, 3 = cy<=cw, 4 = cy>=-cw */
static float pica_clip_f(const PicaVertex *v,int plane) {
    switch(plane) {
    case 0: return v->cw-PICA_W_EPS;
    case 1: return v->cw-v->cx;
    case 2: return v->cw+v->cx;
    case 3: return v->cw-v->cy;
    default: return v->cw+v->cy;
    }
}
/* DQ8_TRACE_CLIPTRI=1 (bounded): log every straddling-primitive clip outcome
   (input w's, kept vertex count, projected screen fan) into the VSDUMP file
   when one is open.  Debug-only; zero cost when the env is absent. */
static int pica_cliptri_on = -1;
static unsigned pica_cliptri_n, pica_cliptri_ndrop;
static int pica_cliptri(int drop_all) {
    if (pica_cliptri_on < 0) {
        const char *e = getenv("DQ8_TRACE_CLIPTRI");
        pica_cliptri_on = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    if (!pica_cliptri_on || !pvs_f) return 0;
    return drop_all ? (pica_cliptri_ndrop < 6u) : (pica_cliptri_n < 40u);
}
static unsigned pica_clip_poly(PicaVertex *out,const PicaVertex *in,unsigned n,int plane) {
    unsigned m=0,i;
    for(i=0;i<n;i++) {
        PicaVertex p=in[i],q=in[(i+1)%n];
        float fp=pica_clip_f(&p,plane),fq=pica_clip_f(&q,plane);
        int pin=fp>=0.0f,qin=fq>=0.0f;
        if(pin)out[m++]=p;
        if(pin!=qin) {
            float den=fp-fq,t=(den!=0.0f)?fp/den:0.5f;
            out[m++]=pica_vertex_lerp(&p,&q,t);
        }
    }
    return m;
}
/* DQ8_TRACE_CLIPSTAT=1: enabled? */
static int pica_clst(void) {
    if (clst_on < 0) {
        const char *e = getenv("DQ8_TRACE_CLIPSTAT");
        clst_on = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return clst_on;
}
static void clst_bbox_add(float *x0, float *x1, float *y0, float *y1,
                          float x, float y) {
    if (x < *x0) *x0 = x;
    if (x > *x1) *x1 = x;
    if (y < *y0) *y0 = y;
    if (y > *y1) *y1 = y;
}
static void pica_queue_triangle_clip(PicaVertex a,PicaVertex b,PicaVertex c) {
    PicaVertex p0[3],p1[16],p2[16];unsigned n=3,plane;PicaVertex *src=p0,*dst=p1;
    if (pica_clst()) {
        float wmin = a.cw < b.cw ? (a.cw < c.cw ? a.cw : c.cw)
                                 : (b.cw < c.cw ? b.cw : c.cw);
        float wmax = a.cw > b.cw ? (a.cw > c.cw ? a.cw : c.cw)
                                 : (b.cw > c.cw ? b.cw : c.cw);
        if (wmin < clst_wmin_all) clst_wmin_all = wmin;
        if (wmax > clst_wmax_all) clst_wmax_all = wmax;
    }
    if(a.cw>PICA_W_EPS && b.cw>PICA_W_EPS && c.cw>PICA_W_EPS) {
        if (pica_clst()) {
            ++clst_fastin;
            clst_bbox_add(&clst_fx0, &clst_fx1, &clst_fy0, &clst_fy1, a.x, a.y);
            clst_bbox_add(&clst_fx0, &clst_fx1, &clst_fy0, &clst_fy1, b.x, b.y);
            clst_bbox_add(&clst_fx0, &clst_fx1, &clst_fy0, &clst_fy1, c.x, c.y);
        }
        pica_queue_triangle(a,b,c);return; /* fast path: nothing to clip */
    }
    if(a.cw<=PICA_W_EPS && b.cw<=PICA_W_EPS && c.cw<=PICA_W_EPS) {
        if (pica_clst()) ++clst_dropall;
        if (pica_cliptri(1)) {
            ++pica_cliptri_ndrop;
            fprintf(pvs_f, "CLIPTRI drop-all wa=%+.9g wb=%+.9g wc=%+.9g\n",
                    a.cw, b.cw, c.cw);
        }
        return;
    }
    int ctr = pica_cliptri(0);
    if (ctr) {
        ++pica_cliptri_n;
        fprintf(pvs_f, "CLIPTRI in wa=%+.9g wb=%+.9g wc=%+.9g "
                       "ca=%+.9g,%+.9g,%+.9g cb=%+.9g,%+.9g,%+.9g "
                       "cc=%+.9g,%+.9g,%+.9g\n",
                a.cw, b.cw, c.cw, a.cx, a.cy, a.cz, b.cx, b.cy, b.cz, c.cx, c.cy, c.cz);
    }
    p0[0]=a;p0[1]=b;p0[2]=c;
    for(plane=0;plane<5 && n>=3;plane++) {
        n=pica_clip_poly(dst,src,n,plane);
        src=dst;dst=(dst==p1)?p2:p1;
    }
    if (ctr) fprintf(pvs_f, "CLIPTRI kept=%u\n", n);
    if (pica_clst()) {
        ++clst_straddle;
        if (n < 3) ++clst_kept0; else ++clst_keptn;
        for (plane = 0; plane < n; plane++) {
            float iw = src[plane].cw > PICA_W_EPS ? 1.0f / src[plane].cw : 0.0f;
            float hw = pica_f24_to_f32(pica_regs[0x41]);
            float hh = pica_f24_to_f32(pica_regs[0x43]);
            float sx = (src[plane].cx * iw + 1) * hw + (pica_regs[0x68] & 1023);
            float sy = (src[plane].cy * iw + 1) * hh + ((pica_regs[0x68] >> 16) & 1023);
            if (src[plane].cw < clst_wmin_kept) clst_wmin_kept = src[plane].cw;
            clst_bbox_add(&clst_cx0, &clst_cx1, &clst_cy0, &clst_cy1, sx, sy);
        }
    }
    if(n<3)return;
    for(plane=1;plane+1<n;plane++) {
        PicaVertex v0=src[0],v1=src[plane],v2=src[plane+1];
        pica_vertex_project(&v0);pica_vertex_project(&v1);pica_vertex_project(&v2);
        if (pica_clst()) {
            ++clst_fan;
            float bx0 = fminf(v0.x, fminf(v1.x, v2.x));
            float bx1 = fmaxf(v0.x, fmaxf(v1.x, v2.x));
            float by0 = fminf(v0.y, fminf(v1.y, v2.y));
            float by1 = fmaxf(v0.y, fmaxf(v1.y, v2.y));
            if (bx1 >= 0.0f && bx0 <= (float)pica_rt_w && by1 >= 0.0f && by0 <= (float)pica_rt_h)
                ++clst_fan_rt;
            else ++clst_fan_off;
        }
        if (ctr)
            fprintf(pvs_f, "CLIPTRI fan sx=%+.9g,%+.9g,%+.9g sy=%+.9g,%+.9g,%+.9g "
                           "cw=%+.9g,%+.9g,%+.9g\n",
                    v0.x, v1.x, v2.x, v0.y, v1.y, v2.y, v0.cw, v1.cw, v2.cw);
        pica_queue_triangle(v0,v1,v2);
    }
}

static void pica_flush_triangles(void) {
    uint32_t n = pica_tri_verts, i;
    if (!n) return;
    pica_tri_verts = 0;
    if (pica_gl_draw_prepared) {
        int gl_ok = pica_gl_st.mode == PICA_GL_TEV;
        int swf_reason = SWF_NOT_TEV;
        if (pica_gl_st.mode == PICA_GL_SOLID) {
            gl_ok = 1;
            swf_reason = SWF_SOLID_NONCONST;
            for (i = 0; i < n; i += 3u) {
                if (memcmp(pica_tri_buf[i].color, pica_tri_buf[i+1].color,
                           sizeof pica_tri_buf[i].color) ||
                    memcmp(pica_tri_buf[i].color, pica_tri_buf[i+2].color,
                           sizeof pica_tri_buf[i].color)) {
                    gl_ok = 0;
                    break;
                }
            }
        }
        if (gl_ok && pica_gl_tev_tex1_on() &&
            (pica_tev_reads_texunit(1u) || pica_tev_reads_texunit(2u))) {
            /* t_e216fc37: the GL fragment shader can only sample texture unit
               0; a chain reading unit 1/2 (TEV source 4/5) would multiply in
               0.0 and paint the quad black (the ground "shadow" decals do
               exactly that).  Refuse here, where the software fallback has
               already flushed the deferred GL episode, so the software
               rasterizer - which now samples unit 1 - draws it in order. */
            gl_ok = 0;
            swf_reason = SWF_TEV_TEX1;
        }
        if (gl_ok && pica_gl_st.tev.tex_en && !pica_gl_tex_wrap_ok) {
            /* Only clamp-to-border (PICA wrap 1) still needs the software
               sampler here: the staged texture reproduces pica_sample_tex0
               for u,v outside [0,1] only through the GL sampler's own
               REPEAT/MIRRORED_REPEAT, and mode 1's border colour has no GL
               equivalent in the staged image.  Interpolated u,v stay inside
               the vertex hull, so per-vertex bounds are sufficient. */
            swf_reason = SWF_UV_OUTSIDE;
            for (i = 0; i < n; ++i) {
                float uu = pica_tri_buf[i].u, vv = pica_tri_buf[i].v;
                if (uu < 0.0f || uu > 1.0f || vv < 0.0f || vv > 1.0f) {
                    gl_ok = 0;
                    break;
                }
            }
        }
        if (!gl_ok) pica_swfall((unsigned)swf_reason, n);
        if (gl_ok) {
            /* Copy with per-triangle tev applied for solid mode, so a later
               GL failure can still fall back to pristine vertices.
               t_bc6b4eb7: the per-draw malloc/free on the draw hot path
               becomes one growable process-lifetime buffer (this flush runs
               on the draw thread only; pica_gl_draw consumes the copy before
               returning, and the fallback paths read pica_tri_buf). */
            static PicaGlVertex *glv_buf;
            static size_t glv_cap;
            PicaGlVertex *verts = NULL, *grown;
            if (glv_cap >= (size_t)n) {
                verts = glv_buf;
            } else if ((grown = (PicaGlVertex *)realloc(
                           glv_buf, (size_t)n * sizeof(PicaGlVertex))) != NULL) {
                glv_buf = grown;
                glv_cap = (size_t)n;
                verts = grown;
            }
            if (verts) {
                /* Field-wise copy: PicaVertex carries zw (depth), which
                   PicaGlVertex does not — a raw memcpy would shift every
                   field after iw (regression introduced with the depth
                   buffer work). */
                for (i = 0; i < n; ++i) {
                    verts[i].x = pica_tri_buf[i].x;
                    verts[i].y = pica_tri_buf[i].y;
                    verts[i].iw = pica_tri_buf[i].iw;
                    verts[i].zw = pica_tri_buf[i].zw;
                    memcpy(verts[i].color, pica_tri_buf[i].color,
                           sizeof verts[i].color);
                    verts[i].u = pica_tri_buf[i].u;
                    verts[i].v = pica_tri_buf[i].v;
                    verts[i].u1 = pica_tri_buf[i].u1;
                    verts[i].v1 = pica_tri_buf[i].v1;
                    verts[i].cx = pica_tri_buf[i].cx;
                    verts[i].cy = pica_tri_buf[i].cy;
                    verts[i].cz = pica_tri_buf[i].cz;
                    verts[i].cw = pica_tri_buf[i].cw;
                }
                if (pica_gl_st.mode == PICA_GL_SOLID) {
                    static const float zero[4] = {0,0,0,0};
                    for (i = 0; i < n; i += 3u) {
                        float solid[4];
                        unsigned k;
                        pica_tev(pica_tri_buf[i].color, zero, zero, solid);
                        for (k = 0; k < 3u; ++k)
                            memcpy(verts[i+k].color, solid, sizeof solid);
                    }
                }
                int pica_gl_rc;
                PF_ENTER(PF_GL);
                pica_gl_rc = (int)pica_gl_draw(verts, n, &pica_gl_st, &pica_gl_host);
                PF_EXIT();
                if (pica_gl_rc) {
                    ++pica_gl_draws_ok;
                    pica_tris_gl_total += n / 3u;
                    pica_gl_capture_ep(); /* deferred write-back target */
                    if (pica_gl_draws_ok <= 8u || (pica_gl_draws_ok % 500u) == 0u)
                        fprintf(stderr, "GL draw #%llu tris=%u mode=%d rt=%ux%u "
                                "blend=%d tex=%dx%d\n",
                                (unsigned long long)pica_gl_draws_ok, n / 3u,
                                pica_gl_st.mode, pica_gl_st.rt_w, pica_gl_st.rt_h,
                                pica_gl_st.blend, pica_gl_st.tex_w, pica_gl_st.tex_h);
                    return;
                }
                pica_swfall(SWF_GL_DRAW_FAIL, n);
            } else {
                pica_swfall(SWF_GL_DRAW_FAIL, n);
            }
        }
    }
    ++pica_gl_draws_sw;
    pica_tris_sw_total += n / 3u;
    /* The software rasterizer reads and writes guest memory directly: any
       deferred GL render-target content must land first. */
    pica_gl_flush_guest_rt();
    /* F1 elision: identical draws whose before-state matches a cached entry
       replay the stored pixel delta instead of re-rasterizing.  GL draws
       never reach this branch, so the GL path is unaffected. */
    if (elide_on()) {
        elide_trace_report();
        if (elide_prepare(pica_tri_buf, n)) {
            int hit = elide_lookup(&elide_cur);
            if (hit >= 0) {
                if (elide_verify_on())
                    elide_verify_hit(pica_tri_buf, n, &elide_tab[hit]);
                else
                    elide_apply(&elide_tab[hit]);
                ++elide_hits;
                return;
            }
            ++elide_misses;
            elide_copy_region(&elide_cur);
            for (i = 0; i < n; i += 3u)
                pica_raster_triangle(pica_tri_buf[i], pica_tri_buf[i+1], pica_tri_buf[i+2]);
            elide_store();
            return;
        }
    }
    for (i = 0; i < n; i += 3u)
        pica_raster_triangle(pica_tri_buf[i], pica_tri_buf[i+1], pica_tri_buf[i+2]);
}

/* DQ8_TRACE_TEV=1: log every DISTINCT fragment-pipeline state once, with a
   live probe of what the stage would actually produce:
     - tex0 config (enable/addr/dims/format) + a center-texel sample attempt;
     - the raw TEV stage registers (sources/ops);
     - the TEV output for a WHITE primary color with the sampled texture AND
       with a zero texture (isolates "texture black" from "TEV math black");
     - alpha-test / blend / color-mask / framebuffer registers.
   This answers, for the black-dialog investigation, WHICH layer yields
   black: texture fetch, TEV sources (e.g. lighting colors we don't feed),
   or the fragment write path. */
static uint64_t pica_tev_state_hash_last;
static unsigned pica_tev_state_logs;
static uint64_t pica_tev_epoch_base;
static void pica_trace_tev_state(void) {
    uint64_t h = 1469598103934665603ull;
    unsigned i, probe, probe_nz = 0;
    uint8_t sr=0,sg=0,sb=0,sa=0, fr=0,fg=0,fb=0,fa=0;
    float tex[4]={0,0,0,0}, white[4]={1,1,1,1}, zero4[4]={0,0,0,0};
    float out_tex[4], out_black[4];
    int sampled;
    /* Epoch: re-arm the dedupe + cap every 1024 draws so late phases (the
       dialog window) are traced too, not just the first boot states. */
    if (pica_draw_count >= pica_tev_epoch_base + 1024u) {
        pica_tev_epoch_base = pica_draw_count;
        pica_tev_state_hash_last = 0;
        pica_tev_state_logs = 0;
    }
#define H(x) do{ h=(h^(uint64_t)(x))*1099511628211ull; }while(0)
    for (i=0x80;i<=0x8f;i++) H(pica_regs[i]);
    for (i=0xc0;i<=0xff;i++) H(pica_regs[i]);
    for (i=0x100;i<=0x107;i++) H(pica_regs[i]);
    for (i=0x117;i<=0x11e;i++) H(pica_regs[i]);
#undef H
    if (h == pica_tev_state_hash_last) return;
    pica_tev_state_hash_last = h;
    if (pica_tev_state_logs++ >= 8u) return; /* per-epoch cap */
    sampled = (pica_regs[0x80]&1u) && pica_sample_tex0_exact(0.5f,0.5f,&sr,&sg,&sb,&sa);
    if (sampled) { tex[0]=sr/255.0f;tex[1]=sg/255.0f;tex[2]=sb/255.0f;tex[3]=sa/255.0f; }
    /* 4x4 probe across the whole texture: how many texels are non-black.
       A font/glyph atlas is mostly black; the center texel alone lies. */
    if ((pica_regs[0x80]&1u)) {
        for (probe = 0; probe < 16u; ++probe) {
            uint8_t pr,pg,pb,pa;
            unsigned px = probe & 3u, py = probe >> 2;
            if (pica_sample_tex0_exact(((float)px + 0.5f) / 4.0f, ((float)py + 0.5f) / 4.0f,
                                 &pr,&pg,&pb,&pa) && (pr|pg|pb)) {
                ++probe_nz;
                if (!fr && !fg && !fb) { fr=pr;fg=pg;fb=pb;fa=pa; }
            }
        }
    }
    pica_tev(white,tex,zero4,out_tex);
    pica_tev(white,zero4,zero4,out_black);
    fprintf(stderr,"TEVSTATE draw=%llu tex_en=%c addr=%08X dim=%08X fmt=%u sample=%s rgba=%u/%u/%u/%u probe4x4_nz=%u first_nz=%u/%u/%u/%u "
            "tev(white, tex)=(%u,%u,%u,%u) tev(white,0)=(%u,%u,%u,%u) "
            "stages=%08X/%08X/%08X %08X/%08X/%08X %08X/%08X/%08X %08X/%08X/%08X %08X/%08X/%08X %08X/%08X/%08X "
            "atest=%08X blend=%08X/%08X mask=%08X fb=%08X/%08X/%08X\n",
            (unsigned long long)pica_draw_count,
            (pica_regs[0x80]&1u)?'1':'0',
            pica_regs[0x85], pica_regs[0x82], pica_regs[0x8e]&15u,
            sampled?"OK":"FAIL", sr,sg,sb,sa, probe_nz, fr,fg,fb,fa,
            (unsigned)(out_tex[0]*255+0.5f),(unsigned)(out_tex[1]*255+0.5f),
            (unsigned)(out_tex[2]*255+0.5f),(unsigned)(out_tex[3]*255+0.5f),
            (unsigned)(out_black[0]*255+0.5f),(unsigned)(out_black[1]*255+0.5f),
            (unsigned)(out_black[2]*255+0.5f),(unsigned)(out_black[3]*255+0.5f),
            pica_regs[0xc0],pica_regs[0xc1],pica_regs[0xc2],
            pica_regs[0xc8],pica_regs[0xc9],pica_regs[0xca],
            pica_regs[0xd0],pica_regs[0xd1],pica_regs[0xd2],
            pica_regs[0xd8],pica_regs[0xd9],pica_regs[0xda],
            pica_regs[0xf0],pica_regs[0xf1],pica_regs[0xf2],
            pica_regs[0xf8],pica_regs[0xf9],pica_regs[0xfa],
            pica_regs[0x104],pica_regs[0x100],pica_regs[0x101],
            pica_regs[0x107],pica_regs[0x117],pica_regs[0x11d],pica_regs[0x11e]);
}
/* ---- DQ8_TRACE_TIME render-cost accounting (t_399eab58) -----------------
   Env DQ8_TRACE_TIME=1: every ~2 s print one aggregate block over the draws
   since the previous report, classified opaque/blended x software/GL, with
   mean cost per draw and a 4-bucket bbox-area histogram (reveals the
   per-pixel vs per-draw cost structure), plus the GL round-trip phase totals
   (pgl_t_* from pica_gl.c) and the vblank frame count (pqt_frames, counted
   in gsp_enqueue_interrupt).  Off by default: one branch per draw. */
static long long pqt_win_ns[4], pqt_win_n[4], pqt_win_area[4];
static long long pqt_win_ab_n[4][4], pqt_win_ab_ns[4][4];
static double pqt_last_rep;
static void pqt_account(long long t0, unsigned long long gl_before) {
    unsigned gl_now = pica_gl_draws_ok != gl_before;
    unsigned cls = (gl_now ? 2u : 0u) + (pqt_slow ? 1u : 0u);
    long long dt = pica_pqt_now_ns() - (unsigned long long)t0;
    long long area = 0, rt_area;
    unsigned b = 0;
    double now_s;
    if (pica_db_x1 >= pica_db_x0 && pica_db_y1 >= pica_db_y0)
        area = (long long)(pica_db_x1 - pica_db_x0 + 1) *
               (long long)(pica_db_y1 - pica_db_y0 + 1);
    pqt_win_n[cls]++;
    pqt_win_ns[cls] += dt;
    pqt_win_area[cls] += area;
    rt_area = (long long)pica_rt_w * pica_rt_h;
    if (rt_area > 0) {
        if (area * 4 >= rt_area * 3) b = 3;
        else if (area * 2 >= rt_area) b = 2;
        else if (area * 4 >= rt_area) b = 1;
    }
    pqt_win_ab_n[cls][b]++;
    pqt_win_ab_ns[cls][b] += dt;
    now_s = dq8_layer_t();
    if (pqt_last_rep == 0.0) { pqt_last_rep = now_s; return; }
    if (now_s - pqt_last_rep >= 2.0) {
        static const char *names[4] = {"sw-fast", "sw-RMW", "gl-fast", "gl-RMW"};
        unsigned c;
        fprintf(stderr, "TIME window=%.1fs frames=%llu rt=%ux%u blend_px(fast/gen)=%llu/%llu\n",
                now_s - pqt_last_rep, pqt_frames, pica_rt_w, pica_rt_h,
                pqt_fast_px, pqt_gen_px);
        for (c = 0; c < 4; c++) {
            unsigned i;
            if (!pqt_win_n[c]) continue;
            fprintf(stderr, "TIME %-9s n=%llu cpu_ms=%.1f mean_us=%.0f "
                    "mean_area=%llu buckets[<25%%|25-50%%|50-75%%|>75%% of rt]:",
                    names[c], pqt_win_n[c], (double)pqt_win_ns[c] / 1e6,
                    (double)pqt_win_ns[c] / (double)pqt_win_n[c] / 1e3,
                    (unsigned long long)(pqt_win_area[c] / pqt_win_n[c]));
            for (i = 0; i < 4; i++)
                fprintf(stderr, " %llu/%.0fus", pqt_win_ab_n[c][i],
                        pqt_win_ab_n[c][i]
                            ? (double)pqt_win_ab_ns[c][i] / pqt_win_ab_n[c][i] / 1e3
                            : 0.0);
            fprintf(stderr, "\n");
        }
        if (pgl_t_n)
            fprintf(stderr, "TIME gl-phase n=%llu read_ms=%.1f upload_ms=%.1f "
                    "draw_ms=%.1f readback_ms=%.1f writeto_ms=%.1f\n",
                    pgl_t_n, (double)pgl_t_read_ns / 1e6,
                    (double)pgl_t_upload_ns / 1e6, (double)pgl_t_draw_ns / 1e6,
                    (double)pgl_t_readback_ns / 1e6,
                    (double)pgl_t_write_ns / 1e6);
        memset(pqt_win_n, 0, sizeof pqt_win_n);
        memset(pqt_win_ns, 0, sizeof pqt_win_ns);
        memset(pqt_win_area, 0, sizeof pqt_win_area);
        memset(pqt_win_ab_n, 0, sizeof pqt_win_ab_n);
        memset(pqt_win_ab_ns, 0, sizeof pqt_win_ab_ns);
        memset(&pgl_t_n, 0, sizeof pgl_t_n);
        memset(&pgl_t_read_ns, 0,
               sizeof pgl_t_read_ns + sizeof pgl_t_upload_ns +
               sizeof pgl_t_draw_ns + sizeof pgl_t_readback_ns +
               sizeof pgl_t_write_ns);
        pqt_frames = 0;
        pqt_last_rep = now_s;
    }
}
/* ===========================================================================
 * Parallel vertex transform (t_5a3c517a).
 *
 * Measured on the operator's Radeon (RX 570, radeonsi): a spike frame is one
 * 3D frame of ~170 draws / ~42 000 vertices, and 73 % of the draw path is
 * pica_load_vertex (guest attribute fetch + the PICA vertex-shader microcode
 * interpreter) at ~2.1 us/vertex on ONE core, while 15 cores sit idle.  The
 * transform is embarrassingly parallel: every vertex reads guest RAM, the
 * draw's PICA registers and the VS program (all constant for the draw) and
 * writes only its own output slot, so the workers cannot disagree with a
 * serial run.  The topology assembly / clipping / queueing stays on the main
 * thread and consumes the finished vertices in order, so the command stream
 * sees exactly the same triangle sequence.
 *
 * DQ8_VSPAR=N selects the worker count (default 4; 0 disables).  Workers are
 * created lazily on the first parallel draw.
 *
 * Idle discipline (t_f40374bc).  The pool used to spin-then-sleep on a
 * condition variable with a 2 ms budget, and the main thread SPUN on the
 * completion counter for its own job.  Measured on the operator's city state
 * (`perf -e cycles:u`, per thread) that idle spin was 46-48 % of ALL host
 * cycles: `pvs_par_worker` was 99.4 % the `pause; mov gen; inc; cmp` loop
 * because draws arrive every ~150 us and the 2 ms budget never expired, and
 * the main thread spent 45.97 % of its own cycles in the same kind of loop.
 *
 * Now: a worker spins for DQ8_VSPAR_SPIN us (default 0) looking for a new
 * generation, then BLOCKS in FUTEX_WAIT on the generation word; the publisher
 * wakes the pool with one FUTEX_WAKE.  The old 50 us spin was there to hide
 * wake latency while the main thread spun too; with the main thread blocking
 * as well, the spin no longer buys guest time (see reports/VSPAR_BLOCKING.md,
 * a 3-round sweep: spin 0/25/50 = 12.96/12.88/12.86 ms guest, i.e. inside the
 * noise, at 2.95/3.38/3.57 cores), so the default is 0.  The spin loop samples
 * the clock every 256 PAUSE instructions, so "0" means at most a few us of
 * pacing before the futex wait, not a hard floor of zero.
 * The main thread does a bounded
 * DQ8_VSPAR_WAIT_SPIN us spin (default 20) and then blocks in FUTEX_WAIT on the
 * completion counter, which the last participant wakes.  The futex value is
 * the token, so a wake that lands between the load and the wait makes the wait
 * return EAGAIN instead of sleeping — no lost wakeup, no mutex, no condvar on
 * either path.  Jobs below DQ8_VSPAR_MIN_VERTS vertices (default 0 = always
 * dispatch) run inline on the main thread: at ~250 vertices, publishing a job
 * and waking eight workers can cost more than the transform itself.
 * ========================================================================= */
#define PVS_PAR_MAX 12
#define PVS_ST_INDEX_FAIL 0xFFu
#if defined(__x86_64__) || defined(__i386__)
#define PVS_PAUSE() __builtin_ia32_pause()
#else
#define PVS_PAUSE() ((void)0)
#endif
/* t_f40374bc: blocking wait primitive.  One syscall, no lock; the futex value
   is the token, so a store between the load and the wait returns EAGAIN
   instead of sleeping (no lost wakeup). */
#if defined(__linux__)
#  include <linux/futex.h>
#  include <sys/syscall.h>
#  define PVS_HAVE_FUTEX 1
#else
#  define PVS_HAVE_FUTEX 0
#endif
static void pvs_park(volatile unsigned *p, unsigned val) {
#if PVS_HAVE_FUTEX
    syscall(SYS_futex, (unsigned *)p, FUTEX_WAIT_PRIVATE, val,
            (void *)0, (void *)0, 0);
#else
    struct timespec ts = {0, 50000};
    while (__atomic_load_n(p, __ATOMIC_ACQUIRE) == val) nanosleep(&ts, NULL);
#endif
}
/* Bounded variant: a park that cannot outlive `ns` even if the token was
   already consumed by the store it is waiting for.  Used by the publisher's
   completion wait, where an unbounded park converts one lost wakeup into a
   permanent freeze (t_e1773c51 step 1b). */
static void pvs_park_to(volatile unsigned *p, unsigned val, uint64_t ns) {
#if PVS_HAVE_FUTEX
    struct timespec ts;
    ts.tv_sec = (time_t)(ns / 1000000000ull);
    ts.tv_nsec = (long)(ns % 1000000000ull);
    syscall(SYS_futex, (unsigned *)p, FUTEX_WAIT_PRIVATE, val,
            &ts, (void *)0, 0);
#else
    struct timespec ts = {0, 50000};
    uint64_t end = pica_pqt_now_ns() + ns;
    while (__atomic_load_n(p, __ATOMIC_ACQUIRE) == val &&
           pica_pqt_now_ns() < end) nanosleep(&ts, NULL);
#endif
}
static void pvs_unpark(volatile unsigned *p) {
#if PVS_HAVE_FUTEX
    syscall(SYS_futex, (unsigned *)p, FUTEX_WAKE_PRIVATE, 0x7fffffff,
            (void *)0, (void *)0, 0);
#else
    (void)p;
#endif
}
static int pvs_par = -1;                 /* -1 unknown, else worker count */
static PicaVertex *pvs_vout;             /* [65536] */
static uint8_t *pvs_vst;                 /* per-vertex status (0 = ok) */
static uint32_t *pvs_vdet;               /* per-vertex pica_vs_fail_detail */
static unsigned pvs_vcap;
static volatile int pvs_par_stop;
static unsigned pvs_par_nw;              /* workers actually started */
static uint64_t pvs_spin_ns = 0u;         /* worker spin before blocking (0 us) */
/* t_45ee02dd item 3: main-thread bounded spin 20 us -> 100 us.  From the
   8-CPU sweep recorded on this card (rig, taskset 0-7, DQ8_VBLANK_WALLCLOCK=1):
   spin=100 + vspar=8 -> 20.9/19.9 fps, spin=20 -> 19.9/19.5, spin=300 ->
   20.0/15.5 (unstable), vspar=4 -> 15.0/15.8.  Eight workers with a 100 us
   bound is the stable point, and it removes the 16-20 fps fluctuation the
   operator sees. */
static uint64_t pvs_wait_spin_ns = 100000u; /* main-thread spin before blocking */
static unsigned pvs_min_verts;           /* inline below this vertex count */
/* Idle accounting (DQ8_VSPAR_STATS=1): how many times a participant actually
   had to block vs how many jobs were dispatched, so a regression back to
   spinning shows up as blk=0 with a full proc_cpu. */
static uint64_t pvs_block_count, pvs_wake_count, pvs_wait_blocks,
                pvs_inline_jobs;
static int pvs_par_injob;                /* re-entrancy guard (main thread) */
static uint64_t pvs_par_jobs, pvs_par_ms_saved_probe;
/* -------------------------------------------------------------------------
 * DQ8_VSPAR_VERIFY=1 (t_968a1fa9): for every pooled draw, transform the SAME
 * vertices a second time serially, in the same order, from the same guest
 * state, and compare the result byte-for-byte (PicaVertex is 14 floats, no
 * padding) plus the per-vertex status/detail.  Any difference is a race in the
 * pool, named by draw and vertex index.  Report-only: the pool output is what
 * gets drawn, so a mismatch is never masked.
 * ---------------------------------------------------------------------- */
static int pvs_verify = -1;
static PicaVertex *pvs_vref;
static uint8_t *pvs_vsref;
static uint32_t *pvs_vdref;
static unsigned long long pvs_verify_jobs, pvs_verify_verts, pvs_verify_bad,
                          pvs_verify_reports;
static double pvs_verify_next_rep;
/* =========================================================================
 * t_e1773c51 STEP 1 — generation-tagged job descriptors.
 *
 * The old handshake was one process-global {gen, next, nchunks, done} set plus
 * one global `done` word that was RESET on publish and incremented once by
 * every worker and once by the main thread; the main thread returned from
 * pvs_job_wait() as soon as `done >= nparts` (nparts = workers + 1, a fixed
 * participant count).  Three things were wrong with it:
 *
 *  (a) the increment carried no generation identity, so an increment that
 *      belonged to an older generation (a worker that observed gen g, was
 *      descheduled, and only reached its `done++` after the main thread had
 *      already published g+1 and reset the word) was counted for g+1.  One
 *      such stray increment makes `done >= nparts` true while another worker
 *      is still inside its chunk for g+1: the main thread then starts
 *      assembling pvs_vout[] -- and publishes the next draw's plan into
 *      pvs_plan_cur -- under a live writer.
 *  (b) `pvs_nparts` was a fixed participation count, not a per-generation
 *      attribute, and nothing tied a participant to the job it counted for.
 *  (c) the chunk cursor and the job arguments were globals too, so a late
 *      participant pulled chunks from whatever job was current, with whatever
 *      `run` the main thread had restored in the meantime, and a generic job's
 *      `ctx` is a STACK object (pgl_wb_*_blocks passes &c) -- a participant
 *      that outlives pvs_par_run() writes guest RAM through a dead stack
 *      frame.  That is how a broken run can leave guest memory (the colour /
 *      depth write-back target) corrupted, which is why a savestate taken
 *      during it stays broken on every build.
 *
 * The replacement is a published descriptor per generation:
 *
 *   - a ring of PVS_JOB_RING descriptors; publishing stamps `gen`, fills the
 *     immutable half, and release-stores pvs_job_gen (the futex word);
 *   - every participant (main + workers) copies the immutable half into its
 *     own stack (`PvsJobDesc job`) and re-validates `gen` after the copy, so
 *     it can never mix two generations' arguments;
 *   - participation is counted per generation, in ONE 64-bit word `live` as
 *     {joined:32, done:32}: join is a CAS increment of the high half, leave a
 *     CAS increment of the low half, so a reader always sees a consistent cut
 *     (a join can never appear without its matching leave);
 *   - the main thread returns from wait when done == joined, i.e. when every
 *     participant that joined has left -- not when a fixed number of
 *     increments happened.  A participant that arrives late finds the cursor
 *     exhausted, does no work, and cannot move the check for a generation it
 *     did not run;
 *   - the chunk cursor and the `run` pointer live in the descriptor, so a
 *     late participant can only pull chunks of its own generation;
 *   - per-generation wake token (b->wake) so the futex wait has no lost
 *     wakeup and no cross-generation token confusion.
 * ========================================================================= */
typedef struct PvsJobDesc {
    unsigned gen;
    unsigned nchunks;
    void (*run)(const struct PvsJobDesc *d, unsigned c);
    uint32_t base, count, chunk;      /* vertex job */
    int indexed;
    const PvsPlan *plan;              /* vertex job: plan snapshot */
    const PvsProg *prog;
    uint64_t plan_key;                /* DQ8_VS4_STRESS: key the plan was built for */
    void (*gfn)(void *, unsigned, unsigned);   /* generic job */
    void *gctx;
    unsigned gbase, gchunk, gn;
} PvsJobDesc;
typedef struct PvsJobBuf {
    PvsJobDesc d;                     /* immutable once published */
    unsigned next;                    /* atomic chunk cursor, per generation */
    unsigned wake;                    /* leave token (futex word for the waiter) */
    uint64_t live;                    /* {joined:32, done:32} */
} PvsJobBuf;
#define PVS_JOB_RING 32768u
static PvsJobBuf *pvs_job_ring;
static volatile unsigned pvs_job_gen;   /* futex word: last published gen */
static unsigned pvs_gen_pub;            /* main thread's last published gen */
static __thread unsigned pvs_wgen;      /* participant: generation it is running */
static __thread const PvsJobDesc *pvs_wjob;  /* participant: its own snapshot */
static uint64_t pvs_in_body;            /* participants inside a job (diagnostic) */
/* t_e1773c51 step 1b: the publisher's completion wait and its failure modes.
   `pvs_wait_lostwake` counts the race the pre-fix ordering slept through: the
   last participant left between the publisher's condition check and its read
   of the leave token, so the token it then parked on already contained that
   leave -- futex_wait(value == expected) never returns and the publisher is
   stuck until the ring slot is reused (32768 jobs later).  `pvs_wait_stall`
   counts waits that ran past PVS_WAIT_STALL_NS. */
static uint64_t pvs_wait_lostwake, pvs_wait_stall, pvs_wait_slow;
static int pvs_wait_oldorder = -1;
#define PVS_WAIT_STALL_NS   50000000ull      /* 50 ms: a chunk is microseconds */
#define PVS_WAIT_PARK_NS     1000000ull      /* 1 ms bound on a completion park */
static inline unsigned pvs_live_joined(uint64_t v) { return (unsigned)((v >> 32) & 0x7fffffffu); }
static inline unsigned pvs_live_done(uint64_t v) { return (unsigned)(v & 0xffffffffu); }
#define PVS_LIVE_CLOSED (1ull << 63)
/* Join is conditional on the generation still accepting participants.  The
   publisher sets PVS_LIVE_CLOSED after its own leave and before it waits, so a
   participant that arrives later is refused and runs nothing -- otherwise a
   join landing between the publisher's leave and its read of `live` would make
   joined > done with nobody left to close it (a hang that needs a one-
   instruction window).  Join, close and leave are CAS/RMW on the SAME word, so
   no join can slip past the close.  Returns 1 when the caller may run the job
   (and owes a leave), 0 when the job is closed. */
static inline int pvs_job_join(PvsJobBuf *b) {
    uint64_t v;
    for (;;) {
        v = __atomic_load_n(&b->live, __ATOMIC_ACQUIRE);
        if (v & PVS_LIVE_CLOSED) return 0;
        if (__atomic_compare_exchange_n(&b->live, &v, v + 0x100000000ull, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            __atomic_add_fetch(&pvs_in_body, 1u, __ATOMIC_RELAXED);
            return 1;
        }
    }
}
static inline void pvs_job_close(PvsJobBuf *b) {
    __atomic_fetch_or(&b->live, PVS_LIVE_CLOSED, __ATOMIC_ACQ_REL);
}
static inline void pvs_job_leave(PvsJobBuf *b) {
    uint64_t v = __atomic_add_fetch(&b->live, 1u, __ATOMIC_ACQ_REL);
    __atomic_add_fetch(&b->wake, 1u, __ATOMIC_RELEASE);
    __atomic_sub_fetch(&pvs_in_body, 1u, __ATOMIC_RELAXED);
    if (pvs_live_done(v) >= pvs_live_joined(v)) pvs_unpark(&b->wake);
}
/* Phase split of the job total: time the MAIN thread spent waiting for the
   pool at the end of a draw, vs the chunks it pulled itself.  DQ8_VSPAR_STATS=1
   prints it every 10 s. */
static uint64_t pvs_par_wait_ns, pvs_par_mainwork_ns;

/* DQ8_VS4_STRESS=1: exercise the handshake.  Forces working participants to
   park between jobs (spin budget 0), one-chunk jobs (so the publish rate and
   the number of wake/park transitions go up by ~20x), and delays a fraction of
   the joins by 20-200 us, which is exactly the "late waker" window the old
   protocol needed.  Level 2 delays every join.  While it is on, each chunk
   validates that the descriptor it runs is still the published one and, for a
   vertex job, that pvs_plan_cur is still the job's plan and that the plan's
   key still matches the live VS program. */
static int pvs_stress = -1;
static unsigned long long pvs_stress_chunks, pvs_stress_stale,
                          pvs_stress_plan_bad, pvs_stress_key_bad,
                          pvs_stress_reports;
static inline unsigned long long pvs_stress_rand(void) {
    static __thread unsigned long long s;
    if (!s) s = 0x9E3779B97F4A7C15ull ^ (unsigned long long)(uintptr_t)&s;
    s ^= s << 13; s ^= s >> 7; s ^= s << 17;
    return s;
}
static inline int pvs_stress_on(void) {
    if (pvs_stress < 0) {
        const char *e = getenv("DQ8_VS4_STRESS");
        pvs_stress = (e && *e && *e != '0') ? atoi(e) : 0;
        if (pvs_stress > 2) pvs_stress = 2;
    }
    return pvs_stress;
}
static void pvs_stress_check(const PvsJobDesc *d, unsigned c) {
    unsigned g;
    __atomic_add_fetch(&pvs_stress_chunks, 1ull, __ATOMIC_RELAXED);
    g = __atomic_load_n(&pvs_job_gen, __ATOMIC_ACQUIRE);
    if (g != pvs_wgen) {
        __atomic_add_fetch(&pvs_stress_stale, 1ull, __ATOMIC_RELAXED);
        if (__atomic_fetch_add(&pvs_stress_reports, 1ull, __ATOMIC_RELAXED) < 16u)
            fprintf(stderr, "VSPARSTRESS STALE job gen=%u live_gen=%u chunk=%u "
                            "draw=%llu -- the main thread retired this job "
                            "while a participant was still in it\n",
                    pvs_wgen, g, c, (unsigned long long)pica_draw_count);
    }
    if (d->plan) {
        if ((const PvsPlan *)__atomic_load_n((const void **)&pvs_plan_cur,
                                             __ATOMIC_ACQUIRE) != d->plan) {
            __atomic_add_fetch(&pvs_stress_plan_bad, 1ull, __ATOMIC_RELAXED);
            if (__atomic_fetch_add(&pvs_stress_reports, 1ull, __ATOMIC_RELAXED) < 16u)
                fprintf(stderr, "VSPARSTRESS PLAN job gen=%u plan=%p live=%p "
                                "draw=%llu chunk=%u\n", pvs_wgen, (const void *)d->plan,
                        (const void *)pvs_plan_cur,
                        (unsigned long long)pica_draw_count, c);
        }
        if (d->plan_key && pvs_plan_key(vs_program_size) != d->plan_key) {
            __atomic_add_fetch(&pvs_stress_key_bad, 1ull, __ATOMIC_RELAXED);
            if (__atomic_fetch_add(&pvs_stress_reports, 1ull, __ATOMIC_RELAXED) < 16u)
                fprintf(stderr, "VSPARSTRESS KEY job gen=%u key=%016llX live=%016llX "
                                "draw=%llu\n", pvs_wgen,
                        (unsigned long long)d->plan_key,
                        (unsigned long long)pvs_plan_key(vs_program_size),
                        (unsigned long long)pica_draw_count);
        }
    }
}
static void pvs_stress_report(void) {
    if (!pvs_stress_on()) return;
    fprintf(stderr, "VSPARSTRESS chunks=%llu stale=%llu plan_bad=%llu key_bad=%llu "
                    "lostwake=%llu stall=%llu gen=%u workers=%u chunk=1 spin=0\n",
            pvs_stress_chunks, pvs_stress_stale, pvs_stress_plan_bad,
            pvs_stress_key_bad, pvs_wait_lostwake, pvs_wait_stall,
            pvs_job_gen, pvs_par_nw);
}
/* Publish `d` as generation pvs_gen_pub+1 and wake the pool.  The main thread
   has already joined (live = {1,0}), so a worker that finishes before the main
   thread even starts its own chunks cannot satisfy done == joined. */
static PvsJobBuf *pvs_job_publish(const PvsJobDesc *d) {
    unsigned g = ++pvs_gen_pub;
    PvsJobBuf *b = &pvs_job_ring[g % PVS_JOB_RING];
    b->d = *d;
    b->d.gen = g;
    __atomic_store_n(&b->next, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&b->wake, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&b->live, 0x100000000ull, __ATOMIC_RELAXED); /* main joined */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&pvs_job_gen, g, __ATOMIC_RELEASE);
    ++pvs_wake_count;
    pvs_unpark(&pvs_job_gen);
    return b;
}
/* One participant's chunk pull loop.  `pub` supplies the cursor and the
   completion counters, `d` the immutable arguments; pvs_tls_plan/prog hold
   `d`'s plan for the duration so the vertex code cannot read the main
   thread's newer plan. */
static void pvs_body_run(PvsJobBuf *pub, const PvsJobDesc *d) {
    const PvsPlan *sp = pvs_tls_plan;
    const PvsProg *sg = pvs_tls_prog;
    pvs_tls_plan = d->plan;
    pvs_tls_prog = d->prog;
    for (;;) {
        unsigned c = __atomic_fetch_add(&pub->next, 1u, __ATOMIC_ACQ_REL);
        if (c >= d->nchunks) break;
        if (pvs_stress) pvs_stress_check(d, c);
        d->run(d, c);
    }
    pvs_tls_plan = sp;
    pvs_tls_prog = sg;
}
static void pvs_par_chunk_gen(const PvsJobDesc *d, unsigned c) {
    unsigned i0 = d->gbase + c * d->gchunk;
    unsigned i1 = i0 + d->gchunk;
    unsigned end = d->gbase + d->gn;
    if (i1 > end) i1 = end;
    d->gfn(d->gctx, i0, i1);
}
static void pvs_par_chunk(const PvsJobDesc *d, unsigned c) {
    uint32_t i0 = c * d->chunk, i1 = i0 + d->chunk;
    uint32_t i;
    if (i1 > d->count) i1 = d->count;
    for (i = i0; i < i1; ) {
        /* t_fdc40e6f step 2: run four vertices per pass over the plan.  Every
           path the batch cannot take falls through to the single-vertex loop
           below, which is byte-for-byte the pre-batch reference code. */
        if (pvs_vs8_gate() && i + 8 <= i1) {
            /* t_e1773c51 STEP 2: eight vertices, one pass over the plan. */
            uint32_t idx[8];
            PicaVertex *vp[8];
            uint8_t rsn[8];
            uint32_t det[8];
            unsigned j, all = 1;
            int r8;
            for (j = 0; j < 8; ++j) {
                memset(&pvs_vout[i + j], 0, sizeof pvs_vout[i + j]);
                idx[j] = pica_regs[0x22a] + i + j;
                vp[j] = &pvs_vout[i + j];
                if (d->indexed) {
                    unsigned bytes = (pica_regs[0x227] >> 31) ? 2u : 1u;
                    uint32_t mapped;
                    uint64_t addr = (uint64_t)d->base +
                                    (pica_regs[0x227] & 0xfffffff) +
                                    (uint64_t)(i + j) * bytes;
                    if (addr > UINT32_MAX ||
                        !guest_memory_range((uint32_t)addr, bytes, &mapped)) {
                        pvs_vst[i + j] = PVS_ST_INDEX_FAIL;
                        all = 0;
                        break;
                    }
                    idx[j] = ram[mapped];
                    if (bytes == 2u) idx[j] |= (uint32_t)ram[mapped + 1] << 8;
                }
            }
            if (all) {
                r8 = pica_load_vertex8(d->base, idx, vp, rsn, det);
                if (r8 >= 0) {
                    for (j = 0; j < 8; ++j) {
                        if (r8 & (1u << j)) pvs_vst[i + j] = 0u;
                        else { pvs_vst[i + j] = rsn[j]; pvs_vdet[i + j] = det[j]; }
                    }
                    i += 8;
                    continue;
                }
            }
            ++pvs4_tls.b8_fallback;
        }
        if (pvs_vs4_gate() && i + 4 <= i1) {
            uint32_t idx[4];
            PicaVertex *vp[4];
            uint8_t reason[4];
            uint32_t detail[4];
            unsigned j, all = 1, r4;
            for (j = 0; j < 4; ++j) {
                memset(&pvs_vout[i + j], 0, sizeof pvs_vout[i + j]);
                idx[j] = pica_regs[0x22a] + i + j;
                vp[j] = &pvs_vout[i + j];
                if (d->indexed) {
                    unsigned bytes = (pica_regs[0x227] >> 31) ? 2u : 1u;
                    uint32_t mapped;
                    uint64_t addr = (uint64_t)d->base +
                                    (pica_regs[0x227] & 0xfffffff) +
                                    (uint64_t)(i + j) * bytes;
                    if (addr > UINT32_MAX ||
                        !guest_memory_range((uint32_t)addr, bytes, &mapped)) {
                        pvs_vst[i + j] = PVS_ST_INDEX_FAIL;
                        all = 0;
                        break;
                    }
                    idx[j] = ram[mapped];
                    if (bytes == 2u) idx[j] |= (uint32_t)ram[mapped + 1] << 8;
                }
            }
            if (all) {
                r4 = (unsigned)pica_load_vertex4(d->base, idx, vp, reason, detail);
                if ((int)r4 >= 0) {
                    for (j = 0; j < 4; ++j) {
                        if (r4 & (1u << j)) pvs_vst[i + j] = 0u;
                        else { pvs_vst[i + j] = reason[j]; pvs_vdet[i + j] = detail[j]; }
                    }
                    i += 4;
                    continue;
                }
            }
            ++pvs4_tls.scalar;
        }
        {
        uint32_t index = pica_regs[0x22a] + i;
        /* t_968a1fa9: pica_load_vertex returns *without* touching its output on
           every failure path, so the slot must be deterministic garbage-free
           memory.  A failed vertex then reads as an all-zero vertex (w=0,
           i.e. clipped) even if some caller ever forgets to check the status,
           instead of whatever the previous draw left there.  It also makes the
           DQ8_VSPAR_VERIFY byte comparison exact (no malloc junk). */
        memset(&pvs_vout[i], 0, sizeof pvs_vout[i]);
        if (d->indexed) {
            unsigned bytes = (pica_regs[0x227] >> 31) ? 2u : 1u;
            uint32_t mapped;
            uint64_t addr = (uint64_t)d->base + (pica_regs[0x227] & 0xfffffff) +
                            (uint64_t)i * bytes;
            if (addr > UINT32_MAX ||
                !guest_memory_range((uint32_t)addr, bytes, &mapped)) {
                pvs_vst[i] = PVS_ST_INDEX_FAIL;
                continue;
            }
            index = ram[mapped];
            if (bytes == 2u) index |= (uint32_t)ram[mapped + 1] << 8;
        }
        if (!pica_load_vertex(d->base, index, &pvs_vout[i])) {
            pvs_vst[i] = (uint8_t)pica_vs_fail_reason;
            pvs_vdet[i] = pica_vs_fail_detail;
        } else {
            pvs_vst[i] = 0u;
        }
        }
        ++i;
    }
}

/* Serial reference pass for DQ8_VSPAR_VERIFY: identical decode + transform,
   in index order, into pvs_vref/pvs_vsref, then byte-compare with the pool's
   result.  Runs on the main thread, so it also proves the pool agrees with the
   single-threaded path exactly. */
static void pvs_par_verify(const PvsJobDesc *d) {
    uint32_t i;
    unsigned bad = 0;
    /* t_fdc40e6f: this reference pass must NOT use the prepared plan, so it
       compares the plan's output against the pre-plan pre-decoded path.  Safe
       on the main thread: the pool job has already completed and this runs
       before the next pvs_plan_sync publishes a new plan. */
    const PvsPlan *saved_plan = pvs_plan_cur;
    const PvsProg *saved_prog = pvs_prog_cur;
    pvs_plan_cur = NULL;
    pvs_prog_cur = NULL;
    for (i = 0; i < d->count; ++i) {
        uint32_t index = pica_regs[0x22a] + i;
        memset(&pvs_vref[i], 0, sizeof pvs_vref[i]);
        if (d->indexed) {
            unsigned bytes = (pica_regs[0x227] >> 31) ? 2u : 1u;
            uint32_t mapped;
            uint64_t addr = (uint64_t)d->base + (pica_regs[0x227] & 0xfffffff) +
                            (uint64_t)i * bytes;
            if (addr > UINT32_MAX ||
                !guest_memory_range((uint32_t)addr, bytes, &mapped)) {
                pvs_vsref[i] = PVS_ST_INDEX_FAIL;
                pvs_vdref[i] = 0u;
                goto cmp;
            }
            index = ram[mapped];
            if (bytes == 2u) index |= (uint32_t)ram[mapped + 1] << 8;
        }
        if (!pica_load_vertex(d->base, index, &pvs_vref[i])) {
            pvs_vsref[i] = (uint8_t)pica_vs_fail_reason;
            pvs_vdref[i] = pica_vs_fail_detail;
        } else {
            pvs_vsref[i] = 0u;
            pvs_vdref[i] = 0u;
        }
cmp:
        if (pvs_vsref[i] != pvs_vst[i] ||
            memcmp(&pvs_vref[i], &pvs_vout[i], sizeof(PicaVertex)) != 0 ||
            (pvs_vsref[i] && pvs_vdref[i] != pvs_vdet[i])) {
            ++pvs_verify_bad;
            ++bad;
            if (pvs_verify_reports < 16u) {
                ++pvs_verify_reports;
                fprintf(stderr,
                        "VSPARVERIFY MISMATCH draw=%llu vert=%u/%u status ser=%u par=%u "
                        "detail ser=%08X par=%08X\n"
                        "  ser c=(%.9g,%.9g,%.9g,%.9g) s=(%.9g,%.9g) uv=(%.9g,%.9g) "
                        "uv1=(%.9g,%.9g) col=(%.9g,%.9g,%.9g,%.9g) zw=%.9g iw=%.9g\n"
                        "  par c=(%.9g,%.9g,%.9g,%.9g) s=(%.9g,%.9g) uv=(%.9g,%.9g) "
                        "uv1=(%.9g,%.9g) col=(%.9g,%.9g,%.9g,%.9g) zw=%.9g iw=%.9g\n",
                        (unsigned long long)pica_draw_count, i, d->count,
                        pvs_vsref[i], pvs_vst[i], pvs_vdref[i], pvs_vdet[i],
                        pvs_vref[i].cx, pvs_vref[i].cy, pvs_vref[i].cz, pvs_vref[i].cw,
                        pvs_vref[i].x, pvs_vref[i].y, pvs_vref[i].u, pvs_vref[i].v,
                        pvs_vref[i].u1, pvs_vref[i].v1,
                        pvs_vref[i].color[0], pvs_vref[i].color[1],
                        pvs_vref[i].color[2], pvs_vref[i].color[3],
                        pvs_vref[i].zw, pvs_vref[i].iw,
                        pvs_vout[i].cx, pvs_vout[i].cy, pvs_vout[i].cz, pvs_vout[i].cw,
                        pvs_vout[i].x, pvs_vout[i].y, pvs_vout[i].u, pvs_vout[i].v,
                        pvs_vout[i].u1, pvs_vout[i].v1,
                        pvs_vout[i].color[0], pvs_vout[i].color[1],
                        pvs_vout[i].color[2], pvs_vout[i].color[3],
                        pvs_vout[i].zw, pvs_vout[i].iw);
            }
        }
    }
    pvs_plan_cur = saved_plan;
    pvs_prog_cur = saved_prog;
    ++pvs_verify_jobs;
    pvs_verify_verts += d->count;
    {
        double now = (double)pica_pqt_now_ns() / 1e9;
        if (now >= pvs_verify_next_rep) {
            pvs_verify_next_rep = now + 5.0;
            fprintf(stderr, "VSPARVERIFY jobs=%llu verts=%llu mismatches=%llu "
                            "(this job: %u/%u) draw=%llu\n",
                    pvs_verify_jobs, pvs_verify_verts, pvs_verify_bad, bad,
                    d->count, (unsigned long long)pica_draw_count);
        }
    }
}

/* Main thread: wait until every participant that JOINED this generation has
   left it.  `live` is one word, so the {joined,done} pair observed here is a
   consistent cut: a join recorded after the cut has not been seen, and its
   owner has not incremented `done` either, so the job is still open for it --
   which is exactly what makes "done == joined" mean "nobody is inside a chunk
   of this generation any more" instead of "n fixed increments happened".

   t_e1773c51 step 1b -- the fix for the freeze the operator hit on b59: the
   leave token MUST be read before the condition is checked a second time.
   `wake` is a monotonically increasing count of leaves, so reading it after
   the final leave yields the post-leave value; parking with that value as
   `expected` matches the futex word and sleeps through the completion it is
   waiting for, until the ring slot is reused PVS_JOB_RING jobs later.  Read
   token, re-check condition, then park (a leave that lands after the re-check
   bumps the token, so the park returns EAGAIN instead of sleeping).
   DQ8_VSPAR_WAIT_OLD=1 restores the old order for one run; it is a probe, not
   a supported mode.  The park is bounded (PVS_WAIT_PARK_NS) so no future
   ordering mistake can turn into an indefinite freeze. */
static void pvs_job_wait(PvsJobBuf *b) {
    unsigned spins = 0;
    uint64_t t0 = pica_pqt_now_ns();
    uint64_t deadline = pvs_wait_spin_ns ? t0 + pvs_wait_spin_ns : t0;
    int reported = 0, did_park = 0;
    unsigned ptok = 0;
    if (pvs_wait_oldorder < 0) {
        const char *e = getenv("DQ8_VSPAR_WAIT_OLD");
        pvs_wait_oldorder = (e && *e && *e != '0') ? 1 : 0;
    }
    for (;;) {
        uint64_t v = __atomic_load_n(&b->live, __ATOMIC_ACQUIRE);
        unsigned tok;
        if (pvs_live_done(v) >= pvs_live_joined(v)) {
            /* A park that returned with the condition true and the token
               unchanged was a lost wakeup: the leave that satisfied the
               condition happened before the token was read, so the unbounded
               park of the pre-fix code would never have returned. */
            if (did_park &&
                __atomic_load_n(&b->wake, __ATOMIC_ACQUIRE) == ptok) {
                if (__atomic_fetch_add(&pvs_wait_lostwake, 1ull, __ATOMIC_RELAXED) < 8u)
                    fprintf(stderr, "VSPARWAIT LOSTWAKE gen=%u joined=%u done=%u "
                                    "token=%u nchunks=%u next=%u -- the publisher "
                                    "parked on a token that already contained the "
                                    "last leave (pre-fix order)\n",
                            b->d.gen, pvs_live_joined(v), pvs_live_done(v), ptok,
                            b->d.nchunks, __atomic_load_n(&b->next, __ATOMIC_RELAXED));
            }
            return;
        }
        tok = __atomic_load_n(&b->wake, __ATOMIC_ACQUIRE);
        if (!pvs_wait_oldorder) {
            v = __atomic_load_n(&b->live, __ATOMIC_ACQUIRE);
            if (pvs_live_done(v) >= pvs_live_joined(v)) return;
        }
        if (pica_pqt_now_ns() < deadline) {
            if ((++spins & 0x3Fu) != 0) { PVS_PAUSE(); continue; }
            continue;
        }
        {
            uint64_t el = pica_pqt_now_ns() - t0;
            if (!reported && el > PVS_WAIT_STALL_NS) {
                reported = 1;
                __atomic_fetch_add(&pvs_wait_stall, 1ull, __ATOMIC_RELAXED);
                fprintf(stderr, "VSPARWAIT STALL gen=%u joined=%u done=%u next=%u "
                                "nchunks=%u in_body=%llu waited_ms=%.1f\n",
                        b->d.gen, pvs_live_joined(v), pvs_live_done(v),
                        __atomic_load_n(&b->next, __ATOMIC_RELAXED), b->d.nchunks,
                        (unsigned long long)__atomic_load_n(&pvs_in_body, __ATOMIC_RELAXED),
                        (double)el / 1e6);
            } else if (el > 1000000ull) {
                __atomic_fetch_add(&pvs_wait_slow, 1ull, __ATOMIC_RELAXED);
            }
        }
        ++pvs_wait_blocks;
        ptok = tok;
        did_park = 1;
        pvs_park_to(&b->wake, tok, PVS_WAIT_PARK_NS);
    }
}

static void *pvs_par_worker(void *arg) {
    /* Do not run a generation that was published before this thread existed:
       the main thread does not wait for participants that never joined, and a
       worker that starts with a stale `seen` would otherwise "participate" in
       a job that is already finished (the old protocol counted exactly those
       increments). */
    unsigned seen;
    (void)arg;
    pvs4_cnt_register();
    seen = __atomic_load_n(&pvs_job_gen, __ATOMIC_ACQUIRE);
    for (;;) {
        unsigned g;
        unsigned spins = 0;
        uint64_t deadline;
        if (__atomic_load_n(&pvs_par_stop, __ATOMIC_RELAXED)) return NULL;
        g = __atomic_load_n(&pvs_job_gen, __ATOMIC_ACQUIRE);
        if (g == seen) {
            deadline = pica_pqt_now_ns() + pvs_spin_ns;
            while (__atomic_load_n(&pvs_job_gen, __ATOMIC_ACQUIRE) == seen) {
                if (__atomic_load_n(&pvs_par_stop, __ATOMIC_RELAXED)) return NULL;
                if ((++spins & 0xFFu) != 0) { PVS_PAUSE(); continue; }
                if (pica_pqt_now_ns() > deadline) break;   /* spin budget spent */
            }
            g = __atomic_load_n(&pvs_job_gen, __ATOMIC_ACQUIRE);
            if (g == seen) {
                /* Idle: block until the publisher bumps the generation.  A
                   spurious wake (signal, or a store that lost the race with
                   the wait) just re-checks from the top. */
                ++pvs_block_count;
                pvs_park(&pvs_job_gen, seen);
                continue;
            }
        }
        seen = g;
        {
            PvsJobBuf *pub = &pvs_job_ring[g % PVS_JOB_RING];
            PvsJobDesc job;
            int level = pvs_stress_on();
            /* Late-join window: in stress mode a participant that was woken
               late is simulated by delaying the join itself. */
            if (level == 2 || (level == 1 && (pvs_stress_rand() & 7u) == 0)) {
                struct timespec ts;
                ts.tv_sec = 0;
                ts.tv_nsec = 20000 + (long)(pvs_stress_rand() % 180000u);
                nanosleep(&ts, NULL);
            }
            if (__atomic_load_n(&pub->d.gen, __ATOMIC_ACQUIRE) != g) continue;
            job = pub->d;                       /* per-thread argument snapshot */
            if (__atomic_load_n(&pub->d.gen, __ATOMIC_ACQUIRE) != g) continue;
            if (!pvs_job_join(pub)) continue;   /* job closed: run nothing */
            pvs_wjob = &job;
            pvs_wgen = g;
            pvs_body_run(pub, &job);
            pvs_wjob = NULL;
            pvs_wgen = 0;
            pvs_job_leave(pub);
        }
    }
}

static void pvs_par_init(void) {
    unsigned want, i;
    const char *e;
    if (pvs_par >= 0) return;
    e = getenv("DQ8_VSPAR");
    /* t_6f9e3fed: default workers 4 -> 8 (PVS_PAR_MAX).  The field scene
       transforms ~52.7k vertices per rendered frame and the pool measured
       186-192 us/draw at 4 workers vs 132 us/draw at 8 on the same binary
       (DRAWPHASE, field scene), i.e. -29 % of the vertex phase.  Output must be
       bit-identical: DQ8_VSPAR_VERIFY=1 compares every pooled transform against
       the serial reference, and DQ8_DETERMINISTIC=1 runs with 4 vs 8 workers
       produce the same frames (reports/VSPAR8_t_6f9e3fed.md).  DQ8_VSPAR=0..8
       still overrides. */
    pvs_par = (e && *e) ? atoi(e) : 8;
    if (pvs_par < 0) pvs_par = 0;
    if (pvs_par > PVS_PAR_MAX) pvs_par = PVS_PAR_MAX;
    {
        const char *se = getenv("DQ8_VSPAR_SPIN");
        if (se && *se) {
            long us = atol(se);
            pvs_spin_ns = us > 0 ? (uint64_t)us * 1000u : 0u;
        }
        se = getenv("DQ8_VSPAR_WAIT_SPIN");
        if (se && *se) {
            long us = atol(se);
            pvs_wait_spin_ns = us > 0 ? (uint64_t)us * 1000u : 0u;
        }
        se = getenv("DQ8_VSPAR_MIN_VERTS");
        if (se && *se) {
            long n = atol(se);
            pvs_min_verts = n > 0 ? (unsigned)n : 0u;
        }
    }
    pvs_vcap = 65536u;
    pvs_vout = (PicaVertex *)malloc(pvs_vcap * sizeof *pvs_vout);
    pvs_vst = (uint8_t *)malloc(pvs_vcap);
    pvs_vdet = (uint32_t *)malloc(pvs_vcap * sizeof *pvs_vdet);
    if (!pvs_vout || !pvs_vst || !pvs_vdet) { pvs_par = 0; return; }
    {
        const char *ve = getenv("DQ8_VSPAR_VERIFY");
        pvs_verify = (ve && *ve && *ve != '0') ? 1 : 0;
    }
    if (pvs_verify) {
        pvs_vref = (PicaVertex *)malloc(pvs_vcap * sizeof *pvs_vref);
        pvs_vsref = (uint8_t *)malloc(pvs_vcap);
        pvs_vdref = (uint32_t *)malloc(pvs_vcap * sizeof *pvs_vdref);
        if (!pvs_vref || !pvs_vsref || !pvs_vdref) pvs_verify = 0;
    }
    want = (unsigned)pvs_par;
    for (i = 0; i < want; ++i) {
        pthread_t th;
        if (pthread_create(&th, NULL, pvs_par_worker, NULL) != 0) break;
        pthread_detach(th);
        ++pvs_par_nw;
    }
    if (!pvs_par_nw) pvs_par = 0;
    /* t_e1773c51: one descriptor ring for the whole run (never freed; the
       workers hold snapshots of entries that a stuck participant could still
       be reading).  PVS_JOB_RING generations of slack is minutes of run time. */
    if (!pvs_job_ring) {
        pvs_job_ring = (PvsJobBuf *)calloc(PVS_JOB_RING, sizeof *pvs_job_ring);
        if (!pvs_job_ring) pvs_par = 0;
    }
    if (pvs_stress_on()) {          /* stress: park between jobs, one chunk */
        pvs_spin_ns = 0;
        pvs_wait_spin_ns = 0;
    }
    if (pvs_par < 0) pvs_par = 0;
    fprintf(stderr, "VSPAR workers=%u chunk=%s spin_us=%llu wait_spin_us=%llu "
            "min_verts=%u futex=%d job=%s stress=%d\n", pvs_par_nw,
            getenv("DQ8_VSPAR_CHUNK") ? getenv("DQ8_VSPAR_CHUNK") : "auto",
            (unsigned long long)(pvs_spin_ns / 1000u),
            (unsigned long long)(pvs_wait_spin_ns / 1000u), pvs_min_verts,
            PVS_HAVE_FUTEX, "gen-descriptor", pvs_stress_on());
    pvs4_cnt_register();
}

/* ===========================================================================
 * Per-draw transformed-vertex cache (t_6f9e3fed).
 *
 * A 3D field frame re-submits ~200 draws / ~50k vertices every frame, and most
 * world geometry is static between frames: the same vertex buffer bytes, the
 * same VS program, the same uniforms and the same attribute layout transform
 * to the same PicaVertex array.  The transform (attribute decode + VS
 * microcode + viewport projection) is the single largest term of the draw
 * path, so a hit skips it entirely and hands the previous frame's output to
 * the serial assembly loop.
 *
 * The key is built from separately hashed components so a miss can say WHICH
 * input moved:
 *   regs   - the PICA register file (attribute config 0x201..0x224, index
 *            format/offset 0x227/0x22A, input/output maps 0x2B9..0x2BD/0x4F
 *            ..0x56, viewport 0x41/0x43/0x68, ...)
 *   prog   - uploaded VS program + swizzle tables + default attribs
 *   uni    - the VS uniform block
 *   attr   - derived attribute offset/stride/format table + base/count/indexed
 *   data   - raw guest bytes the transform reads: the index buffer for indexed
 *            draws, the coalesced attribute byte ranges otherwise
 * A draw whose ranges cannot all be resolved in guest memory is not cacheable
 * and takes the normal path.  The cache is bypassed while VS tracing is armed
 * (DQ8_TRACE_VSDUMP) or when the pool is already running a nested job.
 *
 * DQ8_VSCACHE=0 disables it; DQ8_VSCACHE_STATS=1 prints hit/miss counters plus
 * a per-component repeat analysis (was a miss because the state or the vertex
 * bytes moved?); DQ8_VSCACHE_VERIFY=1 re-transforms every hit serially and
 * byte-compares the result (like DQ8_VSPAR_VERIFY).
 * ========================================================================= */
#define PVS_VC_SLOTS 512u
#define PVS_VC_BUDGET (96u * 1024u * 1024u)
#define PVS_VC_MAX_HASH (8u * 1024u * 1024u)
typedef struct {
    uint64_t key, hs, hd;
    uint32_t base, count;
    uint32_t tick;
    size_t bytes;
    PicaVertex *v;
    uint8_t *st;
    uint32_t *det;
} PvsVcEntry;
static PvsVcEntry pvs_vc_tab[PVS_VC_SLOTS];
static uint32_t pvs_vc_tick;
static size_t pvs_vc_bytes;
static int pvs_vc_on = -1;
static int pvs_vc_verify;
static int pvs_vc_stats;
static unsigned long long pvs_vc_hits, pvs_vc_misses, pvs_vc_kill,
                          pvs_vc_verts_hit, pvs_vc_verts_miss,
                          pvs_vc_hashed, pvs_vc_lookup_ns, pvs_vc_slot_miss;
/* Per-component repeat analysis (DQ8_VSCACHE_STATS=1): remembers the hashes of
   the previous draw with the same (base,count) and counts which components
   changed.  A draw whose five components all repeat is a cacheable hit if the
   table had room; this measures the ceiling independently of slot pressure. */
typedef struct {
    uint32_t base, count, used;
    uint64_t regs, prog, uni, attr, data;
    uint32_t snap_regs[0x300];
    float snap_uni[96][4];
} PvsVcDiag;
#define PVS_VC_DIAG 256u
static PvsVcDiag pvs_vc_diag[PVS_VC_DIAG];
static unsigned pvs_vc_dump_left = 8;
static unsigned long long pvs_vc_d_regs, pvs_vc_d_prog, pvs_vc_d_uni,
                          pvs_vc_d_attr, pvs_vc_d_data, pvs_vc_d_repeat,
                          pvs_vc_d_first;
static unsigned long long pvs_vc_uni_hist[96];
static unsigned long long pvs_vc_reg_hist[0x300];
static unsigned pvs_vc_uni_cmp;
static double pvs_vc_last_rep;
static PicaVertex *pvs_vc_vref;
static uint8_t *pvs_vc_sref;
static uint32_t *pvs_vc_dref;
static unsigned long long pvs_vc_verify_bad, pvs_vc_verify_n;

static int pvs_vc_init(void) {
    if (pvs_vc_on >= 0) return pvs_vc_on;
    {
        const char *e = getenv("DQ8_VSCACHE");
        /* t_6f9e3fed: OFF by default.  Measured on the field scene the sound
           input key hits only ~2 % of draws (the guest re-uploads the whole
           VS state every frame), so enabling it by default would cost the key
           hash without paying it back.  DQ8_VSCACHE=1 turns it on. */
        pvs_vc_on = (e && e[0] == '1' && !e[1]) ? 1 : 0;
    }
    {
        const char *e = getenv("DQ8_VSCACHE_STATS");
        pvs_vc_stats = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    {
        const char *e = getenv("DQ8_VSCACHE_VERIFY");
        pvs_vc_verify = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    if (pvs_vc_verify) {
        pvs_vc_vref = (PicaVertex *)malloc(pvs_vcap * sizeof *pvs_vc_vref);
        pvs_vc_sref = (uint8_t *)malloc(pvs_vcap);
        pvs_vc_dref = (uint32_t *)malloc(pvs_vcap * sizeof *pvs_vc_dref);
        if (!pvs_vc_vref || !pvs_vc_sref || !pvs_vc_dref) pvs_vc_verify = 0;
    }
    return pvs_vc_on;
}

/* t_6f9e3fed: which of the 96 VS uniform registers can the CURRENT program
   read?  Building the vertex-cache key from the whole uniform block made every
   field draw miss: the guest re-uploads the block every frame and slots the
   program never touches differ (camera rows for other passes, bone matrices,
   the 8x8 vs 16x16 texture-scale constant).  The operand layout is fixed by
   the instruction encoding, so the used set is a static property of the
   program:
     MAD-class (op >= 0x30): a = (w>>17)&31 (no relative), b/c = inverted
     operand field with the 2-bit relative selector at bit 22
     everything else:       a = (w>>12)&127 / inverted (w>>14)&31,
                            b = (w>>7)&31 / inverted (w>>7)&127, relative
                            selector at bit 19
   Any relative uniform address makes the index dynamic -> hash the whole
   block (conservative).  Flow-control opcodes are decoded with the same field
   layout, so treating them as ordinary instructions only over-approximates. */
static unsigned char pvs_uni_used[96];
static int pvs_uni_all;
static unsigned pvs_uni_used_n;
static void pvs_uni_mark(unsigned n, unsigned rel) {
    if (rel) { pvs_uni_all = 1; return; }
    if (n >= 32u) {
        unsigned i = n - 32u;
        if (i < 96u && !pvs_uni_used[i]) { pvs_uni_used[i] = 1; ++pvs_uni_used_n; }
    }
}
static void pvs_uni_mask_build(unsigned n) {
    unsigned pc;
    memset(pvs_uni_used, 0, sizeof pvs_uni_used);
    pvs_uni_all = 0;
    pvs_uni_used_n = 0;
    for (pc = 0; pc < n; ++pc) {
        uint32_t w = vs_program[pc];
        unsigned op = w >> 26;
        int mad = op >= 0x30;
        int inv = (op >= 0x18 && op <= 0x1b) || (op >= 0x30 && op < 0x38);
        unsigned rel = (w >> (mad ? 22 : 19)) & 3u;
        if (mad) {
            pvs_uni_mark((w >> 17) & 31u, 0);
            pvs_uni_mark(inv ? ((w >> 12) & 31u) : ((w >> 10) & 127u), inv ? 0 : rel);
            pvs_uni_mark(inv ? ((w >> 5) & 127u) : ((w >> 5) & 31u), inv ? rel : 0);
        } else {
            pvs_uni_mark(inv ? ((w >> 14) & 31u) : ((w >> 12) & 127u), inv ? 0 : rel);
            pvs_uni_mark(inv ? ((w >> 7) & 127u) : ((w >> 7) & 31u), inv ? rel : 0);
        }
    }
}

static void pvs_vc_diag_note(uint32_t base, uint32_t count, const uint64_t c[5]) {
    PvsVcDiag *d;
    if (!pvs_vc_stats) return;
    d = &pvs_vc_diag[(base / 4u + count * 2654435761u) % PVS_VC_DIAG];
    if (d->used && d->base == base && d->count == count) {
        unsigned any = 0;
        if (d->regs != c[0]) { ++pvs_vc_d_regs; any = 1; }
        if (d->prog != c[1]) { ++pvs_vc_d_prog; any = 1; }
        if (d->uni  != c[2]) { ++pvs_vc_d_uni;  any = 1; }
        if (d->attr != c[3]) { ++pvs_vc_d_attr; any = 1; }
        if (d->data != c[4]) { ++pvs_vc_d_data; any = 1; }
        if (!any) ++pvs_vc_d_repeat;
        if (d->uni != c[2]) {
            unsigned k;
            ++pvs_vc_uni_cmp;
            for (k = 0; k < 96u; k++)
                if (memcmp(d->snap_uni[k], vs_uniform_f[k], sizeof vs_uniform_f[k]))
                    ++pvs_vc_uni_hist[k];
        }
        if (d->regs != c[0]) {
            unsigned k;
            for (k = 0x200u; k <= 0x2bfu; k++)
                if (d->snap_regs[k] != pica_regs[k]) ++pvs_vc_reg_hist[k];
            if (d->snap_regs[0x41] != pica_regs[0x41]) ++pvs_vc_reg_hist[0x41];
            if (d->snap_regs[0x43] != pica_regs[0x43]) ++pvs_vc_reg_hist[0x43];
            if (d->snap_regs[0x68] != pica_regs[0x68]) ++pvs_vc_reg_hist[0x68];
            for (k = 0x4fu; k <= 0x56u; k++)
                if (d->snap_regs[k] != pica_regs[k]) ++pvs_vc_reg_hist[k];
        }
        if (any && pvs_vc_dump_left) {
            unsigned i, nregs = 0, nunis = 0;
            char rb[256], ub[256];
            rb[0] = ub[0] = 0;
            --pvs_vc_dump_left;
            for (i = 0; i < 0x300u; i++)
                if (d->snap_regs[i] != pica_regs[i] && nregs < 32u)
                    nregs += snprintf(rb + nregs, sizeof rb - nregs, " %03X:%08X>%08X",
                                      i, d->snap_regs[i], pica_regs[i]);
            for (i = 0; i < 96u; i++) {
                if (memcmp(d->snap_uni[i], vs_uniform_f[i], sizeof vs_uniform_f[i]) &&
                    nunis < 32u)
                    nunis += snprintf(ub + nunis, sizeof ub - nunis,
                                      " %u:[%g,%g,%g,%g]>[%g,%g,%g,%g]",
                                      i,
                                      (double)d->snap_uni[i][0], (double)d->snap_uni[i][1],
                                      (double)d->snap_uni[i][2], (double)d->snap_uni[i][3],
                                      (double)vs_uniform_f[i][0], (double)vs_uniform_f[i][1],
                                      (double)vs_uniform_f[i][2], (double)vs_uniform_f[i][3]);
            }
            fprintf(stderr, "VSCDIFF draw=%llu base=%08X count=%u regs[%u]:%s uni[%u]:%s\n",
                    (unsigned long long)pica_draw_count, base, count,
                    nregs, rb, nunis, ub);
        }
    } else {
        ++pvs_vc_d_first;
    }
    d->used = 1; d->base = base; d->count = count;
    d->regs = c[0]; d->prog = c[1]; d->uni = c[2]; d->attr = c[3]; d->data = c[4];
    memcpy(d->snap_regs, pica_regs, sizeof d->snap_regs);
    memcpy(d->snap_uni, vs_uniform_f, sizeof d->snap_uni);
}

/* Hash every input the vertex transform reads.  Returns 1 when the draw is
   cacheable, 0 when some range cannot be resolved.  `comp` receives the five
   component hashes (regs, prog, uni, attr, data). */
static int pvs_vc_key(uint32_t base, uint32_t count, int indexed,
                      uint64_t *out_hs, uint64_t *out_hd, uint64_t comp[5]) {
    const uint64_t seed = 14695981039346656037ull;
    uint64_t hr = seed, hp = seed, hu = seed, ha = seed, hd = seed;
    uint32_t i, max, imin, imax;
    unsigned ibytes = 0;
    size_t total = 0;
    struct { uint64_t a0, a1; } rng[12];
    int nr = 0;
    pica_vs_attr_ensure();
    /* Only the registers the vertex stage reads take part in the key: the
       VS/attribute block 0x200..0x2BF (attr config 0x201..0x224, index
       0x227/0x22A, bools 0x2B0..0x2B7, entry PC 0x2BA, in/out maps
       0x2B9..0x2BD), the viewport 0x41/0x43/0x68 and the output map
       0x4F..0x56.  Hashing the whole file made every draw miss because the
       fragment-side TEV/texture registers (0xE0, 0xF3, 0x82, 0x85, ...) differ
       between two draws of the SAME geometry with a different texture. */
    for (i = 0x200u; i <= 0x2bfu; i++)
        hr = elide_hash_bytes(hr, (const uint8_t *)&pica_regs[i], 4);
    for (i = 0; i < 3u; i++) {
        static const unsigned vp[3] = { 0x41u, 0x43u, 0x68u };
        hr = elide_hash_bytes(hr, (const uint8_t *)&pica_regs[vp[i]], 4);
    }
    for (i = 0x4fu; i <= 0x56u; i++)
        hr = elide_hash_bytes(hr, (const uint8_t *)&pica_regs[i], 4);
    hp = elide_hash_bytes(hp, (const uint8_t *)vs_program,
                          (size_t)vs_program_size * 4u);
    hp = elide_hash_bytes(hp, (const uint8_t *)vs_swizzle,
                          (size_t)vs_swizzle_size * 4u);
    hp = elide_hash_bytes(hp, (const uint8_t *)vs_default_attrib,
                          sizeof vs_default_attrib);
    {
        static int no_uni = -1;
        static uint64_t uni_hash;
        static int uni_built;
        unsigned n = vs_program_size, k;
        uint64_t uh = 1469598103934665603ULL;
        /* Hash only the uniform registers the program can read (see
           pvs_uni_mask_build).  Rebuild the mask when the program changes. */
        if (no_uni < 0) {
            const char *e = getenv("DQ8_VSCACHE_NOUNI");
            no_uni = (e && e[0] && e[0] != '0') ? 1 : 0;
        }
        for (k = 0; k < n; k++) { uh ^= vs_program[k]; uh *= 1099511628211ULL; }
        uh ^= (uint64_t)n;
        if (!uni_built || uh != uni_hash) {
            pvs_uni_mask_build(n);
            uni_hash = uh;
            uni_built = 1;
        }
        if (!no_uni) {
            if (pvs_uni_all) {
                hu = elide_hash_bytes(hu, (const uint8_t *)vs_uniform_f,
                                      sizeof vs_uniform_f);
            } else {
                for (k = 0; k < 96u; k++)
                    if (pvs_uni_used[k])
                        hu = elide_hash_bytes(hu, (const uint8_t *)vs_uniform_f[k], 16u);
            }
        }
    }
    ha = elide_hash_bytes(ha, (const uint8_t *)pica_vs_attr, sizeof pica_vs_attr);
    ha = elide_hash_bytes(ha, (const uint8_t *)&base, sizeof base);
    ha = elide_hash_bytes(ha, (const uint8_t *)&count, sizeof count);
    ha = elide_hash_bytes(ha, (const uint8_t *)&indexed, sizeof indexed);
    max = pica_regs[0x2b9] & 15u;
    if (max >= 12u) return 0;
    if (indexed) {
        uint32_t mapped;
        uint64_t iaddr = (uint64_t)base + (pica_regs[0x227] & 0x0fffffffu);
        uint64_t ilen;
        ibytes = (pica_regs[0x227] >> 31) ? 2u : 1u;
        ilen = (uint64_t)count * ibytes;
        if (!ilen || ilen > PVS_VC_MAX_HASH || iaddr + ilen > UINT32_MAX)
            return 0;
        if (!guest_memory_range((uint32_t)iaddr, (uint32_t)ilen, &mapped))
            return 0;
        hd = elide_hash_bytes(hd, ram + mapped, (size_t)ilen);
        imin = 0xffffffffu; imax = 0;
        for (i = 0; i < count; i++) {
            uint32_t ix = ram[mapped + i * ibytes];
            if (ibytes == 2u) ix |= (uint32_t)ram[mapped + i * ibytes + 1] << 8;
            if (ix < imin) imin = ix;
            if (ix > imax) imax = ix;
        }
        if (imin > imax) return 0;
    } else {
        imin = pica_regs[0x22a];
        imax = imin + count - 1u;
    }
    for (i = 0; i <= max; i++) {
        uint64_t a0, a1;
        size_t len;
        /* Attributes fed from the default register value are never read. */
        if (pica_regs[0x202] & (1u << (16u + i))) continue;
        if (!pica_vs_attr[i].valid) return 0;
        a0 = (uint64_t)base + pica_vs_attr[i].off +
             (uint64_t)imin * pica_vs_attr[i].stride;
        a1 = (uint64_t)base + pica_vs_attr[i].off +
             (uint64_t)imax * pica_vs_attr[i].stride +
             (uint64_t)pica_vs_attr[i].bytes * pica_vs_attr[i].n;
        if (a1 <= a0 || a1 > UINT32_MAX) return 0;
        len = (size_t)(a1 - a0);
        total += len;
        if (total > PVS_VC_MAX_HASH) return 0;
        rng[nr].a0 = a0; rng[nr].a1 = a1; ++nr;
    }
    /* Coalesce overlapping attribute ranges (interleaved attributes overlap
       almost completely) so each guest byte is hashed once. */
    for (i = 1; i < (uint32_t)nr; i++) {
        uint32_t j = i;
        while (j > 0 && rng[j - 1].a0 > rng[j].a0) {
            uint64_t t0 = rng[j - 1].a0, t1 = rng[j - 1].a1;
            rng[j - 1].a0 = rng[j].a0; rng[j - 1].a1 = rng[j].a1;
            rng[j].a0 = t0; rng[j].a1 = t1;
            --j;
        }
    }
    total = 0;
    for (i = 0; i < (uint32_t)nr; i++) {
        uint64_t a0 = rng[i].a0, a1 = rng[i].a1;
        uint32_t mapped;
        if (i + 1u < (uint32_t)nr && rng[i + 1u].a0 <= a1) {
            uint32_t k = i + 1u;
            while (k < (uint32_t)nr && rng[k].a0 <= a1) {
                if (rng[k].a1 > a1) a1 = rng[k].a1;
                ++k;
            }
            i = k - 1u;
        }
        total += (size_t)(a1 - a0);
        if (total > PVS_VC_MAX_HASH) return 0;
        if (!guest_memory_range((uint32_t)a0, (uint32_t)(a1 - a0), &mapped))
            return 0;
        hd = elide_hash_bytes(hd, ram + mapped, (size_t)(a1 - a0));
    }
    pvs_vc_hashed += total;
    comp[0] = hr ? hr : 1u;
    comp[1] = hp ? hp : 1u;
    comp[2] = hu ? hu : 1u;
    comp[3] = ha ? ha : 1u;
    comp[4] = hd ? hd : 1u;
    pvs_vc_diag_note(base, count, comp);
    {
        uint64_t hs = hr ^ (hp + 0x9E3779B97F4A7C15ull + (hr << 6) + (hr >> 2));
        hs ^= hu + 0x9E3779B97F4A7C15ull + (hs << 6) + (hs >> 2);
        hs ^= ha + 0x9E3779B97F4A7C15ull + (hs << 6) + (hs >> 2);
        *out_hs = hs ? hs : 1u;
    }
    *out_hd = hd;
    return 1;
}

static size_t pvs_vc_entry_bytes(uint32_t count) {
    return (size_t)count * (sizeof(PicaVertex) + 1u + sizeof(uint32_t));
}

static void pvs_vc_entry_free(PvsVcEntry *e) {
    free(e->v); free(e->st); free(e->det);
    pvs_vc_bytes -= e->bytes;
    e->v = NULL; e->st = NULL; e->det = NULL;
    e->bytes = 0; e->count = 0; e->key = 0;
}

static int pvs_vc_lookup(uint64_t key, uint32_t count) {
    /* Direct-mapped: one entry per slot, no probe chains to break on eviction.
       A key collision with a different draw is just a miss (the key is
       re-verified), never a wrong hit. */
    PvsVcEntry *e = &pvs_vc_tab[(unsigned)(key % PVS_VC_SLOTS)];
    if (e->key != key || e->count != count || !e->v) {
        ++pvs_vc_slot_miss;
        return 0;
    }
    memcpy(pvs_vout, e->v, (size_t)count * sizeof(PicaVertex));
    memcpy(pvs_vst, e->st, count);
    memcpy(pvs_vdet, e->det, (size_t)count * sizeof(uint32_t));
    e->tick = ++pvs_vc_tick;
    return 1;
}

static void pvs_vc_store(uint64_t key, uint64_t hs, uint64_t hd,
                         uint32_t base, uint32_t count) {
    PvsVcEntry *e = &pvs_vc_tab[(unsigned)(key % PVS_VC_SLOTS)];
    size_t bytes = pvs_vc_entry_bytes(count);
    if (bytes > PVS_VC_BUDGET) return;
    if (e->key != key || e->count != count) pvs_vc_entry_free(e);
    if (!e->v || !e->st || !e->det) {
        PicaVertex *v = (PicaVertex *)malloc((size_t)count * sizeof(PicaVertex));
        uint8_t *st = (uint8_t *)malloc(count);
        uint32_t *det = (uint32_t *)malloc((size_t)count * sizeof(uint32_t));
        if (!v || !st || !det) { free(v); free(st); free(det); return; }
        pvs_vc_entry_free(e);   /* releases any previous, smaller buffers */
        e->v = v; e->st = st; e->det = det;
        e->count = count;
        e->bytes = bytes;
        pvs_vc_bytes += bytes;
    }
    while (pvs_vc_bytes > PVS_VC_BUDGET) {
        PvsVcEntry *victim = NULL;
        unsigned i;
        for (i = 0; i < PVS_VC_SLOTS; i++) {
            PvsVcEntry *x = &pvs_vc_tab[i];
            if (!x->key || x == e) continue;
            if (!victim || x->tick < victim->tick) victim = x;
        }
        if (!victim) break;
        pvs_vc_entry_free(victim);
    }
    memcpy(e->v, pvs_vout, (size_t)count * sizeof(PicaVertex));
    memcpy(e->st, pvs_vst, count);
    memcpy(e->det, pvs_vdet, (size_t)count * sizeof(uint32_t));
    e->key = key;
    e->hs = hs;
    e->hd = hd;
    e->base = base;
    e->count = count;
    e->tick = ++pvs_vc_tick;
}

/* Serial reference for DQ8_VSCACHE_VERIFY: transform every vertex again from
   the same guest state and compare against the cached array byte-for-byte. */
static void pvs_vc_do_verify(uint32_t base, uint32_t count, int indexed) {
    uint32_t i;
    unsigned bad = 0;
    for (i = 0; i < count; i++) {
        uint32_t index = pica_regs[0x22a] + i;
        memset(&pvs_vc_vref[i], 0, sizeof pvs_vc_vref[i]);
        pvs_vc_dref[i] = 0;
        if (indexed) {
            unsigned bytes = (pica_regs[0x227] >> 31) ? 2u : 1u;
            uint32_t mapped;
            uint64_t addr = (uint64_t)base + (pica_regs[0x227] & 0x0fffffffu) +
                            (uint64_t)i * bytes;
            if (addr > UINT32_MAX ||
                !guest_memory_range((uint32_t)addr, bytes, &mapped)) {
                pvs_vc_sref[i] = PVS_ST_INDEX_FAIL;
                continue;
            }
            index = ram[mapped];
            if (bytes == 2u) index |= (uint32_t)ram[mapped + 1] << 8;
        }
        if (!pica_load_vertex(base, index, &pvs_vc_vref[i])) {
            pvs_vc_sref[i] = (uint8_t)pica_vs_fail_reason;
            pvs_vc_dref[i] = pica_vs_fail_detail;
        } else {
            pvs_vc_sref[i] = 0u;
        }
        if (pvs_vc_sref[i] != pvs_vst[i] ||
            memcmp(&pvs_vc_vref[i], &pvs_vout[i], sizeof(PicaVertex)) != 0 ||
            (pvs_vc_sref[i] && pvs_vc_dref[i] != pvs_vdet[i])) {
            ++pvs_vc_verify_bad;
            ++bad;
            if (pvs_vc_verify_bad <= 8u)
                fprintf(stderr, "VSCACHE MISMATCH draw=%llu vert=%u/%u status cache=%u ref=%u\n",
                        (unsigned long long)pica_draw_count, i, count,
                        pvs_vst[i], pvs_vc_sref[i]);
        }
    }
    pvs_vc_verify_n += count;
    if (bad || (pvs_vc_verify_n % 1000000u) < count)
        fprintf(stderr, "VSCACHE VERIFY verts=%llu mismatches=%llu (this draw %u/%u)\n",
                pvs_vc_verify_n, pvs_vc_verify_bad, bad, count);
}

static void pvs_vc_report(void) {
    double now;
    if (!pvs_vc_stats) return;
    now = (double)pica_pqt_now_ns() / 1e9;
    if (pvs_vc_last_rep == 0.0) { pvs_vc_last_rep = now; return; }
    if (now - pvs_vc_last_rep < 2.0) return;
    fprintf(stderr, "VSCACHE hits=%llu misses=%llu uncacheable=%llu verts_hit=%llu "
            "verts_miss=%llu hashed_mb=%.1f lookup_us=%.0f live_mb=%.1f "
            "verify=%llu/%llu\n",
            pvs_vc_hits, pvs_vc_misses, pvs_vc_kill,
            pvs_vc_verts_hit, pvs_vc_verts_miss,
            (double)pvs_vc_hashed / (1024.0 * 1024.0),
            (double)pvs_vc_lookup_ns / 1e3,
            (double)pvs_vc_bytes / (1024.0 * 1024.0),
            pvs_vc_verify_n, pvs_vc_verify_bad);
    fprintf(stderr, "VSCDIAG first=%llu repeat=%llu changed[regs=%llu prog=%llu "
            "uni=%llu attr=%llu data=%llu]\n",
            pvs_vc_d_first, pvs_vc_d_repeat, pvs_vc_d_regs, pvs_vc_d_prog,
            pvs_vc_d_uni, pvs_vc_d_attr, pvs_vc_d_data);
    {
        unsigned i, n = 0;
        char buf[512];
        buf[0] = 0;
        for (i = 0; i < 96u && n < 400u; i++)
            if (pvs_vc_uni_hist[i])
                n += snprintf(buf + n, sizeof buf - n, " %u:%llu", i, pvs_vc_uni_hist[i]);
        fprintf(stderr, "VSCUNI cmp=%u hist:%s\n", pvs_vc_uni_cmp, buf);
        n = 0; buf[0] = 0;
        for (i = 0; i < 0x300u && n < 400u; i++)
            if (pvs_vc_reg_hist[i])
                n += snprintf(buf + n, sizeof buf - n, " %X:%llu", i, pvs_vc_reg_hist[i]);
        fprintf(stderr, "VSCREG hist:%s\n", buf);
    }
    pvs_vc_last_rep = now;
}

/* Transform `count` vertices into pvs_vout[]/pvs_vst[].  Returns 1 when the
   array is ready for the serial assembly loop, 0 when the caller must run the
   original scalar loop (workers off, tracing on, tiny draw, nested call). */
static int pvs_par_begin(uint32_t base, uint32_t count, int indexed) {
    unsigned c, chunk, total;
    uint64_t t0, t1;
    int cache_key_ok = 0;
    uint64_t cache_key = 0, cache_hs = 0, cache_hd = 0;
    if (pvs_par < 0) pvs_par_init();
    if (!pvs_vout || pvs_par_injob || count < 16u || count > pvs_vcap) return 0;
    if (pvs_f != NULL) return 0;         /* VS tracing inspects per-vertex state */
    /* t_6f9e3fed: transformed-vertex cache.  A hit skips attribute decode, VS
       execution and projection for the whole draw. */
    if (pvs_vc_init()) {
        uint64_t t_lk = pica_pqt_now_ns();
        uint64_t comp[5];
        cache_key_ok = pvs_vc_key(base, count, indexed, &cache_hs, &cache_hd, comp);
        if (cache_key_ok) {
            cache_key = cache_hs ^ (cache_hd + 0x9E3779B97F4A7C15ull +
                                    (cache_hs << 6) + (cache_hs >> 2));
            if (cache_key == 0) cache_key = 1u;
            if (pvs_vc_lookup(cache_key, count)) {
                pvs_vc_lookup_ns += pica_pqt_now_ns() - t_lk;
                ++pvs_vc_hits;
                pvs_vc_verts_hit += count;
                if (pvs_vc_verify) pvs_vc_do_verify(base, count, indexed);
                return 1;
            }
            ++pvs_vc_misses;
            pvs_vc_verts_miss += count;
        } else {
            ++pvs_vc_kill;
        }
        pvs_vc_lookup_ns += pica_pqt_now_ns() - t_lk;
    }
    if (pvs_par <= 0) return 0;
    pvs_par_injob = 1;
    /* t_968a1fa9: build the per-draw attribute table HERE, on the main thread,
       before the job is published.  It used to be lazily filled by whichever
       participant reached its first vertex, i.e. all of them at once, and a
       slow reader could use a half-written entry.  Filling it here also means
       the whole pool transforms with the identical table the serial path
       would have used. */
    if (pica_vsfast) pica_vs_attr_ensure();
    {
        static unsigned chunk_cfg;
        static int chunk_init;
        if (!chunk_init) {
            /* t_275eb521: the flag is separate from the value -- with the old
               `if (!chunk_cfg)` test an unset (or "0") DQ8_VSPAR_CHUNK re-ran
               getenv/atoi on every draw. */
            const char *ce = getenv("DQ8_VSPAR_CHUNK");
            chunk_cfg = (ce && *ce) ? (unsigned)atoi(ce) : 0u;
            chunk_init = 1;
        }
        chunk = chunk_cfg;
    }
    if (!chunk) {
        /* Adaptive: aim for ~4 chunks per participant so a partial draw still
           balances, and keep chunks big enough that the atomic cursor is not
           the cost.  A 250-vertex draw over 5 participants -> 12. */
        unsigned parts = pvs_par_nw ? pvs_par_nw + 1u : 1u;
        chunk = count / (parts * 4u);
        if (chunk < 8u) chunk = 8u;
        if (chunk > 96u) chunk = 96u;
    }
    if (chunk > count) chunk = count;
    if (pvs_stress) chunk = 1;           /* stress: maximum publish rate */
    {
        PvsJobDesc d;
        PvsJobBuf *pub;
        memset(&d, 0, sizeof d);
        d.nchunks = (count + chunk - 1u) / chunk;
        d.run = pvs_par_chunk;
        d.base = base;
        d.count = count;
        d.chunk = chunk;
        d.indexed = indexed;
        d.plan = pvs_plan_cur;           /* snapshot: workers never read the
                                            main thread's newer pvs_plan_cur */
        d.prog = pvs_prog_cur;
        if (pvs_stress) d.plan_key = pvs_plan_key(vs_program_size);
        total = d.nchunks;
        pvs_par_jobs++;
        if (!pvs_par_nw || count < pvs_min_verts) {
            /* Inline: either no workers, or a draw too small to pay for a wake. */
            PvsJobBuf lb;
            memset(&lb, 0, sizeof lb);
            pvs_body_run(&lb, &d);
            ++pvs_inline_jobs;
            if (cache_key_ok) pvs_vc_store(cache_key, cache_hs, cache_hd, base, count);
            return 1;
        }
        pub = pvs_job_publish(&d);
        {
            PvsJobDesc jd = pub->d;
            pvs_wjob = &jd;
            pvs_wgen = jd.gen;
            t0 = pica_pqt_now_ns();
            pvs_body_run(pub, &jd);      /* main thread participates */
            t1 = pica_pqt_now_ns();
            pvs_job_leave(pub);
            pvs_job_close(pub);          /* no more joins: wait only for leavers */
            pvs_job_wait(pub);           /* every joined participant has left */
            pvs_wjob = NULL;
            pvs_wgen = 0;
            {
                uint64_t t2 = pica_pqt_now_ns();
                pvs_par_mainwork_ns += t1 - t0;
                pvs_par_wait_ns += t2 - t1;
            }
            if (pvs_verify) pvs_par_verify(&jd);   /* t_968a1fa9 */
        }
        if (cache_key_ok) pvs_vc_store(cache_key, cache_hs, cache_hd, base, count);
        (void)c;
        return 1;
    }
}
static void pvs_par_end(void) { pvs_par_injob = 0; }

/* Run fn(ctx, i0, i1) over [lo,hi) on the pool.  Returns 1 when the job ran in
   parallel, 0 when it ran on the calling thread (pool off, nested, or tiny). */
static int pvs_par_run(void (*fn)(void *, unsigned, unsigned), void *ctx,
                       unsigned lo, unsigned hi) {
    unsigned parts, chunk;
    if (pvs_par < 0) pvs_par_init();
    if (pvs_par <= 0 || pvs_par_injob || hi <= lo + 1u) { fn(ctx, lo, hi); return 0; }
    pvs_par_injob = 1;
    {
        PvsJobDesc d;
        PvsJobBuf *pub;
        memset(&d, 0, sizeof d);
        d.gfn = fn;
        d.gctx = ctx;               /* NOTE: may be a caller stack frame
                                       (pgl_wb_*_blocks passes &c).  It stays
                                       valid because pvs_job_wait() below only
                                       returns once every participant that
                                       joined this generation has left it. */
        d.gbase = lo;
        d.gn = hi - lo;
        parts = pvs_par_nw + 1u;
        chunk = d.gn / (parts * 4u);
        if (chunk < 1u) chunk = 1u;
        if (chunk > 4u) chunk = 4u;
        if (pvs_stress) chunk = 1;
        d.gchunk = chunk;
        d.nchunks = (d.gn + chunk - 1u) / chunk;
        d.run = pvs_par_chunk_gen;
        pvs_par_jobs++;
        pub = pvs_job_publish(&d);
        {
            PvsJobDesc jd = pub->d;
            pvs_wjob = &jd;
            pvs_wgen = jd.gen;
            pvs_body_run(pub, &jd);
            pvs_job_leave(pub);
            pvs_job_close(pub);
            pvs_job_wait(pub);
            pvs_wjob = NULL;
            pvs_wgen = 0;
        }
    }
    pvs_par_injob = 0;
    return 1;
}
/* Forward declaration used by the GL write-back (pica_gl.c is emitted first). */
static int pgl_par_run(void (*fn)(void *, unsigned, unsigned), void *ctx,
                       unsigned lo, unsigned hi) {
    return pvs_par_run(fn, ctx, lo, hi);
}

static void pica_draw_arrays(uint32_t trigger,int indexed) {
    long long pqt_t0 = 0;
    unsigned long long pqt_gl0 = 0;
    /* DQ8_TRACE_TIME: coarse phase split of the whole draw submission
       (t_77ac52ce): staging/prepare, vertex+clip, triangle flush (which
       includes the GL draw). */
    unsigned long long pvs_ph_t0 = 0, pvs_ph_t1 = 0, pvs_ph_t2 = 0;
    unsigned long long pvs_vs_t0 = 0;
    static unsigned long long pvs_ph_n, pvs_ph_prep, pvs_ph_vert, pvs_ph_flush,
                              pvs_ph_lv, pvs_ph_nv;
    static double pvs_ph_last;
    static int pvs_gpu_trace = -1; /* DQ8_TRACE_GPU, cached once (t_bc6b4eb7) */
    (void)trigger;
    if (pqt_on < 0) {
        const char *e = getenv("DQ8_TRACE_TIME");
        pqt_on = (e && e[0] && e[0] != '0');
    }
    if (pvs_gpu_trace < 0) {
        /* t_bc6b4eb7: this getenv ran INSIDE the per-vertex loop (~78k
           calls/s = 5 % of main-thread samples in the frame-budget report) —
           resolve it once per process, same caching pattern as pqt_on. */
        const char *e = getenv("DQ8_TRACE_GPU");
        pvs_gpu_trace = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    if (pqt_on) { pqt_t0 = (long long)pica_pqt_now_ns(); pqt_gl0 = pica_gl_draws_ok; pqt_slow = 0; }
    pica_bind_framebuffer();pica_bind_depth();
    pica_draw_state_build();   /* per-draw blend fast-path snapshot (t_568f6976) */
    pica_db_reset();
    pica_px_init();
    memset(pica_px_wr, 0, sizeof pica_px_wr);
    for (int k = 0; k < 96; k++) pica_px_me[k] = 1e9f;
    {   /* t_bc6b4eb7: per-draw getenv cached once per process */
        static int tev_trace = -1;
        if (tev_trace < 0) {
            const char *e = getenv("DQ8_TRACE_TEV");
            tev_trace = (e && e[0] && e[0] != '0') ? 1 : 0;
        }
        if (tev_trace) pica_trace_tev_state();
    }
    uint32_t count=pica_regs[0x228],base=((pica_regs[0x200]>>1)&0xfffffff)*16;
    pica_draw_log_count=count;pica_draw_log_tris=0;
    pica_draw_log_clip=0;pica_draw_log_wmin=1e30f;pica_draw_log_wmax=-1e30f;
    pica_draw_log_nfdrop=0;
    pica_draw_log_arej=0; pica_draw_log_drej=0; pica_draw_log_frw=0;
    pica_draw_log_fcw=0; pica_draw_log_fccw=0;
    pica_cull_kept_tris=0; pica_cull_dropped_tris=0;
    memset(pica_thist, 0, sizeof pica_thist);
    pica_cov_px = 0;
    pica_clst_reset();
    pvs_open_for_draw(count);
    pvs_prog_dump_once();
#if defined(PVS_SSE)
    pvs_sse_sync();           /* follow the guest's swizzle-table uploads */
    pvs_dec_sync();           /* t_469b3d92: re-decode the VS program once */
    pvs_plan_sync();          /* t_fdc40e6f: prepared plan, cached per program */
    pvs_ghist_init();         /* t_6f9e3fed: DQ8_VSDEC_HIST=1 */
#endif
    pica_vs_attr_ready = 0;   /* t_5a3c517a: rebuild the attribute table once per draw */
    if(!count || count>65536){pica_vs_fail_reason=1;pica_vs_fail_detail=count;pica_log_draw(1);goto pqt_done;}
    PicaVertex vertices[3];unsigned topology=(pica_regs[0x25e]>>8)&3;
    unsigned vok[3]={0,0,0}; /* validity of the primitive-assembly slots */
    if (pqt_on) pvs_ph_t0 = pica_pqt_now_ns();
    uint64_t pf_vt0 = pf_trap_armed() ? pica_pqt_now_ns() : 0;
    pica_gl_prepare_draw();
    if (pqt_on) pvs_ph_t1 = pica_pqt_now_ns();
    /* t_5a3c517a: transform the whole draw on the worker pool first (the
       assembly loop below stays serial and in order).  pvs_par returns 1 only
       when pvs_vout[]/pvs_vst[] hold every vertex. */
    int pvs_par_ok = pvs_par_begin(base, count, indexed);
    for(uint32_t i=0;i<count;i++) {
        uint32_t index=0;
        PicaVertex v;
        unsigned par_status = 0;
        int load_ok;
        if (pvs_par_ok) {
            par_status = pvs_vst[i];
            v = pvs_vout[i];
            if (par_status == PVS_ST_INDEX_FAIL) goto done;
            /* t_968a1fa9: a pooled vertex that FAILED the transform must be
               handled exactly like a serial failure.  The pool writes the
               status of every vertex, so a failure can happen at any index —
               but the code used to fall through and queue the slot as if it
               were good geometry.  Since pica_load_vertex leaves its output
               untouched on failure, that queued whatever the previous draw
               left in the buffer: one vertex with a far-away position is the
               stretched polygon seen in every 3D scene. */
            load_ok = (par_status == 0u);
            if (par_status) {
                pica_vs_fail_reason = par_status;
                pica_vs_fail_detail = pvs_vdet[i];
            }
            if (pqt_on) ++pvs_ph_nv;
        } else {
        index=pica_regs[0x22a]+i;
        if(indexed) {
            unsigned bytes=(pica_regs[0x227]>>31)?2u:1u;uint32_t mapped;
            uint64_t addr=(uint64_t)base+(pica_regs[0x227]&0xfffffff)+(uint64_t)i*bytes;
            if(addr>UINT32_MAX || !guest_memory_range((uint32_t)addr,bytes,&mapped))goto done;
            index=ram[mapped];if(bytes==2)index|=(uint32_t)ram[mapped+1]<<8;
        }
        if (pqt_on) { pvs_vs_t0 = pica_pqt_now_ns(); ++pvs_ph_nv; }
        load_ok = pica_load_vertex(base,index,&v);
        }
        if(!load_ok) {
            if (pqt_on && !pvs_par_ok) pvs_ph_lv += pica_pqt_now_ns() - pvs_vs_t0;
            if(pica_vs_fail_reason==7u) {
                /* Nonfinite clip positions cannot be interpolated; drop only
                   the primitives that reference this vertex (hardware clips
                   primitives, it never aborts the draw — t_0b95f1a7).  w<=0
                   vertices now LOAD successfully and are clipped properly in
                   pica_queue_triangle_clip. */
                ++pica_draw_log_nfdrop;
                if(pica_vs_fail_reason<8u)pica_vs_fail_totals[pica_vs_fail_reason]++;
                if(topology==0u || topology==3u) vok[i%3]=0u;
                else {
                    if(topology==1u){vertices[0]=vertices[1];vok[0]=vok[1];}
                    vok[1]=0u;
                }
                continue;
            }
            static unsigned failures;
            if(pica_vs_fail_reason<8u)pica_vs_fail_totals[pica_vs_fail_reason]++;
            {
                const char *vsf=getenv("DQ8_TRACE_VSFAIL");
                unsigned cap=(vsf&&*vsf&&*vsf!='0')?64u:8u;
                if(failures++<cap)fprintf(stderr,"PICA VS unsupported/invalid draw=%llu vertex=%u/%u entry=%X rsn=%u det=%08X\n",
                        (unsigned long long)pica_draw_count,i,count,pica_regs[0x2ba],
                        pica_vs_fail_reason,pica_vs_fail_detail);
                if(vsf&&*vsf&&*vsf!='0' && (pica_draw_count%2048u)==0u)
                    fprintf(stderr,"VSFAIL totals draw=%llu skip=%llu maxattr=%llu attr=%llu mem=%llu vs=%llu w=%llu nf=%llu\n",
                            (unsigned long long)pica_draw_count,
                            (unsigned long long)pica_vs_fail_totals[1],(unsigned long long)pica_vs_fail_totals[2],
                            (unsigned long long)pica_vs_fail_totals[3],(unsigned long long)pica_vs_fail_totals[4],
                            (unsigned long long)pica_vs_fail_totals[5],(unsigned long long)pica_vs_fail_totals[6],
                            (unsigned long long)pica_vs_fail_totals[7]);
            }
            goto done;
        }
        if (pqt_on && !pvs_par_ok) pvs_ph_lv += pica_pqt_now_ns() - pvs_vs_t0;
        pica_draw_prim_a=v.color[3];
        if(v.cw<pica_draw_log_wmin)pica_draw_log_wmin=v.cw;
        if(v.cw>pica_draw_log_wmax)pica_draw_log_wmax=v.cw;
        if(v.cw<=PICA_W_EPS)pica_draw_log_clip++;
        if(pvs_gpu_trace && pica_draw_count<=5)
            fprintf(stderr,"PICA VS draw=%llu vertex=%u screen=(%.2f,%.2f) uv=(%.2f,%.2f) alpha=%.4f\n",(unsigned long long)pica_draw_count,i,v.x,v.y,v.u,v.v,v.color[3]);
        if(topology==0 || topology==3) {
            vertices[i%3]=v;vok[i%3]=1u;
            if(i%3==2 && vok[0] && vok[1] && vok[2])pica_queue_triangle_clip(vertices[0],vertices[1],vertices[2]);
        } else if(i<2){vertices[i]=v;vok[i]=1u;}
        else {
            if(vok[0] && vok[1])pica_queue_triangle_clip(vertices[0],vertices[1],v);
            if(topology==1){vertices[0]=vertices[1];vok[0]=vok[1];}
            vertices[1]=v;vok[1]=1u;
        }
    }
done:
    if (pvs_par_ok) pvs_par_end();
    if (pqt_on) pvs_ph_t2 = pica_pqt_now_ns();
    if (pf_vt0) pf_shim(PF_SH_VERT, pica_pqt_now_ns() - pf_vt0);
    pica_flush_triangles();
    if (pqt_on) {
        unsigned long long t3 = pica_pqt_now_ns();
        double now = (double)t3 / 1e9;
        ++pvs_ph_n;
        pvs_ph_prep += pvs_ph_t1 - pvs_ph_t0;
        pvs_ph_vert += pvs_ph_t2 - pvs_ph_t1;
        pvs_ph_flush += t3 - pvs_ph_t2;
        if (pvs_ph_last == 0.0) pvs_ph_last = now;
        else if (now - pvs_ph_last >= 2.0) {
            fprintf(stderr, "DRAWPHASE n=%llu prep_ms=%.1f vert_ms=%.1f "
                    "flush_ms=%.1f loadv_ms=%.1f verts=%llu us_per_vert=%.2f "
                    "mean_us=%.0f\n", pvs_ph_n,
                    (double)pvs_ph_prep / 1e6, (double)pvs_ph_vert / 1e6,
                    (double)pvs_ph_flush / 1e6, (double)pvs_ph_lv / 1e6,
                    pvs_ph_nv,
                    pvs_ph_nv ? (double)pvs_ph_lv / (double)pvs_ph_nv / 1e3 : 0.0,
                    (double)(pvs_ph_prep + pvs_ph_vert + pvs_ph_flush) /
                        (double)pvs_ph_n / 1e3);
            pvs_ph_n = pvs_ph_prep = pvs_ph_vert = pvs_ph_flush = 0;
            pvs_ph_lv = pvs_ph_nv = 0;
            fprintf(stderr, "STAGEPHASE calls=%llu hits=%llu miss_dirty=%llu "
                    "miss_new=%llu miss_evict=%llu decode_ms=%.1f cmp_ms=%.1f "
                    "texels=%llu\n", pica_stage_calls, pica_stage_hits,
                    pica_stage_miss_dirty, pica_stage_miss_new,
                    pica_stage_miss_evict, (double)pica_stage_decode_ns / 1e6,
                    (double)pica_stage_cmp_ns / 1e6,
                    pica_stage_texels_decoded);
            pvs_ph_last = now;
            /* t_6f9e3fed: DQ8_VSDEC_HIST=1 -> which opcodes still take the
               generic interpreter, and their share of executed instructions. */
            if (pvs_ghist_on > 0) pvs_ghist_report();
            if (pvs_stats_on > 0) pvs_plan_report();
            {   /* t_5a3c517a: did the parallel transform actually help, and how
                   much of the draw does the MAIN thread now spend waiting? */
                static uint64_t last_jobs, last_mw, last_wt, last_blk, last_wblk, last_inl;
                uint64_t dj = pvs_par_jobs - last_jobs;
                if (dj) {
                    fprintf(stderr, "VSPARSTAT jobs=%llu inline=%llu main_ms=%.2f wait_ms=%.2f "
                            "wait_share=%.2f blk_per_job=%.2f wblk_per_job=%.2f\n",
                            (unsigned long long)dj,
                            (unsigned long long)(pvs_inline_jobs - last_inl),
                            (double)(pvs_par_mainwork_ns - last_mw) / 1e6 / (double)dj,
                            (double)(pvs_par_wait_ns - last_wt) / 1e6 / (double)dj,
                            (double)(pvs_par_wait_ns - last_wt) /
                                (double)((pvs_par_wait_ns - last_wt) +
                                         (pvs_par_mainwork_ns - last_mw) + 1),
                            (double)(pvs_block_count - last_blk) / (double)dj,
                            (double)(pvs_wait_blocks - last_wblk) / (double)dj);
                }
                last_jobs = pvs_par_jobs;
                last_mw = pvs_par_mainwork_ns;
                last_wt = pvs_par_wait_ns;
                last_blk = pvs_block_count;
                last_wblk = pvs_wait_blocks;
                last_inl = pvs_inline_jobs;
            }
        }
    }
    pica_log_draw(0);
    pvs_vc_report();   /* t_6f9e3fed: DQ8_VSCACHE_STATS=1 counters */
    if (pvs_stress) {  /* t_e1773c51: DQ8_VS4_STRESS handshake counters, 2 s */
        static double pvs_stress_last;
        double now = (double)pica_pqt_now_ns() / 1e9;
        if (now - pvs_stress_last >= 2.0) { pvs_stress_last = now; pvs_stress_report(); }
    }
    if (pica_clst() && count >= 256u) {
        static unsigned clst_lines;
        if (clst_lines++ < 600u)
            fprintf(stderr, "CLIPSTAT count=%u fastin=%u straddle=%u dropall=%u "
                    "kept0=%u keptn=%u fan=%u fan_rt=%u fan_off=%u "
                    "wall=[%+.3f,%+.3f] wkeptmin=%+.3f "
                    "fast_bbox=[%+.1f,%+.1f..%+.1f,%+.1f] clip_bbox=[%+.1f,%+.1f..%+.1f,%+.1f] "
                    "rt=%ux%u tris=%u cov=%llu arej=%u drej=%u frw=%u "
                    "tri_off=%u tri_rt=%u vf=%u vground=%u vfrust=%u ndx=[%+.2f,%+.2f] ndy=[%+.2f,%+.2f]\n",
                    count, clst_fastin, clst_straddle, clst_dropall,
                    clst_kept0, clst_keptn, clst_fan, clst_fan_rt, clst_fan_off,
                    clst_wmin_all, clst_wmax_all, clst_wmin_kept,
                    clst_fx0, clst_fy0, clst_fx1, clst_fy1,
                    clst_cx0, clst_cy0, clst_cx1, clst_cy1,
                    pica_rt_w, pica_rt_h, pica_draw_log_tris,
                    pica_cov_px, pica_draw_log_arej, pica_draw_log_drej,
                    pica_draw_log_frw, clst_tri_nodesign, clst_tri_onscreen,
                    clst_nv_front, clst_nv_ground, clst_nv_infrustum,
                    clst_ndx0, clst_ndx1, clst_ndy0, clst_ndy1);
    }
pqt_done:
    if (pvs_f) {
        int dead = pica_draw_log_count > 0 &&
                   pica_draw_log_clip >= pica_draw_log_count;
        fprintf(pvs_f, "VSEND tris=%u clip=%u wmin=%+.9g wmax=%+.9g "
                       "sxhist=%u,%u,%u,%u,%u,%u\n",
                pica_draw_log_tris, pica_draw_log_clip,
                pica_draw_log_wmin, pica_draw_log_wmax,
                pica_thist[0], pica_thist[1], pica_thist[2],
                pica_thist[3], pica_thist[4], pica_thist[5]);
        if (pvs_dead_only) {
            int keep = dead || pvs_live_saved < pvs_max_live;
            if (!dead && keep) ++pvs_live_saved;
            if (keep && pvs_draws < pvs_max_draws) {
                FILE *f = fopen(pvs_path, pvs_file_made ? "a" : "w");
                if (f) {
                    char buf[8192];
                    size_t n;
                    fflush(pvs_f);
                    rewind(pvs_f);
                    while ((n = fread(buf, 1, sizeof buf, pvs_f)) > 0)
                        fwrite(buf, 1, n, f);
                    fclose(f);
                    pvs_file_made = 1;
                    ++pvs_draws;
                }
            }
            fclose(pvs_f); pvs_f = NULL;
        } else {
            fclose(pvs_f); pvs_f = NULL;
        }
        pvs_trace = 0;
    }
    if (pqt_on) pqt_account(pqt_t0, pqt_gl0);
}
