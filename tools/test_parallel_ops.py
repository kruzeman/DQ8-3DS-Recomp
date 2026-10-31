#!/usr/bin/env python3
# Unit-test par_op/par_sel exactly as shipped: the helper text is extracted
# from the generated runtime (target/dq8-split/dq8_main.c), compiled with a
# vector table derived from the ARM ARM pseudocode (ddi0597 UASX/SAX and the
# parallel-arithmetic family), and run.
import subprocess
import sys
import tempfile

SRC = "/home/hermes/DQ8/target/dq8-split/dq8_main.c"

with open(SRC) as fh:
    text = fh.read()

start = text.index("uint32_t par_op(")
end = text.index("/* SEL: per byte lane")
sel_end = text.index("\n}\n", text.index("uint32_t par_sel(")) + 3
helpers = text[start:sel_end]

CASES = [
    # (label, a, b, bits, op, mode, ge_in, expected_result, expected_ge)
    ("UHADD8", 0xFF010203, 0x01010101, 8, 0, 4, 5, 0x80010102, 5),
    ("UADD8", 0xFFFFFFFF, 0x01010101, 8, 0, 0, 0, 0x00000000, 0xF),
    ("USUB8", 0x00010203, 0x01010101, 8, 1, 0, 0, 0xFF000102, 0x7),
    ("UQADD8", 0xFF800000, 0x02010000, 8, 0, 2, 0, 0xFF810000, 0),
    ("UQSUB8", 0x05040302, 0x01010101, 8, 1, 2, 0, 0x04030201, 0),
    ("UQSUB8_sat", 0x01020304, 0x05050505, 8, 1, 2, 0, 0x00000000, 0),
    ("QADD8", 0x7F7F7F7F, 0x01010101, 8, 0, 1, 0, 0x7F7F7F7F, 0),
    ("QADD8_neg", 0x80808080, 0xFFFFFFFF, 8, 0, 1, 0, 0x80808080, 0),
    ("QSUB8", 0x80808080, 0x7F7F7F7F, 8, 1, 1, 0, 0x80808080, 0),
    ("SHADD8", 0x80808080, 0x00000000, 8, 0, 3, 0, 0xC0C0C0C0, 0),
    ("SHADD8_zero", 0xFF000000, 0x01000000, 8, 0, 3, 0, 0x00000000, 0),
    ("UHSUB8", 0xFF000000, 0x01000000, 8, 1, 4, 0, 0x7F000000, 0),
    ("UQADD16", 0xFFFF0000, 0x00020000, 16, 0, 2, 0, 0xFFFF0000, 0),
    ("USUB16", 0x00010000, 0x00020001, 16, 1, 0, 0, 0xFFFFFFFF, 0),
    ("UADD16_ge", 0x0001FFFF, 0x00010001, 16, 0, 0, 0, 0x00020000, 0x3),
    # ARM ARM UASX: Rd[15:0] = Rn_lo - Rm_hi, Rd[31:16] = Rn_hi + Rm_lo.
    #   Rn=0x00030005 (hi=3, lo=5), Rm=0x00010002 (hi=1, lo=2)
    #   -> lo = 5-1 = 4, hi = 3+2 = 5, GE[1:0]=3 (diff>=0), GE[3:2]=0.
    ("UASX", 0x00030005, 0x00010002, 16, 2, 0, 0, 0x00050004, 0x3),
    # ARM ARM USAX: Rd[15:0] = Rn_lo + Rm_hi, Rd[31:16] = Rn_hi - Rm_lo.
    #   -> lo = 5+1 = 6, hi = 3-2 = 1, GE[1:0]=0, GE[3:2]=3 (diff>=0).
    ("USAX", 0x00030005, 0x00010002, 16, 3, 0, 0, 0x00010006, 0xC),
]

body = []
for label, a, b, bits, op, mode, ge_in, want, want_ge in CASES:
    body.append(
        '    run("%s", 0x%08Xu, 0x%08Xu, %du, %du, %du, %d, 0x%08Xu, %d);'
        % (label, a, b, bits, op, mode, ge_in, want, want_ge)
    )

program = r"""
#include <stdint.h>
#include <stdio.h>
%s
static int failures;
static void run(const char *label, uint32_t a, uint32_t b, unsigned bits,
                unsigned op, unsigned mode, int ge_in,
                uint32_t want, int want_ge) {
    int ge = ge_in;
    uint32_t got = par_op(a, b, bits, op, mode, &ge);
    if (got != want || (mode == 0u && ge != want_ge)) {
        printf("FAIL %%s: got=%%08X ge=%%d want=%%08X ge=%%d\n",
               label, got, ge, want, want_ge);
        failures++;
    } else {
        printf("ok   %%s\n", label);
    }
}
int main(void) {
%s
    /* SEL: byte lane i takes Rn when GE[i] is set.  Rn=0x04030201 so lane i
       holds 0x01,0x02,0x03,0x04; Rm=0x14131211 holds 0x11,0x12,0x13,0x14. */
    if (par_sel(0x04030201u, 0x14131211u, 0x5) != 0x14031201u) {
        printf("FAIL SEL ge=0x5: got=%%08X\n", par_sel(0x04030201u, 0x14131211u, 0x5)); failures++;
    } else { printf("ok   SEL\n"); }
    if (par_sel(0x04030201u, 0x14131211u, 0xA) != 0x04130211u) {
        printf("FAIL SEL ge=0xA: got=%%08X\n", par_sel(0x04030201u, 0x14131211u, 0xA)); failures++;
    } else { printf("ok   SEL inv\n"); }
    if (par_sel(0x04030201u, 0x14131211u, 0xF) != 0x04030201u) {
        printf("FAIL SEL ge=0xF\n"); failures++;
    } else { printf("ok   SEL all rn\n"); }
    if (par_sel(0x04030201u, 0x14131211u, 0x0) != 0x14131211u) {
        printf("FAIL SEL ge=0x0\n"); failures++;
    } else { printf("ok   SEL all rm\n"); }
    printf(failures ? "%%d FAILURES\n" : "ALL PASS\n", failures);
    return failures != 0;
}
""" % (helpers, "\n".join(body))

src = tempfile.mktemp(suffix=".c", dir="/tmp/dq8-sess")
exe = src + ".bin"
with open(src, "w") as fh:
    fh.write(program)
subprocess.run(["clang", "-std=c11", "-O0", "-w", src, "-o", exe], check=True)
proc = subprocess.run([exe], capture_output=True, text=True)
sys.stdout.write(proc.stdout)
sys.exit(proc.returncode)
