#!/usr/bin/env python3
"""Exercise the unified kernel wait-object state machine (audit A07).

One path covers every producer: readiness -> waiter selection ->
acquire/consume -> result recorded -> runnable.  The scenarios below check
the properties the audit listed as required:

  * two and three waiters on a mutex and on a one-shot event,
  * manual-reset events,
  * Wait1/WaitN equivalence, including mixed priority selection,
  * timeout versus signal,
  * the HID/Y2R/GSP producers going through the same acquire/consume path.
"""
from pathlib import Path
import os, subprocess, tempfile, unittest

FIXTURE = r'''
#include <assert.h>
static void park_one(GuestThread *t, uint32_t id, uint32_t handle, int32_t priority) {
    memset(t, 0, sizeof(*t));
    t->active = 1; t->blocked = 1;
    t->guest_thread_id = id;
    t->wait_handle = handle;
    t->priority = priority;
    t->pc = 0x100000; t->r[14] = 0xffffffffu;
    t->r[0] = 0xDEADBEEFu; /* must be overwritten by the recorded result */
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
static void reset_threads(unsigned n) {
    memset(guest_threads, 0, sizeof(guest_threads));
    guest_thread_count = n;
    guest_thread_cursor = 0;
}
/* Park a worker on `handle` exactly the way svcWaitSynchronization does,
   including the scheduler bookkeeping that follows a real SVC. */
static GuestThread *park_via_svc(uint32_t handle, uint64_t timeout) {
    reset_threads(1);
    GuestThread *w = &guest_threads[0];
    w->active = 1; w->guest_thread_id = 0x50000u; w->priority = 40;
    w->pc = 0x100000u; w->r[14] = 0xffffffffu; w->r[0] = 0xDEADBEEFu;
    thread_execution_depth = 1; current_guest_thread_id = 0x50000u;
    assert(svc_wait_synchronization(handle, timeout) == 0);
    assert(thread_blocked_requested && thread_wait_handle_requested == handle);
    w->blocked = 1;
    w->wait_handle = thread_wait_handle_requested;
    w->wake_ns = thread_sleep_requested_ns ? host_monotonic_ns() + thread_sleep_requested_ns : 0u;
    w->wake_result = thread_wake_result_requested;
    thread_execution_depth = 0; current_guest_thread_id = 1u;
    thread_yield_requested = 0; thread_blocked_requested = 0;
    thread_wait_handle_requested = 0; thread_sleep_requested_ns = 0;
    thread_wake_result_requested = 0;
    return w;
}

int main(void) {
    ram = calloc(1, GUEST_RAM_SIZE); assert(ram);
    uint32_t r[16] = {0};

    /* ---- mutex: release hands the lock to exactly one waiter ---- */
    r[1] = 1u; assert(svc_create_mutex(r) == 0);
    uint32_t mtx = r[1];
    unsigned mslot = mtx - 0x10000u;
    assert(mutex_live[mslot] && mutex_locked[mslot]);
    assert(mutex_owner[mslot] == 1u && mutex_recursion[mslot] == 1u);
    current_guest_thread_id = 1u;
    reset_threads(3);
    park_one(&guest_threads[0], 0x50000u, mtx, 40);
    park_one(&guest_threads[1], 0x50001u, mtx, 20);
    park_one(&guest_threads[2], 0x50002u, mtx, 30);
    assert(svc_release_mutex(mtx) == 0);
    printf("mutex_release_1 unblocked=%d/%d/%d locked=%u owner=%08X recursion=%u\n",
           !guest_threads[0].blocked, !guest_threads[1].blocked, !guest_threads[2].blocked,
           mutex_locked[mslot], mutex_owner[mslot], mutex_recursion[mslot]);
    assert(!guest_threads[1].blocked && guest_threads[0].blocked && guest_threads[2].blocked);
    assert(guest_threads[1].r[0] == 0u && mutex_locked[mslot]);
    assert(mutex_owner[mslot] == 0x50001u && mutex_recursion[mslot] == 1u);
    /* the winner releases: the next priority waiter takes it over */
    current_guest_thread_id = 0x50001u;
    assert(svc_release_mutex(mtx) == 0);
    printf("mutex_release_2 unblocked=%d/%d locked=%u owner=%08X\n",
           !guest_threads[0].blocked, !guest_threads[2].blocked,
           mutex_locked[mslot], mutex_owner[mslot]);
    assert(!guest_threads[2].blocked && guest_threads[0].blocked);
    assert(guest_threads[2].r[0] == 0u && mutex_owner[mslot] == 0x50002u && mutex_locked[mslot]);
    current_guest_thread_id = 0x50002u;
    assert(svc_release_mutex(mtx) == 0);
    printf("mutex_release_3 unblocked=%d locked=%u owner=%08X\n",
           !guest_threads[0].blocked, mutex_locked[mslot], mutex_owner[mslot]);
    assert(!guest_threads[0].blocked && guest_threads[0].r[0] == 0u);
    assert(mutex_locked[mslot] && mutex_owner[mslot] == 0x50000u && mutex_recursion[mslot] == 1u);
    /* last holder releases with nobody left waiting: the mutex stays free */
    current_guest_thread_id = 0x50000u;
    assert(svc_release_mutex(mtx) == 0);
    assert(!mutex_locked[mslot] && mutex_owner[mslot] == 0u && mutex_recursion[mslot] == 0u);

    /* ---- mutex: owner/recursion are explicit, other threads stay out ---- */
    r[1] = 0u; assert(svc_create_mutex(r) == 0);
    uint32_t m2 = r[1];
    unsigned s2 = m2 - 0x10000u;
    current_guest_thread_id = 7u;
    assert(guest_object_ready(m2, 7u) && guest_object_acquire(m2, 7u));
    assert(guest_object_acquire(m2, 7u));               /* recursive take */
    assert(mutex_locked[s2] && mutex_owner[s2] == 7u && mutex_recursion[s2] == 2u);
    assert(!guest_object_ready(m2, 8u) && !guest_object_acquire(m2, 8u));
    assert(svc_release_mutex(m2) == 0);
    printf("mutex_recursion after_release locked=%u recursion=%u owner=%08X\n",
           mutex_locked[s2], mutex_recursion[s2], mutex_owner[s2]);
    assert(mutex_locked[s2] && mutex_recursion[s2] == 1u && mutex_owner[s2] == 7u);
    assert(svc_release_mutex(m2) == 0);
    assert(!mutex_locked[s2] && mutex_owner[s2] == 0u);

    /* ---- one-shot event: one signal, one waiter, signal consumed ---- */
    r[1] = 0u; assert(svc_create_event(r) == 0);
    uint32_t ev = r[1];
    reset_threads(2);
    park_one(&guest_threads[0], 0x50000u, ev, 40);
    park_one(&guest_threads[1], 0x50001u, ev, 20);
    current_guest_thread_id = 1u;
    assert(svc_signal_event(ev) == 0);
    printf("oneshot_signal_1 unblocked=%d/%d signaled=%u\n",
           !guest_threads[0].blocked, !guest_threads[1].blocked, event_signaled[ev]);
    assert(!guest_threads[1].blocked && guest_threads[0].blocked);
    assert(guest_threads[1].r[0] == 0u && event_signaled[ev] == 0u);
    assert(svc_signal_event(ev) == 0);
    printf("oneshot_signal_2 unblocked=%d/%d signaled=%u\n",
           !guest_threads[0].blocked, !guest_threads[1].blocked, event_signaled[ev]);
    assert(!guest_threads[0].blocked && guest_threads[0].r[0] == 0u && event_signaled[ev] == 0u);
    /* with nobody waiting the signal latches, and ClearEvent drops it */
    assert(svc_signal_event(ev) == 0);
    assert(event_signaled[ev] == 1u);
    assert(svc_clear_event(ev) == 0 && event_signaled[ev] == 0u);

    /* ---- manual-reset event: every waiter completes, signal survives ---- */
    r[1] = 1u; assert(svc_create_event(r) == 0);
    uint32_t mev = r[1];
    reset_threads(3);
    park_one(&guest_threads[0], 0x50000u, mev, 40);
    park_one(&guest_threads[1], 0x50001u, mev, 20);
    park_one(&guest_threads[2], 0x50002u, mev, 30);
    assert(svc_signal_event(mev) == 0);
    printf("manual_reset unblocked=%d/%d/%d signaled=%u\n",
           !guest_threads[0].blocked, !guest_threads[1].blocked, !guest_threads[2].blocked,
           event_signaled[mev]);
    assert(!guest_threads[0].blocked && !guest_threads[1].blocked && !guest_threads[2].blocked);
    assert(guest_threads[0].r[0] == 0u && guest_threads[1].r[0] == 0u && guest_threads[2].r[0] == 0u);
    assert(event_signaled[mev] == 1u);

    /* ---- Wait1 versus WaitN on the same object, priority decides ---- */
    r[1] = 0u; assert(svc_create_event(r) == 0);
    uint32_t e1 = r[1];
    r[1] = 0u; assert(svc_create_event(r) == 0);
    uint32_t e2 = r[1];
    uint32_t pair[2] = {e1, e2};
    reset_threads(2);
    park_one(&guest_threads[0], 0x50000u, e1, 40);          /* Wait1 */
    park_waitn(&guest_threads[1], 0x50001u, pair, 2, 0, 20); /* WaitN wait-any */
    assert(svc_signal_event(e1) == 0);
    printf("wait1_vs_waitn wait1=%d waitn=%d waitn_index=%08X signaled=%u\n",
           !guest_threads[0].blocked, !guest_threads[1].blocked, guest_threads[1].r[1],
           event_signaled[e1]);
    assert(!guest_threads[1].blocked && guest_threads[0].blocked);
    assert(guest_threads[1].r[0] == 0u && guest_threads[1].r[1] == 0u);
    assert(event_signaled[e1] == 0u);
    /* the remaining Wait1 waiter completes on the next signal */
    assert(svc_signal_event(e1) == 0);
    assert(!guest_threads[0].blocked && guest_threads[0].r[0] == 0u && event_signaled[e1] == 0u);
    /* WaitN reports the index of the object that woke it */
    reset_threads(1);
    park_waitn(&guest_threads[0], 0x50000u, pair, 2, 0, 40);
    assert(svc_signal_event(e2) == 0);
    printf("waitn_index waitn=%d r0=%08X r1=%08X signaled_e2=%u\n",
           !guest_threads[0].blocked, guest_threads[0].r[0], guest_threads[0].r[1],
           event_signaled[e2]);
    assert(!guest_threads[0].blocked && guest_threads[0].r[0] == 0u && guest_threads[0].r[1] == 1u);
    assert(event_signaled[e2] == 0u);
    /* wait-all must not consume a partially satisfied list */
    reset_threads(1);
    park_waitn(&guest_threads[0], 0x50000u, pair, 2, 1, 40);
    assert(svc_signal_event(e1) == 0);
    assert(guest_threads[0].blocked && event_signaled[e1] == 1u);
    assert(svc_signal_event(e2) == 0);
    printf("wait_all waitn=%d r0=%08X e1=%u e2=%u\n",
           !guest_threads[0].blocked, guest_threads[0].r[0], event_signaled[e1], event_signaled[e2]);
    assert(!guest_threads[0].blocked && guest_threads[0].r[0] == 0u);
    assert(event_signaled[e1] == 0u && event_signaled[e2] == 0u);

    /* ---- timeout versus signal ---- */
    r[1] = 0u; assert(svc_create_event(r) == 0);
    uint32_t tev = r[1];
    /* audit R2-05: a signal that arrives after a waiter's deadline has
       already passed must not reach that waiter; the scheduler's deadline
       pass retires it with its own timeout and the signal latches for the
       next active waiter */
    GuestThread *w = park_via_svc(tev, 1000000ull);
    assert(w->wake_result == 0x09401bfeu && w->wake_ns != 0u);
    w->wake_ns = host_monotonic_ns() - 1000000000ull; /* expired a second ago */
    assert(svc_signal_event(tev) == 0);
    printf("late_signal expired_waiter_blocked=%d r0=%08X signaled=%u\n",
           w->blocked, w->r[0], event_signaled[tev]);
    assert(w->blocked && w->r[0] == 0xDEADBEEFu);    /* not consumed */
    assert(event_signaled[tev] == 1u);               /* latched, not lost */
    schedule_guest_thread();
    printf("late_signal_deadline expired_waiter_blocked=%d r0=%08X signaled=%u\n",
           w->blocked, w->r[0], event_signaled[tev]);
    assert(!w->blocked && w->r[0] == 0x09401bfeu);   /* its own timeout */
    assert(event_signaled[tev] == 1u);               /* still there */
    /* an active waiter, even with a worse priority, takes the latched signal */
    assert(svc_wait_synchronization(tev, 0) == 0u && event_signaled[tev] == 0u);
    /* expired high-priority waiter versus active low-priority waiter on the
       same one-shot event: exactly one waiter completes on the signal, and
       it is the active one (both priority permutations below) */
    for (unsigned perm = 0; perm < 2u; ++perm) {
        r[1] = 0u; assert(svc_create_event(r) == 0);
        uint32_t dev = r[1];
        reset_threads(2);
        park_one(&guest_threads[0], 0x50000u, dev, perm ? 20 : 10);
        park_one(&guest_threads[1], 0x50001u, dev, perm ? 10 : 20);
        GuestThread *expired = perm ? &guest_threads[1] : &guest_threads[0];
        GuestThread *active = perm ? &guest_threads[0] : &guest_threads[1];
        expired->wake_ns = host_monotonic_ns() - 1000000000ull;
        expired->wake_result = 0x09401bfeu;
        assert(svc_signal_event(dev) == 0);
        printf("expired_vs_active perm=%u expired_blocked=%d expired_r0=%08X "
               "active_blocked=%d active_r0=%08X signaled=%u\n",
               perm, expired->blocked, expired->r[0],
               active->blocked, active->r[0], event_signaled[dev]);
        assert(expired->blocked && expired->r[0] == 0xDEADBEEFu);
        assert(!active->blocked && active->r[0] == 0u);
        assert(event_signaled[dev] == 0u);            /* consumed by the active one */
        schedule_guest_thread();
        assert(!expired->blocked && expired->r[0] == 0x09401bfeu);
    }
    /* no signal: the deadline records an explicit timeout result */
    w = park_via_svc(tev, 1000000ull);
    w->wake_ns = host_monotonic_ns() - 1u;
    schedule_guest_thread();
    printf("timeout_only r0=%08X active=%d\n", w->r[0], w->active);
    assert(w->r[0] == 0x09401bfeu);
    assert(!w->blocked && !w->active);
    /* a zero timeout polls: nothing ready, immediate timeout, nothing parked */
    current_guest_thread_id = 1u;
    assert(svc_wait_synchronization(tev, 0) == 0x09401bfeu);
    assert(event_signaled[tev] == 0u);

    /* ---- HID producer goes through the common acquire/consume path ---- */
    put32(active_tls_base + 0x80u, 0x000a0000u);
    assert(!svc_send_sync(11u));                       /* HID:GetSharedMemoryHandle */
    assert(!svc_map_memory_block(0x30010u, 0x10000000u));
    uint32_t hid_ev = native_hid_events[0];
    assert(hid_ev && event_live[hid_ev] && !event_manual_reset[hid_ev]);
    reset_threads(2);
    park_one(&guest_threads[0], 0x50000u, hid_ev, 40);
    park_one(&guest_threads[1], 0x50001u, hid_ev, 20);
    native_hid_buttons = 1; native_hid_next_update = 0;
    native_hid_pump();
    printf("hid_producer unblocked=%d/%d r0=%08X signaled=%u\n",
           !guest_threads[0].blocked, !guest_threads[1].blocked,
           guest_threads[1].r[0], event_signaled[hid_ev]);
    assert(!guest_threads[1].blocked && guest_threads[0].blocked);
    assert(guest_threads[1].r[0] == 0u);
    assert(event_signaled[hid_ev] == 0u);             /* consumed, not just woken */
    native_hid_next_update = 0;
    native_hid_pump();
    assert(!guest_threads[0].blocked && event_signaled[hid_ev] == 0u);

    /* ---- Y2R transfer-end producer ---- */
    r[1] = 0u; assert(svc_create_event(r) == 0);
    y2r.end_event = r[1];
    reset_threads(2);
    park_one(&guest_threads[0], 0x50000u, y2r.end_event, 40);
    park_one(&guest_threads[1], 0x50001u, y2r.end_event, 20);
    y2r_signal_end();
    printf("y2r_producer unblocked=%d/%d r0=%08X signaled=%u\n",
           !guest_threads[0].blocked, !guest_threads[1].blocked,
           guest_threads[1].r[0], event_signaled[y2r.end_event]);
    assert(!guest_threads[1].blocked && guest_threads[0].blocked);
    assert(guest_threads[1].r[0] == 0u && event_signaled[y2r.end_event] == 0u);

    /* ---- GSP relay producer (PDC1, so no scan-out snapshot side effect) ---- */
    r[1] = 0u; assert(svc_create_event(r) == 0);
    gsp_relay_event = r[1]; gsp_relay_registered = 1u;
    gsp_shared_base = 0x6000u; gsp_thread_id = 1u;
    reset_threads(2);
    park_one(&guest_threads[0], 0x50000u, gsp_relay_event, 40);
    park_one(&guest_threads[1], 0x50001u, gsp_relay_event, 20);
    gsp_enqueue_interrupt(3u);
    printf("gsp_producer unblocked=%d/%d r0=%08X signaled=%u\n",
           !guest_threads[0].blocked, !guest_threads[1].blocked,
           guest_threads[1].r[0], event_signaled[gsp_relay_event]);
    assert(!guest_threads[1].blocked && guest_threads[0].blocked);
    assert(guest_threads[1].r[0] == 0u && event_signaled[gsp_relay_event] == 0u);
    gsp_relay_registered = 0u;

    printf("kernel_wait_state_machine: all scenarios ok\n");
    free(ram);
    return 0;
}
'''

class KernelWaitTest(unittest.TestCase):
    def test_acquire_consume_state_machine(self):
        with tempfile.TemporaryDirectory(prefix='dq8-kernel-waits-') as d:
            d = Path(d)
            code = d / 'stub.code'
            source = d / 'stub.c'
            exe = d / 'test'
            code.write_bytes(bytes.fromhex('1eff2fe1'))
            subprocess.run([os.environ['DQ8_TOOL'], 'recompile-arm', str(code),
                            '0x00100000', str(source)], check=True, capture_output=True)
            runtime = source.read_text().replace(
                'int main(int argc, char **argv)', 'int guest_main(int argc, char **argv)')
            source.write_text(runtime + FIXTURE)
            subprocess.run(['clang', '-std=c11', '-O0', '-w', str(source), '-lm', '-o', str(exe)],
                           check=True)
            env = dict(os.environ)
            env.pop('DQ8_INPUT_FILE', None)
            env['DQ8_RUN_GUEST_THREADS'] = '1'
            run = subprocess.run([str(exe)], capture_output=True, text=True,
                                 timeout=60, env=env)
            print(run.stdout.strip())
            if run.stderr.strip():
                print(run.stderr.strip())
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

if __name__ == '__main__':
    unittest.main()
