#!/usr/bin/env python3
"""Map guest FS:Read traces to files in a decrypted 3DS RomFS image."""

import argparse
import re
import struct
from collections import defaultdict
from pathlib import Path


IVFC_LEVEL3_OFFSET = 0x1000
ROMFS_HEADER_SIZE = 0x28
NIL = 0xFFFFFFFF


def read_exact_at(file, offset, size):
    file.seek(offset)
    data = file.read(size)
    if len(data) != size:
        raise ValueError(f"truncated RomFS at file offset 0x{offset:X}")
    return data


def romfs_files(image_path):
    """Return (path, logical_offset, size) entries from IVFC Level 3."""
    with image_path.open("rb") as image:
        header = read_exact_at(image, IVFC_LEVEL3_OFFSET, ROMFS_HEADER_SIZE)
        fields = struct.unpack("<10I", header)
        header_size, dir_hash_off, dir_hash_size, dir_meta_off, dir_meta_size, \
            file_hash_off, file_hash_size, file_meta_off, file_meta_size, file_data_off = fields

        if header_size != ROMFS_HEADER_SIZE:
            raise ValueError(f"invalid RomFS header size 0x{header_size:X}")
        if dir_hash_off + dir_hash_size > dir_meta_off:
            raise ValueError("directory hash table overlaps directory metadata")
        if dir_meta_off + dir_meta_size > file_hash_off:
            raise ValueError("directory metadata overlaps file hash table")
        if file_hash_off + file_hash_size > file_meta_off:
            raise ValueError("file hash table overlaps file metadata")
        if file_meta_off + file_meta_size > file_data_off:
            raise ValueError("file metadata overlaps file data")

        dirs = read_exact_at(image, IVFC_LEVEL3_OFFSET + dir_meta_off, dir_meta_size)
        files = read_exact_at(image, IVFC_LEVEL3_OFFSET + file_meta_off, file_meta_size)

    def directory(offset):
        if offset < 0 or offset + 0x18 > len(dirs):
            raise ValueError(f"directory metadata offset out of range: 0x{offset:X}")
        parent, sibling, child_dir, child_file, next_hash, name_size = struct.unpack_from(
            "<6I", dirs, offset
        )
        end = offset + 0x18 + name_size
        if name_size & 1 or end > len(dirs):
            raise ValueError(f"invalid directory name at metadata offset 0x{offset:X}")
        name = dirs[offset + 0x18:end].decode("utf-16le", "replace")
        return parent, sibling, child_dir, child_file, next_hash, name

    def file_entry(offset):
        if offset < 0 or offset + 0x20 > len(files):
            raise ValueError(f"file metadata offset out of range: 0x{offset:X}")
        parent, sibling, data_offset, data_size, next_hash, name_size = struct.unpack_from(
            "<2I2Q2I", files, offset
        )
        end = offset + 0x20 + name_size
        if name_size & 1 or end > len(files):
            raise ValueError(f"invalid file name at metadata offset 0x{offset:X}")
        name = files[offset + 0x20:end].decode("utf-16le", "replace")
        return parent, sibling, data_offset, data_size, next_hash, name

    result = []
    visited_dirs = set()

    def walk(offset, parent_path):
        if offset in visited_dirs:
            raise ValueError(f"directory tree cycle at metadata offset 0x{offset:X}")
        visited_dirs.add(offset)
        _, _, child_dir, child_file, _, name = directory(offset)
        current_path = parent_path if offset == 0 else "/".join(
            part for part in (parent_path, name) if part
        )

        file_offset = child_file
        visited_files = set()
        while file_offset != NIL:
            if file_offset in visited_files:
                raise ValueError(f"file sibling cycle at metadata offset 0x{file_offset:X}")
            visited_files.add(file_offset)
            _, sibling, data_offset, data_size, _, file_name = file_entry(file_offset)
            path = "/".join(part for part in (current_path, file_name) if part)
            result.append((path, file_data_off + data_offset, data_size))
            file_offset = sibling

        dir_offset = child_dir
        visited_siblings = set()
        while dir_offset != NIL:
            if dir_offset in visited_siblings:
                raise ValueError(f"directory sibling cycle at metadata offset 0x{dir_offset:X}")
            visited_siblings.add(dir_offset)
            _, sibling, _, _, _, _ = directory(dir_offset)
            walk(dir_offset, current_path)
            dir_offset = sibling

    walk(0, "")
    return result


def trace_reads(log_path):
    pattern = re.compile(
        r"FS Read handle=\w+ offset=([0-9A-Fa-f]+):([0-9A-Fa-f]+) "
        r"size=([0-9A-Fa-f]+)"
    )
    result = []
    with log_path.open(errors="replace") as log:
        for line in log:
            match = pattern.search(line)
            if match:
                high, low, size = (int(value, 16) for value in match.groups())
                result.append(((high << 32) | low, size))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("romfs", type=Path, help="decrypted IVFC RomFS image used by the runner")
    parser.add_argument("log", type=Path, help="runner log containing FS Read trace lines")
    args = parser.parse_args()

    files = sorted(romfs_files(args.romfs), key=lambda item: item[1])
    reads = trace_reads(args.log)
    totals = defaultdict(lambda: [0, 0])
    unmatched = 0

    for offset, size in reads:
        lo, hi = 0, len(files)
        while lo < hi:
            mid = (lo + hi) // 2
            if files[mid][1] <= offset:
                lo = mid + 1
            else:
                hi = mid
        match = files[lo - 1] if lo else None
        if match and offset < match[1] + match[2]:
            name, file_offset, file_size = match
            totals[name][0] += 1
            totals[name][1] += size
        else:
            unmatched += 1

    print(f"FS:Read calls: {len(reads)}; mapped files: {len(totals)}; unmatched: {unmatched}")
    for name, (count, byte_count) in sorted(totals.items()):
        print(f"{name}\treads={count}\tbytes=0x{byte_count:X}")


if __name__ == "__main__":
    main()
