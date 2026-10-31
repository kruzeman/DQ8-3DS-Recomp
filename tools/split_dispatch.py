#!/usr/bin/env python3
"""Split the recompiler's single-translation-unit output into many small TUs.

The ARM recompiler (cargo run -- recompile-arm ...) emits one C file whose
``translated_from()`` interpreter contains one giant ``switch (pc >> 12)``
with one inner ``switch (pc)`` per translated 4 KiB page.  With the current
root set that file is ~168 MB / 943k instructions, and clang needs >4 GiB of
anonymous memory to build it as a single TU, which exceeds the 4.15 GiB cgroup
memory cap of a Hermes kanban worker (the OOM killer then SIGTERMs clang).

This script rewrites that file into:

  dq8_rt.h          extern declarations for the runtime symbols the translated
                    cases reference, plus the per-page part function type/table
  dq8_main.c        the original runtime (prelude) with the giant switch
                    replaced by a 4096-entry table call, plus main()
  dq8_part_NNN.c    one TU per ADDRESS BAND of translated pages (band = page
                    >> 5, i.e. 32 guest pages per band); each part function
                    carries its own ``switch (pc >> 12)`` and reports whether
                    it handled the PC (return 0 => "translated PC escaped")

Part identity is keyed by page ADDRESS, not by position in the file: adding a
recomp root that creates a new translated page only changes the one band part
that page falls into (plus dq8_main.c's dispatch table), instead of shifting
every subsequent sequential group and forcing a full recompile.  To keep the
cache key stable, dq8_rt.h always declares all 128 possible band functions
even when only a few bands exist.  Stale dq8_part_*.c files from a previous,
larger manifest are deleted after a successful split.

Semantics are preserved: every translated instruction, every SVC shim, the
per-page ``default: ... exit(3)`` escape and the outer escape message behave
exactly like the single-TU build, and so does everything the main loop does
BEFORE dispatching an instruction (the snprintf shim, the fast page-list
shortcut, PC-gated trace hooks, DQ8_TRACE_PC tracing, previous/current-PC
bookkeeping and the `steps` cadence): a part returns to dq8_main.c whenever
``dq8_pc_needs_boundary(pc)`` says the boundary must run, keeps `steps`
counting actual instructions (not part calls), and performs the same
per-instruction PC bookkeeping inline.  Structure changes only; finding A05
covered the earlier version of this paragraph, whose "semantics are
preserved" claim was not true for mid-batch hook entries.

Usage:
    python3 tools/split_dispatch.py <generated.c> <outdir> [--overrides registry.json]

Then:
    clang -std=c11 -O0 -g0 -w -I<outdir> -c <outdir>/dq8_main.c -o <outdir>/dq8_main.o
    (parallel) clang -std=c11 -O0 -g0 -w -I<outdir> -c <outdir>/dq8_part_*.c
    clang <all .o> -o <binary> -lm
"""
import os
import json
import re
import sys
from typing import NoReturn

# Runtime symbols (prelude definitions) referenced by the translated cases.
# Derived from the identifier set of the case region; any symbol missing here
# makes the part compile fail with "use of undeclared identifier", which is the
# safety net for future recompiler output.
SHARED_SYMBOLS = [
    "active_tls_base", "current_guest_pc", "exclusive_addr", "exclusive_size",
    "ram", "trace_guest_write_addr", "trace_guest_write_end", "trace_guest_write_hits",
    "trace_write_n",
    "trace_hid_reads_enabled",
    "fpr", "fpscr", "main_guest_pc", "main_guest_regs",
    "next_memory_block_handle", "previous_guest_pc", "thread_stopped_by_yield",
    "thread_yield_requested", "trace_guest_pc_count", "trace_guest_pcs",
    "arm_shift", "arm_shift_carry", "arm_shift_imm", "arm_shift_imm_carry", "arm_ssat", "arm_ssat16", "arm_usat",
    "branch_indirect", "dq8_pc_needs_boundary",
    "f32_from_bits", "f32_is_nan_bits", "f32_to_bits",
    "f64_from_bits", "f64_is_nan_bits", "f64_to_bits",
    "guest_udf", "guest_unsupported", "mem16", "mem32", "mem8", "put16",
    "put32", "put8", "qsub8", "uqsub8", "par_op", "par_sel",
    "svc_arbitrate", "svc_break", "svc_clear_event", "svc_close_handle",
    "svc_connect_to_port", "svc_control_memory", "svc_create_address_arbiter",
    "svc_create_event", "svc_create_mutex", "svc_create_thread",
    "svc_duplicate_handle", "svc_exit_process", "svc_exit_thread",
    "svc_get_process_id", "svc_get_process_info", "svc_get_resource_limit",
    "svc_get_resource_values", "svc_get_system_tick", "svc_get_thread_id",
    "svc_map_memory_block", "svc_release_mutex", "svc_send_sync",
    "svc_signal_event", "svc_sleep", "svc_unmap_memory_block",
    "svc_wait_synchronization", "svc_wait_synchronization_n",
]

# Block-chaining batch size: how many consecutive in-band guest instructions a
# part function dispatches before returning to the main dispatcher.  The
# single-TU interpreter re-ran one giant switch per instruction (no call), but
# the split makes every instruction a cross-function call/return + 7 pointer
# write-backs, which dominated guest time (~350 ns/insn, -O2-insensitive).  By
# looping inside the part and only returning when the pc leaves the band, a
# blocking SVC clears `running`, dq8_pc_needs_boundary(pc) says the main-loop
# boundary must run, or this budget expires, consecutive guest instructions
# dispatch through the hot in-band switch instead.  The budget bounds how long
# the part can run without returning, so the main dispatcher still gets control
# regularly for round-robin scheduling / host housekeeping (which keys off
# `steps`; parts advance it once per actual instruction they execute and write
# it back on return, so cadence marks in dq8_main.c keep counting guest
# instructions, not part calls).  256 keeps per-slice instruction counts (5000
# step budget * 256) well under the preemption and vblank cadences.
CHAIN_BATCH = 256

PART_FN_TYPE = ("typedef int (*Dq8PartFn)(uint32_t *r, uint32_t *pc_io, int *n_io, "
                "int *z_io, int *c_io, int *v_io, int *ge_io, int *running_io, "
                "uint64_t *steps_io, int is_main);")
PART_PROTO = ("int dq8_part_%03d(uint32_t *r, uint32_t *pc_io, int *n_io, int *z_io, "
              "int *c_io, int *v_io, int *ge_io, int *running_io, "
              "uint64_t *steps_io, int is_main);")

INLINE_MEMORY_HELPERS = r'''/* Selective checked fast path: identity-mapped, single-span guest RAM only.
 * Aliased VRAM/FCRAM, faults, HID reads and traced stores stay on the exact
 * out-of-line helpers.  memcpy preserves the guest's legal unaligned accesses. */
static inline int dq8_mem_identity_span(uint32_t a, unsigned width) {
    if (a > DQ8_RT_RAM_SIZE || width > DQ8_RT_RAM_SIZE - a) return 0;
    if ((a >= 0x18000000u && a < 0x18600000u) ||
        (a >= 0x20000000u && a < 0x30000000u)) return 0;
    return 1;
}
static inline uint32_t dq8_mem8_fast(uint32_t a) {
    if (trace_hid_reads_enabled || !dq8_mem_identity_span(a, 1u)) return mem8(a);
    return ram[a];
}
static inline uint32_t dq8_mem16_fast(uint32_t a) {
    if (trace_hid_reads_enabled || !dq8_mem_identity_span(a, 2u)) return mem16(a);
    uint16_t x; memcpy(&x, ram + a, sizeof(x)); return x;
}
static inline uint32_t dq8_mem32_fast(uint32_t a) {
    if (trace_hid_reads_enabled || !dq8_mem_identity_span(a, 4u)) return mem32(a);
    uint32_t x; memcpy(&x, ram + a, sizeof(x)); return x;
}
static inline void dq8_fast_exclusive_store(uint32_t a, unsigned width) {
    if (exclusive_addr == 0xFFFFFFFFu) return;
    uint64_t end = (uint64_t)a + width;
    uint64_t xend = (uint64_t)exclusive_addr + exclusive_size;
    if ((uint64_t)a < xend && end > (uint64_t)exclusive_addr)
        exclusive_addr = 0xFFFFFFFFu;
}
static inline int dq8_mem_store_traced(uint32_t a) {
    /* up to 4 va:len ranges (t_41f38df0): the scalar form survived the array
       conversion as `a >= trace_guest_write_addr`, which compares against the
       array ADDRESS and silently disabled guest-store tracing for every part. */
    for (unsigned i = 0; i < trace_write_n; ++i)
        if (a >= trace_guest_write_addr[i] && a < trace_guest_write_end[i]) return 1;
    return 0;
}
static inline void dq8_put8_fast(uint32_t a, uint32_t v) {
    if (!dq8_mem_identity_span(a, 1u) || dq8_mem_store_traced(a)) { put8(a, v); return; }
    dq8_fast_exclusive_store(a, 1u); ram[a] = (uint8_t)v;
}
static inline void dq8_put16_fast(uint32_t a, uint32_t v) {
    if (!dq8_mem_identity_span(a, 2u) || dq8_mem_store_traced(a)) { put16(a, v); return; }
    dq8_fast_exclusive_store(a, 2u); uint16_t x = (uint16_t)v;
    memcpy(ram + a, &x, sizeof(x));
}
static inline void dq8_put32_fast(uint32_t a, uint32_t v) {
    if (!dq8_mem_identity_span(a, 4u) || dq8_mem_store_traced(a)) { put32(a, v); return; }
    dq8_fast_exclusive_store(a, 4u); memcpy(ram + a, &v, sizeof(v));
}
'''

ESCAPE_FMT = ('fprintf(stderr, "translated PC escaped to %08X after %08X '
              'r0-r7=%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X lr=%08X sp=%08X\\n", '
              'pc, previous_guest_pc, r[0], r[1], r[2], r[3], r[4], r[5], r[6], '
              'r[7], r[14], r[13]); exit(3);')


def die(msg) -> NoReturn:
    sys.stderr.write("split_dispatch: %s\n" % msg)
    sys.exit(1)


def load_overrides(path):
    """Load the optional named guest-PC region override registry."""
    try:
        with open(path, "r", encoding="utf-8") as fh:
            document = json.load(fh)
    except (OSError, ValueError) as exc:
        die("cannot read override registry %s: %s" % (path, exc))
    if not isinstance(document, dict) or document.get("version") != 1:
        die("override registry must be an object with version=1")
    entries = document.get("overrides")
    if not isinstance(entries, list):
        die("override registry field 'overrides' must be a list")
    parsed = []
    names = set()
    for item in entries:
        if not isinstance(item, dict):
            die("each override entry must be an object")
        name = item.get("name")
        if not isinstance(name, str) or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
            die("override name must be a C identifier")
        if name in names:
            die("duplicate override name %r" % name)
        names.add(name)
        try:
            start = int(item["start"], 0) if isinstance(item["start"], str) else item["start"]
            end = int(item["end"], 0) if isinstance(item["end"], str) else item["end"]
        except (KeyError, TypeError, ValueError):
            die("override %s requires integer start/end addresses" % name)
        body = item.get("body")
        body_file = item.get("body_file")
        if body_file is not None:
            if body is not None or not isinstance(body_file, str) or not body_file:
                die("override %s requires exactly one of body or body_file" % name)
            body_path = os.path.join(os.path.dirname(os.path.abspath(path)), body_file)
            try:
                with open(body_path, "r", encoding="utf-8") as fh:
                    body = fh.read()
            except OSError as exc:
                die("cannot read override body file %s: %s" % (body_path, exc))
        if (isinstance(start, bool) or isinstance(end, bool) or
                not isinstance(start, int) or not isinstance(end, int) or
                start < 0 or end > 0x100000000 or start >= end):
            die("override %s has invalid half-open guest range" % name)
        if not isinstance(body, str) or not body.strip():
            die("override %s requires a nonempty C handler body" % name)
        parsed.append({"name": name, "start": start, "end": end, "body": body})
    parsed.sort(key=lambda x: (x["start"], x["end"]))
    for left, right in zip(parsed, parsed[1:]):
        if left["end"] > right["start"]:
            die("override ranges overlap: %s and %s" % (left["name"], right["name"]))
    return parsed


def override_header(overrides):
    if not overrides:
        return ""
    args = ("uint32_t guest_pc, uint32_t *r, uint32_t *pc_io, int *n_io, "
            "int *z_io, int *c_io, int *v_io, int *ge_io, int *running_io, "
            "uint64_t *steps_io, unsigned *budget_io")
    out = ["#define DQ8_OVERRIDE_CALL_ORIGINAL 0",
           "#define DQ8_OVERRIDE_HANDLED 1",
           "extern int dq8_overrides_enabled;", ""]
    out.extend(["extern uint64_t dq8_override_hits;",
                "extern uint64_t dq8_override_original_fallbacks;", ""])
    for item in overrides:
        out.append("static inline int dq8_override_%s(%s) {" % (item["name"], args))
        out.append(item["body"])
        out.append("}")
    out.append("static inline int dq8_override_dispatch(%s) {" % args)
    out.append("    if (!dq8_overrides_enabled) return DQ8_OVERRIDE_CALL_ORIGINAL;")
    for item in overrides:
        out.append("    if (guest_pc >= 0x%08Xu && guest_pc < 0x%08Xu) {" %
                   (item["start"], item["end"]))
        out.append("        int result = dq8_override_%s(guest_pc, r, pc_io, n_io, z_io, "
                   "c_io, v_io, ge_io, running_io, steps_io, budget_io);" % item["name"])
        out.append("        if (result == DQ8_OVERRIDE_HANDLED) ++dq8_override_hits;")
        out.append("        else if (result == DQ8_OVERRIDE_CALL_ORIGINAL) "
                   "++dq8_override_original_fallbacks;")
        out.append("        return result;")
        out.append("    }")
    out.append("    return DQ8_OVERRIDE_CALL_ORIGINAL;")
    out.append("}")
    return "\n".join(out) + "\n"


def declaration_for(name, lines):
    """Extern declaration for a prelude symbol, derived from its first
    column-0 ``static`` declaration/definition.  Functions become prototypes
    (parameter list only, even for one-line definitions); variables lose their
    initialiser."""
    pat = re.compile(r"^static\s+.*\b%s\b" % re.escape(name))
    for i, line in enumerate(lines):
        if not pat.match(line):
            continue
        text = line[len("static "):]
        if "(" in text:
            start = text.index("(")
            depth = 0
            end = -1
            j = i
            acc = text
            while True:
                depth = 0
                for k in range(start if j == i else 0, len(acc)):
                    if acc[k] == "(":
                        depth += 1
                    elif acc[k] == ")":
                        depth -= 1
                        if depth == 0:
                            end = k
                            break
                if end >= 0:
                    break
                j += 1
                if j >= len(lines):
                    die("unterminated signature for %r at line %d" % (name, i + 1))
                acc += lines[j]
            return acc[:end + 1].strip() + ";"
        decl = text.split(";")[0]
        if "=" in decl:
            decl = decl.split("=")[0]
        return decl.strip() + ";"
    die("shared symbol %r not found in the runtime prelude" % name)
    return None


def main():
    if len(sys.argv) not in (3, 5) or (len(sys.argv) == 5 and sys.argv[3] != "--overrides"):
        die(__doc__)
    src, outdir = sys.argv[1], sys.argv[2]
    overrides = load_overrides(sys.argv[4]) if len(sys.argv) == 5 else []
    inline_memory = os.environ.get("DQ8_DISABLE_MEMORY_FASTPATH") != "1"
    guest_pc_profile = os.environ.get("DQ8_GUEST_PC_PROFILE_BUILD") == "1"
    guest_pc_profile_base = 0
    guest_pc_profile_slots = 0
    if not os.path.exists(src):
        die("missing %s" % src)
    os.makedirs(outdir, exist_ok=True)

    with open(src, "r", encoding="utf-8", errors="replace") as fh:
        lines = fh.readlines()
    sys.stderr.write("read %d lines (%.1f MB)\n"
                     % (len(lines), sum(len(x) for x in lines) / 1e6))

    dispatch_idx = next((i for i, l in enumerate(lines)
                         if l.strip() == "switch (pc >> 12) {"), None)
    if dispatch_idx is None:
        die("outer dispatch switch not found")
    first_case_idx = next((i for i in range(dispatch_idx, len(lines))
                           if re.match(r"^case 0x[0-9A-F]+: switch \(pc\) \{", lines[i])), None)
    if first_case_idx is None:
        die("first inner switch not found")
    outer_default_idx = max(i for i, l in enumerate(lines) if l.startswith("default:"))
    if outer_default_idx <= first_case_idx:
        die("outer default not found after the cases")

    # ---- collect the inner switches (each ends with its own "default: ... } break;")
    blocks = []          # (range_value, first_line_index, past_last_line_index)
    i = first_case_idx
    while i < outer_default_idx:
        m = re.match(r"^case (0x[0-9A-F]+): switch \(pc\) \{", lines[i])
        if not m:
            die("unexpected line %d inside the case region: %r" % (i + 1, lines[i][:80]))
        j = i + 1
        while j < outer_default_idx and not lines[j].startswith("default:"):
            j += 1
        if j >= outer_default_idx:
            die("inner switch at line %d has no default" % (i + 1))
        blocks.append((int(m.group(1), 16), i, j + 1))
        i = j + 1
    sys.stderr.write("found %d inner switches (pages 0x%03X..0x%03X)\n"
                     % (len(blocks), blocks[0][0], blocks[-1][0]))

    translated_pcs = set()
    for _rng, first, past in blocks:
        for line in lines[first + 1:past]:
            match = re.match(r"^case 0x([0-9A-Fa-f]+):", line)
            if match:
                translated_pcs.add(int(match.group(1), 16))
    for item in overrides:
        if item["start"] not in translated_pcs:
            die("override %s entry PC 0x%08X is not a translated guest case" %
                (item["name"], item["start"]))
        if not any(item["start"] <= pc < item["end"] for pc in translated_pcs):
            die("override %s range contains no translated guest PCs" % item["name"])

    if guest_pc_profile:
        guest_pc_profile_base = min(pc & ~1 for pc in translated_pcs)
        guest_pc_profile_limit = max(pc & ~1 for pc in translated_pcs) + 2
        guest_pc_profile_slots = (guest_pc_profile_limit - guest_pc_profile_base) // 2

    # ---- prelude: un-static the shared symbols (everything before the dispatch switch)
    prelude = lines[:dispatch_idx]
    if guest_pc_profile:
        profile_at = next((i for i, line in enumerate(prelude)
                           if line.startswith("static void bench_report(")), None)
        if profile_at is None:
            die("bench_report() not found for guest-PC profile output")
        prelude[profile_at:profile_at] = [
            "int dq8_guest_pc_profile_enabled;\n",
            "uint64_t dq8_guest_pc_profile_pages[4096];\n",
            "uint64_t dq8_guest_pc_profile_pcs[%d];\n" % (guest_pc_profile_slots * 2),
            "static void dq8_guest_pc_profile_report(void) {\n",
            "    if (!dq8_guest_pc_profile_enabled) return;\n",
            "    fprintf(stderr, \"GUEST_PC_PROFILE_BEGIN after_movie_entry=%llu\\n\", (unsigned long long)perf_movie_entries);\n",
            "    for (unsigned page = 0; page < 4096u; ++page)\n",
            "        if (dq8_guest_pc_profile_pages[page])\n",
            "            fprintf(stderr, \"GUEST_PC_PROFILE_PAGE page=%03X steps=%llu\\n\", page, (unsigned long long)dq8_guest_pc_profile_pages[page]);\n",
            "    for (unsigned slot = 0; slot < %du; ++slot)\n" % (guest_pc_profile_slots * 2),
            "        if (dq8_guest_pc_profile_pcs[slot]) {\n",
            "            uint32_t pc = 0x%08Xu + ((slot >> 1) << 1) + (slot & 1u);\n" % guest_pc_profile_base,
            "            fprintf(stderr, \"GUEST_PC_PROFILE_PC pc=%08X steps=%llu\\n\", pc, (unsigned long long)dq8_guest_pc_profile_pcs[slot]);\n",
            "        }\n",
            "}\n",
        ]
        for i, line in enumerate(prelude):
            if line.strip() == "exit(0);" and i and "fflush(stderr);" in prelude[i - 1]:
                prelude[i] = "    dq8_guest_pc_profile_report();\n" + line
                break
        else:
            die("bench_report() exit not found for guest-PC profile output")
    if overrides:
        bench_at = next((i for i, line in enumerate(prelude)
                         if line.startswith("static void bench_report(")), None)
        if bench_at is None:
            die("bench_report() not found for override counters")
        prelude[bench_at:bench_at] = [
            "extern int dq8_overrides_enabled;\n",
            "extern uint64_t dq8_override_hits, dq8_override_original_fallbacks;\n",
        ]
        for i in range(bench_at + 2, len(prelude)):
            if "exit(0);" in prelude[i] and i and (
                    "fflush(stderr);" in prelude[i - 1] or
                    (i > 1 and "fflush(stderr);" in prelude[i - 2])):
                prelude[i] = prelude[i].replace(
                    "exit(0);",
                    "fprintf(stderr, \"BENCH_OVERRIDE handled=%llu call_original=%llu enabled=%d\\n\", "
                    "(unsigned long long)dq8_override_hits, "
                    "(unsigned long long)dq8_override_original_fallbacks, dq8_overrides_enabled);\n    exit(0);")
                break
        else:
            die("bench_report() exit not found for override counters")
    needed = set(SHARED_SYMBOLS)
    if guest_pc_profile:
        needed.add("perf_movie_entries")
    unstatic = re.compile(r"^static\s+.*\b(%s)\b" % "|".join(needed))
    stripped = 0
    for k, line in enumerate(prelude):
        if unstatic.match(line):
            prelude[k] = line[len("static "):]
            stripped += 1
    sys.stderr.write("un-static'd %d prelude declarations\n" % stripped)

    sys.stderr.write("checked inline memory fast path: %s\n" % ("enabled" if inline_memory else "disabled"))
    sys.stderr.write("guest-PC profiling instrumentation: %s\n" % ("enabled" if guest_pc_profile else "disabled"))
    header_decls = [declaration_for(name, lines) for name in SHARED_SYMBOLS]
    if guest_pc_profile:
        header_decls.append("uint64_t perf_movie_entries;")
    ram_size_match = next((re.match(r"^#define GUEST_RAM_SIZE\s+(0x[0-9A-Fa-f]+u?)\s*$", line.strip())
                           for line in lines if line.strip().startswith("#define GUEST_RAM_SIZE")), None)
    if ram_size_match is None:
        die("GUEST_RAM_SIZE definition not found")
    ram_size = ram_size_match.group(1)
    # 12-bit page numbers (pc >> 12) => 4096 pages max; 32 pages per band.
    BAND = 32
    MAX_BANDS = 4096 // BAND

    # ---- part files (one TU per address band so part identity is stable:
    # a new translated page only ever touches its own band's file)
    bands = {}           # band index -> list of (rng, first, past)
    for blk in blocks:
        bands.setdefault(blk[0] >> 5, []).append(blk)

    # ---- chain mode (t_3de68379): the 3dsrecomp scheme.  Each translated PC
    # becomes a C label reached by `goto` from its statically known
    # successor, so a straight-line run pays NO dispatch; the
    # `switch (pc >> 12)` / `switch (pc)` pair survives only as the COLD path
    # (part entry, and transfers the transform cannot prove).  Everything a
    # dispatch iteration does - A05 boundary predicate, steps, PC bookkeeping,
    # running/budget checks - is preserved per instruction in the same order,
    # so a chain retires exactly the same state as the per-instruction loop.
    # Off by default (DQ8_SPLIT_CHAIN=1) so the default generated C stays
    # byte-identical to the pre-chain output.
    chain = os.environ.get("DQ8_SPLIT_CHAIN") == "1"

    PC_ASSIGN = re.compile(r"(?<![<>=!+\-*/%&|^])\bpc\s*=(?!=)")
    LITERAL_RE = re.compile(r"^(?:0[xX][0-9A-Fa-f]+|\d+)[uUlL]*$")
    CASE_RE = re.compile(r"^case 0x([0-9A-Fa-f]+): \{ (.*) break; \}\s*$")

    # Exact static boundary-PC set, parsed from the generated
    # dq8_pc_needs_boundary switch.  main.rs builds that switch and the
    # compile-time page prefilter from ONE list, so they cannot drift; the
    # runtime DQ8_TRACE_PC list is still honoured because every arrival runs
    # dq8_pc_needs_boundary before executing.
    boundary_pcs = set()
    if chain:
        bfn = next((i for i, l in enumerate(lines)
                    if l.startswith("static int dq8_pc_needs_boundary(")), None)
        if bfn is None:
            die("dq8_pc_needs_boundary() not found - cannot build chain mode")
        for k in range(bfn + 1, len(lines)):
            if lines[k].startswith("}"):
                break
            m = re.match(r"\s*case 0x([0-9A-Fa-f]+)u?:", lines[k])
            if m:
                boundary_pcs.add(int(m.group(1), 16))
        sys.stderr.write("chain mode: %d static boundary pcs\n" % len(boundary_pcs))

    def stmt_successors(stmt):
        """Statically known PC successors of one translated statement.

        None when the successor is not provable from the text: any
        `pc = <non-literal>` (indirect branch, LDMIA-to-PC,
        guest_unsupported) or any `&pc` (an SVC shim / override dispatch
        that writes pc itself).  Those transfers always take the cold path -
        never a guess."""
        if "&pc" in stmt:
            return None
        out = set()
        for m in PC_ASSIGN.finditer(stmt):
            semi = stmt.find(";", m.end())
            if semi < 0:
                return None
            rhs = stmt[m.end():semi].strip()
            if not LITERAL_RE.match(rhs):
                return None
            tok = rhs.rstrip("uUlL")
            out.add(int(tok, 16) if tok[:2].lower() == "0x" else int(tok, 10))
        return out or None

    # ---- header: declare ALL possible band functions so the header content
    # does not change when a new band appears (it is part of every .o's
    # cache key; a growing list would invalidate the whole cache).
    with open(os.path.join(outdir, "dq8_rt.h"), "w") as fh:
        fh.write("/* generated by tools/split_dispatch.py - do not edit */\n")
        fh.write("#ifndef DQ8_RT_H\n#define DQ8_RT_H\n")
        fh.write("#include <stdint.h>\n#include <stdio.h>\n#include <stdlib.h>\n"
                 "#include <string.h>\n#include <math.h>\n\n")
        for decl in header_decls:
            fh.write("extern %s\n" % decl)
        fh.write("\n#define DQ8_RT_RAM_SIZE %s\n\n" % ram_size)
        if guest_pc_profile:
            fh.write("extern int dq8_guest_pc_profile_enabled;\n")
            fh.write("extern uint64_t dq8_guest_pc_profile_pages[4096];\n\n")
            fh.write("extern uint64_t dq8_guest_pc_profile_pcs[%d];\n\n" % (guest_pc_profile_slots * 2))
        if inline_memory:
            fh.write(INLINE_MEMORY_HELPERS)
        fh.write("\n")
        fh.write(override_header(overrides))
        fh.write("\n")
        fh.write("\n%s\n" % PART_FN_TYPE)
        for idx in range(MAX_BANDS):
            fh.write("%s\n" % (PART_PROTO % idx))
        fh.write("#endif\n")

    def write_chain_part(path, bi, chunk, pcset):
        """Emit one band as a label/goto straight-line body (chain mode).

        Layout:
          for (;;) {
              int _ph = 0;
              switch (pc >> 12) { case PAGE: switch (pc) { case X: goto L_X } }
              if (!_ph) break;      -> cross-band return (hit is sticky)
              ARRIVE:  full per-instruction prologue, then re-dispatch
              L_X:     same prologue (skipped once via _entry),
                       { statement } running/budget checks <jump>
          }
        """
        cases = []
        for (rng, first, past) in chunk:
            for k in range(first + 1, past - 1):
                m = CASE_RE.match(lines[k].rstrip("\n"))
                if not m:
                    die("chain mode: unparsable case line %d: %r"
                        % (k + 1, lines[k][:120]))
                cases.append((int(m.group(1), 16), m.group(2)))
        if len({pc for pc, _ in cases}) != len(cases):
            die("chain mode: duplicate pc in band %d" % bi)

        with open(path, "w") as fh:
            fh.write("/* generated by tools/split_dispatch.py - do not edit */\n")
            fh.write("/* chain mode (t_3de68379): straight-line goto body, "
                     "cold pc dispatch at entry/indirect transfers only */\n")
            fh.write('#include "dq8_rt.h"\n\n')
            fh.write("int dq8_part_%03d(uint32_t *r, uint32_t *pc_io, int *n_io, "
                     "int *z_io, int *c_io, int *v_io, int *ge_io, int *running_io, "
                     "uint64_t *steps_io, int is_main) {\n" % bi)
            fh.write("    uint32_t pc = *pc_io;\n")
            fh.write("    int n = *n_io, z = *z_io, c = *c_io, v = *v_io, "
                     "ge = *ge_io, running = *running_io;\n")
            fh.write("    uint64_t steps = *steps_io;\n")
            fh.write("    int hit = 0;\n")
            fh.write("    (void)n; (void)z; (void)c; (void)v; (void)ge; "
                     "(void)running; (void)steps; (void)is_main;\n")
            fh.write("    unsigned _budget = %d;\n" % CHAIN_BATCH)
            fh.write("    int _entry = 1;\n")
            fh.write("    for (;;) {\n")
            fh.write("        int _ph = 0;\n")
            fh.write("        switch (pc >> 12) {\n")
            for (rng, first, past) in chunk:
                page_line = lines[first].replace(": switch (pc) {",
                                                 ": _ph = 1; switch (pc) {", 1)
                fh.write(page_line if page_line.endswith("\n") else page_line + "\n")
                for k in range(first + 1, past - 1):
                    m = CASE_RE.match(lines[k].rstrip("\n"))
                    if not m:
                        die("chain mode: unparsable case line %d" % (k + 1))
                    pc = int(m.group(1), 16)
                    fh.write("case 0x%08X: goto L_%08X;\n" % (pc, pc))
                default_line = lines[past - 1]
                fh.write(default_line if default_line.endswith("\n")
                         else default_line + "\n")
            fh.write("        }\n")
            fh.write("        if (!_ph) break;\n")
            # Cold arrival: a transfer the transform could not prove.  Runs
            # the per-instruction prologue the old loop head ran, then
            # re-dispatches (continue -> the switch above).
            fh.write("        ARRIVE: if (!running) break;\n")
            fh.write("        if (--_budget == 0) break;\n")
            fh.write("        if (dq8_pc_needs_boundary(pc)) break;\n")
            fh.write("        ++steps;\n")
            fh.write("        if (is_main) main_guest_pc = pc;\n")
            fh.write("        previous_guest_pc = current_guest_pc;\n")
            fh.write("        current_guest_pc = pc;\n")
            fh.write("        continue;\n")
            # Straight-line body: one label per translated PC.
            for (pc, raw) in cases:
                stmt = raw
                if guest_pc_profile:
                    stmt = ("{ if (dq8_guest_pc_profile_enabled && "
                            "perf_movie_entries) { ++dq8_guest_pc_profile_pages"
                            "[0x%03Xu]; ++dq8_guest_pc_profile_pcs[%du]; } %s }"
                            % (pc >> 12,
                               (((pc & ~1) - guest_pc_profile_base) >> 1) * 2
                               + (pc & 1), stmt))
                if overrides and any(it["start"] <= pc < it["end"]
                                     for it in overrides):
                    stmt = (
                        "{ int _dq8_override_result = DQ8_OVERRIDE_CALL_ORIGINAL; "
                        "if (dq8_overrides_enabled) _dq8_override_result = "
                        "dq8_override_dispatch(pc, r, &pc, &n, &z, &c, &v, &ge, "
                        "&running, &steps, &_budget); "
                        "if (_dq8_override_result == DQ8_OVERRIDE_HANDLED) "
                        "goto ARRIVE; "
                        "if (_dq8_override_result != DQ8_OVERRIDE_CALL_ORIGINAL) { "
                        "fprintf(stderr, \"override %08X returned invalid status "
                        "%d\\n\", pc, _dq8_override_result); exit(17); } "
                        + stmt + " }")
                if inline_memory:
                    for helper in ("mem8", "mem16", "mem32", "put8", "put16",
                                   "put32"):
                        stmt = re.sub(r"\b%s\(" % helper,
                                      "dq8_%s_fast(" % helper, stmt)
                succ = stmt_successors(stmt)
                # Per-instruction checks, in the original order (post-statement
                # running/budget, then the loop-head boundary + step + PC
                # bookkeeping).  A chain jump to a label runs the checks here
                # and the label's prologue runs the bookkeeping; a cold jump
                # hands both to ARRIVE, which does them itself - never both.
                checks = "if (!running) break; if (--_budget == 0) break;"
                if succ is None:
                    tail = "goto ARRIVE;"
                else:
                    ordered = sorted(succ)
                    elig = [t for t in ordered
                            if t in pcset and t not in boundary_pcs]
                    if not elig:
                        tail = "goto ARRIVE;"
                    elif len(elig) == len(ordered):
                        body = "".join("if (pc == 0x%08Xu) goto L_%08X; else "
                                       % (t, t) for t in ordered[:-1])
                        body += "goto L_%08X;" % ordered[-1]
                        tail = checks + " " + body
                    else:
                        # Mixed: test the provable arms (each pays the
                        # checks); every remaining arm is one ARRIVE, which
                        # pays them itself.
                        tail = "".join(
                            "if (pc == 0x%08Xu) { %s goto L_%08X; } else "
                            % (t, checks, t) for t in elig) + "goto ARRIVE;"
                fh.write("    L_%08X: ;\n" % pc)
                fh.write("        if (_entry) { _entry = 0; } else {\n")
                # Provably exact short-circuit: `jump eligibility` already
                # proved this PC is NOT in the static boundary set, so with an
                # empty runtime DQ8_TRACE_PC list the predicate can only
                # return 0 (its page prefilter is an over-approximation; a
                # positive page hit still falls through to the exact switch,
                # which finds no case).  Skipping the CALL - not the check -
                # is what keeps a duplicated per-label prologue cheap: the
                # per-instruction path pays ~40 more host instructions for it.
                fh.write("            if (trace_guest_pc_count && "
                         "dq8_pc_needs_boundary(pc)) break;\n")
                fh.write("            ++steps;\n")
                fh.write("            if (is_main) main_guest_pc = pc;\n")
                fh.write("            previous_guest_pc = current_guest_pc;\n")
                fh.write("            current_guest_pc = pc;\n")
                fh.write("        }\n")
                fh.write("        hit = 1;\n")
                fh.write("        { %s }\n" % stmt)
                fh.write("        if (!running) break;\n")
                fh.write("        if (--_budget == 0) break;\n")
                fh.write("        %s\n" % tail)
            fh.write("    }\n")
            fh.write("    *pc_io = pc; *n_io = n; *z_io = z; *c_io = c; "
                     "*v_io = v; *ge_io = ge; *running_io = running; "
                     "*steps_io = steps;\n")
            fh.write("    return hit;\n}\n")

    parts = []           # (band index, first range of the band)
    for bi in sorted(bands):
        chunk = bands[bi]
        path = os.path.join(outdir, "dq8_part_%03d.c" % bi)
        if chain:
            pcset = set()
            for (rng, first, past) in chunk:
                for k in range(first + 1, past - 1):
                    m = CASE_RE.match(lines[k].rstrip("\n"))
                    if not m:
                        die("chain mode: unparsable case line %d" % (k + 1))
                    pcset.add(int(m.group(1), 16))
            write_chain_part(path, bi, chunk, pcset)
            parts.append((bi, [rng for (rng, _first, _past) in chunk]))
            continue
        with open(path, "w") as fh:
            fh.write("/* generated by tools/split_dispatch.py - do not edit */\n")
            fh.write('#include "dq8_rt.h"\n\n')
            fh.write("int dq8_part_%03d(uint32_t *r, uint32_t *pc_io, int *n_io, int *z_io, "
                     "int *c_io, int *v_io, int *ge_io, int *running_io, "
                     "uint64_t *steps_io, int is_main) {\n" % bi)
            fh.write("    uint32_t pc = *pc_io;\n")
            fh.write("    int n = *n_io, z = *z_io, c = *c_io, v = *v_io, ge = *ge_io, running = *running_io;\n")
            fh.write("    uint64_t steps = *steps_io;\n")
            fh.write("    int hit = 0;\n")
            fh.write("    (void)n; (void)z; (void)c; (void)v; (void)ge; (void)running; "
                     "(void)steps; (void)is_main;\n")
            # Block-chaining loop: dispatch consecutive in-band guest instructions
            # through the hot in-band switch instead of returning to the caller
            # after every single instruction.  Return to the main dispatcher when
            # the pc leaves this band (_ph stays 0), a blocking SVC clears
            # `running`, the batch budget expires (so main can still schedule
            # other threads and run host housekeeping), or - A05 - the next pc
            # requires dq8_main.c's semantic boundary (snprintf shim, fast
            # page-list shortcut, PC-gated trace hooks, DQ8_TRACE_PC pcs).
            # Every pc dispatched inside a batch gets exactly the per-instruction
            # bookkeeping the single-TU loop performs, in the same order:
            # steps++, main_guest_pc (main context only), previous/current pc.
            # The entry pc skips it because dq8_main.c already ran the boundary
            # (including that bookkeeping) before the call.
            fh.write("    unsigned _budget = %d;\n" % CHAIN_BATCH)
            fh.write("    int _entry = 1;\n")
            fh.write("    for (;;) {\n")
            fh.write("        if (!_entry) {\n")
            fh.write("            if (dq8_pc_needs_boundary(pc)) break;\n")
            fh.write("            ++steps;\n")
            fh.write("            if (is_main) main_guest_pc = pc;\n")
            fh.write("            previous_guest_pc = current_guest_pc;\n")
            fh.write("            current_guest_pc = pc;\n")
            fh.write("        }\n")
            fh.write("        _entry = 0;\n")
            fh.write("        int _ph = 0;\n")
            fh.write("        switch (pc >> 12) {\n")
            for (rng, first, past) in chunk:
                for k in range(first, past):
                    line = lines[k]
                    if k == first:
                        line = line.replace(": switch (pc) {", ": _ph = 1; switch (pc) {", 1)
                    case_match = re.match(r"^(case 0x([0-9A-Fa-f]+):) \{ (.*) break; \}\s*$", line)
                    if guest_pc_profile and case_match:
                        case_pc = int(case_match.group(2), 16)
                        line = ("%s { if (dq8_guest_pc_profile_enabled && perf_movie_entries) "
                                "{ ++dq8_guest_pc_profile_pages[0x%03Xu]; "
                                "++dq8_guest_pc_profile_pcs[%du]; } %s break; }\n") % (
                                    case_match.group(1), case_pc >> 12,
                                    (((case_pc & ~1) - guest_pc_profile_base) >> 1) * 2 + (case_pc & 1),
                                    case_match.group(3))
                        case_match = re.match(r"^(case 0x([0-9A-Fa-f]+):) \{ (.*) break; \}\s*$", line)
                    if overrides and case_match:
                        case_pc = int(case_match.group(2), 16)
                        if any(item["start"] <= case_pc < item["end"] for item in overrides):
                            original = case_match.group(3)
                            wrapped = (
                                "%s { int _dq8_override_result = DQ8_OVERRIDE_CALL_ORIGINAL; "
                                "if (dq8_overrides_enabled) _dq8_override_result = "
                                "dq8_override_dispatch(pc, r, &pc, &n, &z, &c, &v, &ge, "
                                "&running, &steps, &_budget); "
                                "if (_dq8_override_result == DQ8_OVERRIDE_HANDLED) break; "
                                "if (_dq8_override_result != DQ8_OVERRIDE_CALL_ORIGINAL) { "
                                "fprintf(stderr, \"override %%08X returned invalid status %%d\\n\", pc, "
                                "_dq8_override_result); exit(17); } %s break; }\n"
                            ) % (case_match.group(1), original)
                            line = wrapped
                    # Only generated guest cases use these fast wrappers. The
                    # original runtime helpers remain the reference/cold path.
                    if inline_memory:
                        for helper in ("mem8", "mem16", "mem32", "put8", "put16", "put32"):
                            line = re.sub(r"\b%s\(" % helper,
                                          "dq8_%s_fast(" % helper, line)
                    fh.write(line if line.endswith("\n") else line + "\n")
            fh.write("        }\n")
            fh.write("        if (!_ph) break;\n")
            fh.write("        hit = 1;\n")
            fh.write("        if (!running) break;\n")
            fh.write("        if (--_budget == 0) break;\n")
            fh.write("    }\n")
            fh.write("    *pc_io = pc; *n_io = n; *z_io = z; *c_io = c; *v_io = v; "
                     "*ge_io = ge; *running_io = running; *steps_io = steps;\n")
            fh.write("    return hit;\n}\n")
        parts.append((bi, [rng for (rng, _first, _past) in chunk]))

    # ---- main TU: prelude + table + dispatch + tail
    tail = lines[outer_default_idx:]
    # drop the outer "default: ... exit(3); } }" prefix, keep " if(steps>=..."
    first = tail[0]
    cut = first.find("} }")
    if cut < 0:
        die("could not locate the end of the outer switch on line %d" % (outer_default_idx + 1))
    tail[0] = first[cut + 3:]

    main_path = os.path.join(outdir, "dq8_main.c")
    tf_idx = next((i for i, l in enumerate(prelude)
                   if l.startswith("static void translated_from(")), None)
    if tf_idx is None:
        die("translated_from() not found in the prelude")
    with open(main_path, "w") as fh:
        fh.writelines(prelude[:tf_idx])
        if overrides:
            fh.write("int dq8_overrides_enabled = 1;\n")
            fh.write("uint64_t dq8_override_hits = 0;\n")
            fh.write("uint64_t dq8_override_original_fallbacks = 0;\n")
        fh.write('#include "dq8_rt.h"\n\n')
        fh.write("static const Dq8PartFn dq8_part_table[4096] = {\n")
        for gi, rngs in parts:
            for rng in rngs:
                fh.write("    [0x%03X] = dq8_part_%03d,\n" % (rng, gi))
        fh.write("};\n\n")
        # everything from translated_from() up to (excluding) the dispatch switch
        fh.writelines(prelude[tf_idx:])
        fh.write("        {\n")
        fh.write("            int _part = (int)(pc >> 12);\n")
        fh.write("            if (_part >= 0 && _part < 4096 && dq8_part_table[_part] &&\n")
        fh.write("                dq8_part_table[_part](r, &pc, &n, &z, &c, &v, &ge, &running,\n")
        fh.write("                                     &steps, context == NULL)) {\n")
        fh.write("            } else {\n")
        fh.write("                %s\n" % ESCAPE_FMT)
        fh.write("            }\n")
        fh.write("        }\n")
        fh.write("        } /* end while */\n")
        tail_text = "".join(tail)
        main_marker = "int main(int argc, char **argv) {"
        if main_marker in tail_text:
            gates = []
            if overrides:
                gates.append("    if (getenv(\"DQ8_DISABLE_OVERRIDES\")) dq8_overrides_enabled = 0;")
            if guest_pc_profile:
                gates.append("    dq8_guest_pc_profile_enabled = getenv(\"DQ8_GUEST_PC_PROFILE\") != NULL;")
            tail_text = tail_text.replace(main_marker, main_marker + "\n" + "\n".join(gates), 1)
        elif guest_pc_profile:
            die("main() not found for guest-PC profile gate")
        fh.write(tail_text)
    n_entries = sum(len(rngs) for _gi, rngs in parts)
    if n_entries != len(blocks):
        die("table has %d entries but there are %d page switches" % (n_entries, len(blocks)))
    sys.stderr.write("wrote %s (+%d part files, %d page table entries)\n"
                     % (main_path, len(parts), n_entries))

    # ---- prune stale part files (e.g. leftovers from a previous, larger
    # manifest) so they are neither compiled nor linked into the binary.
    fresh = {"dq8_part_%03d.c" % bi for bi in bands}
    removed = 0
    for name in os.listdir(outdir):
        if re.match(r"^dq8_part_\d+\.c$", name) and name not in fresh:
            os.remove(os.path.join(outdir, name))
            removed += 1
    if removed:
        sys.stderr.write("pruned %d stale part file(s) not in the fresh manifest\n"
                         % removed)


if __name__ == "__main__":
    main()
