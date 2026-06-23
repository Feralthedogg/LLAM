/**
 * @file src/core/task/task_group_registry.c
 * @brief Public task-group handle registry and destroy path.
 *
 * @copyright Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: Apache-2.0
 */

#include "runtime_internal.h"
#include "task_group_internal.h"

#include <stdlib.h>

static pthread_mutex_t g_llam_task_group_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static llam_task_group_t *g_llam_task_group_registry;
static llam_public_slot_table_t g_llam_task_group_public_slots;

llam_task_group_t *llam_task_group_public_handle(llam_task_group_t *group) {
    if (group == NULL) {
        return NULL;
    }
    return (llam_task_group_t *)llam_public_slot_encode_handle(group->public_handle_slot,
                                                               group->public_handle_generation,
                                                               LLAM_TASK_GROUP_PUBLIC_HANDLE_SHIFT);
}

static int llam_task_group_reserve_public_slot_locked(llam_task_group_t *group, size_t *out_slot) {
    uint32_t generation = 0U;

    return llam_public_slot_reserve_family_secret(&g_llam_task_group_public_slots,
                                                  group,
                                                  64U,
                                                  LLAM_PUBLIC_HANDLE_FAMILY_TASK_GROUP,
                                                  group->owner_runtime != NULL
                                                      ? group->owner_runtime->public_handle_secret
                                                      : 0U,
                                                  out_slot,
                                                  &generation);
}

int llam_task_group_register_live(llam_task_group_t *group) {
    size_t slot = 0U;

    pthread_mutex_lock(&g_llam_task_group_registry_lock);
    if (llam_task_group_reserve_public_slot_locked(group, &slot) != 0) {
        pthread_mutex_unlock(&g_llam_task_group_registry_lock);
        return -1;
    }
    group->public_handle_slot = slot;
    group->public_handle_generation = llam_public_slot_generation(&g_llam_task_group_public_slots, slot);
    group->registry_next = g_llam_task_group_registry;
    g_llam_task_group_registry = group;
    pthread_mutex_unlock(&g_llam_task_group_registry_lock);
    return 0;
}

int llam_task_group_lock_live(llam_task_group_t *handle, llam_task_group_t **out_group) {
    llam_task_group_t *group = NULL;

    if (handle == NULL || out_group == NULL) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&g_llam_task_group_registry_lock);
    group = llam_public_slot_resolve_encoded(&g_llam_task_group_public_slots,
                                             (uintptr_t)handle,
                                             LLAM_TASK_GROUP_PUBLIC_HANDLE_SHIFT,
                                             NULL,
                                             NULL);
    if (group == NULL) {
        pthread_mutex_unlock(&g_llam_task_group_registry_lock);
        errno = EINVAL;
        return -1;
    }
    if (llam_runtime_check_object_owner(group->owner_runtime) != 0) {
        pthread_mutex_unlock(&g_llam_task_group_registry_lock);
        return -1;
    }
    pthread_mutex_lock(&group->lock);
    pthread_mutex_unlock(&g_llam_task_group_registry_lock);
    *out_group = group;
    return 0;
}

llam_task_group_t *llam_task_group_resolve_public_handle(llam_task_group_t *handle) {
    llam_task_group_t *group = NULL;

    if (llam_task_group_lock_live(handle, &group) != 0) {
        return NULL;
    }
    if (llam_public_active_op_try_begin(&group->active_ops) != 0) {
        pthread_mutex_unlock(&group->lock);
        return NULL;
    }
    pthread_mutex_unlock(&group->lock);
    return group;
}

void llam_task_group_end_public_op(llam_task_group_t *group) {
    if (group == NULL) {
        return;
    }
    llam_public_active_op_end(&group->active_ops);
}

static void llam_task_group_unregister_live_locked(llam_task_group_t *group) {
    llam_task_group_t **link = &g_llam_task_group_registry;

    if (group->public_handle_slot < g_llam_task_group_public_slots.count) {
        llam_public_slot_release(&g_llam_task_group_public_slots,
                                 group->public_handle_slot,
                                 group,
                                 group->public_handle_generation);
    }

    while (*link != NULL) {
        if (*link == group) {
            *link = group->registry_next;
            group->registry_next = NULL;
            return;
        }
        link = &(*link)->registry_next;
    }
}

int llam_task_group_destroy(llam_task_group_t *group) {
    uintptr_t raw = (uintptr_t)group;
    size_t slot;
    uint32_t generation;

    pthread_mutex_lock(&g_llam_task_group_registry_lock);
    group = llam_public_slot_resolve_encoded(&g_llam_task_group_public_slots,
                                             raw,
                                             LLAM_TASK_GROUP_PUBLIC_HANDLE_SHIFT,
                                             &slot,
                                             &generation);
    if (group == NULL ||
        group->public_handle_slot != slot ||
        group->public_handle_generation != generation) {
        pthread_mutex_unlock(&g_llam_task_group_registry_lock);
        errno = EINVAL;
        return -1;
    }
    if (llam_runtime_check_object_owner_for_cleanup(group->owner_runtime) != 0) {
        pthread_mutex_unlock(&g_llam_task_group_registry_lock);
        return -1;
    }
    pthread_mutex_lock(&group->lock);
    if (group->count != 0U ||
        group->active_spawns != 0U ||
        llam_public_active_op_count(&group->active_ops) != 0U ||
        group->joining) {
        pthread_mutex_unlock(&group->lock);
        pthread_mutex_unlock(&g_llam_task_group_registry_lock);
        errno = EBUSY;
        return -1;
    }
    if (group->cancel_token != NULL && llam_cancel_token_destroy(group->cancel_token) != 0) {
        pthread_mutex_unlock(&group->lock);
        pthread_mutex_unlock(&g_llam_task_group_registry_lock);
        return -1;
    }
    group->cancel_token = NULL;
    llam_task_group_unregister_live_locked(group);
    pthread_mutex_unlock(&group->lock);
    pthread_mutex_unlock(&g_llam_task_group_registry_lock);

    if (group->lock_initialized) {
        pthread_mutex_destroy(&group->lock);
    }
    free(group->tasks);
    free(group);
    return 0;
}
