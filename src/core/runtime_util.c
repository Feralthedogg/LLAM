/**
 * @file src/core/runtime_util.c
 * @brief Small shared utility helpers used across the runtime.
 *
 * @details
 * These helpers are intentionally dependency-light so low-level runtime modules
 * can use them without creating subsystem cycles.
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Read a runtime environment variable with LLAM/NM compatibility aliases.
 *
 * The project was renamed from @c NM_* to @c LLAM_* public branding. This
 * helper checks the requested name first, then known renamed options, then the
 * equivalent prefix for compatibility.
 *
 * @param name Environment variable name.
 *
 * @return Environment value, or @c NULL when unset or when alias construction
 *         would overflow the local buffer.
 */
const char *llam_env_get(const char *name) {
    char compat_name[128];
    const char *value;

    if (name == NULL) {
        return NULL;
    }
    if (strncmp(name, "LLAM_", 5U) == 0) {
        value = getenv(name);
        if (value != NULL) {
            return value;
        }
        if (strcmp(name, "LLAM_EXPERIMENTAL_WORKER_RINGS") == 0) {
            value = getenv("LLAM_EXPERIMENTAL_SHARD_RINGS");
            if (value == NULL) {
                value = getenv("NM_EXPERIMENTAL_SHARD_RINGS");
            }
            if (value != NULL) {
                return value;
            }
        } else if (strcmp(name, "LLAM_EXPERIMENTAL_WORKER_RINGS_MULTISHOT") == 0) {
            value = getenv("LLAM_EXPERIMENTAL_SHARD_RINGS_MULTISHOT");
            if (value == NULL) {
                value = getenv("NM_EXPERIMENTAL_SHARD_RINGS_MULTISHOT");
            }
            if (value != NULL) {
                return value;
            }
        } else if (strcmp(name, "LLAM_EXPERIMENTAL_DYNAMIC_WORKERS") == 0) {
            value = getenv("LLAM_EXPERIMENTAL_DYNAMIC_SHARDS");
            if (value == NULL) {
                value = getenv("NM_EXPERIMENTAL_DYNAMIC_SHARDS");
            }
            if (value != NULL) {
                return value;
            }
        }
        // Generic LLAM_FOO -> NM_FOO fallback for old scripts.
        if (snprintf(compat_name, sizeof(compat_name), "NM_%s", name + 5) >= (int)sizeof(compat_name)) {
            return NULL;
        }
        return getenv(compat_name);
    }
    if (strncmp(name, "NM_", 3U) == 0) {
        // Generic NM_FOO -> LLAM_FOO lookup lets internal code accept old names.
        if (snprintf(compat_name, sizeof(compat_name), "LLAM_%s", name + 3) < (int)sizeof(compat_name)) {
            value = getenv(compat_name);
            if (value != NULL) {
                return value;
            }
        }
    }
    return getenv(name);
}

/**
 * @brief Atomically update a peak counter if @p value is larger.
 *
 * @param peak  Atomic peak counter.
 * @param value Candidate value.
 */
void llam_atomic_update_peak(atomic_uint *peak, unsigned value) {
    unsigned current = atomic_load(peak);

    while (current < value && !atomic_compare_exchange_weak(peak, &current, value)) {
    }
}

/**
 * @brief Return the larger of two unsigned values.
 *
 * @param a First value.
 * @param b Second value.
 *
 * @return Maximum of @p a and @p b.
 */
unsigned llam_max_unsigned(unsigned a, unsigned b) {
    return a > b ? a : b;
}

/**
 * @brief Round a value up to the next alignment boundary.
 *
 * @param value     Value to align.
 * @param alignment Power-of-two alignment.
 *
 * @return @p value rounded up to @p alignment.
 *
 * @note Callers must pass a non-zero power-of-two alignment.
 */
size_t llam_align_up(size_t value, size_t alignment) {
    size_t mask = alignment - 1U;

    return (value + mask) & ~mask;
}
