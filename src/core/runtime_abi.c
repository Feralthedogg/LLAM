/**
 * @file src/core/runtime_abi.c
 * @brief Public ABI/version metadata entry points for dynamic LLAM loaders.
 *
 * @details
 * Language runtimes and FFI bindings should resolve the tiny ABI surface before
 * binding the rest of the API. The info structs use a caller-size handshake so
 * additive fields remain binary-compatible across minor ABI revisions.
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

#include "llam/runtime.h"

#include "runtime_internal.h"

#include <errno.h>
#include <string.h>

#define LLAM_VERSION_STRING_LITERAL "1.1.0"

/** @brief Return the smaller of two byte counts. */
static size_t llam_min_size(size_t a, size_t b) {
    return a < b ? a : b;
}

uint32_t llam_abi_version(void) {
    return LLAM_ABI_VERSION;
}

const char *llam_version_string(void) {
    return LLAM_VERSION_STRING_LITERAL;
}

int llam_runtime_opts_init(llam_runtime_opts_t *opts, size_t opts_size) {
    llam_runtime_opts_t defaults;
    size_t copy_size;

    if (opts == NULL || opts_size == 0U) {
        errno = EINVAL;
        return -1;
    }

    memset(&defaults, 0, sizeof(defaults));
    defaults.deterministic = 1U;
    defaults.sqpoll_cpu = -1;
    defaults.profile = LLAM_RUNTIME_PROFILE_BALANCED;
    defaults.preempt_mode = LLAM_PREEMPT_AUTO;

    memset(opts, 0, opts_size);
    copy_size = llam_min_size(opts_size, sizeof(defaults));
    memcpy(opts, &defaults, copy_size);
    return 0;
}

int llam_spawn_opts_init(llam_spawn_opts_t *opts, size_t opts_size) {
    llam_spawn_opts_t defaults;
    size_t copy_size;

    if (opts == NULL || opts_size == 0U) {
        errno = EINVAL;
        return -1;
    }

    memset(&defaults, 0, sizeof(defaults));
    defaults.task_class = LLAM_TASK_CLASS_DEFAULT;
    defaults.stack_class = LLAM_STACK_CLASS_DEFAULT;

    memset(opts, 0, opts_size);
    copy_size = llam_min_size(opts_size, sizeof(defaults));
    memcpy(opts, &defaults, copy_size);
    return 0;
}

int llam_abi_get_info(llam_abi_info_t *info, size_t info_size) {
    llam_abi_info_t current;
    size_t copy_size;

    if (info == NULL || info_size == 0U) {
        errno = EINVAL;
        return -1;
    }

    memset(&current, 0, sizeof(current));
    current.abi_major = LLAM_ABI_VERSION_MAJOR;
    current.abi_minor = LLAM_ABI_VERSION_MINOR;
    current.version_major = LLAM_VERSION_MAJOR;
    current.version_minor = LLAM_VERSION_MINOR;
    current.version_patch = LLAM_VERSION_PATCH;
    current.struct_size = sizeof(llam_abi_info_t);
    current.runtime_opts_size = sizeof(llam_runtime_opts_t);
    current.spawn_opts_size = sizeof(llam_spawn_opts_t);
    current.runtime_stats_size = sizeof(llam_runtime_stats_t);
    current.runtime_name = "LLAM";
    current.version_string = LLAM_VERSION_STRING_LITERAL;
    current.platform_name = LLAM_PLATFORM_NAME;

    memset(info, 0, info_size);
    copy_size = llam_min_size(info_size, sizeof(current));
    memcpy(info, &current, copy_size);
    return 0;
}
