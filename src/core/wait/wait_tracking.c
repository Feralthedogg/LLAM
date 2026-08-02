/**
 * @file src/core/wait/wait_tracking.c
 * @brief Runtime accounting for tasks blocked on waits or I/O.
 *
 * @details
 * A parked task records exactly which wait structure owns it: a synchronization
 * wait node, join target, sleep timer, I/O request, or blocking job. This file
 * centralizes those tracking transitions so timeout, cancellation, completion,
 * and reinjection paths can safely detach a task before making it runnable.
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
 * See LICENSES/OLD-LICENSE/Apache-2.0.txt.
 */

#include "runtime_internal.h"

/** @brief Return the runtime that owns a task's wait/I/O bookkeeping. */
static llam_runtime_t *llam_wait_task_runtime(const llam_task_t *task) {
    return task != NULL ? task->owner_runtime : NULL;
}

typedef enum llam_wake_handoff_fail {
    LLAM_WAKE_HANDOFF_FAIL_NONE = 0,
    LLAM_WAKE_HANDOFF_FAIL_CONTEXT,
    LLAM_WAKE_HANDOFF_FAIL_POLICY,
    LLAM_WAKE_HANDOFF_FAIL_BUDGET,
    LLAM_WAKE_HANDOFF_FAIL_RACE,
} llam_wake_handoff_fail_t;

static void llam_wake_handoff_record_attempt(llam_shard_t *shard) {
    llam_task_t *current = g_llam_tls_task;
    bool sample;

    if (shard == NULL) {
        return;
    }
    sample = llam_runtime_should_record_handoff_stats(shard);
    if (current != NULL) {
        current->handoff_sample_current = sample;
    } else {
        shard->autotune_handoff_sample_current = sample;
    }
    if (sample) {
        shard->metrics.wake_handoff_attempts += 1U;
    }
}

static void llam_wake_handoff_record_hit(llam_shard_t *shard) {
    llam_task_t *current = g_llam_tls_task;
    bool sample;

    if (shard == NULL) {
        return;
    }
    sample = current != NULL ? current->handoff_sample_current : shard->autotune_handoff_sample_current;
    if (sample) {
        if (current != NULL) {
            current->handoff_sample_current = false;
        } else {
            shard->autotune_handoff_sample_current = false;
        }
        shard->metrics.wake_handoff_hits += 1U;
    }
}

static void llam_wake_handoff_record_fail(llam_shard_t *shard, llam_wake_handoff_fail_t reason) {
    llam_task_t *current = g_llam_tls_task;
    bool sample;

    if (shard == NULL) {
        return;
    }
    sample = current != NULL ? current->handoff_sample_current : shard->autotune_handoff_sample_current;
    if (!sample) {
        return;
    }
    if (current != NULL) {
        current->handoff_sample_current = false;
    } else {
        shard->autotune_handoff_sample_current = false;
    }

    switch (reason) {
    case LLAM_WAKE_HANDOFF_FAIL_CONTEXT:
        shard->metrics.wake_handoff_fail_context += 1U;
        break;
    case LLAM_WAKE_HANDOFF_FAIL_POLICY:
        shard->metrics.wake_handoff_fail_policy += 1U;
        break;
    case LLAM_WAKE_HANDOFF_FAIL_BUDGET:
        shard->metrics.wake_handoff_fail_policy += 1U;
        shard->metrics.wake_handoff_fail_budget += 1U;
        break;
    case LLAM_WAKE_HANDOFF_FAIL_RACE:
        shard->metrics.wake_handoff_fail_race += 1U;
        break;
    case LLAM_WAKE_HANDOFF_FAIL_NONE:
    default:
        break;
    }
}

static llam_wake_handoff_fail_t llam_wake_handoff_precheck(llam_runtime_t *rt,
                                                          llam_task_t *task,
                                                          unsigned parked_shard) {
    llam_shard_t *shard = g_llam_tls_shard;
    llam_task_t *current = g_llam_tls_task;

    if (rt == NULL || task == NULL || shard == NULL || current == NULL ||
        rt->active_shards == 0U || parked_shard >= rt->active_shards ||
        g_llam_tls_scheduler_ctx != &shard->scheduler_ctx) {
        return LLAM_WAKE_HANDOFF_FAIL_CONTEXT;
    }
    if (shard->runtime != rt || shard->id != parked_shard || current == task) {
        return LLAM_WAKE_HANDOFF_FAIL_CONTEXT;
    }
    if (rt->trace_events_enabled != 0U ||
        rt->run_timing_enabled != 0U ||
        rt->wake_latency_metrics_enabled != 0U ||
        !llam_lockfree_normq_enabled(rt) ||
        !llam_shard_accepts_new_work(shard) ||
        shard->opaque_redirect_active ||
        llam_task_wait_deadline_active(task) ||
        (rt->direct_handoff_allow_timers == 0U &&
         atomic_load_explicit(&shard->timer_count, memory_order_acquire) != 0U)) {
        return LLAM_WAKE_HANDOFF_FAIL_POLICY;
    }
    if (rt->direct_handoff_live_limit != 0U &&
        llam_runtime_live_tasks(rt) > rt->direct_handoff_live_limit) {
        shard->direct_handoff_streak = 0U;
        return LLAM_WAKE_HANDOFF_FAIL_POLICY;
    }
    {
        unsigned handoff_budget = llam_runtime_direct_handoff_budget(rt);

        if (handoff_budget != 0U && shard->direct_handoff_streak >= handoff_budget) {
            shard->direct_handoff_streak = 0U;
            return LLAM_WAKE_HANDOFF_FAIL_BUDGET;
        }
    }
    return LLAM_WAKE_HANDOFF_FAIL_NONE;
}

static bool llam_task_swap_active_io_req(llam_task_t *task,
                                         llam_io_req_t *req,
                                         llam_io_req_t **old_req_out) {
    llam_io_req_t *old_req;
    llam_runtime_t *rt = llam_wait_task_runtime(task);
    bool track_counter;

    if (task == NULL) {
        errno = EINVAL;
        return false;
    }
    if (old_req_out != NULL) {
        *old_req_out = NULL;
    }
    track_counter = rt != NULL && atomic_load_explicit(&rt->initialized, memory_order_acquire);
    if (!track_counter) {
        old_req = atomic_exchange_explicit(&task->active_io_req, req, memory_order_acq_rel);
        if (old_req_out != NULL) {
            *old_req_out = old_req;
        }
        return true;
    }

    old_req = atomic_load_explicit(&task->active_io_req, memory_order_acquire);
    for (;;) {
        if (old_req == req) {
            if (old_req_out != NULL) {
                *old_req_out = old_req;
            }
            return true;
        }
        if (old_req == NULL && req != NULL) {
            // active_io_waiters tracks tasks, not request objects.
            if (!llam_runtime_note_active_io_waiter(rt, 1)) {
                errno = EOVERFLOW;
                return false;
            }
            if (atomic_compare_exchange_weak_explicit(&task->active_io_req,
                                                      &old_req,
                                                      req,
                                                      memory_order_acq_rel,
                                                      memory_order_acquire)) {
                return true;
            }
            (void)llam_runtime_note_active_io_waiter(rt, -1);
            continue;
        }
        if (old_req != NULL && req == NULL) {
            if (atomic_compare_exchange_weak_explicit(&task->active_io_req,
                                                      &old_req,
                                                      NULL,
                                                      memory_order_acq_rel,
                                                      memory_order_acquire)) {
                (void)llam_runtime_note_active_io_waiter(rt, -1);
                if (old_req_out != NULL) {
                    *old_req_out = old_req;
                }
                return true;
            }
            continue;
        }
        if (atomic_compare_exchange_weak_explicit(&task->active_io_req,
                                                  &old_req,
                                                  req,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            if (old_req_out != NULL) {
                *old_req_out = old_req;
            }
            return true;
        }
    }
}

llam_io_req_t *llam_task_active_io_req_load(const llam_task_t *task) {
    if (task == NULL) {
        return NULL;
    }
    return atomic_load_explicit(&((llam_task_t *)task)->active_io_req, memory_order_acquire);
}

llam_block_job_t *llam_task_active_block_job_load(const llam_task_t *task) {
    if (task == NULL) {
        return NULL;
    }
    return atomic_load_explicit(&((llam_task_t *)task)->active_block_job, memory_order_acquire);
}

/** @brief Claim the currently published wait owner for cancellation resolution. */
bool llam_task_wait_resolver_try_begin(llam_task_t *task) {
    unsigned state;

    if (task == NULL) {
        errno = EINVAL;
        return false;
    }
    state = atomic_load_explicit(&task->wait_resolver_state,
                                 memory_order_acquire);
    for (;;) {
        if (LLAM_UNLIKELY(state == UINT_MAX)) {
            errno = EOVERFLOW;
            /* Closed saturation cannot represent a provable resolver drain. */
            abort();
        }
        if ((state & LLAM_WAIT_RESOLVER_CLOSED_BIT) != 0U) {
            return false;
        }
        if (LLAM_UNLIKELY((state & LLAM_WAIT_RESOLVER_REF_MASK) >=
                          LLAM_WAIT_RESOLVER_REF_MASK - 1U)) {
            errno = EOVERFLOW;
            /* An open gate cannot reach this count through supported callers. */
            abort();
        }
        if (atomic_compare_exchange_weak_explicit(&task->wait_resolver_state,
                                                  &state,
                                                  state + 1U,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            return true;
        }
    }
}

/** @brief Release one cancellation resolver claim without reopening ownership. */
void llam_task_wait_resolver_end(llam_task_t *task) {
    unsigned state;

    if (task == NULL) {
        return;
    }
    state = atomic_load_explicit(&task->wait_resolver_state,
                                 memory_order_acquire);
    for (;;) {
        if (LLAM_UNLIKELY((state & LLAM_WAIT_RESOLVER_REF_MASK) ==
                          LLAM_WAIT_RESOLVER_REF_MASK)) {
            errno = EOVERFLOW;
            abort();
        }
        if (LLAM_UNLIKELY((state & LLAM_WAIT_RESOLVER_REF_MASK) == 0U)) {
            errno = EINVAL;
            /* Continuing would permit a raw owner to be recycled unpinned. */
            abort();
        }
        if (atomic_compare_exchange_weak_explicit(&task->wait_resolver_state,
                                                  &state,
                                                  state - 1U,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            return;
        }
    }
}

/**
 * @brief Close wait-owner publication and drain every resolver that entered first.
 *
 * New begin operations fail after the high bit is set. Existing resolvers retain
 * their raw owner snapshots until they decrement the low-bit count. Callers may
 * clear or recycle wait state only after this function returns true.
 */
bool llam_task_close_wait_resolvers(llam_task_t *task) {
    unsigned state;

    if (task == NULL) {
        errno = EINVAL;
        return false;
    }
    (void)atomic_fetch_or_explicit(&task->wait_resolver_state,
                                  LLAM_WAIT_RESOLVER_CLOSED_BIT,
                                  memory_order_acq_rel);
    for (;;) {
        state = atomic_load_explicit(&task->wait_resolver_state,
                                     memory_order_acquire);
        if (LLAM_UNLIKELY(state == UINT_MAX)) {
            llam_runtime_t *rt = llam_wait_task_runtime(task);

            llam_record_fatal_deferred(rt, EOVERFLOW);
            errno = EOVERFLOW;
            return false;
        }
        if ((state & LLAM_WAIT_RESOLVER_REF_MASK) == 0U) {
            return true;
        }
        {
            struct timespec interval = {.tv_sec = 0, .tv_nsec = 100000L};

            /* Never re-enter the cooperative scheduler while an owner lock may be held. */
            (void)nanosleep(&interval, NULL);
        }
    }
}

/** @brief Publish a fully initialized wait owner after the resolver gate drained. */
bool llam_task_publish_wait_tracking(llam_task_t *task) {
    unsigned expected = LLAM_WAIT_RESOLVER_CLOSED_BIT;

    if (task == NULL) {
        errno = EINVAL;
        return false;
    }
    if (!atomic_compare_exchange_strong_explicit(&task->wait_resolver_state,
                                                 &expected,
                                                 0U,
                                                 memory_order_release,
                                                 memory_order_acquire)) {
        llam_runtime_t *rt = llam_wait_task_runtime(task);

        llam_record_fatal_deferred(rt, EINVAL);
        errno = EINVAL;
        return false;
    }
    return true;
}

/**
 * @brief Advance the task's wait epoch without permitting ABA wraparound.
 *
 * A saturated generation permanently disables new timed ownership for this
 * task. Reusing zero after wrap would let an old timeout match a later wait.
 */
static bool llam_task_advance_wait_generation(llam_task_t *task) {
    uint_fast64_t current;
    llam_runtime_t *rt;

    if (task == NULL) {
        errno = EINVAL;
        return false;
    }
    rt = llam_wait_task_runtime(task);
    current = atomic_load_explicit(&task->wait_generation, memory_order_acquire);
    for (;;) {
        if (LLAM_UNLIKELY(current >= UINT64_MAX)) {
            llam_record_fatal_deferred(rt, EOVERFLOW);
            errno = EOVERFLOW;
            return false;
        }
        if (atomic_compare_exchange_weak_explicit(&task->wait_generation,
                                                  &current,
                                                  current + 1U,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            return true;
        }
    }
}

/**
 * @brief Clear all wait ownership fields on a task.
 *
 * @param task Task whose wait tracking should be reset.
 */
bool llam_task_clear_wait_tracking(llam_task_t *task) {
    if (task == NULL) {
        errno = EINVAL;
        return false;
    }

    if (!llam_task_close_wait_resolvers(task)) {
        return false;
    }

    /* Invalidate the published owner before any node/state can be recycled. */
    if (!llam_task_advance_wait_generation(task)) {
        return false;
    }
    atomic_store_explicit(&task->active_wait_node, NULL, memory_order_release);
    atomic_store_explicit(&task->active_wait_queue, NULL, memory_order_release);
    atomic_store_explicit(&task->active_wait_queue_lock, NULL, memory_order_release);
    atomic_store_explicit(&task->active_select_state, NULL, memory_order_release);
    atomic_store_explicit(&task->active_wait_lifetime_ops, NULL, memory_order_release);
    /*
     * Completion, cancellation, and dynamic rehome can run on different OS
     * threads.  Use an exchange so only the first resolver that observes the
     * active I/O owner decrements global I/O waiter accounting.
     */
    (void)llam_task_swap_active_io_req(task, NULL, NULL);
    atomic_store_explicit(&task->active_io_generation, 0U, memory_order_release);
    /*
     * Blocking-job completion/cancellation can clear this field from a helper
     * OS thread while watchdog and timeout diagnostics sample it. Keep the
     * ownership handoff atomic just like active I/O request tracking.
     */
    atomic_store_explicit(&task->active_block_job, NULL, memory_order_release);
    atomic_store_explicit(&task->join_target, NULL, memory_order_release);
    atomic_store_explicit(&task->parked_shard,
                          atomic_load_explicit(&task->last_shard, memory_order_relaxed),
                          memory_order_relaxed);
    return true;
}

/** @brief Clear recyclable wait ownership or stop before an unsafe unwind. */
void llam_task_clear_wait_tracking_or_abort(llam_task_t *task) {
    if (LLAM_UNLIKELY(!llam_task_clear_wait_tracking(task))) {
        /* A saturated/corrupt resolver count cannot prove raw owners quiescent. */
        abort();
    }
}

/** @brief Clear stale owners and publish a fresh, unique wait generation. */
static bool llam_task_prepare_wait_tracking(llam_task_t *task) {
    uint_fast64_t generation;

    if (!llam_task_clear_wait_tracking(task)) {
        return false;
    }
    generation = atomic_load_explicit(&task->wait_generation, memory_order_acquire);
    if (LLAM_UNLIKELY(generation >= UINT64_MAX - 1U)) {
        llam_record_fatal_deferred(llam_wait_task_runtime(task), EOVERFLOW);
        errno = EOVERFLOW;
        return false;
    }
    return llam_task_advance_wait_generation(task);
}

/**
 * @brief Track that a task is parked on a synchronization wait node.
 *
 * @param task         Parked task.
 * @param node         Wait node owned by a primitive.
 * @param queue        Wait queue containing @p node.
 * @param queue_lock   Mutex protecting @p queue.
 * @param parked_shard Shard where the task parked.
 */
bool llam_task_set_wait_node_tracking(llam_task_t *task,
                                           llam_wait_node_t *node,
                                           llam_wait_queue_t *queue,
                                           pthread_mutex_t *queue_lock,
                                           _Atomic size_t *lifetime_ops,
                                           unsigned parked_shard,
                                           llam_wait_reason_t reason) {
    if (task == NULL || node == NULL || queue == NULL || queue_lock == NULL) {
        errno = EINVAL;
        return false;
    }
    if (!llam_task_prepare_wait_tracking(task)) {
        return false;
    }
    atomic_store_explicit(&task->active_wait_node, node, memory_order_release);
    atomic_store_explicit(&task->active_wait_queue, queue, memory_order_release);
    atomic_store_explicit(&task->active_wait_queue_lock, queue_lock, memory_order_release);
    atomic_store_explicit(&task->active_select_state, NULL, memory_order_release);
    atomic_store_explicit(&task->active_wait_lifetime_ops,
                          (void *)lifetime_ops,
                          memory_order_release);
    atomic_store_explicit(&task->active_block_job, NULL, memory_order_release);
    atomic_store_explicit(&task->join_target, NULL, memory_order_release);
    task->parked_shard = parked_shard;
    atomic_store_explicit(&task->wake_error_code, 0, memory_order_release);
    task->state = LLAM_TASK_STATE_PARKED;
    task->wait_reason = reason;
    if (!llam_task_publish_wait_tracking(task)) {
        abort();
    }
    return true;
}

/** @brief Publish a stack-backed channel-select owner for a fresh wait epoch. */
bool llam_task_set_select_tracking(llam_task_t *task,
                                   llam_channel_select_state_t *state,
                                   unsigned parked_shard,
                                   llam_wait_reason_t reason) {
    if (task == NULL) {
        errno = EINVAL;
        return false;
    }
    if (state == NULL || !llam_task_prepare_wait_tracking(task)) {
        if (state == NULL) {
            errno = EINVAL;
        }
        return false;
    }
    atomic_store_explicit(&task->active_wait_node, NULL, memory_order_release);
    atomic_store_explicit(&task->active_wait_queue, NULL, memory_order_release);
    atomic_store_explicit(&task->active_wait_queue_lock, NULL, memory_order_release);
    atomic_store_explicit(&task->active_select_state, state, memory_order_release);
    atomic_store_explicit(&task->active_wait_lifetime_ops, NULL, memory_order_release);
    atomic_store_explicit(&task->active_block_job, NULL, memory_order_release);
    atomic_store_explicit(&task->join_target, NULL, memory_order_release);
    task->parked_shard = parked_shard;
    atomic_store_explicit(&task->wake_error_code, 0, memory_order_release);
    task->state = LLAM_TASK_STATE_PARKED;
    task->wait_reason = reason;
    if (!llam_task_publish_wait_tracking(task)) {
        abort();
    }
    return true;
}

/**
 * @brief Track that a task is parked waiting for another task to exit.
 *
 * @param task         Parked waiter.
 * @param target       Join target.
 * @param parked_shard Shard where the waiter parked.
 */
bool llam_task_set_join_tracking(llam_task_t *task, llam_task_t *target, unsigned parked_shard) {
    if (task == NULL || target == NULL) {
        errno = EINVAL;
        return false;
    }
    if (!llam_task_prepare_wait_tracking(task)) {
        return false;
    }
    atomic_store_explicit(&task->active_wait_node, NULL, memory_order_release);
    atomic_store_explicit(&task->active_wait_queue, NULL, memory_order_release);
    atomic_store_explicit(&task->active_wait_queue_lock, NULL, memory_order_release);
    atomic_store_explicit(&task->active_select_state, NULL, memory_order_release);
    atomic_store_explicit(&task->active_wait_lifetime_ops,
                          target != NULL ? (void *)&target->active_ops : NULL,
                          memory_order_release);
    atomic_store_explicit(&task->active_block_job, NULL, memory_order_release);
    atomic_store_explicit(&task->join_target, target, memory_order_release);
    task->parked_shard = parked_shard;
    atomic_store_explicit(&task->wake_error_code, 0, memory_order_release);
    task->state = LLAM_TASK_STATE_PARKED;
    task->wait_reason = LLAM_WAIT_JOIN;
    if (!llam_task_publish_wait_tracking(task)) {
        abort();
    }
    return true;
}

/**
 * @brief Track that a task is parked only on a sleep/deadline timer.
 *
 * @param task         Parked task.
 * @param parked_shard Shard where the task parked.
 */
bool llam_task_set_sleep_tracking(llam_task_t *task,
                                  llam_wait_node_t *node,
                                  unsigned parked_shard) {
    if (task == NULL || node == NULL) {
        errno = EINVAL;
        return false;
    }
    if (!llam_task_prepare_wait_tracking(task)) {
        return false;
    }
    atomic_store_explicit(&task->active_wait_node, node, memory_order_release);
    atomic_store_explicit(&task->active_wait_queue, NULL, memory_order_release);
    atomic_store_explicit(&task->active_wait_queue_lock, NULL, memory_order_release);
    atomic_store_explicit(&task->active_select_state, NULL, memory_order_release);
    atomic_store_explicit(&task->active_wait_lifetime_ops, NULL, memory_order_release);
    atomic_store_explicit(&task->active_block_job, NULL, memory_order_release);
    atomic_store_explicit(&task->join_target, NULL, memory_order_release);
    task->parked_shard = parked_shard;
    atomic_store_explicit(&task->wake_error_code, 0, memory_order_release);
    task->state = LLAM_TASK_STATE_PARKED;
    task->wait_reason = LLAM_WAIT_SLEEP;
    if (!llam_task_publish_wait_tracking(task)) {
        abort();
    }
    return true;
}

/**
 * @brief Track that a task is parked on an I/O request.
 *
 * @param task         Parked task.
 * @param req          Active I/O request.
 * @param parked_shard Shard where the task parked.
 */
bool llam_task_set_io_tracking(llam_task_t *task, llam_io_req_t *req, unsigned parked_shard) {
    uint64_t operation_generation;

    if (task == NULL) {
        errno = EINVAL;
        return false;
    }
    if (req == NULL || !llam_task_prepare_wait_tracking(task)) {
        if (req == NULL) {
            errno = EINVAL;
        }
        return false;
    }
    atomic_store_explicit(&task->active_wait_node, NULL, memory_order_release);
    atomic_store_explicit(&task->active_wait_queue, NULL, memory_order_release);
    atomic_store_explicit(&task->active_wait_queue_lock, NULL, memory_order_release);
    atomic_store_explicit(&task->active_select_state, NULL, memory_order_release);
    atomic_store_explicit(&task->active_wait_lifetime_ops, NULL, memory_order_release);
    operation_generation = req != NULL
                               ? (uint64_t)atomic_load_explicit(&req->operation_generation,
                                                                memory_order_acquire)
                               : 0U;
    /* Publish the epoch before its pointer; pointer acquire then sees both. */
    atomic_store_explicit(&task->active_io_generation,
                          operation_generation,
                          memory_order_release);
    if (!llam_task_swap_active_io_req(task, req, NULL)) {
        atomic_store_explicit(&task->active_io_generation, 0U, memory_order_release);
        return false;
    }
    atomic_store_explicit(&task->active_block_job, NULL, memory_order_release);
    atomic_store_explicit(&task->join_target, NULL, memory_order_release);
    task->parked_shard = parked_shard;
    atomic_store_explicit(&task->wake_error_code, 0, memory_order_release);
    task->state = LLAM_TASK_STATE_PARKED;
    task->wait_reason = LLAM_WAIT_IO;
    if (!llam_task_publish_wait_tracking(task)) {
        (void)llam_task_swap_active_io_req(task, NULL, NULL);
        abort();
    }
    return true;
}

/**
 * @brief Track that a task is parked on a runtime blocking job.
 *
 * @param task         Parked task.
 * @param job          Blocking job being executed by a helper.
 * @param parked_shard Shard where the task parked.
 */
bool llam_task_set_block_tracking(llam_task_t *task, llam_block_job_t *job, unsigned parked_shard) {
    if (task == NULL || job == NULL) {
        errno = EINVAL;
        return false;
    }
    if (!llam_task_prepare_wait_tracking(task)) {
        return false;
    }
    atomic_store_explicit(&task->active_wait_node, NULL, memory_order_release);
    atomic_store_explicit(&task->active_wait_queue, NULL, memory_order_release);
    atomic_store_explicit(&task->active_wait_queue_lock, NULL, memory_order_release);
    atomic_store_explicit(&task->active_select_state, NULL, memory_order_release);
    atomic_store_explicit(&task->active_wait_lifetime_ops, NULL, memory_order_release);
    atomic_store_explicit(&task->active_block_job, job, memory_order_release);
    atomic_store_explicit(&task->join_target, NULL, memory_order_release);
    task->parked_shard = parked_shard;
    atomic_store_explicit(&task->wake_error_code, 0, memory_order_release);
    task->state = LLAM_TASK_STATE_PARKED;
    task->wait_reason = LLAM_WAIT_BLOCKING;
    if (!llam_task_publish_wait_tracking(task)) {
        abort();
    }
    return true;
}

static llam_shard_t *llam_task_deadline_shard(llam_task_t *task) {
    llam_runtime_t *rt = llam_wait_task_runtime(task);

    if (task == NULL || rt == NULL || rt->active_shards == 0U) {
        return NULL;
    }

    if (task->parked_shard < rt->active_shards) {
        return &rt->shards[task->parked_shard];
    }

    /*
     * Teardown, rehome, and partially failed setup paths can leave parked_shard
     * defensive rather than authoritative.  last_shard still maps the task to a
     * valid shard for timer cleanup.
     */
    return &rt->shards[atomic_load_explicit(&task->last_shard, memory_order_relaxed) % rt->active_shards];
}

/**
 * @brief Arm a deadline timer for a parked task.
 *
 * Timed waits all funnel through the same timer node so timeout and cancellation
 * share one wake path.
 *
 * @param task        Task being parked.
 * @param shard       Timer owner shard.
 * @param deadline_ns Absolute wake deadline.
 * @return 0 on success, -1 on invalid args or timer allocation failure.
 */
int llam_arm_task_wait_deadline(llam_task_t *task, llam_shard_t *shard, uint64_t deadline_ns) {
    bool inserted;
    int insert_errno;

    if (task == NULL || shard == NULL) {
        errno = EINVAL;
        return -1;
    }

    task->deadline_ns = deadline_ns;
    pthread_mutex_lock(&shard->lock);
    llam_timer_insert_locked(shard, task);
    insert_errno = errno;
    /*
     * Decide allocation success while the timer heap lock is still held.  A
     * very short deadline can be popped by the watchdog immediately after this
     * lock is released; that also clears active_timer, but it is completion, not
     * allocation failure.
     */
    inserted = task->active_timer != NULL;
    pthread_mutex_unlock(&shard->lock);
    if (!inserted) {
        task->deadline_ns = 0U;
        errno = insert_errno != 0 ? insert_errno : ENOMEM;
        return -1;
    }
    return 0;
}

/**
 * @brief Check whether a task still owns an active deadline timer.
 *
 * @details
 * Timer expiry clears @c active_timer under the shard timer lock, including
 * from watchdog/helper pthreads. Readers must use the same lock; otherwise a
 * producer wake can race with timer expiry while deciding whether a direct
 * handoff is safe.
 *
 * @param task Task whose deadline ownership should be checked.
 * @return true if the task currently has an active timer node.
 */
bool llam_task_wait_deadline_active(llam_task_t *task) {
    llam_shard_t *shard = llam_task_deadline_shard(task);
    bool active = false;

    if (shard == NULL) {
        return false;
    }

    pthread_mutex_lock(&shard->lock);
    active = task->active_timer != NULL;
    pthread_mutex_unlock(&shard->lock);
    return active;
}

/**
 * @brief Remove a task's active wait deadline, if one exists.
 *
 * @param task Task whose timer should be removed.
 */
void llam_disarm_task_wait_deadline(llam_task_t *task) {
    llam_shard_t *shard = llam_task_deadline_shard(task);

    if (shard == NULL) {
        return;
    }
    pthread_mutex_lock(&shard->lock);
    if (task->active_timer != NULL) {
        (void)llam_timer_remove_locked(shard, task);
    }
    pthread_mutex_unlock(&shard->lock);
}

/**
 * @brief Register a task as a cancellation-token waiter.
 *
 * @param task Task with an optional cancel token.
 * @return 0 on success/no token, -1 with ECANCELED if already canceled.
 */
int llam_cancel_token_register_task(llam_task_t *task) {
    llam_cancel_token_t *token;

    if (task == NULL || task->cancel_token == NULL) {
        return 0;
    }

    token = task->cancel_token;
    pthread_mutex_lock(&token->lock);
    if (token->cancelled) {
        pthread_mutex_unlock(&token->lock);
        errno = ECANCELED;
        return -1;
    }

    if (!task->cancel_registered) {
        // Intrusive task links avoid allocating separate cancel wait nodes.
        task->cancel_prev = NULL;
        task->cancel_next = token->waiters;
        if (token->waiters != NULL) {
            token->waiters->cancel_prev = task;
        }
        token->waiters = task;
        task->cancel_registered = true;
    }
    pthread_mutex_unlock(&token->lock);
    return 0;
}

/**
 * @brief Remove a task from its cancellation-token waiter list.
 *
 * @param task Task to unregister.
 */
void llam_cancel_token_unregister_task(llam_task_t *task) {
    llam_cancel_token_t *token;
    llam_task_t *cur;
    bool linked = false;

    if (task == NULL || task->cancel_token == NULL) {
        return;
    }

    token = task->cancel_token;
    pthread_mutex_lock(&token->lock);
    if (task->cancel_registered) {
        for (cur = token->waiters; cur != NULL; cur = cur->cancel_next) {
            if (cur == task) {
                linked = true;
                break;
            }
        }
        if (linked) {
            if (task->cancel_prev != NULL) {
                task->cancel_prev->cancel_next = task->cancel_next;
            } else {
                token->waiters = task->cancel_next;
            }
            if (task->cancel_next != NULL) {
                task->cancel_next->cancel_prev = task->cancel_prev;
            }
        }
        task->cancel_prev = NULL;
        task->cancel_next = NULL;
        task->cancel_registered = false;
    }
    pthread_mutex_unlock(&token->lock);
}

/**
 * @brief Consume and clear a task wake error code.
 *
 * @param task Task to inspect.
 * @return Stored wake error, or 0.
 */
int llam_consume_task_wake_error(llam_task_t *task) {
    int error_code = 0;

    if (task == NULL) {
        return 0;
    }

    error_code = atomic_exchange_explicit(&task->wake_error_code, 0, memory_order_acq_rel);
    return error_code;
}

/**
 * @brief Fill an I/O request with the result for timeout/cancel aborts.
 *
 * @param req    Request being aborted.
 * @param reason Abort reason.
 */
void llam_io_set_abort_result(llam_io_req_t *req, llam_io_abort_reason_t reason) {
    if (req == NULL) {
        return;
    }

    req->poll_revents = 0;
    if (reason == LLAM_IO_ABORT_TIMEOUT && req->kind == LLAM_IO_KIND_POLL) {
        // poll(2) reports timeout as a successful zero-result completion.
        req->result = 0;
        req->error_code = 0;
        return;
    }

    req->result = -1;
    if (reason == LLAM_IO_ABORT_ERROR) {
        if (req->error_code == 0) {
            req->error_code = EIO;
        }
    } else {
        req->error_code = reason == LLAM_IO_ABORT_TIMEOUT ? ETIMEDOUT : ECANCELED;
    }
}

/**
 * @brief Map an I/O abort reason to the scheduler wait reason used for wakeup.
 *
 * @param reason I/O abort reason.
 * @return Wait reason for metrics/tracing.
 */
llam_wait_reason_t llam_io_abort_wait_reason(llam_io_abort_reason_t reason) {
    switch (reason) {
    case LLAM_IO_ABORT_CANCEL:
        return LLAM_WAIT_CANCEL;
    case LLAM_IO_ABORT_TIMEOUT:
        return LLAM_WAIT_TIMEOUT;
    case LLAM_IO_ABORT_ERROR:
        return LLAM_WAIT_IO;
    default:
        return LLAM_WAIT_IO;
    }
}

/**
 * @brief Account timeout/cancel wake metrics on a shard.
 *
 * @param shard  Shard that will wake the task.
 * @param reason I/O abort reason.
 */
void llam_account_io_abort_wake(llam_shard_t *shard, llam_io_abort_reason_t reason) {
    if (shard == NULL) {
        return;
    }

    pthread_mutex_lock(&shard->lock);
    if (reason == LLAM_IO_ABORT_CANCEL) {
        shard->metrics.cancel_wakes += 1U;
    } else if (reason == LLAM_IO_ABORT_TIMEOUT) {
        shard->metrics.timeout_wakes += 1U;
    }
    pthread_mutex_unlock(&shard->lock);
}

/**
 * @brief Resolve the platform I/O node currently responsible for a request.
 *
 * @param req Request to inspect.
 * @return Node index on success, -1 if no node can be resolved.
 */
int llam_io_req_node_index(const llam_io_req_t *req) {
    llam_runtime_t *rt;
    unsigned attached_node_index;
    unsigned shard_id;

    rt = req != NULL ? req->owner_runtime : NULL;
    if (req == NULL || rt == NULL || rt->active_shards == 0U || rt->active_nodes == 0U) {
        return -1;
    }
    attached_node_index = atomic_load_explicit(&req->attached_node_index,
                                               memory_order_acquire);
    if (attached_node_index < rt->active_nodes) {
        return (int)attached_node_index;
    }

    // Requests that have not attached to a node yet route through their owner
    // shard's assigned I/O node.
    shard_id = atomic_load_explicit(&req->owner_shard, memory_order_acquire);
    shard_id = shard_id < rt->active_shards
                   ? shard_id
                   : (shard_id % rt->active_shards);
    return (int)rt->shards[shard_id].io_node_index;
}

/** @brief Revalidate the exact task wait and recyclable request activation. */
static bool llam_io_abort_owner_matches(const llam_task_t *task,
                                        const llam_io_req_t *req,
                                        const llam_runtime_t *rt,
                                        uint64_t operation_generation,
                                        uint64_t wait_generation) {
    return task != NULL && req != NULL && rt != NULL &&
           operation_generation != 0U &&
           req->owner_runtime == rt && req->task == task &&
           llam_task_active_io_req_load(task) == req &&
           atomic_load_explicit(&task->active_io_generation,
                                memory_order_acquire) == operation_generation &&
           atomic_load_explicit(&task->wait_generation,
                                memory_order_acquire) == wait_generation &&
           atomic_load_explicit(&task->state,
                                memory_order_acquire) == LLAM_TASK_STATE_PARKED &&
           atomic_load_explicit(&task->wait_reason,
                                memory_order_acquire) == LLAM_WAIT_IO &&
           atomic_load_explicit(&req->operation_generation,
                                memory_order_acquire) == operation_generation;
}

/**
 * @brief Abort a task's active I/O wait if it can be synchronously detached.
 *
 * @param task   Task parked on I/O.
 * @param reason Abort reason.
 * @param wait_generation Exact task wait epoch owned by the abort producer.
 * @return true if the task was detached and reinjected immediately.
 */
static bool llam_abort_io_wait_impl(llam_task_t *task,
                                    llam_io_abort_reason_t reason,
                                    uint64_t wait_generation,
                                    bool resolver_held) {
    llam_runtime_t *rt = llam_wait_task_runtime(task);
    llam_io_req_t *req;
    uint64_t operation_generation;
    int node_index;
    llam_node_t *node = NULL;
    llam_shard_t *shard;
    unsigned mode;
    bool removed = false;
    bool detached = false;
    unsigned parked_shard;

    if (task == NULL || rt == NULL || rt->active_shards == 0U ||
        wait_generation == 0U || wait_generation == UINT64_MAX) {
        if (resolver_held && task != NULL) {
            llam_task_wait_resolver_end(task);
        }
        return false;
    }

    /*
     * Pin immediately after the published pointer load.  The embedded request
     * may otherwise complete, reset, and represent a later operation at the
     * same address before cancellation reaches the control queue.
     */
    req = llam_task_active_io_req_load(task);
    if (req == NULL || !llam_io_req_lifetime_try_acquire(req)) {
        if (resolver_held) {
            llam_task_wait_resolver_end(task);
        }
        return false;
    }
    operation_generation = (uint64_t)atomic_load_explicit(&req->operation_generation,
                                                           memory_order_acquire);
    for (;;) {
        unsigned attached_node_index;

        if (!llam_io_abort_owner_matches(task,
                                         req,
                                         rt,
                                         operation_generation,
                                         wait_generation)) {
            goto release_req;
        }
        mode = atomic_load_explicit(&req->wait_mode, memory_order_acquire);
        node_index = llam_io_req_node_index(req);
        if (node_index < 0 || (unsigned)node_index >= rt->active_nodes) {
            goto release_req;
        }
        node = &rt->nodes[(unsigned)node_index];

        if (mode == LLAM_IO_WAIT_MODE_SUBMIT_QUEUE) {
            unsigned detached_node_index = UINT_MAX;
            llam_io_submit_detach_result_t submit_result;

            /*
             * The shared helper excludes every cross-node evacuation and
             * resolves the exact queue. The resolver gate keeps task/wait
             * generations stable for the duration of that locked lookup.
             */
            submit_result = llam_detach_submit_req_current(
                req,
                &detached_node_index);
            if (submit_result == LLAM_IO_SUBMIT_DETACH_OWNER_CHANGED) {
                continue;
            }
            if (submit_result == LLAM_IO_SUBMIT_DETACH_REMOVED &&
                detached_node_index < rt->active_nodes) {
                node = &rt->nodes[detached_node_index];
                removed = true;
                break;
            }
            /*
             * Registered cancellation only observes published waits, so a
             * still-SUBMIT request absent from every locked queue is corrupt.
             * Preserve an abort latch for setup publication and stop closed.
             */
            atomic_store_explicit(&req->abort_reason,
                                  (unsigned)reason,
                                  memory_order_release);
            goto release_req;
        }

        attached_node_index = atomic_load_explicit(&req->attached_node_index,
                                                   memory_order_acquire);
        if (attached_node_index < rt->active_nodes &&
            attached_node_index != (unsigned)node_index) {
            continue;
        }
        if (mode == LLAM_IO_WAIT_MODE_POLL_WATCH ||
            mode == LLAM_IO_WAIT_MODE_ACCEPT_WATCH ||
            mode == LLAM_IO_WAIT_MODE_RECV_WATCH) {
            removed = llam_remove_watch_waiter_after_abort(node,
                                                           req,
                                                           mode,
                                                           true);
            if (removed) {
                break;
            }
            if (llam_io_abort_owner_matches(task,
                                            req,
                                            rt,
                                            operation_generation,
                                            wait_generation) &&
                (atomic_load_explicit(&req->wait_mode,
                                      memory_order_acquire) != mode ||
                 atomic_load_explicit(&req->attached_node_index,
                                      memory_order_acquire) !=
                     attached_node_index)) {
                continue;
            }
            /* A completion that already detached the waiter owns the wake. */
            goto release_req;
        }
        if (mode == LLAM_IO_WAIT_MODE_INFLIGHT) {
            if (!llam_io_abort_owner_matches(task,
                                             req,
                                             rt,
                                             operation_generation,
                                             wait_generation) ||
                atomic_load_explicit(&req->wait_mode,
                                     memory_order_acquire) !=
                    LLAM_IO_WAIT_MODE_INFLIGHT ||
                atomic_load_explicit(&req->attached_node_index,
                                     memory_order_acquire) !=
                    attached_node_index) {
                continue;
            }
            /*
             * Backend ownership is stable after the submit-lock transition.
             * Queue cancellation on that current node; the control owns its
             * own request/task pins until completion or teardown.
             */
            atomic_store_explicit(&req->abort_reason,
                                  (unsigned)reason,
                                  memory_order_release);
            if (atomic_exchange_explicit(&req->cancel_queued,
                                         1U,
                                         memory_order_acq_rel) == 0U &&
                llam_node_queue_control(node,
                                        LLAM_IO_CONTROL_REQ_CANCEL,
                                        req) != 0) {
                atomic_store_explicit(&req->cancel_queued,
                                      0U,
                                      memory_order_release);
            }
            goto release_req;
        }
        goto release_req;
    }

    llam_io_set_abort_result(req, reason);
    parked_shard = atomic_load_explicit(&task->parked_shard,
                                        memory_order_acquire);
    shard = &rt->shards[parked_shard % rt->active_shards];
    if (resolver_held) {
        llam_task_wait_resolver_end(task);
        resolver_held = false;
    }
    llam_account_io_abort_wake(shard, reason);
    /*
     * Keep the wait ownership intact until the generic reinject path runs.
     * It still needs the original parked_shard to remove any active deadline
     * timer from the correct shard before clearing task tracking.
     */
    llam_reinject_task_on_shard(rt,
                              task,
                              parked_shard,
                              true,
                              LLAM_TRACE_WAKE,
                              llam_io_abort_wait_reason(reason));
    detached = true;

release_req:
    if (resolver_held) {
        llam_task_wait_resolver_end(task);
    }
    /* A queued control has acquired its own ref before this transient is put. */
    (void)llam_io_req_lifetime_release(req);
    return detached;
}

bool llam_abort_io_wait(llam_task_t *task,
                        llam_io_abort_reason_t reason,
                        uint64_t wait_generation) {
    return llam_abort_io_wait_impl(task, reason, wait_generation, false);
}

bool llam_abort_io_wait_claimed(llam_task_t *task,
                                llam_io_abort_reason_t reason,
                                uint64_t wait_generation) {
    return llam_abort_io_wait_impl(task, reason, wait_generation, true);
}

/**
 * @brief Wake a wait-node task, optionally fusing the wake with a direct handoff.
 *
 * @param node          Wait node whose task should be reinjected.
 * @param hot           Whether to prefer hot-lane enqueue.
 * @param reason        Wait reason being resolved.
 * @param allow_handoff Whether a same-shard task-to-task handoff may be tried.
 * @return true when the current task switched directly to the woken task.
 */
bool llam_wake_wait_node_and_maybe_handoff(llam_wait_node_t *node,
                                           bool hot,
                                           llam_wait_reason_t reason,
                                           bool allow_handoff) {
    llam_runtime_t *rt;
    llam_task_t *task;
    unsigned parked_shard;

    if (node == NULL || node->task == NULL) {
        return false;
    }
    task = node->task;
    rt = llam_wait_task_runtime(task);
    if (rt == NULL) {
        return false;
    }
    if (node->select_state != NULL) {
        if (!llam_channel_select_node_should_wake(node)) {
            return false;
        }
    } else if (!llam_wait_node_prepare_wake(node)) {
        return false;
    }

    parked_shard = task->parked_shard;
    if (allow_handoff && rt->wake_handoff_enabled != 0U) {
        llam_shard_t *stats_shard = g_llam_tls_shard;
        llam_wake_handoff_fail_t fail_reason = llam_wake_handoff_precheck(rt, task, parked_shard);

        llam_wake_handoff_record_attempt(stats_shard);
        if (fail_reason == LLAM_WAKE_HANDOFF_FAIL_NONE) {
            if (llam_reinject_task_on_shard_and_yield_current(rt,
                                                              task,
                                                              parked_shard,
                                                              hot,
                                                              LLAM_TRACE_WAKE,
                                                              reason)) {
                llam_wake_handoff_record_hit(stats_shard);
                return true;
            }
            fail_reason = LLAM_WAKE_HANDOFF_FAIL_RACE;
        }
        llam_wake_handoff_record_fail(stats_shard, fail_reason);
    }

    llam_reinject_task_on_shard(rt, task, parked_shard, hot, LLAM_TRACE_WAKE, reason);
    return false;
}

/**
 * @brief Wake the task referenced by a wait node.
 *
 * @param node   Wait node whose task should be reinjected.
 * @param hot    Whether to prefer hot-lane enqueue.
 * @param reason Wait reason being resolved.
 */
void llam_wake_wait_node(llam_wait_node_t *node, bool hot, llam_wait_reason_t reason) {
    (void)llam_wake_wait_node_and_maybe_handoff(node, hot, reason, false);
}

/**
 * @brief Park the current managed task and switch back to its scheduler.
 *
 * @param reason Wait reason recorded on the task.
 * @param kind   Trace event kind.
 */
void llam_park_current_task(llam_wait_reason_t reason, llam_trace_kind_t kind) {
    llam_shard_t *shard = g_llam_tls_shard;
    llam_task_t *task = g_llam_tls_task;
    llam_ctx_t *scheduler_ctx;

    if (shard == NULL || task == NULL) {
        return;
    }

    scheduler_ctx = g_llam_tls_scheduler_ctx != NULL ? g_llam_tls_scheduler_ctx : &shard->scheduler_ctx;
    llam_task_ensure_listed(task);
    task->state = LLAM_TASK_STATE_PARKED;
    task->wait_reason = reason;
    shard->metrics.parks += 1U;
    llam_trace_shard(shard, task, kind, LLAM_TASK_STATE_RUNNING, LLAM_TASK_STATE_PARKED, reason);
    llam_task_sample_live_stack(task);
    // The scheduler resumes after this context switch and will later reinject
    // the task when its wait completes.
    llam_switch_task_to_scheduler(task, scheduler_ctx);
}

/**
 * @brief Requeue the common same-shard single join waiter without generic wake routing.
 *
 * @details
 * Spawn/join fanout overwhelmingly wakes exactly one waiter on the same shard
 * that parked it.  The generic reinject path must handle migration, pressure,
 * timers, and cross-worker kicks; this path keeps those semantics out of the
 * hot join wake when they are provably unnecessary.
 *
 * @param rt     Runtime that owns the waiter.
 * @param waiter Single waiter removed from a completed task's join list.
 * @return true when the waiter was published locally.
 */
static bool llam_reinject_single_local_join_waiter(llam_runtime_t *rt, llam_task_t *waiter) {
    llam_shard_t *target;
    llam_task_state_id_t from;

    if (rt == NULL || waiter == NULL || waiter->wait_next != NULL || waiter->parked_shard >= rt->active_shards) {
        return false;
    }
    target = &rt->shards[waiter->parked_shard];
    if (g_llam_tls_shard != target ||
        g_llam_tls_task == NULL ||
        !llam_shard_accepts_new_work(target) ||
        llam_task_wait_deadline_active(waiter)) {
        return false;
    }

    pthread_mutex_lock(&target->lock);
    if (target->opaque_redirect_active) {
        pthread_mutex_unlock(&target->lock);
        return false;
    }
    /*
     * The local fast path can still reject the waiter above.  Do not clear wait
     * ownership until this path has committed; the generic reinject fallback
     * needs the original parked_shard and join tracking intact.
     */
    from = waiter->state;
    llam_task_clear_wait_tracking_or_abort(waiter);
    waiter->state = LLAM_TASK_STATE_RUNNABLE;
    waiter->wait_reason = LLAM_WAIT_NONE;
    waiter->enqueue_hot = 0U;
    waiter->last_runnable_ns = llam_runtime_should_stamp_runnable_latency(target) ? llam_now_ns() : 0U;
    if (llam_queue_push_bounded_locked(target, &target->hot_q, LLAM_HOT_QUEUE_CAP, waiter)) {
        target->metrics.hot_enqueues += 1U;
    }
    target->metrics.wakes += 1U;
    target->metrics.wake_reason_hist[LLAM_WAIT_JOIN] += 1U;
    llam_trace_shard(target, waiter, LLAM_TRACE_WAKE, from, LLAM_TASK_STATE_RUNNABLE, LLAM_WAIT_JOIN);
    pthread_mutex_unlock(&target->lock);
    return true;
}

/**
 * @brief Wake every task waiting to join a completed task.
 *
 * @param rt   Runtime owning the tasks.
 * @param task Completed task whose join waiters should be woken.
 */
void llam_reinject_join_waiters(llam_runtime_t *rt, llam_task_t *task) {
    llam_task_t *waiters;

    pthread_mutex_lock(&task->lock);
    waiters = task->join_waiters;
    // Preserve the exit-time waiter count for reclamation ownership.
    task->join_waiter_count_at_exit = task->join_waiter_count;
    task->join_waiters = NULL;
    task->join_waiter_count = 0U;
    atomic_store_explicit(&task->join_waiter_hint, 0U, memory_order_release);
    pthread_mutex_unlock(&task->lock);

    if (waiters == NULL) {
        return;
    }

    while (waiters != NULL) {
        llam_task_t *next = waiters->wait_next;
        waiters->wait_next = NULL;
        if (!llam_reinject_single_local_join_waiter(rt, waiters)) {
            llam_reinject_task_on_shard(rt, waiters, waiters->parked_shard, true, LLAM_TRACE_WAKE, LLAM_WAIT_JOIN);
        }
        waiters = next;
    }
}

/**
 * @brief Remove a specific join waiter while the target task lock is held.
 *
 * @param target Join target.
 * @param waiter Waiter task to remove.
 * @return true if the waiter was found and removed.
 */
bool llam_join_waiter_remove_locked(llam_task_t *target, llam_task_t *waiter) {
    llam_task_t *prev = NULL;
    llam_task_t *cur = target->join_waiters;

    while (cur != NULL) {
        if (cur == waiter) {
            if (prev != NULL) {
                prev->wait_next = cur->wait_next;
            } else {
                target->join_waiters = cur->wait_next;
            }
            cur->wait_next = NULL;
            if (target->join_waiter_count > 0U) {
                target->join_waiter_count -= 1U;
            }
            atomic_store_explicit(&target->join_waiter_hint, target->join_waiter_count, memory_order_release);
            return true;
        }
        prev = cur;
        cur = cur->wait_next;
    }

    return false;
}
