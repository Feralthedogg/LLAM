/**
 * @file src/core/runtime_handle.c
 * @brief Explicit runtime-handle lifecycle API.
 *
 * @details
 * LLAM 2.x treats explicit handles as the canonical embedding boundary.  The
 * legacy singleton APIs remain wrappers around ::llam_runtime_default, while
 * embedders can create independent heap-backed runtime objects through this
 * translation unit.
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
 * @brief Return the legacy default runtime handle.
 *
 * @details
 * This intentionally does not allocate or retain anything.  It is a stable
 * alias for the process-default runtime used by the legacy convenience APIs.
 */
llam_runtime_t *llam_runtime_default(void) {
    return llam_runtime_default_storage();
}

/**
 * @brief Allocate and initialize an explicit runtime handle.
 *
 * @details
 * Keep the "out is NULL until fully initialized" contract explicit here: FFI
 * callers can retry or clean up safely after any validation/init failure.
 */
int llam_runtime_create(const llam_runtime_opts_t *opts, size_t opts_size, llam_runtime_t **out) {
    llam_runtime_t *runtime;

    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }
    *out = NULL;

    runtime = calloc(1U, sizeof(*runtime));
    if (runtime == NULL) {
        errno = ENOMEM;
        return -1;
    }

    if (llam_runtime_init_rt(runtime, opts, opts_size, true) != 0) {
        free(runtime);
        return -1;
    }
    *out = runtime;
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
 * @brief Tear down an explicit runtime handle.
 *
 * @details
 * NULL is kept as a convenience alias for legacy shutdown.  Unknown handles are
 * ignored because this function has no errno channel; fallible handle APIs
 * reject them before touching runtime-owned state.
 */
void llam_runtime_destroy(llam_runtime_t *runtime) {
    bool heap_runtime;

    if (runtime == NULL) {
        llam_runtime_shutdown_rt(llam_runtime_default_storage());
        return;
    }
    if (llam_runtime_check_handle(runtime) != 0) {
        return;
    }

    heap_runtime = runtime != llam_runtime_default_storage();
    llam_runtime_shutdown_rt(runtime);
    if (heap_runtime) {
        free(runtime);
    }
}
