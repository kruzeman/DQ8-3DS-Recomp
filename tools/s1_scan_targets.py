#!/usr/bin/env python3
"""Static scan: who branches (B/BL) into the candidate region entries/interiors?

Decides whether extending a region entry backwards (subsuming a lead-in block)
can lose hook firings: if outside code branches directly to an interior pc that
was a previous region entry, the extended region's entry hook no longer fires
there (dispatch resumes mid-region).  Purely static; read-only on DQ8.code.
"""
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CODE = ROOT / "DQ8.code"
BASE = 0x00100000

TARGETS = {
    0x001A94F4: "A-entry(R1)",
    0x001A99FC: "B-entry(R1)",
    0x001A96CC: "C-entry(R1)",
    0x001A9904: "D-entry(R1)",
    0x001A94F0: "A-lead ldmia",
    0x001A96C8: "C-lead ldmia",
    0x001A99F8: "B-lead ldmia",
    0x001A93A8: "S1c entry",
    0x001A9428: "extA entry",
    0x001A99A8: "extB entry",
    0x00194D64: "bandC-1 entry",
    0x00195D44: "bandC-2 entry",
}

# regions whose interiors count as "internal" for the external check
INTERNAL = [
    (0x001A93A8, 0x001A9410),   # S1c
    (0x001A9428, 0x001A9608),   # extended A
    (0x001A99A8, 0x001A9A64),   # extended B
    (0x001A96C8, 0x001A97B8),   # extended C
    (0x001A9904, 0x001A9990),   # D
    (0x00194D64, 0x001954B8),   # band C run 1
    (0x00195D44, 0x001965FC),   # band C run 2
]


def is_internal(addr):
    return any(lo <= addr < hi for lo, hi in INTERNAL)


def main():
    data = CODE.read_bytes()
    n = len(data) // 4
    hits = {k: [] for k in TARGETS}
    for i in range(n):
        w = struct.unpack_from("<I", data, i * 4)[0]
        if (w & 0x0E000000) == 0x0A000000 and ((w >> 28) & 0xF) != 0xF:
            off = w & 0x00FFFFFF
            if off & 0x00800000:
                off -= 0x01000000
            tgt = BASE + i * 4 + 8 + off * 4
            if tgt in hits:
                hits[tgt].append(BASE + i * 4)
    for k, label in TARGETS.items():
        srcs = hits[k]
        ext = [s for s in srcs if not is_internal(s)]
        print(f"0x{k:08X} {label:14s} branches={len(srcs):3d} "
              f"external={len(ext):3d} {[hex(s) for s in ext[:8]]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
