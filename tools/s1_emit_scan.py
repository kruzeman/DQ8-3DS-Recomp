#!/usr/bin/env python3
"""Predict the S1 region emitter's refusals before spending a build on them.

For every candidate range this tool re-derives the region's own basic blocks
(`dq8tool extract-blocks CODE START END` — *the same call the recompiler makes
for --s1-cfg*, so window-local block boundaries match the emitter's, which are
NOT the same as the global block dump's) and applies exactly the refusal rules
of `src/block_emit.rs::emit_region`:

  * an instruction with no emitted dispatch case   (untranslated)
  * an instruction that decodes as `unknown`
  * a statement referencing a dispatch-loop local   (FORBIDDEN_TOKENS)
  * a non-final statement in a block that writes `running`
  * a fall-through/End block whose final statement does not set `pc`
  * a DP-to-PC instruction (`mov pc, …`, `add pc, …`) that the Block-IR does
    NOT treat as a block terminator (`src/block_ir.rs`: "classifying DP-to-PC
    as control flow is outside the S1a subset").  In dispatch that statement
    simply rewrites `pc` and the loop re-dispatches; inside a native-CFG
    region the block's remaining statements would still execute, so the region
    would diverge.  The only sound shape is an *unconditional* DP-to-PC as the
    block's LAST instruction whose exit edge leaves the region (then the
    dispatcher resumes at exactly the pc the statement wrote).

Refused blocks are dropped and the remaining sub-ranges are re-extracted and
re-checked until a fixpoint (splitting a window can change the block
boundaries, hence the iteration).  The output feeds DQ8_S1_CFG /
DQ8_S1_CFG_END directly.

usage: python3 tools/s1_emit_scan.py --gen target/r3-full.c 0xSTART:0xEND [...]
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "target" / "release" / "dq8tool"
CODE = ROOT / "DQ8.code"

BLOCK_RE = re.compile(r"^block start=0x([0-9A-F]{8}) end=0x([0-9A-F]{8}) insns=(\d+).* exit=(\S+)")
INSN_RE = re.compile(r"^  insn addr=0x([0-9A-F]{8}) word=\S+ cond=(\S+) kind=(\S+)")
WRITES_RE = re.compile(r"\bwrites=([0-9a-z,]*)")
CASE_RE = re.compile(r"^\s*case 0x([0-9A-F]{8}):")
FORBIDDEN = ["steps", "next_schedule_step", "next_pump_step", "next_progress_step",
             "trace_match", "trace_i", "context", "thread_execution_depth"]


def load_stmts(path: Path) -> dict:
    stmts = {}
    for line in path.read_text(errors="replace").splitlines():
        m = CASE_RE.match(line)
        if not m:
            continue
        s = line.strip()
        open_i = s.find("{ ")
        close_i = s.rfind(" break; }")
        if open_i != -1 and close_i > open_i:
            stmts[int(m.group(1), 16)] = s[open_i + 2:close_i]
    return stmts


def region_blocks(entry: int, end: int, work: Path):
    out = work / f"reg_{entry:08X}_{end:08X}.txt"
    p = subprocess.run([str(TOOL), "extract-blocks", str(CODE),
                        f"0x{entry:08X}", f"0x{end:08X}", "--out", str(out)],
                       text=True, capture_output=True)
    if p.returncode:
        raise SystemExit(f"extract-blocks 0x{entry:08X}: {p.stderr.strip()}")
    blocks, cur = [], None
    for line in out.read_text().splitlines():
        m = BLOCK_RE.match(line)
        if m:
            cur = {"start": int(m.group(1), 16), "end": int(m.group(2), 16),
                   "exit": m.group(4), "insns": []}
            blocks.append(cur)
            continue
        mi = INSN_RE.match(line)
        if mi and cur is not None:
            wm = WRITES_RE.search(line)
            cur["insns"].append((int(mi.group(1), 16), mi.group(2),
                                 mi.group(3), wm.group(1) if wm else ""))
    return blocks


def block_problem(block, stmts):
    insns = block["insns"]
    if not insns:
        return ("empty", f"block 0x{block['start']:08X} parsed with no instructions")
    for i, (addr, cond, kind, writes) in enumerate(insns):
        if "unknown" in kind:
            return ("unknown", f"insn 0x{addr:08X} decodes as unknown")
        stmt = stmts.get(addr)
        if stmt is None:
            return ("no-case", f"insn 0x{addr:08X} has no emitted dispatch case")
        for tok in FORBIDDEN:
            if tok in stmt:
                return ("forbidden", f"insn 0x{addr:08X} statement references `{tok}`")
        if i + 1 != len(insns) and "running" in stmt:
            return ("running", f"insn 0x{addr:08X} statement touches `running` mid-block")
        # DP-to-PC: the Block-IR does not end a block there, so inside a native
        # CFG the statements AFTER the jump would still run.  Sound only when
        # it is the block's last instruction and the block's exit hands control
        # straight back to the dispatcher (`end`/`return`/`indirect` -> L_EXIT).
        if kind.startswith("dp:") and "r15" in writes.split(","):
            if i + 1 != len(insns):
                return ("dp-pc", f"insn 0x{addr:08X} is DP-to-PC mid-block "
                                 f"(Block-IR does not terminate on it)")
            if block["exit"] not in ("end", "return", "indirect"):
                return ("dp-pc", f"insn 0x{addr:08X} is DP-to-PC but the block "
                                 f"exits `{block['exit']}` back into the region")
    if block["exit"] in ("fallthrough", "end"):
        last = stmts.get(insns[-1][0], "")
        if "pc" not in last:
            return ("no-pc", f"block 0x{block['start']:08X} falls through but its "
                             f"last statement does not set pc")
    return None


def split_range(entry: int, end: int, stmts, work):
    """-> (kept [(start,end)], dropped [(start,end,reason)]) for one window."""
    blocks = region_blocks(entry, end, work)
    kept, dropped, seg = [], [], None
    for b in blocks:
        prob = block_problem(b, stmts)
        if prob is None:
            seg = b["start"] if seg is None else seg
            continue
        if seg is not None:
            kept.append((seg, b["start"]))
            seg = None
        dropped.append((b["start"], b["end"], prob))
    if seg is not None:
        kept.append((seg, end))
    return kept, dropped


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--gen", required=True, type=Path)
    ap.add_argument("--max-iter", type=int, default=6)
    ap.add_argument("ranges", nargs="+", metavar="0xSTART:0xEND")
    args = ap.parse_args()

    stmts = load_stmts(args.gen)
    print(f"dispatch cases loaded: {len(stmts)}")

    final, log = [], []
    with tempfile.TemporaryDirectory(prefix="s1-scan-") as td:
        work = Path(td)
        todo = [(int(a, 16), int(b, 16)) for a, b in
                (r.split(":") for r in args.ranges)]
        for _ in range(args.max_iter):
            if not todo:
                break
            nxt, round_drops = [], []
            for entry, end in todo:
                kept, dropped = split_range(entry, end, stmts, work)
                for s, z, p in dropped:
                    log.append((entry, end, s, z, p))
                    round_drops.append((s, z, p))
                if not kept:
                    print(f"!! 0x{entry:08X}..0x{end:08X} has no convertible "
                          f"sub-range ({len(dropped)} refused block(s))")
                nxt.extend(kept)
            final = nxt
            if not round_drops:
                break      # every remaining window re-extracted clean: fixpoint
            todo = nxt     # window changed -> re-derive blocks for each split

    for entry, end, s, z, p in log:
        print(f"DROPPED 0x{s:08X}..0x{z:08X} (in 0x{entry:08X}..0x{end:08X}): "
              f"{p[0]}: {p[1]}")

    # final verification pass over the kept windows
    insns_total = 0
    with tempfile.TemporaryDirectory(prefix="s1-scan2-") as td:
        work = Path(td)
        problems = 0
        for entry, end in final:
            blocks = region_blocks(entry, end, work)
            bad = [b for b in blocks if block_problem(b, stmts)]
            n = sum(len(b["insns"]) for b in blocks)
            insns_total += n
            status = "OK" if not bad else f"BAD({len(bad)})"
            if bad:
                problems += 1
                for b in bad:
                    print(f"  {status} 0x{entry:08X}..0x{end:08X}: "
                          f"{block_problem(b, stmts)}")
            print(f"  {status} 0x{entry:08X}..0x{end:08X} blocks={len(blocks)} "
                  f"insns={n}")

    print("\nACCEPTED ranges (feed to DQ8_S1_CFG / DQ8_S1_CFG_END):")
    print("entries:", ",".join(f"0x{a:08X}" for a, _z in final))
    print("ends:   ", ",".join(f"0x{z:08X}" for _a, z in final))
    print(f"sub-ranges: {len(final)}, total static insns: {insns_total}, "
          f"windows with problems: {problems}")
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
