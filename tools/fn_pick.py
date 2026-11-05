#!/usr/bin/env python3
"""Pick the hot guest FUNCTIONS for the FN_EMIT pilot (t_8b9fda4c).

Unlike tools/find_hot_blocks.py (which ranks basic blocks inside one range),
this ranks whole guest functions and produces the list the emitter consumes.

Inputs
  * the recompiler's generated C (``target/dq8-startup.c``): the authoritative
    static text, one ``case 0xADDR: { ... }`` per translated instruction.
  * one or more ``DQ8_GUEST_PC_PROFILE`` logs: per-PC executed counts.

Model
  A *function* is identified by an entry address and an extent, both recovered
  from the emitted C:
    - entry candidates are CALL TARGETS, i.e. the ``pc = 0xTARGETu`` of a
      statement of the form ``r[14] = 0xRETu; pc = 0xTARGETu;`` (the emitter's
      own encoding of ``bl``/``blx`` immediate);
    - the extent is a recursive descent from the entry over the emitted
      successors: constants inside a bounded window are followed, a
      ``pc = r[14]`` terminates the walk, and a call follows only its return
      address (the callee is another function).
  This is the same successor relation the FN_EMIT emitter walks, so a function
  it ranks "emittable" is one the emitter can actually take.

Emittability is checked with the emitter's own refusal rules (indirect pc,
pc read, dispatch-loop locals, SVC/unsupported instructions, boundary PCs) so
the list handed to the build has no surprises.  Nothing here is authoritative:
``recompile-arm --fn-emit-list`` re-checks every range and prints
``FN_EMIT_SKIP`` for anything it refuses.

usage:
  fn_pick.py --generated target/dq8-startup.c \
             --profile evidence/hot-regions-rx580/prof-field.stderr \
             --profile evidence/hot-regions-rx580/prof-battle.stderr \
             [--top 20] [--max-insns 4000] [--out tools/fn_emit_top20.txt] \
             [--table reports/FN_EMIT_PILOT_LIST.md]
"""
import argparse
import re
from collections import defaultdict

CASE_RE = re.compile(r"^case 0x([0-9A-Fa-f]+): ?(.*)$")
PCASSIGN = re.compile(r"(?<![A-Za-z0-9_])pc\s*=\s*([^;]*);")
CONST = re.compile(r"^0x([0-9A-Fa-f]+)u?$")
CALL = re.compile(r"^\{\s*r\[14\] = 0x([0-9A-Fa-f]+)u; pc = 0x([0-9A-Fa-f]+)u; break; \}\s*$")
CALL_ANY = re.compile(r"r\[14\] = 0x([0-9A-Fa-f]+)u; pc = 0x([0-9A-Fa-f]+)u;")
RET = re.compile(r"(?<![A-Za-z0-9_])pc\s*=\s*r\[14\]")
PROF_TOTAL = re.compile(r"^GUEST_PC_PROFILE_BEGIN .*total_steps=(\d+)")
PROF_PC = re.compile(r"^GUEST_PC_PROFILE_PC pc=([0-9A-F]+) steps=(\d+)")

# Refusal tokens of src/fn_emit.rs (keep in sync).  A statement carrying any of
# them cannot run outside the dispatch loop: it references a dispatch-loop local
# or is a host-service instruction.  Since t_ba4d43cb the emitter does not
# REFUSE the whole function for these -- it ends the native path at that
# instruction and hands that one step to the dispatcher ("opaque exit"), so
# they only cost a dispatcher round trip, not the function.
OPAQUE = [
    "steps", "next_schedule_step", "next_pump_step", "next_progress_step",
    "trace_match", "trace_i", "context", "thread_execution_depth",
    "svc_", "guest_unsupported", "guest_udf",
    "thread_yield", "thread_stop", "yield_requested",
]
# Still genuinely refused: a statement this oracle cannot model at all.
FORBIDDEN_INSN = ["tailcall_indirect(", "return_indirect(", "call_indirect("]


# BOUNDARY_PCS from src/main.rs: the main loop must see these PCs.  A function
# whose extent contains one cannot be emitted (its body would skip the hook).
BOUNDARY_PCS = {
    0x00119198, 0x00143974, 0x00143975, 0x0011A1C0, 0x0011A1EC, 0x003AAD54,
    0x003AB3E8, 0x0016572C, 0x00223E40, 0x00223E44, 0x00223E48, 0x0011D608,
    0x0011D62C, 0x003A5028, 0x003A5088, 0x003A50BC, 0x003A50C4, 0x003A5134,
    0x003A5138, 0x003A3718, 0x003A36B8, 0x003A5110, 0x003A5114, 0x003A5118,
    0x003A511C, 0x002FD5F9, 0x002FD7D7, 0x002FD93F, 0x002FD941, 0x004725E8,
    0x004725EC, 0x002FA318,
}


def load_cases(path):
    cases = {}
    with open(path, errors="replace") as fh:
        for line in fh:
            if not line.startswith("case 0x"):
                continue
            m = CASE_RE.match(line.rstrip("\n"))
            if not m:
                continue
            cases[int(m.group(1), 16)] = m.group(2)
    return cases


def load_profile(path):
    steps = defaultdict(int)
    total = 0
    with open(path, errors="replace") as fh:
        for line in fh:
            m = PROF_TOTAL.match(line)
            if m:
                total = int(m.group(1))
                continue
            m = PROF_PC.match(line)
            if m:
                steps[int(m.group(1), 16)] += int(m.group(2))
    return steps, total


def pc_assigns(body):
    """The right-hand sides of every `pc = ...;` in one emitted statement."""
    return [m.group(1).strip() for m in PCASSIGN.finditer(body)]


def successors(addr, body, window):
    """Successor PCs of one emitted statement (the emitter's own relation).

    Returns (const_targets, calls) where `calls` is [(ret, target)].
    Calls contribute their return address as the in-function successor.
    """
    consts = [int(m.group(1), 16) for m in
              (CONST.match(v) for v in pc_assigns(body)) if m]
    calls = [(int(r, 16), int(t, 16)) for r, t in CALL_ANY.findall(body)]
    return consts, calls


def classify(addr, body):
    """(kind, detail) for one statement.

    Kinds (t_ba4d43cb, the emitter's own vocabulary):
      const   -- `pc = 0xTARGET` (direct branch / return-to-const)
      ret     -- `pc = r[14]` (native return)
      indir   -- `pc = <expression>`: the successor set is not statically known
                 (`bx rX`, `blx rX`, `ldr pc, [..]`, a table load).  The
                 emitter executes the instruction that computes the target and
                 returns to the dispatcher -- so the walk stops here (no
                 successors) but the function is still emittable.
      opaque  -- an instruction the dispatcher owns (SVC shim, undecodable
                 escape, thread yield, dispatch-loop local).  The emitter ends
                 the native path *before* it and lets the dispatcher execute
                 that one instruction; the walked const successors are where
                 control comes back.
      reject  -- genuinely unmodellable (a read of `pc` that is not the
                 just-assigned value, `r[15]`, an unknown control helper).
    """
    kinds = []
    pc_seen = False
    # Walk the `pc` tokens in source order, exactly like the emitter's rewrite()
    # (src/fn_emit.rs): a read is only unmodellable when *no* earlier assignment
    # in the same statement has produced the local.  A read after an assignment
    # (`if (pc == 0xFFFFFFFFu)` in the pop-return idiom, the same test before an
    # `else { pc = T; }` arm of an indirect load) is the emitted body's own local
    # and is fine — the t_ba4d43cb oracle wrongly refused 516 functions for it
    # (t_aab205ba).
    for m in re.finditer(r"(?<![A-Za-z0-9_])pc(?![A-Za-z0-9_])", body):
        after = body[m.end():]
        if not (re.match(r"\s*=[^=]", after)):
            if not pc_seen:
                return "reject", "reads pc before assigning it"
            continue
        rhs = after.split("=", 1)[1].split(";", 1)[0].strip()
        pc_seen = True
        mm = CONST.match(rhs)
        if mm:
            kinds.append("const")
        elif rhs.replace(" ", "") == "r[14]":
            kinds.append("ret")
        else:
            if re.search(r"(?<![A-Za-z0-9_])pc(?![A-Za-z0-9_])", rhs):
                return "reject", "pc source reads pc (`%s`)" % rhs[:32]
            kinds.append("indir")
    for tok in FORBIDDEN_INSN:
        if tok in body:
            return "reject", "contains `%s`" % tok
    for tok in OPAQUE:
        if tok in body:
            return "opaque", "contains `%s` (dispatcher exit)" % tok
    if "r[15]" in body:
        return "reject", "references r[15] (guest pc)"
    if not kinds:
        return "ok", ""
    # A statement can both branch and be opaque; opaque wins (the body exits
    # before executing it), but the const successors still shape the range.
    return kinds[-1], ""


def descend(entry, cases, max_insns, max_span):
    """Recursive descent from `entry`; returns (extent, addrs, reason).

    An `indir` instruction ends the walk (its successors are only known at
    run time); an `opaque` instruction ends the native path *at* it but the
    dispatcher continues at its const successors, so those are still walked and
    become resume entries in the emitted body.
    """
    todo = [entry]
    seen = set()
    addrs = []
    kinds = {}
    while todo:
        a = todo.pop()
        if a in seen:
            continue
        if a not in cases:
            return None, None, None, "no emitted case at 0x%08X" % a
        seen.add(a)
        addrs.append(a)
        if len(addrs) > max_insns:
            return None, None, None, "larger than --max-insns %d" % max_insns
        if a - entry > max_span:
            return None, None, None, "descends past --max-span 0x%X" % max_span
        body = cases[a]
        kind, detail = classify(a, body)
        kinds[a] = kind
        if kind == "reject":
            return None, None, None, "0x%08X: %s" % (a, detail)
        if kind == "indir":
            continue                     # computed target: no static successor
        consts, calls = successors(a, body, max_span)
        for ret, _t in calls:
            if entry <= ret < entry + max_span:
                todo.append(ret)
        if kind == "opaque":
            # The emitted body exits before this instruction; the dispatcher
            # executes it and continues at its statically known successors.
            for c in consts:
                if entry <= c < entry + max_span and c not in seen:
                    todo.append(c)
            continue
        if RET.search(body):
            continue
        for c in consts:
            if entry <= c < entry + max_span and c not in seen:
                todo.append(c)
    width = 2 if entry & 1 else 4
    addrs.sort()
    extent = addrs[-1] + (2 if addrs[-1] & 1 else 4)
    # The emitter walks every instruction address in [entry, extent): a range
    # with a hole is refused there, and a mixed ARM/Thumb stride is not
    # modelled here either.  Require a clean single-stride range.
    want = range(entry, extent, width)
    missing = [a for a in want if a not in cases]
    if missing:
        # t_session: a hole inside the proposed extent used to refuse the WHOLE
        # function.  Emit the clean prefix instead: truncate the range at the
        # first hole and require the instruction before it to terminate (a
        # jump/return), so the emitted body can never fall out of its own code
        # and everything past the hole keeps going through the dispatcher
        # exactly as it does today.
        trunc = missing[0]
        if trunc <= entry:
            return None, None, None, "hole at 0x%08X (stride %d)" % (missing[0], width)
        want = range(entry, trunc, width)
        addrs = [a for a in addrs if a < trunc]
        if not addrs:
            return None, None, None, "hole at 0x%08X (stride %d)" % (missing[0], width)
        extent = addrs[-1] + (2 if addrs[-1] & 1 else 4)
    # The last instruction of the range must be a terminator, otherwise the
    # emitted body would fall out of its own code with a stale pc.
    last_kind = kinds.get(addrs[-1])
    if last_kind not in ("const", "ret", "indir", "opaque"):
        return None, None, None, "last instruction 0x%08X does not terminate" % addrs[-1]
    if kinds.get(entry) == "opaque":
        return None, None, None, "entry instruction 0x%08X needs the dispatcher" % entry
    return extent, list(want), kinds, ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--generated", required=True)
    ap.add_argument("--profile", action="append", default=[])
    ap.add_argument("--top", type=int, default=20)
    ap.add_argument("--max-insns", type=int, default=4000)
    ap.add_argument("--max-span", type=lambda s: int(s, 0), default=0x20000)
    ap.add_argument("--min-insns", type=int, default=2)
    ap.add_argument("--out")
    ap.add_argument("--table")
    ap.add_argument("--explain", type=int, default=0,
                    help="print the top N rejected candidates too")
    ap.add_argument("--hot-entries", type=int, default=0,
                    help="after the call-target selection, probe this many of the "
                         "hottest uncovered executed pcs as extra entry points "
                         "(a guest function reached by a computed branch has no "
                         "`bl` and is invisible to the call-target set)")
    ap.add_argument("--hot-min-steps", type=int, default=0,
                    help="only keep a hot entry whose range adds at least this "
                         "many executed instructions that are not covered yet")
    ap.add_argument("--max-fns", type=int, default=100000,
                    help="hard cap on the number of selected functions")
    args = ap.parse_args()

    cases = load_cases(args.generated)
    print("cases=%d" % len(cases))
    steps = defaultdict(int)
    total = 0
    for p in args.profile:
        s, t = load_profile(p)
        for k, v in s.items():
            steps[k] += v
        total += t
        print("profile %s: %d pcs, total_steps=%d" % (p, len(s), t))
    if not total:
        total = sum(steps.values())
    print("aggregate: %d pcs, %d executed instructions" % (len(steps), total))

    # Entry candidates: call targets.
    entries = set()
    for body in cases.values():
        for _ret, tgt in CALL_ANY.findall(body):
            entries.add(int(tgt, 16))
    print("call-target entries: %d" % len(entries))

    scored = []
    rejected = []

    def candidate(e, hot):
        """Validate one entry as an emitter range; returns a dict or a reason."""
        extent, addrs, kinds, why = descend(e, cases, args.max_insns, args.max_span)
        if extent is None:
            return None, why
        if len(addrs) < args.min_insns:
            return None, "only %d instruction(s)" % len(addrs)
        if any((a in BOUNDARY_PCS or (a | 1) in BOUNDARY_PCS) for a in addrs):
            return None, "contains a main-loop boundary pc"
        got = sum(steps.get(a, 0) for a in addrs)
        if not got:
            return None, "never executed in the profiles"
        calls_all = sum(1 for a in addrs for _r, _t in CALL_ANY.findall(cases[a]))
        return {
            "entry": e, "extent": extent, "insns": len(addrs),
            "steps": got, "share": 100.0 * got / total,
            "calls": calls_all, "addrs": addrs, "hot": hot,
            "exits": sum(1 for a in addrs if kinds.get(a) in ("indir", "opaque")),
            "callees": {int(t, 16) for a in addrs for _r, t in CALL_ANY.findall(cases[a])},
        }, ""

    for e in sorted(entries):
        d, why = candidate(e, False)
        if d is None:
            rejected.append((0, e, why))
        else:
            scored.append(d)
    scored.sort(key=lambda d: -d["steps"])
    print("\nemittable candidates: %d (rejected %d)" % (len(scored), len(rejected)))

    # Greedy selection with PC-set dedupe: a mid-function call target (a shared
    # epilogue, an entry into the middle of a routine) is a valid emitter range
    # but its instructions are already covered by an outer range, and counting
    # it twice would inflate the coverage number.  The one exception is a
    # candidate that a selected function *calls*: taking it turns that call into
    # a real C call (the point of the pilot), and the coverage is still counted
    # as a union, so the number stays honest.
    sel = []
    covered = set()
    call_edges = []          # (caller entry, callee entry)
    for d in scored:
        if len(sel) >= args.top:
            break
        overlap = covered.intersection(d["addrs"])
        if overlap:
            callers = [s for s in sel if d["entry"] in s["callees"]]
            if not callers:
                d["skipped"] = "PCs already covered by a selected function"
                continue
            d["overlap_with"] = callers[0]["entry"]
            call_edges.append((callers[0]["entry"], d["entry"]))
        sel.append(d)
        covered.update(d["addrs"])
    print("call-target phase: %d functions, %.2f%% of executed instructions"
          % (len(sel), 100.0 * sum(steps.get(a, 0) for a in covered) / total))

    # Second phase (t_aab205ba): hot entry points that are NOT call targets.
    #
    # A guest function is not always entered through a `bl`: a dispatch table, a
    # `bx` from a scheduler, a loop head re-entered by the main loop all start a
    # body at a pc that no `r[14] = RET; pc = TARGET;` statement mentions.  The
    # emitter takes an explicit (entry,end) range with no assumption about how
    # control arrives, so such a pc is a perfectly good entry: descend from it,
    # emit the enclosing function, and the hook dispatches it exactly like a
    # call target.  This is where the remaining interpreted instructions are --
    # measured, the call-target-only set stops at 51.8 % while the executed
    # instructions reachable from hot non-call-target entries are another ~15
    # points (reports/FN_EMIT_PCREL.md).
    hot_added = []
    if args.hot_entries:
        probed = 0
        seen_hot = set()
        hot_list = sorted(((n, a) for a, n in steps.items() if a not in covered),
                          reverse=True)
        for _n, a in hot_list:
            if len(sel) >= args.max_fns:
                break
            if a in covered or a in seen_hot:
                continue
            seen_hot.add(a)
            if probed >= args.hot_entries:
                break
            probed += 1
            d, _why = candidate(a, True)
            if d is None:
                continue
            new = sum(steps.get(x, 0) for x in d["addrs"] if x not in covered)
            if new < args.hot_min_steps:
                continue
            d["new_steps"] = new
            d["new_share"] = 100.0 * new / total
            d["new_insns"] = sum(1 for x in d["addrs"] if x not in covered)
            sel.append(d)
            covered.update(d["addrs"])
            hot_added.append(d)
        hot_added.sort(key=lambda d: -d["new_steps"])
        # Value order: the call-target phase first, then the hot entries by the
        # executed instructions they add, so any prefix of the list is the best
        # list of that size (the coverage curve relies on it).
        sel = sel[:len(sel) - len(hot_added)] + hot_added
        print("hot-entry phase: probed %d hot uncovered pcs, added %d functions, "
              "%.2f%% total" % (probed, len(hot_added),
                                100.0 * sum(steps.get(a, 0) for a in covered) / total))
    chosen = {d["entry"] for d in sel}
    for d in sel:
        d["calls_sel"] = sum(
            1 for a in d["addrs"] for _r, t in CALL_ANY.findall(cases[a])
            if int(t, 16) in chosen)
        d["calls_sel_entries"] = sorted(
            {int(t, 16) for a in d["addrs"] for _r, t in CALL_ANY.findall(cases[a])
             if int(t, 16) in chosen})
        d["union_insns"] = len(set(d["addrs"]) - (covered - set(d["addrs"])))
    cov = 100.0 * sum(steps.get(a, 0) for a in covered) / total
    print("selected %d functions, %.2f%% of executed instructions (union of %d PCs)"
          % (len(sel), cov, len(covered)))
    # Coverage curve: the greedy selection is in ranked order, so the list a
    # smaller --top would have produced is exactly this list's prefix.
    curve = " ".join(
        "%d:%.1f%%" % (n, 100.0 * sum(steps.get(a, 0) for a in union) / total)
        for n, union in ((n, {x for d in sel[:n] for x in d["addrs"]})
                         for n in (1, 5, 20, 50, 100, 200, 300, 500, 1000,
                                   2000, 3000, 5000, len(sel)))
        if n <= len(sel))
    print("coverage curve (functions:share): %s" % curve)
    n_exits = sum(d["exits"] for d in sel)
    n_insns = sum(d["insns"] for d in sel)
    print("dispatcher exits inside the selection: %d (%.2f%% of %d emitted instructions)"
          % (n_exits, 100.0 * n_exits / max(1, sum(d["union_insns"] for d in sel)), n_insns))

    print("native-call edges inside the selection: %d %s"
          % (len(call_edges), " ".join("0x%08X->0x%08X" % e for e in call_edges)))
    # Reason histogram for what was left out, weighted by the entry PC's own
    # executed count (a lower bound on what each rejected function carries).
    hist = defaultdict(lambda: [0, 0])
    for _w, e, why in rejected:
        key = why.split(" (")[0]
        if ":" in key:
            key = key.split(":", 1)[1].strip()
        hist[key][0] += 1
        hist[key][1] += steps.get(e, 0)
    print("\nrejections by reason (entry-PC executed instructions as a lower bound):")
    for k, (n, w) in sorted(hist.items(), key=lambda kv: -kv[1][1])[:12]:
        print("  %6d functions  %12d steps  %s" % (n, w, k))

    lines = []
    for i, d in enumerate(sel, 1):
        tag = "T" if d["entry"] & 1 else "ARM"
        lines.append("# %2d  %08X..%08X  %5d insns  %7d steps  %6.3f%%  %2d calls (%d to selected)"
                     % (i, d["entry"], d["extent"], d["insns"], d["steps"],
                        d["share"], d["calls"], d["calls_sel"]))
        lines.append("0x%08X 0x%08X" % (d["entry"], d["extent"]))
    if args.out:
        with open(args.out, "w") as fh:
            fh.write("\n".join(lines) + "\n")
        print("wrote %s" % args.out)

    if args.table:
        with open(args.table, "w") as fh:
            fh.write("| # | entry | extent | insns | executed | share | calls | to selected |\n")
            fh.write("|---:|---|---|---:|---:|---:|---:|---:|\n")
            for i, d in enumerate(sel, 1):
                fh.write("| %d | `0x%08X` | `0x%08X` | %d | %d | %.3f%% | %d | %d |\n" % (
                    i, d["entry"], d["extent"], d["insns"], d["steps"],
                    d["share"], d["calls"], d["calls_sel"]))
        print("wrote %s" % args.table)

    for i, d in enumerate(sel, 1):
        print("%2d  0x%08X-0x%08X %5d insns %6.3f%% calls=%d->sel=%d %s"
              % (i, d["entry"], d["extent"], d["insns"], d["share"],
                 d["calls"], d["calls_sel"],
                 " ".join("0x%08X" % t for t in d["calls_sel_entries"])))
    if args.explain:
        rejected.sort(key=lambda r: -sum(steps.get(a, 0) for a in
                                         range(r[1], r[1] + 0x1000, 4)))
        print("\ntop rejected:")
        for score, e, why in rejected[: args.explain]:
            print("  0x%08X %s" % (e, why))


if __name__ == "__main__":
    main()
