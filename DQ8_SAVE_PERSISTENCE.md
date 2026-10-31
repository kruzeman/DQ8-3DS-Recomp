# DQ8 save persistence across two separate game processes

Task: t_32cd4989 — "DQ8: Verify save persistence across restarts"
Date: 2026-10-21, 16:15–17:05 IDT. Workspace `/home/hermes/DQ8`, branch
`codex/fix-apt-home-resume` (no branch switches, no resets, no source edits).

Everything below is backed by the logs, frame captures and hashes listed at the
end; nothing is claimed without evidence.

## TL;DR

| Run | Question | Verdict |
| --- | --- | --- |
| **RUN 1** (fresh userdata, A presses injected) | save created and written by a real game process | **PASS** |
| **RUN 2** (same userdata, no input) | existing save read back, creation dialogs skipped, no reformat/rewrite | **PASS** |
| Both | is the opening *cinematic visible*? | **NO — still blocked by the no-op `y2r:u` stub** (see §6) |

Save **loading** and **video pipeline start-up** are verified in RUN 2;
video **display** is not. The two are deliberately reported separately.

## 1. Setup (both runs)

* Binary: `target/dq8-startup-split` (built 2026-10-21 14:13 from the current
  6,693 roots — **no rebuild was needed and none was done**).
* One unique, isolated save root for both runs, created empty by this task and
  preserved afterwards:
  `DQ8_USER_DATA_DIR=/home/hermes/DQ8/target/userdata-persist-20261021-a`
* Harness: new `tools/run_persist.sh <label> <userdata> <seconds> [press…]`
  (headless PPM capture, `DQ8_INPUT_FILE` A-press injection held 1000 ms,
  `DQ8_TRACE_IPC` + `DQ8_TRACE_INPUT` + `DQ8_PROGRESS_SECONDS`, SIGTERM at the
  end — never SIGKILL, never touches the save directory).
* Frame capture: the runtime's own snapshot `/tmp/dq8-screen.ppm` copied every
  5 s (and around each press) with a full-length validity check (288015 B).
* Timeline: the runner prints a `guest snapshot` line every 5 s, so log-line
  positions map to wall-clock time (`tools/persist_timeline.py`).

## 2. RUN 1 — fresh save  (330 s, presses at 120/200/280 s) → **PASS**

Frame evidence `target/persist-evidence/run1/montage_top.png` (73 frames,
visually inspected cell by cell):

| Time | Top screen |
| --- | --- |
| 5 s | black (boot) |
| **10–25 s** | SQUARE ENIX logo |
| **35–135 s** | dialog: "No previous save data could be read. / New save data will be created." (types in 35–45 s, waits; hourglass icon 110–120 s) |
| 120 s | A press #1 (down + up edges traced, see below) |
| 125–135 s | same text re-rendered after the press |
| **140–330 s** | dialog gone, both screens black, content never changes (presses #2 at 200 s and #3 at 280 s have no visible effect) |

FS / save evidence (`run1/run.log`, times from the 5 s snapshot markers):

| Event | Log line | ≈ time | Result |
| --- | --- | --- | --- |
| `honeySave.bin` open (before format) | 3316 | ~30 s | `C8804470` *file not found* (fresh dir) |
| HID press / release #1 | 7281 / 7326 | 120 s | `pressed=00000001` / `released=00000001` |
| `FS FormatSaveData` archive=4 | 7345 | ~120 s | `result=00000000` success |
| `honeySave.bin` open OK (×3) | 7984/8085/8088 | ~135 s | `result=00000000` |
| `FS ControlArchive` | 7997 | ~135 s | `result=00000000` |
| `FS:Write` handle 0x2F offset 0 size **0x33E50 = 212560** | — | +137 s | one write, whole save |
| `FS:GetSize` → `0x33E50` | — | ~135 s | size visible to guest |
| `honeySave.bin` mtime | on disk | 16:23:44 (= t+137 s) | 212560 B |
| `HONEY000` dir created (`0x08090182`) | 8148 | ~135 s | success |
| FS:Close on save handles 0x2F/0x30/0x31 (`0x08080000`) | — | after write | guest closed the file before shutdown |
| y2r video start-up (19 cmds) | 8272–8365 | ~140–155 s | see §6 |

Input: **3 presses, 3 releases**, all with traced HID edges
(`HID buttons=00000001 pressed=00000001 …` / `released=00000001 …`), each held
1000 ms (timeline log shows A_DOWN→A_UP exactly 1.019 s / 1.020 s / 1.019 s).
Press #1 is the one that dismisses the dialog; presses #2 and #3 occur during
the black phase and change nothing.

Clean shutdown: `SIGTERM` at t=330 s, exit status 143, no SIGKILL, no crash,
0 escapes, 0 unsupported instructions, 0 `unimplemented`/abort lines. The last
save write completed at t+137 s — 193 s before shutdown — and the guest had
issued `FS:Close` on every save handle, so no data was in flight.

## 3. RUN 2 — same userdata, no input  (300 s, **zero** presses) → **PASS**

Frame evidence `target/persist-evidence/run2/montage_top.png` (61 frames,
visually inspected cell by cell):

| Time | Top screen |
| --- | --- |
| 5 s | black (boot) |
| **10–25 s** | SQUARE ENIX logo (pixel-identical to RUN 1: same 4528 non-black px at t=10 s) |
| **30–305 s** | black, continuously — **no dialog ever appears** |

Side-by-side sheets: `compare_10_35_50_75.png` (visually verified) shows at
35/50/75 s RUN 1 with the dialog and RUN 2 completely black; pixel counts
confirm it (`tools/analyze_ppm.py`: t=50 s RUN 1 → 5205 non-black px,
RUN 2 → 0; t=10 s both → 4528).

FS / save evidence (`run2/run.log`):

| Event | Log line | ≈ time | Result |
| --- | --- | --- | --- |
| `honeySave.bin` open (×4) | 3316/3333/3437/3440 | ~30 s | `result=00000000` success — **no** `C8804470` anywhere |
| `FS:GetSize` → **0x33E50 = 212560** | — | ~30 s | size read from disk |
| `FS:Read` size **0x33E50 = 212560** (whole file) ×**2** | 3336 … | ~30 s | whole save read back from disk |
| `FS FormatSaveData` | — | **never** | save **not** reformatted |
| `FS ControlArchive` | — | never | no save-creation transaction |
| `FS:Write` (`0x08030102`) | — | **never** | 0 writes — save untouched |
| `HONEY000` create dir | — | never | not attempted (data already present) |
| HID events | — | **never** | no input injected at all |
| y2r video start-up (19 cmds) | 3613–3702 | ~30–45 s | reached automatically |

The data really comes from disk: the handle only exists because
`native_fs_dispatch` opened it successfully (`src/native_fs.c:147-152`), the
read path is a real `fread` (`src/native_fs.c:186`), and the zero-filling
fallback (`src/main.rs:5209`) can only run for handles the native FS did **not**
register. Behaviourally, the guest then treats the save as present (dialogs
skipped), which a zeroed/garbled save would not do.

Save integrity across the restart:

```
b849f53e2b8a8667a4c3aa306c7b4d61a782632bb2fb39b6e2862e2efd095660  honeySave.bin   (212560 B, mtime 2026-10-21 16:23:44)
```

identical digest, size **and** mtime before RUN 2 and after RUN 2 → RUN 2 never
wrote the file.

## 4. FS-call comparison (RUN 1 vs RUN 2)

`fs:USER` (session handle 8) command histogram:

| Command | Meaning | RUN 1 | RUN 2 |
| --- | --- | --- | --- |
| `0x080C00C2` | OpenArchive | 5 | 4 |
| `0x080201C2` | OpenFile | 7 | 6 |
| `0x080E0080` | (archive op) | 4 | 3 |
| `0x080B0102` | OpenDirectory | 2 | 2 |
| `0x08030204` | OpenFileDirectly | 2 | 2 |
| `0x084C0242` | **FormatSaveData** | **1** | **0** |
| `0x080D0144` | **ControlArchive** | **1** | **0** |
| `0x08090182` | **CreateDirectory (HONEY000)** | **1** | **0** |
| `0x0817/0x0861/0x0862/0x0863` | other archive ops | 1 each | 1 each |

File-level operations (handles ≥ 0x20):

| Op | RUN 1 | RUN 2 |
| --- | --- | --- |
| `0x080200C2` FS:Read | 204 | 205 |
| `0x08030102` **FS:Write** | **1** (212560 B save) | **0** |
| `0x08040000` FS:GetSize | 4 | 2 |
| `0x08080000` FS:Close | 4 | 5 |

RomFS reads map to the same movie in both runs
(`tools/romfs_read_report.py`): `data/Movie/TAKA_N.moflex` — 4 reads,
0x40000 bytes each run. RUN 1 total: 204 FS reads over 89 RomFS files;
RUN 2: 205.

## 5. Guest execution comparison

`DQ8_PROGRESS_SECONDS` snapshot every 5 s → `main=` PC distribution:

| Phase (main PC) | RUN 1 | RUN 2 |
| --- | --- | --- |
| `0x0012C34C` — startup / dialog wait | **26 snapshots (≈130 s)** | **3 snapshots (≈15 s)** |
| `0x0014C904`, `0x0014A97C`, `0x00223EF0` — transitions | 2 | 3 |
| `0x002FA3xx` — video ring-buffer setup | 3 | 3 |
| `0x002F9334` — **video/movie presentation pump loop** | 36 (≈180 s) | **51 (≈255 s)** |

Both processes end in exactly the same guest loop (`0x002F9334`, the 4-entry
ring-buffer pump identified in DQ8_OPENING_CINEMATIC_PROGRESS.md). RUN 2 enters
it roughly 110 s earlier because no dialog had to be displayed and confirmed —
quantitative proof that the save-creation dialogs were skipped rather than
merely unseen. Both runs: **0 escapes, 0 unsupported instructions, 0
abnormal exits**.

## 6. Video start-up vs. video display (explicitly separated)

Both runs issue the identical 19-command `y2r:u` sequence on service handle
`0x10` (`y2r:u` = handle 16 from `service_handles[]`, `src/main.rs:5257`):

```
0x002B 0x000D 0x000F 0x0027 0x0028 0x0001 0x0003 0x0005 0x0007 0x000D
0x001A 0x001C 0x0020 0x0022
0x0010 buffer=0x151D49E8 size=0x00017700 (96000 B, 400x240 Y plane)
0x0011 buffer=0x151F29E8 size=0x00005DC0 (24000 B, U)
0x0012 buffer=0x151F2AE8 size=0x00005DC0 (24000 B, V)
0x0018 buffer=0x152B2980 size=0x00046500 (288000 B, RGB destination)
0x0026 get transfer end
```

* **Video start-up: verified in both runs** (RUN 1 ≈ t+140–155 s after the
  A press; RUN 2 ≈ t+30–45 s with no input), plus 4×64 KiB reads of
  `data/Movie/TAKA_N.moflex`.
* **Video display: NOT achieved in either run.** Every frame from RUN 1
  140–330 s and RUN 2 30–305 s is fully black (0 of 96000 non-black pixels at
  t=200 s in both). Root cause unchanged from the previous card: the `y2r:u`
  shim answers configuration/transfer commands but performs no YUV→RGB
  conversion and signals no transfer end (`src/main.rs:5807`,
  docs/progress-2026-10-19.md), so the producer of the ring buffer at
  `0x002F928C` never yields a frame.
* **The opening cinematic is therefore NOT claimed to be visible.** What is
  verified is: save loaded → dialogs skipped → movie file read → YUV frame
  submitted for conversion → guest parks in the presentation loop.

## 7. Verdicts

* **RUN 1: PASS** — fresh userdata, Square Enix logo, save/extra-data dialogs
  followed with three 1000 ms A presses (all six HID edges traced),
  `FS:FormatSaveData`/`ControlArchive`/`Write` all succeeded,
  `honeySave.bin` = 212560 B, closed by the guest, process terminated cleanly
  (SIGTERM, no crash), frames + logs captured.
* **RUN 2: PASS** — same userdata directory reused byte-for-byte, save read
  successfully (2 × full 212560 B reads, GetSize 212560), creation dialogs
  skipped (no dialog in any of 61 frames, no `FormatSaveData`, no `Write`),
  automatic transition after the logo into the opening-movie video path,
  save digest/size/mtime unchanged afterwards, zero input injected.
* **Save persistence across two separate processes: VERIFIED.**
* **Cinematic display: still FAIL (blocked by the `y2r:u` no-op stub)** —
  unchanged from the previous card and out of this task's scope.

## 8. Files

New (this task):

* `tools/run_persist.sh` — two-phase run harness (frames, input, clean SIGTERM).
* `tools/montage_frames.py` — validated PPM montage builder.
* `tools/persist_summary.sh` — per-run FS/input/guest/y2r summary.
* `tools/persist_timeline.py` — maps log events to wall-clock time via the 5 s
  snapshots.
* `DQ8_SAVE_PERSISTENCE.md` — this report.

Evidence (preserved):

* `target/userdata-persist-20261021-a/` — the isolated save root **for both
  runs**, kept intact (`archive-00000004-00000000/honeySave.bin`,
  212560 B, sha256 `b849f53e…`).
* `target/persist-evidence/run1/` — `run.log` (8917 lines), `timeline.log`,
  `frames/` (73 validated PPMs), `montage_top.png`, `honeySave.sha256`, `honeySave.stat`.
* `target/persist-evidence/run2/` — `run.log` (4424 lines), `timeline.log`,
  `frames/` (61 validated PPMs), `montage_top.png`.
* `target/persist-evidence/compare_10_35_50_75.png`,
  `target/persist-evidence/compare_100_120_150_200.png` — run1/run2 pairs.

Untouched: `recomp-roots.txt`, `src/main.rs`, `src/native_fs.c`,
`src/native_hid.c`, `src/pica_vs.c` (no runtime changes were needed) and every
pre-existing `target/userdata*` directory.

## 9. Reproduce

```sh
UD=/home/hermes/DQ8/target/userdata-persist-20261021-a
bash tools/run_persist.sh run1 "$UD" 330 120 200 280   # fresh dir, 3 x 1000 ms A
bash tools/persist_summary.sh run1
bash tools/run_persist.sh run2 "$UD" 300               # same dir, no input
bash tools/persist_summary.sh run2
python3 tools/persist_timeline.py target/persist-evidence/run{1,2}/run.log
python3 tools/montage_frames.py out.png --dir target/persist-evidence/run1/frames --cols 6 --scale 1
sha256sum "$UD"/archive-00000004-00000000/honeySave.bin
```
