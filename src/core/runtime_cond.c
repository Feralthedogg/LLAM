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
 * @brief Allocate a runtime-aware condition variable.
 *
 * @return New condition variable on success, or @c NULL with @c errno set.
 */
llam_cond_t *llam_cond_create(void) {
    llam_cond_t *cond = calloc(1, sizeof(*cond));

    if (cond == NULL) {
        return NULL;
    }

    if (pthread_mutex_init(&cond->lock, NULL) != 0) {
        free(cond);
        errno = ENOMEM;
        return NULL;
    }

    return cond;
}

/**
 * @brief Destroy a condition variable.
 *
 * The caller must ensure no tasks remain parked on the condition wait queue.
 *
 * @param cond Condition variable to destroy.
 *
 * @return 0 on success, or -1 with @c errno set to @c EINVAL or @c EBUSY.
 */
int llam_cond_destroy(llam_cond_t *cond) {
    if (cond == NULL) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&cond->lock);
    if (cond->waiters.head != NULL || cond->waiters.depth != 0U) {
        pthread_mutex_unlock(&cond->lock);
        errno = EBUSY;
        return -1;
    }
    pthread_mutex_unlock(&cond->lock);
    pthread_mutex_destroy(&cond->lock);
    free(cond);
    return 0;
}

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
    return llam_mutex_lock_impl(mutex, false, 0U, false);
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

    if (cond == NULL || mutex == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (llam_require_task_context() != 0) {
        return -1;
    }
    task = g_llam_tls_task;
    shard = g_llam_tls_shard;

    if (atomic_load(&mutex->owner) != (uintptr_t)task) {
        errno = EPERM;
        return -1;
    }
    if (has_deadline && llam_deadline_passed(deadline_ns)) {
        errno = ETIMEDOUT;
        return -1;
    }

    node = llam_sync_wait_node_acquire(shard);
    if (node == NULL) {
        errno = ENOMEM;
        return -1;
    }

    node->task = task;
    node->owner_shard = shard->id;

    pthread_mutex_lock(&cond->lock);
    llam_wait_queue_push_tail(&cond->waiters, node);
    owner = atomic_load(&mutex->owner);
    if (owner != (uintptr_t)task) {
        pthread_mutex_unlock(&cond->lock);
        llam_sync_wait_node_release(shard, node);
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
        pthread_mutex_lock(&cond->lock);
        (void)llam_wait_queue_remove(&cond->waiters, node);
        pthread_mutex_unlock(&cond->lock);
        task->state = LLAM_TASK_STATE_RUNNING;
        task->wait_reason = LLAM_WAIT_NONE;
        llam_task_clear_wait_tracking(task);
        llam_sync_wait_node_release(shard, node);
        (void)llam_cond_reacquire_mutex(mutex, task);
        return -1;
    }
    if (task->cancel_token != NULL && llam_cancel_token_register_task(task) != 0) {
        llam_disarm_task_wait_deadline(task);
        pthread_mutex_lock(&cond->lock);
        (void)llam_wait_queue_remove(&cond->waiters, node);
        pthread_mutex_unlock(&cond->lock);
        task->state = LLAM_TASK_STATE_RUNNING;
        task->wait_reason = LLAM_WAIT_NONE;
        llam_task_clear_wait_tracking(task);
        llam_sync_wait_node_release(shard, node);
        (void)llam_cond_reacquire_mutex(mutex, task);
        return -1;
    }

    if (llam_wait_node_should_park(node)) {
        llam_park_current_task(LLAM_WAIT_COND, LLAM_TRACE_STATE);
    }
    if (has_deadline) {
        // Early signal/broadcast can race ahead of timer arming.  The waiter
        // disarms any leftover deadline before reacquiring the user mutex.
        llam_disarm_task_wait_deadline(task);
    }
    if (task->cancel_registered) {
        llam_cancel_token_unregister_task(task);
    }
    llam_task_clear_wait_tracking(task);
    rc = node->error_code;
    llam_sync_wait_node_release(shard, node);
    if (llam_cond_reacquire_mutex(mutex, task) != 0) {
        return -1;
    }
    if (rc != 0) {
        errno = rc;
        return -1;
    }
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

    llam_task_safepoint();

    if (cond == NULL) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&cond->lock);
    node = llam_wait_queue_pop_head(&cond->waiters);
    pthread_mutex_unlock(&cond->lock);

    if (node != NULL) {
        node->error_code = 0;
        llam_wake_wait_node(node, true, LLAM_WAIT_COND);
    }
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
    if (cond == NULL) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&cond->lock);
    llam_wake_wait_queue_all(&cond->waiters, 0, LLAM_WAIT_COND);
    pthread_mutex_unlock(&cond->lock);
    return 0;
}
