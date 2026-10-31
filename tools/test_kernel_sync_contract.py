#!/usr/bin/env python3
"""Contract tests for the kernel synchronization fixes (audit R2-02..R2-06).

Every scenario below drives the real translated guest code through the real
scheduler entry points, the same way the audit reproduced each defect:

  * R2-02 exclusive monitor: A:LDREX -> B:STR -> A:STREX fails without
    overwriting B's store; a bare context switch, CLREX, doubleword and
    halfword reservations, and an overlapping byte alias all drop the
    reservation, while a disjoint store keeps it.
  * R2-03 ExitThread: a worker that owns a recursive kernel mutex exits
    through the real guest SVC 0xEF000009; Wait1, WaitN and thread joiners
    all complete, and exactly one waiter takes the freed mutex.
  * R2-04 ReleaseMutex: a foreign release is refused with WrongLockingThread
    and leaves owner/depth/waiters untouched, for plain and nested owners.
  * R2-06 GSP pacing: a zero-time relay-event poll never parks, a finite
    wait never sleeps past its own deadline, and the pacing park keeps its
    provisional success only when the vblank deadline really lands first.
"""
from pathlib import Path
import os, re, subprocess, tempfile, unittest

ROOT = Path(__file__).resolve().parents[1]

# 0x00100000 svc 0xEF000009        (ExitThread)
# 0x00100004 bx lr
# 0x00100008 ldrex r0,[r1]
# 0x0010000c bx lr
# 0x00100010 strex r2,r3,[r1]
# 0x00100014 bx lr
# 0x00100018 str r3,[r1]
# 0x0010001c bx lr
# 0x00100020 clrex
# 0x00100024 bx lr
# 0x00100028 ldrexd r0,r1,[r2]
# 0x0010002c bx lr
# 0x00100030 strexd r4,r2,r3,[r1]
# 0x00100034 bx lr
# 0x00100038 ldrexh r0,[r1]
# 0x0010003c bx lr
# 0x00100040 strexh r2,r3,[r1]
# 0x00100044 bx lr
CODE = bytes.fromhex(
    '090000ef' '1eff2fe1'          # 0x100000 svc ExitThread; 0x100004 bx lr
    '9f0f91e1' '1eff2fe1'          # 0x100008 ldrex;         0x10000c bx lr
    '932f81e1' '1eff2fe1'          # 0x100010 strex;         0x100014 bx lr
    '003081e5' '1eff2fe1'          # 0x100018 str;           0x10001c bx lr
    '1ff07ff5' '1eff2fe1'          # 0x100020 clrex;         0x100024 bx lr
    '9f0fb2e1' '1eff2fe1'          # 0x100028 ldrexd;        0x10002c bx lr
    '924fa1e1' '1eff2fe1'          # 0x100030 strexd;        0x100034 bx lr
    '9f0ff1e1' '1eff2fe1'          # 0x100038 ldrexh;        0x10003c bx lr
    '932fe1e1' '1eff2fe1')         # 0x100040 strexh;        0x100044 bx lr
EXTRA_ENTRIES = [0x100004, 0x100008, 0x100010, 0x100018, 0x100020,
                 0x100028, 0x100030, 0x100038, 0x100040]

FIXTURE = r'''
#include <assert.h>
static void reset_threads(unsigned n) {
    memset(guest_threads, 0, sizeof(guest_threads));
    guest_thread_count = n;
    guest_thread_cursor = 0;
}
static void park_one(GuestThread *t, uint32_t id, uint32_t handle, int32_t priority) {
    memset(t, 0, sizeof(*t));
    t->active = 1; t->blocked = 1;
    t->guest_thread_id = id;
    t->wait_handle = handle;
    t->priority = priority;
    t->pc = 0x100000; t->r[14] = 0xffffffffu;
    t->r[0] = 0xDEADBEEFu;
}
static void park_waitn(GuestThread *t, uint32_t id, const uint32_t *handles,
                       uint32_t count, uint32_t all, int32_t priority) {
    memset(t, 0, sizeof(*t));
    t->active = 1; t->blocked = 1;
    t->guest_thread_id = id;
    t->priority = priority;
    for (uint32_t i = 0; i < count; ++i) t->wait_handles[i] = handles[i];
    t->wait_count = count; t->wait_all = all; t->wait_n_active = 1;
    t->pc = 0x100000; t->r[14] = 0xffffffffu;
    t->r[0] = 0xDEADBEEFu;
}
/* A runnable worker the scheduler can pick up, mirroring svcCreateThread. */
static GuestThread *make_worker(uint32_t id, uint32_t pc, int32_t priority) {
    reset_threads(1);
    GuestThread *t = &guest_threads[0];
    t->active = 1; t->guest_thread_id = id; t->priority = priority;
    t->pc = pc; t->r[14] = 0xffffffffu;
    return t;
}

int main(void) {
    ram = calloc(1, GUEST_RAM_SIZE); assert(ram);
    uint32_t r[16] = {0};
    uint32_t aregs[16] = {0};
    aregs[14] = 0xffffffffu; /* BX LR returns to the scheduler sentinel */
    current_guest_thread_id = 0x40000u;

    /* ==== R2-02: single-core exclusive monitor ==== */
    /* sanity: LDREX -> STREX with no interference still succeeds */
    put32(0x100u, 1u);
    aregs[1] = 0x100u; aregs[3] = 3u; aregs[2] = 0u;
    translated_from(aregs, 0x00100008u, NULL);   /* ldrex r0,[r1] */
    assert(aregs[0] == 1u && exclusive_addr == 0x100u && exclusive_size == 4u);
    translated_from(aregs, 0x00100010u, NULL);   /* strex r2,r3,[r1] */
    printf("exclusive_sane strex=%u memory=%08X\n", aregs[2], mem32(0x100u));
    assert(aregs[2] == 0u && mem32(0x100u) == 3u);
    assert(exclusive_addr == 0xFFFFFFFFu);       /* STREX consumed it */

    /* interference: A:LDREX -> B:STR (real translated store through the
       real scheduler) -> A:STREX must fail, not overwrite B's value */
    put32(0x100u, 1u);
    GuestThread *b = make_worker(0x50000u, 0x00100018u, 40); /* str r3,[r1] */
    b->r[1] = 0x100u; b->r[3] = 2u;
    aregs[1] = 0x100u; aregs[3] = 3u; aregs[2] = 0u;
    translated_from(aregs, 0x00100008u, NULL);   /* A: ldrex */
    assert(exclusive_addr == 0x100u);
    schedule_guest_thread();                    /* B: str r3,[r1] */
    assert(mem32(0x100u) == 2u && exclusive_addr == 0xFFFFFFFFu);
    translated_from(aregs, 0x00100010u, NULL);   /* A: strex */
    printf("exclusive_interference strex=%u memory=%08X\n", aregs[2], mem32(0x100u));
    assert(aregs[2] == 1u && mem32(0x100u) == 2u);

    /* a bare context switch, with no store at all, clears the monitor */
    put32(0x100u, 5u);
    b = make_worker(0x50000u, 0x00100004u, 40);  /* bx lr only */
    aregs[1] = 0x100u; aregs[3] = 7u; aregs[2] = 0u;
    translated_from(aregs, 0x00100008u, NULL);   /* A: ldrex */
    assert(exclusive_addr == 0x100u);
    schedule_guest_thread();
    assert(exclusive_addr == 0xFFFFFFFFu);
    translated_from(aregs, 0x00100010u, NULL);   /* A: strex */
    printf("exclusive_switch strex=%u memory=%08X\n", aregs[2], mem32(0x100u));
    assert(aregs[2] == 1u && mem32(0x100u) == 5u);

    /* CLREX drops the reservation explicitly */
    aregs[1] = 0x100u; aregs[3] = 9u; aregs[2] = 0u;
    translated_from(aregs, 0x00100008u, NULL);   /* ldrex */
    assert(exclusive_addr == 0x100u);
    translated_from(aregs, 0x00100020u, NULL);   /* clrex */
    assert(exclusive_addr == 0xFFFFFFFFu);
    translated_from(aregs, 0x00100010u, NULL);   /* strex */
    printf("exclusive_clrex strex=%u memory=%08X\n", aregs[2], mem32(0x100u));
    assert(aregs[2] == 1u && mem32(0x100u) == 5u);

    /* doubleword reservation dies on a word store inside its 8-byte range */
    put32(0x200u, 0u); put32(0x204u, 0u);
    aregs[2] = 0x200u;                              /* ldrexd r0,r1,[r2] */
    translated_from(aregs, 0x00100028u, NULL);
    assert(exclusive_addr == 0x200u && exclusive_size == 8u);
    b = make_worker(0x50000u, 0x00100018u, 40);
    b->r[1] = 0x204u; b->r[3] = 0xAAu;              /* store at range tail */
    schedule_guest_thread();
    assert(mem32(0x204u) == 0xAAu && exclusive_addr == 0xFFFFFFFFu);
    aregs[1] = 0x200u; aregs[2] = 0x22222222u;
    aregs[3] = 0x33333333u; aregs[4] = 0u;          /* strexd r4,r2,r3,[r1] */
    translated_from(aregs, 0x00100030u, NULL);
    printf("exclusive_double strex=%u lo=%08X hi=%08X\n",
           aregs[4], mem32(0x200u), mem32(0x204u));
    assert(aregs[4] == 1u && mem32(0x200u) == 0u && mem32(0x204u) == 0xAAu);

    /* halfword reservation dies on a word store at the same address */
    put32(0x300u, 0u);
    aregs[1] = 0x300u;                              /* ldrexh r0,[r1] */
    translated_from(aregs, 0x00100038u, NULL);
    assert(exclusive_addr == 0x300u && exclusive_size == 2u);
    b = make_worker(0x50000u, 0x00100018u, 40);
    b->r[1] = 0x300u; b->r[3] = 0x77u;
    schedule_guest_thread();
    assert(exclusive_addr == 0xFFFFFFFFu);
    aregs[2] = 0u; aregs[3] = 0x58u;                /* strexh r2,r3,[r1] */
    translated_from(aregs, 0x00100040u, NULL);
    printf("exclusive_half strex=%u memory=%04X\n", aregs[2], (unsigned)mem16(0x300u));
    assert(aregs[2] == 1u && mem16(0x300u) == 0x77u);

    /* address alias: a byte store one byte into the reservation kills it */
    put32(0x304u, 0x12345678u);
    aregs[1] = 0x304u; aregs[3] = 0x77u; aregs[2] = 0u;
    translated_from(aregs, 0x00100008u, NULL);      /* ldrex */
    assert(exclusive_addr == 0x304u);
    put8(0x305u, 0x99u);                           /* overlapping byte store */
    assert(exclusive_addr == 0xFFFFFFFFu);
    translated_from(aregs, 0x00100010u, NULL);      /* strex */
    printf("exclusive_alias strex=%u memory=%08X\n", aregs[2], mem32(0x304u));
    assert(aregs[2] == 1u && mem32(0x304u) == 0x12349978u);

    /* a disjoint store must NOT drop the reservation */
    put32(0x308u, 0x11111111u); put32(0x30Cu, 0x22222222u);
    aregs[1] = 0x308u; aregs[3] = 0x44u; aregs[2] = 0u;
    translated_from(aregs, 0x00100008u, NULL);      /* ldrex @0x308 */
    assert(exclusive_addr == 0x308u);
    put32(0x310u, 0xEEu);                          /* neighbouring word */
    assert(exclusive_addr == 0x308u);
    translated_from(aregs, 0x00100010u, NULL);      /* strex */
    printf("exclusive_disjoint strex=%u memory=%08X\n", aregs[2], mem32(0x308u));
    assert(aregs[2] == 0u && mem32(0x308u) == 0x44u);

    /* ==== R2-03: ExitThread releases the mutexes it owns ==== */
    r[1] = 0u; assert(svc_create_mutex(r) == 0);
    uint32_t xm = r[1]; unsigned xs = xm - 0x10000u;
    uint32_t one = xm;
    reset_threads(4);
    GuestThread *a = &guest_threads[0];
    a->active = 1; a->guest_thread_id = 0x50000u; a->priority = 40;
    a->pc = 0x00100000u; a->r[14] = 0xffffffffu;    /* svc ExitThread */
    park_one(&guest_threads[1], 0x50001u, xm, 20);            /* Wait1 */
    park_waitn(&guest_threads[2], 0x50002u, &one, 1, 0, 30);  /* WaitN */
    park_one(&guest_threads[3], 0x50003u, 0x50000u, 50);      /* joiner */
    current_guest_thread_id = 0x50000u;
    assert(guest_object_acquire(xm, 0x50000u));
    assert(guest_object_acquire(xm, 0x50000u));    /* recursion = 2 */
    assert(mutex_locked[xs] && mutex_recursion[xs] == 2u && mutex_owner[xs] == 0x50000u);
    thread_execution_depth = 1;
    translated_from(a->r, a->pc, a);                /* real guest ExitThread */
    thread_execution_depth = 0;
    printf("exit_releases_mutex active=%d locked=%u owner=%08X recursion=%u "
           "wait1=%d waitn=%d joiner=%d\n",
           a->active, mutex_locked[xs], mutex_owner[xs], mutex_recursion[xs],
           !guest_threads[1].blocked, !guest_threads[2].blocked,
           !guest_threads[3].blocked);
    assert(!a->active);
    assert(!guest_threads[1].blocked && guest_threads[1].r[0] == 0u);
    assert(guest_threads[2].blocked);               /* lost the hand-off */
    assert(!guest_threads[3].blocked && guest_threads[3].r[0] == 0u);
    /* the freed mutex went straight to the best waiter, fully unwound */
    assert(mutex_locked[xs] && mutex_owner[xs] == 0x50001u && mutex_recursion[xs] == 1u);
    current_guest_thread_id = 0x50001u;
    assert(svc_release_mutex(xm) == 0u);           /* hands over to WaitN */
    assert(!guest_threads[2].blocked && guest_threads[2].r[0] == 0u);
    assert(mutex_owner[xs] == 0x50002u && mutex_locked[xs]);
    current_guest_thread_id = 0x50002u;
    assert(svc_release_mutex(xm) == 0u);
    assert(!mutex_locked[xs] && mutex_owner[xs] == 0u);

    /* ==== R2-04: a foreign ReleaseMutex is refused ==== */
    r[1] = 0u; assert(svc_create_mutex(r) == 0);
    uint32_t fm = r[1]; unsigned fs = fm - 0x10000u;
    current_guest_thread_id = 7u;
    assert(guest_object_acquire(fm, 7u));
    reset_threads(1);
    park_one(&guest_threads[0], 0x50000u, fm, 20);
    current_guest_thread_id = 8u;                   /* foreign thread */
    uint32_t res = svc_release_mutex(fm);
    printf("foreign_release result=%08X locked=%u owner=%08X recursion=%u waiter_blocked=%d\n",
           res, mutex_locked[fs], mutex_owner[fs], mutex_recursion[fs],
           guest_threads[0].blocked);
    assert(res == 0xD8E0041Fu);
    assert(mutex_locked[fs] && mutex_owner[fs] == 7u && mutex_recursion[fs] == 1u);
    assert(guest_threads[0].blocked);
    current_guest_thread_id = 7u;                   /* the real owner */
    assert(svc_release_mutex(fm) == 0u);
    assert(!guest_threads[0].blocked && guest_threads[0].r[0] == 0u);
    assert(mutex_owner[fs] == 0x50000u && mutex_locked[fs]);
    current_guest_thread_id = 0x50000u;
    assert(svc_release_mutex(fm) == 0u);
    assert(!mutex_locked[fs] && mutex_owner[fs] == 0u);
    /* nested ownership: the foreign release changes nothing at depth 2 */
    r[1] = 0u; assert(svc_create_mutex(r) == 0);
    uint32_t nm = r[1]; unsigned ns = nm - 0x10000u;
    current_guest_thread_id = 9u;
    assert(guest_object_acquire(nm, 9u) && guest_object_acquire(nm, 9u));
    reset_threads(1);
    park_one(&guest_threads[0], 0x50000u, nm, 20);
    current_guest_thread_id = 8u;
    assert(svc_release_mutex(nm) == 0xD8E0041Fu);
    assert(mutex_recursion[ns] == 2u && mutex_owner[ns] == 9u);
    assert(guest_threads[0].blocked);
    current_guest_thread_id = 9u;
    assert(svc_release_mutex(nm) == 0u);           /* depth 2 -> 1 */
    assert(mutex_recursion[ns] == 1u && mutex_locked[ns]);
    assert(guest_threads[0].blocked);
    assert(svc_release_mutex(nm) == 0u);           /* final release hands off */
    assert(!guest_threads[0].blocked && guest_threads[0].r[0] == 0u);
    assert(mutex_locked[ns] && mutex_owner[ns] == 0x50000u && mutex_recursion[ns] == 1u);

    /* ==== R2-06: GSP pacing respects the caller's timeout ==== */
    r[1] = 0u; assert(svc_create_event(r) == 0);
    gsp_relay_event = r[1]; gsp_relay_registered = 1u;
    gsp_shared_base = 0x6000u; gsp_thread_id = 1u;
    gsp_next_vblank_ns = host_monotonic_ns() + 60000000000ull;
    reset_threads(1);
    thread_execution_depth = 1; current_guest_thread_id = 0x50000u;
    /* zero-time poll: returns the kernel timeout at once, parks nothing */
    uint32_t poll = svc_wait_synchronization(gsp_relay_event, 0u);
    printf("gsp_zero_poll result=%08X park=%d\n", poll, thread_blocked_requested);
    assert(poll == 0x09401bfeu);
    assert(!thread_blocked_requested && thread_sleep_requested_ns == 0u);
    /* finite wait below the vblank: parks no longer than its own timeout
       and its deadline records the real kernel timeout */
    event_signaled[gsp_relay_event] = 0u;
    assert(svc_wait_synchronization(gsp_relay_event, 1000u) == 0u);
    assert(thread_yield_requested && thread_blocked_requested);
    assert(thread_wait_handle_requested == gsp_relay_event);
    assert(thread_sleep_requested_ns > 0u && thread_sleep_requested_ns <= 1000u);
    assert(thread_wake_result_requested == 0x09401bfeu);
    printf("gsp_finite_wait park_ns=%llu wake_result=%08X\n",
           (unsigned long long)thread_sleep_requested_ns, thread_wake_result_requested);
    /* finite wait beyond the vblank: parks until the vblank, pacing result */
    event_signaled[gsp_relay_event] = 0u;
    thread_blocked_requested = 0; thread_sleep_requested_ns = 0;
    thread_wake_result_requested = 0; thread_yield_requested = 0;
    assert(svc_wait_synchronization(gsp_relay_event, 120000000000ull) == 0u);
    assert(thread_yield_requested && thread_blocked_requested);
    assert(thread_sleep_requested_ns > 1000000000ll &&
           thread_sleep_requested_ns <= 120000000000ull);
    assert(thread_wake_result_requested == 0u);
    printf("gsp_long_wait park_ns=%llu wake_result=%08X\n",
           (unsigned long long)thread_sleep_requested_ns, thread_wake_result_requested);
    /* infinite wait: the pacing park applies, capped at the vblank */
    event_signaled[gsp_relay_event] = 0u;
    thread_blocked_requested = 0; thread_sleep_requested_ns = 0;
    thread_wake_result_requested = 0; thread_yield_requested = 0;
    assert(svc_wait_synchronization(gsp_relay_event, 0xFFFFFFFFFFFFFFFFull) == 0u);
    assert(thread_yield_requested && thread_blocked_requested);
    assert(thread_sleep_requested_ns > 1000000000ll);
    assert(thread_wake_result_requested == 0u);
    printf("gsp_infinite_wait park_ns=%llu wake_result=%08X\n",
           (unsigned long long)thread_sleep_requested_ns, thread_wake_result_requested);
    /* a due vblank falls through to the ordinary wait semantics: the
       enqueue signals the relay event and the wait succeeds inline */
    event_signaled[gsp_relay_event] = 0u;
    thread_blocked_requested = 0; thread_sleep_requested_ns = 0;
    thread_wake_result_requested = 0; thread_yield_requested = 0;
    gsp_next_vblank_ns = host_monotonic_ns() - 1u;
    assert(svc_wait_synchronization(gsp_relay_event, 1000u) == 0u);
    assert(!thread_blocked_requested);              /* completed inline */
    printf("gsp_due_vblank park=0 signaled=%u\n", event_signaled[gsp_relay_event]);
    thread_execution_depth = 0; current_guest_thread_id = 1u;
    gsp_relay_registered = 0u;

    printf("kernel_sync_contract: all scenarios ok\n");
    free(ram);
    return 0;
}
'''

class KernelSyncContractTest(unittest.TestCase):
    def _run(self, opt, split):
        with tempfile.TemporaryDirectory(prefix='dq8-sync-contract-') as d:
            d = Path(d)
            code = d / 'contract.code'
            source = d / 'contract.c'
            exe = d / 'test'
            code.write_bytes(CODE)
            cmd = [os.environ['DQ8_TOOL'], 'recompile-arm', str(code),
                   '0x00100000', str(source)]
            for entry in EXTRA_ENTRIES:
                cmd += ['--extra-entry', hex(entry)]
            subprocess.run(cmd, check=True, capture_output=True)
            runtime = source.read_text().replace(
                'int main(int argc, char **argv)', 'int guest_main(int argc, char **argv)')
            source.write_text(runtime + FIXTURE)
            if split:
                out = d / 'split'
                subprocess.run(['python3', ROOT / 'tools' / 'split_dispatch.py',
                                str(source), str(out)], check=True, capture_output=True)
                sources = sorted(out.glob('*.c'))
                compile_cmd = ['clang', '-std=c11', opt, '-w', '-I' + str(out),
                               *map(str, sources), '-lm', '-o', str(exe)]
            else:
                compile_cmd = ['clang', '-std=c11', opt, '-w', str(source),
                               '-lm', '-o', str(exe)]
            subprocess.run(compile_cmd, check=True, capture_output=True)
            env = dict(os.environ)
            env.pop('DQ8_INPUT_FILE', None)
            env['DQ8_RUN_GUEST_THREADS'] = '1'
            run = subprocess.run([str(exe)], capture_output=True, text=True,
                                 timeout=120, env=env)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            return run.stdout

    def test_kernel_sync_contract(self):
        outputs = {(opt, split): self._run(opt, split)
                   for opt in ('-O0', '-O2') for split in (False, True)}
        print(outputs[('-O0', False)].strip())
        # park_ns for the ~60 s vblank pacing is wall-clock dependent by
        # construction; everything else must match bit-for-bit.
        norm = lambda s: re.sub(r'park_ns=\d+', 'park_ns=<timed>', s)
        reference = norm(outputs[('-O0', False)])
        for key, output in outputs.items():
            self.assertEqual(norm(output), reference,
                             'kernel sync outputs differ at %s %s' % key)

if __name__ == '__main__':
    unittest.main()
