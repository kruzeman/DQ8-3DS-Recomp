#!/usr/bin/env python3
"""Cut byte slices of DQ8.code around the PC-base-rule candidates so an
authoritative decoder (llvm-objdump via llvm-objcopy) can say which candidate
address is a real Thumb instruction boundary.

slices written next to the script:
  slice_1940.bin  guest 0x00101900..0x00101980  (align -> 0x00101943, noalign -> 0x00101945)
  slice_0ee0.bin  guest 0x00100EA0..0x00100F20  (align -> 0x00100EE1, noalign -> 0x00100EE3)
"""
import os
import sys

CODE = "/home/hermes/DQ8/DQ8.code"
BASE = 0x00100000
OUTDIR = os.path.dirname(os.path.abspath(__file__))

SLICES = {
    "slice_1940.bin": (0x00101900, 0x00101980),
    "slice_0ee0.bin": (0x00100EA0, 0x00100F20),
}

data = open(CODE, "rb").read()
for name, (lo, hi) in SLICES.items():
    blob = data[lo - BASE:hi - BASE]
    path = os.path.join(OUTDIR, name)
    with open(path, "wb") as fh:
        fh.write(blob)
    print("%s: guest 0x%08X..0x%08X (%d bytes) -> %s" % (name, lo, hi, len(blob), path))
