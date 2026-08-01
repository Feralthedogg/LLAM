/**
 * @file src/core/wait/wait_accounting.c
 * @brief Saturating wait-pressure accounting helpers.
 *
 * @details
 * Wait ownership fields are the source of truth, but runtime-wide counters are
 * used by shutdown, scaling, and diagnostics as pressure hints.  These helpers
 * keep corrupted or saturated counters fail-closed so they never wrap through
 * zero and misclassify a busy runtime as idle.
 *
 * @copyright Copyright 2026 Feralthedogg
 *
 * @par License
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 * Licensed under the LLAM Commercial Reciprocity License 1.0.
 * See the LICENSE file distributed with this Software.
 */

#include "runtime_internal.h"

bool llam_runtime_note_active_io_waiter(llam_runtime_t *rt, int delta) {
    unsigned current;

    if (rt == NULL || delta == 0) {
        return false;
    }
    current = atomic_load_explicit(&rt->active_io_waiters, memory_order_acquire);
    if (delta > 0) {
        while (current != UINT_MAX) {
            if (atomic_compare_exchange_weak_explicit(&rt->active_io_waiters,
                                                      &current,
                                                      current + 1U,
                                                      memory_order_acq_rel,
                                                      memory_order_acquire)) {
                return true;
            }
        }
        /* May run while an owner queue lock is held; shutdown is deferred. */
        llam_record_fatal_deferred(rt, EOVERFLOW);
        return false;
    }

    while (current != 0U) {
        if (atomic_compare_exchange_weak_explicit(&rt->active_io_waiters,
                                                  &current,
                                                  current - 1U,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            return true;
        }
    }
    /* May run while an owner queue lock is held; shutdown is deferred. */
    llam_record_fatal_deferred(rt, EINVAL);
    return false;
}
