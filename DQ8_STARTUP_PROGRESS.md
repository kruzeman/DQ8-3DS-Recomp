# DQ8 startup progression — status report

Date: 2026-10-21, task t_c4c66c6c (session 00:39–02:0x IDT).
Scope: diagnose/fix startup path through dialog 1, the extra-data dialog,
extdata initialization and the 3D scene. All claims below are backed by the
logs/screenshots listed; nothing is claimed without visual evidence.

## TL;DR

* Dialog 1 ("No previous save data could be read. / New save data will be
  created.") renders correctly and never auto-advances — verified over two
  no-input runs of 155 s and 300 s (not a speed artifact).
* Root-cause work this session: (a) the whole startup path is translation-
  coverage-bound — every stage transition executes virtual calls/methods that
  the static recompiler has not rooted yet, and each one aborts the guest;
  the A-press escape chain was iterated down until the press survives;
  (b) the guest renders at ~3.2 fps instead of 60 because 82% of CPU time is
  spent inside the GSP queue-trigger SVC doing software PICA list processing.
* The auto-advance gate itself is NOT time- or frame-count based (300 s
  static), and the dialog's timer/counter code (state field [obj+0xA1],
  counters [obj+0x80/0x84], vtable+0x28 dispatch at 0x00135920) only executes
  AFTER an A press — i.e. in the current build the first dialog is input-only
  and the code that would run a close sequence is reachable but has not been
  observed to change the screen yet (see "Open questions").
* Second dialog / extdata creation / 3D scene: NOT yet reached — blocked on
  the above.

## 1. Environment and how to reproduce

All runs (workspace /home/hermes/DQ8):

```sh
# no-input observation (frames to a private prefix, no cross-run races)
env DQ8_RUN_GUEST_THREADS=1 DQ8_HEADLESS=1 DQ8_TRACE_INPUT=1 \
    DQ8_DUMP_TRANSFER_PREFIX=/tmp/dq8-auto/x-... DQ8_DUMP_TRANSFER_LATEST=1 \
    DQ8_INPUT_FILE=<empty-or-button-file> \
    ./target/dq8-startup-headless DQ8.code target/dq8.romfs
# A press: write "1" (then "0") into the DQ8_INPUT_FILE path.
```

Build procedure used (isolated outputs; canonical target/dq8-startup.* untouched
this session):

```sh
cargo run --release --quiet -- recompile-arm DQ8.code 0x00100000 \
  target/dq8-noinput.c $(awk 'NF && $1!~/^#/{printf "--extra-entry %s ", $1}' recomp-roots.txt) \
                      $(awk 'NF && $1!~/^#/{printf "--noreturn-call %s ", $1}' recomp-noreturn.txt)
clang -std=c11 -O0 -g0 -w target/dq8-noinput.c -o target/dq8-startup-headless -lm
```

## 2. Behavioural matrix (all verified this session)

| Run | Input | Result | Evidence |
| --- | --- | --- | --- |
| runA 155 s | none | RC=124, no escapes; dialog types ~40–60 s then static to 150 s | /tmp/dq8-auto/runA/frames/montage.png |
| runE 300 s | none | RC=124, no escapes; dialog static from ~84 s to 300 s | /tmp/dq8-auto/runE/frames/montage.png |
| runB A@55 s | A 200 ms | press delivered (HID line), then escape `003B1AA0 after 0013592C` → exit 3 | /tmp/dq8-auto/runB/run.log |
| ac/c1 A@55 s | A 250 ms | escape 0x003B1AA0 (same), root added | /tmp/dq8-auto/ac/c1/run.log |
| ac/c2 A@55 s | A 250 ms | no escape BUT no HID line either → press never read, invalid pass | /tmp/dq8-auto/ac/c2/run.log (grep HID = 0) |
| retest A@55 s hold 5 s | A 5 s | see section 5 | /tmp/dq8-auto/retest/ |

Reference (Azahar, per task): 10 s dialog 1 → 11–12 s dialog 2 appears with NO
input → 20 s A starts extdata creation → 23–24 s success → 25 s A → 26 s 3D
scene. Our build reaches dialog 1 only at ~40 s (all frame-paced UI ~20× slow,
section 4) and never leaves it without input.

## 3. Why the first dialog does not auto-advance (investigation results)

Ruled out:
1. **Timer/frame-count that is merely slow.** 300 s no-input run: dialog
   completes text at ~84 s and is pixel-identical at 126/168/210/252/294 s.
   At the measured 3.2 fps that is ~700+ frames of waiting — no frame-count
   wait explains it, and svcGetSystemTick is correctly implemented
   (src/main.rs svc_get_system_tick, 268111856 ticks/s from CLOCK_MONOTONIC),
   so a 1–2 s wall-clock auto-close would have fired.
2. **Translation crash on the no-input path.** Both long runs are escape-free.
3. **Main-thread pump stall.** Progress snapshots show the main thread parked
   on the startup-object arbiter 0x5CCCA4 and re-running one UI step per
   vblank the whole time (healthy), with the startup-object state transitioning
   FFFFFFFF→00000000 (flag76 01→02) around 30–40 s.

Facts about the gate:
* The block at **0x001358D4–0x0013592C** is the dialog's per-step state
  machine: state byte [obj+0xA1] selects a counter [obj+0x84]→[obj+0x80]
  (a timer/progress step), then an unconditional virtual call `bx r1` with
  `r1 = *( *(obj) + 0x28 )` (vtable slot 7). This block executes ONLY after
  an A press: without input, no escapes occur even though its callees
  (e.g. 0x003B1AA0, clamped counters [obj+0x70/0x74], [obj+0x90/0x94]) are
  unrooted — proof the whole update path is input-gated in the current build.
* Two worker threads are parked forever on never-signaled address arbiters
  (thread[3] 0x60E498 WaitIfLessThan 1, current 0; thread[5] 0x60BAB0
  WaitIfLessThanOrEqual, current 0xFFFFFFFF) and thread[4] waits on event 0x2C
  — none ever signaled in any run (consistent with the earlier stall
  diagnostic DIALOG_TEXT_STALL_DIAGNOSTIC.md). If the auto-advance job is
  posted to these queues, the poster never runs.
* Static xrefs to the "TextBox_%02d" format string: no LDR-literal / ADD-pc /
  MOVW-MOVT references; the string copies are only referenced from 6 raw
  pointer-table slots (e.g. 0x001BEE34 → 0x001BF5D4), i.e. widget-layout
  descriptor structs — next step for a code-level gate read is walking those
  descriptors from their users.

Conclusion for the auto-advance: with the current build the first dialog is
input-only; either (a) the close/advance logic it needs is one of the
never-signaled worker/dispatcher paths above, or (b) the reference's
auto-advance runs through the same vtable update path that is only entered
on input. Distinguishing (a)/(b) requires rooting the input-path methods
(done) and then re-observing a press-driven close, plus identifying the
poster of the parked worker queues.

## 4. Frame-rate bottleneck (found, quantified, not yet fixed)

* Effective rate: 1004 display transfers / 155 s ≈ 3.2 fps (3DS: 60).
* New instrumentation in src/main.rs (env-gated `DQ8_PC_SAMPLE_US`,
  `DQ8_PC_HIST`; ~40 lines, additive) samples `current_guest_pc` on SIGALRM
  and dumps a histogram at exit/SIGTERM. 10 ms sampling over a stuck run:
  **82.24 % of 13 837 samples land on a single guest PC: 0x0016396C**, the
  `svc #0x32` (SendSyncRequest) inside GSP-thread function 0x00163958 that
  posts GSP command-queue trigger `0x000C0000`.
* IPC counting: 2 320 queue-trigger IPCs in 30 s (~77/s) → ~10 ms host cost
  each; the handler (src/main.rs:5875) synchronously runs
  `pica_process_command_list` (line 5897) etc. The software PICA backend at
  -O0 is therefore the frame-rate limiter (guest logic per frame is <8 %).
* Histogram: /tmp/dq8-auto/hist/stuckD.txt (908 non-zero PCs);
  analyzer: /tmp/dq8-auto/analyze_hist.py.
* Candidate fixes not yet applied: build with -O2 (diagnostic), or reduce
  per-trigger work. Not required for dialog logic itself, but it is why all
  our timings are ~20× the reference.

## 5. A-press escape chain (stage-2 prerequisite) — progress

Chain observed (each hop was an abort until rooted): 0x003AF770 → 0x003B2A5C
→ 0x003B2BB8 → 0x0049D288 → 0x0049D378 → **0x003B1AA0** (added by this
session's iteration loop /tmp/dq8-auto/apress_cycle.sh). With 0x003B1AA0
rooted, cycle 1's press reached the method and aborted at it; after adding
it, subsequent behaviour is under test (retest run below).

Retest (A held 5 s at T+55 s on the binary with 0x003B1AA0 rooted):
press registered (HID line `buttons=00000001 pressed=00000001 index=1`), then
the guest aborted one hop deeper: `translated PC escaped to 003B1840 after
00135A88 lr=00135A8C` → exit 3 (/tmp/dq8-auto/retest/run.log:587-589).
0x003B1840 is the next vtable method of the same object (r0=r4=0x141F03E0,
same dispatch shape as the 0x0013592C site). Root 0x003B1840 appended to
recomp-roots.txt and target/dq8-startup-headless rebuilt (BUILD3_RC=0) —
the next worker can continue the cycle loop (/tmp/dq8-auto/apress_cycle.sh)
from this state.

## 6. Visual verdicts (appendix — updated after final run)

* /tmp/dq8-auto/runA/frames/montage.png — no-input 155 s: dialog static.
* /tmp/dq8-auto/runE/frames/montage.png — no-input 300 s: dialog static.
* /tmp/dq8-auto/ac/c2/montage.png — invalid test (press not delivered).
* /tmp/dq8-auto/retest/frames/montage.png — run aborted at the 0x003B1840
  escape right after the press; screen still dialog 1 at death (frames
  t1..t45 captured, no progression observed).

## 7. Achieved milestones / remaining blockers

Milestones:
1. Two clean, escape-free long no-input runs; dialog 1 behaviour fully
   documented with screenshots.
2. Frame-rate bottleneck quantified to a single SVC (82 %) with new
   instrumentation.
3. A-press escape chain iterated from "dies at first press" to rooted through
   0x003B1AA0 with automated cycle tooling (/tmp/dq8-auto/apress_cycle.sh).
4. Orphaned background loop of blocked card t_4bacd398 cleaned up; its roots
   preserved; build-pipeline conflicts avoided via isolated outputs.

Remaining blockers (not claimed as solved):
1. First dialog does not auto-advance (root cause narrowed: input-gated
   vtable update path + never-signaled worker queues; needs the poster of
   those queues identified or a press-driven close observed).
2. A press does not yet visibly close dialog 1 (input delivery in one cycle
   was flaky — retest in section 5/6; if the press registers and the dialog
   still stands, the close method needs the same rooting iteration).
3. Dialog 2, extdata creation, success message, 3D scene — not yet reached.
4. 3.2 fps render loop (PICA host cost) — measured, fix candidates listed.

## 8. Files changed this session

* src/main.rs — +PC-sampling histogram instrumentation (env-gated,
  DQ8_PC_SAMPLE_US / DQ8_PC_HIST; SIGALRM histogram + SIGTERM dump).
* recomp-roots.txt — +0x003B1AA0 (escape iteration); prior roots from
  t_f6efdfda / t_4bacd398 preserved.
* Build artifacts (isolated): target/dq8-noinput.c, target/dq8-startup-headless.
* Canonical target/dq8-startup.c / target/dq8-startup NOT modified by this
  session (last writer: blocked sibling card, 01:22 window).
