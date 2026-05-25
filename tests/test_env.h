/**
 * @file tests/test_env.h
 * @brief Shared environment parsing helpers for LLAM C tests.
 *
 * @details
 * Randomized and invariant tests use environment variables to scale scenario
 * counts under CI. These helpers intentionally ignore malformed, zero, or empty
 * values so accidental shell input cannot turn a bounded test into an
 * unbounded run.
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

#ifndef LLAM_TEST_ENV_H
#define LLAM_TEST_ENV_H

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

static inline unsigned llam_test_env_u32(const char *name, unsigned default_value, unsigned max_value) {
    const char *raw = getenv(name);
    char *end = NULL;
    unsigned long parsed;

    if (raw == NULL || *raw == '\0') {
        return default_value;
    }
    errno = 0;
    parsed = strtoul(raw, &end, 10);
    if (errno != 0 || end == raw || *end != '\0' || parsed == 0UL) {
        return default_value;
    }
    if (parsed > max_value) {
        parsed = max_value;
    }
    return (unsigned)parsed;
}

static inline uint64_t llam_test_env_u64(const char *name, uint64_t default_value) {
    const char *raw = getenv(name);
    char *end = NULL;
    unsigned long long parsed;

    if (raw == NULL || *raw == '\0') {
        return default_value;
    }
    errno = 0;
    parsed = strtoull(raw, &end, 10);
    if (errno != 0 || end == raw || *end != '\0' || parsed == 0ULL) {
        return default_value;
    }
    return (uint64_t)parsed;
}

#endif
