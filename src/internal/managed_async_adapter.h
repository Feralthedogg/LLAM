/*
 * Copyright 2026 Feralthedogg
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
 * See LICENSES/Apache-2.0.txt.
 */

#ifndef LLAM_MANAGED_ASYNC_ADAPTER_H
#define LLAM_MANAGED_ASYNC_ADAPTER_H

/* Serialized constructor preparation, after module discovery and before G
 * admission. Unsupported profiles fail closed; no native user code has run. */
int llam_managed_async_prepare(llam_runtime_t *runtime);
uint64_t llam_managed_async_effective_quantum(const llam_runtime_t *runtime, llam_task_class_t task_class);
/* Read-only readiness for a dedicated M's inline callback. Caller must own a
 * preempt-disable gate from before this query through the foreign P release;
 * the observation alone does not close native request admission. False requires
 * reopening the gate and a genuine managed return/reader drain before retry. */
bool llam_managed_async_task_quiescent(const llam_task_t *task);
#if LLAM_PLATFORM_LINUX && LLAM_ARCH_X86_64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
int llam_async_preempt_prepare_registered(llam_runtime_t *runtime);
uint64_t llam_async_preempt_effective_quantum(const llam_runtime_t *runtime, llam_task_class_t task_class);
bool llam_async_preempt_task_quiescent(const llam_task_t *task);
bool llam_async_preempt_memory_info(const llam_runtime_t *runtime, uint64_t *charged, uint64_t *per_task);
#endif

bool llam_managed_async_memory_info(const llam_runtime_t *runtime, uint64_t *charged, uint64_t *per_task);

#if defined(LLAM_ENABLE_TEST_HOOKS)
/* Negative admission oracle: STRICT/current quiescent G; -1 error, 0 reject,
 * 1 reserved and rolled back before any native mutation. */
int llam_managed_async_test_capture_admission(llam_task_t *task);
#endif

#endif /* LLAM_MANAGED_ASYNC_ADAPTER_H */
