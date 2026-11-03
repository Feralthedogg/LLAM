/*
 * Copyright 2026 Feralthedogg
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
 * See LICENSES/Apache-2.0.txt.
 */

#include "engine/runtime_watchdog_internal.h"

#include <stdlib.h>

#define LLAM_DEADLOCK_SCAN_TASK_LIMIT 4096U
#define LLAM_DEADLOCK_SCAN_PROCESSOR_LIMIT 64U

_Static_assert(LLAM_DEADLOCK_SCAN_TASK_LIMIT <= SIZE_MAX / sizeof(llam_wait_dependency_t),
               "bounded deadlock scan allocation must fit size_t");

static bool llam_deadlock_contains(const llam_wait_dependency_t *waits, size_t count,
                                   uintptr_t task) {
    for (size_t i = 0U; i < count; ++i) {
        if ((uintptr_t)waits[i].task == task) {
            return true;
        }
    }
    return false;
}

/* Called only by the controller, with every P's lock pair held. */
static bool llam_deadlock_native_quiet(llam_runtime_t *runtime) {
    if (atomic_load_explicit(&runtime->async_preempt_enabled, memory_order_acquire) == 0U) {
        return true;
    }
#if LLAM_ASYNC_CONTEXT_SHARED
    return llam_async_shared_liveness_quiet_pair_locked(runtime);
#else
    /* An adapter without a precise operation snapshot cannot certify deadlock. */
    return false;
#endif
}

static uint64_t llam_deadlock_progress_locked(llam_runtime_t *runtime) {
    uint64_t progress = llam_runtime_live_tasks(runtime);
    for (unsigned i = 0U; i < runtime->active_shards; ++i) {
        progress += llam_watchdog_metrics_progress_sum(&runtime->shards[i].metrics);
    }
    return progress;
}

bool llam_watchdog_confirm_deadlock(llam_runtime_t *runtime, uint64_t expected_progress) {
    llam_wait_dependency_t *waits;
    size_t count = 0U;
    unsigned lease_locks = 0U;
    unsigned payload_locks = 0U;
    unsigned live;
    size_t visited = 0U;
    bool overflow_locked = false;
    bool confirmed = false;

    if (runtime == NULL || runtime->shards == NULL || runtime->active_shards == 0U ||
        runtime->worker_slots == NULL ||
        !atomic_load_explicit(&runtime->initialized, memory_order_acquire) ||
        runtime->active_shards > LLAM_DEADLOCK_SCAN_PROCESSOR_LIMIT ||
        runtime->worker_slot_count < (size_t)runtime->active_shards * 2U ||
        runtime->worker_slot_count > LLAM_DEADLOCK_SCAN_TASK_LIMIT +
                                     2U * LLAM_DEADLOCK_SCAN_PROCESSOR_LIMIT ||
        atomic_load_explicit(&runtime->stop_requested, memory_order_acquire) ||
        atomic_load_explicit(&runtime->shutdown_requested, memory_order_acquire)) {
        return false;
    }
    live = llam_runtime_live_tasks(runtime);
    if (live == 0U || live > LLAM_DEADLOCK_SCAN_TASK_LIMIT) {
        return false;
    }
    /* All allocation happens before freezing P. Failure/truncation is inconclusive. */
    waits = calloc(live, sizeof(*waits));
    if (waits == NULL) {
        return false;
    }
    if (runtime->overflow_lock_initialized) {
        if (pthread_mutex_trylock(&runtime->overflow_lock) != 0) {
            goto done;
        }
        overflow_locked = true;
        if (runtime->overflow_q.depth != 0U) {
            goto done;
        }
    }
    /* Never wait on a worker: acquire all leases, then payloads in P-ID order. */
    for (; lease_locks < runtime->active_shards; ++lease_locks) {
        llam_shard_t *shard = &runtime->shards[lease_locks];
        if (!shard->opaque_lock_initialized || pthread_mutex_trylock(&shard->opaque_lock) != 0) {
            goto done;
        }
    }
    for (; payload_locks < runtime->active_shards; ++payload_locks) {
        llam_shard_t *shard = &runtime->shards[payload_locks];
        if (!shard->lock_initialized || pthread_mutex_trylock(&shard->lock) != 0) {
            goto done;
        }
    }
    if (llam_deadlock_progress_locked(runtime) != expected_progress ||
        atomic_load_explicit(&runtime->block_pending, memory_order_acquire) != 0U ||
        !llam_deadlock_native_quiet(runtime)) {
        goto done;
    }
    for (size_t i = 0U; i < runtime->worker_slot_count; ++i) {
        if (llam_worker_load_active_g(runtime->worker_slots[i]) != NULL) {
            goto done;
        }
    }
    for (unsigned i = 0U; i < runtime->active_shards; ++i) {
        llam_shard_t *shard = &runtime->shards[i];
        llam_processor_t *processor = &shard->processor;
        if (processor->inject_q.depth != 0U || processor->hot_q.depth != 0U ||
            llam_norm_queue_depth(shard) != 0U || llam_async_preempt_ready_depth(shard) != 0U ||
            processor->timers != NULL ||
            atomic_load_explicit(&processor->timer_callbacks_active, memory_order_acquire) != 0U ||
            atomic_load_explicit(&shard->inflight_io_waiters, memory_order_acquire) != 0U) {
            goto done;
        }
        for (llam_task_t *task = shard->all_tasks; task != NULL; task = task->all_next) {
            if (++visited > LLAM_DEADLOCK_SCAN_TASK_LIMIT) {
                goto done;
            }
            unsigned state = atomic_load_explicit(&task->state, memory_order_acquire);
            if (state == LLAM_TASK_STATE_DEAD) {
                /* A retained completed handle is not a runnable continuation.
                 * The backend quiescence check above covers native retirement. */
                continue;
            }
            if (count == live || state != LLAM_TASK_STATE_PARKED ||
                !atomic_load_explicit(&task->task_listed, memory_order_acquire)) {
                goto done;
            }
            waits[count++].task = task;
        }
    }
    if (count != live || llam_runtime_live_tasks(runtime) != live) {
        goto done;
    }
    for (size_t i = 0U; i < count; ++i) {
        if (!llam_task_wait_dependency_read_locked(runtime, waits[i].task, count, &waits[i]) ||
            !llam_deadlock_contains(waits, count, waits[i].owner)) {
            goto done;
        }
    }
    /* Every live G depends on another G in this finite parked set. Therefore
     * at least one closed cycle exists; no separate graph/DFS allocation is needed.
     * Mutex unlock is owner-only and every join edge is checked against its queue. */
    for (size_t i = 0U; i < count; ++i) {
        if (!llam_task_wait_dependency_matches(&waits[i])) {
            goto done;
        }
    }
    confirmed = !atomic_load_explicit(&runtime->stop_requested, memory_order_acquire) &&
                !atomic_load_explicit(&runtime->shutdown_requested, memory_order_acquire) &&
                llam_deadlock_progress_locked(runtime) == expected_progress &&
                llam_runtime_live_tasks(runtime) == live && llam_deadlock_native_quiet(runtime);
done:
    for (size_t i = 0U; i < count; ++i) {
        if (waits[i].pinned) {
            llam_task_wait_resolver_end(waits[i].task);
        }
    }
    while (payload_locks != 0U) {
        --payload_locks;
        pthread_mutex_unlock(&runtime->shards[payload_locks].lock);
    }
    while (lease_locks != 0U) {
        --lease_locks;
        pthread_mutex_unlock(&runtime->shards[lease_locks].opaque_lock);
    }
    if (overflow_locked) {
        pthread_mutex_unlock(&runtime->overflow_lock);
    }
    free(waits);
    return confirmed;
}
