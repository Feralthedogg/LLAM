/**
 * @file src/core/runtime_mutex.c
 * @brief Runtime-aware mutex implementation with timed wait support.
 *
 * @details
 * The mutex owner is stored atomically for a cheap uncontended fast path. When
 * contended, managed tasks park on a FIFO wait queue protected by the mutex's
 * internal pthread mutex. Unlock transfers ownership directly to the next waiter
 * before waking it, so the awakened task owns the mutex when it resumes.
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
 * @brief Allocate a runtime-aware mutex.
 *
 * @return New mutex on success, or @c NULL with @c errno set on failure.
 */
nm_mutex_t *nm_mutex_create(void) {
    nm_mutex_t *mutex = calloc(1, sizeof(*mutex));

    if (mutex == NULL) {
        return NULL;
    }

    atomic_init(&mutex->owner, (uintptr_t)0);
    if (pthread_mutex_init(&mutex->lock, NULL) != 0) {
        free(mutex);
        errno = ENOMEM;
        return NULL;
    }

    return mutex;
}

/**
 * @brief Destroy a runtime-aware mutex.
 *
 * The caller must ensure no task owns or waits on the mutex.
 *
 * @param mutex Mutex to destroy; may be @c NULL.
 */
void nm_mutex_destroy(nm_mutex_t *mutex) {
    if (mutex == NULL) {
        return;
    }

    pthread_mutex_destroy(&mutex->lock);
    free(mutex);
}

/**
 * @brief Attempt to acquire a mutex without parking.
 *
 * @param mutex Mutex to acquire.
 *
 * @return 0 on success, or -1 with @c errno set to @c EINVAL or @c EBUSY.
 */
int nm_mutex_trylock(nm_mutex_t *mutex) {
    uintptr_t expected = 0U;

    nm_task_safepoint();

    if (mutex == NULL || nm_require_task_context() != 0) {
        return -1;
    }

    if (atomic_compare_exchange_strong(&mutex->owner, &expected, (uintptr_t)g_nm_tls_task)) {
        return 0;
    }

    errno = EBUSY;
    return -1;
}

/**
 * @brief Acquire a mutex, optionally with a deadline and cancellation.
 *
 * The uncontended path is a single atomic compare/exchange. The contended path
 * enqueues a wait node, arms optional timeout/cancellation tracking, parks the
 * current task, and consumes the wait node's completion error after wakeup.
 *
 * @param mutex           Mutex to acquire.
 * @param has_deadline    Whether @p deadline_ns is active.
 * @param deadline_ns     Absolute monotonic deadline in nanoseconds.
 * @param register_cancel Whether to register the task's cancellation token.
 *
 * @return 0 on success, or -1 with @c errno set.
 */
int nm_mutex_lock_impl(nm_mutex_t *mutex, bool has_deadline, uint64_t deadline_ns, bool register_cancel) {
    nm_wait_node_t *node;
    uintptr_t expected = 0U;
    int rc = 0;

    nm_task_safepoint();

    if (mutex == NULL || nm_require_task_context() != 0) {
        return -1;
    }

    if (atomic_compare_exchange_strong(&mutex->owner, &expected, (uintptr_t)g_nm_tls_task)) {
        return 0;
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

    pthread_mutex_lock(&mutex->lock);
    expected = 0U;
    if (atomic_compare_exchange_strong(&mutex->owner, &expected, (uintptr_t)g_nm_tls_task)) {
        pthread_mutex_unlock(&mutex->lock);
        nm_sync_wait_node_release(g_nm_tls_shard, node);
        return 0;
    }
    nm_wait_queue_push_tail(&mutex->waiters, node);
    pthread_mutex_unlock(&mutex->lock);
    nm_task_set_wait_node_tracking(g_nm_tls_task, node, &mutex->waiters, &mutex->lock, g_nm_tls_shard->id);
    g_nm_tls_task->state = NM_TASK_STATE_PARKED;
    g_nm_tls_task->wait_reason = NM_WAIT_MUTEX;
    if (has_deadline && nm_arm_task_wait_deadline(g_nm_tls_task, g_nm_tls_shard, deadline_ns) != 0) {
        pthread_mutex_lock(&mutex->lock);
        (void)nm_wait_queue_remove(&mutex->waiters, node);
        pthread_mutex_unlock(&mutex->lock);
        g_nm_tls_task->state = NM_TASK_STATE_RUNNING;
        g_nm_tls_task->wait_reason = NM_WAIT_NONE;
        nm_task_clear_wait_tracking(g_nm_tls_task);
        nm_sync_wait_node_release(g_nm_tls_shard, node);
        return -1;
    }
    if (register_cancel && g_nm_tls_task->cancel_token != NULL && nm_cancel_token_register_task(g_nm_tls_task) != 0) {
        nm_disarm_task_wait_deadline(g_nm_tls_task);
        pthread_mutex_lock(&mutex->lock);
        (void)nm_wait_queue_remove(&mutex->waiters, node);
        pthread_mutex_unlock(&mutex->lock);
        g_nm_tls_task->state = NM_TASK_STATE_RUNNING;
        g_nm_tls_task->wait_reason = NM_WAIT_NONE;
        nm_task_clear_wait_tracking(g_nm_tls_task);
        nm_sync_wait_node_release(g_nm_tls_shard, node);
        return -1;
    }

    nm_park_current_task(NM_WAIT_MUTEX, NM_TRACE_STATE);
    if (register_cancel && g_nm_tls_task->cancel_registered) {
        nm_cancel_token_unregister_task(g_nm_tls_task);
    }
    nm_task_clear_wait_tracking(g_nm_tls_task);
    rc = node->error_code;
    nm_sync_wait_node_release(g_nm_tls_shard, node);
    if (rc != 0) {
        errno = rc;
        return -1;
    }
    return 0;
}

/**
 * @brief Acquire a mutex, parking the current task if necessary.
 *
 * @param mutex Mutex to acquire.
 *
 * @return 0 on success, or -1 with @c errno set.
 */
int nm_mutex_lock(nm_mutex_t *mutex) {
    return nm_mutex_lock_impl(mutex, false, 0U, true);
}

/**
 * @brief Acquire a mutex until an absolute deadline.
 *
 * @param mutex       Mutex to acquire.
 * @param deadline_ns Absolute monotonic deadline in nanoseconds.
 *
 * @return 0 on success, or -1 with @c errno set to @c ETIMEDOUT or another
 *         propagated wait error.
 */
int nm_mutex_lock_until(nm_mutex_t *mutex, uint64_t deadline_ns) {
    return nm_mutex_lock_impl(mutex, true, deadline_ns, true);
}

/**
 * @brief Release a mutex owned by the current task.
 *
 * Ownership is transferred directly to the next waiter, if one exists, before
 * that waiter is woken. This avoids a thundering-herd retry on unlock.
 *
 * @param mutex Mutex to release.
 *
 * @return 0 on success, or -1 with @c errno set to @c EINVAL or @c EPERM.
 */
int nm_mutex_unlock(nm_mutex_t *mutex) {
    nm_wait_node_t *node;
    uintptr_t owner;

    nm_task_safepoint();

    if (mutex == NULL || nm_require_task_context() != 0) {
        return -1;
    }

    owner = atomic_load(&mutex->owner);
    if (owner != (uintptr_t)g_nm_tls_task) {
        errno = EPERM;
        return -1;
    }

    pthread_mutex_lock(&mutex->lock);
    node = nm_wait_queue_pop_head(&mutex->waiters);
    if (node != NULL) {
        atomic_store(&mutex->owner, (uintptr_t)node->task);
    } else {
        atomic_store(&mutex->owner, (uintptr_t)0);
    }
    pthread_mutex_unlock(&mutex->lock);

    if (node != NULL) {
        node->error_code = 0;
        nm_wake_wait_node(node, true, NM_WAIT_MUTEX);
    }
    return 0;
}
