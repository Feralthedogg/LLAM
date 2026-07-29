/**
 * @file src/internal/runtime_external_driver.h
 * @brief Private state for runtime integration with an external host loop.
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

#ifndef LLAM_RUNTIME_EXTERNAL_DRIVER_H
#define LLAM_RUNTIME_EXTERNAL_DRIVER_H

#include "llam/platform.h"

#include <stdatomic.h>
#include <stdbool.h>

/** @brief Runtime-owned native readiness object for external host loops. */
typedef struct llam_external_doorbell {
    atomic_uint pending;
#if LLAM_PLATFORM_WINDOWS
    void *handle;
#else
    int read_fd;
    int write_fd;
#endif
    bool initialized;
} llam_external_doorbell_t;

/** @brief State kept out of the central runtime layout definition. */
typedef struct llam_external_driver_state {
    llam_external_doorbell_t doorbell;
    bool enabled;
} llam_external_driver_state_t;

#endif
