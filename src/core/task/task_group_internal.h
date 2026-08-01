/**
 * @file src/core/task/task_group_internal.h
 * @brief Private task-group registry helpers shared by task group modules.
 *
 * @copyright Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 * Licensed under the LLAM Commercial Reciprocity License 1.0.
 * See the LICENSE file distributed with this Software.
 */

#ifndef LLAM_CORE_TASK_GROUP_INTERNAL_H
#define LLAM_CORE_TASK_GROUP_INTERNAL_H

#if UINTPTR_MAX <= UINT32_MAX
#error "LLAM task-group public handles require uintptr_t wider than 32 bits"
#define LLAM_TASK_GROUP_PUBLIC_HANDLE_SHIFT 0U
#else
#define LLAM_TASK_GROUP_PUBLIC_HANDLE_SHIFT 32U
#endif

llam_task_group_t *llam_task_group_public_handle(llam_task_group_t *group);
int llam_task_group_register_live(llam_task_group_t *group);
int llam_task_group_lock_live(llam_task_group_t *handle, llam_task_group_t **out_group);

#endif
