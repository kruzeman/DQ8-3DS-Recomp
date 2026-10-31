#!/usr/bin/env python3
"""Parse DQ8 PICA command lists exactly like the runtime (main.rs pica_process_command_list),
accumulating pica_regs across lists in filename order, and emit a draw-line whenever a draw
trigger (0x22E/0x22F) is written with a nonzero count, capturing the active texture state."""
import sys, struct, glob, os, re

def apply(regs, reg, value, mask):
    old = regs.get(reg, 0)
    merged = old
    for byte in range(4):
        if mask & (1 << byte):
            merged = (merged & ~(0xFF << (byte*8))) | (value & (0xFF << (byte*8)))
    regs[reg] = merged
    return reg == 0x22E or reg == 0x22F  # draw trigger

def parse_list(data, regs):
    words = len(data)//4
    out = []
    index = 0
    while index + 1 < words:
        index += index & 1
        if index + 1 >= words: break
        value = struct.unpack_from('<I', data, index*4)[0]
        header = struct.unpack_from('<I', data, (index+1)*4)[0]
        index += 2
        reg = header & 0xFFFF
        mask = (header >> 16) & 0xF
        extra = (header >> 20) & 0x7FF
        group = header >> 31
        if extra > words - index:
            break
        drew = apply(regs, reg, value, mask)
        if drew:
            out.append(dict(regs))
        for i in range(extra):
            v2 = struct.unpack_from('<I', data, (index+i)*4)[0]
            r2 = reg + (i+1 if group else 0)
            drew2 = apply(regs, r2, v2, mask)
            if drew2:
                out.append(dict(regs))
        index += extra
    return out

def main():
    d = sys.argv[1] if len(sys.argv) > 1 else '/tmp/dq8-pica'
    paths = sorted(glob.glob(d + '/*.bin'))
    if not paths:
        print("no lists in", d); return
    def num(p):
        m = re.search(r'pica-(\d+)-', p)
        return int(m.group(1)) if m else 0
    paths.sort(key=num)
    regs = {}
    n = 0
    for p in paths:
        data = open(p,'rb').read()
        for snap in parse_list(data, regs):
            n += 1
            tex = snap.get(0x80,0); addr = snap.get(0x85,0)&0x0FFFFFFF
            dim = snap.get(0x82,0); fmt = snap.get(0x8E,0)&0xF
            w = (dim>>16)&0x7FF; h = dim&0x7FF
            fb_addr = snap.get(0x11D,0)&0x0FFFFFFF
            fb_dim = snap.get(0x11E,0); fb_w = fb_dim&0x7FF; fb_h=(fb_dim>>12)&0x3FF
            src=p.split('/')[-1]
            paddr = addr*8
            print(f"{n:05d} {src} DRAW tex0=0x{tex:08x} addr=0x{paddr:08x} dim={w}x{h} fmt={fmt} fb=0x{fb_addr*8:08x} {fb_w}x{fb_h} blend=0x{snap.get(0x100,0):08x}/{snap.get(0x101,0):08x} depth={snap.get(0x228,0)}")

if __name__ == '__main__':
    main()
