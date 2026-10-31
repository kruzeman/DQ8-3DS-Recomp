#!/usr/bin/env python3
"""Exercise save-file IPC against a fresh host directory and real reopen."""
from pathlib import Path
import os, subprocess, tempfile, unittest

# The recompiler inlines src/native_fs.c into the generated runtime, so this
# harness exercises the real FS service code against a host directory.
HARNESS_C = r'''
#include <assert.h>
static uint32_t ipc(uint32_t h,uint32_t cmd) {
    put32(active_tls_base+0x80,cmd);assert(!svc_send_sync(h));return mem32(active_tls_base+0x84);
}
static void filename(const char *s) {
    unsigned i=0;do {put16(0x800+i*2,(unsigned char)s[i]);} while(s[i++]);
}
static void file_request(uint32_t archive,uint32_t flags) {
    put32(active_tls_base+0x88,archive);put32(active_tls_base+0x8c,0);
    put32(active_tls_base+0x90,4);put32(active_tls_base+0x94,30);
    put32(active_tls_base+0x98,flags);put32(active_tls_base+0xa4,0x800);
}
int main(void) {
    ram=calloc(1,GUEST_RAM_SIZE);assert(ram);
    put32(active_tls_base+0x84,4);put32(active_tls_base+0x88,1);put32(active_tls_base+0x8c,1);
    assert(!ipc(8,0x080c00c2));
    assert(mem32(active_tls_base+0x80)==0x080c00c0 && !mem32(active_tls_base+0x8c));
    uint32_t archive=mem32(active_tls_base+0x88);filename("/honeySave.bin");
    file_request(archive,1);assert(ipc(8,0x080201c2)==0xc8804470);
    file_request(archive,0);put32(active_tls_base+0x9c,16);put32(active_tls_base+0xa0,0);put32(active_tls_base+0xa8,0x800);
    assert(!ipc(8,0x080801c2));
    file_request(archive,0);put32(active_tls_base+0x9c,16);put32(active_tls_base+0xa0,0);put32(active_tls_base+0xa8,0x800);
    assert(ipc(8,0x080801c2)==0xc82044b4); // creating twice preserves the file
    file_request(archive,3);assert(!ipc(8,0x080201c2));
    uint32_t file=mem32(active_tls_base+0x8c);assert(mem32(active_tls_base+0x80)==0x08020042);
    memcpy(ram+0x1000,"DQ8 save",8);
    put32(active_tls_base+0x84,4);put32(active_tls_base+0x88,0);put32(active_tls_base+0x8c,8);
    put32(active_tls_base+0x90,1);put32(active_tls_base+0x94,0x8a);put32(active_tls_base+0x98,0x1000);
    assert(!ipc(file,0x08030102) && mem32(active_tls_base+0x88)==8);
    assert(!ipc(file,0x08080000));
    file_request(archive,1);assert(!ipc(8,0x080201c2));file=mem32(active_tls_base+0x8c);
    assert(!ipc(file,0x08040000) && mem32(active_tls_base+0x88)==16);
    put32(active_tls_base+0x84,0);put32(active_tls_base+0x88,0);put32(active_tls_base+0x8c,32);
    put32(active_tls_base+0x90,0x20c);put32(active_tls_base+0x94,0x2000);
    assert(!ipc(file,0x080200c2) && mem32(active_tls_base+0x88)==16);
    assert(!memcmp(ram+0x2004,"DQ8 save",8));
    put32(active_tls_base+0x84,16);put32(active_tls_base+0x88,0);put32(active_tls_base+0x8c,8);
    put32(active_tls_base+0x94,0x2000);
    assert(!ipc(file,0x080200c2) && !mem32(active_tls_base+0x88));
    assert(!ipc(file,0x08080000));
    // A real cursor reaches EOF; it must not repeat an invented save entry.
    filename("/");put32(active_tls_base+0x84,archive);put32(active_tls_base+0x88,0);
    put32(active_tls_base+0x8c,4);put32(active_tls_base+0x90,4);put32(active_tls_base+0x98,0x800);
    assert(!ipc(8,0x080b0102));uint32_t dir=mem32(active_tls_base+0x8c);
    put32(active_tls_base+0x84,1);put32(active_tls_base+0x88,0x228c);put32(active_tls_base+0x8c,0x3000);
    assert(!ipc(dir,0x08010042) && mem32(active_tls_base+0x88)==1);
    assert(mem16(0x3000)=='h' && mem32(0x3220)==16);
    put32(active_tls_base+0x84,1);put32(active_tls_base+0x88,0x228c);put32(active_tls_base+0x8c,0x3000);
    assert(!ipc(dir,0x08010042) && mem32(active_tls_base+0x88)==0);
    assert(!ipc(dir,0x08020000));
    // Traversal must not leave the per-title data directory.
    filename("/../escape.bin");file_request(archive,1);assert(ipc(8,0x080201c2)!=0);
    free(ram);return 0;
}
'''

def build_harness(d):
    """Compile the recompiled runtime + harness into an executable under d."""
    d = Path(d)
    code, source, exe = d/'stub.code', d/'stub.c', d/'test'
    code.write_bytes(bytes.fromhex('1eff2fe1'))
    subprocess.run([os.environ['DQ8_TOOL'],'recompile-arm',str(code),'0x00100000',str(source)],
                   check=True,capture_output=True)
    runtime = source.read_text().replace('int main(int argc, char **argv)',
                                         'int guest_main(int argc, char **argv)')
    source.write_text(runtime+HARNESS_C)
    subprocess.run(['clang','-std=c11','-O0','-w',str(source),'-lm','-o',str(exe)],check=True)
    return exe

def run_harness(exe, userdata=None, cwd=None):
    env = dict(os.environ)
    if userdata is None: env.pop('DQ8_USER_DATA_DIR',None)
    else: env['DQ8_USER_DATA_DIR'] = str(userdata)
    return subprocess.run([str(exe)],capture_output=True,timeout=30,env=env,cwd=cwd)

class NativeSaveFsTest(unittest.TestCase):
    def test_absent_create_write_reopen_eof_and_directory(self):
        with tempfile.TemporaryDirectory(prefix='dq8-save-fs-') as d:
            d=Path(d);exe=build_harness(d)
            env={**os.environ,'DQ8_USER_DATA_DIR':str(d/'userdata')}
            subprocess.run([str(exe)],check=True,timeout=5,env=env,capture_output=True)
            saved=list((d/'userdata').glob('*/honeySave.bin'))
            self.assertEqual(len(saved),1)
            self.assertEqual(saved[0].read_bytes(),b'\x00'*4+b'DQ8 save'+b'\x00'*4)

    def test_user_data_dir_creates_missing_parents(self):
        """A DQ8_USER_DATA_DIR whose parents do not exist must be created."""
        with tempfile.TemporaryDirectory(prefix='dq8-save-nested-') as d:
            d=Path(d);exe=build_harness(d)
            userdata=d/'a'/'b'/'userdata'
            self.assertFalse(userdata.exists())
            run=run_harness(exe,userdata=userdata)
            self.assertEqual(run.returncode,0,run.stderr[-2000:].decode(errors='replace'))
            self.assertEqual(list(userdata.glob('*/honeySave.bin')).__len__(),1)
            self.assertNotIn(b'warning',run.stderr)   # nothing to warn about on success

    def test_default_target_userdata_created_from_fresh_cwd(self):
        """Without DQ8_USER_DATA_DIR the runner creates <cwd>/target/userdata."""
        with tempfile.TemporaryDirectory(prefix='dq8-save-default-') as d:
            d=Path(d);exe=build_harness(d);work=d/'work';work.mkdir()
            self.assertFalse((work/'target').exists())
            run=run_harness(exe,userdata=None,cwd=work)
            self.assertEqual(run.returncode,0,run.stderr[-2000:].decode(errors='replace'))
            self.assertTrue((work/'target'/'userdata').is_dir())
            self.assertEqual(len(list((work/'target'/'userdata').glob('*/honeySave.bin'))),1)

    def test_warns_when_user_data_dir_cannot_be_created(self):
        """Uncreatable root (a regular file in the path) warns and fails loudly."""
        with tempfile.TemporaryDirectory(prefix='dq8-save-blocked-') as d:
            d=Path(d);exe=build_harness(d)
            blocked=d/'afile';blocked.write_bytes(b'not a directory')
            run=run_harness(exe,userdata=blocked/'userdata')
            self.assertNotEqual(run.returncode,0)
            self.assertIn(b'warning: cannot create user-data directory',run.stderr)
            self.assertIn(b'Not a directory',run.stderr)
            self.assertFalse((blocked/'userdata').exists())

if __name__=='__main__':unittest.main()
