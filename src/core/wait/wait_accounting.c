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
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
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
        llam_record_fatal(rt, EOVERFLOW);
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
    llam_record_fatal(rt, EINVAL);
    return false;
}
