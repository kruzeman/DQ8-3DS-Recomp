#!/usr/bin/env python3
"""Noise-check a captured WAV the way the task demands: locate the sound
event with hysteresis (skip isolated single-sample spikes), report the peak
and the per-second RMS profile.

usage: tools/analyze_audio_wav.py <file.wav>
"""
import struct
import sys
import wave


def read_wav(path):
    with wave.open(path, 'rb') as w:
        rate = w.getframerate()
        channels = w.getnchannels()
        frames = w.readframes(w.getnframes())
    samples = struct.unpack('<%dh' % (len(frames) // 2), frames)
    return rate, channels, samples


def main(path):
    rate, channels, samples = read_wav(path)
    n = len(samples) // channels
    duration = n / rate
    peak = max(abs(s) for s in samples) if samples else 0
    win = rate // 100  # 10 ms
    rms = []
    for i in range(0, n - win, win):
        acc = 0
        for j in range(i, i + win):
            v = samples[j * channels]
            acc += v * v
        rms.append((acc / win) ** 0.5)
    peak_rms = max(rms) if rms else 0.0
    thresh = peak_rms * 0.05  # -26 dB below the loudest window
    loud = [i for i, v in enumerate(rms) if v >= thresh]
    # hysteresis: keep runs of >=3 consecutive loud windows (30 ms)
    runs = []
    if loud:
        start = prev = loud[0]
        for i in loud[1:]:
            if i == prev + 1:
                prev = i
                continue
            runs.append((start, prev))
            start = prev = i
        runs.append((start, prev))
    runs = [r for r in runs if r[1] - r[0] >= 2]
    print('file=%s rate=%d ch=%d duration=%.2fs peak=%d (%.1f dBFS)' %
          (path, rate, channels, duration, peak,
           20 * __import__('math').log10(peak / 32768.0) if peak else -120.0))
    print('loudest 10ms window: %.1f dBFS' %
          (20 * __import__('math').log10(peak_rms / 32768.0) if peak_rms else -120.0))
    print('sound segments (>=30ms, >=-26dB below loudest window):')
    for a, b in runs:
        print('  %.2fs .. %.2fs' % (a * 0.01, (b + 1) * 0.01))
    print('per-second RMS (dBFS):')
    for sec in range(int(duration) + 1):
        chunk = [v for i, v in enumerate(rms)
                 if sec <= i * 0.01 < sec + 1]
        if not chunk:
            continue
        r = max(chunk)
        db = 20 * __import__('math').log10(r / 32768.0) if r else -120.0
        print('  t=%2ds %7.1f' % (sec, db))


if __name__ == '__main__':
    main(sys.argv[1])
