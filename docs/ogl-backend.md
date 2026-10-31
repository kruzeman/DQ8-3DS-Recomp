# OpenGL rasterization backend (experimental)

`src/pica_gl.c` is an experimental OpenGL backend for PICA200 draw commands.
It is the first hardware-draw slice: guest-derived geometry is rasterized by
the GPU instead of the CPU rasterizer. The software rasterizer remains the
default and is completely unchanged when the backend is off.

## Status

Measured on the reference run (DQ8 static recompilation of the USA ROM,
`DQ8.code` + extracted RomFS, run 2026-10-22 on this commit's tree,
~30 s wall per game run on the build server):

- `DQ8_GL=1`: 306 of the guest's PICA draw triggers rasterized on the GL
  backend in every run (the untextured solid-color draws; textured draws use
  the game's multi-stage TEV chain and correctly fell back to software).
- All 420 captured display-transfer frames are **byte-identical** between the
  `DQ8_GL=1` run and the `DQ8_GL=0` software run (sha256 over every frame),
  including the guest-rendered SQUARE ENIX logo frame (white glyphs + red
  marks). The GL backend does not change output. (A second run pair matched
  418/418 common frames identically; the capture count varies by ±2 frames
  between runs of the same mode because the guest exits on a timer.)
- `DQ8_GL=0`: zero GL involvement in the log, all draws on the unchanged
  software path.

Implemented and verified:

- GL 3.3 core rasterization of decoded PICA triangles (screen-space, exactly
  the vertices the software rasterizer consumes).
- Per-vertex color interpolation (perspective-correct), texture0 staging
  (CPU-side format/tile decode staged to a GL texture, GPU sampling).
- **Full TEV combiner translation to GLSL** (2026-10-25): the fragment
  shader evaluates the complete 6-stage `pica_tev` chain — every combiner
  op (replace/mul/add/addsub/lerp/sub/dot3/mad/lerp-min), all source and
  modifier selects, per-stage constants, buffer updates and shifts — with
  the same float expression order as the software path.  Verified
  byte-identical (strict, 0-LSB) against the software rasterizer over
  randomized register configurations (`tools/run_renderer_tests.sh`).
- **Blend/alpha-test/color-mask in the shader**: the fragment shader
  reproduces `pica_write_fragment` exactly (all blend factors 0..15 incl.
  dual-source code 14, all equations incl. min/max, the
  `floor(sat(v)*255+0.5)` quantization).  Fixed-function GL blending is not
  used (it blends quantized values and can differ at the 1-LSB level).
- **Batched render-target readback** (2026-10-25): the FBO holds the render
  target across draws; guest-side writes are detected against a raw shadow
  copy (8x8 Morton-block diff + uniform-pattern fast path for GSP fills),
  and the write-back to guest memory is deferred until a consumer needs it
  (GSP display transfer, memory-fill/DMA, capture, scan-out, software draws).
  `DQ8_GL_BATCH=0` restores the pre-batching per-draw upload/readback.
- Src/dst blend factors with all PICA equations, alpha test (PICA
  quantized semantics), color mask.
- Context handling: EGL surfaceless (headless-safe) or a caller-provided GL
  context; runtime-loaded GL entry points (no GL link dependency).

Explicitly NOT yet translated (draws that need these fall back to the
software rasterizer automatically):

- Depth/stencil buffers and depth testing (draws with depth test/write stay
  on software — gated, since the depth buffer work).
- Texture formats/wrap semantics beyond what the staged texture reproduces:
  wrap modes other than clamp-to-edge are only accepted when every vertex
  u,v stays inside [0,1]; GPU-side texture decode is not done.
- Vertex-shader translation: the uploaded PICA VS still runs on the CPU.

## Build

`tools/run_static_recomp.sh` compiles the backend into the generated runner
when EGL development files are present (`pkg-config egl`, header
`/usr/include/EGL/egl.h` — Debian/Ubuntu: `libegl1-mesa-dev`). Nothing else
changes: the same binary defaults to the software rasterizer.

## Run

Runtime switch (independent, per run):

```sh
DQ8_GL=1 ./tools/run_static_recomp.sh          # OpenGL backend for supported draws
./tools/run_static_recomp.sh                   # software rasterizer (default)
```

`DQ8_GL=1` without `DQ8_ENABLE_GL` at build time prints a warning and keeps
the software path. The backend logs its GL vendor/renderer/version at startup
and the first 8 GL draws (`GL draw #N tris=... mode=... rt=...`).

## Reproducible test (no game data needed)

```sh
tools/run_gl_test.sh                 # headless EGL test (works over SSH)
tools/run_gl_test.sh --window 30     # SDL2 window on a desktop PC
```

The test drives the exact `pica_gl_draw()` entry point the guest pipeline
uses: a solid quad, a textured quad, an alpha-blended quad and an alpha-test
quad, all verified pixel-by-pixel against expected values, and writes
`target/gl_test_frame.ppm`. Exit code 0 means all checks passed.

At startup it prints:

```
GL backend: vendor=... renderer=... version=...
```

On this build server the renderer is `llvmpipe` (Mesa software rasterizer —
the server has no GPU, so this only proves the GL path executes correctly).
On a Linux PC with a GPU the same test prints e.g. `NVIDIA GeForce ...`,
`AMD Radeon ...` or `Intel ...` and the draws run in hardware. Run
`tools/run_gl_test.sh --window 30` on your PC to see the rendered frame on
screen.

Dependencies: OpenGL 3.3+ drivers, `libegl1-mesa-dev` (or any EGL dev
package), `libsdl2-dev` for `--window` mode.

## Design notes

- **Renderer-free presentation (fixed 2026-10-22, follow-up).** Even a
  `SDL_RENDERER_SOFTWARE` request made SDL initialise Mesa GL and set
  `SDL_WINDOW_OPENGL` on the window (0x4 → 0x6, exactly what the operator's
  log showed).  Presentation now uses `SDL_GetWindowSurface` +
  `SDL_BlitScaled` + `SDL_UpdateWindowSurface` with **no
  `SDL_CreateRenderer` at all** (a renderer remains only as a fallback when
  the driver has no window framebuffer).  Probe results (SDL 2.30):
  `SDL_VIDEODRIVER=dummy` → 0 GL libraries loaded, window flags unchanged;
  x11 and offscreen drivers still *dlopen* Mesa (GLX/EGL) inside
  `SDL_GetWindowSurface` itself, and the offscreen driver creates its own EGL
  context and makes it current — so driver GL activity persists even without
  a renderer, and the context isolation below is what actually guarantees
  nothing can touch the backend's context (the run log shows
  `foreign context ... was current before draw #N; saving/restoring it`).  With the surface
  path + isolation, a windowed `DQ8_GL=1` run shows: no renderer line,
  stale GL errors 0, non-black readbacks, and byte-identical PPM frames
  against the `DQ8_GL=0` run during stable phases (only ±2 s sample-time
  shifts differ at fade boundaries).
- **Context isolation (fixed 2026-10-22).** SDL on an OPENGL-capable video
  driver (x11, wayland, offscreen — all set `SDL_WINDOW_OPENGL` and load Mesa
  when a renderer is created, even a software one; the offscreen driver also
  creates its own EGL context) issues GL calls on the render thread against
  whichever context happens to be current.  With our context left current
  between draws those calls hit *our* objects: observed as
  `GL_INVALID_OPERATION/GL_INVALID_VALUE` accumulating before every draw
  (`stale@entry`), our VAO/VBO/FBO/textures vanishing, and a Mesa/llvmpipe
  SIGSEGV inside `glTexImage2D`; on a GPU the same contamination wipes the
  render target, so the readback returns black and `rt_put` paints the GSP
  framebuffer black (black window with a working SDL present).  The backend
  now saves the previously current EGL context, binds ours only while its own
  GL code runs, and restores the previous context afterwards — foreign GL
  then runs on its own context (or none) and can never touch ours.  Verified
  on llvmpipe: stale errors 0, 306 hardware draws, readbacks non-black,
  guest frames match the software run.  As a second line of defence
  `pgl_ensure_objects()` re-creates any object that disappears anyway.
- **Diagnostics (`DQ8_GL_DEBUG=1`).** Besides the per-stage `glGetError`
  attribution: the actual SDL renderer name + window flags (an appearing
  `SDL_WINDOW_OPENGL` bit means SDL initialised GL machinery), a
  `foreign context ... was current before draw #N; saving/restoring it` line
  for the first 8 draws, `readback #N nonzero=<nz>/<total>` byte counts
  (an `ALL-BLACK` readback with guest content present is the black-screen
  failure mode), `rebuilt vanished GL objects mask=...`, and shutdown
  totals `(... displacements=N, object rebuilds=N)`.
- Known server-side limitation: under X11 (SDL loads GLX/DRI3) the
  surfaceless EGL context creation can fail with `0x3002 EGL_BAD_MATCH`, so
  the backend stays off and the software rasterizer runs; headless and
  Wayland hosts are unaffected.
- The guest attribute decode and uploaded PICA vertex program still run on
  the CPU (unchanged); the GL backend receives the resulting screen-space
  triangles, so no vertex-shader translation is required for this slice.
- **Batched readback design (2026-10-25).** The FBO holds the render target
  across draws; a raw-byte shadow of the guest region drives cheap
  guest-write detection (single memcmp fast path, per-8x8-Morton-block diff
  with sub-rectangle uploads, and a uniform-pattern path: a whole-region
  repeating 2/3/4-byte pattern — what `gsp_memory_fill` writes — resolves to
  at most one glClear).  The write-back to guest memory happens only at
  consumer points: `pica_gl_flush_guest_rt()` is called before GSP display
  transfers, DMA, host capture, scan-out/PPM capture (src/main.rs, one-line
  hooks) and before software-path draws and texture staging that may alias
  the target (src/pica_vs.c).  Per pixel, a write-back is skipped when the
  guest bytes changed since the FBO was last synchronized (a later
  guest-side write wins).  Residual assumption (documented): guest CPU code
  does not READ the render-target region between a GL draw and the next
  flush point; `DQ8_GL_BATCH=0` restores per-draw readback for differential
  testing.
- Draw-state expressibility is decided per draw call: when the state is not
  expressible (depth test/write active, texture wrap semantics outside the
  staged-texture contract), the whole draw runs on the software rasterizer,
  so output stays correct at all times.

## Next steps

1. ~~TEV combiner translation to GLSL~~ — **done** (full chain, verified
   byte-identical against `pica_tev`).
2. Vertex-shader translation: run the uploaded PICA VS as generated GLSL
   instead of the CPU interpreter.
3. ~~Persistent FBO per render target + dirty tracking~~ — **done**
   (batched readback; see design notes).  Remaining: GPU-side texture
   detiling (direct texture upload from guest memory).
4. Depth/stencil buffers and depth-test state (then the depth gate in
   src/pica_vs.c can be lifted).
5. Frame-dump comparison harness (GL vs software): **done** for the startup
   window (`tools/run_sw_gl_diff.sh` + `tools/analyze_sw_gl_diff.py`); the
   frozen idle-dialog window is covered by `tools/run_gl_matrix.py` cells.
