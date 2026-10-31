#!/usr/bin/env python3
"""Optimisation-level throughput A/B for the generated interpreter dispatch.

The full-game -O2 build is gated on clang -O2 over ~30 x 5 MB split TUs
(>12 min each; see the audit's switch-heavy-C note), which does not fit a
2 h worker budget.  This benchmark isolates the SAME generated dispatch code
(the single-TU interpreter) at -O0 vs -O2 on a fixed-work guest loop
(5M SUBS/BNE iterations = GUEST_INSNS guest instructions) and reports
M guest insn/s, giving the -O2 speedup of the dispatch core directly and
reproducibly in seconds.

Reuses the fixture/generator from tools/bench_split_dispatch.py.

Usage:  python3 tools/bench_opt_levels.py [repeats=5]
Prints one SUMMARY JSON line with per-level ns, M insn/s and the O0->O2 ratio.
"""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bench_split_dispatch as B  # noqa: E402


def build_at(src: Path, out: Path, opt: str) -> None:
    B.build_single(src, out, opt=opt)


def main() -> int:
    repeats = int(sys.argv[1]) if len(sys.argv) > 1 else 5
    work = B.tempfile.mkdtemp(prefix="dq8-opt-ab-")
    work = Path(work)

    code = work / "loop.code"
    code.write_bytes(B.arm_loop_code(B.ITERATIONS))
    src = work / "single.c"
    p = B.run([str(B.ROOT / "target" / "release" / "dq8tool"), "recompile-arm",
               str(code), "0x00100000", str(src), "--base", "0x00100000"])
    p.check_returncode()

    text = src.read_text()
    marker = "int main(int argc, char **argv)"
    assert marker in text
    src.write_text(text.replace(marker, "int original_main(int argc, char **argv)", 1)
                   .rstrip() + "\n" + B.FIXTURE)

    res = {"guest_insns": B.GUEST_INSNS, "repeats": repeats}
    for opt, tag in (("-O0", "O0"), ("-O2", "O2")):
        exe = work / f"run_{tag}"
        build_at(src, exe, opt)
        ns = B.measure(exe, repeats)  # best-of-N wall ns for the fixed loop
        mins = B.GUEST_INSNS / ns * 1e9 / 1e6
        res[tag] = {"ns": ns, "m_insn_s": round(mins, 2)}
    res["speedup_O0_over_O2"] = round(res["O0"]["ns"] / res["O2"]["ns"], 2)
    print("SUMMARY " + json.dumps(res))
    return 0


if __name__ == "__main__":
    sys.exit(main())
