# DQ8 opening cinematic — progress report (task t_feddb4cf)

Date: 2026-10-21, session 13:15–15:00 IDT. Workspace /home/hermes/DQ8, branch
`codex/fix-apt-home-resume` (no branch switches, no resets, no save deletion).
Everything below is backed by the logs/screenshots listed; nothing is claimed
without visual or log evidence.

## TL;DR

1. **The first save dialog now dismisses on an A press** — a first for this
   recompiler. Verified with two 1000 ms presses whose HID press *and* release
   edges are both traced, and with frame captures (dialog 1 present → screen
   clears).
2. **A previously invisible infrastructure blocker was found and fixed**: the
   full-root build (85,711 functions, 168 MB of generated C) could never be
   compiled because clang needs ~4 GiB for the single-translation-unit output
   and the Hermes worker cgroup caps memory at 3.9 GiB — the kernel OOM killer
   had been SIGTERMing clang (journalctl evidence). New tool
   `tools/split_dispatch.py` splits that file into 35 independent TUs; the same
   generated code now builds in **33 seconds** with ~10× lower peak memory.
3. **The next blocker is identified with evidence**: after the dialog closes the
   guest immediately enters the *video/movie presentation path* — it sets up
   `y2r:u` (the 3DS YUV→RGB engine) and submits conversion requests for a
   400×240 YUV 4:2:0 frame (96000 / 24000 / 24000 byte planes → a 288000-byte
   RGB destination) — while our `y2r:u` service is still a no-op stub
   (docs/progress-2026-10-19.md). The screen stays black and the main thread
   spins on a 4-entry video-input ring buffer at `0x002F928C`.

## 1. Prior cinematic research (operator request — searched, not re-derived)

*Git*: 4 branches (`codex/fix-apt-home-resume`, `codex/fix-gsp-overlay-stall`,
`codex/fix-worker-descriptor`, `main`), one worktree, 9 commits, no stash, no
unreachable objects (`git fsck --unreachable` empty). No commit, branch or
dangling object contains a video decoder.

*Existing research files* (all reused, none duplicated):

| Source | Finding |
| --- | --- |
| `docs/runtime-wait-fixes.md:38-41` | the media worker reads 64 KiB blocks of the movie from RomFS; "the source starts **4C32 (Moflex)**"; the media worker used to stall on an event and now reaches decoder code |
| `docs/progress-2026-10-19.md:16` | "**Movies — y2r:u interface has no-op responses; actual YUV conversion/video presentation is not implemented**" |
| `README.md:56` | `tools/romfs_read_report.py` maps the runner's `FS Read` offsets back to RomFS paths — the tool for confirming movie playback |
| `src/main.rs:5807-5846` | the `y2r:u` service shim (commands 0x000F/0x0014-0x0017/0x0020-0x0028 answered, conversion itself not performed) |

*Corroboration done here*: `data/Movie/*.moflex` exist — 41 files, listed with
RomFS offsets and sizes in `/tmp/romfs_files.json` (e.g. `data/Movie/P01.moflex`
25 MB at 1894728992). A hex dump of that offset starts with `4c32 28ae …`, i.e.
exactly the Moflex magic the earlier research recorded. The opening also has 3D
event models under `data/Model/event/e000/opening/`.

No standalone Moflex decoder/player exists anywhere (repo, `tools/`, `/tmp`,
kanban comment history, `azahar-master`, `3DSkit-master` — all grepped). So
nothing could be reused, and nothing was re-derived: the container is Moflex
(MJPEG video + PCM audio), the presentation path is `y2r:u`, and the missing
piece is the Y2R conversion + frame presentation in our runtime.

## 2. Build-pipeline blocker (root cause of the previous session's dead end)

Card t_4bacd398 (runs 40/41) regenerated `target/dq8-startup.c` from the current
`recomp-roots.txt` (6,693 roots, 85,711 reachable functions, 943,560
instructions, 168 MB) but every `clang` run died:

```
kernel: clang invoked oom-killer …
kernel: Memory cgroup out of memory: Killed process 475493 (clang) total-vm:8424300kB, anon-rss:3896664kB
        oom_memcg=…/hermes-worker-kanban-t_4bacd398-run-40.scope
```

`memory.max` for a worker scope is 4157779968 B (3.9 GiB); the single-TU build
needs more. The roots added by that session (including `0x002FCB34`, the escape
that kills the previous 10:53 binary) therefore never reached a binary.

**Fix — `tools/split_dispatch.py` (new, no recompiler changes).** The generated
interpreter is one `switch (pc >> 12)` with one inner `switch (pc)` per
translated 4 KiB page (1,063 pages). The tool rewrites it into

* `dq8_rt.h` — extern declarations for the 59 runtime symbols the translated
  cases reference (derived from the prelude automatically),
* `dq8_main.c` — the original runtime with the giant switch replaced by a
  4,096-entry page table call,
* `dq8_part_NNN.c` — 34 TUs, 32 pages each, each reporting whether it handled
  the PC (return 0 ⇒ identical "translated PC escaped …" behaviour).

Every translated instruction, per-page `default:` escape, SVC shim and the outer
escape message are preserved verbatim — only the file layout changes. Splitting
takes 1.2 s; `dq8_main.c` compiles in 0.4 s; the whole build (35 TUs in
parallel + link) takes **33 s** and produces `target/dq8-startup-split`.

## 3. Behaviour of the new build (verified)

Run `/tmp/dq8-sess/run_cinematic.sh c2` → `/tmp/dq8-sess/run_c2/`:
fresh `target/userdata-cin-c2`, headless PPM capture, no input until T+120 s,
two 1000 ms A presses (T+120, T+200), 320 s total.

| Check | Result |
| --- | --- |
| Escapes / crashes | **0** (also 0 in a separate 200 s IPC-trace run) |
| Unsupported instructions | 0 |
| HID edges | 2 presses + 2 releases traced (`pressed=00000001` / `released=00000001`) |
| Frame rate | 1062 display transfers / 320 s ≈ 3.3 fps (unchanged, PICA-bound) |

Frame timeline (montage `/tmp/dq8-sess/run_c2/montage_top.png`, visually
verified cell by cell):

| Time | Top screen |
| --- | --- |
| 10 s | SQUARE ENIX logo |
| 50–110 s | dialog 1 "No previous save data could be read. / New save data will be created." types in |
| 120 s | A press #1 |
| **140 s → 320 s** | **dialog closed, screen fully black on both screens, content never changes** |

So the A press finally works: dialog 1 is dismissed. Previously every press
aborted the guest at an unrooted vtable method.

## 4. First confirmed blocker toward the cinematic (diagnosed, not yet fixed)

During the black phase the main thread is *not* dead: snapshots show it at
`0x002F9334`, `0x002FA8A0`, `0x004C9C48` and later `0x0014C8DC`, and display
transfers keep happening. Disassembly of the loop (new tool
`tools/dq8_disasm_at.py`):

```
0x002F928C: stmdb sp!, {r4, r5, r6, lr}      ; r4 = object
0x002F929C: r0 = r4 ; call(0x002F93A0)       ; pump the queue
0x002F92A4: compare(r0, 0x0) ; goto.eq(exit)
0x002F92AC: r0 = *(r4+0x58) ; r1 = *(r4+0x5C) ; compare(r0, r1) ; goto.eq(0x002F9320)
0x002F9320: call(0x002FCAB4)
0x002F9324: r0 = *(r0) …
0x002F9334: svc 0x24 (CreateMutex)            ; then call.ne(0x001419F4), loop
```

i.e. a **4-entry ring buffer** (`+0x58` producer / `+0x5C` consumer index,
per-slot flags at `+0x18`) whose producer never fills it.

The decisive evidence is the IPC trace of the same window
(`/tmp/dq8-sess/run_c3/run.log`, press at line 7013): starting at line 8092 the
guest performs the whole `y2r:u` start-up sequence —
`0x002B, 0x000D, 0x000F, 0x0027, 0x0028, 0x0001, 0x0003, 0x0005, 0x0007,
0x000D, 0x001A, 0x001C, 0x0020, 0x0022` — followed by
`0x0010/0x0011/0x0012` with buffers `0x151D49E8` size **0x17700 = 96000 B**
(400×240 Y plane), `0x151F2AE8` size **0x5DC0 = 24000 B** (subsampled C plane,
twice), and `0x152B2980` size **0x46500 = 288000 B** (400×240×3 RGB target),
then `0x0026` (get transfer end).

That is the **opening movie's YUV frame conversion**. Our `y2r:u` shim answers
the configuration commands but performs no conversion and signals no transfer
end (`src/main.rs:5807`), so the video pipeline never produces a frame, the
ring buffer at `0x002F928C` never fills, and the top screen stays black.

**Next concrete step (for the following card):** implement `y2r:u` frame
conversion in the runtime — honour commands 0x0010/0x0011/0x0012/0x0018/0x0026
(the transfer setup + end event), perform the YUV 4:2:0 → RGB conversion into
the guest destination buffer (`0x46500` bytes) when the transfer is triggered,
and signal the conversion-end event so the producer side of the ring buffer at
`0x002F928C` runs. The frames themselves come from the RomFS Moflex files
(`data/Movie/*.moflex`, magic `4C32 28AE`, MJPEG + PCM), whose bytes the guest
already reads through `FS Read` (use `tools/romfs_read_report.py` to confirm
which file is opened).

## 5. Files changed / created this session

* `tools/split_dispatch.py` — new: multi-TU splitter for the generated C
  (the fix that makes full-root builds possible inside the worker cgroup).
* `tools/dq8_disasm_at.py` — new: extract + disassemble a guest address range
  from `DQ8.code` without `dd` (policy-safe helper).
* `target/dq8-split/` — generated split sources (161 MB, rebuildable).
* `target/dq8-startup-split` — working binary from the current full root set.
* `target/userdata-cin-c2`, `target/userdata-cin-c3` — fresh isolated save dirs
  created by this session (no pre-existing save touched or deleted).
* `recomp-roots.txt`, `src/main.rs`, `src/pica_vs.c` — **untouched by this
  session** (the 5,573 roots and the VFP/`ldr pc` decoder fixes from the
  earlier session are preserved and are what the new binary is built from).

## 6. Reproduce

```sh
python3 tools/split_dispatch.py target/dq8-startup.c target/dq8-split
/tmp/dq8-sess/build_split.sh          # 35 TUs in parallel -> target/dq8-startup-split
/tmp/dq8-sess/run_cinematic.sh c4     # fresh userdata, two 1000 ms A presses, PPM frames
```

Artifacts: `/tmp/dq8-sess/run_c2/montage_top.png` (logo → dialog → black),
`/tmp/dq8-sess/run_c2/t90s.top.png` (dialog 1), `/tmp/dq8-sess/run_c2/t310s.top.png`
(black phase), `/tmp/dq8-sess/run_c2/run.log`, `/tmp/dq8-sess/run_c3/run.log`.
