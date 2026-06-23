/**
 * @file src/core/sched/norm_queue_depth.c
 * @brief Normal queue depth accounting guards.
 *
 * @details
 * Runnable queue storage and depth hints are intentionally separate so lock-free
 * owner/thief paths can inspect approximate backlog without taking shard locks.
 * These helpers keep that hint fail-closed when an impossible saturated or
 * underflowed value is observed.
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

bool llam_norm_queue_note_enqueue(llam_shard_t *shard) {
    unsigned current;

    if (shard == NULL) {
        return false;
    }
    current = atomic_load_explicit(&shard->norm_depth, memory_order_acquire);
    while (current != UINT_MAX) {
        if (atomic_compare_exchange_weak_explicit(&shard->norm_depth,
                                                  &current,
                                                  current + 1U,
                                                  memory_order_release,
                                                  memory_order_acquire)) {
            return true;
        }
    }
    if (LLAM_UNLIKELY(current == UINT_MAX)) {
        llam_record_fatal(shard->runtime, EOVERFLOW);
        return false;
    }
    return false;
}

bool llam_norm_queue_note_dequeue(llam_shard_t *shard) {
    unsigned current;

    if (shard == NULL) {
        return false;
    }
    current = atomic_load_explicit(&shard->norm_depth, memory_order_acquire);
    while (current != 0U) {
        if (atomic_compare_exchange_weak_explicit(&shard->norm_depth,
                                                  &current,
                                                  current - 1U,
                                                  memory_order_release,
                                                  memory_order_acquire)) {
            return true;
        }
    }
    if (LLAM_UNLIKELY(current == 0U)) {
        llam_record_fatal(shard->runtime, EINVAL);
        return false;
    }
    return false;
}
