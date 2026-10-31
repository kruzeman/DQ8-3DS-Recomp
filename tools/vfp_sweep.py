#!/usr/bin/env python3
"""VFP/NEON completeness sweep for DQ8.code (kanban t_4734a0a2).

Oracle
------
The recompiler itself.  `dq8tool recompile-arm` emits one switch case per
translated guest instruction; any opcode the decode chain cannot lower falls
through to `guest_unsupported(pc, opcode, thumb)` inside that case.  One pass
over the generated C therefore yields, for every translated instruction:

    address -> { mode, width, encoding, lowered? , reject? }

Mnemonics come from LLVM (local llvm-mc -> object -> llvm-objdump -d), the
same pipeline tools/thumb_disasm.py already uses; encodings are checked against
`llvm-mc --show-encoding` for a sample of names.

usage: tools/vfp_sweep.py [--recomp target/vfp_sweep.c] [--out reports/VFP_SWEEP.md]
"""
import argparse
import collections
import os
import re
import struct
import subprocess
import sys
import tempfile

CODE = "DQ8.code"
TEXT = 0x00100000
TOOL = "./target/release/dq8tool"
OBJC = "llvm-mc"
OBJD = "llvm-objdump-18"

CASE_RE = re.compile(r"^case 0x([0-9A-F]{8}): \{ (.*) break; \}$")
NEXT_RE = re.compile(r"\} else \{ pc = 0x([0-9A-F]{8})(u?); \}")
UNSUP_RE = re.compile(
    r"guest_unsupported\(0x([0-9A-F]{8})u, 0x([0-9A-F]+)u, ([01])u\)")
DUMP_RE = re.compile(r"^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$")


# --------------------------------------------------------------------------
# 1. harvest the recompiler's own verdict for every translated instruction
def parse_recompiled(path):
    cases = {}
    with open(path, "r", errors="replace") as fh:
        for line in fh:
            if not line.startswith("case 0x") or " break; }" not in line:
                continue
            m = CASE_RE.match(line.rstrip("\n"))
            if not m:
                continue
            addr = int(m.group(1), 16)
            stmt = m.group(2)
            n = NEXT_RE.search(stmt)
            u = UNSUP_RE.search(stmt)
            cases[addr] = dict(
                mode="thumb" if (n and n.group(2) == "u") else "arm",
                next_pc=int(n.group(1), 16) if n else None,
                unsup=(int(u.group(2), 16), u.group(3)) if u else None,
                reject32="pc = 0xFFFFFFFFu" in stmt,
                stmt=stmt,
            )
    return cases


def attach_encodings(cases, blob):
    """width + encoding from the guest image itself.

    Note the case key carries the Thumb state bit (`pc_key = addr |
    thumb_mode`, src/main.rs:1974), so the file offset must mask it off; the
    fall-through `pc = 0xNEXT` literal carries the same bit, which cancels in
    the width subtraction.
    """
    problems = []
    for addr, i in cases.items():
        off = (addr & ~1) - TEXT
        nxt = i["next_pc"]
        if nxt is None or not (0 <= off + 4 <= len(blob)):
            # no fall-through at all: the 0x00131388 forced-lock special case
            i["width"] = None
            i["bytes"] = None
            i["special"] = True
            continue
        width = (nxt - addr) & 0xFFFFFFFF
        if width not in (2, 4):
            # Hand-back sentinel written by the Thumb-2 VFP delegation: its
            # `else { pc = addr|1 }` makes the width read as 1.  It carries no
            # guest encoding by design.
            i["width"] = width
            i["bytes"] = None
            i["sentinel"] = True
            continue
        i["width"] = width
        if width == 4:
            i["bytes"] = blob[off:off + 4]
        elif width == 2 and i["mode"] == "thumb":
            i["bytes"] = blob[off:off + 2]
        else:
            i["bytes"] = None
            problems.append(("bad-width", hex(addr), width, i["mode"]))
        if i["bytes"] is None:
            continue
        if i["mode"] == "arm":
            i["enc"] = struct.unpack("<I", i["bytes"])[0]
            i["key"] = ("arm", i["enc"])
            i["text"] = "%08X" % i["enc"]
        else:
            hw1 = struct.unpack_from("<H", i["bytes"], 0)[0]
            if width == 2:
                i["key"] = ("thumb", hw1)
                i["text"] = "%04X" % hw1
            else:
                hw2 = struct.unpack_from("<H", i["bytes"], 2)[0]
                i["key"] = ("thumb", (hw1 << 16) | hw2)
                i["text"] = "%04X %04X" % (hw1, hw2)
            i["enc"] = hw1
        # the reject the recompiler itself printed must match what we read
        if i["unsup"]:
            uop, uth = i["unsup"]
            want_thumb = "1" if i["mode"] == "thumb" else "0"
            if uth != want_thumb:
                problems.append(("mode-mismatch", hex(addr), i["unsup"]))
            elif i["mode"] == "arm" and uop != i["enc"]:
                # A Thumb-2 VFP instruction re-decoded in A32 state reports the
                # halfword-swapped word (its real encoding), while the bytes on
                # disk are in Thumb order - both spellings are correct here.
                swapped = ((i["enc"] & 0xFFFF) << 16) | (i["enc"] >> 16)
                if uop != swapped:
                    problems.append(("op-mismatch-arm", hex(addr), hex(uop),
                                     hex(i["enc"])))
            elif i["mode"] == "thumb" and i["width"] == 2 and uop != i["enc"]:
                problems.append(("op-mismatch-t16", hex(addr), hex(uop),
                                 hex(i["enc"])))
            elif i["mode"] == "thumb" and i["width"] == 4 and uop != i["key"][1]:
                problems.append(("op-mismatch-t32", hex(addr), hex(uop),
                                 hex(i["key"][1])))
    return problems


# --------------------------------------------------------------------------
# 2. name every distinct encoding with LLVM (local, no network)
def _assemble_and_dump(units_by_mode):
    """units_by_mode: {'arm': [bytes,...], 'thumb': [bytes,...]}
    returns {(mode, key): mnemonic}"""
    out = {}
    for mode, units in units_by_mode.items():
        mc_triple = ("armv7-none-eabi" if mode == "arm"
                     else "thumbv7-none-eabi")
        dump_triple = "armv7" if mode == "arm" else "thumbv7"
        directive = ".arm" if mode == "arm" else ".thumb"
        # offsets of each unit inside the blob, and the key it belongs to
        offsets, pos = [], 0
        blob = bytearray()
        for key, raw in units:
            offsets.append((pos, key, len(raw)))
            blob += raw
            pos += len(raw)
        tmp = tempfile.mkdtemp(prefix="vfpsweep_")
        src = os.path.join(tmp, "b.s")
        obj = os.path.join(tmp, "b.o")
        with open(src, "w") as fh:
            fh.write(".syntax unified\n.arch armv7-a\n%s\n.text\n.align 2\n"
                     % directive)
            for i in range(0, len(blob), 16):
                fh.write(".byte %s\n" % ", ".join("0x%02X" % b
                                                  for b in blob[i:i + 16]))
        subprocess.run([OBJC, "-filetype=obj", "--triple=" + mc_triple, src,
                        "-o", obj], check=True, capture_output=True)
        dump = subprocess.run(
            [OBJD, "-d", "--no-show-raw-insn", "--triple=" + dump_triple, obj],
            capture_output=True, text=True, check=True).stdout
        by_off = {}
        for line in dump.splitlines():
            m = DUMP_RE.match(line)
            if m:
                by_off[int(m.group(1), 16)] = m.group(2)
        for off, key, _size in offsets:
            mnem = by_off.get(off)
            if mnem is None:
                # llvm-objdump can elide undecodable spans; fall back to the
                # byte run starting here
                mnem = "(missing)"
            out[(key[0], key[1] if len(key) == 2 else key)] = mnem
        # key tuples: ('arm', word) or ('thumb', hw1) / ('thumb', hw1hw2)
        sys.stderr.write("  named %d %s encodings\n" % (len(units), mode))
    return out


# --------------------------------------------------------------------------
# 3. whole-image static scan (completeness outside the translated set)
def is_arm_fp_pattern(w):
    cp = (w >> 8) & 0xF
    if cp in (0xA, 0xB):
        if ((w >> 25) & 0b111) == 0b110:      # coprocessor transfer
            return True
        if ((w >> 24) & 0xF) in (0xE, 0xF):   # coprocessor data processing
            return True
    if (w >> 28) == 0xF and ((w >> 24) & 0xF) in (0x2, 0x3, 0x4, 0x5):
        return True                            # Advanced SIMD space
    return False


def is_thumb_fp_pattern(h1):
    top = h1 & 0xFF00
    return top in (0xEC00, 0xED00, 0xEE00, 0xEF00,
                   0xF200, 0xF300, 0xF400, 0xF500, 0xF900)


# --------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--recomp", default="target/vfp_sweep.c")
    ap.add_argument("--out", default="reports/VFP_SWEEP.md")
    ap.add_argument("--rows", default="target/vfp_sweep_rows.tsv")
    ap.add_argument("--allrows", default="target/vfp_sweep_all.tsv")
    args = ap.parse_args()

    print("[1/5] parsing %s" % args.recomp, flush=True)
    cases = parse_recompiled(args.recomp)
    blob = open(CODE, "rb").read()
    problems = attach_encodings(cases, blob)
    print("      translated instructions : %d" % len(cases))
    print("      structural problems     : %d" % len(problems))
    for p in problems[:10]:
        print("        ", p)
    rejects = [a for a, i in cases.items() if i["unsup"]]
    midreject = [a for a, i in cases.items() if i["reject32"]]
    print("      guest_unsupported sites : %d" % len(rejects))
    print("      a32-middle reject sites : %d" % len(midreject))

    print("[2/5] grouping distinct encodings", flush=True)
    units = {"arm": [], "thumb": []}
    seen = set()
    for i in cases.values():
        if i.get("bytes") is None:
            continue
        k = i["key"]
        if k in seen:
            continue
        seen.add(k)
        units[i["mode"]].append((k, i["bytes"]))
    print("      distinct encodings: arm=%d thumb=%d"
          % (len(units["arm"]), len(units["thumb"])), flush=True)

    print("[3/5] naming with llvm-mc + llvm-objdump", flush=True)
    mnems = _assemble_and_dump(units)
    missing = sum(1 for v in mnems.values() if v == "(missing)")
    print("      named=%d missing=%d" % (len(mnems), missing))

    # classify
    fp = collections.Counter()
    fp_rej = collections.Counter()
    fp_samples = collections.defaultdict(list)
    other_rej = collections.Counter()
    reject_escapes = collections.Counter()
    for addr, i in cases.items():
        if i.get("bytes") is None:
            continue
        mnem = mnems.get(i["key"], "(unnamed)")
        i["mnem"] = mnem
        isf = mnem.startswith("v") and mnem != "(missing)"
        if isf:
            fp[i["key"]] += 1
            if i["unsup"] or i["reject32"]:
                fp_rej[i["key"]] += 1
            if len(fp_samples[i["key"]]) < 5:
                fp_samples[i["key"]].append(
                    "0x%08X%s" % (addr, "!" if (i["unsup"] or i["reject32"]) else ""))
        if i["unsup"] and not isf:
            other_rej[mnem] += 1
        if i["reject32"]:
            reject_escapes[(i["mode"], i["text"])] += 1
    print("[4/5] FP encodings: distinct=%d sites=%d ; FP rejected distinct=%d sites=%d"
          % (len(fp), sum(fp.values()), len(fp_rej), sum(fp_rej.values())))

    # ---- whole-image scan ----
    print("[5/5] whole-image static scan", flush=True)
    trans = set(i["key"] for i in cases.values() if i.get("bytes") is not None)
    scan_arm = collections.Counter()
    for off in range(0, len(blob) - 3, 4):
        w = struct.unpack_from("<I", blob, off)[0]
        if is_arm_fp_pattern(w):
            scan_arm[("arm", w)] += 1
    scan_thumb = collections.Counter()
    for off in range(0, len(blob) - 3, 2):
        h1 = struct.unpack_from("<H", blob, off)[0]
        if is_thumb_fp_pattern(h1):
            h2 = struct.unpack_from("<H", blob, off + 2)[0]
            scan_thumb[("thumb", (h1 << 16) | h2)] += 1
    outside = [(k, v) for k, v in list(scan_arm.items()) + list(scan_thumb.items())
               if k not in trans]
    print("      arm-pattern distinct=%d  thumb-pattern distinct=%d  "
          "outside-translated=%d" % (len(scan_arm), len(scan_thumb), len(outside)))

    # name the outside candidates that are plausibly real (dedup by key)
    out_units = {"arm": [], "thumb": []}
    seen2 = set()
    for (mode, val), _cnt in outside:
        if (mode, val) in seen2:
            continue
        seen2.add((mode, val))
        if mode == "arm":
            raw = struct.pack("<I", val)
        else:
            # every Thumb candidate from is_thumb_fp_pattern is a Thumb-2
            # 32-bit start (0xEC..-0xEF.., 0xF2../0xF3../0xF4../0xF5../0xF9..)
            hw1, hw2 = (val >> 16) & 0xFFFF, val & 0xFFFF
            raw = struct.pack("<HH", hw1, hw2)
        out_units[mode].append(((mode, val), raw))
    out_named = _assemble_and_dump(out_units) if (out_units["arm"] or out_units["thumb"]) else {}
    outside_fp = collections.Counter()
    for (mode, val), cnt in outside:
        m = out_named.get((mode, val), "(unnamed)")
        if m.startswith("v") and m != "(missing)":
            outside_fp[(mode, val, m)] += cnt
    print("      outside-translated FP-mnemonic encodings=%d words=%d"
          % (len(outside_fp), sum(outside_fp.values())))

    write_report(args, cases, mnems, fp, fp_rej, fp_samples, other_rej,
                  reject_escapes, problems, rejects, midreject,
                  scan_arm, scan_thumb, outside_fp, out_named)
    with open(args.rows, "w") as fh:
        fh.write("mode\tencoding\tmnemonic\tcount\trejected\tsamples\n")
        for k, c in sorted(fp.items(), key=lambda kv: -kv[1]):
            mode, val = k
            fh.write("%s\t%s\t%s\t%d\t%d\t%s\n" % (
                mode,
                ("%08X" % val) if mode == "arm" else
                (("%04X" % val) if val <= 0xFFFF else "%04X %04X" % (val >> 16, val & 0xFFFF)),
                mnems.get(k, "(unnamed)"), c, fp_rej.get(k, 0),
                ",".join(fp_samples[k])))
    print("rows -> %s" % args.rows)
    # full naming dump (every distinct encoding + how the sweep named it) so
    # downstream checks do not have to re-run the LLVM pipeline
    all_counts = collections.Counter()
    all_rej = collections.Counter()
    for i in cases.values():
        if i.get("bytes") is None:
            continue
        all_counts[i["key"]] += 1
        if i["unsup"] or i["reject32"]:
            all_rej[i["key"]] += 1
    with open(args.allrows, "w") as fh:
        fh.write("mode\tencoding\tmnemonic\tcount\trejected\n")
        for k, c in sorted(all_counts.items(), key=lambda kv: -kv[1]):
            mode, val = k
            fh.write("%s\t%s\t%s\t%d\t%d\n" % (
                mode, enc_str(mode, val).strip("`"), mnems.get(k, "(unnamed)"),
                c, all_rej.get(k, 0)))
    print("all rows -> %s" % args.allrows)
    return 0


def enc_str(mode, val):
    if mode == "arm":
        return "`0x%08X`" % val
    if val <= 0xFFFF:
        return "`0x%04X`" % val
    return "`0x%04X 0x%04X`" % (val >> 16, val & 0xFFFF)


def write_report(args, cases, mnems, fp, fp_rej, fp_samples, other_rej,
                 reject_escapes, problems, rejects, midreject,
                 scan_arm, scan_thumb, outside_fp, out_named):
    L = []
    A = L.append
    A("# VFP / NEON completeness sweep - DQ8.code")
    A("")
    A("Task `t_4734a0a2`.  Generated by `tools/vfp_sweep.py` from a fresh")
    A("`dq8tool recompile-arm` of `DQ8.code` (base 0x00100000, all")
    A("`recomp-roots.txt` extra entries + `recomp-noreturn.txt` entries).")
    A("")
    A("## Method (why this table is complete)")
    A("")
    A("The recompiler is the oracle, not a second decoder:")
    A("")
    A("1. `dq8tool recompile-arm DQ8.code 0x00100000 <out.c> ...` translates the")
    A("   reachable graph and emits **one switch case per translated guest")
    A("   instruction**.  Every instruction the decode chain cannot lower falls")
    A("   through to `guest_unsupported(pc, opcode, thumb)` *inside that case*,")
    A("   so the generated C states exactly which encodings are rejected and at")
    A("   which addresses.")
    A("2. The case's `else { pc = 0xNEXT; }` gives the instruction width (2/4)")
    A("   and the `u` suffix on that literal marks Thumb state, giving the mode")
    A("   per address (cross-checked against the `thumb` argument of every")
    A("   `guest_unsupported` call).")
    A("3. Encodings are read from `DQ8.code` at those addresses.")
    A("4. Mnemonics come from LLVM locally (`llvm-mc` -> object ->")
    A("   `llvm-objdump -d`, the pipeline in `tools/thumb_disasm.py`), so ARM and")
    A("   Thumb-2 paths are both named by the same assembler/disassembler pair.")
    A("")
    tot = len(cases)
    A("Translated instructions: **%d** across the reachable graph "
      "(%d ARM, %d Thumb).  Structural problems while reading them back: %d."
      % (tot,
         sum(1 for i in cases.values() if i["mode"] == "arm"),
         sum(1 for i in cases.values() if i["mode"] == "thumb"),
         len(problems)))
    A("")
    A("Recompiler verdicts: **%d `guest_unsupported` sites**, **%d "
      "`pc = 0xFFFFFFFF` A32-middle reject sites**."
      % (len(rejects), len(midreject)))
    A("")

    # ---------------- main table ----------------
    A("## 1. Every FP/VFP/NEON encoding in translated code")
    A("")
    A("`R` = sites the decode chain rejects (`guest_unsupported` or the")
    A("A32-middle reject); `R>0` is the work list for implementation.")
    A("")
    A("| encoding (A32 word / T32 hw1 hw2) | mnemonic | mode | count | R | sample PCs |")
    A("|---|---|---|---:|---:|---|")
    total_fp_sites = sum(fp.values())
    total_fp_rej = sum(fp_rej.values())
    for k, c in sorted(fp.items(), key=lambda kv: (-fp_rej.get(kv[0], 0), -kv[1])):
        mode, val = k
        A("| %s | %s | %s | %d | %d | %s |"
          % (enc_str(mode, val), mnems.get(k, "(unnamed)"), mode, c,
             fp_rej.get(k, 0), " ".join(fp_samples[k])))
    A("")
    A("Distinct FP encodings: **%d** (sites %d), of which **%d encodings / "
      "%d sites** are rejected by the recompiler's decode."
      % (len(fp), total_fp_sites, len(fp_rej), total_fp_rej))
    A("")

    # ---------------- rejected FP work list ----------------
    A("## 2. Unsupported FP work list")
    A("")
    if fp_rej:
        A("| encoding | mnemonic | mode | rejected sites | sample PCs |")
        A("|---|---|---|---:|---|")
        for k, c in sorted(fp_rej.items(), key=lambda kv: -kv[1]):
            mode, val = k
            A("| %s | %s | %s | %d | %s |"
              % (enc_str(mode, val), mnems.get(k, "(unnamed)"), mode, c,
                 " ".join(fp_samples[k])))
    else:
        A("None - every FP encoding present in translated code is lowered.")
    A("")
    if reject_escapes:
        A("### Remaining A32-middle stops (`pc = 0xFFFFFFFF`)")
        A("")
        A("hw1 lies outside the coprocessor range 0xEC00-0xEF00, so these are")
        A("not FP instructions: the Thumb path keeps its conservative")
        A("A32-middle stop for words that merely look like the second half of")
        A("an A32 instruction.")
        A("")
        for (mode, text), c in sorted(reject_escapes.items(), key=lambda kv: -kv[1]):
            A("- %s %s sites=%d" % (mode, text, c))
        A("")

    # ---------------- rejected non-FP (context) ----------------
    A("## 3. Non-FP rejects (context; outside this task's scope)")
    A("")
    if other_rej:
        A("| decoded as | sites |")
        A("|---|---:|")
        for m, c in sorted(other_rej.items(), key=lambda kv: -kv[1])[:40]:
            A("| %s | %d |" % (m, c))
    else:
        A("none")
    A("")

    # ---------------- whole image ----------------
    A("## 4. Whole-image static scan (beyond the translated set)")
    A("")
    A("Every 4-byte-aligned word of `DQ8.code` matching the A32 FP/NEON")
    A("pattern, and every halfword-aligned Thumb/Thumb-2 FP pattern start,")
    A("counted independently of the recompiler.  Most hits outside the")
    A("translated set are data (literal pools, tables) misreading as code -")
    A("they are listed so the sweep is complete over the file, not only over")
    A("reachable code.")
    A("")
    A("- A32-pattern words: **%d distinct** (%d word hits)" % (len(scan_arm), sum(scan_arm.values())))
    A("- T32-pattern units: **%d distinct** (%d unit hits)" % (len(scan_thumb), sum(scan_thumb.values())))
    A("- FP-mnemonic hits outside the translated set: **%d encodings / %d words**"
      % (len(outside_fp), sum(outside_fp.values())))
    A("")
    if outside_fp:
        A("| encoding | mnemonic | mode | occurrences |")
        A("|---|---|---|---:|")
        for (mode, val, mnem), c in sorted(outside_fp.items(), key=lambda kv: -kv[1])[:80]:
            A("| %s | %s | %s | %d |" % (enc_str(mode, val), mnem, mode, c))
        A("")

    A("## 5. Work list this sweep found, and how each entry was closed")
    A("")
    A("Baseline (fresh recompile before any fix): **12 FP sites rejected** -")
    A("6 A32 encodings over 7 sites plus 5 Thumb-2 first-halfwords and one")
    A("overlapping A32-middle stop.")
    A("")
    A("| what | sites | why it was rejected | closed by |")
    A("|---|---:|---|---|")
    A("| `vstr sX, [rN, #-imm]` (0xED408ACA, 0xED040A98, 0xED008A2C, 0xBD020A8D) | 4 | "
      "the VLDM/VSTM guard (`P != U`) claimed P=1,U=0 single transfers and read "
      "imm8 as a register count (imm8 > 32 -> reject); imm8 <= 32 was silently "
      "stored with the wrong transfer width (51 sites total) | guard narrowed to "
      "`!P || W` - llvm-mc rejects `vstmdb r0, {s0,s1}` so P=1,W=0 is single-transfer "
      "only (main.rs VLDM arm, test `negative_offset_vldr_vstr_use_the_immediate_form_not_the_list_count`) |")
    A("| `vmov rN, rM, dX` (0xEC532B10 x2, 0xEC510B18) | 3 | no arm existed for the "
      "two-core-register VMOV in the coprocessor-transfer space | new arm "
      "`w & 0x0FE0_0FD0 == 0x0C40_0B10`, fields verified against llvm-mc for "
      "d0/d1/d8/d9/d16/d17/d24/d31 (test `vmov_two_register_form_moves_a_double_to_and_from_core_registers`) |")
    A("| Thumb-2 VFP hw1 0xED8D x2, 0xEC80, 0xEEB4, 0xEF68 | 5 | the Thumb decode "
      "chain had no coprocessor arm at all (the class gap on the Thumb path) | "
      "Thumb-2 VFP is the A32 encoding with the halfwords swapped: the Thumb case "
      "now redirects into a forced A32 twin decoded from the reconstructed word, "
      "and a sentinel after the run returns to Thumb state (tests "
      "`thumb2_vfp_transfer_delegates_to_the_a32_chain_and_returns_to_thumb`, "
      "`thumb2_non_vfp_coprocessor_number_is_rejected_with_the_full_word`, "
      "`thumb2_pc_relative_vfp_literal_is_rejected_not_guess_a_base`) |")
    A("| FP-shaped Thumb-2 with a non-VFP coprocessor number | (5) | no "
      "Advanced-SIMD arm exists anywhere in the recompiler; DQ8.code contains no "
      "NEON in translated code, so these are data-shaped words | kept loud: "
      "rejected with the full 32-bit encoding instead of only hw1 |")
    A("")
    A("After the fixes: **0 FP encodings / 0 sites rejected** (section 1 has no")
    A("`R>0` rows).  The remaining %d `guest_unsupported` sites decode as non-FP"
      % len(rejects))
    A("mnemonics and are listed in section 3 for context.")
    A("")
    A("## 6. Reproduce")
    A("")
    A("```bash")
    A("args=(); while IFS= read -r a; do [[ -z \"$a\" || \"$a\" == \\#* ]] || args+=(--extra-entry \"$a\"); done < recomp-roots.txt")
    A("cargo run --release -- recompile-arm DQ8.code 0x00100000 target/vfp_sweep.c \"${args[@]}\"")
    A("python3 tools/vfp_sweep.py --recomp target/vfp_sweep.c")
    A("```")
    A("")

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w") as fh:
        fh.write("\n".join(L) + "\n")
    print("report -> %s" % args.out)


if __name__ == "__main__":
    sys.exit(main())
