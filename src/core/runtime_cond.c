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
nm_cond_t *nm_cond_create(void) {
    nm_cond_t *cond = calloc(1, sizeof(*cond));

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
 * @param cond Condition variable to destroy; may be @c NULL.
 */
void nm_cond_destroy(nm_cond_t *cond) {
    if (cond == NULL) {
        return;
    }

    pthread_mutex_destroy(&cond->lock);
    free(cond);
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
int nm_cond_wait_impl(nm_cond_t *cond, nm_mutex_t *mutex, bool has_deadline, uint64_t deadline_ns) {
    nm_wait_node_t *node;
    nm_wait_node_t *mutex_waiter;
    uintptr_t owner;
    int rc;

    nm_task_safepoint();

    if (cond == NULL || mutex == NULL || nm_require_task_context() != 0) {
        return -1;
    }

    if (atomic_load(&mutex->owner) != (uintptr_t)g_nm_tls_task) {
        errno = EPERM;
        return -1;
    }
    if (has_deadline && nm_deadline_passed(deadline_ns)) {
        errno = ETIMEDOUT;
        return -1;
    }

    node = nm_sync_wait_node_acquire(g_nm_tls_shard);
    if (node == NULL) {
        errno = ENOMEM;
        return -1;
    }

    node->task = g_nm_tls_task;
    node->owner_shard = g_nm_tls_shard->id;

    pthread_mutex_lock(&cond->lock);
    nm_wait_queue_push_tail(&cond->waiters, node);
    owner = atomic_load(&mutex->owner);
    if (owner != (uintptr_t)g_nm_tls_task) {
        pthread_mutex_unlock(&cond->lock);
        nm_sync_wait_node_release(g_nm_tls_shard, node);
        errno = EPERM;
        return -1;
    }

    pthread_mutex_lock(&mutex->lock);
    mutex_waiter = nm_wait_queue_pop_head(&mutex->waiters);
    if (mutex_waiter != NULL) {
        atomic_store(&mutex->owner, (uintptr_t)mutex_waiter->task);
    } else {
        atomic_store(&mutex->owner, (uintptr_t)0);
    }
    pthread_mutex_unlock(&mutex->lock);
    if (mutex_waiter != NULL) {
        mutex_waiter->error_code = 0;
        nm_wake_wait_node(mutex_waiter, true, NM_WAIT_MUTEX);
    }
    pthread_mutex_unlock(&cond->lock);
    nm_task_set_wait_node_tracking(g_nm_tls_task, node, &cond->waiters, &cond->lock, g_nm_tls_shard->id);
    g_nm_tls_task->state = NM_TASK_STATE_PARKED;
    g_nm_tls_task->wait_reason = NM_WAIT_COND;
    if (has_deadline && nm_arm_task_wait_deadline(g_nm_tls_task, g_nm_tls_shard, deadline_ns) != 0) {
        pthread_mutex_lock(&cond->lock);
        (void)nm_wait_queue_remove(&cond->waiters, node);
        pthread_mutex_unlock(&cond->lock);
        g_nm_tls_task->state = NM_TASK_STATE_RUNNING;
        g_nm_tls_task->wait_reason = NM_WAIT_NONE;
        nm_task_clear_wait_tracking(g_nm_tls_task);
        nm_sync_wait_node_release(g_nm_tls_shard, node);
        (void)nm_mutex_lock_impl(mutex, false, 0U, false);
        return -1;
    }
    if (g_nm_tls_task->cancel_token != NULL && nm_cancel_token_register_task(g_nm_tls_task) != 0) {
        nm_disarm_task_wait_deadline(g_nm_tls_task);
        pthread_mutex_lock(&cond->lock);
        (void)nm_wait_queue_remove(&cond->waiters, node);
        pthread_mutex_unlock(&cond->lock);
        g_nm_tls_task->state = NM_TASK_STATE_RUNNING;
        g_nm_tls_task->wait_reason = NM_WAIT_NONE;
        nm_task_clear_wait_tracking(g_nm_tls_task);
        nm_sync_wait_node_release(g_nm_tls_shard, node);
        (void)nm_mutex_lock_impl(mutex, false, 0U, false);
        return -1;
    }

    nm_park_current_task(NM_WAIT_COND, NM_TRACE_STATE);
    if (g_nm_tls_task->cancel_registered) {
        nm_cancel_token_unregister_task(g_nm_tls_task);
    }
    nm_task_clear_wait_tracking(g_nm_tls_task);
    rc = node->error_code;
    nm_sync_wait_node_release(g_nm_tls_shard, node);
    if (nm_mutex_lock_impl(mutex, false, 0U, false) != 0) {
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
int nm_cond_wait(nm_cond_t *cond, nm_mutex_t *mutex) {
    return nm_cond_wait_impl(cond, mutex, false, 0U);
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
int nm_cond_wait_until(nm_cond_t *cond, nm_mutex_t *mutex, uint64_t deadline_ns) {
    return nm_cond_wait_impl(cond, mutex, true, deadline_ns);
}

/**
 * @brief Wake one task waiting on a condition variable.
 *
 * @param cond Condition variable to signal.
 *
 * @return 0 on success, or -1 with @c errno set to @c EINVAL.
 */
int nm_cond_signal(nm_cond_t *cond) {
    nm_wait_node_t *node;

    nm_task_safepoint();

    if (cond == NULL) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&cond->lock);
    node = nm_wait_queue_pop_head(&cond->waiters);
    pthread_mutex_unlock(&cond->lock);

    if (node != NULL) {
        node->error_code = 0;
        nm_wake_wait_node(node, true, NM_WAIT_COND);
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
int nm_cond_broadcast(nm_cond_t *cond) {
    if (cond == NULL) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&cond->lock);
    nm_wake_wait_queue_all(&cond->waiters, 0, NM_WAIT_COND);
    pthread_mutex_unlock(&cond->lock);
    return 0;
}
