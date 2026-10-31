# Guest-rendered startup logo

Work is on `codex/fix-gsp-overlay-stall`; the pre-existing generated runner in
`target/` is not replaced by the validation builds.

## Fixed

- GetProcessInfo SVC 0x2B returns its 64-bit value in r1/r2. The game's saved
  output-pointer wrapper then stores those registers. Returning it directly
  through memory was overwritten by the wrapper, corrupting the physical-memory
  delta (type 20). With the proper ABI, the first vertex buffer changes from
  invalid PAddr 0x14B8A9B0 to 0x20B929B0 and contains real guest attributes.
- Float32 uniform uploads reverse their four components; packed f24 uploads and
  fixed attributes use the 3-word/4-component packing. The f24 decoder preserves
  sign, infinities, NaNs and subnormals.
- `src/pica_vs.c` executes the uploaded PICA program, swizzles, temporary and
  address registers, comparisons, conditional branches and calls. Attribute
  loaders honor component formats, alignment, integer values, defaults,
  register maps and indices. Output registers map through the configured output
  mask and semantic map. Unsupported instructions fail explicitly and execution
  has a finite budget.
- Rasterization interpolates the shader's UVs and color, evaluates six TEV
  stages and alpha blending, and writes the guest's tiled color buffer. Shared
  triangle edges have a single owner. Disabled textures do not trigger sampling.
  The DMA unit-quad fallback, invented UVs and UV-derived colors are removed.
- Texture sampling uses the PICA bottom-left origin. Host portrait rotation
  uses the source width, and host RGB/RGBA presentation decodes GPU BGR/ABGR.

## Evidence

The native runner is generated from the user's decrypted ARM code and uses the
user's extracted RomFS. No emulator process, decoded PNG, layout overlay or
resource-name lookup supplies its logo pixels. The guest's fourth and fifth
draws bind the two RGB565 logo textures and execute its uploaded VS. The outputs
are the 256x32 and 64x32 logo rectangles with a starting alpha of 1/255.

Headless captures contain a correctly assembled SQUARE ENIX logo, including
its red marks and final X/registered symbol. Sequential captures show fade-in,
a fully visible interval and fade-out. For example:

- `/tmp/dq8-real-vs-frames/frame-137.ppm`: native portrait 240x400 capture;
- `/tmp/dq8-sqex-guest-first.png`: same capture rotated for normal display;
- `/tmp/dq8-real-vs-next/`: longer run through fade-out.

The longer run reached 614 draws and 370 display transfers before escaping to
untranslated ARM 0x003AEE54 from virtual destructor call 0x003ABEC4. Disassembly
confirms a real destructor, now added to `recomp-roots.txt`. This is the next
startup transition; it is not evidence of a rendered title screen.

Validation: 32 Rust tests, including a native GetProcessInfo ARM-wrapper test;
two native C runtime tests under ASan/UBSan for f24 packing, shader math/masks,
control flow/bounds and shared-edge alpha blending. Leak detection alone is
disabled in the test child because this sandbox cannot support LeakSanitizer's
ptrace requirements.

## Remaining limits

This is an initial UI GPU implementation. LOOP/geometry shaders, clipping,
depth/stencil/scissor, lighting, ETC textures, full sampler modes and all logic
operations are not implemented. Those require explicit work as later guest
screens exercise them. The existing framebuffer-size fallback also needs review.
The title screen, movies, gameplay and audio are not validated by this result.
