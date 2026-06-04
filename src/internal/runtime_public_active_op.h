/**
 * @file src/internal/runtime_public_active_op.h
 * @brief Active public-operation guard helpers for opaque handles.
 *
 * @copyright Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LLAM_RUNTIME_PUBLIC_ACTIVE_OP_H
#define LLAM_RUNTIME_PUBLIC_ACTIVE_OP_H

#define LLAM_PUBLIC_ACTIVE_OP_RESERVED_THRESHOLD ((SIZE_MAX / 2U) - 1U)
#define LLAM_PUBLIC_ACTIVE_OP_BEGIN_LIMIT (LLAM_PUBLIC_ACTIVE_OP_RESERVED_THRESHOLD - 1U)
#define LLAM_PUBLIC_ACTIVE_OP_BUSY_SENTINEL SIZE_MAX

static inline void llam_public_active_op_init(_Atomic size_t *active_ops) {
    atomic_init(active_ops, 0U);
}

static inline int llam_public_active_op_try_begin(_Atomic size_t *active_ops) {
    size_t current;
    size_t previous;

    if (active_ops == NULL) {
        return 0;
    }
    current = atomic_load_explicit(active_ops, memory_order_relaxed);
    if (LLAM_UNLIKELY(current >= LLAM_PUBLIC_ACTIVE_OP_BEGIN_LIMIT)) {
        /*
         * The final low-half value and the high half are reserved sentinel
         * space. A begin from the last valid count would increment into that
         * range, and a later end could decrement it back into an apparently
         * valid count. Publish the permanent busy sentinel before arithmetic.
         */
        atomic_store_explicit(active_ops, LLAM_PUBLIC_ACTIVE_OP_BUSY_SENTINEL, memory_order_relaxed);
        errno = EBUSY;
        return -1;
    }
    previous = atomic_fetch_add_explicit(active_ops, 1U, memory_order_relaxed);
    if (LLAM_LIKELY(previous < LLAM_PUBLIC_ACTIVE_OP_BEGIN_LIMIT)) {
        return 0;
    }
    atomic_store_explicit(active_ops, LLAM_PUBLIC_ACTIVE_OP_BUSY_SENTINEL, memory_order_relaxed);
    errno = EBUSY;
    return -1;
}

static inline void llam_public_active_op_begin(_Atomic size_t *active_ops) {
    (void)llam_public_active_op_try_begin(active_ops);
}

static inline void llam_public_active_op_end(_Atomic size_t *active_ops) {
    size_t current;
    size_t previous;

    if (active_ops == NULL) {
        return;
    }
    current = atomic_load_explicit(active_ops, memory_order_relaxed);
    if (current == 0U) {
        return;
    }
    if (LLAM_UNLIKELY(current >= LLAM_PUBLIC_ACTIVE_OP_RESERVED_THRESHOLD)) {
        /*
         * Saturated/corrupt counters are permanent EBUSY sentinels. Do not use
         * fetch_sub on that state: SIZE_MAX - 1 would look like a huge but
         * different active count instead of the canonical sentinel.
         */
        atomic_store_explicit(active_ops, LLAM_PUBLIC_ACTIVE_OP_BUSY_SENTINEL, memory_order_relaxed);
        return;
    }
    previous = atomic_fetch_sub_explicit(active_ops, 1U, memory_order_relaxed);
    if (LLAM_LIKELY(previous > 0U && previous < LLAM_PUBLIC_ACTIVE_OP_RESERVED_THRESHOLD)) {
        return;
    }
    if (previous == 0U) {
        (void)atomic_fetch_add_explicit(active_ops, 1U, memory_order_relaxed);
        return;
    }
    atomic_store_explicit(active_ops, LLAM_PUBLIC_ACTIVE_OP_BUSY_SENTINEL, memory_order_relaxed);
}

static inline size_t llam_public_active_op_count(const _Atomic size_t *active_ops) {
    return active_ops != NULL ? atomic_load_explicit(active_ops, memory_order_relaxed) : 0U;
}

static inline bool llam_public_active_op_is_saturated(size_t active_ops) {
    return active_ops >= LLAM_PUBLIC_ACTIVE_OP_RESERVED_THRESHOLD;
}

#endif
