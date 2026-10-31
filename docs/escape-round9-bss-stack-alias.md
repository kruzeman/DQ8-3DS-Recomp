# Escape-hunting round 9 — 0x005CB6E0 is a stack-aliasing artifact, not a rootable entry

Task t_ae4a1a2b. Companion to `docs/escape-round10-fault-15EF30.md` (round 10) and
round 8's fault stack dump.

## TL;DR

The operator hypothesis ("translated-PC escapes to 0x005CB6E0 during title startup —
maybe an undiscovered function entry; check what table points to it") is answered:
**nothing points to 0x005CB6E0 — it is not code and cannot be a function entry.**
It is an address inside the game's static main-thread stack region (.bss, main
thread sp ~0x005CDBB8), which ends up in collect's (0x003E8428) saved-lr slot
because **collect's worker-thread stack frame memory is aliased by a live heap
buffer (a ~40-byte serializer/string record containing callback pointers)** that
other threads memcpy into. The pop reads the record as saved registers; the
record's pointer field value happens to be 0x005CB6E0, so the popped pc is that.

One aliasing bug explains both round 8's fault stack dump and round 10's anatomy,
at both stack bases (0x085A9xxx and 0x08690388) and with deterministic record
content.

## 1. 0x005CB6E0: exhaustive static negative result

Guest code image `DQ8.code` (file base 0x00100000), DQ8-startup md5
70fa036ac62081da family:

- bytes at guest 0x005CB6E0 (file 0x4CB6E0): all zero; image covers to ~0x005FA000
- word xrefs (unaligned-capable): **0** (no literal pool, table or data word equals it)
- ARM B/BL/BLX-immediate branches targeting it: **0**
- Thumb B.W/BL/BLX (32-bit encodings, 2-byte aligned scan): **0**
- MOVW/MOVT pairs materializing it: **0**
- `add/sub Rd, pc, #imm` (ADR-style) materializing it: **0**

0x005CB6E0 lies in the static .bss main-thread stack region (round-10 doc: main
thread sp=0x005CDBB8 observed). Rooting it would translate zeros — the forbidden
data-as-code silent-stub. It is only ever seen as a VALUE inside runtime stacks.

The value can only be produced at runtime as `sp +/- imm` or a load of a stored
pointer — consistent with "some object legitimately stores &main_stack_slot in a
record field".

## 2. Self-escape line ("to X after X") is a split-dispatch artifact

The escape line prints `to 005CB6E0 after 005CB6E0` because the pop at collect's
epilogue (case 0x003E8538 in part_031) sets `pc=0x005CB6E0`; the part loop-top
updates `previous_guest_pc = current_guest_pc; current_guest_pc = pc`; the page
switch misses (no part covers page 0x005CB — `dq8_part_table` has no entry, and
no `case 0x005CB:` exists), the part breaks with hit=1, and the main loop
re-dispatches 0x005CB6E0 with a NULL part → the `_ph==0` arm prints both the
current pc and previous pc, both now 0x005CB6E0. No second guest instruction
executed at 0x005CB6E0.

## 3. Collect frame anatomy (byte-exact)

```
0x003E8428: push {r4-r11,lr}      ; saves 9 words at [sp-36..sp)
0x003E842C: sub sp,#12            ; 3 vector words (local1..3)
...body, 160-iteration table walk, blx into thumb code at 0x003E84E4...
0x003E8534: add sp,#12
0x003E8538: pop {r4-r11,pc}       ; reads the 9 words back
```

Worker thread entry sp in repro runs: **0x08690388** (stable across
esc10-entry4's 6 entries and esc10-closefix), so the saved-lr slot = 0x08690384.
Static refs to 0x003E8428: exactly the 3 legitimate `bl` sites 0x001CB268 /
0x001CB320 / 0x001CB3E8 (matching every traced entry's lr), nothing else — with
all materialization doors closed (§1 method applied to 0x003E8428 too), a "4th
entry" cannot be produced by static code.

## 4. The corruption: memcpy into a heap record that aliases the stack frame

Probe esc9-lrwatch1_400 (write-watch `DQ8_TRACE_WRITE=0x08690364:0x28`, repro
schedule, 400 s, rc=3): 1012 captured writes into collect's saved-frame region:

- writers are **two memcpy implementations**: 0x00300030 (8-byte-unrolled
  newlib memcpy, 614 writes, dst range exactly [0x08690364..0x08690388]) and
  0x0014A964/96C (32-byte-unrolled `__aeabi_memcpy8`, 353 writes). The
  random-looking payloads are copy data.
- caller (lr at write time) decode:
  - `lr=00462D68/00462D88` — std::string append: `ldr r0,[r4,#4]; add r0,r0,r1;
    bl memcpy` — dst = the string object's heap data pointer (~40 B buffer)
  - `lr=004B699C/004B69B0/004B689C` — serializer: `ldr r0,[r9,#0x58]; …;
    add r0,r0,r4; bl memcpy` — chunked record copies
  - writers execute on other threads (sp=0x085BCD18/38/48)
- **0x08690358-0x08690388 is therefore a live ~40-byte heap buffer while also
  being collect's stack frame** (entry sp=08690388). The popped "STRUCT" from
  round 8/10 (`r4=0x00300604, r5=0xC82044B4, r6=heap, r7=0xC82044B4,
  r8=0x002FDB90, r9=1, r10=0x18F1, r11=0, pc=0x005CB6E0`) is exactly the kind
  of content such a record holds (function-pointer fields 0x00300604/0x002FDB90
  = return addresses after `bl` at 0x00300600/0x002FDB8C; 0xC82044B4 result
  codes; 0x005CB6E0 a stored main-stack pointer).

Timing: the memcpy traffic is present early in the run (t < 170 s), collect's
frame usage comes later (t ≈ 274 s) — i.e. the thread's stack region was handed
out over a pre-existing live allocation (or vice versa): a **heap/thread-stack
aliasing in the game's allocator state**. On retail this allocation cannot
overlap; in our run it does, so the upstream cause is an earlier corruption of
the game's allocator metadata or a layout divergence (heap start/sizes, stack
pool) — this is round-12 material (§7).

## 5. Round-9 batch roots (callback-pool entry family + trampolines)

esc9-lrwatch1 escaped to 0x003F1278 (`blx r1` at 0x003F137C, r1=[obj+8]) — the
5th member of the destroy-callback family. Static enumeration of the whole image
found **61 `cmp r0,#0; bxeq lr` null-check stubs** (the 6 roots from rounds 8-10
are a subset — same prologue idiom); all disasm-verified. Appended 57 roots +
trampoline pair 0x003F1278 (tail-b) and 0x002FDE18 (its `nop; b` target, chain
ending in the 0x00300C6C stub family).

Second idiom enumerated the same way — **callback trampolines** of the form
`ldr rN,[pc,#imm]; str rN,[rM]; b <tail>` (12 image-wide; 4 had already been
rooted by observation in earlier rounds: 0x00293118, 0x002D0E00, 0x003AF5B4,
0x0046F758). esc9-lrwatch2 escaped to 0x0023FE7C (a member of exactly this
family), so the remaining 8 were root-verified and added. Getter-thunk cluster
(`ldr rN,[pc,#N]; ldr; ldr; bx lr`, e.g. 0x003F127C) documented, not rooted — no
escape has landed there yet.

Total this round: 65 new roots (7327 → 7396), both idioms discovered by static
enumeration rather than one-by-one whack-a-mole.

## 6. Discriminator run (esc9-lrwatch2_400) — stomp confirmed, aliasing caught live

`DQ8_TRACE_PC=0x003E8428,0x003E8538 DQ8_TRACE_PC_MAX=64 DQ8_TRACE_WRITE=0x08690384:4`,
repro schedule, 400 s, rc=3 (died at 0x0023FE7C, since rooted).

- **6 collect entries, all legitimate**: lr ∈ {0x001CB26C, 0x001CB324,
  0x001CB3EC} — the three real `bl` callers, two invocations each; sp=0x08690388
  every time. **No entry with a stale lr — stale-lr-entry (round-10's open
  alternative) is disproven for this reproduction.**
- **6 epilogues, all with a clean frame** about to be popped
  ({08693768/086957F8, 0, 085DC040, 0868F4B0, 085A9C54, 0, 0, 086903DC,
  <legit caller>}) — i.e. collect's push survived its body in all 6: the
  corruption is **not** a deterministic in-body stomp either; it needs a
  scheduler interleaving that tracing perturbs (the round-10 doc's
  "traced runs diverge" observation, now with a mechanism).
- **The aliasing itself was caught live in the same run** (run.log lines
  21688-21692, immediately before the 0x0023FE7C escape): four writes to the
  lr slot 0x08690384 alternating between two stack contexts —

  ```
  pc=002FE175 (thumb block @0x002FE174, lr=00278124) sp=08690380 -> [08690384]=08692720
  pc=00300C74  (ARM, lr=00100C5F)                          sp=08690388 -> [08690384]=00100C5F
  pc=00131334  (ARM)                                      sp=08690388 -> [08690384]=00100C5F
  pc=002FE175  (thumb)                                     sp=08690380 -> [08690384]=08693768
  ```

  A thread running thumb list-manipulation code (0x002FE174: `push {r3,r5,lr}`;
  node-pointer stores of r4=heap) with sp=0x08690380 — 8 bytes below the other
  thread's stack top 0x08690388 — stores heap pointers straight through the
  other thread's saved-lr slot. **Two guest threads' stacks alias at the same
  top (0x08690388/0x08690380); whichever thread's frame loses wins the garbage
  pc.** This is the same aliasing class as §4's stack-vs-heap-observation,
  observed directly between two stack frames.

So round 9 closes both of round-10's alternatives: **not a stale-lr entry, not a
mystery writer — a deterministic-content frame stomp from aliased thread
stacks**, with the record content (§4) explaining the exact popped values
including 0x005CB6E0.

**Own-run confirmation (esc9-rootsmoke_400, untraced, rc=3, line 21535):** the
exact operator signature recurred after the batch roots were installed —
`escaped to 005CB6E0 … r4-r7={00300604, C82044B4, 085BFACC, C82044B4}
lr=003E84E8 sp=08690388` — the popped record byte-identical to esc10-closefix
and round-8's fault dump (r6=085BFACC in both), with a second unblocked thread
(thread[6]) active in the dump immediately preceding the escape. Full dynamic
confirmation: collect's frame was replaced by the record between push and pop
in a plain run; roots do not and cannot fix this class (they only removed the
three unrelated callback-thunk escapes that previously masked it earlier in
runs).


## 7. Round-12 handoff

- Upstream cause of the aliasing: find what corrupted the game's allocator
  metadata or diverged the memory layout before t≈170 (prime suspects: FS/romfs
  read sizing, the one-shot romfs reads at t≈274 (handle 26, buffers
  0x086904E0/0x086275E4), exheader app-memory sizing). A heap-trace probe
  (wrap the guest's malloc entry points with DQ8_TRACE-style logging, or
  correlate svcControlMemory addresses with the two colliding allocations) will
  name the first overlap.
- Getter-thunk family enumeration (ldr-pc pattern) if a future escape lands in
  a 0x003F12xx thunk.
- The batch roots are safe to keep regardless: every rooted address is real
  code (null-check prologue / verified trampoline), and they are exactly the
  callback-pool dispatch targets the game uses.
