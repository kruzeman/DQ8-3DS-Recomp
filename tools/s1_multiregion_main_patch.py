#!/usr/bin/env python3
"""One-shot source edit: multi-region S1 rollout support in the recompiler.

`recompile-arm` now accepts repeated --s1-cfg A --s1-cfg-end B pairs, emits all
of them into one generated file, and registers every entry in the A05 boundary
set.  Refuses to run twice.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TARGET = ROOT / "src" / "main.rs"

SIG_OLD = "    s1_cfg: Option<(u32, u32)>,\n"
SIG_NEW = "    s1_cfg: Option<Vec<(u32, u32)>>,\n"

STRUCT_ANCHOR = "fn recompile_arm_function_with_noreturn(\n"
STRUCT_NEW = '''/// S1 rollout: every region converted in ONE build (the multi-region
/// `DQ8_S1_CFG` gate), with the concatenated C splices for the three
/// `S1_CFG_*` markers.  `entries` are the region entries (the A05 boundary
/// set and the runtime gate both need them).
struct S1CfgRegions {
    entries: Vec<u32>,
    function_c: String,
    hook_c: String,
    gate_c: String,
}

'''

EMIT_START = "    let s1_cfg = match s1_cfg {\n"
EMIT_END = "    let mut c = String::from(\n"

EMIT_NEW = '''    let s1_cfg = match s1_cfg {
        None => None,
        Some(regions) => {
            let mut function_c = String::new();
            let mut hook_c = String::new();
            let mut gate_c = String::new();
            let mut entries = Vec::new();
            for (entry, end) in regions {
                if entry & 3 != 0 {
                    return Err(format!("--s1-cfg 0x{entry:08X} is not word aligned"));
                }
                if end <= entry {
                    return Err(format!(
                        "--s1-cfg-end 0x{end:08X} must be above the region entry 0x{entry:08X}"
                    ));
                }
                let Some(off) = entry.checked_sub(base).map(|v| v as usize) else {
                    return Err(format!("--s1-cfg 0x{entry:08X} is below the code base"));
                };
                if off + 4 > bytes.len() || (end - base) as usize > bytes.len() {
                    return Err(format!(
                        "--s1-cfg region 0x{entry:08X}..0x{end:08X} leaves the code image"
                    ));
                }
                let region = block_ir::extract_region(&bytes, base, entry, end)
                    .map_err(|e| format!("--s1-cfg 0x{entry:08X}: {e}"))?;
                if region.start != entry {
                    return Err(format!(
                        "--s1-cfg 0x{entry:08X}: no basic block starts at the entry"
                    ));
                }
                let part = block_emit::emit_region(&region, &emitted)
                    .map_err(|e| format!("--s1-cfg 0x{entry:08X}..0x{end:08X}: {e}"))?;
                entries.push(part.entry);
                function_c.push_str(&part.function_c);
                hook_c.push_str(&part.hook_c);
                gate_c.push_str(&part.gate_c);
            }
            Some(S1CfgRegions {
                entries,
                function_c,
                hook_c,
                gate_c,
            })
        }
    };
'''

BOUNDARY_OLD = '''    if let Some(cfg) = &s1_cfg {
        boundary_pcs.push(cfg.entry);
    }
'''
BOUNDARY_NEW = '''    if let Some(cfg) = &s1_cfg {
        for entry in &cfg.entries {
            boundary_pcs.push(*entry);
        }
    }
'''

CLI_DECL_OLD = '''            let mut s1_cfg_entry: Option<u32> = None;
            let mut s1_cfg_end: Option<u32> = None;
'''
CLI_DECL_NEW = '''            let mut s1_cfg_entry: Option<u32> = None;
            let mut s1_cfg_regions: Vec<(u32, u32)> = Vec::new();
'''

CLI_ARMS_OLD = '''                    "--s1-cfg" => s1_cfg_entry = Some(parsed),
                    "--s1-cfg-end" => s1_cfg_end = Some(parsed),
'''
CLI_ARMS_NEW = '''                    "--s1-cfg" => s1_cfg_entry = Some(parsed),
                    "--s1-cfg-end" => {
                        let entry = s1_cfg_entry
                            .take()
                            .ok_or("--s1-cfg-end requires --s1-cfg")?;
                        s1_cfg_regions.push((entry, parsed));
                    }
'''

CLI_MATCH_OLD = '''            // S1c: the region is selected by its entry AND its exclusive end.
            let s1_cfg = match (s1_cfg_entry, s1_cfg_end) {
                (None, None) => None,
                (Some(entry), Some(end)) => Some((entry, end)),
                (Some(_), None) => return Err("--s1-cfg requires --s1-cfg-end".into()),
                (None, Some(_)) => return Err("--s1-cfg-end requires --s1-cfg".into()),
            };
'''
CLI_MATCH_NEW = '''            // S1c: each region is selected by its entry AND its exclusive
            // end; the rollout passes several --s1-cfg/--s1-cfg-end pairs to
            // convert several regions in one build.
            if s1_cfg_entry.is_some() {
                return Err("--s1-cfg requires --s1-cfg-end".into());
            }
            let s1_cfg = if s1_cfg_regions.is_empty() {
                None
            } else {
                Some(s1_cfg_regions)
            };
'''

REPLACEMENTS = [
    ("SIG", SIG_OLD, SIG_NEW, 1),
    ("STRUCT", STRUCT_ANCHOR, STRUCT_NEW, 1),
    ("BOUNDARY", BOUNDARY_OLD, BOUNDARY_NEW, 1),
    ("CLI_DECL", CLI_DECL_OLD, CLI_DECL_NEW, 1),
    ("CLI_ARMS", CLI_ARMS_OLD, CLI_ARMS_NEW, 1),
    ("CLI_MATCH", CLI_MATCH_OLD, CLI_MATCH_NEW, 1),
]


def main() -> int:
    src = TARGET.read_text()
    if "struct S1CfgRegions" in src:
        print("already applied")
        return 0
    for name, old, _new, want in REPLACEMENTS:
        n = src.count(old)
        if n != want:
            print(f"{name}: {n} matches (need {want})")
            return 1

    src = src.replace(SIG_OLD, SIG_NEW)
    src = src.replace(STRUCT_ANCHOR, STRUCT_NEW + STRUCT_ANCHOR)

    i = src.find(EMIT_START)
    j = src.find(EMIT_END, i)
    if i < 0 or j < 0:
        print(f"emit block not found (i={i}, j={j})")
        return 1
    src = src[:i] + EMIT_NEW + src[j:]

    src = src.replace(BOUNDARY_OLD, BOUNDARY_NEW)
    src = src.replace(CLI_DECL_OLD, CLI_DECL_NEW)
    src = src.replace(CLI_ARMS_OLD, CLI_ARMS_NEW)
    src = src.replace(CLI_MATCH_OLD, CLI_MATCH_NEW)

    TARGET.write_text(src)
    print(f"patched {TARGET}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
