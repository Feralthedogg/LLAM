/**
 * @file src/core/runtime_cond.c
 * @brief Runtime-aware condition variable implementation.
 *
 * @details
 * Condition waits atomically enqueue the current task on the condition wait
 * queue, release the associated runtime mutex, and park the task. On wakeup the
 * implementation reacquires the mutex before returning to match traditional
 * condition-variable semantics.
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
 * @brief Reacquire a condition wait mutex after a signal/broadcast wake.
 *
 * @details
 * Runtime mutex unlock transfers ownership directly to the next waiter before
 * waking it.  A condition waiter that was woken and then queued for the
 * associated mutex may therefore already own @p mutex by the time it resumes.
 * That is the normal condvar reacquire success path, not recursive locking.
 */
static int llam_cond_reacquire_mutex(llam_mutex_t *mutex, llam_task_t *task) {
    if (atomic_load(&mutex->owner) == (uintptr_t)task) {
        return 0;
    }
    return llam_mutex_lock_resolved_impl(mutex, false, 0U, false);
}

/**
 * @brief Mark that a condition waiter was popped but has not returned yet.
 *
 * Signal/broadcast removes a waiter from the cond queue before the waiter runs
 * again.  The wait-node payload flag lets only those completed cond waits drop
 * the cond's in-flight reference; timeout/cancel paths that remove their own
 * node never increment this counter.
 */
static void llam_cond_waiter_popped(llam_cond_t *cond, llam_wait_node_t *node) {
    if (node != NULL) {
        node->scalar_value = 1;
    }
    if (cond != NULL) {
        (void)llam_sync_note_inflight_waiter(cond->owner_runtime, &cond->inflight_waiters, 1U);
    }
}

/**
 * @brief Release a condition waiter's in-flight destroy reference.
 */
static void llam_cond_waiter_consumed(llam_cond_t *cond, llam_wait_node_t *node) {
    if (cond != NULL && node != NULL && node->scalar_value != 0) {
        (void)llam_sync_complete_inflight_waiter(cond->owner_runtime, &cond->inflight_waiters, 1U);
    }
}

/**
 * @brief Wait on a condition variable while releasing and reacquiring a mutex.
 *
 * The current task must own @p mutex. The function enqueues the task on the
 * condition wait list, releases the mutex to the next waiter if one exists,
 * parks the task, then reacquires @p mutex before returning.
 *
 * @param cond        Condition variable to wait on.
 * @param mutex       Associated runtime mutex currently owned by the task.
 * @param has_deadline Whether @p deadline_ns is active.
 * @param deadline_ns Absolute monotonic deadline in nanoseconds.
 *
 * @return 0 on signal/broadcast success, or -1 with @c errno set.
 */
int llam_cond_wait_impl(llam_cond_t *cond, llam_mutex_t *mutex, bool has_deadline, uint64_t deadline_ns) {
    llam_shard_t *shard;
    llam_task_t *task;
    llam_wait_node_t *node;
    llam_wait_node_t *mutex_waiter;
    uintptr_t owner;
    int rc;

    llam_task_safepoint();

    cond = llam_cond_resolve_public_handle(cond);
    mutex = llam_mutex_resolve_public_handle(mutex);
    if (cond == NULL || mutex == NULL) {
        llam_cond_end_public_op(cond);
        llam_mutex_end_public_op(mutex);
        return -1;
    }
    if (llam_require_task_context() != 0) {
        llam_cond_end_public_op(cond);
        llam_mutex_end_public_op(mutex);
        return -1;
    }
    if (llam_runtime_check_object_owner(cond->owner_runtime) != 0 ||
        llam_runtime_check_object_owner(mutex->owner_runtime) != 0) {
        llam_cond_end_public_op(cond);
        llam_mutex_end_public_op(mutex);
        return -1;
    }
    task = g_llam_tls_task;
    shard = g_llam_tls_shard;

    if (atomic_load(&mutex->owner) != (uintptr_t)task) {
        llam_cond_end_public_op(cond);
        llam_mutex_end_public_op(mutex);
        errno = EPERM;
        return -1;
    }
    if (has_deadline && llam_deadline_passed(deadline_ns)) {
        llam_cond_end_public_op(cond);
        llam_mutex_end_public_op(mutex);
        errno = ETIMEDOUT;
        return -1;
    }

    node = llam_sync_wait_node_acquire(shard);
    if (node == NULL) {
        llam_cond_end_public_op(cond);
        llam_mutex_end_public_op(mutex);
        errno = ENOMEM;
        return -1;
    }

    node->task = task;
    node->owner_shard = shard->id;

    pthread_mutex_lock(&cond->lock);
    llam_wait_queue_push_tail(&cond->waiters, node);
    owner = atomic_load(&mutex->owner);
    if (owner != (uintptr_t)task) {
        /*
         * This is a defensive recheck after the node is already visible on the
         * cond queue.  If ownership changed unexpectedly, unlink before
         * releasing the node; otherwise signal/broadcast/destroy would later see
         * a stale waiter that has already returned to the shard cache.
         */
        (void)llam_wait_queue_remove(&cond->waiters, node);
        pthread_mutex_unlock(&cond->lock);
        llam_sync_wait_node_release(shard, node);
        llam_cond_end_public_op(cond);
        llam_mutex_end_public_op(mutex);
        errno = EPERM;
        return -1;
    }

    pthread_mutex_lock(&mutex->lock);
    mutex_waiter = llam_wait_queue_pop_head(&mutex->waiters);
    if (mutex_waiter != NULL) {
        atomic_store(&mutex->owner, (uintptr_t)mutex_waiter->task);
    } else {
        atomic_store(&mutex->owner, (uintptr_t)0);
    }
    pthread_mutex_unlock(&mutex->lock);
    atomic_store_explicit(&task->task_class,
                          atomic_load_explicit(&task->base_task_class, memory_order_acquire),
                          memory_order_release);
    if (mutex_waiter != NULL) {
        mutex_waiter->error_code = 0;
        llam_wake_wait_node(mutex_waiter, true, LLAM_WAIT_MUTEX);
    }
    pthread_mutex_unlock(&cond->lock);
    llam_task_ensure_listed(task);
    llam_task_set_wait_node_tracking(task, node, &cond->waiters, &cond->lock, shard->id);
    task->state = LLAM_TASK_STATE_PARKED;
    task->wait_reason = LLAM_WAIT_COND;
    if (has_deadline && llam_arm_task_wait_deadline(task, shard, deadline_ns) != 0) {
        bool removed;

        if (llam_wait_node_completed(node)) {
            goto wait_ready;
        }
        pthread_mutex_lock(&cond->lock);
        removed = llam_wait_queue_remove(&cond->waiters, node);
        pthread_mutex_unlock(&cond->lock);
        if (!removed) {
            /* signal/broadcast already popped the node; reacquire below. */
            goto wait_ready;
        }
        task->state = LLAM_TASK_STATE_RUNNING;
        task->wait_reason = LLAM_WAIT_NONE;
        llam_task_clear_wait_tracking(task);
        llam_sync_wait_node_release(shard, node);
        (void)llam_cond_reacquire_mutex(mutex, task);
        llam_cond_end_public_op(cond);
        llam_mutex_end_public_op(mutex);
        return -1;
    }
    if (task->cancel_token != NULL && llam_cancel_token_register_task(task) != 0) {
        bool removed;

        if (llam_wait_node_completed(node)) {
            goto wait_ready;
        }
        llam_disarm_task_wait_deadline(task);
        pthread_mutex_lock(&cond->lock);
        removed = llam_wait_queue_remove(&cond->waiters, node);
        pthread_mutex_unlock(&cond->lock);
        if (!removed) {
            /* signal/broadcast already popped the node; reacquire below. */
            goto wait_ready;
        }
        task->state = LLAM_TASK_STATE_RUNNING;
        task->wait_reason = LLAM_WAIT_NONE;
        llam_task_clear_wait_tracking(task);
        llam_sync_wait_node_release(shard, node);
        (void)llam_cond_reacquire_mutex(mutex, task);
        llam_cond_end_public_op(cond);
        llam_mutex_end_public_op(mutex);
        return -1;
    }

wait_ready:
    if (llam_wait_node_should_park(node)) {
        llam_park_current_task(LLAM_WAIT_COND, LLAM_TRACE_STATE);
    }
    if (has_deadline) {
        // Early signal/broadcast can race ahead of timer arming.  The waiter
        // disarms any leftover deadline before reacquiring the user mutex.
        llam_disarm_task_wait_deadline(task);
    }
    llam_cancel_token_unregister_task(task);
    llam_task_clear_wait_tracking(task);
    rc = node->error_code;
    llam_cond_waiter_consumed(cond, node);
    llam_sync_wait_node_release(shard, node);
    if (llam_cond_reacquire_mutex(mutex, task) != 0) {
        llam_cond_end_public_op(cond);
        llam_mutex_end_public_op(mutex);
        return -1;
    }
    if (rc != 0) {
        llam_cond_end_public_op(cond);
        llam_mutex_end_public_op(mutex);
        errno = rc;
        return -1;
    }
    llam_cond_end_public_op(cond);
    llam_mutex_end_public_op(mutex);
    return 0;
}

/**
 * @brief Wait on a condition variable without a timeout.
 *
 * @param cond  Condition variable to wait on.
 * @param mutex Associated mutex owned by the current task.
 *
 * @return 0 on success, or -1 with @c errno set.
 */
int llam_cond_wait(llam_cond_t *cond, llam_mutex_t *mutex) {
    return llam_cond_wait_impl(cond, mutex, false, 0U);
}

/**
 * @brief Wait on a condition variable until an absolute deadline.
 *
 * @param cond        Condition variable to wait on.
 * @param mutex       Associated mutex owned by the current task.
 * @param deadline_ns Absolute monotonic deadline in nanoseconds.
 *
 * @return 0 on success, or -1 with @c errno set to @c ETIMEDOUT or another
 *         propagated wait error.
 */
int llam_cond_wait_until(llam_cond_t *cond, llam_mutex_t *mutex, uint64_t deadline_ns) {
    return llam_cond_wait_impl(cond, mutex, true, deadline_ns);
}

/**
 * @brief Wake one task waiting on a condition variable.
 *
 * @param cond Condition variable to signal.
 *
 * @return 0 on success, or -1 with @c errno set to @c EINVAL.
 */
int llam_cond_signal(llam_cond_t *cond) {
    llam_wait_node_t *node;
    llam_runtime_t *pinned_runtime = NULL;

    llam_task_safepoint();

    cond = llam_cond_resolve_public_handle(cond);
    if (cond == NULL) {
        return -1;
    }
    if (llam_runtime_begin_live_object_owner_op(cond->owner_runtime, &pinned_runtime, ENOTSUP) != 0) {
        llam_cond_end_public_op(cond);
        return -1;
    }

    pthread_mutex_lock(&cond->lock);
    node = llam_wait_queue_pop_head(&cond->waiters);
    if (node != NULL) {
        llam_cond_waiter_popped(cond, node);
    }
    pthread_mutex_unlock(&cond->lock);

    if (node != NULL) {
        node->error_code = 0;
        llam_wake_wait_node(node, true, LLAM_WAIT_COND);
    }
    llam_runtime_end_public_op(pinned_runtime);
    llam_cond_end_public_op(cond);
    return 0;
}

/**
 * @brief Wake all tasks waiting on a condition variable.
 *
 * @param cond Condition variable to broadcast.
 *
 * @return 0 on success, or -1 with @c errno set to @c EINVAL.
 */
int llam_cond_broadcast(llam_cond_t *cond) {
    llam_wait_node_t *node;
    llam_runtime_t *pinned_runtime = NULL;

    cond = llam_cond_resolve_public_handle(cond);
    if (cond == NULL) {
        return -1;
    }
    if (llam_runtime_begin_live_object_owner_op(cond->owner_runtime, &pinned_runtime, ENOTSUP) != 0) {
        llam_cond_end_public_op(cond);
        return -1;
    }

    pthread_mutex_lock(&cond->lock);
    while ((node = llam_wait_queue_pop_head(&cond->waiters)) != NULL) {
        llam_cond_waiter_popped(cond, node);
        node->error_code = 0;
        llam_wake_wait_node(node, true, LLAM_WAIT_COND);
    }
    pthread_mutex_unlock(&cond->lock);
    llam_runtime_end_public_op(pinned_runtime);
    llam_cond_end_public_op(cond);
    return 0;
}
