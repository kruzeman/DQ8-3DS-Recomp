//! S1b: emit native C for ONE straight-line Block-IR block.
//!
//! This module is the S1b deliverable of the S1 (hot-region native CFG)
//! roadmap item.  It turns one `block_ir::Block` (S1a's model) into a C
//! function that can be dropped into the generated interpreter next to the
//! dispatch switch.  Design constraints inherited from the S1 chain:
//!
//! * **Statements are reused, never re-derived.**  The frontend already
//!   lowers every instruction to a self-contained statement string (the
//!   `case 0x…: { … }` bodies of `translated_from`).  The emitter looks those
//!   exact strings up by address and splices them into the block function, so
//!   the emitted block has *by construction* the same register, flag and
//!   guest-memory semantics as the dispatch path — no second decoder, no
//!   inlining, no optimisation (that is S1c/S1d scope).
//! * **Fall-through only.**  The block function executes the block's
//!   instructions and returns with the pc/flags/regs the dispatch path would
//!   have produced; every exit (branch/call/return/indirect) is taken by the
//!   dispatcher on the next loop iteration.  No cross-block edges are
//!   compiled here.
//! * **Runtime env gate.**  The generated hook runs the block only when
//!   `DQ8_S1_BLOCK=<entry>` selects it at startup; with the gate unset the
//!   dispatcher behaves exactly as before.
//!
//! Safety net: an instruction whose statement references a `translated_from`
//! loop-local (`steps`, scheduling watermarks, …) cannot run inside the block
//! function, so such blocks are rejected at generation time rather than
//! silently diverging.

use crate::block_ir::{Block, Exit, Kind, Region};
use std::collections::{BTreeMap, BTreeSet};

/// Prefix of every emitted block function (`<prefix><entry hex>`).
pub const FN_PREFIX: &str = "dq8_s1_block_";

/// C identifier of the emitted block function for `entry`.
pub fn fn_name(entry: u32) -> String {
    format!("{FN_PREFIX}{entry:08X}")
}

/// Tokens that identify a statement as depending on `translated_from()`'s
/// loop-local state, which the emitted block function does not provide.
/// `running` is deliberately NOT listed: SVC statements legitimately clear it
/// and the block function passes it in/out.
const FORBIDDEN_TOKENS: &[&str] = &[
    "steps",
    "next_schedule_step",
    "next_pump_step",
    "next_progress_step",
    "trace_match",
    "trace_i",
    "context",
    "thread_execution_depth",
];

/// A successfully emitted block.
#[derive(Debug)]
#[cfg_attr(not(test), allow(dead_code))] // fields are asserted by unit tests
pub struct EmittedBlock {
    /// Entry PC (the block's start address) — the env-gate value.
    pub entry: u32,
    /// One past the block's last instruction.
    pub end: u32,
    /// Number of guest instructions in the block (dispatch-step span).
    pub insn_count: usize,
    /// Human-readable exit summary for the emitted-code comment.
    pub exit_summary: String,
    /// The C definitions (globals + block function + atexit reporter) to
    /// splice into the generated file before `translated_from`.
    pub function_c: String,
    /// The one-line dispatch hook to splice into the main loop.
    pub hook_c: String,
    /// The `main()` startup snippet that reads the `DQ8_S1_BLOCK` gate.
    pub gate_c: String,
}

const COND_NAMES: [&str; 16] = [
    "eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "al", "nv",
];

fn cond_name(cond: u8) -> &'static str {
    COND_NAMES
        .get(cond as usize)
        .copied()
        .unwrap_or("??")
}

fn describe_exit(exit: &Exit) -> String {
    match exit {
        Exit::FallThrough { next } => format!("fallthrough=0x{next:08X}"),
        Exit::Branch {
            target,
            cond,
            fallthrough,
        } => format!(
            "branch target=0x{target:08X} cond={}{ft}",
            cond_name(*cond),
            ft = match fallthrough {
                Some(ft) => format!(" fallthrough=0x{ft:08X}"),
                None => String::new(),
            }
        ),
        Exit::Call {
            target,
            cond,
            fallthrough,
        } => format!(
            "call target=0x{target:08X} cond={}{ft}",
            cond_name(*cond),
            ft = match fallthrough {
                Some(ft) => format!(" fallthrough=0x{ft:08X}"),
                None => String::new(),
            }
        ),
        Exit::Return => "return".to_string(),
        Exit::Indirect => "indirect".to_string(),
        Exit::End => "end-of-region".to_string(),
    }
}

/// Emit the C for one straight-line basic block.
///
/// `emitted` is the frontend's per-PC statement map (`addr -> case body`);
/// every instruction of the block must have an entry there, and its statement
/// must not reference dispatch-loop locals.
pub fn emit_block(block: &Block, emitted: &BTreeMap<u32, String>) -> Result<EmittedBlock, String> {
    if block.insns.is_empty() {
        return Err(format!("block 0x{:08X} is empty", block.start));
    }
    if block.start & 3 != 0 {
        return Err(format!("block 0x{:08X} is not word aligned", block.start));
    }
    match block.exit {
        // A block that runs to the extraction window boundary (or to the end
        // of the region) is a truncated window, not a terminated basic block:
        // refusing keeps the emitter honest instead of emitting a partial
        // block whose exit semantics the dispatcher would mis-handle.
        Exit::FallThrough { .. } | Exit::End => {
            return Err(format!(
                "block 0x{:08X} exit is {} — not a terminated basic block",
                block.start,
                describe_exit(&block.exit)
            ));
        }
        _ => {}
    }

    let mut body = String::new();
    for insn in &block.insns {
        let stmt = emitted.get(&insn.addr).ok_or_else(|| {
            format!(
                "block instruction 0x{:08X} has no emitted dispatch case",
                insn.addr
            )
        })?;
        for token in FORBIDDEN_TOKENS {
            if stmt.contains(token) {
                return Err(format!(
                    "instruction 0x{:08X} statement references dispatch-loop local `{token}`; \
                     refusing to emit this block",
                    insn.addr
                ));
            }
        }
        body.push_str(&format!("        {{ {stmt} }}\n"));
    }

    let entry = block.start;
    let name = fn_name(entry);
    let exit_summary = describe_exit(&block.exit);
    let insn_count = block.insns.len();
    // The dispatch loop retires one step per guest instruction; running the
    // whole block inside one iteration must advance `steps` by the same
    // amount, so the hook adds (count - 1) on top of the loop's own ++.
    let steps_credit = insn_count - 1;

    let function_c = format!(
        "/* S1b emitted straight-line block 0x{entry:08X}..0x{end:08X} ({count} insns, exit: {exit_summary}).\n\
         Statements are the dispatch path's own per-instruction cases, reused verbatim (same\n\
         helpers, same flag/memory semantics); no inlining, no cross-block edges: after the\n\
         block returns, the dispatcher continues at whatever pc the dispatch path would have\n\
         produced.  Runtime gate: DQ8_S1_BLOCK=0x{entry:08X}. */\n\
         static unsigned long long dq8_s1_block_runs;\n\
         static int dq8_s1_block_enabled;\n\
         static void dq8_s1_block_report(void) {{\n\
         \x20   if (dq8_s1_block_enabled)\n\
         \x20       fprintf(stderr, \"S1_BLOCK entry=0x{entry:08X} runs=%llu\\n\", dq8_s1_block_runs);\n\
         }}\n\
         static void {name}(uint32_t *r, uint32_t *pc_io, int *n_io, int *z_io,\n\
         \x20               int *c_io, int *v_io, int *ge_io, int *running_io) {{\n\
         \x20   uint32_t pc = *pc_io;\n\
         \x20   int n = *n_io, z = *z_io, c = *c_io, v = *v_io, ge = *ge_io, running = *running_io;\n\
         \x20   (void)ge; (void)running;\n\
         {body}\
         \x20   *pc_io = pc; *n_io = n; *z_io = z; *c_io = c; *v_io = v; *ge_io = ge; *running_io = running;\n\
         }}\n",
        entry = entry,
        end = block.end,
        count = insn_count,
        exit_summary = exit_summary,
        name = name,
        body = body,
    );

    let hook_c = format!(
        "        if (dq8_s1_block_enabled && pc == 0x{entry:08X}u) {{ ++dq8_s1_block_runs; \
         {name}(r, &pc, &n, &z, &c, &v, &ge, &running); steps += {credit}ull; continue; }}\n",
        entry = entry,
        name = name,
        credit = steps_credit,
    );

    let gate_c = format!(
        "    {{ const char *s1_text = getenv(\"DQ8_S1_BLOCK\");\n\
         \x20     if (s1_text && *s1_text) {{\n\
         \x20         unsigned long long s1_entry = strtoull(s1_text, NULL, 0);\n\
         \x20         dq8_s1_block_enabled = (s1_entry == 0x{entry:08X}ull);\n\
         \x20         if (!dq8_s1_block_enabled)\n\
         \x20             fprintf(stderr, \"DQ8_S1_BLOCK=%s does not match the emitted block entry 0x{entry:08X}; hook disabled\\n\", s1_text);\n\
         \x20     }}\n\
         \x20     if (dq8_s1_block_enabled) atexit(dq8_s1_block_report); }}\n",
        entry = entry,
    );

    Ok(EmittedBlock {
        entry,
        end: block.end,
        insn_count,
        exit_summary,
        function_c,
        hook_c,
        gate_c,
    })
}

// ---------------------------------------------------------------------------
// S1c: chain ALL of a region's blocks into ONE native-CFG function.
// ---------------------------------------------------------------------------

/// Prefix of every emitted region-CFG function (`<prefix><entry hex>`).
pub const CFG_FN_PREFIX: &str = "dq8_s1_cfg_";

/// C identifier of the emitted region-CFG function for `entry`.
pub fn cfg_fn_name(entry: u32) -> String {
    format!("{CFG_FN_PREFIX}{entry:08X}")
}

/// C label for a block inside the region function.
fn label(addr: u32) -> String {
    format!("L_{addr:08X}")
}

/// A successfully emitted region CFG.
#[derive(Debug)]
#[cfg_attr(not(test), allow(dead_code))] // fields are asserted by unit tests
pub struct EmittedRegion {
    /// Region entry PC — the env-gate value.
    pub entry: u32,
    /// One past the region's last instruction.
    pub end: u32,
    /// Number of basic blocks chained by direct edges.
    pub block_count: usize,
    /// Static guest-instruction count of the region.
    pub insn_count: usize,
    /// Compiled direct edges between two region blocks (incl. back edges).
    pub intra_edges: usize,
    /// Compiled edges that leave the region (fall back to the dispatcher).
    pub exit_edges: usize,
    /// Subset of `intra_edges` whose target block starts at or before its
    /// source (the loop-closing edges).
    pub back_edges: usize,
    /// The C definitions (globals + region function + atexit reporter) to
    /// splice into the generated file before `translated_from`.
    pub function_c: String,
    /// The one-line dispatch hook to splice into the main loop.
    pub hook_c: String,
    /// The `main()` startup snippet that reads the `DQ8_S1_CFG` gate.
    pub gate_c: String,
}

/// Emit one region's basic blocks as a single native-CFG C function.
///
/// Same reuse rule as S1b: every instruction statement is the dispatch path's
/// own `case 0x…` body, spliced verbatim (same helpers, same flag/memory
/// semantics, no inlining, no constant propagation).  What S1c adds over S1b
/// is the *edges*: a block exit that stays inside the region compiles to a
/// direct `goto` of the destination block's label, so **no instruction
/// dispatch happens between two region blocks** — the loop that used to leave
/// and re-enter `translated_from` through the switch now closes inside the
/// function.  Every edge that leaves the region (branch/call target outside,
/// return, indirect) compiles to `goto L_EXIT`, and the function returns with
/// the pc/flags/regs the dispatch path would have produced, so the dispatcher
/// resumes exactly where it would have.
///
/// Fall-through exits (R2): a block whose last instruction is not control
/// flow — a non-PC `ldmia`/`stmia` lead-in, or a block cut at the next
/// instruction's leader — is *also* a direct edge: the reused statement has
/// already set `pc` to the sequential address, which `extract_region`
/// guarantees is either the next block's start (compile to `goto L_next`) or
/// the region end (compile to `goto L_EXIT`, `Exit::End`).  Two guards keep
/// that honest: the last statement must actually set `pc` (refused otherwise),
/// and if it touches `running` (e.g. an SVC that stops the guest) the edge
/// goes to `L_EXIT` instead of the next block, so the dispatcher's per-
/// iteration `running` check is observed before any further instruction.
///
/// Safety nets beyond S1b's (loop-local references):
/// * an `Unknown` instruction has no verified semantics -> refused;
/// * a mid-block statement that touches `running` would only be observed when
///   the region finally exits -> refused (the block terminator may clear it,
///   e.g. `ldr pc,[sp],#4` with the sentinel return address);
/// * an exit target inside the region that is not a block start -> refused
///   (dispatch could legally enter mid-block; the CFG cannot).
pub fn emit_region(
    region: &Region,
    emitted: &BTreeMap<u32, String>,
) -> Result<EmittedRegion, String> {
    if region.blocks.is_empty() {
        return Err("region is empty".to_string());
    }
    if region.start & 3 != 0 {
        return Err(format!(
            "region entry 0x{:08X} is not word aligned",
            region.start
        ));
    }
    let starts: BTreeSet<u32> = region.blocks.iter().map(|b| b.start).collect();
    // Structural validation first: an empty or misaligned block is refused
    // for its shape, before any statement lookup, so the error names the real
    // problem.  Fall-through/end exits are no longer refused here (R2): they
    // compile as direct edges, guarded per block in the emit loop below.
    for block in &region.blocks {
        if block.insns.is_empty() {
            return Err(format!("block 0x{:08X} is empty", block.start));
        }
    }
    let mut body = String::new();
    let mut insn_count = 0usize;
    let mut intra_edges = 0usize;
    let mut exit_edges = 0usize;
    let mut back_edges = 0usize;

    for block in &region.blocks {
        let count = block.insns.len();
        insn_count += count;
        body.push_str(&format!("{}:\n        exec += {count}ull;\n", label(block.start)));
        let mut last_stmt: Option<String> = None;
        for (i, insn) in block.insns.iter().enumerate() {
            if matches!(insn.kind, Kind::Unknown) {
                return Err(format!(
                    "instruction 0x{:08X} decodes as Unknown (no verified semantics); \
                     refusing to emit this region",
                    insn.addr
                ));
            }
            let stmt = emitted.get(&insn.addr).ok_or_else(|| {
                format!(
                    "block instruction 0x{:08X} has no emitted dispatch case",
                    insn.addr
                )
            })?;
            for token in FORBIDDEN_TOKENS {
                if stmt.contains(token) {
                    return Err(format!(
                        "instruction 0x{:08X} statement references dispatch-loop local `{token}`; \
                         refusing to emit this region",
                        insn.addr
                    ));
                }
            }
            if i + 1 != count && stmt.contains("running") {
                return Err(format!(
                    "instruction 0x{:08X} statement touches `running` mid-block; \
                     refusing to emit this region",
                    insn.addr
                ));
            }
            body.push_str(&format!("        {{ {stmt} }}\n"));
            last_stmt = Some(stmt.clone());
        }
        // A fall-through/end exit reuses the last statement's already-set pc;
        // that is only sound if the statement really does set pc, and the
        // dispatcher's per-iteration `running` check must be observed before
        // the edge can stay inside the region (region_exit_edges routes a
        // running-clearing terminator to L_EXIT instead).
        let last_stmt = last_stmt.expect("block is non-empty");
        if matches!(block.exit, Exit::FallThrough { .. } | Exit::End)
            && !last_stmt.contains("pc")
        {
            return Err(format!(
                "block 0x{:08X} exits by fall-through but its last statement does not set pc; \
                 refusing to emit this region",
                block.start
            ));
        }
        let last_touches_running = last_stmt.contains("running");
        let (edges, ie, ee, be) =
            region_exit_edges(&block.exit, block.start, region, &starts, last_touches_running)?;
        intra_edges += ie;
        exit_edges += ee;
        back_edges += be;
        body.push_str(&edges);
    }

    // S1b-prototype self-check: a REFERENCE function that replays the same
    // per-instruction statements through a per-instruction switch — i.e. the
    // dispatch path, minus the dispatcher's bookkeeping.  Both arms start from
    // the same state and are compared (regs, flags, pc, running, exec count);
    // it is emitted into every --s1-cfg build but only called when
    // DQ8_S1_SELFCHECK is set, so the default generated C is unaffected.
    let mut ref_body = String::new();
    for block in &region.blocks {
        for insn in &block.insns {
            let stmt = emitted
                .get(&insn.addr)
                .expect("validated above: every region instruction has a case");
            ref_body.push_str(&format!(
                "        case 0x{:08X}u: {{ {stmt} }} ++exec; break;\n",
                insn.addr
            ));
        }
    }

    let entry = region.start;

    let end = region.end;
    let name = cfg_fn_name(entry);
    // Per-entry static names: a rollout build emits SEVERAL regions into one
    // generated file, so every global this region owns carries its entry.
    let s_runs = format!("dq8_s1_cfg_runs_{entry:08X}");
    let s_insns = format!("dq8_s1_cfg_insns_{entry:08X}");
    let s_enabled = format!("dq8_s1_cfg_enabled_{entry:08X}");
    let s_report = format!("dq8_s1_cfg_report_{entry:08X}");
    // S1b-prototype self-check state (per region, like the other statics).
    let s_selfcheck = format!("dq8_s1_cfg_selfcheck_{entry:08X}");
    let s_sc_ok = format!("dq8_s1_cfg_sc_ok_{entry:08X}");
    let s_sc_bad = format!("dq8_s1_cfg_sc_bad_{entry:08X}");
    let block_count = region.blocks.len();
    let exits: String = region
        .blocks
        .iter()
        .map(|b| format!("\n *   0x{:08X} -> {}", b.start, describe_exit(&b.exit)))
        .collect();

    let function_c = format!(
        "/* S1c emitted native-CFG region 0x{entry:08X}..0x{end:08X}: {block_count} blocks, \
         {insn_count} guest instructions, {intra_edges} direct intra-region edge(s) ({back_edges} \
         loop-closing), {exit_edges} edge(s) that leave the region and fall back to the dispatcher.\n\
         \x20* Block exits:{exits}\n\
         \x20* Statements are the dispatch path's own per-instruction cases, reused verbatim (same\n\
         \x20* helpers, same flag/memory semantics); no inlining, no constant propagation.  Between\n\
         \x20* two region blocks there is NO instruction dispatch: the edges are direct gotos.\n\
         \x20* Runtime gate: DQ8_S1_CFG=0x{entry:08X}. */\n\
         static unsigned long long {s_runs};\n\
         static unsigned long long {s_insns};\n\
         static int {s_enabled};\n\
         static int {s_selfcheck};\n\
         static unsigned long long {s_sc_ok};\n\
         static unsigned long long {s_sc_bad};\n\
         static void {s_report}(void) {{\n\
         \x20   if ({s_enabled})\n\
         \x20       fprintf(stderr, \"S1_CFG entry=0x{entry:08X} runs=%llu insns=%llu\\n\",\n\
         \x20               {s_runs}, {s_insns});\n\
         \x20   if ({s_selfcheck})\n\
         \x20       fprintf(stderr, \"S1_SELFCHECK entry=0x{entry:08X} ok=%llu bad=%llu\\n\",\n\
         \x20               {s_sc_ok}, {s_sc_bad});\n\
         }}\n\
         static unsigned long long {name}(uint32_t *r, uint32_t *pc_io, int *n_io, int *z_io,\n\
         \x20                              int *c_io, int *v_io, int *ge_io, int *running_io) {{\n\
         \x20   uint32_t pc = *pc_io;\n\
         \x20   int n = *n_io, z = *z_io, c = *c_io, v = *v_io, ge = *ge_io, running = *running_io;\n\
         \x20   (void)ge; (void)running;\n\
         \x20   unsigned long long exec = 0;\n\
         {body}\n\
         L_EXIT:\n\
         \x20   *pc_io = pc; *n_io = n; *z_io = z; *c_io = c; *v_io = v; *ge_io = ge; *running_io = running;\n\
         \x20   return exec;\n\
         }}\n",
        entry = entry,
        end = end,
        block_count = block_count,
        insn_count = insn_count,
        intra_edges = intra_edges,
        exit_edges = exit_edges,
        back_edges = back_edges,
        exits = exits,
        name = name,
        body = body,
    );
    let ref_c = format!(
        "/* S1b-prototype self-check reference for region 0x{entry:08X}: the same per-instruction\n\
         \x20* statements, dispatched one by one (the instruction path) instead of chained through\n\
         \x20* direct edges.  Called only when DQ8_S1_SELFCHECK is set; the dispatch hook runs it\n\
         \x20* on the SAME saved input state and compares regs/flags/pc/exec with the region arm. */\n\
         static unsigned long long {name}_ref(uint32_t *r, uint32_t *pc_io, int *n_io, int *z_io,\n\
         \x20                                  int *c_io, int *v_io, int *ge_io, int *running_io) {{\n\
         \x20   uint32_t pc = *pc_io;\n\
         \x20   int n = *n_io, z = *z_io, c = *c_io, v = *v_io, ge = *ge_io, running = *running_io;\n\
         \x20   (void)ge; (void)running;\n\
         \x20   unsigned long long exec = 0;\n\
         \x20   for (;;) {{\n\
         \x20       switch (pc) {{\n\
         {ref_body}\n\
         \x20       default: goto L_REF_RET;\n\
         \x20       }}\n\
         \x20       if (pc < 0x{entry:08X}u || pc >= 0x{end:08X}u) break;\n\
         \x20       if (!running) break;\n\
         \x20   }}\n\
         L_REF_RET:\n\
         \x20   *pc_io = pc; *n_io = n; *z_io = z; *c_io = c; *v_io = v; *ge_io = ge; *running_io = running;\n\
         \x20   return exec;\n\
         }}\n",
        entry = entry,
        end = end,
        name = name,
        ref_body = ref_body,
    );
    let function_c = format!("{function_c}{ref_c}");

    // The dispatch loop retires one step per guest instruction; the region
    // function executes `exec` of them inside one iteration, so the hook adds
    // (exec - 1) on top of the loop's own ++.  `exec` is exact: each block
    // credits its static instruction count when it is entered.
    let hook_c = format!(
        "        case 0x{entry:08X}u: {{ if (!{s_enabled}) break; ++{s_runs}; \\\n\
         \x20     unsigned long long s1_exec; \\\n\
         \x20     if ({s_selfcheck}) {{ \\\n\
         \x20         uint32_t s1_sr[16], s1_nr[16], s1_spc = pc, s1_npc; \\\n\
         \x20         int s1_sn = n, s1_sz = z, s1_sc = c, s1_sv = v, s1_sge = ge, s1_srun = running; \\\n\
         \x20         int s1_nn, s1_nz, s1_nc, s1_nv, s1_nge, s1_nrun; \\\n\
         \x20         memcpy(s1_sr, r, sizeof s1_sr); \\\n\
         \x20         s1_exec = {name}(r, &pc, &n, &z, &c, &v, &ge, &running); \\\n\
         \x20         memcpy(s1_nr, r, sizeof s1_nr); \\\n\
         \x20         s1_npc = pc; s1_nn = n; s1_nz = z; s1_nc = c; s1_nv = v; s1_nge = ge; s1_nrun = running; \\\n\
         \x20         memcpy(r, s1_sr, sizeof s1_sr); \\\n\
         \x20         pc = s1_spc; n = s1_sn; z = s1_sz; c = s1_sc; v = s1_sv; ge = s1_sge; running = s1_srun; \\\n\
         \x20         unsigned long long s1_rexec = {name}_ref(r, &pc, &n, &z, &c, &v, &ge, &running); \\\n\
         \x20         int s1_diff = (pc != s1_npc) || (n != s1_nn) || (z != s1_nz) || (c != s1_nc) || \\\n\
         \x20                       (v != s1_nv) || (ge != s1_nge) || (running != s1_nrun) || (s1_rexec != s1_exec); \\\n\
         \x20         unsigned s1_reg = 16u, s1_rv = 0u; \\\n\
         \x20         for (unsigned s1_i = 0; s1_i < 16u; ++s1_i) \\\n\
         \x20             if (r[s1_i] != s1_nr[s1_i]) {{ if (!s1_diff) {{ s1_diff = 1; s1_reg = s1_i; s1_rv = r[s1_i]; }} }} \\\n\
         \x20         if (s1_diff) {{ \\\n\
         \x20             ++{s_sc_bad}; \\\n\
         \x20             if ({s_sc_bad} <= 8u) \\\n\
         \x20       fprintf(stderr, \"S1_SELFCHECK DIVERGENCE entry=0x{entry:08X} native_pc=%08X ref_pc=%08X native_exec=%llu ref_exec=%llu reg=r%u ref=%08X native=%08X nzcv=%d%d%d%d/%d%d%d%d running=%d/%d\\n\",\n\
         \x20                         s1_npc, pc, s1_exec, s1_rexec, s1_reg, s1_rv, \\\n\
         \x20                         (s1_reg < 16u ? s1_nr[s1_reg] : 0u), \\\n\
         \x20                         s1_nn, s1_nz, s1_nc, s1_nv, n, z, c, v, s1_nrun, running); \\\n\
         \x20         }} else ++{s_sc_ok}; \\\n\
         \x20         memcpy(r, s1_nr, sizeof s1_nr); \\\n\
         \x20         pc = s1_npc; n = s1_nn; z = s1_nz; c = s1_nc; v = s1_nv; ge = s1_nge; running = s1_nrun; \\\n\
         \x20     }} else {{ \\\n\
         \x20         s1_exec = {name}(r, &pc, &n, &z, &c, &v, &ge, &running); \\\n\
         \x20     }} \\\n\
         \x20     {s_insns} += s1_exec; steps += s1_exec - 1ull; continue; }}\n",
        entry = entry,
        name = name,
        s_selfcheck = s_selfcheck,
        s_sc_ok = s_sc_ok,
        s_sc_bad = s_sc_bad,
    );

    // Built without backslash-escaped quotes: `q` is a double quote, so the
    // emitted C cannot pick up stray backslashes from Rust string escaping.
    // The gate parses a LIST (comma/colon/semicolon separated, or the token
    // `all`) so one build can carry several converted regions; a single value
    // behaves exactly like the S1c single-region gate.
    let q = '"';
    let gate_c = format!(
        "    {{ const char *s1_list = getenv({q}DQ8_S1_CFG{q});\n\
         \x20     {{ const char *s1_sc = getenv({q}DQ8_S1_SELFCHECK{q}); {s_selfcheck} = (s1_sc && *s1_sc && *s1_sc != '0') ? 1 : 0; }}\n\
         \x20     if (s1_list && *s1_list) {{\n\
         \x20         int s1_hit = 0, s1_toks = 0;\n\
         \x20         const char *s1_p = s1_list;\n\
         \x20         while (*s1_p) {{\n\
         \x20             const char *s1_q = s1_p;\n\
         \x20             while (*s1_q && *s1_q != ',' && *s1_q != ':' && *s1_q != ';') ++s1_q;\n\
         \x20             char s1_tok[40];\n\
         \x20             size_t s1_len = (size_t)(s1_q - s1_p);\n\
         \x20             if (s1_len >= sizeof s1_tok) s1_len = sizeof s1_tok - 1u;\n\
         \x20             memcpy(s1_tok, s1_p, s1_len); s1_tok[s1_len] = 0;\n\
         \x20             if (s1_len) {{\n\
         \x20                 ++s1_toks;\n\
         \x20                 if (!strcmp(s1_tok, {q}all{q})) s1_hit = 1;\n\
         \x20                 else {{ char *s1_e = 0; unsigned long long s1_v = strtoull(s1_tok, &s1_e, 0);\n\
         \x20                        if (s1_e != s1_tok && s1_v == 0x{entry:08X}ull) s1_hit = 1; }}\n\
         \x20             }}\n\
         \x20             s1_p = (*s1_q ? s1_q + 1 : s1_q);\n\
         \x20         }}\n\
         \x20         {s_enabled} = s1_hit;\n\
         \x20         if (s1_hit) atexit({s_report});\n\
         \x20         else if (s1_toks == 1)\n\
         \x20             fprintf(stderr, {q}DQ8_S1_CFG=%s does not match the emitted region entry 0x{entry:08X}; hook disabled\\n{q}, s1_list);\n\
         \x20     }} }}\n",
        entry = entry,
        q = q,
        s_enabled = s_enabled,
        s_report = s_report,
        s_selfcheck = s_selfcheck,
    );

    Ok(EmittedRegion {
        entry,
        end,
        block_count,
        insn_count,
        intra_edges,
        exit_edges,
        back_edges,
        function_c,
        hook_c,
        gate_c,
    })
}

/// The goto-edges for one block exit.
///
/// Returns `(code, intra_edges, exit_edges, back_edges)`.  Both outgoing edges
/// of a conditional exit are compiled; when they land on the same label the
/// condition is dropped (the reused statement already set `pc` identically on
/// both paths, so an unconditional jump is equivalent).
///
/// `last_touches_running` is true when the block's final statement writes the
/// dispatcher's `running` flag (an SVC, or a store that stops the guest): a
/// fall-through edge may then not stay inside the region, because the
/// dispatcher checks `running` between every two instructions — such an edge
/// is compiled to `L_EXIT` instead, and the dispatcher observes the flag
/// before whatever pc the statement left behind.
#[allow(clippy::too_many_arguments)]
fn region_exit_edges(
    exit: &Exit,
    from: u32,
    region: &Region,
    starts: &BTreeSet<u32>,
    last_touches_running: bool,
) -> Result<(String, usize, usize, usize), String> {
    // One edge -> (code, is_intra, is_back_edge).
    fn edge(
        target: u32,
        from: u32,
        region: &Region,
        starts: &BTreeSet<u32>,
    ) -> Result<(String, bool, bool), String> {
        if starts.contains(&target) {
            Ok((format!("goto {};", label(target)), true, target <= from))
        } else if target >= region.start && target < region.end {
            // Dispatch may legally arrive mid-block; the CFG has no label
            // there, so refuse instead of silently redirecting.
            Err(format!(
                "exit target 0x{target:08X} lies inside the region but is not a block start"
            ))
        } else {
            Ok(("goto L_EXIT;".to_string(), false, false))
        }
    }
    fn tally(code: String, intra: bool, back: bool) -> (String, usize, usize, usize) {
        (code, intra as usize, (!intra) as usize, back as usize)
    }

    match *exit {
        Exit::Branch {
            target,
            cond,
            fallthrough,
        }
        | Exit::Call {
            target,
            cond,
            fallthrough,
        } => {
            let Some(ft) = fallthrough else {
                let (code, intra, back) = edge(target, from, region, starts)?;
                return Ok(tally(format!("        {code}\n"), intra, back));
            };
            // cond 15 (NV) is never taken by the dispatch statement itself.
            if cond == 15 {
                let (code, intra, back) = edge(ft, from, region, starts)?;
                return Ok(tally(format!("        {code}\n"), intra, back));
            }
            let (t_code, t_intra, t_back) = edge(target, from, region, starts)?;
            let (f_code, f_intra, f_back) = edge(ft, from, region, starts)?;
            if t_code == f_code {
                return Ok(tally(t_code, t_intra, t_back));
            }
            if cond == 14 {
                return Ok(tally(t_code, t_intra, t_back));
            }
            let cond_expr = crate::arm_condition_c(u32::from(cond));
            Ok((
                format!("        if ({cond_expr}) {t_code}\n        {f_code}\n"),
                (t_intra as usize) + (f_intra as usize),
                (!t_intra as usize) + (!f_intra as usize),
                (t_back as usize) + (f_back as usize),
            ))
        }
        // The statement already left pc/running where the dispatcher would
        // find them; returning to the dispatcher is the whole edge.
        Exit::Return | Exit::Indirect => Ok(("        goto L_EXIT;\n".to_string(), 0, 1, 0)),
        // R2: fall-through/end exits are direct edges.  The reused last
        // statement has already set `pc` to the sequential address (checked by
        // emit_region), which is the next block's start or the region end —
        // so the same `edge()` resolution applies as for a branch target.
        // A terminator that touched `running` must be observed by the
        // dispatcher before any further instruction runs, so it exits.
        Exit::FallThrough { next } => {
            if last_touches_running {
                return Ok(("        goto L_EXIT;\n".to_string(), 0, 1, 0));
            }
            let (code, intra, back) = edge(next, from, region, starts)?;
            Ok(tally(format!("        {code}\n"), intra, back))
        }
        // The block ends at the region boundary with pc already set past it;
        // L_EXIT hands that pc back to the dispatcher unchanged.
        Exit::End => Ok(("        goto L_EXIT;\n".to_string(), 0, 1, 0)),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::block_ir::{extract_region, GUEST_BASE};

    const REGION_START: u32 = 0x001A_93A8;
    const REGION_END: u32 = 0x001A_9410;
    /// The S1b target block: block 1 of the Perf-G movie region.
    const BLOCK_ENTRY: u32 = 0x001A_93B4;

    fn real_bytes() -> Option<Vec<u8>> {
        std::fs::read("DQ8.code").ok()
    }

    /// Re-derive the frontend's own emitted statements for the block by
    /// recompiling the real region and scraping the `case 0x…` bodies — this
    /// keeps the unit test independent of main.rs internals while still
    /// testing against the real dispatch lowering.
    fn scrape_emitted(case_source: &str) -> BTreeMap<u32, String> {
        let mut map = BTreeMap::new();
        for line in case_source.lines() {
            let Some(rest) = line.trim().strip_prefix("case 0x") else {
                continue;
            };
            let Some(hex_end) = rest.find(':') else {
                continue;
            };
            let Ok(addr) = u32::from_str_radix(&rest[..hex_end], 16) else {
                continue;
            };
            let Some(open) = rest.find("{ ") else {
                continue;
            };
            let Some(close) = rest.rfind(" break; }") else {
                continue;
            };
            if close > open {
                map.insert(addr, rest[open + 2..close].to_string());
            }
        }
        map
    }

    #[test]
    fn emits_the_real_movie_block_from_the_real_dispatch_cases() {
        let Some(bytes) = real_bytes() else {
            eprintln!("skipping: DQ8.code not present next to the crate root");
            return;
        };
        // The frontend's per-PC cases for the region, obtained by running the
        // recompiler on the region entry and scraping its emitted switch.
        let tmp = std::env::temp_dir().join(format!("dq8-s1b-case-{}.c", std::process::id()));
        crate::recompile_arm_function(
            std::path::Path::new("DQ8.code"),
            &tmp,
            GUEST_BASE,
            REGION_START,
            &[],
        )
        .expect("recompile region");
        let text = std::fs::read_to_string(&tmp).expect("read generated");
        let _ = std::fs::remove_file(&tmp);
        let emitted = scrape_emitted(&text);
        assert!(emitted.contains_key(&BLOCK_ENTRY), "case map missing block entry");

        let region = extract_region(&bytes, GUEST_BASE, REGION_START, REGION_END).expect("extract");
        let block = region
            .blocks
            .iter()
            .find(|b| b.start == BLOCK_ENTRY)
            .expect("block 1 present");
        let out = emit_block(block, &emitted).expect("emit");
        assert_eq!(out.entry, BLOCK_ENTRY);
        assert_eq!(out.end, 0x001A_93E0);
        assert_eq!(out.insn_count, 11);
        assert!(out.exit_summary.contains("call target=0x001A7468 cond=lt"));
        // Spot-check the reused dispatch statements inside the block body.
        assert!(out.function_c.contains("r[4] = _dp_b; pc = 0x001A93B8;"));
        assert!(out.function_c.contains("r[4] = mem16("));
        assert!(out.function_c.contains("r[14] = 0x001A93E0u"));
        assert!(out.function_c.contains("steps") == false);
        // The hook retires the block as 11 dispatch steps.
        assert!(out.hook_c.contains("steps += 10ull;"));
        assert!(out.hook_c.contains("pc == 0x001A93B4u"));
        assert!(out.gate_c.contains("DQ8_S1_BLOCK"));
    }

    #[test]
    fn truncated_fallthrough_blocks_are_rejected() {
        let Some(bytes) = real_bytes() else {
            eprintln!("skipping: DQ8.code not present next to the crate root");
            return;
        };
        // Extract a window that cuts the region mid-block: block 1's tail is
        // outside, so the last block falls through the window edge.
        let region =
            extract_region(&bytes, GUEST_BASE, REGION_START, REGION_START + 0x20).expect("extract");
        let last = region.blocks.last().expect("block");
        let err = emit_block(last, &BTreeMap::new()).expect_err("must reject");
        assert!(err.contains("not a terminated basic block"), "{err}");
    }

    #[test]
    fn missing_dispatch_case_is_rejected() {
        let Some(bytes) = real_bytes() else {
            eprintln!("skipping: DQ8.code not present next to the crate root");
            return;
        };
        let region =
            extract_region(&bytes, GUEST_BASE, REGION_START, REGION_END).expect("extract");
        let block = region
            .blocks
            .iter()
            .find(|b| b.start == BLOCK_ENTRY)
            .expect("block");
        let err = emit_block(block, &BTreeMap::new()).expect_err("must reject");
        assert!(err.contains("no emitted dispatch case"), "{err}");
    }

    #[test]
    fn statements_touching_dispatch_loop_locals_are_rejected() {
        let Some(bytes) = real_bytes() else {
            eprintln!("skipping: DQ8.code not present next to the crate root");
            return;
        };
        let region =
            extract_region(&bytes, GUEST_BASE, REGION_START, REGION_END).expect("extract");
        let block = region
            .blocks
            .iter()
            .find(|b| b.start == BLOCK_ENTRY)
            .expect("block");
        let mut map = BTreeMap::new();
        for insn in &block.insns {
            map.insert(insn.addr, "steps += 1;".to_string());
        }
        let err = emit_block(block, &map).expect_err("must reject");
        assert!(err.contains("`steps`"), "{err}");
    }

    // --- S1c: the region CFG -------------------------------------------------

    /// The frontend's per-PC cases for the real Perf-G region.
    fn real_region_cases(tag: &str) -> Option<(Vec<u8>, crate::block_ir::Region, BTreeMap<u32, String>)> {
        let bytes = real_bytes()?;
        let tmp = std::env::temp_dir().join(format!("dq8-s1c-case-{}-{tag}.c", std::process::id()));
        crate::recompile_arm_function(
            std::path::Path::new("DQ8.code"),
            &tmp,
            GUEST_BASE,
            REGION_START,
            &[],
        )
        .expect("recompile region");
        let text = std::fs::read_to_string(&tmp).expect("read generated");
        let _ = std::fs::remove_file(&tmp);
        let emitted = scrape_emitted(&text);
        let region = extract_region(&bytes, GUEST_BASE, REGION_START, REGION_END).expect("extract");
        Some((bytes, region, emitted))
    }

    #[test]
    fn emits_the_real_movie_region_as_one_native_cfg() {
        let Some((_bytes, region, emitted)) = real_region_cases("emit") else {
            eprintln!("skipping: DQ8.code not present next to the crate root");
            return;
        };
        let out = emit_region(&region, &emitted).expect("emit");
        assert_eq!(out.entry, REGION_START);
        assert_eq!(out.end, REGION_END);
        assert_eq!(out.block_count, 4);
        assert_eq!(out.insn_count, 26);
        // 4 direct intra-region edges (B0->B1, B1->B2, B2->B3, B2->B0 back
        // edge) and 3 edges that leave the region (B0 branch-out, B1 call-out,
        // B3 return).
        assert_eq!(out.intra_edges, 4);
        assert_eq!(out.back_edges, 1);
        assert_eq!(out.exit_edges, 3);
        // Every block has a label, and the loop back-edge is a direct goto.
        for entry in [0x001A_93A8u32, 0x001A_93B4, 0x001A_93E0, 0x001A_940C] {
            assert!(out.function_c.contains(&format!("L_{entry:08X}:" )), "missing label for {entry:08X}");
        }
        assert!(out.function_c.contains("if (z) goto L_001A93A8;"));
        assert!(out.function_c.contains("goto L_001A93B4;"));
        assert!(out.function_c.contains("if (n != v) goto L_EXIT;"));
        assert!(out.function_c.contains("goto L_001A940C;"));
        // Static instruction credits per block keep `steps` accounting exact.
        assert!(out.function_c.contains("exec += 3ull;"));
        assert!(out.function_c.contains("exec += 11ull;"));
        assert!(out.function_c.contains("exec += 1ull;"));
        // Statements are reused verbatim; there is no dispatch inside.
        assert!(out.function_c.contains("r[4] = mem16("));
        assert!(out.function_c.contains("r[14] = 0x001A93E0u"));
        assert!(!out.function_c.contains("switch"));
        assert!(!out.function_c.contains("steps"));
        assert!(out.hook_c.contains("steps += s1_exec - 1ull;"));
        assert!(out.hook_c.contains("pc == 0x001A93A8u"));
        assert!(out.gate_c.contains("DQ8_S1_CFG"));
    }

    #[test]
    fn interior_exit_target_that_is_not_a_block_start_is_rejected() {
        let Some((_bytes, mut region, emitted)) = real_region_cases("interior") else {
            eprintln!("skipping: DQ8.code not present next to the crate root");
            return;
        };
        // Point block 0's conditional branch at an address inside the region
        // that is not a block start (0x001A93B0 is block 0's own `beq`):
        // dispatch could legally arrive there, so the CFG must refuse rather
        // than mis-route.
        region.blocks[0].exit = crate::block_ir::Exit::Branch {
            target: REGION_START + 0x8,
            cond: 0,
            fallthrough: None,
        };
        let err = emit_region(&region, &emitted).expect_err("must reject");
        assert!(err.contains("not a block start"), "{err}");
    }

    #[test]
    fn unknown_instructions_are_rejected() {
        let Some((_bytes, mut region, emitted)) = real_region_cases("unknown") else {
            eprintln!("skipping: DQ8.code not present next to the crate root");
            return;
        };
        region.blocks[2].insns[3].kind = crate::block_ir::Kind::Unknown;
        let err = emit_region(&region, &emitted).expect_err("must reject");
        assert!(err.contains("Unknown"), "{err}");
    }

    #[test]
    fn mid_block_running_writes_are_rejected() {
        let Some((_bytes, region, mut emitted)) = real_region_cases("running") else {
            eprintln!("skipping: DQ8.code not present next to the crate root");
            return;
        };
        // A mid-block statement that clears `running` would only be observed
        // when the region finally exits - refuse instead.
        emitted.insert(0x001A_93B8u32, "running = 0;".to_string());
        let err = emit_region(&region, &emitted).expect_err("must reject");
        assert!(err.contains("`running` mid-block"), "{err}");
    }

    #[test]
    fn truncated_region_windows_exit_to_the_dispatcher() {
        let Some(bytes) = real_bytes() else {
            eprintln!("skipping: DQ8.code not present next to the crate root");
            return;
        };
        // A window that cuts block 2 (0x001A93E0..0x001A940C) in half leaves
        // its tail outside.  R2 compiles that end-of-window exit as a direct
        // `goto L_EXIT`: the reused statements already set pc to the first
        // instruction past the window, so the dispatcher resumes exactly where
        // the truncated dispatch path would have — the refusal of S1c is
        // replaced by an honest edge, and the hook's steps credit stays exact.
        let region = extract_region(
            &bytes,
            GUEST_BASE,
            REGION_START,
            REGION_START + 0x58,
        )
        .expect("extract");
        let tmp = std::env::temp_dir().join(format!("dq8-s1c-case-{}-trunc.c", std::process::id()));
        crate::recompile_arm_function(
            std::path::Path::new("DQ8.code"),
            &tmp,
            GUEST_BASE,
            REGION_START,
            &[],
        )
        .expect("recompile region");
        let text = std::fs::read_to_string(&tmp).expect("read generated");
        let _ = std::fs::remove_file(&tmp);
        let out = emit_region(&region, &scrape_emitted(&text)).expect("truncated window now emits");
        assert!(out.function_c.contains("goto L_EXIT;"));
        assert!(!out.function_c.contains("switch"));
    }

    #[test]
    fn fallthrough_block_compiles_to_a_direct_edge() {
        let Some(bytes) = real_bytes() else {
            eprintln!("skipping: DQ8.code not present next to the crate root");
            return;
        };
        // 0x001A94F0 is a one-instruction `ldmia r11!,{r0-r7}` lead-in whose
        // exit is a fall-through to 0x001A94F4 (the S1c region's entry).  In
        // the extended region 0x001A9428..0x001A9608 it must compile to a
        // direct goto of the next block's label, not a dispatch return.
        let region =
            extract_region(&bytes, GUEST_BASE, 0x001A_9428, 0x001A_9608).expect("extract");
        let tmp = std::env::temp_dir().join(format!("dq8-s1c-case-{}-ft.c", std::process::id()));
        crate::recompile_arm_function(
            std::path::Path::new("DQ8.code"),
            &tmp,
            GUEST_BASE,
            0x001A_9428,
            &[],
        )
        .expect("recompile region");
        let text = std::fs::read_to_string(&tmp).expect("read generated");
        let _ = std::fs::remove_file(&tmp);
        let out = emit_region(&region, &scrape_emitted(&text)).expect("emit extended region");
        // 44-insn block falls through to 0x001A94D8; the 1-insn lead-ins at
        // 0x001A94F0 falls through to the region entry 0x001A94F4... which is
        // not a block start of THIS region (0x001A94F4 is, 0x001A94F0 -> it
        // is).  Both must be direct gotos.
        assert!(out.function_c.contains("goto L_001A94D8;"), "44-insn fall-through");
        assert!(out.function_c.contains("goto L_001A94F4;"), "ldmia lead-in fall-through");
        assert!(out.function_c.contains("goto L_001A94F0;"), "5-insn fall-through");
        assert!(!out.function_c.contains("switch"));
        // The loop back-edge from 0x001A94D8 (bne 0x001A9428) stays intra.
        assert!(out.back_edges >= 1);
    }
}
