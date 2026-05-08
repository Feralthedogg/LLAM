/**
 * @file src/internal/runtime_windows_iocp.h
 * @brief Internal IOCP primitive wrappers for the native Windows backend.
 *
 * @details
 * This layer intentionally does not depend on the scheduler or node structs.
 * It gives the Windows backend a small, testable surface for creating an IOCP,
 * posting control packets, and draining completion batches before those pieces
 * are wired into the full runtime I/O node implementation.
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

#ifndef LLAM_RUNTIME_WINDOWS_IOCP_H
#define LLAM_RUNTIME_WINDOWS_IOCP_H

#include "runtime_windows.h"

#include <stddef.h>
#include <stdint.h>

#define LLAM_WINDOWS_IOCP_WAKE_KEY UINTPTR_MAX

typedef struct llam_windows_iocp_completion {
    uintptr_t key;
    uintptr_t overlapped;
    uint32_t bytes;
    uint32_t error_code;
} llam_windows_iocp_completion_t;

int llam_windows_iocp_create(const llam_windows_iocp_policy_t *policy, void **handle_out);
void llam_windows_iocp_close(void *handle);
int llam_windows_iocp_post(void *handle, uintptr_t key, uintptr_t overlapped, uint32_t bytes);
int llam_windows_iocp_drain(void *handle,
                            llam_windows_iocp_completion_t *entries,
                            size_t entry_count,
                            uint32_t timeout_ms,
                            size_t *count_out);

#endif
