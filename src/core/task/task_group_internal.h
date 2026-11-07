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
 * See LICENSE.
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

void llam_task_group_storage_free(llam_task_group_t *group);
void llam_task_group_storage_release(llam_task_group_t *group);
bool llam_task_group_signal_locked(llam_task_group_t *group);
void llam_task_group_signal(llam_task_group_t *group, bool signal);
int llam_task_group_consume_children(llam_task_group_t *group, bool has_deadline, uint64_t deadline_ns);

#if defined(LLAM_ENABLE_TEST_HOOKS)
void llam_task_group_test_spawn_hook(void (*hook)(unsigned, void *), void *argument);
#endif

#endif
