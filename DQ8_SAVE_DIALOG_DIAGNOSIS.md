# DQ8 3DS — save dialog text missing: diagnosis

Date: 2026-10-20, task t_593bdaee. Time spent: ~30 min (21:48–22:18 IDT).
Scope: diagnosis only. No game/renderer code was changed.

## 1. Reproduction command and observed behaviour

```sh
cd /home/hermes/DQ8
DQ8_HEADLESS=1 DQ8_DISPLAY=1 DQ8_RUN_GUEST_THREADS=1 \
  DQ8_TRACE_GPU=1 DQ8_TRACE_TEXCFG=1 DQ8_TRACE_FORMAT_ARGS=1 DQ8_TRACE_STRING_OBJECT=1 \
  DQ8_SCAN_MSG="No previous save data" \
  timeout 110 ./target/dq8-startup-headless DQ8.code target/dq8.romfs > /tmp/t1.log 2>&1
```

(`./tools/run_static_recomp.sh` is the SDL variant of the same runner; the headless
binary is the clang build of `target/dq8-startup.c`, which is generated from
`src/main.rs`. Current tree: `src/main.rs` 17:52 == `target/dq8-startup.c` 17:52,
`target/dq8-startup-headless` 17:55 — build was already up to date.)

Observed: the Square Enix logo plays, a dialog box frame is rasterised on the top
screen, then the guest stops submitting draws (2799 draw triggers, all before the
first 45 s) and waits. The box interior is empty.

Frame evidence (`/tmp/dq8-screen.ppm`, `tools/analyze_ppm.py`, `scratch/box_ink.py`):

```
dims 400 240 nonblack 2955 / 96000
white px 577 bbox x 0 397 y 146 237
box=(0, 146, 399, 239) interior_ink=126 bright=3
```

126 interior pixels / 3 bright pixels = the window frame only, no glyphs.

## 2. Relevant files and functions

| Where | What |
| --- | --- |
| `src/main.rs:6720` `guest_host_snprintf()` | host shim for guest `0x00143974`; formats names/paths for the guest |
| `src/main.rs:4953` `branch_indirect()` | null indirect target is silently turned into "return to LR" (log line at 4968) |
| `src/pica_vs.c:319` `pica_draw_arrays()` | draw trigger handling + `DRAWDBG`/`DQ8_TRACE_DRAW_FROM` instrumentation |
| guest `0x00224C84…0x00224CD8` | dialog text-widget loop: `snprintf(name, 0x1E, 0x1D, "TextBox_%02d_%02d", col, row)` → `find_child(0x001BC5F0)` → virtual call at `vtable+0x58` |
| guest `0x00224CE0…` | same loop shape with `"SelectText_%02d"` (selection cursor) |
| guest `0x001BC5F0` | name-based child lookup: calls `vtable+0x2C` (name compare) then walks the child list |

## 3. Pipeline stages: what works and what fails

* A — game requests the message: **PASS**. `RAMSCAN ascii addr=086062DC tail=No
  previous save data could be read..New sa…` (also 0x085AF5AF, 0x08606340,
  0x092FFF7C) in `/tmp/diag4.log` and in the current run `/tmp/t1.log`; the
  guest also performs the format/lookup passes (`format result len=…`,
  `string lookup … blyt/timg/anim` lines in `/tmp/t2.log`).
* B — message resource loaded/decoded: **PASS** (string materialised in RAM).
  Font asset also present: `rom/Font/FontDefault.bffnt` (140 612 B, valid FFNT
  header, FINF/TGLP sections) in `target/dq8.romfs`.
* C — text layout / widget resolution: **FAIL — this is the earliest failing
  stage.** The 5×5 `TextBox_%02d_%02d` widget lookup returns NULL for every
  element, so no text object ever exists and no glyph quads are generated.
* D — glyph submission: never reached. 0 draws were skipped (`grep -c 'draw
  skipped' = 0`), 0 vertex-shader failures, and no draw in the whole run binds a
  font-sized texture (`TEXCFG` list contains only 8×8 / 16×16 UI sprites and two
  32×256 / 32×64 surfaces; no L8/A8/LA8 glyph sheet).
* E — visibility: untested, and not the problem: everything the guest *does*
  submit is visible (dialog frame, sprites, logo).

## 4. Root cause (confirmed)

`guest_host_snprintf()` (`src/main.rs:6732-6756`) parses the format spec —
flags, width, precision — and then **throws the flags/width away**:

```c
while (*f >= '0' && *f <= '9') ++f;          /* width consumed … */
...
} else if (conv == 'd' || conv == 'i') {
    n = snprintf(piece, sizeof(piece), "%d", (int32_t)arg_va);   /* … but not used */
```

The dialog loop formats child names with `"TextBox_%02d_%02d"` (format string
bytes verified at guest `0x00224D50`: `54 65 78 74 42 6f 78 5f 25 30 32 64 5f
25 30 32 64`), so the host shim emits `TextBox_0_0` … `TextBox_4_4`.

The layout actually names the widgets zero-padded (extracted from the RomFS
binary):

```
TextBox_00_00 TextBox_00_01 … TextBox_05_03
```

Consequence chain, all observed in evidence:

1. `find_child(0x001BC5F0, "TextBox_0_0")` finds nothing → returns NULL.
2. Caller does `r1 = *r0` (reads RAM[0] = 0), `r3 = *(r1+0x58)` = RAM[0x58] = 0,
   `blx r3` → null indirect branch at `pc=0x00224CC0`.
3. The recompiler logs `guest null indirect branch` and returns to LR
   (`src/main.rs:4968`), so each of the 25 calls is silently skipped.

Evidence: **92** `guest null indirect branch: pc=0x00224CC0 r0=00000000
r4=0859BAC4 r5=0,1,2,3…` lines in both `/tmp/t1.log` and `/tmp/t2.log` — r5 is
the loop index, exactly matching the `for r5 in 0..4 / for r6 in 0..4` loop.
No other null branch site occurs.

The same defect explains the missing `SelectText_%02d` selection cursor.

## 5. Evidence index

* `/tmp/t1.log` — TEXCFG/GPU run: 2799 draws, 92 null branches, RAMSCAN hits.
* `/tmp/t2.log` — FS/format/lookup run: `format result`, `string lookup`,
  `lookup frame` traces; RAMSCAN hits.
* `/tmp/diag4.log` — earlier RAMSCAN confirmation of the save message in RAM.
* `/tmp/dq8-screen.ppm` (+ `.png`) — final frame: dialog border, empty interior.
* Guest disassembly of `0x00224C40…0x00224D00` and `0x001BC5F0…0x001BC64C`
  (`dq8tool disasm`), format-string bytes at `0x00224D50` / `0x00224D68`.
* RomFS widget-name scan: `grep -a -o 'TextBox_[0-9]*_[0-9]*' target/dq8.romfs`.

## 6. Proposed narrowly scoped fix (not applied)

Make the integer conversions honour the parsed spec instead of hard-coding `%d`:
keep the original spec substring (flags + width + precision) while scanning, then

```c
char spec[32];
snprintf(spec, sizeof spec, "%%%s%c", flags_width, conv);  /* e.g. "%02d" */
n = snprintf(piece, sizeof piece, spec, (int32_t)arg_va);
```

Only `guest_host_snprintf()` needs to change; no rendering, layout or guest-code
changes.

## 7. Next experiment

1. Apply the shim fix, `cargo run --release -- recompile-arm …` + clang build
   (or `./tools/run_static_recomp.sh`), rerun headless with
   `DQ8_SCAN_MSG="TextBox_00_00"`, `DQ8_TRACE_GPU=1`, `DQ8_TRACE_DRAW_FROM=<dialog draws>`.
2. Expected: `guest null indirect branch pc=0x00224CC0` count drops from 92 to 0,
   new draws appear that bind a glyph-sheet texture, and the captured frame
   shows ink inside the box (`box_ink.py` interior_ink >> 126).
3. If the null branches disappear but glyphs still do not appear, the next
   candidate is the font-sheet texture format (BFFNT TGLP sheet) not being bound
   or not decoded by `pica_sample_tex0()` — that was NOT investigated here.
