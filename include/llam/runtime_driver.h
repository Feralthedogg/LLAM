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

/** @brief Authority that advances scheduler work for one runtime. */
typedef enum llam_runtime_driver_mode {
    LLAM_RUNTIME_DRIVER_INTERNAL = 0,
    LLAM_RUNTIME_DRIVER_EXTERNAL = 1,
} llam_runtime_driver_mode_t;

/** @brief Result of one bounded external scheduler quantum. */
typedef enum llam_runtime_drive_result {
    LLAM_RUNTIME_DRIVE_PROGRESS = 0,
    LLAM_RUNTIME_DRIVE_IDLE = 1,
    LLAM_RUNTIME_DRIVE_DONE = 2,
} llam_runtime_drive_result_t;

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

/**
 * @brief Execute at most one task segment on an externally driven runtime.
 *
 * @param runtime Runtime created with ::LLAM_RUNTIME_DRIVER_EXTERNAL.
 * @param result Receives one of ::llam_runtime_drive_result_t.
 * @return 0 on success, or -1 with @c errno set.
 */
LLAM_API int llam_runtime_drive_once(llam_runtime_t *runtime,
                                     uint32_t *result);

/**
 * @brief Read the earliest absolute scheduler deadline.
 *
 * @param runtime Externally driven runtime.
 * @param deadline_ns Receives a ::llam_now_ns timestamp or @c UINT64_MAX.
 * @return 0 on success, or -1 with @c errno set.
 */
LLAM_API int llam_runtime_next_deadline(llam_runtime_t *runtime,
                                        uint64_t *deadline_ns);

/**
 * @brief Project the runtime-owned readiness object into a caller-sized value.
 *
 * The returned native object is borrowed. The host must never close it.
 *
 * @param runtime Externally driven runtime.
 * @param readiness Caller storage receiving the known prefix.
 * @param readiness_size Bytes available at @p readiness.
 * @return 0 on success, or -1 with @c errno set.
 */
LLAM_API int llam_runtime_get_readiness(
    llam_runtime_t *runtime,
    llam_runtime_readiness_t *readiness,
    size_t readiness_size);

/**
 * @brief Make an externally driven runtime observable by its host loop.
 *
 * @param runtime Externally driven runtime.
 * @return 0 on success, or -1 with @c errno set.
 */
LLAM_API int llam_runtime_wake(llam_runtime_t *runtime);

#ifdef __cplusplus
}
#endif

#endif
