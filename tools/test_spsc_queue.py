#!/usr/bin/env python3
"""Compile and exercise the host-only SPSC queue and idle worker gate."""

import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

TEST_C = r'''#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "native_spsc.h"

#define CAPACITY 64u
#define ITEMS 1000000u

typedef struct {
    Dq8SpscQueue *queue;
    _Atomic bool *start;
    _Atomic unsigned *errors;
} ThreadArgs;

static void *produce(void *opaque) {
    ThreadArgs *args = (ThreadArgs *)opaque;
    while (!atomic_load_explicit(args->start, memory_order_acquire)) {}
    for (uintptr_t value = 0; value < ITEMS; ++value) {
        while (!dq8_spsc_try_push(args->queue, value)) {}
    }
    return NULL;
}

static void *consume(void *opaque) {
    ThreadArgs *args = (ThreadArgs *)opaque;
    while (!atomic_load_explicit(args->start, memory_order_acquire)) {}
    for (uintptr_t expected = 0; expected < ITEMS; ++expected) {
        uintptr_t value;
        while (!dq8_spsc_try_pop(args->queue, &value)) {}
        if (value != expected)
            atomic_fetch_add_explicit(args->errors, 1u, memory_order_relaxed);
    }
    return NULL;
}

int main(void) {
    Dq8SpscQueue q;
    uintptr_t storage[CAPACITY];
    uintptr_t value = 0xDEADBEEFu;
    assert(!dq8_spsc_init(&q, storage, 1u));
    assert(!dq8_spsc_init(&q, storage, 3u));
    assert(dq8_spsc_init(&q, storage, CAPACITY));
    assert(!dq8_spsc_try_pop(&q, &value));

    /* Full, FIFO order, wraparound, and exact empty/full boundaries. */
    for (uintptr_t i = 0; i < CAPACITY; ++i)
        assert(dq8_spsc_try_push(&q, i));
    assert(!dq8_spsc_try_push(&q, 999u));
    for (uintptr_t i = 0; i < CAPACITY / 2u; ++i) {
        assert(dq8_spsc_try_pop(&q, &value));
        assert(value == i);
    }
    for (uintptr_t i = CAPACITY; i < CAPACITY + CAPACITY / 2u; ++i)
        assert(dq8_spsc_try_push(&q, i));
    for (uintptr_t i = CAPACITY / 2u; i < CAPACITY + CAPACITY / 2u; ++i) {
        assert(dq8_spsc_try_pop(&q, &value));
        assert(value == i);
    }
    assert(!dq8_spsc_try_pop(&q, &value));

    /* Monotonic unsigned counters wrap independently of the power-of-two slot index. */
    atomic_store_explicit(&q.head, UINT32_MAX - 1u, memory_order_relaxed);
    atomic_store_explicit(&q.tail, UINT32_MAX - 1u, memory_order_relaxed);
    for (uintptr_t i = 0; i < 4u; ++i)
        assert(dq8_spsc_try_push(&q, 0xA0u + i));
    for (uintptr_t i = 0; i < 4u; ++i) {
        assert(dq8_spsc_try_pop(&q, &value));
        assert(value == 0xA0u + i);
    }
    assert(!dq8_spsc_try_pop(&q, &value));

    /* Worker scaffolding is explicitly opt-in and returns to IDLE only after join. */
    Dq8WorkerScaffold worker;
    dq8_worker_scaffold_init(&worker);
    assert(!dq8_worker_scaffold_is_active(&worker));
    assert(dq8_worker_scaffold_activate(&worker));
    assert(dq8_worker_scaffold_is_active(&worker));
    assert(!dq8_worker_scaffold_activate(&worker));
    dq8_worker_scaffold_deactivate(&worker);
    assert(!dq8_worker_scaffold_is_active(&worker));

    assert(dq8_spsc_init(&q, storage, CAPACITY));
    _Atomic bool start;
    _Atomic unsigned errors;
    atomic_init(&start, false);
    atomic_init(&errors, 0u);
    ThreadArgs args = { &q, &start, &errors };
    pthread_t producer, consumer;
    assert(pthread_create(&producer, NULL, produce, &args) == 0);
    assert(pthread_create(&consumer, NULL, consume, &args) == 0);
    atomic_store_explicit(&start, true, memory_order_release);
    assert(pthread_join(producer, NULL) == 0);
    assert(pthread_join(consumer, NULL) == 0);
    assert(atomic_load_explicit(&errors, memory_order_relaxed) == 0u);
    assert(!dq8_spsc_try_pop(&q, &value));
    puts("spsc_queue: capacity/wrap/FIFO, idle gate, and 1M concurrent transfers OK");
    return 0;
}
'''


def main():
    compiler = os.environ.get("CC", "clang")
    with tempfile.TemporaryDirectory(prefix="dq8-spsc-test-") as tmp:
        tmp = Path(tmp)
        source = tmp / "test_spsc_queue.c"
        binary = tmp / "test_spsc_queue"
        source.write_text(TEST_C)
        subprocess.run(
            [compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
             "-pthread", "-I", str(ROOT / "src"), str(source), "-o", str(binary)],
            check=True,
        )
        env = dict(os.environ)
        env["DQ8_RUN_GUEST_THREADS"] = "1"
        subprocess.run([str(binary)], check=True, timeout=30, env=env)


if __name__ == "__main__":
    main()
