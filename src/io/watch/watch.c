/**
 * @file src/io/watch/watch.c
 * @brief Shared watch-object lifecycle glue used by platform I/O backends.
 *
 * @details
 * Platform-specific watch behavior lives in the Linux and Darwin backend files.
 * This file intentionally stays small as the common watch compilation unit for
 * builds that include the watch subsystem.
 *
 * @copyright Copyright 2026 Feralthedogg
 *
 * @par License
 * SPDX-License-Identifier: Apache-2.0
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * See LICENSE.
 */

#include "runtime_internal.h"

uint64_t llam_hash_watch_identity(uint64_t device,
                                  uint64_t inode,
                                  uint64_t descriptor) {
    uint64_t hash;

    /* Preserve field order so device/inode swaps cannot cancel through XOR. */
    hash = llam_mix64(device ^ UINT64_C(0x6465766963650001));
    hash = llam_mix64(hash ^ inode ^ UINT64_C(0x696e6f6465000002));
    return llam_mix64(hash ^ descriptor ^ UINT64_C(0x6664000000000003));
}
