#!/usr/bin/env python3
"""vspar_probe.py -- one measured run with per-thread CPU accounting.

Launches the runtime, lets it settle, then samples /proc/<pid>/task/*/stat
twice (utime+stime, 100 Hz clock) over a fixed wall window and reports
CPU-seconds per second of wall clock (i.e. cores used) per thread and for the
whole process.  Kills the process afterwards.

  tools/vspar_probe.py --tag city-base-8 --secs 20 --settle 4 --out DIR -- CMD...

Writes <out>/<tag>.err (the runtime's stderr), <out>/<tag>.cpu (the table) and
<out>/<tag>.out (stdout).  Exit status is the run's, unless killed.
"""
import argparse
import os
import signal
import subprocess
import sys
import time

CLK_TCK = os.sysconf("SC_CLK_TCK")


def snap(pid):
    """{tid: (comm, utime+stime ticks)} for every task of pid."""
    out = {}
    base = "/proc/%d/task" % pid
    try:
        tids = os.listdir(base)
    except OSError:
        return None
    for tid in tids:
        try:
            with open("%s/%s/stat" % (base, tid), "rb") as f:
                data = f.read()
        except OSError:
            continue
        # comm may contain spaces/parens: split after the last ')'
        r = data.rfind(b")")
        comm = data[data.find(b"(") + 1:r].decode("utf-8", "replace")
        f = data[r + 2:].split()
        # f[0] is state, so utime is field 14 -> index 11, stime 15 -> index 12
        utime, stime = int(f[11]), int(f[12])
        key = tid if isinstance(tid, str) else tid.decode("utf-8", "replace")
        out[key] = (comm, utime + stime)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", required=True)
    ap.add_argument("--secs", type=float, default=20.0)
    ap.add_argument("--settle", type=float, default=4.0)
    ap.add_argument("--out", default=".")
    ap.add_argument("cmd", nargs=argparse.REMAINDER)
    a = ap.parse_args()
    cmd = a.cmd[1:] if a.cmd and a.cmd[0] == "--" else a.cmd
    if not cmd:
        print("no command", file=sys.stderr)
        return 2
    os.makedirs(a.out, exist_ok=True)
    errp = os.path.join(a.out, a.tag + ".err")
    outp = os.path.join(a.out, a.tag + ".out")
    with open(outp, "wb") as so, open(errp, "wb") as se:
        p = subprocess.Popen(cmd, stdout=so, stderr=se, start_new_session=True)
        t_launch = time.monotonic()
        # wait for the runtime to have at least one extra thread (the pool) or
        # the settle time to elapse, whichever comes first
        while time.monotonic() - t_launch < a.settle:
            if p.poll() is not None:
                break
            s = snap(p.pid)
            if s and len(s) >= 2:
                break
            time.sleep(0.05)
        time.sleep(max(0.0, a.settle - (time.monotonic() - t_launch)))
        if p.poll() is not None:
            print("RUN DIED early rc=%s (see %s)" % (p.returncode, errp),
                  file=sys.stderr)
            return p.returncode or 1
        s0 = snap(p.pid)
        t0 = time.monotonic()
        time.sleep(a.secs)
        s1 = snap(p.pid)
        t1 = time.monotonic()
        # Hard guard: nothing below may leave the emulator running.
        try:
            os.killpg(p.pid, signal.SIGKILL)
        except OSError:
            pass
        try:
            rc = p.wait(timeout=15)
        except subprocess.TimeoutExpired:
            p.kill()
            rc = p.wait()
    if not s0 or not s1:
        print("no /proc samples", file=sys.stderr)
        return 1
    el = t1 - t0
    rows = []
    for tid, (comm, j1) in s1.items():
        if tid not in s0:
            continue
        j0 = s0[tid][1]
        rows.append((tid, comm, (j1 - j0) / CLK_TCK / el))
    rows.sort(key=lambda r: -r[2])
    total = sum(r[2] for r in rows)
    workers = sum(r[2] for r in rows if r[0] != str(p.pid))
    with open(os.path.join(a.out, a.tag + ".cpu"), "w") as f:
        f.write("# tag=%s wall=%.2fs threads=%d whole=%.3f cores "
                "main=%.3f workers=%.3f\n"
                % (a.tag, el, len(rows), total,
                   sum(r[2] for r in rows if r[0] == str(p.pid)), workers))
        f.write("# tid\tcomm\tcores\n")
        for tid, comm, c in rows:
            f.write("%s\t%s\t%.3f\n" % (tid, comm, c))
    print("%-28s wall=%5.2fs threads=%2d whole=%5.3f cores main=%5.3f "
          "workers=%5.3f (%.3f/thread)" %
          (a.tag, el, len(rows), total,
           sum(r[2] for r in rows if r[0] == str(p.pid)), workers,
           workers / max(1, len(rows) - 1)))
    print("  top:", ", ".join("%s=%.2f" % (r[0], r[2]) for r in rows[:4]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
