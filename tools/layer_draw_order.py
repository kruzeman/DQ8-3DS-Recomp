#!/usr/bin/env python3
"""Summarise the per-draw trace (DQ8_TRACE_LAYER) for selected command lists.

usage: tools/layer_draw_order.py RUN.LOG [list_no ...]
Prints, in execution order, each draw's index, screen bbox and blend state,
so the order of the menu pass vs the naming pass inside one list is visible.
"""
import re
import sys

DRAW = re.compile(
    r"DRAW t=([0-9.]+) list=(\d+) draw=(\d+) bbox=(-?\d+),(-?\d+)\.\.(-?\d+),(-?\d+) "
    r"blend=([0-9A-F]+)/([0-9A-F]+) mask=([0-9A-F]+) atest=([0-9A-F]+)")


def img(x0, y0, x1, y1):
    """RT raster coords (x = RT column, y = bottom-up row) -> top-screen image px."""
    def X(y):
        return 399 - y
    def Y(x):
        return 239 - x // 2
    xs = sorted((X(y0), X(y1)))
    ys = sorted((Y(x0), Y(x1)))
    return "%3d,%3d..%3d,%3d" % (xs[0], ys[0], xs[1], ys[1])


def main():
    path = sys.argv[1]
    wanted = set(sys.argv[2:])
    cur = {}
    with open(path, "r", errors="replace") as fh:
        for line in fh:
            m = DRAW.match(line)
            if not m:
                continue
            if wanted and m.group(2) not in wanted:
                continue
            cur.setdefault(m.group(2), []).append(m)
    for lst, ms in cur.items():
        print("== list %s: %d draws, t=%s..%s" % (lst, len(ms), ms[0].group(1), ms[-1].group(1)))
        for i, m in enumerate(ms, 1):
            x0, y0, x1, y1 = (int(m.group(k)) for k in (4, 5, 6, 7))
            print("  %3d draw=%s rt=%s img=%s blend=%s/%s atest=%s" % (
                i, m.group(3), "%s,%s..%s,%s" % (x0, y0, x1, y1),
                img(x0, y0, x1, y1), m.group(8), m.group(9), m.group(11)))


if __name__ == "__main__":
    main()
