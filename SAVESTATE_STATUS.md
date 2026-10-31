# t_d50c9503 — SAVESTATE TOOLING (F5 save / F9 load + startup -load-state)

Status: in progress (minute-1 status file, committed as required by COMMIT-OR-DIE).

Base: anchor pkg13r-ANCHOR, commit 96ec3ca, branch `feature/savestate`
(worktree /home/hermes/DQ8/worktrees/savestate — the anchor worktree
black-world-t_0b95f1a7 is NOT touched).

## Design (studied /home/hermes/DQ8/azahar-master/src/core/savestate.cpp)
Azahar: CSTHeader{magic "CST"0x1B, title id, git revision, build name/version,
time} + one compressed stream of System::serialize =
timing(CoreTiming events) + CPU cores + service manager + archive manager
(FS backends) + DSP HLE + full MemorySystem RAM + Kernel (processes, threads,
mutex/event/semaphore/timer/address-arbiter objects) + GPU + movie.
Header validates magic/title/build so a foreign state is rejected.

Our runtime is a static recompiler with a FLAT dispatch loop
(`translated_from(r, pc, context)`), guest "calls" are pc transitions, so a
state is exact if it is taken at a dispatch boundary:
  * whole 1 GiB guest address space `ram` (sparse: only non-zero 64 KiB chunks);
  * main context pc/r[16]/APSR flags + worker GuestThread[] (regs, pc, flags,
    wait_*, tls, priority, handle);
  * kernel object tables + handle allocators (mutex/event/arbiter, thread and
    memory-block handle counters);
  * service bookkeeping our shims keep outside guest RAM: HID ring, GSP
    scan-out/framebuffer pair, PICA RT/present counters, Y2R config, DSP HLE
    counters/pipe, APT signals, FS archive/dir tables, presented shadow
    framebuffers;
  * host resources that cannot cross a process (FILE*/DIR*) are recorded as
    {path, mode, offset} and reopened on load.
Header: magic + format version + build signature (struct/var-table layout
hash) + save time, so a state from another build is rejected with a message.

## Files
- src/native_savestate.c (new) — format, save/load, sparse RAM, FS reopen.
- src/native_fs.c — record path+mode per guest file handle (reopen on load).
- src/main.rs — F5/F9 in host_display_collect_events, savestate service hook at
  the dispatch boundary, `--load-state/--state` argv handling, cold load in
  main(), splice of src/native_savestate.c.

## Triggers
F5 = save, F9 = load (SDL window). Startup: `--load-state PATH` (or
`--state PATH`, DQ8_LOAD_STATE, DQ8_STATE_FILE, DQ8_STATE_DIR).
Headless smoke hooks: DQ8_SAVESTATE_AUTOSAVE_AT=<sec>, DQ8_SAVESTATE_AUTOLOAD_AT=<sec>.

## Next
build with tools/build_rfix5.sh (BUILD_JOBS=24), smoke: autosave writes a file,
startup --load-state restores without crash; then binary + operator instructions.
