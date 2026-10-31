#!/usr/bin/env python3
"""Compare a captured WAV against the reference save-SE.

Reports: duration, peak dBFS, clipped-sample count, the non-silent span
(with 10 ms RMS envelope, hysteresis), per-channel peaks and a coarse
spectral profile of the loud span (8 bands, so clipping-vs-jingle shape
is visible without ffmpeg).
usage: wav_cmp.py <file.wav> [ref.wav]
"""
import struct
import sys
import math
import wave


def read_wav(path):
    with wave.open(path, 'rb') as w:
        rate = w.getframerate()
        ch = w.getnchannels()
        n = w.getnframes()
        frames = w.readframes(n)
    samples = struct.unpack('<%dh' % (len(frames) // 2), frames)
    return rate, ch, samples


def db(x):
    return 20 * math.log10(x / 32768.0) if x > 0 else -120.0


def analyze(path):
    rate, ch, s = read_wav(path)
    n = len(s) // ch
    left = s[0::ch]
    peak = max(abs(v) for v in s)
    clipped_hi = sum(1 for v in s if v >= 32767)
    clipped_lo = sum(1 for v in s if v <= -32768)
    # 10 ms RMS envelope on left channel
    win = max(1, rate // 100)
    env = []
    for i in range(0, n - win + 1, win):
        acc = 0
        for j in range(i, i + win):
            acc += left[j] * left[j]
        env.append(math.sqrt(acc / win))
    peak_rms = max(env) if env else 0.0
    thresh = peak_rms * 0.05
    loud = [i for i, v in enumerate(env) if v >= thresh]
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
    print('== %s' % path)
    print('rate=%d ch=%d duration=%.2fs peak=%d (%.1f dBFS) clipped(+32767)=%d clipped(-32768)=%d'
          % (rate, ch, n / rate, peak, db(peak), clipped_hi, clipped_lo))
    print('non-silent spans (>=30 ms, >=-26 dB rel loudest window):')
    for a, b in runs:
        seg = s[a * win * ch:(b + 1) * win * ch]
        sp = max(abs(v) for v in seg) if seg else 0
        print('  %.3fs .. %.3fs  (len %.3fs, segment peak %d = %.1f dBFS)'
              % (a * win / rate, (b + 1) * win / rate, (b + 1 - a) * win / rate, sp, db(sp)))
    if runs:
        a, b = runs[0]
        # coarse spectrum of the first loud span (left channel)
        seg = left[a * win:(b + 1) * win]
        m = len(seg)
        # Hann window + 512-bin DFT magnitude averaged over the segment
        NB = 512
        bands = [0.0] * 8
        hops = max(1, m // NB)
        cnt = 0
        for off in range(0, m - NB + 1, NB):
            cnt += 1
            for k in range(NB // 2):
                re = im = 0.0
                for t in range(0, NB, 4):  # stride 4, coarse but fine for band shape
                    w = 0.5 - 0.5 * math.cos(2 * math.pi * t / (NB - 1))
                    v = seg[off + t] * w
                    ang = 2 * math.pi * k * t / NB
                    re += v * math.cos(ang)
                    im -= v * math.sin(ang)
                mag = math.hypot(re, im)
                f = k * rate / NB
                bidx = min(7, int(f / (rate / 2) * 8))
                bands[bidx] += mag
        if cnt:
            tot = sum(bands) or 1.0
            print('band energy fractions (0-1/8=0-2.75k ... 7/8..nyq):')
            names = ['0-2.7k', '2.7-5.5k', '5.5-8.2k', '8.2-11k', '11-13.8k', '13.8-16.5k', '16.5-19.3k', '19.3-22k']
            for i, v in enumerate(bands):
                print('  %s: %.3f' % (names[i], v / tot))
    return rate, ch, s


def main():
    analyze(sys.argv[1])
    if len(sys.argv) > 2:
        print()
        analyze(sys.argv[2])


if __name__ == '__main__':
    main()
