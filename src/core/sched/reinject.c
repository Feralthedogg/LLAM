/**
 * @file src/core/sched/reinject.c
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
 * See LICENSE.
 */

#include "runtime_internal.h"

#if LLAM_RUNTIME_BACKEND_LINUX || LLAM_RUNTIME_BACKEND_KQUEUE || LLAM_RUNTIME_BACKEND_WINDOWS
#define LLAM_REINJECT_DIRECT_OWNER_HANDOFF 1
#else
#define LLAM_REINJECT_DIRECT_OWNER_HANDOFF 0
#endif

/**
 * @brief Stamp and publish a directly reinjected task on the current worker.
 *
 * Direct reinjection skips the scheduler dispatch prologue, so it refreshes
 * the worker safepoint before exposing the replacement active-G to watchdogs.
 * The release publication is sequenced after that timestamp store.
 */
static void llam_publish_reinject_active_g(llam_shard_t *target,
                                           llam_task_t *task) {
    llam_worker_t *worker = g_llam_tls_worker;

    if (!llam_worker_matches_shard(worker, target)) {
        return;
    }
    if (target != NULL && target->runtime != NULL &&
        target->runtime->preempt_mode >= LLAM_PREEMPT_AUTO) {
        atomic_store_explicit(&worker->last_safepoint_ns,
                              llam_now_ns(),
                              memory_order_release);
    }
    llam_worker_begin_g_run(worker, task);
    llam_worker_publish_active_g(worker, task);
}

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
    bool direct_local,
    const llam_processor_queue_access_t *access) {
    llam_task_state_id_t from = task->state;
    if (direct_local &&
        !llam_processor_queue_access_validate_owner_mutation_locked(shard, access)) {
        direct_local = false;
    }
    if (shard->runtime->experimental_dynamic_shards != 0U &&
        llam_processor_try_set_online(&shard->processor, true)) {
        // Waking work onto an offline dynamic shard brings it back online and
        // updates the runtime's min/max online-shard diagnostics.
        llam_runtime_note_online_shards(shard->runtime,
                                      atomic_fetch_add_explicit(&shard->runtime->online_shards, 1U, memory_order_acq_rel) + 1U);
    }

    task->state = LLAM_TASK_STATE_RUNNABLE;
    task->wait_reason = LLAM_WAIT_NONE;
    task->enqueue_hot = hot ? 1U : 0U;
    task->last_runnable_ns =
        llam_runtime_should_stamp_runnable_latency(shard) ? llam_now_ns() : 0U;

    if (llam_async_preempt_enqueue_locked(shard, task)) {
        /* Wait-generation ownership is already resolved; preserve the G's native origin. */
    } else if (shard->opaque_redirect_active) {
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
            if (llam_queue_push_bounded_locked(shard,
                                               &shard->processor.hot_q,
                                               LLAM_HOT_QUEUE_CAP,
                                               task,
                                               access)) {
                shard->metrics.hot_enqueues += 1U;
            }
        } else {
            (void)llam_norm_queue_push_owner_locked(shard, access, task);
        }
    } else if (llam_queue_push_bounded_locked(shard,
                                             &shard->processor.inject_q,
                                             LLAM_INJECT_QUEUE_CAP,
                                             task,
                                             NULL)) {
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
    llam_processor_queue_access_t access;
    bool access_valid = false;

    if (rt == NULL || task == NULL || rt->active_shards == 0U) {
        return;
    }
    {
        unsigned origin_id = llam_async_preempt_task_origin_shard(task);

        if (origin_id != UINT_MAX) {
            target_id = origin_id;
        }
    }
    if (target_id >= rt->active_shards || !llam_shard_accepts_new_work(&rt->shards[target_id])) {
        target_id = llam_pick_runnable_shard(rt, task);
    }

    if (rt->managed_policy_pools) {
        llam_worker_t *execution_worker = task->managed_execution_worker;
        if (execution_worker != NULL && execution_worker->role == LLAM_WORKER_ROLE_LEGACY_COMPAT) {
            target_id = execution_worker->compat_shard->id;
        } else if (target_id >= rt->portable_processor_count) {
            if (rt->portable_processor_count == 0U) abort();
            target_id %= rt->portable_processor_count;
        }
    }
    target = &rt->shards[target_id];
    pressure = llam_runtime_pressure_signal(rt);
    effective_hot = hot || atomic_load_explicit(&task->task_class, memory_order_acquire) == (unsigned)LLAM_TASK_CLASS_LATENCY;
    direct_local = (g_llam_tls_shard == target && g_llam_tls_task != NULL);
    if (direct_local) {
        access_valid = llam_processor_queue_access_snapshot(&target->processor, &access) &&
                       (access.mode == LLAM_PROCESSOR_QUEUE_ACCESS_EXECUTION ||
                        access.mode == LLAM_PROCESSOR_QUEUE_ACCESS_STOPPING_DRAIN);
        if (!access_valid) {
            direct_local = false;
        }
    }

    llam_disarm_task_wait_deadline(task);
    // Wake ownership transfers from the wait primitive/I/O request back to the
    // scheduler queue before the task becomes runnable.
    llam_task_clear_wait_tracking_or_abort(task);
    pthread_mutex_lock(&target->lock);
    effective_hot = llam_should_enqueue_hot_locked(target, task, effective_hot, pressure);
    llam_mark_runnable_locked(target,
                              task,
                              effective_hot,
                              kind,
                              reason,
                              direct_local,
                              access_valid ? &access : NULL);
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
    int caller_errno = llam_thread_errno_load();
    llam_processor_queue_access_t access;

    if (rt == NULL || task == NULL || current == NULL || rt->active_shards == 0U ||
        target_id >= rt->active_shards || !llam_shard_accepts_new_work(&rt->shards[target_id])) {
        return false;
    }
    if (rt->execution_lease_mode == LLAM_EXECUTION_LEASE_REGISTERED) {
        /* Every registered execution/restoration needs a fresh scheduler claim. */
        return false;
    }
#if defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT) && \
    ((defined(__linux__) && defined(__x86_64__)) || (LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64) || LLAM_ASYNC_CONTEXT_SHARED)
    if (atomic_load_explicit(&rt->async_preempt_enabled, memory_order_acquire) != 0U) {
        /* Preserve capture/restore retirement and dispatch through the scheduler. */
        return false;
    }
#endif

    target = &rt->shards[target_id];
    if (g_llam_tls_shard != target || current == task) {
        return false;
    }
    if (!llam_processor_queue_access_snapshot(&target->processor, &access) ||
        (access.mode != LLAM_PROCESSOR_QUEUE_ACCESS_EXECUTION &&
         access.mode != LLAM_PROCESSOR_QUEUE_ACCESS_STOPPING_DRAIN)) {
        return false;
    }

    if (rt->run_timing_enabled != 0U || rt->wake_latency_metrics_enabled != 0U ||
        llam_task_wait_deadline_active(task)) {
        return false;
    }

#if !LLAM_REINJECT_DIRECT_OWNER_HANDOFF
    return false;
#endif

    effective_hot = hot || atomic_load_explicit(&task->task_class, memory_order_acquire) == (unsigned)LLAM_TASK_CLASS_LATENCY;

#if LLAM_REINJECT_DIRECT_OWNER_HANDOFF
    if (llam_tls_has_legacy_direct_handoff_context(target) &&
        rt->trace_events_enabled == 0U &&
        rt->run_timing_enabled == 0U &&
        rt->wake_latency_metrics_enabled == 0U &&
        llam_lockfree_normq_enabled(rt) &&
        !target->opaque_redirect_active &&
        (rt->direct_handoff_allow_timers != 0U ||
         atomic_load_explicit(&target->processor.timer_count, memory_order_acquire) == 0U)) {
    if (llam_norm_queue_push_yield_unlocked(target, current)) {
            uint64_t now_ns = llam_runtime_should_stamp_runnable_latency(target) ? llam_now_ns() : 0U;

            /*
             * This is the first point where direct handoff has committed.  The
             * try path can still fail before this and must leave wait ownership
             * untouched so the caller's normal reinject fallback sees a fully
             * tracked parked task.
             */
            current->forced_yield_budget = rt->forced_yield_every;
            current->state = LLAM_TASK_STATE_RUNNABLE;
            current->wait_reason = LLAM_WAIT_NONE;
            current->last_yield_ns = 0U;
            current->last_runnable_ns = now_ns;
            llam_task_clear_wait_tracking_or_abort(task);
            task_from = task->state;
            task->state = LLAM_TASK_STATE_RUNNING;
            task->wait_reason = LLAM_WAIT_NONE;
            task->enqueue_hot = effective_hot ? 1U : 0U;
            atomic_store_explicit(&task->last_shard, target->id, memory_order_relaxed);
            task->last_started_ns = 0U;
            if (now_ns != 0U) {
                llam_runtime_record_autotune_wake_latency(target, 0U);
            }
            llam_publish_reinject_active_g(target, task);
            llam_legacy_lease_test_pause(LLAM_LEASE_TEST_POINT_AFTER_DIRECT_REINJECT);
            g_llam_tls_task = task;
            target->metrics.wakes += 1U;
            if (reason > LLAM_WAIT_NONE && reason <= LLAM_WAIT_TIMEOUT) {
                target->metrics.wake_reason_hist[reason] += 1U;
            }
            target->metrics.yields += 1U;
            target->metrics.ctx_switches += 1U;
            target->processor.direct_handoff_streak += 1U;
            (void)kind;
            (void)task_from;
            llam_thread_errno_store(caller_errno);
            llam_switch_task_to_task_hot(current, task);
            return true;
        }
    }
#endif

    pthread_mutex_lock(&target->lock);
    if (!llam_processor_queue_access_validate_owner_mutation_locked(target, &access) ||
        target->opaque_redirect_active ||
        target->processor.norm_q.depth >= LLAM_NORM_QUEUE_CAP) {
        pthread_mutex_unlock(&target->lock);
        return false;
    }

    effective_hot = llam_should_enqueue_hot_locked(target, task, effective_hot, false);
    // Put the yielding task on the normal FIFO side so the just-woken peer can
    // run promptly without starving older normal work.
    if (!llam_norm_queue_push_yield_locked(target, &access, current)) {
        pthread_mutex_unlock(&target->lock);
        return false;
    }
    {
        uint64_t now_ns = llam_runtime_should_stamp_runnable_latency(target) ? llam_now_ns() : 0U;

        /*
         * The locked path has committed once the current task is queued.  Keep both
         * the current task state and the woken task's wait ownership untouched until
         * that point so failed try paths remain invisible to the caller.
         */
        current->forced_yield_budget = rt->forced_yield_every;
        current->state = LLAM_TASK_STATE_RUNNABLE;
        current->wait_reason = LLAM_WAIT_NONE;
        current->last_yield_ns = 0U;
        current->last_runnable_ns = now_ns;
        llam_task_clear_wait_tracking_or_abort(task);
        task_from = task->state;
        task->state = LLAM_TASK_STATE_RUNNING;
        task->wait_reason = LLAM_WAIT_NONE;
        task->enqueue_hot = effective_hot ? 1U : 0U;
        atomic_store_explicit(&task->last_shard, target->id, memory_order_relaxed);
        task->last_started_ns = 0U;
        if (now_ns != 0U) {
            llam_runtime_record_autotune_wake_latency(target, 0U);
        }
    }
    llam_publish_reinject_active_g(target, task);
    g_llam_tls_task = task;
    target->metrics.wakes += 1U;
    if (reason > LLAM_WAIT_NONE && reason <= LLAM_WAIT_TIMEOUT) {
        target->metrics.wake_reason_hist[reason] += 1U;
    }
    target->metrics.yields += 1U;
    target->metrics.ctx_switches += 1U;
    target->processor.direct_handoff_streak += 1U;
    llam_trace_shard(target, task, kind, task_from, LLAM_TASK_STATE_RUNNING, reason);
    llam_trace_shard(target, current, LLAM_TRACE_STATE, LLAM_TASK_STATE_RUNNING, LLAM_TASK_STATE_RUNNABLE, LLAM_WAIT_YIELD);
    pthread_mutex_unlock(&target->lock);

    llam_legacy_lease_test_pause(LLAM_LEASE_TEST_POINT_AFTER_DIRECT_REINJECT);
    llam_thread_errno_store(caller_errno);
    llam_switch_task_to_task_hot(current, task);
    return true;
}
