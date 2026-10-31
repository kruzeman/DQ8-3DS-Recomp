#!/usr/bin/env python3
"""Print nonzero-pixel count of a PPM (P6) top-screen capture. Usage: ppm_nz.py file.ppm"""
import sys


def main():
    try:
        with open(sys.argv[1], 'rb') as f:
            data = f.read()
    except OSError:
        print(-1)
        return
    if not data.startswith(b'P6'):
        print(-1)
        return
    i = 2
    toks = []
    while len(toks) < 3 and i < len(data):
        while i < len(data) and data[i:i+1].isspace():
            i += 1
        if data[i:i+1] == b'#':
            while i < len(data) and data[i] != 0x0A:
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j+1].isspace():
            j += 1
        toks.append(data[i:j])
        i = j
    if len(toks) < 3:
        print(-1)
        return
    w, h = int(toks[0]), int(toks[1])
    i += 1
    body = data[i:]
    nz = 0
    npx = w * h
    n = min(len(body), npx * 3)
    n -= n % 3
    for k in range(0, n, 3):
        if body[k] or body[k+1] or body[k+2]:
            nz += 1
    print(nz)


if __name__ == '__main__':
    main()
