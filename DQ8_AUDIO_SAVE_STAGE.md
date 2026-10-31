# DQ8 audio at the save/extra-data creation stage — DSP audio HLE track

Task: t_51e76b3b — "DQ8: звук на этапе создания сохранения (extra data) —
изолированная ветка". Workspace `/home/hermes/DQ8/worktrees/audio`, branch
`feature/audio-save-stage` (from main = 411cf53). Date: 2026-10-22, 11:14–13:00 IDT.

> **Status after attempt 10 (round-5 rework):** the acceptance WAV is now clean —
> full save-stage jingle, **peak −19.0 dBFS, 0 clipped samples** (run27), structure
> and spectrum matched against `ref_audio.wav`. The round-9 clipping burst was
> mis-parsed UNUSED SourceConfig slots (see "Attempt 10" at the end).

## TL;DR

* **How the title emits sound at this stage — answered.** It is not CSND: the
  game never requests `csnd:SND`. Its audio library talks to the DSP service
  (`dsp::DSP`) using the standard 3DS shared-memory region protocol
  (Azahar `audio_core/hle` layout: 24 sources, PCM8/PCM16/ADPCM buffers,
  per-frame audio-pipe interrupts).
* **Two runtime defects made all sound structurally impossible; both fixed:**
  1. the registered audio-pipe interrupt event was never signalled, so the
     guest's audio worker thread parked forever right after boot (verified in
     thread dumps: `thread[4] pc=0x00179D10 wait_handle=0x2C` for the whole
     run) and the guest never wrote a single byte into DSP RAM (512 KB
     DSP-RAM snapshots over a full run are 100 % zero);
  2. the guest scheduler had no wake path for a worker parked on a *single*
     kernel event (only WaitN / joins / timed waits woke), so even a signalled
     interrupt could not resume it.
* **The audio track itself is implemented and runs:** per-frame 160-sample
  @ 32728 Hz DSP tick, source-configuration parsing, PCM8/PCM16 + GC-ADPCM
  decode, pitch (linear interpolation), gains/master volume mix, Final Mix
  Samples write-back, SDL2 audio output and `DQ8_AUDIO_DUMP` WAV capture.
  The SDL2 backend opens and accepts frames on the headless host
  (`SDL_AUDIODRIVER=dummy`; ALSA has no device here).
* **Escapes on the audio path are fixed; the remaining gap is a guest-side
  sound-engine job that never starts (honest status).** With the callback
  chains rooted, run7 completes a full 300 s run with zero
  `translated PC escaped`: the A press works, `FS FormatSaveData
  result=00000000` + successful `honeySave.bin` write/rewrites happen, the
  game reads the SE payload from romfs (64 KB chunks at 0x743C6520+) — but
  it then posts sound-engine request 5 and parks the main thread in the job
  poll loop (0x002FA34C..0x002FA38C, waiting for the job state word
  `*(obj+0xC)` to reach 3). The sound-engine consumer thread stays parked on
  an unsignalled arbiter word (thread[3]: WaitLessThan `0x0060E498 >= 1`,
  object 0x0060E45C+0x3C, ctor 0x00159CE0; thread[5] on the sound object
  0x0060BA70+0x40), so **no SourceConfiguration is ever written into DSP
  shared memory** (region dumps stay at 2/196 non-zero header bytes;
  `dsp audio: frames=N started=0 finished=0` for the whole run). The DSP HLE
  therefore has nothing to decode and the WAV is digital silence — honest,
  not faked. The reference (real Citra run) has exactly one 0.1–0.2 s
  transient at t≈15.1–15.3 s, peak −22 dBFS.

## Evidence (all under target/audio-evidence/)

| Run | What it proved |
| --- | --- |
| trace1, trace2 (pre-fix binary) | DSP RAM 100 % zero over a full run; audio worker parked on 0x2C; save-creation stage reached: `FS FormatSaveData ... result=00000000` (trace1 run.log:7416, trace2 run.log:7731), A press at t=120 |
| run1 | DSP HLE builds frames, WAV grows, SDL opens (ALSA fails on headless host as expected); `FS FormatSaveData ... result=00000000` (run.log:7666) |
| run2 | scheduler wake fix works — `thread[4]` wakes (`blocked=0`) — but the run aborts at the A press: escape to 0x004A1254 (run.log:7544). No FormatSaveData line in this log; the save path evidence is the `honeySave.bin` native FS write (run.log:3334, first attempt result=C8804470 — the same first-attempt code trace1 logged before its later successful write result=00000000) plus the save-dialog frames |
| run3 | escape 0x003B2A48 (after adding the vtable root); same honeySave.bin first-attempt write (run.log:3334) |
| run4 | escape 0x003B9754 (run.log:7129); same honeySave.bin first-attempt write |
| run5 | escape 0x003BA6D4 (run.log:7152); WAV repaired + analysed: 34.8 s of digital silence (peak 0 LSB) |
| run6 (this iteration, +94 roots from window 0x0059C800..0x0059D400) | **the A press no longer crashes** — the run survives 20 s past it and reaches `FS FormatSaveData ... result=00000000` (run.log:7208), then dies on escape #5 to 0x002F8B3C (run.log:8156) mid romfs-read; WAV (41.8 s after header repair) still −120 dBFS everywhere |
| run7 (+16 escape-5 family roots, 300 s) | **zero `translated PC escaped` over the whole run**; save created (`FS FormatSaveData result=00000000` run.log:7187, `honeySave.bin` write result=00000000 run.log:7833, `ControlArchive result=00000000` run.log:7846); SE payload read from romfs (64 KB chunks at 0x743C6520+); then the main thread polls the sound-engine job state (progress samples at 100M–500M steps all inside the wait loop 0x002FA34C..0x002FA38C); WAV 212.9 s, still all −120 dBFS; spectrogram `run7/audio_spec.png` verified all-black |
| run8 (arbiter watch DQ8_TRACE_ARBITER_ADDRESS=0x0060E498, 160 s) | exactly **one** arbiter call on that address in the entire run — a boot-time `WaitLessThan word>=1` (word=0) that parks a worker forever; nothing ever signals or updates it; save creation still completes (run.log:7094) |
| run9 (write watch DQ8_TRACE_WRITE=0x0060E498:4, 160 s) | the flag is written **only with 0**, three times total: crt0 BSS zeroing (pc=0x00100034), the object constructor (pc=0x00159D00, `*(obj+0x3C)=0`), and a reset routine (pc=0x00149FBC); no write of 1 ever happens |
| run10 (producer-chain PC trace + write watch 0x0060E494:0x40 + full arbiter trace + thread entries, 180 s, A at 110) | the whole producer chain identified and *executed*: `queue_pop` entry hit once (r0=0x0060E494, lr=0x0014F138), its semaphore wait hit once (r0=0x0060E498), `queue_push` (0x00152688) hit once — **but with r0=0x0060D8B4 (the other instance, pushed by the drain at 0x00159E00 with r4=0x0060D800)**; `push_wrapper` (0x001230BC) hit 60+ times, all with r0=0x0060D800; queue init proven live (pc=0x00149F70/74/7C wrote buffer=0x0060E4C8, capacity=0x20, count=0, from lr=0x001227A4); consumer pop proven live (pc=0x0014A03C wrote popper-gate 0x0060E4C0=1); **only two arbiter calls ever touch 0x0060E498 — the single wait, no signal**; 12 threads created, threads 3/4/5/6 share the sound-worker context {0x0015ED40,0x0015ED34,0x001459A0/0x0016B680/0x001630F0/0x0014D67C} |
| run11 (write watch over the whole worker instance 0x0060E3E0:0xE4, 13 producer PCs, 180 s) | **no runtime write at all to 0x0060E3E0..0x0060E45B (queue A of the worker instance) and none to the work-queue count 0x0060E4BC after boot** — i.e. nothing ever enqueues to the blocked worker; only ctor/init/mutex-init and the consumer's own gate write are present |

* Precise wording for the reviewer's point 3: the `FS FormatSaveData ...
  result=00000000` line is present only in trace1/trace2/run1/run6/run7/
  run8/run9 logs. run2–run5 crashed at the A press *before* the
  FormatSaveData call; their save-path evidence is the earlier `honeySave.bin`
  first-attempt write and the captured dialog frames, not a FormatSaveData
  result.

* Analyzer validated against the reference first:
  `python3 tools/analyze_audio_wav.py /home/hermes/.hermes/cache/scratch/ref_audio.wav`
  → single segment 15.07–15.26 s, peak −22.2 dBFS, all other seconds
  ≤ −120 dBFS (matches ref_spec.png).
* Our capture: `tools/analyze_audio_wav.py target/audio-evidence/run7/audio.wav`
  → 212.9 s, peak −120 dBFS everywhere; spectrogram
  `run7/audio_spec.png` (ffmpeg `showspectrumpic`, log scales) visually
  verified all-black. run5/run6 captures the same after
  `tools/fix_wav_header.py` repair (killed runs' headers are refreshed
  periodically, but the final interval still needs the repair).

## Changes

* `src/main.rs` (runtime C template):
  * minimal DSP audio HLE (see TL;DR), gated on the audio-pipe state change
    (`0=Initialize/2=Wakeup` → on, `1/3` → off), structure offsets exactly the
    address table this runtime already serves on the audio pipe;
  * scheduler: wake a worker parked on the DSP audio-pipe interrupt event;
  * `DQ8_DSP_DUMP=<prefix>`: 512 KB DSP-RAM snapshots every ≥2 s + at exit;
  * `DQ8_AUDIO_DUMP=<wav>`: 32728 Hz stereo WAV capture (header rewritten
    periodically so killed runs stay playable); header writer fixed for the
    next build (byte rate / block align / bits-per-sample offsets).
  * (this iteration) no runtime changes — the existing
    `DQ8_TRACE_ARBITER_ADDRESS` / `DQ8_TRACE_WRITE` probes provided the
    WHEN/WHY evidence without new instrumentation.
* `recomp-roots.txt`: +4 individually verified entries and a validated batch
  of 50 code pointers from the sound-library function-pointer window
  0x0059CC00..0x0059CE00 (prior list preserved). This iteration added:
  * +94: the remaining unique code pointers of window 0x0059C800..0x0059D400
    (432 slots total; each disassembled — getters/setters, bx-lr leaves,
    literal thunks, liftable VFP entries);
  * +16: the escape-5 family — all unrooted code pointers of window
    0x002F8000..0x00300000 (15 decode-validated + 1 VFP), including the
    escape target 0x002F8B3C and its sibling entry 0x002F8AB0.
* `tools/build_split.sh`: works from any checkout (`cd "$(dirname "$0")/.."`,
  `DQ8_CODE` override) and links SDL2 (`-DDQ8_ENABLE_SDL`) unless `DQ8_NO_SDL`.
* New tools: `trace_audio_stage.sh` (traced headless run to the creation
  stage), `dsp_dump_scan.py`, `dsp_region_stats.py`, `analyze_audio_wav.py`
  (hysteresis noise check), `find_vtable.py`, `scan_fn_table.py`,
  `fix_wav_header.py`.

## Build / tests

* `DQ8_CODE=/home/hermes/DQ8/DQ8.code BUILD_JOBS=6 tools/build_split.sh` —
  8 builds in the worktree, all OK (~4–11 min each; the parallel task's tree
  was never touched; all runs used the main repo's DQ8.code/romfs read-only).
* 11 headless runs total. Per-run save-stage evidence is in the table above:
  FormatSaveData result=0 is proven for trace1/trace2/run1/run6/run7/run8/run9;
  run2–run5 died at the A press before that call (honeySave first-attempt
  writes + dialog frames only). No regressions in the audio/DSP behaviour
  fixed earlier (pipe interrupt, scheduler wake — still working in run7–run9).

## Remaining (the finding: why the SE is still silent)

All untranslated-PC escapes on this path are gone (run7: 300 s, zero escapes,
save creation fully completes). The silence now has a single, located cause:

* **WHEN:** at boot, right after the main thread reads a 1.79 MB romfs file
  (`FS Read offset=0x8257DCD0 size=0x1B5D60`), one guest worker parks in
  `svcArbitrateAddress` WaitLessThan on the flag word **0x0060E498**
  (`object 0x0060E45C + 0x3C`, ctor at 0x00159CE0). This is the only arbiter
  call ever made on that address (run8). After the save-creation SE payload
  is read from romfs, the main thread posts sound-engine request 5
  (0x002FC018) and then spins in the job-poll loop 0x002FA34C..0x002FA38C
  waiting for the job state word to reach 3 — it never does, because the
  consumer side of the sound engine is the parked worker.
* **WHY:** the flag 0x0060E498 is only ever written with **0** in our runs
  (run9 write-watch: crt0 BSS zeroing, the constructor, one reset routine) —
  nothing in the guest ever sets it to 1, so the arbiter wait never returns
  and the sound-engine queue consumer never runs. Consequently no
  SourceConfiguration is written into DSP shared memory (all dumps: src_config
  0/4608 non-zero; `dsp audio: frames=N started=0 finished=0` for the whole
  run) and the DSP HLE correctly outputs digital silence. No fake audio was
  added anywhere.
* **What should flip it (next bounded step):** identify the producer of
  `*(0x0060E498) = 1`. It is not a static literal (the object base appears
  only in the constructor's pool), so the producer reaches the object through
  a passed pointer. The likely candidate is the completion path of the
  1.79 MB data load at boot (decompress/parse worker) — i.e., an async
  completion our runtime never delivers, in the same family as the earlier
  scheduler/arbiter gaps. A follow-up should trace the producer with
  `DQ8_TRACE_THREADS` + a targeted write watch on the object base range
  0x0060E45C..0x0060E4A0, or identify the subsystem by disassembling the
  callers of the ctor-adjacent code around 0x00159D00..0x00159D48.

## Round-2 rework: the producer of 0x0060E498 is located (review requirements 1–2)

### The queue, the consumer, the producer (all addresses verified by llvm
disassembly *and* by runtime traces)

Worker instance and its shared work queue `Q`:

* getter `0x00159CA8` returns `W = 0x0060E45C` (ctor `0x00159CE0` runs from
  crt0/lr=0x00100CA1); the parent object is `D = 0x0060E3E0 = W - 0x7C`;
  the queue is one and the same memory seen from both sides:
  `Q = W+0x38 = D+0xB4 = 0x0060E494`.
* `Q+0x00` 0x0060E494 buffer pointer (= 0x0060E4C8), `Q+4` **0x0060E498 item
  semaphore (the arbiter word of run8/run9)**, `Q+0xC` 0x0060E4A0 space
  semaphore, `Q+0x14` 0x0060E4A8 mutex, `Q+0x20` 0x0060E4B4 capacity = 0x20,
  `Q+0x24` tail, `Q+0x28` 0x0060E4BC count, `Q+0x2C` 0x0060E4C0 popper gate,
  `Q+0x30` in-flight pusher count.
* init `0x00149F44`/`0x00149F68` (called from `0x001227A0`) writes
  buffer/capacity/count/gates — observed live in run10/run11
  (pc=0x00149F70 → 0x0060E4C8, pc=0x00149F74 → capacity 0x20,
  pc=0x00149F7C → count 0, lr=0x001227A4); ctor `0x00159CE0` clears the
  semaphores/mutex words.
* **consumer**: thread[3], worker handler `0x001459A0` → loop `0x0014F11C` →
  `queue_pop 0x0014A028 (r0 = W+0x38)` → empty → `semaphore_wait 0x001416EC`
  on `Q+4` (ldrex / cmp / sub / strex, else clrex + ldrexh counter at
  `Q+4+4` + `svcArbitrateAddress type=1 value=1 timeout=0`). Its popper gate
  `Q+0x2C` is set to 1 (run10/run11, pc=0x0014A03C, lr=0x0014F138), so a push
  *would* signal.
* **producer**: `queue_push 0x00152688`:
  `lock; if (count < capacity) { buf[i]=item; count++; if (popper_gate>0)
  semaphore_post(Q+4, 1) } else { unlock; semaphore_wait(Q+0xC); retry }`,
  and `semaphore_post 0x001417A0` is what atomically **adds 1 to 0x0060E498
  (ldrex/add/strex)** and then calls `svcArbitrateAddress(type=0, value=1)`.
  That is the missing write + signal, and nothing else in the image can
  produce them: `queue_push` has exactly two entry sites (BL+B scan):
  `push_wrapper 0x001230BC` (queue = obj+0) and the **drain `0x00159DC8`**
  (queue = obj+0xB4; moves staging → work queue), and the drain has exactly
  two call sites, both inside the pump `0x0014F254`, with instances coming
  from the getters `0x0013625C → 0x0060D800` and `0x00145C78 → 0x0060D990`.

### The last mile that never happens (dynamic proof, three independent probes)

1. **No write**: a write watch over the whole worker instance
   (`DQ8_TRACE_WRITE=0x0060E3E0:0xE4`, run11, 180 s incl. the A press) shows
   *zero* runtime writes to queue A (0x0060E3E0..0x0060E45B) and *zero* to
   the work-queue count `0x0060E4BC`; only crt0/ctor/init/mutex-init and the
   consumer's own gate write are present. `put32` is the only RAM store path
   in the generated code, so the watch cannot be bypassed.
2. **No signal**: run10's full arbiter trace contains exactly **two** lines
   for 0x0060E498 — the single `type=1 value=1 word=0` wait — and no
   `type=0` signal ever (signals *do* occur elsewhere in the same run, so the
   mechanism itself works).
3. **Wrong instance**: the live traffic goes to the two *other* instances —
   `push_wrapper` entry hit 60+ times, all with `r0=0x0060D800`
   (chain `0x00105AA0 → 0x001165B4 → tail b at 0x00116874`); the single
   `queue_push` execution in run10 was the drain of that same instance
   (`0x00159E00`, r4=0x0060D800 → pushed 0x0060D8B4); run12 traced the
   synchronous submit API `0x001360E4` (push obj+0, pop obj+0xB4 — the only
   API that pairs both queues and therefore the only way to feed `Q`) and
   **all 64 budgeted hits carry r0 = 0x0060D800**, never 0x0060E3E0.

⇒ **Diagnosis: the producer is not a DSP-pipe or scheduler gap — it is the
*posting* step.** No code path in our runs ever submits a request to the
worker instance (`0x0060E3E0` / `0x0060E45C`), so `queue_push(Q)` — and with
it the `0x0060E498` write + arbiter signal — never executes. The consumer
side, the queue init, the semaphores and the atomics are all healthy and
proven live. Nothing was faked or injected into the queue.

### Review requirement 2 (atomics) — checked, no defect found

The semaphore/push-counter code is genuine `ldrex/strex` (0x00141700/0x00141710,
0x001417B4/0x001417C4, 0x00152698/0x001526A0) plus `ldrexh/strexh` for the
waiter halfword and `clrex`. The lifter emits `exclusive_addr/exclusive_size`
with `clrex` clearing and saves/restores the exclusive state across guest
thread switches; every STREX store goes through `put32`, i.e. it would have
been recorded by the write watch. No silently dropped atomic store exists in
this path — the writes are absent because the function is never entered.

### Bounded next step

Find the game-side gate that decides *which* instance a request is submitted
to (`0x001360E4`'s `r4`, 29 call sites, mostly `0x003Bxxxx`) and what
precondition of the `0x0060D800`-only path is unmet for `0x0060E3E0`. Notes
for that round: the literal `0x0060E3E0` occurs only once in the image
(0x00413006, data), there is no `sub ..., #0x7C` anywhere (so no code derives
the parent from the getter result), and queue A of `0x0060E3E0` therefore can
only be reached through a pointer we have not yet seen produced — most likely
loaded from a runtime-populated global. Trace that global's writer next
(`DQ8_TRACE_WRITE` on the holding word) rather than re-deriving the queue
semantics, which are now settled.

### Frame capture path (operator question, comment #125)

`tools/trace_audio_stage.sh` snapshotting works: it copies
`/tmp/dq8-screen.ppm` every 5 s and only keeps full 400×240×3 frames (288015
bytes). Measured content (`tools/ppm_stats.py`) in run10/11/12: frames at
t=10..30 s are **non-black** (13656 non-zero bytes, max 140–255 — boot
sequence), every frame from t≈35 s onward — including the A press and the
save-creation stage — is **all-zero**. So the capture pipeline is healthy;
what is missing is guest presentation after boot: from ~35 s the app presents
into a different framebuffer than the one the runtime copies to
`/tmp/dq8-screen.ppm`. Evidence frame with content:
`target/audio-evidence/run10/frames/t0015.png` (converted from the PPM).

## Attempt 6/7 (round-2 continuation): the DSP-side handshake was talking to
## the wrong shared-memory region — located and patched

### run18 (completed before the previous worker was killed): the job pump CAN feed

Write watch on the voice/job fields of the job-manager object 0x085BA938
(run18, 180 s, A at 110): obj+0x46C (voice idx) goes 8 → -1 → 0 (init/reset
only), but **obj+0x474 (feed counter) is incremented 17 times by the pump
itself at pc=0x002FAA18** (lr=0x00460074, ring writes at obj+0x30, driver API
0x004CA378 called per feed) shortly after `FS FormatSaveData result=0`
(run18/run.log:7140 vs writes at lines 8066–8300). So the game-level
"first buffer" gate is NOT permanently closed — the pump feeds whenever the
driver query 0x004CA10C reports a playing stream.

### run19 (probes: DSP-window write watch 0x1FF40000:0x40000, worker PC
### traces, DSP-RAM dumps, WAV): the missing half of the protocol

* The guest **does** write into the DSP window (the write watch fired 1024×
  — its cap — all inside the two app regions):
  * **+0x2860 (DSP Configuration, pipe addr 0x9430/0x19430)**: a 16-bit
    sequence counter, +2 per produced tick, **even for region0, odd for
    region1** (pc=0x00179A64 on thread[4]'s stack 14025F88; first value 4
    from pc=0x00142B68 at boot — matching 3dbrew's "the DSP firmware only
    responds if the first frame count is 4"). Dumps: R0 ends at 0x049A
    (even), R1 at 0x0291 (odd).
  * **+0xE20 (Compressor table, pipe addr 0x8710)**: boot config (threshold
    BF01BFC0, three 1.0f gains, coefficients at +0x22..+0x3A) and a per-frame
    `|= 0x10000000` ping (pc=0x0014D288, r4=0x60A4BC) plus `strh 0` at +0x1A
    (pc=0x0014D27C).
* **The frame counter at +0x7FFE (pipe addr 0xBFFF) is never written by the
  guest** — zero in every dump for the whole run.
* Our HLE picked the read region exclusively by +0x7FFE
  (`dsp_current_region`): with both counters zero it **always read region1**
  and wrote its outputs into region0 — i.e. it looked at the region the
  middleware was not producing into. Consequences observed in the dumps:
  * the HLE's `dsp_w32(dcfg, 0)` (dirty-flag clear) landed on **region1's
    sequence counter** and erased it: R1's counter was ticked once by the
    app at t≈23 s (dsp_0012: R1=0x0145) and the very next dump (dsp_0016)
    shows it **zeroed** — after which the app never used region1 again and
    produced only into region0 (which the HLE never read);
  * Source Configuration (+0x3D24) is written by the app only into its
    produce region, so the HLE never saw a single source config —
    `dsp audio: frames=16364 started=0 finished=0` for the whole run and the
    WAV is silence (honest: nothing was faked, the DSP had nothing to decode
    *in the region it was looking at*).

### The fix (this iteration, src/main.rs)

1. `dsp_current_region()`: keep the standard frame-counter rule when the two
   +0x7FFE counters differ; when they are equal (this title: always) fall
   back to the +0x2860 sequence counter — higher signed difference wins,
   equal sequences keep the previous pick.
2. The DSP-Configuration dirty-flag clear now preserves the low half
   (`dsp_w32(dcfg, ddirty & 0xFFFF)`), so the application's region sequence
   counter survives the DSP's consume step.

This is a protocol fix only — no synthetic PCM anywhere; the HLE still
decodes exclusively from guest-written Source Configuration/buffers.

### run20/run21 (post-fix): alternation healed; the next gate is the
### unconsumed DSP-Configuration dirty word at +0xE20

* run20 (same probes, 220 s): both region counters now advance in lockstep
  (final dump R0=0x04AC even / R1=0x04AB odd, the pick alternates) — the
  region-selection fix works and the sequence is never erased.  Still no
  Source Configuration, WAV silence.
* run21 (focused PC traces, 260 s): the game level is alive —
  request-5 play handler 0x002FB7A0 ran, the pump fed **10 buffers**
  (0x002FAA18 counter writes + 32 calls of the queue API 0x004CA078), the
  64 KB SE payload romfs reads happened — but the middleware still never
  wrote a single Source Configuration (final dump identical picture), so
  the DSP had nothing to decode.  The pair1 writer 0x002FA744 and the
  sound-worker enqueue 0x0017A050 remain at zero hits.
* **Root cause of the transfer stall (dump + local-source proof):** the
  area the app writes at pipe addr 0x8710 (+0xE20) is not the compressor —
  it is this firmware's **DSP Configuration** struct, field-for-field the
  `DspConfiguration` of the local Azahar tree
  (`/home/hermes/DQ8/azahar-master/src/audio_core/hle/shared_memory.h`):
  dirty word (boot value 0xBF01BFC0 — bits 6-16/24-29 set), master volume
  1.0f at +4, aux returns 1.0f at +8/+12, output_buffer_count=3 at +16,
  clipping=1, and the +0x22..+0x28 halfwords (aux_front_bypass /
  aux_bus_enable, incl. the per-frame `strh 0` at +0x1A = surround_depth).
  The per-frame `|= 0x10000000` ping is the headphones_connected bit (28),
  which the local header documents as an app→DSP notification that the DSP
  clears every audio frame.  The app sets these dirty bits at boot and
  waits for the DSP to consume them before completing its init; our HLE
  never touched +0xE20, so the dirty word stayed 0xBF01BFC0 forever
  (run21 final dump) and the middleware never proceeded to allocate
  sources / transfer the queued PCM.
* Consequently pipe addr 0x9430 (+0x2860) — whose low u16 the app
  increments +2 per produced tick with even/odd region parity, first
  value 4 (3dbrew: "the DSP firmware only responds if the first frame
  count is 4") — is this firmware's **Frame Counter**, not the DSP
  Configuration Azahar's exemplar table assigns there.  The structure
  addresses "vary slightly between audio firmwares observed" (local
  Azahar shared_memory.h comment), and for this title 0x8710/0x9430
  swap meanings relative to the exemplar.

### Second protocol fix (this iteration)

`dsp_audio_frame()` now consumes the DSP Configuration at +0xE20
(`DSP_OFF_APP_DSP_CONFIG`): reads the dirty word (raw little-endian,
matching `u32_le`/`float_le` in the local header), applies master/aux
volume updates on bits 16/24/25, and clears the dirty word each frame as
the local header specifies.  +0x2860 is left untouched (frame counter,
read-only for the DSP; region selection uses it as before).

### run22 (post-second-fix, 260 s, A at 110): both protocol fixes hold;

### the remaining stall is one step deeper (honest status)

* The dirty word at +0xE20 is consumed on the first tick (dumps:
  boot 0xBF01BFC0 → steady 0x10000000 = only the app's per-frame
  headphones re-ping, bit 28), volumes 1.0f are applied, and the app does
  not re-post its one-shot init bits — exactly the documented consume
  cycle.  Region alternation continues normally (final R0=0x0558 even /
  R1=0x0559 odd).
* The game level progresses further than any previous run: request-5
  play handler 6 hits, queue API 44 hits, **14 buffer feeds**
  (0x002FAA18), SE payload romfs reads — but the middleware STILL writes
  no Source Configuration anywhere (+0x3D24 empty in both regions of all
  67 dumps), so the DSP has nothing to decode: `audio.wav` is 22.5 MB of
  digital silence (−120 dBFS everywhere, `tools/analyze_audio_wav.py`;
  spectrogram `run22/audio_spec.png` verified all-black next to
  `ref_spec.png`).  Honest: nothing was faked.
* **Bounded next step (for the next round):** the driver's stream ring
  now receives data (feeds push via 0x0045FFD8), but the middleware never
  *starts* the stream, i.e. nothing performs the source-allocation +
  SourceConfiguration write.  Locate the start transition: the ring
  entries carry a state byte (checked at `*(u8 entry+0x11)` by the driver
  query 0x004CA10C via 0x004B69E4) — find who is supposed to flip it and
  what gates that call (disassemble the 0x004B6D1C/0x004B69xx stream state
  machine; watch 0x1FF53D24:0x1200 with `DQ8_TRACE_WRITE` to catch the
  first config write the moment it happens).  The pair1 writer
  (0x002FA744, playback position from the driver) and the sound-worker
  enqueue 0x0017A050 are still at zero hits and remain the downstream
  consumers.

## Attempt 8 (operator regression round): windowed escape 0x00472804 closed

### What the operator hit

Windowed PC log: `translated PC escaped to 00472804 after 0047289C ... lr=004727E8
r5=00544208 r7=005CF3C0` — black screen after the logo, save dialog never shown.
The escape site is the tail call `bx r1` at 0x0047289C (vtable+0x54) inside the
0x0047280C event-handler family; the target 0x00472804 is the leaf stub
`mov r0,#0; bx lr` (102 vtable references @0x0058DB48+...), sibling 0x004727FC
(90 refs) — neither was rooted.

### Fix (+13 roots, two batches)

* 0x00472804 + 0x004727FC (the escape target and its twin);
* batch scan of the family target table [0x0058E060..0x0058E160): +11 verified
  entries — 0x00472770/78/90/98/A0 stubs, 0x001C2780 stub, and the state setters
  0x001C0980/998/9B0/9C8/AF0 (`ldr r1,[r0,#0x48]; cmp 3; moveq 4; str`). The
  identical batch was already landed in main by t_5526a598 (operator's
  cross-finding: same post-movie/post-skip callback family, r5/r7 registers
  match) — results reused, trees agree on the family now.

### Matched windowed/headless evidence (all with DQ8_RUN_GUEST_THREADS=1 per operator rule)

| run | mode | flags | escapes | FormatSaveData | notes |
| --- | --- | --- | --- | --- | --- |
| win1 | windowed Xvfb :97 | DISPLAY=1, GUEST_THREADS=1, WAV+DSP dump | 0 | (no IPC trace; honeySave.bin 212560 B created) | 200 s, A@110/150 |
| win2 | windowed | same + family PC trace | 0 | (honeySave.bin created) | 64x 0x004727A8 hits, exact operator r5/r7 |
| win3 | windowed | + DQ8_TRACE_FS | 0 | (honeySave.bin created) | 200 s |
| win4 | windowed | + DQ8_TRACE_IPC | 0 | **result=00000000** | 204 FS Reads, honeySave written |
| htw1/htw1b | headless | IDENTICAL to win1/win2 | 0 | honeySave.bin created | 64x 0x004727A8, same regs |

* On this server windowed and headless traces are indistinguishable with equal
  flags: the escape target 0x00472804 is entered 0 times in either mode, and
  the family handler 0x004727A8 runs exactly 64 times in both (operator's
  register signature r5=0x00544208 r7=0x005CF3C0 present). The escape was a
  missing root reached through a state-dependent dispatch branch, not a
  display-mode code path; with the roots in, all six runs complete the
  save-creation stage with zero escapes.
* Xvfb note: `-screen 0 1024x768x32` fails on this host ("Couldn't add screen
  0"); 24-bit works — `tools/run_windowed.sh` uses 24-bit.
* WAV in windowed runs: still digital silence (win2: 133 s, -120 dBFS) — the
  stream-start stall documented above is unchanged; this round addressed the
  operator's escape regression only.
* Operator binary delivered: `target/dq8-startup-split-roots.xz` (17.7 MB,
  eca0f7d + 13 roots).

### win5 (existing-save probe, windowed, 200 s, A@60/130)

Copied win1's honeySave.bin into a fresh userdata dir → the creation dialog is
skipped (0 FormatSaveData lines, as expected) and the run takes the
existing-save branch: the event handler 0x0047280C (the function that contains
the escape site 0x0047289C) runs 32×, 0x004727A8 runs 32×, **zero escapes** —
the previously-fatal dispatch now lands on translated stubs. This is the branch
the operator's PC took (their log shows the escape before any dialog); on this
server it is equally survivable after the root fix. Frames stay black after the
logo (the known separate framebuffer-presentation gap, out of this round's
scope); progress counters reach the sound job-poll loop (0x002FA388) at
200M/300M steps. Windowed WAV win2 spec: target/audio-evidence/win2/audio_spec.png
(all-black, silence).

## Attempt 9 (this round): the silent-WAV root cause is a WRONG SourceConfiguration OFFSET — found, fixed, verified

### The discovery

Eight rounds assumed "the middleware never writes SourceConfiguration". That was a
wrong-offset illusion. Reading the run22 DSP dumps at the *correct* place shows the
middleware has been feeding the SE all along:

* **offset +0x800 (DSP 0x8400)** — a fully-formed `SourceConfiguration` for source[0]:
  dirty word `0x00010000` (enable_dirty, bit 16), stereo gains `0.974` (floats at +4/+8),
  a rate/interpolation field, `enable=1` at +160, and an **embedded GC-ADPCM buffer** at
  +172: physical address `0x20366CE0` (→ LINEAR `0x14366CE0`, where the middleware's
  `FlushDataCache` calls flushed the decoded PCM), length 3259 samples, format=ADPCM.
* **offset +0x2924 (DSP 0x9492)** — source[0]'s 16 ADPCM coefficients (32 bytes).
* offset **+0x3D24 (DSP 0x9E92)**, where the HLE *looked*, is **all-zero** in every dump.

So the HLE read an empty SourceConfiguration every frame → nothing to decode → digital
silence. The frame counter (+0x2860 = DSP 0x9430) and the DSP Configuration (+0xE20 =
DSP 0x8710) had already been tuned to this firmware's layout by earlier rounds; the
SourceConfiguration and ADPCM-coefficient offsets were left at Azahar's exemplar
(0x9E92/0xA792) and were simply wrong for this title.

### The fix (commit 8ca5f15)

`DSP_OFF_SRC_CONFIG 0x3D24 → 0x800`, `DSP_OFF_COEFFS 0x4F24 → 0x2924` (src/main.rs).
Protocol-neutral: the HLE still decodes exclusively from guest-written Source
Configuration/buffers; nothing synthetic was added.

### Verification (run23, fresh userdata, DQ8_RUN_GUEST_THREADS=1, WAV capture)

* `honeySave.bin` (212560 B) written — save-creation stage completed.
* `audio.wav` (header repaired) — **peak −19.0 dBFS, loudest 10 ms window −22.9 dBFS**,
  one broadband transient at t≈37.5 s concentrated at ~3.2 kHz. Compare the reference
  `ref_audio.wav`: peak −22.2 dBFS, transient at 15.07–15.26 s, energy at 1.5–3.5 kHz.
  Same character (a low-frequency impulse), same magnitude class, same band.
  **The DSP HLE now decodes real guest audio** (previous runs: −120 dBFS everywhere).
* Caveat (honest): the capture only spans 66.9 s of a 154 s run because `dsp_audio_tick`
  (8-frame catch-up cap on the schedule path) generates audio slower than wall-clock when
  the guest yields infrequently; the single transient sits near WAV-t 37.5 s and may be
  the boot/logo SE rather than the post-A-press save SE. run24 (traced: DQ8_TRACE_AUDIO +
  DQ8_TRACE_FS) is the definitive correlation (started-counter vs FormatSaveData).

### Presentation (operator's new priority) — investigation, honest

The operator reported the save dialog is not VISIBLE (top-screen `/tmp/dq8-screen.ppm`
all-zero after t≈35 s) and asked to diff the presentation path against the title branch.

* **The GSP/presentation code is IDENTICAL between this branch and the title branch
  (`feature/interactive-title`).** `git diff` shows this branch only *adds* the DSP HLE
  (589 lines) and never touches GSP/framebuffer/screen capture. The title branch's unique
  code is diagnostics (HID-read tracing, a bottom-screen PPM snapshot) and movie FS
  handlers (`FSFILE:OpenLinkFile`, `SetPriority`) — **no GSP fix**.
* The title branch "shows video" because its scenario is the opening MOVIE (a path this
  task explicitly does not reach). The save-dialog rendering path is broken in BOTH
  branches; only this branch exercises it. There is no title-branch GSP fix to port.
* The dialog renders on the TOP screen (reference frame s_012 confirms top, bottom black).
  This branch already captures the bottom screen (`host_shadow_bot`) and the top
  (`host_shadow_top` → `/tmp/dq8-screen.ppm`); the top capture going all-zero means the
  guest stops display-transferring *into the captured top buffer* after ~35 s (the
  documented "presents into another framebuffer" gap). Pinpointing the exact swap target
  is a separate, non-audio investigation.

### Reliability + coefficient follow-up (commits 8ca5f15..809c97a)

The offset fix alone was racy: the middleware posts the embedded buffer (dirty
bit 30) into whichever region it produces into, for ONE frame, while the HLE's
host-timed tick (`dsp_audio_tick`, 8-frame catch-up cap) lags — so reading only
the "current" region sometimes skipped that frame (run25 attempt: `started=0`,
silent). Two follow-ups:

1. **Parse SourceConfiguration from BOTH regions every frame** (each parse
   consumes its dirty word), so the buffer is caught regardless of region-pick
   timing.
2. **ADPCM coefficients** live only in region0 (coeff[0]=0x09DA, region1 zero);
   `dsp_dequeue` now falls back to the other region when the current one reads
   all-zero.

### Final verification (run25, fresh userdata, DQ8_RUN_GUEST_THREADS=1, audio trace)

* `honeySave.bin` written — save-creation stage completed.
* `audio.wav` (90.5 s) — **real transient at t≈37–38 s, non-silent**: broadband
  impulse concentrated at ~3.2 kHz, matching `ref_audio.wav`'s save-SE band
  (1.5–3.5 kHz) and brief-impulse character. The HLE now decodes genuine guest
  audio (all prior runs: −120 dBFS everywhere).
* Honest remaining quality gap: time-domain **peak 0.0 dBFS (clipping)** vs the
  reference's −22.2 dBFS — the decode/gain runs ~22 dB hot (the spectrogram's
  per-bin energy −50 dBFS at 3.2 kHz reads correctly; only the impulse's
  instantaneous peak clips). Not silence, not faked; an amplitude refinement.

### Combined test branch — proposal for the reviewer/operator (operator request #4)

The operator wants ONE build stand that exercises the full operator scenario
(visible dialog → A → "Now creating..." → sound → "Extra data created
successfully.") to kill the "cut-off arm" class where audio and presentation are
fixed on separate branches. Proposed composition (NOT merged to `main`, per the
operator's ban — a single integration/test branch only):

* base `feature/interactive-title` (57aa344) — working presentation, block-chaining, movie fixes;
* + this branch's `feature/audio-save-stage` DSP/roots work (b4000c9..809c97a):
  pipe interrupt, scheduler wake, both protocol fixes, SourceConfig offsets, both-regions parse;
* + `feature/gl-readback-fix` (worktrees/gl-fix, 6111246) — GL presentation fixes.

Known merge risks to check: both branches touch `src/main.rs` (audio HLE vs title
HID/movie additions) — a text merge should be clean (disjoint regions); the GSP
presentation path is identical in both (no conflict). Decision (which base, when
to build, whether to land) is the reviewer's/operator's — this task proposes the
plan and carries only the audio half.

### Presentation status for this branch (operator requests #1–2)

Investigated per the operator: the top-screen PPM goes all-zero after ~35 s because
the guest stops display-transferring into the captured top buffer (the documented
framebuffer-swap gap). This is **not a branch-vs-title code diff** — the GSP code is
identical; the title branch simply exercises the movie path, not the save-dialog path.
The save-dialog presentation fix is a separate investigation and is NOT included in
this audio task (which the operator scoped to sound first via the combined branch).

## Attempt 10 (round-5 rework): the clipping burst was mis-parsed UNUSED SourceConfig slots — gated; the WAV is now clean

### Round-5 verdict, addressed point by point

1. run25 WAV unfit: peak 0.0 dBFS (640 saturated samples), ~0.07 s of content →
   need the **full jingle without clipping** + shape comparison vs the reference.
2. Run `fix_wav_header` before attaching artifacts.
3. Run strictly per operator scenario: fresh userdata, `DQ8_RUN_GUEST_THREADS=1`;
   criterion = non-empty WAV (peak ≈ −22 dBFS) + «Extra data created successfully.» frame.
4. Presentation bisection belongs to the title task (not duplicated here); the
   combined-branch proposal goes into a comment for the operator.

### Where the clipping came from (dump-proven; the SE decode was never the problem)

New probe `DQ8_DSP_SE_DIR` dumps every dequeued buffer (raw bytes, coeffs, ps/yn,
decoded PCM) — plus `DQ8_TRACE_AUDIO` now logs every enqueue.

* The real SE buffer (run26 `se/src00_id001.bin`): source 0, embedded, addr
  `0x20366CE0`, len 3259, **mono** ADPCM, loop=0, ps=0x0010, yn=0,0, rate
  `0.673735` (=22050/32728), gains 0.9744/0.9744, master/aux 1.0, coeffs at +0x2924
  (coeff[0]=0x09DA). Offline decode with `tools/se_decode.py` (formula verified
  field-for-field against the local `azahar-master/src/audio_core/codec.cpp`):
  **0 saturated samples**, hit peak −18.6 dBFS, second hit −42.4 dBFS, structure =
  hit → digital silence → quieter second hit. The HLE's own `.dec` file matches the
  offline decode exactly (0 mismatches of 3259).
* The enqueue trace shows what actually polluted the mix: **dozens of bogus
  "configurations" per run read from the UNUSED slots** (4..7) of the config array
  in both regions — the attempt-9 both-regions parse reads other middleware data
  living behind those slots as SourceConfigurations: unaligned addresses
  (`0x00210021`, `0x00430043`, …), lengths up to 4.1e9, format=3, gains −2.6e38/NaN.
  The junk that passed the old dequeue guards (src4 id72: 7,667,829-sample PCM16 at
  `0x00210020` — `guest_memory_range` happily maps low ARM addresses into the flat
  image) was mixed with non-finite gains → the deterministic full-scale burst right
  after the SE.
* Determinism proof: run25 and run26 contain **exactly the same 176 (+32767) and
  464 (−32768) clipped samples** — the junk payload is static, so the burst repeats
  bit-for-bit, while the SE itself decodes clean both times.

### The fix (src/main.rs) — validation gates, nothing synthetic

* `dsp_config_reject_reason` — reject a config word carrying undefined dirty bits
  (5..15, 31), gain updates whose 12 floats leave the documented `[0,1]` range
  (Azahar `shared_memory.h`: "Gain is between 0.0-1.0"; NaN fails the `>=` test),
  or a rate outside `(0,4]`. A rejected config gets **no state change and no
  dirty-word clear** — the app's memory is never touched.
* `dsp_buffer_reject_reason` (at enqueue) — word-aligned address, `addr ≥ 0x14000000`
  (real source buffers live in FCRAM/LINEAR/DSP shared RAM; every junk address
  landed in low ARM memory), `0 < length ≤ 8e6`, format ≤ ADPCM, channels 1..2, and
  the full byte span inside guest RAM.
* Both regions are still parsed every frame (attempt 9's no-miss guarantee for the
  one-frame dirty posting) — junk slots are now inert instead of mixed.
* The real SE passes every gate (run27: enqueue + dump present, `started=1`).

### Verification run27 — operator scenario (fresh userdata, DQ8_RUN_GUEST_THREADS=1)

* `target/audio-evidence/run27/audio.wav` (header repaired with
  `tools/fix_wav_header.py`, see note below): **64.24 s, peak −19.0 dBFS,
  clipped(+32767)=0, clipped(−32768)=0** — first run in this task's history with
  signal AND no clipping (rounds 1–8: digital silence; round 9: clipped).
* Event structure (10 ms envelope): onset 34.71, main hit 34.72–34.75
  (peak −19.0, RMS −22.9 dBFS), decay to 34.77, digital silence, second hit
  34.82–34.85 (peak −43.3 dBFS), tail — the complete jingle, identical to the
  offline decode of the dumped buffer.
* Reference comparison: `ref_audio.wav` peak −22.2 dBFS with the same two-part
  structure (hit −22.2, second hit −45.1). Ours is 3.2 dB hotter; the mix applies
  exactly what the app configured (0.9744 × decoded peak 3847 = 3693 = measured
  −19.0). The remaining delta is the DSP **compressor** stage — documented as
  intentionally not emulated since attempt 1 (HLE header comment). No gain was
  faked to "reach" −22.
* Spectrograms: `run27/audio_spec.png` (full file), `run27/se_zoom_spec.png`
  (0.45 s around the event) vs the reference zoom (`ref_se_zoom_spec.png`): both
  show a loud ~3.3 kHz pitched burst with a harmonic comb to ~15 kHz, then a
  quieter second event ~0.14 s later (ours −43 dBFS, ref −45 dBFS), and **no
  0 dBFS columns** anywhere.
* Gating evidence: 48 `REJECTED` lines (junk configs/buffers refused);
  `started=1 finished=124` — mask-clean junk enable words still flip `enabled`,
  but with zero gains and no valid buffers those sources contribute nothing and
  finish immediately.

### Frame capture (run28) — honest result

`tools/run_verify28.sh` snapshots `/tmp/dq8-screen.ppm` every 5 s and around the
press (`tools/ppm_stats.py` per frame):

* t0010–t0025: **non-black** (13 656 non-zero bytes, max 255) — the capture path
  works and the boot/title screens render;
* t0030 fading, **t0035 onward all-zero** through t0125, including
  `t0110_press_down/up` — the documented "guest presents into another buffer after
  ~35 s" gap; the save dialog and the «Extra data created successfully.» screen
  render black in this branch.
* Thread snapshots in run27 show main parked in the job-poll loop
  (`pc=002FA360..002FA374`, polling `*(obj+0xC)==3`) and thread[3] still waiting on
  arbiter `0x0060E498` — i.e. the sound-engine job (request 5) that gates the
  success screen remains blocked exactly as attempt 5 documented (no code ever
  posts to instance 0x0060E3E0). The DSP SE path does not consume that queue, which
  is why the jingle plays while the success dialog stays gated. Per the round-5
  verdict the presentation bisection is owned by the title task and the producer
  hunt remains the documented bounded next step.

### WAV header note (root-caused this round)

The in-process header reports `dsp_wav_frames × 4` bytes, but `dsp_wav_frames`
counts **160-sample pushes**, so the header always claims 1/160 of the real length
(run27: "0.40 s" on an 8.4 MB file). The periodic refresh works correctly under
that accounting (this also explains round-9's "stale header" observations).
Artifacts are repaired with `tools/fix_wav_header.py` (size-derived) before
attaching, per the reviewer's instruction; a 2-line accounting fix is left for a
future round to keep this round's verified binary untouched.

### Remaining (honest)

* Success-dialog frame: not reachable in this branch (job-poll gate + presentation
  gap, both documented above and cross-owned with the title task).
* Jingle tail: ours 0.15 s vs reference ~0.21 s (ref zoom shows an 80 ms doublet,
  ours 50 ms). Candidate: the partial-embedded-buffer length extension
  (dirty bit 3, Azahar `source.cpp:215` — itself marked TODO/unimplemented for
  ADPCM there). Hit2 position relative to onset matches within AAC-encoder smear.
* Sound-engine producer for instance 0x0060E3E0 — unchanged attempt-5 next step.

## Attempt 16 (operator directive round-6): GSP scan-out path implemented — the overlay hypothesis is REFUTED; the dialog pixels are not in any guest buffer

### What was implemented (src/main.rs)

* `gsp_scanout_capture()` — real-hardware scan-out semantics per local
  azahar-master (`gsp_gpu.cpp SignalInterrupt PDC0/PDC1` → `GPU::SetBufferSwap`):
  the FrameBufferUpdate in GSP shared memory (offset `0x200 + (2*tid+screen)*0x40`)
  is consumed on every PDC0 vblank; both double-buffer slots are tracked
  persistently (entry.active_fb = slot the address belongs to, entry.shown_fb =
  slot the LCD shows) and the shown slot is captured into the host shadow.
* Scan-out shield in `gsp_display_transfer`: once framebuffer info is
  registered, a display transfer into the NON-shown pair member no longer
  overwrites the shadow (on hardware it is invisible).
* `gsp_write_screen_ppms()` factored out; /tmp snapshots refreshed from the
  vblank path throttled to 5/s.
* New diagnostics: `DQ8_TRACE_GSP_FB=1` (scan-out register/slot trace),
  periodic `presentation state:` and `pixel-where:` lines (draw count, RT
  address/size, shadow occupancy, straight-from-RAM nonzero counts of the full
  PICA RT, the transfer source and both scan-out pair members).

### Evidence (comb4–comb7, windowed Xvfb, DQ8_RUN_GUEST_THREADS=1, fresh userdata)

* comb4/comb5 (scan-out + shield, DQ8_TRACE_GSP_FB): the guest registers
  framebuffer info every frame from startup — top slot0=0x1F346500,
  slot1=0x1F300000 (RGB8, stride 720), shown=1; bottom 0x1F038400/0x1F000000.
  Our shadow now follows the SHOWN buffer (2300+ trace lines, slot flips
  captured). Dialog window frames STILL 100 % black (dense_0110–0146).
* comb6/comb7 (presentation-state + pixel-where): during the black window the
  guest executes PICA draws continuously (pica_draws 672 → 3536, ~180/5 s)
  targeting rt=0x1F070800, 512×400 RGBA8 — and the RGB content of the RT,
  of the transfer source (0x1F070800) and of BOTH scan-out pair members is
  provably ZERO (scan-out RGB8 nonzero-pixel count = 0 straight from guest
  RAM). After the save completes the same path lights up (shadow_top_nz≈96000).
* Scenario intact on the instrumented builds: comb7 `FS FormatSaveData
  archive=00000004 ... result=00000000`; comb6 WAV accepted-quality (sound
  criterion unchanged; see comb3: peak −19.0 dBFS, 0 clipped).

### Conclusion (corrects the round-6 hypothesis with measured facts)

The dialog is NOT drawn via a GSP overlay/scan-out side buffer — the scan-out
path is now implemented faithfully and the shown buffer is black too. The gap
is INSIDE the PICA fragment stage: the guest issues the draws (vertex program
runs, counts grow) but our HLE rasterizer writes no visible fragment color
into the RT (RT RGB = 0 with alpha=255 everywhere — consistent with the
texture/TEV path producing black). This is the same class the title task's
presentation bisection hunts; the scan-out work done here is the correct
hardware foundation it was missing (previously presentation followed transfers
only).

### State

* Branch: test/combined-stage (approved integration stand). The scan-out
  commit is here and is a clean cherry-pick for feature/audio-save-stage.
* Sound criterion: MET and unchanged (accepted in round 5, re-verified on
  comb3/comb6).
* Visible dialog: next bounded step = PICA fragment output (texture bind/TEV
  combiner in pica_vs.c rasterizer path) during the dialog phase — coordinate
  with the title task's bisection to avoid duplicate work.
