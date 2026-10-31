#!/usr/bin/env python3
"""Summarize DQ8_GUEST_PC_PROFILE_PAGE records from a profile-run stderr log.

The sampler records 4 KiB guest-PC pages after the movie-entry marker.
This tool aggregates those pages into the split-dispatcher's 128 KiB bands.
"""
import argparse
from collections import defaultdict
from pathlib import Path
import re
import sys

PAGE_RE = re.compile(r"^GUEST_PC_PROFILE_PAGE page=([0-9A-F]{3}) steps=(\d+)$", re.M)
PC_RE = re.compile(r"^GUEST_PC_PROFILE_PC pc=([0-9A-F]{8}) steps=(\d+)$", re.M)
BEGIN_RE = re.compile(r"^GUEST_PC_PROFILE_BEGIN after_movie_entry=(\d+)$", re.M)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stderr_log", type=Path)
    parser.add_argument("--bands", type=int, default=20,
                        help="number of highest-count 128 KiB bands to print")
    args = parser.parse_args()
    text = args.stderr_log.read_text(errors="replace")
    bench = re.findall(r"^BENCH steps=.*$", text, re.M)
    starts = BEGIN_RE.findall(text)
    pages = {int(page, 16): int(count) for page, count in PAGE_RE.findall(text)}
    pcs = [(int(pc, 16), int(count)) for pc, count in PC_RE.findall(text)]
    if len(bench) != 1 or len(starts) != 1 or not pages or not pcs:
        print("expected one BENCH line, one movie profile marker, and nonzero page/PC counts",
              file=sys.stderr)
        return 2
    if "translated PC escaped" in text:
        print("profile run contains an escaped-PC diagnostic", file=sys.stderr)
        return 3

    total = sum(pages.values())
    exact_total = sum(count for _pc, count in pcs)
    if exact_total != total:
        print(f"PC/page histogram mismatch: exact={exact_total} page={total}", file=sys.stderr)
        return 4
    bands = defaultdict(int)
    for page, count in pages.items():
        bands[page >> 5] += count
    ordered = sorted(bands.items(), key=lambda item: (-item[1], item[0]))
    print(bench[0])
    print("GUEST_PC_PROFILE_BEGIN after_movie_entry=" + starts[0])
    print(f"profiled_guest_steps={total} nonzero_pages={len(pages)} nonzero_128KiB_bands={len(bands)}")
    print("band  guest_range                 steps       share    cumulative")
    cumulative = 0
    for band, count in ordered[:args.bands]:
        cumulative += count
        lo = band * 0x20000
        hi = lo + 0x1FFFF
        print(f"{band:03X}   0x{lo:08X}-0x{hi:08X}  {count:11d}  "
              f"{count * 100 / total:7.3f}%  {cumulative * 100 / total:7.3f}%")

    print("top pages in leading bands:")
    for band, _count in ordered[:min(5, args.bands)]:
        top = sorted(((page, count) for page, count in pages.items() if page >> 5 == band),
                     key=lambda item: (-item[1], item[0]))[:8]
        for page, count in top:
            print(f"  page=0x{page:03X} range=0x{page << 12:08X}-0x{(page + 1) * 0x1000 - 1:08X} "
                  f"steps={count} share={count * 100 / total:.3f}%")
    print("top guest PCs:")
    cumulative_pc = 0
    for pc, count in sorted(pcs, key=lambda item: (-item[1], item[0]))[:40]:
        cumulative_pc += count
        print(f"  pc=0x{pc:08X} steps={count:11d} share={count * 100 / total:7.3f}% "
              f"cumulative_top40={cumulative_pc * 100 / total:7.3f}%")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
