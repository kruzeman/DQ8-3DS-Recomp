# Perf diagnosis: main menu slows down with semi-transparent windows

Task t_399eab58 — diagnosis with numbers (software + GL), hot functions, fix plan.
Binary: `target/dq8-menu-perf-gl` (built from HEAD e3553fc + instrumentation, `DQ8_GL=1`,
one binary serves both modes). Method: new env-gated `DQ8_TRACE_TIME` (commits 7cd1f3b,
bd42c79) for per-draw wall time / slope; callgrind for function attribution; DQ8_GL phase
counters for the GL round-trip.

## 1. Measured slope (software mode — DQ8_TRACE_TIME, native)

Classifies every draw as **sw-fast** (opaque, put-only fast path) vs **sw-RMW**
(translucent: `get_pixel + per-channel blend + put_pixel` per fragment — the true
"semi-transparent" signal, `pqt_slow`), with a 4-bucket bbox-area histogram.

| scene | translucent RMW cost | share of wall |
|---|---|---|
| save-data dialog (1 window) | ~1.20–1.35 s CPU / 2 s window | **60–67 %** |
| main menu (dimmed title + full-screen scrim) | ~1.0–1.3 s / 2 s | **50–65 %** |
| naming screen (3 stacked windows) | ~1.2–1.35 s / 2 s | **60–67 %** |

**Per-pixel RMW ≈ 0.4–0.55 µs/px.** Cost is linear in blended area (the bucket table
shows µs rising with bbox area bucket):

- full-screen (512×400 = 204 800 px) translucent window ≈ **61–86 ms per redraw**
- 25–50 %-screen window ≈ **30–40 ms per redraw**
- small glyph/text quads (≈6 200 px) ≈ **1.7–3 ms each**

**Slope = cost per translucent window per frame ≈ (window area) × ~0.45 µs/px.**
Each additional stacked window adds one more full-area RMW pass per redraw → CPU grows
*linearly with window count × area*. That is exactly the reported symptom: more
semi-transparent windows → more lag, independent of mode (the RMW path is shared CPU).

## 2. Named hot functions (callgrind, Ir %)

Instrumentation off during boot, on for a bounded window. The scene-matched window
(`menu-prof-dialog3`, PRE=140 s) **did land on the save-data dialog** (confirmed frame: the
"No previous save data could be read…" translucent window over black). The blend share varies
by window content — solid translucent backgrounds are blend-dominated, text/glyph windows are
TEV-dominated — and both scale with window count:

```
menu-prof-dialog3  (save-data dialog, TEXT-heavy, scene-matched):
  43.50%  pica_tev                <- 6-stage TEV combiner (font-glyph texture modulate)
  17.05%  host_display_capture_transfer  <- per-frame present (NOT window-scaled)
   9.53%  pica_write_fragment     <- per-pixel blend + depth + write       (BLEND CORE)
   5.90%  pica_raster_triangle
   4.51%  pica_get_pixel          <- Morton read (RMW only)                (BLEND)
   3.24%  pica_sample_tex0        <- font-atlas texture sample

menu-prof-dialog  (solid translucent, blend-heavy window):
  32.66%  pica_write_fragment     (BLEND CORE)
  18.15%  host_display_capture_transfer
  12.44%  pica_raster_triangle
   7.77%  pica_tev
   7.72%  pica_get_pixel          (BLEND)
   3.94%  pica_put_pixel          (BLEND)
```

- **Alpha-blend core (`pica_write_fragment` + `pica_get_pixel` + `pica_put_pixel`) = 14–44 % of
  all instructions** — a top-3 group in every window; **highest (~44 %) on solid translucent
  windows/scrims** (the "semi-transparent box" case), ~14 % on text-heavy windows where
  `pica_tev` (glyph texture modulate) rises to ~43 %.
- The two co-scale with window count: **blend ∝ window area**, **TEV ∝ glyph count** — so every
  added window pushes both. The alpha-blend hypothesis is **confirmed as a major area-scaling
  component**, operating alongside per-fragment TEV/texture on text windows.
- `host_display_capture_transfer` (17–30 %) is a **per-frame** present cost, does **not** scale
  with window count (partly the `DQ8_CAPTURE_PPM=1` write, `gsp_write_screen_ppms` 2.7 %) — a
  fixed intercept, not the reported cause.

Why `pica_write_fragment` leads on solid windows: each translucent fragment runs depth-test →
`pica_get_pixel` (Morton read) → 8× `pica_blend_factor` switches → `pica_put_pixel` (Morton
write). Paid once per blended pixel → the area×layers slope.

## 3. GL mode — per-draw full-frame round-trip (task suspects #2 and #3)

`DQ8_TRACE_TIME` GL phase counters (`pica_gl.c`) on the GL path (llvmpipe, 512×400):

```
gl-phase n=53  read_ms=174.1  upload_ms=8.6  draw_ms=14.2  readback_ms=37.2  writeto_ms=147.9
```

Per GL draw ≈ 7.2 ms total, split:
- **host `rt_get` full-frame read = 3.28 ms/draw** (per-pixel Morton read of the WHOLE RT)
- **host `rt_put` full-frame write-back = 2.79 ms/draw** (per-pixel Morton write of the WHOLE RT)
- actual GPU draw = 0.27 ms/draw; upload 0.16 ms; `glReadPixels` 0.70 ms

**84 % of every GL draw's cost is the CPU-side full-frame render-target read + write-back
round-trip, and only 3.7 % is the real GPU draw.** `pica_gl_draw_locked` unconditionally
loops `rt_get` over all rt_w×rt_h pixels before the draw and `rt_put` over all pixels after,
regardless of the draw's bounding box, and does a full `glTexImage2D`/`glReadPixels` per draw.
Each draw therefore pays a fixed full-frame cost → N windows = N full-frame round-trips.
GL mode also proved **intermittently unstable** (llvmpipe context contamination → SIGSEGV
after ~50 draws in one run; a 20 s smoke survived), consistent with the documented issue.

## 4. Fix plan (ordered by risk / payoff — implement behind gates)

1. **GL: cache the RT across draws within a frame (kills 84 % of GL per-draw cost).**
   Keep the render target resident in the FBO; upload once at the first draw / on guest-RT
   change and read back only at display-transfer / present boundaries (or dirty-rect), not
   per draw. Clears suspects #2 (readback round-trip) and #3 (full-frame rewrite). GL-only.
2. **Software: specialized blend fast path for the common src-alpha/one-minus-src-alpha
   config** — replace the 8 switch-based `pica_blend_factor` calls per pixel with the direct
   `s = src*srcA; d = dest*(1-srcA)` formula when cfg matches (exact-match, provable).
   The opaque fast path already special-cases cfg==0x76760000; the translucent equivalent
   is missing.
3. **Software: hoist `guest_memory_range` / Morton base out of the per-pixel path** — resolve
   the RT base + row/tile bases once per draw instead of per fragment (`pica_get_pixel`,
   `pica_put_pixel`, `pica_depth_test_write` each re-validate per pixel).
4. **Skip depth-test/write for pure UI overlay quads** where the guest disabled depth writes
   (translucent windows typically draw with depth-test on but depth-write off) — verify
   against the naming-screen layering gate before landing.
5. **Dirty-rect present** — bound `host_display_capture_transfer` to changed regions (this is
   a separate per-frame cost, not the window-scaled one).

## 5. Evidence / artifacts
- `target/title-evidence/menu-walk-sw/run.log` — software slope on main-menu walk (TIME lines).
- `target/title-evidence/menu-perf-sw/run.log` — save-data dialog steady state.
- `target/title-evidence/menu-walk-gl/run.log` — GL phase split.
- `target/title-evidence/menu-prof-dialog/callgrind.out.1` — function attribution.
- Instrumentation commits: `7cd1f3b` (initial), `bd42c79` (RMW classification). Diagnosis-only,
  env-gated (`DQ8_TRACE_TIME=1`), default off (one branch/draw when off).
