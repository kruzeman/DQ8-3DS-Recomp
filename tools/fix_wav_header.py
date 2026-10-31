#!/usr/bin/env python3
"""Repair the RIFF/data sizes of a WAV captured by a run whose process died
without running atexit (the runtime aborts on an untranslated guest PC).  The
payload itself is intact; only the 8 header bytes need to match the file.

usage: tools/fix_wav_header.py <file.wav>
"""
import os
import struct
import sys


def main(path):
    size = os.path.getsize(path)
    if size < 44:
        raise SystemExit('%s too small (%d)' % (path, size))
    rate = 32728
    channels = 2
    data_bytes = size - 44
    header = struct.pack('<4sI4s4sIHHIIHH4sI', b'RIFF', size - 8, b'WAVE',
                         b'fmt ', 16, 1, channels, rate, rate * channels * 2,
                         channels * 2, 16, b'data', data_bytes)
    with open(path, 'r+b') as fh:
        fh.seek(0)
        magic = fh.read(4)
        if magic != b'RIFF':
            raise SystemExit('%s is not a RIFF file' % path)
        fh.seek(0)
        fh.write(header)
    print('repaired %s: %d bytes, data=%d' % (path, size, data_bytes))


if __name__ == '__main__':
    main(sys.argv[1])
