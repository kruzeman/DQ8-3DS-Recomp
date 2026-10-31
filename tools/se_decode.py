#!/usr/bin/env python3
"""Reproduce the HLE's ADPCM decode offline from a DQ8_DSP_SE_DIR dump.

Reads srcXX_idYYY.bin (text header + raw ADPCM/PCM bytes), decodes with the
Azahar Codec::DecodeADPCM formula, and prints: header, decoded envelope
(10 ms RMS), saturation statistics and where the decode runs off into
garbage.  Also cross-checks the .dec file the HLE wrote, if present.

usage: se_decode.py <se-dir>
"""
import glob
import math
import os
import struct
import sys

SIGNED_NIBBLES = [0, 1, 2, 3, 4, 5, 6, 7, -8, -7, -6, -5, -4, -3, -2, -1]


def decode_adpcm(data, length, coeff, yn1, yn2):
    frames = (length + 13) // 14
    out = []
    for fi in range(frames):
        base = fi * 8
        if base >= len(data):
            break
        header = data[base]
        scale = 1 << (header & 0xF)
        idx = (header >> 4) & 0x7
        c1, c2 = coeff[idx * 2], coeff[idx * 2 + 1]
        for i in range(14):
            if fi * 14 + i >= length:
                break
            byte = data[base + 1 + i // 2]
            nib = SIGNED_NIBBLES[(byte >> 4) if i % 2 == 0 else (byte & 0xF)]
            xn = nib * scale
            val = ((xn << 11) + 0x400 + c1 * yn1 + c2 * yn2) >> 11
            val = max(-32768, min(32767, val))
            yn2, yn1 = yn1, val
            out.append(val)
    return out


def analyze(path):
    with open(path, 'rb') as f:
        blob = f.read()
    # split text header from raw bytes at the 'raw_bytes=N\n' line
    marker = b'raw_bytes='
    pos = blob.find(marker)
    if pos < 0:
        print('%s: no header' % path)
        return
    line_end = blob.find(b'\n', pos)
    header = blob[:line_end].decode()
    raw = blob[line_end + 1:]
    hdr = {}
    for ln in header.splitlines():
        k = ln.split('=', 1)[0].split()[0]
        hdr[k] = ln
    print('== %s' % os.path.basename(path))
    print(hdr.get('src', ''))
    print(hdr.get('coeff', ''))
    print(hdr.get('rate', ''))
    print(hdr.get('gains', ''))
    print(header.splitlines()[-1])
    # simple field extraction
    def ival(name, line_idx=0):
        for ln in header.splitlines():
            for kv in ln.split():
                if kv.startswith(name + '='):
                    return int(kv.split('=')[1], 0)
        return None
    length = ival('len')
    fmt = ival('fmt')
    if fmt == 2:
        coeff = [int(x, 16) if int(x, 16) < 32768 else int(x, 16) - 65536
                 for x in hdr['coeff'].split()[1:]]
        yn = [int(x.split()[0], 16) for x in hdr['src'].split('yn=')[1].split(',')[:2]]
        dec = decode_adpcm(raw, length, coeff, yn[0], yn[1])
    else:
        dec = []
    if not dec:
        return
    n = len(dec)
    sat = sum(1 for v in dec if v >= 32767 or v <= -32768)
    print('decoded %d samples (%.3fs at rate)  saturated=%d' %
          (n, n / 32728.0, sat))
    # 10 ms envelope
    win = 327
    for i in range(0, n, win):
        seg = dec[i:i + win]
        rms = math.sqrt(sum(v * v for v in seg) / len(seg))
        pk = max(abs(v) for v in seg)
        db = 20 * math.log10(rms / 32768.0) if rms else -120.0
        pdb = 20 * math.log10(pk / 32768.0) if pk else -120.0
        print('  n=%6d t=%6.3fs rms=%7.0f (%6.1f dBFS) peak=%6d (%6.1f)' %
              (i, i / 32728.0, rms, db, pk, pdb))
    # cross-check the HLE's own decode
    decpath = path[:-4] + '.dec'
    if os.path.exists(decpath):
        with open(decpath, 'rb') as f:
            mine = struct.unpack('<%dh' % (len(f.read()) // 2), f.read()) if False else None
        with open(decpath, 'rb') as f:
            b = f.read()
        hle = struct.unpack('<%dh' % (len(b) // 2), b)
        mism = sum(1 for a, c in zip(dec, hle) if a != c)
        print('HLE .dec has %d samples, mismatches vs offline=%d' % (len(hle), mism))


def main():
    d = sys.argv[1]
    files = sorted(glob.glob(os.path.join(d, 'src*.bin')))
    if not files:
        print('no dumps in %s' % d)
        return
    for p in files:
        analyze(p)


if __name__ == '__main__':
    main()
