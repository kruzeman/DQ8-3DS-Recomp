#!/usr/bin/env python3
"""Count white-ish pixels (>200 on all channels) in a P6 PPM; print the count."""
import sys

def read_ppm(path):
    with open(path, 'rb') as f:
        data = f.read()
    parts = []
    i = 0
    while len(parts) < 4:
        while i < len(data) and data[i:i+1].isspace():
            i += 1
        if data[i:i+1] == b'#':
            while i < len(data) and data[i:i+1] != b'\n':
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j+1].isspace():
            j += 1
        parts.append(data[i:j])
        i = j
    i += 1
    w, h = int(parts[1]), int(parts[2])
    return w, h, data[i:i + w * h * 3]

def main():
    w, h, px = read_ppm(sys.argv[1])
    if len(px) < w * h * 3:
        print(-1)
        return
    white = 0
    for o in range(0, w * h * 3, 3):
        if px[o] > 200 and px[o+1] > 200 and px[o+2] > 200:
            white += 1
    print(white)

if __name__ == '__main__':
    main()
