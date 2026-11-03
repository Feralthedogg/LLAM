/*
 * Copyright 2026 Feralthedogg
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

/* Keep this body unchanged for the pinned LLVM 23 Win64 scalar-leaf oracle. */
__attribute__((noinline, annotate("llam-preempt-async-safe"))) unsigned long long
llam_r8_cpu_loop_v1(unsigned long long iterations) {
    unsigned long long value = 1ULL;
    for (unsigned long long index = 0ULL; index < iterations; ++index) {
        value = ((value << 7U) ^ (value >> 3U)) + index;
    }
    return value;
}
