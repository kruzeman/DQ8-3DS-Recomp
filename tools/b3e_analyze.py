#!/usr/bin/env python3
"""B3e capture analysis: per-second continuity + jingle peak + silence runs."""
import math
import re
import sys
import wave
import struct

wav_path = sys.argv[1]
analysis_txt = sys.argv[2] if len(sys.argv) > 2 else None

if analysis_txt:
    body = open(analysis_txt).read()
    m = re.search(r'per-second RMS \(dBFS\):(.*)', body, re.S)
    secs = re.findall(r'^\s*t=\s*(\d+)s\s+(-?\d+\.\d+|-?inf)', m.group(1), re.M) if m else []
    vals = [(int(a), float(b.replace('-inf', '-999'))) for a, b in secs]
    print("seconds analyzed:", len(vals))
    after = [(t, v) for t, v in vals if 31 <= t <= 460]
    silent = [t for t, v in after if v <= -60.0]
    print("t=31..460:", len(after), "seconds; silent(<=-60dBFS):", len(silent),
          silent[:20])
    run = best = 0
    for t, v in after:
        if v <= -60.0:
            run += 1
            best = max(best, run)
        else:
            run = 0
    print("longest silent run in t=31..460:", best, "s")
    # full-length coverage check: any second with NO audio at all after t=31
    have = set(t for t, v in after)
    missing = [t for t in range(31, 461) if t not in have]
    print("seconds missing from analysis after t=31:", missing[:20])

w = wave.open(wav_path)
rate, n, ch = w.getframerate(), w.getnframes(), w.getnchannels()
data = struct.unpack('<%dh' % (n * ch), w.readframes(n))
print("duration=%.2fs" % (n / rate))

def peak_in(lo, hi):
    best, at = 0, 0.0
    for i in range(int(lo * rate) * ch, min(int(hi * rate) * n // n * ch if False else int(hi * rate) * ch, len(data)), ch):
        v = abs(data[i])
        if v > best:
            best, at = v, i / ch / rate
    return best, at

# jingle window: prior runs place it ~25.06-25.09s (fresh save)
p, at = peak_in(20.0, 31.0)
db = 20 * math.log10(p / 32768.0) if p else -120.0
print("jingle-window 20-31s peak=%d at t=%.2fs (%.1f dBFS)" % (p, at, db))
# whole-file peak for reference
p2, at2 = peak_in(0.0, n / rate)
print("whole-file peak=%d at t=%.2fs" % (p2, at2))
