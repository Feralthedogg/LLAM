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

#include "metadata.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define TEST_HEADER_FLAGS_OFFSET 28U
#define TEST_HEADER_VERSION_OFFSET 4U
#define TEST_HEADER_SIZE_OFFSET 12U
#define TEST_RANGE_FLAGS_OFFSET (LLAM_PREEMPT_METADATA_HEADER_SIZE + 8U)

static unsigned g_failures;

static void test_check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "test_preempt_metadata: %s\n", message);
        g_failures += 1U;
    }
}

static int32_t test_relative_offset(const unsigned char *record,
                                   size_t field_offset,
                                   uintptr_t target) {
    uintptr_t field = (uintptr_t)(record + field_offset);

    return (int32_t)((intptr_t)target - (intptr_t)field);
}

static size_t test_make_two_ranges(unsigned char *bytes, size_t capacity, uint32_t map_flags) {
    llam_preempt_metadata_range_input_t ranges[2];
    uintptr_t buffer_base = (uintptr_t)bytes;
    size_t written = 0U;

    memset(bytes, 0, capacity);
    memset(ranges, 0, sizeof(ranges));
    ranges[0].start_offset = test_relative_offset(bytes, 32U, buffer_base + 320U);
    ranges[0].end_offset = test_relative_offset(bytes, 36U, buffer_base + 336U);
    ranges[0].flags = LLAM_PREEMPT_RANGE_ASYNC_SAFE;
    ranges[1].start_offset = test_relative_offset(bytes, 48U, buffer_base + 352U);
    ranges[1].end_offset = test_relative_offset(bytes, 52U, buffer_base + 368U);
    ranges[1].flags = LLAM_PREEMPT_RANGE_ASYNC_SAFE;
    if (llam_preempt_metadata_encode(LLAM_PREEMPT_METADATA_ARCH_X86_64,
                                     UINT64_C(0x0123456789abcdef),
                                     map_flags | LLAM_PREEMPT_METADATA_F_ASYNC_RANGES,
                                     ranges,
                                     2U,
                                     bytes,
                                     capacity,
                                     &written) != 0) {
        test_check(false, "could not encode the valid metadata fixture");
        return 0U;
    }
    return written;
}

static void test_valid_ranges_and_image_bounds(void) {
    unsigned char bytes[512];
    uintptr_t base = (uintptr_t)bytes;
    llam_preempt_metadata_t metadata;
    size_t consumed = 0U;
    size_t size = test_make_two_ranges(bytes, sizeof(bytes), 0U);
    llam_preempt_metadata_result_t result;

    result = llam_preempt_metadata_parse_one(bytes,
                                             size,
                                             false,
                                             LLAM_PREEMPT_METADATA_ARCH_X86_64,
                                             base + 256U,
                                             256U,
                                             &metadata,
                                             &consumed);
    test_check(result == LLAM_PREEMPT_METADATA_OK, "valid ranges were rejected");
    test_check(consumed == size, "valid record reported the wrong byte count");
    test_check(metadata.range_count == 2U, "valid range count was not preserved");
    if (metadata.range_count == 2U) {
        test_check(metadata.ranges[0].start_pc == base + 320U &&
                       metadata.ranges[0].end_pc == base + 336U,
                   "first PC-relative range resolved incorrectly");
        test_check(metadata.ranges[1].start_pc == base + 352U &&
                       metadata.ranges[1].end_pc == base + 368U,
                   "second PC-relative range resolved incorrectly");
    }
    test_check(metadata.build_id == UINT64_C(0x0123456789abcdef),
               "build id was not preserved");
    llam_preempt_metadata_destroy(&metadata);

    result = llam_preempt_metadata_parse_one(bytes,
                                             size,
                                             false,
                                             LLAM_PREEMPT_METADATA_ARCH_X86_64,
                                             base + 321U,
                                             100U,
                                             &metadata,
                                             &consumed);
    test_check(result == LLAM_PREEMPT_METADATA_INVALID,
               "range outside the supplied executable image was accepted");
    llam_preempt_metadata_destroy(&metadata);
}

static void test_section_absence_and_required_mode(void) {
    llam_preempt_metadata_t metadata;
    size_t consumed = 9U;

    test_check(llam_preempt_metadata_parse_one(NULL,
                                               0U,
                                               false,
                                               LLAM_PREEMPT_METADATA_ARCH_UNKNOWN,
                                               0U,
                                               0U,
                                               &metadata,
                                               &consumed) == LLAM_PREEMPT_METADATA_ABSENT,
               "missing optional section did not retain the legacy path");
    test_check(consumed == 0U && metadata.ranges == NULL,
               "absent section left parse output populated");
    test_check(llam_preempt_metadata_parse_one(NULL,
                                               0U,
                                               true,
                                               LLAM_PREEMPT_METADATA_ARCH_UNKNOWN,
                                               0U,
                                               0U,
                                               &metadata,
                                               &consumed) == LLAM_PREEMPT_METADATA_UNSUPPORTED,
               "required mode silently accepted an absent section");
}

static void test_short_bad_magic_version_and_size(void) {
    unsigned char bytes[512];
    llam_preempt_metadata_t metadata;
    size_t consumed = 0U;
    size_t size = test_make_two_ranges(bytes, sizeof(bytes), 0U);

    test_check(llam_preempt_metadata_parse_one(bytes,
                                               LLAM_PREEMPT_METADATA_HEADER_SIZE - 1U,
                                               false,
                                               LLAM_PREEMPT_METADATA_ARCH_UNKNOWN,
                                               0U,
                                               0U,
                                               &metadata,
                                               &consumed) == LLAM_PREEMPT_METADATA_INVALID,
               "short header was not rejected");
    llam_preempt_metadata_destroy(&metadata);

    bytes[0] ^= 0x1U;
    test_check(llam_preempt_metadata_parse_one(bytes,
                                               size,
                                               false,
                                               LLAM_PREEMPT_METADATA_ARCH_UNKNOWN,
                                               0U,
                                               0U,
                                               &metadata,
                                               &consumed) == LLAM_PREEMPT_METADATA_INVALID,
               "bad magic was not rejected");
    llam_preempt_metadata_destroy(&metadata);
    bytes[0] ^= 0x1U;

    bytes[TEST_HEADER_VERSION_OFFSET] = 99U;
    test_check(llam_preempt_metadata_parse_one(bytes,
                                               size,
                                               false,
                                               LLAM_PREEMPT_METADATA_ARCH_UNKNOWN,
                                               0U,
                                               0U,
                                               &metadata,
                                               &consumed) == LLAM_PREEMPT_METADATA_IGNORED_OPTIONAL,
               "unknown optional version did not fail closed to legacy behavior");
    llam_preempt_metadata_destroy(&metadata);
    test_check(llam_preempt_metadata_parse_one(bytes,
                                               size,
                                               true,
                                               LLAM_PREEMPT_METADATA_ARCH_UNKNOWN,
                                               0U,
                                               0U,
                                               &metadata,
                                               &consumed) == LLAM_PREEMPT_METADATA_UNSUPPORTED,
               "required mode silently accepted an unknown version");
    llam_preempt_metadata_destroy(&metadata);
    bytes[TEST_HEADER_VERSION_OFFSET] = LLAM_PREEMPT_METADATA_VERSION;

    bytes[TEST_HEADER_SIZE_OFFSET] -= 1U;
    test_check(llam_preempt_metadata_parse_one(bytes,
                                               size,
                                               false,
                                               LLAM_PREEMPT_METADATA_ARCH_UNKNOWN,
                                               0U,
                                               0U,
                                               &metadata,
                                               &consumed) == LLAM_PREEMPT_METADATA_INVALID,
               "record-size mismatch was not rejected");
    llam_preempt_metadata_destroy(&metadata);
}

static void test_unknown_optional_and_required_flags(void) {
    unsigned char bytes[512];
    llam_preempt_metadata_t metadata;
    size_t consumed = 0U;
    size_t size = test_make_two_ranges(bytes, sizeof(bytes), 0U);
    llam_preempt_metadata_result_t result;

    bytes[TEST_HEADER_FLAGS_OFFSET] |= 0x80U;
    result = llam_preempt_metadata_parse_one(bytes,
                                             size,
                                             false,
                                             LLAM_PREEMPT_METADATA_ARCH_UNKNOWN,
                                             0U,
                                             0U,
                                             &metadata,
                                             &consumed);
    test_check(result == LLAM_PREEMPT_METADATA_IGNORED_OPTIONAL,
               "unknown optional header flag changed the default policy");
    llam_preempt_metadata_destroy(&metadata);

    bytes[TEST_HEADER_FLAGS_OFFSET] |= (unsigned char)LLAM_PREEMPT_METADATA_F_REQUIRED;
    result = llam_preempt_metadata_parse_one(bytes,
                                             size,
                                             false,
                                             LLAM_PREEMPT_METADATA_ARCH_UNKNOWN,
                                             0U,
                                             0U,
                                             &metadata,
                                             &consumed);
    test_check(result == LLAM_PREEMPT_METADATA_UNSUPPORTED,
               "unknown required header flag was not rejected");
    llam_preempt_metadata_destroy(&metadata);

    size = test_make_two_ranges(bytes, sizeof(bytes), 0U);
    bytes[TEST_RANGE_FLAGS_OFFSET + 0U] |= 0x80U;
    result = llam_preempt_metadata_parse_one(bytes,
                                             size,
                                             false,
                                             LLAM_PREEMPT_METADATA_ARCH_UNKNOWN,
                                             0U,
                                             0U,
                                             &metadata,
                                             &consumed);
    test_check(result == LLAM_PREEMPT_METADATA_OK &&
                   metadata.range_count == 1U &&
                   metadata.ignored_optional_ranges == 1U,
               "unknown optional range flag was not ignored as one record");
    llam_preempt_metadata_destroy(&metadata);

    bytes[TEST_HEADER_FLAGS_OFFSET] |= (unsigned char)LLAM_PREEMPT_METADATA_F_REQUIRED;
    result = llam_preempt_metadata_parse_one(bytes,
                                             size,
                                             false,
                                             LLAM_PREEMPT_METADATA_ARCH_UNKNOWN,
                                             0U,
                                             0U,
                                             &metadata,
                                             &consumed);
    test_check(result == LLAM_PREEMPT_METADATA_UNSUPPORTED,
               "unknown required range flag was not rejected");
    llam_preempt_metadata_destroy(&metadata);
}

static void test_duplicate_overflow_and_concatenated_records(void) {
    unsigned char bytes[1024];
    llam_preempt_metadata_range_input_t ranges[2];
    llam_preempt_metadata_range_input_t single_range;
    llam_preempt_metadata_t metadata;
    size_t first_size = 0U;
    size_t second_size = 0U;
    size_t consumed = 0U;
    uintptr_t base = (uintptr_t)bytes;
    llam_preempt_metadata_result_t result;

    memset(bytes, 0, sizeof(bytes));
    memset(ranges, 0, sizeof(ranges));
    ranges[0].start_offset = test_relative_offset(bytes, 32U, base + 320U);
    ranges[0].end_offset = test_relative_offset(bytes, 36U, base + 336U);
    ranges[0].flags = LLAM_PREEMPT_RANGE_ASYNC_SAFE;
    ranges[1] = ranges[0];
    ranges[1].start_offset = test_relative_offset(bytes, 48U, base + 320U);
    ranges[1].end_offset = test_relative_offset(bytes, 52U, base + 336U);
    test_check(llam_preempt_metadata_encode(LLAM_PREEMPT_METADATA_ARCH_X86_64,
                                            1U,
                                            LLAM_PREEMPT_METADATA_F_ASYNC_RANGES,
                                            ranges,
                                            2U,
                                            bytes,
                                            sizeof(bytes),
                                            &first_size) == 0,
               "duplicate fixture encode failed");
    result = llam_preempt_metadata_parse_one(bytes,
                                             first_size,
                                             false,
                                             LLAM_PREEMPT_METADATA_ARCH_UNKNOWN,
                                             0U,
                                             0U,
                                             &metadata,
                                             &consumed);
    test_check(result == LLAM_PREEMPT_METADATA_INVALID,
               "duplicate/overlapping ranges were accepted");
    llam_preempt_metadata_destroy(&metadata);

    single_range = ranges[0];
    single_range.start_offset = INT32_MAX;
    single_range.end_offset = INT32_MIN;
    test_check(llam_preempt_metadata_encode(LLAM_PREEMPT_METADATA_ARCH_X86_64,
                                            1U,
                                            LLAM_PREEMPT_METADATA_F_ASYNC_RANGES,
                                            &single_range,
                                            1U,
                                            bytes,
                                            sizeof(bytes),
                                            &first_size) == 0,
               "overflow fixture encode failed");
    result = llam_preempt_metadata_parse_one(bytes,
                                             first_size,
                                             false,
                                             LLAM_PREEMPT_METADATA_ARCH_UNKNOWN,
                                             UINTPTR_MAX - 8U,
                                             32U,
                                             &metadata,
                                             &consumed);
    test_check(result == LLAM_PREEMPT_METADATA_INVALID,
               "image-address overflow was accepted");
    llam_preempt_metadata_destroy(&metadata);

    memset(&single_range, 0, sizeof(single_range));
    single_range.start_offset = test_relative_offset(bytes, 32U,
                                                     (uintptr_t)bytes + 400U);
    single_range.end_offset = test_relative_offset(bytes, 36U,
                                                   (uintptr_t)bytes + 416U);
    single_range.flags = LLAM_PREEMPT_RANGE_COOPERATIVE_POLL;
    test_check(llam_preempt_metadata_encode(LLAM_PREEMPT_METADATA_ARCH_X86_64,
                                            1U,
                                            LLAM_PREEMPT_METADATA_F_COOPERATIVE_POLLS,
                                            &single_range,
                                            1U,
                                            bytes,
                                            sizeof(bytes),
                                            &second_size) == 0,
               "first concatenated record encode failed");
    memset(&single_range, 0, sizeof(single_range));
    single_range.start_offset = test_relative_offset(bytes + second_size,
                                                     32U,
                                                     (uintptr_t)bytes + 440U);
    single_range.end_offset = test_relative_offset(bytes + second_size,
                                                   36U,
                                                   (uintptr_t)bytes + 456U);
    single_range.flags = LLAM_PREEMPT_RANGE_COOPERATIVE_POLL;
    test_check(llam_preempt_metadata_encode(LLAM_PREEMPT_METADATA_ARCH_X86_64,
                                            2U,
                                            LLAM_PREEMPT_METADATA_F_COOPERATIVE_POLLS,
                                            &single_range,
                                            1U,
                                            bytes + second_size,
                                            sizeof(bytes) - second_size,
                                            &first_size) == 0,
               "second concatenated record encode failed");
    result = llam_preempt_metadata_parse_one(bytes,
                                             second_size + first_size,
                                             false,
                                             LLAM_PREEMPT_METADATA_ARCH_X86_64,
                                             0U,
                                             0U,
                                             &metadata,
                                             &consumed);
    test_check(result == LLAM_PREEMPT_METADATA_OK && consumed == second_size,
               "concatenated-record cursor did not report the first record size");
    llam_preempt_metadata_destroy(&metadata);
    result = llam_preempt_metadata_parse_one(bytes + consumed,
                                             first_size,
                                             false,
                                             LLAM_PREEMPT_METADATA_ARCH_X86_64,
                                             0U,
                                             0U,
                                             &metadata,
                                             &consumed);
    test_check(result == LLAM_PREEMPT_METADATA_OK && consumed == first_size &&
                   metadata.build_id == 2U,
               "second concatenated record did not parse independently");
    llam_preempt_metadata_destroy(&metadata);
}

int main(void) {
    test_valid_ranges_and_image_bounds();
    test_section_absence_and_required_mode();
    test_short_bad_magic_version_and_size();
    test_unknown_optional_and_required_flags();
    test_duplicate_overflow_and_concatenated_records();
    if (g_failures != 0U) {
        fprintf(stderr, "test_preempt_metadata: %u failure(s)\n", g_failures);
        return 1;
    }
    puts("test_preempt_metadata: versioned record checks passed");
    return 0;
}
