# Crash fix at PC 0x00195D7C (and follow-up 0x00194DE4)

Task: t_bd99b9b8 — FOCUSED BUGFIX ONLY.

## Root causes (two, same defect class)

The movie-decoder blend loops are written in ARMv6 media instructions that the
recompiler's lifter never implemented, so the generic LDR/STR path rejected the
first unimplemented opcode it reached and the guest aborted at runtime via
`guest_unsupported()`.

1. **PC 0x00195D7C, opcode 0xE672CF93 = `UHADD8 r12, r2, r3`** (original
   report). The whole loop at 0x00195C20 is the parallel/SIMD arithmetic
   family (uhadd8/uadd8/usub8/uqadd8/uqsub8/sel...). Fixed in the previous
   run: generic parallel-arithmetic lifter + `par_op`/`par_sel` runtime
   helpers + per-thread GE flag consumed by SEL. Verified by 21 ARM-ARM
   vectors and confirmed in the generated C
   (`case 0x00195D7C: r[12] = par_op(r[2], r[3], 8u, 0u, 4u, &ge)`).

2. **PC 0x00194DE4, opcode 0xE6CF1073 = `UXTB16 r1, r3`** (first failure
   after fix 1; run9, t=325 s). The extend-family matcher in `src/main.rs`
   covered only the four Rn=15 SXTB/SXTH/UXTB/UXTH forms. The blend loop at
   0x00194DE4..0x00194E28 is built from `UXTB16` (with `ror #8`) + ORR byte
   shuffles; DQ8 also contains accumulate forms (SXTAB `ror #8` x8 at
   0x00189A74, UXTAB16 at 0x00187264, UXTAB/SXTAH elsewhere).

## Fix (this run)

`src/main.rs` — replaced the narrow matcher with the full ARMv6 extend
family on the shared media encoding (bits[27:24]=0110, bits[9:4]=000111):

- op1 8/12 = SXTB16/UXTB16 (two independent 16-bit lanes from bytes 0 and 2,
  SXTB16 sign-extends each lane), rotate applied before extraction.
- op1 10/14 + Rn!=15 = SXTAB/UXTAB, op1 11/15 + Rn!=15 = SXTAH/UXTAH:
  `Rd = Rn + extended(Rm)`.
- op1 8/12 + Rn!=15 = SXTAB16/UXTAB16: per-halfword add, no carry between
  lanes.
- Correct sign/zero width per op (byte vs halfword vs 16-bit lane).

New tools: `tools/scan_extend.py`, `tools/scan_extend_addrs.py` (encode the
family's exact masks and census the binary).

## Verification

- 10-vector C test of the exact emitted expressions
  (`scratch/test_extend.c`): UXTB16 ror0/ror8, SXTB16 sign lanes, UXTB/SXTB/
  UXTH/SXTH, UXTAB, SXTAB ror8, UXTAB16 cross-carry discriminator — ALL PASS.
  (Vectors were hand-checked against ror/lane arithmetic; three initial
  expectation errors were in the test, not the lifter.)
- `python3 tools/test_parallel_ops.py` (previous run's suite): ALL PASS — no
  regression to fix 1.
- Regenerated translated C: abort site now lifted
  (`target/dq8-split/dq8_part_004.c:7996`, `case 0x00194DE4` emits the UXTB16
  shuffle), ror-8 form at :7999, accumulate SXTAH at part with
  `case 0x002061E8`, original site `case 0x00195D7C` keeps `par_op`.
- `tools/build_split.sh` → OK (92 MB `target/dq8-startup-split`, 23:53).
- **Bounded regression `tools/run_y2r.sh run10 target/userdata-y2r-1 450`:**
  full 450 s (clean SIGTERM, exit 143), **0 `unsupported`/`escaped` lines**
  (run9 had 1, at 0x00194DE4), 91 full-size frames, content renders from
  ~t=330 s onward (run9 aborted at t=325 with all-black frames).
  Final frame `target/y2r-evidence/run10/t0450_final.png` is clean, artifact-
  free DQ8 in-engine 3D content.

## Evidence paths

- `target/y2r-evidence/run10/run.log`, `timeline.log`, `frames/`
- `target/y2r-evidence/run10/t0450_final.png`
- Diff: `src/main.rs` (+1053/-99 total across both runs; this run added the
  ~70-line extend matcher) plus new `tools/scan_extend*.py`.

## Note

This run could not post board comments or call request_review: the only
exposed kanban tool was `kanban_schedule`, and `hermes kanban` CLI mutations
are rejected for delegate-task child contexts.

## Review cleanup cycle (run 62, 2026-10-22)

The first review asked to strip everything from the diff that is not part of
the ARMv6 media-instruction fix. What was done, with proof where a revert
proved impossible:

Reverted (originals kept under `review-reverts-backup/`):
- PC-sampling histogram (`guest_pc_hist_*`, `DQ8_PC_SAMPLE_US`, `<sys/time.h>`,
  `main()` wiring).
- `pica_sample_tex0` texture-format work (ETC1/ETC1A4 plus the fmt 2/4/6/9/10/11
  decoders) — back to HEAD behaviour.
- PC-relative store Rn=15 tweak (restores
  `return Err("store with PC base at ...")`).
- `src/pica_vs.c` draw tracing (whole file back to HEAD).
- `README.md` (see note below).

Proven REQUIRED — each revert was attempted and the generation stage failed
immediately, so the change was re-applied:
- VFP register-list error path: `dq8tool: invalid VFP register list at
  0x00220138` — a root in the approved root set decodes into a word that only
  resembles a VFP multiple transfer; with HEAD's hard `Err` no root set can be
  generated at all.
- `arm_pc_relative_ldr_target` bit-25 check: `dq8tool: function 0x005443BC: no
  supported BX LR within 4096 instructions` — exactly the bogus literal-pool
  word the check exists to reject (0x005443BC is enqueued as a data address
  "function entry").

Proven NOT required any more: the store-Rn=15 tweak. With the full extend
matcher in place, `0xE6CF1073` (0x00194DE4) is matched as UXTB16 before the
generic store path is reached, and generation of all 13544 entries succeeds
with the tweak reverted.

Kept, with justification:
- `recomp-roots.txt`: the 69 verified movie-decoder roots were approved as
  part of card t_95c65386 (its review artifacts include this file). Root
  `0x00195C20` — the blend-loop entry holding 0x00195D7C — is NOT in HEAD:
  without it the run escapes at 0x00195C20 (documented in run6 of t_95c65386)
  instead of ever translating the crash site, so the regression could not
  prove anything.
- `README.md` was reverted per the review, but note its actual content was the
  "Save-data directory" section required by approved card t_6872909e (not the
  histogram/ETC1 documentation the review referenced); the original text is
  preserved in `review-reverts-backup/README-save-data-section.patch` for that
  card's owner to re-apply.

## Regression after cleanup: run11

- `tools/build_split.sh` → OK (13544 extra entries, 949296 instructions,
  85880 functions, binary 92 MB at 01:32).
- `python3 tools/test_parallel_ops.py` → ALL PASS (parallel/SIMD + SEL vectors).
- Extend-family C vectors (`scratch/test_extend.c`) → 10/10 ALL PASS.
- `Y2R_EXTRA='' tools/run_y2r.sh run11 target/userdata-y2r-1 450`:
  full 450 s, clean SIGTERM (exit 143), **0 `unsupported`/`escaped`/
  `segmentation` lines**, 91 full-size frames, final frame stats
  `max=46 nonzero=253666` — byte-for-byte identical statistics to approved
  run10's final frame; run log is 5098 lines vs run10's 5097 (same content,
  thread-order jitter only).
- Generated C re-checked: `case 0x00195D7C` still emits
  `r[12] = par_op(r[2], r[3], 8u, 0u, 4u, &ge)` and `case 0x00194DE4` emits the
  UXTB16 lane shuffle (`target/dq8-split/dq8_part_004.c`).
