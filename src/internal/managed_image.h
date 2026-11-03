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

#ifndef LLAM_MANAGED_IMAGE_H
#define LLAM_MANAGED_IMAGE_H

#include "metadata_v2.h"

#define LLAM_MANAGED_IMAGE_MAX_SECTIONS 96U
#define LLAM_MANAGED_IMAGE_MAX_RECORDS 4096U
#define LLAM_MANAGED_IMAGE_MAX_EXEC_BYTES (256U * 1024U * 1024U)

typedef struct llam_managed_image_section {
    uintptr_t begin;
    size_t size;
    bool executable;
    bool readonly_after_relocation;
} llam_managed_image_section_t;

typedef struct llam_managed_image {
    void *native_pin;
    uintptr_t base;
    size_t size;
    uintptr_t metadata;
    size_t metadata_size;
    uint16_t architecture;
    uint16_t native_abi;
    unsigned char build_id[16];
    size_t section_count;
    llam_managed_image_section_t sections[LLAM_MANAGED_IMAGE_MAX_SECTIONS];
} llam_managed_image_t;

typedef struct llam_managed_image_records {
    size_t count;
    llam_llpm2_t *records;
    uint64_t executable_identity;
} llam_managed_image_records_t;

/* Cold ordinary context only. No LLAM/loader lock may be held by the caller.
 * caller_reference guarantees the host retains its original image reference
 * until this function returns. Auto discovery passes false and must tolerate
 * images disappearing between snapshot and independent pin acquisition.
 * Returns ENOENT for a native image without LLPM2; never accepts a heap blob.
 * Every failure leaves an empty output and releases its own native reference. */
int llam_managed_image_open(const void *address, bool caller_reference, llam_managed_image_t *out);
void llam_managed_image_close(llam_managed_image_t *image);
uintptr_t llam_managed_image_symbol(const llam_managed_image_t *image, const char *name);
bool llam_managed_image_readonly(const llam_managed_image_t *image, uintptr_t address, size_t bytes);
bool llam_managed_image_guard_matches(const llam_managed_image_t *image,
                                       uintptr_t actual, uintptr_t expected, unsigned guard_index);
bool llam_managed_image_import_matches(const llam_managed_image_t *image,
                                        uintptr_t actual, uintptr_t expected, const char *symbol_name);
bool llam_managed_image_same_provider(uintptr_t first, uintptr_t second);
int llam_managed_image_discover(int (*visitor)(const void *, void *), void *opaque);

/* Loaded code is checked against OS section bounds, native build identity,
 * contribution anchors, immutable guard bindings and the final-link seal.
 * expected_guards may be NULL for image inspection; runtime admission MUST
 * supply its own noninterposable compiler-ABI entry identities. */
int llam_managed_image_validate(const llam_managed_image_t *image,
                                 const uintptr_t expected_guards[2],
                                 llam_managed_image_records_t *out);
void llam_managed_image_records_destroy(llam_managed_image_records_t *records);
bool llam_managed_image_executable(const llam_managed_image_t *image, uintptr_t address, size_t bytes);
bool llam_managed_image_contains(const llam_managed_image_t *image, uintptr_t address, size_t bytes);
void llam_managed_image_anchor_name(const unsigned char id[16], char out[55]);

#endif /* LLAM_MANAGED_IMAGE_H */
