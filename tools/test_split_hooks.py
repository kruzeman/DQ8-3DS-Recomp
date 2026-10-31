#!/usr/bin/env python3
"""A05 differential regression tests: semantic PC hooks must behave
identically in the single-TU interpreter and the split batch dispatcher.

Every case compiles the same synthetic guest twice - as one translation unit
and through tools/split_dispatch.py - at -O0 and -O2, runs both, and requires
identical guest-visible results:

  * the guest_host_snprintf shim must fire for a branch into 0x00143974
    whether the entry happens at batch start, in the middle of a batch,
    exactly on the batch-budget boundary, or via a cross-band jump.  The
    factual check is the shim's result (r0=0 for this argument setup), not a
    string in the generated C: the old A05 code let the split build execute
    the raw `MOV r0,#7` instead and printed r0=00000007;
  * a DQ8_TRACE_PC pc in the middle of a batch is reported exactly once by
    both builds;
  * the escape diagnostic's previous-PC attribution is identical, which
    requires per-instruction previous/current-PC bookkeeping inside batches;
  * with guest scheduling enabled, worker-thread slices are bounded by guest
    instructions rather than part calls in both builds;
  * the boundary and dq8_pc_needs_boundary pc sets stay in sync, so a future
    hook pc cannot silently miss the predicate (coverage check on generated C).

Run:  python3 tools/test_split_hooks.py
"""
import os
import re
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "target" / "release" / "dq8tool"
OPTS = ["-O0", "-O2"]

def sh(cmd, **kw):
    return subprocess.run(list(map(str, cmd)), text=True, capture_output=True, **kw)


def ensure_tool():
    if TOOL.exists():
        return
    p = sh(["cargo", "build", "--release"], cwd=ROOT)
    assert p.returncode == 0, p.stderr
    assert TOOL.exists(), "dq8tool did not build"


def arm(word):
    return struct.pack("<I", word)


def mov(rd, imm8):
    return arm(0xE3A00000 | (rd << 12) | (imm8 & 0xFF))


def branch(frm, to):
    return arm(0xEA000000 | (((to - frm - 8) >> 2) & 0xFFFFFF))


def bne(frm, to):
    return arm(0x1A000000 | (((to - frm - 8) >> 2) & 0xFFFFFF))


BX_LR = arm(0xE12FFF1E)
HOOK_PC = 0x00143974

HOOK_FIXTURE = r"""
int main(void) {
    ram = calloc(1, GUEST_RAM_SIZE);
    uint32_t r[16] = {0};
    r[13] = 0x10000000u; r[14] = 0xFFFFFFFFu;
    translated_from(r, 0x{entry}u, NULL);
    printf("hook r0=%08X\n", r[0]);
    return 0;
}
"""

TRACE_FIXTURE = r"""
int main(void) {
    ram = calloc(1, GUEST_RAM_SIZE);
    /* The generated main() (which we replaced) is what normally parses
       DQ8_TRACE_PC into trace_guest_pcs[]; replicate that here. */
    const char *tp = getenv("DQ8_TRACE_PC");
    if (tp) {
        char tbuf[512];
        snprintf(tbuf, sizeof tbuf, "%s", tp);
        char *tsave = NULL;
        for (char *q = strtok_r(tbuf, ",", &tsave); q;
             q = strtok_r(NULL, ",", &tsave))
            if (trace_guest_pc_count < 16u)
                trace_guest_pcs[trace_guest_pc_count++] = (uint32_t)strtoul(q, NULL, 0);
    }
    uint32_t r[16] = {0};
    r[13] = 0x10000000u; r[14] = 0xFFFFFFFFu;
    translated_from(r, 0x{entry}u, NULL);
    printf("trace r0=%08X\n", r[0]);
    return 0;
}
"""

ESCAPE_FIXTURE = r"""
int main(void) {
    ram = calloc(1, GUEST_RAM_SIZE);
    uint32_t r[16] = {0};
    r[13] = 0x10000000u; r[14] = 0x00143950u; /* never translated */
    translated_from(r, 0x00143900u, NULL);
    printf("unreachable\n");
    return 0;
}
"""

SCHED_FIXTURE = r"""
int main(void) {
    ram = calloc(1, GUEST_RAM_SIZE);
    guest_thread_count = 2;
    GuestThread *wt = &guest_threads[1];
    memset(wt, 0, sizeof *wt);
    wt->active = 1;
    wt->guest_thread_id = 0x50001u;
    wt->pc = 0x00100100u;
    wt->r[14] = 0xFFFFFFFFu;
    uint32_t r[16] = {0};
    r[13] = 0x10000000u; r[14] = 0xFFFFFFFFu;
    translated_from(r, 0x00100000u, NULL);
    printf("sched main_r0=%u worker_r1=%u\n", r[0], wt->r[1]);
    return 0;
}
"""


class Build:
    """Generated + compiled variants of one synthetic guest."""

    def __init__(self, work: Path, tag: str, code: bytes, base: int,
                 fixture: str, extras=()):
        self.work = work
        code_path = work / (tag + ".code")
        code_path.write_bytes(code)
        raw = work / (tag + ".raw.c")
        cmd = [TOOL, "recompile-arm", str(code_path), hex(base), str(raw),
               "--base", hex(base)]
        for e in extras:
            cmd += ["--extra-entry", hex(e)]
        p = sh(cmd)
        assert p.returncode == 0, (tag, p.stderr)
        text = raw.read_text()
        marker = "int main(int argc, char **argv)"
        assert marker in text, (tag, "generated main() not found")
        self.src = work / (tag + ".c")
        self.src.write_text(
            text.replace(marker, "int original_main(int argc, char **argv)", 1)
                .rstrip() + "\n" + fixture.replace(
                    "{entry}", "%08X" % base))

    def exe(self, opt: str, split: bool) -> Path:
        opt_tag = opt.replace("-", "")
        name = "%s-%s-%s" % (self.src.stem, opt_tag, "split" if split else "single")
        exe = self.work / name
        if split:
            outdir = self.work / (name + ".d")
            p = sh(["python3", ROOT / "tools" / "split_dispatch.py",
                    str(self.src), str(outdir)])
            assert p.returncode == 0, p.stderr
            sources = sorted(outdir.glob("*.c"))
            p = sh(["clang", "-std=c11", opt, "-w", "-I" + str(outdir),
                    *map(str, sources), "-lm", "-o", str(exe)])
        else:
            p = sh(["clang", "-std=c11", opt, "-w", str(self.src),
                    "-lm", "-o", str(exe)])
        assert p.returncode == 0, (name, p.stderr)
        return exe


def run_both(build: Build, env_extra=None, timeout=60):
    """Run single and split at both -O0 and -O2; return
    {(opt, variant): (rc, stdout, stderr)}."""
    env = dict(os.environ, DQ8_RUN_GUEST_THREADS="0")
    env.update(env_extra or {})
    out = {}
    for opt in OPTS:
        for split in (False, True):
            exe = build.exe(opt, split)
            p = sh([exe], env=env, timeout=timeout)
            out[(opt, split)] = (p.returncode, p.stdout, p.stderr)
    return out


class SplitHookDifferential(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        ensure_tool()
        cls.tmp = tempfile.TemporaryDirectory(prefix="dq8-a05-")
        cls.work = Path(cls.tmp.name)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def assert_identical(self, results, why):
        for opt in OPTS:
            single = results[(opt, False)]
            split = results[(opt, True)]
            self.assertEqual(single, split,
                             "%s: single vs split differ at %s\n single=%r\n split=%r"
                             % (why, opt, single, split))

    # --- the A05 counterexample: in-band branch 0x00143960 -> 0x00143974 ----
    def test_audit_repro_inband_branch(self):
        base = 0x00143960
        code = bytearray(0x1C)
        code[0x00:0x04] = branch(base, HOOK_PC)     # B 0x00143974 (in-band)
        code[0x14:0x18] = mov(0, 7)                 # MOV r0,#7 (raw body)
        code[0x18:0x1C] = BX_LR
        b = Build(self.work, "repro", bytes(code), base, HOOK_FIXTURE)
        results = run_both(b)
        self.assert_identical(results, "audit repro")
        for opt in OPTS:
            rc, out, _ = results[(opt, False)]
            self.assertEqual(rc, 0)
            self.assertIn("hook r0=00000000", out,
                          "snprintf shim must produce r0=0 (%s)" % opt)

    # --- hook entered in the middle of a batch ------------------------------
    def test_hook_mid_batch(self):
        base = 0x00143900
        code = bytearray(0x7C)
        code[0x00:0x04] = mov(0, 0xAA)
        code[0x04:0x08] = mov(0, 0x11)
        code[0x08:0x0C] = branch(base + 0x08, HOOK_PC)
        code[0x74:0x78] = mov(0, 7)
        code[0x78:0x7C] = BX_LR
        b = Build(self.work, "midbatch", bytes(code), base, HOOK_FIXTURE)
        results = run_both(b)
        self.assert_identical(results, "mid-batch hook")
        for opt in OPTS:
            self.assertIn("hook r0=00000000", results[(opt, False)][1])

    # --- hook pc as the very entry of translated_from -----------------------
    def test_hook_direct_entry(self):
        base = HOOK_PC
        code = mov(0, 7) + BX_LR
        b = Build(self.work, "directentry", code, base, HOOK_FIXTURE)
        results = run_both(b)
        self.assert_identical(results, "direct hook entry")
        for opt in OPTS:
            self.assertIn("hook r0=00000000", results[(opt, False)][1])

    # --- hook pc landing exactly on the batch-budget boundary ---------------
    def test_hook_at_batch_budget_boundary(self):
        # N straight-line MOVs, then a branch into the hook pc: the batch
        # budget (256 dispatches) expires at a different point for every N,
        # so one of these alignments puts the hook pc right at the boundary
        # and the rest cover +-2 dispatches around it.
        for n in (253, 254, 255, 256):
            base = 0x00143000
            code = bytearray(0x97C)
            for i in range(n):
                code[4 * i:4 * i + 4] = mov(0, 0x11)
            at = base + 4 * n
            code[4 * n:4 * n + 4] = branch(at, HOOK_PC)
            code[0x974:0x978] = mov(0, 7)
            code[0x978:0x97C] = BX_LR
            b = Build(self.work, "budget%d" % n, bytes(code), base, HOOK_FIXTURE)
            results = run_both(b)
            self.assert_identical(results, "budget boundary n=%d" % n)
            for opt in OPTS:
                self.assertIn("hook r0=00000000", results[(opt, False)][1],
                              "n=%d %s" % (n, opt))

    # --- cross-band branch into the hook pc ---------------------------------
    def test_cross_band_hook(self):
        base = 0x00100000
        code = bytearray(HOOK_PC + 8 - base)
        code[0x00:0x04] = mov(0, 0xAA)
        code[0x04:0x08] = branch(base + 0x04, HOOK_PC)  # band 0x100 -> 0x143
        code[HOOK_PC - base:HOOK_PC - base + 4] = mov(0, 7)
        code[HOOK_PC - base + 4:HOOK_PC - base + 8] = BX_LR
        b = Build(self.work, "crossband", bytes(code), base, HOOK_FIXTURE)
        results = run_both(b)
        self.assert_identical(results, "cross-band hook")
        for opt in OPTS:
            self.assertIn("hook r0=00000000", results[(opt, False)][1])

    # --- trace pc in the middle of a batch ----------------------------------
    def test_trace_pc_mid_batch(self):
        base = 0x00143900
        code = bytearray(0x10)
        code[0x00:0x04] = mov(0, 0xAA)
        code[0x04:0x08] = mov(0, 0x11)     # traced, mid-batch, not a hook pc
        code[0x08:0x0C] = mov(0, 0x22)
        code[0x0C:0x10] = BX_LR
        b = Build(self.work, "tracepc", bytes(code), base, TRACE_FIXTURE)
        env = {"DQ8_TRACE_PC": "0x00143904", "DQ8_TRACE_PC_MAX": "64"}
        results = run_both(b, env_extra=env)
        self.assert_identical(results, "trace pc")
        for opt in OPTS:
            for split in (False, True):
                _, out, err = results[(opt, split)]
                self.assertIn("trace r0=00000022", out)
                hits = [l for l in err.splitlines()
                        if l.startswith("guest trace pc=00143904")]
                self.assertEqual(len(hits), 1,
                                 "trace pc must be reported exactly once "
                                 "(%s, split=%s): %r" % (opt, split, err))

    # --- previous-PC attribution in the escape diagnostic -------------------
    def test_escape_previous_pc_attribution(self):
        base = 0x00143900
        code = bytearray(0x10)
        code[0x00:0x04] = mov(0, 0xAA)
        code[0x04:0x08] = mov(0, 0x11)
        code[0x08:0x0C] = BX_LR           # pc := r14 = 0x00143950 (untranslated)
        code[0x0C:0x10] = BX_LR
        b = Build(self.work, "escape", bytes(code), base, ESCAPE_FIXTURE)
        results = run_both(b)
        self.assert_identical(results, "escape attribution")
        for opt in OPTS:
            rc, _, err = results[(opt, False)]
            self.assertEqual(rc, 3, (opt, err))
            self.assertIn("translated PC escaped to 00143950 after 00143908",
                          err, "previous-PC attribution must equal the last "
                               "dispatched instruction (%s)" % opt)

    # --- guest scheduling enabled: slices bounded by instructions -----------
    def test_guest_scheduling_slices(self):
        base = 0x00100000
        iterations = 125000              # ~250k guest instructions total
        lo = iterations & 0xFF
        hi = (iterations >> 8) & 0xFF
        mid = (iterations >> 16) & 0xFF
        setup = [0xE3A00000 | lo]                          # MOV r0, #lo
        if hi:
            setup.append(0xE3800000 | (12 << 8) | hi)      # ORR r0,r0,#hi<<8
        if mid:
            setup.append(0xE3800000 | (8 << 8) | mid)      # ORR r0,r0,#mid<<16
        worker = 0x00100100
        code = bytearray(0x110)
        pc = base
        for w in setup:
            code[pc - base:pc - base + 4] = arm(w)
            pc += 4
        loop = pc
        code[pc - base:pc - base + 4] = arm(0xE2500001)    # SUBS r0, r0, #1
        pc += 4
        code[pc - base:pc - base + 4] = bne(pc, loop)       # BNE loop
        pc += 4
        code[pc - base:pc - base + 4] = BX_LR
        # worker: an infinite ADD/B loop, resumed one slice per schedule
        code[worker - base:worker - base + 4] = arm(0xE2811001)  # ADD r1,r1,#1
        code[worker - base + 4:worker - base + 8] = branch(worker + 4, worker)
        b = Build(self.work, "sched", bytes(code), base, SCHED_FIXTURE,
                  extras=(worker,))
        results = run_both(b, env_extra={"DQ8_RUN_GUEST_THREADS": "1"})
        counts = {}
        for opt in OPTS:
            for split in (False, True):
                rc, out, err = results[(opt, split)]
                self.assertEqual(rc, 0, (opt, split, err))
                m = re.search(r"main_r0=(\d+) worker_r1=(\d+)", out)
                self.assertIsNotNone(m, out)
                counts[(opt, split)] = (int(m.group(1)), int(m.group(2)))
        for opt in OPTS:
            main_s, worker_s = counts[(opt, False)]
            main_p, worker_p = counts[(opt, True)]
            self.assertEqual(main_s, 0, "main loop must finish (%s)" % opt)
            self.assertEqual(main_p, 0, "main loop must finish (split %s)" % opt)
            # Both builds must schedule (worker progressed at all), and the
            # worker slice must be bounded by guest instructions: the A05 code
            # counted part calls, letting a slice run ~256x longer (or never
            # fire at all).  A slice is 5000 dispatches +- one batch (256).
            self.assertGreaterEqual(worker_s, 4900, "single never scheduled")
            self.assertGreaterEqual(worker_p, 4900,
                                    "split never scheduled (%s)" % opt)
            self.assertLessEqual(abs(worker_s - worker_p), 300,
                                 "slice bounded by instructions, not part "
                                 "calls: single=%d split=%d (%s)"
                                 % (worker_s, worker_p, opt))

    # --- coverage: boundary pc set must be a subset of the predicate --------
    def test_predicate_covers_boundary_pcs(self):
        base = 0x00100000
        code = mov(0, 1) + BX_LR
        tiny = self.work / "coverage.code"
        tiny.write_bytes(code)
        raw = self.work / "coverage.raw.c"
        p = sh([TOOL, "recompile-arm", str(tiny), hex(base), str(raw),
                "--base", hex(base)])
        self.assertEqual(p.returncode, 0, p.stderr)
        text = raw.read_text()

        b_start = text.index("while (running && steps++ <")
        b_end = text.index("switch (pc >> 12) {", b_start)
        boundary = text[b_start:b_end]
        f_start = text.index("static int guest_fast_page_list(")
        f_end = text.index("static void svc_exit_thread(", f_start)
        fastpage = text[f_start:f_end]
        p_start = text.index("static int dq8_pc_needs_boundary(")
        p_end = text.index("return 0;\n}", p_start) + len("return 0;\n}")
        predicate = text[p_start:p_end]

        def boundary_pcs(region):
            pats = [
                r"\(pc\s*&\s*~1u\)\s*==\s*(0x[0-9A-Fa-f]+)u?",
                r"(?<![\w.])pc\s*==\s*(0x[0-9A-Fa-f]+)u?",
                r"\*pc\s*[!=]=\s*(0x[0-9A-Fa-f]+)u?",
            ]
            pcs = set()
            for pat in pats:
                for m in re.finditer(pat, region):
                    pcs.add(int(m.group(1), 16))
            return pcs

        needed = boundary_pcs(boundary) | boundary_pcs(fastpage)
        provided = {int(m.group(1), 16)
                    for m in re.finditer(r"case\s+(0x[0-9A-Fa-f]+)u?:",
                                         predicate)}
        missing = sorted(pc for pc in needed if pc not in provided)
        self.assertEqual(
            missing, [],
            "dq8_pc_needs_boundary is missing boundary pc(s): %s"
            % ", ".join("0x%08X" % pc for pc in missing))
        # The runtime DQ8_TRACE_PC list must be consulted as well.
        self.assertIn("trace_guest_pc_count", predicate)
        self.assertIn("trace_guest_pcs", predicate)
        # The page prefilter must be compile-time initialised from exactly
        # the same pc set: a case pc whose page is missing from
        # dq8_pc_boundary_page would silently disable that hook through the
        # fast path (a prefilter 0 short-circuits the exact switch).
        tbl_start = text.index(
            "static const unsigned char dq8_pc_boundary_page[4096]")
        tbl_end = text.index("};", tbl_start)
        tbl_pages = {int(m.group(1), 16)
                     for m in re.finditer(r"\[(0x[0-9A-Fa-f]+)u?\]",
                                          text[tbl_start:tbl_end])}
        self.assertEqual(
            tbl_pages, {pc >> 12 for pc in provided},
            "dq8_pc_boundary_page prefilter out of sync with predicate cases")
        # Runtime DQ8_TRACE_PC pcs are not in the static table, so the fast
        # path must fall back to the exact checks while that list is non-empty.
        self.assertRegex(predicate, r"trace_guest_pc_count == 0")
        # The fast prefilter itself must exist and gate the exact switch.
        self.assertRegex(
            predicate,
            r"dq8_pc_boundary_page\[\(pc >> 12\) & 0xFFFu\]")
        # Sanity: the boundary really does exist in this generated C.
        self.assertGreater(len(needed), 10, "boundary pc scan found too few")


if __name__ == "__main__":
    unittest.main(verbosity=2)
