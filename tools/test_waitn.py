#!/usr/bin/env python3
"""Compile and exercise the emitted multi-object wait and scheduler."""
from pathlib import Path
import os, subprocess, tempfile, unittest

class WaitNTest(unittest.TestCase):
    def test_abi_wait_any_all_and_resumption(self):
        with tempfile.TemporaryDirectory(prefix='dq8-waitn-test-') as d:
            d=Path(d); code=d/'stub.code'; source=d/'stub.c'; exe=d/'test'
            # Entry 0 is a return stub for the existing worker fixtures;
            # entry 1 signals an arbiter from a scheduled guest worker.
            code.write_bytes(bytes.fromhex('1eff2fe1220000ef1eff2fe1'))
            subprocess.run([os.environ['DQ8_TOOL'],'recompile-arm',str(code),'0x00100000',str(source),
                            '--extra-entry','0x00100004'],check=True,capture_output=True)
            runtime=source.read_text().replace('int main(int argc, char **argv)','int guest_main(int argc, char **argv)')
            source.write_text(runtime+r'''
#include <assert.h>
static void setup_child(void) {
    memset(&guest_threads[0],0,sizeof(GuestThread));
    guest_thread_count=1;
    guest_threads[0].active=1;guest_threads[0].guest_thread_id=0x50000;
    guest_threads[0].pc=0x100000;guest_threads[0].r[14]=0xffffffff;
    guest_threads[0].tls_base=0x1ff82200;
}
int main(void) {
    ram=calloc(1,GUEST_RAM_SIZE);assert(ram);
    uint32_t r[16]={0}; r[1]=0;svc_create_event(r);uint32_t first=r[1];
    r[1]=0;svc_create_event(r);uint32_t second=r[1];
    put32(0x100,first);put32(0x104,second);
    active_guest_regs=r; r[13]=0x200;
    put32(0x200,0xffffffff);put32(0x204,0xffffffff); // stale stack must not be timeout
    r[0]=0;r[1]=0x100;r[2]=2;r[3]=0;r[4]=0;
    assert(svc_wait_synchronization_n(r)==0x09401bfe);
    svc_signal_event(first);svc_signal_event(second);
    r[0]=0;r[1]=0x100;r[4]=0;
    assert(svc_wait_synchronization_n(r)==0 && r[1]==0);
    assert(!event_signaled[first] && event_signaled[second]);
    // wait-all must not consume a partially satisfied list.
    r[0]=0;r[1]=0x100;r[3]=1;
    assert(svc_wait_synchronization_n(r)==0x09401bfe && event_signaled[second]);
    svc_signal_event(first);r[0]=0;r[1]=0x100;
    assert(svc_wait_synchronization_n(r)==0);
    assert(!event_signaled[first] && !event_signaled[second]);
    // An infinite wait parks. Signalling its SECOND object resumes with index 1.
    setup_child();thread_execution_depth=1;current_guest_thread_id=0x50000;
    GuestThread *child=&guest_threads[0];
    child->r[0]=0xffffffff;child->r[4]=0xffffffff;child->r[1]=0x100;child->r[2]=2;
    child->r[13]=0x200;
    assert(svc_wait_synchronization_n(child->r)==0 && thread_blocked_requested);
    assert(child->wait_count==2 && !thread_sleep_requested_ns);
    child->blocked=1;
    thread_execution_depth=0;current_guest_thread_id=1;thread_blocked_requested=0;
    thread_yield_requested=0;
    svc_signal_event(second);schedule_guest_thread();
    assert(!child->active && child->r[0]==0 && child->r[1]==1);
    // Finite timeout is taken from r0/r4 and delivered on resumed context.
    setup_child();child=&guest_threads[0];
    child->r[0]=1;child->r[4]=0;child->r[1]=0x100;child->r[2]=2;child->r[13]=0x200;
    thread_execution_depth=1;current_guest_thread_id=0x50000;
    assert(svc_wait_synchronization_n(child->r)==0 && thread_sleep_requested_ns==1);
    child->blocked=1;child->wake_ns=host_monotonic_ns()-1;
    thread_execution_depth=0;current_guest_thread_id=1;thread_blocked_requested=0;
    thread_yield_requested=0;
    schedule_guest_thread();
    assert(!child->active && child->r[0]==0x09401bfe && child->r[1]==0xffffffff);
    put32(0x100,0);r[0]=0;r[1]=0x100;r[2]=1;r[3]=0;r[4]=0;
    assert(svc_wait_synchronization_n(r)==0xd8e007f7);
    // Opening a save must not overwrite either live RomFS session or events.
    romfs_image=tmpfile();assert(romfs_image);
    put32(active_tls_base+0x88,3);put32(active_tls_base+0x94,2);
    put32(active_tls_base+0x98,12);put32(active_tls_base+0xb0,0x800);
    put32(active_tls_base+0x80,0x08030204);assert(!svc_send_sync(8));
    uint32_t rom_a=mem32(active_tls_base+0x8c);
    put32(active_tls_base+0x88,3);put32(active_tls_base+0x94,2);
    put32(active_tls_base+0x98,12);put32(active_tls_base+0xb0,0x800);
    put32(active_tls_base+0x80,0x08030204);assert(!svc_send_sync(8));
    uint32_t rom_b=mem32(active_tls_base+0x8c);
    put32(active_tls_base+0x80,0x080201c2);assert(!svc_send_sync(8));
    uint32_t save=mem32(active_tls_base+0x8c);
    assert(rom_a!=rom_b && save!=rom_a && save!=rom_b);
    assert(guest_file_is_romfs[rom_a] && guest_file_is_romfs[rom_b]);
    assert(!event_live[rom_a] && !event_live[rom_b] && !event_live[save]);
    assert(save!=first && save!=second);
    r[1]=0;svc_create_event(r);assert(r[1]!=rom_a && r[1]!=rom_b && r[1]!=save);
    fclose(romfs_image);
    // Arbiter Signal must preserve guest event state and honor wake count/priority.
    uint32_t regs[16]={0};svc_create_address_arbiter(regs);uint32_t arb=regs[1];
    guest_thread_count=3;
    for(unsigned i=0;i<3;i++) {
        memset(&guest_threads[i],0,sizeof(GuestThread));
        guest_threads[i].active=guest_threads[i].blocked=1;
        guest_threads[i].wait_address=0x300;guest_threads[i].wait_arbiter=arb;
        guest_threads[i].priority=i==1?20:40;
    }
    put32(0x300,1);
    assert(guest_threads[0].blocked && guest_threads[1].blocked && guest_threads[2].blocked);
    assert(!svc_arbitrate(arb,0x300,0,0,0,0));
    assert(mem32(0x300)==1 && guest_threads[1].blocked);
    assert(!svc_arbitrate(arb,0x300,0,1,0,0));
    assert(mem32(0x300)==1 && !guest_threads[1].blocked);
    assert(guest_threads[0].blocked && guest_threads[2].blocked);
    assert(!svc_arbitrate(arb,0x300,0,0xffffffff,0,0));
    assert(mem32(0x300)==1 && !guest_threads[0].blocked && !guest_threads[2].blocked);
    put32(0x300,2);assert(!svc_arbitrate(arb,0x300,2,1,0,0));
    assert(mem32(0x300)==2); // Decrement only if it must actually wait.

    /* The main context must park on the arbiter read-set until a guest peer
       signals it; a low word alone is not permission to return early. */
    uint32_t main_arb_regs[16]={0};
    assert(svc_create_address_arbiter(main_arb_regs)==0u);
    uint32_t main_arb=main_arb_regs[1];
    guest_thread_count=1;guest_thread_cursor=0;
    GuestThread *signal_worker=&guest_threads[0];
    memset(signal_worker,0,sizeof(*signal_worker));
    signal_worker->active=1;signal_worker->guest_thread_id=0x50000u;
    signal_worker->priority=40;signal_worker->pc=0x00100004u;
    signal_worker->r[14]=0xffffffffu;
    signal_worker->r[0]=main_arb;signal_worker->r[1]=0x300u;
    signal_worker->r[2]=0u;signal_worker->r[3]=1u;
    put32(0x300,0u);
    current_guest_thread_id=1u;thread_execution_depth=0;
    uint64_t t0=host_monotonic_ns();
    uint32_t main_wait_result=svc_arbitrate(main_arb,0x300u,1u,1u,0u,0u);
    uint64_t elapsed=host_monotonic_ns()-t0;
    printf("main_arbiter_signal result=%08X word=%08X elapsed_ns=%llu\n",
           main_wait_result,mem32(0x300u),(unsigned long long)elapsed);
    assert(main_wait_result==0u && mem32(0x300u)==0u && !main_arbiter_waiting);
    assert(signal_worker->r[0]==0u);

    /* Decrement-and-wait mutates the word exactly once before parking. */
    guest_thread_cursor=0;
    memset(signal_worker,0,sizeof(*signal_worker));
    signal_worker->active=1;signal_worker->guest_thread_id=0x50000u;
    signal_worker->priority=40;signal_worker->pc=0x00100004u;
    signal_worker->r[14]=0xffffffffu;
    signal_worker->r[0]=main_arb;signal_worker->r[1]=0x304u;
    signal_worker->r[2]=0u;signal_worker->r[3]=1u;
    put32(0x304,0u);
    main_wait_result=svc_arbitrate(main_arb,0x304u,2u,1u,0u,0u);
    printf("main_arbiter_decrement result=%08X word=%08X\n",main_wait_result,mem32(0x304u));
    assert(main_wait_result==0u && mem32(0x304u)==0xffffffffu && !main_arbiter_waiting);

    /* A timed main arbiter wait parks to its timeout instead of returning
       after an arbitrary peer-quantum budget. */
    signal_worker->active=0;
    put32(0x308,0u);
    uint64_t naps_before=perf_host_sleeps;
    t0=host_monotonic_ns();
    main_wait_result=svc_arbitrate(main_arb,0x308u,3u,1u,2000000u,0u);
    elapsed=host_monotonic_ns()-t0;
    printf("main_arbiter_timeout result=%08X elapsed_ns=%llu host_sleeps=%llu\n",
           main_wait_result,(unsigned long long)elapsed,
           (unsigned long long)(perf_host_sleeps-naps_before));
    assert(main_wait_result==0x09401bfeu && elapsed>=2000000u);
    assert(perf_host_sleeps>naps_before && mem32(0x308u)==0u && !main_arbiter_waiting);

    /* Worker timed waits retain a deadline and resume with TIMEOUT. */
    memset(signal_worker,0,sizeof(*signal_worker));
    guest_thread_count=1;guest_thread_cursor=0;
    signal_worker->active=1;signal_worker->guest_thread_id=0x50000u;
    signal_worker->priority=40;signal_worker->pc=0x00100000u;
    signal_worker->r[14]=0xffffffffu;
    put32(0x30c,0u);
    thread_execution_depth=1;current_guest_thread_id=0x50000u;
    thread_yield_requested=thread_blocked_requested=0;
    thread_sleep_requested_ns=0;thread_wake_result_requested=0;
    uint32_t worker_result=svc_arbitrate(main_arb,0x30cu,3u,1u,2000000u,0u);
    assert(worker_result==0x09401bfeu && thread_blocked_requested);
    assert(thread_sleep_requested_ns==2000000u &&
           thread_wake_result_requested==0x09401bfeu);
    signal_worker->blocked=1;signal_worker->wait_arbiter=main_arb;
    signal_worker->wait_address=0x30cu;signal_worker->wait_type=3u;
    signal_worker->wait_value=1u;signal_worker->wake_ns=host_monotonic_ns()-1u;
    signal_worker->wake_result=thread_wake_result_requested;
    thread_execution_depth=0;current_guest_thread_id=1u;
    thread_yield_requested=thread_blocked_requested=0;
    schedule_guest_thread();
    assert(!signal_worker->blocked && signal_worker->r[0]==0x09401bfeu);

    free(ram);return 0;
    }
''')
            subprocess.run(['clang','-std=c11','-O0','-w',str(source),'-lm','-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True,timeout=5,env={**os.environ,'DQ8_RUN_GUEST_THREADS':'1'})
if __name__=='__main__':unittest.main()
