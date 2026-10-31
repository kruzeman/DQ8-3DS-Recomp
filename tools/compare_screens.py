#!/usr/bin/env python3
"""Pixel-compare two PNG screenshots (8-bit RGB/RGBA, non-interlaced).

Dependency-free (stdlib zlib only): used for the same-frame differential of
the gsp_memory_fill fast path (t_025a1172) — the two runs capture the same
scene at t=155 s and only the blink-cursor phase may differ.

Usage: python3 tools/compare_screens.py A.png B.png
Prints: size, identical-pixel percentage, differing-pixel count and the
bounding box of the differences. Exit 0 = images exist and were compared
(the verdict is in the output), 2 = decode error.
"""
import struct
import sys
import zlib


def decode_png(path):
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG: %s" % path)
    pos = 8
    width = height = None
    bitdepth = colortype = None
    idat = b""
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if ctype == b"IHDR":
            width, height, bitdepth, colortype = struct.unpack(">IIBB", chunk[:10])
        elif ctype == b"IDAT":
            idat += chunk
        elif ctype == b"IEND":
            break
    if width is None or height is None:
        raise ValueError("missing IHDR: %s" % path)
    if bitdepth != 8 or colortype not in (2, 6):
        raise ValueError("unsupported PNG (bitdepth=%s colortype=%s)" % (bitdepth, colortype))
    bpp = 3 if colortype == 2 else 4
    raw = zlib.decompress(idat)
    stride = width * bpp
    out = bytearray(height * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(height):
        ftype = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        if ftype == 1:  # Sub
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif ftype == 2:  # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ftype == 3:  # Average
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif ftype == 4:  # Paeth
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 0xFF
        elif ftype != 0:
            raise ValueError("bad filter %d" % ftype)
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return width, height, bpp, out


def to_rgb(width, height, bpp, px):
    if bpp == 3:
        return px
    out = bytearray(width * height * 3)
    for i in range(width * height):
        out[i * 3:i * 3 + 3] = px[i * 4:i * 4 + 3]
    return out


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    try:
        wa, ha, ba, pa = decode_png(sys.argv[1])
        wb, hb, bb, pb = decode_png(sys.argv[2])
    except ValueError as exc:
        print("decode error: %s" % exc)
        return 2
    if (wa, ha) != (wb, hb):
        print("SIZE MISMATCH: %dx%d vs %dx%d" % (wa, ha, wb, hb))
        return 2
    ra = to_rgb(wa, ha, ba, pa)
    rb = to_rgb(wb, hb, bb, pb)
    diff = 0
    minx, miny, maxx, maxy = wa, ha, -1, -1
    for y in range(ha):
        base = y * wa * 3
        for x in range(wa):
            i = base + x * 3
            if ra[i:i + 3] != rb[i:i + 3]:
                diff += 1
                if x < minx: minx = x
                if x > maxx: maxx = x
                if y < miny: miny = y
                if y > maxy: maxy = y
    total = wa * ha
    pct = 100.0 * (total - diff) / total
    print("size %dx%d" % (wa, ha))
    print("identical %.4f%% (%d/%d pixels)" % (pct, total - diff, total))
    print("differing %d pixels" % diff)
    if diff:
        print("diff bbox x %d..%d y %d..%d" % (minx, maxx, miny, maxy))
    else:
        print("diff bbox none (byte-identical images)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
