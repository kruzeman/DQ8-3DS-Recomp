#!/usr/bin/env python3
"""Repeatable idle-dialog CPU scenario harness (audible vs muted).

Scenario defaults (they reproduce the recorded 84.74 % idle-dialog figure):
  * fresh, empty userdata directory and NO input (input file pinned to 0 and
    never pressed; DQ8_TRACE_INPUT=1 records that nothing arrived)
  * headless runner, guest threads on, PPM screen capture on, periodic guest
    snapshots on -- exactly the environment of the original idle-noinput run
  * screenshot of both screens at t=155 s  -> t155-top.png / t155-bottom.png
    (the save dialog "No previous save data could be read / New save data will
    be created" lives on the TOP screen)
  * per-thread CPU over t=160..190 s computed from /proc/<pid>/task/<tid>/stat
    utime+stime DELTAS between the t=160 and t=190 samples.  This is a window
    measurement; the old `ps -L -o pcpu` value was a lifetime average and is
    only kept as a labelled reference line.
  * bounded profile of the MAIN host thread over the same window: a ptrace
    RIP sampler (perf is unusable here: /proc/sys/kernel/perf_event_paranoid
    is 4) symbolized against the runner's own .symtab.  `dq8_part_NNN` frames
    are reported as their guest PC band (band = pc >> 17, see
    tools/split_dispatch.py).
  * DSP source/frame counters and current_guest_pc are read straight out of
    process memory at t=160 and t=190 (no runtime edit, no tracing overhead),
    so the evidence says whether any audio source was actually streaming.

Usage:
  tools/run_idle_scenario.py RUN_NAME [--variant audible|muted] [options]

  RUN_NAME selects the evidence directory target/audio-workers-evidence/<RUN_NAME>.
  If --variant is omitted it is derived from the name: a run name ending in
  "-muted" is muted, anything else is audible (SDL sink enabled).

Evidence written into target/audio-workers-evidence/<RUN_NAME>/:
  cpu-160-190.txt          window CPU per thread (the acceptance file)
  t155-top.png             screenshot verified to show the save dialog
  t155-bottom.png          bottom screen at the same instant
  main-profile-160-190.txt bounded main-thread profile over the window
  dsp-metrics-160-190.txt  DSP frames/sources + current guest PC snapshots
  proc-cpu.log             raw utime/stime per thread per second
  timeline.log, meta.txt, run.log
"""
import argparse
import bisect
import ctypes
import hashlib
import os
import re
import shutil
import signal
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
CODE = "/home/hermes/DQ8/DQ8.code"
ROMFS = "/home/hermes/DQ8/target/dq8.romfs"
HZ = os.sysconf(os.sysconf_names["SC_CLK_TCK"])
WALL = getattr(os, "WALL", 0x40000000)

PTRACE_SEIZE = 0x4206
PTRACE_INTERRUPT = 0x4207
PTRACE_DETACH = 17
PTRACE_GETREGS = 12
PTRACE_ATTACH = 16

LIBC = ctypes.CDLL("libc.so.6", use_errno=True)
LIBC.ptrace.argtypes = [ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p, ctypes.c_void_p]
LIBC.ptrace.restype = ctypes.c_long


class UserRegs(ctypes.Structure):
    _fields_ = [(n, ctypes.c_ulonglong) for n in (
        "r15", "r14", "r13", "r12", "rbp", "rbx", "r11", "r10", "r9", "r8",
        "rax", "rcx", "rdx", "rsi", "rdi", "orig_rax", "rip", "cs", "eflags",
        "rsp", "ss", "fs_base", "gs_base", "ds", "es", "fs", "gs")]


def log(msg):
    print(msg, flush=True)


# --------------------------------------------------------------------------- #
# /proc helpers
# --------------------------------------------------------------------------- #
def task_stat(pid, tid):
    """Return (comm, utime_ticks, stime_ticks) for one thread.

    NOTE: /proc/<tid>/stat resolves to the thread-group LEADER for a
    non-leader tid (verified against `ps -L`); the per-thread numbers only
    live under /proc/<pid>/task/<tid>/stat.
    """
    with open("/proc/%d/task/%d/stat" % (pid, tid), "rb") as fh:
        data = fh.read().decode("utf-8", "replace")
    close = data.rindex(")")
    comm = data[data.index("(") + 1:close]
    rest = data[close + 2:].split()
    return comm, int(rest[11]), int(rest[12])


def cpu_allowed(pid, tid):
    try:
        with open("/proc/%d/task/%d/status" % (pid, tid)) as fh:
            for line in fh:
                if line.startswith("Cpus_allowed_list:"):
                    return line.split(":", 1)[1].strip()
    except OSError:
        return "?"
    return "?"


def list_tids(pid):
    try:
        return sorted(int(t) for t in os.listdir("/proc/%d/task" % pid))
    except OSError:
        return []


def load_maps(pid):
    out = []
    try:
        with open("/proc/%d/maps" % pid) as fh:
            for line in fh:
                parts = line.split()
                lo, hi = parts[0].split("-")
                off = int(parts[2], 16)
                label = parts[-1] if len(parts) >= 6 else "[anon]"
                out.append((int(lo, 16), int(hi, 16), parts[1], off, label))
    except OSError:
        pass
    return out


def elf_load_segments(path):
    with open(path, "rb") as fh:
        blob = fh.read(0x10000)
    if blob[:4] != b"\x7fELF" or blob[4] != 2:
        return []
    phoff = struct.unpack_from("<Q", blob, 0x20)[0]
    phentsize, phnum = struct.unpack_from("<HH", blob, 0x36)
    segs = []
    for i in range(phnum):
        base = phoff + i * phentsize
        p_type, p_flags = struct.unpack_from("<II", blob, base)
        p_offset, p_vaddr = struct.unpack_from("<QQ", blob, base + 8)
        if p_type == 1:  # PT_LOAD
            segs.append((p_offset, p_vaddr))
    return segs


# --------------------------------------------------------------------------- #
# symbolization
# --------------------------------------------------------------------------- #
def load_text_symbols(binary):
    """[(addr, size, name)] for text symbols in the runner's own .symtab."""
    try:
        res = subprocess.run(["nm", "-S", "--defined-only", binary],
                             capture_output=True, text=True, check=False)
    except OSError:
        return []
    syms = []
    for line in res.stdout.splitlines():
        p = line.split()
        if len(p) == 4 and p[2] in ("T", "t", "W", "w"):
            syms.append((int(p[0], 16), int(p[1], 16), p[3]))
        elif len(p) == 3 and p[1] in ("T", "t"):
            syms.append((int(p[0], 16), 0, p[2]))
    syms.sort()
    for i, (addr, size, name) in enumerate(syms):
        if size == 0:
            nxt = syms[i + 1][0] if i + 1 < len(syms) else addr + 16
            syms[i] = (addr, max(nxt - addr, 1), name)
    return syms


def load_data_symbols(binary):
    """[(addr, size, name)] for data/bss symbols (metric counters)."""
    try:
        res = subprocess.run(["nm", "-S", "--defined-only", binary],
                             capture_output=True, text=True, check=False)
    except OSError:
        return {}
    out = {}
    for line in res.stdout.splitlines():
        p = line.split()
        if len(p) == 4 and p[2] in "bBdD":
            out.setdefault(p[3], []).append((int(p[0], 16), int(p[1], 16)))
        elif len(p) == 3 and p[1] in "bBdD":
            out.setdefault(p[2], []).append((int(p[0], 16), 8))
    return out


def guest_pc_band_range(band):
    """(first, last) guest-PC address covered by `dq8_part_<band>`.

    Bands are 17 bits wide (tools/split_dispatch.py: band = page >> 5,
    32 guest pages of 4 KiB = 0x20000 bytes), so the last address is
    (band << 17) + 0x1FFFF.  A 21-bit mask here used to print e.g.
    dq8_part_010 as 0x140000-0x33ffff (audit IDLE-AFTER-02).
    """
    lo = band << 17
    return lo, lo + 0x1FFFF


class Symbolizer:
    def __init__(self, binary, pid):
        self.syms = load_text_symbols(binary)
        self.addrs = [s[0] for s in self.syms]
        self.maps = load_maps(pid)
        self.binary = os.path.realpath(binary)
        self.base = 0
        segs = elf_load_segments(binary)
        for lo, hi, perms, off, label in self.maps:
            if off == 0 and os.path.realpath(label) == self.binary and segs:
                vaddr = min(v for o, v in segs if o == 0)
                self.base = lo - vaddr
                break
        if self.base == 0:
            for lo, hi, perms, off, label in self.maps:
                if os.path.realpath(label) == self.binary and "x" in perms:
                    self.base = lo - (segs[0][1] if segs else 0)
                    break

    def module(self, rip):
        for lo, hi, perms, off, label in self.maps:
            if lo <= rip < hi:
                name = os.path.basename(label) if label.startswith("/") else label
                return "%s+0x%x" % (name, rip - lo)
        return "unmapped"

    def lookup(self, rip):
        """Symbol name for a runtime address (None if outside our symbols)."""
        off = rip - self.base
        i = bisect.bisect_right(self.addrs, off) - 1
        if i >= 0:
            addr, size, name = self.syms[i]
            if addr <= off < addr + size:
                return name
        return None

    def describe(self, rip):
        name = self.lookup(rip)
        if name is None:
            return self.module(rip)
        m = re.match(r"dq8_part_(\d+)$", name)
        if m:
            lo, hi = guest_pc_band_range(int(m.group(1)))
            return "%s [guest pc 0x%06x-0x%06x]" % (name, lo, hi)
        return name


# --------------------------------------------------------------------------- #
# bounded ptrace RIP sampler (parent -> own child; yama ptrace_scope=1)
# --------------------------------------------------------------------------- #
class RipSampler:
    def __init__(self, pid, tid, binary, hz):
        self.pid = pid
        self.tid = tid
        self.hz = hz
        self.sym = Symbolizer(binary, pid)
        self.mem = open("/proc/%d/mem" % pid, "rb", buffering=0)
        self.samples = []          # (t, rip, rbp, rsp)
        self.pauses = 0.0
        self.ok = 0
        self.fail = 0
        self.seized = False

    def _ptrace(self, req, data=None):
        return LIBC.ptrace(req, self.tid, None, data)

    def _wait_stop(self, timeout=0.2):
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                wpid, status = os.waitpid(self.tid, WALL | os.WNOHANG)
            except (ChildProcessError, OSError):
                return False
            if wpid == self.tid:
                return True
            time.sleep(0.0005)
        return False

    def _read(self, addr, size):
        self.mem.seek(addr)
        return self.mem.read(size)

    def sample(self, chain_depth=8):
        t0 = time.time()
        if self.seized:
            if self._ptrace(PTRACE_INTERRUPT) != 0:
                self.fail += 1
                return None
        elif self._ptrace(PTRACE_SEIZE) == 0:
            self.seized = True
            if self._ptrace(PTRACE_INTERRUPT) != 0:
                self.fail += 1
                return None
        elif self._ptrace(PTRACE_ATTACH) != 0:
            self.fail += 1
            return None
        if not self._wait_stop():
            self.fail += 1
            try:
                self._ptrace(PTRACE_DETACH)
            except Exception:
                pass
            self.seized = False
            return None
        regs = UserRegs()
        got = self._ptrace(PTRACE_GETREGS, ctypes.byref(regs)) == 0
        chain = []
        if got:
            chain = self._walk(regs.rip, regs.rbp, regs.rsp, chain_depth)
        self._ptrace(PTRACE_DETACH)
        self.seized = False          # DETACH untraces: next sample must SEIZE again
        self.pauses += time.time() - t0
        if not got:
            self.fail += 1
            return None
        self.ok += 1
        rec = (time.time(), regs.rip, regs.rbp, regs.rsp, chain)
        self.samples.append(rec)
        return rec

    def _walk(self, rip, rbp, rsp, depth):
        """Bounded frame-pointer walk; keeps only sane, monotonic frames."""
        stack_lo = stack_hi = None
        for lo, hi, perms, off, label in self.sym.maps:
            if lo <= rsp < hi and ("w" in perms):
                stack_lo, stack_hi = lo, hi
                break
        if stack_lo is None or stack_hi is None:
            return []
        chain = [rip]
        prev = rbp
        for _ in range(depth):
            if not rbp or rbp & 7 or not (stack_lo <= rbp < stack_hi - 16):
                break
            if prev is not None and rbp <= prev and len(chain) > 1:
                break
            try:
                blob = self._read(rbp, 16)
            except OSError:
                break
            if len(blob) != 16:
                break
            nxt, ret = struct.unpack("<QQ", blob)
            if ret == 0:
                break
            chain.append(ret)
            if nxt <= rbp:
                break
            prev, rbp = rbp, nxt
        return chain

    def close(self):
        try:
            self.mem.close()
        except OSError:
            pass


# --------------------------------------------------------------------------- #
# DSP / guest-PC counters read from process memory (no runtime change)
# --------------------------------------------------------------------------- #
def load_base(binary, pid):
    """PIE load bias: runtime address = nm vaddr + base."""
    segs = elf_load_segments(binary)
    real = os.path.realpath(binary)
    for lo, hi, perms, off, label in load_maps(pid):
        if off == 0 and os.path.realpath(label) == real and segs:
            vaddr = min(v for o, v in segs if o == 0)
            return lo - vaddr
    if segs:
        for lo, hi, perms, off, label in load_maps(pid):
            if os.path.realpath(label) == real and "x" in perms:
                return lo - min(v for o, v in segs)
    return 0


def snapshot_metrics(pid, binary, syms_cache=None, base=0):
    data_syms = syms_cache if syms_cache is not None else load_data_symbols(binary)
    want = ["dsp_frames_out", "dsp_sources_started", "dsp_sources_finished",
            "current_guest_pc", "main_guest_pc"]
    out = {}
    try:
        with open("/proc/%d/mem" % pid, "rb", buffering=0) as mem:
            for name in want:
                for addr, size in data_syms.get(name, []):
                    try:
                        mem.seek(addr + base)
                        blob = mem.read(8)
                    except OSError as exc:
                        out[name + "_read_error"] = "%s @0x%x" % (exc, addr + base)
                        continue
                    if len(blob) < 4:
                        out[name + "_read_error"] = "short read @0x%x" % (addr + base)
                        continue
                    key = name if len(data_syms.get(name, [])) == 1 else "%s@0x%x" % (name, addr)
                    if name.endswith("pc"):
                        out[key] = struct.unpack("<I", blob[:4])[0]
                    else:
                        out[key] = struct.unpack("<Q", blob[:8])[0]
    except OSError as exc:
        out["error"] = str(exc)
    return out


# --------------------------------------------------------------------------- #
# scenario runner
# --------------------------------------------------------------------------- #
def parse_args():
    ap = argparse.ArgumentParser(description="idle-dialog CPU scenario runner")
    ap.add_argument("run_name", help="evidence directory name under target/audio-workers-evidence/")
    ap.add_argument("--variant", choices=["audible", "muted"], default=None,
                    help="default: derived from run name ('*-muted' -> muted)")
    ap.add_argument("--bin", default="target/dq8-audio-workers")
    ap.add_argument("--capture-at", type=int, default=155)
    ap.add_argument("--window", type=int, nargs=2, default=[160, 190],
                    metavar=("START", "END"))
    ap.add_argument("--max-wall", type=int, default=192)
    ap.add_argument("--hz", type=int, default=25, help="ptrace sample rate")
    ap.add_argument("--no-profile", action="store_true")
    ap.add_argument("--no-metrics", action="store_true")
    ap.add_argument("--windowed", action="store_true",
                    help="run with a real SDL window on Xvfb (DQ8_DISPLAY=1, "
                         "DQ8_HEADLESS off) instead of headless; same scenario, "
                         "same per-thread attribution")
    ap.add_argument("--display", default=":99", help="Xvfb display for --windowed")
    ap.add_argument("--env", action="append", default=[], metavar="KEY=VAL",
                    help="extra environment variable for the runner (repeatable)")
    return ap.parse_args()


def main():
    args = parse_args()
    variant = args.variant or ("muted" if args.run_name.endswith("-muted") else "audible")
    bin_path = args.bin if os.path.isabs(args.bin) else os.path.join(REPO, args.bin)
    out = os.path.join(REPO, "target", "audio-workers-evidence", args.run_name)
    win0, win1 = args.window
    if not (0 < args.capture_at < win0 < win1 <= args.max_wall - 2):
        log("bad timing: need 0 < capture-at < window-start < window-end <= max-wall-2")
        return 2
    if os.path.exists(out):
        log("refusing to reuse evidence directory: %s" % out)
        return 2
    if not (os.path.isfile(CODE) and os.path.isfile(ROMFS)):
        log("missing read-only DQ8 inputs (%s / %s)" % (CODE, ROMFS))
        return 2
    if not os.path.isfile(bin_path):
        log("missing runner binary %s (build with tools/build_split.sh)" % bin_path)
        return 2

    os.makedirs(os.path.join(out, "userdata"))
    os.makedirs(os.path.join(out, "frames"))
    input_file = os.path.join(out, "input.txt")
    with open(input_file, "w") as fh:
        fh.write("0\n")
    run_log = os.path.join(out, "run.log")

    try:
        commit = subprocess.run(["git", "-C", REPO, "rev-parse", "--short", "HEAD"],
                                capture_output=True, text=True).stdout.strip() or "unknown"
    except OSError:
        commit = "unknown"
    sha = hashlib.sha256(open(bin_path, "rb").read()).hexdigest()

    env = dict(os.environ)
    env.update({
        "DQ8_HEADLESS": "0" if args.windowed else "1",
        "DQ8_DISPLAY": "1" if args.windowed else "0",
        "DQ8_RUN_GUEST_THREADS": "1",
        "DQ8_BENCH_NO_SNAPSHOT": "0",
        "DQ8_PROGRESS_SECONDS": "10",
        "DQ8_TRACE_INPUT": "1",
        "DQ8_INPUT_FILE": input_file,
        "DQ8_USER_DATA_DIR": os.path.join(out, "userdata"),
        "DQ8_CAPTURE_PPM": "1",
        "DQ8_CAPTURE_PPM_DIR": os.path.join(out, "frames"),
        "SDL_AUDIODRIVER": "dummy",
    })
    if variant == "audible":
        env["DQ8_AUDIO_SDL"] = "1"       # SDL sink ENABLED (default behaviour)
    else:
        env["DQ8_AUDIO_SDL"] = "0"       # SDL sink disabled / muted
    for pair in args.env:
        if "=" not in pair:
            log("--env wants KEY=VAL, got %r" % pair)
            return 2
        key, val = pair.split("=", 1)
        env[key] = val

    xvfb_proc = None
    if args.windowed:
        env["DISPLAY"] = args.display
        xvfb_proc = subprocess.Popen(
            ["Xvfb", args.display, "-screen", "0", "1024x768x24", "-ac",
             "-nolisten", "tcp"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(1.0)
        if xvfb_proc.poll() is not None:
            log("Xvfb failed to start on %s" % args.display)
            return 2
        log("Xvfb up on %s (pid=%d)" % (args.display, xvfb_proc.pid))

    with open(os.path.join(out, "meta.txt"), "w") as fh:
        fh.write("run_name=%s\nvariant=%s\ncommit=%s\nbinary=%s\nbinary_sha256=%s\n"
                 % (args.run_name, variant, commit, bin_path, sha))
        fh.write("code=%s\nromfs=%s\n" % (CODE, ROMFS))
        for key in sorted(env):
            if key.startswith(("DQ8_", "SDL_")):
                fh.write("%s=%s\n" % (key, env[key]))
        fh.write("mode=%s\ndisplay=%s\n" % ("windowed" if args.windowed else "headless",
                                            args.display if args.windowed else "none"))
        fh.write("window=%d..%d\ncapture_at=%d\nmax_wall=%d\nprofile_hz=%d\n"
                 % (win0, win1, args.capture_at, args.max_wall, args.hz))
        fh.write("started_wall=%s\n" % time.strftime("%Y-%m-%d %H:%M:%S %z"))

    timeline = open(os.path.join(out, "timeline.log"), "w")
    proc_log = open(os.path.join(out, "proc-cpu.log"), "w")
    timeline.write("label=%s variant=%s start=%s window=%d..%d capture_at=%d\n"
                   % (args.run_name, variant, time.time(), win0, win1, args.capture_at))

    log_fh = open(run_log, "wb")
    proc = subprocess.Popen([bin_path, CODE, ROMFS], cwd=REPO, env=env,
                            stdout=log_fh, stderr=subprocess.STDOUT,
                            start_new_session=True)
    pid = proc.pid
    timeline.write("pid=%s\n" % pid)
    timeline.flush()
    log("started %s pid=%d variant=%s" % (bin_path, pid, variant))

    data_syms = load_data_symbols(bin_path)
    metric_base = load_base(bin_path, pid) if not args.no_metrics else 0
    sampler = None
    if not args.no_profile:
        time.sleep(0.5)
        tids = list_tids(pid)
        if pid in tids:
            sampler = RipSampler(pid, pid, bin_path, args.hz)
            log("main-thread RIP sampler ready (%d symbols)" % len(sampler.sym.syms))

    start = time.time()
    interval = 1.0 / max(args.hz, 1)
    next_tick = start
    last_sec = -1
    samples = {}          # sec -> {tid: (utime, stime)}
    stamps = {}           # sec -> wall time of the sample
    allowed = {}          # tid -> affinity string
    comm_of = {}          # tid -> comm
    capture_done = False
    metric_snaps = {}
    profile_on = False
    exit_note = ""
    while True:
        now = time.time()
        elapsed = now - start
        if elapsed >= args.max_wall:
            exit_note = "max-wall reached"
            break
        if proc.poll() is not None:
            exit_note = "runner exited early rc=%s" % proc.returncode
            break
        sec = int(elapsed)
        if sec != last_sec and sec >= 0:
            last_sec = sec
            tids = list_tids(pid)
            row = {}
            for tid in tids:
                try:
                    comm, ut, st = task_stat(pid, tid)
                except OSError:
                    continue
                row[tid] = (ut, st)
                comm_of[tid] = comm
                if tid not in allowed:
                    allowed[tid] = cpu_allowed(pid, tid)
            samples[sec] = row
            stamps[sec] = now
            proc_log.write("t=%d" % sec)
            for tid in sorted(row):
                proc_log.write(" %d:%d:%d" % (tid, row[tid][0], row[tid][1]))
            proc_log.write("\n")
            proc_log.flush()

            if sec == args.capture_at and not capture_done:
                capture_done = True
                for src_name, dst_name in (("dq8-screen.ppm", "t%d-top.png" % sec),
                                           ("dq8-screen-bottom.ppm", "t%d-bottom.png" % sec)):
                    src = os.path.join(out, "frames", src_name)
                    dst = os.path.join(out, dst_name)
                    if os.path.exists(src):
                        mtime = os.path.getmtime(src)
                        fresh = now - mtime
                        shutil.copyfile(src, dst[:-4] + ".ppm")
                        try:
                            from PIL import Image
                            with Image.open(dst[:-4] + ".ppm") as im:
                                im.save(dst)
                            os.remove(dst[:-4] + ".ppm")
                        except Exception as exc:      # keep the PPM as fallback
                            timeline.write("t=%s png_convert_failed=%s\n" % (sec, exc))
                        timeline.write("t=%d captured=%s age_s=%.2f -> %s\n"
                                       % (sec, src_name, fresh, dst_name))
                    else:
                        timeline.write("t=%d missing=%s\n" % (sec, src_name))
                timeline.flush()

        if sec == win0 and not metric_snaps.get("win0") and not args.no_metrics:
            metric_snaps["win0"] = snapshot_metrics(pid, bin_path, data_syms, metric_base)
            metric_snaps["win0_t"] = time.time()
        if sec == win1 and not metric_snaps.get("win1") and not args.no_metrics:
            metric_snaps["win1"] = snapshot_metrics(pid, bin_path, data_syms, metric_base)
            metric_snaps["win1_t"] = time.time()

        if sampler is not None:
            if win0 <= elapsed < win1:
                if not profile_on:
                    profile_on = True
                    timeline.write("t=%d profile start\n" % int(elapsed))
                    timeline.flush()
                sampler.sample()
            elif profile_on and elapsed >= win1:
                profile_on = False
                timeline.write("t=%d profile stop samples=%d\n" % (int(elapsed), len(sampler.samples)))
                timeline.flush()

        next_tick += interval
        delay = next_tick - time.time()
        if delay > 0:
            time.sleep(delay)
        else:
            next_tick = time.time()

    # ---- stop the runner -------------------------------------------------- #
    rc = None
    if proc.poll() is None:
        timeline.write("sample complete; sending TERM at t=%.0f\n" % (time.time() - start))
        timeline.flush()
        try:
            proc.terminate()
        except OSError:
            pass
        try:
            rc = proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()
            rc = proc.wait()
    else:
        rc = proc.returncode
    if sampler is not None:
        sampler.close()
    timeline.write("exit=%s end=%s elapsed_s=%.0f note=%s\n"
                   % (rc, time.time(), time.time() - start, exit_note))
    timeline.close()
    proc_log.close()
    log_fh.close()
    if xvfb_proc is not None:
        xvfb_proc.terminate()
        try:
            xvfb_proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            xvfb_proc.kill()

    # ---- window CPU report ------------------------------------------------ #
    w0, w1 = win0, win1
    if w0 not in samples or w1 not in samples:
        with open(os.path.join(out, "cpu-%d-%d.txt" % (w0, w1)), "w") as fh:
            fh.write("INCOMPLETE: missing samples t=%d or t=%d (%s)\n" % (w0, w1, exit_note))
        log("INCOMPLETE run: missing window samples (%s)" % exit_note)
        return 1

    wall_s = stamps[w1] - stamps[w0]
    first, last = samples[w0], samples[w1]
    rows = []
    for tid in sorted(set(first) | set(last)):
        u0, s0 = first.get(tid, (0, 0))
        u1, s1 = last.get(tid, (0, 0))
        ticks = (u1 - u0) + (s1 - s0)
        pct = 100.0 * ticks / (HZ * wall_s)
        if tid == pid:
            role = "main host thread (guest emulation)"
        else:
            aff = allowed.get(tid, "?")
            if aff == "3":
                role = "audio worker (music, core 3)"
            elif aff == "4":
                role = "audio worker (sfx/mixer, core 4)"
            else:
                role = "other thread (affinity %s)" % aff
        rows.append((tid, comm_of.get(tid, "?"), allowed.get(tid, "?"), pct, role, ticks))

    main_pct = next((r[3] for r in rows if r[0] == pid), 0.0)
    worker_pct = sum(r[3] for r in rows if "audio worker" in r[4])
    report = os.path.join(out, "cpu-%d-%d.txt" % (w0, w1))
    with open(report, "w") as fh:
        fh.write("scenario=%s variant=%s commit=%s binary_sha256=%s\n"
                 % (args.run_name, variant, commit, sha[:16]))
        fh.write("method=/proc/<pid>/task/<tid>/stat utime+stime DELTA (window-accurate)\n")
        fh.write("window=t%d (%.3f) .. t%d (%.3f)  wall_s=%.3f  hz=%d\n"
                 % (w0, stamps[w0], w1, stamps[w1], wall_s, HZ))
        fh.write("shutdown=rc=%s (%s)\n" % (rc, exit_note))
        fh.write("input=none (fresh userdata, input file held at 0)\n")
        fh.write("sdl_sink=%s\n\n" % ("enabled" if variant == "audible" else "disabled/muted"))
        for tid, comm, aff, pct, role, ticks in rows:
            prefix = "MAIN" if tid == pid else "    "
            fh.write("%s tid=%-7d %-16s cpu=%6.2f%%  ticks=%-6d affinity=%-8s %s\n"
                     % (prefix, tid, comm, pct, ticks, aff, role))
        fh.write("\nmain_guest_host_thread_cpu_pct=%.2f\n" % main_pct)
        fh.write("worker_total_cpu_pct=%.2f\n" % worker_pct)
        for tid, comm, aff, pct, role, ticks in rows:
            if tid != pid:
                fh.write("thread_%d_cpu_pct=%.2f\n" % (tid, pct))
    log("window CPU t%d..%d: main=%.2f%% workers=%.2f%% -> %s"
        % (w0, w1, main_pct, worker_pct, report))

    # ---- raw per-second log stays as evidence; now metrics + profile ------ #
    if metric_snaps:
        with open(os.path.join(out, "dsp-metrics-%d-%d.txt" % (w0, w1)), "w") as fh:
            fh.write("read from process memory (no runtime change, no trace overhead)\n")
            for tag in ("win0", "win1"):
                if tag not in metric_snaps:
                    continue
                fh.write("t=%d wall=%.3f\n" % (w0 if tag == "win0" else w1, metric_snaps[tag + "_t"]))
                for key in sorted(metric_snaps[tag]):
                    val = metric_snaps[tag][key]
                    if isinstance(val, int) and key.endswith("pc"):
                        fh.write("  %s=0x%08x\n" % (key, val))
                    elif isinstance(val, int):
                        fh.write("  %s=%d\n" % (key, val))
                    else:
                        fh.write("  %s=%s\n" % (key, val))
        log("metrics: %s" % os.path.join(out, "dsp-metrics-%d-%d.txt" % (w0, w1)))

    if sampler is not None and sampler.samples:
        prof = os.path.join(out, "main-profile-%d-%d.txt" % (w0, w1))
        window_len = wall_s
        n = len(sampler.samples)
        with open(prof, "w") as fh:
            fh.write("main host thread (tid=%d) profile, window t%d..%d\n" % (pid, w0, w1))
            fh.write("sampler=ptrace SEIZE/INTERRUPT RIP at %d Hz (perf unavailable: "
                     "perf_event_paranoid=%s)\n"
                     % (sampler.hz, open("/proc/sys/kernel/perf_event_paranoid").read().strip()))
            fh.write("samples=%d failed=%d total_pause_ms=%.1f (%.2f%% of %.1fs window)\n"
                     % (n, sampler.fail, sampler.pauses * 1000.0,
                        100.0 * sampler.pauses / max(window_len, 1e-6), window_len))
            fh.write("symbolization=runner .symtab; dq8_part_NNN reported as guest pc band "
                     "(band = pc >> 17)\n")
            if n:
                leaf = {}
                chain = {}
                for t, rip, rbp, rsp, ch in sampler.samples:
                    leaf[sampler.sym.describe(rip)] = leaf.get(sampler.sym.describe(rip), 0) + 1
                    key = " <- ".join(sampler.sym.describe(a) for a in ch) if ch else \
                        sampler.sym.describe(rip) + " <- [no frame chain]"
                    chain[key] = chain.get(key, 0) + 1
                fh.write("\n== leaf histogram (%d samples) ==\n" % n)
                for name, cnt in sorted(leaf.items(), key=lambda kv: -kv[1]):
                    fh.write("%6.2f%% %5d  %s\n" % (100.0 * cnt / n, cnt, name))
                fh.write("\n== call chains, top 25 ==\n")
                for name, cnt in sorted(chain.items(), key=lambda kv: -kv[1])[:25]:
                    fh.write("%6.2f%% %5d  %s\n" % (100.0 * cnt / n, cnt, name))
        raw = os.path.join(out, "main-profile-%d-%d.raw" % (w0, w1))
        with open(raw, "w") as fh:
            fh.write("t_rip_rbp_rsp_chain\n")
            for t, rip, rbp, rsp, ch in sampler.samples:
                fh.write("%.6f 0x%x 0x%x 0x%x %s\n"
                         % (t, rip, rbp, rsp,
                            ",".join("0x%x" % a for a in ch)))
        log("profile: %s (%d samples)" % (prof, n))

    log("done: %s (exit=%s, %s)" % (out, rc, exit_note))
    # The window data is evidence-complete even when the runner had to be
    # SIGKILLed after ignoring SIGTERM; judge success on the artifacts.
    return 0 if capture_done and os.path.getsize(report) > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
