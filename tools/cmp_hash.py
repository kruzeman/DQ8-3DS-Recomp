import sys

def load(p):
    return [l.strip() for l in open(p)]

def first_diff(x, y):
    for i, (p, q) in enumerate(zip(x, y)):
        if p != q:
            return i
    return min(len(x), len(y))

a = load('/tmp/xfer-run/a1.h')
b = load('/tmp/xfer-run/b1.h')
c = load('/tmp/xfer-run/a2.h')
print('a1 vs a2 first diff at', first_diff(a, c), 'common', min(len(a), len(c)))
print('a1 vs b1 first diff at', first_diff(a, b), 'common', min(len(a), len(b)))
