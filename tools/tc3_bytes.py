#!/usr/bin/env python3
"""Exact byte diff between two PPMs (no verdict heuristics). usage: tc3_bytes.py a.ppm b.ppm"""
import sys

def readppm(p):
    f = open(p, 'rb')
    assert f.readline().strip() == b'P6', p
    l = f.readline()
    while l.startswith(b'#'):
        l = f.readline()
    w, h = map(int, l.split())
    int(f.readline())
    return w, h, f.read()

w, h, da = readppm(sys.argv[1])
w2, h2, db = readppm(sys.argv[2])
assert (w, h) == (w2, h2)
n = len(da)
bad = 0
first = None
for i in range(n):
    if da[i] != db[i]:
        bad += 1
        if first is None:
            first = i
print('%s vs %s: %d/%d bytes differ (%.4f%%)' % (sys.argv[1], sys.argv[2], bad, n, 100.0*bad/n))
if first is not None:
    px = first // 3
    print('first diff at byte %d -> pixel (%d,%d)' % (first, px % w, px // w))
