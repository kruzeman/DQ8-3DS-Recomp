#!/usr/bin/env python3
"""List and sanity-check code pointers inside a DQ8.code data window.

usage: tools/scan_fn_table.py 0x0059CC00 0x0059CE00
Prints each aligned word that looks like a .text pointer together with the
first decoded instruction so the entries can be validated before being added
to recomp-roots.txt.
"""
import struct
import subprocess
import sys

BASE = 0x00100000
TEXT_END = 0x0053BCB4
DQ8_CODE = '/home/hermes/DQ8/DQ8.code'


def first_insn(addr):
    off = addr - BASE
    with open(DQ8_CODE, 'rb') as fh:
        fh.seek(off)
        data = fh.read(4)
    with open('/tmp/dq8-sess/scan.bin', 'wb') as fh:
        fh.write(data)
    out = subprocess.run(['/home/hermes/DQ8/target/release/dq8tool', 'disasm',
                          '/tmp/dq8-sess/scan.bin', '--mode', 'arm',
                          '--base', '0x%08X' % addr, '--count', '1'],
                         capture_output=True, text=True)
    return out.stdout.strip().split('\n')[0] if out.stdout.strip() else '(no decode)'


def main(start, end):
    start = int(start, 16)
    end = int(end, 16)
    with open(DQ8_CODE, 'rb') as fh:
        fh.seek(start - BASE)
        data = fh.read(end - start)
    for off in range(0, len(data) - 3, 4):
        v = struct.unpack_from('<I', data, off)[0]
        if BASE <= v < TEXT_END and v % 4 == 0:
            print('0x%08X: 0x%08X  %s' % (start + off, v, first_insn(v)))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
