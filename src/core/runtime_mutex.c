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
 * @brief Donate waiter priority to a current mutex owner.
 *
 * This is a bounded, non-transitive priority-inheritance hint: latency-class
 * waiters can temporarily raise the owner class until unlock restores the
 * owner's base class.
 */
static void llam_mutex_donate_priority(uintptr_t owner_value, const llam_task_t *waiter) {
    llam_task_t *owner = (llam_task_t *)owner_value;
    unsigned waiter_class;
    unsigned owner_class;

    if (owner == NULL || waiter == NULL || owner == waiter) {
        return;
    }
    waiter_class = atomic_load_explicit(&((llam_task_t *)waiter)->task_class, memory_order_acquire);
    owner_class = atomic_load_explicit(&owner->task_class, memory_order_acquire);
    while (waiter_class < owner_class &&
           !atomic_compare_exchange_weak_explicit(&owner->task_class,
                                                  &owner_class,
                                                  waiter_class,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
    }
}

/**
 * @brief Allocate a runtime-aware mutex.
 *
 * @return New mutex on success, or @c NULL with @c errno set on failure.
 */
llam_mutex_t *llam_mutex_create(void) {
    llam_mutex_t *mutex = calloc(1, sizeof(*mutex));
    int rc;

    if (mutex == NULL) {
        return NULL;
    }

    atomic_init(&mutex->owner, (uintptr_t)0);
    rc = pthread_mutex_init(&mutex->lock, NULL);
    if (rc != 0) {
        free(mutex);
        // pthread mutex APIs return the error code directly; do not collapse
        // resource exhaustion, permission, or system-limit failures to ENOMEM.
        errno = rc;
        return NULL;
    }

    return mutex;
}

/**
 * @brief Destroy a runtime-aware mutex.
 *
 * The caller must ensure no task owns or waits on the mutex.
 *
 * @param mutex Mutex to destroy.
 *
 * @return 0 on success, or -1 with @c errno set to @c EINVAL or @c EBUSY.
 */
int llam_mutex_destroy(llam_mutex_t *mutex) {
    if (mutex == NULL) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&mutex->lock);
    if (atomic_load(&mutex->owner) != (uintptr_t)0 ||
        mutex->waiters.head != NULL ||
        mutex->waiters.depth != 0U) {
        pthread_mutex_unlock(&mutex->lock);
        errno = EBUSY;
        return -1;
    }
    pthread_mutex_unlock(&mutex->lock);
    pthread_mutex_destroy(&mutex->lock);
    free(mutex);
    return 0;
}

/**
 * @brief Attempt to acquire a mutex without parking.
 *
 * @param mutex Mutex to acquire.
 *
 * @return 0 on success, or -1 with @c errno set to @c EINVAL or @c EBUSY.
 */
int llam_mutex_trylock(llam_mutex_t *mutex) {
    uintptr_t expected = 0U;

    llam_task_safepoint();

    if (mutex == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (llam_require_task_context() != 0) {
        return -1;
    }

    if (atomic_compare_exchange_strong(&mutex->owner, &expected, (uintptr_t)g_llam_tls_task)) {
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
int llam_mutex_lock_impl(llam_mutex_t *mutex, bool has_deadline, uint64_t deadline_ns, bool register_cancel) {
    llam_shard_t *shard;
    llam_task_t *task;
    llam_wait_node_t *node;
    uintptr_t expected = 0U;
    uintptr_t current;
    int rc = 0;

    llam_task_safepoint();

    if (mutex == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (llam_require_task_context() != 0) {
        return -1;
    }
    task = g_llam_tls_task;
    shard = g_llam_tls_shard;
    current = (uintptr_t)task;
    if (atomic_load(&mutex->owner) == current) {
        errno = EDEADLK;
        return -1;
    }

    if (atomic_compare_exchange_strong(&mutex->owner, &expected, current)) {
        return 0;
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

    pthread_mutex_lock(&mutex->lock);
    expected = 0U;
    if (atomic_compare_exchange_strong(&mutex->owner, &expected, (uintptr_t)task)) {
        pthread_mutex_unlock(&mutex->lock);
        llam_sync_wait_node_release(shard, node);
        return 0;
    }
    llam_mutex_donate_priority(expected, task);
    llam_wait_queue_push_tail(&mutex->waiters, node);
    pthread_mutex_unlock(&mutex->lock);
    llam_task_ensure_listed(task);
    llam_task_set_wait_node_tracking(task, node, &mutex->waiters, &mutex->lock, shard->id);
    task->state = LLAM_TASK_STATE_PARKED;
    task->wait_reason = LLAM_WAIT_MUTEX;
    if (has_deadline && llam_arm_task_wait_deadline(task, shard, deadline_ns) != 0) {
        bool removed;

        if (llam_wait_node_completed(node)) {
            goto wait_ready;
        }
        pthread_mutex_lock(&mutex->lock);
        removed = llam_wait_queue_remove(&mutex->waiters, node);
        pthread_mutex_unlock(&mutex->lock);
        if (!removed) {
            /* unlock already transferred ownership to this waiter. */
            goto wait_ready;
        }
        task->state = LLAM_TASK_STATE_RUNNING;
        task->wait_reason = LLAM_WAIT_NONE;
        llam_task_clear_wait_tracking(task);
        llam_sync_wait_node_release(shard, node);
        return -1;
    }
    if (register_cancel && task->cancel_token != NULL && llam_cancel_token_register_task(task) != 0) {
        bool removed;

        if (llam_wait_node_completed(node)) {
            goto wait_ready;
        }
        llam_disarm_task_wait_deadline(task);
        pthread_mutex_lock(&mutex->lock);
        removed = llam_wait_queue_remove(&mutex->waiters, node);
        pthread_mutex_unlock(&mutex->lock);
        if (!removed) {
            /* unlock already transferred ownership to this waiter. */
            goto wait_ready;
        }
        task->state = LLAM_TASK_STATE_RUNNING;
        task->wait_reason = LLAM_WAIT_NONE;
        llam_task_clear_wait_tracking(task);
        llam_sync_wait_node_release(shard, node);
        return -1;
    }

wait_ready:
    if (llam_wait_node_should_park(node)) {
        llam_park_current_task(LLAM_WAIT_MUTEX, LLAM_TRACE_STATE);
    }
    if (has_deadline) {
        /*
         * A peer can complete the wait after this node is queued but before the
         * task has actually switched to the scheduler.  In that early-wake
         * case the generic wake path cannot disarm a timer that has not been
         * armed yet, so the waiter owns the final cleanup after it resumes.
         */
        llam_disarm_task_wait_deadline(task);
    }
    if (register_cancel) {
        llam_cancel_token_unregister_task(task);
    }
    llam_task_clear_wait_tracking(task);
    rc = node->error_code;
    llam_sync_wait_node_release(shard, node);
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
int llam_mutex_lock(llam_mutex_t *mutex) {
    return llam_mutex_lock_impl(mutex, false, 0U, true);
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
int llam_mutex_lock_until(llam_mutex_t *mutex, uint64_t deadline_ns) {
    return llam_mutex_lock_impl(mutex, true, deadline_ns, true);
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
int llam_mutex_unlock(llam_mutex_t *mutex) {
    llam_wait_node_t *node;
    uintptr_t owner;

    llam_task_safepoint();

    if (mutex == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (llam_require_task_context() != 0) {
        return -1;
    }

    owner = atomic_load(&mutex->owner);
    if (owner != (uintptr_t)g_llam_tls_task) {
        errno = EPERM;
        return -1;
    }

    pthread_mutex_lock(&mutex->lock);
    node = llam_wait_queue_pop_head(&mutex->waiters);
    if (node != NULL) {
        atomic_store(&mutex->owner, (uintptr_t)node->task);
    } else {
        atomic_store(&mutex->owner, (uintptr_t)0);
    }
    pthread_mutex_unlock(&mutex->lock);
    atomic_store_explicit(&g_llam_tls_task->task_class,
                          atomic_load_explicit(&g_llam_tls_task->base_task_class, memory_order_acquire),
                          memory_order_release);

    if (node != NULL) {
        node->error_code = 0;
        llam_wake_wait_node(node, true, LLAM_WAIT_MUTEX);
    }
    return 0;
}
