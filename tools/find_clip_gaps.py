#!/usr/bin/env python3
"""Locate clipped samples and produced-silence runs inside a captured WAV.

Distinguishes the two remaining artifact classes:
  * clipped samples (|s| >= 32767) with their time positions,
  * runs of consecutive all-zero DSP frames (160-sample blocks) = audible
    dropouts the runtime actually emitted (late-refill starvation).
usage: tools/find_clip_gaps.py <file.wav> [t0] [t1]
"""
import array
import struct
import sys

path = sys.argv[1]
t0 = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0
t1 = float(sys.argv[3]) if len(sys.argv) > 3 else 1e9

with open(path, 'rb') as f:
    data = f.read()
pos, fmt, pcm = 12, None, None
while pos + 8 <= len(data):
    cid = data[pos:pos + 4]
    csz = struct.unpack('<I', data[pos + 4:pos + 8])[0]
    body = data[pos + 8:pos + 8 + csz]
    if cid == b'fmt ':
        fmt = struct.unpack('<HHIIHH', body[:16])
    elif cid == b'data':
        pcm = body
    pos += 8 + csz + (csz & 1)
rate, ch = fmt[2], fmt[1]
x = array.array('h')
x.frombytes(pcm)
n = len(x) // ch

i0 = max(0, int(t0 * rate))
i1 = min(n, int(t1 * rate))

# Clipped samples with positions (grouped into 10ms clusters).
clusters = []
run_start = None
for i in range(i0, i1):
    clipped = abs(x[i * ch]) >= 32767 or (ch > 1 and abs(x[i * ch + 1]) >= 32767)
    if clipped and run_start is None:
        run_start = i
    elif not clipped and run_start is not None:
        clusters.append((run_start, i))
        run_start = None
if run_start is not None:
    clusters.append((run_start, i1))
total = sum(b - a for a, b in clusters)
print('clip clusters (>=1 consecutive clipped frame), total clipped frames=%d:' % total)
for a, b in clusters[:40]:
    print('  %.3fs .. %.3fs  frames=%d' % (a / rate, b / rate, b - a))
if len(clusters) > 40:
    print('  ... %d more clusters' % (len(clusters) - 40))

# Runs of consecutive all-zero 160-sample DSP frames inside the window.
frame = 160
gaps = []
run = None
for f in range(i0 // frame, (i1 + frame - 1) // frame):
    base = f * frame
    silent = True
    for s in range(frame):
        i = base + s
        if i >= n:
            silent = False
            break
        if x[i * ch] != 0 or (ch > 1 and x[i * ch + 1] != 0):
            silent = False
            break
    if silent and run is None:
        run = f
    elif not silent and run is not None:
        gaps.append((run, f))
        run = None
if run is not None:
    gaps.append((run, (i1 + frame - 1) // frame))
print('produced-silence runs (consecutive all-zero DSP frames), count=%d:' % len(gaps))
for a, b in gaps[:40]:
    print('  %.3fs .. %.3fs  frames=%d (%.1f ms)' %
          (a * frame / rate, b * frame / rate, b - a, (b - a) * frame * 1000.0 / rate))
if len(gaps) > 40:
    print('  ... %d more runs' % (len(gaps) - 40))
