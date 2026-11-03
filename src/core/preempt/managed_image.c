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

#include "runtime_internal.h"
#include "managed_image.h"
#if !LLAM_PLATFORM_WINDOWS
#include <dlfcn.h>
#endif

static bool range_contains(uintptr_t begin, size_t size, uintptr_t address, size_t bytes) {
    return address >= begin && bytes <= size && address - begin <= size - bytes;
}

bool llam_managed_image_contains(const llam_managed_image_t *image, uintptr_t address, size_t bytes) {
    return image != NULL && range_contains(image->base, image->size, address, bytes);
}

bool llam_managed_image_executable(const llam_managed_image_t *image, uintptr_t address, size_t bytes) {
    if (image == NULL || bytes == 0U) return false;
    unsigned matches = 0U;
    for (size_t i = 0U; i < image->section_count; ++i) {
        const llam_managed_image_section_t *s = &image->sections[i];
        if (s->executable && range_contains(s->begin, s->size, address, bytes)) ++matches;
    }
    return matches == 1U;
}

void llam_managed_image_anchor_name(const unsigned char id[16], char out[55]) {
    static const char hex[] = "0123456789abcdef";
    memcpy(out, "__llam_managed_anchor_", 22U);
    for (size_t i = 0U; i < 16U; ++i) {
        out[22U + i * 2U] = hex[id[i] >> 4U];
        out[23U + i * 2U] = hex[id[i] & 15U];
    }
    out[54U] = '\0';
}

static int section_compare(const void *a, const void *b) {
    const llam_managed_image_section_t *left = a;
    const llam_managed_image_section_t *right = b;
    return left->begin < right->begin ? -1 : left->begin != right->begin;
}

static uint64_t hash_byte(uint64_t hash, unsigned char byte) {
    return (hash ^ byte) * UINT64_C(1099511628211);
}

static int executable_identity(const llam_managed_image_t *image, uint64_t *out) {
    llam_managed_image_section_t sections[LLAM_MANAGED_IMAGE_MAX_SECTIONS];
    size_t count = 0U;
    size_t charge = 0U;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0U; i < image->section_count; ++i) {
        if (image->sections[i].executable && image->sections[i].size != 0U) sections[count++] = image->sections[i];
    }
    if (count == 0U) { errno = EINVAL; return -1; }
    qsort(sections, count, sizeof(*sections), section_compare);
    uintptr_t previous_end = image->base;
    for (size_t i = 0U; i < count; ++i) {
        const llam_managed_image_section_t *s = &sections[i];
        if (s->begin < previous_end || !s->readonly_after_relocation ||
            !llam_managed_image_contains(image, s->begin, s->size) ||
            !llam_managed_image_readonly(image, s->begin, s->size) ||
            s->size > LLAM_MANAGED_IMAGE_MAX_EXEC_BYTES - charge) { errno = EINVAL; return -1; }
        charge += s->size;
        previous_end = s->begin + s->size;
        uint64_t rva = s->begin - image->base;
        for (unsigned j = 0U; j < 8U; ++j) hash = hash_byte(hash, (unsigned char)(rva >> (j * 8U)));
        for (unsigned j = 0U; j < 8U; ++j) hash = hash_byte(hash, (unsigned char)((uint64_t)s->size >> (j * 8U)));
        const unsigned char *code = (const unsigned char *)s->begin;
        for (size_t j = 0U; j < s->size; ++j) hash = hash_byte(hash, code[j]);
    }
    *out = hash;
    return 0;
}

void llam_managed_image_records_destroy(llam_managed_image_records_t *set) {
    if (set == NULL) return;
    for (size_t i = 0U; i < set->count; ++i) llam_llpm2_destroy(&set->records[i]);
    free(set->records);
    memset(set, 0, sizeof(*set));
}

typedef struct image_anchor {
    unsigned char id[16];
    uintptr_t record;
} image_anchor_t;

static int anchor_compare(const void *a, const void *b) {
    const image_anchor_t *left = a;
    const image_anchor_t *right = b;
    int order = memcmp(left->id, right->id, sizeof(left->id));
    return order != 0 ? order : (left->record < right->record ? -1 : left->record != right->record);
}

static bool validate_entry(const llam_managed_image_t *image, const llam_llpm2_entry_t *entry,
                            const uintptr_t expected_guards[2]) {
    if (!llam_managed_image_executable(image, entry->native_entry, 1U) ||
        !llam_managed_image_executable(image, entry->code_begin, entry->code_end - entry->code_begin) ||
        (entry->native_end != 0U &&
         !llam_managed_image_executable(image, entry->native_entry, entry->native_end - entry->native_entry))) return false;
    if (entry->guard_bindings != 0U) {
        if (!llam_managed_image_readonly(image, entry->guard_bindings, 16U)) return false;
        if (expected_guards != NULL) {
            uintptr_t actual[2];
            _Static_assert(sizeof(actual) == 16U, "managed images require 64-bit function pointers");
            memcpy(actual, (const void *)entry->guard_bindings, sizeof(actual));
            if (!llam_managed_image_guard_matches(image, actual[0], expected_guards[0], 0U) ||
                !llam_managed_image_guard_matches(image, actual[1], expected_guards[1], 1U)) {
                errno = EXDEV;
                return false;
            }
        }
    }
    return true;
}

#if !LLAM_PLATFORM_WINDOWS
bool llam_managed_image_guard_matches(const llam_managed_image_t *image,
                                       uintptr_t actual, uintptr_t expected, unsigned guard_index) {
    (void)image;
    return guard_index < 2U && actual == expected;
}

bool llam_managed_image_import_matches(const llam_managed_image_t *image,
                                        uintptr_t actual, uintptr_t expected, const char *symbol_name) {
    (void)image;
    return symbol_name != NULL && actual == expected;
}

bool llam_managed_image_same_provider(uintptr_t first, uintptr_t second) {
    Dl_info a, b;
    return first != 0U && second != 0U && dladdr((const void *)first, &a) != 0 &&
        dladdr((const void *)second, &b) != 0 && a.dli_fbase == b.dli_fbase;
}
#endif

static bool runtime_import(uint64_t contract_id, uintptr_t *address, const char **symbol_name) {
    void (*function)(void) = NULL;
    switch (contract_id) {
#define LLAM_RUNTIME_CONTRACT(id, name, signature, effect) \
    case id: function = (void (*)(void))name; *symbol_name = #name; break;
#include "managed_runtime_contracts.def"
#undef LLAM_RUNTIME_CONTRACT
    default: return false;
    }
    _Static_assert(sizeof(function) == sizeof(*address), "runtime import function-pointer ABI");
    memcpy(address, &function, sizeof(function));
    return true;
}

static bool validate_runtime_import(const llam_managed_image_t *image, const llam_llpm2_call_t *call,
                                      const uintptr_t expected_guards[2]) {
    if ((call->flags & LLAM_LLPM2_CALL_RUNTIME_BODY) == 0U) return true;
    if (!llam_managed_image_readonly(image, call->native_target, sizeof(uintptr_t))) return false;
    uintptr_t expected = 0U, actual = 0U;
    const char *name = NULL;
    if (!runtime_import(call->contract_id, &expected, &name) ||
        call->effect != llam_llpm2_runtime_effect(call->contract_id) ||
        call->type_id != llam_llpm2_runtime_type_id(call->contract_id, image->architecture, image->native_abi)) {
        errno = ENOTSUP;
        return false;
    }
    if (expected_guards == NULL) return true; /* Structural inspection only. */
    memcpy(&actual, (const void *)call->native_target, sizeof(actual));
    if (!llam_managed_image_same_provider(expected, expected_guards[0]) ||
        !llam_managed_image_import_matches(image, actual, expected, name)) {
        errno = EXDEV;
        return false;
    }
    return true;
}

int llam_managed_image_validate(const llam_managed_image_t *image, const uintptr_t expected_guards[2],
                                 llam_managed_image_records_t *out) {
    llam_managed_image_records_t set = {0};
    image_anchor_t *anchors = NULL;
    size_t offset = 0U;
    int error = EINVAL;
    if (out != NULL) memset(out, 0, sizeof(*out));
    if (image == NULL || out == NULL || image->native_pin == NULL || image->section_count == 0U ||
        image->section_count > LLAM_MANAGED_IMAGE_MAX_SECTIONS || image->metadata_size == 0U ||
        image->metadata_size > LLAM_LLPM2_MAX_BYTES ||
        !llam_managed_image_readonly(image, image->metadata, image->metadata_size)) goto failed;
    if (expected_guards != NULL && (expected_guards[0] == 0U || expected_guards[1] == 0U)) goto failed;
    size_t capacity = image->metadata_size / (LLAM_LLPM2_HEADER_BYTES + LLAM_LLPM2_ENTRY_BYTES);
    if (capacity == 0U) { error = EINVAL; goto failed; }
    if (capacity > LLAM_MANAGED_IMAGE_MAX_RECORDS) capacity = LLAM_MANAGED_IMAGE_MAX_RECORDS;
    set.records = calloc(capacity, sizeof(*set.records));
    anchors = calloc(capacity, sizeof(*anchors));
    if (set.records == NULL || anchors == NULL) { error = ENOMEM; goto failed; }
    if (executable_identity(image, &set.executable_identity) != 0) { error = errno; goto failed; }
    const llam_llpm2_requirements_t requirements = {
        .architecture = image->architecture, .native_abi = image->native_abi,
        .runtime_major = 2U, .runtime_minor = 2U, .toolchain_id = LLAM_LLPM2_PROFILE_LLVM23,
        .required = true, .require_sealed = true
    };
    while (offset < image->metadata_size) {
        if (set.count == capacity) { error = E2BIG; goto failed; }
        llam_llpm2_t *record = &set.records[set.count];
        size_t consumed;
        llam_preempt_metadata_result_t result = llam_llpm2_parse((const void *)(image->metadata + offset),
            image->metadata_size - offset, image->metadata + offset, image->base, image->size,
            &requirements, record, &consumed);
        if (result != LLAM_PREEMPT_METADATA_OK) {
            error = result == LLAM_PREEMPT_METADATA_UNSUPPORTED ? ENOTSUP : EINVAL;
            goto failed;
        }
        anchors[set.count].record = image->metadata + offset;
        memcpy(anchors[set.count].id, record->header.anchor_id, 16U);
        ++set.count; // Every successfully allocated record is now an owned cleanup root.
        if (consumed == 0U || record->header.executable_identity != set.executable_identity ||
            memcmp(record->header.build_id, image->build_id, 16U) != 0) goto failed;
        for (size_t i = 0U; i < record->entry_count; ++i) {
            errno = EINVAL;
            if (!validate_entry(image, &record->entries[i], expected_guards)) { error = errno; goto failed; }
        }
        for (size_t i = 0U; i < record->call_count; ++i) {
            errno = EINVAL;
            if (!validate_runtime_import(image, &record->calls[i], expected_guards)) { error = errno; goto failed; }
        }
        offset += consumed;
    }
    qsort(anchors, set.count, sizeof(*anchors), anchor_compare);
    for (size_t i = 0U; i < set.count; ++i) {
        if (i != 0U && memcmp(anchors[i - 1U].id, anchors[i].id, 16U) == 0) continue;
        char name[55];
        llam_managed_image_anchor_name(anchors[i].id, name);
        if (llam_managed_image_symbol(image, name) != anchors[i].record) goto failed;
    }
    free(anchors);
    *out = set;
    return 0;
failed:
    free(anchors);
    llam_managed_image_records_destroy(&set);
    errno = error;
    return -1;
}
