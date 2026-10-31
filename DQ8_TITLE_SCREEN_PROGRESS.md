# DQ8 interactive title screen — progress report (task t_5526a598)

Date: 2026-10-22, workspace `/home/hermes/DQ8`, branch `feature/interactive-title`
(created from fresh `main` = 411cf53). Every claim below is backed by the
logs/frames listed; nothing is claimed without evidence.

## TL;DR (honest status: goal NOT reached)

1. **The intro movie cannot be skipped in the current build, because nothing
   polls input while the movie state is active.** 19 HID press/release edges
   (A and START, 1000 ms each) were published through the real HID shared
   memory during the movie in runs t1/t2/t3; the top screen stayed
   byte-identical before and after every press and no new guest path ran.
2. **The movie is also decode-bound**: ~9 YUV frames converted in 540 s
   (~1 frame/min under interpretation), so "wait for the movie to end" is
   hours of wall clock. The title screen therefore cannot be reached in a
   bounded run by waiting either.
3. **Real progress this task**: three previously-fatal decoder aborts fixed
   (verified roots `0x001973CC`, `0x00196A74`, `0x0019368C`); run t3 completed
   the full 540 s with **0 escapes** where t1 died at t=475 s and t2 at
   t=481 s. A reproducible, bounded, headless startup test
   (`tools/test_startup_title.sh`) and a bottom-screen snapshot path were added.

## 1. Environment / how to reproduce

```sh
# prepared save/extra data is only ever COPIED (originals untouched):
#   target/userdata-title-1  <- cp -a target/userdata-y2r-1
TITLE_EXTRA='DQ8_TRACE_Y2R=1' tools/test_startup_title.sh <label> <seconds> \
    340:START 380:A 420:START 460:A      # 1000 ms presses at those seconds
```

The test runs `target/dq8-startup-split` headless (`DQ8_HEADLESS=1`, no SDL
window, so no xvfb is required), copies the prepared save into
`target/title-evidence/<label>/userdata`, injects scripted 1000 ms button
presses through `DQ8_INPUT_FILE` (HID shared-memory ring, press + release
edges traced by `DQ8_TRACE_INPUT`), snapshots both presented screens every 5 s
and around every press, and prints a summary (escapes, HID edges, Y2R
conversions, final frame stats). Runs are hard-bounded and SIGTERMed at the
requested duration.

## 2. Runs (all headless, all on prepared-save copies)

| Run | Binary | Input | Result |
| --- | --- | --- | --- |
| t1 | +0x001973CC | A @340/400/460 s | died t=475 s, escape `0x001973CC after 001B3148` |
| t2 | +0x001973CC | A every 20 s from 300 s | died t=481 s, escape `0x00196A74 after 001973F0`; 19 HID edges, screen unchanged around every press |
| t3 | +0x00196A74/+0x0019368C | START @340/420/500, A @380/460 | **completed 540 s, 0 escapes**, 9 Y2R conversions, top+bottom screens captured |

Artifacts: `target/title-evidence/{t1,t2,t3}/` (`run.log`, `timeline.log`,
`frames/` incl. `*.bottom.ppm`, `summary.txt`, `png/`).

## 3. Why the movie cannot be skipped (evidence)

* **Input publishing is correct.** `src/native_hid.c` writes the PAD state at
  shared-mem `+0x1C`, the ring index at `+0x10` and 8×0x10 entries at `+0x28`
  with pressed/released deltas — this matches 3dbrew "HID Shared Memory" and
  Azahar's `SharedMem` struct (checked 2026-10-22). The same mechanism
  dismissed save dialog 1 in earlier tasks, so delivery works when guest code
  reads it.
* **Nobody reads it during the movie.** From the 5 s progress snapshots of t1
  (`tools/snapshot_threads.py target/y2r-evidence/t1/run.log`): the main thread
  alternates between `svcWaitSynchronization` (`0x0012C3E4`) and the startup
  wait loop `0x002FA34C` (read `[obj+0xC]`; ==3 → done, ==1/7 → fail, else wait
  and re-check); the startup thread[6] sleeps on its message queue
  (`0x0014D6A4`); worker threads[3]/[5] wait forever on never-signaled address
  arbiters and thread[4] on event `0x2C`; thread[11] is the movie decoder. The
  ring-buffer wait loop `0x002F928C` (disassembled) waits on an event and never
  reads the PAD. So during the movie state no guest code samples input — the
  press is published and ignored, which is exactly what t1/t2/t3 show.
* **The parked waits are not HID waits.** The new `DQ8_TRACE_INPUT` handle
  print (run `smoke`) shows the HID events are `pad=0x27/0x28, accel=0x29,
  gyro=0x2A, debugpad=0x2B`; thread[4] parks on `0x2C` and the others on
  address arbiters, i.e. no guest thread waits on a HID pad event at all, and
  the runtime's HID wake path (which signals `0x27`/`0x28` every 16.7 ms) is
  therefore never exercised during the movie.
* **Waiting is not an option either.** ~9 converted YUV frames in 540 s of a
  25 MB Moflex (`data/Movie/P01.moflex`); the decoder runs interpreted ARM at
  ~1 frame/min, so the movie needs hours, and the title state is only entered
  when the movie finishes.

## 4. Changed files

| File | Change |
| --- | --- |
| `recomp-roots.txt` | +3 verified movie-decoder roots: `0x001973CC` (t1 escape), `0x00196A74` + `0x0019368C` (the two `bx r3` literals inside `0x001973CC`, t2 escape). Every entry verified with `tools/dq8_disasm_at.py` for a genuine ARM entry prologue; prior roots preserved, timestamped backups kept. |
| `src/native_hid.c` | +4 lines: env-gated (`DQ8_TRACE_INPUT`) print of the shared-memory base and the 5 HID event handles — needed to map "who waits on which HID event" in the handoff. |
| `src/main.rs` | +17 lines: bottom-screen snapshot next to the existing top-screen one (`/tmp/dq8-screen-bottom.ppm`) so headless runs can prove 3DS menu content. |
| `tools/test_startup_title.sh` | new: bounded, reproducible, headless startup/title test (see §1). |
| `tools/snapshot_threads.py` | new: per-thread guest-PC summary from a run log's progress snapshots. |

## 5. Build / tests

* `BUILD_JOBS=6 tools/build_split.sh` → OK three times (binaries 07:56, 08:16,
  and after the bottom-screen change).
* `tools/test_startup_title.sh t3 540 340:START 380:A 420:START 460:A 500:START`
  → full 540 s, `exit=143` (bounded SIGTERM), **0** escape/unsupported lines,
  9 `Y2R conversion #` lines, 10 press/release edge pairs traced.
* Note for the next worker: `DQ8_DUMP_TRANSFER_PREFIX` (per-transfer PPM dump
  inside the GSP handler) roughly halves guest speed — the harness therefore
  relies on the new `/tmp/dq8-screen-bottom.ppm` snapshot instead.

## 6. Next step (exact, bounded)

Make the guest sample input during the movie state, then re-run the test with
A/START presses and capture the title screen:

1. Determine which guest code reads the HID PAD during the movie (the new
   `DQ8_TRACE_INPUT` handle print identifies the pad-event handles; compare
   with the parked waits `0x2C`/arbiters in the snapshots). If a guest thread
   waits on a pad event and never wakes, the runtime's HID wake path is the
   bug to fix.
2. If no such code exists, the skip is not implemented in the guest's movie
   path — then the options are (a) a much faster decoder (the PICA software
   backend eats most CPU; see DQ8_STARTUP_PROGRESS.md §4) or (b) accepting the
   multi-hour movie. Do **not** fake the movie end or the title state in the
   runtime.

---

## Attempt 3 (review round 1 + operator direction): read-tracer + long holds

Goal status: **still NOT reached — the menu screenshots do not exist.** What
this round adds is direct read-level proof of who samples the PAD, when, and
that the movie state ignores it, plus a playback-waits analysis.

### New instrumentation (env-gated, off by default)

* `DQ8_TRACE_HID_READS=1` — every guest load that lands in the HID shared
  memory page (base set when the guest maps handle 0x30010) is logged from the
  `mem32/mem8/mem16` funnel: `HIDREAD t= <sec> th= <thread> pc= addr= off=
  val= lr= sp= hits=`. Per-PC throttling (first hit + value changes + 10 s
  heartbeat) keeps the log bounded (424-452 lines per 600 s run). Runtime
  helper reads (the HID pump's own clock upkeep) are excluded.
* `tools/test_startup_title.sh` now supports long holds: spec
  `<down>-<up>:<button>` (e.g. `340-460:A` = one 120 s hold); overlapping
  specs OR into one button mask, press/release edges logged + snapshotted.

### Runs (all headless, prepared-save copies, 0 escapes, binary 09:26)

| Run | Input | Key result |
| --- | --- | --- |
| smoke2 90 s | hold 40–80:A | harness OK (edges exactly t=40/80); tracer OK |
| t4 600 s | presses 10/20/30/100/180/260/480/560:A + hold 340–460:A | 9 Y2R conv.; frames: boot logo → black → movie frame 1 (t≈330–490) → movie frame 2 (t≈495–600) |
| t5 600 s | sticky A whole run (1–9999) | playback IDENTICAL to t4 (same frame md5s, 9 conv.), no skip, no state change |

### What the HIDREAD trace proves

1. **The guest polls the HID ring directly and heavily during the boot-logo
   phase**: ~6.5 reads/s for t=4–35 (ring index +0x10, current entry buttons/
   pressed/released at +0x28+k·0x10, timestamps, circle pad, motion). Reads
   from PCs `0x001435A4/0x00143680/0x00121D14/0x0013420C`, all with `th=99`
   (bootstrap/main startup context — the startup thread itself is the poller).
2. **Polling then collapses at t≈35** to rare wake-batches only:
   t≈55, 73, 162, 326/328, 491/495 — i.e. **input is sampled roughly once per
   movie-frame presentation (~165 s)**, not at 60 Hz. Every 1000 ms press
   therefore misses with ~98% probability; even the reviewer's 120 s hold
   `[340,460)` (t4) contained **zero reads** (wakes at 328 and 495 fall
   outside the window).
3. **Sticky A is genuinely read during the movie and does nothing** (t5):
   ring entries with `val=00000001` were read at t=4/14/24/34/55/73/162/326/
   491 — including both frame presentations — and playback ran on unchanged,
   identical frame-by-frame to t4. So the skip/confirm path is **not active in
   the current state**; the long-hold shortcut (reviewer step 1) is refuted
   with evidence, not merely unobserved.

### Playback waits (operator direction, items 1–3)

* **Nothing event-starved in the display path.** During t=350–540 (t3, 109
  snapshots) the decoder thread[11] NEVER blocks and its pcs vary across
  0x001A7Axx–0x001A9Exx each sample → it is executing varied code, not
  parked on a semaphore/queue: decode is **CPU-bound under interpretation**
  (~9 Y2R conversions/600 s; screen advances ≈ every 165 s). Y2R end events
  are signaled (conversions complete) and GSP transfers complete (frames are
  shown), so no missing runtime signal was found in the frame path.
* **Constant waits during the movie** (identical before/during/after): main
  in startup wait loop 0x002FA34C with startup object state=5 (target state
  =3, never reached); th[3] light-lock arbiter 0x0060E498 (v=1); th[5]
  arbiter 0x0060BAB0 (v=0); th[4] event 0x2C; th[6] startup queue 0x2E;
  th[1]/th[10] sleep at 0x0011D3F8; th[0] event 0x02. These are all present
  from the logo phase on, so none of them gates the frame path.
* **Intro re-logo (t3, md5 = boot logo at t405–425) did not occur in t4/t5**
  (neither pressed START; t3 pressed START at 340/420/500) → best-supported
  explanation: a START read at the t≈328/340 wake **aborted the intro to a
  logo restart** — input only matters at those rare wakes, and A (read at
  every wake in t5) has no effect in this state.

### Honest blocker + concrete next step

The title screen requires either the intro movie finishing (hours at
interpreter speed: no waits to fix — it is decode throughput) or an active
skip path (not observed: A=1 read repeatedly during playback, nothing
runs). Minimal bounded next probe, no faking: (1) `DQ8_TRACE_WRITE` on the
startup state object (0x005CCCA4 region) to find who is supposed to set
state=3 and when it fires in real runs; (2) disassemble the consumer of the
`0x001435xx` poller output (what the startup thread does with the sampled
buttons) to prove whether a skip action exists at all in this state —
per prior report §5, if none exists the options are a faster decoder or
accepting the multi-hour movie.

Artifacts: `target/title-evidence/{t4,t5}/` (run.log with HIDREAD lines,
timeline.log, frames/, png/, summary.txt), `target/build_hidtrace2.log`.

## Attempt 4 (after run-70 timeout): movie-end path mapped end to end

### The object, the state machine, the finish condition (static + traced)

* The intro/startup **player object is at guest RAM `0x0882EDD4`** (main's `r4`
  inside the wait loop; state word `0x0882EDE0` = obj+0xC, wait object
  obj+0x20 = `0x0882EDF4`). Address is stable across runs.
* `0x002FA318` = *play-and-wait* for main: entered with state 1 or 8 -> sets
  state **5**, waits on obj+0x20, then polls: state **3** -> return 1,
  state 7/1 -> return 0.
* `0x002FA0F0` = *stop-and-wait*: state 3 -> **2** (via setter `0x002FC018`),
  waits until the driver parks it at **1**.
* `0x002FBBE0` (driver step, entered from thread loop `0x002FA2EC`) owns the
  2 -> 1 transition (`0x002FBC90`) and calls the finish check every step.
* **The only writer of state 3 is `0x002FA8D0`** inside the finish check
  `0x002FA81C`: stream count (from `0x004CA0E4`) must be nonzero **and** both
  per-stream diffs `(vtbl[0x20] result) - (vtbl[0x10] result)` must be 0.
* Controller `0x0024F584` (called from `0x0023B4D4`) does: stop -> tail-call
  play-wait (`0x0024F618`), i.e. it **restarts playback after every finish**.

Instrument: `DQ8_TRACE_WRITE=0x0882EDE0:0x30` (state writes, PC+sp per write),
`DQ8_TRACE_PC=0x002FA844,0x002FA8B0` (finish check: stack +0C=count,
+10/+14=diffs), new `DQ8_TRACE_PC_MAX` (raises the existing 64-hit cap).

### Measured behaviour (runs st1/st2/st3, headless, prepared-save copies, 0 escapes)

| run | env | length | result |
|---|---|---|---|
| st1 | state-write + PC tracer | 300 s | 3 full cycles 5->3->2->1->5, all frames black t35..t300 |
| st2 | + DQ8_TRACE_Y2R + finish-check stack | 300 s | count=2 always; diff0 walks **6->5->4->3->2->1->0**, diff1=0; at 0: state 5->3, 1-2 Y2R conversions, controller stop+restart (3 restarts); 6 conversions |
| st3 | DQ8_TRACE_WAIT + DQ8_TRACE_ARBITER | 180 s | 6 conversions; 19 SignalEvent lines (handles 0x2D, 0x36 only); arbiter ops: 0x005CCCA4 Wait x125, 0x0060E498 Wait x2, 0x0060BAB0 Wait x1 — **zero Signal (type 0) ops on any of them** |

So the movie-end/clip-end path **works**: each cycle drains a 6-item stream-0
buffer, the finish check fires, main consumes state 3, the controller starts
the next cycle. Clip cycles completed at t=55/75/165 s (st2) and a 4th was
mid-drain at t=290 s. What kills bounded runs is the drain rate: **one stream-0
item takes 3-15 s** (cycle 20-110 s) against a 33 ms reference item (~100-450x
per item; whole intro = hours), i.e. operator's item (б), not a missing
movie-end signal. Guest sustains ~2.0 M instructions/s and the decoder
thread[11] is runnable in 53/60 snapshots (decoder-range PCs in 44/60) while
main polls the wait loop.

Both screens stay byte-identical black after t=35 (top md5 7718d14f, bottom
533a3c5b) even though Y2R conversions run at every cycle end.

### Operator's single-event wake question (t_51e76b3b) — checked in THIS tree

* Single-event wake exists **only** from guest `svc_signal_event`
  (`src/main.rs:7049`, wakes `wait_handle` matchers). Runtime-side
  `event_signaled[]=1` (GSP relay, Y2R end, APT, DSP pipe) does **not** wake a
  parked single-handle waiter; `schedule_guest_thread` wakes only WaitN
  (`wait_n_active`), thread-handle joins and timed (`wake_ns`) waits — same
  gap class the audio task fixed in `feature/audio-save-stage` (b4000c9).
* In our runs the parked single-event waiters are only **th[0] handle 0x02**
  (SRV, deliberately parked) and **th[4] handle 0x2C** = the DSP audio-pipe
  event: nothing in this branch ever signals it (no DSP HLE here) — the audio
  task's DSP HLE + wake is the fix and it is not merged into this branch.
* th[3]/th[5] are address-arbiter (LightLock-style) waiters on 0x0060E498
  (word 0, want >= 1) and 0x0060BAB0 (word -1, want >= 0); the arbiter wake
  machinery (`svc_arbitrate` type 0, `src/main.rs:5225`) exists but **no
  Signal is ever issued** on those addresses (st3: only Wait ops), i.e. the
  unlock side never runs — most plausibly the same blocked audio-side chain.
  Main's own startup poll `ArbiterAddress(Wait, 0x005CCCA4, 0)` (word stays
  FFFFFFFF) is a depth-0 poll loop, so it keeps making progress.

Conclusion for this task: no missing-wake call site is on the observed
intro/title path (the clip cycles advance without those threads); after
`feature/audio-save-stage` merges, re-check whether th[3]/[4]/[5] wake, and
port the generic single-event wake (one condition in `schedule_guest_thread`)
if a parked waiter ever matches a runtime-signaled event.

### Next step (bounded)

Profile where a single stream-0 item's ~10-30 M guest instructions go:
PC histogram of decoder thread[11] (0x001A7Axx-0x001A9Exx) over the drain
window + FS/Moflex read counts per item (existing `DQ8_TRACE_*` + tools).
Reference: 33 ms/item, ours 3-15 s/item.

### st4 (240 s, DQ8_TRACE_FS_SERVICE + DQ8_TRACE_Y2R): the time is CPU, not file I/O

Same reproducible shape as st2: Y2R conversions #1-6 at t≈55/70/160 s (three
clip cycles), 0 escapes, both screens black. FS IPC during the whole run:
**13 service requests total** (4x 0x080C00C2, 6x 0x080201C2, 3x 0x080E0080) —
the movie data is already resident (archive/RAM), so the 3-15 s per stream-0
item is spent in interpreted guest code (decoder thread[11], runnable 53/60
snapshots), not in repeated file reads. Rules out the "re-scan of the file"
hypothesis for this build; the next probe is a decoder PC/instruction profile.

## Attempt 5 (review round 4): -O2 rebuild, progression proof, t6 skip test

### -O2 vs -O0 (operator item 1, mandatory before/after) — hypothesis REFUTED

`tools/build_split.sh` compiled the whole generated runtime at `-O0` (both
compile lines); changed to `-O2` and rebuilt `target/dq8-startup-o2`
(35.9 MB, commit 15f38aa). Measured on identical runs:

| metric | -O0 (st2/st4) | -O2 (o2a, t6) |
|---|---|---|
| guest throughput | 2.22-2.50M steps/s | 2.50-2.86M steps/s (+15-29%) |
| clip-cycle cadence | 55/75/165 s (gaps 20/90 s) | 40/130/270 s (gaps 90/140 s); t6: 135/280/435/565 s (gaps 95/145/155/130 s) |
| stream-item time | 3-15 s | ~20-40 s per item (batches shrink: 4→2→1→1 frames per batch) |
| Y2R conv | 6/300 s, 9/600 s | 11/300 s, 13/600 s |

**-O2 does not remove the pathology** (~5000x needed, we got ~1.15x). The
bottleneck is not host-code quality; per-guest-instruction dispatch costs
~350 ns (~1000 host cycles) — ~30x slower than a plausible C interpreter and
almost -O2-insensitive. `ptrace_scope=1`/`perf_event_paranoid=4` block
external sampling, so the next probe is in-process PC histogram
(env-gated, dispatch-loop sample counter).

### Item 2: progression vs looping — PROGRESSION (t6, 600 s)

t6 (`tools/test_startup_title.sh t6 600 70-400:A 480-540:A 570-595:A`,
-O2 binary, DQ8_TRACE_Y2R + state trace): 0 escapes, 6/6 HID edges
published, 4 clip cycles. Frames are a monotonic sequence of DISTINCT md5s
with growing nonzero counts (92b24c6c/233988 → f8a825bf/253666 →
5325ff98/259644 → a493bd5e/264958), and only **1 FS read** during the whole
movie phase (offset 0x74406520, 64 KB) — each cycle consumes NEW
RAM-resident clip data. No restart of content was observed. New signal:
frames-per-batch shrinks (4→2→1→1) while batch gaps stay ~130-155 s, i.e.
per-frame decode cost grows with clip position (candidate: re-decode from a
fixed start/keyframe; needs decoder-position instrumentation to confirm).

### Reviewer round-3 item 1 (decisive edge test): t6 ran — no skip

A pressed at t=70 (AFTER logo-phase polling ends at t≈34, BEFORE the first
movie-phase poll at t≈135) and held through movie polls at t≈135/280 with
fresh 0→1 edge in the verified HID ring. Result: playback continued
normally, state machine kept cycling 5→3→2→1→5, **no skip, no title**.
Combined with t5 (level A=1 read at every movie wake, no effect): input
published and consumed by the guest HID-OS filter, but no skip/confirm
reaction fires during the movie state in our runtime. The consumer reads
the filter's *output buffer* (not sharedmem) so DQ8_TRACE_HID_READS cannot
see it; its call site is via vtable/BLX (no direct B/BL, no literal ref to
0x00143580) and was not statically resolved this attempt.

### Honest status

Acceptance NOT met: no "Press any button." screenshot. Menu arrives only
when the clip really ends (operator: it comes automatically); at ~1 frame
per 130-155 s with growing per-frame cost the clip needs hours. Two bounded
levers remain: (1) interpreter dispatch overhead (~30x upside; would let
the clip finish in a bounded run), (2) skip-consumer resolution (would make
A-skip work as requirement 2 asks). Audio-task cross-finding on arbiter
0x0060E498 is theirs; nothing in my runs reaches the last-clip state.

## Attempt 6 (this run): root-cause of the ~5000x slowdown FIXED (block-chaining); movie now renders; new precise blocker = unimplemented FS IPC

### Root cause found and fixed
The ~350 ns/guest-insn (~5000x) pathology was the SPLIT dispatch, not codegen.
The single-TU interpreter (`target/dq8-startup.c`) dispatches by re-running one
giant `switch (pc >> 12) { ... }` inside `translated_from`'s while-loop (no call
per instruction).  `tools/split_dispatch.py` (needed because clang OOMs on the
168 MB single TU) split that switch into per-band functions that each executed
EXACTLY ONE guest instruction then returned — so every guest instruction cost a
cross-function call + 7 pointer write-backs + return + main-loop table lookup +
i-cache thrash across a ~500 MB code image.  That is why `-O2` only gave
+15-29%: it is dispatch structure, not host-code quality.

**Fix (self-contained in `tools/split_dispatch.py`):** each part function now
BLOCK-CHAINS — a `for(;;)` loop dispatches up to `CHAIN_BATCH=256` consecutive
in-band guest instructions through the hot in-band switch, returning to main
only when (a) the pc leaves the band, (b) a blocking SVC clears `running`
(`thread_yield_requested` path `src/main.rs:2803`, so yield/block is preserved),
or (c) the batch budget expires (so main keeps its round-robin / host
housekeeping cadence).  The main loop and runtime are UNCHANGED.

### Measured (run chain1/chain2/chain3, headless, prepared-save copies, -O0, 0 escapes)
* **~37x faster frame decode:** 16 Y2R conversions in ~60 s vs baseline ~9 in
  540 s.  The intro cinematic now RENDERS real video content (chain2 `t_final`
  = a bird soaring over the DQ8 landscape — vision-verified, NOT black; prior
  builds showed only black/logo frames).
* Build cycle for a new verified root is ~13 s (recompile-arm + split + 1-band
  recompile via the content-hash .o cache).
* Added verified root `0x003AF418` (genuine ARM `stmdb sp!,{r4,r5,r6,lr}`
  prologue) — a memclr/init helper reached from Thumb `0x00100C8C` only once the
  guest ran fast enough; it escaped chain1 (rc=3) and is fixed in chain2/3.

### NEW precise blocker (the next bounded step)
Once the movie plays fast, the guest reaches, at ~t=60 s, an **unimplemented FS
IPC** and the runtime aborts with `exit(10)`:
```
unimplemented 3DS SVC SendSyncRequest: handle=0x00000026 command=0x080C0000
  ipc=1FF82080 words=00000000/00000000/00002580/FFFF0A80/00000000/FFFF8001
```
This is the generic fallback at `src/main.rs:6629`.  Command id `0x080C` is the
FS `OpenArchive` family (the session OpenArchive `0x080C00C2` IS handled on
handle 8 in `src/native_fs.c:110`, but `0x080C0000` on handle `0x26` is not —
handle 0x26 is below the native-archive base `native_next_archive=0x40`, so it
is a distinct FS session/archive handle).  This exit fires before the movie can
finish (menu arrives only after movie-end, operator-confirmed) and before an
A-skip can be observed, so it is THE gate now.  Fix = identify what handle 0x26
/ command 0x080C0000 is and implement it in `native_fs_dispatch` correctly (not
faked).

### Status
Goal still NOT reached (no "Press any button." screenshot) — but the two hard
 blockers are gone: (1) the ~5000x dispatch slowdown is fixed (~37x measured,
 verified), (2) the missing decoder/init roots are added as they surface.  The
remaining gate is a single unimplemented FS IPC (`exit(10)`, above), a bounded,
well-localized follow-up.

## Attempt 7 (this run): FS IPC gates cleared + ~20 teardown roots; movie plays fully, zero escapes, now parked on a post-movie white screen

### FS IPC gates (real bugs, fixed)
The `exit(10)` at `handle=0x26 command=0x080C0000` was **FSFILE::OpenLinkFile**
(azahar `file.cpp:52/385`: `{0x080C, &File::OpenLinkFile}` — a stub returning a
new client session to the same backend, offset reset). Handle `0x26` is a
*synthetic RomFS* file (reads served by `romfs_image` at `main.rs:5673`, not
`native_files[]`), so the handler went into `main.rs`'s synthetic path (a
native-file variant + shared-safe `fclose` also added in `native_fs.c` so an
original and its link alias can be closed without double-free). Next gate
`handle=0x38 command=0x080A0040` = **FSFILE::SetPriority** (0x080A); handled
SetPriority + GetPriority (`0x080B`) with azahar-matching response counts
0x40/0x80.

### Roots added (all verified genuine ARM entries via `tools/dq8_disasm_at.py`)
`0x004C9D3C` (leaf getter), the **movie-teardown vtable thunk/method cluster**
`0x00225A88–0x00225D90` (escapes `0x00225B50`/`0x00225BF8`/`0x00225AA8`/`0x00225D90`,
dispatched indirectly from the destructor walker `0x00225C0C` via vtables →
untraceable by the static recompiler), scene-transition methods
`0x00509564`/`0x00509574`/`0x005095F8`, object helpers `0x002FC87C`/`0x002FC8A8`,
empty stub `0x00126B24`.

### Measured (runs link1–link13, headless, prepared-save copies, `DQ8_TRACE_Y2R=1`)
* **Movie now DECODES TO COMPLETION: 184 Y2R conversions, real frames**
  (bird-over-landscape), vs attempt-6 where it aborted at movie setup.
* Every intermediate run (link3–link11) surfaced exactly one more teardown /
  transition escape root; each was verified and rooted (bounded whack-a-mole).
* **link12: ZERO escapes/unimplemented**, ran to 305 s+ (184 Y2R, no crash) —
  the guest reaches a **stable parked state**: top screen solid white
  (post-movie fade), bottom black, all guest threads idle (`thread[0]` blocked
  on event `0x2`, others at `pc=0012C3C4`). Far past attempt-6's `exit(10)` at
  t=60 s.

### Remaining gate (the next bounded step)
The title does **not** render after the movie: the runtime parks on a white
screen instead of drawing "Press any button." Either (a) the title state waits
on an event/timer/GSP-VBlank signal that this runtime never fires, or (b) the
title-render path needs further roots/PICA work. link13 probes whether an A
press at t≈296–320 advances it. **Not yet reached: menu screenshot (req 5).**

## Attempt 8 (this run): A-skip escapes cleared (0 escapes, 360 s), SDL gates restored, and the post-skip black screen root-caused to a startup-object restart loop

### Reviewer round-5 item 1 — DONE: the A-skip path now runs with ZERO escapes
* `0x001C0AF0` rooted (escape `after 00472828`, run rev1). Disasm-verified
  genuine ARM leaf entry: `ldr r1,[r0,#0x48]; cmp 3; moveq r1,4; str; bx lr`
  — the obj+0x48 state setter 3→4 on the movie-stop path.
* `0x003A6690` rooted (escape `after 00100C8D`, run r6 t≈334). Disasm-verified
  VFP helper leaf (VLDR/VSTR pair + bx lr) called from the Thumb dispatcher
  `0x00100C8C`.
* **r7 (360 s, same A-press specs as rev1, DQ8_TRACE_Y2R + state trace):
  rc=143 bounded, 0 escapes/unsupported, 16 HID edges** — the skip path no
  longer dies ~2 s after the A press. (r6, before the second root, died at
  t=334 with 1 escape; r7 after it ran clean.)

### Operator item — DONE: SDL gate restored, windowed run verified
`tools/build_split.sh` lost its SDL/GL gates in the 91755e0 merge from main.
Restored the audio-branch pattern (`cflags`/`ldflags` arrays, `-DDQ8_ENABLE_SDL
$(sdl2-config --cflags)` + `$(sdl2-config --libs)` behind `DQ8_NO_SDL`). GL is
deliberately NOT enabled here: `src/pica_gl.c`/`-DDQ8_ENABLE_GL` exist only on
the Stage-2 renderer worktree and would not link on this branch (noted in the
script comment). Verified: `ldd target/dq8-startup-split` → libSDL2-2.0.so.0;
windowed run **win1** under Xvfb with `DQ8_RUN_GUEST_THREADS=1` opened an SDL
800x960 window and ran 150 s with 0 escapes. Fresh binary staged for the
operator: `target/dq8-startup-split` (+ `.xz`, 17.6 MB).

### The black screen is NOT missing draws — it is a startup-object restart loop
* r8 (240 s, `DQ8_TRACE_GPU=1` + `DQ8_DUMP_PICA_ALL`): even in the black phase
  the guest submits **22–25 PICA draws + 8–10 display transfers per 5 s**
  (RT addr reg 0x11D = 0x0300E100 ×8 → 0x1F070800 = exactly the transferred
  top-screen buffer), yet every transferred frame is black.
* The main thread parks in `svc ArbitrateAddress(handle=0x20000,
  addr=0x005CCCA4, type=1, value=0)` — it waits for the **startup-object
  state word** to become ≥ 0. `DQ8_TRACE_WRITE=0x005CCCA4:0x4` (r9 skip path,
  r10 full-movie path) shows the word **oscillating 0 ↔ 0xFFFFFFFF forever in
  BOTH paths** (1024-hit trace cap exhausted in both): a worker
  (lr=0x0014D7A8, thread stack 0x005CDC58) writes 0 = "startup done", and the
  main thread itself immediately re-claims it via STREX 0→−1 in the state-0
  path of `0x0014C844` (disassembled: state −1/other → arbiter-wait at
  0x0014C8DC, state 2 → wait at 0x0014C880, state 0 → ldrex/strex claim at
  0x0014C8A8 then return; the caller loops back to 0x0014C854).
* So the intro/clip scene **restarts indefinitely**: the title scene is never
  kept. This reproduces with A-skip AND with the full movie (r10, 400 s:
  movie decodes to black at t≈360, state still oscillating at run end) — it
  is NOT caused by the skip.
* New bounded analysis helpers (all in tools/, env-free, read-only):
  `frame_stats.py`, `ipc_profile.py`, `gpu_profile.py`, `write_timeline.py`,
  `pica_regs_extract.py`.

### Next bounded step
Discriminate "title state reached but overlay not drawn" vs "intro scene
still looping" by pad-poll density in the black phase
(`DQ8_TRACE_HID_READS=1`, run r12): a continuous 60 Hz-style poll loop would
prove the input-wait state is active and only the overlay draw is missing;
sparse reads confirm the scene loop never exits, in which case the next probe
is the scene manager's restart trigger (the clip-controller restart
`0x0024F618` tail-called from `0x0023B4D4`, attempt-6 finding). **Not yet
reached: menu screenshot (req 5).**

### r12 result (the discriminator, 240 s, A-skip, DQ8_TRACE_HID_READS=1)
Pad-poll density is **~1 read/s (same PC 0x0014357C, HID page off=0x010),
identical in the pre-skip (63 reads/<60 s), fade (82 reads/60–150 s) and
black (95 reads/>150 s) phases**. A "Press any button." input-wait state
would poll at frame rate; this steady ~1/s cadence is the clip-loop polling
pattern. **Verdict: the intro/clip scene never exits — the title scene is
never entered** (with A-skip and with the full movie alike), consistent with
the 0↔−1 startup-object oscillation above. The next probe is therefore the
scene restart trigger itself (clip-controller restart `0x0024F618`,
tail-called from `0x0023B4D4` — attempt-6), not the renderer.
