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

#ifndef LLAM_TEST_ASYNC_DARWIN_LEAF_H
#define LLAM_TEST_ASYNC_DARWIN_LEAF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct llam_async_darwin_snapshot {
    uint64_t x[31];
    uint64_t sp;
    uint64_t fpcr;
    uint64_t fpsr;
    uint64_t nzcv;
    _Alignas(16) unsigned char q[32][16];
    /* Written before entering the no-call region, independently of restoration. */
    uint64_t expected_platform;
    uint64_t expected_fp;
    uint64_t expected_lr;
    uint64_t expected_sp;
} llam_async_darwin_snapshot_t;

_Static_assert(offsetof(llam_async_darwin_snapshot_t, q) == 288U, "fixture SIMD layout");
_Static_assert(offsetof(llam_async_darwin_snapshot_t, expected_platform) == 800U, "fixture entry-state layout");
_Static_assert(sizeof(llam_async_darwin_snapshot_t) == 832U, "fixture snapshot layout");

extern uint64_t llam_async_darwin_leaf(uint64_t count, llam_async_darwin_snapshot_t *out);
extern const unsigned char llam_async_darwin_leaf_begin[];
extern const unsigned char llam_async_darwin_leaf_end[];

static inline bool llam_async_darwin_snapshot_valid(
    const llam_async_darwin_snapshot_t *s, uint64_t result, uintptr_t low, uintptr_t high) {
    if (s->x[0] != 0U || s->x[1] != result ||
        s->x[18] != s->expected_platform || s->x[29] != s->expected_fp ||
        s->x[30] != s->expected_lr || s->sp != s->expected_sp ||
        s->fpcr != UINT64_C(0x800000) || s->fpsr != UINT64_C(0x8000000) ||
        s->nzcv != UINT64_C(0xa0000000) || s->sp < low || s->sp >= high) return false;
    for (unsigned i = 2U; i < 29U; ++i) {
        if (i != 18U && s->x[i] != UINT64_C(0x5100) + i) return false;
    }
    for (unsigned i = 0U; i < 32U; ++i) {
        for (unsigned j = 0U; j < 16U; ++j) {
            if (s->q[i][j] != (unsigned char)(i + 1U)) return false;
        }
    }
    return true;
}
#endif
