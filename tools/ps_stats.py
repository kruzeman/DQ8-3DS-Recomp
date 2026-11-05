#!/usr/bin/env python3
"""t_fdd26eba: window-level difference statistics between two composed images.

    python3 tools/ps_stats.py <a.ppm> <b.ppm>

Reports, per screen rect (top 800x480, bottom 640x480 at x=80):
  * mean |d| and the mean SIGNED difference per channel (a systematic colour
    shift shows up as a non-zero signed mean, detail does not),
  * the share of pixels whose largest channel difference exceeds 24,
  * the byte-identity of the rect (so "the bottom screen still goes through the
    guest path" is a fact, not an assumption).
"""
import sys

TOP_W, TOP_H = 800, 480
BOT_DX, BOT_DY, BOT_W, BOT_H = 80, 480, 640, 480


def read_ppm(path):
    with open(path, "rb") as f:
        d = f.read()
    if not d.startswith(b"P6"):
        raise SystemExit("%s: not a P6 PPM" % path)
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


def rect_stats(wa, pa, pb, x0, y0, w, h):
    ident = True
    tot = n = 0
    signed = [0, 0, 0]
    big = 0
    for y in range(y0, y0 + h):
        ra = pa[y * wa * 3:(y + 1) * wa * 3]
        rb = pb[y * wa * 3:(y + 1) * wa * 3]
        for x in range(x0, x0 + w):
            o = x * 3
            mx = 0
            for c in range(3):
                d = ra[o + c] - rb[o + c]
                if d:
                    ident = False
                signed[c] += d
                tot += abs(d)
                if abs(d) > mx:
                    mx = abs(d)
            n += 1
            if mx > 24:
                big += 1
    return dict(mean_abs=tot / float(n * 3),
                signed=[s / float(n) for s in signed],
                big_pct=100.0 * big / n, identical=ident, pixels=n)


def box2(wa, pa, x0, y0, w, h):
    """2x2 box average of a rect -> (w//2, h//2) RGB bytes."""
    ow, oh = w // 2, h // 2
    out = bytearray(ow * oh * 3)
    for y in range(oh):
        for x in range(ow):
            acc = [0, 0, 0]
            for yy in (2 * y, 2 * y + 1):
                row = (y0 + yy) * wa * 3
                for xx in (2 * x, 2 * x + 1):
                    o = row + (x0 + xx) * 3
                    for c in range(3):
                        acc[c] += pa[o + c]
            o = (y * ow + x) * 3
            for c in range(3):
                out[o + c] = (acc[c] + 2) // 4
    return ow, oh, bytes(out)


def main():
    ap, bp = sys.argv[1], sys.argv[2]
    wa, ha, pa = read_ppm(ap)
    wb, hb, pb = read_ppm(bp)
    if (wa, ha) != (wb, hb) or (wa, ha) != (800, 960):
        raise SystemExit("unexpected sizes %dx%d / %dx%d" % (wa, ha, wb, hb))
    for name, box in (("top   ", (0, 0, TOP_W, TOP_H)),
                      ("bottom", (BOT_DX, BOT_DY, BOT_W, BOT_H))):
        s = rect_stats(wa, pa, pb, *box)
        print("%s rect: mean|d|=%.3f signed(r,g,b)=(%+.3f,%+.3f,%+.3f) "
              ">24levels=%.2f%% byte_identical=%s"
              % (name, s["mean_abs"], s["signed"][0], s["signed"][1],
                 s["signed"][2], s["big_pct"], s["identical"]))
    # Same picture at native resolution: both top rects box-downscaled 2:1.
    ow, oh, da = box2(wa, pa, 0, 0, TOP_W, TOP_H)
    _, _, db = box2(wb, pb, 0, 0, TOP_W, TOP_H)
    s = rect_stats(ow, da, db, 0, 0, ow, oh)
    print("top downscaled 2:1 (%dx%d): mean|d|=%.3f signed(r,g,b)="
          "(%+.3f,%+.3f,%+.3f) >24levels=%.2f%%"
          % (ow, oh, s["mean_abs"], s["signed"][0], s["signed"][1],
             s["signed"][2], s["big_pct"]))


if __name__ == "__main__":
    main()
