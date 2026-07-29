/**
 * @file include/llam/runtime_driver.h
 * @brief Public LLAM contracts for host-driven runtime integration.
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

#ifndef LLAM_RUNTIME_DRIVER_H
#define LLAM_RUNTIME_DRIVER_H

#include "llam/platform.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opaque runtime handle accepted by external-driver APIs. */
typedef struct llam_runtime llam_runtime_t;

/** @brief Native object kind returned for external runtime readiness. */
typedef enum llam_runtime_readiness_kind {
    LLAM_RUNTIME_READINESS_NONE = 0,
    LLAM_RUNTIME_READINESS_FD = 1,
    LLAM_RUNTIME_READINESS_WINDOWS_HANDLE = 2,
} llam_runtime_readiness_kind_t;

/**
 * @brief Borrowed native readiness object owned by a runtime.
 *
 * Hosts may wait on @c value according to @c kind but must never close it.
 * The caller-sized public projection is populated by external-driver APIs.
 */
typedef struct llam_runtime_readiness {
    uint32_t kind;
    uint32_t reserved0;
    uintptr_t value;
} llam_runtime_readiness_t;

/** @brief Current readiness projection size for ABI-aware callers. */
#define LLAM_RUNTIME_READINESS_CURRENT_SIZE \
    ((size_t)sizeof(llam_runtime_readiness_t))

#ifdef __cplusplus
}
#endif

#endif
