#!/usr/bin/env python3
"""t_432a85ab: ptrace leaf-function sampler (perf blocked by perf_event_paranoid=4,
gdb attach blocked by yama ptrace_scope=1 — but the tracer here is the PARENT of
the target, which yama allows).

Spawns the target as its own child, seizes every thread, then repeatedly
PTRACE_INTERRUPT -> GETREGS(RIP) -> PTRACE_CONT at ~5 Hz and writes
  ts  tid  comm  rip  module  symbol
"""
import ctypes, ctypes.util, os, signal, subprocess, sys, time, collections

libc = ctypes.CDLL(ctypes.util.find_library('c'), use_errno=True)
PTRACE_SEIZE = 0x4206
PTRACE_INTERRUPT = 0x4207
PTRACE_CONT = 7
PTRACE_DETACH = 17
PTRACE_GETREGS = 12
__WALL = 0x40000000

class user_regs_struct(ctypes.Structure):
    _fields_ = [(n, ctypes.c_ulonglong) for n in
                ('r15','r14','r13','r12','rbp','rbx','r11','r10','r9','r8',
                 'rax','rcx','rdx','rsi','rdi','orig_rax','rip','cs','eflags',
                 'rsp','ss','fs_base','gs_base','ds','es','fs','gs')]

def ptrace(req, pid, addr=0, data=0):
    libc.ptrace.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p]
    libc.ptrace.restype = ctypes.c_long
    r = libc.ptrace(req, pid, ctypes.c_void_p(addr), ctypes.c_void_p(data))
    return r

def ptrace_getregs(pid):
    regs = user_regs_struct()
    r = libc.ptrace(PTRACE_GETREGS, pid, None, ctypes.byref(regs))
    if r != 0:
        return None
    return regs

# ---------- symbol tables ----------
def load_symbols(path):
    """return sorted [(addr, size, name)] from nm -S -n (fall back to nm -n)."""
    out = []
    for cmd in (['nm', '-S', '-n', path], ['nm', '-D', '-S', '-n', path]):
        try:
            res = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        except Exception:
            continue
        if res.returncode != 0:
            continue
        for line in res.stdout.splitlines():
            p = line.split()
            if len(p) >= 4 and all(c in '0123456789abcdefABCDEF' for c in p[0]) \
               and all(c in '0123456789abcdefABCDEF' for c in p[1]):
                out.append((int(p[0], 16), int(p[1], 16), p[3]))
            elif len(p) == 3 and p[1] in 'tT' and all(c in '0123456789abcdefABCDEF' for c in p[0]):
                out.append((int(p[0], 16), 0, p[2]))
        if out:
            break
    out.sort()
    return out

def load_maps(pid):
    maps = []
    with open('/proc/%d/maps' % pid) as f:
        for line in f:
            rng, rest = line.split(' ', 1)
            a, b = rng.split('-')
            parts = rest.split()
            off = int(parts[1], 16)
            path = parts[-1] if len(parts) >= 5 else ''
            if path.startswith('/'):
                maps.append((int(a, 16), int(b, 16), off, path))
    return maps

class Resolver:
    def __init__(self, pid):
        self.maps = load_maps(pid)
        self.cache = {}
        self.modcache = {}
    def module_of(self, rip):
        for a, b, off, path in self.maps:
            if a <= rip < b:
                return path, off + (rip - a)
        return None, None
    def symtab(self, path):
        if path not in self.cache:
            self.cache[path] = load_symbols(path)
        return self.cache[path]
    def resolve(self, rip):
        path, link = self.module_of(rip)
        if path is None:
            return '?', '?'
        tab = self.symtab(path)
        # binary search: greatest symbol <= link
        lo, hi = 0, len(tab)
        while lo < hi:
            mid = (lo + hi) // 2
            if tab[mid][0] <= link:
                lo = mid + 1
            else:
                hi = mid
        idx = lo - 1
        mod = os.path.basename(path)
        if idx < 0:
            return mod, '<%s+0x%x>' % (mod, link)
        addr, size, name = tab[idx]
        if size and link - addr >= size:
            return mod, '<%s+0x%x>' % (mod, link - addr)
        if not size and idx + 1 < len(tab) and link >= tab[idx + 1][0]:
            return mod, '<%s+0x%x>' % (mod, link - addr)
        return mod, name

def main():
    ws = '/home/hermes/.hermes/kanban/workspaces/t_bc6b4eb7'
    binpath = sys.argv[5] if len(sys.argv) > 5 else '/home/hermes/DQ8/worktrees/texcache2-t_66c89cf2/target/dq8-texcache3'
    code = '/home/hermes/DQ8/DQ8.code'
    romfs = '/home/hermes/DQ8/target/dq8.romfs'
    state = '/home/hermes/DQ8/saves/dq8-state.sav'
    honey = '/home/hermes/DQ8/saves/honeySave.bin'
    lab = sys.argv[1] if len(sys.argv) > 1 else 'ptrace1'
    dur = float(sys.argv[2]) if len(sys.argv) > 2 else 60.0
    warm = float(sys.argv[3]) if len(sys.argv) > 3 else 10.0
    hz = float(sys.argv[4]) if len(sys.argv) > 4 else 5.0

    out = os.path.join(ws, 'evidence', lab)
    os.makedirs(os.path.join(out, 'userdata', 'archive-00000004-00000000'), exist_ok=True)
    with open(os.path.join(out, 'userdata', 'archive-00000004-00000000', 'honeySave.bin'), 'wb') as f:
        f.write(open(honey, 'rb').read())
    with open(os.path.join(out, 'input.txt'), 'w') as f:
        f.write('0\n')
    env = dict(os.environ)
    env.update(DQ8_HEADLESS='1', DQ8_RUN_GUEST_THREADS='1', DQ8_FRAME_STATS='1',
               DQ8_TRACE_TIME='1', DQ8_GL='1',
               DQ8_USER_DATA_DIR=os.path.join(out, 'userdata'),
               DQ8_INPUT_FILE=os.path.join(out, 'input.txt'))
    env.pop('DQ8_PROGRESS_SECONDS', None)
    logf = open(os.path.join(out, 'run.log'), 'w')
    p = subprocess.Popen([binpath, code, romfs, '--load-state', state],
                         env=env, stdout=logf, stderr=subprocess.STDOUT,
                         cwd='/home/hermes/DQ8/worktrees/texcache2-t_66c89cf2')
    pid = p.pid
    with open(os.path.join(out, 'pid.txt'), 'w') as f:
        f.write(str(pid))
    print('target pid', pid, 'warmup', warm, flush=True)
    time.sleep(warm)
    if p.poll() is not None:
        sys.exit('target exited early: %s' % p.poll())

    resolver = Resolver(pid)
    tids = [int(t) for t in os.listdir('/proc/%d/task' % pid)]
    seized = {}
    for t in tids:
        if ptrace(PTRACE_SEIZE, t) == 0:
            seized[t] = True
        else:
            print('seize failed', t, os.strerror(ctypes.get_errno()))
    print('seized', len(seized), 'threads', flush=True)

    comm = {}
    def comm_of(t):
        if t not in comm:
            try:
                with open('/proc/%d/task/%d/comm' % (pid, t)) as f:
                    comm[t] = f.read().strip()
            except Exception:
                comm[t] = '?'
        return comm[t]

    rows = []
    proc_rows = []

    def read_proc(t):
        """utime, stime, wchan, state from /proc/<pid>/task/<t>/{stat,wchan}"""
        try:
            with open('/proc/%d/task/%d/stat' % (pid, t)) as f:
                s = f.read()
            s = s[s.rindex(')') + 2:].split()
            state = s[0]
            ut, st = int(s[11]), int(s[12])
            with open('/proc/%d/task/%d/wchan' % (pid, t)) as f:
                w = f.read().strip()
            return ut, st, w, state
        except Exception:
            return None

    n = int(dur * hz)
    t_end = time.time() + dur
    for i in range(n):
        ts = time.time()
        # refresh thread list (threads may appear)
        try:
            cur = set(int(t) for t in os.listdir('/proc/%d/task' % pid))
        except Exception:
            break
        for t in list(seized):
            if t not in cur:
                del seized[t]
        for t in cur - set(seized):
            if ptrace(PTRACE_SEIZE, t) == 0:
                seized[t] = True
        for t in list(seized):
            if ptrace(PTRACE_INTERRUPT, t) != 0:
                continue
            try:
                os.waitpid(t, __WALL)
            except ChildProcessError:
                continue
            regs = ptrace_getregs(t)
            if regs is not None:
                rows.append((ts, t, comm_of(t), regs.rip))
            ptrace(PTRACE_CONT, t)
        for t in list(seized):
            p4 = read_proc(t)
            if p4 is not None:
                proc_rows.append((ts, t, comm_of(t)) + p4)
        dt = time.time() - ts
        nxt = 1.0 / hz - dt
        if nxt > 0:
            time.sleep(min(nxt, max(0.0, t_end - time.time())))
        if time.time() >= t_end:
            break

    for t in list(seized):
        ptrace(PTRACE_DETACH, t)
    # resolve
    with open(os.path.join(out, 'samples.tsv'), 'w') as f:
        f.write('ts\ttid\tcomm\trip\tmodule\tsymbol\n')
        for ts, t, c, rip in rows:
            mod, sym = resolver.resolve(rip)
            f.write('%.6f\t%d\t%s\t0x%x\t%s\t%s\n' % (ts, t, c, rip, mod, sym))
    print('samples', len(rows), '->', os.path.join(out, 'samples.tsv'), flush=True)
    with open(os.path.join(out, 'proc.tsv'), 'w') as f:
        f.write('ts\ttid\tcomm\tutime\tstime\twchan\tstate\n')
        for r in proc_rows:
            f.write('%.6f\t%d\t%s\t%d\t%d\t%s\t%s\n' % r)
    print('proc rows', len(proc_rows), flush=True)
    p.send_signal(signal.SIGTERM)
    time.sleep(2)
    p.kill()
    logf.close()

if __name__ == '__main__':
    main()
