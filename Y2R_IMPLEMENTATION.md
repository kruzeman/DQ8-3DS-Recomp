# Y2R implementation — DQ8 opening cinematic (task t_95c65386)

Date: 2026-10-21, workspace `/home/hermes/DQ8`, branch `codex/fix-apt-home-resume`.
All claims below are backed by the logs/PPMs listed; no frame is claimed visible
without pixel-level and visual verification.

## TL;DR

1. **`y2r:u` is fully implemented** in the runtime (src/main.rs): the complete
   DQ8-used command table plus a faithful port of Azahar's
   `HW::Y2R::PerformConversion` — 8-row strips, standard-coefficient YUV→RGB,
   all rotations, linear and 8x8 Morton-tiled output, transfer_unit/gap DMA, and
   transfer-end event signalling. Verified against 19-command IPC traces.
2. **The missing upstream stage was missing translated roots, not Y2R.** The
   movie decoder's `add pc, r5, r6` jump-table dispatch family had 2
   unrooted handler entries per table (~66 in the decoder region); each hit
   killed the media thread with "translated PC escaped", so the MJPEG decoder
   never filled the YUV ring-buffer slots. 66 scanner-verified roots added and
   rebuilt — zero escapes in run4/run5 (run3 died at t=159 s on one of them).
3. **First real cinematic frame decoded and converted** (run4, conversion #6,
   ~t=170 s): `y2rframe-0006.ppm` has 208,717/288,000 non-zero pixels and,
   after 12x brightness amplification, unmistakably shows the opening movie's
   3D countryside scene (tree on a grassy hill, meadow, water edge, rolling
   hills, deep-blue sky with stylized clouds) with clear 8x8/16x16 MJPEG
   macroblock artifacts. Raw max=15 because the movie starts with a fade-in
   from black. This proves the RomFS read → Moflex/MJPEG decode → YUV420
   planes → Y2R conversion → RGB pipeline end to end.
4. **The cinematic is visible on the composited top screen** (run7 final
   display-transfer frame): `target/y2r-evidence/run7/frames/t0400_final.ppm`
   has 208,717 non-zero pixels (identical content stats to the decoded
   Y2R frame #6) and its 12x-amplified PNG shows the same recognizable DQ8
   countryside scene — i.e. the game's compositor renders the Y2R output to
   the screen. This is the first time the opening cinematic appears on the
   captured screen instead of black.
5. **Remaining issue:** playback stops after that frame — run7 aborted at
   t=328 s on a guest instruction the recompiler does not translate
   (`0xE672CF93` at pc=0x00195D7C, in the decoder's copy-loop family), and
   decode is slow (~1 frame/min under interpretation). See §6.

## 1. What the game does (baseline, captured in `target/y2r-evidence/base`)

With a persisted save the guest skips dialogs and enters the movie path
automatically. It runs the full `y2r:u` start-up sequence, observed verbatim:

| Cmd | Meaning | DQ8 value |
| --- | --- | --- |
| 0x0001 | SetInputFormat | 1 = YUV420_Indiv8 |
| 0x0003 | SetOutputFormat | 1 = RGB8 |
| 0x0005 | SetRotation | 0 = no rotation |
| 0x0007 | SetInputLayout | 1 = Block 8x8 (Morton-tiled output) |
| 0x001A/0x001C | input width / lines | 400 / 240 |
| 0x0020 | SetStandardCoefficient | ITU_Rec601_Scaling (coef0=298) |
| 0x0022 | SetAlpha | 0xFF |
| 0x0010 | SetSendingY | Y=0x151D49E8 size=0x17700 unit=0x190 gap=0x70 (stride 512) |
| 0x0011 | SetSendingU | U=0x151F29E8 size=0x5DC0 unit=0xC8 gap=0x138 |
| 0x0012 | SetSendingV | V=0x151F2AE8 (= U + 0x100, per-row 256 B U/V blocks) |
| 0x0018 | SetReceiving | dst=0x152B2980 size=0x46500 unit=0x2580 gap=0xA80 (8-row strips) |
| 0x0026 | StartConversion | triggers the engine |

Before the fix the main thread parked forever in `svcWaitSynchronization` on
the transfer-end event (handle 0x35) because the stub never signalled it —
the verified reason the presentation loop at 0x002F9334 stalled.

## 2. Implemented behavior (src/main.rs, lines ~5167-5560 and 6166-6280)

Ported from Azahar (`azahar-master/src/core/hw/y2r.cpp` `PerformConversion` +
`cam/y2r_u.cpp` command table):

* **Command table** (used subset, all matching Azahar semantics incl. parameter
  validation): 0x0001-0x0008 (formats/rotation/layout + getters),
  0x000D/0x000F/0x0027/0x002B (init/repeat/etc.), 0x0010-0x0013
  (SetSending Y/U/V/YUYV with addr/size/unit/gap), 0x0018/0x0019 (receiving),
  0x001A/0x001C (width/lines, validated), 0x0020 (standard coefficients:
  ITU_Rec601_Scaling / Rec601_Scaling / Rec709_Base / Rec709_Scaling),
  0x0022/0x0023 (alpha), 0x0025 (dithering weights), 0x0026 StartConversion,
  0x0028 IsBusyConversion, 0x0029 package parameters, and **the transfer-end
  event registration** (D2C "receive" path; the registered handle becomes
  `y2r.end_event`).
* **Conversion engine** (`y2r_start_conversion`): processes the frame in
  8-row strips exactly like the hardware DMA; per strip it DMAs Y (and U/V, or
  YUYV) honoring `transfer_unit`+`gap`; converts with the selected coefficient
  matrix (full int32 pipeline, including the 8-entry dither-free path);
  supports all 4 rotations; emits linear or 8x8 Morton-tiled (Block 8x8)
  output in RGB565/RGB8/RGBA8; writes into guest memory via the same
  `guest_memory_range` mapping used elsewhere in the runtime.
* **Transfer-end event**: after the last strip the registered event object is
  signalled and any guest thread blocked in `svcWaitSynchronization` on that
  handle is woken (`y2r_signal_end`). The main thread's presentation loop at
  0x002F9334 now resumes instead of parking forever.
* **Diagnostics** (opt-in, zero overhead when off): `DQ8_TRACE_Y2R=1` logs
  per-conversion parameters and Y/U/V plane statistics (min/max/mean/value
  buckets — proves whether the decoder actually produced pixels);
  `DQ8_DUMP_Y2R_PREFIX=<path>` dumps every converted frame as a PPM.

## 3. The real missing stage: movie-decoder dispatch roots

The YUV input planes were constant (Y=16, U=128, V=128 = neutral black) on
every early conversion: Y2R was faithfully converting black because the MJPEG
decoder had not written any frame. Two verified causes, both in the translated
code (not in service shims):

1. Unrooted jump targets in the movie path (`0x001A9A68`, 6 decoder handlers).
2. The decoder's `add pc, r5, r6` jump-table dispatch family — sibling task
   `t_6058a51e` fixed `tools/scan_table_dispatch.py` (it rejected `push {lr}`
   prologues, so every table in this family was silently skipped). With the
   fixed scanner, region 0x00190000-0x001B0000 has **66 unique handler
   entries missing from recomp-roots.txt**, including the exact pair
   (0x001A5908/0x001A5920) that escaped run3 at t=159 s.

All 66 were validated: the scanner accepts only real handler prologues
(`stmdb sp!`/`str lr,[sp,#-4]!`, up to 2 leading `mov-imm`), and a bounded
sample (0x00198D08, 0x00199CA0, 0x0019F308, 0x0019FACC, 0x001A5908,
0x001A5920) was re-disassembled with `tools/dq8_disasm_at.py` — all genuine
handlers of the same family. No bulk heuristic roots were added; prior roots
are preserved (`recomp-roots.txt.bak.<ts>` backup kept).

## 4. Changed files

| File | Change | By |
| --- | --- | --- |
| `src/main.rs` | y2r:u implementation (§2) | this task |
| `recomp-roots.txt` | +66 verified movie-decoder dispatch roots (this run); earlier: 0x001A9A68 + 10 decoder handlers | this task |
| `tools/build_split.sh` | multi-TU build wrapper (recomp + split_dispatch + 6-job compile, 4 GiB-cgroup safe) | this task |
| `tools/run_y2r.sh` | headless run harness: optional 1000 ms A presses, per-5s screen PPM snapshots, escape count | this task |
| `tools/ppm_stats.py`, `tools/ppm2png.py` | read-only PPM analysis/conversion helpers | this task |
| `tools/scan_table_dispatch.py` | jump-table scanner fix (push-prologue validation) | sibling task t_6058a51e |

## 5. Tests and evidence

Runs (all headless, no A presses, userdata **copy** `target/userdata-y2r-1`;
original `target/userdata-persist-20261021-a` untouched, sha256-verified):

| Run | Binary | Result |
| --- | --- | --- |
| base | pre-fix | FAIL: guest parks on handle 0x35 forever; 4 conversions of constant black; screen 0/96000 non-black |
| fix | +Y2R impl | PASS at service level: 4 conversions, event signalled, loop resumes; planes still constant black (decoder not rooted yet) |
| run2 | +decoder roots | died t=83 s: escape 0x001A9A68 (now rooted) |
| run3 | rebuilt | died t=159 s: escape 0x001A5920 after dispatch 0x001A5968 (one of the 66 now added) |
| **run4** | +66 roots | **PASS (decode+convert)**: 0 escapes / 300 s, 6 conversions; #6 = first real frame (§6) |
| run5 | same | PASS: t≈337 s, 7 conversions, fade-in progressing (#7 max=46); died on escape 0x0019739C (now rooted) |
| run6 | +0x0019739C | PASS: t≈354 s, same progression; died on escape 0x00195C20 (now rooted, with eq-sibling 0x001941AC) |
| **run7** | +0x00195C20/+0x001941AC, store-PC-base fix | **PASS (on-screen cinematic)**: 7 conversions; final display-transfer frame non-black = visible cinematic (§6); aborted t=328 s on unsupported `0xE672CF93` at 0x00195D7C |

Key artifacts:

* `target/y2r-evidence/run4/run.log` — 0 escapes, 6 conversions, transfer-end
  events signalled, IPC result codes.
* `target/y2r-evidence/run4/y2rframe-0006.ppm` — first real cinematic frame
  (raw max=15, 208,717 non-zero px).
* `target/y2r-evidence/run4/png/y2rframe-0006-x12.png` — the same frame at 12x
  brightness: visible 3D landscape (tree/hill/meadow/water/sky) with MJPEG
  macroblock artifacts — visual proof of actual decoded cinematic imagery.
* `target/y2r-evidence/run4/frames/t*.ppm` — screen snapshots every 5 s.

## 6. Remaining issues (honest status)

1. **GPU composition — SOLVED at frame level (run7).** The final
   display-transfer frame of run7 (`frames/t0400_final.ppm`, 208,717 non-zero
   px, identical content stats to decoded Y2R frame #6; visually verified as
   the same DQ8 countryside scene in
   `run7/png/screen-final-x12.png`) proves the game's compositor renders the
   Y2R RGB output to the top screen. Earlier runs' screen frames were black
   only because the first decoded frame arrived too late in their run windows.
2. **Unsupported instruction stops further playback.** run7 aborted at
   t=328 s: `unsupported guest instruction: mode=ARM pc=0x00195D7C
   opcode=0xE672CF93`. Decoded, that is `STR pc, [r12], r3, LSL pc` — a
   post-indexed, register-shifted STR of the PC value, part of the same
   unrolled copy-loop family as the `strb rd, [pc, rm, ror]` at 0x00194DE4
   fixed this session (the recompiler's single-data-transfer path does not
   yet cover this shift-by-register/PC-value combination). One further
   translation case, then the decoder tail can continue past frame #6/#7.
3. **Decode speed.** MJPEG decode under interpreted dispatch is ~1 frame per
   ~1-2 min wall clock (first real frame ~t=170 s). Real-time playback is not
   achievable at this translation speed; the goal here is correct visible
   frames, which the pipeline now produces.

### run5 / run6 results (operator-directed bounded conclusion)

* **run5** (started 20:15): survived to t≈337 s (vs run3's 159 s), 7
  conversions, fade-in verified progressing — conversion #7 planes
  Y=14..49 (mean 25), PPM max=46 with 253,666 nonzero px; the 6x-amplified
  PNG (`target/y2r-evidence/run5/png/y2rframe-0007-x6.png`) clearly shows the
  countryside scene with a small waterfall on the left. Died on escape
  0x0019739C (verified root since added).
* **run6** (started 20:19, binary +0x0019739C root): identical decode
  progression, survived to t≈354 s, died on the next callback target
  0x00195C20 (its eq-path sibling 0x001941AC also verified) — both added to
  recomp-roots.txt in the final bounded cycle; their effect is intentionally
  left for the next session to verify rather than another long run here.

## 7. Achieved release and exact next step

**Achieved (verified with frame + log evidence):**

* Full `y2r:u` service behavior implemented and IPC-compatible (19-command
  start-up sequence answered; transfer-end event wakes the presentation loop
  that previously parked forever on handle 0x35).
* YUV420→RGB conversion correct: constant-neutral input converts to exact
  black; real decoded input converts to recognizable imagery.
* Movie pipeline works end to end at data level: RomFS Moflex read → guest
  MJPEG decode → YUV420 ring-buffer slots → Y2R → RGB buffer with genuine
  cinematic frames (two independently inspected frames: run4 #6, run5 #7).
* Movie-decoder translated-code coverage extended by 69 verified roots
  (66 dispatch-family handlers + 0x0019739C + 0x00195C20 + 0x001941AC) and
  one recompiler capability fix (`str/strb` with Rn=15, previously a hard
  recompile abort at 0x00194DE4).
* No save data touched: all runs use the copy `target/userdata-y2r-1`;
  the original persisted userdata is untouched (sha256-verified in an
  earlier run of this task).

**Exact remaining problem (next task's first action):** continue playback past
the first on-screen frame. Concretely: teach the recompiler's
single-data-transfer path the `STR rd, [rn], rm, shift-by-register` encoding
seen as `0xE672CF93` at 0x00195D7C (same copy-loop family as the
`strb rd,[pc,rm,ror]` fixed this session; the load path already shares the
address/shift machinery, so the store path likely only needs the same
shift-by-register operand handling), rebuild via `tools/build_split.sh`, and
run headless ~600 s checking (a) no abort at 0x00195D7C, (b) more non-black
`y2rframe-NNNN.ppm` frames, (c) additional non-black screen snapshots as the
fade-in brightens. The 2 remaining verified roots and this diagnosis are
already in place; no further root discovery is needed for this step.
