#!/usr/bin/env python3
"""Generate the FN_EMIT bench function table (t_8b9fda4c, extended t_ba4d43cb).

Reads the `0xENTRY 0xEND` list (tools/fn_emit_top300.txt) and writes the C table
that tools/fn_emit_bench.c includes.  One row per emitted function: entry, end,
instruction count, the emitted body function, the band part function(s) that
cover the range in the split build (the dispatcher arm calls exactly the TUs the
real binary uses), and the PCs at which the two arms must stop.

Those stop PCs matter for the measurement rig: a split part chains instructions
inside a band without returning, so the dispatcher arm would run past the
function's end.  The harness arms `dq8_pc_needs_boundary` at exactly those PCs
(the A05 predicate the dispatch loop already runs per instruction) and the
dispatcher arm stops *before* executing one -- which is exactly where the
emitted body stops for the two exit kinds added by t_ba4d43cb:

  * an indirect transfer (`bx rX`, `blx rX`, `ldr pc, [...]`, a table load):
    the body computes the target, writes it to pc and returns to the
    dispatcher, so the interpreted arm must not execute that instruction;
  * an opaque instruction (SVC shim, undecodable escape, thread yield): the
    body exits *before* it and the dispatcher owns that step.

Exits the body does execute (a native return, a direct branch, the pop-return
idiom) need no stop PC: both arms run the instruction and end at the same pc.

usage: fn_emit_bench_gen.py <list> <outdir> [split_dir]
"""
import os
import re
import sys

# A `L_<addr>:` label followed by `++exec;` and a one-line statement that leaves
# through L_EXIT.  Only the *unconditional* forms appear as one line; the
# conditional forms are emitted inside a do/while with an _fn_ex exit and both
# arms execute them equally.
EXIT_STMT = re.compile(
    r"^L_([0-9A-F]{8}):\n"
    r"\s*\+\+exec;\n"
    r"\s*\{([^\n]*goto L_EXIT;)\s*\}\s*$",
    re.M,
)
OPAQUE_PC = re.compile(r"^\{ pc = 0x([0-9A-F]{8})u; goto L_EXIT; \}$")
MAX_BOUNDARY = 15  # the runtime's trace_guest_pcs[] has 16 slots
C_RANGE_END = re.compile(r"^pc = 0x([0-9A-F]{8})u;")


def stops_of(path):
    """PCs at which the two arms must stop.

    Returns (before, after):
      before -- the emitted body exits *before* the instruction (opaque: SVC
                shim, undecodable escape, thread yield), so the interpreted arm
                must be stopped before executing it;
      after  -- the body *executes* an indirect transfer (bx/blx/ldr pc) and
                then returns to the dispatcher with pc = target, so the
                interpreted arm runs the instruction and has to be stopped on
                arrival at the target instead (the harness arms the native
                arm's resulting pc, which it only knows at run time)."""
    if not os.path.exists(path):
        return [], [], 0
    text = open(path, errors="replace").read()
    before = set()
    after = set()
    over = 0
    for m in EXIT_STMT.finditer(text):
        addr = int(m.group(1), 16)
        stmt = m.group(2).strip()
        if "pc = r14" in stmt:
            continue                       # the body executes the return
        op = OPAQUE_PC.match(stmt)
        if op:
            tgt = int(op.group(1), 16)
            if tgt == addr:
                before.add(addr)            # opaque exit: dispatcher owns it
            continue                        # plain jump to a constant
        c = C_RANGE_END.match(stmt)
        if c:
            continue                        # const branch/exit, both arms run it
        after.add(addr)                     # indirect exit: body executes it
    return sorted(before), sorted(after), over


def main():
    lst, outdir = sys.argv[1], sys.argv[2]
    split = sys.argv[3] if len(sys.argv) > 3 else None
    rows = []
    for line in open(lst):
        line = line.split("#")[0].strip()
        if not line:
            continue
        a, b = line.split()
        entry = int(a, 16) & 0xFFFFFFFF
        end = int(b, 16) & 0xFFFFFFFF
        width = 2 if entry & 1 else 4
        insns = (end - entry) // width
        bands = sorted({p >> 17 for p in range(entry, end, width)})
        st = []
        if split:
            shard = os.path.join(split, "dq8_fnshard_%03d.c" % len(rows))
            # only the before-execution stops are armed statically; the arrival
            # pc of an executed indirect exit is armed at run time from the
            # native arm's own resulting pc (see tools/fn_emit_bench.c).
            st, _after, _ = stops_of(shard)
        bounds = sorted(set(st) | {end})
        if len(bounds) > MAX_BOUNDARY:
            sys.stderr.write(
                "warning: 0x%08X has %d stop pcs, truncated to %d\n"
                % (entry, len(bounds), MAX_BOUNDARY))
            bounds = bounds[:MAX_BOUNDARY]
        rows.append((entry, end, insns, bands, bounds))
    os.makedirs(outdir, exist_ok=True)
    path = os.path.join(outdir, "fn_table.h")
    with open(path, "w") as fh:
        fh.write("/* generated by tools/fn_emit_bench_gen.py - do not edit */\n")
        fh.write("#define FN_BOUND_MAX %d\n" % (MAX_BOUNDARY + 1))
        fh.write("typedef struct {\n")
        fh.write("    uint32_t entry, end;\n")
        fh.write("    unsigned insns;\n")
        fh.write("    unsigned long long (*native)(Dq8FnState *st, uint32_t resume);\n")
        fh.write("    int nbands;\n")
        fh.write("    uint32_t band[2];\n")
        fh.write("    int (*part[2])(uint32_t *, uint32_t *, int *, int *, int *, int *, int *, int *, uint64_t *, int);\n")
        fh.write("    int nbounds;\n")
        fh.write("    uint32_t bounds[%d];\n" % (MAX_BOUNDARY + 1))
        fh.write("} FnSpec;\n")
        fh.write("static const FnSpec fn_specs[] = {\n")
        for entry, end, insns, bands, bounds in rows:
            names = ", ".join("dq8_part_%03d" % b for b in bands)
            bl = ", ".join("0x%08Xu" % b for b in bounds)
            fh.write(
                "    { 0x%08Xu, 0x%08Xu, %du, dq8_fnb_%08X, %d, { %s }, { %s }, %d, { %s } },\n"
                % (entry, end, insns, entry, len(bands),
                   ", ".join("%du" % b for b in bands), names, len(bounds), bl)
            )
        fh.write("};\n")
        fh.write("#define FN_SPECS_COUNT %d\n" % len(rows))
    sys.stderr.write("wrote %s (%d functions)\n" % (path, len(rows)))


if __name__ == "__main__":
    main()
