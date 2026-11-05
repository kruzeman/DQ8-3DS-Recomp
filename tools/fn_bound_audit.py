#!/usr/bin/env python3
"""Audit the emitted FN_EMIT shards: is every local the body *uses* loaded, and
is every local the body *writes* stored?  (t_21dda9b5)"""
import glob
import re
import sys

shards = sorted(glob.glob((sys.argv[1] if len(sys.argv) > 1 else 'target/fnbound-split')
                          + '/dq8_fnshard_*.c'))
name_re = re.compile(r'^unsigned long long (dq8_fnb_[0-9A-F]+)\(Dq8FnState \*st, uint32_t resume\) \{')
local_re = re.compile(r'\b(r(?:1[0-4]|[0-9])|running|ge|n|z|c|v)\b')
load_re = re.compile(r'^\s+(r(?:1[0-4]|[0-9])|running|ge|n|z|c|v)=st->')
store_re = re.compile(r'^\s+st->(r\[\d+\]|running|ge|n|z|c|v)=(r(?:1[0-4]|[0-9])|running|ge|n|z|c|v);')
bad = 0
for path in shards:
    lines = open(path).read().split('\n')
    name = None
    body = []
    for line in lines:
        m = name_re.match(line)
        if m:
            name = m.group(1)
            body = []
            continue
        if name is None:
            continue
        if line.startswith('}'):
            # analyse
            loads = set()
            stores = set()
            code = []
            for bl in body:
                lm = load_re.match(bl)
                if lm:
                    loads.add(lm.group(1))
                    continue
                sm = store_re.match(bl)
                if sm:
                    stores.add(sm.group(2))
                    continue
                code.append(bl)
            text = '\n'.join(code)
            used = set()
            for m2 in local_re.finditer(text):
                used.add(m2.group(1))
            # `pc`, `exec`, `_fn_ex`, `_fn_budget` are ours; `n`/`c`/`v` may
            # appear inside identifiers, but \b keeps them honest.
            missing_load = sorted(used - loads)
            written = set()
            # a local written by the body: name followed by an assignment op
            for m2 in re.finditer(r'\b(r(?:1[0-4]|[0-9])|running|ge|n|z|c|v)\s*(?:[-+*/&|^]?=|\+\+|--)(?!=)', text):
                written.add(m2.group(1))
            missing_store = sorted(written - stores)
            if missing_load or missing_store:
                bad += 1
                print('%s loads=%d stores=%d used-not-loaded=%s written-not-stored=%s'
                      % (name, len(loads), len(stores), missing_load, missing_store))
            name = None
if not bad:
    print('all shards consistent')
print('checked %d shards, %d inconsistent' % (len(shards), bad))
