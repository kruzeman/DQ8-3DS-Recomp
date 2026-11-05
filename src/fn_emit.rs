//! FN_EMIT: emit guest functions as real C functions (registers as C locals,
//! direct intra-function `goto` edges, native `return`, and — for calls whose
//! callee is another emitted function — a real C call).
//!
//! This is the multi-function pilot (t_8b9fda4c) grown out of the one-function
//! spike (t_f110c9ca, `reports/FN_EMIT_SPIKE.md`), extended by t_ba4d43cb from
//! "only functions without indirect control flow" to **every function that is
//! safe to emit**, by reusing the opaque-instruction exit (t_2fe227ca).
//!
//! What is different from the spike:
//!
//! * **A list, not one hard-coded range.**  `--fn-emit-list FILE` takes
//!   `0xENTRY 0xEND` pairs (see `tools/fn_pick.py`).  A range the emitter
//!   cannot express is *skipped* with a printed reason instead of failing the
//!   build, so the list can be regenerated from a profile without hand-editing.
//! * **Calls between selected functions are real C calls.**  A statement whose
//!   whole body is `r[14] = RET; pc = TARGET;` is a guest `bl`; when TARGET is
//!   another emitted function the body calls `dq8_fnb_TARGET(st, 0)` — one C
//!   call, no dispatcher round trip.  The callee runs with its own register
//!   locals; the shared `Dq8FnState` carries the register file across the call
//!   (spill before, reload after) and the caller resumes at the return address
//!   with a direct `goto`.
//! * **A call to a NON-selected function keeps the dispatcher.**  The
//!   statement still sets `lr` and `pc` and leaves through `L_EXIT` exactly as
//!   in the spike; the return address becomes a *resume entry*, so when the
//!   dispatcher eventually delivers control back to it the function body
//!   resumes at that label instead of being re-entered at its entry.
//! * **Indirect control flow no longer disqualifies a function** (t_ba4d43cb).
//!   `bx r3`, `blx r3`, `ldr pc, [sp, #8]`, a dispatch-table load — anything
//!   whose successor set is not statically known — used to refuse the whole
//!   function; 9 614 of 15 074 candidate functions were refused for it.  Now
//!   the walk ends there: the body executes the instruction that computes the
//!   target (including the target validation the dispatcher applies), writes
//!   the architectural state back, sets `pc` to the computed target and
//!   returns to the dispatcher.  The dispatcher takes that one step and may
//!   re-enter the same body later at a resume entry — the same mechanism the
//!   opaque exit already used for undecodable instructions.
//! * **An instruction the body cannot run is an *opaque exit*** (t_ba4d43cb).
//!   SVC shims, undecodable escapes, thread yield/stop and anything that
//!   references a dispatch-loop local end the native path *before* the
//!   instruction (`pc = addr; goto L_EXIT`) and the address after it becomes a
//!   resume entry, so the dispatcher executes exactly that one instruction and
//!   the body continues natively.  Reusing the t_2fe227ca machinery is the
//!   point: there is one fallback convention, not two.
//! * **Sharded output.**  Each emitted function is written into its own
//!   `FN_EMIT_SHARD` block, which `tools/split_dispatch.py` cuts into a
//!   separate small TU (`dq8_fnshard_NNN.c`) so that editing one function
//!   recompiles one shard, not the runtime TU or the 6 MB dispatch band.
//!
//! Since t_21dda9b5 a body does **not** copy the register file: `Dq8FnState.r`
//! points at the dispatcher's own array, which is the architectural file, and a
//! body flushes only the slots it wrote (exact read/write sets, see `FnSets`).
//!
//! Since t_40d37ebb the call boundary is chosen per call site from a per-body
//! classification ([`fn_may_dispatch`]): a callee that cannot hand control to
//! the dispatcher runs straight through and publishes exactly its write set, so
//! the caller keeps the exact sets; a callee that can leaves through `L_EXIT`
//! with a pc of its own and the interpreter then runs against the same array, so
//! the caller reloads every slot it holds in locals.  See
//! `reports/FN_BOUNDARY_COST.md` §3 and §7.
//!
//! What is deliberately NOT different from the spike: the per-instruction
//! statements are the dispatch path's own case bodies, reused verbatim except
//! for `r[k]` -> `rk` and `pc = ` -> `goto`; flags, guest memory and `fpr[]`
//! stay where they are; no second decoder and no inlining.
//!
//! The dispatcher stays the oracle and the fallback: an unselected entry, an
//! indirect target, an SVC, an unsupported instruction or any construct this
//! module does not model is either refused at generation time (the function is
//! skipped, listed in `FN_EMIT_SKIP`) or left to exit through `L_EXIT` with the
//! pc the statement produced.  With no list, every marker below is the empty
//! string and the generated C is byte-identical to a build without the flags
//! (checked by sha256 in `reports/FN_EMIT_INDIRECT_t_ba4d43cb.md`).

use std::collections::{BTreeMap, BTreeSet};

/// Prefix of every emitted body function (`dq8_fnb_<entry hex>`).
pub const FN_BODY_PREFIX: &str = "dq8_fnb_";

/// C identifier of the emitted body function for `entry`.
pub fn fn_name(entry: u32) -> String {
    format!("{FN_BODY_PREFIX}{entry:08X}")
}

/// Instructions that the emitted body cannot execute itself, so the path ends
/// *before* them and the dispatcher executes exactly that one instruction
/// (opaque exit, t_2fe227ca).  Two families:
///
/// * dispatch-loop locals (`steps`, `context`, …): the body has no such
///   variables, and the dispatcher must run the hook they drive;
/// * host-service instructions (`svc_*`, `guest_unsupported`, `guest_udf`,
///   thread yield/stop): the dispatcher owns the helper, its state and its
///   side effects.
///
/// This is NOT a refusal list any more: a function containing one is still
/// emitted, it just has one exit point there.
const OPAQUE_TOKENS: &[&str] = &[
    "steps",
    "next_schedule_step",
    "next_pump_step",
    "next_progress_step",
    "trace_match",
    "trace_i",
    "context",
    "thread_execution_depth",
    "svc_",
    "guest_unsupported",
    "guest_udf",
    "thread_yield",
    "thread_stop",
    "yield_requested",
];

/// Upper bound on the size of one emitted function, a sanity check on the
/// caller-supplied range (a bad `end` would swallow a whole image region).
const MAX_FN_INSNS: usize = 20000;

/// Backward-edge budget of one body invocation: the body must return to the
/// dispatcher at least this often so the main loop keeps servicing guest
/// threads, HID, the GPU and the deterministic-clock watermark.  The dispatch
/// parts use 256 (`CHAIN_BATCH` in `tools/split_dispatch.py`); matching it
/// keeps the interleaving granularity the same as the interpreter's.
const FN_BUDGET: u32 = 256;

/// Spill the register locals into the shared state.  Used before a native call
/// and before `branch_indirect`, which reads the *live* register file through
/// `active_guest_regs` (the null-callback fallback returns `lr`).
///
/// Since t_21dda9b5 this is only the fallback for the unit tests: the emitter
/// spills exact sets (see [`FnSets`]).
const SPILL_ALL: &str = concat!(
    "st->r[0]=r0; st->r[1]=r1; st->r[2]=r2; st->r[3]=r3; st->r[4]=r4; ",
    "st->r[5]=r5; st->r[6]=r6; st->r[7]=r7; st->r[8]=r8; st->r[9]=r9; ",
    "st->r[10]=r10; st->r[11]=r11; st->r[12]=r12; st->r[13]=r13; st->r[14]=r14;"
);

// ---------------------------------------------------------------------------
// Exact register sets (t_21dda9b5)
// ---------------------------------------------------------------------------
//
// A body keeps r0..r14, the five flags and `running` as C locals.  The old
// boundary spilled/reloaded all 21 slots on *every* native call and every hook
// entry.  That is unnecessary because of one invariant, which this emitter
// maintains:
//
//   I1  at every point of a body, `st` holds the architectural value of every
//       slot the body has NOT written since it was entered.
//
// * The dispatcher hook establishes it on entry: the hook copies the live
//   register file into the state (as it always did) and the body's epilogue
//   stores exactly its own write set, so a slot the body never touched keeps
//   the value the hook put there.
// * A body's prelude therefore only loads the slots it actually touches, and
//   its epilogue only stores the slots it writes.
// * At a native call the caller spills every slot it can have written (its
//   static write set, plus `r14` which the `bl` sets).  It reloads either the
//   callee's write set (callee cannot dispatch) or its own touched set (callee
//   can dispatch, so `st` may have been through the dispatcher).
//
// The spill set is deliberately the whole *static* write set and not a "dirty"
// set accumulated along a linear walk of the address order: a backward edge can
// reach a call site with a slot written at a higher address, so the linear walk
// under-approximates and leaves a live local unspilled.  That was the t_21dda9b5
// fault (a caller in a loop handed the interpreter/callee a stale register).
//
// The correctness trap — a callee that exits to the dispatcher (indirect exit,
// opaque instruction, budget expiry, a call left to the interpreter) publishes
// the architectural state through the epilogue and the dispatcher then runs
// arbitrary guest code against the same array — is why the caller reloads all of
// its own locals after such a call instead of assuming the callee's write set is
// the whole delta.  Nothing is implicit.
//
// Slots are numbered uniformly so one analysis and one set of helpers serve
// both halves of the register file:
//   0..=14  r0..r14
//   15..=19 n, z, c, v, ge
//   20      running
const NSLOT: usize = 21;

const SLOT_NAMES: [&str; NSLOT] = [
    "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "r13",
    "r14", "n", "z", "c", "v", "ge", "running",
];

fn slot_c(i: usize) -> &'static str {
    SLOT_NAMES[i]
}

fn slot_ty(i: usize) -> &'static str {
    if i < 15 {
        "uint32_t"
    } else {
        "int"
    }
}

/// The `Dq8FnState` member a slot is stored in.
fn slot_state(i: usize) -> String {
    if i < 15 {
        format!("st->r[{i}]")
    } else {
        format!("st->{}", slot_c(i))
    }
}

/// `slot = st->...;` for one slot.
fn slot_load(i: usize) -> String {
    format!("{}={};", slot_c(i), slot_state(i))
}

/// `st->... = slot;` for one slot.
fn slot_store(i: usize) -> String {
    format!("{}={};", slot_state(i), slot_c(i))
}

/// C statements that spill (store) a set of slots, one per line.
fn slots_spill(set: &BTreeSet<usize>, indent: &str) -> String {
    let mut out = String::new();
    for &i in set {
        out.push_str(indent);
        out.push_str(&slot_store(i));
        out.push('\n');
    }
    out
}

/// C statements that reload (load) a set of slots, one per line.
fn slots_load(set: &BTreeSet<usize>, indent: &str) -> String {
    let mut out = String::new();
    for &i in set {
        out.push_str(indent);
        out.push_str(&slot_load(i));
        out.push('\n');
    }
    out
}

/// Read/written slot sets of one emitted function, computed from the same
/// per-instruction statements the dispatcher uses (the emitter's own decoder
/// output), never from C text after the fact.
#[derive(Debug, Default, Clone)]
struct FnSets {
    /// Slots read anywhere in the body.  Deliberately not "read before
    /// written": a resume entry can re-enter the body *after* a write, so a
    /// slot read anywhere may need its entry value.
    reads: BTreeSet<usize>,
    /// Slots the body may write on some path.
    writes: BTreeSet<usize>,
}

impl FnSets {
    fn touched(&self) -> BTreeSet<usize> {
        self.reads.union(&self.writes).copied().collect()
    }
}

fn is_ident_byte_ascii(b: u8) -> bool {
    b.is_ascii_alphanumeric() || b == b'_'
}

/// Classify one dispatch statement: which slots it reads and which it writes.
///
/// This walks the statement's C tokens, not a regex over the C file: a `r[k]`
/// reference is a write when an assignment operator follows it (compound
/// operators read as well), and a flag/`running` identifier is a write the
/// same way.  Anything else is a read.  False positives cost one extra
/// load/store and cannot change behaviour; a missed write would, so the
/// operator set is closed (the emitters only produce `=`, `+=`, `-=`, `*=`,
/// `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=` on a register local).
fn scan_slots(stmt: &str, sets: &mut FnSets) {
    let bytes = stmt.as_bytes();
    let mut i = 0usize;
    while i < bytes.len() {
        let b = bytes[i];
        if !(b.is_ascii_alphabetic() || b == b'_') || (i > 0 && is_ident_byte_ascii(bytes[i - 1])) {
            i += 1;
            continue;
        }
        // `r[<digits>]`
        if b == b'r' && i + 1 < bytes.len() && bytes[i + 1] == b'[' {
            let mut j = i + 2;
            while j < bytes.len() && bytes[j].is_ascii_digit() {
                j += 1;
            }
            if j > i + 2 && j < bytes.len() && bytes[j] == b']' {
                if let Ok(idx) = stmt[i + 2..j].parse::<usize>() {
                    if idx < 15 {
                        classify_after(stmt, j + 1, idx, sets);
                    }
                }
                i = j + 1;
                continue;
            }
        }
        // flag / running identifier
        let mut j = i;
        while j < bytes.len() && is_ident_byte_ascii(bytes[j]) {
            j += 1;
        }
        let word = &stmt[i..j];
        if let Some(k) = SLOT_NAMES[15..].iter().position(|w| *w == word) {
            classify_after(stmt, j, 15 + k, sets);
        }
        i = j;
    }
}

/// After a slot reference ending at byte `at`: an assignment operator makes it
/// a write (a compound one reads too), anything else a read.
fn classify_after(stmt: &str, at: usize, slot: usize, sets: &mut FnSets) {
    let rest = stmt[at..].trim_start();
    let compound = ["+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="];
    if rest.starts_with("==") || rest.starts_with("!=") {
        sets.reads.insert(slot);
        return;
    }
    if rest.starts_with('=') {
        sets.writes.insert(slot);
        return;
    }
    if compound.iter().any(|op| rest.starts_with(op)) {
        sets.writes.insert(slot);
        sets.reads.insert(slot);
        return;
    }
    sets.reads.insert(slot);
}

/// `DQ8_FN_EXACT=0` (generation time) forces the whole-file spill/reload sets —
/// the dq8-b30 behaviour, kept as the A/B control for this card.
fn exact_sets_enabled() -> bool {
    match std::env::var("DQ8_FN_EXACT") {
        Ok(v) => !(v.is_empty() || v == "0"),
        Err(_) => true,
    }
}

/// `DQ8_FN_RELOAD=callee` (generation time) is a *diagnostic* control arm: it
/// keeps the conservative spill but reloads only the callee's write set even
/// when the callee can dispatch, which is the t_21dda9b5 assumption.  Used to
/// attribute a fault to one half of the boundary; not a supported mode.
fn reload_callee_only() -> bool {
    matches!(std::env::var("DQ8_FN_RELOAD").as_deref(), Ok("callee"))
}

/// The read/written sets of one planned function.
fn fn_sets(plan: &Plan, emitted: &BTreeMap<u32, String>) -> FnSets {
    let mut sets = FnSets::default();
    if !exact_sets_enabled() {
        // A/B control (t_21dda9b5): dq8-b30's boundary was "spill everything
        // always".  DQ8_FN_EXACT=0 at *generation* time reproduces exactly
        // that traffic (every body loads and stores all 21 slots, every call
        // site spills and reloads all of them) while keeping the new
        // instrumentation, so the two emitters can be measured on one harness.
        sets.reads.extend(0..NSLOT);
        sets.writes.extend(0..NSLOT);
        return sets;
    }
    for addr in &plan.addrs {
        let stmt = &emitted[addr];
        // An opaque instruction is executed by the dispatcher, never by the
        // body.  The shims hand it the *whole* register array (`svc_x(r)`), so
        // which slots it writes is not statically known: such a body keeps the
        // whole-file sets (as dq8-b30 had) — 6 of the 300 functions have an
        // opaque instruction, so the exact-set win is untouched elsewhere.
        // Getting this wrong would be silent: an SVC that returns a value in
        // r0 must be visible in `st` when the body exits, because the caller
        // only reloads the slots the callee declares.
        if OPAQUE_TOKENS.iter().any(|t| stmt.contains(t)) {
            sets.reads.extend(0..NSLOT);
            sets.writes.extend(0..NSLOT);
            return sets;
        }
        scan_slots(stmt, &mut sets);
    }
    sets
}

/// Does one invocation of this body stay inside the body when it returns to its
/// C caller?
///
/// This is the classification the call sites need (t_40d37ebb).  A caller may
/// only assume `st` is the complete architectural state after a native call if
/// the callee *returned* to it: `pc == ret` and the register file in `st` is the
/// guest's at `ret`.  That holds when the callee never hands control to the
/// dispatcher, because then its whole execution was C code that publishes
/// exactly its write set.  It does **not** hold when the callee leaves through
/// `L_EXIT` with a pc of its own, because the dispatcher then runs guest code
/// against the same `r[]` array `st->r` points at, and the caller would resume
/// from locals that never saw those writes.
///
/// The emitter already knows every way out, so this is metadata, not a new
/// analysis:
///
/// * an **opaque exit** (SVC / undecodable / thread / dispatch-loop local) —
///   the dispatcher executes that instruction;
/// * an **indirect exit** (`pc = <expression>`, incl. `bx`, `ldr pc`, a table
///   load) — the dispatcher validates and takes the transfer;
/// * a **backward edge** — the `_fn_budget` check can expire and take `L_EXIT`;
/// * a **call left to the dispatcher** (target not emitted) — the callee is
///   interpreted, so control leaves the native path by definition;
/// * a **`running` clobber** — the body leaves when the guest stops.
///
/// A plain return (`pc = r14` -> `L_EXIT`) is not a dispatch: it is the callee
/// giving control back, with `st` complete.
fn fn_may_dispatch(plan: &Plan, emitted: &BTreeMap<u32, String>, accepted: &BTreeSet<u32>) -> bool {
    for &addr in &plan.addrs {
        let stmt = &emitted[&addr];
        if let Some((_, target)) = simple_call(stmt) {
            if !accepted.contains(&target) {
                return true;
            }
            continue;
        }
        let rw = match rewrite(addr, plan.entry, plan.end, stmt, "") {
            Ok(rw) => rw,
            // The emit pass will refuse the function anyway; be conservative.
            Err(_) => return true,
        };
        if rw.opaque || rw.indirect || rw.touches_running || !rw.resume.is_empty() {
            return true;
        }
    }
    false
}


/// The slots a body must have in locals.  `touched()` plus `running`: the
/// post-call resume check reads `running`, and a body whose statements never
/// mention it would otherwise test an uninitialised local (t_40d37ebb).
fn body_load_set(sets: &FnSets) -> BTreeSet<usize> {
    let mut load = sets.touched();
    load.insert(20);
    load
}

/// Spill statements on one line (used inline before a `branch_indirect`).
fn slots_spill_compact(set: &BTreeSet<usize>) -> String {
    set.iter()
        .map(|&i| slot_store(i))
        .collect::<Vec<_>>()
        .join(" ")
}

/// One emitted function.
#[derive(Debug)]
pub struct EmittedFn {
    /// Entry PC (the value the runtime gate and the hook match).
    pub entry: u32,
    /// One past the last instruction of the range.
    pub end: u32,
    /// Number of guest instructions in the range.
    pub insn_count: usize,
    /// Guest `bl` sites in the range.
    pub calls: usize,
    /// `bl` sites whose target is another emitted function (real C call).
    pub calls_selected: usize,
    /// `bl` sites left to the dispatcher.
    pub calls_dispatcher: usize,
    /// Instructions that end the native path and hand one step to the
    /// dispatcher (indirect transfers + opaque instructions).
    pub dispatcher_exits: usize,
    /// Indirect transfers in the range (`bx`/`blx`/`ldr pc`/table load).
    pub indirect_exits: usize,
    /// Opaque instructions in the range (SVC / undecodable / thread).
    pub opaque_exits: usize,
    /// The body can hand control to the dispatcher in the middle of an
    /// invocation (indirect/opaque exit, backward edge, dispatcher call,
    /// `running` clobber): a caller may not assume `st` survived the call.
    pub may_dispatch: bool,
    /// Native call sites of this body whose callee cannot dispatch (exact
    /// reload).
    pub call_sites_exact: usize,
    /// Native call sites of this body whose callee can dispatch (full reload).
    pub call_sites_full: usize,
    /// Return addresses that need a resume entry (calls left to the dispatcher,
    /// returns of native calls that may unwind through the dispatcher, the
    /// successors of an opaque exit and loop heads a budget exit can land on).
    pub resume_points: Vec<u32>,
}

/// A range that was dropped, with the reason (printed as `FN_EMIT_SKIP`).
#[derive(Debug)]
pub struct SkippedFn {
    pub entry: u32,
    pub end: u32,
    pub reason: String,
}

/// The whole emitted program: every marker body `main.rs` splices into the
/// generated C, plus the shard TUs the splitter cuts out.
#[derive(Debug, Default)]
pub struct Program {
    /// `FN_EMIT_DECLS`: typedef + prototypes, moved into `dq8_rt.h` by the
    /// splitter so `dq8_main.c` and every shard see the same declarations.
    pub decls_c: String,
    /// `FN_EMIT_DEF`: per-function counters, report and runtime gate
    /// (dq8_main.c).
    pub def_c: String,
    /// `FN_EMIT_HOOK`: `switch (pc)` cases in the dispatch loop.
    pub hook_c: String,
    /// `FN_EMIT_GATE`: the call that parses the runtime gate in `main()`.
    pub gate_c: String,
    /// One shard TU body per emitted function, in list order.
    pub shards: Vec<String>,
    pub funcs: Vec<EmittedFn>,
    pub skipped: Vec<SkippedFn>,
    /// Every PC the dispatch loop must observe (entries + resume entries).
    pub hook_pcs: Vec<u32>,
}

/// Apply the same checked-memory fast-path substitution `tools/split_dispatch.py`
/// applies to every guest `case` body (`mem32(` -> `dq8_mem32_fast(`, …), so the
/// emitted function pays exactly the same per-access cost as the dispatch path
/// it is compared against.  The emitters' statements are the pre-split text,
/// which still calls the slow wrappers.
fn fast_memory(stmt: &str) -> String {
    let mut out = stmt.to_string();
    for helper in ["mem8", "mem16", "mem32", "put8", "put16", "put32"] {
        let mut next = String::with_capacity(out.len());
        let needle = format!("{helper}(");
        let mut rest: &str = &out;
        while let Some(pos) = rest.find(&needle) {
            // Word boundary: `dq8_mem32_fast(` must not match a plain `mem32(`.
            let at_word = pos == 0
                || !(rest.as_bytes()[pos - 1].is_ascii_alphanumeric()
                    || rest.as_bytes()[pos - 1] == b'_');
            next.push_str(&rest[..pos]);
            if at_word {
                next.push_str(&format!("dq8_{helper}_fast("));
            } else {
                next.push_str(&needle);
            }
            rest = &rest[pos + needle.len()..];
        }
        next.push_str(rest);
        out = next;
    }
    out
}

fn is_ident_byte(b: u8) -> bool {
    b.is_ascii_alphanumeric() || b == b'_'
}

/// Whitespace-free copy of a statement (used to match the `bl` shape).
fn squeeze(stmt: &str) -> String {
    stmt.chars().filter(|c| !c.is_whitespace()).collect()
}

/// Parse `0x<hex>u;` at the start of `s`, returning the value and the rest.
fn parse_hex_u(s: &str) -> Option<(u32, &str)> {
    let body = s.strip_prefix("0x").or_else(|| s.strip_prefix("0X"))?;
    let end = body.find(|c: char| !c.is_ascii_hexdigit())?;
    if end == 0 {
        return None;
    }
    let value = u32::from_str_radix(&body[..end], 16).ok()?;
    let rest = body[end..].strip_prefix('u')?;
    let rest = rest.strip_prefix(';')?;
    Some((value, rest))
}

/// `0x<hex>u?` (no trailing `;`).
fn parse_hex_const(s: &str) -> Option<u32> {
    let body = s.strip_prefix("0x").or_else(|| s.strip_prefix("0X"))?;
    let digits = body.strip_suffix('u').unwrap_or(body);
    if digits.is_empty() || !digits.chars().all(|c| c.is_ascii_hexdigit()) {
        return None;
    }
    u32::from_str_radix(digits, 16).ok()
}

/// Recognise a statement that is *exactly* a guest `bl`/`blx` immediate:
/// `{ r[14] = 0xRETu; pc = 0xTARGETu; break; }` (the frontend's own encoding,
/// either brace layout).  Returns `(ret, target)`.
///
/// Anything with an extra statement, a conditional, or a different pc source
/// is *not* a call for this emitter; it is left to the generic rewrite, which
/// keeps it as a pc transfer (still correct, just not a native call).
fn simple_call(stmt: &str) -> Option<(u32, u32)> {
    let mut t = squeeze(stmt);
    for _ in 0..2 {
        if let Some(x) = t.strip_prefix('{') {
            t = x.to_string();
        }
        if let Some(x) = t.strip_suffix('}') {
            t = x.to_string();
        }
    }
    if let Some(x) = t.strip_suffix("break;") {
        t = x.to_string();
    }
    while t.ends_with('}') {
        t.pop();
    }
    let (ret, rest) = parse_hex_u(t.strip_prefix("r[14]=")?)?;
    let (target, rest) = parse_hex_u(rest.strip_prefix("pc=")?)?;
    if !rest.is_empty() {
        return None;
    }
    Some((ret, target))
}

/// True when nothing but block closers and `break;` follows an assignment, so
/// the instruction cannot do anything else after setting pc.
fn tail_is_empty(rest: &str) -> bool {
    let mut s: String = rest.chars().filter(|c| !c.is_whitespace()).collect();
    loop {
        if let Some(x) = s.strip_suffix('}') {
            s = x.to_string();
            continue;
        }
        if let Some(x) = s.strip_suffix("break;") {
            s = x.to_string();
            continue;
        }
        if let Some(x) = s.strip_suffix(';') {
            s = x.to_string();
            continue;
        }
        break;
    }
    s.is_empty()
}

/// Rename `r[k]` -> `rk` inside an expression (the pc-assignment RHS).
/// `r[15]` (guest pc) and a bare `r` (the dispatch register array) cannot be
/// reproduced from locals: those statements become opaque exits.
fn rename_expr(expr: &str) -> Result<String, ()> {
    let bytes = expr.as_bytes();
    let mut out = String::with_capacity(expr.len());
    let mut i = 0usize;
    while i < bytes.len() {
        if bytes[i] == b'r'
            && (i == 0 || !is_ident_byte(bytes[i - 1]))
            && i + 1 < bytes.len()
            && bytes[i + 1] == b'['
        {
            let mut j = i + 2;
            while j < bytes.len() && bytes[j].is_ascii_digit() {
                j += 1;
            }
            if j == i + 2 || j >= bytes.len() || bytes[j] != b']' {
                return Err(());
            }
            let idx: u32 = expr[i + 2..j].parse().map_err(|_| ())?;
            if idx > 14 {
                return Err(());
            }
            out.push_str(&format!("r{idx}"));
            i = j + 1;
            continue;
        }
        if bytes[i] == b'r'
            && (i == 0 || !is_ident_byte(bytes[i - 1]))
            && (i + 1 == bytes.len() || !is_ident_byte(bytes[i + 1]))
        {
            return Err(()); // bare `r`: the dispatch register array
        }
        out.push(bytes[i] as char);
        i += 1;
    }
    Ok(out)
}

/// One exit site of a statement: the statement set `_fn_ex = flag`, and the
/// target the dispatcher must continue at.  `label = Some(pc)` is a target
/// inside the function (direct `goto`); `None` means "pc already holds the
/// target, leave through L_EXIT".
#[derive(Debug)]
struct ExitSite {
    flag: u32,
    label: Option<u32>,
}

/// Result of rewriting one dispatch statement into the emitted function's body.
#[derive(Debug, Default)]
struct Rewritten {
    c: String,
    exits: Vec<ExitSite>,
    /// The statement can clear `running` and needs the post-check.
    touches_running: bool,
    /// The dispatcher must execute this instruction (opaque exit).
    opaque: bool,
    /// The statement performs an indirect pc transfer.
    indirect: bool,
    /// In-range loop heads a budget exit can land on (need resume entries).
    resume: Vec<u32>,
}

/// Rewrite one dispatch statement so that it runs against the emitted
/// function's register locals and its `pc` assignments become exits.
///
/// `addr` is the guest PC of the statement (used for the budget check and the
/// `dq8_fn_branch_indirect` site), `entry`/`end` the emitted range, and
/// `spill` the C statements that materialise the live register file into `st`
/// before a `branch_indirect()` call (exact set, see [`FnSets`]).
fn rewrite(addr: u32, entry: u32, end: u32, stmt: &str, spill: &str) -> Result<Rewritten, String> {
    if OPAQUE_TOKENS.iter().any(|t| stmt.contains(t)) {
        return Ok(Rewritten {
            opaque: true,
            touches_running: stmt.contains("running"),
            ..Default::default()
        });
    }
    let bytes = stmt.as_bytes();
    let mut out = String::with_capacity(stmt.len() + 32);
    let mut i = 0usize;
    let mut pc_assigned = false;
    let mut rw = Rewritten::default();
    let mut flag = 0u32;

    // Emit the "leave through L_EXIT" / direct-goto form of an exit.
    // A backward edge pays a budget check so one invocation cannot monopolise
    // the dispatch loop (thread/HID/GPU servicing, bench step limit).
    let mut emit_jump = |rw: &mut Rewritten, out: &mut String, target: Option<u32>| {
        match target {
            Some(t) if t <= addr => {
                if !rw.resume.contains(&t) {
                    rw.resume.push(t);
                }
                out.push_str(&format!(
                    "{{ if (--_fn_budget == 0) {{ pc = 0x{t:08X}u; goto L_EXIT; }} goto L_{t:08X}; }}"
                ));
            }
            Some(t) => out.push_str(&format!("goto L_{t:08X};")),
            None => out.push_str("goto L_EXIT;"),
        }
    };

    while i < bytes.len() {
        // `r[k]` -> `rk` (only a real guest register reference; `fpr[16]` and
        // `dq8_mem32_fast(` must not match).
        if bytes[i] == b'r'
            && i + 1 < bytes.len()
            && bytes[i + 1] == b'['
            && (i == 0 || !is_ident_byte(bytes[i - 1]))
        {
            let mut j = i + 2;
            while j < bytes.len() && bytes[j].is_ascii_digit() {
                j += 1;
            }
            if j == i + 2 || j >= bytes.len() || bytes[j] != b']' {
                return Err(format!("malformed register reference in statement: {stmt}"));
            }
            let idx: u32 = stmt[i + 2..j]
                .parse()
                .map_err(|_| format!("malformed register index in statement: {stmt}"))?;
            if idx > 14 {
                // r[15] is the guest pc inside the dispatch loop; there is no
                // such local here, so the dispatcher has to run the statement.
                return Ok(Rewritten {
                    opaque: true,
                    touches_running: stmt.contains("running"),
                    ..Default::default()
                });
            }
            out.push_str(&format!("r{idx}"));
            i = j + 1;
            continue;
        }
        // A bare `r` is the dispatch register array (`svc_x(r)`): opaque.
        if bytes[i] == b'r'
            && (i == 0 || !is_ident_byte(bytes[i - 1]))
            && (i + 1 == bytes.len() || !is_ident_byte(bytes[i + 1]))
            && !(i + 1 < bytes.len() && bytes[i + 1] == b'[')
        {
            return Ok(Rewritten {
                opaque: true,
                touches_running: stmt.contains("running"),
                ..Default::default()
            });
        }
        // A `pc` token is either the left-hand side of an assignment or a read
        // of the value just assigned (`if (pc == 0xFFFFFFFFu)`, the pop-return
        // idiom).  A read *before* any assignment is the pipeline value
        // (adr / pc-relative load) and cannot be reproduced from a local.
        if bytes[i] == b'p'
            && i + 1 < bytes.len()
            && bytes[i + 1] == b'c'
            && (i == 0 || !is_ident_byte(bytes[i - 1]))
            && (i + 2 == bytes.len() || !is_ident_byte(bytes[i + 2]))
        {
            let after = &stmt[i + 2..];
            let after_trimmed = after.trim_start();
            let assign = after_trimmed.starts_with('=') && !after_trimmed.starts_with("==");
            if !assign {
                if !pc_assigned {
                    return Ok(Rewritten {
                        opaque: true,
                        touches_running: stmt.contains("running"),
                        ..Default::default()
                    });
                }
                out.push_str("pc");
                i += 2;
                continue;
            }
            let after_eq = &after_trimmed[1..];
            let rhs_start = after_eq.trim_start();
            let semi = rhs_start
                .find(';')
                .ok_or_else(|| format!("unterminated pc assignment in statement: {stmt}"))?;
            let rhs = rhs_start[..semi].trim();
            // Byte offset just past the closing ';'.
            let past = i + 2 + (after.len() - rhs_start.len()) + semi + 1;
            pc_assigned = true;

            let tail_empty = tail_is_empty(&stmt[past..]);
            let mut needs_spill = false;
            let (text, target, indirect) = if let Some(t) = parse_hex_const(rhs) {
                (
                    format!("pc = 0x{t:08X}u;"),
                    if t >= entry && t < end { Some(t) } else { None },
                    false,
                )
            } else if rhs.replace(' ', "") == "r[14]" {
                ("pc = r14;".to_string(), None, false)
            } else {
                let expr = rename_expr(rhs).map_err(|_| {
                    format!("pc source `{rhs}` references the dispatch register array or r[15]")
                })?;
                let expr = if expr.contains("branch_indirect(") {
                    // The dispatcher's own validator (null-callback and
                    // out-of-range handling, diagnostics).  It reads the live
                    // register file through active_guest_regs, so spill first.
                    needs_spill = true;
                    expr.replace(
                        "branch_indirect(",
                        &format!("dq8_fn_branch_indirect(st, 0x{addr:08X}u, "),
                    )
                } else {
                    expr
                };
                (format!("pc = {expr};"), None, true)
            };
            if indirect {
                rw.indirect = true;
            }
            if tail_empty {
                if indirect {
                    if needs_spill {
                        out.push_str(&format!("{{ {spill} {text} goto L_EXIT; }}"));
                    } else {
                        out.push_str(&format!("{{ {text} goto L_EXIT; }}"));
                    }
                } else {
                    out.push_str("{ ");
                    out.push_str(&text);
                    out.push(' ');
                    emit_jump(&mut rw, &mut out, target);
                    out.push_str(" }");
                }
            } else {
                flag += 1;
                out.push_str("{ ");
                if needs_spill {
                    out.push_str(spill);
                    out.push(' ');
                }
                out.push_str(&text);
                out.push_str(&format!(" _fn_ex = {flag}; }}"));
                rw.exits.push(ExitSite { flag, label: target });
            }
            i = past;
            continue;
        }
        out.push(bytes[i] as char);
        i += 1;
    }
    rw.c = out;
    rw.touches_running = stmt.contains("running");
    Ok(rw)
}

/// Const pc successors of a statement (used to place resume entries after an
/// opaque exit, so the body resumes where the dispatcher lands).
fn const_successors(stmt: &str, entry: u32, end: u32) -> Vec<u32> {
    let mut v = Vec::new();
    let bytes = stmt.as_bytes();
    let mut i = 0usize;
    while i + 1 < bytes.len() {
        if bytes[i] == b'p'
            && bytes[i + 1] == b'c'
            && (i == 0 || !is_ident_byte(bytes[i - 1]))
            && (i + 2 == bytes.len() || !is_ident_byte(bytes[i + 2]))
        {
            let after = stmt[i + 2..].trim_start();
            if let Some(rest) = after.strip_prefix('=') {
                if !rest.starts_with('=') {
                    let rest = rest.trim_start();
                    if let Some(semi) = rest.find(';') {
                        if let Some(t) = parse_hex_const(rest[..semi].trim()) {
                            if t >= entry && t < end {
                                v.push(t);
                            }
                        }
                    }
                }
            }
        }
        i += 1;
    }
    v.sort_unstable();
    v.dedup();
    v
}

/// One planned function, before the cross-call pass.
struct Plan {
    entry: u32,
    end: u32,
    addrs: Vec<u32>,
}

fn stride(addr: u32) -> u32 {
    if addr & 1 != 0 {
        2
    } else {
        4
    }
}

/// Validate one `(entry, end)` range against the frontend's statement map and
/// return either a plan or the reason it cannot be emitted.
///
/// Since t_ba4d43cb this is deliberately permissive: an indirect transfer or an
/// instruction the body cannot run is an *exit*, not a refusal.  What is still
/// refused is what the emitted body cannot represent at all — a range with a
/// hole (data island inside the extent), a main-loop boundary pc (its hook must
/// run), an entry that needs the dispatcher itself (re-entering the body there
/// would make no progress), a last instruction that does not terminate (the
/// body would fall out of its own code), and a statement whose expression
/// references `r[15]`/the register array.
fn plan_fn(
    entry: u32,
    end: u32,
    emitted: &BTreeMap<u32, String>,
    boundary_pcs: &BTreeSet<u32>,
) -> Result<Plan, String> {
    if entry & 2 != 0 {
        return Err("entry is not ARM (4-byte) or Thumb (odd) aligned".to_string());
    }
    if end <= entry {
        return Err("end is not above the entry".to_string());
    }
    if ((end - entry) / 2) as usize > MAX_FN_INSNS {
        return Err(format!(
            "range holds more than {MAX_FN_INSNS} instructions (bad boundary?)"
        ));
    }
    let addrs: Vec<u32> = emitted.range(entry..end).map(|(a, _)| *a).collect();
    if addrs.is_empty() || addrs[0] != entry {
        return Err(format!("no emitted dispatch case at the entry 0x{entry:08X}"));
    }
    // Contiguity: the range must be one instruction stream (no holes), so the
    // emitted labels and their fall-through order match the guest exactly.
    let mut expect = entry;
    for a in &addrs {
        if *a != expect {
            return Err(format!(
                "hole at 0x{expect:08X} (next emitted instruction is 0x{a:08X}); mixed \
                 ARM/Thumb range or a data island"
            ));
        }
        expect = a + stride(*a);
    }
    if expect != end {
        return Err(format!(
            "end 0x{end:08X} does not follow the last instruction (expected 0x{expect:08X})"
        ));
    }
    let mut last_leaves = false;
    for a in &addrs {
        let stmt = &emitted[a];
        if boundary_pcs.contains(a) || boundary_pcs.contains(&(a & !1)) {
            return Err(format!(
                "instruction 0x{a:08X} is a main-loop boundary pc (its hook must run)"
            ));
        }
        // Same rewrite the emit pass will run, so nothing is planned that the
        // body cannot express.
        let rw = rewrite(*a, entry, end, stmt, "")?;
        if *a == entry && rw.opaque {
            return Err(format!(
                "entry instruction 0x{entry:08X} needs the dispatcher (no progress possible)"
            ));
        }
        if *a == *addrs.last().unwrap() {
            last_leaves = rw.opaque || stmt_terminates(stmt);
        }
    }
    if !last_leaves {
        return Err(format!(
            "last instruction 0x{:08X} does not leave the body (falls out of the range)",
            addrs.last().unwrap()
        ));
    }
    Ok(Plan { entry, end, addrs })
}

/// Does this statement always leave the body (ret / const / indirect / opaque)?
fn stmt_terminates(stmt: &str) -> bool {
    if OPAQUE_TOKENS.iter().any(|t| stmt.contains(t)) {
        return true;
    }
    stmt.contains("pc = r[14]") || {
        let mut found = false;
        let bytes = stmt.as_bytes();
        let mut i = 0usize;
        while i + 1 < bytes.len() {
            if bytes[i] == b'p'
                && bytes[i + 1] == b'c'
                && (i == 0 || !is_ident_byte(bytes[i - 1]))
                && (i + 2 == bytes.len() || !is_ident_byte(bytes[i + 2]))
            {
                let after = stmt[i + 2..].trim_start();
                if let Some(rest) = after.strip_prefix('=') {
                    if !rest.starts_with('=') {
                        found = true;
                    }
                }
            }
            i += 1;
        }
        found
    }
}

/// Plan + emit the whole list.  Functions that cannot be expressed are
/// returned in `Program::skipped` with the reason; every other one is emitted.
pub fn emit_program(
    list: &[(u32, u32)],
    emitted: &BTreeMap<u32, String>,
    fast_mem: bool,
    boundary_pcs: &BTreeSet<u32>,
) -> Program {
    let mut prog = Program::default();

    // ---- pass 1: validate every range, drop the ones this emitter cannot
    // express.  Duplicate entries keep the first range.
    let mut seen_entries = BTreeSet::new();
    let mut plans: Vec<Plan> = Vec::new();
    for &(entry, end) in list {
        if !seen_entries.insert(entry) {
            continue;
        }
        match plan_fn(entry, end, emitted, boundary_pcs) {
            Ok(p) => plans.push(p),
            Err(reason) => prog.skipped.push(SkippedFn { entry, end, reason }),
        }
    }
    let accepted: BTreeSet<u32> = plans.iter().map(|p| p.entry).collect();

    // ---- pass 1b: exact read/write sets of every function, from the same
    // per-instruction statements its body is built from.  A call site is
    // emitted from the *callee's* write set, so this has to be complete before
    // any body is emitted.
    let mut sets_by_entry: BTreeMap<u32, FnSets> = BTreeMap::new();
    for plan in &plans {
        sets_by_entry.insert(plan.entry, fn_sets(plan, emitted));
    }
    let empty_sets = FnSets::default();
    let sets_of = |entry: u32| -> FnSets {
        sets_by_entry
            .get(&entry)
            .cloned()
            .unwrap_or_else(|| empty_sets.clone())
    };

    // ---- pass 1c: which bodies can hand control to the dispatcher mid-call?
    // A call site whose callee can is emitted with the conservative boundary
    // (spill the caller's live registers, reload them after the call); a call
    // site whose callee cannot keeps the exact sets, because that callee runs
    // straight through and publishes exactly its write set (t_40d37ebb).
    let mut may_dispatch: BTreeSet<u32> = BTreeSet::new();
    for plan in &plans {
        if fn_may_dispatch(plan, emitted, &accepted) {
            may_dispatch.insert(plan.entry);
        }
    }

    // ---- pass 2: emit the bodies.  A call whose target is in `accepted`
    // becomes a real C call; every other call stays a dispatcher transfer and
    // contributes a resume entry at its return address.
    let mut pending_cases: Vec<(u32, u32, usize)> = Vec::new();
    let mut decls = String::new();
    let mut hook_pcs: Vec<u32> = Vec::new();
    // DQ8_FN_STATS: static boundary-traffic accounting, one entry per emitted
    // function (prelude load ops, epilogue store ops) and per native call site
    // (spill stores, reload loads).
    let mut fn_prelude_ops: Vec<usize> = Vec::new();
    let mut fn_epilogue_ops: Vec<usize> = Vec::new();
    let mut site_spill_ops: Vec<usize> = Vec::new();
    let mut site_reload_ops: Vec<usize> = Vec::new();
    // 1 = the callee can dispatch (conservative reload), 0 = exact reload.
    let mut site_full: Vec<bool> = Vec::new();

    decls.push_str(
        r#"/* FN_EMIT (t_21dda9b5): register/flag state shared by the native guest
 * functions.  A body keeps r0..r14, the flags and `running` as C locals for the
 * whole function.  The locals are loaded once on entry and stored once on
 * exit, and a native call spills every slot the caller can have written and
 * reloads the callee's write set, or all of its own locals when the callee can
 * hand control to the dispatcher: every other slot in this struct is already
 * architecturally correct, because nothing has written it since it was copied
 * in. */
typedef struct {
    uint32_t *r;            /* the dispatcher's own register array: it IS the
                             * architectural file, the body only caches it in
                             * C locals and flushes what it changed */
    uint32_t pc;
    int n, z, c, v, ge;
    int running;
} Dq8FnState;
/* Indirect-target validation for an emitted body: the dispatcher's own
 * branch_indirect(), run with the body's live register file so its
 * null-callback fallback returns the right lr and its diagnostics name the
 * right site.  site is the guest pc of the bx/blx/ldr-pc. */
uint32_t dq8_fn_branch_indirect(Dq8FnState *st, uint32_t site, uint32_t target);
/* DQ8_FN_STATS (t_21dda9b5): one counter per native call site; the definition
 * and the static per-site / per-function op tables live in the runtime TU. */
extern unsigned long long dq8_fn_sc[];
extern int dq8_fn_stats_on;
/* t_40d37ebb diagnosis: per-call-boundary register trace (DQ8_FN_TRACE_CALLS). */
extern int dq8_fn_tr;
"#,
    );

    for (idx, plan) in plans.iter().enumerate() {
        let entry = plan.entry;
        let end = plan.end;
        let name = fn_name(entry);
        let fsets = sets_of(entry);

        let mut body = String::new();
        let mut calls = 0usize;
        let mut calls_selected = 0usize;
        let mut calls_dispatcher = 0usize;
        let mut indirect_exits = 0usize;
        let mut opaque_exits = 0usize;
        let mut resume_points: Vec<u32> = Vec::new();
        let mut resume_seen: BTreeSet<u32> = BTreeSet::new();
        let mut failed: Option<String> = None;
        // The slots whose C local can differ from `st` at a call site: the slots
        // this body writes *anywhere* (t_40d37ebb).  The t_21dda9b5 emitter
        // tracked a "dirty" set with a linear walk of the address order, which
        // under-approximates it as soon as a backward edge reaches the site with
        // a slot written at a higher address: the slot is then live in a local,
        // was never spilled, and the callee (or the interpreter) reads the stale
        // array value.  A superset costs one store; a miss corrupts the guest,
        // so the whole function's write set is the spill set.
        let caller_writes = fsets.writes.clone();
        // Locals the body has (see body_load_set: touched + `running`), and the
        // slots its epilogue publishes.
        let load_set = body_load_set(&fsets);
        let store_set = fsets.writes.clone();
        let mut call_sites_exact = 0usize;
        let mut call_sites_full = 0usize;

        for &addr in &plan.addrs {
            let stmt = &emitted[&addr];
            let stmt_for_body = if fast_mem {
                fast_memory(stmt)
            } else {
                stmt.clone()
            };
            // Before a branch_indirect() call the dispatcher's validator reads
            // the live file through st->r, so every slot this body can have
            // written has to be in `st` first.  `r14` is included only when this
            // body actually loaded it (i.e. it touches r14): spilling a local
            // that was never loaded would publish garbage over the caller's lr,
            // and the array value is already correct when r14 is not touched.
            let mut spill_set = caller_writes.clone();
            if load_set.contains(&14) {
                spill_set.insert(14);
            }
            let spill = slots_spill_compact(&spill_set);
            body.push_str(&format!("L_{addr:08X}:\n"));
            if let Some((ret, target)) = simple_call(stmt) {
                calls += 1;
                if accepted.contains(&target) {
                    calls_selected += 1;
                    let callee = fn_name(target);
                    let csets = sets_of(target);
                    // The boundary (t_40d37ebb): the caller spills the slots
                    // whose local can differ from `st` — its whole static write
                    // set, plus r14, which the guest `bl` sets before the call.
                    //
                    // `r14 = ret` is the other half of the guest `bl`: a callee
                    // that returns through `lr` (a leaf that does not pop pc from
                    // its own frame) must see the return address in r14, exactly
                    // as the interpreter would.
                    //
                    // What the call may *assume* afterwards depends on the
                    // callee:
                    //
                    // * callee cannot dispatch (hot leaves): it ran entirely in
                    //   C, so `st` holds exactly the caller's spill plus the
                    //   callee's writes and nothing else moved.  Reload only the
                    //   callee's write set — the exact-set win.
                    // * callee can dispatch: it may leave through `L_EXIT` with
                    //   a pc of its own, the interpreter then runs against the
                    //   same r[] array, and control can come back to this
                    //   caller at a resume point.  Never assume `st` survived a
                    //   side trip: reload everything this body holds in locals
                    //   (its touched set), i.e. the dq8-b30 boundary for the
                    //   reload half.
                    let mut call_spill = if exact_sets_enabled() {
                        caller_writes.clone()
                    } else {
                        // A/B control: dq8-b30 spilled every slot at every call.
                        (0..NSLOT).collect()
                    };
                    call_spill.insert(14);
                    let callee_dispatches = may_dispatch.contains(&target);
                    let mut reload_set = if callee_dispatches && !reload_callee_only() {
                        fsets.touched()
                    } else {
                        csets.writes.clone()
                    };
                    // `running` is re-read by the resume check below, so it has
                    // to be current in the local too.
                    reload_set.insert(20);
                    if callee_dispatches {
                        call_sites_full += 1;
                    } else {
                        call_sites_exact += 1;
                    }
                    let site = site_spill_ops.len();
                    site_spill_ops.push(call_spill.len());
                    site_reload_ops.push(reload_set.len());
                    site_full.push(callee_dispatches);
                    body.push_str("    ++exec;\n");
                    body.push_str(&format!("    r14 = 0x{ret:08X}u;\n"));
                    body.push_str(&format!(
                        "    if (dq8_fn_tr) {{ unsigned _i; fprintf(stderr, \"FNCALL fn=0x{addr:08X} to=0x{target:08X} st=\"); for (_i = 0; _i < 15u; ++_i) fprintf(stderr, \"%08X,\", st->r[_i]); fprintf(stderr, \"%d%d%d%d%d,%d\\n\", st->n, st->z, st->c, st->v, st->ge, st->running); }}\n"
                    ));
                    body.push_str(&slots_spill(&call_spill, "    "));
                    body.push_str(&format!("    st->pc = 0x{ret:08X}u;\n"));
                    body.push_str(&format!(
                        "    if (dq8_fn_stats_on) ++dq8_fn_sc[{site}];\n"
                    ));
                    body.push_str(&format!("    exec += {callee}(st, 0u);\n"));
                    body.push_str("    pc = st->pc;\n");
                    body.push_str(&slots_load(&reload_set, "    "));
                    body.push_str(&format!(
                        "    if (dq8_fn_tr) fprintf(stderr, \"FNRET  from=0x{addr:08X} to=0x{target:08X} r12=%08X st12=%08X r12s=%08X pc=%08X sp=%08X\\n\", r12, st->r[12], st->r[12], pc, r13);\n"
                    ));
                    // The resume point: the callee returns natively to `ret`, or
                    // unwinds through the dispatcher (it may itself have an
                    // indirect or opaque exit): then the dispatcher delivers
                    // control to `ret` and the caller has to resume there.
                    if ret >= entry && ret < end && resume_seen.insert(ret) {
                        resume_points.push(ret);
                    }
                    if ret >= entry && ret < end {
                        body.push_str(&format!(
                            "    if (pc == 0x{ret:08X}u && running) goto L_{ret:08X}; else goto L_TAKEOVER;\n"
                        ));
                    } else {
                        body.push_str(&format!(
                            "    if (pc == 0x{ret:08X}u && running) goto L_EXIT; else goto L_TAKEOVER;\n"
                        ));
                    }
                } else {
                    calls_dispatcher += 1;
                    // Exactly the spike's behaviour: set lr and pc, leave.
                    let rw = match rewrite(addr, entry, end, &stmt_for_body, &spill) {
                        Ok(rw) => rw,
                        Err(e) => {
                            failed = Some(e);
                            break;
                        }
                    };
                    body.push_str("    ++exec;\n");
                    body.push_str(&format!("    do {{ {} }} while (0);\n", rw.c));
                    if ret >= entry && ret < end && resume_seen.insert(ret) {
                        resume_points.push(ret);
                    }
                }
                continue;
            }
            let rw = match rewrite(addr, entry, end, &stmt_for_body, &spill) {
                Ok(rw) => rw,
                Err(e) => {
                    failed = Some(e);
                    break;
                }
            };
            if rw.opaque {
                // Opaque exit (t_2fe227ca): hand this one instruction to the
                // dispatcher and resume after it.  No `++exec`: the body
                // retires nothing here (the hook's step accounting relies on
                // that, see below).
                opaque_exits += 1;
                body.push_str(&format!("    {{ pc = 0x{addr:08X}u; goto L_EXIT; }}\n"));
                for t in const_successors(stmt, entry, end) {
                    if resume_seen.insert(t) {
                        resume_points.push(t);
                    }
                }
                let next = addr + stride(addr);
                if next >= entry && next < end && resume_seen.insert(next) {
                    resume_points.push(next);
                }
                // The statement is executed by the dispatcher (through the
                // hook, which copies the whole file), so it neither dirties a
                // local nor contributes to the body's sets.
                continue;
            }
            body.push_str("    ++exec;\n");
            body.push_str("    {\n    unsigned _fn_ex = 0;\n");
            body.push_str(&format!("    do {{ {} }} while (0);\n", rw.c));
            if !rw.exits.is_empty() {
                body.push_str("    switch (_fn_ex) {\n");
                for e in &rw.exits {
                    match e.label {
                        Some(t) if t <= addr => {
                            if resume_seen.insert(t) {
                                resume_points.push(t);
                            }
                            body.push_str(&format!(
                                "    case {}: if (--_fn_budget == 0) {{ pc = 0x{t:08X}u; goto L_EXIT; }} goto L_{t:08X};\n",
                                e.flag
                            ));
                        }
                        Some(t) => {
                            body.push_str(&format!("    case {}: goto L_{t:08X};\n", e.flag));
                        }
                        None => {
                            body.push_str(&format!("    case {}: goto L_EXIT;\n", e.flag));
                        }
                    }
                }
                body.push_str("    default: break;\n    }\n");
            }
            if rw.touches_running {
                let next = addr + stride(addr);
                body.push_str(&format!(
                    "    if (!running) {{ pc = 0x{next:08X}u; goto L_EXIT; }}\n"
                ));
            }
            body.push_str("    }\n");
            if rw.indirect {
                indirect_exits += 1;
            }
            for t in &rw.resume {
                if *t >= entry && *t < end && resume_seen.insert(*t) {
                    resume_points.push(*t);
                }
            }
        }
        if let Some(reason) = failed {
            prog.skipped.push(SkippedFn { entry, end, reason });
            continue;
        }

        let insn_count = plan.addrs.len();
        let mut resumes_c = String::new();
        if !resume_points.is_empty() {
            resumes_c.push_str("    if (resume) {\n        switch (resume) {\n");
            for r in &resume_points {
                resumes_c.push_str(&format!("        case 0x{r:08X}u: goto L_{r:08X};\n"));
            }
            resumes_c.push_str("        default: break;\n        }\n    }\n");
        }
        resume_points.sort_unstable();

        // Prelude: load exactly the slots this body can read or write (plus
        // `running`, see body_load_set).  A resume entry can re-enter after a
        // write, so "read anywhere" (not "read before written") is the load set;
        // writes are loaded too so a path that only stores them publishes a
        // copy, not a garbage local.
        fn_prelude_ops.push(load_set.len() + 1);
        fn_epilogue_ops.push(store_set.len() + 1);

        let mut shard = String::new();
        shard.push_str(&format!(
            "/* FN_EMIT shard {idx}: guest function 0x{entry:08X}..0x{end:08X}\n"
        ));
        shard.push_str(&format!(
            " * ({insn_count} guest instructions, {calls} bl sites: {calls_selected} to emitted\n"
        ));
        shard.push_str(&format!(
            " * functions as real C calls, {calls_dispatcher} left to the dispatcher;\n"
        ));
        shard.push_str(&format!(
            " * {indirect_exits} indirect exit(s) + {opaque_exits} opaque exit(s) hand one step\n"
        ));
        shard.push_str(" * back to the dispatcher).\n");
        shard.push_str(&format!(
            " * Exact slots (t_21dda9b5): loads {} ({:?}), stores {} ({:?}); a call\n",
            load_set.len(),
            load_set,
            store_set.len(),
            store_set
        ));
        shard.push_str(&format!(
            " * spills the caller's write set ({} slots + r14); {} call site(s) keep the\n",
            caller_writes.len(),
            call_sites_exact
        ));
        shard.push_str(&format!(
            " * exact reload (the callee cannot dispatch), {call_sites_full} take the full\n"
        ));
        shard.push_str(&format!(
            " * reload because the callee can (may_dispatch={}). */\n",
            if may_dispatch.contains(&entry) { 1 } else { 0 }
        ));
        shard.push_str(&format!(
            "unsigned long long {name}(Dq8FnState *st, uint32_t resume) {{\n"
        ));
        shard.push_str("    uint32_t pc = st->pc;\n");
        shard.push_str("    uint32_t r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12, r13, r14;\n");
        shard.push_str("    int n, z, c, v, ge, running;\n");
        shard.push_str(&slots_load(&load_set, "    "));
        shard.push_str("    unsigned long long exec = 0;\n");
        shard.push_str(&format!("    unsigned _fn_budget = {FN_BUDGET};\n"));
        shard.push_str(&resumes_c);
        shard.push_str(&body);
        // A native call that did not come back to its return address (the
        // callee tail-branched or its own callee handed control to the
        // dispatcher) leaves the authoritative state in `st`, and this frame's
        // locals are stale.  Unwind without the store-back, exactly as the
        // interpreter would: the callee already published pc, r[], flags.
        shard.push_str("L_TAKEOVER:\n");
        shard.push_str(&format!(
            "    if (dq8_fn_tr) {{ unsigned _i; fprintf(stderr, \"EXITTK fn=0x{entry:08X} pc=%08X st=\", pc); for (_i = 0; _i < 15u; ++_i) fprintf(stderr, \"%08X,\", st->r[_i]); fprintf(stderr, \"%d%d%d%d%d,%d\\n\", st->n, st->z, st->c, st->v, st->ge, st->running); }}\n"
        ));
        shard.push_str("    return exec;\n");
        shard.push_str("L_EXIT:\n");
        shard.push_str(&slots_spill(&store_set, "    "));
        shard.push_str(&format!(
            "    if (dq8_fn_tr) {{ unsigned _i; fprintf(stderr, \"EXIT   fn=0x{entry:08X} pc=%08X st=\", pc); for (_i = 0; _i < 15u; ++_i) fprintf(stderr, \"%08X,\", st->r[_i]); fprintf(stderr, \"%d%d%d%d%d,%d\\n\", st->n, st->z, st->c, st->v, st->ge, st->running); }}\n"
        ));
        shard.push_str("    st->pc = pc;\n");
        shard.push_str("    return exec;\n");
        shard.push_str("}\n");

        decls.push_str(&format!(
            "unsigned long long {name}(Dq8FnState *st, uint32_t resume);\n"
        ));

        // The hook: one case at the entry (resume = 0) and one per resume
        // entry.  A resume PC that is itself an emitted entry is skipped here:
        // the entry hook already runs that code, and running the function from
        // its entry at that address is the same instruction sequence.
        //
        // Cases are collected here and emitted after the loop: two functions
        // can claim the same pc (a shared return address, a call target that is
        // also a resume point of an outer function) and a switch cannot have
        // two cases for one value.  Entries are emitted first, so a pc that is
        // an entry of one function and a resume point of another dispatches to
        // the entry (the resume body would run the same instruction sequence
        // from a different start, so the entry is the correct owner).
        for r in &resume_points {
            if !accepted.contains(r) {
                pending_cases.push((*r, *r, idx));
            }
        }
        pending_cases.push((entry, 0, idx));

        prog.shards.push(shard);
        prog.funcs.push(EmittedFn {
            entry,
            end,
            insn_count,
            calls,
            calls_selected,
            calls_dispatcher,
            dispatcher_exits: indirect_exits + opaque_exits,
            indirect_exits,
            opaque_exits,
            may_dispatch: may_dispatch.contains(&entry),
            call_sites_exact,
            call_sites_full,
            resume_points,
        });
    }

    // ---- the hook: O(1) entry check (t_e1547d93).  Entries first
    // (resume = 0), then resume points; a pc claimed by more than one function
    // is dispatched once.
    //
    // The hook sits in the dispatcher's hot loop, so the *cost of a miss*
    // matters more than the cost of a hit: with 2 800 emitted functions a flat
    // `switch (pc)` grows to 6 688 sparse cases over a 4.5 MB range, which
    // clang compiles to a compare tree, and every *interpreted* instruction
    // pays it (measured on the gate-off arm: 42.6 ms/frame against 37.9
    // ms/frame for the 300-function build); page-grouping the switch still
    // costs an indirect jump through a 1 085-entry table plus an inner switch.
    //
    // The check is now two levels, both sized here from the list itself:
    //   1. a bitmap over [BASE, BASE+SPAN) at 32-byte granularity (~17 KB for
    //      this list, so it stays in L1/L2) rejects almost every dispatch with
    //      one subtract, one compare, one load and a shift+test;
    //   2. a hit costs one open-addressed probe into dq8_fn_keys/dq8_fn_ix
    //      (multiplier and probe bound chosen here, at generation, to minimise
    //      the worst probe), then the body is called through dq8_fn_tab.
    // Semantics are the old hook's, exactly: the pc is matched exactly, the
    // body gets `resume = 0` at its entry and `resume = pc` at a resume point,
    // and a body that retires nothing falls through to the dispatcher in the
    // same loop iteration.
    //
    // DQ8_FN_HOOKMODE (runtime) selects the arm the cost report measures:
    //   0 off, 1 bitmap check only, 2 bitmap check + dispatch (default),
    //   3 page-switch check only, 4 flat-switch check only, 5 null call.
    // 3/4/5 keep the *old* checks measurable in the same binary; they are pure
    // predicates, so nothing is dispatched off mode 2.
    pending_cases.sort_by_key(|&(pcv, resume, idx)| (if resume == 0 { 0u8 } else { 1 }, idx, pcv));
    let mut hook_seen: BTreeSet<u32> = BTreeSet::new();
    let mut dropped_cases = 0usize;
    let mut unique_cases: Vec<(u32, u32, usize)> = Vec::new();
    for (pcv, resume, idx) in pending_cases {
        if !hook_seen.insert(pcv) {
            dropped_cases += 1;
            continue;
        }
        hook_pcs.push(pcv);
        unique_cases.push((pcv, resume, idx));
    }
    unique_cases.sort_unstable_by_key(|c| c.0);
    let nkeys = unique_cases.len();

    // ---- level 0: bitmap over the emitted pc range, one bit per 32 bytes.
    let (fn_base, fn_limit) = if unique_cases.is_empty() {
        (0x0010_0000u32, 0x0010_0000u32)
    } else {
        let lo = unique_cases[0].0 & !31u32;
        let hi = (unique_cases[nkeys - 1].0 + 32u32) & !31u32;
        (lo, hi)
    };
    assert!(fn_base != 0, "FN_EMIT: base 0 collides with the empty hash slot");
    let fn_span = fn_limit - fn_base;
    let map_words = ((fn_span as usize + 1023) / 1024).max(1);
    let mut map = vec![0u32; map_words];
    for &(pcv, _, _) in &unique_cases {
        let off = (pcv - fn_base) as usize;
        map[off >> 10] |= 1u32 << ((off >> 5) & 31);
    }

    // ---- level 1: open-addressed table at a 2x load factor; of five
    // multiplier candidates the one with the shortest worst-case probe wins
    // (the constants are odd 32-bit hashes, so the multiply is a shift-xor-free
    // one-instruction hash).
    let mut hbits = 1u32;
    while (1usize << hbits) < 2 * nkeys.max(1) {
        hbits += 1;
    }
    let hsize = 1usize << hbits;
    let mut best: Option<(u32, Vec<u32>, Vec<u16>, usize)> = None;
    if nkeys > 0 {
        for &mult in &[
            0x9E37_79B1u32,
            0x85EB_CA6Bu32,
            0xC2B2_AE35u32,
            0x27D4_EB2Fu32,
            0x1656_67B1u32,
        ] {
            let mut k = vec![0u32; hsize];
            let mut j = vec![0u16; hsize];
            let mut maxp = 0usize;
            let mut overflow = false;
            for &(pcv, _, idx) in &unique_cases {
                let mut s = (pcv.wrapping_mul(mult) >> (32 - hbits)) as usize & (hsize - 1);
                let mut p = 0usize;
                while k[s] != 0 {
                    s = (s + 1) & (hsize - 1);
                    p += 1;
                    if p >= 64 {
                        overflow = true;
                        break;
                    }
                }
                if overflow {
                    break;
                }
                k[s] = pcv;
                j[s] = idx as u16;
                maxp = maxp.max(p);
            }
            if !overflow && best.as_ref().map_or(true, |b| maxp < b.3) {
                best = Some((mult, k, j, maxp));
            }
        }
    }
    let (mult, keys, ix, maxp) = match best {
        Some((m, k, j, p)) => (m, k, j, p),
        None => (0x9E37_79B1u32, vec![0u32; hsize], vec![0u16; hsize], 0usize),
    };
    let probes = (maxp + 2).max(2);
    assert!(
        probes <= 32,
        "FN_EMIT: hash worst-case probe {maxp} is too long for the inline loop"
    );
    assert!(nkeys <= 0xFFFF, "FN_EMIT: {nkeys} cases do not fit the uint16 index table");

    // ---- verify both levels against the case list.  A false negative would
    // silently drop a body back to the dispatcher (coverage loss with no error)
    // and a wrong owner would change which body runs at a shared pc, so the
    // tables are checked here, at generation, not inferred from a run.
    for &(pcv, resume, idx) in &unique_cases {
        let off = (pcv - fn_base) as usize;
        assert!(
            off < fn_span as usize && (map[off >> 10] >> ((off >> 5) & 31)) & 1 != 0,
            "FN_EMIT: bitmap does not cover 0x{pcv:08X}"
        );
        let mut s = (pcv.wrapping_mul(mult) >> (32 - hbits)) as usize & (hsize - 1);
        let mut found: Option<usize> = None;
        for _ in 0..probes {
            if keys[s] == pcv {
                found = Some(ix[s] as usize);
                break;
            }
            if keys[s] == 0 {
                break;
            }
            s = (s + 1) & (hsize - 1);
        }
        assert_eq!(found, Some(idx), "FN_EMIT: hash lookup mismatch at 0x{pcv:08X}");
        let want_resume = if pcv == prog.funcs[idx].entry { 0 } else { pcv };
        assert_eq!(
            resume, want_resume,
            "FN_EMIT: resume value for 0x{pcv:08X} is not derivable from the entry"
        );
    }

    let mut map_init = String::new();
    for (i, w) in map.iter().enumerate() {
        map_init.push_str(&format!("0x{w:08X}u, "));
        if i % 12 == 11 {
            map_init.push('\n');
        }
    }
    let mut keys_init = String::new();
    for (i, k) in keys.iter().enumerate() {
        keys_init.push_str(&format!("0x{k:08X}u, "));
        if i % 12 == 11 {
            keys_init.push('\n');
        }
    }
    let mut ix_init = String::new();
    for (i, v) in ix.iter().enumerate() {
        ix_init.push_str(&format!("{v}u, "));
        if i % 24 == 23 {
            ix_init.push('\n');
        }
    }
    let mut tab_init = String::new();
    for f in &prog.funcs {
        tab_init.push_str(&format!("{}, ", fn_name(f.entry)));
    }
    // 64 evenly spaced real pcs for the amplifier arms.
    let mut pool_init = String::new();
    if !unique_cases.is_empty() {
        for j in 0..64usize {
            let idx = (j * nkeys) / 64;
            pool_init.push_str(&format!("0x{:08X}u, ", unique_cases[idx.min(nkeys - 1)].0));
        }
    } else {
        pool_init.push_str("0, ");
    }
    // The old checks, kept as pure predicates for DQ8_FN_HOOKMODE=3/4.
    let mut flat_cases = String::new();
    let mut page_cases = String::new();
    let mut cases_by_page: BTreeMap<u32, String> = BTreeMap::new();
    for &(pcv, _, _) in &unique_cases {
        let line = format!("    case 0x{pcv:08X}u: return 1;\n");
        flat_cases.push_str(&line);
        cases_by_page.entry(pcv >> 12).or_default().push_str(&line);
    }
    for (page, cases) in &cases_by_page {
        page_cases.push_str(&format!(
            "    case 0x{page:05X}u: switch (pcv) {{\n{cases}    }} break;\n"
        ));
    }
    eprintln!(
        "FN_EMIT_HOOK cases={} unique_pcs={} dropped_shared={} bitmap_bytes={} slots={} probes={} base=0x{fn_base:08X} span=0x{fn_span:X}",
        hook_pcs.len() + dropped_cases,
        hook_pcs.len(),
        dropped_cases,
        map_words * 4,
        hsize,
        probes
    );

    // ---- the runtime def block: counters, traffic report, gate.
    let n = prog.funcs.len();
    let sites = site_spill_ops.len();
    let mut entries_init = String::new();
    let mut prelude_init = String::new();
    let mut epilogue_init = String::new();
    for f in &prog.funcs {
        entries_init.push_str(&format!("0x{:08X}u, ", f.entry));
    }
    for i in 0..n {
        prelude_init.push_str(&format!("{}, ", fn_prelude_ops.get(i).copied().unwrap_or(0)));
        epilogue_init.push_str(&format!("{}, ", fn_epilogue_ops.get(i).copied().unwrap_or(0)));
    }
    let mut spill_init = String::new();
    let mut reload_init = String::new();
    let mut full_init = String::new();
    for i in 0..sites {
        spill_init.push_str(&format!("{}, ", site_spill_ops[i]));
        reload_init.push_str(&format!("{}, ", site_reload_ops[i]));
        full_init.push_str(&format!("{}, ", if site_full[i] { 1 } else { 0 }));
    }
    if sites == 0 {
        spill_init.push_str("0, ");
        reload_init.push_str("0, ");
        full_init.push_str("0, ");
    }
    let sites_exact = site_full.iter().filter(|f| !**f).count();
    let sites_full = sites - sites_exact;
    let fn_exact = prog.funcs.iter().filter(|f| !f.may_dispatch).count();
    let fn_full = n - fn_exact;
    eprintln!(
        "FN_EMIT_BOUND functions={n} may_dispatch=0: {fn_exact} may_dispatch=1: {fn_full} \
         sites={sites} exact={sites_exact} full={sites_full}"
    );
    prog.decls_c = decls;

    // ---- the emitted tables and the two hook arms (t_e1547d93).  The tokens
    // are replaced instead of using format! so the C braces stay literal.
    let hook_defs_c = r#"/* FN_EMIT entry check (t_e1547d93): two levels, O(1) per dispatch entry.
 *   level 0  dq8_fn_map             one bit per 32 B over [BASE, BASE+SPAN)
 *   level 1  dq8_fn_keys/dq8_fn_ix  open-addressed, multiplier and bound
 *                                   chosen at generation; the index selects
 *                                   the body through dq8_fn_tab[].
 * DQ8_FN_HOOKMODE 0/1/3/4/5 are the cost-report arms; 2 is production. */
#define DQ8_FN_BASE 0x@@BASE@@u
#define DQ8_FN_SPAN 0x@@SPAN@@u
#define DQ8_FN_HBITS @@HBITS@@u
#define DQ8_FN_HMASK 0x@@HMASK@@u
#define DQ8_FN_HMULT 0x@@HMULT@@u
#define DQ8_FN_PROBES @@PROBES@@u
#define DQ8_FN_HASHSLOT(pcv) ((unsigned)((((pcv) * DQ8_FN_HMULT) >> (32u - DQ8_FN_HBITS)) & DQ8_FN_HMASK))
static const uint32_t dq8_fn_map[@@MAPW@@] = { @@MAP@@ };
static const uint32_t dq8_fn_keys[@@HSIZE@@] = { @@KEYS@@ };
static const unsigned short dq8_fn_ix[@@HSIZE@@] = { @@IX@@ };
static unsigned long long (*const dq8_fn_tab[DQ8_FN_N ? DQ8_FN_N : 1])(Dq8FnState *, uint32_t) = { @@TAB@@ };
static int dq8_fn_hookmode = 2;
static unsigned long long dq8_fn_hook_entries, dq8_fn_map_hit, dq8_fn_map_miss,
                          dq8_fn_key_miss, dq8_fn_dispatched, dq8_fn_probe_hits;
/* 64 real emitted pcs, evenly spaced through the case list: the amplifier arms
 * probe these instead of one pc repeated, so the old switch arms pay the
 * branch mispredictions a real dispatch stream pays. */
#define DQ8_FN_POOL 64u
static const uint32_t dq8_fn_pcpool[DQ8_FN_POOL] = { @@POOL@@ };
/* The checks, as pure predicates: 3 = page-grouped switch, 4 = flat switch,
 * 6 = this bitmap, 9 = null (the call/pool-indexing control). */
__attribute__((noinline)) static int dq8_fn_bitmap_check(uint32_t pcv) {
    uint32_t o = pcv - DQ8_FN_BASE;
    return o < DQ8_FN_SPAN && ((dq8_fn_map[o >> 10] >> ((o >> 5) & 31u)) & 1u);
}
__attribute__((noinline)) static int dq8_fn_null_check(uint32_t pcv) { (void)pcv; return 1; }
__attribute__((noinline)) static int dq8_fn_page_check(uint32_t pcv) {
    switch (pcv >> 12) {
@@PAGE@@    }
    return 0;
}
__attribute__((noinline)) static int dq8_fn_flat_check(uint32_t pcv) {
    switch (pcv) {
@@FLAT@@    }
    return 0;
}
"#
    .replace("@@BASE@@", &format!("{fn_base:08X}"))
    .replace("@@SPAN@@", &format!("{fn_span:X}"))
    .replace("@@HBITS@@", &hbits.to_string())
    .replace("@@HMASK@@", &format!("{:X}", hsize - 1))
    .replace("@@HMULT@@", &format!("{mult:08X}"))
    .replace("@@PROBES@@", &probes.to_string())
    .replace("@@MAPW@@", &map_words.to_string())
    .replace("@@MAP@@", &map_init)
    .replace("@@HSIZE@@", &hsize.to_string())
    .replace("@@KEYS@@", &keys_init)
    .replace("@@IX@@", &ix_init)
    .replace("@@TAB@@", &tab_init)
    .replace("@@PAGE@@", &page_cases)
    .replace("@@FLAT@@", &flat_cases)
    .replace("@@POOL@@", &pool_init);
    let mut def_c = String::new();
    def_c.push_str(&format!(
        "/* FN_EMIT (t_21dda9b5): {n} guest functions emitted as real C functions,\n\
         * {sites} native call sites with exact spill/reload sets.\n\
         * Gate: DQ8_FN_EMIT=1|all (every emitted function) or a comma-separated\n\
         * list of entry addresses; unset/0 leaves every function on the\n\
         * dispatcher (the default).  DQ8_FN_TIME=1 adds a CPU timer per call,\n\
         * DQ8_FN_STATS=1 prints the call-boundary traffic per frame. */\n\
         #define DQ8_FN_N {n}\n\
         #define DQ8_FN_SITES {sites}\n\
         static unsigned long long dq8_fn_runs[DQ8_FN_N], dq8_fn_insns[DQ8_FN_N];\n\
         static unsigned long long dq8_fn_calls[DQ8_FN_N], dq8_fn_ns[DQ8_FN_N];\n\
         static int dq8_fn_on[DQ8_FN_N];\n\
         static int dq8_fn_time;\n\
         static const unsigned dq8_fn_entry[DQ8_FN_N] = {{ {entries_init} }};\n\
         unsigned long long dq8_fn_sc[DQ8_FN_SITES ? DQ8_FN_SITES : 1];\n\
         int dq8_fn_stats_on;\n\
         {hook_defs_c}\n\
         int dq8_fn_tr;\n\
         static const unsigned char dq8_fn_prelude_ops[DQ8_FN_N] = {{ {prelude_init} }};\n\
         static const unsigned char dq8_fn_epilogue_ops[DQ8_FN_N] = {{ {epilogue_init} }};\n\
         static const unsigned char dq8_fn_site_spill[DQ8_FN_SITES ? DQ8_FN_SITES : 1] = {{ {spill_init} }};\n\
         static const unsigned char dq8_fn_site_reload[DQ8_FN_SITES ? DQ8_FN_SITES : 1] = {{ {reload_init} }};\n\
         static const unsigned char dq8_fn_site_full[DQ8_FN_SITES ? DQ8_FN_SITES : 1] = {{ {full_init} }};\n"
    ));
    def_c.push_str(
        r#"uint32_t dq8_fn_branch_indirect(Dq8FnState *st, uint32_t site, uint32_t target) {
    uint32_t *saved_regs = active_guest_regs;
    uint32_t saved_pc = current_guest_pc;
    uint32_t out;
    active_guest_regs = st->r;
    current_guest_pc = site;
    out = branch_indirect(target);
    active_guest_regs = saved_regs;
    current_guest_pc = saved_pc;
    return out;
}
static void dq8_fn_report(void) {
    unsigned i;
    for (i = 0; i < DQ8_FN_N; ++i) {
        if (!dq8_fn_on[i]) continue;
        if (dq8_fn_time)
            fprintf(stderr, "FN_EMIT fn=0x%08X runs=%llu insns=%llu calls=%llu cpu_ns=%llu ns_per_call=%.2f ns_per_insn=%.3f\n",
                    dq8_fn_entry[i], dq8_fn_runs[i], dq8_fn_insns[i], dq8_fn_calls[i], dq8_fn_ns[i],
                    dq8_fn_calls[i] ? (double)dq8_fn_ns[i] / (double)dq8_fn_calls[i] : 0.0,
                    dq8_fn_insns[i] ? (double)dq8_fn_ns[i] / (double)dq8_fn_insns[i] : 0.0);
        else
            fprintf(stderr, "FN_EMIT fn=0x%08X runs=%llu insns=%llu calls=%llu\n",
                    dq8_fn_entry[i], dq8_fn_runs[i], dq8_fn_insns[i], dq8_fn_calls[i]);
    }
    if (dq8_fn_stats_on) {
        unsigned long long site_calls = 0, spill = 0, reload = 0, hooks = 0, pre = 0, epi = 0;
        unsigned long long c_ex = 0, c_full = 0, sp_ex = 0, sp_full = 0, rl_ex = 0, rl_full = 0;
        unsigned long long frames = atomic_load_explicit(&guest_frame_swaps, memory_order_relaxed);
        unsigned k;
        unsigned n_ex = 0, n_full = 0;
        for (k = 0; k < (unsigned)DQ8_FN_SITES; ++k) {
            site_calls += dq8_fn_sc[k];
            spill += dq8_fn_sc[k] * dq8_fn_site_spill[k];
            reload += dq8_fn_sc[k] * dq8_fn_site_reload[k];
            /* Per-class split (t_40d37ebb): a site is "full" when its callee can
             * hand control to the dispatcher and the boundary has to be the
             * conservative one. */
            if (dq8_fn_site_full[k]) {
                ++n_full;
                c_full += dq8_fn_sc[k];
                sp_full += dq8_fn_sc[k] * dq8_fn_site_spill[k];
                rl_full += dq8_fn_sc[k] * dq8_fn_site_reload[k];
            } else {
                ++n_ex;
                c_ex += dq8_fn_sc[k];
                sp_ex += dq8_fn_sc[k] * dq8_fn_site_spill[k];
                rl_ex += dq8_fn_sc[k] * dq8_fn_site_reload[k];
            }
        }
        for (k = 0; k < (unsigned)DQ8_FN_N; ++k) {
            hooks += dq8_fn_calls[k];
            pre += dq8_fn_calls[k] * dq8_fn_prelude_ops[k];
            epi += dq8_fn_calls[k] * dq8_fn_epilogue_ops[k];
        }
        fprintf(stderr,
                "FN_STATS frames=%llu sites=%d native_calls=%llu calls_per_frame=%.2f spill_stores=%llu reload_loads=%llu hook_entries=%llu prelude_loads=%llu epilogue_stores=%llu ops_per_frame=%.2f bytes_per_frame=%.1f\n",
                frames, DQ8_FN_SITES, site_calls,
                frames ? (double)site_calls / (double)frames : 0.0,
                spill, reload, hooks, pre, epi,
                frames ? (double)(spill + reload + pre + epi) / (double)frames : 0.0,
                4.0 * (frames ? (double)(spill + reload + pre + epi) / (double)frames : 0.0));
        fprintf(stderr,
                "FN_HOOK mode=%d entries=%llu map_hit=%llu map_miss=%llu key_miss=%llu dispatched=%llu probe_hits=%llu bitmap_bytes=%u slots=%u probes=%u\n",
                dq8_fn_hookmode, dq8_fn_hook_entries, dq8_fn_map_hit, dq8_fn_map_miss,
                dq8_fn_key_miss, dq8_fn_dispatched, dq8_fn_probe_hits,
                (unsigned)sizeof dq8_fn_map, (unsigned)DQ8_FN_HMASK + 1u,
                (unsigned)DQ8_FN_PROBES);
        /* Same accounting per call-site class, so the report can say which
         * class the traffic is actually in. */
        fprintf(stderr,
                "FN_STATS_CLASS sites_exact=%u sites_full=%u calls_exact=%llu calls_full=%llu spill_exact=%llu reload_exact=%llu spill_full=%llu reload_full=%llu ops_per_frame_exact=%.2f ops_per_frame_full=%.2f\n",
                n_ex, n_full, c_ex, c_full, sp_ex, rl_ex, sp_full, rl_full,
                frames ? (double)(sp_ex + rl_ex) / (double)frames : 0.0,
                frames ? (double)(sp_full + rl_full) / (double)frames : 0.0);
    }
}
static void dq8_fn_gate(void) {
    const char *list = getenv("DQ8_FN_EMIT");
    const char *tm = getenv("DQ8_FN_TIME");
    const char *st = getenv("DQ8_FN_STATS");
    const char *hm = getenv("DQ8_FN_HOOKMODE");
    unsigned i; int any = 0;
    dq8_fn_time = (tm && *tm && *tm != '0') ? 1 : 0;
    dq8_fn_stats_on = (st && *st && *st != '0') ? 1 : 0;
    if (hm && *hm) {
        long v = strtol(hm, 0, 0);
        if (v >= 0 && v <= 9) dq8_fn_hookmode = (int)v;
        else fprintf(stderr, "DQ8_FN_HOOKMODE=%s is outside 0..9; staying at %d\n", hm, dq8_fn_hookmode);
    }
    {
        const char *tr = getenv("DQ8_FN_TRACE_CALLS");
        dq8_fn_tr = (tr && *tr && *tr != '0') ? 1 : 0;
    }
    if (dq8_fn_stats_on || dq8_fn_hookmode != 2)
        fprintf(stderr, "FN_EMIT hook mode=%d (0 off, 1 bitmap check, 2 bitmap+dispatch, 3 page check, 4 flat check, 5 null call) bitmap_bytes=%u slots=%u probes=%u base=0x%08X span=0x%X\n",
                dq8_fn_hookmode, (unsigned)sizeof dq8_fn_map, (unsigned)DQ8_FN_HMASK + 1u,
                (unsigned)DQ8_FN_PROBES, (unsigned)DQ8_FN_BASE, (unsigned)DQ8_FN_SPAN);
    if (!list || !*list || (list[0] == '0' && !list[1])) {
        if (dq8_fn_stats_on) atexit(dq8_fn_report);
        return;
    }
    if (!strcmp(list, "1") || !strcmp(list, "all")) {
        for (i = 0; i < DQ8_FN_N; ++i) dq8_fn_on[i] = 1;
        any = 1;
    } else {
        const char *p = list;
        while (*p) {
            const char *q = p;
            while (*q && *q != ',' && *q != ':' && *q != ';' && *q != ' ') ++q;
            char tok[32]; size_t len = (size_t)(q - p);
            if (len >= sizeof tok) len = sizeof tok - 1u;
            memcpy(tok, p, len); tok[len] = 0;
            if (len) {
                char *e = 0; unsigned long long v = strtoull(tok, &e, 0);
                if (e != tok) {
                    int hit = 0;
                    for (i = 0; i < DQ8_FN_N; ++i)
                        if ((unsigned long long)dq8_fn_entry[i] == v) { dq8_fn_on[i] = 1; hit = 1; any = 1; }
                    if (!hit) fprintf(stderr, "DQ8_FN_EMIT: 0x%llX is not an emitted function\n", v);
                }
            }
            p = (*q ? q + 1 : q);
        }
    }
    if (any || dq8_fn_stats_on) atexit(dq8_fn_report);
    if (!any) fprintf(stderr, "DQ8_FN_EMIT=%s enabled nothing; all functions stay on the dispatcher\n", list);
}
"#,
    );
    prog.def_c = def_c;
    prog.gate_c = "    dq8_fn_gate();\n".to_string();
    prog.hook_c = if unique_cases.is_empty() {
        String::new()
    } else {
        r#"        /* FN_EMIT entry check (t_e1547d93): bitmap reject, then one
           open-addressed probe, then the body through the pointer table.  The
           pc is matched exactly; an entry gets resume = 0, a resume point gets
           resume = pc; a body that retires nothing falls through to the
           dispatcher in this same iteration (the hook's +1 above already pays
           for it). */
        if (dq8_fn_stats_on) ++dq8_fn_hook_entries;
        if (dq8_fn_hookmode >= 6) {
            /* Cost amplifier (t_e1547d93): the same probe 16 times per entry.
               The probe pc comes from a 64-entry pool of REAL emitted pcs,
               advanced by the entry counter, so the switch below sees pcs it
               cannot learn from the previous entry -- otherwise a hot,
               perfectly predicted measurement would hide the branch
               mispredictions that dominate a 6 688-case switch.  All three
               checks go through noinline predicates and pay the same pool
               indexing, so the null-call arm (mode 9) cancels the overhead. */
            unsigned dq8_acc = 0, dq8_j;
            unsigned dq8_p0 = ((pc >> 2) + (unsigned)dq8_fn_hook_entries) & (DQ8_FN_POOL - 1u);
            if (dq8_fn_hookmode == 6) {
                for (dq8_j = 0; dq8_j < 16u; ++dq8_j) {
                    dq8_acc += (unsigned)dq8_fn_bitmap_check(dq8_fn_pcpool[(dq8_p0 + dq8_j) & (DQ8_FN_POOL - 1u)]);
                    __asm__ __volatile__("" ::: "memory");
                }
            } else if (dq8_fn_hookmode == 7) {
                for (dq8_j = 0; dq8_j < 16u; ++dq8_j) {
                    dq8_acc += (unsigned)dq8_fn_page_check(dq8_fn_pcpool[(dq8_p0 + dq8_j) & (DQ8_FN_POOL - 1u)]);
                    __asm__ __volatile__("" ::: "memory");
                }
            } else if (dq8_fn_hookmode == 8) {
                for (dq8_j = 0; dq8_j < 16u; ++dq8_j) {
                    dq8_acc += (unsigned)dq8_fn_flat_check(dq8_fn_pcpool[(dq8_p0 + dq8_j) & (DQ8_FN_POOL - 1u)]);
                    __asm__ __volatile__("" ::: "memory");
                }
            } else {
                for (dq8_j = 0; dq8_j < 16u; ++dq8_j) {
                    dq8_acc += (unsigned)dq8_fn_null_check(dq8_fn_pcpool[(dq8_p0 + dq8_j) & (DQ8_FN_POOL - 1u)]);
                    __asm__ __volatile__("" ::: "memory");
                }
            }
            dq8_fn_probe_hits += dq8_acc;
        } else if (dq8_fn_hookmode >= 3) {
            int dq8_fh;
            if (dq8_fn_hookmode == 5) dq8_fh = dq8_fn_null_check(pc);
            else if (dq8_fn_hookmode == 3) dq8_fh = dq8_fn_page_check(pc);
            else dq8_fh = dq8_fn_flat_check(pc);
            if (dq8_fn_stats_on) dq8_fn_probe_hits += (unsigned long long)(unsigned)dq8_fh;
        } else if (dq8_fn_hookmode) {
            uint32_t dq8_fo = pc - DQ8_FN_BASE;
            if (dq8_fo < DQ8_FN_SPAN && ((dq8_fn_map[dq8_fo >> 10] >> ((dq8_fo >> 5) & 31u)) & 1u)) {
                unsigned dq8_hs = DQ8_FN_HASHSLOT(pc);
                unsigned dq8_p;
                if (dq8_fn_stats_on) ++dq8_fn_map_hit;
                for (dq8_p = 0; dq8_p < DQ8_FN_PROBES; ++dq8_p) {
                    uint32_t dq8_k = dq8_fn_keys[dq8_hs];
                    if (dq8_k == pc) break;
                    if (dq8_k == 0u) { dq8_p = DQ8_FN_PROBES; break; }
                    dq8_hs = (dq8_hs + 1u) & DQ8_FN_HMASK;
                }
                if (dq8_p >= DQ8_FN_PROBES) {
                    if (dq8_fn_stats_on) ++dq8_fn_key_miss;
                } else {
                    unsigned dq8_i = dq8_fn_ix[dq8_hs];
                    if (dq8_fn_hookmode == 2 && dq8_fn_on[dq8_i]) {
                        Dq8FnState fn_st; unsigned long long fn_exec;
                        /* The dispatcher's register array is the architectural
                           file: the body points the state at it instead of
                           copying 15 registers in and out. */
                        if (dq8_fn_stats_on) ++dq8_fn_dispatched;
                        ++dq8_fn_calls[dq8_i];
                        fn_st.pc = pc; fn_st.r = r;
                        fn_st.n = n; fn_st.z = z; fn_st.c = c; fn_st.v = v; fn_st.ge = ge; fn_st.running = running;
                        if (dq8_fn_time) {
                            struct timespec fn_t0, fn_t1;
                            clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &fn_t0);
                            fn_exec = dq8_fn_tab[dq8_i](&fn_st, pc == dq8_fn_entry[dq8_i] ? 0u : pc);
                            clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &fn_t1);
                            dq8_fn_ns[dq8_i] += (unsigned long long)(fn_t1.tv_sec - fn_t0.tv_sec) * 1000000000ull + (unsigned long long)(fn_t1.tv_nsec - fn_t0.tv_nsec);
                        } else fn_exec = dq8_fn_tab[dq8_i](&fn_st, pc == dq8_fn_entry[dq8_i] ? 0u : pc);
                        pc = fn_st.pc; n = fn_st.n; z = fn_st.z; c = fn_st.c; v = fn_st.v; ge = fn_st.ge; running = fn_st.running;
                        if (fn_exec) { ++dq8_fn_runs[dq8_i]; dq8_fn_insns[dq8_i] += fn_exec; steps += fn_exec - 1ull; continue; }
                    }
                }
            } else if (dq8_fn_stats_on) ++dq8_fn_map_miss;
        }
"#
        .to_string()
    };
    hook_pcs.sort_unstable();
    hook_pcs.dedup();
    prog.hook_pcs = hook_pcs;
    prog
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn localize_renames_registers_but_not_fpr() {
        let out = rewrite(
            0x1004,
            0x1000,
            0x1100,
            "fpr[16] = r[3] + r[13]; pc = 0x00001004; break;",
            "",
        )
        .unwrap();
        assert!(out.c.contains("fpr[16] = r3 + r13;"), "{}", out.c);
        assert!(out.c.contains("goto L_00001004;"), "{}", out.c);
    }

    #[test]
    fn return_becomes_exit() {
        let out = rewrite(
            0x1004,
            0x1000,
            0x1100,
            "if (r[14] == 0xFFFFFFFFu) running = 0; else pc = r[14]; break;",
            "",
        )
        .unwrap();
        assert!(out.c.contains("pc = r14; goto L_EXIT;"), "{}", out.c);
        assert!(out.touches_running);
    }

    #[test]
    fn conditional_return_needs_a_flag() {
        let out = rewrite(
            0x1004,
            0x1000,
            0x1100,
            "if (r[0]) { pc = r[14]; } else { pc = 0x00001008; } break;",
            "",
        )
        .unwrap();
        assert!(out.c.contains("_fn_ex = 1;"), "{}", out.c);
        assert!(out.c.contains("goto L_00001008;"), "{}", out.c);
        assert_eq!(out.exits.len(), 1);
        assert_eq!(out.exits[0].label, None);
    }

    #[test]
    fn out_of_function_target_exits_with_pc() {
        let out = rewrite(0x1004, 0x1000, 0x1100, "pc = 0x00123456; break;", "").unwrap();
        assert!(out.c.contains("pc = 0x00123456u;"), "{}", out.c);
        assert!(out.c.contains("goto L_EXIT;"), "{}", out.c);
    }

    #[test]
    fn indirect_pc_assignment_is_an_exit_not_a_refusal() {
        for stmt in [
            "pc = branch_indirect(r[3]); break;",
            "r[14] = 0x00160014u; pc = branch_indirect(r[2]); break;",
            "pc = (_dp_b) & ~3u; break;",
        ] {
            let out = rewrite(0x1004, 0x1000, 0x1100, stmt, "").unwrap_or_else(|e| panic!("{stmt}: {e}"));
            assert!(!out.opaque, "{stmt}: unexpectedly opaque");
            assert!(out.indirect, "{stmt}: not marked indirect");
            assert!(out.c.contains("goto L_EXIT;"), "{stmt}: {}", out.c);
        }
        // The pop-return idiom: the value is assigned first, then inspected,
        // then a trailing register write, so it needs the exit flag.
        let out = rewrite(
            0x1004,
            0x1000,
            0x1100,
            "pc = mem32(r[13] + 8u); if (pc == 0xFFFFFFFFu) running = 0; r[13] += 12u; break;",
            "",
        )
        .unwrap();
        assert!(!out.opaque);
        assert_eq!(out.exits.len(), 1);
        assert!(out.c.contains("r13 += 12u;"), "{}", out.c);
        assert!(!out.c.contains("st->r[14]=r14;"), "no spill needed for a plain load");
    }

    #[test]
    fn branch_indirect_spills_the_live_registers() {
        // The exact set is supplied by the emitter (t_21dda9b5); the rewrite
        // has to put it *before* the branch_indirect() call, so the validator
        // sees the values the statement has just produced.
        let spill = slots_spill_compact(&[2usize, 14usize].into_iter().collect());
        let out = rewrite(
            0x1004,
            0x1000,
            0x1100,
            "r[14] = 0x00160014u; pc = branch_indirect(r[2]); break;",
            &spill,
        )
        .unwrap();
        assert!(out.c.contains("st->r[14]=r14;"), "{}", out.c);
        assert!(out.c.contains("dq8_fn_branch_indirect(st, 0x00001004u, r2)"), "{}", out.c);
        let p_spill = out.c.find("st->r[14]=r14;").unwrap();
        let p_call = out.c.find("dq8_fn_branch_indirect").unwrap();
        assert!(p_spill < p_call, "spill must precede the validator: {}", out.c);
    }

    #[test]
    fn conditional_indirect_exit_keeps_the_other_arm() {
        let out = rewrite(
            0x1004,
            0x1000,
            0x1100,
            "if (!c) { { uint32_t _e=r[0]; pc = mem32(_e); if (pc == 0xFFFFFFFFu) running = 0; } } else { pc = 0x00001008; } break;",
            "",
        )
        .unwrap();
        assert_eq!(out.exits.len(), 1, "{}", out.c);
        assert!(out.c.contains("_fn_ex = 1;"), "{}", out.c);
        assert!(out.c.contains("goto L_00001008;"), "{}", out.c);
    }

    #[test]
    fn pc_read_is_an_opaque_exit() {
        let out = rewrite(0x1004, 0x1000, 0x1100, "r[0] = pc; pc = 0x00001004; break;", "").unwrap();
        assert!(out.opaque);
    }

    #[test]
    fn svc_is_an_opaque_exit() {
        let out = rewrite(
            0x1004,
            0x1000,
            0x1100,
            "svc_get_system_tick(r); pc = 0x00101004; break;",
            "",
        )
        .unwrap();
        assert!(out.opaque);
    }

    #[test]
    fn call_shape_is_recognised() {
        assert_eq!(
            simple_call(" { r[14] = 0x00160014u; pc = 0x001736FCu; break; } "),
            Some((0x00160014, 0x001736FC))
        );
        assert_eq!(
            simple_call(" { uint32_t a=r[0]; r[14] = 0x00160014u; pc = 0x001736FCu; break; } "),
            None
        );
        assert_eq!(simple_call(" { pc = 0x00160014u; break; }"), None);
    }

    #[test]
    fn scan_slots_finds_reads_and_writes() {
        let mut sets = FnSets::default();
        scan_slots(
            "uint32_t a=r[13] - 24; dq8_put32_fast(a + 0, r[3]); r[13] -= 24u;",
            &mut sets,
        );
        assert!(sets.reads.contains(&3), "r3 is read");
        assert!(sets.reads.contains(&13) && sets.writes.contains(&13), "r13 -= reads and writes");
        assert_eq!(sets.writes.len(), 1);

        let mut sets = FnSets::default();
        scan_slots(
            "if (!z) { uint32_t t = r[5] - 0u; z=(t==0); n=(t>>31); c=(r[5]>=0u); v=0; }",
            &mut sets,
        );
        assert!(sets.reads.contains(&16), "z read by the condition");
        assert!(sets.reads.contains(&5), "r5 read");
        for f in [15usize, 16, 17, 18] {
            assert!(sets.writes.contains(&f), "flag {f} written");
        }

        let mut sets = FnSets::default();
        scan_slots("if (!c) { pc = r[14]; } else { pc = 0x00001008u; }", &mut sets);
        assert!(sets.reads.contains(&17));
        assert!(sets.reads.contains(&14));
        assert!(sets.writes.is_empty());
    }

    #[test]
    fn scan_slots_ignores_memory_helpers_and_identifiers() {
        let mut sets = FnSets::default();
        scan_slots(
            "uint32_t _dp_cin=(uint32_t)c; fpr[16] = dq8_mem32_fast(r[13] + 4u); ge = 0;",
            &mut sets,
        );
        // fpr[16] must not be read as r16, and the helper names must not be
        // mistaken for flags.
        assert!(sets.reads.contains(&13));
        assert!(sets.reads.contains(&17));
        assert!(sets.writes.contains(&19));
        assert_eq!(sets.writes.len(), 1);
        assert_eq!(sets.reads.len(), 2);
    }

    #[test]
    fn scan_slots_handles_running_and_compound_ops() {
        let mut sets = FnSets::default();
        scan_slots("if (running) { r[0] |= r[1]; running = 0; }", &mut sets);
        assert!(sets.reads.contains(&20), "running read");
        assert!(sets.writes.contains(&20), "running written");
        assert!(sets.reads.contains(&0) && sets.writes.contains(&0), "|= reads and writes");
        assert!(sets.reads.contains(&1));
    }
}
