/* tools/vspar_protocol_model.c  -- t_e1773c51
 *
 * A standalone model of the two vertex-pool completion protocols, used to show
 * WHY the pre-t_e1773c51 handshake could let the main thread leave a job while
 * a participant was still executing it (the precondition for the intermittent
 * world loss), and that the generation-tagged protocol cannot.
 *
 * This is a model, not the game: the "chunk work" is a short busy loop and the
 * schedule is adversarial on purpose (random join delays, workers started late
 * relative to the publisher).  Build:
 *
 *   gcc -O2 -pthread -o /tmp/vsparmodel tools/vspar_protocol_model.c
 *   /tmp/vsparmodel old        # pre-t_e1773c51 protocol
 *   /tmp/vsparmodel new        # generation-tagged descriptor + live word
 *
 * Both modes run JOBS jobs with 8 workers and report:
 *   violations - the main thread left a job / started the next one while at
 *                least one participant was inside a body (the race),
 *   orphan     - the main thread saw done>=nparts after a number of increments
 *                that did not come from participants of that job.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <linux/futex.h>
#include <sys/syscall.h>

#define NW 8
#define JOBS 2000
#define RING 32768

static int mode_new;
static volatile int stop_flag;
static volatile unsigned gen_word;            /* old: gen, new: gen (futex) */
static unsigned gen_pub;
static atomic_int in_body;                    /* participants inside a body */
static unsigned long violations, orphan, jobs_done;
static void *watchdog(void *arg) {
    unsigned long last = 0; int still = 0;
    (void)arg;
    for (;;) {
        struct timespec ts = {0, 250000000};
        nanosleep(&ts, NULL);
        if (atomic_load(&stop_flag)) return NULL;
        if (jobs_done == last) { if (++still >= 8) {
            printf("STALLED at job %lu gen=%u in_body=%d (a participant skipped a generation: the publisher waits for a count that never comes)\n",
                   jobs_done, gen_word, atomic_load(&in_body));
            fflush(stdout); _exit(3); } } else { still = 0; last = jobs_done; }
    }
}


/* ---------------- old protocol state (b8a1c06) ---------------- */
static volatile unsigned o_done;               /* reset at publish */
static unsigned o_nparts = NW + 1;
static volatile unsigned o_next, o_nchunks;

/* ---------------- new protocol state ---------------- */
typedef struct {
    unsigned gen, nchunks;
    unsigned next, wake;
    uint64_t live;                             /* {joined:32, done:32} */
} Job;
static Job ring[RING];
static atomic_int new_violations;

static uint64_t tls_rand_state;
static unsigned long long xr(void) {
    if (!tls_rand_state) tls_rand_state = (uint64_t)(uintptr_t)&tls_rand_state ^ 0x9E3779B97F4A7C15ull;
    tls_rand_state ^= tls_rand_state << 13;
    tls_rand_state ^= tls_rand_state >> 7;
    tls_rand_state ^= tls_rand_state << 17;
    return tls_rand_state;
}
static void park(volatile unsigned *p, unsigned v) {
    syscall(SYS_futex, (unsigned *)p, FUTEX_WAIT_PRIVATE, v, NULL, NULL, 0);
}
static void unpark(volatile unsigned *p) {
    syscall(SYS_futex, (unsigned *)p, FUTEX_WAKE_PRIVATE, 0x7fffffff, NULL, NULL, 0);
}
static void body_work(void) {
    /* variable-length "chunk": a few hundred ns..2 us of real work */
    volatile unsigned x = 0;
    unsigned n = 200u + (unsigned)(xr() % 2000u);
    for (unsigned i = 0; i < n; ++i) x += i;
    (void)x;
}
static void run_chunk(void) {
    atomic_fetch_add(&in_body, 1);
    body_work();
    atomic_fetch_sub(&in_body, 1);
}
static void join_delay(void) {
    /* the late-waker window: 0..400 us, plus an occasional long stall */
    unsigned long long r = xr();
    struct timespec ts;
    if ((r & 0xFF) == 0) { ts.tv_sec = 0; ts.tv_nsec = 200000 + (long)(r % 800000u); nanosleep(&ts, NULL); }
    else if ((r & 7) == 0) { ts.tv_sec = 0; ts.tv_nsec = (long)(r % 400000u); nanosleep(&ts, NULL); }
    else sched_yield();
}

/* =========================== OLD (pre-fix) =========================== */
static void *old_worker(void *arg) {
    unsigned seen;
    (void)arg;
    /* b8a1c06: `unsigned seen = 0;` -- a thread created but scheduled late
       therefore believes generation 0 is "new" and runs a body + increments
       the shared completion counter for whatever generation is current. */
    seen = 0;
    for (;;) {
        unsigned g;
        if (atomic_load(&stop_flag)) return NULL;
        g = atomic_load(&gen_word);
        if (g == seen) { park(&gen_word, seen); continue; }
        seen = g;
        join_delay();
        run_chunk();
        if (atomic_fetch_add(&o_done, 1) == o_nparts) unpark(&o_done);
    }
}
static void old_publish(void) {
    unsigned spins = 0;
    o_next = 0;
    o_done = 0;
    atomic_fetch_add(&gen_word, 1);
    unpark(&gen_word);
    /* main thread participates */
    for (;;) {
        unsigned c = atomic_fetch_add(&o_next, 1);
        if (c >= o_nchunks) break;
        if (c != 0) run_chunk(); else { atomic_fetch_add(&in_body, 1); body_work(); atomic_fetch_sub(&in_body, 1); }
    }
    atomic_fetch_add(&o_done, 1);
    for (;;) {
        unsigned d = atomic_load(&o_done);
        if (d >= o_nparts) break;
        if ((++spins & 0xFF) == 0) park(&o_done, d);
        else sched_yield();
    }
    /* The main thread now assembles pvs_vout[], publishes the next draw's plan
       into pvs_plan_cur and starts the next draw. */
    if (atomic_load(&in_body) != 0) { atomic_fetch_add(&new_violations, 1); }
}
static void *old_pub_thread(void *arg) {
    (void)arg;
    for (unsigned i = 0; i < JOBS; ++i) {
        o_nchunks = 1 + (unsigned)(xr() % 12);
        old_publish();
        jobs_done++;
    }
    atomic_store(&stop_flag, 1);
    unpark(&gen_word);
    return NULL;
}

/* =========================== NEW (t_e1773c51) =========================== */
static inline unsigned live_done(uint64_t v) { return (unsigned)v; }
static inline unsigned live_joined(uint64_t v) { return (unsigned)(v >> 32); }
static void new_worker_loop(void) {
    unsigned seen = atomic_load(&gen_word);   /* never "run" a pre-existing gen */
    for (;;) {
        unsigned g;
        struct timespec ts;
        if (atomic_load(&stop_flag)) return;
        g = atomic_load(&gen_word);
        if (g == seen) { park(&gen_word, seen); continue; }
        seen = g;
        join_delay();
        {
            Job *b = &ring[g % RING];
            if (atomic_load(&b->gen) != g) continue;
            atomic_fetch_add(&b->live, 1ull << 32);
            for (;;) {
                unsigned c = atomic_fetch_add(&b->next, 1);
                if (c >= b->nchunks) break;
                run_chunk();
            }
            {
                uint64_t v = atomic_fetch_add(&b->live, 1ull);
                atomic_fetch_add(&b->wake, 1);
                if (live_done(v) + 1 >= live_joined(v)) unpark(&b->wake);
            }
        }
    }
}
static void *new_worker(void *arg) { (void)arg; new_worker_loop(); return NULL; }
static void new_publish(unsigned nchunks) {
    Job *b;
    unsigned g = ++gen_pub;
    b = &ring[g % RING];
    b->gen = 0;
    b->nchunks = nchunks;
    b->next = 0;
    b->wake = 0;
    b->live = 1ull << 32;
    b->gen = g;
    atomic_store(&gen_word, g);
    unpark(&gen_word);
    for (;;) {
        unsigned c = atomic_fetch_add(&b->next, 1);
        if (c >= b->nchunks) break;
        run_chunk();
    }
    {
        uint64_t v = atomic_fetch_add(&b->live, 1ull);
        atomic_fetch_add(&b->wake, 1);
        if (live_done(v) + 1 >= live_joined(v)) unpark(&b->wake);
    }
    for (;;) {
        uint64_t v = atomic_load(&b->live);
        unsigned tok;
        if (live_done(v) >= live_joined(v)) break;
        tok = atomic_load(&b->wake);
        park(&b->wake, tok);
    }
    if (atomic_load(&in_body) != 0) atomic_fetch_add(&new_violations, 1);
}
static void *new_pub_thread(void *arg) {
    (void)arg;
    for (unsigned i = 0; i < JOBS; ++i) {
        new_publish(1 + (unsigned)(xr() % 12));
        jobs_done++;
    }
    atomic_store(&stop_flag, 1);
    unpark(&gen_word);
    return NULL;
}

int main(int argc, char **argv) {
    pthread_t th[NW + 1];
    int stagger;
    if (argc > 1 && !strcmp(argv[1], "new")) mode_new = 1;
    /* create the workers, but let some of them start LATE relative to the
       publisher: exactly what happens when the pool is created lazily on the
       first parallel draw of a run. */
    stagger = 1;
    { pthread_t wd; pthread_create(&wd, NULL, watchdog, NULL); pthread_detach(wd); }
    for (int i = 0; i < NW; ++i)
        pthread_create(&th[i], NULL, mode_new ? new_worker : old_worker, NULL);
    if (stagger) { struct timespec ts = {0, 3000000}; nanosleep(&ts, NULL); }
    pthread_create(&th[NW], NULL, mode_new ? new_pub_thread : old_pub_thread, NULL);
    pthread_join(th[NW], NULL);
    for (int i = 0; i < NW; ++i) pthread_join(th[i], NULL);
    printf("%-3s jobs=%lu  main-left-while-participant-live=%lu\n",
           mode_new ? "new" : "old", jobs_done,
           (unsigned long)(mode_new ? atomic_load(&new_violations) : atomic_load(&new_violations)));
    return 0;
}
