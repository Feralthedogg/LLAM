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

typedef unsigned long long u64;

__attribute__((noinline)) static u64 managed_mix(u64 n) {
    return (n * 17U) ^ (n >> 3U);
}

u64 managed_array(const u64 *values, u64 count) {
    u64 sum = 0U;
    for (u64 i = 0U; i < count; ++i) sum += values[i];
    return managed_mix(sum);
}

u64 managed_stack(unsigned count) {
    volatile u64 values[512];
    count %= 512U;
    for (unsigned i = 0U; i < count; ++i) values[i] = (u64)i * 3U;
    u64 sum = 0U;
    for (unsigned i = 0U; i < count; ++i) sum += values[i];
    return sum;
}

int managed_signed_byte(int value) {
    volatile signed char local = (signed char)value;
    return local;
}

long long managed_signed_byte_wide(int value) {
    volatile signed char local = (signed char)value;
    return local;
}

int managed_signed_half(int value) {
    volatile short local = (short)value;
    return local;
}

long long managed_signed_half_wide(int value) {
    volatile short local = (short)value;
    return local;
}

long long managed_signed_word(int value) {
    volatile int local = value;
    return local;
}

unsigned managed_unsigned_byte(unsigned value) {
    volatile unsigned char local = (unsigned char)value;
    return local;
}

unsigned managed_unsigned_half(unsigned value) {
    volatile unsigned short local = (unsigned short)value;
    return local;
}

__attribute__((noinline)) u64 managed_recursive(unsigned count) {
    if (count == 0U) return 1U;
    return count * managed_recursive(count - 1U);
}

u64 managed_atomic(u64 *value, u64 count) {
    u64 sum = 0U;
    for (u64 i = 0U; i < count; ++i) sum += __atomic_fetch_add(value, 1U, __ATOMIC_ACQ_REL);
    return sum;
}

u64 managed_elapsed_lag(u64 elapsed, u64 expected) {
    return elapsed > expected ? elapsed - expected : 0U;
}

_Thread_local u64 managed_tls_value;

u64 managed_tls(u64 delta) {
    managed_tls_value += delta;
    return managed_tls_value;
}

__attribute__((noreturn)) void managed_never(void *argument) {
    volatile u64 *value = argument;
    for (;;) *value += 1U;
}
