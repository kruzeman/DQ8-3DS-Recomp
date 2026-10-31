#!/usr/bin/env python3
"""Detect the intermittent buzz artifacts in a captured DSP WAV.

Finds, without numpy:
  * clipped samples (|s| >= 32767) per 100 ms window and total (acceptance:
    clipping count must be 0),
  * broadband noise bursts: 50 ms windows where the first-difference energy
    ratio is high (white/noise-like spectrum) while RMS is loud — the 0.4-0.5 s
    full-buffer garbage decodes the operator hears as buzzing,
  * short digital-silence dropouts (<30 ms) between loud windows (clicks),
  * per-second RMS in dBFS for the continuity check.

usage: tools/analyze_buzz_wav.py <file.wav>
"""
import array
import math
import struct
import sys


def read_wav(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:4] != b'RIFF' or data[8:12] != b'WAVE':
        raise SystemExit('not a RIFF/WAVE file: %s' % path)
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
    return rate, ch, x


def main(path):
    rate, ch, x = read_wav(path)
    n = len(x) // ch
    print('file=%s rate=%d ch=%d duration=%.3fs frames=%d' % (path, rate, ch, n / rate, n))
    peak = max(abs(v) for v in x)
    clips = sum(1 for v in x if abs(v) >= 32767)
    print('peak=%d (%.2f dBFS)  clipped_samples=%d' %
          (peak, 20 * math.log10(peak / 32768.0) if peak else -120.0, clips))

    # Mono mix for structure detection.
    if ch == 2:
        mono = array.array('h', ((x[i * 2] + x[i * 2 + 1]) // 2 for i in range(n)))
    else:
        mono = x

    win = int(0.05 * rate)  # 50 ms
    nwin = n // win
    rms = [0.0] * nwin
    diffr = [0.0] * nwin
    for k in range(nwin):
        base = k * win
        acc = 0.0
        dacc = 0.0
        prev = mono[base]
        for i in range(base, base + win):
            v = mono[i]
            acc += v * v
            d = v - prev
            dacc += d * d
            prev = v
        rms[k] = math.sqrt(acc / win)
        diffr[k] = dacc / acc if acc > 0 else 0.0

    loud = max(rms) if rms else 0.0
    loud_thresh = loud * 0.1  # -20 dB relative to the loudest 50 ms window

    # Noise-like runs: RMS loud AND difference-energy ratio high (white-ish).
    # Tonal music sits near 0.0-0.2; clipped noise near 2.0.
    runs = []
    in_run = False
    for k in range(nwin):
        noisy = rms[k] > loud_thresh and diffr[k] > 0.9
        if noisy and not in_run:
            start, in_run = k, True
        elif not noisy and in_run:
            runs.append((start, k - 1))
            in_run = False
    if in_run:
        runs.append((start, nwin - 1))
    runs = [r for r in runs if (r[1] - r[0] + 1) * 0.05 >= 0.15]  # >=150 ms
    print('noise bursts (>=150ms, loud + white-like, diff-energy ratio > 0.9):')
    total_burst = 0.0
    for a, b in runs:
        dur = 0.05 * (b - a + 1)
        total_burst += dur
        print('  %.2fs .. %.2fs  dur=%.2fs  meanRMS=%.0f  meanDiffRatio=%.2f' %
              (a * 0.05, (b + 1) * 0.05, dur,
               sum(rms[a:b + 1]) / (b - a + 1), sum(diffr[a:b + 1]) / (b - a + 1)))
    dur_s = n / rate
    print('burst events=%d total_burst_s=%.2f  events_per_minute=%.2f' %
          (len(runs), total_burst, len(runs) * 60.0 / dur_s if dur_s > 0 else 0))

    # Clipping per burst window.
    if clips:
        print('clip distribution in bursts:')
        for a, b in runs:
            i0, i1 = int(a * win) * ch, min(len(x), int((b + 1) * win) * ch)
            c = sum(1 for i in range(i0, i1) if abs(x[i]) >= 32767)
            print('  %.2fs..%.2fs clipped=%d' % (a * 0.05, (b + 1) * 0.05, c))

    # Short silences surrounded by loud audio (dropouts/clicks).
    dips = []
    for k in range(1, nwin - 1):
        if rms[k] < loud_thresh * 0.02 and rms[k - 1] > loud_thresh and rms[k + 1] > loud_thresh:
            dips.append(k)
    print('short silence windows between loud audio (click candidates): %d' % len(dips))
    for k in dips[:20]:
        print('  %.2fs rms=%.1f' % (k * 0.05, rms[k]))

    # Per-second RMS for the continuity gate.
    print('per-second RMS dBFS:')
    sec_win = rate
    for s in range(int(dur_s) + 1):
        i0 = s * sec_win
        if i0 >= n:
            break
        i1 = min(n, i0 + sec_win)
        acc = 0
        for i in range(i0, i1, 8):
            v = mono[i]
            acc += v * v
        r = math.sqrt(acc / len(range(i0, i1, 8)))
        db = 20 * math.log10(r / 32768.0) if r > 0 else -120.0
        print('  t=%3ds rms=%8.1f dBFS %s' % (s, db, 'SILENCE' if r == 0 else ''))


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    main(sys.argv[1])
