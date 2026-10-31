#!/usr/bin/env python3
"""10 ms RMS/peak envelope dump of a time window of a WAV.
usage: wav_env.py <file.wav> <t0> <t1>"""
import struct
import sys
import math
import wave

path, t0, t1 = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
with wave.open(path, 'rb') as w:
    rate = w.getframerate()
    ch = w.getnchannels()
    n = w.getnframes()
    s = struct.unpack('<%dh' % (n * ch), w.readframes(n))
left = s[0::ch]
win = rate // 100
i0, i1 = int(t0 * rate), int(t1 * rate)
print('rate=%d window=%d samples (%.0f ms)' % (rate, win, 1000.0 * win / rate))
i = i0
while i < i1:
    j = min(i + win, len(left))
    seg = left[i:j]
    if seg:
        rms = math.sqrt(sum(v * v for v in seg) / len(seg))
        pk = max(abs(v) for v in seg)
        nz = sum(1 for v in seg if v)
        print('t=%7.3f  rms=%6d (%6.1f dBFS)  peak=%6d (%6.1f dBFS)  nz=%d/%d'
              % (i / rate, rms, 20 * math.log10(rms / 32768.0) if rms else -120,
                 pk, 20 * math.log10(pk / 32768.0) if pk else -120, nz, len(seg)))
    i += win
