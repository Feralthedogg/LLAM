/**
 * @file src/internal/runtime_mix.h
 * @brief Shared non-cryptographic 64-bit avalanche mixer.
 *
 * @copyright Copyright 2026 Feralthedogg
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
 * See LICENSES/Apache-2.0.txt.
 */

#ifndef LLAM_RUNTIME_MIX_H
#define LLAM_RUNTIME_MIX_H

#include <stdint.h>

/**
 * @brief Apply the SplitMix64 finalizer to one 64-bit value.
 *
 * @details This is a non-cryptographic avalanche function. The algorithm was
 * published by Sebastiano Vigna and released into the public domain. See
 * THIRD_PARTY_NOTICES.md and https://prng.di.unimi.it/splitmix64.c.
 */
static inline uint64_t llam_mix64(uint64_t value) {
    value = (value ^ (value >> 30U)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27U)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31U);
}

#endif
