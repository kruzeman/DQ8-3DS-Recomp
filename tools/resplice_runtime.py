#!/usr/bin/env python3
"""Re-splice src/pica_gl.c / src/pica_vs.c (and friends) into the generated
native runtime TU ($SPLIT/dq8_main.c) without re-running the ARM recompiler.

Why this exists: the Rust tool embeds the runtime sources with include_str!()
and substitutes them at marker sites (src/main.rs, "/* PICA_GL_RUNTIME */" et
al).  That means every edit to src/pica_gl.c normally forces
`cargo build` + a full `recompile-arm` pass over the 5 MB .code image (~10+
minutes) just to refresh a C file that is a pure concatenation.  The generation
is deterministic: dq8_main.c == template.replace(marker, include_str!(file)),
so replacing the OLD file body with the NEW one inside the already generated
dq8_main.c is byte-identical to a full regeneration (for that splice).

usage: tools/resplice_runtime.py <split-dir> [--check]

  --check   only report whether the splices are current (exit 1 if stale).

The "old" body is taken from git (HEAD:src/<file>), i.e. exactly what the last
generated dq8_main.c was built from, unless the run was already re-spliced, in
which case the current src/<file> is used and the replace is a no-op.
"""
import os
import shutil
import subprocess
import sys

SITES = [
    ("pica_gl.c", "/* PICA_GL_RUNTIME */"),
    ("pica_vs.c", "/* PICA_VS_RUNTIME */"),
    ("frame_prof.c", "/* FRAME_PROF_RUNTIME */"),
]


def git_head(path):
    try:
        return subprocess.run(["git", "show", "HEAD:" + path],
                              capture_output=True, check=True).stdout.decode()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    split = argv[1]
    check = "--check" in argv
    main_c = os.path.join(split, "dq8_main.c")
    state = os.path.join(split, ".splice")
    os.makedirs(state, exist_ok=True)
    with open(main_c, encoding="utf-8", errors="surrogateescape") as f:
        text = f.read()
    changed = 0
    updated = []
    for name, marker in SITES:
        src_path = os.path.join("src", name)
        with open(src_path, encoding="utf-8", errors="surrogateescape") as f:
            new = f.read()
        if new in text:
            if not check:
                shutil.copyfile(src_path, os.path.join(state, name))
            continue                       # already current
        last = os.path.join(state, name)
        if os.path.exists(last):
            with open(last, encoding="utf-8", errors="surrogateescape") as f:
                old = f.read()
        else:
            old = git_head(src_path)
        if old is None or old not in text:
            print("resplice: cannot locate the %s body in %s — "
                  "run a full build_split.sh regen" % (name, main_c),
                  file=sys.stderr)
            return 2
        if check:
            print("resplice: %s is STALE in %s" % (name, main_c))
            changed += 1
            continue
        text = text.replace(old, new, 1)
        changed += 1
        print("resplice: %s spliced (%d -> %d bytes)"
              % (name, len(old), len(new)))
        # Keep the state copy in sync with the body that is now in dq8_main.c.
        # Without this the NEXT edit compares against the pre-splice body and
        # fails with "cannot locate ... run a full regen", which is exactly the
        # trap that cost a full rebuild before (t_7d18d1b6).
        updated.append((src_path, os.path.join(state, name)))
    if check:
        return 1 if changed else 0
    if changed:
        with open(main_c, "w", encoding="utf-8", errors="surrogateescape") as f:
            f.write(text)
        for src_path, state_path in updated:
            shutil.copyfile(src_path, state_path)
        print("resplice: wrote %s" % main_c)
    else:
        print("resplice: %s already current" % main_c)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
