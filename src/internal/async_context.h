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

#ifndef LLAM_ASYNC_CONTEXT_H
#define LLAM_ASYNC_CONTEXT_H

#include "async_context_phase.h"

/*
 * Backend-neutral lifecycle seams. Native context formats and their lifetime
 * remain backend-owned; do not turn an unavailable adapter into a successful
 * enablement. Disabled builds inline the per-task no-ops out of the hot path.
 */
#if LLAM_ASYNC_CONTEXT_SHARED || (defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT) && \
    LLAM_ARCH_X86_64 && (LLAM_PLATFORM_LINUX || LLAM_PLATFORM_WINDOWS))
#define LLAM_ASYNC_CONTEXT_COMPILED 1
#else
#define LLAM_ASYNC_CONTEXT_COMPILED 0
#endif

#if LLAM_ASYNC_CONTEXT_COMPILED
int llam_async_context_run_begin(llam_runtime_t *runtime);
int llam_async_context_driver_ready(llam_runtime_t *runtime);
int llam_async_context_run_commit(llam_runtime_t *runtime);
void llam_async_context_run_abort(llam_runtime_t *runtime, int error);
bool llam_async_context_driver_close(llam_runtime_t *runtime);
void llam_async_context_run_end(llam_runtime_t *runtime);
int llam_async_context_task_init(llam_task_t *task);
void llam_async_context_task_destroy(llam_task_t *task);
void llam_async_context_stack_retire(llam_task_t *task);
#else
static inline int llam_async_context_run_begin(llam_runtime_t *runtime) {
    (void)runtime;
    return 0;
}
static inline int llam_async_context_driver_ready(llam_runtime_t *runtime) {
    (void)runtime;
    return 0;
}
static inline int llam_async_context_run_commit(llam_runtime_t *runtime) {
    (void)runtime;
    return 0;
}
static inline void llam_async_context_run_abort(llam_runtime_t *runtime, int error) {
    (void)runtime;
    (void)error;
}
static inline bool llam_async_context_driver_close(llam_runtime_t *runtime) {
    (void)runtime;
    return true;
}
static inline void llam_async_context_run_end(llam_runtime_t *runtime) {
    (void)runtime;
}
static inline int llam_async_context_task_init(llam_task_t *task) {
    (void)task;
    return 0;
}
static inline void llam_async_context_task_destroy(llam_task_t *task) {
    (void)task;
}
static inline void llam_async_context_stack_retire(llam_task_t *task) {
    (void)task;
}
#endif

/* Linux's disabled implementation still owns any registered range storage. */
void llam_async_context_shutdown(llam_runtime_t *runtime);

#endif
