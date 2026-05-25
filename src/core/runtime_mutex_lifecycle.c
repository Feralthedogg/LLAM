/**
 * @file src/core/runtime_mutex_lifecycle.c
 * @brief Public handle registry and lifetime operations for LLAM mutexes.
 *
 * @details
 * Mutex objects are exposed through slot+generation public handles instead of
 * raw object addresses. This file owns the live mutex registry, stale-handle
 * rejection, and create/destroy paths; runtime_mutex.c keeps the hot
 * lock/unlock state machine separate.
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

static pthread_mutex_t g_llam_mutex_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static llam_mutex_t *g_llam_mutex_registry;
static llam_public_slot_table_t g_llam_mutex_public_slots;

/*
 * Public mutex handles are slot+generation values.  The live registry and slot
 * table let every public entry point reject stale handles without reading a
 * mutex object that has already been freed or reallocated.
 */
static int llam_mutex_reserve_public_slot_locked(llam_mutex_t *mutex, size_t *out_slot) {
    uint32_t generation = 0U;

    return llam_public_slot_reserve_family_secret(&g_llam_mutex_public_slots,
                                                  mutex,
                                                  64U,
                                                  LLAM_PUBLIC_HANDLE_FAMILY_MUTEX,
                                                  mutex->owner_runtime != NULL
                                                      ? mutex->owner_runtime->public_handle_secret
                                                      : 0U,
                                                  out_slot,
                                                  &generation);
}

static int llam_mutex_register_live(llam_mutex_t *mutex) {
    size_t slot = 0U;

    pthread_mutex_lock(&g_llam_mutex_registry_lock);
    if (llam_mutex_reserve_public_slot_locked(mutex, &slot) != 0) {
        pthread_mutex_unlock(&g_llam_mutex_registry_lock);
        return -1;
    }
    mutex->public_handle_slot = slot;
    mutex->public_handle_generation = llam_public_slot_generation(&g_llam_mutex_public_slots, slot);
    mutex->registry_next = g_llam_mutex_registry;
    g_llam_mutex_registry = mutex;
    pthread_mutex_unlock(&g_llam_mutex_registry_lock);
    return 0;
}

static void llam_mutex_unregister_live_locked(llam_mutex_t *mutex) {
    llam_mutex_t **link = &g_llam_mutex_registry;

    if (mutex->public_handle_slot < g_llam_mutex_public_slots.count) {
        llam_public_slot_release(&g_llam_mutex_public_slots,
                                 mutex->public_handle_slot,
                                 mutex,
                                 mutex->public_handle_generation);
    }

    while (*link != NULL) {
        if (*link == mutex) {
            *link = mutex->registry_next;
            mutex->registry_next = NULL;
            return;
        }
        link = &(*link)->registry_next;
    }
}

llam_mutex_t *llam_mutex_resolve_public_handle(const llam_mutex_t *handle) {
    llam_mutex_t *mutex = NULL;

    if (handle == NULL) {
        return NULL;
    }

    /*
     * Do not cast the incoming public handle into an object address. A consumed
     * handle may name a slot whose storage was reused, but it cannot match the
     * replacement mutex generation.
     */
    pthread_mutex_lock(&g_llam_mutex_registry_lock);
    mutex = llam_public_slot_resolve_encoded(&g_llam_mutex_public_slots,
                                             (uintptr_t)handle,
                                             LLAM_SYNC_PUBLIC_HANDLE_SHIFT,
                                             NULL,
                                             NULL);
    if (mutex != NULL) {
        llam_public_active_op_begin(&mutex->active_ops);
    }
    pthread_mutex_unlock(&g_llam_mutex_registry_lock);
    return mutex;
}

void llam_mutex_end_public_op(llam_mutex_t *mutex) {
    if (mutex == NULL) {
        return;
    }
    llam_public_active_op_end(&mutex->active_ops);
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

    mutex->owner_runtime = llam_runtime_owner_for_new_object();
    llam_public_active_op_init(&mutex->active_ops);
    atomic_init(&mutex->owner, (uintptr_t)0);
    rc = pthread_mutex_init(&mutex->lock, NULL);
    if (rc != 0) {
        free(mutex);
        // pthread mutex APIs return the error code directly; do not collapse
        // resource exhaustion, permission, or system-limit failures to ENOMEM.
        errno = rc;
        return NULL;
    }
    if (llam_mutex_register_live(mutex) != 0) {
        pthread_mutex_destroy(&mutex->lock);
        free(mutex);
        return NULL;
    }

    return llam_mutex_public_handle(mutex);
}

/**
 * @brief Destroy a runtime-aware mutex.
 *
 * The caller must ensure no task owns or waits on the mutex and no public
 * operation is currently pinned in the handle registry.
 *
 * @param mutex Mutex to destroy.
 *
 * @return 0 on success, or -1 with @c errno set to @c EINVAL or @c EBUSY.
 */
int llam_mutex_destroy(llam_mutex_t *mutex) {
    uintptr_t handle = (uintptr_t)mutex;
    size_t slot;
    uint32_t generation;

    pthread_mutex_lock(&g_llam_mutex_registry_lock);
    mutex = llam_public_slot_resolve_encoded(&g_llam_mutex_public_slots,
                                             handle,
                                             LLAM_SYNC_PUBLIC_HANDLE_SHIFT,
                                             &slot,
                                             &generation);
    if (mutex == NULL ||
        mutex->public_handle_slot != slot ||
        mutex->public_handle_generation != generation) {
        pthread_mutex_unlock(&g_llam_mutex_registry_lock);
        errno = EINVAL;
        return -1;
    }
    if (llam_runtime_check_object_owner_for_cleanup(mutex->owner_runtime) != 0) {
        pthread_mutex_unlock(&g_llam_mutex_registry_lock);
        return -1;
    }

    pthread_mutex_lock(&mutex->lock);
    if (atomic_load(&mutex->owner) != (uintptr_t)0 ||
        mutex->waiters.head != NULL ||
        mutex->waiters.depth != 0U ||
        llam_public_active_op_count(&mutex->active_ops) != 0U) {
        pthread_mutex_unlock(&mutex->lock);
        pthread_mutex_unlock(&g_llam_mutex_registry_lock);
        errno = EBUSY;
        return -1;
    }
    llam_mutex_unregister_live_locked(mutex);
    pthread_mutex_unlock(&mutex->lock);
    pthread_mutex_unlock(&g_llam_mutex_registry_lock);
    pthread_mutex_destroy(&mutex->lock);
    free(mutex);
    return 0;
}
