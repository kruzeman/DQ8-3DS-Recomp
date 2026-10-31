#!/usr/bin/env python3
"""Extract one file from the decrypted 3DS RomFS image (target/dq8.romfs).

Reuses the same IVFC Level 3 parsing as tools/romfs_read_report.py so the
movie/pipeline work can inspect real guest file contents offline.

usage: tools/romfs_extract.py <romfs-image> <romfs/path> <output>
"""
import struct
import sys
from pathlib import Path

IVFC_LEVEL3_OFFSET = 0x1000
ROMFS_HEADER_SIZE = 0x28


def read_exact_at(fh, offset, size):
    fh.seek(offset)
    data = fh.read(size)
    if len(data) != size:
        raise ValueError("truncated RomFS at file offset 0x%X" % offset)
    return data


def entries(image_path):
    with image_path.open("rb") as image:
        header = read_exact_at(image, IVFC_LEVEL3_OFFSET, ROMFS_HEADER_SIZE)
        (header_size, dir_hash_off, dir_hash_size, dir_meta_off, dir_meta_size,
         file_hash_off, file_hash_size, file_meta_off, file_meta_size,
         file_data_off) = struct.unpack("<10I", header)
        dirs = read_exact_at(image, IVFC_LEVEL3_OFFSET + dir_meta_off, dir_meta_size)
        files = read_exact_at(image, IVFC_LEVEL3_OFFSET + file_meta_off, file_meta_size)

    def directory(offset):
        parent, sibling, child_dir, child_file, next_hash, name_size = struct.unpack_from(
            "<6I", dirs, offset)
        name = dirs[offset + 0x18:offset + 0x18 + name_size].decode("utf-16le", "replace")
        return parent, sibling, child_dir, child_file, name

    def file_entry(offset):
        parent, sibling, name_size, _, data_off_lo, data_off_hi, data_size_lo, data_size_hi = \
            struct.unpack_from("<8I", files, offset)
        name = files[offset + 0x20:offset + 0x20 + name_size].decode("utf-16le", "replace")
        data_off = data_off_lo | (data_off_hi << 32)
        data_size = data_size_lo | (data_size_hi << 32)
        return sibling, name, data_off, data_size

    # Walk the directory tree to build full paths.
    results = {}
    stack = [(0, "")]
    while stack:
        dir_off, prefix = stack.pop()
        _, _, child_dir, child_file, dname = directory(dir_off)
        base = prefix + "/" + dname if prefix or dname else ""
        f = child_file
        while f != 0xFFFFFFFF:
            sibling, name, off, size = file_entry(f)
            results[base + "/" + name] = (off, size)
            f = sibling
        d = child_dir
        while d != 0xFFFFFFFF:
            _, sibling, _, _, _ = directory(d)
            stack.append((d, base))
            d = sibling
    return results, file_data_off


def main(argv):
    image_path = Path(argv[1])
    wanted = argv[2]
    output = Path(argv[3])
    table, file_data_off = entries(image_path)
    key = wanted if wanted in table else "/" + wanted.lstrip("/")
    if key not in table:
        print("not found: %s" % wanted)
        print("closest matches:")
        for name in sorted(table):
            if wanted.rsplit("/", 1)[-1] in name:
                print("  %s" % name)
        return 1
    off, size = table[key]
    with image_path.open("rb") as image:
        data = read_exact_at(image, IVFC_LEVEL3_OFFSET + file_data_off + off, size)
    output.write_bytes(data)
    print("%s -> %s (0x%X bytes, romfs data offset 0x%X)"
          % (key, output, size, off))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
