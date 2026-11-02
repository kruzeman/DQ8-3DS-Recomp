#!/usr/bin/env python3
"""t_d559a510: find the FIRST genuine depth divergence (pre-depth matched,
post-depth differs) between two DQ8_GL_DRAWHASH arms, and classify every depth
divergence as genuine vs cascade (pre-depth already differed).

usage: glz_first.py <arm-a.log> <arm-b.log> [max_draw]
"""
import sys
sys.path.insert(0, "/home/hermes/DQ8/worktrees/glblend-t_d559a510/tools")
from glhash_zcmp import load, cfg  # noqa: E402


def main():
    A, B = load(sys.argv[1]), load(sys.argv[2])
    max_draw = int(sys.argv[3]) if len(sys.argv) > 3 else 10 ** 9
    common = [n for n in sorted(set(A) & set(B)) if n <= max_draw]
    genuine = []
    cascade = 0
    for n in common:
        a, b = A[n], B[n]
        if not (a.get("zpost") and b.get("zpost")):
            continue
        if a["zpost"] == b["zpost"]:
            continue
        if a["zpre"] != b["zpre"]:
            cascade += 1
            continue
        genuine.append(n)
    print("draws compared: %d   depth divergences: genuine=%d cascade(already "
          "differed before the draw)=%d" % (len(common), len(genuine), cascade))
    if genuine:
        print("first genuine: %s" % genuine[:12])
        for n in genuine[:6]:
            a, b = A[n], B[n]
            print("\ndraw %d  %s" % (n, cfg(a)))
            print("  box=%s" % a["box"])
            print("  A zpre=%s zpost=%s zsum=%s"
                  % (a.get("zpre"), a.get("zpost"), a.get("zqsum")))
            print("  B zpre=%s zpost=%s zsum=%s"
                  % (b.get("zpre"), b.get("zpost"), b.get("zqsum")))
            print("  A colour post=%s  B colour post=%s%s"
                  % (a["post"], b["post"],
                     "  (identical)" if a["post"] == b["post"] else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
