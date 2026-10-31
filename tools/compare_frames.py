#!/usr/bin/env python3
"""Compare two PPM frames (top screen) pixel-by-pixel; report differing bytes
and a coarse similarity verdict.  Used to check the naming-screen blended
output + layering is preserved between the base and fast-path binaries."""
import sys
from pathlib import Path


def load_ppm_rgb(p):
    d = Path(p).read_bytes()
    # P6 header: magic, dims, maxval, each terminated by \n
    parts = []
    i = 0
    while len(parts) < 4 and i < len(d):
        if d[i] == 10:
            parts.append(i)
        i += 1
    start = parts[-1] + 1
    return d[start:]


def main():
    if len(sys.argv) != 3:
        print("usage: compare_frames.py A.ppm B.ppm", file=sys.stderr)
        return 2
    a = load_ppm_rgb(sys.argv[1])
    b = load_ppm_rgb(sys.argv[2])
    n = min(len(a), len(b))
    diff = 0
    maxd = 0
    for i in range(n):
        if a[i] != b[i]:
            diff += 1
            d = abs(a[i] - b[i])
            if d > maxd:
                maxd = d
    pct = 100.0 * diff / n if n else 0
    print(f"A={sys.argv[1]} bytes={len(a)}")
    print(f"B={sys.argv[2]} bytes={len(b)}")
    print(f"compared={n} differing_bytes={diff} ({pct:.3f}%) max_channel_delta={maxd}")
    print("VERDICT:", "IDENTICAL" if (len(a) == len(b) and diff == 0)
          else f"DIFFERENT ({pct:.2f}% of bytes, max delta {maxd})")
    return 0


if __name__ == '__main__':
    sys.exit(main())
