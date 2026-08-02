/**
 * @file src/core/task/task_handle_registry_internal.h
 * @brief Shared internals for task public-handle registry modules.
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

#ifndef LLAM_CORE_TASK_HANDLE_REGISTRY_INTERNAL_H
#define LLAM_CORE_TASK_HANDLE_REGISTRY_INTERNAL_H

extern pthread_mutex_t g_llam_task_registry_lock;
extern llam_public_slot_table_t g_llam_task_public_slots;

llam_task_t *llam_task_resolve_public_handle_locked(const llam_task_t *handle);
void llam_task_invalidate_public_handle_locked(llam_task_t *task);

#endif
