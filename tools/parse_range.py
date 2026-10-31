#!/usr/bin/env python3
import sys, struct, glob, re

def apply(regs, reg, value, mask):
    old = regs.get(reg, 0); merged = old
    for byte in range(4):
        if mask & (1 << byte):
            merged = (merged & ~(0xFF << (byte*8))) | (value & (0xFF << (byte*8)))
    regs[reg] = merged
    return reg in (0x22E, 0x22F)

def parse_list(data, regs):
    words = len(data)//4; out = []; index = 0
    while index + 1 < words:
        index += index & 1
        if index + 1 >= words: break
        value = struct.unpack_from('<I', data, index*4)[0]
        header = struct.unpack_from('<I', data, (index+1)*4)[0]
        index += 2
        reg = header & 0xFFFF; mask = (header >> 16) & 0xF
        extra = (header >> 20) & 0x7FF; group = header >> 31
        if extra > words - index: break
        if apply(regs, reg, value, mask): out.append(dict(regs))
        for i in range(extra):
            v2 = struct.unpack_from('<I', data, (index+i)*4)[0]
            r2 = reg + (i+1 if group else 0)
            if apply(regs, r2, v2, mask): out.append(dict(regs))
        index += extra
    return out

def num(p):
    m = re.search(r'pica-(\d+)-', p); return int(m.group(1))

d = sys.argv[1] if len(sys.argv) > 1 else '/tmp/dq8-pica3'
lo = int(sys.argv[2]) if len(sys.argv) > 2 else 0
hi = int(sys.argv[3]) if len(sys.argv) > 3 else 99999
paths = sorted(glob.glob(d + '/*.bin'), key=num)
regs = {}; seen = set()
for p in paths:
    ln = num(p)
    if ln < lo or ln > hi: continue
    data = open(p, 'rb').read()
    for snap in parse_list(data, regs):
        addr = snap.get(0x85, 0) & 0x0FFFFFFF
        dim = snap.get(0x82, 0); fmt = snap.get(0x8E, 0) & 0xF
        w = (dim >> 16) & 0x7FF; h = dim & 0x7FF
        tex = snap.get(0x80, 0)
        key = (fmt, addr, w, h)
        if key not in seen:
            seen.add(key)
            if addr or (tex & 1):
                print(f"list{ln:04d} tex0=0x{tex:08x} addr=0x{addr*8:08x} dim={w}x{h} fmt={fmt} "
                      f"blend=0x{snap.get(0x100,0):08x}/{snap.get(0x101,0):08x} "
                      f"tev0=0x{snap.get(0xc0,0):08x} alpha={snap.get(0x22c,0):#x}")
