import sys
sys.path.insert(0, "/home/hermes/DQ8/worktrees/glblend-t_d559a510/tools")
from glhash_zcmp import load, cfg

A, B = load(sys.argv[1]), load(sys.argv[2])
common = sorted(set(A) & set(B))
prev = None
for n in common:
    a, b = A[n], B[n]
    za, zb = a.get("zpre"), b.get("zpre")
    if za and zb and za != zb:
        print("earliest zpre mismatch at draw %d %s box=%s" % (n, cfg(a), a["box"]))
        for k in [n-3, n-2, n-1, n]:
            if k in A and k in B:
                x, y = A[k], B[k]
                print("  draw %d %s" % (k, cfg(x)))
                print("     box=%s" % x["box"])
                print("     A zpre=%s zpost=%s" % (x.get("zpre"), x.get("zpost")))
                print("     B zpre=%s zpost=%s" % (y.get("zpre"), y.get("zpost")))
                print("     A post=%s B post=%s" % (x["post"], y["post"]))
        break
else:
    print("no zpre mismatch among %d common draws" % len(common))
