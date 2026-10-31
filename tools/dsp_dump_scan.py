#!/usr/bin/env python3
"""Summarize non-zero regions of DQ8_DSP_DUMP DSP RAM snapshots."""
import glob
import sys


def runs(data, base):
    out = []
    i = 0
    n = len(data)
    while i < n:
        if data[i] != 0:
            j = i
            while j < n and data[j] != 0:
                j += 1
            out.append((base + i, j - i))
            i = j
        else:
            i += 1
    return out


def main(pattern):
    files = sorted(glob.glob(pattern))
    if not files:
        print("no dumps for", pattern)
        return
    picks = [files[0], files[len(files) // 2], files[-1]]
    for f in picks:
        d = open(f, 'rb').read()
        print("==", f, len(d))
        rs = [r for r in runs(d, 0x1FF00000) if r[1] >= 8]
        merged = []
        for off, ln in rs:
            if merged and off <= merged[-1][0] + merged[-1][1] + 0x100:
                merged[-1] = (merged[-1][0], off + ln - merged[-1][0])
            else:
                merged.append((off, ln))
        for off, ln in merged[:60]:
            print("  %08X +%06X" % (off, ln))


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'r_*.bin')
