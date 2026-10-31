#!/usr/bin/env python3
"""Scan a 512 KB DSP shared-memory dump: for both regions (0x00000 / 0x20000
inside the file, matching guest phys 0x1FF50000 / 0x1FF70000), print the
dirty word + first fields of every 192-byte SourceConfiguration slot at
+0x800..+0x2000, plus the words at the other known offsets.

usage: scan_dsp_layout.py <dsp_XXXX.bin>
"""
import struct
import sys

path = sys.argv[1]
with open(path, 'rb') as f:
    blob = f.read()
print('%s: %d bytes' % (path, len(blob)))


def r16(off):
    return struct.unpack_from('<H', blob, off)[0]


def r32(off):
    return struct.unpack_from('<I', blob, off)[0]


def f32(off):
    return struct.unpack_from('<f', blob, off)[0]


for name, base in (('R0', 0x00000), ('R1', 0x20000)):
    if base + 0x2000 > len(blob):
        continue
    print('--- %s (base +%05X) ---' % (name, base))
    for i in range(24):
        cfg = base + 0x800 + i * 192
        dirty = r32(cfg)
        if dirty == 0:
            continue
        g = [f32(cfg + 4 + j * 4) for j in range(4)]
        rate = f32(cfg + 52)
        en = blob[cfg + 160]
        addr = r32(cfg + 172)
        alen = r32(cfg + 176)
        # u32_dsp (middle-endian) versions
        addr_d = ((addr << 16) | (addr >> 16)) & 0xFFFFFFFF
        alen_d = ((alen << 16) | (alen >> 16)) & 0xFFFFFFFF
        print('slot %2d @+%04X dirty=%08X gains[0..1]=%.3f,%.3f rate=%.3f en=%d '
              'addr=%08X(len_d=%d) addr_d=%08X(len=%d)'
              % (i, 0x800 + i * 192, dirty, g[0], g[1], rate, en,
                 addr, alen, addr_d, alen_d))
    # other landmarks
    for label, off in (('STATUS@0x800', 0x800), ('FINAL@0xA80', 0xA80),
                       ('SRC_STATUS@0xD00', 0xD00), ('APP_DCFG@0xE20', 0xE20),
                       ('DSP_CFG@0x2860', 0x2860), ('COEFFS@0x2924', 0x2924),
                       ('FRAMECTR@0x7FFE', 0x7FFE)):
        v = r16(base + off)
        v32 = r32(base + off)
        print('%s: w=%04X dw=%08X' % (label, v, v32))
