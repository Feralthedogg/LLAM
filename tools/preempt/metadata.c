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

#include "metadata.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

enum {
    LLAM_METADATA_OFFSET_MAGIC = 0U,
    LLAM_METADATA_OFFSET_VERSION = 4U,
    LLAM_METADATA_OFFSET_ARCH = 6U,
    LLAM_METADATA_OFFSET_HEADER_SIZE = 8U,
    LLAM_METADATA_OFFSET_TOTAL_SIZE = 12U,
    LLAM_METADATA_OFFSET_BUILD_ID = 16U,
    LLAM_METADATA_OFFSET_RANGE_COUNT = 24U,
    LLAM_METADATA_OFFSET_FLAGS = 28U,
    LLAM_METADATA_RANGE_OFFSET_START = 0U,
    LLAM_METADATA_RANGE_OFFSET_END = 4U,
    LLAM_METADATA_RANGE_OFFSET_FLAGS = 8U,
    LLAM_METADATA_RANGE_OFFSET_RESERVED = 12U,
};

#define LLAM_METADATA_HEADER_FLAGS_KNOWN                                      \
    ((uint32_t)LLAM_PREEMPT_METADATA_F_REQUIRED |                            \
     (uint32_t)LLAM_PREEMPT_METADATA_F_COOPERATIVE_POLLS |                    \
     (uint32_t)LLAM_PREEMPT_METADATA_F_ASYNC_RANGES)
#define LLAM_METADATA_RANGE_FLAGS_KNOWN                                        \
    ((uint32_t)LLAM_PREEMPT_RANGE_ASYNC_SAFE |                               \
     (uint32_t)LLAM_PREEMPT_RANGE_RUNTIME_UNSAFE |                           \
     (uint32_t)LLAM_PREEMPT_RANGE_FOREIGN_CALL |                             \
     (uint32_t)LLAM_PREEMPT_RANGE_STACK_POINTER_UNSTABLE |                    \
     (uint32_t)LLAM_PREEMPT_RANGE_THREAD_AFFINE |                            \
     (uint32_t)LLAM_PREEMPT_RANGE_NO_PREEMPT |                               \
     (uint32_t)LLAM_PREEMPT_RANGE_COOPERATIVE_POLL)
#define LLAM_METADATA_ASYNC_BLOCKERS                                          \
    ((uint32_t)LLAM_PREEMPT_RANGE_RUNTIME_UNSAFE |                           \
     (uint32_t)LLAM_PREEMPT_RANGE_FOREIGN_CALL |                             \
     (uint32_t)LLAM_PREEMPT_RANGE_STACK_POINTER_UNSTABLE |                    \
     (uint32_t)LLAM_PREEMPT_RANGE_THREAD_AFFINE |                            \
     (uint32_t)LLAM_PREEMPT_RANGE_NO_PREEMPT |                               \
     (uint32_t)LLAM_PREEMPT_RANGE_COOPERATIVE_POLL)

static uint16_t llam_metadata_read_u16(const unsigned char *bytes) {
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8U));
}

static uint32_t llam_metadata_read_u32(const unsigned char *bytes) {
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

static int32_t llam_metadata_read_i32(const unsigned char *bytes) {
    uint32_t bits = llam_metadata_read_u32(bytes);
    int64_t value = (bits & UINT32_C(0x80000000)) != 0U
                        ? (int64_t)bits - INT64_C(0x100000000)
                        : (int64_t)bits;

    return (int32_t)value;
}

static uint64_t llam_metadata_read_u64(const unsigned char *bytes) {
    return (uint64_t)llam_metadata_read_u32(bytes) |
           ((uint64_t)llam_metadata_read_u32(bytes + 4U) << 32U);
}

static void llam_metadata_write_u16(unsigned char *bytes, uint16_t value) {
    bytes[0] = (unsigned char)value;
    bytes[1] = (unsigned char)(value >> 8U);
}

static void llam_metadata_write_u32(unsigned char *bytes, uint32_t value) {
    bytes[0] = (unsigned char)value;
    bytes[1] = (unsigned char)(value >> 8U);
    bytes[2] = (unsigned char)(value >> 16U);
    bytes[3] = (unsigned char)(value >> 24U);
}

static void llam_metadata_write_u64(unsigned char *bytes, uint64_t value) {
    llam_metadata_write_u32(bytes, (uint32_t)value);
    llam_metadata_write_u32(bytes + 4U, (uint32_t)(value >> 32U));
}

static bool llam_metadata_resolve_relative(uintptr_t field_address,
                                           int32_t relative,
                                           uintptr_t *out_address) {
    uint64_t magnitude;

    if (out_address == NULL) {
        return false;
    }
    if (relative >= 0) {
        magnitude = (uint32_t)relative;
        if (magnitude > UINTPTR_MAX - field_address) {
            return false;
        }
        *out_address = field_address + (uintptr_t)magnitude;
        return true;
    }
    magnitude = (uint64_t)(-(int64_t)relative);
    if (magnitude > field_address) {
        return false;
    }
    *out_address = field_address - (uintptr_t)magnitude;
    return true;
}

static int llam_metadata_range_compare(const void *left, const void *right) {
    const llam_preempt_metadata_range_t *left_range = left;
    const llam_preempt_metadata_range_t *right_range = right;

    if (left_range->start_pc < right_range->start_pc) {
        return -1;
    }
    if (left_range->start_pc > right_range->start_pc) {
        return 1;
    }
    if (left_range->end_pc < right_range->end_pc) {
        return -1;
    }
    if (left_range->end_pc > right_range->end_pc) {
        return 1;
    }
    return 0;
}

static llam_preempt_metadata_result_t llam_metadata_unsupported_or_ignore(
    bool required_mode,
    uint32_t header_flags,
    size_t record_size,
    size_t *consumed) {
    if (consumed != NULL) {
        *consumed = record_size;
    }
    if (required_mode || (header_flags & LLAM_PREEMPT_METADATA_F_REQUIRED) != 0U) {
        return LLAM_PREEMPT_METADATA_UNSUPPORTED;
    }
    return LLAM_PREEMPT_METADATA_IGNORED_OPTIONAL;
}

void llam_preempt_metadata_destroy(llam_preempt_metadata_t *metadata) {
    if (metadata == NULL) {
        return;
    }
    free(metadata->ranges);
    memset(metadata, 0, sizeof(*metadata));
}

llam_preempt_metadata_result_t llam_preempt_metadata_parse_one(
    const void *opaque_bytes,
    size_t available,
    bool required_mode,
    uint16_t expected_architecture,
    uintptr_t image_base,
    size_t image_size,
    llam_preempt_metadata_t *out_metadata,
    size_t *consumed) {
    const unsigned char *bytes = opaque_bytes;
    uint16_t version;
    uint16_t architecture;
    uint32_t header_size;
    uint32_t total_size;
    uint32_t range_count;
    uint32_t header_flags;
    uint32_t unknown_header_flags;
    size_t expected_size;
    size_t index;
    uintptr_t image_end = 0U;

    if (out_metadata == NULL || consumed == NULL) {
        errno = EINVAL;
        return LLAM_PREEMPT_METADATA_INVALID;
    }
    memset(out_metadata, 0, sizeof(*out_metadata));
    *consumed = 0U;
    if (bytes == NULL || available == 0U) {
        return required_mode ? LLAM_PREEMPT_METADATA_UNSUPPORTED
                             : LLAM_PREEMPT_METADATA_ABSENT;
    }
    if (available < LLAM_PREEMPT_METADATA_HEADER_SIZE) {
        errno = EINVAL;
        return LLAM_PREEMPT_METADATA_INVALID;
    }
    if (llam_metadata_read_u32(bytes + LLAM_METADATA_OFFSET_MAGIC) !=
        LLAM_PREEMPT_METADATA_MAGIC) {
        errno = EINVAL;
        return LLAM_PREEMPT_METADATA_INVALID;
    }

    version = llam_metadata_read_u16(bytes + LLAM_METADATA_OFFSET_VERSION);
    architecture = llam_metadata_read_u16(bytes + LLAM_METADATA_OFFSET_ARCH);
    header_size = llam_metadata_read_u32(bytes + LLAM_METADATA_OFFSET_HEADER_SIZE);
    total_size = llam_metadata_read_u32(bytes + LLAM_METADATA_OFFSET_TOTAL_SIZE);
    range_count = llam_metadata_read_u32(bytes + LLAM_METADATA_OFFSET_RANGE_COUNT);
    header_flags = llam_metadata_read_u32(bytes + LLAM_METADATA_OFFSET_FLAGS);

    if (header_size != LLAM_PREEMPT_METADATA_HEADER_SIZE ||
        total_size < header_size || total_size > available) {
        errno = EINVAL;
        return LLAM_PREEMPT_METADATA_INVALID;
    }
    if (version != LLAM_PREEMPT_METADATA_VERSION) {
        return llam_metadata_unsupported_or_ignore(required_mode,
                                                   header_flags,
                                                   total_size,
                                                   consumed);
    }
    unknown_header_flags = header_flags & ~LLAM_METADATA_HEADER_FLAGS_KNOWN;
    if (unknown_header_flags != 0U) {
        return llam_metadata_unsupported_or_ignore(required_mode,
                                                   header_flags,
                                                   total_size,
                                                   consumed);
    }
    if ((expected_architecture != LLAM_PREEMPT_METADATA_ARCH_UNKNOWN &&
         architecture != expected_architecture) ||
        (architecture != LLAM_PREEMPT_METADATA_ARCH_X86_64 &&
         architecture != LLAM_PREEMPT_METADATA_ARCH_AARCH64)) {
        return llam_metadata_unsupported_or_ignore(required_mode,
                                                   header_flags,
                                                   total_size,
                                                   consumed);
    }
    if (range_count > LLAM_PREEMPT_METADATA_MAX_RANGES ||
        (size_t)range_count > (SIZE_MAX - header_size) / LLAM_PREEMPT_METADATA_RANGE_SIZE) {
        errno = EOVERFLOW;
        return LLAM_PREEMPT_METADATA_INVALID;
    }
    expected_size = header_size + (size_t)range_count * LLAM_PREEMPT_METADATA_RANGE_SIZE;
    if (expected_size != total_size ||
        ((header_flags & LLAM_PREEMPT_METADATA_F_ASYNC_RANGES) != 0U &&
         range_count == 0U) ||
        ((image_base == 0U) != (image_size == 0U))) {
        errno = EINVAL;
        return LLAM_PREEMPT_METADATA_INVALID;
    }
    if (image_size != 0U) {
        if (image_size > UINTPTR_MAX - image_base) {
            errno = EOVERFLOW;
            return LLAM_PREEMPT_METADATA_INVALID;
        }
        image_end = image_base + image_size;
    }

    out_metadata->version = version;
    out_metadata->architecture = (llam_preempt_metadata_arch_t)architecture;
    out_metadata->build_id = llam_metadata_read_u64(bytes + LLAM_METADATA_OFFSET_BUILD_ID);
    out_metadata->flags = header_flags;
    if (range_count != 0U) {
        out_metadata->ranges = calloc(range_count, sizeof(*out_metadata->ranges));
        if (out_metadata->ranges == NULL) {
            errno = ENOMEM;
            return LLAM_PREEMPT_METADATA_INVALID;
        }
    }

    for (index = 0U; index < range_count; ++index) {
        size_t offset = header_size + index * LLAM_PREEMPT_METADATA_RANGE_SIZE;
        const unsigned char *record = bytes + offset;
        int32_t start_offset = llam_metadata_read_i32(
            record + LLAM_METADATA_RANGE_OFFSET_START);
        int32_t end_offset = llam_metadata_read_i32(
            record + LLAM_METADATA_RANGE_OFFSET_END);
        uint32_t flags = llam_metadata_read_u32(record + LLAM_METADATA_RANGE_OFFSET_FLAGS);
        uint32_t reserved = llam_metadata_read_u32(record + LLAM_METADATA_RANGE_OFFSET_RESERVED);
        uintptr_t start_pc;
        uintptr_t end_pc;
        uint32_t blockers = flags & LLAM_METADATA_ASYNC_BLOCKERS;

        if (reserved != 0U ||
            !llam_metadata_resolve_relative((uintptr_t)(record + LLAM_METADATA_RANGE_OFFSET_START),
                                            start_offset,
                                            &start_pc) ||
            !llam_metadata_resolve_relative((uintptr_t)(record + LLAM_METADATA_RANGE_OFFSET_END),
                                            end_offset,
                                            &end_pc) ||
            start_pc >= end_pc ||
            (image_size != 0U && (start_pc < image_base || end_pc > image_end))) {
            errno = EINVAL;
            llam_preempt_metadata_destroy(out_metadata);
            return LLAM_PREEMPT_METADATA_INVALID;
        }
        if ((flags & ~LLAM_METADATA_RANGE_FLAGS_KNOWN) != 0U) {
            if (required_mode ||
                (header_flags & LLAM_PREEMPT_METADATA_F_REQUIRED) != 0U) {
                llam_preempt_metadata_destroy(out_metadata);
                *consumed = total_size;
                return LLAM_PREEMPT_METADATA_UNSUPPORTED;
            }
            out_metadata->ignored_optional_ranges += 1U;
            continue;
        }
        if (((flags & LLAM_PREEMPT_RANGE_ASYNC_SAFE) != 0U && blockers != 0U) ||
            ((flags & LLAM_PREEMPT_RANGE_ASYNC_SAFE) == 0U &&
             (header_flags & LLAM_PREEMPT_METADATA_F_ASYNC_RANGES) != 0U)) {
            errno = EINVAL;
            llam_preempt_metadata_destroy(out_metadata);
            return LLAM_PREEMPT_METADATA_INVALID;
        }
        out_metadata->ranges[out_metadata->range_count].start_offset = start_offset;
        out_metadata->ranges[out_metadata->range_count].end_offset = end_offset;
        out_metadata->ranges[out_metadata->range_count].flags = flags;
        out_metadata->ranges[out_metadata->range_count].reserved = 0U;
        out_metadata->ranges[out_metadata->range_count].start_pc = start_pc;
        out_metadata->ranges[out_metadata->range_count].end_pc = end_pc;
        out_metadata->range_count += 1U;
    }

    if (out_metadata->range_count > 1U) {
        qsort(out_metadata->ranges,
              out_metadata->range_count,
              sizeof(*out_metadata->ranges),
              llam_metadata_range_compare);
        for (index = 1U; index < out_metadata->range_count; ++index) {
            if (out_metadata->ranges[index - 1U].end_pc >
                out_metadata->ranges[index].start_pc) {
                errno = EINVAL;
                llam_preempt_metadata_destroy(out_metadata);
                return LLAM_PREEMPT_METADATA_INVALID;
            }
        }
    }
    *consumed = total_size;
    return LLAM_PREEMPT_METADATA_OK;
}

int llam_preempt_metadata_encode(uint16_t architecture,
                                 uint64_t build_id,
                                 uint32_t flags,
                                 const llam_preempt_metadata_range_input_t *ranges,
                                 size_t range_count,
                                 void *destination,
                                 size_t destination_capacity,
                                 size_t *written) {
    unsigned char *bytes = destination;
    size_t total_size;
    size_t index;

    if (written != NULL) {
        *written = 0U;
    }
    if (destination == NULL || written == NULL ||
        (range_count != 0U && ranges == NULL) ||
        range_count > LLAM_PREEMPT_METADATA_MAX_RANGES ||
        range_count > (SIZE_MAX - LLAM_PREEMPT_METADATA_HEADER_SIZE) /
                          LLAM_PREEMPT_METADATA_RANGE_SIZE) {
        errno = EINVAL;
        return -1;
    }
    total_size = LLAM_PREEMPT_METADATA_HEADER_SIZE +
                 range_count * LLAM_PREEMPT_METADATA_RANGE_SIZE;
    if (total_size > UINT32_MAX || destination_capacity < total_size) {
        errno = ENOSPC;
        return -1;
    }
    memset(bytes, 0, total_size);
    llam_metadata_write_u32(bytes + LLAM_METADATA_OFFSET_MAGIC,
                            LLAM_PREEMPT_METADATA_MAGIC);
    llam_metadata_write_u16(bytes + LLAM_METADATA_OFFSET_VERSION,
                            LLAM_PREEMPT_METADATA_VERSION);
    llam_metadata_write_u16(bytes + LLAM_METADATA_OFFSET_ARCH, architecture);
    llam_metadata_write_u32(bytes + LLAM_METADATA_OFFSET_HEADER_SIZE,
                            LLAM_PREEMPT_METADATA_HEADER_SIZE);
    llam_metadata_write_u32(bytes + LLAM_METADATA_OFFSET_TOTAL_SIZE,
                            (uint32_t)total_size);
    llam_metadata_write_u64(bytes + LLAM_METADATA_OFFSET_BUILD_ID, build_id);
    llam_metadata_write_u32(bytes + LLAM_METADATA_OFFSET_RANGE_COUNT,
                            (uint32_t)range_count);
    llam_metadata_write_u32(bytes + LLAM_METADATA_OFFSET_FLAGS, flags);
    for (index = 0U; index < range_count; ++index) {
        unsigned char *record = bytes + LLAM_PREEMPT_METADATA_HEADER_SIZE +
                               index * LLAM_PREEMPT_METADATA_RANGE_SIZE;

        llam_metadata_write_u32(record + LLAM_METADATA_RANGE_OFFSET_START,
                                (uint32_t)ranges[index].start_offset);
        llam_metadata_write_u32(record + LLAM_METADATA_RANGE_OFFSET_END,
                                (uint32_t)ranges[index].end_offset);
        llam_metadata_write_u32(record + LLAM_METADATA_RANGE_OFFSET_FLAGS,
                                ranges[index].flags);
        llam_metadata_write_u32(record + LLAM_METADATA_RANGE_OFFSET_RESERVED,
                                ranges[index].reserved);
    }
    *written = total_size;
    return 0;
}

const char *llam_preempt_metadata_result_name(llam_preempt_metadata_result_t result) {
    switch (result) {
    case LLAM_PREEMPT_METADATA_OK:
        return "ok";
    case LLAM_PREEMPT_METADATA_ABSENT:
        return "absent";
    case LLAM_PREEMPT_METADATA_IGNORED_OPTIONAL:
        return "ignored-optional";
    case LLAM_PREEMPT_METADATA_INVALID:
        return "invalid";
    case LLAM_PREEMPT_METADATA_UNSUPPORTED:
        return "unsupported-required";
    default:
        return "unknown-result";
    }
}
