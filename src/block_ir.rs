//! Block-IR: a representation-only basic-block model for one guest region.
//!
//! This module is the S1a deliverable of the S1 (hot-region native CFG)
//! roadmap item (`reports/DQ8_OPTIMIZATION_ROADMAP.md`).  It provides a
//! **data model** and an **extractor**: guest code in an address range is
//! decoded into basic blocks with explicit register read/write sets, flag
//! reads/writes and block exits (fall-through / branch / call / return /
//! indirect).  It emits no C, changes no behaviour and is not wired into the
//! translation frontend — the generated `target/dq8-startup.c` stays
//! byte-identical to `main`.  Emission and optimisation are S1b/S1c scope.
//!
//! Design decisions recorded here so S1b inherits them:
//!
//! * **Conservative set direction.**  Reads and writes are both
//!   over-approximated (a register/flag that may be touched is listed).  A
//!   superset is the safe direction for the later liveness/residency passes:
//!   a pass may only act when the sets say it is safe.
//! * **Conditionally-executed instructions do not end a block.**  A32
//!   predication (`rsbhs`, `subsne`, …) is flag-guarded data flow, not
//!   control flow, so it stays inside its block and contributes flag reads.
//! * **Targets outside the region are recorded, not followed.**  The
//!   extractor decodes only `[start, end)`; a branch/call outside the range
//!   still yields the correct `Exit` with that target, but no block is
//!   created for it.
//! * **Raw words are authoritative.**  Every `Insn` keeps the guest encoding;
//!   the derived fields (kind, sets, exit) are an interpretation of it.  The
//!   text dump is a canonical serialisation of exactly these fields and
//!   round-trips (dump → parse → equal → dump → identical).
//!
//! The decoder covers the A32 subset the movie region uses (data processing
//! with all shift forms, multiply, single/halfword transfers, LDM/STM, branch
//! /call/BLX, register branches, loads to PC).  Anything else is kept as
//! `Kind::Unknown` with its raw word, so a wider region still extracts
//! honestly instead of guessing semantics.

use crate::arm_branch_target;
use std::collections::BTreeSet;

/// Default guest code-image base (matches the recompiler's `--base` default).
pub const GUEST_BASE: u32 = 0x0010_0000;

/// Bit-packed set of guest GPRs; bit `i` is `r[i]` (bit 15 = pc).
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct RegSet(u32);

impl RegSet {
    pub const fn new() -> Self {
        Self(0)
    }

    pub fn insert(&mut self, reg: u8) {
        if reg < 16 {
            self.0 |= 1 << reg;
        }
    }

    pub fn contains(self, reg: u8) -> bool {
        reg < 16 && self.0 & (1 << reg) != 0
    }

    #[allow(dead_code)] // part of the set API S1b will use
    pub fn is_empty(self) -> bool {
        self.0 == 0
    }

    pub fn iter(self) -> impl Iterator<Item = u8> {
        (0u8..16).filter(move |r| self.contains(*r))
    }

    fn union(&mut self, other: RegSet) {
        self.0 |= other.0;
    }
}

/// Bit-packed APSR flag set.  Bits 0..3 = N,Z,C,V; bits 4..7 = GE lane 0..3.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct FlagSet(u8);

pub const FLAG_N: u8 = 1 << 0;
pub const FLAG_Z: u8 = 1 << 1;
pub const FLAG_C: u8 = 1 << 2;
pub const FLAG_V: u8 = 1 << 3;
pub const FLAG_GE0: u8 = 1 << 4;
pub const FLAG_GE1: u8 = 1 << 5;
pub const FLAG_GE2: u8 = 1 << 6;
pub const FLAG_GE3: u8 = 1 << 7;

impl FlagSet {
    pub const fn new() -> Self {
        Self(0)
    }

    pub fn insert(&mut self, flag: u8) {
        self.0 |= flag;
    }

    pub fn contains(self, flag: u8) -> bool {
        self.0 & flag != 0
    }

    #[allow(dead_code)] // part of the set API S1b will use
    pub fn is_empty(self) -> bool {
        self.0 == 0
    }

    fn union(&mut self, other: FlagSet) {
        self.0 |= other.0;
    }
}

/// N,Z,C,V — written by any S-form arithmetic op (used by tests + S1b).
#[allow(dead_code)]
const NZCV: u8 = FLAG_N | FLAG_Z | FLAG_C | FLAG_V;
/// N,Z — written by any S-form op (used by tests + S1b).
#[allow(dead_code)]
const NZ: u8 = FLAG_N | FLAG_Z;
/// N,Z,C — written by S-form logical ops whose shifter publishes a carry.
#[allow(dead_code)]
const NZC: u8 = FLAG_N | FLAG_Z | FLAG_C;

/// A32 condition code (bits 31..28 of the encoding).
pub type Cond = u8;

/// The "always" condition.
pub const COND_AL: Cond = 0xE;

/// Flags read when an instruction executes under `cond`.
fn cond_flag_reads(cond: Cond) -> FlagSet {
    let mut f = FlagSet::new();
    match cond {
        0x0 | 0x1 => f.insert(FLAG_Z),          // EQ / NE
        0x2 | 0x3 => f.insert(FLAG_C),          // HS/CS / LO/CC
        0x4 | 0x5 => f.insert(FLAG_N),          // MI / PL
        0x6 | 0x7 => f.insert(FLAG_V),          // VS / VC
        0x8 | 0x9 => f.insert(FLAG_C | FLAG_Z), // HI / LS
        0xA | 0xB => f.insert(FLAG_N | FLAG_V), // GE / LT
        0xC | 0xD => f.insert(FLAG_N | FLAG_Z | FLAG_V), // GT / LE
        _ => {}
    }
    f
}

const COND_NAMES: [&str; 16] = [
    "eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "al", "nv",
];

fn cond_from_name(name: &str) -> Option<Cond> {
    COND_NAMES
        .iter()
        .position(|n| *n == name)
        .map(|i| i as u8)
}

/// Barrel-shifter specification for register-form operand2.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Shift {
    None,
    /// Immediate shift: `typ` 0=lsl 1=lsr 2=asr 3=rrx, `amount` 0..=31.
    Imm { typ: u8, amount: u8 },
    /// Register-specified shift: `typ` 0..3, amount from `Rs` (dynamic).
    Reg { typ: u8, rs: u8 },
}

impl Shift {
    fn material(self) -> bool {
        // Does the shifter compute a carry-out an S-form flag write publishes
        // into C?  An immediate LSL/LSR/ASR by 0 leaves C untouched; RRX and
        // any register-specified amount are dynamic and publish C.
        match self {
            Shift::None => false,
            Shift::Imm { typ, amount } => typ == 3 || amount != 0,
            Shift::Reg { .. } => true,
        }
    }
}

/// Data-processing opcodes (bits 24..21).
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum DP {
    And,
    Eor,
    Sub,
    Rsb,
    Add,
    Adc,
    Sbc,
    Rsc,
    Tst,
    Teq,
    Cmp,
    Cmn,
    Orr,
    Mov,
    Bic,
    Mvn,
}

impl DP {
    fn name(self) -> &'static str {
        match self {
            DP::And => "and",
            DP::Eor => "eor",
            DP::Sub => "sub",
            DP::Rsb => "rsb",
            DP::Add => "add",
            DP::Adc => "adc",
            DP::Sbc => "sbc",
            DP::Rsc => "rsc",
            DP::Tst => "tst",
            DP::Teq => "teq",
            DP::Cmp => "cmp",
            DP::Cmn => "cmn",
            DP::Orr => "orr",
            DP::Mov => "mov",
            DP::Bic => "bic",
            DP::Mvn => "mvn",
        }
    }

    fn from_name(name: &str) -> Option<DP> {
        Some(match name {
            "and" => DP::And,
            "eor" => DP::Eor,
            "sub" => DP::Sub,
            "rsb" => DP::Rsb,
            "add" => DP::Add,
            "adc" => DP::Adc,
            "sbc" => DP::Sbc,
            "rsc" => DP::Rsc,
            "tst" => DP::Tst,
            "teq" => DP::Teq,
            "cmp" => DP::Cmp,
            "cmn" => DP::Cmn,
            "orr" => DP::Orr,
            "mov" => DP::Mov,
            "bic" => DP::Bic,
            "mvn" => DP::Mvn,
            _ => return None,
        })
    }

    fn from_op(op: u32) -> DP {
        match op & 0xF {
            0x0 => DP::And,
            0x1 => DP::Eor,
            0x2 => DP::Sub,
            0x3 => DP::Rsb,
            0x4 => DP::Add,
            0x5 => DP::Adc,
            0x6 => DP::Sbc,
            0x7 => DP::Rsc,
            0x8 => DP::Tst,
            0x9 => DP::Teq,
            0xA => DP::Cmp,
            0xB => DP::Cmn,
            0xC => DP::Orr,
            0xD => DP::Mov,
            0xE => DP::Bic,
            _ => DP::Mvn,
        }
    }

    fn uses_rn(self) -> bool {
        !matches!(self, DP::Mov | DP::Mvn)
    }

    fn writes_rd(self) -> bool {
        !matches!(self, DP::Tst | DP::Teq | DP::Cmp | DP::Cmn)
    }

    fn is_logical(self) -> bool {
        matches!(
            self,
            DP::And | DP::Eor | DP::Orr | DP::Mov | DP::Bic | DP::Mvn | DP::Tst | DP::Teq
        )
    }

    fn is_arith(self) -> bool {
        matches!(
            self,
            DP::Sub | DP::Rsb | DP::Add | DP::Adc | DP::Sbc | DP::Rsc | DP::Cmp | DP::Cmn
        )
    }

    /// Carry-in read (ADC/SBC/RSC are defined from the stored C).
    fn reads_carry(self) -> bool {
        matches!(self, DP::Adc | DP::Sbc | DP::Rsc)
    }
}

/// Memory width for single/halfword transfers.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Size {
    Byte,
    Word,
    Half,
    SignedByte,
    SignedHalf,
}

impl Size {
    fn name(self) -> &'static str {
        match self {
            Size::Byte => "b",
            Size::Word => "w",
            Size::Half => "h",
            Size::SignedByte => "sb",
            Size::SignedHalf => "sh",
        }
    }

    fn from_name(name: &str) -> Option<Size> {
        Some(match name {
            "b" => Size::Byte,
            "w" => Size::Word,
            "h" => Size::Half,
            "sb" => Size::SignedByte,
            "sh" => Size::SignedHalf,
            _ => return None,
        })
    }
}

/// Instruction classification.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Kind {
    /// Data processing (`and`..`mvn`); the S bit is folded into `sets_flags`.
    Dp { op: DP },
    /// Multiply; `add` distinguishes MLA from MUL.
    Mul { add: bool },
    /// Single or halfword transfer.  `pre` = pre-index, `up` = positive
    /// offset, `wb` = base writeback (W bit set or post-indexed),
    /// `reg_off` = register-specified offset.
    Mem {
        load: bool,
        size: Size,
        pre: bool,
        up: bool,
        wb: bool,
        reg_off: bool,
    },
    /// Load-multiple / store-multiple; the register list stays in the raw
    /// word (bits 15..0).
    Multi { load: bool, wb: bool },
    /// Load whose destination is PC (pop-return or veneer); the exit
    /// classifies it as `Return` (base = SP) or `Indirect`.
    LoadPc { pre: bool, up: bool, wb: bool },
    /// Direct branch or branch-with-link; `target` is the static destination.
    Branch { link: bool, target: u32 },
    /// Register-indirect branch (BX / BLX Rm); the target is dynamic.
    BranchReg { link: bool },
    /// A hint the dispatcher translates as a fall-through no-op: PLD/PLI,
    /// CLREX, the CP15 barrier MCRs and the ARMv6 NOP (main.rs emits
    /// `/* PLD hint */ pc = addr+4;`-shaped statements for them).  They share
    /// encoding space with real instructions — PLD/CLREX look like `ldr` with
    /// `rd = r15`, which would otherwise decode as `LoadPc` and classify as
    /// control flow — so they are intercepted here to keep block splitting
    /// aligned with what dispatch actually does: continue at addr+4.
    Hint,
    /// Encoding outside the decoded subset; `word` stays authoritative.
    Unknown,
}

/// One decoded guest instruction with its derived dataflow.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Insn {
    pub addr: u32,
    pub word: u32,
    pub cond: Cond,
    pub kind: Kind,
    /// Destination register, `NO_REG` when the instruction writes none.
    pub rd: u8,
    /// Base / first-operand register, `NO_REG` when unused.
    pub rn: u8,
    /// Second-operand register, `NO_REG` when unused.
    pub rm: u8,
    /// Decoded immediate (already rotated), when the encoding is immediate.
    pub imm: Option<u32>,
    pub shift: Shift,
    pub sets_flags: bool,
    pub reads: RegSet,
    pub writes: RegSet,
    pub flag_reads: FlagSet,
    pub flag_writes: FlagSet,
}

/// Sentinel for "no register operand".
pub const NO_REG: u8 = 0xFF;

fn reg(bitfield: u32) -> u8 {
    bitfield as u8
}

/// Decode one A32 word.  Branch targets reuse the frontend's
/// `arm_branch_target` maths so IR edges match the generated dispatcher.
fn decode_a32(word: u32, addr: u32) -> Insn {
    let cond = (word >> 28) as u8 & 0xF;
    let mut insn = Insn {
        addr,
        word,
        cond,
        kind: Kind::Unknown,
        rd: NO_REG,
        rn: NO_REG,
        rm: NO_REG,
        imm: None,
        shift: Shift::None,
        sets_flags: false,
        reads: RegSet::new(),
        writes: RegSet::new(),
        flag_reads: cond_flag_reads(cond),
        flag_writes: FlagSet::new(),
    };

    // Hints the dispatcher translates as fall-through no-ops (main.rs emits
    // `__sync_synchronize();`, `/* PLD hint */` etc. and the case then sets
    // pc = addr + 4).  The masks mirror main.rs's checks exactly.  This must
    // run before the single-transfer path: PLD/PLI and CLREX share encoding
    // space with `ldr rd = pc`, which would otherwise decode as `LoadPc` —
    // control flow — and split blocks where dispatch simply continues at the
    // next instruction (that mismatch would strand the native-CFG band
    // regions at every prefetch hint).
    if word & 0x0FFF_0F00 == 0x0E07_0F00
        || word & 0xFD70_F000 == 0xF550_F000
        || word == 0xE320_F000
        || word == 0xF57F_F01F
    {
        insn.kind = Kind::Hint;
        return insn;
    }

    // Branch space: B / BL / BLX imm.
    if let Some((target, link, _is_cond)) = arm_branch_target(word, addr) {
        if cond == 0xF {
            // BLX imm is an unconditional interworking call; the cond field
            // holds 1111 by encoding, not by predication.
            insn.cond = COND_AL;
        }
        insn.kind = Kind::Branch { link, target };
        if link {
            // BL/BLX write the return address into lr (the C frontend does
            // `r[14] = <addr+4>` in the same case).
            insn.writes.insert(14);
        }
        return insn;
    }

    // Register branch BX / BLX Rm.
    if word & 0x0FFF_FFF0 == 0x012F_FF10 || word & 0x0FFF_FFF0 == 0x012F_FF30 {
        let link = word & 0x20 != 0;
        let rm = reg(word & 0xF);
        insn.kind = Kind::BranchReg { link };
        insn.rm = rm;
        insn.reads.insert(rm);
        if link {
            insn.writes.insert(14);
        }
        return insn;
    }

    // Multiply: bits 27..22 == 000000 and bits 7..4 == 1001 (MUL/MLA).
    if word & 0x0FC0_00F0 == 0x0000_0090 {
        let rd = reg((word >> 16) & 0xF);
        let rn = reg((word >> 12) & 0xF); // accumulate operand for MLA
        let rs = reg((word >> 8) & 0xF);
        let rm = reg(word & 0xF);
        let s = word & (1 << 20) != 0;
        let add = word & (1 << 21) != 0;
        insn.kind = Kind::Mul { add };
        insn.rd = rd;
        insn.rn = if add { rn } else { NO_REG };
        insn.rm = rm;
        insn.reads.insert(rm);
        insn.reads.insert(rs);
        if add {
            insn.reads.insert(rn);
        }
        insn.writes.insert(rd);
        insn.sets_flags = s;
        if s {
            // MUL/MLA S-form write N,Z only (no shifter, no carry).
            insn.flag_writes.insert(FLAG_N);
            insn.flag_writes.insert(FLAG_Z);
        }
        return insn;
    }

    // Halfword / signed transfers: bits 27..25 == 000 with bits 7 and 4 set.
    if word & 0x0E00_0090 == 0x0000_0090 {
        let size = match (word >> 5) & 3 {
            1 => Size::Half,
            2 => Size::SignedByte,
            3 => Size::SignedHalf,
            _ => {
                insn.kind = Kind::Unknown; // SH == 00 is LDR/STR, not here
                return insn;
            }
        };
        let pre = word & (1 << 24) != 0;
        let up = word & (1 << 23) != 0;
        // For halfword/signed transfers bit 22 selects the IMMEDIATE form
        // (1 = immH:immL, 0 = register Rm) — the opposite of single transfers.
        let reg_off = word & (1 << 22) == 0;
        let wb = word & (1 << 21) != 0 || !pre;
        let load = word & (1 << 20) != 0;
        let rn = reg((word >> 16) & 0xF);
        let rd = reg((word >> 12) & 0xF);
        insn.kind = Kind::Mem {
            load,
            size,
            pre,
            up,
            wb,
            reg_off,
        };
        insn.rn = rn;
        insn.reads.insert(rn);
        if reg_off {
            let rm = reg(word & 0xF);
            insn.rm = rm;
            insn.reads.insert(rm);
        } else {
            // immH:immL split across bits 11..8 and 3..0.
            insn.imm = Some((((word >> 4) & 0xF0) | (word & 0xF)) as u32);
        }
        if load {
            if rd == 15 {
                insn.kind = Kind::LoadPc { pre, up, wb };
                insn.rd = NO_REG;
            } else {
                insn.rd = rd;
                insn.writes.insert(rd);
            }
        } else {
            insn.reads.insert(rd); // stored value (STRH pc is UNPREDICTABLE)
        }
        if wb {
            insn.writes.insert(rn);
        }
        return insn;
    }

    // LDM / STM: bits 27..25 == 100.
    if word & 0x0E00_0000 == 0x0800_0000 {
        let load = word & (1 << 20) != 0;
        let wb = word & (1 << 21) != 0;
        let rn = reg((word >> 16) & 0xF);
        let list = (word & 0xFFFF) as u16;
        insn.kind = Kind::Multi { load, wb };
        insn.rn = rn;
        insn.reads.insert(rn);
        for r in 0..16u8 {
            if list & (1 << r) != 0 {
                if load {
                    insn.writes.insert(r); // pc (bit 15) handled by the exit
                } else {
                    insn.reads.insert(r);
                }
            }
        }
        if wb {
            insn.writes.insert(rn);
        }
        return insn;
    }

    // Single transfers (LDR / STR, byte or word): bits 27..26 == 01.
    if word & 0x0C00_0000 == 0x0400_0000 {
        let reg_off = word & (1 << 25) != 0;
        let pre = word & (1 << 24) != 0;
        let up = word & (1 << 23) != 0;
        let byte = word & (1 << 22) != 0;
        let wb = word & (1 << 21) != 0 || !pre;
        let load = word & (1 << 20) != 0;
        let rn = reg((word >> 16) & 0xF);
        let rd = reg((word >> 12) & 0xF);
        let size = if byte { Size::Byte } else { Size::Word };
        let mut rm = NO_REG;
        if reg_off {
            let typ = ((word >> 5) & 3) as u8;
            rm = reg(word & 0xF);
            insn.shift = if word & (1 << 4) != 0 {
                let rs = ((word >> 8) & 0xF) as u8;
                insn.reads.insert(rs);
                Shift::Reg { typ, rs }
            } else {
                Shift::Imm {
                    typ,
                    amount: ((word >> 7) & 0x1F) as u8,
                }
            };
        } else {
            insn.imm = Some(word & 0xFFF);
        }
        insn.kind = Kind::Mem {
            load,
            size,
            pre,
            up,
            wb,
            reg_off,
        };
        insn.rn = rn;
        insn.rm = rm;
        insn.reads.insert(rn);
        if reg_off {
            insn.reads.insert(rm);
        }
        if load {
            if rd == 15 {
                insn.kind = Kind::LoadPc { pre, up, wb };
                insn.rd = NO_REG;
            } else {
                insn.rd = rd;
                insn.writes.insert(rd);
            }
        } else {
            insn.reads.insert(rd & 15);
        }
        if wb {
            insn.writes.insert(rn);
        }
        return insn;
    }

    // Data processing: bits 27..26 == 00.
    if word & 0x0C00_0000 == 0x0000_0000 {
        let imm_form = word & (1 << 25) != 0;
        let op = DP::from_op((word >> 21) & 0xF);
        let s = word & (1 << 20) != 0;
        let rn = reg((word >> 16) & 0xF);
        let rd = reg((word >> 12) & 0xF);
        insn.kind = Kind::Dp { op };
        insn.rn = if op.uses_rn() { rn } else { NO_REG };
        insn.rd = if op.writes_rd() { rd } else { NO_REG };
        insn.sets_flags = s;
        if op.writes_rd() {
            // An rd of pc is recorded as a write to r15; classifying DP-to-PC
            // as control flow is outside the S1a subset (the movie region
            // has none) and is noted in the module docs.
            insn.writes.insert(rd);
        }
        if op.uses_rn() {
            insn.reads.insert(rn);
        }
        // Does the barrel shifter publish a carry-out into C?
        let shifter_carry;
        if imm_form {
            let rot = ((word >> 8) & 0xF) as u32;
            insn.imm = Some((word & 0xFF).rotate_right(rot * 2));
            shifter_carry = rot != 0;
        } else {
            let typ = ((word >> 5) & 3) as u8;
            let rm = reg(word & 0xF);
            insn.rm = rm;
            insn.reads.insert(rm);
            insn.shift = if word & (1 << 4) != 0 {
                let rs = ((word >> 8) & 0xF) as u8;
                insn.reads.insert(rs);
                Shift::Reg { typ, rs }
            } else {
                Shift::Imm {
                    typ,
                    amount: ((word >> 7) & 0x1F) as u8,
                }
            };
            shifter_carry = insn.shift.material();
        }
        if s {
            // Arithmetic S-forms write NZCV; logical S-forms write N,Z and
            // C only when the shifter materialises a carry-out.
            if op.is_arith() {
                insn.flag_writes.insert(FLAG_N);
                insn.flag_writes.insert(FLAG_Z);
                insn.flag_writes.insert(FLAG_C);
                insn.flag_writes.insert(FLAG_V);
            } else if op.is_logical() {
                insn.flag_writes.insert(FLAG_N);
                insn.flag_writes.insert(FLAG_Z);
                if shifter_carry {
                    insn.flag_writes.insert(FLAG_C);
                }
            }
        }
        if op.reads_carry() {
            insn.flag_reads.insert(FLAG_C);
        }
        return insn;
    }

    insn.kind = Kind::Unknown;
    insn
}

/// How a block leaves its last instruction.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Exit {
    /// Straight-line into the next sequential address.
    FallThrough { next: u32 },
    /// Direct branch; `fallthrough` is the sequential address for a
    /// conditional branch and `None` for an unconditional one.
    Branch {
        target: u32,
        cond: Cond,
        fallthrough: Option<u32>,
    },
    /// Direct call (BL / BLX imm); `fallthrough` is the return address.
    Call {
        target: u32,
        cond: Cond,
        fallthrough: Option<u32>,
    },
    /// The block returns to its caller (`ldr pc,[sp],#n` / `pop {…,pc}`).
    Return,
    /// The target is computed at runtime (BX / BLX Rm, other load-to-pc).
    Indirect,
    /// The block runs to the region boundary without control flow.
    End,
}

/// One basic block: a maximal straight-line run of instructions.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Block {
    pub start: u32,
    /// Exclusive end address (one past the last instruction).
    pub end: u32,
    pub insns: Vec<Insn>,
    pub reads: RegSet,
    pub writes: RegSet,
    pub flag_reads: FlagSet,
    pub flag_writes: FlagSet,
    pub exit: Exit,
}

/// Extracted region: `[start, end)` split into basic blocks in address
/// order.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Region {
    pub base: u32,
    pub start: u32,
    pub end: u32,
    pub blocks: Vec<Block>,
}

/// Does this instruction end a basic block (is it control flow)?
fn is_control(insn: &Insn) -> bool {
    matches!(
        insn.kind,
        Kind::Branch { .. } | Kind::BranchReg { .. } | Kind::LoadPc { .. } | Kind::Multi { .. }
    ) || is_dp_pc(insn)
}

/// A data-processing instruction with `rd = pc` (`mov pc, lr`, `add pc, …`).
///
/// These used to be *not* control flow (S1a subset note: "the movie region has
/// none"), so a block ran straight past them.  That is harmless on the
/// dispatch path — the statement rewrites `pc` and the loop re-dispatches — but
/// inside a native-CFG region the statements after the jump would still
/// execute, which dispatch never does.  Treating it as control makes the block
/// end there: `control_exit` returns `Indirect` (`goto L_EXIT`), and the
/// dispatcher resumes at exactly the pc the statement wrote.  The lowering
/// always writes pc — the taken arm sets `value & ~3`, the else arm sets
/// `addr + 4` — so leaving the region on that edge is equivalent to what the
/// dispatcher would do at the top of its loop, for both branch directions.
fn is_dp_pc(insn: &Insn) -> bool {
    matches!(insn.kind, Kind::Dp { .. }) && insn.writes.contains(15)
}

/// Compute the block exit for a control-flow instruction.
fn control_exit(insn: &Insn) -> Exit {
    match insn.kind {
        Kind::Branch { link, target } => {
            if link {
                Exit::Call {
                    target,
                    cond: insn.cond,
                    fallthrough: Some(insn.addr + 4),
                }
            } else if insn.cond == COND_AL {
                Exit::Branch {
                    target,
                    cond: insn.cond,
                    fallthrough: None,
                }
            } else {
                Exit::Branch {
                    target,
                    cond: insn.cond,
                    fallthrough: Some(insn.addr + 4),
                }
            }
        }
        Kind::BranchReg { .. } => Exit::Indirect,
        Kind::LoadPc { .. } => {
            // `ldr pc, [sp], #n` is the canonical tail-return of this ABI;
            // any other base is an indirect veneer.
            if insn.rn == 13 {
                Exit::Return
            } else {
                Exit::Indirect
            }
        }
        Kind::Multi { load, .. } => {
            if load && insn.word & (1 << 15) != 0 {
                Exit::Return // pop {…, pc}
            } else {
                Exit::FallThrough {
                    next: insn.addr + 4,
                }
            }
        }
        // DP-to-PC: the destination is dynamic, so the block hands control
        // back to the dispatcher, which continues at the pc the statement set.
        Kind::Dp { .. } => Exit::Indirect,
        _ => Exit::End,
    }
}

/// Extract the Block-IR for `[start, end)` from a guest code image.
///
/// `bytes` is the raw code image (the same buffer `recompile-arm` reads);
/// guest address `a` lives at `bytes[a - base]`.  Thumb is out of scope for
/// S1a (the movie region is A32); a non-word-aligned `start` is rejected.
pub fn extract_region(bytes: &[u8], base: u32, start: u32, end: u32) -> Result<Region, String> {
    if start & 3 != 0 {
        return Err(format!("region start 0x{start:08X} is not A32-aligned"));
    }
    if end <= start {
        return Err(format!(
            "region end 0x{end:08X} is not above start 0x{start:08X}"
        ));
    }
    let off = start
        .checked_sub(base)
        .ok_or_else(|| format!("region start 0x{start:08X} is below base 0x{base:08X}"))? as usize;
    let size = (end - start) as usize;
    if off + size > bytes.len() {
        return Err(format!(
            "region 0x{start:08X}..0x{end:08X} exceeds the code image ({} bytes)",
            bytes.len()
        ));
    }

    // Decode every instruction in the range.
    let mut insns = Vec::with_capacity(size / 4);
    for addr in (start..end).step_by(4) {
        let o = (addr - base) as usize;
        let word = u32::from_le_bytes(bytes[o..o + 4].try_into().unwrap());
        insns.push(decode_a32(word, addr));
    }

    // Leaders: the region entry, every static branch/call target that lands
    // inside the region, and the sequential successor of EVERY control-flow
    // instruction — that is what ends a block at its jump (see the note below).
    let mut leaders = BTreeSet::new();
    leaders.insert(start);
    for insn in &insns {
        if let Kind::Branch { target, .. } = insn.kind {
            if target >= start && target < end {
                leaders.insert(target);
            }
        }
        if is_control(insn) {
            // EVERY control instruction must end its block, and grouping only
            // splits at leaders, so the sequential successor has to become one:
            // otherwise the statements after the jump would stay inside this
            // block.  `falls_through` alone misses the unconditional cases —
            // `b`/`bx`/`ldr pc`/`pop {…,pc}` reach `addr + 4` only as a branch
            // target (or never), but the emitted region would still fall into
            // them, which dispatch never does: real bug at 0x001A70D8
            // (`b 0x001A7218` out of window, block kept 0x001A70DC.. dead code
            // glued on — the native CFG executed it and corrupted r2/r3/r4 for
            // the caller of S1c).  With cond != AL the successor is genuinely
            // reachable; with cond == AL it is dead code that only a branch
            // target can ever enter — either way it must not share a block with
            // the jump.
            let next = insn.addr + 4;
            if next < end {
                leaders.insert(next);
            }
        }
    }

    // Group instructions leader-to-leader and compute each block's exit.
    let mut blocks: Vec<Block> = Vec::new();
    let mut current: Vec<Insn> = Vec::new();
    for insn in insns {
        if leaders.contains(&insn.addr) && !current.is_empty() {
            blocks.push(finish_block(std::mem::take(&mut current), end));
        }
        current.push(insn);
    }
    if !current.is_empty() {
        blocks.push(finish_block(current, end));
    }

    Ok(Region {
        base,
        start,
        end,
        blocks,
    })
}

fn finish_block(insns: Vec<Insn>, region_end: u32) -> Block {
    let start = insns[0].addr;
    let last = *insns.last().expect("block has at least one instruction");
    let end = last.addr + 4;
    let mut reads = RegSet::new();
    let mut writes = RegSet::new();
    let mut flag_reads = FlagSet::new();
    let mut flag_writes = FlagSet::new();
    for insn in &insns {
        reads.union(insn.reads);
        writes.union(insn.writes);
        flag_reads.union(insn.flag_reads);
        flag_writes.union(insn.flag_writes);
    }
    let exit = if is_control(&last) {
        control_exit(&last)
    } else if end < region_end {
        Exit::FallThrough { next: end }
    } else {
        Exit::End
    };
    Block {
        start,
        end,
        insns,
        reads,
        writes,
        flag_reads,
        flag_writes,
        exit,
    }
}

// ---------------------------------------------------------------------------
// Deterministic text dump + parser (round-trip).
// ---------------------------------------------------------------------------

fn write_reg_list(out: &mut String, set: RegSet) {
    let names: Vec<String> = set.iter().map(|r| format!("r{r}")).collect();
    out.push_str(&names.join(","));
}

fn write_flag_list(out: &mut String, set: FlagSet) {
    let mut names: Vec<String> = Vec::new();
    for (bit, name) in [
        (FLAG_N, "N"),
        (FLAG_Z, "Z"),
        (FLAG_C, "C"),
        (FLAG_V, "V"),
        (FLAG_GE0, "GE0"),
        (FLAG_GE1, "GE1"),
        (FLAG_GE2, "GE2"),
        (FLAG_GE3, "GE3"),
    ] {
        if set.contains(bit) {
            names.push(name.to_string());
        }
    }
    out.push_str(&names.join(","));
}

fn parse_reg_list(field: &str) -> Result<RegSet, String> {
    let mut set = RegSet::new();
    if field.is_empty() {
        return Ok(set);
    }
    for item in field.split(',') {
        let r = item
            .strip_prefix('r')
            .ok_or_else(|| format!("bad register '{item}'"))?
            .parse::<u8>()
            .map_err(|_| format!("bad register '{item}'"))?;
        if r > 15 {
            return Err(format!("register '{item}' out of range"));
        }
        set.insert(r);
    }
    Ok(set)
}

fn parse_flag_list(field: &str) -> Result<FlagSet, String> {
    let mut set = FlagSet::new();
    if field.is_empty() {
        return Ok(set);
    }
    for item in field.split(',') {
        let bit = match item {
            "N" => FLAG_N,
            "Z" => FLAG_Z,
            "C" => FLAG_C,
            "V" => FLAG_V,
            "GE0" => FLAG_GE0,
            "GE1" => FLAG_GE1,
            "GE2" => FLAG_GE2,
            "GE3" => FLAG_GE3,
            _ => return Err(format!("bad flag '{item}'")),
        };
        set.insert(bit);
    }
    Ok(set)
}

fn shift_type_name(t: u8) -> &'static str {
    match t {
        0 => "lsl",
        1 => "lsr",
        2 => "asr",
        3 => "rrx",
        _ => "??",
    }
}

fn shift_name(shift: Shift) -> String {
    match shift {
        Shift::None => "none".to_string(),
        Shift::Imm { typ: 3, amount: 0 } => "rrx".to_string(),
        Shift::Imm { typ, amount } => format!("{}#{}", shift_type_name(typ), amount),
        Shift::Reg { typ, rs } => format!("{}#reg:r{}", shift_type_name(typ), rs),
    }
}

fn parse_shift(field: &str) -> Result<Shift, String> {
    if field == "none" {
        return Ok(Shift::None);
    }
    if field == "rrx" {
        return Ok(Shift::Imm { typ: 3, amount: 0 });
    }
    if let Some((typ_name, rs)) = field.split_once("#reg:r") {
        let typ = parse_shift_type(typ_name)?;
        let rs: u8 = rs.parse().map_err(|_| format!("bad shift '{field}'"))?;
        return Ok(Shift::Reg { typ, rs });
    }
    let (name, amount) = field
        .split_once('#')
        .ok_or_else(|| format!("bad shift '{field}'"))?;
    let typ = parse_shift_type(name)?;
    let amount: u8 = amount.parse().map_err(|_| format!("bad shift '{field}'"))?;
    Ok(Shift::Imm { typ, amount })
}

fn parse_shift_type(name: &str) -> Result<u8, String> {
    match name {
        "lsl" => Ok(0),
        "lsr" => Ok(1),
        "asr" => Ok(2),
        "rrx" => Ok(3),
        _ => Err(format!("bad shift type '{name}'")),
    }
}

fn parse_hex_u32(field: &str) -> Result<u32, String> {
    u32::from_str_radix(field.trim_start_matches("0x"), 16)
        .map_err(|_| format!("bad hex '{field}'"))
}

fn parse_opt_hex(field: &str) -> Result<Option<u32>, String> {
    if field == "-" {
        Ok(None)
    } else {
        parse_hex_u32(field).map(Some)
    }
}

fn parse_reg_field(field: &str) -> Result<u8, String> {
    if field == "-" {
        return Ok(NO_REG);
    }
    let r = field
        .strip_prefix('r')
        .ok_or_else(|| format!("bad register '{field}'"))?
        .parse::<u8>()
        .map_err(|_| format!("bad register '{field}'"))?;
    if r > 15 {
        return Err(format!("register '{field}' out of range"));
    }
    Ok(r)
}

fn reg_field(r: u8) -> String {
    if r == NO_REG {
        "-".to_string()
    } else {
        format!("r{r}")
    }
}

fn opt_hex(value: Option<u32>) -> String {
    match value {
        Some(v) => format!("0x{v:08X}"),
        None => "-".to_string(),
    }
}

fn write_exit(out: &mut String, exit: Exit) {
    match exit {
        Exit::FallThrough { next } => {
            out.push_str(&format!("fallthrough 0x{next:08X}"));
        }
        Exit::Branch {
            target,
            cond,
            fallthrough,
        } => {
            out.push_str(&format!(
                "branch target=0x{target:08X} cond={} fallthrough={}",
                COND_NAMES[cond as usize & 0xF],
                opt_hex(fallthrough),
            ));
        }
        Exit::Call {
            target,
            cond,
            fallthrough,
        } => {
            out.push_str(&format!(
                "call target=0x{target:08X} cond={} fallthrough={}",
                COND_NAMES[cond as usize & 0xF],
                opt_hex(fallthrough),
            ));
        }
        Exit::Return => out.push_str("return"),
        Exit::Indirect => out.push_str("indirect"),
        Exit::End => out.push_str("end"),
    }
}

fn parse_exit(rest: &str) -> Result<Exit, String> {
    let mut words = rest.split_whitespace();
    let tag = words.next().ok_or("missing exit tag")?;
    match tag {
        "fallthrough" => {
            let next = words.next().ok_or("missing fallthrough target")?;
            Ok(Exit::FallThrough {
                next: parse_hex_u32(next)?,
            })
        }
        "branch" | "call" => {
            let mut target = None;
            let mut cond = COND_AL;
            let mut fallthrough = None;
            for word in words {
                let (k, v) = word
                    .split_once('=')
                    .ok_or_else(|| format!("bad exit field '{word}'"))?;
                match k {
                    "target" => target = Some(parse_hex_u32(v)?),
                    "cond" => cond = cond_from_name(v).ok_or_else(|| format!("bad cond '{v}'"))?,
                    "fallthrough" => fallthrough = parse_opt_hex(v)?,
                    _ => return Err(format!("unknown exit field '{k}'")),
                }
            }
            let target = target.ok_or("missing exit target")?;
            Ok(if tag == "branch" {
                Exit::Branch {
                    target,
                    cond,
                    fallthrough,
                }
            } else {
                Exit::Call {
                    target,
                    cond,
                    fallthrough,
                }
            })
        }
        "return" => Ok(Exit::Return),
        "indirect" => Ok(Exit::Indirect),
        "end" => Ok(Exit::End),
        other => Err(format!("unknown exit tag '{other}'")),
    }
}

/// Canonical, deterministic text dump (stable field order; diff-friendly).
pub fn dump(region: &Region) -> String {
    let mut out = String::new();
    out.push_str("# dq8 block-ir v1\n");
    out.push_str(&format!(
        "region base=0x{:08X} start=0x{:08X} end=0x{:08X} blocks={}\n",
        region.base,
        region.start,
        region.end,
        region.blocks.len()
    ));
    for block in &region.blocks {
        out.push_str(&format!(
            "block start=0x{:08X} end=0x{:08X} insns={} reads=",
            block.start,
            block.end,
            block.insns.len()
        ));
        write_reg_list(&mut out, block.reads);
        out.push_str(" writes=");
        write_reg_list(&mut out, block.writes);
        out.push_str(" flag_reads=");
        write_flag_list(&mut out, block.flag_reads);
        out.push_str(" flag_writes=");
        write_flag_list(&mut out, block.flag_writes);
        out.push_str(" exit=");
        write_exit(&mut out, block.exit);
        out.push('\n');
        for insn in &block.insns {
            out.push_str(&format!(
                "  insn addr=0x{:08X} word=0x{:08X} cond={} kind={} rd={} rn={} rm={} imm={} shift={} flags=",
                insn.addr,
                insn.word,
                COND_NAMES[insn.cond as usize & 0xF],
                kind_name(insn.kind),
                reg_field(insn.rd),
                reg_field(insn.rn),
                reg_field(insn.rm),
                opt_hex(insn.imm),
                shift_name(insn.shift),
            ));
            out.push_str(if insn.sets_flags { "S" } else { "-" });
            out.push_str(" reads=");
            write_reg_list(&mut out, insn.reads);
            out.push_str(" writes=");
            write_reg_list(&mut out, insn.writes);
            out.push_str(" flag_reads=");
            write_flag_list(&mut out, insn.flag_reads);
            out.push_str(" flag_writes=");
            write_flag_list(&mut out, insn.flag_writes);
            out.push('\n');
        }
    }
    out
}

fn b(v: bool) -> u8 {
    u8::from(v)
}

fn kind_name(kind: Kind) -> String {
    match kind {
        Kind::Dp { op } => format!("dp:{}", op.name()),
        Kind::Mul { add } => format!("mul:{}", if add { "mla" } else { "mul" }),
        Kind::Mem {
            load,
            size,
            pre,
            up,
            wb,
            reg_off,
        } => format!(
            "mem:{}{}:p{}u{}w{}r{}",
            if load { "ldr" } else { "str" },
            size.name(),
            b(pre),
            b(up),
            b(wb),
            b(reg_off)
        ),
        Kind::Multi { load, wb } => format!(
            "multi:{}:w{}",
            if load { "ldm" } else { "stm" },
            b(wb)
        ),
        Kind::LoadPc { pre, up, wb } => format!("loadpc:p{}u{}w{}", b(pre), b(up), b(wb)),
        Kind::Branch { link, target } => format!(
            "b:{}@0x{:08X}",
            if link { "bl" } else { "b" },
            target
        ),
        Kind::BranchReg { link } => format!("breg:{}", if link { "blx" } else { "bx" }),
        Kind::Hint => "hint".to_string(),
        Kind::Unknown => "unknown".to_string(),
    }
}

fn parse_onoff(field: char) -> Result<bool, String> {
    match field {
        '0' => Ok(false),
        '1' => Ok(true),
        _ => Err(format!("bad flag char '{field}'")),
    }
}

fn parse_kind(field: &str) -> Result<Kind, String> {
    let mut parts = field.splitn(3, ':');
    let tag = parts.next().unwrap_or("");
    match tag {
        "dp" => {
            let op = parts.next().ok_or("missing dp opcode")?;
            Ok(Kind::Dp {
                op: DP::from_name(op).ok_or_else(|| format!("bad dp opcode '{op}'"))?,
            })
        }
        "mul" => match parts.next() {
            Some("mul") => Ok(Kind::Mul { add: false }),
            Some("mla") => Ok(Kind::Mul { add: true }),
            _ => Err(format!("bad mul kind '{field}'")),
        },
        "mem" => {
            let rest = parts.next().ok_or("missing mem kind")?;
            let (dir, size) = rest.split_at(3);
            let (load, size) = match dir {
                "ldr" => (true, Size::from_name(size).ok_or("bad mem size")?),
                "str" => (false, Size::from_name(size).ok_or("bad mem size")?),
                _ => return Err(format!("bad mem dir '{dir}'")),
            };
            // Trailing flags: p<u>u<u>w<u>r<u> (no separators except the
            // leading one, which `splitn` already consumed).
            let flags = parts.next().ok_or("missing mem flags")?;
            let bytes = flags.as_bytes();
            if bytes.len() != 8 || bytes[0] != b'p' || bytes[2] != b'u' || bytes[4] != b'w'
                || bytes[6] != b'r'
            {
                return Err(format!("bad mem flags '{flags}'"));
            }
            Ok(Kind::Mem {
                load,
                size,
                pre: parse_onoff(bytes[1] as char)?,
                up: parse_onoff(bytes[3] as char)?,
                wb: parse_onoff(bytes[5] as char)?,
                reg_off: parse_onoff(bytes[7] as char)?,
            })
        }
        "multi" => {
            let rest = parts.next().ok_or("missing multi kind")?;
            let (dir, flags) = rest.split_at(3);
            let load = match dir {
                "ldm" => true,
                "stm" => false,
                _ => return Err(format!("bad multi dir '{dir}'")),
            };
            let bytes = flags.as_bytes();
            if bytes.len() != 2 || bytes[0] != b'w' {
                return Err(format!("bad multi flags '{flags}'"));
            }
            Ok(Kind::Multi {
                load,
                wb: parse_onoff(bytes[1] as char)?,
            })
        }
        "loadpc" => {
            let flags = parts.next().ok_or("missing loadpc flags")?;
            let bytes = flags.as_bytes();
            if bytes.len() != 6 || bytes[0] != b'p' || bytes[2] != b'u' || bytes[4] != b'w' {
                return Err(format!("bad loadpc flags '{flags}'"));
            }
            Ok(Kind::LoadPc {
                pre: parse_onoff(bytes[1] as char)?,
                up: parse_onoff(bytes[3] as char)?,
                wb: parse_onoff(bytes[5] as char)?,
            })
        }
        "b" => {
            let rest = parts.next().ok_or("missing branch kind")?;
            let (dir, target) = rest
                .split_once('@')
                .ok_or_else(|| format!("missing branch target in '{field}'"))?;
            let link = match dir {
                "b" => false,
                "bl" => true,
                _ => return Err(format!("bad branch kind '{field}'")),
            };
            Ok(Kind::Branch {
                link,
                target: parse_hex_u32(target)?,
            })
        }
        "breg" => match parts.next() {
            Some("bx") => Ok(Kind::BranchReg { link: false }),
            Some("blx") => Ok(Kind::BranchReg { link: true }),
            _ => Err(format!("bad breg kind '{field}'")),
        },
        "hint" => Ok(Kind::Hint),
        "unknown" => Ok(Kind::Unknown),
        _ => Err(format!("unknown kind '{field}'")),
    }
}

/// Parse a dump produced by [`dump`]; the result compares equal to the
/// original `Region` (round-trip).
pub fn parse(text: &str) -> Result<Region, String> {
    let mut lines = text.lines().filter(|l| !l.trim().is_empty());
    let header = lines.next().ok_or("empty dump")?;
    if header.trim() != "# dq8 block-ir v1" {
        return Err(format!("bad header '{header}'"));
    }
    let region_line = lines.next().ok_or("missing region line")?;
    let region_line = region_line
        .strip_prefix("region ")
        .ok_or_else(|| format!("bad region line '{region_line}'"))?;
    let mut base = None;
    let mut start = None;
    let mut end = None;
    let mut block_count = None;
    for field in region_line.split_whitespace() {
        let (k, v) = field
            .split_once('=')
            .ok_or_else(|| format!("bad region field '{field}'"))?;
        match k {
            "base" => base = Some(parse_hex_u32(v)?),
            "start" => start = Some(parse_hex_u32(v)?),
            "end" => end = Some(parse_hex_u32(v)?),
            "blocks" => {
                block_count = Some(
                    v.parse::<usize>()
                        .map_err(|_| format!("bad block count '{v}'"))?,
                )
            }
            _ => return Err(format!("unknown region field '{k}'")),
        }
    }

    let mut blocks: Vec<Block> = Vec::new();
    let mut current: Option<Block> = None;
    for line in lines {
        let line = line.trim_end();
        if let Some(rest) = line.strip_prefix("block ") {
            if let Some(block) = current.take() {
                blocks.push(block);
            }
            current = Some(parse_block_line(rest)?);
        } else if let Some(rest) = line.strip_prefix("  insn ") {
            let block = current.as_mut().ok_or("insn before block")?;
            block.insns.push(parse_insn_line(rest)?);
        } else {
            return Err(format!("unexpected line '{line}'"));
        }
    }
    if let Some(block) = current.take() {
        blocks.push(block);
    }
    if let Some(want) = block_count {
        if want != blocks.len() {
            return Err(format!("block count {want} != {} parsed", blocks.len()));
        }
    }
    Ok(Region {
        base: base.ok_or("missing base")?,
        start: start.ok_or("missing start")?,
        end: end.ok_or("missing end")?,
        blocks,
    })
}

fn parse_block_line(rest: &str) -> Result<Block, String> {
    // `exit=` is last and its value contains spaces; split it off first.
    let exit_pos = rest.find(" exit=").ok_or("missing block exit")?;
    let fields = &rest[..exit_pos];
    let exit = parse_exit(&rest[exit_pos + 6..])?;
    let mut bstart = None;
    let mut bend = None;
    let mut reads = RegSet::new();
    let mut writes = RegSet::new();
    let mut flag_reads = FlagSet::new();
    let mut flag_writes = FlagSet::new();
    for field in fields.split_whitespace() {
        let (k, v) = field
            .split_once('=')
            .ok_or_else(|| format!("bad block field '{field}'"))?;
        match k {
            "start" => bstart = Some(parse_hex_u32(v)?),
            "end" => bend = Some(parse_hex_u32(v)?),
            "insns" => {}
            "reads" => reads = parse_reg_list(v)?,
            "writes" => writes = parse_reg_list(v)?,
            "flag_reads" => flag_reads = parse_flag_list(v)?,
            "flag_writes" => flag_writes = parse_flag_list(v)?,
            _ => return Err(format!("unknown block field '{k}'")),
        }
    }
    Ok(Block {
        start: bstart.ok_or("missing block start")?,
        end: bend.ok_or("missing block end")?,
        insns: Vec::new(),
        reads,
        writes,
        flag_reads,
        flag_writes,
        exit,
    })
}

fn parse_insn_line(rest: &str) -> Result<Insn, String> {
    let mut addr = None;
    let mut word = None;
    let mut cond = None;
    let mut kind = None;
    let mut rd = NO_REG;
    let mut rn = NO_REG;
    let mut rm = NO_REG;
    let mut imm = None;
    let mut shift = Shift::None;
    let mut sets_flags = false;
    let mut reads = RegSet::new();
    let mut writes = RegSet::new();
    let mut flag_reads = FlagSet::new();
    let mut flag_writes = FlagSet::new();
    for field in rest.split_whitespace() {
        let (k, v) = field
            .split_once('=')
            .ok_or_else(|| format!("bad insn field '{field}'"))?;
        match k {
            "addr" => addr = Some(parse_hex_u32(v)?),
            "word" => word = Some(parse_hex_u32(v)?),
            "cond" => cond = Some(cond_from_name(v).ok_or_else(|| format!("bad cond '{v}'"))?),
            "kind" => kind = Some(parse_kind(v)?),
            "rd" => rd = parse_reg_field(v)?,
            "rn" => rn = parse_reg_field(v)?,
            "rm" => rm = parse_reg_field(v)?,
            "imm" => imm = parse_opt_hex(v)?,
            "shift" => shift = parse_shift(v)?,
            "flags" => sets_flags = v == "S",
            "reads" => reads = parse_reg_list(v)?,
            "writes" => writes = parse_reg_list(v)?,
            "flag_reads" => flag_reads = parse_flag_list(v)?,
            "flag_writes" => flag_writes = parse_flag_list(v)?,
            _ => return Err(format!("unknown insn field '{k}'")),
        }
    }
    Ok(Insn {
        addr: addr.ok_or("missing insn addr")?,
        word: word.ok_or("missing insn word")?,
        cond: cond.ok_or("missing insn cond")?,
        kind: kind.ok_or("missing insn kind")?,
        rd,
        rn,
        rm,
        imm,
        shift,
        sets_flags,
        reads,
        writes,
        flag_reads,
        flag_writes,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The 26 A32 words of the Perf-G-selected movie region
    /// `0x001A93A8..0x001A9410` (packed-field unpack loop plus its
    /// tail-return), copied from `DQ8.code`.
    const REGION_WORDS: [u32; 26] = [
        0xE1A0_4CA3, 0xE354_0003, 0x0AFF_FFC2, 0xE1A0_4A23, 0xE08B_4084, 0xE1D4_40B0, 0xE204_500F,
        0xE1A0_4224, 0xE204_601F, 0xE1A0_42A4, 0xE1B0_3513, 0x2266_6000, 0xE052_2005, 0xBBFF_F821,
        0xE204_503F, 0xE1A0_4324, 0xE08C_C105, 0xE49C_8004, 0xE208_50FF, 0xE1A0_7428, 0xE007_0796,
        0xE280_8F6E, 0xE788_7105, 0xE314_0001, 0x0AFF_FFE6, 0xE49D_F004,
    ];
    const REGION_START: u32 = 0x001A_93A8;
    const REGION_END: u32 = 0x001A_9410;

    fn region_bytes() -> Vec<u8> {
        let mut bytes = vec![0u8; (REGION_END - GUEST_BASE) as usize];
        for (i, word) in REGION_WORDS.iter().enumerate() {
            let off = (REGION_START - GUEST_BASE) as usize + i * 4;
            bytes[off..off + 4].copy_from_slice(&word.to_le_bytes());
        }
        bytes
    }

    fn movie_region() -> Region {
        extract_region(&region_bytes(), GUEST_BASE, REGION_START, REGION_END).expect("extract")
    }

    #[test]
    fn extracts_expected_block_boundaries() {
        let region = movie_region();
        let starts: Vec<u32> = region.blocks.iter().map(|b| b.start).collect();
        // Leaders: region entry, the fall-through of the conditional `beq`,
        // the fall-through of the conditional `bllt`, and the fall-through of
        // the loop-back `beq`.  A control-flow instruction ENDS its block; it
        // does not start one.
        assert_eq!(
            starts,
            vec![
                0x001A_93A8, // block 0: lsr, cmp, beq
                0x001A_93B4, // block 1: unpack straight-line … subs, bllt
                0x001A_93E0, // block 2: second half … tst, beq
                0x001A_940C, // block 3: tail return
            ]
        );
        let ends: Vec<u32> = region.blocks.iter().map(|b| b.end).collect();
        assert_eq!(
            ends,
            vec![
                0x001A_93B4, // 3 insns
                0x001A_93E0, // 11 insns: lsr add ldrh and lsr and lsr lsls rsbhs subs bllt
                0x001A_940C, // 11 insns: and lsr add ldr and lsr mul add str tst beq
                0x001A_9410, // 1 insn: ldr pc,[sp],#4
            ]
        );
        assert_eq!(region.blocks[1].insns.len(), 11);
        assert_eq!(region.blocks[2].insns.len(), 11);
    }

    #[test]
    fn block_exits_match_control_flow() {
        let region = movie_region();
        assert_eq!(
            region.blocks[0].exit,
            Exit::Branch {
                target: 0x001A_92C0,
                cond: 0x0,
                fallthrough: Some(0x001A_93B4),
            }
        );
        // The conditional `bllt` (0x001A93DC) ends block 1 as a call whose
        // target comes from the same maths the dispatcher uses.
        let (bl_target, _, _) = arm_branch_target(0xBBFF_F821, 0x001A_93DC).expect("branch target");
        match region.blocks[1].exit {
            Exit::Call {
                target,
                cond,
                fallthrough,
            } => {
                assert_eq!(target, bl_target);
                assert_eq!(cond, 0xB); // LT
                assert_eq!(fallthrough, Some(0x001A_93E0));
            }
            other => panic!("expected call exit, got {other:?}"),
        }
        assert_eq!(
            region.blocks[2].exit,
            Exit::Branch {
                target: 0x001A_93A8,
                cond: 0x0,
                fallthrough: Some(0x001A_940C),
            }
        );
        assert_eq!(region.blocks[3].exit, Exit::Return);
    }

    #[test]
    fn instruction_semantics_match_encodings() {
        let region = movie_region();
        let insn = |addr: u32| -> Insn {
            region
                .blocks
                .iter()
                .flat_map(|b| b.insns.iter())
                .find(|i| i.addr == addr)
                .copied()
                .unwrap_or_else(|| panic!("no insn at 0x{addr:08X}"))
        };

        // 0x001A93A8: lsr r4, r3, #25 -> reads r3, writes r4, no flags.
        let i = insn(0x001A_93A8);
        assert_eq!(i.kind, Kind::Dp { op: DP::Mov });
        assert_eq!(i.shift, Shift::Imm { typ: 1, amount: 25 });
        assert!(i.reads.contains(3) && !i.reads.contains(4));
        assert!(i.writes.contains(4) && !i.writes.contains(3));
        assert!(i.flag_writes.is_empty());

        // 0x001A93AC: cmp r4, #3 -> reads r4, writes N,Z,C,V, writes nothing.
        let i = insn(0x001A_93AC);
        assert_eq!(i.kind, Kind::Dp { op: DP::Cmp });
        assert_eq!(i.imm, Some(3));
        assert!(i.reads.contains(4));
        assert!(i.writes.is_empty());
        assert_eq!(i.flag_writes.0, NZCV);

        // 0x001A93BC: ldrh r4, [r4] -> halfword load, no writeback.
        let i = insn(0x001A_93BC);
        assert_eq!(
            i.kind,
            Kind::Mem {
                load: true,
                size: Size::Half,
                pre: true,
                up: true,
                wb: false,
                reg_off: false,
            }
        );
        // Bit 22 set means the immediate form with a zero offset here.
        assert_eq!(i.imm, Some(0));
        assert!(i.reads.contains(4) && i.writes.contains(4));
        assert!(!i.reads.contains(0), "no phantom r0 read from the imm form");

        // 0x001A93D0: lsls r3, r3, r5 -> S-form logical with register
        // shift: reads r3,r5 writes r3 and N,Z,C (dynamic shifter carry).
        let i = insn(0x001A_93D0);
        assert_eq!(i.kind, Kind::Dp { op: DP::Mov });
        assert!(i.sets_flags);
        assert_eq!(i.flag_writes.0, NZC);
        assert!(i.reads.contains(3) && i.reads.contains(5));
        assert!(i.writes.contains(3) && !i.writes.contains(5));

        // 0x001A93D4: rsbhs r6, r6, #0 -> predicated (reads C), no flags.
        let i = insn(0x001A_93D4);
        assert_eq!(i.cond, 0x2); // HS/CS
        assert_eq!(i.kind, Kind::Dp { op: DP::Rsb });
        assert!(i.flag_reads.contains(FLAG_C));
        assert!(!i.sets_flags);
        assert!(i.reads.contains(6) && i.writes.contains(6));

        // 0x001A93D8: subs r2, r2, r5 -> writes N,Z,C,V; reads r2,r5.
        let i = insn(0x001A_93D8);
        assert_eq!(i.kind, Kind::Dp { op: DP::Sub });
        assert_eq!(i.flag_writes.0, NZCV);
        assert!(i.reads.contains(2) && i.reads.contains(5) && i.writes.contains(2));

        // 0x001A93F8: mul r7, r6, r7 -> reads r6,r7 writes r7, no flags.
        let i = insn(0x001A_93F8);
        assert_eq!(i.kind, Kind::Mul { add: false });
        assert!(i.reads.contains(6) && i.reads.contains(7));
        assert!(i.writes.contains(7));
        assert!(!i.sets_flags);

        // 0x001A93EC: ldr r8, [r12], #4 -> post-indexed: reads r12, writes
        // r8 and the base r12 (writeback).
        let i = insn(0x001A_93EC);
        assert_eq!(
            i.kind,
            Kind::Mem {
                load: true,
                size: Size::Word,
                pre: false,
                up: true,
                wb: true,
                reg_off: false,
            }
        );
        assert_eq!(i.imm, Some(4));
        assert!(i.reads.contains(12));
        assert!(i.writes.contains(8) && i.writes.contains(12));

        // 0x001A9400: str r7, [r8, r5, lsl #2] -> reads r8,r5,r7, no writes.
        let i = insn(0x001A_9400);
        assert_eq!(
            i.kind,
            Kind::Mem {
                load: false,
                size: Size::Word,
                pre: true,
                up: true,
                wb: false,
                reg_off: true,
            }
        );
        assert_eq!(i.shift, Shift::Imm { typ: 0, amount: 2 });
        assert!(i.reads.contains(8) && i.reads.contains(5) && i.reads.contains(7));
        assert!(i.writes.is_empty());

        // 0x001A9404: tst r4, #1 -> flag-only test; the immediate rotation is
        // zero, so the shifter leaves C untouched and only N,Z are written.
        let i = insn(0x001A_9404);
        assert_eq!(i.kind, Kind::Dp { op: DP::Tst });
        assert_eq!(i.flag_writes.0, NZ);
        assert!(i.writes.is_empty());

        // 0x001A93DC: bllt -> conditional call; writes lr like the frontend
        // (the generated case is `if (n != v) { r[14] = 0x001A93E0; pc =
        // 0x001A7468; }`).
        let i = insn(0x001A_93DC);
        assert_eq!(
            i.kind,
            Kind::Branch {
                link: true,
                target: 0x001A_7468
            }
        );
        assert!(i.writes.contains(14) && i.reads.is_empty());

        // 0x001A940C: ldr pc, [sp], #4 -> return; reads sp, writes sp.
        let i = insn(0x001A_940C);
        assert_eq!(
            i.kind,
            Kind::LoadPc {
                pre: false,
                up: true,
                wb: true
            }
        );
        assert!(i.reads.contains(13) && i.writes.contains(13));
    }

    #[test]
    fn conditional_control_reads_its_condition_flags() {
        let region = movie_region();
        // The `beq` at the end of block 0 reads Z.
        assert!(region.blocks[0].flag_reads.contains(FLAG_Z));
        // The `bllt` that ends block 1 reads N and V.
        assert!(region.blocks[1].flag_reads.contains(FLAG_N));
        assert!(region.blocks[1].flag_reads.contains(FLAG_V));
        // The `subs` inside block 1 writes N,Z,C,V.
        assert_eq!(region.blocks[1].flag_writes.0, NZCV);
        // The `tst` inside block 2 writes N,Z.
        assert_eq!(region.blocks[2].flag_writes.0, NZ);
    }

    #[test]
    fn block_sets_are_unions_of_their_instructions() {
        let region = movie_region();
        for block in &region.blocks {
            for insn in &block.insns {
                for r in insn.reads.iter() {
                    assert!(block.reads.contains(r), "block must inherit read r{r}");
                }
                for r in insn.writes.iter() {
                    assert!(block.writes.contains(r), "block must inherit write r{r}");
                }
                for f in [FLAG_N, FLAG_Z, FLAG_C, FLAG_V] {
                    if insn.flag_reads.contains(f) {
                        assert!(
                            block.flag_reads.contains(f),
                            "block must inherit flag read {f:#x}"
                        );
                    }
                    if insn.flag_writes.contains(f) {
                        assert!(
                            block.flag_writes.contains(f),
                            "block must inherit flag write {f:#x}"
                        );
                    }
                }
            }
        }
        // Spot-check the second half (block 2): the `ldr` reads r12 and
        // writes r8; only the `tst` writes flags.
        let block = &region.blocks[2];
        assert!(block.reads.contains(12) && block.writes.contains(8));
        assert_eq!(block.flag_writes.0, NZ);
    }

    #[test]
    fn dump_round_trips_to_an_identical_region() {
        let region = movie_region();
        let text = dump(&region);
        let parsed = parse(&text).expect("parse");
        assert_eq!(parsed, region, "parsed region must equal the extracted one");
        // And the dump of the parsed region is byte-identical.
        assert_eq!(dump(&parsed), text);
    }

    #[test]
    fn dump_is_deterministic() {
        assert_eq!(dump(&movie_region()), dump(&movie_region()));
    }

    #[test]
    fn unknown_encodings_are_preserved_not_guessed() {
        let mut bytes = region_bytes();
        // 0xFF000000 sits in the SWI space (bits 27..25 = 111), which this
        // bounded decoder deliberately does not interpret.
        let off = (REGION_START - GUEST_BASE) as usize;
        bytes[off..off + 4].copy_from_slice(&0xFF00_0000u32.to_le_bytes());
        let region = extract_region(&bytes, GUEST_BASE, REGION_START, REGION_END).expect("extract");
        let insn = &region.blocks[0].insns[0];
        assert_eq!(insn.kind, Kind::Unknown);
        assert_eq!(insn.word, 0xFF00_0000);
        // Round-trip still holds with an unknown kind.
        let text = dump(&region);
        assert_eq!(dump(&parse(&text).expect("parse")), text);
    }

    #[test]
    fn rejects_misaligned_or_out_of_image_regions() {
        let bytes = region_bytes();
        assert!(extract_region(&bytes, GUEST_BASE, REGION_START + 2, REGION_END).is_err());
        assert!(extract_region(&bytes, GUEST_BASE, REGION_START, REGION_START).is_err());
        // End beyond the image the buffer can serve.
        assert!(extract_region(&bytes, GUEST_BASE, REGION_START, 0x0020_0000).is_err());
    }

    /// The extractor must produce identical IR for the real code image when
    /// it is present next to the crate root (it is, in the DQ8 worktrees).
    #[test]
    fn extracts_the_real_movie_region_when_image_is_present() {
        let path = std::path::Path::new("DQ8.code");
        let Ok(bytes) = std::fs::read(path) else {
            eprintln!("skipping: DQ8.code not present next to the crate root");
            return;
        };
        let region =
            extract_region(&bytes, GUEST_BASE, REGION_START, REGION_END).expect("extract real");
        assert_eq!(region.blocks.len(), 4);
        let starts: Vec<u32> = region.blocks.iter().map(|b| b.start).collect();
        assert_eq!(
            starts,
            vec![0x001A_93A8, 0x001A_93B4, 0x001A_93E0, 0x001A_940C]
        );
        // The real image must decode identically to the synthetic copy.
        assert_eq!(dump(&region), dump(&movie_region()));
    }

    /// A data-processing jump (`mov pc, lr`, `add pc, …`) must END its block
    /// with an `Indirect` exit and its sequential successor must start a new
    /// block — otherwise a native-CFG region keeps executing the statements
    /// after the jump, which the dispatch loop never does (it re-dispatches at
    /// the pc the statement wrote).
    #[test]
    fn dp_to_pc_ends_its_block_and_hands_back_to_the_dispatcher() {
        let path = std::path::Path::new("DQ8.code");
        let Ok(bytes) = std::fs::read(path) else {
            eprintln!("skipping: DQ8.code not present next to the crate root");
            return;
        };
        // Unconditional: 0x001A9E1C is `mov pc, lr` (cond=al).
        let region = extract_region(&bytes, GUEST_BASE, 0x001A_9E10, 0x001A_9E28)
            .expect("extract");
        let jump = region
            .blocks
            .iter()
            .find(|b| b.insns.last().map(|i| i.addr) == Some(0x001A_9E1C))
            .expect("mov pc, lr must end its own block");
        assert!(matches!(jump.exit, Exit::Indirect), "exit = {:?}", jump.exit);
        assert!(
            region.blocks.iter().any(|b| b.start == 0x001A_9E20),
            "the instruction after the jump must start a new block"
        );
        // Conditional: 0x001AB194 is `moveq pc, lr`.  The lowering writes pc
        // on both paths (taken -> value & ~3, else -> addr + 4), so the same
        // `Indirect` -> L_EXIT edge is equivalent for either branch direction.
        let region2 = extract_region(&bytes, GUEST_BASE, 0x001A_B190, 0x001A_B1A0)
            .expect("extract");
        let cond_jump = region2
            .blocks
            .iter()
            .find(|b| b.insns.last().map(|i| i.addr) == Some(0x001A_B194))
            .expect("moveq pc, lr must end its own block");
        assert!(
            matches!(cond_jump.exit, Exit::Indirect),
            "exit = {:?}",
            cond_jump.exit
        );
    }
}
