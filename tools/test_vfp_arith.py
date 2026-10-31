#!/usr/bin/env python3
"""Executable semantic matrix for VFP arithmetic precision (audit R2-01)
and the F64 helper family in the split ABI (audit A06-R2).

Every case is lowered with the real generator (dq8tool recompile-arm); the
generated C is compiled at -O0 AND -O2 AND under UBSan; every run's full
fpr[0..31]+fpscr snapshot must match an independent Python IEEE-754 oracle;
-O0 stdout must equal -O2 stdout bit-for-bit.  The same case is then split
with tools/split_dispatch.py, compiled as separate TUs at -O0 and -O2, and
executed: split stdout must equal single-TU stdout (single-TU == split-TU ==
oracle, which closes A06 for the F64 families).

Matrix: VADD/VSUB/VMUL/VNMUL/VDIV + MAC families VMLA/VMLS/VNMLA/VNMLS in
F32 AND F64; finite operands (1/2/3 plus values whose double words are
nonzero), destination aliases (Vd==Vn and Vd==Vm in both precisions),
FPSCR preservation, VSQRT.F64 with Vd != Vm, and explicit-reject guards:
unimplemented F64 forms (VABS/VMOV/VCVT.F64) and out-of-bank d16+
arithmetic must fail loudly via guest_unsupported (exit 15) - audit R2-01
forbids silent execution, and an explicit error is the accepted alternative.

Usage:  python3 tools/test_vfp_arith.py [--jobs N]
Env:    DQ8_TOOL (default target/debug/dq8tool), DQ8_CLANG (default clang),
        DQ8_TEST_JOBS, DQ8_TEST_KEEP=1 keeps generated C/binaries.
Exit code 0 only when every case passes at -O0/-O2, matches the oracle,
splits identically, and UBSan stays silent.
"""
import argparse
import json
import math
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
SPLIT = ROOT / "tools" / "split_dispatch.py"
ENV = {**os.environ, "DQ8_RUN_GUEST_THREADS": "1"}

FPSCR_PRESET = 0x01000000
NWORDS = 32
REJECT_RC = 15


def preset(i):
    return (0xC0DE0000 | i) & 0xFFFFFFFF


# ---------------------------------------------------------------------------
# Encoding helpers (validated against llvm-mc --mattr=+vfp2)
# ---------------------------------------------------------------------------
# VFP two-source: grp/b6 per op; bit8 (sz) selects double vs single.
GRP = {
    "mla": (0, 0), "mls": (0, 1),        # VMLA / VMLS
    "nmls": (1, 0), "nmla": (1, 1),      # VNMLS / VNMLA (bit6 inverted vs grp0)
    "mul": (2, 0), "nmul": (2, 1),       # VMUL / VNMUL
    "add": (3, 0), "sub": (3, 1),        # VADD / VSUB
    "div": (8, 0),                       # VDIV
}


def vfp2(op, double, dd, dn, dm):
    """dd/dn/dm are s-indices (0..31) for F32, d-indices (0..15) for F64."""
    grp, b6 = GRP[op]
    w = 0xEE000A00 | (grp << 20) | (b6 << 6)
    if double:
        w |= 0x100  # sz
        d_vd, d_d = dd & 15, dd >> 4
        n_vd, n_n = dn & 15, dn >> 4
        m_vm, m_m = dm & 15, dm >> 4
    else:
        d_vd, d_d = dd >> 1, dd & 1
        n_vd, n_n = dn >> 1, dn & 1
        m_vm, m_m = dm >> 1, dm & 1
    w |= (d_d << 22) | (d_vd << 12) | (n_n << 7) | (n_vd << 16) | (m_m << 5) | m_vm
    return w


def vsqrt_f64(dd, dm):
    # 0xEEB10BC0 == VSQRT.F64 d0,d0 (audit A06-R2 trigger opcode).
    return (0xEEB10BC0 | ((dd & 15) << 12) | ((dd >> 4) << 22) |
            ((dm & 15)) | ((dm >> 4) << 5))


VABS_F64, VMOV_F64, VCVT_F64_S32 = 0xEEB00BC0, 0xEEB00B40, 0xEEB80BC0


# ---------------------------------------------------------------------------
# Independent IEEE-754 oracle
# ---------------------------------------------------------------------------
def f32(x):
    return struct.unpack("<f", struct.pack("<f", float(x)))[0]


def bits32(x):
    return struct.unpack("<I", struct.pack("<f", float(x)))[0]


def bits64(x):
    return struct.unpack("<Q", struct.pack("<d", float(x)))[0]


def ref(op, double, d0, a, b):
    """Result value of `op` for operands already in the target format.
    Mirrors the emitted C semantics exactly: the multiply of a MAC form is
    rounded to the format first, then the add/negate (two f32 roundings,
    two f64 roundings), and arithmetic never touches FPSCR."""
    if not double:
        a, b, d0 = f32(a), f32(b), f32(d0)
        prod = f32(a * b)          # C: float multiply rounds once
        if op == "mla":
            return f32(d0 + prod)
        if op == "mls":
            return f32(d0 - prod)
        if op == "nmls":
            return f32(-(d0 - prod))
        if op == "nmla":
            return f32(-(d0 + prod))
        if op == "add":
            return f32(a + b)
        if op == "sub":
            return f32(a - b)
        if op == "mul":
            return f32(a * b)
        if op == "nmul":
            return f32(-(a * b))
        if op == "div":
            return f32(a / b)
    else:
        prod = a * b
        if op == "mla":
            return d0 + prod
        if op == "mls":
            return d0 - prod
        if op == "nmls":
            return -(d0 - prod)
        if op == "nmla":
            return -(d0 + prod)
        if op == "add":
            return a + b
        if op == "sub":
            return a - b
        if op == "mul":
            return a * b
        if op == "nmul":
            return -(a * b)
        if op == "div":
            return a / b
    raise ValueError(op)


# ---------------------------------------------------------------------------
# Case model
# ---------------------------------------------------------------------------
class Case:
    def __init__(self, name, words, fixture, expect, kind="arith",
                 description=""):
        self.name = name
        self.words = words
        self.fixture = fixture
        self.expect = expect      # dict slot->bits (+ "fpscr") or None
        self.kind = kind          # "arith" | "reject"
        self.description = description


def c_fixture(setup_lines):
    fmt = " ".join("f%02d=%%08X" % i for i in range(NWORDS)) + " fpscr=%08X"
    args = "".join(", fpr[%d]" % i for i in range(NWORDS)) + ", fpscr"
    setup = "\n".join("    " + ln for ln in setup_lines)
    return (
        "\nint main(void) {\n"
        "    GuestThread t = {0}; t.active = 1; t.pc = 0x100000u;\n"
        "    t.r[14] = 0xFFFFFFFFu;\n"
        f"{setup}\n"
        "    translated_from(t.r, t.pc, &t);\n"
        f"    printf(\"{fmt}\\n\"{args});\n"
        "    return 0;\n"
        "}\n"
    )


def preset_setup():
    return ["for (int i = 0; i < 32; i++) fpr[i] = 0xC0DE0000u | (unsigned)i;",
            "fpscr = 0x%08Xu;" % FPSCR_PRESET]


def slot_init_lines(inits):
    return ["fpr[%d] = 0x%08Xu;" % (s, v) for s, v in sorted(inits.items())]


def skey(slot):
    """Snapshot dict key for an fpr slot: parse_snapshot yields 'f00'..'f31'."""
    return "f%02d" % slot


def snapshot_expect(base=None):
    exp: dict = {skey(i): preset(i) for i in range(NWORDS)}
    exp["fpscr"] = FPSCR_PRESET
    if base:
        exp.update(base)
    return exp


def split_slots_f64(reg):
    return reg * 2, reg * 2 + 1


def put_value_f64(inits, exp, reg, value, result=None):
    lo = bits64(value) & 0xFFFFFFFF
    hi = bits64(value) >> 32
    lo_s, hi_s = split_slots_f64(reg)
    inits[lo_s], inits[hi_s] = lo, hi
    if result is None:
        exp[skey(lo_s)], exp[skey(hi_s)] = lo, hi
    else:
        exp[skey(lo_s)] = bits64(result) & 0xFFFFFFFF
        exp[skey(hi_s)] = bits64(result) >> 32


def arith_case(name, op, double, a, b, d0, dd, dn, dm, desc):
    """Operands: Vn=a, Vm=b, Vd=d0 (also the accumulator for MAC forms).
    For alias flavours the caller passes d0 equal to the shared operand's
    value, because Vd and that operand are the same register."""
    w = vfp2(op, double, dd, dn, dm)
    inits, exp = {}, snapshot_expect()
    result = ref(op, double, d0, a, b)
    if double:
        # operands first, destination last: on an alias the accumulator
        # value wins the shared slot, matching what the instruction reads.
        put_value_f64(inits, exp, dn, a)
        put_value_f64(inits, exp, dm, b)
        put_value_f64(inits, exp, dd, d0)
        exp[skey(dd * 2)] = bits64(result) & 0xFFFFFFFF
        exp[skey(dd * 2 + 1)] = bits64(result) >> 32
    else:
        inits[dn], inits[dm], inits[dd] = bits32(a), bits32(b), bits32(d0)
        exp[skey(dn)], exp[skey(dm)] = bits32(a), bits32(b)
        exp[skey(dd)] = bits32(result)
    setup = preset_setup() + slot_init_lines(inits)
    return Case(name, [w], c_fixture(setup), exp, "arith", desc)


def reject_case(name, word, desc):
    setup = preset_setup()
    return Case(name, [word], c_fixture(setup), None, "reject", desc)


def build_cases():
    cases = []
    # (label, Vn, Vm, Vd-init); MAC forms read Vd before writing it.
    values = [
        ("one_two", 1.0, 2.0, 0.0),   # audit finite 1/2/3 -> 3.0 for VADD
        ("frac", 1.1, 2.2, 0.5),       # both f64 words nonzero everywhere
        ("alias", 0.1, 3.0, 1.0),      # used for the alias flavours below
    ]
    ops = ["add", "sub", "mul", "nmul", "div", "mla", "mls", "nmls", "nmla"]
    # F32 division: keep the divisor a power of two so the exact quotient
    # fits in the double the oracle computes in (no double-rounding gap
    # between the oracle and the emitted float division).
    f32_div_b = {"one_two": 2.0, "frac": 0.5, "alias": 4.0}
    for double in (False, True):
        prec = "f64" if double else "f32"
        if double:
            n, m = 2, 3                 # operands d2/d3
            base_d = 1                  # result d1
        else:
            n, m = 4, 6                 # operands s4/s6
            base_d = 2                  # result s2
        for op in ops:
            for vlabel, a, b, d0 in values[:2]:
                if not double and op == "div":
                    b = f32_div_b[vlabel]
                cases.append(arith_case(
                    f"{op}_{prec}_{vlabel}", op, double, a, b, d0,
                    base_d, n, m,
                    f"{op}.{prec} non-alias {vlabel}"))
            # destination aliases, both flavours
            for alias in ("dn", "dm"):
                vlabel, a, b, d0 = values[2]
                if not double and op == "div":
                    b = f32_div_b["alias"]
                dd = n if alias == "dn" else m
                # shared slot: the accumulator IS the aliased operand's value
                acc = a if alias == "dn" else b
                cases.append(arith_case(
                    f"{op}_{prec}_alias_{alias}", op, double, a, b, acc,
                    dd, n, m,
                    f"{op}.{prec} Vd=={'Vn' if alias == 'dn' else 'Vm'}"))

    def sqrt_case(name, word, src_reg, val, dst_reg):
        inits, exp = {}, snapshot_expect()
        put_value_f64(inits, exp, src_reg, val)
        res = math.sqrt(val)
        exp[skey(dst_reg * 2)] = bits64(res) & 0xFFFFFFFF
        exp[skey(dst_reg * 2 + 1)] = bits64(res) >> 32
        setup = preset_setup() + slot_init_lines(inits)
        return Case(name, [word], c_fixture(setup), exp, "arith",
                    "VSQRT.F64 pair source/destination")

    cases.append(sqrt_case("vsqrt_f64_d0_d0", vsqrt_f64(0, 0), 0, 4.0, 0))
    cases.append(sqrt_case("vsqrt_f64_d5_d1", vsqrt_f64(5, 1), 1, 4.0, 5))
    # Explicit-reject guards (audit R2-01 alternative): loud exit, never F32.
    cases.append(reject_case("reject_vabs_f64", VABS_F64,
                             "VABS.F64 must reject explicitly"))
    cases.append(reject_case("reject_vmov_f64", VMOV_F64,
                             "VMOV.F64 must reject explicitly"))
    cases.append(reject_case("reject_vcvt_f64_s32", VCVT_F64_S32,
                             "VCVT.F64.S32 must reject explicitly"))
    cases.append(reject_case("reject_vadd_d16",
                             vfp2("add", True, 16, 1, 2),
                             "VADD.F64 d16+ is out of bank -> explicit"))
    return cases


# ---------------------------------------------------------------------------
# Lowering / compile / run (mirrors tools/test_arm_flags.py)
# ---------------------------------------------------------------------------
def generate(outdir, idx, case):
    raw = outdir / f"c{idx:04d}.code"
    src = outdir / f"c{idx:04d}.c"
    blob = b"".join(struct.pack("<I", w) for w in case.words)
    blob += struct.pack("<I", 0xE12FFF1E)
    raw.write_bytes(blob)
    p = subprocess.run([str(TOOL), "recompile-arm", str(raw), "0x100000",
                        str(src)], text=True, capture_output=True, env=ENV,
                       timeout=120)
    if p.returncode != 0:
        raise RuntimeError(f"{case.name}: dq8tool failed: {p.stderr.strip()}")
    text = src.read_text().replace("int main(int argc, char **argv)",
                                   "int original_main(int argc, char **argv)")
    src.write_text(text + case.fixture)
    return src


def compile_and_run(src, opt, split, workdir, tag):
    """-> (status, stdout, stderr); status OK|COMPILE_FAIL|UBSAN|RUN_FAIL."""
    if split:
        sdir = workdir / f"split-{tag}-{opt.strip('-')}"
        p = subprocess.run([sys.executable, str(SPLIT), str(src), str(sdir)],
                           text=True, capture_output=True, env=ENV, timeout=300)
        if p.returncode != 0:
            return ("COMPILE_FAIL", "", f"split failed: {p.stderr.strip()[:400]}")
        files = [sdir / "dq8_main.c"] + sorted(sdir.glob("dq8_part_*.c"))
        exe = workdir / f"c{tag}.{opt.strip('-')}.split.bin"
    else:
        files = [src]
        exe = workdir / f"c{tag}.{opt.strip('-')}.bin"
    if opt == "ubsan":
        cmd = [CLANG, "-std=c11", "-O1", "-w", "-fsanitize=undefined",
               "-fno-sanitize-recover=all"] + [str(f) for f in files] + \
              ["-lm", "-o", str(exe)]
    else:
        cmd = [CLANG, "-std=c11", opt, "-w"] + [str(f) for f in files] + \
              ["-lm", "-o", str(exe)]
    p = subprocess.run(cmd, text=True, capture_output=True, env=ENV, timeout=180)
    if p.returncode != 0:
        return ("COMPILE_FAIL", "", p.stderr.strip()[:600])
    p = subprocess.run([str(exe)], text=True, capture_output=True, env=ENV,
                       timeout=60)
    if opt == "ubsan":
        if p.returncode != 0 or "runtime error" in p.stderr:
            return ("UBSAN", p.stdout, f"exit={p.returncode} {p.stderr.strip()[:400]}")
        return ("OK", p.stdout, "")
    if p.returncode != 0:
        return ("RUN_FAIL", p.stdout, f"exit={p.returncode} {p.stderr.strip()[:300]}")
    return ("OK", p.stdout, p.stderr)


def parse_snapshot(stdout):
    vals = {}
    for tok in stdout.split():
        k, _, v = tok.partition("=")
        if not v:
            continue
        vals[k] = int(v, 16)
    return vals


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--jobs", type=int,
                    default=int(os.environ.get("DQ8_TEST_JOBS", "12")))
    args = ap.parse_args()
    if not TOOL.exists():
        print(f"FAIL: dq8tool not found at {TOOL}; build it first",
              file=sys.stderr)
        return 2
    outdir = Path(tempfile.mkdtemp(prefix="vfp-arith-matrix-",
                                   dir=os.environ.get("TMPDIR")))
    print(f"work dir: {outdir}")
    cases = build_cases()
    sources = []
    for idx, case in enumerate(cases):
        sources.append((idx, case, generate(outdir, idx, case)))
    print(f"generated {len(sources)} cases; compiling/running "
          f"O0/O2/ubsan + split O0/O2 with {args.jobs} jobs...")

    tasks = []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for idx, case, src in sources:
            for opt in ("-O0", "-O2"):
                tasks.append((idx, case, opt, False,
                              pool.submit(compile_and_run, src, opt, False,
                                          outdir, f"{idx:04d}")))
            if case.kind == "arith":
                tasks.append((idx, case, "ubsan", False,
                              pool.submit(compile_and_run, src, "ubsan", False,
                                          outdir, f"{idx:04d}")))
            for opt in ("-O0", "-O2"):
                tasks.append((idx, case, opt, True,
                              pool.submit(compile_and_run, src, opt, True,
                                          outdir, f"{idx:04d}")))
        results = [(idx, case, opt, split, fut.result())
                   for idx, case, opt, split, fut in tasks]

    failures, ubsan_errors, lines = [], [], []
    o0_stdout = {}
    runs_checked = 0
    for idx, case, opt, split, (status, stdout, stderr) in results:
        runs_checked += 1
        lane = "split" if split else "single"
        tag = f"{lane}/{opt} {case.name}"
        if case.kind == "reject":
            # Loud, explicit rejection: exit 15 + guest_unsupported message.
            if opt == "ubsan":
                continue
            if status != "RUN_FAIL" or "exit=15" not in stderr or \
                    "unsupported guest instruction" not in stderr:
                failures.append((tag, f"status={status} {stderr}"))
                lines.append(f"REJECT {tag} status={status} {stderr[:160]}")
            elif stdout:
                failures.append((tag, f"unexpected stdout {stdout.strip()[:80]}"))
                lines.append(f"REJECT {tag} unexpected stdout")
            else:
                lines.append(f"REJECT {tag} exit=15 ok")
            continue
        if opt == "ubsan":
            if status != "OK":
                ubsan_errors.append((case.name, stderr))
                lines.append(f"UBSAN {case.name} {stderr}")
            else:
                lines.append(f"UBSAN {case.name} silent")
            continue
        if status != "OK":
            failures.append((tag, f"{status} {stderr}"))
            lines.append(f"{tag} {status} {stderr[:200]}")
            continue
        got = parse_snapshot(stdout)
        if got != case.expect:
            diffs = {k: (hex(got.get(k, -1)), hex(v))
                     for k, v in case.expect.items() if got.get(k) != v}
            failures.append((tag, f"snapshot mismatch {diffs}"))
            lines.append(f"{tag} MISMATCH {diffs}")
            continue
        key = case.name
        if not split and opt == "-O0":
            o0_stdout[key] = stdout
        elif not split and o0_stdout.get(key) != stdout:
            failures.append((tag, "-O2 stdout differs from -O0"))
            lines.append(f"{tag} O0/O2 DIVERGE")
            continue
        elif split and o0_stdout.get(key) != stdout:
            failures.append((tag, "split stdout differs from single-TU"))
            lines.append(f"{tag} SPLIT DIVERGE")
            continue
        lines.append(f"{tag} ok")

    for ln in lines:
        print(ln)
    summary = {
        "total_cases": len(sources),
        "runs_checked": runs_checked,
        "failures": len(failures),
        "ubsan_errors": len(ubsan_errors),
        "note": ("arith cases: full fpr+fpscr snapshot vs independent "
                 "IEEE-754 oracle at O0/O2, UBSan silent, split-TU == "
                 "single-TU; reject cases: exit 15 guest_unsupported"),
    }
    print(json.dumps(summary))
    log = outdir / "vfp-arith-matrix.log"
    log.write_text("\n".join(lines) + "\n" + json.dumps(summary, indent=2) + "\n")
    print(f"log: {log}")
    if failures:
        print("FAILURES:")
        for name, data in failures[:40]:
            print(f"  {name}: {data}")
    if ubsan_errors:
        print("UBSAN ERRORS:")
        for name, data in ubsan_errors[:20]:
            print(f"  {name}: {data}")
    if os.environ.get("DQ8_TEST_KEEP") != "1" and not failures \
            and not ubsan_errors:
        for pattern in ("c*.code", "c*.c", "*.bin"):
            for f in outdir.glob(pattern):
                f.unlink()
    return 0 if not failures and not ubsan_errors else 1


if __name__ == "__main__":
    sys.exit(main())
