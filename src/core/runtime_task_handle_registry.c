/**
 * @file src/core/runtime_task_handle_registry.c
 * @brief Public task-handle slot registry and consuming handle claims.
 *
 * @details
 * Task allocation and reuse stay in runtime_task_alloc.c.  This file owns the
 * public slot+generation table that prevents stale or forged task handles from
 * becoming raw task pointers.  Join and detach use specialized claim helpers so
 * their handle-consuming hot paths avoid a short active-op pin while retaining
 * the same stale-handle and owner-runtime checks as diagnostic APIs.
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

static pthread_mutex_t g_llam_task_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static llam_task_t *g_llam_task_registry;
static llam_public_slot_table_t g_llam_task_public_slots;

/**
 * @brief Return true when a task object is still registered as allocated storage.
 *
 * Task slab entries live in an allocation registry even before they receive a
 * public slot.  That registry is separate from the slot table: the registry
 * protects slab teardown and diagnostics, while the slot table validates
 * exported slot+generation handles without treating the handle value as a raw
 * task pointer.
 */
static bool llam_task_is_live_locked(const llam_task_t *task) {
    const llam_task_t *iter;

    for (iter = g_llam_task_registry; iter != NULL; iter = iter->registry_next) {
        if (iter == task) {
            return true;
        }
    }
    return false;
}

static int llam_task_reserve_public_slot_locked(llam_task_t *task, size_t *out_slot) {
    uint32_t generation = 0U;

    return llam_public_slot_reserve_family_secret(&g_llam_task_public_slots,
                                                  task,
                                                  256U,
                                                  LLAM_PUBLIC_HANDLE_FAMILY_TASK,
                                                  task->owner_runtime != NULL
                                                      ? task->owner_runtime->public_handle_secret
                                                      : 0U,
                                                  out_slot,
                                                  &generation);
}

static void llam_task_invalidate_public_handle_locked(llam_task_t *task) {
    uint32_t generation = 0U;
    int saved_errno = errno;

    if (task == NULL ||
        llam_public_slot_reactivate_family_secret(&g_llam_task_public_slots,
                                                  task->public_handle_slot,
                                                  task,
                                                  LLAM_PUBLIC_HANDLE_FAMILY_TASK,
                                                  task->owner_runtime != NULL
                                                      ? task->owner_runtime->public_handle_secret
                                                      : 0U,
                                                  &generation) == 0) {
        if (task != NULL) {
            atomic_store_explicit(&task->public_handle_generation, generation, memory_order_release);
        }
        errno = saved_errno;
        return;
    }
    if (errno == EOVERFLOW) {
        size_t old_slot = task->public_handle_slot;
        uint32_t old_generation = atomic_load_explicit(&task->public_handle_generation, memory_order_acquire);
        size_t new_slot = 0U;

        if (old_slot < g_llam_task_public_slots.count) {
            llam_public_slot_release(&g_llam_task_public_slots, old_slot, task, old_generation);
        }
        task->public_handle_slot = SIZE_MAX;
        atomic_store_explicit(&task->public_handle_generation, 0U, memory_order_release);
        if (llam_task_reserve_public_slot_locked(task, &new_slot) == 0) {
            task->public_handle_slot = new_slot;
            atomic_store_explicit(&task->public_handle_generation,
                                  llam_public_slot_generation(&g_llam_task_public_slots, new_slot),
                                  memory_order_release);
        }
    }
    errno = saved_errno;
}

int llam_task_activate_public_handle(llam_task_t *task) {
    int rc = 0;

    if (task == NULL) {
        errno = EINVAL;
        return -1;
    }
    /*
     * Recycled tasks keep their reserved public slot. Reclaim already advanced
     * the generation that consumed the previous handle, so the spawn hot path
     * can reuse the prepared slot without taking the task registry mutex.
     */
    if (task->public_handle_slot != SIZE_MAX &&
        atomic_load_explicit(&task->public_handle_generation, memory_order_acquire) != 0U) {
        return 0;
    }
    pthread_mutex_lock(&g_llam_task_registry_lock);
    if (task->public_handle_slot >= g_llam_task_public_slots.count ||
        llam_public_slot_resolve(&g_llam_task_public_slots,
                                 task->public_handle_slot,
                                 llam_public_slot_generation(&g_llam_task_public_slots,
                                                             task->public_handle_slot)) != task) {
        size_t slot = 0U;

        if (llam_task_reserve_public_slot_locked(task, &slot) != 0) {
            rc = -1;
        } else {
            task->public_handle_slot = slot;
            atomic_store_explicit(&task->public_handle_generation,
                                  llam_public_slot_generation(&g_llam_task_public_slots, slot),
                                  memory_order_release);
        }
    } else {
        llam_task_invalidate_public_handle_locked(task);
    }
    pthread_mutex_unlock(&g_llam_task_registry_lock);
    return rc;
}

void llam_task_invalidate_public_handle(llam_task_t *task) {
    if (task == NULL) {
        return;
    }
    pthread_mutex_lock(&g_llam_task_registry_lock);
    if (task->public_handle_slot < g_llam_task_public_slots.count &&
        llam_public_slot_resolve(&g_llam_task_public_slots,
                                 task->public_handle_slot,
                                 atomic_load_explicit(&task->public_handle_generation, memory_order_acquire)) == task) {
        llam_task_invalidate_public_handle_locked(task);
    }
    pthread_mutex_unlock(&g_llam_task_registry_lock);
}

static llam_task_t *llam_task_resolve_public_handle_locked(const llam_task_t *handle) {
    if (handle == NULL) {
        return NULL;
    }
    return llam_public_slot_resolve_encoded(&g_llam_task_public_slots,
                                            (uintptr_t)handle,
                                            LLAM_TASK_PUBLIC_HANDLE_SHIFT,
                                            NULL,
                                            NULL);
}

int llam_task_claim_join_public_handle(const llam_task_t *handle,
                                       llam_task_t *self,
                                       llam_task_t **out_task,
                                       llam_runtime_t **out_rt,
                                       bool *out_task_pinned) {
    llam_task_t *task;
    llam_runtime_t *rt;
    unsigned expected = 0U;

    if (LLAM_UNLIKELY(out_task == NULL || out_rt == NULL || out_task_pinned == NULL)) {
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
    if (LLAM_UNLIKELY(self == task)) {
        pthread_mutex_unlock(&g_llam_task_registry_lock);
        errno = EDEADLK;
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
         * Pin the task before dropping the registry lock so concurrent runtime
         * destruction cannot free the slab while the host thread is still
         * validating or timing out the join.
         */
        llam_public_active_op_begin(&task->active_ops);
        *out_task_pinned = true;
    }

    pthread_mutex_unlock(&g_llam_task_registry_lock);

    *out_task = task;
    *out_rt = rt;
    return 0;
}

int llam_task_claim_detach_public_handle(const llam_task_t *handle,
                                         llam_task_t **out_task,
                                         llam_runtime_t **out_rt,
                                         bool *out_reclaim_after_unlock,
                                         bool *out_task_pinned) {
    llam_task_t *task;
    llam_runtime_t *rt;
    unsigned expected = 0U;

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
        if (g_llam_tls_task == NULL && g_llam_tls_shard == NULL) {
            /*
             * Host-side detach may need to reclaim after the registry lock is
             * released. Hold a short public-op pin so runtime_destroy cannot
             * free the task slab before detach reaches the reclaim path.
             */
            llam_public_active_op_begin(&task->active_ops);
            *out_task_pinned = true;
        }
    }
    pthread_mutex_unlock(&task->lock);
    pthread_mutex_unlock(&g_llam_task_registry_lock);

    *out_task = task;
    *out_rt = rt;
    return 0;
}

static void llam_task_unregister_live_locked(llam_task_t *task) {
    llam_task_t **link = &g_llam_task_registry;

    if (task->public_handle_slot < g_llam_task_public_slots.count) {
        llam_public_slot_release(&g_llam_task_public_slots,
                                 task->public_handle_slot,
                                 task,
                                 atomic_load_explicit(&task->public_handle_generation, memory_order_acquire));
        atomic_store_explicit(&task->public_handle_generation, 0U, memory_order_release);
    }

    while (*link != NULL) {
        if (*link == task) {
            *link = task->registry_next;
            task->registry_next = NULL;
            return;
        }
        link = &(*link)->registry_next;
    }
}

void llam_task_register_public_slab(llam_task_t *items, unsigned count) {
    unsigned i;

    if (items == NULL || count == 0U) {
        return;
    }
    pthread_mutex_lock(&g_llam_task_registry_lock);
    for (i = 0U; i < count; ++i) {
        llam_public_active_op_init(&items[i].active_ops);
        items[i].public_handle_slot = SIZE_MAX;
        atomic_init(&items[i].public_handle_generation, 0U);
        items[i].registry_next = g_llam_task_registry;
        g_llam_task_registry = &items[i];
    }
    pthread_mutex_unlock(&g_llam_task_registry_lock);
}

void llam_task_unregister_public_slab(llam_task_t *items, unsigned count) {
    unsigned i;

    if (items == NULL || count == 0U) {
        return;
    }
    for (i = 0U; i < count; ++i) {
        for (;;) {
            bool removed = false;

            pthread_mutex_lock(&g_llam_task_registry_lock);
            if (!llam_task_is_live_locked(&items[i])) {
                removed = true;
            } else if (llam_public_active_op_count(&items[i].active_ops) == 0U) {
                llam_task_unregister_live_locked(&items[i]);
                removed = true;
            }
            pthread_mutex_unlock(&g_llam_task_registry_lock);
            if (removed) {
                break;
            }
            llam_pause_cpu();
        }
    }
}

llam_task_t *llam_task_resolve_public_handle(const llam_task_t *handle) {
    llam_task_t *task = NULL;

    if (handle == NULL) {
        return NULL;
    }

    pthread_mutex_lock(&g_llam_task_registry_lock);
    task = llam_public_slot_resolve_encoded(&g_llam_task_public_slots,
                                            (uintptr_t)handle,
                                            LLAM_TASK_PUBLIC_HANDLE_SHIFT,
                                            NULL,
                                            NULL);
    if (task != NULL) {
        llam_public_active_op_begin(&task->active_ops);
    }
    pthread_mutex_unlock(&g_llam_task_registry_lock);
    return task;
}

void llam_task_end_public_op(llam_task_t *task) {
    if (task == NULL) {
        return;
    }
    llam_public_active_op_end(&task->active_ops);
}

void llam_task_wait_public_ops_quiescent(llam_task_t *task) {
    if (task == NULL) {
        return;
    }
    for (;;) {
        size_t active_ops = llam_public_active_op_count(&task->active_ops);

        if (active_ops == 0U) {
            return;
        }
        llam_pause_cpu();
    }
}
