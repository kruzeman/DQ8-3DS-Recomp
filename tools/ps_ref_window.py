#!/usr/bin/env python3
"""t_fdd26eba: rebuild the composed window image from the two screen PPMs.

    python3 tools/ps_ref_window.py <top.ppm> <bottom.ppm> <out.ppm>

The window composer (t_5286064e) is a fixed geometry: top 400x240 at (0,0)
scaled 2x, bottom 320x240 at (80,480) scaled 2x, everything else black
(HOST_SCALE=2, 800x960).  This is the reference the guest-source composer must
reproduce byte for byte at x1, so "x1 is byte-identical to dq8-b48" can be
checked against the base binary's own screen dumps.
"""
import sys

PW, PH = 800, 960


def read_ppm(path):
    with open(path, "rb") as f:
        d = f.read()
    fields, i = [], 2
    while len(fields) < 3:
        while d[i:i + 1].isspace():
            i += 1
        if d[i:i + 1] == b"#":
            while d[i:i + 1] not in (b"\n", b""):
                i += 1
            continue
        j = i
        while not d[j:j + 1].isspace():
            j += 1
        fields.append(int(d[i:j]))
        i = j
    while d[i:i + 1].isspace():
        i += 1
    w, h, _mx = fields
    return w, h, d[i:i + w * h * 3]


def blit2(dst, src, sw, sh, dx, dy):
    for y in range(sh):
        row = y * sw * 3
        for x in range(sw):
            o = row + x * 3
            px = src[o:o + 3]
            for yy in (2 * y, 2 * y + 1):
                d = ((dy + yy) * PW + dx + 2 * x) * 3
                dst[d:d + 3] = px
                dst[d + 3:d + 6] = px
    return dst


def main():
    top_p, bot_p, out_p = sys.argv[1], sys.argv[2], sys.argv[3]
    tw, th, tp = read_ppm(top_p)
    bw, bh, bp = read_ppm(bot_p)
    if (tw, th) != (400, 240) or (bw, bh) != (320, 240):
        raise SystemExit("unexpected screen sizes %dx%d / %dx%d" % (tw, th, bw, bh))
    win = bytearray(PW * PH * 3)
    blit2(win, tp, tw, th, 0, 0)
    blit2(win, bp, bw, bh, (400 - 320) // 2 * 2, 480)
    with open(out_p, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (PW, PH))
        f.write(bytes(win))


if __name__ == "__main__":
    main()
