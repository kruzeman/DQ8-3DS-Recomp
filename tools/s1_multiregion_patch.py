#!/usr/bin/env python3
"""One-shot source edit: make the S1c region emitter multi-region capable.

Each emitted region owns entry-suffixed statics (`dq8_s1_cfg_runs_001A94F4`
...) so several regions can live in one generated file, and the `DQ8_S1_CFG`
runtime gate parses a comma/colon list of entries (plus the token `all`)
instead of a single value.  The report line format
(`S1_CFG entry=... runs=... insns=...`) is unchanged, so the existing
harnesses (tools/s1d_ab.py, tools/stop1_gate_runs.py) keep working.

Run from the repo root; refuses to run twice.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TARGET = ROOT / "src" / "block_emit.rs"

SYM_ANCHOR = """    let entry = region.start;
    let end = region.end;
    let name = cfg_fn_name(entry);
"""

SYM_NEW = """    let entry = region.start;
    let end = region.end;
    let name = cfg_fn_name(entry);
    // Per-entry static names: a rollout build emits SEVERAL regions into one
    // generated file, so every global this region owns carries its entry.
    let s_runs = format!("dq8_s1_cfg_runs_{entry:08X}");
    let s_insns = format!("dq8_s1_cfg_insns_{entry:08X}");
    let s_enabled = format!("dq8_s1_cfg_enabled_{entry:08X}");
    let s_report = format!("dq8_s1_cfg_report_{entry:08X}");
"""

GATE_START = ("    // Built without backslash-escaped quotes: `q` is a double quote, "
              "so the")
GATE_END = "    Ok(EmittedRegion {"

NEW_GATE = '''    // Built without backslash-escaped quotes: `q` is a double quote, so the
    // emitted C cannot pick up stray backslashes from Rust string escaping.
    // The gate parses a LIST (comma/colon/semicolon separated, or the token
    // `all`) so one build can carry several converted regions; a single value
    // behaves exactly like the S1c single-region gate.
    let q = '"';
    let gate_c = format!(
        "    {{ const char *s1_list = getenv({q}DQ8_S1_CFG{q});\\n\\
         \\x20     if (s1_list && *s1_list) {{\\n\\
         \\x20         int s1_hit = 0, s1_toks = 0;\\n\\
         \\x20         const char *s1_p = s1_list;\\n\\
         \\x20         while (*s1_p) {{\\n\\
         \\x20             const char *s1_q = s1_p;\\n\\
         \\x20             while (*s1_q && *s1_q != ',' && *s1_q != ':' && *s1_q != ';') ++s1_q;\\n\\
         \\x20             char s1_tok[40];\\n\\
         \\x20             size_t s1_len = (size_t)(s1_q - s1_p);\\n\\
         \\x20             if (s1_len >= sizeof s1_tok) s1_len = sizeof s1_tok - 1u;\\n\\
         \\x20             memcpy(s1_tok, s1_p, s1_len); s1_tok[s1_len] = 0;\\n\\
         \\x20             if (s1_len) {{\\n\\
         \\x20                 ++s1_toks;\\n\\
         \\x20                 if (!strcmp(s1_tok, {q}all{q})) s1_hit = 1;\\n\\
         \\x20                 else {{ char *s1_e = 0; unsigned long long s1_v = strtoull(s1_tok, &s1_e, 0);\\n\\
         \\x20                        if (s1_e != s1_tok && s1_v == 0x{entry:08X}ull) s1_hit = 1; }}\\n\\
         \\x20             }}\\n\\
         \\x20             s1_p = (*s1_q ? s1_q + 1 : s1_q);\\n\\
         \\x20         }}\\n\\
         \\x20         {s_enabled} = s1_hit;\\n\\
         \\x20         if (s1_hit) atexit({s_report});\\n\\
         \\x20         else if (s1_toks == 1)\\n\\
         \\x20             fprintf(stderr, {q}DQ8_S1_CFG=%s does not match the emitted region entry 0x{entry:08X}; hook disabled\\\\n{q}, s1_list);\\n\\
         \\x20     }} }}\\n",
        entry = entry,
        q = q,
        s_enabled = s_enabled,
        s_report = s_report,
    );

'''

RENAMES = [
    ("dq8_s1_cfg_runs", "{s_runs}"),
    ("dq8_s1_cfg_insns", "{s_insns}"),
    ("dq8_s1_cfg_enabled", "{s_enabled}"),
    ("dq8_s1_cfg_report", "{s_report}"),
]


def main() -> int:
    src = TARGET.read_text()
    if "s_enabled = s_enabled" in src:
        print("already applied")
        return 0
    i = src.find(GATE_START)
    j = src.find(GATE_END, i)
    if i < 0 or j < 0 or src.count(GATE_START) != 1:
        print(f"gate block: start={i} end={j} count={src.count(GATE_START)}")
        return 1
    src = src[:i] + NEW_GATE + src[j:]

    for old, new in RENAMES:
        n = src.count(old)
        if n == 0:
            print(f"no occurrences of {old}")
            return 1
        src = src.replace(old, new)
        print(f"  {old} -> {new}  ({n} sites)")

    if src.count(SYM_ANCHOR) != 1:
        print(f"SYM_ANCHOR: {src.count(SYM_ANCHOR)} matches (need 1)")
        return 1
    src = src.replace(SYM_ANCHOR, SYM_NEW)

    TARGET.write_text(src)
    print(f"patched {TARGET}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
