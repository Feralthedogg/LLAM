/**
 * @file src/core/task/task_group_internal.h
 * @brief Private task-group registry helpers shared by task group modules.
 *
 * @copyright Copyright 2026 Feralthedogg
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
 * See LICENSES/OLD-LICENSE/Apache-2.0.txt.
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
