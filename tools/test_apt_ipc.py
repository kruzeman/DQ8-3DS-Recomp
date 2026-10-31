#!/usr/bin/env python3
"""Exercise the emitted APT service, not a second Python implementation."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class AptProtocolTest(unittest.TestCase):
    def test_home_resume_mailbox_and_events(self):
        tool = os.environ['DQ8_TOOL']
        with tempfile.TemporaryDirectory(prefix='dq8-apt-test-') as directory:
            work = Path(directory)
            code, source, binary = work/'stub.code', work/'stub.c', work/'test'
            code.write_bytes(bytes.fromhex('1eff2fe1'))  # ARM bx lr
            subprocess.run([tool, 'recompile-arm', str(code), '0x00100000', str(source)], check=True, capture_output=True)
            runtime = source.read_text().replace('int main(int argc, char **argv)', 'int guest_main(int argc, char **argv)')
            source.write_text(runtime + r'''
#include <assert.h>
static void request(uint32_t command) {
    put32(active_tls_base+0x80, command);
    assert(svc_send_sync(5)==0);
}
int main(void) {
    ram=calloc(1,GUEST_RAM_SIZE); assert(ram);
    put32(active_tls_base+0x84,0);
    put32(active_tls_base+0x88,0x3a545041); // Stale SRV "APT:A" bytes.
    put32(active_tls_base+0x8c,0x66630041);
    request(0x00010040);
    assert(mem32(active_tls_base+0x80)==0x000100c2);
    assert(mem32(active_tls_base+0x88)==0 && mem32(active_tls_base+0x8c)==0);
    assert(mem32(active_tls_base+0x90)==0);
    uint32_t lock=mem32(active_tls_base+0x94);
    assert(lock>=0x10000 && mutex_live[lock-0x10000]);
    request(0x00020080);
    assert(mem32(active_tls_base+0x80)==0x00020043);
    uint32_t notification=mem32(active_tls_base+0x8c);
    uint32_t parameter=mem32(active_tls_base+0x90);
    assert(notification!=parameter && event_live[notification] && event_live[parameter]);
    assert(!event_signaled[notification] && event_signaled[parameter]);
    put32(active_tls_base+0x184,0x1000);
    put32(active_tls_base+0x88,0);
    request(0x000e0080); // Glance preserves the message.
    assert(mem32(active_tls_base+0x80)==0x000e0104);
    assert(mem32(active_tls_base+0x8c)==1 && event_signaled[parameter]);
    request(0x000d0080);
    assert(mem32(active_tls_base+0x80)==0x000d0104);
    assert(mem32(active_tls_base+0x8c)==1 && !event_signaled[parameter]);
    request(0x000d0080); // Empty is NoData, never an invented repeating Wakeup.
    assert(mem32(active_tls_base+0x84)==0xc8a0cfef);
    put32(0x005ccc40,0x12345678); put32(0x005ccca4,0x12345678);
    put32(0x14000000+28,0x12345678);
    request(0x002c0044);
    assert(event_signaled[parameter] && !event_signaled[notification]);
    assert(mem32(0x005ccc40)==0x12345678 && mem32(0x005ccca4)==0x12345678);
    assert(mem32(0x14000000+28)==0x12345678);
    request(0x000d0080);
    assert(mem32(active_tls_base+0x8c)==0xb && !event_signaled[parameter]);
    assert(mem32(active_tls_base+0x94)==0x10 && mem32(active_tls_base+0x98)==0);
    assert(mem32(active_tls_base+0xa0)==0x1000);
    request(0x000d0080); assert(mem32(active_tls_base+0x84)==0xc8a0cfef);
    for (unsigned utility=4; utility<=7; ++utility) {
        unsigned size=utility==6?4:1;
        put32(active_tls_base+0x84,utility);
        put32(active_tls_base+0x88,utility==5?8:utility==4?1:4);
        put32(active_tls_base+0x8c,size);
        put32(active_tls_base+0x184,0x1000);
        put32(0x1000,0xffffffff);
        request(0x004b00c2);
        assert(mem32(active_tls_base+0x80)==0x004b0082);
        assert(mem8(0x1000)==(utility==6));
        if(utility==6) assert(mem32(0x1000)==1);
        assert(mem32(active_tls_base+0x8c)==((size<<14)|2));
        assert(mem32(active_tls_base+0x90)==0x1000);
    }
    free(ram); return 0;
}
''')
            subprocess.run(['clang','-std=c11','-O0','-w',str(source),'-lm','-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True,timeout=5)

if __name__=='__main__':
    unittest.main()
