import glob, re, sys, collections

def mean(path):
    s = n = 0
    for line in open(path, errors="replace"):
        if not line.startswith("FRAMEPROF f="):
            continue
        g = t = 0.0
        for tok in line.split():
            if tok.startswith("guest_ms="):
                g = float(tok.split("=")[1])
            elif tok.startswith("total_ms="):
                t = float(tok.split("=")[1])
        if t > 2.0:
            s += g
            n += 1
    return (s / n if n else 0.0), n

pats = sys.argv[1:] or ["target/fn-ev-ab/*.err"]
files = sorted(f for p in pats for f in glob.glob(p))
by = collections.defaultdict(list)
for f in files:
    m = re.match(r".*/([a-z0-9]+)-(field|battle)-(\d)-(off|on)\.err$", f)
    if not m:
        continue
    tag, scene, pair, arm = m.groups()
    g, n = mean(f)
    by[(tag, scene, int(pair))].append((arm, g, n))
for k in sorted(by):
    row = " ".join("%s=%.3f n=%d" % (a, g, n) for a, g, n in sorted(by[k]))
    d = dict((a, g) for a, g, n in by[k])
    delta = ""
    if "off" in d and "on" in d:
        delta = "  gain=%.1f%%" % (100.0 * (d["off"] - d["on"]) / d["off"])
    print("%-18s %-6s pair%d: %s%s" % (k[0], k[1], k[2], row, delta))
