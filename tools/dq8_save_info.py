#!/usr/bin/env python3
"""dq8_save_info.py — read the DQ8 3DS save (honeySave.bin) and print the
per-slot metadata: play time, hero name and location.

    ./dq8_save_info.py honeySave.bin

Why this exists: the in-game "Continue your adventure" screen currently shows
"Time played 0:00" on our builds (save-slot display regression, task
t_1a408742), but the values ARE in the file.  This reads them straight out, so
game time can still be tracked while that card is open.

Layout (verified against the operator's save, md5 7c204014284e09452136451958bcb7b6):
  each slot occupies 0x10000 bytes from 0x00000 / 0x10000 / 0x20000
  within a slot:
    +0x0AC         u32  play time in SECONDS (479 -> the game shows "0:07")
    +0x0B4         u16  location name length in UTF-16 code units (with NUL)
    +0x0B8         char16 location name, UTF-16LE, NUL terminated
    +0x0A0/+0x0A4  u32 pair, both 2 in a used slot
The hero name is stored the same way a little earlier; it is printed when the
pattern is unambiguous.
"""
import struct
import sys

SLOT_SIZE = 0x10000
SLOT_TIME = 0x2CA0      # u32 seconds, absolute within the slot block
SLOT_NAME = 0x2CB8      # UTF-16LE, NUL terminated
SLOT_STAMP = 0x2CA4     # u32, looks like a packed date/time stamp


def utf16_at(data: bytes, off: int, maxlen: int = 64) -> str:
    out = []
    for i in range(maxlen):
        if off + 2 * i + 1 >= len(data):
            break
        ch = struct.unpack_from('<H', data, off + 2 * i)[0]
        if ch == 0:
            break
        out.append(chr(ch))
    return ''.join(out)


def fmt_time(seconds: int) -> str:
    # The game prints hours:minutes, so 479 s appears as 0:07 (7:59 rounded down).
    return f"{seconds // 3600}:{(seconds % 3600) // 60:02d}"


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    data = open(sys.argv[1], 'rb').read()
    print(f"file: {sys.argv[1]}  ({len(data)} bytes)")
    any_slot = False
    for slot in range(3):
        base = slot * SLOT_SIZE
        if base + SLOT_SIZE > len(data):
            break
        secs = struct.unpack_from('<I', data, base + SLOT_TIME)[0]
        name = utf16_at(data, base + SLOT_NAME)
        used = (name != '') or 0 < secs < 100000
        if not used:
            print(f"slot {slot + 1}: unused")
            continue
        any_slot = True
        print(f"slot {slot + 1}: time played {fmt_time(secs)}  ({secs} s)"
              f"   location: {name or '?'}")
        # Best-effort hero name: the first printable UTF-16 string in the header.
        for off in range(0x20, 0xA0, 2):
            s = utf16_at(data, base + off, 16)
            if s and all(32 <= ord(c) < 127 for c in s) and s != name and len(s) <= 12:
                print(f"          hero name candidate: {s!r}")
                break
    if not any_slot:
        print("no used slot found")
    return 0


if __name__ == '__main__':
    sys.exit(main())
