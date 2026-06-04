/**
 * @file src/core/task/task_handle_claim.c
 * @brief Consuming join/detach claims for public task handles.
 *
 * @copyright Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: Apache-2.0
 */

#include "runtime_internal.h"
#include "task_handle_registry_internal.h"

static int llam_task_claim_join_public_handle_for_group(const llam_task_t *handle,
                                                        llam_task_group_t *group,
                                                        bool group_join,
                                                        llam_task_t *self,
                                                        llam_task_t **out_task,
                                                        llam_runtime_t **out_rt,
                                                        bool *out_task_pinned) {
    llam_task_t *task;
    llam_runtime_t *rt;
    unsigned expected = 0U;

    if (LLAM_UNLIKELY(out_task == NULL || out_rt == NULL || out_task_pinned == NULL ||
                      (group_join && group == NULL))) {
        errno = EINVAL;
        return -1;
    }
    *out_task = NULL;
    *out_rt = NULL;
    *out_task_pinned = false;

    pthread_mutex_lock(&g_llam_task_registry_lock);
    task = llam_task_resolve_public_handle_locked(handle);
    if (LLAM_UNLIKELY(task == NULL || task->owner_runtime == NULL)) {
        pthread_mutex_unlock(&g_llam_task_registry_lock);
        errno = EINVAL;
        return -1;
    }
    if (LLAM_UNLIKELY(llam_runtime_check_object_owner(task->owner_runtime) != 0)) {
        pthread_mutex_unlock(&g_llam_task_registry_lock);
        return -1;
    }
    rt = task->owner_runtime;
    if (LLAM_UNLIKELY(!atomic_load_explicit(&rt->initialized, memory_order_acquire))) {
        pthread_mutex_unlock(&g_llam_task_registry_lock);
        errno = EINVAL;
        return -1;
    }
    if (LLAM_UNLIKELY(atomic_load_explicit(&task->detached, memory_order_acquire) != 0U)) {
        pthread_mutex_unlock(&g_llam_task_registry_lock);
        errno = EINVAL;
        return -1;
    }
    if (LLAM_UNLIKELY(task->owning_group != NULL)) {
        if (!group_join || task->owning_group != group) {
            pthread_mutex_unlock(&g_llam_task_registry_lock);
            errno = EBUSY;
            return -1;
        }
    } else if (LLAM_UNLIKELY(group_join)) {
        pthread_mutex_unlock(&g_llam_task_registry_lock);
        errno = EINVAL;
        return -1;
    }
    if (LLAM_UNLIKELY(self == task)) {
        pthread_mutex_unlock(&g_llam_task_registry_lock);
        errno = EDEADLK;
        return -1;
    }
    if (LLAM_UNLIKELY(self == NULL &&
                      llam_public_active_op_is_saturated(llam_public_active_op_count(&task->active_ops)))) {
        pthread_mutex_unlock(&g_llam_task_registry_lock);
        errno = EBUSY;
        return -1;
    }
    if (LLAM_UNLIKELY(!atomic_compare_exchange_strong_explicit(&task->join_claimed,
                                                               &expected,
                                                               1U,
                                                               memory_order_acq_rel,
                                                               memory_order_acquire))) {
        pthread_mutex_unlock(&g_llam_task_registry_lock);
        errno = EBUSY;
        return -1;
    }
    if (self == NULL) {
        /*
         * Unmanaged joins poll a raw task pointer outside scheduler protection.
         * Pin before dropping the registry lock so runtime destruction cannot
         * free the slab while the host thread validates or times out the join.
         */
        if (llam_public_active_op_try_begin(&task->active_ops) != 0) {
            atomic_store_explicit(&task->join_claimed, 0U, memory_order_release);
            pthread_mutex_unlock(&g_llam_task_registry_lock);
            return -1;
        }
        *out_task_pinned = true;
    }

    pthread_mutex_unlock(&g_llam_task_registry_lock);

    *out_task = task;
    *out_rt = rt;
    return 0;
}

int llam_task_claim_join_public_handle(const llam_task_t *handle,
                                       llam_task_t *self,
                                       llam_task_t **out_task,
                                       llam_runtime_t **out_rt,
                                       bool *out_task_pinned) {
    return llam_task_claim_join_public_handle_for_group(handle,
                                                        NULL,
                                                        false,
                                                        self,
                                                        out_task,
                                                        out_rt,
                                                        out_task_pinned);
}

int llam_task_claim_group_join_public_handle(const llam_task_t *handle,
                                             llam_task_group_t *group,
                                             llam_task_t *self,
                                             llam_task_t **out_task,
                                             llam_runtime_t **out_rt,
                                             bool *out_task_pinned) {
    return llam_task_claim_join_public_handle_for_group(handle,
                                                        group,
                                                        true,
                                                        self,
                                                        out_task,
                                                        out_rt,
                                                        out_task_pinned);
}

int llam_task_claim_detach_public_handle(const llam_task_t *handle,
                                         llam_task_t **out_task,
                                         llam_runtime_t **out_rt,
                                         bool *out_reclaim_after_unlock,
                                         bool *out_task_pinned) {
    llam_task_t *task;
    llam_runtime_t *rt;
    unsigned expected = 0U;
    bool host_thread;
    bool host_pin_taken = false;

    if (LLAM_UNLIKELY(out_task == NULL || out_rt == NULL || out_reclaim_after_unlock == NULL || out_task_pinned == NULL)) {
        errno = EINVAL;
        return -1;
    }
    *out_task = NULL;
    *out_rt = NULL;
    *out_reclaim_after_unlock = false;
    *out_task_pinned = false;

    pthread_mutex_lock(&g_llam_task_registry_lock);
    task = llam_task_resolve_public_handle_locked(handle);
    if (LLAM_UNLIKELY(task == NULL || task->owner_runtime == NULL)) {
        pthread_mutex_unlock(&g_llam_task_registry_lock);
        errno = EINVAL;
        return -1;
    }
    if (LLAM_UNLIKELY(llam_runtime_check_object_owner(task->owner_runtime) != 0)) {
        pthread_mutex_unlock(&g_llam_task_registry_lock);
        return -1;
    }
    rt = task->owner_runtime;
    if (LLAM_UNLIKELY(!atomic_load_explicit(&rt->initialized, memory_order_acquire))) {
        pthread_mutex_unlock(&g_llam_task_registry_lock);
        errno = EINVAL;
        return -1;
    }
    if (LLAM_UNLIKELY(task->owning_group != NULL)) {
        pthread_mutex_unlock(&g_llam_task_registry_lock);
        errno = EBUSY;
        return -1;
    }

    pthread_mutex_lock(&task->lock);
    if (LLAM_UNLIKELY(atomic_load_explicit(&task->detached, memory_order_acquire) != 0U)) {
        pthread_mutex_unlock(&task->lock);
        pthread_mutex_unlock(&g_llam_task_registry_lock);
        errno = EINVAL;
        return -1;
    }
    if (LLAM_UNLIKELY(task->join_waiter_count > 0U)) {
        pthread_mutex_unlock(&task->lock);
        pthread_mutex_unlock(&g_llam_task_registry_lock);
        errno = EBUSY;
        return -1;
    }
    if (LLAM_UNLIKELY(!atomic_compare_exchange_strong_explicit(&task->join_claimed,
                                                               &expected,
                                                               1U,
                                                               memory_order_acq_rel,
                                                               memory_order_acquire))) {
        pthread_mutex_unlock(&task->lock);
        pthread_mutex_unlock(&g_llam_task_registry_lock);
        errno = EBUSY;
        return -1;
    }

    host_thread = g_llam_tls_task == NULL && g_llam_tls_shard == NULL;
    if (host_thread && atomic_load_explicit(&task->reclaim_ready, memory_order_acquire) != 0U) {
        if (llam_public_active_op_try_begin(&task->active_ops) != 0) {
            atomic_store_explicit(&task->join_claimed, 0U, memory_order_release);
            pthread_mutex_unlock(&task->lock);
            pthread_mutex_unlock(&g_llam_task_registry_lock);
            return -1;
        }
        host_pin_taken = true;
    }

    llam_task_invalidate_public_handle_locked(task);
    atomic_store_explicit(&task->detached, 1U, memory_order_release);
    expected = 0U;
    if (atomic_load_explicit(&task->reclaim_ready, memory_order_acquire) != 0U &&
        atomic_compare_exchange_strong_explicit(&task->reclaim_claimed,
                                                &expected,
                                                1U,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        *out_reclaim_after_unlock = true;
        if (host_pin_taken) {
            *out_task_pinned = true;
        }
    } else if (host_pin_taken) {
        llam_public_active_op_end(&task->active_ops);
    }
    pthread_mutex_unlock(&task->lock);
    pthread_mutex_unlock(&g_llam_task_registry_lock);

    *out_task = task;
    *out_rt = rt;
    return 0;
}
