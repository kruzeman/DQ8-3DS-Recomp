#!/usr/bin/env python3
"""Static coverage scan: how much of DQ8.code does the recompiler translate?

Compares the game's code image against the build's actual translated set — the
`case 0xXXXXXXXX:` guest-PC labels emitted into the generated split C by
`tools/build_split.sh` (recompile-arm + split_dispatch) — plus the translation
registry (recomp-roots.txt) and the image's own call/vtable entry structure.

Units (both, honestly):
  * BYTES   - translated guest instructions (4 B each) over (a) the whole
              DQ8.code image and (b) the translated address span (which still
              contains literal pools / data, so it is the tighter denominator,
              not a true code-bytes denominator; a true code/data split would
              need a recursive disassembly the repo does not keep).
  * FUNCTIONS - candidate function entries enumerated from the image itself:
              direct BL call targets + pointer-table (vtable) slots + the
              manual roots registry.  How many of those entries have translated
              code.  This is an estimate: call targets miss indirect-only
              functions (which is what the roots registry exists for), and
              pointer tables can over-count; both directions are reported.

Also reports latent escape sites: direct BL targets with NO translated code
(the guest would abort with "translated PC escaped" if it ever reached one)
and `guest_unsupported` addresses (decoded but compiled into an abort).

usage:
  python3 tools/coverage_scan.py [--code DQ8.code] [--split target/dq8-split]
                                 [--roots recomp-roots.txt] [--json out.json]
                                 [--bands]
"""
import argparse
import json
import re
import sys
from pathlib import Path

BASE = 0x00100000
CASE_RE = re.compile(r"^\s*case 0x([0-9A-F]{8}):", re.M)
UNSUPPORTED_RE = re.compile(r"guest_unsupported\(0x([0-9A-F]{8})u")


def load_code(path: Path) -> bytes:
    return path.read_bytes()


def translated_set(split_dir: Path):
    """Guest PCs with translated code (instruction case labels) + unsupported."""
    translated = set()
    unsupported = set()
    parts = sorted(split_dir.glob("dq8_part_*.c"))
    if not parts:
        sys.exit(f"no dq8_part_*.c in {split_dir} (run tools/build_split.sh first)")
    for part in parts:
        text = part.read_text(errors="replace")
        translated.update(int(m, 16) for m in CASE_RE.findall(text))
        unsupported.update(int(m, 16) for m in UNSUPPORTED_RE.findall(text))
    return translated, unsupported, len(parts)


def branch_targets(data: bytes):
    """Direct branch/call targets over a linear sweep of the image.

    ARM B/BL/BLX-immediate all carry bits[27:25]=101; the link bit separates
    calls (BL/BLX) from tail branches (B).  A linear sweep also walks data
    (literal pools, vtables), so the results are *candidates*, cross-checked
    against the translated set below.
    """
    bl, bx, b = set(), set(), set()
    n_words = len(data) // 4
    for i in range(n_words):
        w = int.from_bytes(data[i * 4:i * 4 + 4], "little")
        if (w & 0x0E000000) != 0x0A000000:
            continue
        imm24 = w & 0x00FFFFFF
        if imm24 & 0x00800000:
            imm24 -= 0x01000000
        src = BASE + i * 4
        tgt = src + 8 + (imm24 << 2)
        cond = w >> 28
        link = (w >> 24) & 1
        if cond == 0xF:            # BLX (immediate) -> Thumb target
            if link and BASE <= tgt < BASE + len(data):
                bx.add(tgt & ~1)
        elif link:
            if BASE <= tgt < BASE + len(data):
                bl.add(tgt)
        else:
            if BASE <= tgt < BASE + len(data):
                b.add(tgt)
    return bl, b, bx


def pointer_tables(data: bytes, plausible_end: int):
    """Pointer windows (vtables): runs of consecutive in-text code pointers.

    A word qualifies when it points into the image's code span and is word
    aligned; a window needs >= 8 consecutive qualifying words (matches the
    small vtable families the roots registry documents, e.g. the 244-entry
    layout windows).  Every slot of a qualifying window is a candidate
    function entry - deliberately NOT cross-checked against the translated
    set, so vtables whose targets the recompiler has not reached still show
    up in the population (that is exactly the "what remains" the report
    needs).
    """
    ok = []
    n_words = len(data) // 4
    for i in range(n_words):
        v = int.from_bytes(data[i * 4:i * 4 + 4], "little")
        ok.append(BASE <= v < plausible_end and v % 4 == 0)
    slots = set()
    runs = 0
    i = 0
    while i < n_words:
        if not ok[i]:
            i += 1
            continue
        j = i
        while j < n_words and ok[j]:
            j += 1
        if j - i >= 8:
            runs += 1
            for k in range(i, j):
                slots.add(int.from_bytes(data[k * 4:k * 4 + 4], "little"))
        i = j
    return slots, runs


def load_roots(path: Path):
    roots = set()
    for line in path.read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            roots.add(int(line, 16))
    return roots


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--code", type=Path, default=Path("DQ8.code"))
    ap.add_argument("--split", type=Path, default=Path("target/dq8-split"))
    ap.add_argument("--roots", type=Path, default=Path("recomp-roots.txt"))
    ap.add_argument("--json", type=Path)
    ap.add_argument("--bands", action="store_true",
                    help="print the per-128KiB-band table")
    args = ap.parse_args()

    data = load_code(args.code)
    image_end = BASE + len(data)
    translated, unsupported, n_parts = translated_set(args.split)
    roots = load_roots(args.roots)
    bl, b, bx = branch_targets(data)

    code_extent = max(translated) + 4 if translated else BASE
    tables_slots, n_tables = pointer_tables(data, code_extent)

    entries = bl | tables_slots | roots
    entries_in_span = {e for e in entries if e < code_extent}
    translated_entries = entries_in_span & translated
    untranslated_entries = entries_in_span - translated
    latent_escapes = {t for t in bl if t < code_extent and t not in translated}
    bl_in_span = {t for t in bl if t < code_extent}
    bl_cov = bl_in_span & translated

    t_min = min(translated)
    span_bytes = code_extent - t_min
    # ARM (A32) instructions are 4 B; the runtime keeps the Thumb bit in the
    # PC, so odd case labels are 2 B Thumb instructions.
    arm_pcs = {pc for pc in translated if (pc & 1) == 0}
    thumb_pcs = translated - arm_pcs
    covered_bytes = 4 * len(arm_pcs) + 2 * len(thumb_pcs)

    out = {
        "image_bytes": len(data),
        "image_range": f"0x{BASE:08X}-0x{image_end - 1:08X}",
        "translated_insn": len(translated),
        "translated_arm_insn": len(arm_pcs),
        "translated_thumb_insn": len(thumb_pcs),
        "translated_bytes": covered_bytes,
        "translated_span": f"0x{t_min:08X}-0x{code_extent - 1:08X}",
        "span_bytes": span_bytes,
        "pct_of_image_bytes": covered_bytes * 100.0 / len(data),
        "pct_of_span_bytes": covered_bytes * 100.0 / span_bytes,
        "unsupported_insns": len(unsupported),
        "bl_targets": len(bl),
        "bl_targets_translated": len(bl_in_span & translated),
        "b_targets": len(b),
        "blx_targets": len(bx),
        "vtable_windows": n_tables,
        "vtable_slots": len(tables_slots),
        "roots": len(roots),
        "entries_total": len(entries_in_span),
        "entries_translated": len(translated_entries),
        "pct_entries_translated": (len(translated_entries) * 100.0
                                   / len(entries_in_span) if entries_in_span else 0),
        "roots_covered": len(roots & translated),
        "latent_escape_sites": len(latent_escapes),
        "split_parts": n_parts,
    }

    print(f"image                {out['image_range']}  {len(data):,} B")
    print(f"translated span      {out['translated_span']}  {span_bytes:,} B")
    print(f"split parts          {n_parts}")
    print()
    print(f"translated guest instructions : {len(translated):,}"
          f"  (ARM {len(arm_pcs):,} x4B + Thumb {len(thumb_pcs):,} x2B"
          f" = {covered_bytes:,} B)")
    print(f"  of whole image bytes        : {out['pct_of_image_bytes']:.2f}%"
          f"  (lower bound: image includes vtables/literal pools/data)")
    print(f"  of translated-span bytes    : {out['pct_of_span_bytes']:.2f}%"
          f"  (span still contains pools + untranslated code)")
    print(f"decoded-but-unsupported (abort) : {len(unsupported)}")
    print()
    print(f"BL call targets (direct calls)  : {len(bl):,}")
    print(f"  with translated code          : {len(bl_cov):,}"
          f"  ({len(bl_cov) * 100.0 / len(bl_in_span):.2f}%)"
          if bl_in_span else "  (none in span)")
    print(f"B  branch targets                : {len(b):,}")
    print(f"BLX (Thumb) call targets         : {len(bx):,}")
    print(f"vtable windows (>=8 slots)       : {n_tables}"
          f"   slots: {len(tables_slots):,}")
    print(f"roots registry                   : {len(roots):,}"
          f"   already translated: {out['roots_covered']:,}")
    print()
    print(f"function entries (BL targets + vtable slots + roots, "
          f"in span) : {len(entries_in_span):,}")
    print(f"  with translated code          : {len(translated_entries):,}"
          f"  ({out['pct_entries_translated']:.2f}%)")
    print(f"  without (latent escape sites) : {len(untranslated_entries):,}")
    print(f"direct-call latent escape sites : {len(latent_escapes):,}"
          f"  (BL target with no translated code)")
    if latent_escapes:
        sample = sorted(latent_escapes)[:12]
        print("  sample: " + " ".join(f"0x{a:08X}" for a in sample))

    if args.bands:
        print()
        print("per-128KiB band (band = addr >> 17):")
        print(f"{'band':>5} {'range':>26} {'img B':>10} {'insn':>9} "
              f"{'cov B':>10} {'%band':>7}")
        bands = {}
        for pc in translated:
            rec = bands.setdefault(pc >> 17, [0, 0, 0])
            rec[0] += 1
            rec[1 if (pc & 1) else 2] += 1
        first_band = BASE >> 17
        last_band = (image_end - 1) >> 17
        band_rows = []
        for band in range(first_band, last_band + 1):
            lo = max(BASE, band << 17)
            hi = min((band + 1) << 17, image_end)
            img_b = hi - lo
            insn, n_thumb, n_arm = bands.get(band, [0, 0, 0])
            if img_b <= 0:
                continue
            cov_b = 4 * n_arm + 2 * n_thumb
            band_rows.append((band, lo, hi, img_b, insn, cov_b))
            print(f"0x{band:03X} 0x{lo:08X}-0x{hi - 1:08X} {img_b:>10,} "
                  f"{insn:>9,} {cov_b:>10,} {cov_b * 100.0 / img_b:>6.1f}%")
        dense = [r for r in band_rows if r[5] * 100.0 / r[3] >= 10.0]
        if dense:
            d0, d1 = dense[0][0], dense[-1][0]
            drows = [r for r in band_rows if d0 <= r[0] <= d1]
            dimg = sum(r[3] for r in drows)
            dinsn = sum(r[4] for r in drows)
            dcov = sum(r[5] for r in drows)
            print(f"\ndense code region 0x{d0 << 17:08X}-0x{((d1 + 1) << 17) - 1:08X}"
                  f" (first..last band >=10% covered):")
            print(f"  image bytes {dimg:,}  translated insn {dinsn:,}"
                  f"  covered bytes {dcov:,}  = {dcov * 100.0 / dimg:.2f}%")
            thin = [r for r in drows if r[5] * 100.0 / r[3] < 10.0]
            if thin:
                print("  bands below 10% inside it: " + ", ".join(
                    f"0x{r[0]:03X}({r[5] * 100.0 / r[3]:.1f}%)" for r in thin))
        out["dense_region"] = (f"0x{dense[0][0] << 17:08X}"
                               if dense else None)

    if args.json:
        out["latent_escape_sample"] = [f"0x{a:08X}" for a in
                                       sorted(latent_escapes)[:64]]
        out["unsupported_sample"] = [f"0x{a:08X}" for a in
                                     sorted(unsupported)[:64]]
        args.json.write_text(json.dumps(out, indent=2))
        print(f"\nwrote {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
