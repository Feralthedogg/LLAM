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
 * See LICENSE.
 */

#include "runtime_internal.h"
#include "managed_catalog.h"
#include "managed_async_adapter.h"
#if LLAM_PLATFORM_WINDOWS && LLAM_ASYNC_CONTEXT_COMPILED
#include "windows_async_lifetime.h"
#endif

int llam_managed_async_prepare(llam_runtime_t *runtime) {
    if (runtime == NULL) { errno = EINVAL; return -1; }
    if (llam_managed_catalog_policy(runtime) == LLAM_PREEMPT_POLICY_COOPERATIVE ||
        runtime->preempt_mode < LLAM_PREEMPT_AUTO) return 0;
#if LLAM_PLATFORM_LINUX && defined(LLAM_TSAN_BUILD)
    /* Signal-context copying by TSan is incompatible with native redirection
     * on either Linux architecture. Explicit cooperative catalogs still work. */
    errno = ENOTSUP;
    return -1;
#endif
#if LLAM_ASYNC_CONTEXT_SHARED
    return llam_async_shared_prepare_registered(runtime);
#elif LLAM_PLATFORM_LINUX && LLAM_ARCH_X86_64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
    return llam_async_preempt_prepare_registered(runtime);
#elif LLAM_PLATFORM_WINDOWS && LLAM_ASYNC_CONTEXT_COMPILED
    return llam_windows_async_prepare_registered(runtime);
#else
    errno = ENOTSUP;
    return -1;
#endif
}

bool llam_managed_async_memory_info(const llam_runtime_t *runtime, uint64_t *charged, uint64_t *per_task) {
#if LLAM_ASYNC_CONTEXT_SHARED
    return llam_async_shared_memory_info(runtime, charged, per_task);
#elif LLAM_PLATFORM_LINUX && LLAM_ARCH_X86_64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
    return llam_async_preempt_memory_info(runtime, charged, per_task);
#elif LLAM_PLATFORM_WINDOWS && LLAM_ASYNC_CONTEXT_COMPILED
    return llam_windows_async_memory_info(runtime, charged, per_task);
#else
    (void)runtime;
    (void)charged;
    (void)per_task;
    return false;
#endif
}

bool llam_managed_async_task_quiescent(const llam_task_t *task) {
    if (task == NULL) return false;
    if (atomic_load_explicit(&task->owner_runtime->async_preempt_enabled, memory_order_acquire) == 0U)
        return true;
    if (g_llam_tls_task != task || g_llam_tls_worker == NULL ||
        atomic_load_explicit(&g_llam_tls_worker->preempt_disable_depth, memory_order_acquire) != 1U)
        return false;
#if LLAM_ASYNC_CONTEXT_SHARED
    return llam_async_shared_task_quiescent(task);
#elif LLAM_PLATFORM_LINUX && LLAM_ARCH_X86_64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
    return llam_async_preempt_task_quiescent(task);
#elif LLAM_PLATFORM_WINDOWS && LLAM_ASYNC_CONTEXT_COMPILED
    return llam_windows_async_task_quiescent(task);
#else
    /* Each enabled native backend must prove both G banks and M service quiet. */
    return false;
#endif
}

uint64_t llam_managed_async_effective_quantum(const llam_runtime_t *runtime,
                                             llam_task_class_t task_class) {
    if (runtime == NULL) return 0U;
#if LLAM_ASYNC_CONTEXT_SHARED
    (void)task_class;
    return llam_async_shared_effective_quantum(runtime);
#elif LLAM_PLATFORM_LINUX && LLAM_ARCH_X86_64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
    return llam_async_preempt_effective_quantum(runtime, task_class);
#elif LLAM_PLATFORM_WINDOWS && LLAM_ASYNC_CONTEXT_COMPILED
    (void)task_class;
    return llam_windows_async_effective_quantum(runtime);
#else
    (void)task_class;
    return 0U;
#endif
}

#if defined(LLAM_ENABLE_TEST_HOOKS)
int llam_managed_async_test_capture_admission(llam_task_t *task) {
#if LLAM_ASYNC_CONTEXT_SHARED
    return llam_async_shared_test_capture_admission(task);
#else
    (void)task;
    errno = ENOTSUP;
    return -1;
#endif
}
#endif
