#!/usr/bin/env python3
"""Decide, from the game's own code, whether Thumb `ADD Rd, PC` reads
PC = instr+4 or Align(instr+4, 4).

Round-12 (t_f0271792): the esc22 escape target 0x00101943 was produced by
    ldr r3, [pc, #0x84]      (at 0x0030071C, addr % 4 == 0)
    add r3, pc               (at 0x0030071E, addr % 4 == 2)
our recompiler emits `r[3] += 0x00300720` = Align(0x0030071E+4, 4)  (src/main.rs:1425)
which yields 0x00101943 -> bytes `FF FF` (undefined Thumb).
Using instr+4 = 0x00300722 yields 0x00101945 -> the coherent function at
0x00101944 (`ldr r2,[r0,#4]; cmp; beq; ...; bx lr`).

The two rules only differ when the ADD is at addr % 4 == 2, i.e. for the
GCC pattern `ldr rd,[pc,#imm]` at addr%4==0 followed by `add rd,pc`.
For every such pair we compute both candidate targets and count how many land
on a *validated root entry* (recomp-roots.txt: 9992 entries already proven to
be real, recompilable function entries).  A large asymmetry proves the rule.
"""
import struct

CODE = "/home/hermes/DQ8/DQ8.code"
ROOTS = "/home/hermes/DQ8/recomp-roots.txt"
BASE = 0x00100000

data = open(CODE, "rb").read()
roots = set()
for line in open(ROOTS):
    line = line.strip()
    if line.startswith("0x"):
        try:
            roots.add(int(line, 16))
        except ValueError:
            pass

def u32(addr):
    off = addr - BASE
    if 0 <= off <= len(data) - 4:
        return struct.unpack_from("<I", data, off)[0]
    return None

def u16(addr):
    off = addr - BASE
    if 0 <= off <= len(data) - 2:
        return struct.unpack_from("<H", data, off)[0]
    return None

n_pairs = 0
n_code = 0
hit_noalign = hit_align = 0
in_text_noalign = in_text_noalign_n = 0
odd_examples = []

for a in range(BASE, BASE + len(data) - 4, 2):
    hw = u16(a)
    if hw is None or (hw & 0xF800) != 0x4800:      # LDR Rd, [PC, #imm]
        continue
    hw2 = u16(a + 2)
    if hw2 is None:
        continue
    if (hw2 & 0xFF00) != 0x4400:                   # ADD (register) op=0
        continue
    if (hw2 & 0x78) != 0x78:                       # Rm == 15 (PC)
        continue
    if (a + 2) % 4 != 2:                           # rules only differ here
        continue
    rd_l = (hw >> 8) & 7          # LDR literal: Rt = bits 10:8 (NOT bits 2:0)
    rd_a = ((hw2 & 7) | ((hw2 >> 4) & 8))
    if rd_l != rd_a:
        continue                                   # must write the same reg
    imm = hw & 0xFF
    lit_addr = ((a + 4) & ~3) + imm * 4
    lit = u32(lit_addr)
    if lit is None:
        continue
    pc_noinline = (a + 2) + 4                       # instr + 4
    pc_aligned = ((a + 2) + 4) & ~3                 # Align(instr+4, 4)
    t_noinline = (lit + pc_noinline) & 0xFFFFFFFF
    t_aligned = (lit + pc_aligned) & 0xFFFFFFFF
    n_pairs += 1
    code_ptr = (t_noinline & 1) or (t_aligned & 1)   # odd -> Thumb code pointer
    if code_ptr:
        n_code += 1
        if t_noinline in roots:
            hit_noalign += 1
        if t_aligned in roots:
            hit_align += 1
        if len(odd_examples) < 8:
            odd_examples.append((a, lit & 0xFFFFFFFF, t_noinline, t_aligned))

print("pairs where ADD Rd,PC sits at addr mod 4 == 2 : %d (of which Thumb code pointers: %d)"
      % (n_pairs, n_code))
print("  code-pointer target == validated root (instr+4, NO align) : %d (%.1f%%)"
      % (hit_noalign, 100.0 * hit_noalign / max(n_code, 1)))
print("  code-pointer target == validated root (Align(instr+4,4))  : %d (%.1f%%)"
      % (hit_align, 100.0 * hit_align / max(n_code, 1)))
print("examples (ldr_addr, literal, target_noinline, target_aligned):")
for e in odd_examples:
    print("   0x%08X  lit=0x%08X  -> 0x%08X / 0x%08X" % e)
