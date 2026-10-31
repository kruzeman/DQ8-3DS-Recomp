#!/usr/bin/env python3
"""t_432a85ab: turn ptrace leaf samples + /proc CPU counters into a frame-budget table."""
import sys, collections, os

lab = sys.argv[1]
base = os.path.join('/home/hermes/.hermes/kanban/workspaces/t_bc6b4eb7/evidence', lab)

# ---- symbol -> bucket -------------------------------------------------
FUTEX_RET = 'libc.so.6+0x181'   # instruction right after the futex syscall (verified by disassembly)

def bucket(mod, sym, comm):
    s = sym
    if s == '<libc.so.6+0x181>' or s == 'pthread_barrier_wait@GLIBC_2.2.5' \
       or s.startswith('pthread_cond_wait') or s.startswith('pthread_cond_timedwait') \
       or s.startswith('futex'):
        return 'sync/wait (futex)'
    if s.startswith('clock_nanosleep') or s.startswith('nanosleep') \
       or s.startswith('clock_nanosleep@') or 'nanosleep' in s:
        return 'sync/wait (sleep)'
    if s.startswith('pthread_cond_signal') or s.startswith('pthread_mutex') \
       or s.startswith('pthread_create') or s.startswith('pthread_join'):
        return 'sync/wait (other)'
    if mod == '?' or mod == '':
        return 'JIT shader code (anon)'
    if mod.startswith('libgallium') or mod.startswith('libLLVM') \
       or mod.startswith('libdrm') or mod.startswith('libelf'):
        return 'PICA rasterization (llvmpipe/Mesa)'
    if mod.startswith('libSDL2'):
        if 'Audio' in s or 'audio' in s:
            return 'audio (SDL)'
        return 'present/compositing (SDL)'
    if mod.startswith('libasound'):
        return 'audio (ALSA)'
    if mod.startswith('dq8-'):
        if s.startswith('dq8_part_') or s.startswith('guest_') or s.startswith('dq8_mem') \
           or s.startswith('recomp_') or s.startswith('svc_') or s.startswith('__rust'):
            return 'guest code interpretation'
        if s.startswith('pica_') or s.startswith('pvs_') or s.startswith('pgl_') \
           or s.startswith('pqt') or s.startswith('gsp_') or s.startswith('pp_'):
            return 'PICA emulation + GL submit (host)'
        if s.startswith('native_audio') or 'audio' in s.lower() or s.startswith('dsp') \
           or s.startswith('mixer'):
            return 'audio (host)'
        if 'present' in s or 'capture' in s or 'display' in s or 'transfer' in s:
            return 'present/compositing (host)'
        if s.startswith('native_fs') or 'file' in s.lower() or 'romfs' in s:
            return 'I/O (fs/romfs)'
        if s.startswith('getenv') or s.startswith('getpid') or s.startswith('fopen') \
           or s.startswith('fprintf') or s.startswith('malloc') or s.startswith('free') \
           or s.startswith('memcpy') or s.startswith('memcmp') or s.startswith('strlen') \
           or s.startswith('read') or s.startswith('write') or s.startswith('snprintf') \
           or s.startswith('strcmp') or s.startswith('qsort'):
            return 'libc helpers (memcpy/stdio/alloc)'
        if s.startswith('<'):
            return 'our code (unsymbolized)'
        return 'our engine/host code: ' + s.split('@')[0]
    if mod == 'libc.so.6':
        if 'memcpy' in s or 'memmove' in s or 'memset' in s or 'memcmp' in s:
            return 'libc helpers (memcpy/stdio/alloc)'
        if s.startswith('madvise') or s.startswith('mmap') or s.startswith('munmap') \
           or s.startswith('mprotect') or s.startswith('malloc') or s.startswith('free') \
           or s.startswith('calloc') or s.startswith('realloc'):
            return 'allocator/mmap'
        if s.startswith('strlen') or s.startswith('strcmp') or s.startswith('strncmp') \
           or s.startswith('strchr') or s.startswith('strstr') or s.startswith('strcpy'):
            return 'libc helpers (memcpy/stdio/alloc)'
        return 'libc other: ' + s.split('@')[0]
    return 'other module: ' + mod

def group(comm, tid, main_tid):
    if tid == main_tid:
        return 'MAIN'
    if comm.startswith('llvmpipe'):
        return 'llvmpipe'
    return 'other-host'

# ---- load ------------------------------------------------------------
samples = []
with open(os.path.join(base, 'samples.tsv')) as f:
    next(f)
    for line in f:
        p = line.rstrip('\n').split('\t')
        samples.append((float(p[0]), int(p[1]), p[2], p[4], p[5]))

proc = []
with open(os.path.join(base, 'proc.tsv')) as f:
    next(f)
    for line in f:
        p = line.rstrip('\n').split('\t')
        if len(p) < 7:
            continue
        proc.append((float(p[0]), int(p[1]), p[2], int(p[3]), int(p[4]), p[5], p[6]))

if not samples or not proc:
    sys.exit('no data')

# main thread = most CPU among dq8-texcache3 named threads
cpu_by_tid = collections.defaultdict(list)
for r in proc:
    cpu_by_tid[r[1]].append((r[3] + r[4], r[0]))
main_tid = max((v for k, v in cpu_by_tid.items()),
               key=lambda v: v[-1][0] - v[0][0])[0][1] if False else None
best = -1
for tid, v in cpu_by_tid.items():
    d = v[-1][0] - v[0][0]
    if d > best and proc and any(r[1] == tid and str(r[2]).startswith('dq8-') for r in proc[:200]):
        best = d; main_tid = tid

t0 = min(r[0] for r in proc); t1 = max(r[0] for r in proc)
wall = t1 - t0

# ---- CPU by group ----------------------------------------------------
first = {}; last = {}
for r in proc:
    first.setdefault(r[1], r)
    last[r[1]] = r
cpu_group = collections.defaultdict(int)
for tid, r in first.items():
    lr = last[tid]
    cpu_group[group(r[2], tid, main_tid)] += (lr[3] + lr[4]) - (r[3] + r[4])
total_cpu = sum(cpu_group.values())

# ---- samples by bucket x group --------------------------------------
cnt = collections.defaultdict(collections.Counter)
for ts, tid, comm, mod, sym in samples:
    g = group(comm, tid, main_tid)
    cnt[g][bucket(mod, sym, comm)] += 1
tot_samples = sum(sum(c.values()) for c in cnt.values())

print('=== run window: %.1f s wall, %d samples, main_tid=%d' % (wall, tot_samples, main_tid))
print('=== CPU seconds (from /proc utime+stime, USER_HZ=100) ===')
for g, c in sorted(cpu_group.items(), key=lambda kv: -kv[1]):
    print('  %-10s %7.1f s  %5.1f%% of total CPU  %5.1f%% of one core' %
          (g, c / 100.0, 100.0 * c / total_cpu, 100.0 * c / 100.0 / wall))
print('  %-10s %7.1f s' % ('TOTAL', total_cpu / 100.0))
print()
for g in ('MAIN', 'llvmpipe', 'other-host'):
    c = cnt[g]
    n = sum(c.values())
    if not n:
        continue
    print('=== leaf-sample shares for %s (%d samples) ===' % (g, n))
    for b, v in c.most_common():
        print('  %-45s %5d  %5.1f%%' % (b, v, 100.0 * v / n))
    print()
print('=== ALL threads combined (%d samples) ===' % tot_samples)
allc = collections.Counter()
for c in cnt.values():
    allc.update(c)
for b, v in allc.most_common():
    print('  %-45s %5d  %5.1f%%' % (b, v, 100.0 * v / tot_samples))
