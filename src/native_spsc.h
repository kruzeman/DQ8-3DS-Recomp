#ifndef DQ8_NATIVE_SPSC_H
#define DQ8_NATIVE_SPSC_H

/*
 * Host-only single-producer/single-consumer queue primitives for later
 * audio/render workers. This header is deliberately not included by the
 * current guest runtime: queue and worker scaffolding have no default runtime
 * cost and do not change the serialized guest path.
 *
 * A successful push transfers the uintptr_t token to the consumer; a failed
 * push leaves it with the producer. A successful pop transfers the token to
 * the consumer; after the consumer publishes tail, the producer may reuse the
 * slot. If a token is a pointer, its pointed-to payload must follow the same
 * ownership transfer and must not be guest RAM that the guest can mutate.
 *
 * Exactly one producer may call try_push and exactly one consumer may call
 * try_pop. Queue storage, capacity, and the queue object are immutable after
 * init and must be published to both threads before either starts. Full/empty
 * are non-blocking results: this primitive never drops, overwrites, allocates,
 * waits, or chooses a service-specific backpressure policy.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#define DQ8_SPSC_MIN_CAPACITY 2u
#define DQ8_SPSC_MAX_CAPACITY (UINT32_MAX / 2u)

typedef struct {
    _Atomic uint32_t head;       /* producer-owned counter; release publishes */
    _Atomic uint32_t tail;       /* consumer-owned counter; release frees slots */
    uint32_t capacity;            /* immutable after init; power of two */
    uint32_t mask;                /* capacity - 1 */
    uintptr_t *slots;             /* caller-owned, immutable backing storage */
} Dq8SpscQueue;

/* Returns false for invalid storage/capacity or non-lock-free host atomics. */
static inline bool dq8_spsc_init(Dq8SpscQueue *q, uintptr_t *storage,
                                 uint32_t capacity) {
    if (q == NULL || storage == NULL || capacity < DQ8_SPSC_MIN_CAPACITY ||
        capacity > DQ8_SPSC_MAX_CAPACITY || (capacity & (capacity - 1u)) != 0u)
        return false;

    atomic_init(&q->head, 0u);
    atomic_init(&q->tail, 0u);
    q->capacity = capacity;
    q->mask = capacity - 1u;
    q->slots = storage;
    return atomic_is_lock_free(&q->head) && atomic_is_lock_free(&q->tail);
}

/* Producer only. A false return means full (or a conservative stale-tail read). */
static inline bool dq8_spsc_try_push(Dq8SpscQueue *q, uintptr_t value) {
    uint32_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    if ((uint32_t)(head - tail) >= q->capacity)
        return false;

    q->slots[head & q->mask] = value;
    atomic_store_explicit(&q->head, head + 1u, memory_order_release);
    return true;
}

/* Consumer only. A false return means empty (or a conservative stale-head read). */
static inline bool dq8_spsc_try_pop(Dq8SpscQueue *q, uintptr_t *value) {
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    if (tail == head)
        return false;

    *value = q->slots[tail & q->mask];
    atomic_store_explicit(&q->tail, tail + 1u, memory_order_release);
    return true;
}

/* Minimal future-worker gate only. It creates/joins no OS thread by itself. */
enum {
    DQ8_WORKER_IDLE = 0u,
    DQ8_WORKER_ACTIVE = 1u
};

typedef struct {
    _Atomic uint32_t state;
} Dq8WorkerScaffold;

static inline void dq8_worker_scaffold_init(Dq8WorkerScaffold *worker) {
    atomic_init(&worker->state, DQ8_WORKER_IDLE);
}

static inline bool dq8_worker_scaffold_is_active(const Dq8WorkerScaffold *worker) {
    return atomic_load_explicit(&worker->state, memory_order_acquire) == DQ8_WORKER_ACTIVE;
}

/* Called by the owning guest/runtime thread only after successful worker setup. */
static inline bool dq8_worker_scaffold_activate(Dq8WorkerScaffold *worker) {
    uint32_t expected = DQ8_WORKER_IDLE;
    return atomic_compare_exchange_strong_explicit(
        &worker->state, &expected, DQ8_WORKER_ACTIVE,
        memory_order_release, memory_order_relaxed);
}

/* Call only after the OS worker has stopped and been joined. */
static inline void dq8_worker_scaffold_deactivate(Dq8WorkerScaffold *worker) {
    atomic_store_explicit(&worker->state, DQ8_WORKER_IDLE, memory_order_release);
}

#endif /* DQ8_NATIVE_SPSC_H */
