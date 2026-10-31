#!/usr/bin/env python3
"""PERF-01/PERF-02 regression tests: SleepThread and honest bench counters.

PERF-01 (main-context sleep contract): the boot-era svc_sleep() only called
sched_yield(), so a positive SleepThread duration was lost in the main context
and the guest workers never ran during the "sleep".  A real sleep must

  * hand execution to runnable guest workers while the main context waits,
  * keep the main context parked until the requested deadline,
  * not busy-poll the host (sched_yield count must not grow),
  * treat duration 0 as a pure yield and a negative duration as "no wake
    deadline" without overflowing the deadline arithmetic,
  * work with no ready peers, for worker-context sleeps, and in both the
    single-translation-unit and split-batch runtimes at -O0/-O2.

PERF-02 (honest counters): the §5.1 harness reported only the main context's
`steps`, so worker retirement was invisible.  The runtime now keeps main,
per-worker and aggregate retired-instruction counters plus dispatch, sleep
and idle/wait counts, and BENCH reports all of them.  The test builds a guest
whose main context retires K=3 instructions and whose worker retires M=3 and
requires aggregate = K + M with per-thread totals and matching dispatch
counts.

Run:  python3 tools/test_sleep_scheduler.py [--keep]
Env:  DQ8_TOOL (default target/release/dq8tool), DQ8_CLANG (default clang)
"""

import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOL = Path(os.environ.get("DQ8_TOOL", ROOT / "target" / "release" / "dq8tool"))
CLANG = os.environ.get("DQ8_CLANG", "clang")
OPTS = ["-O0", "-O2"]
KEEP = "--keep" in sys.argv

NX = 0x00100000
BX_LR = struct.pack("<I", 0xE12FFF1E)          # bx lr
SPIN = struct.pack("<I", 0xEAFFFFFE)            # b .  (infinite loop)
EXIT3 = struct.pack("<I", 0xE1A00000) * 2 + BX_LR  # mov r0,r0; mov r0,r0; bx lr


def sh(cmd, **kw):
    return subprocess.run([str(c) for c in cmd], text=True, capture_output=True, **kw)


class Build:
    def __init__(self, work: Path, tag: str, code: bytes, fixture: str):
        self.work = work
        code_path = work / (tag + ".code")
        code_path.write_bytes(code)
        raw = work / (tag + ".raw.c")
        p = sh([TOOL, "recompile-arm", code_path, hex(NX), raw])
        assert p.returncode == 0, (tag, p.stderr)
        text = raw.read_text()
        marker = "int main(int argc, char **argv)"
        assert marker in text, (tag, "generated main() not found")
        self.src = work / (tag + ".c")
        self.src.write_text(
            text.replace(marker, "int original_main(int argc, char **argv)", 1).rstrip()
            + "\n" + fixture)

    def exe(self, opt: str, split: bool) -> Path:
        name = "%s-%s-%s" % (self.src.stem, opt.replace("-", ""), "split" if split else "single")
        exe = self.work / name
        if split:
            outdir = self.work / (name + ".d")
            p = sh(["python3", ROOT / "tools" / "split_dispatch.py", self.src, outdir])
            assert p.returncode == 0, p.stderr
            sources = sorted(outdir.glob("*.c"))
            p = sh([CLANG, "-std=c11", opt, "-w", "-I" + str(outdir),
                    *sources, "-lm", "-o", exe])
        else:
            p = sh([CLANG, "-std=c11", opt, "-w", self.src, "-lm", "-o", exe])
        assert p.returncode == 0, (name, p.stderr)
        return exe


SLEEP_FIXTURE = r'''
#include <assert.h>
static uint64_t process_cpu_ns(void) {
    struct timespec ts;
    assert(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) == 0);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
/* A runnable worker that never exits: every dispatch retires 5000
   instructions and returns to the scheduler with the thread still runnable,
   which is exactly the shape of a decoder/audio worker. */
static void make_worker(void) {
    memset(guest_threads, 0, sizeof(guest_threads));
    guest_thread_count = 1;
    guest_thread_cursor = 0;
    GuestThread *w = &guest_threads[0];
    w->active = 1;
    w->guest_thread_id = 0x50000u;
    w->priority = 40;
    w->pc = 0x00100000u;
    w->r[14] = 0xFFFFFFFFu;
}
int main(void) {
    ram = calloc(1, GUEST_RAM_SIZE);
    assert(ram);
    make_worker();
    GuestThread *w = &guest_threads[0];
    current_guest_thread_id = 1u;

    /* ---- positive duration: worker runs, main honours the deadline ---- */
    uint64_t y0 = perf_host_yields, s0 = perf_host_sleeps;
    uint64_t t0 = host_monotonic_ns();
    svc_sleep(500000ull);
    uint64_t elapsed = host_monotonic_ns() - t0;
    uint64_t worker_retired = w->retired_steps, worker_dispatch = w->dispatch_calls;
    printf("sleep_main elapsed_ns=%llu worker_retired=%llu worker_dispatches=%llu "
           "host_yields=%llu host_sleeps=%llu main_sleeps=%llu\n",
           (unsigned long long)elapsed, (unsigned long long)worker_retired,
           (unsigned long long)worker_dispatch,
           (unsigned long long)(perf_host_yields - y0),
           (unsigned long long)(perf_host_sleeps - s0),
           (unsigned long long)perf_main_sleeps);
    assert(elapsed >= 500000ull);            /* no early resume */
    assert(elapsed < 50000000ull);           /* and no 50 ms blind nanosleep */
    assert(worker_retired > 0);              /* the worker really executed */
    assert(worker_dispatch > 0);
    assert(perf_host_sleeps == s0);           /* runnable peers never nap the host */
    assert(w->retired_steps == worker_retired);
    assert(perf_host_yields == y0);           /* not a busy sched_yield poll */
    assert(perf_main_sleeps == 1);
    assert(perf_main_sleep_ns == 500000ull);

    /* a longer request keeps the worker progressing throughout and paces the
       host with real sleeps instead of spinning */
    uint64_t before = w->retired_steps;
    uint64_t s2 = perf_host_sleeps;
    t0 = host_monotonic_ns();
    svc_sleep(2000000ull);
    elapsed = host_monotonic_ns() - t0;
    printf("sleep_long elapsed_ns=%llu worker_delta=%llu host_sleeps=%llu\n",
           (unsigned long long)elapsed,
           (unsigned long long)(w->retired_steps - before),
           (unsigned long long)(perf_host_sleeps - s2));
    assert(elapsed >= 2000000ull);
    assert(w->retired_steps > before);
    assert(perf_host_sleeps == s2);           /* keep running slices to deadline */

    /* ---- zero duration: one yield, no sleep, no park ---- */
    uint64_t y1 = perf_host_yields, s1 = perf_host_sleeps;
    svc_sleep(0u);
    printf("sleep_zero host_yields=%llu host_sleeps=%llu\n",
           (unsigned long long)(perf_host_yields - y1),
           (unsigned long long)(perf_host_sleeps - s1));
    assert(perf_host_yields == y1 + 1u);
    assert(perf_host_sleeps == s1);

    /* ---- no ready peers: the deadline is still honoured ---- */
    w->blocked = 1;
    uint64_t dispatch_before = w->dispatch_calls;
    uint64_t sleeps_before = perf_host_sleeps;
    t0 = host_monotonic_ns();
    svc_sleep(300000ull);
    elapsed = host_monotonic_ns() - t0;
    printf("sleep_no_peers elapsed_ns=%llu worker_dispatches=%llu\n",
           (unsigned long long)elapsed,
           (unsigned long long)(w->dispatch_calls - dispatch_before));
    assert(elapsed >= 300000ull);
    assert(w->dispatch_calls == dispatch_before);
    assert(perf_host_sleeps > sleeps_before); /* no runnable work really parks */

    /* Main-context waits use the same no-runnable deadline service. */
    uint32_t regs[16] = {0};
    assert(svc_create_event(regs) == 0u);
    uint32_t wait_event = regs[1];
    sleeps_before = perf_host_sleeps;
    t0 = host_monotonic_ns();
    uint32_t wait_result = svc_wait_synchronization(wait_event, 300000ull);
    elapsed = host_monotonic_ns() - t0;
    printf("wait_no_peers elapsed_ns=%llu host_sleeps=%llu result=%08X\n",
           (unsigned long long)elapsed,
           (unsigned long long)(perf_host_sleeps - sleeps_before), wait_result);
    assert(wait_result == 0x09401BFEu);
    assert(elapsed >= 300000ull);
    assert(perf_host_sleeps > sleeps_before);

    /* A genuinely idle main context should sleep rather than consume a core. */
    uint64_t cpu_before = process_cpu_ns();
    t0 = host_monotonic_ns();
    svc_sleep(100000000ull);
    elapsed = host_monotonic_ns() - t0;
    uint64_t cpu_elapsed = process_cpu_ns() - cpu_before;
    printf("sleep_idle_cpu wall_ns=%llu cpu_ns=%llu\n",
           (unsigned long long)elapsed, (unsigned long long)cpu_elapsed);
    assert(elapsed >= 100000000ull);
    assert(cpu_elapsed * 100u <= elapsed * 5u); /* <=5% of one core */

    /* ---- negative duration: no 2^64 deadline, no deadlock ---- */
    t0 = host_monotonic_ns();
    svc_sleep((uint64_t)-1);
    elapsed = host_monotonic_ns() - t0;
    printf("sleep_negative elapsed_ns=%llu\n", (unsigned long long)elapsed);
    assert(elapsed < 200000000ull);          /* did not sleep ~584 years */
    assert(perf_main_sleeps == 5);           /* zero duration not counted */

    /* ---- worker context: the scheduler owns the park ---- */
    thread_execution_depth = 1;
    current_guest_thread_id = 0x50000u;
    svc_sleep(2000000ull);
    printf("worker_sleep yield=%d blocked=%d ns=%llu host_yields=%llu\n",
           thread_yield_requested, thread_blocked_requested,
           (unsigned long long)thread_sleep_requested_ns,
           (unsigned long long)perf_host_yields);
    assert(thread_yield_requested && thread_blocked_requested);
    assert(thread_sleep_requested_ns == 2000000ull);
    assert(perf_worker_sleeps == 1 && w->sleep_calls == 1);
    /* the dispatch loop records exactly this deadline */
    w->blocked = 1;
    w->wake_ns = host_monotonic_ns() + thread_sleep_requested_ns;
    w->wake_result = 0;
    thread_yield_requested = thread_blocked_requested = 0;
    thread_sleep_requested_ns = 0;
    thread_execution_depth = 0;
    schedule_guest_thread();
    printf("worker_before_deadline blocked=%d\n", w->blocked);
    assert(w->blocked);
    guest_host_sleep_ns(2200000ull);
    schedule_guest_thread();
    printf("worker_after_deadline blocked=%d active=%d\n", w->blocked, w->active);
    assert(!w->blocked);                     /* resumed by the deadline pass */

    /* zero duration from a worker: yield only */
    thread_execution_depth = 1;
    current_guest_thread_id = 0x50000u;
    thread_yield_requested = thread_blocked_requested = 0;
    thread_sleep_requested_ns = 123u;
    svc_sleep(0u);
    printf("worker_sleep_zero yield=%d blocked=%d ns=%llu\n",
           thread_yield_requested, thread_blocked_requested,
           (unsigned long long)thread_sleep_requested_ns);
    assert(thread_yield_requested && !thread_blocked_requested);
    assert(thread_sleep_requested_ns == 0u);

    /* negative duration from a worker: block with no deadline (wake_ns 0),
       never wrap around into an already-expired deadline */
    thread_yield_requested = thread_blocked_requested = 0;
    svc_sleep((uint64_t)-1);
    printf("worker_sleep_negative blocked=%d ns=%llu\n",
           thread_blocked_requested, (unsigned long long)thread_sleep_requested_ns);
    assert(thread_yield_requested && thread_blocked_requested);
    assert(thread_sleep_requested_ns == 0u);
    w->blocked = 1;
    w->wake_ns = thread_sleep_requested_ns ? host_monotonic_ns() + thread_sleep_requested_ns : 0u;
    thread_yield_requested = thread_blocked_requested = 0;
    thread_execution_depth = 0;
    schedule_guest_thread();
    printf("worker_negative_deadline blocked=%d wake_ns=%llu\n",
           w->blocked, (unsigned long long)w->wake_ns);
    assert(w->blocked && w->wake_ns == 0u);

    w->active = 0;
    printf("sleep_scheduler: all scenarios ok\n");
    return 0;
}
'''

COUNTER_FIXTURE = r'''
#include <assert.h>
int main(void) {
    ram = calloc(1, GUEST_RAM_SIZE);
    assert(ram);
    memset(guest_threads, 0, sizeof(guest_threads));
    guest_thread_count = 1;
    GuestThread *w = &guest_threads[0];
    w->active = 1;
    w->guest_thread_id = 0x50000u;
    w->pc = 0x00100000u;
    w->r[14] = 0xFFFFFFFFu;

    /* main context: K = 3 retired instructions, 1 dispatch */
    uint32_t r[16] = {0};
    r[13] = 0x10000000u;
    r[14] = 0xFFFFFFFFu;
    translated_from(r, 0x00100000u, NULL);
    uint64_t main_steps = perf_main_retired;

    /* worker context: M = 3 retired instructions, 1 dispatch */
    schedule_guest_thread();
    uint64_t worker_steps = perf_worker_retired;

    printf("COUNTERS main=%llu worker=%llu aggregate=%llu per_thread=%llu "
           "main_dispatches=%llu worker_dispatches=%llu dispatch_calls=%llu\n",
           (unsigned long long)main_steps, (unsigned long long)worker_steps,
           (unsigned long long)(main_steps + worker_steps),
           (unsigned long long)w->retired_steps,
           (unsigned long long)perf_main_dispatches,
           (unsigned long long)perf_worker_dispatches,
           (unsigned long long)(perf_main_dispatches + perf_worker_dispatches));
    assert(main_steps == 3ull);
    assert(worker_steps == 3ull);
    assert(w->retired_steps == 3ull);
    assert(w->dispatch_calls == 1ull);
    assert(perf_main_dispatches == 1ull && perf_worker_dispatches == 1ull);
    assert(main_steps + worker_steps == 6ull);
    printf("counters: all scenarios ok\n");
    return 0;
}
'''

BENCH_FIXTURE = r'''
#include <assert.h>
int main(void) {
    ram = calloc(1, GUEST_RAM_SIZE);
    assert(ram);
    memset(guest_threads, 0, sizeof(guest_threads));
    guest_thread_count = 1;
    GuestThread *w = &guest_threads[0];
    w->active = 1;
    w->guest_thread_id = 0x50000u;
    w->pc = 0x00100000u;
    w->r[14] = 0xFFFFFFFFu;
    /* worker already retired 3 instructions and dispatched once */
    schedule_guest_thread();
    assert(perf_worker_retired == 3ull);
    /* main context: 3 retired instructions, one dispatch */
    uint32_t r[16] = {0};
    r[13] = 0x10000000u;
    r[14] = 0xFFFFFFFFu;
    translated_from(r, 0x00100000u, NULL);
    assert(perf_main_retired == 3ull && perf_main_dispatches == 1ull);
    bench_t0_ns = host_monotonic_ns();
    bench_report(0ull, 0x00100000u);   /* prints BENCH/BENCH_THREAD and exits */
    return 0;
}
'''


class SleepSchedulerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not TOOL.exists():
            p = sh(["cargo", "build", "--offline", "--release"], cwd=ROOT)
            assert p.returncode == 0, p.stderr
        cls._tmp = tempfile.TemporaryDirectory(prefix="dq8-perf-sleep-")
        cls.work = Path(cls._tmp.name)

    @classmethod
    def tearDownClass(cls):
        if KEEP:
            print("kept:", cls.work)
        else:
            cls._tmp.cleanup()

    def run_variants(self, build):
        env = dict(os.environ, DQ8_RUN_GUEST_THREADS="1")
        env.pop("DQ8_INPUT_FILE", None)
        out = {}
        for opt in OPTS:
            for split in (False, True):
                exe = build.exe(opt, split)
                p = sh([exe], env=env, timeout=120)
                out[(opt, split)] = p
                print("== %s %s" % (opt, "split" if split else "single"))
                print(p.stdout.strip())
                if p.returncode != 0:
                    print(p.stderr.strip())
                self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        return out

    def test_main_sleep_contract(self):
        build = Build(self.work, "sleep", SPIN, SLEEP_FIXTURE)
        out = self.run_variants(build)
        for (opt, split), p in out.items():
            self.assertIn("sleep_scheduler: all scenarios ok", p.stdout,
                          (opt, split))

    def test_counters_main_worker_aggregate(self):
        build = Build(self.work, "counters", EXIT3, COUNTER_FIXTURE)
        out = self.run_variants(build)
        for (opt, split), p in out.items():
            self.assertIn("aggregate=6", p.stdout, (opt, split))
            self.assertIn("counters: all scenarios ok", p.stdout, (opt, split))

    def test_bench_line_reports_split_counters(self):
        build = Build(self.work, "bench", EXIT3, BENCH_FIXTURE)
        out = self.run_variants(build)
        for (opt, split), p in out.items():
            line = [l for l in p.stderr.splitlines() if l.startswith("BENCH ")]
            self.assertEqual(len(line), 1, (opt, split, p.stderr[-500:]))
            line = line[0]
            for key, want in (("main_steps=3", "3"), ("worker_steps=3", "3"),
                              ("aggregate_steps=6", "6"),
                              ("main_dispatches=1", "1"),
                              ("worker_dispatches=1", "1"),
                              ("dispatch_calls=2", "2"),
                              ("worker_threads=1", "1")):
                self.assertIn(key, line, (opt, split, line))
            thread = [l for l in p.stderr.splitlines() if l.startswith("BENCH_THREAD")]
            self.assertTrue(thread, (opt, split))
            self.assertIn("retired=3", thread[0])
            self.assertIn("dispatches=1", thread[0])


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]] + [a for a in sys.argv[1:] if a != "--keep"])
