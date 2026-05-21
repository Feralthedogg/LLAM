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

llam_runtime_t *llam_runtime_default(void) {
    return &g_llam_runtime;
}

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

int llam_runtime_run_handle(llam_runtime_t *runtime) {
    return llam_runtime_run_rt(runtime);
}

void llam_runtime_destroy(llam_runtime_t *runtime) {
    if (runtime == NULL || runtime == &g_llam_runtime) {
        llam_runtime_shutdown_rt(runtime);
    }
}
