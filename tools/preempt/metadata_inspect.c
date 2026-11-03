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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LLAM_METADATA_INSPECT_MAX_BYTES (64U * 1024U * 1024U)

static void print_flags(uint32_t flags) {
    bool wrote_flag = false;

    if ((flags & LLAM_PREEMPT_RANGE_ASYNC_SAFE) != 0U) {
        (void)fputs("ASYNC_SAFE", stdout);
        wrote_flag = true;
    }
    if ((flags & LLAM_PREEMPT_RANGE_COOPERATIVE_POLL) != 0U) {
        (void)printf("%sCOOPERATIVE_POLL", wrote_flag ? "," : "");
        wrote_flag = true;
    }
    if ((flags & LLAM_PREEMPT_RANGE_RUNTIME_UNSAFE) != 0U) {
        (void)printf("%sRUNTIME_UNSAFE", wrote_flag ? "," : "");
        wrote_flag = true;
    }
    if ((flags & LLAM_PREEMPT_RANGE_FOREIGN_CALL) != 0U) {
        (void)printf("%sFOREIGN_CALL", wrote_flag ? "," : "");
        wrote_flag = true;
    }
    if ((flags & LLAM_PREEMPT_RANGE_STACK_POINTER_UNSTABLE) != 0U) {
        (void)printf("%sSTACK_POINTER_UNSTABLE", wrote_flag ? "," : "");
        wrote_flag = true;
    }
    if ((flags & LLAM_PREEMPT_RANGE_THREAD_AFFINE) != 0U) {
        (void)printf("%sTHREAD_AFFINE", wrote_flag ? "," : "");
        wrote_flag = true;
    }
    if ((flags & LLAM_PREEMPT_RANGE_NO_PREEMPT) != 0U) {
        (void)printf("%sNO_PREEMPT", wrote_flag ? "," : "");
        wrote_flag = true;
    }
    if (!wrote_flag) {
        (void)fputs("none", stdout);
    }
}

static void print_header_flags(uint32_t flags) {
    bool wrote_flag = false;

    if ((flags & LLAM_PREEMPT_METADATA_F_REQUIRED) != 0U) {
        (void)fputs("REQUIRED", stdout);
        wrote_flag = true;
    }
    if ((flags & LLAM_PREEMPT_METADATA_F_COOPERATIVE_POLLS) != 0U) {
        (void)printf("%sCOOPERATIVE_POLLS", wrote_flag ? "," : "");
        wrote_flag = true;
    }
    if ((flags & LLAM_PREEMPT_METADATA_F_ASYNC_RANGES) != 0U) {
        (void)printf("%sASYNC_RANGES", wrote_flag ? "," : "");
        wrote_flag = true;
    }
    if (!wrote_flag) {
        (void)fputs("none", stdout);
    }
}

int main(int argc, char **argv) {
    bool required = false;
    const char *path;
    FILE *file;
    unsigned char *bytes;
    long file_size;
    size_t offset = 0U;
    size_t map_number = 0U;
    int result = 0;
    bool found_required_async_range = false;

    if (argc == 3 && strcmp(argv[1], "--required") == 0) {
        required = true;
        path = argv[2];
    } else if (argc == 2) {
        path = argv[1];
    } else {
        (void)fprintf(stderr,
                      "usage: %s [--required] <raw-.llam.preempt-section>\n",
                      argv[0]);
        return 2;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        (void)perror(path);
        return 2;
    }
    if (fseek(file, 0L, SEEK_END) != 0 || (file_size = ftell(file)) < 0L ||
        (unsigned long)file_size > LLAM_METADATA_INSPECT_MAX_BYTES ||
        fseek(file, 0L, SEEK_SET) != 0) {
        (void)fprintf(stderr, "%s: invalid or oversized metadata input\n", path);
        (void)fclose(file);
        return 2;
    }
    if (file_size == 0L) {
        llam_preempt_metadata_t metadata;
        size_t consumed = 0U;
        llam_preempt_metadata_result_t parsed =
            llam_preempt_metadata_parse_one(NULL,
                                            0U,
                                            required,
                                            LLAM_PREEMPT_METADATA_ARCH_UNKNOWN,
                                            0U,
                                            0U,
                                            &metadata,
                                            &consumed);

        (void)fclose(file);
        (void)printf("%s: %s (legacy cooperative behavior)\n",
                     path,
                     llam_preempt_metadata_result_name(parsed));
        if (required) {
            (void)fprintf(stderr,
                          "%s: required mode found no verified async-safe range\n",
                          path);
            return 3;
        }
        return parsed == LLAM_PREEMPT_METADATA_UNSUPPORTED ? 3 : 0;
    }
    bytes = malloc((size_t)file_size);
    if (bytes == NULL) {
        (void)fprintf(stderr, "%s: out of memory\n", path);
        (void)fclose(file);
        return 2;
    }
    if (fread(bytes, 1U, (size_t)file_size, file) != (size_t)file_size) {
        (void)fprintf(stderr, "%s: short metadata read\n", path);
        free(bytes);
        (void)fclose(file);
        return 2;
    }
    (void)fclose(file);

    while (offset < (size_t)file_size) {
        llam_preempt_metadata_t metadata;
        size_t consumed = 0U;
        llam_preempt_metadata_result_t parsed = llam_preempt_metadata_parse_one(
            bytes + offset,
            (size_t)file_size - offset,
            required,
            LLAM_PREEMPT_METADATA_ARCH_UNKNOWN,
            0U,
            0U,
            &metadata,
            &consumed);
        size_t range_index;

        if (parsed == LLAM_PREEMPT_METADATA_ABSENT || consumed == 0U) {
            (void)fprintf(stderr, "%s: trailing bytes are not a metadata record\n", path);
            result = 2;
            break;
        }
        if (parsed == LLAM_PREEMPT_METADATA_INVALID ||
            parsed == LLAM_PREEMPT_METADATA_UNSUPPORTED) {
            (void)fprintf(stderr,
                          "%s: record %zu: %s\n",
                          path,
                          map_number,
                          llam_preempt_metadata_result_name(parsed));
            llam_preempt_metadata_destroy(&metadata);
            result = parsed == LLAM_PREEMPT_METADATA_UNSUPPORTED ? 3 : 2;
            break;
        }
        if (parsed == LLAM_PREEMPT_METADATA_OK) {
            if ((metadata.flags & LLAM_PREEMPT_METADATA_F_ASYNC_RANGES) != 0U &&
                metadata.range_count != 0U) {
                found_required_async_range = true;
            }
            (void)printf("record %zu: version=%u arch=%u build_id=0x%016llx "
                         "flags=0x%08x[",
                         map_number,
                         (unsigned)metadata.version,
                         (unsigned)metadata.architecture,
                         (unsigned long long)metadata.build_id,
                         metadata.flags);
            print_header_flags(metadata.flags);
            (void)printf("] ranges=%zu ignored_optional=%zu\n",
                         metadata.range_count,
                         metadata.ignored_optional_ranges);
            for (range_index = 0U; range_index < metadata.range_count; ++range_index) {
                const llam_preempt_metadata_range_t *range =
                    &metadata.ranges[range_index];

                (void)printf("  range[%zu] pc=[0x%llx,0x%llx) flags=",
                             range_index,
                             (unsigned long long)range->start_pc,
                             (unsigned long long)range->end_pc);
                print_flags(range->flags);
                (void)fputc('\n', stdout);
            }
        } else {
            (void)printf("record %zu: %s (legacy cooperative behavior)\n",
                         map_number,
                         llam_preempt_metadata_result_name(parsed));
        }
        llam_preempt_metadata_destroy(&metadata);
        offset += consumed;
        map_number += 1U;
    }
    free(bytes);
    if (required && result == 0 && !found_required_async_range) {
        (void)fprintf(stderr,
                      "%s: required mode found no verified async-safe range\n",
                      path);
        result = 3;
    }
    return result;
}
