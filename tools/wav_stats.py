#!/usr/bin/env python3
"""Analyze a WAV: length, peak dBFS, clipped counts, non-silent windows."""
import struct
import sys
import math
import wave


def analyze(path):
    with wave.open(path, 'rb') as w:
        n = w.getnframes()
        ch = w.getnchannels()
        sw = w.getsampwidth()
        rate = w.getframerate()
        raw = w.readframes(n)
    if sw != 2:
        print('sample width', sw)
        return
    samples = struct.unpack('<%dh' % (len(raw) // 2), raw)
    peak = max(max(samples), -min(samples))
    clipped_pos = sum(1 for s in samples if s >= 32767)
    clipped_neg = sum(1 for s in samples if s <= -32768)
    print(f'{path}: frames={n} ch={ch} rate={rate} dur={n/rate:.2f}s '
          f'peak={peak} ({20*math.log10(peak/32768) if peak else -999:.1f} dBFS) '
          f'clipped+= {clipped_pos} clipped-= {clipped_neg}')
    # non-silent windows (0.1 s) per channel-mixed max
    win = int(rate * 0.1)
    loud = []
    for i in range(0, n, win):
        seg = samples[i * ch:(i + win) * ch]
        if not seg:
            break
        m = max(max(seg), -min(seg))
        if m > 32:
            loud.append((i / rate, m, 20 * math.log10(m / 32768)))
    print(f'non-silent 0.1s windows (|x|>32): {len(loud)}')
    for t, m, db in loud[:40]:
        print(f'  t={t:7.2f}s peak={m:6d} ({db:6.1f} dBFS)')


for p in sys.argv[1:]:
    analyze(p)
