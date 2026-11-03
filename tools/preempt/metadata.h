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

#ifndef LLAM_PREEMPT_METADATA_H
#define LLAM_PREEMPT_METADATA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LLAM_PREEMPT_METADATA_MAGIC UINT32_C(0x4d504c4c) /* "LLPM" little-endian */
#define LLAM_PREEMPT_METADATA_VERSION 1U
#define LLAM_PREEMPT_METADATA_HEADER_SIZE 32U
#define LLAM_PREEMPT_METADATA_RANGE_SIZE 16U
#define LLAM_PREEMPT_METADATA_MAX_RANGES 65536U
#define LLAM_PREEMPT_METADATA_SECTION ".llam.preempt"

typedef enum llam_preempt_metadata_arch {
    LLAM_PREEMPT_METADATA_ARCH_UNKNOWN = 0,
    LLAM_PREEMPT_METADATA_ARCH_X86_64 = 1,
    LLAM_PREEMPT_METADATA_ARCH_AARCH64 = 2,
} llam_preempt_metadata_arch_t;

typedef enum llam_preempt_metadata_header_flag {
    LLAM_PREEMPT_METADATA_F_REQUIRED = UINT32_C(1) << 0U,
    LLAM_PREEMPT_METADATA_F_COOPERATIVE_POLLS = UINT32_C(1) << 1U,
    LLAM_PREEMPT_METADATA_F_ASYNC_RANGES = UINT32_C(1) << 2U,
} llam_preempt_metadata_header_flag_t;

typedef enum llam_preempt_metadata_range_flag {
    LLAM_PREEMPT_RANGE_ASYNC_SAFE = UINT32_C(1) << 0U,
    LLAM_PREEMPT_RANGE_RUNTIME_UNSAFE = UINT32_C(1) << 1U,
    LLAM_PREEMPT_RANGE_FOREIGN_CALL = UINT32_C(1) << 2U,
    LLAM_PREEMPT_RANGE_STACK_POINTER_UNSTABLE = UINT32_C(1) << 3U,
    LLAM_PREEMPT_RANGE_THREAD_AFFINE = UINT32_C(1) << 4U,
    LLAM_PREEMPT_RANGE_NO_PREEMPT = UINT32_C(1) << 5U,
    LLAM_PREEMPT_RANGE_COOPERATIVE_POLL = UINT32_C(1) << 6U,
} llam_preempt_metadata_range_flag_t;

typedef enum llam_preempt_metadata_result {
    LLAM_PREEMPT_METADATA_OK = 0,
    LLAM_PREEMPT_METADATA_ABSENT = 1,
    LLAM_PREEMPT_METADATA_IGNORED_OPTIONAL = 2,
    LLAM_PREEMPT_METADATA_INVALID = -1,
    LLAM_PREEMPT_METADATA_UNSUPPORTED = -2,
} llam_preempt_metadata_result_t;

typedef struct llam_preempt_metadata_range {
    int32_t start_offset;
    int32_t end_offset;
    uint32_t flags;
    uint32_t reserved;
    uintptr_t start_pc;
    uintptr_t end_pc;
} llam_preempt_metadata_range_t;

typedef struct llam_preempt_metadata {
    uint16_t version;
    llam_preempt_metadata_arch_t architecture;
    uint64_t build_id;
    uint32_t flags;
    size_t range_count;
    size_t ignored_optional_ranges;
    llam_preempt_metadata_range_t *ranges;
} llam_preempt_metadata_t;

typedef struct llam_preempt_metadata_range_input {
    int32_t start_offset;
    int32_t end_offset;
    uint32_t flags;
    uint32_t reserved;
} llam_preempt_metadata_range_input_t;

/**
 * Parse one record from a mapped `.llam.preempt` section.
 *
 * Range offsets are signed displacements from the corresponding offset field
 * address, which permits link-time PC-relative relocations. `image_base` and
 * `image_size` may both be zero when the caller only wants structural parsing;
 * otherwise every resolved range must fall inside that executable image.
 * `consumed` reports the record size so callers can walk concatenated records.
 */
llam_preempt_metadata_result_t llam_preempt_metadata_parse_one(
    const void *bytes,
    size_t available,
    bool required_mode,
    uint16_t expected_architecture,
    uintptr_t image_base,
    size_t image_size,
    llam_preempt_metadata_t *out_metadata,
    size_t *consumed);

void llam_preempt_metadata_destroy(llam_preempt_metadata_t *metadata);

int llam_preempt_metadata_encode(uint16_t architecture,
                                 uint64_t build_id,
                                 uint32_t flags,
                                 const llam_preempt_metadata_range_input_t *ranges,
                                 size_t range_count,
                                 void *destination,
                                 size_t destination_capacity,
                                 size_t *written);

const char *llam_preempt_metadata_result_name(llam_preempt_metadata_result_t result);

#ifdef __cplusplus
}
#endif

#endif /* LLAM_PREEMPT_METADATA_H */
