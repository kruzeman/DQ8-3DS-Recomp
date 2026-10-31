#!/usr/bin/env python3
"""List the loudest 10 ms windows of a WAV with timestamps."""
import struct
import sys
import math
import wave

path = sys.argv[1]
with wave.open(path, 'rb') as w:
    rate = w.getframerate()
    ch = w.getnchannels()
    n = w.getnframes()
    s = struct.unpack('<%dh' % (n * ch), w.readframes(n))
left = s[0::ch]
win = rate // 100
env = []
for i in range(0, n - win + 1, win):
    seg = left[i:i + win]
    rms = math.sqrt(sum(v * v for v in seg) / len(seg))
    pk = max(abs(v) for v in seg)
    env.append((rms, pk, i / rate))
env.sort(reverse=True)
print('top 15 windows by RMS:')
for rms, pk, t in env[:15]:
    print('  t=%7.3f rms=%6d (%6.1f dBFS) peak=%6d (%6.1f dBFS)'
          % (t, rms, 20 * math.log10(rms / 32768) if rms else -120,
             pk, 20 * math.log10(pk / 32768) if pk else -120))
# count non-zero samples overall and their time distribution
nz = sum(1 for v in left if v)
print('left-channel nonzero samples: %d / %d' % (nz, n))
