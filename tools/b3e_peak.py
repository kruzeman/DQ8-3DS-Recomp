#!/usr/bin/env python3
"""Peak of a time-window in a WAV (default: the 22.3-22.4s jingle event)."""
import math
import sys
import wave
import struct

wav_path = sys.argv[1]
lo = float(sys.argv[2]) if len(sys.argv) > 2 else 22.30
hi = float(sys.argv[3]) if len(sys.argv) > 3 else 22.45

w = wave.open(wav_path)
rate, n, ch = w.getframerate(), w.getnframes(), w.getnchannels()
data = struct.unpack('<%dh' % (n * ch), w.readframes(n))
best, at = 0, 0.0
start, end = int(lo * rate) * ch, min(int(hi * rate) * ch, len(data))
for i in range(start, end, ch):
    v = abs(data[i])
    if v > best:
        best, at = v, i / ch / rate
db = 20 * math.log10(best / 32768.0) if best else -120.0
print("window %.2f..%.2fs peak=%d at t=%.3fs (%.1f dBFS)" % (lo, hi, best, at, db))
