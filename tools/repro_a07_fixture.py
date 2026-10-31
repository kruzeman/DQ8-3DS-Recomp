#!/usr/bin/env python3
"""Re-run the audit's own A07 fixture (audit80-cpu.py kernel part) against the
FIXED runtime from this worktree and print the same three evidence lines.

The fixture is copied verbatim from
/home/hermes/DQ8/reports/DQ8_CODE_AUDIT_evidence/audit80-cpu.py so the output
is directly comparable with the recorded bug reproduction in cpu-reproductions.log:
    mutex_release blocked=0/0 locked=0 (both resume success without ownership transfer)
    mutex_waiters returned=1/1 status=00000000/00000000 locked=0 expected_locked=1
    oneshot_event returned=1/1 signaled=1 (expected one acquired, signal consumed)
"""
from pathlib import Path
import os, subprocess, sys, tempfile

ROOT = Path(__file__).resolve().parents[1]
tool = os.environ.get('DQ8_TOOL')
if not tool:
    sys.exit('set DQ8_TOOL to the dq8tool built from this worktree')

FIXTURE = r'''
int main(void) {
 ram=calloc(1,GUEST_RAM_SIZE); uint32_t regs[16]={0}; regs[1]=1;svc_create_mutex(regs);uint32_t h=regs[1];
 guest_thread_count=2;
 for(unsigned i=0;i<2;i++) {GuestThread *t=&guest_threads[i];t->active=1;t->blocked=1;t->wait_handle=h;t->guest_thread_id=0x50000+i;t->pc=0x100000;t->r[14]=0xffffffffu;t->r[0]=0xdeadbeefu;}
 svc_release_mutex(h);
 printf("mutex_release blocked=%d/%d locked=%d\n",guest_threads[0].blocked,guest_threads[1].blocked,mutex_locked[0]);
 schedule_guest_thread();schedule_guest_thread();
 printf("mutex_waiters returned=%d/%d status=%08X/%08X locked=%d owner=%08X expected_locked=1\n",!guest_threads[0].active,!guest_threads[1].active,guest_threads[0].r[0],guest_threads[1].r[0],mutex_locked[0],mutex_owner[0]);
 regs[1]=0;svc_create_event(regs);h=regs[1];
 memset(guest_threads,0,sizeof(guest_threads));guest_thread_count=2;
 for(unsigned i=0;i<2;i++){GuestThread*t=&guest_threads[i];t->active=t->blocked=1;t->wait_handle=h;t->guest_thread_id=0x50000+i;t->pc=0x100000;t->r[14]=0xffffffffu;t->r[0]=0xdeadbeefu;}
 svc_signal_event(h);schedule_guest_thread();schedule_guest_thread();
 printf("oneshot_event returned=%d/%d signaled=%d (expected one acquired, signal consumed)\n",!guest_threads[0].active,!guest_threads[1].active,event_signaled[h]);
 free(ram);return 0;
}
'''

env = dict(os.environ)
env['DQ8_RUN_GUEST_THREADS'] = '1'
with tempfile.TemporaryDirectory(prefix='dq8-a07-fixture-') as d:
    d = Path(d)
    code, src, exe = d / 'stub.code', d / 'stub.c', d / 'kernel'
    code.write_bytes(bytes.fromhex('1eff2fe1'))
    subprocess.run([tool, 'recompile-arm', str(code), '0x00100000', str(src)],
                   check=True, capture_output=True, env=env)
    runtime = src.read_text().replace('int main(int argc, char **argv)',
                                      'int original_main(int argc, char **argv)')
    src.write_text(runtime + FIXTURE)
    subprocess.run(['clang', '-std=c11', '-O2', '-w', str(src), '-lm', '-o', str(exe)], check=True)
    run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60, env=env)
    sys.stdout.write(run.stdout)
    sys.stderr.write(run.stderr)
    sys.exit(run.returncode)
