# DQ8 save-dialog text stall — diagnostic handoff (2026-10-20)

Task: t_63f2c5cb "Diagnosing and resolving the dialogue text rendering stall"
Status: root-cause candidate identified, fix NOT yet implemented/verified.

## Symptom
Save-confirmation dialog shows window frame (9 x 8x8 border fragments) + cursor
(16x16) every frame, but the message text "No previous save data exists. New
save data will be created." is never formatted or drawn. Font atlases
(FontDefault.bffnt / FontAscii.bffnt) are loaded but never bound.

## Verified facts (headless runs, logs in /tmp/dq8-r23/runA..F.log)
1. Main-thread frame pump healthy: GSP thread[6] signals startup-object arbiter
   0x5CCCA4 each vblank; main wakes, runs one UI step, re-arms to -1.
   PICA finalize fired 2553x. Guest is alive, not hung.
2. Guest never issues glyph draws. UI-object/selection/bflyt traces (0x4725E8,
   0x223E40) show only title-screen panes (title_e000_u/l, title_e002_u; manager
   0x54B644). Dialog text pane never laid out. 23 container lookups, none touch
   the message table.
3. Message text exists in RAM ONLY as raw ASCII inside RomFS msg table blocks:
   file buffer 0x085AF5AF, copies 0x086062DC / 0x08606340 / 0x092FFF7C.
   Record delimiter is literal "[end]" (binary sites 0x239DEC, 0x2BD358,
   0x49167C, 0x543AD2, 0x5AA330 next to "floating_win000" string table).
   No literal-pool / movw+movt xrefs to delimiter or worker arbiter words —
   guest computes them from loaded structure pointers. UTF-16 working-buffer
   scan for the message: 0 hits (runE). Extraction/decode never runs.
4. ROOT-CAUSE CANDIDATE — two pool workers parked forever on unsignaled
   address arbiters; no job ever posted:
   - thread[3] entry 0x16B680: svcArbitrateAddress WaitIfLessThan (type 1,
     value 1) on word 0x60E498 == 0.
   - thread[5] entry 0x1630F0: WaitIfLessThanOrEqual (type 3, value 0) on word
     0x60BAB0 == 0xFFFFFFFF.
   Run B write-trace: 0x60E498 written ONCE at boot (to 0), never again.
   No Signal (type 0) ever targets 0x60E498/0x60BAB0 (only 0x5CCCA4 + heap
   mutexes get signaled). Message extraction / glyph layout presumably belongs
   on these workers; the dispatcher never enqueues.
5. No failed/unsupported/unimplemented guest calls in runA after filtering
   expected ENOENT probes.

## Next steps (unresolved)
- Disassemble worker entry prologues 0x16B680 / 0x1630F0 and their
  0x15ED40/0x15ED34 argword wrapper to identify the owning subsystem
  (text vs sound), then trace back to the enqueue path that never runs.
- Prime suspect for the missing enqueue: svc_arbitrate_address semantics in
  src/main.rs (WaitIfLessThan / Signal handshake) or a stubbed SVC/event the
  guest expects to wake the poster thread (thread[4] waits on event 0x2C,
  thread[1] in WaitSyncN on handle set — both never signaled either).
- After the runtime fix: rebuild target/dq8-startup-headless, verify glyph
  draws appear and the message decodes to UTF-16.

## Repro
cd /home/hermes/DQ8
timeout 100 env DQ8_RUN_GUEST_THREADS=1 DQ8_HEADLESS=1 DQ8_TRACE_THREADS=1 \
  DQ8_TRACE_ARBITER=1 DQ8_TRACE_UI_OBJECT=1 DQ8_TRACE_LOOKUP=1 \
  DQ8_SCAN_MSG='No previous save data' \
  ./target/dq8-startup-headless DQ8.code target/dq8.romfs

## Scratch tooling (this session)
~/.hermes/profiles/coder/cache/scratch/{disasm3,find_callers,scan_lit,
find_prologue,find_splitter,xref_endstr,disasm_endstr,scan_movw_pair,
an_runA}.py
