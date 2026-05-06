/**
 * @file src/engine/runtime_watchdog_probe.c
 * @brief Watchdog probing logic for scheduler and I/O progress signals.
 *
 * @details
 * Probe helpers provide cheap yes/no signals used by the controller thread:
 * runnable work, timer backlog, pending I/O, opaque blocking compensation, and
 * per-shard load estimates. They deliberately avoid mutating scheduler state
 * except for preemption requests made by ::llam_watchdog_check_shard.
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

#include "runtime_watchdog_internal.h"

/**
 * @brief Check whether dynamic-shard diagnostic tracing is enabled.
 *
 * @return @c true when @c LLAM_DYNAMIC_TRACE is set to a non-zero value.
 */
bool llam_dynamic_trace_enabled(void) {
    static atomic_int cached = ATOMIC_VAR_INIT(-1);
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        const char *env = llam_env_get("LLAM_DYNAMIC_TRACE");

        value = (env != NULL && env[0] != '\0' && strcmp(env, "0") != 0) ? 1 : 0;
        atomic_store_explicit(&cached, value, memory_order_release);
    }
    return value != 0;
}

/**
 * @brief Inspect a running shard for missed safepoints.
 *
 * Opaque-blocking tasks are ignored because their compensation path is expected
 * to keep the runtime productive while the primary thread is in foreign code.
 * Other long-running tasks receive a preemption signal when they exceed their
 * slice budget without a safepoint.
 *
 * @param shard  Shard to inspect.
 * @param now_ns Current monotonic timestamp.
 */
void llam_watchdog_check_shard(llam_shard_t *shard, uint64_t now_ns) {
    llam_task_t *current;

    pthread_mutex_lock(&shard->lock);
    current = atomic_load_explicit(&shard->current, memory_order_acquire);
    if (current != NULL) {
        uint64_t last_safepoint = atomic_load_explicit(&shard->last_safepoint_ns, memory_order_relaxed);
        uint64_t slice_ns = llam_slice_ns(current->task_class);

        if (current->opaque_blocking_depth > 0U || current->state == LLAM_TASK_STATE_BLOCKED_OPAQUE) {
            pthread_mutex_unlock(&shard->lock);
            return;
        }

        if (last_safepoint > 0U && now_ns > last_safepoint && now_ns - last_safepoint > slice_ns * 2U) {
            if (atomic_exchange(&current->preempt_requested, 1U) == 0U) {
                shard->metrics.watchdog_hits += 1U;
                shard->metrics.long_no_safepoint += 1U;
                llam_trace_shard(shard,
                               current,
                               LLAM_TRACE_WATCHDOG,
                               LLAM_TASK_STATE_RUNNING,
                               LLAM_TASK_STATE_RUNNING,
                               LLAM_WAIT_NONE);
                (void)pthread_kill(shard->thread, LLAM_PREEMPT_SIGNAL);
            }
        }
    }
    pthread_mutex_unlock(&shard->lock);
}

/**
 * @brief Check whether any shard has pending timers.
 *
 * @param rt Runtime to inspect.
 *
 * @return @c true when at least one timer root is present.
 */
bool llam_runtime_has_pending_timers(llam_runtime_t *rt) {
    unsigned i;

    for (i = 0; i < rt->active_shards; ++i) {
        bool has_timer;

        pthread_mutex_lock(&rt->shards[i].lock);
        has_timer = rt->shards[i].timers != NULL;
        pthread_mutex_unlock(&rt->shards[i].lock);
        if (has_timer) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Check whether the runtime has runnable work or currently running tasks.
 *
 * @param rt Runtime to inspect.
 *
 * @return @c true when work exists outside parked/timer/I/O-only states.
 */
bool llam_runtime_has_runnable_work(llam_runtime_t *rt) {
    unsigned i;

    if (rt->overflow_lock_initialized) {
        bool has_overflow;

        pthread_mutex_lock(&rt->overflow_lock);
        has_overflow = rt->overflow_q.depth > 0U;
        pthread_mutex_unlock(&rt->overflow_lock);
        if (has_overflow) {
            return true;
        }
    }

    for (i = 0; i < rt->active_shards; ++i) {
        bool has_work;

        pthread_mutex_lock(&rt->shards[i].lock);
        has_work = rt->shards[i].inject_q.depth > 0U || rt->shards[i].hot_q.depth > 0U || llam_norm_queue_depth(&rt->shards[i]) > 0U ||
                   atomic_load_explicit(&rt->shards[i].current, memory_order_acquire) != NULL;
        pthread_mutex_unlock(&rt->shards[i].lock);
        if (has_work) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Check whether runnable queues have backlog, excluding current tasks.
 *
 * @param rt Runtime to inspect.
 *
 * @return @c true when any overflow, inject, hot, or normal queue is non-empty.
 */
bool llam_runtime_has_runnable_backlog(llam_runtime_t *rt) {
    unsigned i;

    if (rt == NULL) {
        return false;
    }

    if (rt->overflow_lock_initialized) {
        bool has_overflow;

        pthread_mutex_lock(&rt->overflow_lock);
        has_overflow = rt->overflow_q.depth > 0U;
        pthread_mutex_unlock(&rt->overflow_lock);
        if (has_overflow) {
            return true;
        }
    }

    for (i = 0; i < rt->active_shards; ++i) {
        bool has_backlog;

        pthread_mutex_lock(&rt->shards[i].lock);
        has_backlog = rt->shards[i].inject_q.depth > 0U ||
                      rt->shards[i].hot_q.depth > 0U ||
                      llam_norm_queue_depth(&rt->shards[i]) > 0U;
        pthread_mutex_unlock(&rt->shards[i].lock);
        if (has_backlog) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Check whether any I/O node has pending backend operations.
 *
 * @param rt Runtime to inspect.
 *
 * @return @c true when a node pending counter is non-zero.
 */
bool llam_runtime_has_io_pending(llam_runtime_t *rt) {
    unsigned i;

    for (i = 0; i < rt->active_nodes; ++i) {
        if (atomic_load(&rt->nodes[i].pending_ops) > 0U) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Return the runtime-wide count of active I/O waiters.
 *
 * @param rt Runtime to inspect.
 *
 * @return Active I/O waiter count, or 0 for @c NULL.
 */
unsigned llam_runtime_active_io_waiters(llam_runtime_t *rt) {
    if (rt == NULL) {
        return 0U;
    }
    return atomic_load_explicit(&rt->active_io_waiters, memory_order_acquire);
}

/**
 * @brief Check whether any shard is currently using opaque-block compensation.
 *
 * @param rt Runtime to inspect.
 *
 * @return @c true when helper or redirect compensation is active.
 */
bool llam_runtime_has_opaque_blocking(llam_runtime_t *rt) {
    unsigned i;

    for (i = 0; i < rt->active_shards; ++i) {
        bool has_opaque;

        pthread_mutex_lock(&rt->shards[i].opaque_lock);
        has_opaque = rt->shards[i].opaque_compensation_depth > 0U || rt->shards[i].opaque_redirect_depth > 0U ||
                     rt->shards[i].opaque_helper_active;
        pthread_mutex_unlock(&rt->shards[i].opaque_lock);
        if (has_opaque) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Estimate shard load for merge-target and scaling decisions.
 *
 * The estimate includes queued work, the current task, a timer root, and a large
 * penalty for active opaque redirect because redirecting shards should not be
 * selected as quiet merge targets.
 *
 * @param shard Shard to inspect.
 *
 * @return Load estimate, or @c UINT_MAX for @c NULL.
 */
unsigned llam_watchdog_snapshot_shard_load(llam_shard_t *shard) {
    unsigned load = 0U;

    if (shard == NULL) {
        return UINT_MAX;
    }

    pthread_mutex_lock(&shard->lock);
    load = shard->inject_q.depth + shard->hot_q.depth + llam_norm_queue_depth(shard);
    if (atomic_load_explicit(&shard->current, memory_order_acquire) != NULL) {
        load += 1U;
    }
    if (shard->timers != NULL) {
        load += 1U;
    }
    if (shard->opaque_redirect_active) {
        load += LLAM_INJECT_QUEUE_CAP;
    }
    pthread_mutex_unlock(&shard->lock);
    return load;
}
