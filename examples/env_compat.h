/**
 * @file examples/env_compat.h
 * @brief Environment-variable compatibility helper shared by examples and benchmarks.
 *
 * @details
 * Examples accept current LLAM_* names first, then old NM_* aliases. Keeping
 * that fallback here avoids stale automation breakage without leaking the old
 * prefix into new user-facing documentation.
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

#ifndef LLAM_EXAMPLES_ENV_COMPAT_H
#define LLAM_EXAMPLES_ENV_COMPAT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline const char *llam_example_env_get(const char *name) {
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
        if (snprintf(compat_name, sizeof(compat_name), "NM_%s", name + 5) >= (int)sizeof(compat_name)) {
            return NULL;
        }
        return getenv(compat_name);
    }
    if (strncmp(name, "NM_", 3U) == 0) {
        if (snprintf(compat_name, sizeof(compat_name), "LLAM_%s", name + 3) < (int)sizeof(compat_name)) {
            value = getenv(compat_name);
            if (value != NULL) {
                return value;
            }
        }
    }
    return getenv(name);
}

#endif
