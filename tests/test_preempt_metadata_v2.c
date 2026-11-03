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

#include "metadata_v2.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); abort(); \
} } while (0)

enum { BUFFER_BYTES = 1024, MAPPING = 0x8000 };

static llam_llpm2_entry_t entries[] = {
    {.native_entry = 0x1000, .managed_entry = 0x1100,
     .code_begin = 0x1100, .code_end = 0x1140,
     .execution_class = LLAM_LLPM2_PORTABLE, .effects = 7U,
     .required_isa = LLAM_LLPM2_ISA_BASE | LLAM_LLPM2_ISA_FP_SIMD,
     .frame_profile = 1U, .range_count = 3U, .call_count = 4U, .type_id = 72U,
     .guard_bindings = 0x1800, .native_end = 0x1040},
    {.native_entry = 0x1200, .code_begin = 0x1200, .code_end = 0x1220,
     .execution_class = LLAM_LLPM2_LEGACY, .effects = 32U,
     .frame_profile = 1U, .first_range = 3U, .first_call = 4U, .type_id = 19U,
     .guard_bindings = 0x1800}
};
static llam_llpm2_range_t ranges[] = {
    {.kind = LLAM_LLPM2_UNSAFE_STACK, .begin = 0x1100, .end = 0x1108},
    {.kind = LLAM_LLPM2_SAFE, .begin = 0x1108, .end = 0x1120, .stack_alignment_log2 = 4U},
    {.kind = LLAM_LLPM2_UNSAFE_FOREIGN, .begin = 0x1124, .end = 0x1128}
};
static llam_llpm2_call_t calls[] = {
    {.effect = LLAM_LLPM2_EFFECT_FOREIGN_FAST, .site = 0x1124,
     .continuation = 0x1128, .native_target = 0x1300, .type_id = 41U},
    {.effect = LLAM_LLPM2_EFFECT_RUNTIME_CRITICAL, .flags = 13U,
     .site = 0x1008, .continuation = 0x100c, .native_target = 0x1800,
     .type_id = UINT64_C(13500169461951013840)},
    {.effect = LLAM_LLPM2_EFFECT_MANAGED, .flags = LLAM_LLPM2_CALL_NATIVE_THUNK,
     .site = 0x1010, .continuation = 0x1014, .native_target = 0x1000, .type_id = 72U},
    {.effect = LLAM_LLPM2_EFFECT_RUNTIME_CRITICAL, .flags = 21U,
     .site = 0x1018, .continuation = 0x101c, .native_target = 0x1808,
     .type_id = UINT64_C(13500169461951013840)}
};

static llam_llpm2_t fixture(void) {
    llam_llpm2_t m = {
        .header = {.architecture = LLAM_PREEMPT_METADATA_ARCH_AARCH64,
                   .native_abi = LLAM_LLPM2_ABI_DARWIN, .managed_major = 1U,
                   .runtime_major = 2U, .runtime_minor = 2U, .required_flags = 63U,
                   .build_id = {1U, 2U, 3U}, .toolchain_id = LLAM_LLPM2_PROFILE_LLVM23,
                   .executable_identity = UINT64_C(0x9876543210), .anchor_id = {0x4aU, 0x7eU}},
        .entry_count = 2U, .range_count = 3U, .call_count = 4U,
        .entries = entries, .ranges = ranges, .calls = calls
    };
    return m;
}

static llam_llpm2_requirements_t requirements(void) {
    return (llam_llpm2_requirements_t){
        .architecture = LLAM_PREEMPT_METADATA_ARCH_AARCH64,
        .native_abi = LLAM_LLPM2_ABI_DARWIN, .runtime_major = 2U, .runtime_minor = 2U,
        .toolchain_id = LLAM_LLPM2_PROFILE_LLVM23, .available_isa = 3U,
        .required = true, .require_sealed = true
    };
}

static void expect_failure(const unsigned char *bytes, size_t size,
                            llam_preempt_metadata_result_t expected) {
    llam_llpm2_t out;
    size_t consumed = 123U;
    llam_llpm2_requirements_t r = requirements();
    memset(&out, 0x5a, sizeof(out));
    CHECK(llam_llpm2_parse(bytes, size, MAPPING, 0x1000, 0x1000, &r, &out, &consumed) == expected);
    CHECK(out.entries == NULL && out.ranges == NULL && out.calls == NULL && out.entry_count == 0U);
    CHECK(consumed == (expected == LLAM_PREEMPT_METADATA_UNSUPPORTED ? size : 0U));
}

static void test_roundtrip_and_bounds(unsigned char *bytes, size_t size) {
    llam_llpm2_t out = {0};
    llam_llpm2_requirements_t r = requirements();
    size_t consumed = 0U;
    CHECK(llam_llpm2_parse(bytes, size + 8U, MAPPING, 0x1000, 0x1000, &r, &out, &consumed) == 0);
    CHECK(consumed == size && out.entry_count == 2U && out.range_count == 3U && out.call_count == 4U);
    CHECK(out.entries[0].native_entry == 0x1000 && out.entries[0].managed_entry == 0x1100);
    CHECK(out.entries[1].managed_entry == 0U && out.calls[0].native_target == 0x1300);
    CHECK(out.header.executable_identity == UINT64_C(0x9876543210));
    CHECK(llam_llpm2_find_safe(&out, 0U, 0x1108) == &out.ranges[1]);
    CHECK(llam_llpm2_find_safe(&out, 0U, 0x111f) == &out.ranges[1]);
    const uintptr_t unsafe[] = {0U, 0x1000, 0x1100, 0x1120, 0x1123, 0x1124, 0x1128, 0x1140};
    for (size_t i = 0U; i < sizeof(unsafe) / sizeof(unsafe[0]); ++i) {
        CHECK(llam_llpm2_find_safe(&out, 0U, unsafe[i]) == NULL);
    }
    CHECK(llam_llpm2_find_safe(&out, 1U, 0x1200) == NULL);
    CHECK(llam_llpm2_find_safe(&out, 2U, 0x1108) == NULL);
    llam_llpm2_destroy(&out);
    llam_llpm2_destroy(&out);
    for (size_t n = 1U; n < size; ++n) expect_failure(bytes, n, LLAM_PREEMPT_METADATA_INVALID);
    CHECK(llam_llpm2_parse(NULL, 0U, 0U, 0U, 0U, &r, &out, &consumed) == LLAM_PREEMPT_METADATA_ABSENT);
    CHECK(consumed == 0U);
    CHECK(llam_llpm2_parse(bytes, size, UINTPTR_MAX - 64U, 0U, 0U, &r, &out, &consumed) == -1);
    CHECK(llam_llpm2_parse(bytes, size, MAPPING, 0x1100, 0x100, &r, &out, &consumed) == -1);
}

static void test_mutations(const unsigned char *original, size_t size) {
    unsigned char bytes[BUFFER_BYTES];
    /* Header/table alias, reserved bits, bad counts, class/body mismatch, PC
     * alignment, overlapping intervals and native entry accidentally in SAFE. */
    const struct mutation { size_t offset; unsigned char value; } invalid[] = {
        {0U, 0U}, {16U, 0U}, {16U, 255U}, {80U, 120U}, {88U, 128U}, {96U, 128U},
        {128U + 80U, 1U}, {128U + 88U, 1U},
        {128U + 32U, 0U}, {128U + 52U, 1U}, {128U + 56U, 255U},
        {320U + 6U, 1U}, {320U + 28U, 1U}, {352U + 24U, 1U},
        {416U + 4U, 5U}, {416U + 40U, 1U}
    };
    for (size_t i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        memcpy(bytes, original, size);
        bytes[invalid[i].offset] = invalid[i].value;
        expect_failure(bytes, size, LLAM_PREEMPT_METADATA_INVALID);
    }
    const struct mutation unsupported_cases[] = {
        {4U, 3U}, {6U, 120U}, {28U, 80U}, {30U, 16U}, {32U, 40U}, {34U, 77U},
        {36U, 77U}, {38U, 2U}, {42U, 3U}, {44U, 3U}, {46U, 2U}, {48U, 127U},
        {72U, 0U}, {128U + 32U, 3U}, {128U + 48U, 2U}, {320U + 4U, 99U}
    };
    for (size_t i = 0U; i < sizeof(unsupported_cases) / sizeof(unsupported_cases[0]); ++i) {
        memcpy(bytes, original, size);
        bytes[unsupported_cases[i].offset] = unsupported_cases[i].value;
        expect_failure(bytes, size, LLAM_PREEMPT_METADATA_UNSUPPORTED);
    }
    llam_llpm2_t m = fixture();
    llam_llpm2_entry_t changed[2];
    memcpy(changed, entries, sizeof(changed));
    m.entries = changed;
    changed[0].native_entry = 0x1108;
    size_t written = 123U;
    memset(bytes, 0xa5, sizeof(bytes));
    errno = 0;
    CHECK(llam_llpm2_encode(&m, MAPPING, bytes, sizeof(bytes), &written) == -1 && errno == EINVAL);
    CHECK(written == 0U && bytes[0] == 0xa5U);
    m = fixture();
    CHECK(llam_llpm2_encode(&m, MAPPING, bytes, size - 1U, &written) == -1 && errno == ENOSPC);
    CHECK(written == 0U && bytes[0] == 0xa5U);
}

static void test_optional_and_profile(const unsigned char *original, size_t size) {
    unsigned char bytes[BUFFER_BYTES];
    memcpy(bytes, original, size);
    bytes[48U] &= (unsigned char)~LLAM_LLPM2_REQUIRED;
    bytes[34U] = 77U;
    llam_llpm2_t out = {0};
    llam_llpm2_requirements_t r = requirements();
    size_t consumed;
    r.required = false;
    CHECK(llam_llpm2_parse(bytes, size, MAPPING, 0U, 0U, &r, &out, &consumed) ==
          LLAM_PREEMPT_METADATA_IGNORED_OPTIONAL);
    CHECK(consumed == size && out.entry_count == 0U);
    r = requirements();
    r.available_isa = LLAM_LLPM2_ISA_BASE;
    CHECK(llam_llpm2_parse(original, size, MAPPING, 0U, 0U, &r, &out, &consumed) == -2);
    memcpy(bytes, original, size);
    bytes[48U] &= (unsigned char)~LLAM_LLPM2_LINK_VERIFIED;
    CHECK(llam_llpm2_parse(bytes, size, MAPPING, 0U, 0U, &r, &out, &consumed) == -2);
}

static void test_runtime_contract_binding(void) {
    llam_llpm2_t m = fixture();
    llam_llpm2_entry_t entry = {
        .native_entry = 0x1400, .code_begin = 0x1400, .code_end = 0x1420,
        .effects = 1U << LLAM_LLPM2_EFFECT_RUNTIME_CRITICAL, .frame_profile = 1U,
        .required_isa = LLAM_LLPM2_ISA_BASE, .call_count = 1U,
        .flags = LLAM_LLPM2_ENTRY_INTERNAL | LLAM_LLPM2_ENTRY_RUNTIME_BRIDGE,
        .type_id = llam_llpm2_runtime_type_id(1U, LLAM_PREEMPT_METADATA_ARCH_AARCH64, LLAM_LLPM2_ABI_DARWIN)
    };
    llam_llpm2_call_t call = {
        .site = 0x1408, .continuation = 0x140c, .native_target = 0x1900,
        .flags = LLAM_LLPM2_CALL_INDIRECT | LLAM_LLPM2_CALL_RUNTIME_BODY,
        .effect = LLAM_LLPM2_EFFECT_RUNTIME_CRITICAL, .type_id = entry.type_id, .contract_id = 1U
    };
    CHECK(entry.type_id == UINT64_C(13500169461951013840));
    CHECK(llam_llpm2_runtime_type_id(0U, 2U, 2U) == 0U);
    CHECK(llam_llpm2_runtime_type_id(UINT64_MAX, 2U, 2U) == 0U);
    CHECK(llam_llpm2_runtime_type_id(1U, 0U, 2U) == 0U);
    CHECK(llam_llpm2_runtime_type_id(1U, 2U, LLAM_LLPM2_ABI_WIN64) == 0U);
    CHECK(llam_llpm2_runtime_type_id(85U, 1U, 1U) != llam_llpm2_runtime_type_id(85U, 1U, 3U));
    CHECK(llam_llpm2_runtime_effect(1U) == LLAM_LLPM2_EFFECT_RUNTIME_CRITICAL);
    CHECK(llam_llpm2_runtime_effect(109U) == LLAM_LLPM2_EFFECT_NATIVE_AFFINE);
    CHECK(llam_llpm2_runtime_effect(0U) == LLAM_LLPM2_EFFECT_UNKNOWN);
    m.header.required_flags &= (uint32_t)~LLAM_LLPM2_GUARD_BINDINGS;
    m.entry_count = 1U;
    m.range_count = 0U;
    m.call_count = 1U;
    m.entries = &entry;
    m.ranges = NULL;
    m.calls = &call;
    unsigned char bytes[BUFFER_BYTES];
    size_t written = 0U;
    CHECK(llam_llpm2_encode(&m, MAPPING, bytes, sizeof(bytes), &written) == 0);
    CHECK(written == LLAM_LLPM2_HEADER_BYTES + LLAM_LLPM2_ENTRY_BYTES + LLAM_LLPM2_CALL_BYTES);
    llam_llpm2_t out = {0};
    llam_llpm2_requirements_t r = requirements();
    size_t consumed = 0U;
    CHECK(llam_llpm2_parse(bytes, written, MAPPING, 0x1000, 0x1000, &r, &out, &consumed) == 0);
    CHECK(out.calls[0].contract_id == 1U && out.calls[0].native_target == 0x1900);
    CHECK(llam_llpm2_find_safe(&out, 0U, call.site) == NULL);
    llam_llpm2_destroy(&out);
    const struct { size_t offset; unsigned char value; } mutations[] = {
        {128U + 68U, LLAM_LLPM2_ENTRY_INTERNAL},
        {128U + 68U, LLAM_LLPM2_ENTRY_RUNTIME_BRIDGE},
        {128U + 68U, LLAM_LLPM2_ENTRY_INTERNAL | LLAM_LLPM2_ENTRY_RUNTIME_BRIDGE | LLAM_LLPM2_ENTRY_NATIVE_BOUNDARY},
        {128U + 32U, LLAM_LLPM2_PORTABLE}, {128U + 36U, 6U}, {128U + 64U, 0U},
        {224U + 6U, LLAM_LLPM2_CALL_RUNTIME_BODY}, {224U + 6U, LLAM_LLPM2_CALL_INDIRECT},
        {224U + 4U, LLAM_LLPM2_EFFECT_FOREIGN_FAST}, {224U + 24U, 1U},
        {224U + 32U, 0U}, {224U + 40U, 0U}, {224U + 40U, 3U}, {224U + 40U, 255U}
    };
    for (size_t i = 0U; i < sizeof(mutations) / sizeof(mutations[0]); ++i) {
        unsigned char changed[BUFFER_BYTES];
        memcpy(changed, bytes, written);
        changed[mutations[i].offset] = mutations[i].value;
        expect_failure(changed, written, LLAM_PREEMPT_METADATA_INVALID);
    }
    call.contract_id = 109U;
    call.type_id = llam_llpm2_runtime_type_id(call.contract_id, m.header.architecture, m.header.native_abi);
    entry.type_id = call.type_id;
    call.effect = LLAM_LLPM2_EFFECT_NATIVE_AFFINE;
    entry.effects = 1U << LLAM_LLPM2_EFFECT_NATIVE_AFFINE;
    CHECK(llam_llpm2_encode(&m, MAPPING, bytes, sizeof(bytes), &written) == 0);
    CHECK(llam_llpm2_parse(bytes, written, MAPPING, 0x1000, 0x1000, &r, &out, &consumed) == 0);
    CHECK(out.calls[0].contract_id == 109U && out.calls[0].effect == LLAM_LLPM2_EFFECT_NATIVE_AFFINE);
    llam_llpm2_destroy(&out);
    call.effect = LLAM_LLPM2_EFFECT_RUNTIME_CRITICAL;
    CHECK(llam_llpm2_encode(&m, MAPPING, bytes, sizeof(bytes), &written) == -1 && errno == EINVAL);
    call.contract_id = 0U;
    CHECK(llam_llpm2_encode(&m, MAPPING, bytes, sizeof(bytes), &written) == -1 && errno == EINVAL);
}

int main(void) {
    llam_llpm2_t m = fixture();
    unsigned char bytes[BUFFER_BYTES] = {0};
    size_t size;
    CHECK(llam_llpm2_encode(&m, MAPPING, bytes, sizeof(bytes), &size) == 0);
    CHECK(size == 608U);
    test_roundtrip_and_bounds(bytes, size);
    test_mutations(bytes, size);
    test_optional_and_profile(bytes, size);
    test_runtime_contract_binding();
    puts("LLPM v2 bounds, identities, execution classes and unsafe PC tests passed");
    return 0;
}
