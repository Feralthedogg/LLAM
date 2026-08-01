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
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 */

#include "runtime_internal.h"

uint64_t llam_hash_watch_identity_u64(uint64_t value) {
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31;
    return value;
}
