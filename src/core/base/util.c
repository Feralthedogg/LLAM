/**
 * @file src/core/base/util.c
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

static int llam_ascii_tolower(int ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return ch + ('a' - 'A');
    }
    return ch;
}

bool llam_ascii_is_space(int ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

static bool llam_ascii_equal_ci(const char *lhs, const char *rhs) {
    while (*lhs != '\0' && *rhs != '\0') {
        if (llam_ascii_tolower((unsigned char)*lhs) != llam_ascii_tolower((unsigned char)*rhs)) {
            return false;
        }
        ++lhs;
        ++rhs;
    }
    return *lhs == '\0' && *rhs == '\0';
}

/**
 * @brief Parse a boolean-like environment value using explicit tokens.
 *
 * @details
 * The runtime accepts conventional true/false strings instead of treating any
 * non-zero text as enabled. That prevents mistakes such as
 * @c LLAM_EXPERIMENTAL_SQPOLL=false from silently enabling an experimental
 * path. Unknown values preserve @p default_value so future policy sentinels
 * such as "auto" can be represented by callers without an extra parse pass.
 */
unsigned llam_env_flag_value(const char *value, unsigned default_value) {
    if (value == NULL || value[0] == '\0') {
        return default_value;
    }
    if (llam_ascii_equal_ci(value, "1") ||
        llam_ascii_equal_ci(value, "true") ||
        llam_ascii_equal_ci(value, "yes") ||
        llam_ascii_equal_ci(value, "on")) {
        return 1U;
    }
    if (llam_ascii_equal_ci(value, "0") ||
        llam_ascii_equal_ci(value, "false") ||
        llam_ascii_equal_ci(value, "no") ||
        llam_ascii_equal_ci(value, "off")) {
        return 0U;
    }
    return default_value;
}

/**
 * @brief Read and parse a boolean-like runtime environment variable.
 */
unsigned llam_env_flag(const char *name, unsigned default_value) {
    return llam_env_flag_value(llam_env_get(name), default_value);
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
 * @brief Checked round-up to the next alignment boundary.
 *
 * @details
 * Alignment is often used before allocator or backend-sized memory operations.
 * Overflow must fail closed; wrapping to a small allocation size would turn a
 * large request into a later buffer overrun.
 */
int llam_align_up_checked(size_t value, size_t alignment, size_t *out_value) {
    size_t mask = alignment - 1U;

    if (out_value == NULL || alignment == 0U || (alignment & mask) != 0U) {
        errno = EINVAL;
        return -1;
    }
    if (value > SIZE_MAX - mask) {
        errno = ENOMEM;
        return -1;
    }
    *out_value = (value + mask) & ~mask;
    return 0;
}

/**
 * @brief Round a value up to the next alignment boundary.
 *
 * @param value     Value to align.
 * @param alignment Power-of-two alignment.
 *
 * @return @p value rounded up to @p alignment, or 0 on invalid/overflowing
 *         input with @c errno set.
 */
size_t llam_align_up(size_t value, size_t alignment) {
    size_t aligned = 0U;

    if (llam_align_up_checked(value, alignment, &aligned) != 0) {
        return 0U;
    }
    return aligned;
}
