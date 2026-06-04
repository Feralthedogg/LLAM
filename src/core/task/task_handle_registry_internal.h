/**
 * @file src/core/task/task_handle_registry_internal.h
 * @brief Shared internals for task public-handle registry modules.
 *
 * @copyright Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LLAM_CORE_TASK_HANDLE_REGISTRY_INTERNAL_H
#define LLAM_CORE_TASK_HANDLE_REGISTRY_INTERNAL_H

extern pthread_mutex_t g_llam_task_registry_lock;
extern llam_public_slot_table_t g_llam_task_public_slots;

llam_task_t *llam_task_resolve_public_handle_locked(const llam_task_t *handle);
void llam_task_invalidate_public_handle_locked(llam_task_t *task);

#endif
