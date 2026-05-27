/**
 * @file src/core/runtime_timer.c
 * @brief Timer heap management and deadline expiration dispatch.
 *
 * @details
 * Each shard owns a min-heap of timer nodes keyed by absolute monotonic
 * deadlines. Sleeping tasks use their embedded timer node; other wait kinds use
 * the same timeout dispatch path after their deadline node fires.
 *
 * The shard lock protects heap structure and task timer ownership. Expired
 * nodes are detached under the lock and processed afterward so reinjection and
 * wait-queue removal do not hold the timer heap lock longer than necessary.
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

/** @brief Return the runtime that owns a parked task's timer/wait state. */
static llam_runtime_t *llam_timer_task_runtime(const llam_task_t *task) {
    return task != NULL ? task->owner_runtime : NULL;
}

/**
 * @brief Insert a task's active deadline into the shard timer heap.
 *
 * The task uses its embedded timer node. On allocation failure while growing the
 * heap, @c active_timer remains @c NULL so the caller can convert the failed
 * park attempt into an error.
 *
 * @param shard Shard owning the heap. The shard lock must already be held.
 * @param task  Task with @c deadline_ns already populated.
 */
void llam_timer_insert_locked(llam_shard_t *shard, llam_task_t *task) {
    llam_timer_node_t *node;

    if (task == NULL || task->active_timer != NULL) {
        return;
    }

    node = &task->embedded_timer_node;
    memset(node, 0, sizeof(*node));
    node->owner_shard = UINT_MAX;

    node->task = task;
    node->deadline_ns = task->deadline_ns;
    if (!llam_timer_heap_push_locked(shard, node)) {
        node->task = NULL;
        node->deadline_ns = 0U;
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
        node->next = NULL;
        node->task = NULL;
        task->active_timer = NULL;
        task->deadline_ns = 0U;
        llam_timer_node_free(shard, node);
        return true;
    }

    for (i = 0U; i < shard->timer_heap_len; ++i) {
        if (shard->timer_heap[i] == node) {
            (void)llam_timer_heap_remove_at_locked(shard, i);
            node->next = NULL;
            node->task = NULL;
            task->active_timer = NULL;
            task->deadline_ns = 0U;
            llam_timer_node_free(shard, node);
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
void llam_cancel_task_wait(llam_task_t *task) {
    llam_runtime_t *rt = llam_timer_task_runtime(task);
    bool removed = false;

    if (task == NULL || task->state != LLAM_TASK_STATE_PARKED ||
        rt == NULL || rt->active_shards == 0U) {
        return;
    }

    llam_wait_reason_t wait_reason = (llam_wait_reason_t)atomic_load_explicit(&task->wait_reason, memory_order_acquire);
    switch (wait_reason) {
    case LLAM_WAIT_SLEEP:
        if (task->parked_shard < rt->active_shards) {
            llam_shard_t *shard = &rt->shards[task->parked_shard];
            llam_wait_node_t *node = atomic_load_explicit(&task->active_wait_node, memory_order_acquire);

            pthread_mutex_lock(&shard->lock);
            removed = llam_timer_remove_locked(shard, task);
            if (removed) {
                shard->metrics.cancel_wakes += 1U;
            }
            pthread_mutex_unlock(&shard->lock);
            if (removed) {
                atomic_store_explicit(&task->wake_error_code, ECANCELED, memory_order_release);
                if (node != NULL) {
                    node->error_code = ECANCELED;
                }
                /*
                 * The sleep may not have switched to the scheduler yet.  Only
                 * enqueue an already-parked task; otherwise the caller consumes
                 * the cancellation inline through its wait node.
                 */
                if (node == NULL || llam_wait_node_prepare_wake(node)) {
                    llam_reinject_task_on_shard(rt,
                                              task,
                                              task->parked_shard,
                                              true,
                                              LLAM_TRACE_WAKE,
                                              LLAM_WAIT_CANCEL);
                }
            }
        }
        break;
    case LLAM_WAIT_JOIN:
        {
            llam_task_t *join_target = atomic_load_explicit(&task->join_target, memory_order_acquire);

            /*
             * Join completion owns target->lock and may clear task->join_target
             * while stop cancellation is scanning parked tasks.  Snapshot the
             * target once so lock/unlock operate on the same object.
             */
            if (join_target == NULL) {
                break;
            }
            pthread_mutex_lock(&join_target->lock);
            removed = llam_join_waiter_remove_locked(join_target, task);
            pthread_mutex_unlock(&join_target->lock);
            if (removed) {
                llam_shard_t *shard = &rt->shards[task->parked_shard % rt->active_shards];

                pthread_mutex_lock(&shard->lock);
                shard->metrics.cancel_wakes += 1U;
                pthread_mutex_unlock(&shard->lock);
                atomic_store_explicit(&task->wake_error_code, ECANCELED, memory_order_release);
                // Preserve join/deadline ownership until generic reinject disarms it.
                llam_reinject_task(rt, task, true, LLAM_TRACE_WAKE, LLAM_WAIT_CANCEL);
            }
        }
        break;
    case LLAM_WAIT_MUTEX:
    case LLAM_WAIT_COND:
    case LLAM_WAIT_CHANNEL_SEND:
    case LLAM_WAIT_CHANNEL_RECV:
        if (atomic_load_explicit(&task->active_select_state, memory_order_acquire) != NULL) {
            if (llam_channel_select_abort_task_wait(task, ECANCELED, LLAM_WAIT_CANCEL)) {
                llam_shard_t *shard = &rt->shards[task->parked_shard % rt->active_shards];

                pthread_mutex_lock(&shard->lock);
                shard->metrics.cancel_wakes += 1U;
                pthread_mutex_unlock(&shard->lock);
            }
            break;
        }
        {
            llam_wait_queue_t *queue = atomic_load_explicit(&task->active_wait_queue, memory_order_acquire);
            pthread_mutex_t *queue_lock = atomic_load_explicit(&task->active_wait_queue_lock, memory_order_acquire);
            llam_wait_node_t *node = atomic_load_explicit(&task->active_wait_node, memory_order_acquire);

            /*
             * The normal producer wake path may clear task wait ownership while
             * runtime-stop cancellation is scanning parked tasks.  Snapshot the
             * queue, lock, and node once; otherwise an unlock can reload a NULL
             * lock after a concurrent successful wake disarms the task fields.
             */
            if (queue == NULL || queue_lock == NULL || node == NULL) {
                break;
            }
            pthread_mutex_lock(queue_lock);
            removed = llam_wait_queue_remove(queue, node);
            if (removed) {
                node->error_code = ECANCELED;
            }
            pthread_mutex_unlock(queue_lock);
            if (removed) {
                llam_shard_t *shard = &rt->shards[task->parked_shard % rt->active_shards];

                pthread_mutex_lock(&shard->lock);
                shard->metrics.cancel_wakes += 1U;
                pthread_mutex_unlock(&shard->lock);
                // Preserve wait/deadline ownership until the wait-node wake commits.
                llam_wake_wait_node(node, true, LLAM_WAIT_CANCEL);
            }
        }
        break;
    case LLAM_WAIT_IO:
        (void)llam_abort_io_wait(task, LLAM_IO_ABORT_CANCEL);
        break;
    case LLAM_WAIT_BLOCKING:
        {
            llam_block_job_t *job = llam_task_active_block_job_load(task);
            unsigned expected = LLAM_BLOCK_JOB_QUEUED;

            if (job == NULL) {
                break;
            }
            if (atomic_compare_exchange_strong(&job->state, &expected, LLAM_BLOCK_JOB_ABORTED)) {
                if (task->parked_shard < rt->active_shards) {
                    llam_shard_t *shard = &rt->shards[task->parked_shard];

                    pthread_mutex_lock(&shard->lock);
                    shard->metrics.cancel_wakes += 1U;
                    pthread_mutex_unlock(&shard->lock);
                }
                atomic_store_explicit(&task->wake_error_code, ECANCELED, memory_order_release);
                if (job->wait_node != NULL) {
                    job->wait_node->error_code = ECANCELED;
                }
                /*
                 * A blocking helper can finish before the caller has actually
                 * switched to the scheduler.  Use the same wait-node arm/complete
                 * handshake as channels and mutexes so a queued-job cancellation
                 * can be consumed inline instead of reinjecting a still-running
                 * task.
                 */
                if (job->wait_node == NULL || llam_wait_node_prepare_wake(job->wait_node)) {
                    llam_reinject_task_on_shard(rt,
                                              task,
                                              task->parked_shard,
                                              true,
                                              LLAM_TRACE_WAKE,
                                              LLAM_WAIT_CANCEL);
                }
                break;
            }

            expected = LLAM_BLOCK_JOB_RUNNING;
            if (atomic_compare_exchange_strong(&job->state, &expected, LLAM_BLOCK_JOB_ABORTED)) {
                /*
                 * The worker already owns user code.  Do not wake the task
                 * here: the worker completion path will report ECANCELED after
                 * the callback returns, preserving callback argument lifetime.
                 */
                break;
            }
        }
        break;
    default:
        break;
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
                atomic_fetch_add_explicit(&task->scan_refs, 1U, memory_order_acq_rel);
                tasks[count++] = task;
            }
            pthread_mutex_unlock(&owner->lock);
        }

        for (i = 0U; i < count; ++i) {
            llam_cancel_task_wait(tasks[i]);
            atomic_fetch_sub_explicit(&tasks[i]->scan_refs, 1U, memory_order_acq_rel);
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
 * @param task Parked task whose deadline expired.
 */
void llam_timeout_task_wait(llam_task_t *task) {
    llam_runtime_t *rt = llam_timer_task_runtime(task);
    bool removed = false;

    if (task == NULL || task->state != LLAM_TASK_STATE_PARKED ||
        rt == NULL || rt->active_shards == 0U) {
        return;
    }

    llam_wait_reason_t wait_reason = (llam_wait_reason_t)atomic_load_explicit(&task->wait_reason, memory_order_acquire);
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
            removed = llam_join_waiter_remove_locked(join_target, task);
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
        if (atomic_load_explicit(&task->active_select_state, memory_order_acquire) != NULL) {
            if (llam_channel_select_abort_task_wait(task, ETIMEDOUT, LLAM_WAIT_TIMEOUT)) {
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
            pthread_mutex_lock(queue_lock);
            removed = llam_wait_queue_remove(queue, node);
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
        (void)llam_abort_io_wait(task, LLAM_IO_ABORT_TIMEOUT);
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
    llam_task_t *sleep_head = NULL;
    llam_task_t *sleep_tail = NULL;
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
        llam_timer_node_t *timer = llam_timer_heap_pop_min_locked(shard);

        if (timer == NULL) {
            break;
        }
        timer->next = NULL;
        if (timer->task != NULL) {
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

        expired_head = timer->next;
        timer->next = NULL;
        if (timer->task != NULL &&
            (llam_wait_reason_t)atomic_load_explicit(&timer->task->wait_reason, memory_order_acquire) ==
                LLAM_WAIT_SLEEP &&
            timer->task->parked_shard == shard->id) {
            timer->task->wait_next = NULL;
            if (sleep_tail != NULL) {
                sleep_tail->wait_next = timer->task;
            } else {
                sleep_head = timer->task;
            }
            sleep_tail = timer->task;
        } else {
            llam_timeout_task_wait(timer->task);
        }
        llam_timer_node_free(shard, timer);
    }

    if (sleep_head != NULL) {
        llam_runtime_t *rt = shard->runtime;
        bool pressure = llam_runtime_pressure_signal(rt);
        llam_task_t *task;

        pthread_mutex_lock(&shard->lock);
        task = sleep_head;
        while (task != NULL) {
            llam_task_t *next = task->wait_next;
            llam_wait_node_t *node = atomic_load_explicit(&task->active_wait_node, memory_order_acquire);
            bool hot;

            task->wait_next = NULL;
            shard->metrics.timeout_wakes += 1U;
            atomic_store_explicit(&task->wake_error_code, 0, memory_order_release);
            if (node != NULL) {
                node->error_code = 0;
            }
            if (node != NULL && !llam_wait_node_prepare_wake(node)) {
                task = next;
                continue;
            }
            llam_task_clear_wait_tracking(task);
            hot = llam_should_enqueue_hot_locked(shard, task, true, pressure);
            llam_mark_runnable_locked(shard, task, hot, LLAM_TRACE_WAKE, LLAM_WAIT_TIMEOUT, true);
            task = next;
        }
        pthread_mutex_unlock(&shard->lock);
    }
    if (expired_count > 0U) {
        atomic_fetch_sub_explicit(&shard->timer_callbacks_active, expired_count, memory_order_acq_rel);
    }
}
