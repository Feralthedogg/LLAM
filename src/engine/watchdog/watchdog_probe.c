/**
 * @file src/engine/watchdog/watchdog_probe.c
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

#include "engine/runtime_watchdog_internal.h"

/**
 * @brief Check whether dynamic-shard diagnostic tracing is enabled.
 *
 * @return @c true when @c LLAM_DYNAMIC_TRACE is set to a non-zero value.
 */
bool llam_dynamic_trace_enabled(void) {
    static atomic_int cached = -1;
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        const char *env = llam_env_get("LLAM_DYNAMIC_TRACE");

        value = llam_env_flag_value(env, 0U) != 0U ? 1 : 0;
        atomic_store_explicit(&cached, value, memory_order_release);
    }
    return value != 0;
}

/**
 * @brief Return whether a running task is blocking useful local/global work.
 *
 * The watchdog uses this to avoid sending preemption nudges when a task is the
 * only runnable work in the runtime.  Strict mode intentionally bypasses this
 * pressure gate for diagnostics.
 */
static bool llam_watchdog_preempt_pressure_locked(llam_shard_t *shard) {
    llam_runtime_t *rt;

    if (shard == NULL || shard->runtime == NULL) {
        return false;
    }
    rt = shard->runtime;
    if (rt->preempt_mode == LLAM_PREEMPT_STRICT) {
        return true;
    }
    if (atomic_load_explicit(&shard->processor.inject_depth, memory_order_acquire) != 0U ||
        shard->processor.hot_q.depth != 0U ||
        llam_norm_queue_depth(shard) != 0U ||
        llam_async_preempt_ready_depth(shard) != 0U ||
        atomic_load_explicit(&shard->processor.timer_count, memory_order_acquire) != 0U ||
        atomic_load_explicit(llam_processor_event_pending_const(&shard->processor),
                             memory_order_acquire) != 0U ||
        llam_runtime_overflow_depth(rt) != 0U ||
        atomic_load_explicit(&rt->block_pending, memory_order_acquire) != 0U) {
        return true;
    }
    return false;
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
    llam_runtime_t *rt = shard != NULL ? shard->runtime : NULL;
    llam_worker_t *owner = NULL;
    llam_worker_t *validated_owner = NULL;
    llam_task_t *current = NULL;
    llam_processor_owner_ticket_t owner_ticket = {0};
    llam_processor_owner_ticket_t validated_ticket = {0};
    uint64_t owner_incarnation = 0U;
    uint64_t epoch = 0U;
    uint64_t validated_incarnation = 0U;
    uint64_t validated_epoch = 0U;
    bool ticket_still_matches = false;
    pthread_t native_thread;
    pthread_t validated_native_thread;
    bool scan_ref = false;
    bool candidate = false;

    if (rt == NULL || rt->preempt_mode < LLAM_PREEMPT_AUTO) {
        return;
    }
    memset(&native_thread, 0, sizeof(native_thread));
    memset(&validated_native_thread, 0, sizeof(validated_native_thread));

    /* The lease lock protects owner/incarnation/native-thread publication. */
    pthread_mutex_lock(&shard->opaque_lock);
    if (!llam_legacy_lease_snapshot_watchdog_locked(shard,
                                                     &owner,
                                                     &owner_incarnation,
                                                     &epoch,
                                                     &native_thread)) {
        pthread_mutex_unlock(&shard->opaque_lock);
        return;
    }
    pthread_mutex_lock(&shard->lock);
    current = llam_worker_load_active_g(owner);
    if (current != NULL) {
        /* Keep the task object alive across the signal-target revalidation. */
        scan_ref = llam_task_scan_ref_try_acquire(rt, current);
        if (scan_ref) {
            uint64_t last_safepoint = atomic_load_explicit(&owner->last_safepoint_ns,
                                                           memory_order_acquire);
            uint64_t slice_ns = rt->preempt_quantum_ns != 0U
                                    ? rt->preempt_quantum_ns
                                    : llam_slice_ns((llam_task_class_t)atomic_load_explicit(&current->task_class,
                                                                                             memory_order_acquire));
            unsigned task_state = atomic_load_explicit(&current->state,
                                                        memory_order_acquire);
            bool p_ticket_valid =
                llam_processor_owner_ticket_snapshot_pair_locked(shard,
                                                                  &owner_ticket) &&
                owner_ticket.owner_m == owner &&
                owner_ticket.worker_incarnation == owner_incarnation &&
                owner_ticket.lease_projection_epoch == epoch;
            bool safe_m = p_ticket_valid &&
                          llam_worker_has_safe_managed_g(owner,
                                                        &shard->processor,
                                                        current);

            if (!safe_m &&
                (current->flags & LLAM_TASK_FLAG_NO_PREEMPT) == 0U &&
                task_state == LLAM_TASK_STATE_RUNNING &&
                atomic_load_explicit(&current->opaque_blocking_depth,
                                     memory_order_acquire) == 0U) {
                unsigned exec_state = atomic_load_explicit(&owner->execution_state,
                                                            memory_order_acquire);

                if (exec_state > LLAM_WORKER_EXEC_STOPPING) {
                    atomic_fetch_add_explicit(&shard->metrics.relief_deferred_unknown,
                                              1U,
                                              memory_order_relaxed);
                } else if (!p_ticket_valid ||
                           atomic_load_explicit(&owner->current_p,
                                                memory_order_acquire) !=
                               &shard->processor) {
                    atomic_fetch_add_explicit(&shard->metrics.relief_deferred_stale_ticket,
                                              1U,
                                              memory_order_relaxed);
                } else {
                    atomic_fetch_add_explicit(&shard->metrics.relief_deferred_unsafe,
                                              1U,
                                              memory_order_relaxed);
                }
            }
            if (safe_m &&
                (current->flags & LLAM_TASK_FLAG_NO_PREEMPT) == 0U &&
                atomic_load_explicit(&current->opaque_blocking_depth,
                                     memory_order_acquire) == 0U &&
                task_state == LLAM_TASK_STATE_RUNNING) {
                if (rt->preempt_mode != LLAM_PREEMPT_STRICT &&
                    slice_ns <= UINT64_MAX / 2U) {
                    slice_ns *= 2U;
                }
                candidate = last_safepoint > 0U && now_ns > last_safepoint &&
                            now_ns - last_safepoint > slice_ns &&
                            llam_watchdog_preempt_pressure_locked(shard);
                if (!candidate && last_safepoint > 0U && now_ns > last_safepoint &&
                    now_ns - last_safepoint > slice_ns) {
                    shard->metrics.preempt_suppressed += 1U;
                }
            }
        }
    }

    if (!candidate) {
        if (scan_ref) {
            (void)llam_task_scan_ref_release(rt, current);
        }
        pthread_mutex_unlock(&shard->lock);
        pthread_mutex_unlock(&shard->opaque_lock);
        return;
    }

    /*
     * Leave both locks for the optional deterministic test gate. The scan ref
     * pins the task object while the worker binding is allowed to hand back;
     * the target and active-G tuple is revalidated before pthread_kill().
     */
    pthread_mutex_unlock(&shard->lock);
    pthread_mutex_unlock(&shard->opaque_lock);
    llam_legacy_lease_test_pause(LLAM_LEASE_TEST_POINT_BEFORE_WATCHDOG_SIGNAL);

    pthread_mutex_lock(&shard->opaque_lock);
    pthread_mutex_lock(&shard->lock);
    ticket_still_matches =
        llam_processor_owner_ticket_snapshot_pair_locked(shard,
                                                          &validated_ticket) &&
        llam_processor_owner_ticket_equal(&owner_ticket, &validated_ticket);
    atomic_fetch_add_explicit(&shard->metrics.relief_deferred_stale_ticket,
                              ticket_still_matches ? 0U : 1U,
                              memory_order_relaxed);

    if (!llam_legacy_lease_snapshot_watchdog_locked(shard,
                                                     &validated_owner,
                                                     &validated_incarnation,
                                                     &validated_epoch,
                                                     &validated_native_thread) ||
        validated_owner != owner || validated_incarnation != owner_incarnation ||
        validated_epoch != epoch ||
        pthread_equal(validated_native_thread, native_thread) == 0 ||
        llam_worker_load_active_g(validated_owner) != current ||
        !llam_worker_binding_live(validated_owner, validated_incarnation) ||
        !ticket_still_matches) {
        (void)llam_task_scan_ref_release(rt, current);
        pthread_mutex_unlock(&shard->lock);
        pthread_mutex_unlock(&shard->opaque_lock);
        return;
    }

    /* Recompute policy after the tuple validation; a late safepoint wins. */
    {
        uint64_t last_safepoint = atomic_load_explicit(&validated_owner->last_safepoint_ns,
                                                       memory_order_acquire);
        uint64_t slice_ns = rt->preempt_quantum_ns != 0U
                                ? rt->preempt_quantum_ns
                                : llam_slice_ns((llam_task_class_t)atomic_load_explicit(&current->task_class,
                                                                                         memory_order_acquire));
        unsigned task_state = atomic_load_explicit(&current->state,
                                                    memory_order_acquire);
        bool safe_m = ticket_still_matches &&
                      llam_worker_has_safe_managed_g(validated_owner,
                                                     &shard->processor,
                                                     current);
        bool overdue;

        if (rt->preempt_mode != LLAM_PREEMPT_STRICT &&
            slice_ns <= UINT64_MAX / 2U) {
            slice_ns *= 2U;
        }
        overdue = safe_m && last_safepoint > 0U && now_ns > last_safepoint &&
                  now_ns - last_safepoint > slice_ns &&
                  (current->flags & LLAM_TASK_FLAG_NO_PREEMPT) == 0U &&
                  atomic_load_explicit(&current->opaque_blocking_depth,
                                       memory_order_acquire) == 0U &&
                  task_state == LLAM_TASK_STATE_RUNNING &&
                  llam_watchdog_preempt_pressure_locked(shard);
        if (overdue) {
            bool first_request = atomic_exchange(&current->preempt_requested, 1U) == 0U;
            bool send_signal = first_request;
            int signal_number = LLAM_PREEMPT_SIGNAL;
            uint64_t async_request_generation = 0U;
            bool async_request_prepared = false;
            int signal_rc;

            if (first_request) {
                shard->metrics.watchdog_hits += 1U;
                shard->metrics.long_no_safepoint += 1U;
                shard->metrics.preempt_requests += 1U;
                /* An overdue G with no announced boundary stays on cooperative recovery. */
                atomic_fetch_add_explicit(&shard->metrics.relief_deferred_unannounced,
                                          1U,
                                          memory_order_relaxed);
                llam_trace_shard(shard,
                                 current,
                                 LLAM_TRACE_WATCHDOG,
                                 LLAM_TASK_STATE_RUNNING,
                                 LLAM_TASK_STATE_RUNNING,
                                 LLAM_WAIT_NONE);
            }
            /*
             * The experiment is strictly opt-in. A live, exact P ticket is
             * embedded in the generation-bearing request; all other cases
             * retain the ordinary one-store cooperative signal path.
             */
            async_request_prepared = llam_async_preempt_prepare_request(
                validated_owner,
                current,
                &validated_ticket,
                atomic_load_explicit(&validated_owner->run_generation,
                                     memory_order_acquire),
                atomic_load_explicit(&current->run_generation,
                                     memory_order_acquire),
                &async_request_generation);
            if (async_request_prepared && rt->async_preempt_signal > 0) {
                signal_number = rt->async_preempt_signal;
            }
            send_signal = first_request || async_request_prepared;
#if LLAM_ASYNC_CONTEXT_SHARED
            if (atomic_load_explicit(&rt->async_preempt_enabled, memory_order_acquire) != 0U) {
                /* The exact G request was already stored above. Shared native
                 * adapters own their transport; a second SIGUSR1 can leave a
                 * Unix signal activation in flight across a Mach redirect. */
                send_signal = false;
            }
#endif
            if (send_signal) {
                signal_rc = pthread_kill(validated_native_thread, signal_number);
                if (signal_rc == 0) {
                    shard->metrics.preempt_signals += 1U;
                }
                if (async_request_prepared && signal_rc != 0) {
                    llam_async_preempt_cancel_request(validated_owner,
                                                      async_request_generation);
                }
            }
        }
    }
    (void)llam_task_scan_ref_release(rt, current);
    pthread_mutex_unlock(&shard->lock);
    pthread_mutex_unlock(&shard->opaque_lock);
}

/**
 * @brief Check whether any shard has pending timers.
 *
 * @param rt Runtime to inspect.
 *
 * @return @c true when at least one timer root or unresolved expired timer is present.
 */
bool llam_runtime_has_pending_timers(llam_runtime_t *rt) {
    unsigned i;

    for (i = 0; i < rt->active_shards; ++i) {
        bool has_timer;

        pthread_mutex_lock(&rt->shards[i].lock);
        has_timer = rt->shards[i].processor.timers != NULL ||
                    atomic_load_explicit(&rt->shards[i].processor.timer_callbacks_active, memory_order_acquire) != 0U;
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

        pthread_mutex_lock(&rt->shards[i].opaque_lock);
        pthread_mutex_lock(&rt->shards[i].lock);
        has_work = rt->shards[i].processor.inject_q.depth > 0U || rt->shards[i].processor.hot_q.depth > 0U || llam_norm_queue_depth(&rt->shards[i]) > 0U ||
                   llam_async_preempt_ready_depth(&rt->shards[i]) > 0U ||
                   llam_shard_load_lease_current(&rt->shards[i]) != NULL;
        pthread_mutex_unlock(&rt->shards[i].lock);
        pthread_mutex_unlock(&rt->shards[i].opaque_lock);
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
        has_backlog = rt->shards[i].processor.inject_q.depth > 0U ||
                      rt->shards[i].processor.hot_q.depth > 0U ||
                      llam_norm_queue_depth(&rt->shards[i]) > 0U ||
                      llam_async_preempt_ready_depth(&rt->shards[i]) > 0U;
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

    pthread_mutex_lock(&shard->opaque_lock);
    pthread_mutex_lock(&shard->lock);
    load = shard->processor.inject_q.depth + shard->processor.hot_q.depth + llam_norm_queue_depth(shard);
    load += llam_async_preempt_ready_depth(shard);
    if (llam_shard_load_lease_current(shard) != NULL) {
        load += 1U;
    }
    if (shard->processor.timers != NULL) {
        load += 1U;
    }
    if (shard->opaque_redirect_active) {
        load += LLAM_INJECT_QUEUE_CAP;
    }
    pthread_mutex_unlock(&shard->lock);
    pthread_mutex_unlock(&shard->opaque_lock);
    return load;
}
