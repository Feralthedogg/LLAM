/**
 * @file src/core/runtime_safepoint.c
 * @brief Cooperative safepoint and preemption accounting helpers.
 *
 * @details
 * Managed tasks call safepoints from long-running loops and runtime wait paths.
 * A safepoint records liveness for the watchdog, optionally samples stack
 * usage, drains allocator remote frees, honors forced-yield testing policy, and
 * responds to async preemption requests.
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

/**
 * @brief Execute a cooperative task safepoint.
 *
 * Safe points are no-ops outside managed task context.
 */
void nm_task_safepoint(void) {
    nm_shard_t *shard = g_nm_tls_shard;
    nm_task_t *task = g_nm_tls_task;
    nm_runtime_t *rt = &g_nm_runtime;

    if (shard == NULL || task == NULL) {
        return;
    }

    if (rt->cheap_safepoint == 0U) {
        // Full safepoints do all diagnostic/reclamation work every time.
        nm_task_sample_live_stack(task);
        atomic_store_explicit(&shard->last_safepoint_ns, nm_now_ns(), memory_order_relaxed);
        nm_allocator_quiescent(shard);
    } else {
        unsigned clock_period = rt->safepoint_clock_period;

        // Cheap safepoints avoid the clock and allocator work on most calls,
        // trading watchdog precision for lower hot-loop overhead.
        task->safepoint_tick += 1U;
        if (task->safepoint_tick >= clock_period) {
            task->safepoint_tick = 0U;
            atomic_store_explicit(&shard->last_safepoint_ns, nm_now_ns(), memory_order_relaxed);
        }
    }

    if ((task->flags & NM_TASK_FLAG_NO_PREEMPT) != 0U) {
        return;
    }

    if (g_nm_runtime.forced_yield_every > 0U) {
        // Test/diagnostic knob: make cooperative scheduling deterministic by
        // forcing periodic yields even without explicit user yields.
        if (task->forced_yield_budget == 0U) {
            task->forced_yield_budget = g_nm_runtime.forced_yield_every;
        }
        task->forced_yield_budget -= 1U;
        if (task->forced_yield_budget == 0U) {
            task->forced_yield_budget = g_nm_runtime.forced_yield_every;
            nm_yield();
            return;
        }
    }

    if (rt->cheap_safepoint != 0U) {
        unsigned preempt_period = rt->preempt_poll_period;

        if (preempt_period > 1U) {
            task->preempt_poll_tick += 1U;
            if (task->preempt_poll_tick < preempt_period) {
                return;
            }
            task->preempt_poll_tick = 0U;
        }
    }

    if (atomic_load_explicit(&task->preempt_requested, memory_order_relaxed) != 0U &&
        atomic_exchange_explicit(&task->preempt_requested, 0U, memory_order_acq_rel) != 0U) {
        // Consume one preempt request and yield exactly once for this safepoint.
        nm_yield();
    }
}
