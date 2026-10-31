# Escape-hunting round 10 — fault pc=0x0015EF30 / address=0x73747859 (save-menu flow)

Task: t_c2bff117 (follows t_911eff5c round 8/9). Branch: fix/audio-buzz @ 9645867
(evidence base) + round-10 commit(s) in this doc's history.

## Signature (esc8-fixverify2_400, rc=16, t=274)

```
guest memory fault: pc=0x0015EF30 address=0x73747859 width=4
r0=r4=0054B3E4 r1=73747865 lr=00524554 sp=08690330
```

## Anatomy (llvm-mc ground truth)

- pc=0x0015EF30 is `ldr r2, [r1, #-12]!` inside the refcount-release leaf at
  0x0015EF20 (shared-ptr style: object ptr p with refcount at p-12; null
  sentinel 0x00613AB8, static sentinel 0x00613AAC, -1 = immortal).
  The leaf was entered with r0 = a pointer-holder slot; `[r0]` = 0x73747865,
  i.e. ASCII "exts" — bytes of the static FS-archive descriptor at 0x0054B3E4
  ("extsdmc:" path data + UTF-16 "sdmc:") read as an object pointer.
- lr=0x00524554 is NOT string data (red herring): 0x00524550 is a legit
  `bl 0x0015EF20` inside vector<smart_ptr>::erase's caller family
  (the loop at 0x0052454C..0x0052455C releases vacated slots).
- erase() itself is at 0x005244A8 (`push {r3-r11,lr}`, this=r6,
  `ldr r0/r7, [this]` = begin/end, shift loop, then tail-release loop,
  `bic r1, r1, #3` strips iterator tags — round-9 semantics).
- The erase call at 0x003E84BC belongs to the entity-collect function
  0x003E8428 (validates 160 x 0x30-byte entries from a table into a
  vector<smart_ptr> local). Its three legit callers are 0x001CB268/0x001CB320/
  0x001CB3E8, which zero the stack vector first.
- In the fault run, 0x003E8428's frame shows: saved lr slot = 0x005CB6E0
  (runtime .bss main-thread stack region — NOT a code return address), and the
  vector local at 0x08690388 = {begin=0x0054B3E4, end=0x086903E8,
  cap=0x00050018} = an ancestor frame's saved r4/r5/r6 (FS descriptor ptr,
  stack ptr, thread id 0x00050018). So the function ran with frames/args read
  from the wrong stack words: control-flow/context corruption upstream, not a
  container bug.

## Sibling manifestation (esc10-trace1_400, rc=3)

Escape to heap pc=0x085DD4E8 popped by `pop {r4,r5,r6,pc}` at 0x0027814C
(event/handle method frame) — popped {0, 0x085DD4F0, 0x0868F4B0, 0x085DD4E8}
= heap-object words as saved regs. Same window: immediately after the one-shot
romfs reads (handle 0x26 @ 0x758AF9A0/0x758AED20) that follow
FS DeleteFile SendPostcard. Note lr=0x0027812C and sp=0x086903B0 are legitimate
for that frame; the frame *contents* were foreign.

## Reproducibility (5 runs, schedule 30/60/90/195/203/207/211/216:START/.../380)

| run | binary | outcome |
|---|---|---|
| esc8-fixverify2_400 | 9645867 | FAULT pc=0x0015EF30 at t=274 |
| esc10-trace1_400 (7 PC trace) | 9645867 | escape 0x085DD4E8, rc=3 |
| esc10-trace2_400 (7 PC trace) | 9645867 | clean until CloseHandle(0x11) abort t=280 |
| esc10-repro1_400 (untraced, identical cmd) | 9645867 | clean until CloseHandle(0x11) abort t=280 |
| esc10-trace3_400 (3 low-cost PC trace) | 9645867 | clean until CloseHandle(0x11) abort t=280 |

Conclusion: the fault/escape are input-timing dependent (wall-clock press
schedule vs guest step progress; guest threads are cooperative on one host
thread — no host memory race). The only 0x003E8428 entries ever observed live
are the 3 legit ones (lr=0x001CB26C/0x001CB324/0x001CB3EC).

## Deterministic gap fixed in round 10

Every run that survives past t=274 dies at t=280 with
`unimplemented 3DS SVC CloseHandle: handle=00000011` (exit 15): the runtime's
srv:GetServiceHandle assigns cecd:u = handle 17 (service_handles[12]) but
svc_close_handle only whitelisted 5..16. The game legitimately closes cecd:u
when leaving the save-menu/StreetPass-postcard flow. Fix: whitelist 5..17
(real 3DS semantics: closing a valid srv session succeeds).

## Escape mechanism proven (esc10-closefix_400, fixed binary, untraced, t=278)

The second untraced run on the fixed binary reproduced the corruption family
without PC-trace overhead:

```
translated PC escaped to 005CB6E0 after 005CB6E0
r0-r7=00000000/00000000/00000000/0869035C/00300604/C82044B4/085BFD1C/C82044B4
lr=003E84E8 sp=08690388
```

Decoded: 0x003E8428 (collect) ran to its epilogue
`add sp,#12; pop {r4-r11,pc}` and popped pc = [0x08690384] = **0x005CB6E0** —
the same value that sat in the fault run's saved-lr slot. The popped r4-r7
(0x00300604, 0xC82044B4, 0x085BFD1C, 0xC82044B4) are exactly the saved-reg
pattern seen in the fault run's stack at a 12-byte-shifted offset, and
r3=0x0869035C is the legit collect local. Two mechanisms remain consistent
with the evidence and the single-PC entry traces (6 entries, all legit — never
caught a bad one): (a) collect was ENTERED via an unlinked/corrupt transfer
with a stale lr (the push then stored it), or (b) the frame slots were
stomped after the push by a structured write. Both are discriminated by a
DQ8_TRACE_PC=0x003E8428 hit on a recurrence run: at ENTRY, saved-lr-in-r14
vs later stack corruption is unambiguous. Note the popped r4-r7
(0x00300604 code, 0xC82044B4 x2 float-shaped, 0x085BFxxx heap) do NOT match
any legit caller's register pattern (legit entries show
0x085ABA54/0x08690388/0xFFFFFFE0/0), so the corruption is structured, not
random. 0x005CB6E0 lies in the game's static (.bss) thread-stack region
(main thread sp=0x005CDBB8 was observed), i.e. a stack ADDRESS is leaking
into an lr/pc slot.

Same root explains the fault run: the same corrupt frame/saved-lr state, and
the frame positions shift so the vector argument reads ancestor saved-regs
and the refcount release dereferences the FS-descriptor string bytes.

Corruption recurrence so far (untraced): fault (orig), clean-to-abort
(repro1), escape (closefix) ≈ 2/3; PC-traced runs diverged before it.

## Round-10 roots (esc10-entry4_400 escape, third manifestation at t=279)

entry4 (single-PC trace on 0x003E8428 — all 6 collect entries LEGIT) still
corrupted in the same window: after SendPostcard delete + the one-shot romfs
reads, the MAIN thread's control-block destroy path executed

```
0x003EF7DC: blx r1        ; r1 = [r4+8] destroy callback, r0 = [r4] object
                           ; lr=0x003EF7E0, sp=0x0FFD44D8 (main stack)
→ escaped to 0x003EE91C (untranslated) with r1=0x003EE91C
```

0x003EE91C is a genuine callback entry: `cmp r0,#0; bxeq lr; add r0,r0,#52;
push {r4,lr}...` — the same null-check-prologue family as the round-9 roots,
simply never added to recomp-roots.txt. A family scan of 0x003ED000..0x003F1000
for that prologue found exactly 4 entries: 0x003EFCA8 + 0x003EFE80 (rooted in
round 9), plus unrooted siblings **0x003EE91C** (proven escape) and
**0x003F03AC** (same prologue). Both siblings rooted in round 10.

A follow-up smoke (esc10-rootsmoke_300) then escaped at the NEXT sibling of
the same class: `blx r1` destroy dispatch at lr=0x003EF3E8 with
r1=[obj+8]=**0x001BC4FC** (r0=0x085BD16C object, r2=0xFFFFFFFF refcount,
r4=control block, sp=0x0FFD44C0 main stack, died t=277 after the same
SendPostcard delete + romfs reads). 0x001BC4FC disassembles to a genuine
stub entry (`b <shared impl>` right after a function's `pop {r4,pc}`) —
rooted as well. The destroy-callback family is WHACK-A-MOLE: callbacks are
stored per-object at [obj+8] across many regions, so round 11 should
enumerate the family systematically (e.g. collect all pointers dispatched
from control-block +8 slots in a live run, or scan for the stub/null-check
entry patterns) instead of one-by-one.

## Next steps (round 11)

- Best next probe (validated cheap): `DQ8_TRACE_PC=0x003E8428
  DQ8_TRACE_PC_MAX=64` only — collect runs ~3x/run so overhead is negligible;
  on a corruption-recurrence run (~2/3 untraced) the entry trace prints the
  corrupt lr + r0-r12 + 12 stack words at ENTRY, naming the unlinked transfer
  source. Attempt 2-3 runs.
- The value 0x005CB6E0 (static stack region) in an lr/pc slot suggests a
  context-restore/stale-pointer class; the entry trace's stack words should
  identify the frame that produced it.
- Verify the CloseHandle fix in-flow: the two runs that reached t=280
  pre-fix always aborted (exit 15); post-fix runs have not yet reached t=280
  (they corrupted at t=278 first). One clean post-fix run past t=280
  completes that verification.
