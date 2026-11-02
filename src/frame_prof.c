/* FRAMEPROF (t_e778afc0): env-gated per-frame time budget.
 *
 * DQ8_FRAMEPROF=1 turns on a phase clock over the runtime's known frame
 * boundaries.  Default OFF: every hook is one load+branch (`pf_active()`),
 * so a build with the knob unset behaves exactly like the parent revision.
 *
 * FRAME DEFINITION: one PDC0 (top screen) scan-out = one guest frame, the
 * same boundary FRAMESTAT uses (gsp_enqueue_interrupt -> gsp_scanout_capture).
 *
 * PHASES (measured with CLOCK_MONOTONIC on the main thread only; the phase
 * stack makes nested regions attribute to the innermost phase):
 *   PF_GUEST   the guest dispatch loop (translated_from) and everything the
 *              interpreter calls, minus the phases below
 *   PF_GL      GL submit and worker/readback wait (pica_gl_draw, the
 *              deferred write-back flush), including a GL worker wait
 *   PF_PRESENT present/swap + vblank parking + scan-out publish
 *   PF_AUDIO   DSP mix + host audio queue push
 *   PF_IDLE    host sleeps not attributable to a vblank deadline
 *   other      remainder (computed, so the columns always sum to total)
 *
 * OUTPUT (stderr, exactly one line per frame):
 *   FRAMEPROF f=<n> t=<s> total_ms= guest_ms= gl_ms= present_ms= audio_ms=
 *             idle_ms= other_ms=
 * every 10 s, one summary line with window fps/averages plus process and
 * per-core/per-thread CPU% sampled from /proc (see pf_summary).
 *
 * Cost when enabled: 2 clock_gettime per hooked region boundary (a few
 * hundred per frame) + one ~200 byte write per frame; < 0.1 % of a frame.
 */
#ifndef DQ8_FRAME_PROF_C
#define DQ8_FRAME_PROF_C
#include <dirent.h>
#include <ctype.h>

enum { PF_IDLE = 0, PF_GUEST, PF_GL, PF_PRESENT, PF_AUDIO, PF_OTHER, PF_NPHASE };
#define PF_STACK_MAX 40
#define PF_SUM_PERIOD_NS 10000000000ull

static int pf_on = -1;              /* -1 unknown, 0 off, 1 on */
static uint64_t pf_owner;           /* pthread_self of the instrumented thread */
static __thread int pf_foreign;     /* 1 on any thread that is not pf_owner */
static uint64_t pf_acc_ns[PF_NPHASE];
static int pf_cur = PF_OTHER;
static uint64_t pf_ts;
static unsigned char pf_stack[PF_STACK_MAX];
static int pf_depth;
static uint64_t pf_frames;          /* frames emitted so far */
static uint64_t pf_t0;              /* profiler start (t= origin) */
static uint64_t pf_frame_t0;        /* start of the current frame */
static uint64_t pf_win_frames;
static uint64_t pf_win_t0;
static double pf_win_ms[PF_NPHASE];
static double pf_win_other_ms;
static double pf_win_total_ms;
/* /proc sampling state for the 10 s summary. */
static long pf_clk_tck;
static uint64_t pf_prev_self_ticks;
static uint64_t pf_prev_core_total[512], pf_prev_core_idle[512];
static int pf_prev_cores;
static uint64_t pf_prev_thread_ticks[64];
static int pf_prev_thread_tid[64];
static int pf_prev_thread_n;
static char pf_prev_thread_name[64][24];

static uint64_t pf_now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* utime+stime in clock ticks from a /proc stat line, and the comm of it. */
static uint64_t pf_stat_ticks(const char *line, char *comm, size_t comm_size) {
    const char *open_par, *close_par, *p;
    unsigned long long ut = 0, st = 0;
    int field = 0;
    if (!line) return 0;
    open_par = strchr(line, '(');
    close_par = strrchr(line, ')');
    if (comm && comm_size && open_par && close_par && close_par > open_par) {
        size_t len = (size_t)(close_par - open_par - 1);
        if (len >= comm_size) len = comm_size - 1u;
        memcpy(comm, open_par + 1, len);
        comm[len] = 0;
    }
    if (!close_par) return 0;
    /* After ") " field 0 = state (stat field 3), so utime (field 14) is
       field index 11 and stime (field 15) index 12. */
    p = close_par + 1;
    while (*p && field <= 12) {
        while (*p == ' ') ++p;
        if (field == 11) ut = strtoull(p, NULL, 10);
        else if (field == 12) { st = strtoull(p, NULL, 10); break; }
        ++field;
        while (*p && *p != ' ') ++p;
    }
    return ut + st;
}

static uint64_t pf_read_self_ticks(void) {
    char buf[1024];
    FILE *f = fopen("/proc/self/stat", "r");
    uint64_t v;
    if (!f) return 0;
    if (!fgets(buf, sizeof buf, f)) { fclose(f); return 0; }
    fclose(f);
    v = pf_stat_ticks(buf, NULL, 0);
    return v;
}

static void pf_summary(uint64_t now);

static void pf_start(uint64_t now) {
    pf_t0 = now;
    pf_frame_t0 = now;
    pf_win_t0 = now;
    pf_ts = now;
    pf_clk_tck = sysconf(_SC_CLK_TCK);
    if (pf_clk_tck <= 0) pf_clk_tck = 100;
    pf_prev_self_ticks = pf_read_self_ticks();
}

static int pf_active(void) {
    if (pf_on < 0) {
        const char *e = getenv("DQ8_FRAMEPROF");
        pf_on = (e && e[0] && e[0] != '0') ? 1 : 0;
        if (pf_on) pf_start(pf_now_ns());
    }
    return pf_on;
}

/* Per-thread gate: only the thread that armed the profiler (the guest main
   thread) owns the phase stack; a worker thread must not disturb it. */
static int pf_mine(void) {
    if (pf_foreign) return 0;
    if (pf_owner == 0) { pf_owner = (uint64_t)pthread_self(); return 1; }
    if ((uint64_t)pthread_self() != pf_owner) { pf_foreign = 1; return 0; }
    return 1;
}

static void pf_push(int phase) {
    uint64_t now;
    if (!pf_active()) return;
    if (!pf_mine()) return;
    now = pf_now_ns();
    pf_acc_ns[pf_cur] += now - pf_ts;
    pf_ts = now;
    if (pf_depth < PF_STACK_MAX) pf_stack[pf_depth++] = (unsigned char)pf_cur;
    pf_cur = phase;
}

static void pf_pop(void) {
    uint64_t now;
    if (!pf_active()) return;
    if (pf_foreign) return;
    now = pf_now_ns();
    pf_acc_ns[pf_cur] += now - pf_ts;
    pf_ts = now;
    if (pf_depth > 0) pf_cur = (int)pf_stack[--pf_depth];
    else pf_cur = PF_OTHER;
}

/* Bracket a whole statement: pf_enter(X); <work>; pf_exit(); */
#define PF_ENTER(phase) pf_push(phase)
#define PF_EXIT() pf_pop()

static void pf_summary(uint64_t now) {
    char line[2048];
    char cores[512], threads[512];
    size_t cores_len = 0, threads_len = 0;
    double elapsed = (double)(now - pf_win_t0) / 1e9;
    double frames = (double)pf_win_frames;
    double proc_pct = 0.0;
    uint64_t self_ticks;
    FILE *f;
    int i;

    if (elapsed <= 0.0) elapsed = 1e-9;
    cores[0] = 0;
    threads[0] = 0;
    if (!frames) frames = 1.0;

    /* Process CPU (all threads) over the window: utime+stime delta. */
    self_ticks = pf_read_self_ticks();
    proc_pct = 100.0 * ((double)(self_ticks - pf_prev_self_ticks) / (double)pf_clk_tck) / elapsed;
    pf_prev_self_ticks = self_ticks;

    /* Per-core system busy% (>20% only) from /proc/stat. */
    f = fopen("/proc/stat", "r");
    if (f) {
        char lbuf[512];
        int cores_seen = pf_prev_cores;
        while (fgets(lbuf, sizeof lbuf, f)) {
            int core;
            unsigned long long v[10];
            int nv, k;
            if (strncmp(lbuf, "cpu", 3) != 0) break;      /* cpu lines are first */
            if (!isdigit((unsigned char)lbuf[3])) continue; /* aggregate "cpu " */
            core = atoi(lbuf + 3);
            if (core < 0 || core >= (int)(sizeof pf_prev_core_total / sizeof pf_prev_core_total[0]))
                continue;
            nv = sscanf(lbuf, "cpu%*d %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                        &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
            if (nv < 4) continue;
            {
                unsigned long long total = 0, idle = v[3] + (nv > 4 ? v[4] : 0ull);
                for (k = 0; k < nv && k < 10; ++k) total += v[k];
                /* pf_prev_cores starts at 0: the first summary only primes the
                   deltas, so no window can report a percentage against zeros. */
                if (core < pf_prev_cores) {
                    unsigned long long dt = total - pf_prev_core_total[core];
                    unsigned long long di = idle - pf_prev_core_idle[core];
                    double pct = dt ? 100.0 * (double)(dt - (di > dt ? dt : di)) / (double)dt : 0.0;
                    if (pct > 20.0 && cores_len < sizeof cores - 32)
                        cores_len += (size_t)snprintf(cores + cores_len, sizeof cores - cores_len,
                                                      "%scpu%d:%.0f", cores_len ? "," : "", core, pct);
                }
                pf_prev_core_total[core] = total;
                pf_prev_core_idle[core] = idle;
                if (core + 1 > cores_seen) cores_seen = core + 1;
            }
        }
        fclose(f);
        pf_prev_cores = cores_seen;
    }

    /* Per-thread CPU% (>20% only) for the process: which thread owns a core. */
    {
        DIR *d = opendir("/proc/self/task");
        if (d) {
            struct dirent *de;
            uint64_t new_ticks[64];
            int new_tid[64], new_n = 0;
            char new_name[64][24];
            while ((de = readdir(d)) != NULL && new_n < 64) {
                char path[64], lbuf[1024], comm[24];
                long tid = strtol(de->d_name, NULL, 10);
                uint64_t ticks;
                FILE *tf;
                if (tid <= 0) continue;
                snprintf(path, sizeof path, "/proc/self/task/%ld/stat", tid);
                tf = fopen(path, "r");
                if (!tf) continue;
                if (!fgets(lbuf, sizeof lbuf, tf)) { fclose(tf); continue; }
                fclose(tf);
                ticks = pf_stat_ticks(lbuf, comm, sizeof comm);
                new_tid[new_n] = (int)tid;
                new_ticks[new_n] = ticks;
                snprintf(new_name[new_n], sizeof new_name[new_n], "%s", comm);
                ++new_n;
            }
            closedir(d);
            for (i = 0; i < new_n; ++i) {
                double pct = 0.0;
                int j;
                for (j = 0; j < pf_prev_thread_n; ++j)
                    if (pf_prev_thread_tid[j] == new_tid[i]) {
                        pct = 100.0 * ((double)(new_ticks[i] - pf_prev_thread_ticks[j]) /
                                       (double)pf_clk_tck) / elapsed;
                        break;
                    }
                if (pct > 20.0 && threads_len < sizeof threads - 48)
                    threads_len += (size_t)snprintf(threads + threads_len, sizeof threads - threads_len,
                                                    "%s%s:%.0f", threads_len ? "," : "",
                                                    new_name[i], pct);
            }
            pf_prev_thread_n = new_n;
            for (i = 0; i < new_n; ++i) {
                pf_prev_thread_tid[i] = new_tid[i];
                pf_prev_thread_ticks[i] = new_ticks[i];
                snprintf(pf_prev_thread_name[i], sizeof pf_prev_thread_name[i], "%s", new_name[i]);
            }
        }
    }

    snprintf(line, sizeof line,
             "FRAMEPROF-SUM frames=%llu fps=%.2f "
             "avg(total/guest/gl/present/audio/idle)=%.2f/%.2f/%.2f/%.2f/%.2f/%.2f "
             "other=%.2f proc_cpu_pct=%.1f cores=>20%%:[%s] threads=>20%%:[%s]\n",
             (unsigned long long)pf_win_frames, frames / elapsed,
             pf_win_total_ms / frames, pf_win_ms[PF_GUEST] / frames,
             pf_win_ms[PF_GL] / frames, pf_win_ms[PF_PRESENT] / frames,
             pf_win_ms[PF_AUDIO] / frames, pf_win_ms[PF_IDLE] / frames,
             pf_win_other_ms / frames, proc_pct, cores, threads);
    fputs(line, stderr);
    pf_win_frames = 0;
    pf_win_total_ms = 0.0;
    pf_win_other_ms = 0.0;
    for (i = 0; i < PF_NPHASE; ++i) pf_win_ms[i] = 0.0;
    pf_win_t0 = now;
}

/* ---------------------------------------------------------------------------
 * SPIKETRAP (t_5a3c517a): per-frame attribution of a slow frame.
 *
 * The frame profiler attributes wall time to five coarse phases; on a real
 * GPU the whole spike sits in PF_GUEST, which is "everything the guest
 * dispatch loop and the shims it calls did".  That is not actionable, so when
 * DQ8_SPIKETRAP=<ms threshold> is set every shim that can own that time
 * accumulates its own nanoseconds into a per-frame bucket, and a frame whose
 * total exceeds the threshold prints one SPIKETRAP line with the whole split
 * plus the guest PC that was executing.  Off by default: pf_shim() is one
 * predictable load+branch per call site.
 *
 * Buckets are nested (a display transfer's flush is inside PF_GL, a vertex
 * loop is inside PF_GUEST); they are evidence for "which shim", not a
 * partition, so they are printed independently.
 * ------------------------------------------------------------------------- */
enum {
    PF_SH_XFER_FLUSH = 0, PF_SH_XFER_LOOP, PF_SH_XFER_CAP,
    PF_SH_VERT, PF_SH_ATTR, PF_SH_VSEXEC, PF_SH_TRIFLUSH,
    PF_SH_REFRESH, PF_SH_WB, PF_SH_STAGE, PF_SH_GLDRAW, PF_SH_N
};
static const char *const pf_sh_name[PF_SH_N] = {
    "xfer_flush", "xfer_loop", "xfer_cap",
    "vert", "attr", "vsexec", "triflush",
    "refresh", "wb", "stage", "gldraw"
};
static int pf_trap_on = -1;             /* -1 unknown, 0 off, 1 on */
static double pf_trap_thresh_ms = 80.0;
/* Per-PARTICIPANT counters, one padded row per thread (t_5a3c517a).
   The parallel vertex pool calls these from worker threads; a single shared
   accumulator cost a cache-line ping-pong per VS instruction (measured: it
   doubled the spike frame it was trying to attribute).  Each thread owns its
   row, so the hot path is a plain store-free add with no atomics, and the
   report sums the rows.  PF_SH_VSEXEC/ATTR/VERT rows are therefore
   THREAD-SECONDS (they overlap when participants run in parallel), while
   vert/wb/refresh are main-thread wall time. */
#define PF_SLOT_MAX 10
#define PF_SLOT_STRIDE 16               /* 128 B: one cache line per bucket row */
static uint64_t pf_sh_ns[PF_SLOT_MAX][PF_SLOT_STRIDE];
static uint64_t pf_sh_n[PF_SLOT_MAX][PF_SLOT_STRIDE];
static uint64_t pf_vs_steps_s[PF_SLOT_MAX];
static int pf_slot_next;
static _Thread_local int pf_slot = -1;
static int pf_slot_cur(void) {
    if (pf_slot < 0) {
        int s = __atomic_fetch_add(&pf_slot_next, 1, __ATOMIC_RELAXED);
        pf_slot = (s >= 0 && s < PF_SLOT_MAX) ? s : PF_SLOT_MAX - 1;
    }
    return pf_slot;
}
static uint32_t pf_trap_pc;             /* last dispatched guest PC */
static uint64_t pf_trap_frames;         /* frames over the threshold */
static uint64_t pf_trap_total_ns[PF_SH_N];
static uint64_t pf_trap_reported[PF_SH_N];

static int pf_trap(void) {
    if (pf_trap_on < 0) {
        const char *e = getenv("DQ8_SPIKETRAP");
        pf_trap_on = 0;
        pf_trap_thresh_ms = 80.0;
        if (e && e[0] && !(e[0] == '0' && !e[1])) {
            double v = atof(e);
            if (v > 1.0) pf_trap_thresh_ms = v;
            pf_trap_on = 1;
        }
    }
    return pf_trap_on;
}

/* Account `ns` to bucket `id` and bump its call count.  Called from the
   parallel vertex workers too: each thread owns a padded row, no atomics. */
static void pf_shim(int id, uint64_t ns) {
    int s;
    if (pf_trap_on < 0) pf_trap();
    if (pf_trap_on <= 0 || id < 0 || id >= PF_SH_N) return;
    s = pf_slot_cur();
    pf_sh_ns[s][id] += ns;
    ++pf_sh_n[s][id];
}

/* Account `ns` to bucket `id` without a call count (aggregations). */
static void pf_shim_add(int id, uint64_t ns) {
    int s;
    if (pf_trap_on < 0) pf_trap();
    if (pf_trap_on <= 0 || id < 0 || id >= PF_SH_N) return;
    s = pf_slot_cur();
    pf_sh_ns[s][id] += ns;
}

/* Count one executed PICA VS microcode instruction. */
static void pf_vs_step(void) {
    int s = pf_slot_cur();
    ++pf_vs_steps_s[s];
}

/* Cheap "is the trap armed" test for call sites that need a clock pair. */
static int pf_trap_armed(void) {
    if (pf_trap_on < 0) pf_trap();
    return pf_trap_on > 0;
}

static void pf_trap_reset(void) {
    int i, s;
    for (s = 0; s < PF_SLOT_MAX; ++s) {
        for (i = 0; i < PF_SH_N; ++i) { pf_sh_ns[s][i] = 0; pf_sh_n[s][i] = 0; }
        pf_vs_steps_s[s] = 0;
    }
}

static void pf_trap_report(double total_ms, double guest_ms, double gl_ms,
                          double present_ms) {
    char extra[1200];
    size_t len = 0;
    int i, s;
    uint64_t vs_steps = 0;
    ++pf_trap_frames;
    for (s = 0; s < PF_SLOT_MAX; ++s) {
        vs_steps += pf_vs_steps_s[s];
        for (i = 0; i < PF_SH_N; ++i) {
            pf_sh_ns[0][i] += (s ? pf_sh_ns[s][i] : 0);
            pf_sh_n[0][i] += (s ? pf_sh_n[s][i] : 0);
        }
    }
    for (i = 0; i < PF_SH_N; ++i) pf_trap_total_ns[i] += pf_sh_ns[0][i];
    for (i = 0; i < PF_SH_N; ++i) {
        pf_trap_reported[i] = pf_sh_ns[0][i];
        if (!pf_sh_ns[0][i]) continue;
        len += (size_t)snprintf(extra + len, sizeof extra - len,
                                " %s=%.1f/%llu", pf_sh_name[i],
                                (double)pf_sh_ns[0][i] / 1e6,
                                (unsigned long long)pf_sh_n[0][i]);
        if (len > sizeof extra - 96) break;
    }
    fprintf(stderr, "SPIKETRAP f=%llu t=%.3f total_ms=%.1f guest=%.1f gl=%.1f "
                    "present=%.1f pc=%08X vs_steps=%llu%s\n",
            (unsigned long long)pf_frames, (double)(pf_now_ns() - pf_t0) / 1e9,
            total_ms, guest_ms, gl_ms, present_ms, pf_trap_pc,
            (unsigned long long)vs_steps, extra);
}

static void pf_trap_summary(void) {
    char extra[1200];
    size_t len = 0;
    int i;
    for (i = 0; i < PF_SH_N; ++i) {
        if (!pf_trap_total_ns[i]) continue;
        len += (size_t)snprintf(extra + len, sizeof extra - len,
                                " %s=%.0f", pf_sh_name[i],
                                (double)pf_trap_total_ns[i] / 1e6);
        if (len > sizeof extra - 96) break;
    }
    fprintf(stderr, "SPIKETRAP-SUM spikes=%llu thresh_ms=%.0f total_ms_per_cause:%s\n",
            (unsigned long long)pf_trap_frames, pf_trap_thresh_ms, extra);
}

/* Frame boundary: called once per PDC0 scan-out, after the publish. */
static void pf_frame_end(void) {
    char line[320];
    uint64_t now, total;
    double total_ms, guest_ms, gl_ms, present_ms, audio_ms, idle_ms, other_ms;
    int i, n;
    if (!pf_active()) return;
    if (pf_foreign) return;
    if (!pf_frame_t0) { pf_frame_t0 = pf_now_ns(); return; }
    now = pf_now_ns();
    pf_acc_ns[pf_cur] += now - pf_ts;
    pf_ts = now;
    total = now - pf_frame_t0;
    total_ms = (double)total / 1e6;
    guest_ms = (double)pf_acc_ns[PF_GUEST] / 1e6;
    gl_ms = (double)pf_acc_ns[PF_GL] / 1e6;
    present_ms = (double)pf_acc_ns[PF_PRESENT] / 1e6;
    audio_ms = (double)pf_acc_ns[PF_AUDIO] / 1e6;
    idle_ms = (double)pf_acc_ns[PF_IDLE] / 1e6;
    other_ms = total_ms - (guest_ms + gl_ms + present_ms + audio_ms + idle_ms);
    if (other_ms < 0.0) other_ms = 0.0;   /* clock skew between the two reads */
    ++pf_frames;
    if (pf_trap_armed()) {
        static uint64_t pf_trap_sum_last;
        if (total_ms > pf_trap_thresh_ms)
            pf_trap_report(total_ms, guest_ms, gl_ms, present_ms);
        pf_trap_reset();
        if (!pf_trap_sum_last) pf_trap_sum_last = now;
        else if (now - pf_trap_sum_last >= PF_SUM_PERIOD_NS && pf_trap_frames) {
            pf_trap_summary();
            { int i; for (i = 0; i < PF_SH_N; ++i) pf_trap_total_ns[i] = 0; }
            pf_trap_frames = 0;
            pf_trap_sum_last = now;
        }
    }
    n = snprintf(line, sizeof line,
                 "FRAMEPROF f=%llu t=%.3f total_ms=%.3f guest_ms=%.3f gl_ms=%.3f "
                 "present_ms=%.3f audio_ms=%.3f idle_ms=%.3f other_ms=%.3f\n",
                 (unsigned long long)pf_frames, (double)(now - pf_t0) / 1e9,
                 total_ms, guest_ms, gl_ms, present_ms, audio_ms, idle_ms, other_ms);
    if (n > 0) {
        if (n > (int)sizeof line - 1) n = (int)sizeof line - 1;
        fwrite(line, 1, (size_t)n, stderr);
    }
    ++pf_win_frames;
    pf_win_total_ms += total_ms;
    pf_win_other_ms += other_ms;
    pf_win_ms[PF_GUEST] += guest_ms;
    pf_win_ms[PF_GL] += gl_ms;
    pf_win_ms[PF_PRESENT] += present_ms;
    pf_win_ms[PF_AUDIO] += audio_ms;
    pf_win_ms[PF_IDLE] += idle_ms;
    for (i = 0; i < PF_NPHASE; ++i) pf_acc_ns[i] = 0;
    pf_frame_t0 = now;
    if (!pf_win_t0) pf_win_t0 = now;
    if (now - pf_win_t0 >= PF_SUM_PERIOD_NS) pf_summary(now);
}

#endif /* DQ8_FRAME_PROF_C */
