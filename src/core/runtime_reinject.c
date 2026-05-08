/**
 * @file src/core/runtime_reinject.c
 * @brief Helpers that requeue runnable tasks after migration or deferred wakeup.
 *
 * @details
 * Reinjecting is the common wake path after timers, joins, synchronization
 * primitives, I/O completions, cancellation, and migration decide a parked task
 * is runnable again. This file centralizes state transitions, queue selection,
 * dynamic-shard reactivation, tracing, and worker wakeup.
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

#if LLAM_RUNTIME_BACKEND_WINDOWS || defined(__linux__)
#define LLAM_REINJECT_DIRECT_OWNER_HANDOFF 1
#else
#define LLAM_REINJECT_DIRECT_OWNER_HANDOFF 0
#endif

/**
 * @brief Mark a task runnable and enqueue it on a shard while the shard lock is held.
 *
 * @param shard        Target shard.
 * @param task         Task to make runnable.
 * @param hot          Whether the task should prefer the hot lane.
 * @param kind         Trace event kind.
 * @param reason       Wait reason being resolved.
 * @param direct_local Whether the caller already runs on @p shard.
 */
void llam_mark_runnable_locked(llam_shard_t *shard,
                                    llam_task_t *task,
                                    bool hot,
                                    llam_trace_kind_t kind,
                                    llam_wait_reason_t reason,
                                    bool direct_local) {
    llam_task_state_id_t from = task->state;
    if (shard->runtime->experimental_dynamic_shards != 0U &&
        atomic_load_explicit(&shard->online, memory_order_relaxed) == 0U) {
        // Waking work onto an offline dynamic shard brings it back online and
        // updates the runtime's min/max online-shard diagnostics.
        atomic_store_explicit(&shard->online, 1U, memory_order_release);
        llam_runtime_note_online_shards(shard->runtime,
                                      atomic_fetch_add_explicit(&shard->runtime->online_shards, 1U, memory_order_acq_rel) + 1U);
    }

    task->state = LLAM_TASK_STATE_RUNNABLE;
    task->wait_reason = LLAM_WAIT_NONE;
    task->enqueue_hot = hot ? 1U : 0U;
    task->last_runnable_ns = shard->runtime->wake_latency_metrics_enabled != 0U ? llam_now_ns() : 0U;

    if (shard->opaque_redirect_active) {
        // A shard in opaque-block redirect mode should not receive local work;
        // send the task to its redirect target or spill to overflow.
        shard->metrics.migrations += 1U;
        if (!llam_enqueue_opaque_redirect_task_locked(shard, task, hot)) {
            task->enqueue_hot = 0U;
            llam_enqueue_overflow_task(shard->runtime, task);
        }
    } else if (direct_local) {
        // Same-shard wake can bypass inject_q because the owner lock is held and
        // the current worker will immediately see local queue state.
        task->enqueue_hot = 0U;
        if (hot) {
            if (llam_queue_push_bounded_locked(shard, &shard->hot_q, LLAM_HOT_QUEUE_CAP, task)) {
                shard->metrics.hot_enqueues += 1U;
            }
        } else {
            (void)llam_norm_queue_push_owner_locked(shard, task);
        }
    } else if (llam_queue_push_bounded_locked(shard, &shard->inject_q, LLAM_INJECT_QUEUE_CAP, task)) {
        shard->metrics.inject_enqueues += 1U;
    }
    shard->metrics.wakes += 1U;
    if (reason > LLAM_WAIT_NONE && reason <= LLAM_WAIT_TIMEOUT) {
        shard->metrics.wake_reason_hist[reason] += 1U;
    }
    llam_trace_shard(shard, task, kind, from, LLAM_TASK_STATE_RUNNABLE, reason);
}

/**
 * @brief Reinject a task onto a suitable runnable shard.
 *
 * @param rt     Runtime owning the task.
 * @param task   Task to wake.
 * @param hot    Whether the task should prefer hot-lane dispatch.
 * @param kind   Trace event kind.
 * @param reason Wait reason being resolved.
 */
void llam_reinject_task(llam_runtime_t *rt,
                             llam_task_t *task,
                             bool hot,
                             llam_trace_kind_t kind,
                             llam_wait_reason_t reason) {
    unsigned target_id = llam_pick_runnable_shard(rt, task);

    llam_reinject_task_on_shard(rt, task, target_id, hot, kind, reason);
}

/**
 * @brief Reinject a task onto a specific shard, falling back if unavailable.
 *
 * @param rt        Runtime owning the task.
 * @param task      Task to wake.
 * @param target_id Preferred target shard id.
 * @param hot       Whether the task should prefer hot-lane dispatch.
 * @param kind      Trace event kind.
 * @param reason    Wait reason being resolved.
 */
void llam_reinject_task_on_shard(llam_runtime_t *rt,
                                      llam_task_t *task,
                                      unsigned target_id,
                                      bool hot,
                                      llam_trace_kind_t kind,
                                      llam_wait_reason_t reason) {
    llam_shard_t *target;
    bool pressure;
    bool effective_hot;
    bool direct_local;

    if (rt == NULL || task == NULL || rt->active_shards == 0U) {
        return;
    }
    if (target_id >= rt->active_shards || !llam_shard_accepts_new_work(&rt->shards[target_id])) {
        target_id = llam_pick_runnable_shard(rt, task);
    }

    target = &rt->shards[target_id];
    pressure = llam_runtime_pressure_signal(rt);
    effective_hot = hot || task->task_class == LLAM_TASK_CLASS_LATENCY;
    direct_local = (g_llam_tls_shard == target && g_llam_tls_task != NULL);

    if (task->active_timer != NULL) {
        llam_disarm_task_wait_deadline(task);
    }
    // Wake ownership transfers from the wait primitive/I/O request back to the
    // scheduler queue before the task becomes runnable.
    llam_task_clear_wait_tracking(task);
    pthread_mutex_lock(&target->lock);
    effective_hot = llam_should_enqueue_hot_locked(target, task, effective_hot, pressure);
    llam_mark_runnable_locked(target, task, effective_hot, kind, reason, direct_local);
    pthread_mutex_unlock(&target->lock);
    if (!direct_local) {
        llam_kick_shard(target);
    }
}

/**
 * @brief Wake a peer task and yield the current task in one same-shard handoff.
 *
 * @param rt        Runtime owning both tasks.
 * @param task      Peer task to make runnable.
 * @param target_id Required target shard id.
 * @param hot       Whether the peer should prefer hot-lane dispatch.
 * @param kind      Trace event kind for the peer wake.
 * @param reason    Wait reason being resolved.
 * @return true if the handoff happened, false if normal wake should be used.
 */
bool llam_reinject_task_on_shard_and_yield_current(llam_runtime_t *rt,
                                                 llam_task_t *task,
                                                 unsigned target_id,
                                                 bool hot,
                                                 llam_trace_kind_t kind,
                                                 llam_wait_reason_t reason) {
    llam_shard_t *target;
    llam_task_t *current = g_llam_tls_task;
    llam_task_state_id_t task_from;
    bool effective_hot;
    int caller_errno = errno;

    if (rt == NULL || task == NULL || current == NULL || rt->active_shards == 0U ||
        target_id >= rt->active_shards || !llam_shard_accepts_new_work(&rt->shards[target_id])) {
        return false;
    }

    target = &rt->shards[target_id];
    if (g_llam_tls_shard != target || current == task) {
        return false;
    }

    if (rt->run_timing_enabled != 0U || rt->wake_latency_metrics_enabled != 0U ||
        task->active_timer != NULL) {
        return false;
    }

    effective_hot = hot || task->task_class == LLAM_TASK_CLASS_LATENCY;

    llam_task_clear_wait_tracking(task);

#if LLAM_REINJECT_DIRECT_OWNER_HANDOFF
    if (g_llam_tls_scheduler_ctx == &target->scheduler_ctx &&
        rt->trace_events_enabled == 0U &&
        rt->run_timing_enabled == 0U &&
        rt->wake_latency_metrics_enabled == 0U &&
        llam_lockfree_normq_enabled(rt) &&
        !target->opaque_redirect_active &&
        atomic_load_explicit(&target->timer_count, memory_order_acquire) == 0U) {
        current->forced_yield_budget = rt->forced_yield_every;
        current->state = LLAM_TASK_STATE_RUNNABLE;
        current->wait_reason = LLAM_WAIT_NONE;
        current->last_yield_ns = 0U;
        if (llam_norm_queue_push_yield_unlocked(target, current)) {
            task_from = task->state;
            task->state = LLAM_TASK_STATE_RUNNING;
            task->wait_reason = LLAM_WAIT_NONE;
            task->enqueue_hot = effective_hot ? 1U : 0U;
            task->last_shard = target->id;
            task->last_started_ns = 0U;
            atomic_store_explicit(&target->current, task, memory_order_release);
            g_llam_tls_task = task;
            target->metrics.wakes += 1U;
            if (reason > LLAM_WAIT_NONE && reason <= LLAM_WAIT_TIMEOUT) {
                target->metrics.wake_reason_hist[reason] += 1U;
            }
            target->metrics.yields += 1U;
            target->metrics.ctx_switches += 1U;
            (void)kind;
            (void)task_from;
            errno = caller_errno;
            llam_switch_task_to_task_hot(current, task);
            return true;
        }
    }
#endif

    pthread_mutex_lock(&target->lock);
    if (target->opaque_redirect_active || target->norm_q.depth >= LLAM_NORM_QUEUE_CAP) {
        pthread_mutex_unlock(&target->lock);
        return false;
    }

    current->forced_yield_budget = rt->forced_yield_every;
    current->state = LLAM_TASK_STATE_RUNNABLE;
    current->wait_reason = LLAM_WAIT_NONE;
    current->last_yield_ns = 0U;

    effective_hot = llam_should_enqueue_hot_locked(target, task, effective_hot, false);
    // Put the yielding task on the normal FIFO side so the just-woken peer can
    // run promptly without starving older normal work.
    (void)llam_norm_queue_push_yield_locked(target, current);
    task_from = task->state;
    task->state = LLAM_TASK_STATE_RUNNING;
    task->wait_reason = LLAM_WAIT_NONE;
    task->enqueue_hot = effective_hot ? 1U : 0U;
    task->last_shard = target->id;
    task->last_started_ns = 0U;
    atomic_store_explicit(&target->current, task, memory_order_release);
    g_llam_tls_task = task;
    target->metrics.wakes += 1U;
    if (reason > LLAM_WAIT_NONE && reason <= LLAM_WAIT_TIMEOUT) {
        target->metrics.wake_reason_hist[reason] += 1U;
    }
    target->metrics.yields += 1U;
    target->metrics.ctx_switches += 1U;
    llam_trace_shard(target, task, kind, task_from, LLAM_TASK_STATE_RUNNING, reason);
    llam_trace_shard(target, current, LLAM_TRACE_STATE, LLAM_TASK_STATE_RUNNING, LLAM_TASK_STATE_RUNNABLE, LLAM_WAIT_YIELD);
    pthread_mutex_unlock(&target->lock);

    errno = caller_errno;
    llam_switch_task_to_task_hot(current, task);
    return true;
}
