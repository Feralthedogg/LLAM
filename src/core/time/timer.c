/**
 * @file src/core/time/timer.c
 * @brief Timer heap management and deadline expiration dispatch.
 *
 * @details
 * Each shard owns a min-heap of timer nodes keyed by absolute monotonic
 * deadlines. Timer nodes are independently allocated so an expired callback
 * record cannot be overwritten when its task starts another timed wait.
 *
 * The shard lock protects heap structure and task timer ownership. Expired
 * nodes are detached under the lock and processed afterward so reinjection and
 * wait-queue removal do not hold the timer heap lock longer than necessary.
 *
 * @copyright Copyright 2026 Feralthedogg
 *
 * @par License
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 * Licensed under the LLAM Commercial Reciprocity License 1.0.
 * See the LICENSE file distributed with this Software.
 */

#include "runtime_internal.h"

/** @brief Return the runtime that owns a parked task's timer/wait state. */
static llam_runtime_t *llam_timer_task_runtime(const llam_task_t *task) {
    return task != NULL ? task->owner_runtime : NULL;
}

/** @brief Return whether an immutable timer record still names the live wait. */
static bool llam_timer_wait_generation_matches(const llam_timer_node_t *timer) {
    llam_task_t *task;
    uint64_t generation;

    if (timer == NULL || timer->task == NULL) {
        return false;
    }
    task = timer->task;
    generation = timer->wait_generation;
    return generation != 0U && generation != UINT64_MAX &&
           atomic_load_explicit(&task->wait_generation, memory_order_acquire) ==
               generation;
}

/**
 * @brief Drop every exact lifetime reference owned by a timer record.
 *
 * Task scan ownership is released last because the timer node, select state,
 * and primitive active-op pointer can all be embedded in or reached from task
 * wait state.
 */
static void llam_timer_release_ownership(llam_shard_t *shard,
                                         llam_timer_node_t *timer) {
    llam_task_t *task;
    llam_runtime_t *rt;
    llam_channel_select_state_t *select_state;
    _Atomic size_t *lifetime_ops;
    bool holds_task_ref;

    if (timer == NULL) {
        return;
    }
    task = timer->task;
    rt = llam_timer_task_runtime(task);
    select_state = timer->select_state;
    lifetime_ops = timer->wait_lifetime_ops;
    holds_task_ref = timer->holds_task_ref;
    timer->task = NULL;
    timer->select_state = NULL;
    timer->wait_lifetime_ops = NULL;
    timer->holds_task_ref = false;

    if (select_state != NULL && rt != NULL) {
        (void)llam_sync_complete_inflight_waiter(rt,
                                                 &select_state->timer_refs,
                                                 1U);
    }
    llam_public_active_op_end(lifetime_ops);
    llam_timer_node_free(shard, timer);
    if (holds_task_ref && rt != NULL && task != NULL) {
        (void)llam_task_scan_ref_release(rt, task);
    }
}

/**
 * @brief Insert a task's active deadline into the shard timer heap.
 *
 * The timer record owns a task wait-generation snapshot, an exact primitive
 * active-op pin for scalar/join waits, and a select-state reference when the
 * wait uses stack-backed channel select state.
 *
 * @param shard Shard owning the heap. The shard lock must already be held.
 * @param task  Task with @c deadline_ns already populated.
 */
void llam_timer_insert_locked(llam_shard_t *shard, llam_task_t *task) {
    llam_timer_node_t *node;
    llam_channel_select_state_t *select_state;
    _Atomic size_t *lifetime_ops;
    uint64_t wait_generation;

    if (shard == NULL || task == NULL || task->active_timer != NULL) {
        return;
    }

    wait_generation = (uint64_t)atomic_load_explicit(&task->wait_generation,
                                                     memory_order_acquire);
    if (LLAM_UNLIKELY(wait_generation == 0U || wait_generation == UINT64_MAX)) {
        errno = EOVERFLOW;
        return;
    }
    node = llam_timer_node_alloc(shard);
    if (node == NULL) {
        return;
    }
    lifetime_ops = (_Atomic size_t *)atomic_load_explicit(
        &task->active_wait_lifetime_ops,
        memory_order_acquire);
    if (lifetime_ops != NULL &&
        llam_public_active_op_try_begin(lifetime_ops) != 0) {
        llam_timer_node_free(shard, node);
        return;
    }
    select_state = atomic_load_explicit(&task->active_select_state,
                                        memory_order_acquire);
    if (select_state != NULL &&
        !llam_sync_note_inflight_waiter(task->owner_runtime,
                                        &select_state->timer_refs,
                                        1U)) {
        llam_public_active_op_end(lifetime_ops);
        llam_timer_node_free(shard, node);
        return;
    }

    node->task = task;
    node->select_state = select_state;
    node->wait_lifetime_ops = lifetime_ops;
    node->deadline_ns = task->deadline_ns;
    node->wait_generation = wait_generation;
    if (!llam_timer_heap_push_locked(shard, node)) {
        llam_timer_release_ownership(shard, node);
        return;
    }
    task->active_timer = node;
}

/**
 * @brief Remove a task's timer from a shard heap.
 *
 * The fast path trusts the node's heap index. A defensive linear scan handles
 * stale indexes without corrupting the heap, which keeps cancellation paths
 * robust across future timer-node ownership changes.
 *
 * @param shard Shard owning the heap. The shard lock must already be held.
 * @param task  Task whose active timer should be removed.
 *
 * @return @c true when a timer was found and removed.
 */
bool llam_timer_remove_locked(llam_shard_t *shard, llam_task_t *task) {
    llam_timer_node_t *node;
    size_t i;

    if (task == NULL || task->active_timer == NULL) {
        return false;
    }

    node = task->active_timer;
    if (node->heap_index < shard->timer_heap_len && shard->timer_heap[node->heap_index] == node) {
        (void)llam_timer_heap_remove_at_locked(shard, node->heap_index);
        task->active_timer = NULL;
        task->deadline_ns = 0U;
        node->next = NULL;
        llam_timer_release_ownership(shard, node);
        return true;
    }

    for (i = 0U; i < shard->timer_heap_len; ++i) {
        if (shard->timer_heap[i] == node) {
            (void)llam_timer_heap_remove_at_locked(shard, i);
            task->active_timer = NULL;
            task->deadline_ns = 0U;
            node->next = NULL;
            llam_timer_release_ownership(shard, node);
            return true;
        }
    }

    return false;
}

/**
 * @brief Cancel whatever wait structure currently owns a parked task.
 *
 * This is the shared cancellation resolver for sleep, join, synchronization
 * waits, I/O waits, and blocking-worker jobs. It removes the task from the
 * owning data structure before reinjecting it, preventing a later timeout or
 * completion from waking the same task twice.
 *
 * @param task Parked task to cancel.
 */
static void llam_cancel_wait_metric(llam_runtime_t *rt, unsigned parked_shard) {
    llam_shard_t *shard;

    if (rt == NULL || rt->active_shards == 0U) {
        return;
    }
    shard = &rt->shards[parked_shard % rt->active_shards];
    pthread_mutex_lock(&shard->lock);
    shard->metrics.cancel_wakes += 1U;
    pthread_mutex_unlock(&shard->lock);
}

/* Sleep expiry clears tracking while holding the shard lock, so use lock->gate. */
static void llam_cancel_sleep_wait(llam_task_t *task, llam_runtime_t *rt) {
    unsigned parked_shard;
    llam_shard_t *shard;
    llam_wait_node_t *node = NULL;
    uint64_t generation;
    bool removed = false;

    parked_shard = atomic_load_explicit(&task->parked_shard, memory_order_acquire);
    if (parked_shard >= rt->active_shards) {
        return;
    }
    shard = &rt->shards[parked_shard];
    pthread_mutex_lock(&shard->lock);
    if (!llam_task_wait_resolver_try_begin(task)) {
        pthread_mutex_unlock(&shard->lock);
        return;
    }
    generation = (uint64_t)atomic_load_explicit(&task->wait_generation,
                                                memory_order_acquire);
    if (generation != 0U && generation != UINT64_MAX &&
        atomic_load_explicit(&task->state, memory_order_acquire) ==
            LLAM_TASK_STATE_PARKED &&
        atomic_load_explicit(&task->wait_reason, memory_order_acquire) ==
            LLAM_WAIT_SLEEP &&
        atomic_load_explicit(&task->parked_shard, memory_order_acquire) ==
            parked_shard) {
        node = atomic_load_explicit(&task->active_wait_node, memory_order_acquire);
        removed = llam_timer_remove_locked(shard, task);
        if (removed) {
            shard->metrics.cancel_wakes += 1U;
        }
    }
    pthread_mutex_unlock(&shard->lock);
    if (removed) {
        atomic_store_explicit(&task->wake_error_code, ECANCELED, memory_order_release);
        if (node != NULL) {
            node->error_code = ECANCELED;
        }
    }
    llam_task_wait_resolver_end(task);
    if (removed && (node == NULL || llam_wait_node_prepare_wake(node))) {
        llam_reinject_task_on_shard(rt,
                                  task,
                                  parked_shard,
                                  true,
                                  LLAM_TRACE_WAKE,
                                  LLAM_WAIT_CANCEL);
    }
}

void llam_cancel_task_wait(llam_task_t *task) {
    llam_runtime_t *rt = llam_timer_task_runtime(task);
    llam_wait_reason_t wait_reason;
    uint64_t wait_generation;
    unsigned parked_shard;
    bool resolver_held = false;
    bool removed = false;

    if (task == NULL || rt == NULL || rt->active_shards == 0U) {
        return;
    }

    wait_reason = (llam_wait_reason_t)atomic_load_explicit(&task->wait_reason,
                                                           memory_order_acquire);
    if (wait_reason == LLAM_WAIT_SLEEP) {
        llam_cancel_sleep_wait(task, rt);
        return;
    }
    if (!llam_task_wait_resolver_try_begin(task)) {
        return;
    }
    resolver_held = true;
    wait_generation = (uint64_t)atomic_load_explicit(&task->wait_generation,
                                                     memory_order_acquire);
    wait_reason = (llam_wait_reason_t)atomic_load_explicit(&task->wait_reason,
                                                           memory_order_acquire);
    parked_shard = atomic_load_explicit(&task->parked_shard, memory_order_acquire);
    if (atomic_load_explicit(&task->state, memory_order_acquire) !=
            LLAM_TASK_STATE_PARKED ||
        wait_generation == 0U || wait_generation == UINT64_MAX) {
        goto done;
    }
    if (wait_reason == LLAM_WAIT_SLEEP) {
        llam_task_wait_resolver_end(task);
        llam_cancel_sleep_wait(task, rt);
        return;
    }

    switch (wait_reason) {
    case LLAM_WAIT_JOIN: {
        llam_task_t *join_target = atomic_load_explicit(&task->join_target,
                                                        memory_order_acquire);

        if (join_target == NULL ||
            atomic_load_explicit(&task->wait_generation, memory_order_acquire) !=
                wait_generation) {
            break;
        }
        pthread_mutex_lock(&join_target->lock);
        if (atomic_load_explicit(&task->wait_generation, memory_order_acquire) ==
                wait_generation &&
            atomic_load_explicit(&task->join_target, memory_order_acquire) ==
                join_target &&
            atomic_load_explicit(&task->state, memory_order_acquire) ==
                LLAM_TASK_STATE_PARKED &&
            atomic_load_explicit(&task->wait_reason, memory_order_acquire) ==
                LLAM_WAIT_JOIN) {
            removed = llam_join_waiter_remove_locked(join_target, task);
        }
        pthread_mutex_unlock(&join_target->lock);
        if (removed) {
            atomic_store_explicit(&task->wake_error_code,
                                  ECANCELED,
                                  memory_order_release);
            llam_task_wait_resolver_end(task);
            resolver_held = false;
            llam_cancel_wait_metric(rt, parked_shard);
            llam_reinject_task(rt, task, true, LLAM_TRACE_WAKE, LLAM_WAIT_CANCEL);
        }
        break;
    }
    case LLAM_WAIT_MUTEX:
    case LLAM_WAIT_COND:
    case LLAM_WAIT_CHANNEL_SEND:
    case LLAM_WAIT_CHANNEL_RECV: {
        llam_channel_select_state_t *select_state = atomic_load_explicit(
            &task->active_select_state,
            memory_order_acquire);

        if (select_state != NULL) {
            llam_select_completion_result_t completion =
                llam_channel_select_abort_task_wait_claimed(task,
                                                            select_state,
                                                            wait_generation,
                                                            ECANCELED);

            if (completion != LLAM_SELECT_COMPLETION_LOST) {
                llam_task_wait_resolver_end(task);
                resolver_held = false;
                llam_cancel_wait_metric(rt, parked_shard);
                if (completion == LLAM_SELECT_COMPLETION_QUEUED) {
                    llam_reinject_task_on_shard(rt,
                                              task,
                                              parked_shard,
                                              true,
                                              LLAM_TRACE_WAKE,
                                              LLAM_WAIT_CANCEL);
                }
            }
            break;
        }
        {
            llam_wait_queue_t *queue = atomic_load_explicit(
                &task->active_wait_queue,
                memory_order_acquire);
            pthread_mutex_t *queue_lock = atomic_load_explicit(
                &task->active_wait_queue_lock,
                memory_order_acquire);
            llam_wait_node_t *node = atomic_load_explicit(
                &task->active_wait_node,
                memory_order_acquire);

            if (queue == NULL || queue_lock == NULL || node == NULL ||
                atomic_load_explicit(&task->wait_generation,
                                     memory_order_acquire) != wait_generation) {
                break;
            }
            pthread_mutex_lock(queue_lock);
            if (atomic_load_explicit(&task->wait_generation,
                                     memory_order_acquire) == wait_generation &&
                atomic_load_explicit(&task->active_wait_queue,
                                     memory_order_acquire) == queue &&
                atomic_load_explicit(&task->active_wait_queue_lock,
                                     memory_order_acquire) == queue_lock &&
                atomic_load_explicit(&task->active_wait_node,
                                     memory_order_acquire) == node &&
                atomic_load_explicit(&task->state, memory_order_acquire) ==
                    LLAM_TASK_STATE_PARKED &&
                atomic_load_explicit(&task->wait_reason, memory_order_acquire) ==
                    (unsigned)wait_reason) {
                removed = llam_wait_queue_remove(queue, node);
                if (removed) {
                    node->error_code = ECANCELED;
                }
            }
            pthread_mutex_unlock(queue_lock);
            if (removed) {
                atomic_store_explicit(&task->wake_error_code,
                                      ECANCELED,
                                      memory_order_release);
                llam_task_wait_resolver_end(task);
                resolver_held = false;
                llam_cancel_wait_metric(rt, parked_shard);
                llam_wake_wait_node(node, true, LLAM_WAIT_CANCEL);
            }
        }
        break;
    }
    case LLAM_WAIT_IO:
        resolver_held = false;
        (void)llam_abort_io_wait_claimed(task,
                                         LLAM_IO_ABORT_CANCEL,
                                         wait_generation);
        break;
    case LLAM_WAIT_BLOCKING: {
        llam_block_job_t *job = llam_task_active_block_job_load(task);
        llam_wait_node_t *node;
        unsigned expected = LLAM_BLOCK_JOB_QUEUED;

        if (job == NULL || job->task != task ||
            atomic_load_explicit(&task->wait_generation, memory_order_acquire) !=
                wait_generation ||
            llam_task_active_block_job_load(task) != job ||
            atomic_load_explicit(&task->state, memory_order_acquire) !=
                LLAM_TASK_STATE_PARKED ||
            atomic_load_explicit(&task->wait_reason, memory_order_acquire) !=
                LLAM_WAIT_BLOCKING) {
            break;
        }
        if (atomic_compare_exchange_strong_explicit(&job->state,
                                                    &expected,
                                                    LLAM_BLOCK_JOB_ABORTED,
                                                    memory_order_acq_rel,
                                                    memory_order_acquire)) {
            node = job->wait_node;
            atomic_store_explicit(&task->wake_error_code,
                                  ECANCELED,
                                  memory_order_release);
            if (node != NULL) {
                node->error_code = ECANCELED;
            }
            llam_task_wait_resolver_end(task);
            resolver_held = false;
            llam_cancel_wait_metric(rt, parked_shard);
            if (node == NULL || llam_wait_node_prepare_wake(node)) {
                llam_reinject_task_on_shard(rt,
                                          task,
                                          parked_shard,
                                          true,
                                          LLAM_TRACE_WAKE,
                                          LLAM_WAIT_CANCEL);
            }
            break;
        }
        expected = LLAM_BLOCK_JOB_RUNNING;
        if (job->task != task ||
            atomic_load_explicit(&task->wait_generation, memory_order_acquire) !=
                wait_generation ||
            llam_task_active_block_job_load(task) != job ||
            atomic_load_explicit(&task->state, memory_order_acquire) !=
                LLAM_TASK_STATE_PARKED ||
            atomic_load_explicit(&task->wait_reason, memory_order_acquire) !=
                LLAM_WAIT_BLOCKING) {
            break;
        }
        (void)atomic_compare_exchange_strong_explicit(&job->state,
                                                      &expected,
                                                      LLAM_BLOCK_JOB_ABORTED,
                                                      memory_order_acq_rel,
                                                      memory_order_acquire);
        break;
    }
    default:
        break;
    }

done:
    if (resolver_held) {
        llam_task_wait_resolver_end(task);
    }
}

/**
 * @brief Cancel every currently parked managed wait during runtime stop.
 *
 * @details
 * Runtime stop/fatal shutdown is not tied to a user cancellation token, so
 * parked waits on channels, mutexes, joins, sleeps, I/O, or blocking callbacks
 * would otherwise keep the live-task count non-zero forever.  The diagnostic
 * task lists already contain every parked task; this pass takes short scan
 * references under the shard-list locks, then resolves waits outside those
 * locks to preserve the wait-queue lock ordering used by normal producers.
 *
 * @param rt Runtime whose listed parked tasks should observe ECANCELED.
 */
void llam_runtime_cancel_parked_waiters(llam_runtime_t *rt) {
    llam_task_t *stack_tasks[64];
    llam_task_t **tasks;
    size_t count = 0U;
    size_t capacity;
    bool heap_tasks = true;
    size_t max_passes;
    size_t pass;
    unsigned i;

    if (rt == NULL || rt->shards == NULL || rt->active_shards == 0U) {
        return;
    }

    capacity = (size_t)llam_runtime_live_tasks(rt) + (size_t)rt->active_shards + 16U;
    tasks = calloc(capacity, sizeof(*tasks));
    if (tasks == NULL) {
        /*
         * Runtime stop must not depend on heap availability. If OOM hits while
         * stopping, cancel parked tasks in fixed-size batches; otherwise a
         * single sleeping/channel/I/O waiter can keep llam_run() alive forever.
         */
        tasks = stack_tasks;
        capacity = sizeof(stack_tasks) / sizeof(stack_tasks[0]);
        heap_tasks = false;
    }

    max_passes = heap_tasks ? 1U : ((size_t)llam_runtime_live_tasks(rt) / capacity) + (size_t)rt->active_shards + 2U;
    for (pass = 0U; pass < max_passes; ++pass) {
        count = 0U;

        for (i = 0U; i < rt->active_shards && count < capacity; ++i) {
            llam_shard_t *owner = &rt->shards[i];
            llam_task_t *task;

            if (!owner->lock_initialized) {
                continue;
            }
            pthread_mutex_lock(&owner->lock);
            for (task = owner->all_tasks; task != NULL && count < capacity; task = task->all_next) {
                llam_wait_reason_t wait_reason = (llam_wait_reason_t)atomic_load_explicit(&task->wait_reason, memory_order_acquire);
                if (task->state != LLAM_TASK_STATE_PARKED || wait_reason == LLAM_WAIT_NONE ||
                    atomic_load_explicit(&task->reclaim_claimed, memory_order_acquire) != 0U) {
                    continue;
                }
                if (!llam_task_scan_ref_try_acquire(rt, task)) {
                    continue;
                }
                tasks[count++] = task;
            }
            pthread_mutex_unlock(&owner->lock);
        }

        for (i = 0U; i < count; ++i) {
            llam_cancel_task_wait(tasks[i]);
            (void)llam_task_scan_ref_release(rt, tasks[i]);
        }

        if (heap_tasks || count < capacity) {
            break;
        }
    }

    if (heap_tasks) {
        free(tasks);
    }
}

/**
 * @brief Resolve a deadline expiration for a parked task.
 *
 * Sleep timers already own their timer node, while other wait kinds need to be
 * removed from their join/wait/I/O structures before wakeup. Successful timeout
 * resolution stores the appropriate wake error and reinjects the task.
 *
 * @param timer Immutable callback record for the expired wait generation.
 */
static void llam_timeout_task_wait(llam_timer_node_t *timer) {
    llam_task_t *task = timer != NULL ? timer->task : NULL;
    llam_runtime_t *rt = llam_timer_task_runtime(task);
    bool removed = false;
    uint64_t wait_generation = timer != NULL ? timer->wait_generation : 0U;

    if (!llam_timer_wait_generation_matches(timer) ||
        task->state != LLAM_TASK_STATE_PARKED ||
        rt == NULL || rt->active_shards == 0U) {
        return;
    }

    llam_wait_reason_t wait_reason = (llam_wait_reason_t)atomic_load_explicit(&task->wait_reason, memory_order_acquire);
    if (!llam_timer_wait_generation_matches(timer)) {
        return;
    }
    switch (wait_reason) {
    case LLAM_WAIT_SLEEP: {
        llam_shard_t *shard = &rt->shards[task->parked_shard % rt->active_shards];
        llam_wait_node_t *node = atomic_load_explicit(&task->active_wait_node, memory_order_acquire);

        shard->metrics.timeout_wakes += 1U;
        atomic_store_explicit(&task->wake_error_code, 0, memory_order_release);
        if (node != NULL) {
            node->error_code = 0;
        }
        if (node == NULL || llam_wait_node_prepare_wake(node)) {
            llam_reinject_task_on_shard(rt,
                                      task,
                                      task->parked_shard,
                                      true,
                                      LLAM_TRACE_WAKE,
                                      LLAM_WAIT_TIMEOUT);
        }
        break;
    }
    case LLAM_WAIT_JOIN:
        {
            llam_task_t *join_target = atomic_load_explicit(&task->join_target, memory_order_acquire);
            if (join_target == NULL) {
                break;
            }
            llam_shard_t *shard = &rt->shards[task->parked_shard % rt->active_shards];

            pthread_mutex_lock(&join_target->lock);
            if (atomic_load_explicit(&task->wait_generation, memory_order_acquire) ==
                    wait_generation &&
                atomic_load_explicit(&task->join_target, memory_order_acquire) ==
                    join_target) {
                removed = llam_join_waiter_remove_locked(join_target, task);
            }
            pthread_mutex_unlock(&join_target->lock);
            if (removed) {
                shard->metrics.timeout_wakes += 1U;
                atomic_store_explicit(&task->wake_error_code, ETIMEDOUT, memory_order_release);
                llam_reinject_task(rt, task, true, LLAM_TRACE_WAKE, LLAM_WAIT_TIMEOUT);
            }
        }
        break;
    case LLAM_WAIT_MUTEX:
    case LLAM_WAIT_COND:
    case LLAM_WAIT_CHANNEL_SEND:
    case LLAM_WAIT_CHANNEL_RECV:
        if (timer->select_state != NULL) {
            if (llam_channel_select_abort_task_wait_generation(task,
                                                               timer->select_state,
                                                               wait_generation,
                                                               ETIMEDOUT,
                                                               LLAM_WAIT_TIMEOUT)) {
                llam_shard_t *shard = &rt->shards[task->parked_shard % rt->active_shards];

                shard->metrics.timeout_wakes += 1U;
            }
            break;
        }
        {
            llam_wait_queue_t *queue = atomic_load_explicit(&task->active_wait_queue, memory_order_acquire);
            pthread_mutex_t *queue_lock = atomic_load_explicit(&task->active_wait_queue_lock, memory_order_acquire);
            llam_wait_node_t *node = atomic_load_explicit(&task->active_wait_node, memory_order_acquire);
            llam_shard_t *shard = &rt->shards[task->parked_shard % rt->active_shards];

            if (queue == NULL || queue_lock == NULL || node == NULL) {
                break;
            }
            if (atomic_load_explicit(&task->wait_generation, memory_order_acquire) !=
                wait_generation) {
                break;
            }
            pthread_mutex_lock(queue_lock);
            if (atomic_load_explicit(&task->wait_generation, memory_order_acquire) ==
                    wait_generation &&
                atomic_load_explicit(&task->active_wait_queue, memory_order_acquire) ==
                    queue &&
                atomic_load_explicit(&task->active_wait_queue_lock,
                                     memory_order_acquire) == queue_lock &&
                atomic_load_explicit(&task->active_wait_node, memory_order_acquire) ==
                    node) {
                removed = llam_wait_queue_remove(queue, node);
            }
            if (removed) {
                node->error_code = ETIMEDOUT;
            }
            pthread_mutex_unlock(queue_lock);
            if (removed) {
                shard->metrics.timeout_wakes += 1U;
                llam_wake_wait_node(node, true, LLAM_WAIT_TIMEOUT);
            }
        }
        break;
    case LLAM_WAIT_IO:
        if (atomic_load_explicit(&task->wait_generation, memory_order_acquire) ==
            wait_generation) {
            (void)llam_abort_io_wait(task,
                                     LLAM_IO_ABORT_TIMEOUT,
                                     wait_generation);
        }
        break;
    default:
        break;
    }
}

/**
 * @brief Ensure the shard has a ready opaque helper thread.
 *
 * The caller must hold @c shard->opaque_lock. Existing helper startup is waited
 * out; new helper startup publishes readiness through the opaque wake path.
 *
 * @param shard Shard requiring helper compensation.
 *
 * @return 0 when the helper is ready.
 * @return -1 with @c errno set when helper creation or startup fails.
 */
int llam_ensure_opaque_helper_locked(llam_shard_t *shard) {
    int rc;

    if (shard->opaque_helper_thread_started) {
        while (!shard->opaque_helper_ready && !shard->opaque_helper_failed) {
            llam_opaque_wake_wait(shard);
        }
        if (shard->opaque_helper_failed) {
            errno = EIO;
            return -1;
        }
        return 0;
    }

    shard->opaque_helper_stop = false;
    shard->opaque_helper_failed = false;
    shard->opaque_helper_ready = false;
    shard->opaque_helper_active = false;
    atomic_store_explicit(&shard->opaque_helper_active_hint, 0U, memory_order_release);
    rc = pthread_create(&shard->opaque_helper_thread, NULL, llam_opaque_helper_main, shard);
    if (rc != 0) {
        shard->opaque_helper_failed = true;
        errno = rc;
        return -1;
    }
    shard->opaque_helper_thread_started = true;
    while (!shard->opaque_helper_ready && !shard->opaque_helper_failed) {
        llam_opaque_wake_wait(shard);
    }
    if (shard->opaque_helper_failed) {
        errno = EIO;
        return -1;
    }
    return 0;
}

/**
 * @brief Expire all timers due on a shard and wake their tasks.
 *
 * Due timers are popped under the shard lock, then processed outside the lock.
 * Plain sleep wakeups are batched back under the lock so they can use the normal
 * hot/cold runnable placement policy, while non-sleep wait kinds dispatch
 * through ::llam_timeout_task_wait.
 *
 * @param shard Shard whose timer heap should be checked.
 */
void llam_fire_expired_timers(llam_shard_t *shard) {
    llam_timer_node_t *expired_head = NULL;
    llam_timer_node_t *expired_tail = NULL;
    llam_timer_node_t *sleep_head = NULL;
    llam_timer_node_t *sleep_tail = NULL;
    uint64_t now;
    unsigned expired_count = 0U;

    if (atomic_load_explicit(&shard->timer_count, memory_order_acquire) == 0U) {
        return;
    }

    pthread_mutex_lock(&shard->lock);
    if (shard->timers == NULL) {
        pthread_mutex_unlock(&shard->lock);
        return;
    }

    now = llam_now_ns();
    while (shard->timers != NULL && shard->timers->deadline_ns <= now) {
        llam_timer_node_t *timer = shard->timers;

        /*
         * The heap lock still proves the task and timer are live here. Acquire
         * the exact task pin before detaching either pointer into the callback
         * batch that will run after unlock.
         */
        if (timer->task != NULL &&
            !llam_task_scan_ref_try_acquire(shard->runtime, timer->task)) {
            /* Saturated/corrupt lifetime state is already fatal; leave owned. */
            break;
        }
        timer->holds_task_ref = timer->task != NULL;
        timer = llam_timer_heap_pop_min_locked(shard);
        if (timer == NULL) {
            break;
        }
        timer->next = NULL;
        if (timer->task != NULL && timer->task->active_timer == timer) {
            timer->task->active_timer = NULL;
            timer->task->deadline_ns = 0U;
        }
        expired_count += 1U;
        if (expired_tail != NULL) {
            expired_tail->next = timer;
        } else {
            expired_head = timer;
        }
        expired_tail = timer;
    }
    if (expired_count > 0U) {
        /*
         * A timer removed from the heap still owns its task until the timeout
         * resolver detaches the wait structure or publishes the sleep wake.
         * Dynamic shard offlining must see that in-between ownership and avoid
         * treating an empty heap as quiescent.
         */
        atomic_fetch_add_explicit(&shard->timer_callbacks_active, expired_count, memory_order_release);
    }
    pthread_mutex_unlock(&shard->lock);

    while (expired_head != NULL) {
        llam_timer_node_t *timer = expired_head;
        llam_timer_node_t *next = timer->next;

        expired_head = next;
        timer->next = NULL;
        if (llam_timer_wait_generation_matches(timer) &&
            (llam_wait_reason_t)atomic_load_explicit(&timer->task->wait_reason, memory_order_acquire) ==
                LLAM_WAIT_SLEEP &&
            timer->task->parked_shard == shard->id) {
            if (sleep_tail != NULL) {
                sleep_tail->next = timer;
            } else {
                sleep_head = timer;
            }
            sleep_tail = timer;
        } else {
            llam_timeout_task_wait(timer);
            llam_timer_release_ownership(shard, timer);
        }
    }

    if (sleep_head != NULL) {
        llam_runtime_t *rt = shard->runtime;
        bool pressure = llam_runtime_pressure_signal(rt);
        llam_timer_node_t *timer;

        pthread_mutex_lock(&shard->lock);
        timer = sleep_head;
        while (timer != NULL) {
            llam_task_t *task = timer->task;
            llam_wait_node_t *node = atomic_load_explicit(&task->active_wait_node, memory_order_acquire);
            bool hot;

            if (!llam_timer_wait_generation_matches(timer) ||
                (llam_wait_reason_t)atomic_load_explicit(&task->wait_reason,
                                                         memory_order_acquire) !=
                    LLAM_WAIT_SLEEP ||
                task->parked_shard != shard->id) {
                timer = timer->next;
                continue;
            }
            shard->metrics.timeout_wakes += 1U;
            atomic_store_explicit(&task->wake_error_code, 0, memory_order_release);
            if (node != NULL) {
                node->error_code = 0;
            }
            if (node != NULL && !llam_wait_node_prepare_wake(node)) {
                timer = timer->next;
                continue;
            }
            llam_task_clear_wait_tracking_or_abort(task);
            hot = llam_should_enqueue_hot_locked(shard, task, true, pressure);
            llam_mark_runnable_locked(shard, task, hot, LLAM_TRACE_WAKE, LLAM_WAIT_TIMEOUT, true);
            timer = timer->next;
        }
        pthread_mutex_unlock(&shard->lock);

        timer = sleep_head;
        while (timer != NULL) {
            llam_timer_node_t *next = timer->next;

            llam_timer_release_ownership(shard, timer);
            timer = next;
        }
    }
    if (expired_count > 0U) {
        atomic_fetch_sub_explicit(&shard->timer_callbacks_active, expired_count, memory_order_acq_rel);
    }
}
