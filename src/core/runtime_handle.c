/**
 * @file src/core/runtime_handle.c
 * @brief Explicit runtime-handle compatibility API.
 *
 * @details
 * LLAM 1.x continues to use one process-wide runtime singleton. These wrappers
 * expose a handle-shaped API for embedders and reserve ABI space for true
 * multi-runtime isolation in a later major version.
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
 * @brief Return the singleton handle accepted by 1.x handle-shaped APIs.
 *
 * @details
 * This intentionally does not allocate or retain anything.  The explicit handle
 * API is a stable embedding boundary for future migration work, while the
 * concrete 1.x implementation still aliases the process-global runtime object.
 */
llam_runtime_t *llam_runtime_default(void) {
    return &g_llam_runtime;
}

/**
 * @brief Initialize the process runtime and return its 1.x handle alias.
 *
 * @details
 * Keep the "out is NULL until fully initialized" contract explicit here: FFI
 * callers can retry or clean up safely after any validation/init failure.  The
 * EBUSY branch is the public guard that prevents embedders from accidentally
 * assuming concurrent multi-runtime isolation before the 2.x migration exists.
 */
int llam_runtime_create(const llam_runtime_opts_t *opts, size_t opts_size, llam_runtime_t **out) {
    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }
    *out = NULL;
    if (atomic_load_explicit(&g_llam_runtime.initialized, memory_order_acquire)) {
        errno = EBUSY;
        return -1;
    }
    if (llam_runtime_init_ex(opts, opts_size) != 0) {
        return -1;
    }
    *out = &g_llam_runtime;
    return 0;
}

/**
 * @brief Drive the runtime through the explicit-handle entry point.
 *
 * @details
 * The validation happens in ::llam_runtime_run_rt so run/shutdown/stats paths
 * share the same non-default-handle errno contract.
 */
int llam_runtime_run_handle(llam_runtime_t *runtime) {
    return llam_runtime_run_rt(runtime);
}

/**
 * @brief Tear down the singleton when called with the accepted 1.x handle.
 *
 * @details
 * NULL is kept as a convenience alias for legacy shutdown.  Unknown non-default
 * handles are ignored because this function has no errno channel; rejecting
 * those handles is enforced by the fallible handle APIs.
 */
void llam_runtime_destroy(llam_runtime_t *runtime) {
    if (runtime == NULL || runtime == &g_llam_runtime) {
        llam_runtime_shutdown_rt(runtime);
    }
}
