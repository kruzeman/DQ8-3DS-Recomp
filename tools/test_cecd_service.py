#!/usr/bin/env python3
"""Exercise the emitted cecd:u (CECD/StreetPass) handler, not a second
Python implementation.

Surface: `unimplemented SRV:GetServiceHandle service=cecd:u` raised by the
operator right after the adventure log is created.  The handler is
src/native_cecd.c, written against the local reference
azahar-master/src/core/hle/service/cecd (cecd_u.cpp command table +
cecd.cpp implementations); this test pins the azahar-visible contract:

  * SRV hands out a session handle for "cecd:u" (handle 17),
  * Open(MboxDir, create) creates the box directory and reports ZERO entries
    (azahar cecd.cpp:71-101),
  * ReadMessage on a console with no StreetPass history returns the CEC
    "NoData/NotFound/Status" result 0xC8810BEF and zero bytes - a valid empty
    answer, not zeros-as-success,
  * GetCecdState returns CecdState::NdmStatusIdle (1),
  * the info/change-state events are real kernel events (manual reset,
    unsignaled - azahar never signals them without StreetPass traffic),
  * Start/Stop are azahar stubs that return ResultSuccess.

usage: DQ8_TOOL=target/release/dq8tool python3 tools/test_cecd_service.py
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

CECD_ERR_NODATA = 0xC8810BEF


class CecdServiceTest(unittest.TestCase):
    def test_cecd_command_table(self):
        tool = os.environ.get('DQ8_TOOL', str(ROOT/'target/release/dq8tool'))
        with tempfile.TemporaryDirectory(prefix='dq8-cecd-test-') as directory:
            work = Path(directory)
            code, source, binary = work/'stub.code', work/'stub.c', work/'test'
            user_data = work/'userdata'
            user_data.mkdir()
            code.write_bytes(bytes.fromhex('1eff2fe1'))  # ARM bx lr
            subprocess.run([tool, 'recompile-arm', str(code), '0x00100000', str(source)],
                           check=True, capture_output=True)
            runtime = source.read_text().replace(
                'int main(int argc, char **argv)', 'int guest_main(int argc, char **argv)')
            source.write_text(runtime + r'''
#include <assert.h>
static void request(uint32_t handle, uint32_t command) {
    put32(active_tls_base+0x80, command);
    assert(svc_send_sync(handle)==0);
}
int main(void) {
    ram=calloc(1,GUEST_RAM_SIZE); assert(ram);
    uint32_t tls=active_tls_base;

    /* SRV:GetServiceHandle("cecd:u") - the operator's escape. */
    for (unsigned i=0;i<8;i++) put32(tls+0x84+i, 0);
    put32(tls+0x84, 0x64636563);           /* "cecd" (LE bytes c,e,c,d) */
    put32(tls+0x88, 0x0000753A);           /* ":u\0\0" */
    put32(tls+0x8C, 6);                    /* name length */
    request(1u, 0x00050100u);
    assert(mem32(tls+0x84)==0);            /* Result success */
    uint32_t session=mem32(tls+0x8C);
    assert(session==17u);                  /* CECD_SERVICE_HANDLE */

    /* GetCecdState (0x000E): Result + CecdState::NdmStatusIdle (azahar
       cecd.cpp:698-706, enum cecd.h:213). */
    request(session, 0x000E0002u);
    assert(mem32(tls+0x84)==0);
    assert(mem32(tls+0x88)==1u);

    /* Open (0x0001) MboxDir with the create flag: azahar creates the
       directory and answers success + ZERO entries. */
    put32(tls+0x84, 0x0011D8C0u);          /* ncch_program_id (DQ8) */
    put32(tls+0x88, 11u);                  /* CecDataPathType::MboxDir */
    put32(tls+0x8Cu, 0x8u);                /* open_mode: create */
    put32(tls+0x90u, 0); put32(tls+0x94u, 0);
    request(session, 0x00010182u);
    assert(mem32(tls+0x84)==0);
    assert(mem32(tls+0x88)==0u);           /* zero entries */

    /* ReadMessage (0x0003) for an absent message: CEC NoData/NotFound/Status
       and zero bytes read, plus the two mapped buffers echoed back. */
    uint32_t id_buf=0x08600000u, out_buf=0x08600100u;
    put32(tls+0x84, 0x0011D8C0u);          /* program id */
    put32(tls+0x88, 0u);                   /* is_outbox = false */
    put32(tls+0x8Cu, 0u);                  /* message_id_size = 0 */
    put32(tls+0x90u, 0x200u);              /* buffer_size */
    put32(tls+0x94u, 0x20u); put32(tls+0x98u, id_buf);
    put32(tls+0x9Cu, 0x200u); put32(tls+0xA0u, out_buf);
    request(session, 0x00030202u);
    assert(mem32(tls+0x84)==0x%08Xu);
    assert(mem32(tls+0x88)==0u);           /* zero bytes read */
    assert(mem32(tls+0x8Cu)==0x20u && mem32(tls+0x90u)==id_buf);
    assert(mem32(tls+0x94u)==0x200u && mem32(tls+0x98u)==out_buf);

    /* GetCecInfoEventHandle (0x000F) / GetChangeStateEventHandle (0x0010):
       a real event handle, manual reset, unsignaled. */
    request(session, 0x000F0002u);
    assert(mem32(tls+0x84)==0 && mem32(tls+0x88)==0x10u);
    uint32_t info_event=mem32(tls+0x8Cu);
    assert(info_event>=0x20u && event_live[info_event]);
    assert(event_manual_reset[info_event] && !event_signaled[info_event]);
    request(session, 0x00100002u);
    assert(mem32(tls+0x84)==0 && mem32(tls+0x88)==0x10u);
    uint32_t state_event=mem32(tls+0x8Cu);
    assert(state_event>=0x20u && state_event!=info_event && event_live[state_event]);

    /* Start (0x000B) / Stop (0x000C): azahar stubs -> ResultSuccess. */
    put32(tls+0x84, 1u);                   /* CecCommand::Start */
    request(session, 0x000B0042u);
    assert(mem32(tls+0x84)==0);
    put32(tls+0x84, 0x0Bu);                /* CecCommand::Stop */
    request(session, 0x000C0042u);
    assert(mem32(tls+0x84)==0);

    free(ram); return 0;
}
'''.replace('0x%08Xu', '0x%08Xu' % CECD_ERR_NODATA))
            env = dict(os.environ, DQ8_USER_DATA_DIR=str(user_data))
            subprocess.run(['clang', '-std=c11', '-O0', '-w', str(source), '-lm', '-o', str(binary)],
                           check=True)
            subprocess.run([str(binary)], check=True, timeout=20, env=env)
            # Open(MboxDir, create) really created azahar's box directory below
            # <user-data>/cecd/CEC/<program id>.
            created = list(user_data.glob('cecd/CEC/*'))
            self.assertTrue(created, 'CECD Open(MboxDir, create) must create the box directory')


if __name__ == '__main__':
    unittest.main()
