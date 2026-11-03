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

#include "runtime_internal.h"
#include "metadata.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned g_failures;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "test_preempt_metadata_registration: %s\n", message);
        g_failures += 1U;
    }
}

#if defined(__linux__) && defined(__x86_64__) && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
static int32_t relative_offset(const unsigned char *record,
                               size_t field_offset,
                               const unsigned char *target) {
    intptr_t field = (intptr_t)(uintptr_t)(record + field_offset);
    intptr_t destination = (intptr_t)(uintptr_t)target;

    return (int32_t)(destination - field);
}

static size_t encode_async_record(unsigned char *record,
                                  size_t capacity,
                                  const unsigned char *image,
                                  size_t image_size) {
    llam_preempt_metadata_range_input_t range;
    size_t written = 0U;

    memset(&range, 0, sizeof(range));
    range.start_offset = relative_offset(record, LLAM_PREEMPT_METADATA_HEADER_SIZE,
                                          image + 8U);
    range.end_offset = relative_offset(record, LLAM_PREEMPT_METADATA_HEADER_SIZE + 4U,
                                        image + image_size - 8U);
    range.flags = LLAM_PREEMPT_RANGE_ASYNC_SAFE;
    if (llam_preempt_metadata_encode(LLAM_PREEMPT_METADATA_ARCH_X86_64,
                                     UINT64_C(0x1001),
                                     LLAM_PREEMPT_METADATA_F_ASYNC_RANGES,
                                     &range,
                                     1U,
                                     record,
                                     capacity,
                                     &written) != 0) {
        return 0U;
    }
    return written;
}

static size_t encode_cooperative_record(unsigned char *record,
                                        size_t capacity,
                                        const unsigned char *image) {
    llam_preempt_metadata_range_input_t range;
    size_t written = 0U;

    memset(&range, 0, sizeof(range));
    range.start_offset = relative_offset(record,
                                          LLAM_PREEMPT_METADATA_HEADER_SIZE,
                                          image + 40U);
    range.end_offset = relative_offset(record,
                                        LLAM_PREEMPT_METADATA_HEADER_SIZE + 4U,
                                        image + 48U);
    range.flags = LLAM_PREEMPT_RANGE_COOPERATIVE_POLL;
    if (llam_preempt_metadata_encode(LLAM_PREEMPT_METADATA_ARCH_X86_64,
                                     UINT64_C(0x1002),
                                     LLAM_PREEMPT_METADATA_F_COOPERATIVE_POLLS,
                                     &range,
                                     1U,
                                     record,
                                     capacity,
                                     &written) != 0) {
        return 0U;
    }
    return written;
}
#endif

int main(void) {
#if defined(__linux__) && defined(__x86_64__) && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
    llam_runtime_opts_t options;
    llam_runtime_t *runtime = NULL;
    unsigned char image[128];
    unsigned char record[LLAM_PREEMPT_METADATA_HEADER_SIZE +
                         LLAM_PREEMPT_METADATA_RANGE_SIZE];
    unsigned char cooperative[LLAM_PREEMPT_METADATA_HEADER_SIZE +
                               LLAM_PREEMPT_METADATA_RANGE_SIZE];
    size_t record_size;
    size_t cooperative_size;
    int saved_errno;

    memset(image, 0, sizeof(image));
    memset(record, 0, sizeof(record));
    memset(cooperative, 0, sizeof(cooperative));
    record_size = encode_async_record(record, sizeof(record), image, sizeof(image));
    cooperative_size = encode_cooperative_record(cooperative,
                                                  sizeof(cooperative),
                                                  image);
    check(record_size != 0U && cooperative_size != 0U,
          "metadata fixtures could not be encoded");
    if (record_size == 0U || cooperative_size == 0U ||
        llam_runtime_opts_init(&options, sizeof(options)) != 0) {
        return 1;
    }
    options.deterministic = 1U;
    options.preempt_mode = LLAM_PREEMPT_AUTO;
    check(llam_runtime_create(&options, sizeof(options), &runtime) == 0 && runtime != NULL,
          "runtime creation failed");
    if (runtime == NULL) {
        return 1;
    }

    check(llam_preempt_module_register(runtime,
                                       (uintptr_t)image,
                                       sizeof(image),
                                       record,
                                       record_size,
                                       true) == 0,
          "required async metadata registration failed");
    check(runtime->async_preempt_range_count == 1U &&
              runtime->async_preempt_ranges[0].begin == (uintptr_t)(image + 8U) &&
              runtime->async_preempt_ranges[0].end == (uintptr_t)(image + sizeof(image) - 8U),
          "registered range did not resolve against the module image");
    check(atomic_load_explicit(&runtime->preempt_capability.registered_safe_ranges,
                               memory_order_acquire) == 1U,
          "capability record did not publish the module range");

    saved_errno = errno;
    check(llam_preempt_module_register(runtime,
                                       (uintptr_t)image,
                                       sizeof(image),
                                       record,
                                       record_size,
                                       true) != 0 && errno == EEXIST,
          "duplicate module range was accepted");
    check(runtime->async_preempt_range_count == 1U,
          "duplicate module registration changed the range set");
    errno = saved_errno;

    check(llam_preempt_module_register(runtime,
                                       (uintptr_t)image,
                                       sizeof(image),
                                       cooperative,
                                       cooperative_size,
                                       false) == 0 &&
              runtime->async_preempt_range_count == 1U,
          "optional cooperative-only metadata did not preserve the range set");
    check(llam_preempt_module_register(runtime,
                                       (uintptr_t)image,
                                       sizeof(image),
                                       cooperative,
                                       cooperative_size,
                                       true) != 0 && errno == ENOTSUP,
          "required cooperative-only metadata did not fail closed");
    check(llam_preempt_module_register(runtime,
                                       (uintptr_t)image,
                                       sizeof(image),
                                       NULL,
                                       0U,
                                       false) == 0,
          "optional absent metadata did not preserve legacy behavior");
    check(llam_preempt_module_register(runtime,
                                       (uintptr_t)image,
                                       sizeof(image),
                                       NULL,
                                       0U,
                                       true) != 0 && errno == ENOTSUP,
          "required absent metadata did not fail closed");
    llam_runtime_destroy(runtime);
#else
    check(llam_preempt_module_register(NULL, 0U, 0U, NULL, 0U, false) == 0,
          "optional unsupported module metadata did not downgrade");
    check(llam_preempt_module_register(NULL, 0U, 0U, NULL, 0U, true) != 0 &&
              errno == ENOTSUP,
          "required unsupported module metadata did not return ENOTSUP");
#endif

    if (g_failures != 0U) {
        fprintf(stderr,
                "test_preempt_metadata_registration: %u failure(s)\n",
                g_failures);
        return 1;
    }
    puts("test_preempt_metadata_registration: module registration and fallback passed");
    return 0;
}
