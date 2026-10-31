#!/usr/bin/env python3
"""t_432a85ab: parse gdb 'thread apply all bt' samples into per-thread categories."""
import sys, re, collections

path = sys.argv[1]
text = open(path, errors='replace').read()

# split on sample markers
parts = re.split(r'=== SAMPLE (\d+) ===', text)
# parts[0] = preamble, then pairs (num, body)
samples = []
for i in range(1, len(parts), 2):
    samples.append((int(parts[i]), parts[i + 1]))
print('parsed samples:', len(samples))

thread_re = re.compile(r'^Thread (\d+) \(.*?\(LWP (\d+)\)(?:\s+"([^"]*)")?\):', re.M)
frame_re = re.compile(r'^\s*#(\d+)\s+(?:0x[0-9a-f]+\s+)?(?:in\s+(.*?)\s*(?:\(|$))?(?:from\s+(\S+))?', re.M)

LIBC_WAIT = ('futex', 'pthread_cond_wait', 'pthread_cond_timedwait', 'pthread_barrier',
             'clock_nanosleep', 'nanosleep', 'select', 'poll', 'ppoll', 'epoll', 'read')

def classify_main(frames):
    """frames: list of (idx, text, lib_or_none). text is everything after 'in '."""
    if not frames:
        return 'unknown (no frames)', ''
    top = frames[0][1]
    top_lib = frames[0][2] or ''
    name = top.split('(')[0].strip()
    waiters = []
    if any(w in name for w in LIBC_WAIT):
        # find the first frame that is not a pure libc wait helper
        caller = ''
        for idx, t, lib in frames[1:]:
            n = t.split('(')[0].strip()
            if lib == 'libc.so.6' and any(w in n for w in LIBC_WAIT + ('pthread_cond', 'futex')):
                continue
            caller = ('%s [%s]' % (n, lib)) if lib else n
            break
        return 'WAIT: ' + name, caller
    if name.startswith(('pica_', 'pvs_', 'pgl_', 'pqt', 'gsp_', 'pp_')):
        return 'PICA/GL-submit: ' + name, ''
    if name.startswith('dq8_part_') or name.startswith('guest_') or name.startswith('dq8_mem') \
       or name.startswith('dq8_pc') or name.startswith('svc_'):
        return 'guest interpretation: ' + name, ''
    if name.startswith(('memcpy', 'memmove', 'memset', 'memcmp', 'bcmp')):
        return 'libc memcpy/memset', ''
    if 'libgallium' in top_lib:
        return 'Mesa/llvmpipe (on main): ' + name, ''
    if 'libEGL' in top_lib or 'libglapi' in top_lib:
        return 'GL loader (on main): ' + name, ''
    if 'libSDL2' in top_lib:
        return 'SDL (on main): ' + name, ''
    if name in ('??', ''):
        return 'anon/JIT (on main)', ''
    return 'other: ' + name + (' [%s]' % top_lib if top_lib else ''), ''

def classify_worker(frames, comm):
    if not frames:
        return 'unknown', ''
    name = frames[0][1].split('(')[0].strip()
    if any(w in name for w in LIBC_WAIT) or name.startswith('pthread_cond'):
        caller = ''
        for idx, t, lib in frames[1:]:
            n = t.split('(')[0].strip()
            if lib == 'libc.so.6' and (any(w in n for w in LIBC_WAIT) or n.startswith('pthread_')):
                continue
            caller = n + (' [%s]' % (lib.split('/')[-1][:16]) if lib else '')
            break
        return 'IDLE: ' + name, caller
    if name in ('??', ''):
        return 'JIT/anon EXEC', ''
    if frames[0][2] and 'libgallium' in frames[0][2]:
        return 'libgallium EXEC: ' + name, ''
    if frames[0][2] and 'libLLVM' in frames[0][2]:
        return 'libLLVM EXEC: ' + name, ''
    if frames[0][2] and 'libc.so.6' in frames[0][2]:
        return 'libc EXEC: ' + name, ''
    return 'EXEC other: ' + name, ''

main_counts = collections.Counter()
main_wait_caller = collections.Counter()
main_top_funcs = collections.Counter()
llv_counts = collections.Counter()
llv_idle_caller = collections.Counter()
other_counts = collections.Counter()
main_frames_seen = []

for num, body in samples:
    # cut off inferior output after the last thread block: split by thread headers
    th = list(thread_re.finditer(body))
    lwps = [int(m.group(2)) for m in th]
    main_lwp = min(lwps) if lwps else None
    for j, m in enumerate(th):
        tid = int(m.group(1)); comm = m.group(3) or ''
        end = th[j + 1].start() if j + 1 < len(th) else len(body)
        seg = body[m.end():end]
        frames = []
        for line in seg.splitlines():
            line = line.strip()
            if not line.startswith('#'):
                if frames:
                    break
                continue
            fm = re.match(r'#\d+\s+(?:0x[0-9a-f]+\s+)?(in\s+.*?)(?:\s+from\s+(\S+))?$', line)
            if fm:
                txt = fm.group(1)
                lib = fm.group(2)
                if lib is None:
                    # 'in X ()' may be followed by nothing; infer lib from the line
                    lib = None
                frames.append((len(frames), txt, lib))
                # detect 'from /lib/...' appended in the raw line
                if ' from ' in line and frames[-1][2] is None:
                    frames[-1] = (frames[-1][0], frames[-1][1], line.split(' from ', 1)[1].strip())
        if not frames:
            continue
        is_main = (int(m.group(2)) == main_lwp)
        if is_main:
            cat, caller = classify_main(frames)
            main_counts[cat] += 1
            if cat.startswith('WAIT'):
                main_wait_caller[caller] += 1
            main_top_funcs[frames[0][1].split('(')[0].strip()] += 1
            if num in (1, 20, 40, 60):
                main_frames_seen.append((num, [f[1] for f in frames[:8]]))
        elif comm.startswith('llvmpipe'):
            cat, caller = classify_worker(frames, comm)
            llv_counts[cat] += 1
            if cat.startswith('IDLE'):
                llv_idle_caller[caller] += 1
        else:
            n = frames[0][1].split('(')[0].strip()
            if any(w in n for w in LIBC_WAIT) or n.startswith('pthread_'):
                other_counts['IDLE: ' + n] += 1
            else:
                other_counts['EXEC: ' + n + (' [%s]' % frames[0][2] if frames[0][2] else '')] += 1

def show(title, ctr, total=None):
    total = total or sum(ctr.values())
    print('--- %s (n=%d)' % (title, total))
    for k, v in ctr.most_common(18):
        print('   %-60s %4d  %5.1f%%' % (k, v, 100.0 * v / total))
    print()

show('MAIN thread top-of-stack categories', main_counts)
show('MAIN thread: who is the wait CALLER', main_wait_caller)
show('MAIN thread: exact top functions', main_top_funcs)
show('llvmpipe worker categories', llv_counts)
show('llvmpipe IDLE: who they wait on', llv_idle_caller)
show('other host threads', other_counts)

print('--- main stacks at selected samples')
for num, fr in main_frames_seen:
    print('  sample %d:' % num)
    for f in fr:
        print('      %s' % f[:110])
