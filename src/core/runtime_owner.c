/**
 * @file src/core/runtime_owner.c
 * @brief Runtime-owner diagnostics for the multi-runtime migration boundary.
 *
 * @details
 * LLAM 1.x still exposes one live scheduler singleton, but runtime-aware
 * objects now record the runtime that created them.  These helpers provide a
 * cheap owner check for sync, task, cancellation, and I/O objects so the 1.2.x
 * line can catch accidental cross-runtime use before the 2.x multi-runtime
 * implementation removes the singleton restriction.  Keep the public-object
 * check singleton-fast in 1.x; it is called from channel/select hot paths.
 *
 * @copyright Copyright 2026 Feralthedogg
 *
 * @par License
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "runtime_internal.h"

/**
 * @brief Validate a runtime handle accepted by the 1.x singleton bridge.
 *
 * LLAM 1.x has a handle-shaped API for embedders, but the only runnable handle
 * is still the default singleton.  Keep this validation centralized so every
 * runtime-owned wrapper fails non-default handles with the same public errno.
 */
int llam_runtime_check_handle(const llam_runtime_t *runtime) {
    if (LLAM_UNLIKELY(runtime != &g_llam_runtime)) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

/**
 * @brief Return the runtime associated with the current managed context.
 *
 * The shard is the most reliable owner cursor while a worker is running.  The
 * task field is a fallback for direct task-to-task handoff windows where the
 * task cursor has been installed before every helper has sampled the shard.
 */
llam_runtime_t *llam_runtime_current_owner(void) {
    if (LLAM_LIKELY(g_llam_tls_shard != NULL && g_llam_tls_shard->runtime != NULL)) {
        return g_llam_tls_shard->runtime;
    }
    if (LLAM_UNLIKELY(g_llam_tls_task != NULL && g_llam_tls_task->owner_runtime != NULL)) {
        return g_llam_tls_task->owner_runtime;
    }
    return &g_llam_runtime;
}

/**
 * @brief Return the owner to stamp onto a newly created runtime-aware object.
 */
llam_runtime_t *llam_runtime_owner_for_new_object(void) {
    /*
     * 1.x allows only the process-default runtime to create public objects.
     * Avoid sampling TLS here so object creation and cache reuse do not pay for
     * a migration hook that cannot select any other live runtime yet.
     */
    return &g_llam_runtime;
}

/**
 * @brief Validate that an object belongs to the current runtime context.
 *
 * Because 1.x has exactly one live runtime, a public object's owner is valid
 * iff it is the process-default runtime.  That preserves deterministic
 * @c EXDEV diagnostics for forged or stale future-runtime owners without
 * sampling TLS on every channel, select, mutex, or join operation.
 */
int llam_runtime_check_object_owner(const llam_runtime_t *owner_runtime) {
#if !LLAM_RUNTIME_DISABLE_OWNER_CHECKS
    if (LLAM_LIKELY(owner_runtime == &g_llam_runtime)) {
        return 0;
    }
    /*
     * Keep the valid singleton path to one predicted branch.  The cold error
     * side still distinguishes null handles from future-runtime owner mismatch
     * so embedders get stable diagnostics without another hot-path compare.
     */
    errno = owner_runtime == NULL ? EINVAL : EXDEV;
    return -1;
#else
    if (LLAM_UNLIKELY(owner_runtime == NULL)) {
        errno = EINVAL;
        return -1;
    }
    (void)owner_runtime;
#endif
    return 0;
}

/**
 * @brief Require both managed-task context and matching object runtime owner.
 */
int llam_runtime_require_object_owner(const llam_runtime_t *owner_runtime) {
    if (llam_require_task_context() != 0) {
        return -1;
    }
    return llam_runtime_check_object_owner(owner_runtime);
}
