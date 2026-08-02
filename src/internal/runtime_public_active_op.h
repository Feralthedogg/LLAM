/**
 * @file src/internal/runtime_public_active_op.h
 * @brief Active public-operation guard helpers for opaque handles.
 *
 * @copyright Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: Apache-2.0
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * See LICENSES/OLD-LICENSE/Apache-2.0.txt.
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

    if (active_ops == NULL) {
        return;
    }
    current = atomic_load_explicit(active_ops, memory_order_relaxed);
    for (;;) {
        if (current == 0U) {
            return;
        }
        if (LLAM_UNLIKELY(current >= LLAM_PUBLIC_ACTIVE_OP_RESERVED_THRESHOLD)) {
            /*
             * Saturated/corrupt counters are permanent EBUSY sentinels. Do not
             * decrement that state into an apparently ordinary active count.
             */
            atomic_store_explicit(active_ops,
                                  LLAM_PUBLIC_ACTIVE_OP_BUSY_SENTINEL,
                                  memory_order_relaxed);
            return;
        }
        /*
         * Release publishes every protected scalar/object access before its
         * pin is dropped. Consecutive release RMWs form release sequences, so
         * an acquire load that observes the final zero imports all completed
         * public operations before destructive reuse begins.
         */
        if (atomic_compare_exchange_weak_explicit(active_ops,
                                                  &current,
                                                  current - 1U,
                                                  memory_order_release,
                                                  memory_order_relaxed)) {
            return;
        }
    }
}

static inline size_t llam_public_active_op_count(const _Atomic size_t *active_ops) {
    /*
     * Destructive users wait for zero through this helper. Acquire pairs with
     * the release decrement that published the final protected access.
     */
    return active_ops != NULL ? atomic_load_explicit(active_ops, memory_order_acquire) : 0U;
}

static inline bool llam_public_active_op_is_saturated(size_t active_ops) {
    return active_ops >= LLAM_PUBLIC_ACTIVE_OP_RESERVED_THRESHOLD;
}

#endif
