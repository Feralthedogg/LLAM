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

/* Isolated compiler oracle. These counters test generated domain transitions;
 * they deliberately do not pretend to be the runtime's native-domain guard. */
typedef unsigned long long u64;
static unsigned enters;
static unsigned leaves;
static unsigned depth;
static unsigned invalid;

void llam_managed_native_enter(void) { ++enters; ++depth; }
void llam_managed_native_leave(void) {
    ++leaves;
    if (depth == 0U) invalid = 1U;
    else --depth;
}

extern u64 managed_array(const u64 *, u64);
extern u64 managed_stack(unsigned);
extern u64 managed_recursive(unsigned);
extern u64 managed_atomic(u64 *, u64);
extern u64 managed_tls(u64);
extern u64 managed_elapsed_lag(u64, u64);
extern int managed_signed_byte(int);
extern long long managed_signed_byte_wide(int);
extern int managed_signed_half(int);
extern long long managed_signed_half_wide(int);
extern long long managed_signed_word(int);
extern unsigned managed_unsigned_byte(unsigned);
extern unsigned managed_unsigned_half(unsigned);

static int narrow_stack_loads_match(void) {
    const int bytes[] = {-128, -1, 0, 127};
    const int halves[] = {-32768, -1, 0, 32767};
    const int words[] = {-2147483647 - 1, -1, 0, 2147483647};
    const unsigned unsigned_bytes[] = {0U, 127U, 128U, 255U};
    const unsigned unsigned_halves[] = {0U, 32767U, 32768U, 65535U};
    for (unsigned i = 0U; i < sizeof(bytes) / sizeof(bytes[0]); ++i) {
        if (managed_signed_byte(bytes[i]) != bytes[i] ||
            managed_signed_byte_wide(bytes[i]) != bytes[i]) return 0;
    }
    for (unsigned i = 0U; i < sizeof(halves) / sizeof(halves[0]); ++i) {
        if (managed_signed_half(halves[i]) != halves[i] ||
            managed_signed_half_wide(halves[i]) != halves[i]) return 0;
    }
    for (unsigned i = 0U; i < sizeof(words) / sizeof(words[0]); ++i) {
        if (managed_signed_word(words[i]) != words[i]) return 0;
    }
    for (unsigned i = 0U; i < sizeof(unsigned_bytes) / sizeof(unsigned_bytes[0]); ++i) {
        if (managed_unsigned_byte(unsigned_bytes[i]) != unsigned_bytes[i]) return 0;
    }
    for (unsigned i = 0U; i < sizeof(unsigned_halves) / sizeof(unsigned_halves[0]); ++i) {
        if (managed_unsigned_half(unsigned_halves[i]) != unsigned_halves[i]) return 0;
    }
    return 1;
}

int main(void) {
    const u64 values[] = {1U, 2U, 3U, 4U, 5U};
    unsigned before = enters;
    if (managed_array(values, 5U) != ((15U * 17U) ^ (15U >> 3U))) return 1;
    /* A managed-to-managed helper call must bypass the native thunk. */
    if (enters != before + 1U) return 2;
    if (managed_stack(400U) != 3U * 399U * 400U / 2U) return 3;
    before = enters;
    if (managed_recursive(12U) != 479001600U || enters != before + 1U) return 4;
    u64 value = 0U;
    if (managed_atomic(&value, 100U) != 4950U || value != 100U) return 5;
    if (managed_tls(7U) != 7U || managed_tls(2U) != 9U) return 6;
    if (managed_elapsed_lag(3U, 7U) != 0U || managed_elapsed_lag(7U, 7U) != 0U ||
        managed_elapsed_lag(11U, 7U) != 4U || managed_elapsed_lag(~(u64)0U, 1U) != ~(u64)1U)
        return 8;
    if (!narrow_stack_loads_match()) return 9;
    return invalid || depth != 0U || enters != leaves ? 7 : 0;
}
