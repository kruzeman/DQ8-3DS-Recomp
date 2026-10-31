#!/usr/bin/env python3
"""Which module owns the frames BELOW the main thread's cond_wait?"""
import re, collections, sys
path = sys.argv[1] if len(sys.argv) > 1 else 'evidence/stacks1/gdb.log'
text = open(path, errors='replace').read()
parts = re.split(r'=== SAMPLE (\d+) ===', text)
samples = [(int(parts[i]), parts[i + 1]) for i in range(1, len(parts), 2)]
thr_re = re.compile(r'^Thread (\d+) \(.*?\(LWP (\d+)\)(?:\s+"([^"]*)")?\):', re.M)
main_wait_mod = collections.Counter()
for num, body in samples:
    th = list(thr_re.finditer(body))
    lwps = [int(m.group(2)) for m in th]
    if not lwps:
        continue
    ml = min(lwps)
    for j, m in enumerate(th):
        if int(m.group(2)) != ml:
            continue
        end = th[j + 1].start() if j + 1 < len(th) else len(body)
        seg = body[m.end():end]
        frames = [l for l in seg.splitlines() if l.strip().startswith('#')]
        if not frames:
            continue
        if 'futex' not in frames[0]:
            continue
        mods = []
        for l in frames[4:11]:
            if 'from /lib' in l:
                mods.append(l.split(' from ')[-1].split('/')[-1][:14])
            elif re.search(r'0x000055', l):
                mods.append('MAINBIN')
            else:
                mods.append('?')
        main_wait_mod[tuple(mods[:5])] += 1
print('main-thread WAIT stacks, frames 4..8 module pattern:')
for k, v in main_wait_mod.most_common(10):
    print('  %3d  %s' % (v, ' -> '.join(k)))
