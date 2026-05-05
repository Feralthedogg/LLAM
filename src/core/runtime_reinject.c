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
void nm_mark_runnable_locked(nm_shard_t *shard,
                                    nm_task_t *task,
                                    bool hot,
                                    nm_trace_kind_t kind,
                                    nm_wait_reason_t reason,
                                    bool direct_local) {
    nm_task_state_id_t from = task->state;
    if (shard->runtime->experimental_dynamic_shards != 0U &&
        atomic_load_explicit(&shard->online, memory_order_relaxed) == 0U) {
        // Waking work onto an offline dynamic shard brings it back online and
        // updates the runtime's min/max online-shard diagnostics.
        atomic_store_explicit(&shard->online, 1U, memory_order_release);
        nm_runtime_note_online_shards(shard->runtime,
                                      atomic_fetch_add_explicit(&shard->runtime->online_shards, 1U, memory_order_acq_rel) + 1U);
    }

    task->state = NM_TASK_STATE_RUNNABLE;
    task->wait_reason = NM_WAIT_NONE;
    task->enqueue_hot = hot ? 1U : 0U;
    task->last_runnable_ns = shard->runtime->wake_latency_metrics_enabled != 0U ? nm_now_ns() : 0U;

    if (shard->opaque_redirect_active) {
        // A shard in opaque-block redirect mode should not receive local work;
        // send the task to its redirect target or spill to overflow.
        shard->metrics.migrations += 1U;
        if (!nm_enqueue_opaque_redirect_task_locked(shard, task, hot)) {
            task->enqueue_hot = 0U;
            nm_enqueue_overflow_task(shard->runtime, task);
        }
    } else if (direct_local) {
        // Same-shard wake can bypass inject_q because the owner lock is held and
        // the current worker will immediately see local queue state.
        task->enqueue_hot = 0U;
        if (hot) {
            if (nm_queue_push_bounded_locked(shard, &shard->hot_q, NM_HOT_QUEUE_CAP, task)) {
                shard->metrics.hot_enqueues += 1U;
            }
        } else {
            (void)nm_norm_queue_push_owner_locked(shard, task);
        }
    } else if (nm_queue_push_bounded_locked(shard, &shard->inject_q, NM_INJECT_QUEUE_CAP, task)) {
        shard->metrics.inject_enqueues += 1U;
    }
    shard->metrics.wakes += 1U;
    if (reason > NM_WAIT_NONE && reason <= NM_WAIT_TIMEOUT) {
        shard->metrics.wake_reason_hist[reason] += 1U;
    }
    nm_trace_shard(shard, task, kind, from, NM_TASK_STATE_RUNNABLE, reason);
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
void nm_reinject_task(nm_runtime_t *rt,
                             nm_task_t *task,
                             bool hot,
                             nm_trace_kind_t kind,
                             nm_wait_reason_t reason) {
    unsigned target_id = nm_pick_runnable_shard(rt, task);

    nm_reinject_task_on_shard(rt, task, target_id, hot, kind, reason);
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
void nm_reinject_task_on_shard(nm_runtime_t *rt,
                                      nm_task_t *task,
                                      unsigned target_id,
                                      bool hot,
                                      nm_trace_kind_t kind,
                                      nm_wait_reason_t reason) {
    nm_shard_t *target;
    bool pressure;
    bool effective_hot;
    bool direct_local;

    if (rt == NULL || task == NULL || rt->active_shards == 0U) {
        return;
    }
    if (target_id >= rt->active_shards || !nm_shard_accepts_new_work(&rt->shards[target_id])) {
        target_id = nm_pick_runnable_shard(rt, task);
    }

    target = &rt->shards[target_id];
    pressure = nm_runtime_pressure_signal(rt);
    effective_hot = hot || task->task_class == NM_TASK_CLASS_LATENCY;
    direct_local = (g_nm_tls_shard == target && g_nm_tls_task != NULL);

    if (task->active_timer != NULL) {
        nm_disarm_task_wait_deadline(task);
    }
    // Wake ownership transfers from the wait primitive/I/O request back to the
    // scheduler queue before the task becomes runnable.
    nm_task_clear_wait_tracking(task);
    pthread_mutex_lock(&target->lock);
    effective_hot = nm_should_enqueue_hot_locked(target, task, effective_hot, pressure);
    nm_mark_runnable_locked(target, task, effective_hot, kind, reason, direct_local);
    pthread_mutex_unlock(&target->lock);
    if (!direct_local) {
        nm_kick_shard(target);
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
bool nm_reinject_task_on_shard_and_yield_current(nm_runtime_t *rt,
                                                 nm_task_t *task,
                                                 unsigned target_id,
                                                 bool hot,
                                                 nm_trace_kind_t kind,
                                                 nm_wait_reason_t reason) {
    nm_shard_t *target;
    nm_task_t *current = g_nm_tls_task;
    bool effective_hot;

    if (rt == NULL || task == NULL || current == NULL || rt->active_shards == 0U ||
        target_id >= rt->active_shards || !nm_shard_accepts_new_work(&rt->shards[target_id])) {
        return false;
    }

    target = &rt->shards[target_id];
    if (g_nm_tls_shard != target || current == task) {
        return false;
    }

    effective_hot = hot || task->task_class == NM_TASK_CLASS_LATENCY;

    if (task->active_timer != NULL) {
        nm_disarm_task_wait_deadline(task);
    }
    nm_task_clear_wait_tracking(task);

    current->forced_yield_budget = rt->forced_yield_every;
    current->state = NM_TASK_STATE_RUNNABLE;
    current->wait_reason = NM_WAIT_NONE;
    current->last_yield_ns = 0U;

    pthread_mutex_lock(&target->lock);
    effective_hot = nm_should_enqueue_hot_locked(target, task, effective_hot, false);
    nm_mark_runnable_locked(target, task, effective_hot, kind, reason, true);
    // Put the yielding task on the normal FIFO side so the just-woken peer can
    // run promptly without starving older normal work.
    (void)nm_norm_queue_push_yield_locked(target, current);
    target->metrics.yields += 1U;
    nm_trace_shard(target, current, NM_TRACE_STATE, NM_TASK_STATE_RUNNING, NM_TASK_STATE_RUNNABLE, NM_WAIT_YIELD);
    pthread_mutex_unlock(&target->lock);

    nm_task_sample_live_stack(current);
    nm_ctx_switch(&current->ctx, g_nm_tls_scheduler_ctx != NULL ? g_nm_tls_scheduler_ctx : &target->scheduler_ctx);
    return true;
}
