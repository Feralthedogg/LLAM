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
 * @brief Check whether an over-budget task should yield at this safepoint.
 *
 * The automatic path is deliberately pressure-aware in normal mode.  A CPU
 * loop that polls frequently should not pay extra yields when no runnable work,
 * timers, or injected wakeups are waiting behind it.  Strict mode is reserved
 * for tests and diagnostics and therefore bypasses the pressure gate.
 */
static bool llam_safepoint_should_auto_preempt(llam_runtime_t *rt,
                                               llam_shard_t *shard,
                                               llam_task_t *task,
                                               uint64_t now_ns) {
    uint64_t quantum_ns;
    uint64_t elapsed_ns;

    if (rt == NULL ||
        shard == NULL ||
        task == NULL ||
        now_ns == 0U ||
        rt->preempt_mode < LLAM_PREEMPT_AUTO ||
        task->last_started_ns == 0U ||
        now_ns <= task->last_started_ns) {
        return false;
    }

    quantum_ns = rt->preempt_quantum_ns != 0U
                     ? rt->preempt_quantum_ns
                     : llam_slice_ns((llam_task_class_t)atomic_load_explicit(&task->task_class, memory_order_acquire));
    elapsed_ns = now_ns - task->last_started_ns;
    if (rt->preempt_mode != LLAM_PREEMPT_STRICT && quantum_ns <= UINT64_MAX / 2U) {
        quantum_ns *= 2U;
    }
    if (elapsed_ns <= quantum_ns) {
        return false;
    }
    if (rt->preempt_mode == LLAM_PREEMPT_STRICT) {
        return true;
    }

    if (atomic_load_explicit(&shard->inject_depth, memory_order_acquire) != 0U ||
        llam_norm_queue_depth(shard) != 0U ||
        atomic_load_explicit(&shard->timer_count, memory_order_acquire) != 0U ||
        atomic_load_explicit(&shard->event_pending, memory_order_acquire) != 0U ||
        llam_runtime_overflow_depth(rt) != 0U ||
        atomic_load_explicit(&rt->block_pending, memory_order_acquire) != 0U) {
        return true;
    }

    shard->metrics.preempt_suppressed += 1U;
    return false;
}

/**
 * @brief Execute a cooperative task safepoint.
 *
 * Safe points are no-ops outside managed task context.
 */
void llam_task_safepoint(void) {
    llam_shard_t *shard = g_llam_tls_shard;
    llam_task_t *task = g_llam_tls_task;
    llam_runtime_t *rt = &g_llam_runtime;
    uint64_t now_ns = 0U;

    if (shard == NULL || task == NULL) {
        return;
    }

    if (rt->cheap_safepoint == 0U) {
        // Full safepoints do all diagnostic/reclamation work every time.
        llam_task_sample_live_stack(task);
        now_ns = llam_now_ns();
        atomic_store_explicit(&shard->last_safepoint_ns, now_ns, memory_order_relaxed);
        llam_allocator_quiescent(shard);
    } else {
        unsigned clock_period = rt->safepoint_clock_period;

        // Cheap safepoints avoid the clock and allocator work on most calls,
        // trading watchdog precision for lower hot-loop overhead.
        task->safepoint_tick += 1U;
        if (task->safepoint_tick >= clock_period) {
            task->safepoint_tick = 0U;
            now_ns = llam_now_ns();
            atomic_store_explicit(&shard->last_safepoint_ns, now_ns, memory_order_relaxed);
        }
    }

    if ((task->flags & LLAM_TASK_FLAG_NO_PREEMPT) != 0U) {
        return;
    }

    if (g_llam_runtime.forced_yield_every > 0U) {
        // Test/diagnostic knob: make cooperative scheduling deterministic by
        // forcing periodic yields even without explicit user yields.
        if (task->forced_yield_budget == 0U) {
            task->forced_yield_budget = g_llam_runtime.forced_yield_every;
        }
        task->forced_yield_budget -= 1U;
        if (task->forced_yield_budget == 0U) {
            task->forced_yield_budget = g_llam_runtime.forced_yield_every;
            llam_yield();
            return;
        }
    }

    if (llam_safepoint_should_auto_preempt(rt, shard, task, now_ns)) {
        shard->metrics.preempt_requests += 1U;
        shard->metrics.preempt_yields += 1U;
        llam_yield();
        return;
    }

    if (rt->preempt_mode == LLAM_PREEMPT_OFF) {
        return;
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
        shard->metrics.preempt_yields += 1U;
        llam_yield();
    }
}
