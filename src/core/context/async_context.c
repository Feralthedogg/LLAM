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

#include "runtime_internal.h"
#if LLAM_PLATFORM_WINDOWS && LLAM_ASYNC_CONTEXT_COMPILED
#include "windows_async_lifetime.h"
#endif

#if LLAM_ASYNC_CONTEXT_COMPILED
int llam_async_context_run_begin(llam_runtime_t *runtime) {
#if LLAM_PLATFORM_WINDOWS
    return llam_windows_async_lifetime_run_begin(runtime);
#elif LLAM_ASYNC_CONTEXT_SHARED
    return llam_async_shared_run_begin(runtime);
#else
    return llam_async_preempt_run_begin(runtime);
#endif
}

int llam_async_context_driver_ready(llam_runtime_t *runtime) {
#if LLAM_PLATFORM_WINDOWS
    return llam_windows_async_lifetime_register_worker(&runtime->shards[0]);
#elif LLAM_ASYNC_CONTEXT_SHARED
    return llam_async_shared_driver_ready(runtime);
#else
    (void)runtime;
    return 0; /* Signal backends qualify after installing each M's altstack. */
#endif
}

int llam_async_context_run_commit(llam_runtime_t *runtime) {
#if LLAM_PLATFORM_WINDOWS
    return llam_windows_async_lifetime_run_commit(runtime);
#elif LLAM_ASYNC_CONTEXT_SHARED
    return llam_async_shared_run_commit(runtime);
#else
    (void)runtime;
    return 0; /* Last successfully probed M opens the signal startup barrier. */
#endif
}

void llam_async_context_run_abort(llam_runtime_t *runtime, int error) {
#if LLAM_PLATFORM_WINDOWS
    llam_windows_async_lifetime_run_abort(runtime, error);
#elif LLAM_ASYNC_CONTEXT_SHARED
    llam_async_shared_run_abort(runtime, error);
#else
    llam_async_preempt_run_abort(runtime, error, LLAM_PREEMPT_FAILURE_WORKER_START);
#endif
}

bool llam_async_context_driver_close(llam_runtime_t *runtime) {
#if LLAM_PLATFORM_WINDOWS
    return llam_windows_async_lifetime_close_binding(&runtime->shards[0]);
#elif LLAM_ASYNC_CONTEXT_SHARED
    return llam_async_shared_driver_close(runtime);
#else
    (void)runtime;
    return true; /* Signal unbind/drain remains on its owning native M. */
#endif
}

void llam_async_context_run_end(llam_runtime_t *runtime) {
#if LLAM_PLATFORM_WINDOWS
    llam_windows_async_lifetime_run_end(runtime);
#elif LLAM_ASYNC_CONTEXT_SHARED
    llam_async_shared_run_end(runtime);
#else
    llam_async_preempt_run_end(runtime);
#endif
}

int llam_async_context_task_init(llam_task_t *task) {
#if LLAM_PLATFORM_WINDOWS
    return llam_windows_async_lifetime_task_init(task);
#elif LLAM_ASYNC_CONTEXT_SHARED
    return llam_async_shared_task_init(task);
#else
    return llam_async_preempt_task_init(task);
#endif
}

void llam_async_context_task_destroy(llam_task_t *task) {
#if LLAM_PLATFORM_WINDOWS
    llam_windows_async_lifetime_task_destroy(task);
#elif LLAM_ASYNC_CONTEXT_SHARED
    llam_async_shared_task_destroy(task);
#else
    llam_async_preempt_task_destroy(task);
#endif
}

void llam_async_context_stack_retire(llam_task_t *task) {
#if LLAM_PLATFORM_WINDOWS || LLAM_ASYNC_CONTEXT_SHARED
    /* Only the complete DEAD-retirement wrapper may discharge remote readers. */
    if (task->async_preempt_task != NULL) {
        abort();
    }
#else
    llam_async_preempt_task_destroy(task);
#endif
}
#endif

void llam_async_context_shutdown(llam_runtime_t *runtime) {
#if LLAM_PLATFORM_LINUX && LLAM_ARCH_X86_64
    llam_async_preempt_shutdown_runtime(runtime);
#elif LLAM_PLATFORM_WINDOWS && LLAM_ASYNC_CONTEXT_COMPILED
    llam_windows_async_lifetime_shutdown(runtime);
#elif LLAM_ASYNC_CONTEXT_SHARED
    llam_async_shared_shutdown(runtime);
#else
    (void)runtime;
#endif
}
