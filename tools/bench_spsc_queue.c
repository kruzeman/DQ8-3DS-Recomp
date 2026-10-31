#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "native_spsc.h"

#define CAPACITY 1024u
#define DEFAULT_ITEMS 10000000u
#define LATENCY_SAMPLES 50000u

typedef struct {
    Dq8SpscQueue *queue;
    uint64_t items;
    _Atomic bool *start;
    _Atomic uint64_t *bad_order;
} BenchArgs;

/* Keep queue calls opaque to the single-thread optimizer. Otherwise it can
   prove a local push/pop round trip and delete the measurement's subject. */
#if defined(__GNUC__)
__attribute__((noinline))
#endif
static bool bench_push(Dq8SpscQueue *q, uintptr_t value) {
    return dq8_spsc_try_push(q, value);
}

#if defined(__GNUC__)
__attribute__((noinline))
#endif
static bool bench_pop(Dq8SpscQueue *q, uintptr_t *value) {
    return dq8_spsc_try_pop(q, value);
}

static uint64_t now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime");
        exit(2);
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int compare_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static void *producer(void *opaque) {
    BenchArgs *args = (BenchArgs *)opaque;
    while (!atomic_load_explicit(args->start, memory_order_acquire)) {}
    for (uint64_t i = 0; i < args->items; ++i)
        while (!bench_push(args->queue, (uintptr_t)i)) {}
    return NULL;
}

static void *consumer(void *opaque) {
    BenchArgs *args = (BenchArgs *)opaque;
    while (!atomic_load_explicit(args->start, memory_order_acquire)) {}
    for (uint64_t expected = 0; expected < args->items; ++expected) {
        uintptr_t value;
        while (!bench_pop(args->queue, &value)) {}
        if (value != (uintptr_t)expected)
            atomic_fetch_add_explicit(args->bad_order, 1u, memory_order_relaxed);
    }
    return NULL;
}

static void report_latency(uint64_t samples) {
    Dq8SpscQueue q;
    uintptr_t slots[CAPACITY], value;
    uint64_t *latency = (uint64_t *)malloc((size_t)samples * sizeof(*latency));
    if (!latency || !dq8_spsc_init(&q, slots, CAPACITY)) {
        fprintf(stderr, "latency benchmark initialization failed\n");
        exit(2);
    }
    for (uint64_t i = 0; i < samples; ++i) {
        uint64_t begin = now_ns();
        if (!bench_push(&q, (uintptr_t)i) || !bench_pop(&q, &value) ||
            value != (uintptr_t)i) {
            fprintf(stderr, "latency benchmark queue check failed\n");
            exit(2);
        }
        latency[i] = now_ns() - begin;
    }
    qsort(latency, (size_t)samples, sizeof(*latency), compare_u64);
    printf("roundtrip_latency_ns samples=%llu p50=%llu p95=%llu p99=%llu "
           "(includes two clock_gettime calls)\n",
           (unsigned long long)samples,
           (unsigned long long)latency[samples / 2u],
           (unsigned long long)latency[(samples * 95u) / 100u],
           (unsigned long long)latency[(samples * 99u) / 100u]);
    free(latency);
}

static void report_single_thread(uint64_t items) {
    Dq8SpscQueue q;
    uintptr_t slots[CAPACITY], value;
    if (!dq8_spsc_init(&q, slots, CAPACITY)) {
        fprintf(stderr, "single-thread benchmark initialization failed\n");
        exit(2);
    }
    uintptr_t checksum = 0u;
    uint64_t begin = now_ns();
    for (uint64_t i = 0; i < items; ++i) {
        if (!bench_push(&q, (uintptr_t)i) || !bench_pop(&q, &value) ||
            value != (uintptr_t)i) {
            fprintf(stderr, "single-thread benchmark queue check failed\n");
            exit(2);
        }
        checksum = checksum * (uintptr_t)33u + value;
    }
    uint64_t elapsed = now_ns() - begin;
    printf("single_thread items=%llu elapsed_ns=%llu roundtrips_per_s=%.0f "
           "ns_per_push_plus_pop=%.2f checksum=%llu\n",
           (unsigned long long)items, (unsigned long long)elapsed,
           (double)items * 1e9 / (double)elapsed,
           (double)elapsed / (double)items,
           (unsigned long long)checksum);
}

static void report_contended(uint64_t items) {
    Dq8SpscQueue q;
    uintptr_t slots[CAPACITY];
    _Atomic bool start;
    _Atomic uint64_t bad_order;
    atomic_init(&start, false);
    atomic_init(&bad_order, 0u);
    if (!dq8_spsc_init(&q, slots, CAPACITY)) {
        fprintf(stderr, "contended benchmark initialization failed\n");
        exit(2);
    }
    BenchArgs args = { &q, items, &start, &bad_order };
    pthread_t prod, cons;
    if (pthread_create(&prod, NULL, producer, &args) != 0 ||
        pthread_create(&cons, NULL, consumer, &args) != 0) {
        perror("pthread_create");
        exit(2);
    }
    uint64_t begin = now_ns();
    atomic_store_explicit(&start, true, memory_order_release);
    if (pthread_join(prod, NULL) != 0 || pthread_join(cons, NULL) != 0) {
        perror("pthread_join");
        exit(2);
    }
    uint64_t elapsed = now_ns() - begin;
    uint64_t errors = atomic_load_explicit(&bad_order, memory_order_relaxed);
    printf("spsc_contended items_each_direction=%llu elapsed_ns=%llu "
           "items_per_s=%.0f combined_push_pop_ops_per_s=%.0f ordering_errors=%llu\n",
           (unsigned long long)items, (unsigned long long)elapsed,
           (double)items * 1e9 / (double)elapsed,
           (double)items * 2e9 / (double)elapsed,
           (unsigned long long)errors);
    if (errors != 0u)
        exit(1);
}

int main(int argc, char **argv) {
    uint64_t items = DEFAULT_ITEMS;
    if (argc > 1) {
        char *end = NULL;
        items = strtoull(argv[1], &end, 10);
        if (end == argv[1] || *end != '\0' || items == 0u) {
            fprintf(stderr, "usage: %s [positive-item-count]\n", argv[0]);
            return 2;
        }
    }
    report_latency(LATENCY_SAMPLES);
    report_single_thread(items);
    report_contended(items);
    return 0;
}
