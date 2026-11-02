#!/usr/bin/env python3
"""t_d559a510: index-aligned per-draw A/B of the GL fixed-function blend path
against the shader (software) blend path, INCLUDING the emulated depth buffer
(COLOR_ATTACHMENT1).

usage: glhash_zcmp.py <arm-a.log> <arm-b.log> [max_print]

Both logs must come from runs with DQ8_GL_DRAWHASH=1 on the same binary, the
same savestate and no input; the arms differ only in the blend path:
  A: DQ8_GL_BLENDS=0                      (shader blend = reference)
  B: DQ8_GL_BLENDS=1                      (fixed-function blend)
  B: DQ8_GL_BLENDS=1 DQ8_GL_BLENDDEPTH=0  (pre-fix: depth attachment blended)

Draws are compared BY INDEX: the guest executes the same draw sequence from the
same savestate, so index N is the same PICA draw in both arms.  A colour
difference of a few LSB propagates into later boxes ("cascade"), so the metrics
that matter are:
  * state mismatches (box / blend config / depth flags differ at the same index)
    -> the runs are not comparable at all;
  * |post byte-sum delta| per draw (colour): <= ~40 over a 20x240 box is the
    8-bit rounding class, thousands would be a real divergence;
  * depth agreement (zpost / zpost_sum): the fixed-function path must write the
    depth attachment bit-identically, since it is REPLACED, not blended.
"""
import re
import sys
from collections import Counter

LINE = re.compile(
    r"GLHASH draw=(?P<draw>\d+) tris=(?P<tris>\d+) mode=(?P<mode>-?\d+) "
    r"blend=(?P<blend>-?\d+) glb=(?P<glb>-?\d+) "
    r"src=(?P<srcr>\d+),(?P<srca>\d+) dst=(?P<dstr>\d+),(?P<dsta>\d+) "
    r"eq=(?P<eqr>\d+),(?P<eqa>\d+) a=(?P<at>-?\d+)/(?P<af>\d+)/(?P<aref>[-\d.]+) "
    r"mask=(?P<mask>[0-9A-Fa-f]+) depth=(?P<dt>-?\d+)/(?P<dw>-?\d+)/(?P<don>-?\d+) "
    r"box=(?P<box>[-\d,\.]+) pre=(?P<pre>[0-9a-f]+) post=(?P<post>[0-9a-f]+) "
    r"pre_sum=(?P<psum>\d+) post_sum=(?P<qsum>\d+)"
    r"(?: zpre=(?P<zpre>[0-9a-f]+) zpost=(?P<zpost>[0-9a-f]+) "
    r"zpre_sum=(?P<zpsum>\d+) zpost_sum=(?P<zqsum>\d+))?")


def load(path):
    out = {}
    with open(path, "r", errors="replace") as f:
        for ln in f:
            m = LINE.search(ln)
            if not m:
                continue
            d = m.groupdict()
            out[int(d["draw"])] = d
    return out


def cfg(d):
    return ("src=%s,%s dst=%s,%s eq=%s,%s mask=%s depth=%s/%s/%s tris=%s"
            % (d["srcr"], d["srca"], d["dstr"], d["dsta"], d["eqr"], d["eqa"],
               d["mask"], d["dt"], d["dw"], d["don"], d["tris"]))


def main():
    A, B = load(sys.argv[1]), load(sys.argv[2])
    max_print = int(sys.argv[3]) if len(sys.argv) > 3 else 4
    max_draw = int(sys.argv[4]) if len(sys.argv) > 4 else None
    common = sorted(set(A) & set(B))
    if max_draw is not None:
        common = [n for n in common if n <= max_draw]
    print("draws: A=%d B=%d common=%d" % (len(A), len(B), len(common)))
    if not common:
        print("no common draw indices -> not comparable")
        return 2
    state_mis = 0
    c_same = 0
    c_round = 0            # |dsum| <= 40: the RGBA8 rounding class
    c_big = 0
    big = []
    d_cmp = 0
    d_same = 0
    d_bad = []
    d_bad_cfg = Counter()
    dsum_max = 0
    zdsum_max = 0
    for n in common:
        a, b = A[n], B[n]
        if a["box"] != b["box"] or cfg(a) != cfg(b):
            state_mis += 1
        dsum = abs(int(a["qsum"]) - int(b["qsum"]))
        if a["post"] == b["post"]:
            c_same += 1
        elif dsum <= 40:
            c_round += 1
        else:
            c_big += 1
            if len(big) < max_print:
                big.append((n, a, b, dsum))
        if dsum > dsum_max:
            dsum_max = dsum
        if a.get("zpost") and b.get("zpost"):
            d_cmp += 1
            if a["zpost"] == b["zpost"]:
                d_same += 1
            else:
                d_bad_cfg[cfg(a)] += 1
                if len(d_bad) < max_print:
                    d_bad.append((n, a, b))
        if a.get("zqsum") and b.get("zqsum"):
            dz = abs(int(a["zqsum"]) - int(b["zqsum"]))
            if dz > zdsum_max:
                zdsum_max = dz
    print("state (box/config) mismatches at same index: %d" % state_mis)
    print("colour: identical post=%d  rounding-class(|dsum|<=40)=%d  "
          "divergent=%d   max|dsum|=%d" % (c_same, c_round, c_big, dsum_max))
    print("depth : compared=%d  identical post=%d  divergent=%d  max|zsum delta|=%d"
          % (d_cmp, d_same, d_cmp - d_same, zdsum_max))
    if d_bad_cfg:
        print("\nDEPTH divergences by draw config:")
        for k, v in d_bad_cfg.most_common(8):
            print("  %6d  %s" % (v, k))
    for n, a, b, dsum in big:
        print("\ncolour divergence draw %d  %s  |dsum|=%d" % (n, cfg(a), dsum))
        print("  A post=%s post_sum=%s" % (a["post"], a["qsum"]))
        print("  B post=%s post_sum=%s" % (b["post"], b["qsum"]))
    for n, a, b in d_bad:
        print("\nDEPTH divergence draw %d  %s" % (n, cfg(a)))
        print("  box=%s zpre A=%s B=%s" % (a["box"], a.get("zpre"), b.get("zpre")))
        print("        zpost A=%s (sum %s) B=%s (sum %s)"
              % (a.get("zpost"), a.get("zqsum"), b.get("zpost"), b.get("zqsum")))
    return 0 if (c_big == 0 and d_cmp - d_same == 0) else 1


if __name__ == "__main__":
    sys.exit(main())
