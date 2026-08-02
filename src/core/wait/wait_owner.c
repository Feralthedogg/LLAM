/**
 * @file src/core/wait/wait_owner.c
 * @brief Wait-owner publication, generations, and active request tracking.
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

llam_runtime_t *llam_wait_task_runtime(const llam_task_t *task) {
    return task != NULL ? task->owner_runtime : NULL;
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
