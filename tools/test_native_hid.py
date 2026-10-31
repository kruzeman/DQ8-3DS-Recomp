#!/usr/bin/env python3
"""Exercise the emitted 3DS HID ring, events and two distinct A presses."""
from pathlib import Path
import os,subprocess,tempfile,unittest
class NativeHidTest(unittest.TestCase):
    def test_shared_ring_press_release_press(self):
        with tempfile.TemporaryDirectory(prefix='dq8-hid-test-') as d:
            d=Path(d);code=d/'stub.code';source=d/'stub.c';exe=d/'test'
            code.write_bytes(bytes.fromhex('1eff2fe1'))
            subprocess.run([os.environ['DQ8_TOOL'],'recompile-arm',str(code),'0x00100000',str(source)],check=True,capture_output=True)
            runtime=source.read_text().replace('int main(int argc, char **argv)','int guest_main(int argc, char **argv)')
            source.write_text(runtime+r'''
#include <assert.h>
int main(void) {
    ram=calloc(1,GUEST_RAM_SIZE);assert(ram);
    put32(active_tls_base+0x80,0x000a0000);assert(!svc_send_sync(11));
    assert(mem32(active_tls_base+0x80)==0x000a0047 && mem32(active_tls_base+0x88)==0x14000000);
    assert(mem32(active_tls_base+0x8c)==0x30010);
    for(unsigned i=0;i<5;i++)assert(event_live[mem32(active_tls_base+0x90+i*4)]);
    assert(!svc_map_memory_block(0x30010,0x10000000));
    put32(0x14000000,0x12345678); // Former wrong HID address is game heap.
    native_hid_buttons=1;native_hid_next_update=0;native_hid_pump();
    assert(mem32(native_hid_base+28)==1 && mem32(native_hid_base+16)==0);
    assert(mem32(native_hid_base+40)==1 && mem32(native_hid_base+44)==1 && !mem32(native_hid_base+48));
    assert(mem32(native_hid_base) || mem32(native_hid_base+4));
    native_hid_buttons=0;native_hid_next_update=0;native_hid_pump();
    assert(mem32(native_hid_base+16)==1 && !mem32(native_hid_base+56));
    assert(!mem32(native_hid_base+60) && mem32(native_hid_base+64)==1);
    native_hid_buttons=1;native_hid_next_update=0;native_hid_pump();
    assert(mem32(native_hid_base+16)==2 && mem32(native_hid_base+72)==1 && mem32(native_hid_base+76)==1);
    assert(mem32(0x14000000)==0x12345678);
    assert(event_signaled[native_hid_events[0]] && event_signaled[native_hid_events[1]]);
    free(ram);return 0;
}
''')
            subprocess.run(['clang','-std=c11','-O0','-w',str(source),'-lm','-o',str(exe)],check=True)
            env=dict(os.environ);env.pop('DQ8_INPUT_FILE',None)
            subprocess.run([str(exe)],check=True,timeout=5,env=env)
if __name__=='__main__':unittest.main()
