#!/usr/bin/env python3
"""Summarise a callgrind profile of the DQ8 runner into perf-audit zones.

usage: tools/bench_zones.py <callgrind.out> [--top N]

The runner is built with -O0, so every runtime helper (mem32, put32, svc_*,
pica_*, dsp_*) is a real function with its own symbol: the callgrind self cost
of a symbol can therefore be attributed to exactly one audit zone without any
in-process instrumentation.

Zones (audit 5.1 wording in parentheses):
  dispatch          generated interpreter: dq8_part_* / translated_from / scheduler
  mem32/put32       guest memory access helpers + address translation
  flags/helpers     ARM flag/shift/saturation, FP bit helpers, branch helpers
  host-calls        svc_* service shims and their kernel/arbiter machinery
  DSP ticks         dsp_* audio/HLE engine and the host audio sink
  present           GPU emulation (pica_*), GSP display transfer, scan-out,
                    Y2R video path, framebuffer capture/present
  libc I/O          fopen/fwrite/_IO_*/mem* inside libc (mostly the
                    diagnostic /tmp PPM screenshots)
  other             everything else

Inclusive costs of a few anchors are printed after the tables; they show which
caller is responsible for the libc I/O and present costs.
"""
import re
import subprocess
import sys

ZONES = [
    ("dispatch", re.compile(r"^(dq8_part_\d+|translated_from|schedule_guest_thread|guest_fast_page_list|run_guest|thread_execution)")),
    ("mem32/put32", re.compile(r"^(mem8|mem16|mem32|put8|put16|put32|guest_mem_check|guest_memory_address|guest_memory_range|guest_mem_)")),
    ("flags/helpers", re.compile(r"^(arm_|f32_|f64_|par_op|par_sel|uqsub8|qsub8|qsub16|branch_indirect|dq8_pc_needs_boundary|guest_udf|guest_unsupported|VFP|vfp_)")),
    ("host-calls", re.compile(r"^(svc_|guest_notify_handle|event_|arbiter_|exclusive_|wait_|signal_|kernel_)")),
    ("DSP ticks", re.compile(r"^(dsp_|host_audio|audio_)")),
    ("present", re.compile(r"^(pica_|gsp_|host_display|y2r_|bflim_|gpu_|tev_|host_shadow)")),
    ("libc I/O", re.compile(r"^(fwrite|fread|fopen|fclose|_IO_|__mempcpy|__memmove|memset|memcpy|memmove|printf|fprintf|snprintf|vfprintf|malloc|free|calloc|realloc|strlen|strtoull|getenv|clock_gettime|nanosleep|poll|write|read|ppoll)")),
]
ANCHORS = [
    "gsp_write_screen_ppms",
    "host_display_capture_transfer",
    "gsp_scanout_capture",
    "svc_arbitrate",
    "svc_send_sync",
    "pica_raster_triangle",
    "translated_from",
    "schedule_guest_thread",
]

LINE = re.compile(r"^\s*([\d,]+)\s+\(\s*([\d.]+)%\)\s+(\S.*)$")
FN = re.compile(r"^(.*?)\s*\[([^\]]*)\]$")


def parse(output):
    rows = []
    for line in output.splitlines():
        m = LINE.match(line)
        if not m:
            continue
        ir = int(m.group(1).replace(",", ""))
        pct = float(m.group(2))
        rest = m.group(3).strip()
        m2 = FN.match(rest)
        if m2:
            rest = m2.group(1).strip()
        name = rest.split(":")[-1].strip()
        if name.startswith("'"):
            name = name.lstrip("'")
        rows.append((ir, pct, name, rest))
    return rows


def zone_of(name):
    for zone, rx in ZONES:
        if rx.match(name):
            return zone
    return "other"


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    top = 10
    if "--top" in sys.argv:
        top = int(sys.argv[sys.argv.index("--top") + 1])

    self_out = subprocess.run(["callgrind_annotate", "--threshold=100", path],
                              capture_output=True, text=True).stdout
    rows = [r for r in parse(self_out) if r[2] != "PROGRAM TOTALS"]
    total = None
    for line in self_out.splitlines():
        if "PROGRAM TOTALS" in line:
            total = int(line.split()[0].replace(",", ""))
            break
    if not rows or not total:
        print("could not parse callgrind self costs", file=sys.stderr)
        return 1

    print("profile: %s" % path)
    print("total collected: %s Ir" % format(total, ","))
    print()
    print("top-%d hot functions (self cost, %% of collected instructions)" % top)
    for ir, pct, name, _ in rows[:top]:
        print("  %8.2f%%  %14s Ir  %-44s %s" % (pct, format(ir, ","), name, zone_of(name)))
    print()
    print("zone breakdown (self cost)")
    zones = {}
    for ir, pct, name, _ in rows:
        z = zone_of(name)
        a = zones.setdefault(z, [0, 0.0, []])
        a[0] += ir
        a[1] += pct
        a[2].append((ir, name))
    for z, (ir, pct, fns) in sorted(zones.items(), key=lambda kv: -kv[1][1]):
        top3 = ", ".join("%s %.1f%%" % (n, 100.0 * i / total)
                         for i, n in sorted(fns, key=lambda t: -t[0])[:3])
        print("  %-14s %7.2f%%  %14s Ir   [%s]" % (z, pct, format(ir, ","), top3))
    print()
    print("inclusive anchors (% of collected instructions, callgrind --inclusive)")
    inc_out = subprocess.run(["callgrind_annotate", "--inclusive=yes", "--threshold=100", path],
                             capture_output=True, text=True).stdout
    inc = {name: (ir, pct) for ir, pct, name, _ in parse(inc_out)}
    for a in ANCHORS:
        if a in inc:
            print("  %-32s %7.2f%%  %14s Ir" % (a, inc[a][1], format(inc[a][0], ",")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
