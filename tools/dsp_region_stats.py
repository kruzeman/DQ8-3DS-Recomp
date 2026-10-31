#!/usr/bin/env python3
"""Byte-level summary of DQ8_DSP_DUMP snapshots: non-zero counts per
shared-memory region half and the frame counters."""
import glob
import struct
import sys

REGIONS = {'region0': 0x50000, 'region1': 0x70000}
OFFS = {'status': 0x800, 'final': 0xA80, 'src_status': 0xD00,
        'dsp_config': 0x2860, 'int_mix': 0x2924, 'src_config': 0x3D24,
        'coeffs': 0x4F24, 'frame_counter': 0x7FFE}


def summarize(path):
    d = open(path, 'rb').read()
    print('==', path)
    for name, base in REGIONS.items():
        seg = d[base:base + 0x8000]
        nz = sum(1 for b in seg if b)
        fc = struct.unpack_from('<H', d, base + OFFS['frame_counter'])[0]
        print('  %s non-zero bytes=%d frame_counter=0x%04X' % (name, nz, fc))
        for sname in ('src_status', 'dsp_config', 'src_config', 'final', 'coeffs'):
            o = base + OFFS[sname]
            size = {'src_status': 24 * 12, 'dsp_config': 196,
                    'src_config': 24 * 192, 'final': 640, 'coeffs': 768}[sname]
            sseg = d[o:o + size]
            snz = sum(1 for b in sseg if b)
            if snz:
                print('    %s non-zero=%d/%d first bytes=%s' %
                      (sname, snz, size, sseg[:24].hex()))


if __name__ == '__main__':
    for p in sys.argv[1:]:
        summarize(p)
