/**
 * @file src/internal/runtime_windows.h
 * @brief Internal Windows 10/11 generation detection and IOCP policy helpers.
 *
 * @details
 * Windows 10 and Windows 11 share the same core IOCP API surface. LLAM treats
 * the OS generation as a tuning input instead of a different correctness
 * contract: Windows 11 starts at NT 10.0 build 22000, while Windows 10 is NT
 * 10.0 with a lower build number.
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

#ifndef LLAM_RUNTIME_WINDOWS_H
#define LLAM_RUNTIME_WINDOWS_H

#include <stdint.h>

/** @brief First NT 10.0 build reported as Windows 11. */
#define LLAM_WINDOWS_11_MIN_BUILD 22000U

/** @brief Windows generation known to the native IOCP backend. */
typedef enum llam_windows_generation {
    LLAM_WINDOWS_GENERATION_UNSUPPORTED = 0,
    LLAM_WINDOWS_GENERATION_10 = 10,
    LLAM_WINDOWS_GENERATION_11 = 11,
} llam_windows_generation_t;

/** @brief Internal I/O completion strategy selected per Windows generation. */
typedef enum llam_windows_iocp_strategy {
    LLAM_WINDOWS_IOCP_STRATEGY_UNSUPPORTED = 0,
    LLAM_WINDOWS_IOCP_STRATEGY_WIN10_CONSERVATIVE = 10,
    LLAM_WINDOWS_IOCP_STRATEGY_WIN11_BATCHED = 11,
} llam_windows_iocp_strategy_t;

/** @brief IOCP tuning selected from Windows generation and processor count. */
typedef struct llam_windows_iocp_policy {
    uint32_t generation;
    uint32_t strategy;
    uint32_t major;
    uint32_t minor;
    uint32_t build;
    uint32_t processor_count;
    uint32_t iocp_concurrency;
    uint32_t completion_batch;
    uint32_t control_batch;
    uint32_t accept_prepost;
    uint32_t recv_prepost;
    uint32_t idle_spin_iters;
    uint32_t poll_timeout_ms;
    uint32_t timer_granularity_ms;
    uint32_t use_gqcs_ex;
    uint32_t use_skip_completion_on_success;
} llam_windows_iocp_policy_t;

uint32_t llam_windows_generation_from_version(uint32_t major, uint32_t minor, uint32_t build);
const char *llam_windows_generation_name(uint32_t generation);
const char *llam_windows_iocp_strategy_name(uint32_t strategy);
int llam_windows_detect_iocp_policy(llam_windows_iocp_policy_t *policy);
void llam_windows_iocp_policy_for_windows10(uint32_t major,
                                            uint32_t minor,
                                            uint32_t build,
                                            uint32_t processor_count,
                                            llam_windows_iocp_policy_t *policy);
void llam_windows_iocp_policy_for_windows11(uint32_t major,
                                            uint32_t minor,
                                            uint32_t build,
                                            uint32_t processor_count,
                                            llam_windows_iocp_policy_t *policy);
void llam_windows_default_iocp_policy(uint32_t generation,
                                      uint32_t major,
                                      uint32_t minor,
                                      uint32_t build,
                                      uint32_t processor_count,
                                      llam_windows_iocp_policy_t *policy);

#endif
