#!/usr/bin/env python3
# Scan a guest range for VFP/NEON instruction classes; flag double-precision (sz=1),
# vcvt (single<->double mixing), vmov core<->double, vmrs/vmsr.
# Usage: python3 tools/vfp_scan.py 0x003D6F70 0x003D7560
import struct, sys

start = int(sys.argv[1], 16); end = int(sys.argv[2], 16)
off = start - 0x00100000
data = open("/home/hermes/DQ8/DQ8.code", "rb").read()[off:off + (end - start)]

def sname(D, Vd): return "s%d" % ((D << 4) | Vd)
def dname(D, Vd): return "d%d" % ((D << 3) | (Vd >> 1))

for i in range(0, len(data) - 3, 4):
    w = struct.unpack("<I", data[i:i + 4])[0]
    pc = start + i
    if (w >> 28) == 0xF or (w >> 24) & 0xFE == 0xC0 or (w >> 24) & 0xF0 != 0xE0:
        # only cond=AL-ish or any; keep broad
        pass
    top = (w >> 24) & 0xFF
    if (w >> 24) & 0x0E != 0x0E:
        continue  # not a coprocessor instr (bits 27:24 must be 111x)
    if ((w >> 24) & 0x0F) != 0x0E:
        continue
    cop = (w >> 8) & 0xF
    if cop not in (0xA, 0xB):
        continue  # not VFP (cp=10/11)
    bits27_25 = (w >> 25) & 7
    notes = []
    if bits27_25 == 6:  # ldm/stm
        P = (w >> 24) & 1; U = (w >> 23) & 1; D = (w >> 22) & 1
        W = (w >> 21) & 1; L = (w >> 20) & 1; Rn = (w >> 16) & 0xF
        Vd = (w >> 12) & 0xF; sz = (w >> 8) & 1; imm8 = w & 0xFF
        kind = "vldm" if L else "vstm"
        prepost = "db" if P else "ia"
        if sz == 1:
            n = imm8 // 2
            regs = ",".join(dname(D, (Vd >> 1) + 2 * k) for k in range(min(n, 8)))
            notes.append("DOUBLE %s%s r%d%s, {%s} imm8=%d" % (kind, prepost, Rn, "!" if W else "", regs, imm8))
        else:
            notes.append("single %s%s r%d%s, {%s..%s} imm8=%d" % (kind, prepost, Rn, "!" if W else "", sname(D, Vd), sname(D, Vd + imm8 - 1) if Vd + imm8 - 1 < 16 else "s%d" % ((D << 4) + Vd + imm8 - 1), imm8))
    elif bits27_25 == 7:  # ld/st imm offset
        P = (w >> 24) & 1; U = (w >> 23) & 1; D = (w >> 22) & 1
        W = (w >> 21) & 1; L = (w >> 20) & 1; Rn = (w >> 16) & 0xF
        Vd = (w >> 12) & 0xF; sz = (w >> 8) & 1; imm8 = w & 0xFF
        kind = "vldr" if L else "vstr"
        if sz == 1:
            notes.append("DOUBLE %s %s, [r%d, #%d]" % (kind, dname(D, Vd), Rn, imm8 * 4 if U else -imm8 * 4))
        else:
            notes.append("single %s %s, [r%d, #%d]" % (kind, sname(D, Vd), Rn, imm8 * 4 if U else -imm8 * 4))
    elif bits27_25 == 4:  # dp / misc
        opc_hi = (w >> 20) & 0xF  # includes D at bit22
        D = (w >> 22) & 1; Vd = (w >> 12) & 0xF
        sz = (w >> 8) & 1; N = (w >> 7) & 1; op = (w >> 6) & 1; M = (w >> 5) & 1
        Vm = w & 0xF; Vn = (w >> 16) & 0xF
        opc20 = opc_hi & 3  # bits21:20
        if opc20 == 3 and sz == 0:
            # arithmetic / abs/neg/cmp/cvt group with bits21:20=11
            sub = (w >> 16) & 0xF
            if sub == 0:
                name = "vadd" if op == 0 else "vsub"
                notes.append("single %s %s, %s, %s" % (name, sname(D, Vd), sname(N, Vn), sname(M, Vm)))
            elif sub == 4:
                notes.append("VCMP? %s vs %s" % (sname(D, Vd), sname(M, Vm)))
            elif sub in (0xC, 0xD, 0xE):
                notes.append("VCVT %s op2=%x" % (sname(D, Vd), sub))
            else:
                notes.append("dp(bits19:16=%x) %s, %s, %s" % (sub, sname(D, Vd), sname(N, Vn), sname(M, Vm)))
        elif opc20 == 3 and sz == 1:
            notes.append("DOUBLE dp(bits19:16=%x op=%d) %s, %s, %s" % ((w >> 16) & 0xF, op, dname(D, Vd), dname(N, Vn), dname(M, Vm)))
        elif (w & 0x0F000E00) == 0x0A000A00 and ((w >> 20) & 0xF) in (0, 1) and opc20 != 3:
            notes.append("DOUBLE/other dp opc=%x sz=%d" % (opc_hi, sz))
        else:
            # vmrs/vmsr / others
            if ((w >> 20) & 0xF) == 0xF and (w & 0x0F000F00) == 0x0A000A00:
                notes.append("vmrs/vmsr reg=%d" % ((w >> 12) & 0xF))
            else:
                notes.append("dp opc=%x sz=%d N=%d op=%d M=%d Vd=%d Vn=%d Vm=%d" % (opc_hi, sz, N, op, M, Vd, Vn, Vm))
    if notes:
        print("0x%08X: %08X  %s" % (pc, w, "; ".join(notes)))
