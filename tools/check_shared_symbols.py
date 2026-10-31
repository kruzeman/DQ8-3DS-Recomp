#!/usr/bin/env python3
# List function-like identifiers referenced by the generated dispatch cases of
# target/dq8-startup.c that are NOT declared in tools/split_dispatch.py's
# SHARED_SYMBOLS list (i.e. the set the part TUs can see).  This catches every
# missing header declaration in one pass instead of one build failure at a time.
import re
import sys

sys.path.insert(0, "/home/hermes/DQ8/tools")
import split_dispatch  # noqa: E402

known = set(split_dispatch.SHARED_SYMBOLS)
libc = {
    "fprintf", "fputc", "fopen", "fclose", "fread", "fwrite", "getenv", "exit",
    "memcpy", "memset", "memcmp", "memmove", "strcpy", "strcmp", "strlen",
    "sprintf", "snprintf", "malloc", "calloc", "realloc", "free", "abort",
    "sqrt", "pow", "floor", "ceil", "round", "fmod", "sin", "cos", "tan",
    "usleep", "sched_yield", "printf", "puts", "putchar", "atoi", "strtoul",
    "isnan", "isfinite", "fabs", "llrint", "lrint", "fmax", "fmin", "qsort",
}
keywords = {"if", "for", "while", "switch", "return", "sizeof", "typeof"}

calls = {}
pat = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")
with open("/home/hermes/DQ8/target/dq8-startup.c") as fh:
    for line in fh:
        if "case 0x" not in line:
            continue
        for name in pat.findall(line):
            if name in keywords or name in known or name in libc:
                continue
            calls[name] = calls.get(name, 0) + 1

for name, count in sorted(calls.items()):
    print("%-32s %d" % (name, count))
