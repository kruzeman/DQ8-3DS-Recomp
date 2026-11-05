#!/usr/bin/env python3
"""tools/present_kernel_test.py -- build and run the composed-present kernel
pixel-identity test (t_5286064e).

The kernel is extracted VERBATIM from src/main.rs's runtime template (the
region that holds the destination-kind enum, the DQ8_PRESENT_SCALER knob and
the SSE2/scalar 2x scaler), so the test validates the code the runtime actually
ships instead of a copy that can drift.

usage: tools/present_kernel_test.py
exit: 0 = every case byte-identical to SDL's FillRect + 2x BlitScaled path in
      both kernel modes (SSE2 and DQ8_PRESENT_SCALER=2 scalar).
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, 'src', 'main.rs')
HARNESS = os.path.join(HERE, 'present_kernel_verify.c')

START = 'enum { HP_KIND_RGBA = 0'
END = '/* A presentation target: one pixel image plus the rectangle each screen'


def extract():
    text = open(SRC, encoding='utf-8').read()
    i = text.find(START)
    j = text.find(END, i)
    if i < 0 or j < 0 or j <= i:
        sys.exit('kernel region not found in src/main.rs (markers moved?)')
    return text[i:j]


def main():
    outdir = os.path.join(ROOT, 'target', 'present-kernel-test')
    os.makedirs(outdir, exist_ok=True)
    body = extract()
    if '#if defined(__SSE2__)' in body and 'emmintrin' not in body:
        body = '#include <emmintrin.h>\n' + body
    kpath = os.path.join(outdir, 'present_kernel_extracted.c')
    with open(kpath, 'w', encoding='utf-8') as f:
        f.write('/* extracted from src/main.rs by tools/present_kernel_test.py */\n')
        f.write(body)
    sdl = subprocess.run(['sdl2-config', '--cflags', '--libs'],
                         capture_output=True, text=True, check=True).stdout.split()
    exe = os.path.join(outdir, 'present_kernel_test')
    cmd = ['cc', '-O2', '-Wall', '-Wno-unused-function', '-I', outdir,
           '-o', exe, HARNESS] + sdl
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.stdout:
        sys.stdout.write(r.stdout)
    if r.stderr:
        sys.stderr.write(r.stderr)
    if r.returncode != 0:
        sys.exit('compile failed')

    fails = 0
    for mode in ('sse2', 'scalar'):
        env = dict(os.environ, SDL_VIDEODRIVER='dummy')
        env.pop('DQ8_PRESENT_SCALER', None)
        if mode == 'scalar':
            env['DQ8_PRESENT_SCALER'] = '2'
        print('== kernel mode: %s' % mode)
        rc = subprocess.run([exe, 'require-dummy'], env=env).returncode
        fails += (rc != 0)
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
