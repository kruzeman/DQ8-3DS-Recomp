#!/usr/bin/env python3
"""Read little-endian words from DQ8.code at given guest addresses (probe)."""
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASE = 0x00100000
data = (ROOT / "DQ8.code").read_bytes()
for arg in sys.argv[1:]:
    a = int(arg, 16)
    w = struct.unpack_from("<I", data, a - BASE)[0]
    print(f"0x{a:08X}: 0x{w:08X}")
