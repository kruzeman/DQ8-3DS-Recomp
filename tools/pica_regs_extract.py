#!/usr/bin/env python3
"""Extract selected PICA register writes from captured command lists.

Mirrors src/main.rs pica_process_command_list decoding:
  word[i]=value, word[i+1]=header; reg=header&0xFFFF, mask=(header>>16)&0xF,
  extra=(header>>20)&0x7FF, group=header>>31; i advances by 2+extra,
  auto-aligning to even indices.

Usage: pica_regs_extract.py <list-dir> <watch-regs-csv> [n-last]
Prints, for the last N lists (default 3), the watch-reg writes in order.
"""
import os
import struct
import sys

d = sys.argv[1]
watch = {int(x, 16) for x in sys.argv[2].split(",")}
n_last = int(sys.argv[3]) if len(sys.argv) > 3 else 3
files = sorted(os.listdir(d))
sel = files[-n_last:]
for name in files[:-n_last][-1:]:
    print(f"(context list {name})")
for name in sel:
    data = open(os.path.join(d, name), "rb").read()
    words = list(struct.unpack("<%dI" % (len(data) // 4), data[: len(data) // 4 * 4]))
    print(f"== {name} words={len(words)}")
    i = 0
    while i + 1 < len(words):
        i += i & 1
        if i + 1 >= len(words):
            break
        value = words[i]
        header = words[i + 1]
        i += 2
        reg = header & 0xFFFF
        mask = (header >> 16) & 0xF
        extra = (header >> 20) & 0x7FF
        group = header >> 31
        if extra > len(words) - i:
            print("  truncated")
            break
        if reg in watch or any((reg + (k + 1 if group else 0)) in watch for k in range(extra)):
            print(f"  reg={reg:03X} mask={mask:X} value={value:08X} extra={extra} group={group}")
            for k in range(extra):
                r2 = reg + (k + 1 if group else 0)
                if r2 in watch:
                    print(f"    +{k+1} reg={r2:03X} value={words[i+k]:08X}")
        i += extra
