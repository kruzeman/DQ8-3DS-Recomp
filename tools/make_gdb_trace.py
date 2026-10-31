#!/usr/bin/env python3
"""Generate a one-hit-per-site GDB script from dq8tool's indirect-site report."""

from __future__ import annotations

import re
import sys
from pathlib import Path


SITE = re.compile(r"^## Indirect (?:calls|tail calls|branches): 0x([0-9A-Fa-f]+)", re.M)


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} REPORT.md OUTPUT.gdb", file=sys.stderr)
        return 2

    report_path, output_path = map(Path, sys.argv[1:])
    report = report_path.read_text(encoding="utf-8")
    matches = list(SITE.finditer(report))
    sites: dict[int, str] = {}
    aliases = {"ip": "r12", "sp": "r13", "lr": "r14", "pc": "r15"}

    for index, match in enumerate(matches):
        address = int(match.group(1), 16)
        section_end = matches[index + 1].start() if index + 1 < len(matches) else len(report)
        section = report[match.start() : section_end]
        instruction = re.search(
            rf"^0x{address:08X}:.*?\b(?:call|branch|tailcall)_indirect\((r\d+|ip|sp|lr|pc)\)",
            section,
            re.M,
        )
        if instruction is None:
            raise ValueError(f"could not find indirect target register for 0x{address:08X}")
        register = aliases.get(instruction.group(1), instruction.group(1))
        previous = sites.setdefault(address, register)
        if previous != register:
            raise ValueError(f"conflicting target registers for 0x{address:08X}")

    lines = [
        "set pagination off",
        "set confirm off",
        "set remotetimeout 10",
        "set architecture arm",
        # Azahar reports guest CPU exceptions through the remote GDB stub as
        # signals. Stopping on them would freeze gameplay in this trace session.
        "handle SIGILL nostop noprint pass",
        "handle SIGSEGV nostop noprint pass",
        "set logging file DQ8.runtime-gdb.log",
        # Keep earlier samples if the user reruns the generated trace script.
        "set logging overwrite off",
        "set logging enabled on",
    ]

    for breakpoint_number, (address, register) in enumerate(sorted(sites.items()), 1):
        lines.extend(
            [
                f"break *0x{address:08X}",
                "commands",
                "silent",
                f'printf "DQ8_SITE 0x{address:08X} target=0x%08x\\n", ${register}',
                "info registers r0 r1 r2 r3 r12 sp lr pc cpsr",
                # Capture the first hit at each site only to limit slowdown and log size.
                f"disable {breakpoint_number}",
                "continue",
                "end",
            ]
        )

    lines.extend(["target remote localhost:24689", "continue", ""])
    output_path.write_text("\n".join(lines), encoding="utf-8")
    print(f"Wrote {len(sites)} one-hit breakpoints to {output_path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
