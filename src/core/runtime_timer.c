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

/** Timer heap fanout. A 4-ary heap reduces sift depth and improves cache locality. */
#define LLAM_TIMER_HEAP_ARITY 4U

/** @brief Return the parent index for a non-root heap entry. */
static size_t llam_timer_heap_parent(size_t index) {
    return (index - 1U) / LLAM_TIMER_HEAP_ARITY;
}

/**
 * @brief Compare two timer heap nodes.
 *
 * Deadlines are primary keys. Task ids are used as a stable tie-breaker for
 * task-backed timers, and pointer order is the final fallback for sentinel or
 * non-task nodes.
 *
 * @param lhs Left timer node.
 * @param rhs Right timer node.
 *
 * @return @c true when @p lhs should appear before @p rhs in the min-heap.
 */
static bool llam_timer_heap_less(const llam_timer_node_t *lhs, const llam_timer_node_t *rhs) {
    if (lhs->deadline_ns != rhs->deadline_ns) {
        return lhs->deadline_ns < rhs->deadline_ns;
    }
    if (lhs->task == NULL || rhs->task == NULL) {
        return lhs < rhs;
    }
    return lhs->task->id < rhs->task->id;
}

/**
 * @brief Refresh the legacy root pointer from the heap array.
 *
 * @param shard Shard whose timer root is updated.
 */
static void llam_timer_heap_refresh_root(llam_shard_t *shard) {
    shard->timers = shard->timer_heap_len > 0U ? shard->timer_heap[0] : NULL;
}

/**
 * @brief Swap two heap positions and update their back-pointers.
 *
 * @param shard Shard owning the heap.
 * @param lhs   First heap index.
 * @param rhs   Second heap index.
 */
static void llam_timer_heap_swap(llam_shard_t *shard, size_t lhs, size_t rhs) {
    llam_timer_node_t *tmp = shard->timer_heap[lhs];

    shard->timer_heap[lhs] = shard->timer_heap[rhs];
    shard->timer_heap[rhs] = tmp;
    shard->timer_heap[lhs]->heap_index = lhs;
    shard->timer_heap[rhs]->heap_index = rhs;
}

/**
 * @brief Ensure the shard timer heap can hold at least @p needed nodes.
 *
 * @param shard  Shard owning the heap.
 * @param needed Required capacity.
 *
 * @return @c true on success, or @c false with @c errno set to @c ENOMEM.
 */
static bool llam_timer_heap_reserve(llam_shard_t *shard, size_t needed) {
    llam_timer_node_t **items;
    size_t new_cap;

    if (needed <= shard->timer_heap_cap) {
        return true;
    }
    new_cap = shard->timer_heap_cap != 0U ? shard->timer_heap_cap * 2U : 64U;
    while (new_cap < needed) {
        if (new_cap > (SIZE_MAX / 2U)) {
            errno = ENOMEM;
            return false;
        }
        new_cap *= 2U;
    }
    items = realloc(shard->timer_heap, new_cap * sizeof(*items));
    if (items == NULL) {
        errno = ENOMEM;
        return false;
    }
    shard->timer_heap = items;
    shard->timer_heap_cap = new_cap;
    return true;
}

/**
 * @brief Restore heap ordering by moving a node toward the root.
 *
 * @param shard Shard owning the heap.
 * @param index Initial node index.
 */
static void llam_timer_heap_sift_up(llam_shard_t *shard, size_t index) {
    while (index > 0U) {
        size_t parent = llam_timer_heap_parent(index);

        if (!llam_timer_heap_less(shard->timer_heap[index], shard->timer_heap[parent])) {
            break;
        }
        llam_timer_heap_swap(shard, index, parent);
        index = parent;
    }
}

/**
 * @brief Restore heap ordering by moving a node toward the leaves.
 *
 * @param shard Shard owning the heap.
 * @param index Initial node index.
 */
static void llam_timer_heap_sift_down(llam_shard_t *shard, size_t index) {
    for (;;) {
        size_t first_child = index * LLAM_TIMER_HEAP_ARITY + 1U;
        size_t best = index;
        size_t child;

        for (child = first_child;
             child < shard->timer_heap_len && child < first_child + LLAM_TIMER_HEAP_ARITY;
             ++child) {
            if (llam_timer_heap_less(shard->timer_heap[child], shard->timer_heap[best])) {
                best = child;
            }
        }
        if (best == index) {
            break;
        }
        llam_timer_heap_swap(shard, index, best);
        index = best;
    }
}

/**
 * @brief Push a timer node into the shard heap.
 *
 * @param shard Shard owning the heap. The shard lock must already be held.
 * @param node  Timer node to insert.
 *
 * @return @c true on success, or @c false when heap growth fails.
 */
static bool llam_timer_heap_push_locked(llam_shard_t *shard, llam_timer_node_t *node) {
    size_t index;

    if (!llam_timer_heap_reserve(shard, shard->timer_heap_len + 1U)) {
        return false;
    }
    index = shard->timer_heap_len++;
    shard->timer_heap[index] = node;
    node->heap_index = index;
    llam_timer_heap_sift_up(shard, index);
    llam_timer_heap_refresh_root(shard);
    atomic_fetch_add_explicit(&shard->timer_count, 1U, memory_order_release);
    return true;
}

/**
 * @brief Remove a timer node at a specific heap index.
 *
 * The last heap node is moved into the removed slot and sifted in the direction
 * required to restore min-heap ordering.
 *
 * @param shard Shard owning the heap. The shard lock must already be held.
 * @param index Heap index to remove.
 *
 * @return Removed timer node, or @c NULL if @p index is out of range.
 */
static llam_timer_node_t *llam_timer_heap_remove_at_locked(llam_shard_t *shard, size_t index) {
    llam_timer_node_t *node;
    llam_timer_node_t *last;

    if (index >= shard->timer_heap_len) {
        return NULL;
    }

    node = shard->timer_heap[index];
    shard->timer_heap_len -= 1U;
    if (index != shard->timer_heap_len) {
        last = shard->timer_heap[shard->timer_heap_len];
        shard->timer_heap[index] = last;
        last->heap_index = index;
        if (index > 0U &&
            llam_timer_heap_less(last, shard->timer_heap[llam_timer_heap_parent(index)])) {
            llam_timer_heap_sift_up(shard, index);
        } else {
            llam_timer_heap_sift_down(shard, index);
        }
    }
    shard->timer_heap[shard->timer_heap_len] = NULL;
    node->heap_index = 0U;
    llam_timer_heap_refresh_root(shard);
    atomic_fetch_sub_explicit(&shard->timer_count, 1U, memory_order_release);
    return node;
}

/**
 * @brief Pop the earliest timer node from the heap.
 *
 * @param shard Shard owning the heap. The shard lock must already be held.
 *
 * @return Earliest timer node, or @c NULL if the heap is empty.
 */
static llam_timer_node_t *llam_timer_heap_pop_min_locked(llam_shard_t *shard) {
    return llam_timer_heap_remove_at_locked(shard, 0U);
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
    bool removed = false;

    if (task == NULL || task->state != LLAM_TASK_STATE_PARKED) {
        return;
    }

    llam_wait_reason_t wait_reason = (llam_wait_reason_t)atomic_load_explicit(&task->wait_reason, memory_order_acquire);
    switch (wait_reason) {
    case LLAM_WAIT_SLEEP:
        if (task->parked_shard < g_llam_runtime.active_shards) {
            llam_shard_t *shard = &g_llam_runtime.shards[task->parked_shard];
            llam_wait_node_t *node = task->active_wait_node;

            pthread_mutex_lock(&shard->lock);
            removed = llam_timer_remove_locked(shard, task);
            if (removed) {
                shard->metrics.cancel_wakes += 1U;
            }
            pthread_mutex_unlock(&shard->lock);
            if (removed) {
                task->wake_error_code = ECANCELED;
                if (node != NULL) {
                    node->error_code = ECANCELED;
                }
                /*
                 * The sleep may not have switched to the scheduler yet.  Only
                 * enqueue an already-parked task; otherwise the caller consumes
                 * the cancellation inline through its wait node.
                 */
                if (node == NULL || llam_wait_node_prepare_wake(node)) {
                    llam_reinject_task_on_shard(&g_llam_runtime,
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
        if (task->join_target != NULL) {
            pthread_mutex_lock(&task->join_target->lock);
            removed = llam_join_waiter_remove_locked(task->join_target, task);
            pthread_mutex_unlock(&task->join_target->lock);
            if (removed) {
                llam_shard_t *shard = &g_llam_runtime.shards[task->parked_shard % g_llam_runtime.active_shards];

                pthread_mutex_lock(&shard->lock);
                shard->metrics.cancel_wakes += 1U;
                pthread_mutex_unlock(&shard->lock);
                task->wake_error_code = ECANCELED;
                // Preserve join/deadline ownership until generic reinject disarms it.
                llam_reinject_task(&g_llam_runtime, task, true, LLAM_TRACE_WAKE, LLAM_WAIT_CANCEL);
            }
        }
        break;
    case LLAM_WAIT_MUTEX:
    case LLAM_WAIT_COND:
    case LLAM_WAIT_CHANNEL_SEND:
    case LLAM_WAIT_CHANNEL_RECV:
        if (task->active_select_state != NULL) {
            if (llam_channel_select_abort_task_wait(task, ECANCELED, LLAM_WAIT_CANCEL)) {
                llam_shard_t *shard = &g_llam_runtime.shards[task->parked_shard % g_llam_runtime.active_shards];

                pthread_mutex_lock(&shard->lock);
                shard->metrics.cancel_wakes += 1U;
                pthread_mutex_unlock(&shard->lock);
            }
            break;
        }
        if (task->active_wait_queue != NULL && task->active_wait_queue_lock != NULL && task->active_wait_node != NULL) {
            llam_wait_node_t *node = task->active_wait_node;

            pthread_mutex_lock(task->active_wait_queue_lock);
            removed = llam_wait_queue_remove(task->active_wait_queue, node);
            if (removed) {
                node->error_code = ECANCELED;
            }
            pthread_mutex_unlock(task->active_wait_queue_lock);
            if (removed) {
                llam_shard_t *shard = &g_llam_runtime.shards[task->parked_shard % g_llam_runtime.active_shards];

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
                if (task->parked_shard < g_llam_runtime.active_shards) {
                    llam_shard_t *shard = &g_llam_runtime.shards[task->parked_shard];

                    pthread_mutex_lock(&shard->lock);
                    shard->metrics.cancel_wakes += 1U;
                    pthread_mutex_unlock(&shard->lock);
                }
                task->wake_error_code = ECANCELED;
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
                    llam_reinject_task_on_shard(&g_llam_runtime,
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
    bool removed = false;

    if (task == NULL || task->state != LLAM_TASK_STATE_PARKED || g_llam_runtime.active_shards == 0U) {
        return;
    }

    llam_wait_reason_t wait_reason = (llam_wait_reason_t)atomic_load_explicit(&task->wait_reason, memory_order_acquire);
    switch (wait_reason) {
    case LLAM_WAIT_SLEEP: {
        llam_shard_t *shard = &g_llam_runtime.shards[task->parked_shard % g_llam_runtime.active_shards];
        llam_wait_node_t *node = task->active_wait_node;

        shard->metrics.timeout_wakes += 1U;
        task->wake_error_code = 0;
        if (node != NULL) {
            node->error_code = 0;
        }
        if (node == NULL || llam_wait_node_prepare_wake(node)) {
            llam_reinject_task_on_shard(&g_llam_runtime,
                                      task,
                                      task->parked_shard,
                                      true,
                                      LLAM_TRACE_WAKE,
                                      LLAM_WAIT_TIMEOUT);
        }
        break;
    }
    case LLAM_WAIT_JOIN:
        if (task->join_target != NULL) {
            llam_shard_t *shard = &g_llam_runtime.shards[task->parked_shard % g_llam_runtime.active_shards];

            pthread_mutex_lock(&task->join_target->lock);
            removed = llam_join_waiter_remove_locked(task->join_target, task);
            pthread_mutex_unlock(&task->join_target->lock);
            if (removed) {
                shard->metrics.timeout_wakes += 1U;
                task->wake_error_code = ETIMEDOUT;
                llam_reinject_task(&g_llam_runtime, task, true, LLAM_TRACE_WAKE, LLAM_WAIT_TIMEOUT);
            }
        }
        break;
    case LLAM_WAIT_MUTEX:
    case LLAM_WAIT_COND:
    case LLAM_WAIT_CHANNEL_SEND:
    case LLAM_WAIT_CHANNEL_RECV:
        if (task->active_select_state != NULL) {
            if (llam_channel_select_abort_task_wait(task, ETIMEDOUT, LLAM_WAIT_TIMEOUT)) {
                llam_shard_t *shard = &g_llam_runtime.shards[task->parked_shard % g_llam_runtime.active_shards];

                shard->metrics.timeout_wakes += 1U;
            }
            break;
        }
        if (task->active_wait_queue != NULL && task->active_wait_queue_lock != NULL && task->active_wait_node != NULL) {
            llam_wait_node_t *node = task->active_wait_node;
            llam_shard_t *shard = &g_llam_runtime.shards[task->parked_shard % g_llam_runtime.active_shards];

            pthread_mutex_lock(task->active_wait_queue_lock);
            removed = llam_wait_queue_remove(task->active_wait_queue, node);
            if (removed) {
                node->error_code = ETIMEDOUT;
            }
            pthread_mutex_unlock(task->active_wait_queue_lock);
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
            llam_wait_node_t *node = task->active_wait_node;
            bool hot;

            task->wait_next = NULL;
            shard->metrics.timeout_wakes += 1U;
            task->wake_error_code = 0;
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
