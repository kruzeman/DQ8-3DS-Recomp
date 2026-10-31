#!/usr/bin/env python3
"""Peak level of a sound event inside a time window of a captured WAV.

Used for the save-jingle acceptance gate (reference: -19.0 dBFS).

usage: tools/jingle_peak.py <file.wav> <t0> <t1>
"""
import math
import struct
import sys
import wave


def main(path, t0, t1):
    with wave.open(path, 'rb') as w:
        rate = w.getframerate()
        ch = w.getnchannels()
        start = int(float(t0) * rate)
        n = int((float(t1) - float(t0)) * rate)
        w.setpos(min(start, max(0, w.getnframes() - 1)))
        data = w.readframes(n)
    samples = struct.unpack('<%dh' % (len(data) // 2), data)
    peak = 0
    peak_t = float(t0)
    for i in range(0, len(samples), ch):
        v = max(abs(samples[i]), abs(samples[i + 1]) if ch > 1 else 0)
        if v > peak:
            peak = v
            peak_t = float(t0) + (i // ch) / rate
    db = 20 * math.log10(peak / 32768.0) if peak else -120.0
    print('%s window %.2f-%.2fs: peak=%d (%.1f dBFS) at t=%.3fs' %
          (path, float(t0), float(t1), peak, db, peak_t))


if __name__ == '__main__':
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    main(sys.argv[1], sys.argv[2], sys.argv[3])
