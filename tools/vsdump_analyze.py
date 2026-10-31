#!/usr/bin/env python3
"""Analyze a DQ8_TRACE_VSDUMP capture (t_664244f9 POSITION_W forensics).

Re-executes each captured vertex shader program with AZAHAR semantics
(nihstro shader_bytecode.h field layout + shader_interpreter.cpp behaviour,
verified field-by-field against azahar-master) and diffs per instruction
against the C runtime's VSI trace.  Reports the FIRST divergent instruction,
then the output-register / semantic divergence.

Usage: tools/vsdump_analyze.py <dump-file> [draw-selector]
  draw-selector: index of captured draw to analyze (default: all)

Reference semantics sources (checked out at /home/hermes/DQ8/azahar-master):
  - externals/nihstro shader_bytecode.h: Instruction.{common,mad,flow_control},
    SwizzlePattern bit layout
  - src/video_core/shader/shader_interpreter.cpp: RunInterpreter
  - src/video_core/pica/output_vertex.cpp + shader_unit.cpp: output mapping
"""
import math
import re
import sys

# ---------------------------------------------------------------- f24 helpers
def f24(x):
    """Round to PICA float24 (1+7+16), ties away-from-zero like f32 round."""
    if x == 0.0 or not math.isfinite(x):
        return x
    m, e = math.frexp(x)
    m16 = round(m * 65536.0) / 65536.0
    if abs(m16) >= 1.0:
        m16 /= 2.0
        e += 1
    return math.ldexp(m16, e)

def f24_from_raw(raw):
    sign = -1.0 if (raw & 0x800000) else 1.0
    exp = (raw >> 16) & 0x7F
    mant = raw & 0xFFFF
    if exp == 0:
        return sign * math.ldexp(float(mant), -78)
    if exp == 127:
        return math.inf * sign if mant == 0 else math.nan
    return sign * math.ldexp(1.0 + mant / 65536.0, exp - 63)

# ---------------------------------------------------------------- dump parser
class Draw:
    def __init__(self):
        self.regs = {}
        self.uni = []
        self.default = []
        self.prog = []
        self.swz = []
        self.verts = []
        self.meta = {}
        self.end = {}

def parse(path):
    draws = []
    d = None
    v = None
    with open(path) as f:
        for line in f:
            t = line.split()
            if not t:
                continue
            k = t[0]
            if k == "VSDRAW":
                d = Draw()
                d.regs = {}
                d.uni = []
                d.default = []
                d.prog = []
                d.swz = []
                d.verts = []
                d.meta = dict(x.split("=", 1) for x in t[1:])
                draws.append(d)
            elif k == "VSREGS":
                for x in t[1:]:
                    r, val = x.split("=")
                    d.regs[int(r, 16)] = int(val, 16)
            elif k == "VSUNI":
                d.uni.append([float(x.split("=")[1]) for x in t[2:]])
            elif k == "VSDEF":
                d.default.append([float(x.split("=")[1]) for x in t[2:]])
            elif k == "VSPROG":
                d.prog.append(int(t[2], 16))
                assert len(d.prog) == int(t[1]) + 1
            elif k == "VSSWZ":
                d.swz.append(int(t[2], 16))
                assert len(d.swz) == int(t[1]) + 1
            elif k == "VSVERTEX":
                v = {"meta": dict(x.split("=", 1) for x in t[1:]),
                     "attrs": None, "ins": [], "in": {}, "out": {}, "sem": {}}
                d.verts.append(v)
            elif k == "VSATTRCFG":
                v["attrs"] = t[1:]
            elif k == "VSIN":
                v["in"][int(t[1])] = [float(x) for x in t[2:]]
            elif k == "VSI":
                rec = {"kind": "i"}
                for x in t[1:]:
                    kk, vv = x.split("=", 1)
                    if kk in ("a", "b", "c", "v"):
                        rec[kk] = [float(y) for y in vv.split(",")]
                    elif kk in ("w", "mask"):
                        rec[kk] = int(vv, 16)
                    else:
                        rec[kk] = int(vv)
                v["ins"].append(rec)
            elif k == "VSF":
                rec = {"kind": "f"}
                for x in t[1:]:
                    kk, vv = x.split("=", 1)
                    rec[kk] = int(vv, 16) if kk == "w" else int(vv)
                v["ins"].append(rec)
            elif k == "VSCC":
                v["ins"].append({"kind": "c", "pc": int(t[1].split("=")[1]),
                                 "w": int(t[2].split("=")[1], 16),
                                 "cc": t[3].split("=")[1]})
            elif k == "VSOUT":
                v["out"][int(t[1])] = [float(x) for x in t[2:]]
            elif k == "VSSEM":
                for x in t[1:]:
                    a, b = x.split("=")
                    v["sem"][int(a)] = float(b)
            elif k == "VSEND":
                d.end = dict(x.split("=", 1) for x in t[1:])
    return draws

# ------------------------------------------------- azahar reference execution
ONES = [1.0, 1.0, 1.0, 1.0]

class Ref:
    def __init__(self, d, vi):
        self.p = d.prog
        self.swz = d.swz
        self.u = [list(x) for x in d.uni]
        v = d.verts[vi]
        self.inp = [list(v["in"].get(i, [0.0, 0.0, 0.0, 1.0])) for i in range(16)]
        self.tmp = [[0.0] * 4 for _ in range(16)]
        self.out = [[0.0] * 4 for _ in range(16)]
        self.addr = [0, 0, 0]
        self.cc = [0, 0]
        self.entry = d.regs.get(0x2BA, 0) & 0xFFFF
        self.bools = d.regs.get(0x2B0, 0)

    def src(self, n, rel):
        if n < 16:
            return self.inp[n]
        if n < 32:
            return self.tmp[n - 16]
        i = n - 32
        if rel:
            off = self.addr[rel - 1]
            if off < -128 or off > 127:
                off = 0
            i = (i + off) & 0x7F
            if i >= 96:
                return ONES
        return self.u[i] if i < 96 else ONES

    def swsrc_direct(self, n, rel, sw, opnd):
        sh = (11, 20, 29)[opnd]
        neg = (4, 13, 22)[opnd]
        base = self.src(n, rel) if n is not None else [0.0] * 4
        out = []
        for k in range(4):
            x = base[(sw >> (sh - 2 * k)) & 3]
            if (sw >> neg) & 1:
                x = -x
            out.append(x)
        return out

def run_ref(d, vi, trace=False):
    r = Ref(d, vi)
    calls, branches = [], []
    pc = r.entry
    boundary = 1 << 30
    steps = []
    for _ in range(16384):
        cb = boundary
        pops = 0
        while pops < 4 and calls and cb == calls[-1][0]:
            end, ret = calls.pop()
            if pops < 3:
                pc = cb = ret
            pops += 1
        if branches and boundary == branches[-1][0]:
            pc = branches.pop()[1]
        if pc >= len(r.p):
            return None, steps, "pc-out"
        boundary = pc + 1
        w = r.p[pc]
        op = w >> 26
        nxt = pc + 1
        if op == 0x22:
            steps.append(("f", pc, w, nxt))
            return r, steps, None
        if op == 0x21:
            steps.append(("f", pc, w, nxt))
            pc = nxt
            continue
        if 0x24 <= op <= 0x28:
            target = (w >> 10) & 0xFFF
            n = w & 0xFF
            if op == 0x24:
                yes = True
            elif op in (0x25, 0x28):
                x = r.cc[0] == ((w >> 25) & 1)
                y = r.cc[1] == ((w >> 24) & 1)
                c = (w >> 22) & 3
                yes = (x or y) if c == 0 else (x and y) if c == 1 else x if c == 2 else y
            else:
                yes = ((r.bools >> ((w >> 22) & 15)) & 1) != 0
            if op <= 0x26:
                if yes:
                    calls.append((target + n, nxt))
                    nxt = target
            elif yes:
                branches.append((target, target + n))
            else:
                nxt = target
            steps.append(("f", pc, w, nxt))
            pc = nxt
            continue
        if op in (0x2C, 0x2D):
            if op == 0x2C:
                x = r.cc[0] == ((w >> 25) & 1)
                y = r.cc[1] == ((w >> 24) & 1)
                c = (w >> 22) & 3
                yes = (x or y) if c == 0 else (x and y) if c == 1 else x if c == 2 else y
            else:
                yes = (((r.bools >> ((w >> 22) & 15)) & 1) == (0 if (w & 1) else 1))
            steps.append(("f", pc, w, (w >> 10) & 0xFFF if yes else nxt))
            pc = (w >> 10) & 0xFFF if yes else nxt
            continue
        mad = op >= 0x30
        inv = (0x18 <= op <= 0x1B) or (0x30 <= op < 0x38)
        if mad:
            desc = w & 31
            n1 = (w >> 17) & 31
            n2 = ((w >> 12) & 31) if inv else ((w >> 10) & 127)
            n3 = ((w >> 5) & 127) if inv else ((w >> 5) & 31)
            rel = (w >> 22) & 3
            rel1 = 0
            rel2 = 0 if inv else rel
            rel3 = rel if inv else 0
            dst = (w >> 24) & 31
        else:
            desc = w & 127
            n1 = ((w >> 14) & 31) if inv else ((w >> 12) & 127)
            n2 = ((w >> 7) & 127) if inv else ((w >> 7) & 31)
            n3 = None
            rel = (w >> 19) & 3
            rel1 = 0 if inv else rel
            rel2 = rel if inv else 0
            rel3 = 0
            dst = (w >> 21) & 31
        if desc >= len(r.swz):
            return None, steps, "swz-oob"
        sw = r.swz[desc]
        a = r.swsrc_direct(n1, rel1, sw, 0)
        b = r.swsrc_direct(n2, rel2, sw, 1)
        c = r.swsrc_direct(n3, rel3, sw, 2) if n3 is not None else None
        if mad:
            v = [f24(f24(f24(a[k] * b[k])) + c[k]) for k in range(4)]
        elif op in (0x2E, 0x2F):
            def cmp(x, y, o):
                return [x == y, x != y, x < y, x <= y, x > y, x >= y, 1, 1][o]
            r.cc[0] = 1 if cmp(a[0], b[0], (w >> 24) & 7) else 0
            r.cc[1] = 1 if cmp(a[1], b[1], (w >> 21) & 7) else 0
            steps.append(("c", pc, w, a, b, None, None, dst, sw & 15))
            pc = nxt
            continue
        elif op == 0x12:
            for k in range(2):
                if (sw & (8 >> k)) and math.isfinite(a[k]) and -2147483648.0 <= a[k] < 2147483648.0:
                    r.addr[k] = int(a[k])
            steps.append(("i", pc, w, a, None, None, None, 0, sw & 15))
            pc = nxt
            continue
        elif op in (1, 2, 3, 0x18):
            aa = list(a)
            if op in (3, 0x18):
                aa[3] = 1.0
            n = 4 if op == 2 else 3
            dot = 0.0
            for k in range(n):
                dot = f24(dot + f24(aa[k] * b[k]))
            v = [dot] * 4
        else:
            v = []
            for k in range(4):
                if op == 0:
                    v.append(f24(a[k] + b[k]))
                elif op == 5:
                    v.append(f24(2.0 ** a[0]) if math.isfinite(a[0]) else float("nan"))
                elif op == 6:
                    v.append(f24(math.log2(a[0])) if a[0] > 0 else float("nan"))
                elif op == 8:
                    v.append(f24(a[k] * b[k]))
                elif op == 9 or op == 0x1A:
                    v.append(1.0 if a[k] >= b[k] else 0.0)
                elif op == 10 or op == 0x1B:
                    v.append(1.0 if a[k] < b[k] else 0.0)
                elif op == 11:
                    v.append(f24(math.floor(a[k])))
                elif op == 12:
                    v.append(a[k] if a[k] > b[k] else b[k])
                elif op == 13:
                    v.append(a[k] if a[k] < b[k] else b[k])
                elif op == 14:
                    v.append(f24(1.0 / a[0]) if a[0] != 0 else math.copysign(math.inf, a[0]))
                elif op == 15:
                    v.append(f24(1.0 / math.sqrt(a[0])) if a[0] > 0 else math.inf)
                elif op == 0x13:
                    v.append(a[k])
                else:
                    return None, steps, "op-%02X" % op
        dvec = r.out[dst] if dst < 16 else r.tmp[dst - 16]
        for k in range(4):
            if sw & (8 >> k):
                dvec[k] = v[k]
        steps.append(("i", pc, w, a, b, c, v, dst, sw & 15))
        pc = nxt
    return None, steps, "step-limit"

# ------------------------------------------------------------- diff utilities
def close(x, y):
    if math.isnan(x) and math.isnan(y):
        return True
    if not (math.isfinite(x) and math.isfinite(y)):
        return x == y
    return abs(x - y) <= 1e-3 * max(1.0, abs(x), abs(y))

def dev(x, y):
    if not (math.isfinite(x) and math.isfinite(y)):
        return 0.0
    return abs(x - y) / max(1.0, abs(x), abs(y))

def fmt(v):
    return ",".join("%+.9g" % x for x in v)

def analyze(d, vi):
    r, steps, err = run_ref(d, vi)
    print("  vertex %d: %s" % (vi, err or "ref executed %d steps" % len(steps)))
    if err:
        return
    v = d.verts[vi]
    ctr = [x for x in v["ins"] if x["kind"] in ("i", "c", "f")]
    print("    C trace %d records, ref %d steps" % (len(ctr), len(steps)))
    ndiff = 0
    maxdev = [0.0]
    if not ctr:
        print("    (no C instruction trace in dump; ref-only run)")
    for j, s in enumerate(steps):
        if not ctr:
            break
        if j >= len(ctr):
            print("    [%d] pc=%d C-TRACE-MISSING ref w=%08X" % (j, s[1], s[2]))
            ndiff += 1
            continue
        c = ctr[j]
        if c["pc"] != s[1] or c["w"] != s[2]:
            print("    [%d] DIVERGED FLOW: C pc=%d w=%08X vs ref pc=%d w=%08X"
                  % (j, c["pc"], c["w"], s[1], s[2]))
            ndiff += 1
            break
        if s[0] == "f":
            if c.get("next") is not None and c["next"] != s[3]:
                print("    [%d] pc=%d w=%08X NEXT MISMATCH: C=%d ref=%d"
                      % (j, s[1], s[2], c["next"], s[3]))
                ndiff += 1
                break
            continue
        if s[0] == "i":
            worst = 0.0
            for fld, idx in (("a", 3), ("b", 4), ("c", 5), ("v", 6)):
                rv = s[idx]
                cv = c.get(fld)
                if rv is None and cv is None:
                    continue
                if rv is None or cv is None:
                    print("    [%d] pc=%d %s: C=%s ref=%s" % (j, s[1], fld, cv, rv))
                    ndiff += 1
                    continue
                bad = [k for k in range(4) if not close(cv[k], rv[k])]
                if bad:
                    print("    [%d] pc=%d w=%08X %s MISMATCH: C=%s ref=%s"
                          % (j, s[1], s[2], fld, fmt(cv), fmt(rv)))
                    ndiff += 1
                    break
                worst = max(worst, max(dev(cv[k], rv[k]) for k in range(4)))
            if worst > maxdev[0]:
                maxdev[0] = worst
    if not ndiff:
        print("    per-instruction trace matches reference (max rel dev %.2e, f24 rounding)"
              % maxdev[0])
    # output registers + semantics
    outdiff = 0
    for rr in range(16):
        cv = v["out"].get(rr)
        rv = r.out[rr]
        if cv and any(not close(cv[k], rv[k]) for k in range(4)):
            print("    VSOUT %d MISMATCH: C=%s ref=%s" % (rr, fmt(cv), fmt(rv)))
            outdiff += 1
    # semantic mapping (ours == azahar when popcount(mask)==num_attribs)
    mask = d.regs.get(0x2BD, 0) & 0xFFFF
    attribs = d.regs.get(0x4F, 0) & 7
    sem = {}
    packed = 0
    for reg in range(16):
        if packed >= attribs or packed >= 7:
            break
        if not (mask & (1 << reg)):
            continue
        for k in range(4):
            sid = (d.regs.get(0x50 + packed, 0) >> (8 * k)) & 31
            if sid < 31:
                sem[sid] = r.out[reg][k]
        packed += 1
    for sid in range(8):
        cv = v["sem"].get(sid)
        rv = sem.get(sid, 1.0)
        if cv is not None and not close(cv, rv):
            print("    VSSEM %d MISMATCH: C=%+.9g ref=%+.9g" % (sid, cv, rv))
            outdiff += 1
    print("    semantic3 (POSITION_W): C=%+.9g ref=%+.9g"
          % (v["sem"].get(3, float("nan")), sem.get(3, float("nan"))))
    print("    outmask=%04X popcount=%d vs_outmap_total=%d"
          % (mask, bin(mask).count("1"), attribs))
    return ndiff, outdiff

def main():
    path = sys.argv[1]
    sel = int(sys.argv[2]) if len(sys.argv) > 2 else None
    draws = parse(path)
    print("parsed %d captured draws" % len(draws))
    for i, d in enumerate(draws):
        if sel is not None and i != sel:
            continue
        print("== draw %d: draw=%s t=%s count=%s entry=%s  end=%s"
              % (i, d.meta.get("draw"), d.meta.get("t"), d.meta.get("count"),
                 d.meta.get("entry"), getattr(d, "end", {})))
        for vi in range(len(d.verts)):
            analyze(d, vi)

if __name__ == "__main__":
    main()
