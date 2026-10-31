#!/usr/bin/env python3
"""Report PICA command-list register writes from a GSP ProcessCommandList dump."""

import collections
import argparse
import struct
from pathlib import Path


def parse(path: Path):
    data = path.read_bytes()
    if len(data) % 8:
        raise ValueError(f"command list size must be 8-byte aligned: {len(data)}")
    words = struct.unpack(f"<{len(data) // 4}I", data)
    offset = 0
    writes = collections.Counter()
    triggers = []
    records = 0

    while offset < len(words):
        # PICA commands start at an 8-byte boundary. Each record is a value,
        # header, then header.extra_data_length additional values.
        offset += offset & 1
        if offset + 1 >= len(words):
            break
        value, header = words[offset : offset + 2]
        first_word = offset
        offset += 2
        reg = header & 0xFFFF
        extra = (header >> 20) & 0x7FF
        group = (header >> 31) & 1
        if offset + extra > len(words):
            raise ValueError(
                f"truncated PICA record at byte {first_word * 4:#x}: "
                f"reg={reg:#x} extra={extra} remaining={len(words)-offset}"
            )
        for i in range(extra + 1):
            reg_id = reg + group * i
            reg_value = value if i == 0 else words[offset + i - 1]
            writes[reg_id] += 1
            if reg_id in (0x22E, 0x22F):
                triggers.append((first_word * 4, reg_id, reg_value))
        offset += extra
        records += 1
    return len(data), records, writes, triggers


def report_one(path: Path) -> None:
    size, records, writes, triggers = parse(path)
    print(f"bytes={size} records={records} register_ids={len(writes)}")
    print("most-written registers:")
    for reg, count in writes.most_common(24):
        print(f"  {reg:03X}: {count}")
    print("draw triggers (0x22E arrays / 0x22F indexed):")
    if triggers:
        for byte_offset, reg, value in triggers:
            print(f"  byte={byte_offset:#x} reg={reg:#05x} value={value:#010x}")
    else:
        print("  none")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("lists", nargs="+", type=Path, help="one or more PICA command-list dumps")
    args = parser.parse_args()
    if len(args.lists) == 1:
        report_one(args.lists[0])
        return 0

    print("file\tbytes\trecords\tregister_ids\tdraw_triggers")
    for path in args.lists:
        size, records, writes, triggers = parse(path)
        draw_text = ",".join(f"{reg:#05x}={value:#010x}" for _, reg, value in triggers) or "none"
        print(f"{path}\t{size}\t{records}\t{len(writes)}\t{draw_text}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
