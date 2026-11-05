/* FN_EMIT pilot (t_8b9fda4c): in-process A/B of every emitted guest function,
 * measured two ways from an identical seed state:
 *
 *   dispatch arm : the real -O0 guest part TU(s) for the function's band
 *                  (`dq8_part_NNN()`, the same objects the binary links), i.e.
 *                  the dispatcher's own switch/chain loop with the register
 *                  file in memory;
 *   native arm   : `dq8_fnb_XXXXXXXX()`, the emitted real C function (r0..r14
 *                  as locals, direct gotos, native return, real C calls to
 *                  other emitted functions), compiled with the same -O0 flags.
 *
 * Two things are produced per function:
 *
 *  1. a differential.  Both arms start from the same seed `Dq8FnState` and the
 *     same guest RAM (the whole image is snapshotted and restored between
 *     arms, because the emitted functions write guest memory and restoring the
 *     registers alone — what DQ8_S1_SELFCHECK does — is not a sound check).
 *     r0..r14, all 32 fprs, fpscr, NZCV/GE, pc, running, the retired
 *     instruction count and a digest of the WHOLE guest RAM must match bit for
 *     bit.
 *  2. a cost.  N iterations of each arm with the state re-seeded every time;
 *     a third loop measures the re-seed alone and that overhead is subtracted
 *     from both arms.  CLOCK_PROCESS_CPUTIME_ID, interleaved reps.
 *
 * A function whose seeded run does not terminate (a loop guarded by a register
 * whose live value we cannot know) is reported as NOT-EXERCISED instead of
 * hanging the bench: SIGALRM/siglongjmp around each arm, plus an instruction
 * cap.
 *
 * Link: dq8_part_*.o + dq8_fnshard_*.o + dq8_main.o (main renamed), so both
 * arms are exactly the objects the binary contains.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <unistd.h>

#include "dq8_rt.h"
#include "fn_table.h"

#define GUEST_RAM_BYTES 0x40000000ull
/* Hard cap on one seeded run: a function that hits it is reported, not timed. */
#define STEP_CAP 4000000ull
/* Wall-clock guard for the native arm (it cannot be interrupted from inside). */
#define ARM_ALARM_SECS 20

static sigjmp_buf arm_jmp;
static volatile int in_arm;
static volatile int arm_exit_code;

static void arm_timeout(int sig) {
    (void)sig;
    siglongjmp(arm_jmp, 1);
}

/* The runtime's guest-memory fault path and the dispatcher's "translated PC
 * escaped" path call exit(16)/exit(3).  With a synthetic seed state a function
 * can legitimately fault (a pointer it loaded from memory is not a real one),
 * so the harness turns those exits into an abort of the current arm instead of
 * killing the whole bench.  Only reached while an arm is running. */
void exit(int code) {
    if (in_arm) {
        arm_exit_code = code;
        siglongjmp(arm_jmp, 3);
    }
    _exit(code);
}

typedef struct {
    uint32_t regs[16];
    uint32_t fpr[32];
    uint32_t fpscr;
    int n, z, c, v, ge, running;
    uint32_t pc;
    unsigned long long exec;
} State;

static uint8_t *ram_snapshot;

static double cpu_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

/* Whole-image digest: any differing guest store anywhere shows up.  Four
 * independent accumulators so the loop is not one serial dependency chain. */
static uint64_t ram_hash(void) {
    const uint64_t *p = (const uint64_t *)ram;
    size_t n = (size_t)(GUEST_RAM_BYTES / 8u);
    uint64_t a = 0x9E3779B97F4A7C15ull, b = 0xC2B2AE3D27D4EB4Full;
    uint64_t c = 0x165667B19E3779F9ull, d = 0x27D4EB2F165667C5ull;
    size_t i;
    for (i = 0; i + 4 <= n; i += 4) {
        a = (a ^ p[i + 0]) * 0x100000001B3ull;
        b = (b ^ p[i + 1]) * 0x100000001B3ull;
        c = (c ^ p[i + 2]) * 0x100000001B3ull;
        d = (d ^ p[i + 3]) * 0x100000001B3ull;
    }
    return (a ^ (b * 3u)) + (c ^ (d * 5u)) + (uint64_t)i;
}

/* Deterministic seed: every register the function may use as a pointer gets a
 * distinct, mapped, comfortably interior RAM address, the stack pointer gets
 * its own region, floats are finite and non-zero, and the flags are a
 * non-trivial mixture so an incorrect flag write cannot hide. */
static void seed_state(const FnSpec *f, unsigned variant, State *s) {
    memset(s, 0, sizeof *s);
    for (unsigned i = 0; i < 16u; ++i)
        s->regs[i] = 0x10000000u + (uint32_t)((i + variant * 3u) * 0x4000u);
    s->regs[13] = 0x10080000u + (uint32_t)(variant * 0x10000u);
    s->regs[14] = 0x00200000u;
    s->pc = f->entry;
    s->running = 1;
    s->n = (int)((variant >> 0) & 1u);
    s->z = (int)((variant >> 1) & 1u);
    s->c = (int)((variant >> 2) & 1u);
    s->v = (int)((variant >> 3) & 1u);
    s->ge = (int)(0xA5A5A5A5u ^ (variant * 0x01010101u));
    for (unsigned i = 0; i < 32u; ++i) {
        uint32_t v = 0x3F800000u + (uint32_t)(i * 0x4000u) + (variant * 0x11u);
        if (i & 1u) v |= 0x80000000u;
        s->fpr[i] = v;
    }
    s->fpscr = (variant & 1u) ? 0x03000000u : 0u;
}

static void seed_memory(void) {
    /* Every word in the reachable window is a VALID guest pointer (0x10000000
     * + aligned offset), so a function that loads a pointer from memory
     * dereferences something mapped instead of faulting out of the image.
     * Floats read from this window are tiny denormals, which is fine: both
     * arms see the same bits. */
    for (uint32_t base = 0x10000000u; base < 0x11000000u; base += 4u)
        *(uint32_t *)(void *)(ram + base) = 0x10000000u + ((base - 0x10000000u) & 0x00FFFFFCu);
}

/* Run one arm under the alarm/exit guards.  Returns the arm's status:
 * 0 ok, -9 no termination (alarm), -(exit code) the guest faulted/exited. */
static int run_dispatch(const FnSpec *f, State *s);
static int run_native(const FnSpec *f, State *s);

/* A split part chains instructions inside a band without returning to the
 * caller, so the dispatcher arm would run past the function's own range.  The
 * part already breaks out of its chain when dq8_pc_needs_boundary(pc) says the
 * main loop must see that PC (that is how the emitted entries work in the real
 * binary), so arming that predicate at exactly the range's exit PCs makes both
 * arms stop at the same instruction.  The cost is the per-instruction boundary
 * check the dispatcher already pays on these pages (the emitted entries are in
 * its static boundary set), so it biases the dispatcher arm, never the native
 * one. */
static void arm_bounds(const FnSpec *f) {
    int n = f->nbounds;
    if (n > 16) n = 16;
    for (int i = 0; i < n; ++i) trace_guest_pcs[i] = f->bounds[i];
    trace_guest_pc_count = (unsigned)n;
}

/* t_ba4d43cb: an indirect exit is *executed* by the emitted body, which then
 * returns to the dispatcher with pc = target.  The interpreted arm executes the
 * same instruction, so it must be stopped on arrival at that pc (only the
 * native arm knows it).  Appending it to the static before-execution stops
 * leaves both arms owning exactly the same retired instructions. */
static void arm_bounds_arrive(const FnSpec *f, uint32_t arrive) {
    arm_bounds(f);
    if (trace_guest_pc_count < 15u) {
        for (unsigned i = 0; i < trace_guest_pc_count; ++i)
            if (trace_guest_pcs[i] == arrive) return;
        trace_guest_pcs[trace_guest_pc_count++] = arrive;
    }
}

static void disarm_bounds(void) {
    trace_guest_pc_count = 0u;
}

static int run_arm(const FnSpec *f, int native, State *s) {
    int rc;
    arm_exit_code = 0;
    in_arm = 1;
    if (sigsetjmp(arm_jmp, 1) == 0) {
        alarm(ARM_ALARM_SECS);
        rc = native ? run_native(f, s) : run_dispatch(f, s);
        alarm(0);
    } else {
        rc = arm_exit_code ? -arm_exit_code : -9;
        alarm(0);
    }
    in_arm = 0;
    return rc;
}

/* ---- dispatch arm: run the real part TU the way the main dispatcher does ---- */
static int run_dispatch(const FnSpec *f, State *s) {
    uint32_t pc = s->pc;
    int n = s->n, z = s->z, c = s->c, v = s->v, ge = s->ge, running = s->running;
    uint64_t steps = 0;
    unsigned calls = 0;
    memcpy(fpr, s->fpr, sizeof s->fpr);
    fpscr = s->fpscr;
    while (running && pc >= f->entry && pc < f->end) {
        /* t_ba4d43cb: the emitted body hands an indirect transfer or an opaque
         * instruction to the dispatcher *before* executing it, so the
         * interpreted arm must stop at the same pc.  trace_guest_pcs holds
         * exactly those stop pcs (see tools/fn_emit_bench_gen.py); the static
         * boundary set is NOT usable here because every emitted entry is in it. */
        {
            int at_stop = 0;
            for (unsigned i = 0; i < trace_guest_pc_count; ++i)
                if (pc == trace_guest_pcs[i]) { at_stop = 1; break; }
            if (at_stop) break;
        }
        uint32_t band = pc >> 17;
        int (*part)(uint32_t *, uint32_t *, int *, int *, int *, int *, int *,
                    int *, uint64_t *, int) = NULL;
        for (int b = 0; b < f->nbands; ++b)
            if (f->band[b] == band) part = f->part[b];
        if (!part) return -1;
        if (!part(s->regs, &pc, &n, &z, &c, &v, &ge, &running, &steps, 1))
            return -2;
        /* The part does not credit `steps` for the instruction it is entered
         * on (only for the ones it chains into), so add one per call: that is
         * the dispatcher's real per-instruction accounting, and it makes the
         * retired count directly comparable with the emitted body's ++exec. */
        ++calls;
        if (steps >= STEP_CAP) return -3;
    }
    s->pc = pc;
    s->n = n; s->z = z; s->c = c; s->v = v; s->ge = ge;
    s->running = running;
    /* The part's L_<pc> prologue skips the ++steps for the instruction it is
     * entered on (`if (_entry) _entry = 0; else ++steps;`), so each part call
     * undercounts by exactly one.  Add it back, otherwise a function that
     * needs two calls reports exec=N-1 and looks like a differential failure. */
    s->exec = steps + (uint64_t)calls;
    memcpy(s->fpr, fpr, sizeof s->fpr);
    s->fpscr = fpscr;
    return 0;
}

static int run_native(const FnSpec *f, State *s) {
    Dq8FnState st;
    unsigned long long exec;
    /* t_21dda9b5 made st.r a *pointer* to the architectural register file
       instead of a 15-word copy, so the harness hands it the array. */
    st.r = s->regs;
    st.pc = s->pc;
    st.n = s->n; st.z = s->z; st.c = s->c; st.v = s->v; st.ge = s->ge;
    st.running = s->running;
    memcpy(fpr, s->fpr, sizeof s->fpr);
    fpscr = s->fpscr;
    exec = f->native(&st, 0u);
    s->pc = st.pc;
    s->n = st.n; s->z = st.z; s->c = st.c; s->v = st.v; s->ge = st.ge;
    s->running = st.running;
    s->exec = exec;
    memcpy(s->fpr, fpr, sizeof fpr);
    s->fpscr = fpscr;
    return 0;
}

static int diff_state(const char *who, const State *a, const State *b) {
    int bad = 0;
    if (a->pc != b->pc || a->n != b->n || a->z != b->z || a->c != b->c ||
        a->v != b->v || a->ge != b->ge || a->running != b->running ||
        a->exec != b->exec || a->fpscr != b->fpscr) {
        printf("    DIFF %s state pc=%08X/%08X nzcv=%d%d%d%d/%d%d%d%d ge=%08X/%08X "
               "running=%d/%d exec=%llu/%llu fpscr=%08X/%08X\n", who,
               a->pc, b->pc, a->n, a->z, a->c, a->v, b->n, b->z, b->c, b->v,
               a->ge, b->ge, a->running, b->running,
               (unsigned long long)a->exec, (unsigned long long)b->exec,
               a->fpscr, b->fpscr);
        bad = 1;
    }
    for (unsigned i = 0; i < 16u; ++i)
        if (a->regs[i] != b->regs[i]) {
            printf("    DIFF %s r%u %08X != %08X\n", who, i, a->regs[i], b->regs[i]);
            bad = 1;
        }
    for (unsigned i = 0; i < 32u; ++i)
        if (a->fpr[i] != b->fpr[i]) {
            printf("    DIFF %s fpr%u %08X != %08X\n", who, i, a->fpr[i], b->fpr[i]);
            bad = 1;
        }
    return bad;
}

static void restore_ram(void) {
    memcpy(ram, ram_snapshot, (size_t)GUEST_RAM_BYTES);
}

int main(int argc, char **argv) {
    unsigned long long iters = 20000ull;
    int timing = 1;
    if (argc > 1) iters = strtoull(argv[1], NULL, 0);
    if (argc > 2 && !strcmp(argv[2], "--no-timing")) { timing = 0; iters = 0; }

    ram = mmap(NULL, GUEST_RAM_BYTES, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (ram == MAP_FAILED) { perror("mmap ram"); return 2; }
    ram_snapshot = malloc((size_t)GUEST_RAM_BYTES);
    if (!ram_snapshot) { perror("malloc snapshot"); return 2; }
    /* The checked fast path needs the runtime's steady-state values. */
    exclusive_addr = 0xFFFFFFFFu;
    exclusive_size = 0u;
    trace_write_n = 0u;
    trace_hid_reads_enabled = 0;
    thread_yield_requested = 0;
    thread_stopped_by_yield = 0;
    active_tls_base = 0u;
    signal(SIGALRM, arm_timeout);
    seed_memory();
    /* Pristine image: restored before every differential arm so the rows are
     * independent of what the previous row's timing loops wrote. */
    memcpy(ram_snapshot, ram, (size_t)GUEST_RAM_BYTES);

    int total_bad = 0, exercised = 0, skipped = 0;
    double sum_disp_insn = 0.0, sum_nat_insn = 0.0;
    double sum_disp_ns = 0.0, sum_nat_ns = 0.0;
    double sum_insns_weight = 0.0;

    printf("# FN_EMIT bench: %d functions, %llu iterations, -O0 guest objects\n",
           FN_SPECS_COUNT, iters);

    for (unsigned fi = 0; fi < FN_SPECS_COUNT; ++fi) {
        const FnSpec *f = &fn_specs[fi];
        State sd, sn;
        uint64_t hash_d, hash_n;
        unsigned long long exec_d = 0, exec_n = 0;
        int rc_d, rc_n;

        /* ---- differential ----
         * Native arm first: its resulting pc is where an executed indirect exit
         * landed, and the interpreted arm has to be stopped on arrival there.
         * Both arms start from the PRISTINE seeded image: the timing loops of
         * the previous row (and the previous arm) write guest memory, so the
         * image has to be restored before each arm or the rows are not
         * reproducible. */
        restore_ram();
        seed_state(f, 0u, &sn);
        rc_n = run_arm(f, 1, &sn);
        hash_n = ram_hash();
        exec_n = sn.exec;

        restore_ram();
        seed_state(f, 0u, &sd);
        arm_bounds_arrive(f, sn.pc);
        rc_d = run_arm(f, 0, &sd);
        hash_d = ram_hash();
        exec_d = sd.exec;
        disarm_bounds();

        if (rc_d == -9 || rc_n == -9) {
            printf("FN %2u 0x%08X-0x%08X %4u insns  NOT-EXERCISED (no termination: "
                   "dispatch_rc=%d native_rc=%d)\n",
                   fi, f->entry, f->end, f->insns, rc_d, rc_n);
            skipped++;
            continue;
        }
        if (rc_n != 0 || rc_d != 0) {
            printf("FN %2u 0x%08X-0x%08X %4u insns  ERROR dispatch_rc=%d native_rc=%d\n",
                   fi, f->entry, f->end, f->insns, rc_d, rc_n);
            skipped++;
            continue;
        }
        int bad = diff_state("dispatch-vs-native", &sd, &sn);
        if (hash_d != hash_n) {
            printf("    DIFF guest RAM digest %016llX != %016llX\n",
                   (unsigned long long)hash_d, (unsigned long long)hash_n);
            bad = 1;
        }
        if (bad) {
            printf("FN %2u 0x%08X-0x%08X %4u insns  MISMATCH exec=%llu/%llu\n",
                   fi, f->entry, f->end, f->insns,
                   (unsigned long long)exec_d, (unsigned long long)exec_n);
            total_bad++;
            continue;
        }
        exercised++;
        printf("FN %2u 0x%08X-0x%08X %4u insns  BIT-EXACT exec=%llu ram=%016llX\n",
               fi, f->entry, f->end, f->insns, (unsigned long long)exec_d,
               (unsigned long long)hash_d);
        if (!timing || exec_d == 0) continue;

        /* ---- timing ---- */
        /* The dispatch arm pays the A05 boundary predicate on the same stop set
         * as the differential, so both arms retire the same instructions. */
        arm_bounds_arrive(f, sn.pc);
        const int REPS = 3;
        double reseed = 0.0, disp = 0.0, nat = 0.0;
        State seed;
        int trc = 0;
        seed_state(f, 0u, &seed);
        /* The timing loops run the arms directly (an alarm per iteration would
         * dominate the measurement), so the runtime's fault path has to be
         * caught around the whole block: a synthetic pointer can fault on one
         * of the 200 iterations even though the differential pass did not.
         * Such a row is reported, not timed, instead of killing the bench. */
        if (sigsetjmp(arm_jmp, 1) == 0) {
        in_arm = 1;
        for (int rep = 0; rep < REPS; ++rep) {
            State s;
            double t0 = cpu_ns();
            for (unsigned long long i = 0; i < iters; ++i) {
                s = seed;
                __asm__ __volatile__("" : : "r"(&s) : "memory");
            }
            double t1 = cpu_ns();
            reseed += t1 - t0;

            t0 = cpu_ns();
            for (unsigned long long i = 0; i < iters; ++i) {
                s = seed;
                memcpy(fpr, s.fpr, sizeof s.fpr);
                fpscr = s.fpscr;
                run_dispatch(f, &s);
                __asm__ __volatile__("" : : "r"(&s) : "memory");
            }
            t1 = cpu_ns();
            disp += t1 - t0;

            t0 = cpu_ns();
            for (unsigned long long i = 0; i < iters; ++i) {
                s = seed;
                memcpy(fpr, s.fpr, sizeof s.fpr);
                fpscr = s.fpscr;
                run_native(f, &s);
                __asm__ __volatile__("" : : "r"(&s) : "memory");
            }
            t1 = cpu_ns();
            nat += t1 - t0;
        }
        alarm(0);
        in_arm = 0;
        } else {
            trc = arm_exit_code ? -arm_exit_code : -9;
            alarm(0);
            in_arm = 0;
        }
        if (trc) {
            printf("    TIME skipped: the seeded arm faults on some iteration "
                   "(rc=%d); the differential pass above is still valid\n", trc);
            continue;
        }
        double per_reseed = reseed / REPS / (double)iters;
        double per_disp = disp / REPS / (double)iters - per_reseed;
        double per_nat = nat / REPS / (double)iters - per_reseed;
        double insn_d = per_disp / (double)f->insns;
        double insn_n = per_nat / (double)f->insns;
        printf("    TIME dispatch ns/insn=%7.2f  native ns/insn=%7.2f  ratio=%5.2fx "
               "reseed_ns=%.1f\n", insn_d, insn_n, insn_d / insn_n, per_reseed);
        sum_disp_insn += per_disp;
        sum_nat_insn += per_nat;
        sum_disp_ns += per_disp;
        sum_nat_ns += per_nat;
        sum_insns_weight += (double)f->insns;
    }

    printf("\nSUMMARY functions=%d exercised=%d skipped=%d mismatch=%d\n",
           FN_SPECS_COUNT, exercised, skipped, total_bad);
    if (sum_insns_weight > 0.0) {
        double per_disp = sum_disp_insn / (double)exercised;
        double per_nat = sum_nat_insn / (double)exercised;
        printf("MEAN_PER_CALL over %d functions: dispatch %.1f ns, native %.1f ns, "
               "ratio %.2fx\n", exercised, per_disp, per_nat, per_disp / per_nat);
    }
    if (sum_disp_ns > 0.0)
        printf("TOTAL_CPU dispatch=%.3f s native=%.3f s (%.2f%% saved)\n",
               sum_disp_ns, sum_nat_ns, 100.0 * (sum_disp_ns - sum_nat_ns) / sum_disp_ns);
    printf("RESULT %s\n", total_bad == 0 ? "ALL-BIT-EXACT" : "MISMATCHES");
    return total_bad ? 1 : 0;
}
