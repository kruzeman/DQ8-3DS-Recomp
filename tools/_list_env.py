#!/usr/bin/env python3
"""Print a fn_emit list file (one `0xENTRY 0xEND` pair per line) as the comma
separated DQ8_FN_EMIT value, for a bounded runtime subset."""
import sys

pairs = []
for line in open(sys.argv[1]):
    line = line.strip()
    if not line or line.startswith("#"):
        continue
    f = line.split()
    if len(f) >= 2 and f[0].startswith("0x"):
        pairs.append(f[0])
print(",".join(pairs))
