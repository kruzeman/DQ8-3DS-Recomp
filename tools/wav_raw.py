#!/usr/bin/env python3
"""Dump raw interleaved samples of a window (stride optional).
usage: wav_raw.py <file.wav> <t0> <t1> [stride] [channel]"""
import struct
import sys
import wave

path, t0, t1 = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
stride = int(sys.argv[4]) if len(sys.argv) > 4 else 8
ch_sel = int(sys.argv[5]) if len(sys.argv) > 5 else None
with wave.open(path, 'rb') as w:
    rate = w.getframerate()
    ch = w.getnchannels()
    n = w.getnframes()
    s = struct.unpack('<%dh' % (n * ch), w.readframes(n))
i0, i1 = int(t0 * rate) * ch, int(t1 * rate) * ch
vals = s[i0:i1]
if ch_sel is not None:
    vals = vals[ch_sel::ch]
for k in range(0, len(vals), stride):
    print('%8.4f %6d' % ((i0 + k) / rate / ch if ch_sel is None else (i0 // ch + k / ch), vals[k]), end='   ')
    if (k // stride) % 6 == 5:
        print()
print()
