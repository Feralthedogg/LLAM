/**
 * @file src/core/task/task_handle_registry.c
 * @brief Public task-handle slot registry and consuming handle claims.
 *
 * @details
 * Task allocation and reuse stay in task_alloc.c.  This file owns the
 * public slot+generation table that prevents stale or forged task handles from
 * becoming raw task pointers.  Join and detach use specialized claim helpers so
 * their handle-consuming hot paths avoid a short active-op pin while retaining
 * the same stale-handle and owner-runtime checks as diagnostic APIs.
 *
 * @copyright Copyright 2026 Feralthedogg
 *
 * @par License
 * SPDX-License-Identifier: Apache-2.0
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * See LICENSE.
 */

#include "runtime_internal.h"
#include "task_handle_registry_internal.h"

pthread_mutex_t g_llam_task_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static llam_task_t *g_llam_task_registry;
llam_public_slot_table_t g_llam_task_public_slots;

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

void llam_task_invalidate_public_handle_locked(llam_task_t *task) {
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

llam_task_t *llam_task_resolve_public_handle_locked(const llam_task_t *handle) {
    if (handle == NULL) {
        return NULL;
    }
    return llam_public_slot_resolve_encoded(&g_llam_task_public_slots,
                                            (uintptr_t)handle,
                                            LLAM_TASK_PUBLIC_HANDLE_SHIFT,
                                            NULL,
                                            NULL);
}

static void llam_task_unregister_live_locked(llam_task_t *task) {

    if (task->public_handle_slot < g_llam_task_public_slots.count) {
        llam_public_slot_release(&g_llam_task_public_slots,
                                 task->public_handle_slot,
                                 task,
                                 atomic_load_explicit(&task->public_handle_generation, memory_order_acquire));
        atomic_store_explicit(&task->public_handle_generation, 0U, memory_order_release);
    }

    if (task->registry_prev_link != NULL) {
        *task->registry_prev_link = task->registry_next;
        if (task->registry_next != NULL) {
            task->registry_next->registry_prev_link = task->registry_prev_link;
        }
        task->registry_next = NULL;
        task->registry_prev_link = NULL;
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
        items[i].registry_prev_link = &g_llam_task_registry;
        if (g_llam_task_registry != NULL) {
            g_llam_task_registry->registry_prev_link = &items[i].registry_next;
        }
        g_llam_task_registry = &items[i];
    }
    pthread_mutex_unlock(&g_llam_task_registry_lock);
}

int llam_task_unregister_public_slab(llam_task_t *items, unsigned count) {
    unsigned i;

    if (items == NULL || count == 0U) {
        return 0;
    }
    for (i = 0U; i < count; ++i) {
        for (;;) {
            size_t active_ops = 0U;
            bool removed = false;

            pthread_mutex_lock(&g_llam_task_registry_lock);
            if (!llam_task_is_live_locked(&items[i])) {
                removed = true;
            } else {
                active_ops = llam_public_active_op_count(&items[i].active_ops);
                if (active_ops == 0U) {
                    llam_task_unregister_live_locked(&items[i]);
                    removed = true;
                }
            }
            pthread_mutex_unlock(&g_llam_task_registry_lock);
            if (removed) {
                break;
            }
            if (LLAM_UNLIKELY(llam_public_active_op_is_saturated(active_ops))) {
                /*
                 * A saturated counter is the permanent busy sentinel produced
                 * after active-op corruption/overflow.  Waiting for it to drain
                 * would hang teardown; callers must keep the slab alive instead
                 * of freeing storage that may still be externally referenced.
                 */
                errno = EBUSY;
                return -1;
            }
            llam_pause_cpu();
        }
    }
    return 0;
}

/**
 * @brief Retire an idle host span without waiting on readers or registry locks.
 *
 * The allocator owns every slot and has observed a zero live-item count. The
 * registry lock excludes new public pins while all existing pins/scan refs are
 * checked and every slot is invalidated. A busy span remains registered and
 * addressable; no partial retirement is performed. Allocator-owned registered
 * storage carries an unlink link, so retirement never scans unrelated tasks.
 * Untrusted public/raw-pointer membership checks still walk/validate as before.
 */
bool llam_task_try_unregister_public_slab(llam_task_t *items, unsigned count) {
    int rc;

    if (items == NULL || count == 0U) {
        errno = EINVAL;
        return false;
    }
    rc = pthread_mutex_trylock(&g_llam_task_registry_lock);
    if (rc != 0) {
        errno = rc;
        return false;
    }
    for (unsigned i = 0U; i < count; ++i) {
        if (llam_public_active_op_count(&items[i].active_ops) != 0U ||
            (items[i].owner_runtime != NULL &&
             atomic_load_explicit(&items[i].scan_refs, memory_order_acquire) != 0U)) {
            pthread_mutex_unlock(&g_llam_task_registry_lock);
            errno = EBUSY;
            return false;
        }
    }
    for (unsigned i = 0U; i < count; ++i) {
        if (items[i].registry_prev_link != NULL) {
            llam_task_unregister_live_locked(&items[i]);
            items[i].public_handle_slot = SIZE_MAX;
        }
    }
    pthread_mutex_unlock(&g_llam_task_registry_lock);
    return true;
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
    if (task != NULL &&
        llam_public_active_op_try_begin(&task->active_ops) != 0) {
        task = NULL;
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

int llam_task_wait_public_ops_quiescent(llam_task_t *task) {
    if (task == NULL) {
        return 0;
    }
    for (;;) {
        size_t active_ops = llam_public_active_op_count(&task->active_ops);

        if (active_ops == 0U) {
            return 0;
        }
        if (LLAM_UNLIKELY(llam_public_active_op_is_saturated(active_ops))) {
            errno = EBUSY;
            return -1;
        }
        llam_pause_cpu();
    }
}
