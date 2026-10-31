#!/usr/bin/env python3
"""Inspect a Moflex (.moflex) movie file from the DQ8 RomFS.

Moflex is the 3DS streaming container (magic 4C 32 28 AE): an index of
audio/video packets, each packet carrying a compressed payload (MJPEG video
plus PCM audio for this title).  This tool only reports structure so we can
check what the guest media worker should be reading and decoding.
"""
import struct
import sys


def main(argv):
    path = argv[1]
    limit = int(argv[2], 0) if len(argv) > 2 else 8
    with open(path, "rb") as fh:
        data = fh.read()
    print("file %s size=0x%X" % (path, len(data)))
    print("first 64 bytes: %s" % data[:64].hex())
    if data[:4] != b"\x4c\x32\x28\xae":
        print("NOT a Moflex file (magic mismatch)")
        return 1
    # Header: magic(4) then a table of section offsets.
    words = struct.unpack_from("<16I", data, 4)
    print("header words after magic: %s" % " ".join("%08X" % w for w in words))
    # Walk records: many Moflex files start each packet with 0x20-byte header
    # containing type/size.  Scan for JPEG SOI markers (FFD8) to locate frames.
    frames = []
    i = 0
    while True:
        i = data.find(b"\xff\xd8\xff", i)
        if i < 0 or len(frames) > 4000:
            break
        frames.append(i)
        i += 2
    print("JPEG SOI markers found: %d" % len(frames))
    for off in frames[:limit]:
        end = data.find(b"\xff\xd9", off)
        print("  frame at 0x%08X end=0x%08X size=%s"
              % (off, end, ("0x%X" % (end - off + 2)) if end > 0 else "?"))
    if len(frames) > limit:
        print("  ... %d more" % (len(frames) - limit))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
