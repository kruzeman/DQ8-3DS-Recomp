# Post-logo runtime fixes, 2026-10-19

Branch: codex/fix-apt-home-resume. The next visual milestone remains the actual
in-game save-creation prompt, followed by two separate A presses.

Fixed three confirmed runtime defects:

1. WaitSynchronizationN (SVC 0x25) timeout is low r0/high r4, as the game's
   veneer at 0x0011D3E8 proves. r0 is not a guest output pointer, and the
   current stack slots are not the timeout. The old implementation returned an
   invalid-pointer error for an infinite wait and left a stale result index;
   input-reader thread 10 consequently exited. The scheduler now retains the
   whole wait set, wakes on any/all conditions, returns the first ready index,
   consumes auto-reset events, and reports finite timeout on resumption.
2. File handles from OpenFileDirectly and OpenFile previously started at the
   same 0x20 independently. Save-file opens overwrote live RomFS flags, causing
   subsequent asset reads to return invented zeros. Files/directories/events
   now use the same allocation counter; directory dispatch checks object type.
3. ArbitrateAddress Signal's value is the number of threads to wake, not a
   value to add to memory. The old implementation changed a signaled manual
   LightEvent from 1 to 0 when asked to wake all (-1). Its first wait consumed
   that state and the media worker then blocked forever. Signal now leaves
   memory untouched and wakes the specified count in priority order. Plain
   put32 no longer invents arbiter wakeups. DecrementAndWait only decrements
   when its wait predicate holds; an already-satisfied predicate succeeds.

Callbacks in the two live decoder-input tables are now translation roots.
The first previously missing callbacks were 0x002FD770 (file read),
0x0023AB5C / 0x0023AFFC (input interface), and 0x00488280/0x00488290.
These are real translated instructions, not replacements for the guest SDK.

Validation: 34 Rust tests, native APT test, native wait/arbiter/handle test,
and two native PICA tests under ASan/UBSan pass. The new fixture checks the
second event wakes a waiting child with index 1, wait-all atomic acquisition,
finite timeout, file-handle isolation, Signal preserving memory, count zero,
one/highest-priority wake, wake-all, and conditional decrement.

Real-raster logs: /tmp/dq8-waitn-run/run2.err through run8.err. The input reader
now stays alive and reads 64 KiB blocks at RomFS offset 0x743C6520; the source
starts 4C32 (Moflex). The media worker progresses past the former event stall
into decoder code. run8 stops at an as-yet ungenerated callback 0x00460324.
The latest top/bottom captures are black; this does NOT establish the save
prompt or title screen. DQ8_DUMP_TRANSFER_LATEST=1 limits raw captures to the
latest top/bottom PPM and avoids filling /tmp.

Still separate: the existing FS stubs fabricate honeySave.bin, zeroed reads
and endless directory entries. Those must be replaced with actual absent-file
errors and consistent create/write/read state to validate a fresh-save path.
