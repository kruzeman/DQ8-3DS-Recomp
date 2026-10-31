#!/usr/bin/env python3
"""A09 regression: a buffered save WRITE must not report success when the
underlying flush fails (ENOSPC / I-O), and must not report a partial `bytes`
count on failure (azahar's File::Write pushes 0 written on failure).

Extracts the file-handle dispatch branch straight out of src/native_fs.c so the
suite always exercises the CURRENT runtime code, then drives it against a
controllable fopencookie sink (failure injection) and a real file (persistence).

Scenarios:
  A  ENOSPC on flush      -> error != ResultSuccess, bytes == 0 (the A09 defect)
  B  short fwrite         -> error, bytes == 0
  C  fseeko failure       -> error
  D  transient fail + retry -> first attempt honest-fails, retry succeeds
  E  happy path           -> write/flush/close succeed, data persists on reopen
"""
from pathlib import Path
import subprocess, tempfile, os, unittest

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / 'src' / 'native_fs.c'

def _extract_branch(sl):
    start = next(i for i, l in enumerate(sl) if 'if(handle<256u && native_files[handle]) {' in l)
    depth = 0
    for j in range(start, len(sl)):
        depth += sl[j].count('{') - sl[j].count('}')
        if depth == 0 and j > start:
            return start, j
    raise RuntimeError('unbalanced dispatch branch')

HARNESS = r'''
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>
static uint8_t ram[4096]; static uint32_t active_tls_base; static FILE *native_files[256];
static uint32_t mem32(uint32_t p){uint32_t v;memcpy(&v,ram+p,4);return v;}
static void put32(uint32_t p,uint32_t v){memcpy(ram+p,&v,4);}
static int guest_memory_range(uint32_t p,uint64_t n,uint32_t *mapped){*mapped=p;return p+n<=sizeof(ram);}
static uint8_t guest_file_is_romfs[256]; static uint32_t guest_alloc_ipc_handle(void){return 33;}
__HEADER__
static int tested_dispatch(uint32_t handle,uint32_t command){uint32_t tls=active_tls_base;
__BRANCH__
return 0;}
struct sink { uint8_t buf[128]; size_t len; off64_t pos; int fail_writes; int short_writes; int fail_seek; };
static ssize_t sink_write(void *cookie,const char *buf,size_t n){
    struct sink *s=cookie;
    if(s->fail_writes){ s->fail_writes--; errno=ENOSPC; return -1; }
    if(s->short_writes){ s->short_writes--; size_t k=n>2?2:n; memcpy(s->buf+s->pos,buf,k); s->pos+=k; if((size_t)s->pos>s->len)s->len=(size_t)s->pos; return (ssize_t)k; }
    if((size_t)s->pos+n>sizeof(s->buf)){ errno=ENOSPC; return -1; }
    memcpy(s->buf+s->pos,buf,n); s->pos+=n; if((size_t)s->pos>s->len)s->len=(size_t)s->pos; return (ssize_t)n;
}
static int sink_seek(void *cookie,off64_t *offset,int whence){
    struct sink *s=cookie; if(s->fail_seek){ errno=EIO; return -1; }
    off64_t base=(whence==SEEK_SET)?0:((whence==SEEK_CUR)?s->pos:((whence==SEEK_END)?(off64_t)s->len:(off64_t)LLONG_MIN));
    if(base==(off64_t)LLONG_MIN){ errno=EINVAL; return -1; }
    off64_t t=base+*offset; if(t<0){ errno=EINVAL; return -1; }
    s->pos=t; *offset=t; return 0;
}
static void do_write(uint32_t h,const char *data,uint32_t n){
    put32(0x84,0);put32(0x88,0);put32(0x8C,n);put32(0x94,0);put32(0x98,2048);
    memcpy(ram+2048,data,n); tested_dispatch(h,0x08030102u);
}
int main(void){
    memset(ram,0,sizeof(ram)); active_tls_base=0; int fails=0;
    /* A: ENOSPC on flush */
    { struct sink s={0}; s.fail_writes=1;
      cookie_io_functions_t f={.write=sink_write,.seek=sink_seek};
      FILE *st=fopencookie(&s,"w+",f); char bb[BUFSIZ]; setvbuf(st,bb,_IOFBF,sizeof(bb)); native_files[32]=st;
      do_write(32,"SAVE",4);
      uint32_t res=mem32(0x84), by=mem32(0x88);
      printf("A ENOSPC-flush result=%08X bytes=%u\n",res,by);
      if(!(res!=0 && by==0))fails++; fclose(st); native_files[32]=NULL; }
    /* B: short fwrite (unbuffered) */
    { struct sink s={0}; s.short_writes=1;
      cookie_io_functions_t f={.write=sink_write,.seek=sink_seek};
      FILE *st=fopencookie(&s,"w+",f); setvbuf(st,NULL,_IONBF,0); native_files[33]=st;
      do_write(33,"SAVE",4);
      uint32_t res=mem32(0x84), by=mem32(0x88);
      printf("B short-fwrite result=%08X bytes=%u\n",res,by);
      if(!(res!=0 && by==0))fails++; fclose(st); native_files[33]=NULL; }
    /* C: fseeko failure */
    { struct sink s={0}; s.fail_seek=1;
      cookie_io_functions_t f={.write=sink_write,.seek=sink_seek};
      FILE *st=fopencookie(&s,"w+",f); native_files[34]=st;
      do_write(34,"SAVE",4);
      uint32_t res=mem32(0x84);
      printf("C fseeko-fail result=%08X\n",res);
      if(!(res!=0))fails++; fclose(st); native_files[34]=NULL; }
    /* D: transient fail then successful retry */
    { struct sink s={0}; s.fail_writes=1;
      cookie_io_functions_t f={.write=sink_write,.seek=sink_seek};
      FILE *st=fopencookie(&s,"w+",f); char bb[BUFSIZ]; setvbuf(st,bb,_IOFBF,sizeof(bb)); native_files[35]=st;
      do_write(35,"SAVE",4); uint32_t r1=mem32(0x84),b1=mem32(0x88);
      clearerr(st); do_write(35,"SAVE",4); uint32_t r2=mem32(0x84),b2=mem32(0x88);
      printf("D retry first=%08X/%u retry=%08X/%u\n",r1,b1,r2,b2);
      if(!(r1!=0 && b1==0 && r2==0 && b2==4))fails++; fclose(st); native_files[35]=NULL; }
    /* E: happy-path persistence on a real file */
    { char path[]="/tmp/dq8_a09u_XXXXXX"; int fd=mkstemp(path); if(fd>=0)close(fd);
      FILE *st=fopen(path,"w+b"); native_files[36]=st;
      do_write(36,"HONEY",5); uint32_t wr=mem32(0x84),wb=mem32(0x88);
      put32(0x84,0);tested_dispatch(36,0x08090000u);uint32_t fr=mem32(0x84);
      put32(0x84,0);tested_dispatch(36,0x08080000u);uint32_t cr=mem32(0x84);
      FILE *ck=fopen(path,"rb"); char got[8]={0}; size_t n=fread(got,1,5,ck); fclose(ck);
      printf("E happy write=%08X/%u flush=%08X close=%08X readback=%zu\n",wr,wb,fr,cr,n);
      if(!(wr==0&&wb==5&&fr==0&&cr==0&&n==5&&!memcmp(got,"HONEY",5)))fails++;
      native_files[36]=NULL; unlink(path); }
    printf("%s fails=%d\n", fails?"FAIL":"PASS", fails);
    return fails?1:0;
}
'''

def build_and_run(workdir):
    sl = SRC.read_text().splitlines(True)
    header = ''.join(sl[7:16])  # native_fs_error + native_fs_reply (identical pre/post)
    bs, be = _extract_branch(sl)
    branch = ''.join(sl[bs:be+1])
    src = HARNESS.replace('__HEADER__', header).replace('__BRANCH__', branch)
    c = workdir / 'a09.c'; exe = workdir / 'a09'
    c.write_text(src)
    b = subprocess.run(['cc', '-std=c11', '-O0', '-w', str(c), '-o', str(exe)],
                       capture_output=True, text=True)
    if b.returncode != 0:
        return None, 'compile failed:\n' + b.stderr
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    return r, None

class BufferedWriteFlushTest(unittest.TestCase):
    def test_flush_failure_not_reported_as_success(self):
        with tempfile.TemporaryDirectory(prefix='dq8-a09-') as d:
            r, err = build_and_run(Path(d))
            self.assertIsNone(err, err)
            self.assertEqual(r.returncode, 0,
                             'flush/short-write failure leaked as false success or partial bytes:\n' + r.stdout)
            self.assertIn('PASS', r.stdout)

if __name__ == '__main__':
    unittest.main()
