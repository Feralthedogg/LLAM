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
 * See LICENSES/Apache-2.0.txt.
 */

#if defined(LLAM_R8_NEG_STACK)
__attribute__((noinline, annotate("llam-preempt-async-safe")))
unsigned long long llam_late_stack_argument(
    unsigned long long first, unsigned long long second,
    unsigned long long third, unsigned long long fourth,
    unsigned long long fifth) {
    return first + second + third + fourth + fifth;
}
#elif defined(LLAM_R8_NEG_HELPER)
__attribute__((noinline, annotate("llam-preempt-async-safe")))
unsigned long long llam_late_wide_div(
    unsigned long long high, unsigned long long low,
    unsigned long long divisor_high, unsigned long long divisor_low) {
    unsigned __int128 numerator =
        ((unsigned __int128)high << 64U) | low;
    unsigned __int128 divisor =
        (((unsigned __int128)divisor_high << 64U) | divisor_low) | 1U;
    return (unsigned long long)(numerator / divisor);
}
#elif defined(LLAM_R8_NEG_CALLEE_SAVE)
__attribute__((noinline, annotate("llam-preempt-async-safe")))
unsigned long long llam_late_register_pressure(unsigned long long iterations) {
    unsigned long long a = 1ULL, b = 2ULL, c = 3ULL;
    unsigned long long d = 4ULL, e = 5ULL, f = 6ULL;
    unsigned long long g = 7ULL, h = 8ULL, i = 9ULL;
    unsigned long long j = 10ULL, k = 11ULL, l = 12ULL;
    for (unsigned long long index = 0ULL; index < iterations; ++index) {
        a = (a << 7U) ^ b;
        b = (b << 11U) ^ c;
        c = (c << 13U) ^ d;
        d = (d << 17U) ^ e;
        e = (e << 19U) ^ f;
        f = (f << 23U) ^ g;
        g = (g << 29U) ^ h;
        h = (h << 31U) ^ i;
        i = (i << 37U) ^ j;
        j = (j << 41U) ^ k;
        k = (k << 43U) ^ l;
        l = (l << 47U) ^ a;
    }
    return a ^ b ^ c ^ d ^ e ^ f ^ g ^ h ^ i ^ j ^ k ^ l;
}
#elif defined(LLAM_R8_NEG_MIXED_GOOD_FIRST)
__attribute__((noinline, annotate("llam-preempt-async-safe")))
unsigned long long llam_late_mixed_good(unsigned long long value) {
    return value * 3ULL + 1ULL;
}
__attribute__((noinline, annotate("llam-preempt-async-safe")))
unsigned long long llam_late_mixed_bad(
    unsigned long long a, unsigned long long b,
    unsigned long long c, unsigned long long d,
    unsigned long long e) {
    return a + b + c + d + e;
}
unsigned long long llam_late_mixed_ordinary(unsigned long long value) {
    return value + 9ULL;
}
#elif defined(LLAM_R8_NEG_MIXED_BAD_FIRST)
__attribute__((noinline, annotate("llam-preempt-async-safe")))
unsigned long long llam_late_mixed_bad_first(
    unsigned long long a, unsigned long long b,
    unsigned long long c, unsigned long long d,
    unsigned long long e) {
    return a + b + c + d + e;
}
__attribute__((noinline, annotate("llam-preempt-async-safe")))
unsigned long long llam_late_mixed_good_second(unsigned long long value) {
    return value * 3ULL + 1ULL;
}
unsigned long long llam_late_mixed_ordinary_last(unsigned long long value) {
    return value + 9ULL;
}
#else
#error "Select one LLAM_R8_NEG_* fixture shape"
#endif
