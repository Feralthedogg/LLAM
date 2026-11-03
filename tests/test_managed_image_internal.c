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
#include "managed_image.h"

#if !LLAM_PLATFORM_WINDOWS
#include <dlfcn.h>
#endif
#include <stdio.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "managed image:%d: %s (errno=%d)\n", __LINE__, #condition, errno); return 1; \
} } while (0)

typedef unsigned long long (*image_leaf_fn)(unsigned long long);
typedef void (*image_observe_fn)(unsigned *);

static int discovered(const void *metadata, void *opaque) {
    uintptr_t *expected_and_count = opaque;
    if ((uintptr_t)metadata == expected_and_count[0]) ++expected_and_count[1];
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
#if LLAM_PLATFORM_WINDOWS
    (void)argv;
    return 77; /* Windows has the PE-specific native loader oracle. */
#else
    /* AUTO enumeration must ignore ordinary host/libc images, even when their
     * native loader format does not satisfy the stronger LLPM profile. */
    llam_managed_image_t rejected;
    void *native_symbol = dlsym(RTLD_DEFAULT, "malloc");
    CHECK(native_symbol != NULL);
    CHECK(llam_managed_image_open(native_symbol, true, &rejected) == -1 && errno == ENOENT);
    CHECK(rejected.native_pin == NULL && rejected.metadata == 0U);
    CHECK(llam_managed_image_open((const void *)(uintptr_t)1U, false, &rejected) == -1 && errno == ESRCH);
    CHECK(rejected.native_pin == NULL);
    CHECK(llam_managed_image_open((const void *)(uintptr_t)1U, true, &rejected) == -1 && errno == EINVAL);
    void *module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    CHECK(module != NULL);
    void *leaf_address = dlsym(module, "managed_image_leaf");
    void *observe_address = dlsym(module, "managed_image_observe_unload");
    CHECK(leaf_address != NULL && observe_address != NULL);
    image_leaf_fn leaf;
    image_observe_fn observe;
    _Static_assert(sizeof(leaf) == sizeof(leaf_address), "qualified native pointer ABI");
    memcpy(&leaf, &leaf_address, sizeof(leaf));
    memcpy(&observe, &observe_address, sizeof(observe));
    unsigned unloaded = 0U;
    observe(&unloaded);
    CHECK(leaf(9U) == 66U);

    llam_managed_image_t image;
    CHECK(llam_managed_image_open(leaf_address, true, &image) == 0);
    CHECK(image.metadata != 0U && image.metadata_size >= LLAM_LLPM2_HEADER_BYTES);
    uintptr_t guards[2];
    llam_native_domain_guard_identities(guards);
    llam_managed_image_records_t records;
    CHECK(llam_managed_image_validate(&image, guards, &records) == 0);
    CHECK(records.count >= 3U && records.executable_identity != 0U);
    llam_managed_image_records_destroy(&records);

    uintptr_t wrong[2] = {guards[0] + 1U, guards[1]};
    CHECK(llam_managed_image_validate(&image, wrong, &records) == -1 && errno == EXDEV);
    CHECK(records.records == NULL && records.count == 0U);
    llam_managed_image_t changed = image;
    changed.build_id[0] ^= 1U;
    CHECK(llam_managed_image_validate(&changed, guards, &records) == -1 && errno == EINVAL);
    changed = image;
    changed.metadata_size -= 1U;
    CHECK(llam_managed_image_validate(&changed, guards, &records) == -1);
    unsigned char *blob = malloc(image.metadata_size);
    CHECK(blob != NULL);
    memcpy(blob, (const void *)image.metadata, image.metadata_size);
    CHECK(llam_managed_image_open(blob, true, &rejected) == -1 && rejected.native_pin == NULL);
    free(blob);

    uintptr_t seen[2] = {image.metadata, 0U};
    CHECK(llam_managed_image_discover(discovered, seen) == 0 && seen[1] == 1U);
    CHECK(dlclose(module) == 0 && unloaded == 0U);
    CHECK(leaf(10U) == 73U); /* Only LLAM's independent loader reference remains. */
    CHECK(llam_managed_image_validate(&image, guards, &records) == 0);
    llam_managed_image_records_destroy(&records);
    llam_managed_image_close(&image);
    CHECK(unloaded == 1U && image.native_pin == NULL);
    CHECK(llam_managed_image_open(leaf_address, false, &rejected) == -1 && errno == ESRCH);
    CHECK(rejected.native_pin == NULL && rejected.metadata == 0U);
    puts("managed image: native exclusion, seal, guards, discovery, pin and expired snapshot passed");
    return 0;
#endif
}
