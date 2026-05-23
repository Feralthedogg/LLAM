/**
 * @file src/core/runtime_cond_lifecycle.c
 * @brief Condition-variable public handles, creation, and destruction.
 *
 * @details
 * Wait, signal, and broadcast stay in runtime_cond.c.  This file keeps the
 * public slot+generation registry and destruction checks together so stale
 * handles, active public operations, and live waiters share one validation
 * path.
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

static pthread_mutex_t g_llam_cond_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static llam_cond_t *g_llam_cond_registry;
static llam_public_slot_table_t g_llam_cond_public_slots;

static int llam_cond_reserve_public_slot_locked(llam_cond_t *cond, size_t *out_slot) {
    uint32_t generation = 0U;

    return llam_public_slot_reserve(&g_llam_cond_public_slots, cond, 64U, out_slot, &generation);
}

static int llam_cond_register_live(llam_cond_t *cond) {
    size_t slot = 0U;

    pthread_mutex_lock(&g_llam_cond_registry_lock);
    if (llam_cond_reserve_public_slot_locked(cond, &slot) != 0) {
        pthread_mutex_unlock(&g_llam_cond_registry_lock);
        return -1;
    }
    cond->public_handle_slot = slot;
    cond->public_handle_generation = llam_public_slot_generation(&g_llam_cond_public_slots, slot);
    cond->registry_next = g_llam_cond_registry;
    g_llam_cond_registry = cond;
    pthread_mutex_unlock(&g_llam_cond_registry_lock);
    return 0;
}

static void llam_cond_unregister_live_locked(llam_cond_t *cond) {
    llam_cond_t **link = &g_llam_cond_registry;

    if (cond->public_handle_slot < g_llam_cond_public_slots.count) {
        llam_public_slot_release(&g_llam_cond_public_slots,
                                 cond->public_handle_slot,
                                 cond,
                                 cond->public_handle_generation);
    }

    while (*link != NULL) {
        if (*link == cond) {
            *link = cond->registry_next;
            cond->registry_next = NULL;
            return;
        }
        link = &(*link)->registry_next;
    }
}

llam_cond_t *llam_cond_resolve_public_handle(const llam_cond_t *handle) {
    llam_cond_t *cond = NULL;

    if (handle == NULL) {
        return NULL;
    }

    /*
     * Public handles are encoded slot+generation values. Never reinterpret the
     * incoming value as an address; stale handles must fail even after storage
     * reuse.
     */
    pthread_mutex_lock(&g_llam_cond_registry_lock);
    cond = llam_public_slot_resolve_encoded(&g_llam_cond_public_slots,
                                            (uintptr_t)handle,
                                            LLAM_SYNC_PUBLIC_HANDLE_SHIFT,
                                            NULL,
                                            NULL);
    if (cond != NULL) {
        llam_public_active_op_begin(&cond->active_ops);
    }
    pthread_mutex_unlock(&g_llam_cond_registry_lock);
    return cond;
}

void llam_cond_end_public_op(llam_cond_t *cond) {
    if (cond == NULL) {
        return;
    }
    llam_public_active_op_end(&cond->active_ops);
}

llam_cond_t *llam_cond_create(void) {
    llam_cond_t *cond = calloc(1, sizeof(*cond));
    int rc;

    if (cond == NULL) {
        return NULL;
    }

    cond->owner_runtime = llam_runtime_owner_for_new_object();
    llam_public_active_op_init(&cond->active_ops);
    atomic_init(&cond->inflight_waiters, 0U);
    rc = pthread_mutex_init(&cond->lock, NULL);
    if (rc != 0) {
        free(cond);
        errno = rc;
        return NULL;
    }
    if (llam_cond_register_live(cond) != 0) {
        pthread_mutex_destroy(&cond->lock);
        free(cond);
        return NULL;
    }

    return llam_cond_public_handle(cond);
}

int llam_cond_destroy(llam_cond_t *cond) {
    uintptr_t handle = (uintptr_t)cond;
    size_t slot;
    uint32_t generation;

    pthread_mutex_lock(&g_llam_cond_registry_lock);
    cond = llam_public_slot_resolve_encoded(&g_llam_cond_public_slots,
                                            handle,
                                            LLAM_SYNC_PUBLIC_HANDLE_SHIFT,
                                            &slot,
                                            &generation);
    if (cond == NULL ||
        cond->public_handle_slot != slot ||
        cond->public_handle_generation != generation) {
        pthread_mutex_unlock(&g_llam_cond_registry_lock);
        errno = EINVAL;
        return -1;
    }
    if (llam_runtime_check_object_owner(cond->owner_runtime) != 0) {
        pthread_mutex_unlock(&g_llam_cond_registry_lock);
        return -1;
    }

    pthread_mutex_lock(&cond->lock);
    if (atomic_load_explicit(&cond->inflight_waiters, memory_order_acquire) != 0U ||
        llam_public_active_op_count(&cond->active_ops) != 0U ||
        cond->waiters.head != NULL ||
        cond->waiters.depth != 0U) {
        pthread_mutex_unlock(&cond->lock);
        pthread_mutex_unlock(&g_llam_cond_registry_lock);
        errno = EBUSY;
        return -1;
    }
    llam_cond_unregister_live_locked(cond);
    pthread_mutex_unlock(&cond->lock);
    pthread_mutex_unlock(&g_llam_cond_registry_lock);
    pthread_mutex_destroy(&cond->lock);
    free(cond);
    return 0;
}
