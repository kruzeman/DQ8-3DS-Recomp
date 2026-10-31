#!/usr/bin/env python3
"""Executable semantic matrix for ARM/Thumb flag & shifter bit-exactness.

Regression harness for audit findings A01-A04 (DQ8_CODE_AUDIT.md sections 3-4):
  A01 ARM logical-S shifter carry (rotated immediate, LSR/ASR #32 encodings)
  A02 Thumb immediate shifts (Rd=Rm aliasing, encoded #32, shift-on-width UB)
  A03 Thumb TST must not perform CMP (N/Z only, C/V preserved)
  A04 Thumb ADC/SBC carry out of b + carry-in must survive operand wrap

Each case is lowered with the real generator (dq8tool recompile-arm), the
generated C is compiled at -O0 AND -O2 AND under UBSan, and every run's full
NZCV plus result registers must match an independent Python reference model.
UBSan must stay silent (no shift-on-width undefined behavior).

Usage:  python3 tools/test_arm_flags.py [--jobs N]
Env:    DQ8_TOOL (path to dq8tool, default target/debug/dq8tool)
        DQ8_CLANG (default clang), DQ8_TEST_JOBS (default 8)
        DQ8_TEST_KEEP=1 keeps generated C and binaries for inspection.
Exit code 0 only when every case passes at -O0, matches -O2 bit-for-bit, and
no UBSan diagnostics were produced.
"""
import argparse
import json
import os
import struct
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOL = Path(os.environ.get("DQ8_TOOL", ROOT / "target" / "debug" / "dq8tool"))
CLANG = os.environ.get("DQ8_CLANG", "clang")
M = 0xFFFFFFFF
ENV = {**os.environ, "DQ8_RUN_GUEST_THREADS": "1"}
V_KEEP = 1  # preset V so its preservation is observable


def s32(x):
    return x - 0x100000000 if x & 0x80000000 else x


def nzv(res, c, v):
    return (res >> 31) & 1, 1 if res == 0 else 0, c, v


def ref_imm_shift(kind, src, amt, cin):
    """Immediate-form shift -> (result, carry).  Encoded amount 0 means
    LSL #0 / LSR #32 / ASR #32 / RRX for kinds 0/1/2/3 (ARM A8.5.3)."""
    if kind == 0:
        if amt == 0:
            return src, cin
        return (src << amt) & M, (src >> (32 - amt)) & 1
    if kind == 1:
        if amt == 0:
            return 0, src >> 31
        return src >> amt, (src >> (amt - 1)) & 1
    if kind == 2:
        if amt == 0:
            return (M if src >> 31 else 0), src >> 31
        return (s32(src) >> amt) & M, (src >> (amt - 1)) & 1
    if amt == 0:  # RRX
        return ((cin << 31) | (src >> 1)) & M, src & 1
    rot = amt & 31
    return ((src >> rot) | (src << (32 - rot))) & M, (src >> (rot - 1)) & 1


def ref_reg_shift(kind, src, amt, cin):
    """Register-controlled shift (amount = Rs & 255; 0 = no shift, C kept)."""
    amt &= 255
    if amt == 0:
        return src, cin
    if kind == 0:
        res = ((src << amt) & M) if amt < 32 else 0
        carry = ((src >> (32 - amt)) & 1) if amt < 32 else (src & 1 if amt == 32 else 0)
    elif kind == 1:
        res = (src >> amt) if amt < 32 else 0
        carry = ((src >> (amt - 1)) & 1) if amt < 32 else (src >> 31 if amt == 32 else 0)
    else:
        res = ((s32(src) >> amt) & M) if amt < 32 else (M if src >> 31 else 0)
        carry = ((src >> (amt - 1)) & 1) if amt < 32 else src >> 31
    return res, carry


def ref_adc(a, b, cin):
    full = a + b + cin
    sw = s32(a) + s32(b) + cin
    v = 1 if sw > 0x7FFFFFFF or sw < -0x80000000 else 0
    return full & M, full >> 32, v


def ref_sbc(a, b, cin):
    borrow = 1 - cin
    sub = b + borrow
    sw = s32(a) - s32(b) - borrow
    v = 1 if sw > 0x7FFFFFFF or sw < -0x80000000 else 0
    return (a - sub) & M, (1 if a >= sub else 0), v


def ror32(value, rot):
    rot &= 31
    if rot == 0:
        return value & M
    return ((value >> rot) | (value << (32 - rot))) & M


# ---------------------------------------------------------------------------
# Case table.  case(name, thumb, words, setup_c, expected=(r0, r1, n, z, c, v))
# ---------------------------------------------------------------------------
CASES = []
REGS4 = [0x00000000, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF]


def case(name, thumb, words, setup, r0, r1, flags):
    CASES.append((name, thumb, words, setup, (r0, M & r1, *flags)))


# --- group 1: exact audit evidence reproductions ---------------------------
case("evidence_arm_rotated_imm_carry", False, [0xE3B01102],
     "t.r[0]=0; t.c=0; t.v=1;", 0, 0x80000000, nzv(0x80000000, 1, 1))
res, carry = ref_imm_shift(1, 0x80000000, 0, 0)  # LSR #32 of r0=0x80000000
case("evidence_arm_lsr32_carry", False, [0xE1B01020],
     "t.r[0]=0x80000000u; t.c=0; t.v=1;", 0x80000000, res, nzv(res, carry, 1))
case("evidence_thumb_tst", True, [0x4208],
     "t.r[0]=1; t.r[1]=2; t.c=1; t.v=1;", 1, 2, nzv(0, 1, 1))
res, carry = ref_imm_shift(0, 0x80000000, 1, 0)
case("evidence_thumb_lsl_alias", True, [0x0040],
     "t.r[0]=0x80000000u; t.c=0; t.v=0;", res, 0, nzv(res, carry, 0))
res, carry = ref_imm_shift(1, 0x80000000, 0, 0)
case("evidence_thumb_lsr32", True, [0x0800],
     "t.r[0]=0x80000000u; t.c=0; t.v=0;", res, 0, nzv(res, carry, 0))
res, carry = ref_imm_shift(2, 0x40000000, 0, 1)
case("evidence_thumb_asr32", True, [0x1008],
     "t.r[0]=0; t.r[1]=0x40000000u; t.c=1; t.v=0;", res, 0x40000000,
     nzv(res, carry, 0))
res, carry, v = ref_adc(0, 0xFFFFFFFF, 1)
case("evidence_thumb_adc_wrap", True, [0x4148],
     "t.r[0]=0; t.r[1]=0xFFFFFFFFu; t.c=1; t.v=0;", res, 0xFFFFFFFF,
     nzv(res, carry, v))

# --- group 2: Thumb immediate shifts full matrix ---------------------------
for base, kind, tag in [(0x0000, 0, "lsl"), (0x0800, 1, "lsr"), (0x1000, 2, "asr")]:
    for src in REGS4:
        for amt in (0, 1, 31):
            for cin in (0, 1):
                res, carry = ref_imm_shift(kind, src, amt, cin)
                # alias: rd == rm == 0 (source read back after write)
                w = base | (amt << 6) | (0 << 3) | 0
                case(f"thumb_{tag}_s{src:08X}_a{amt}_c{cin}_alias", True, [w],
                     f"t.r[0]=0x{src:08X}u; t.r[1]=0; t.c={cin}; t.v={V_KEEP};",
                     res, 0, nzv(res, carry, V_KEEP))
                # distinct: rd=0, rm=1
                w = base | (amt << 6) | (1 << 3) | 0
                case(f"thumb_{tag}_s{src:08X}_a{amt}_c{cin}_dist", True, [w],
                     f"t.r[0]=0; t.r[1]=0x{src:08X}u; t.c={cin}; t.v={V_KEEP};",
                     res, src, nzv(res, carry, V_KEEP))

# --- group 3: Thumb register-controlled shifts (regression guard) ----------
for base, kind in [(0x4080, 0), (0x40C0, 1), (0x4100, 2)]:
    for src in (0x80000000, 0xFFFFFFFF):
        for amt in (0, 1, 32, 255):
            for cin in (0, 1):
                res, carry = ref_reg_shift(kind, src, amt, cin)
                w = base | (1 << 3) | 0  # rd=r0 (source+dest), rm=r1 (amount)
                case(f"thumb_rshift_k{kind}_s{src:08X}_a{amt}_c{cin}", True, [w],
                     f"t.r[0]=0x{src:08X}u; t.r[1]={amt}; t.c={cin}; t.v={V_KEEP};",
                     res, amt, nzv(res, carry, V_KEEP))

# --- group 4: Thumb TST ----------------------------------------------------
TST_PAIRS = [(1, 2), (2, 1), (0x80000000, 1), (1, 0x80000000),
             (0xFFFFFFFF, 0xFFFFFFFF), (0xAAAAAAAA, 0x55555555),
             (0, 0xFFFFFFFF), (0x80000000, 0x80000000),
             (0x7FFFFFFF, 0xFFFFFFFF), (3, 4)]
for a, b in TST_PAIRS:
    for cin in (0, 1):
        for vin in (0, 1):
            t = a & b
            w = 0x4200 | (1 << 3) | 0  # TST r0, r1 (no register write)
            case(f"thumb_tst_a{a:08X}_b{b:08X}_c{cin}_v{vin}", True, [w],
                 f"t.r[0]=0x{a:08X}u; t.r[1]=0x{b:08X}u; t.c={cin}; t.v={vin};",
                 a, b, nzv(t, cin, vin))
for a in REGS4:  # aliasing form TST r0, r0
    for cin in (0, 1):
        for vin in (0, 1):
            w = 0x4200 | (0 << 3) | 0
            case(f"thumb_tst_alias_a{a:08X}_c{cin}_v{vin}", True, [w],
                 f"t.r[0]=0x{a:08X}u; t.r[1]=0; t.c={cin}; t.v={vin};",
                 a, 0, nzv(a & a, cin, vin))

# --- group 5: Thumb ADCS / SBCS -------------------------------------------
for tag, op, ref in [("adcs", 0x4140, ref_adc), ("sbcs", 0x4180, ref_sbc)]:
    for a in REGS4:
        for b in REGS4:
            for cin in (0, 1):
                res, carry, v = ref(a, b, cin)
                w = op | (1 << 3) | 0  # rd=0, rm=1
                case(f"thumb_{tag}_a{a:08X}_b{b:08X}_c{cin}", True, [w],
                     f"t.r[0]=0x{a:08X}u; t.r[1]=0x{b:08X}u; t.c={cin}; t.v=0;",
                     res, b, nzv(res, carry, v))
    for a, b in [(0, 0xFFFFFFFF), (0x80000000, 0x80000000),
                 (0x7FFFFFFF, 0x7FFFFFFF), (1, 0xFFFFFFFF)]:
        for cin in (0, 1):
            # alias form rd == rm == 0: both operands read r0 (= a); r1 only
            # proves the instruction leaves other registers alone.
            res, carry, v = ref(a, a, cin)
            w = op | (0 << 3) | 0  # rd == rm == 0
            case(f"thumb_{tag}_alias_a{a:08X}_b{b:08X}_c{cin}", True, [w],
                 f"t.r[0]=0x{a:08X}u; t.r[1]=0x{b:08X}u; t.c={cin}; t.v=0;",
                 res, b, nzv(res, carry, v))

# --- group 6: ARM logical-S with rotated immediates ------------------------
def arm_dp_imm(op, s, rn, rd, rot, imm8):
    return (0xE2000000 | (op << 21) | (s << 20) | (rn << 16) | (rd << 12)
            | (rot << 8) | imm8)


ARM_IMM_OPS = [("movs", 13, None), ("ands", 0, lambda a, x: a & x),
               ("eors", 1, lambda a, x: a ^ x), ("orrs", 12, lambda a, x: a | x)]
for name, op, fn in ARM_IMM_OPS:
    rots = (0, 1, 2, 8, 15) if fn is None else (0, 1)
    imms = (0x01, 0x80, 0xFF) if fn is None else (0x01, 0x80)
    for rot in rots:
        for imm8 in imms:
            expanded = ror32(imm8, rot * 2) if rot else imm8
            for cin in (0, 1):
                res = expanded if fn is None else fn(0x12345678, expanded)
                carry = (expanded >> 31) if rot else cin
                case(f"arm_{name}_i{imm8:02X}_r{rot}_c{cin}", False,
                     [arm_dp_imm(op, 1, 0, 1, rot, imm8)],
                     f"t.r[0]=0x12345678u; t.c={cin}; t.v={V_KEEP};",
                     0x12345678, res, nzv(res, carry, V_KEEP))
for name, op, fn in [("tst", 8, lambda a, x: a & x), ("teq", 9, lambda a, x: a ^ x)]:
    for rot in (0, 1):
        for imm8 in (0x01, 0x80):
            expanded = ror32(imm8, rot * 2) if rot else imm8
            for cin in (0, 1):
                res = fn(0x12345678, expanded)
                carry = (expanded >> 31) if rot else cin
                case(f"arm_{name}_i{imm8:02X}_r{rot}_c{cin}", False,
                     [arm_dp_imm(op, 1, 0, 0, rot, imm8)],
                     f"t.r[0]=0x12345678u; t.c={cin}; t.v={V_KEEP};",
                     0x12345678, 0, nzv(res, carry, V_KEEP))

# --- group 7: ARM logical-S with immediate-shift register operands ---------
def arm_dp_reg(op, s, rn, rd, amount, typ, rm=0):
    return (0xE0000000 | (op << 21) | (s << 20) | (rn << 16) | (rd << 12)
            | (amount << 7) | (typ << 5) | rm)


ARM_SHIFTS = [("lsl0", 0, 0), ("lsr1", 1, 1), ("lsr32", 1, 0),
              ("asr1", 2, 1), ("asr32", 2, 0), ("rrx", 3, 0), ("ror3", 3, 3)]
for sname, typ, amount in ARM_SHIFTS:
    for src in REGS4:
        for cin in (0, 1):
            resv, carry = ref_imm_shift(typ, src, amount, cin)
            case(f"arm_movs_{sname}_s{src:08X}_c{cin}", False,
                 [arm_dp_reg(13, 1, 0, 1, amount, typ)],
                 f"t.r[0]=0x{src:08X}u; t.c={cin}; t.v={V_KEEP};",
                 src, resv, nzv(resv, carry, V_KEEP))
for sname, typ, amount in [("lsr32", 1, 0), ("asr32", 2, 0), ("rrx", 3, 0)]:
    for src in (0x80000000, 0xFFFFFFFF):
        for cin in (0, 1):
            resv, carry = ref_imm_shift(typ, src, amount, cin)
            case(f"arm_movs_{sname}_alias_s{src:08X}_c{cin}", False,
                 [arm_dp_reg(13, 1, 0, 0, amount, typ)],
                 f"t.r[0]=0x{src:08X}u; t.c={cin}; t.v={V_KEEP};",
                 resv, 0, nzv(resv, carry, V_KEEP))
for opname, op, fn in [("ands", 0, lambda a, x: a & x), ("eors", 1, lambda a, x: a ^ x)]:
    for sname, typ, amount in [("lsl0", 0, 0), ("lsr32", 1, 0),
                               ("asr32", 2, 0), ("rrx", 3, 0)]:
        for cin in (0, 1):
            resv, carry = ref_imm_shift(typ, 0x12345678, amount, cin)
            res = fn(0x12345678, resv)
            case(f"arm_{opname}_{sname}_c{cin}", False,
                 [arm_dp_reg(op, 1, 0, 1, amount, typ)],
                 f"t.r[0]=0x12345678u; t.c={cin}; t.v={V_KEEP};",
                 0x12345678, res, nzv(res, carry, V_KEEP))


# ---------------------------------------------------------------------------
# Lowering / compile / run
# ---------------------------------------------------------------------------
def generate(outdir, idx, name, thumb, words, setup):
    raw = outdir / f"c{idx:04d}.code"
    src = outdir / f"c{idx:04d}.c"
    if thumb:
        blob = struct.pack("<I", 0xE12FFF1E)  # ARM BX lr prefix
        blob += b"".join(struct.pack("<H", w) for w in words)
        blob += struct.pack("<H", 0x4770)  # Thumb BX lr
    else:
        blob = b"".join(struct.pack("<I", w) for w in words)
        blob += struct.pack("<I", 0xE12FFF1E)
    raw.write_bytes(blob)
    cmd = [str(TOOL), "recompile-arm", str(raw), "0x100000", str(src)]
    if thumb:
        cmd += ["--extra-entry", "0x100005"]
    p = subprocess.run(cmd, text=True, capture_output=True, env=ENV, timeout=120)
    if p.returncode != 0:
        raise RuntimeError(f"{name}: dq8tool failed: {p.stderr.strip()}")
    text = src.read_text().replace("int main(int argc, char **argv)",
                                   "int original_main(int argc, char **argv)")
    start = "0x100005u" if thumb else "0x100000u"
    fixture = (
        "\nint main(void) {\n"
        "    GuestThread t = {0}; t.active = 1; t.pc = START; t.r[14] = 0xFFFFFFFFu;\n"
        "    SETUP\n"
        "    translated_from(t.r, t.pc, &t);\n"
        "    printf(\"r0=%08X r1=%08X N=%d Z=%d C=%d V=%d\\n\", t.r[0], t.r[1], t.n, t.z, t.c, t.v);\n"
        "    return 0;\n"
        "}\n"
    ).replace("START", start).replace("SETUP", setup)
    src.write_text(text + fixture)
    return src


def compile_and_run(src, opt):
    exe = src.with_suffix(f".{opt.strip('-')}.bin")
    if opt == "ubsan":
        cmd = [CLANG, "-std=c11", "-O1", "-w", "-fsanitize=undefined",
               "-fno-sanitize-recover=all", str(src), "-lm", "-o", str(exe)]
    else:
        cmd = [CLANG, "-std=c11", opt, "-w", str(src), "-lm", "-o", str(exe)]
    p = subprocess.run(cmd, text=True, capture_output=True, env=ENV, timeout=180)
    if p.returncode != 0:
        return ("COMPILE_FAIL", p.stderr.strip()[:500])
    p = subprocess.run([str(exe)], text=True, capture_output=True, env=ENV, timeout=60)
    if opt == "ubsan":
        if p.returncode != 0 or p.stderr.strip():
            return ("UBSAN", f"exit={p.returncode} {p.stderr.strip()[:500]}")
        return ("OK", "")
    if p.returncode != 0:
        return ("RUN_FAIL", f"exit={p.returncode} {p.stderr.strip()[:300]}")
    return ("OK", p.stdout.strip())


def parse_flags(out):
    vals = {}
    for tok in out.split():
        k, _, v = tok.partition("=")
        vals[k] = int(v, 16) if k in ("r0", "r1") else int(v)
    return (vals["r0"], vals["r1"], vals["N"], vals["Z"], vals["C"], vals["V"])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--jobs", type=int,
                    default=int(os.environ.get("DQ8_TEST_JOBS", "8")))
    args = ap.parse_args()
    if not TOOL.exists():
        print(f"FAIL: dq8tool not found at {TOOL}; build it first", file=sys.stderr)
        return 2
    outdir = Path(tempfile.mkdtemp(prefix="arm-flags-matrix-",
                                   dir=os.environ.get("TMPDIR")))
    print(f"work dir: {outdir}")
    sources = []
    for idx, (name, thumb, words, setup, expected) in enumerate(CASES):
        sources.append((idx, name, expected,
                        generate(outdir, idx, name, thumb, words, setup)))
    print(f"generated {len(sources)} cases; compiling -O0/-O2/ubsan "
          f"with {args.jobs} jobs...")
    tasks = []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for idx, name, expected, src in sources:
            for opt in ("-O0", "-O2", "ubsan"):
                tasks.append((idx, name, opt, expected, src,
                              pool.submit(compile_and_run, src, opt)))
        results = [(idx, name, opt, expected, src, fut.result())
                   for idx, name, opt, expected, src, fut in tasks]
    failures = []
    ubsan_errors = []
    o0_out = {}
    lines = []
    for idx, name, opt, expected, src, (status, data) in results:
        if opt == "ubsan":
            if status != "OK":
                ubsan_errors.append((name, data))
                lines.append(f"UBSAN {name} {data}")
            continue
        if status != "OK":
            failures.append((name, opt, data))
            lines.append(f"{opt} {name} {status} {data}")
            continue
        got = parse_flags(data)
        ok = got == expected
        if not ok:
            failures.append((name, opt,
                             f"got={tuple(hex(x) for x in got)} "
                             f"want={tuple(hex(x) for x in expected)}"))
        lines.append(f"{opt} {name} r0={got[0]:08X} r1={got[1]:08X} "
                     f"N={got[2]} Z={got[3]} C={got[4]} V={got[5]} "
                     f"expected_ok={int(ok)}")
        if opt == "-O0":
            o0_out[name] = data
        elif o0_out.get(name) != data:
            failures.append((name, "-O2-vs-O0",
                             f"-O0:{o0_out.get(name)!r} -O2:{data!r}"))
    for ln in lines:
        print(ln)
    summary = {"total_cases": len(sources), "native_runs_checked": len(lines),
               "failures": len(failures), "ubsan_errors": len(ubsan_errors),
               "note": "every case compiled+run at -O0 and -O2 and compared "
                       "to the Python reference; -O0 output must equal -O2"}
    print(json.dumps(summary))
    log = outdir / "arm-flags-matrix.log"
    log.write_text("\n".join(lines) + "\n" + json.dumps(summary, indent=2) + "\n")
    print(f"log: {log}")
    if failures:
        print("FAILURES:")
        for name, opt, data in failures[:40]:
            print(f"  {opt} {name}: {data}")
    if ubsan_errors:
        print("UBSAN ERRORS:")
        for name, data in ubsan_errors[:20]:
            print(f"  {name}: {data}")
    if os.environ.get("DQ8_TEST_KEEP") != "1" and not failures and not ubsan_errors:
        for pattern in ("c*.code", "c*.c", "*.bin"):
            for f in outdir.glob(pattern):
                f.unlink()
    return 0 if not failures and not ubsan_errors else 1


if __name__ == "__main__":
    sys.exit(main())
