#!/usr/bin/env python3
"""t_41f38df0: decode a captured PICA command list (tools/cmdlist_find_float.py
only greps raw f32 words, which misses the PackedAttribute f24 path).

Mirrors src/main.rs pica_process_command_list_body exactly:
  index skips to even, value = w[index], header = w[index+1],
  header = {reg:16, mask:4, extra:11, group:1}; the first parameter is the
  `value` word, then `extra` further words go to reg + (group ? i+1 : 0).

Replays the VS float-uniform upload (reg 0x2C0 config, 0x2C1..0x2C8 data)
for BOTH packings — f32 (4 words, first word -> component 3) and f24
(3 words -> four 24-bit values, same PackedAttribute::AsFloat24 unpack the
runtime uses) — so the camera words can be located and their byte offsets
turned into a guest VA for DQ8_TRACE_WRITE.

usage:
  cmdlist_decode.py <cmdbin> <list_va_hex> [--regs 2C0,2C1] [--uniforms 3,4,5,25,26,27,85,90,91,92] [--all-uniforms] [--max N]
"""
import argparse
import math
import struct
import sys


def f24_to_f32(raw):
    """pica_f24_to_f32 (src/pica_vs.c:207): sign(1) exp(7,bias63) mantissa(16)."""
    raw &= 0xFFFFFF
    sign = (raw & 0x800000) << 8
    exp = (raw >> 16) & 127
    mant = raw & 65535
    if not exp:
        f = math.ldexp(float(mant), -78)
        return -f if sign else f
    bits = sign | ((255 if exp == 127 else exp + 64) << 23) | (mant << 7)
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmdbin")
    ap.add_argument("list_va", type=lambda s: int(s, 16))
    ap.add_argument("--uniforms", default="3,4,5,25,26,27,85,90,91,92")
    ap.add_argument("--all-uniforms", action="store_true")
    ap.add_argument("--regs", default="")
    ap.add_argument("--max", type=int, default=0, help="max printed commands")
    ap.add_argument("--dump-uniforms", default="", help="final values of these uniform indices")
    a = ap.parse_args()

    data = open(a.cmdbin, "rb").read()
    words = len(data) // 4
    W = list(struct.unpack_from("<%dI" % words, data, 0))
    want_regs = {int(x, 16) for x in a.regs.split(",") if x} if a.regs else None
    want_uni = {int(x) for x in a.uniforms.split(",")} if not a.all_uniforms else set(range(96))
    dump_uni = {int(x) for x in a.dump_uniforms.split(",")} if a.dump_uniforms else set()

    print(f"{a.cmdbin}: {len(data)} bytes ({words} words) at guest {a.list_va:#010x}")

    # VS float-uniform upload state (main.rs pica_write_register 0x2C0-0x2C8)
    cfg = None           # (is_f32, idx) from reg 0x2C0
    q = []               # pending words
    uni = {}             # idx -> [4] floats (final state)
    uni_writes = []      # (byte_offset, idx, comp, value, packed_raw)

    index = 0
    printed = 0
    ncmd = 0
    while index + 1 < words:
        index += index & 1
        if index + 1 >= words:
            break
        val = W[index]
        hdr = W[index + 1]
        hdr_off = (index + 1) * 4
        index += 2
        reg = hdr & 0xFFFF
        mask = (hdr >> 16) & 0xF
        extra = (hdr >> 20) & 0x7FF
        group = hdr >> 31
        if extra > words - index:
            print(f"  truncated at {hdr_off:#x} reg={reg:#05X} extra={extra}")
            break
        params = [(index * 4, val)]  # byte offset of the first parameter word
        for i in range(extra):
            params.append(((index + i) * 4, W[index + i]))
        index += extra
        ncmd += 1

        # replay float-uniform upload for this command's register stream
        for k, (boff, w) in enumerate(params):
            r = reg + (i_off := (k if (group and k >= 1) else 0))
            if r == 0x2C0:
                cfg = ((w >> 31) & 1, w & 0x7F)
                q = []
            elif 0x2C1 <= r <= 0x2C8 and cfg is not None:
                is_f32, idx = cfg
                q.append(w)
                need = 4 if is_f32 else 3
                if len(q) >= need:
                    if is_f32:
                        vals = [struct.unpack("<f", struct.pack("<I", q[k2] & 0xFFFFFFFF))[0]
                                for k2 in range(4)]
                        vals = list(reversed(vals))      # [3-k] assignment
                        raws = list(reversed(q[:4]))
                    else:
                        b0, b1, b2 = q[0] & 0xFFFFFFFF, q[1] & 0xFFFFFFFF, q[2] & 0xFFFFFFFF
                        raw = [b2 & 0xFFFFFF,
                               ((b1 & 0xFFFF) << 8) | ((b2 >> 24) & 0xFF),
                               ((b0 & 0xFF) << 16) | ((b1 >> 16) & 0xFFFF),
                               (b0 >> 8) & 0xFFFFFF]
                        vals = [f24_to_f32(x) for x in raw]
                        raws = raw
                    prev = uni.get(idx, [0.0, 0.0, 0.0, 1.0])
                    uni[idx] = vals
                    if idx in want_uni:
                        uni_writes.append((params[0][0], idx, vals, raws, "f32" if is_f32 else "f24"))
                    q = []
                    # main.rs pica_write_register auto-increments the 0x2C0 index
                    # after every completed group (so 12 words -> uni 25,26,27)
                    if cfg and cfg[1] < 0x7F:
                        cfg = (cfg[0], (cfg[1] + 1) & 0x7F)
                    if idx in dump_uni:
                        print(f"  UNISET {idx:3d} {'f32' if is_f32 else 'f24'} @+{params[0][0]:#06x} "
                              f"va {a.list_va + params[0][0]:#010x} = "
                              f"[{vals[0]:+.9g},{vals[1]:+.9g},{vals[2]:+.9g},{vals[3]:+.9g}]")

        if want_regs is None:
            show = a.max and printed < a.max
        else:
            show = reg in want_regs
        if show:
            print(f"  +{hdr_off - 4:#06x} va {a.list_va + hdr_off - 4:#010x} reg={reg:#05X} mask={mask:X} "
                  f"extra={extra} grp={group} vals=" +
                  ",".join(f"{w:08X}" for _, w in params[:6]))
            printed += 1

    print(f"commands={ncmd}")
    if uni_writes:
        print("--- camera-uniform upload words (byte offset in list -> guest VA) ---")
        for off, idx, vals, raws, kind in uni_writes:
            print(f"  +{off:#06x} va {a.list_va + off:#010x} UNI {idx:3d} [{kind}] = "
                  f"[{vals[0]:+.9g},{vals[1]:+.9g},{vals[2]:+.9g},{vals[3]:+.9g}]")
    else:
        print("no float-uniform writes for the requested indices")
    print("--- final uniform state for requested indices ---")
    for idx in sorted(want_uni):
        if idx in uni:
            v = uni[idx]
            print(f"  UNI {idx:3d} [{v[0]:+.9g},{v[1]:+.9g},{v[2]:+.9g},{v[3]:+.9g}]")


if __name__ == "__main__":
    main()
