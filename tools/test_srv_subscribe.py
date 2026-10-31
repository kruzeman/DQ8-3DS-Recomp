#!/usr/bin/env python3
"""Exercise the emitted srv: Subscribe/Unsubscribe handler, not a second
Python implementation.  Root-caused from the operator escape:
  unimplemented 3DS SVC SendSyncRequest: handle=0x00000001 command=0x00090040
on the 'Create a new adventure log' flow.  Azahar
(src/core/hle/service/sm/srv.cpp:198-232) implements both commands as stubs:
input word 1 = notification ID, response = header echo + Result success."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class SrvSubscribeTest(unittest.TestCase):
    def test_subscribe_unsubscribe_result_success(self):
        tool = os.environ['DQ8_TOOL']
        with tempfile.TemporaryDirectory(prefix='dq8-srv-test-') as directory:
            work = Path(directory)
            code, source, binary = work/'stub.code', work/'stub.c', work/'test'
            code.write_bytes(bytes.fromhex('1eff2fe1'))  # ARM bx lr
            subprocess.run([tool, 'recompile-arm', str(code), '0x00100000', str(source)], check=True, capture_output=True)
            runtime = source.read_text().replace('int main(int argc, char **argv)', 'int guest_main(int argc, char **argv)')
            source.write_text(runtime + r'''
#include <assert.h>
static void request(uint32_t command) {
    put32(active_tls_base+0x80, command);
    assert(svc_send_sync(1)==0);
}
int main(void) {
    ram=calloc(1,GUEST_RAM_SIZE); assert(ram);
    /* SRV:Subscribe (operator escape command): notification ID 0x209 in
       word 1.  Response must echo the header and return Result success. */
    put32(active_tls_base+0x84, 0x209);
    request(0x00090040);
    assert(mem32(active_tls_base+0x80)==0x00090040);
    assert(mem32(active_tls_base+0x84)==0);
    /* SRV:Unsubscribe: same stub shape per azahar srv.cpp:216-232. */
    put32(active_tls_base+0x84, 0x209);
    request(0x000A0040);
    assert(mem32(active_tls_base+0x80)==0x000A0040);
    assert(mem32(active_tls_base+0x84)==0);
    free(ram); return 0;
}
''')
            subprocess.run(['clang','-std=c11','-O0','-w',str(source),'-lm','-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True,timeout=5)

if __name__=='__main__':
    unittest.main()
