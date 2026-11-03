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

/* Private collector for an existing LLPM v1 record in a loaded PE image.
 * This is structural ingestion, not compiler emission or a proof of semantic
 * interruption safety. The caller owns a valid HMODULE reference at entry. */
#include "runtime_internal.h"
#include "windows_async_lifetime.h"
#include "metadata.h"

#define LLAM_WINDOWS_PE_MAX_HEADER_BYTES (1024U * 1024U)
#define LLAM_WINDOWS_PE_MAX_METADATA_BYTES (1024U * 1024U)
#define LLAM_WINDOWS_PE_MAX_SECTIONS 96U
#define LLAM_WINDOWS_PE_MAX_RECORDS 128U
#define LLAM_WINDOWS_PE_MAX_RANGES 128U

static const unsigned char g_llam_windows_pe_metadata_name[8] = {
    '.', 'l', 'l', 'a', 'm', 'p', 'm', 0
};

static bool llam_windows_pe_add(uintptr_t base, size_t offset,
                                uintptr_t *result) {
    if (result == NULL || offset > UINTPTR_MAX - base) {
        return false;
    }
    *result = base + offset;
    return true;
}

static bool llam_windows_pe_region_matches(HMODULE module, uintptr_t address,
                                           MEMORY_BASIC_INFORMATION *out) {
    DWORD protection;

    if (VirtualQuery((const void *)address, out, sizeof(*out)) == 0U ||
        out->State != MEM_COMMIT || out->Type != MEM_IMAGE ||
        out->AllocationBase != module || out->RegionSize == 0U ||
        (out->Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0U) {
        return false;
    }
    protection = out->Protect & 0xFFU;
    return protection == PAGE_READONLY ||
           protection == PAGE_READWRITE ||
           protection == PAGE_WRITECOPY ||
           protection == PAGE_EXECUTE_READ ||
           protection == PAGE_EXECUTE_READWRITE ||
           protection == PAGE_EXECUTE_WRITECOPY;
}

static bool llam_windows_pe_readable(HMODULE module, uintptr_t address,
                                     size_t length) {
    uintptr_t cursor = address;
    uintptr_t end;

    if (length == 0U || !llam_windows_pe_add(address, length, &end)) {
        return false;
    }
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info;
        uintptr_t region_base;
        uintptr_t region_end;

        if (!llam_windows_pe_region_matches(module, cursor, &info)) {
            return false;
        }
        region_base = (uintptr_t)info.BaseAddress;
        if (region_base > cursor ||
            !llam_windows_pe_add(region_base, info.RegionSize,
                                 &region_end) || region_end <= cursor) {
            return false;
        }
        cursor = region_end < end ? region_end : end;
    }
    return true;
}

static bool llam_windows_pe_copy(HMODULE module, uintptr_t address,
                                 void *destination, size_t length) {
    if (destination == NULL ||
        !llam_windows_pe_readable(module, address, length)) {
        return false;
    }
    memcpy(destination, (const void *)address, length);
    return true;
}

static bool llam_windows_pe_executable(HMODULE module, uintptr_t begin,
                                       uintptr_t end) {
    uintptr_t cursor = begin;

    if (begin == 0U || end <= begin) {
        return false;
    }
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info;
        uintptr_t region_base;
        uintptr_t region_end;
        DWORD protection;

        if (VirtualQuery((const void *)cursor, &info, sizeof(info)) == 0U ||
            info.State != MEM_COMMIT || info.Type != MEM_IMAGE ||
            info.AllocationBase != module || info.RegionSize == 0U ||
            (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0U) {
            return false;
        }
        protection = info.Protect & 0xFFU;
        if (protection != PAGE_EXECUTE &&
            protection != PAGE_EXECUTE_READ &&
            protection != PAGE_EXECUTE_READWRITE &&
            protection != PAGE_EXECUTE_WRITECOPY) {
            return false;
        }
        region_base = (uintptr_t)info.BaseAddress;
        if (region_base > cursor ||
            !llam_windows_pe_add(region_base, info.RegionSize,
                                 &region_end) || region_end <= cursor) {
            return false;
        }
        cursor = region_end < end ? region_end : end;
    }
    return true;
}

static uint32_t llam_windows_pe_read_u32(const unsigned char *bytes) {
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

static int llam_windows_pe_range_compare(const void *left, const void *right) {
    const llam_windows_async_preempt_range_t *a = left;
    const llam_windows_async_preempt_range_t *b = right;

    if (a->begin_pc < b->begin_pc) {
        return -1;
    }
    if (a->begin_pc > b->begin_pc) {
        return 1;
    }
    if (a->end_pc < b->end_pc) {
        return -1;
    }
    if (a->end_pc > b->end_pc) {
        return 1;
    }
    return 0;
}

void llam_windows_async_pe_release_ranges(
    llam_windows_async_pe_range_set_t *collected) {
    if (collected == NULL) {
        return;
    }
    free(collected->ranges);
    if (collected->held_module != NULL) {
        FreeLibrary(collected->held_module);
    }
    memset(collected, 0, sizeof(*collected));
}

int llam_windows_async_pe_collect_ranges(
    HMODULE caller_held_module,
    llam_windows_async_pe_range_set_t *out) {
    HMODULE held = NULL;
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS64 nt;
    IMAGE_SECTION_HEADER metadata_section;
    llam_windows_async_preempt_range_t *ranges = NULL;
    uintptr_t base;
    uintptr_t nt_address;
    uintptr_t section_table_address;
    uintptr_t metadata_address;
    size_t section_table_offset;
    size_t metadata_bytes;
    size_t offset = 0U;
    size_t selected = 0U;
    unsigned record_count = 0U;
    unsigned section_matches = 0U;
    unsigned i;
    int saved_errno = EINVAL;

    if (caller_held_module == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (out->held_module != NULL || out->ranges != NULL ||
        out->range_count != 0U) {
        errno = EBUSY;
        return -1;
    }
    /* Loader ownership is acquired before the first image-memory read.
     * A caller must itself keep the HMODULE valid until this call begins. */
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            (LPCSTR)(const void *)caller_held_module,
                            &held) || held == NULL) {
        errno = ENOTSUP;
        return -1;
    }
    if (held != caller_held_module) {
        saved_errno = EINVAL;
        goto fail;
    }
    base = (uintptr_t)held;
    if (!llam_windows_pe_copy(held, base, &dos, sizeof(dos)) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 ||
        (uint32_t)dos.e_lfanew > LLAM_WINDOWS_PE_MAX_HEADER_BYTES ||
        !llam_windows_pe_add(base, (size_t)dos.e_lfanew, &nt_address) ||
        !llam_windows_pe_copy(held, nt_address, &nt, sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64) ||
        nt.FileHeader.NumberOfSections == 0U ||
        nt.FileHeader.NumberOfSections > LLAM_WINDOWS_PE_MAX_SECTIONS ||
        nt.OptionalHeader.SizeOfHeaders == 0U ||
        nt.OptionalHeader.SizeOfHeaders > LLAM_WINDOWS_PE_MAX_HEADER_BYTES ||
        nt.OptionalHeader.SizeOfImage == 0U ||
        nt.OptionalHeader.SizeOfHeaders > nt.OptionalHeader.SizeOfImage ||
        (size_t)dos.e_lfanew > nt.OptionalHeader.SizeOfHeaders) {
        saved_errno = EINVAL;
        goto fail;
    }
    section_table_offset = (size_t)dos.e_lfanew + sizeof(DWORD) +
                           sizeof(IMAGE_FILE_HEADER) +
                           nt.FileHeader.SizeOfOptionalHeader;
    if (section_table_offset > nt.OptionalHeader.SizeOfHeaders ||
        (size_t)nt.FileHeader.NumberOfSections >
            (nt.OptionalHeader.SizeOfHeaders - section_table_offset) /
                sizeof(IMAGE_SECTION_HEADER) ||
        !llam_windows_pe_add(base, section_table_offset,
                             &section_table_address)) {
        saved_errno = EINVAL;
        goto fail;
    }
    for (i = 0U; i < nt.FileHeader.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER section;
        uintptr_t section_address;

        if (!llam_windows_pe_add(section_table_address,
                                 (size_t)i * sizeof(section),
                                 &section_address) ||
            !llam_windows_pe_copy(held, section_address,
                                  &section, sizeof(section))) {
            saved_errno = EINVAL;
            goto fail;
        }
        if (memcmp(section.Name, g_llam_windows_pe_metadata_name,
                   sizeof(section.Name)) == 0) {
            metadata_section = section;
            section_matches += 1U;
        }
    }
    if (section_matches == 0U) {
        FreeLibrary(held);
        return LLAM_WINDOWS_ASYNC_PE_METADATA_ABSENT;
    }
    if (section_matches != 1U ||
        (metadata_section.Characteristics &
         (IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ)) !=
            (IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ) ||
        (metadata_section.Characteristics &
         (IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE |
          IMAGE_SCN_MEM_DISCARDABLE)) != 0U ||
        metadata_section.Misc.VirtualSize == 0U ||
        metadata_section.SizeOfRawData == 0U ||
        metadata_section.VirtualAddress < nt.OptionalHeader.SizeOfHeaders ||
        (uint64_t)metadata_section.VirtualAddress +
                metadata_section.Misc.VirtualSize >
            nt.OptionalHeader.SizeOfImage) {
        saved_errno = EINVAL;
        goto fail;
    }
    metadata_bytes = metadata_section.Misc.VirtualSize <
                             metadata_section.SizeOfRawData
                         ? metadata_section.Misc.VirtualSize
                         : metadata_section.SizeOfRawData;
    if (metadata_bytes < LLAM_PREEMPT_METADATA_HEADER_SIZE ||
        metadata_bytes > LLAM_WINDOWS_PE_MAX_METADATA_BYTES) {
        saved_errno = metadata_bytes > LLAM_WINDOWS_PE_MAX_METADATA_BYTES
                          ? EOVERFLOW
                          : EINVAL;
        goto fail;
    }
    /* metadata_bytes <= VirtualSize; the checked RVA + VirtualSize bound
     * therefore covers the payload. Walk its actual mapping before parsing. */
    if (!llam_windows_pe_add(base, metadata_section.VirtualAddress,
                             &metadata_address) ||
        !llam_windows_pe_readable(held, metadata_address,
                                  metadata_bytes)) {
        saved_errno = EINVAL;
        goto fail;
    }
    ranges = calloc(LLAM_WINDOWS_PE_MAX_RANGES, sizeof(*ranges));
    if (ranges == NULL) {
        saved_errno = ENOMEM;
        goto fail;
    }
    while (offset < metadata_bytes) {
        const unsigned char *record =
            (const unsigned char *)(metadata_address + offset);
        llam_preempt_metadata_t parsed;
        llam_preempt_metadata_result_t parse_result;
        size_t consumed = 0U;
        uint32_t declared_ranges;
        size_t j;

        if (metadata_bytes - offset < LLAM_PREEMPT_METADATA_HEADER_SIZE ||
            ++record_count > LLAM_WINDOWS_PE_MAX_RECORDS) {
            saved_errno = EINVAL;
            goto fail;
        }
        declared_ranges = llam_windows_pe_read_u32(record + 24U);
        if (declared_ranges > LLAM_WINDOWS_PE_MAX_RANGES - selected) {
            saved_errno = EOVERFLOW;
            goto fail;
        }
        errno = 0;
        parse_result = llam_preempt_metadata_parse_one(
            record, metadata_bytes - offset, true,
            LLAM_PREEMPT_METADATA_ARCH_X86_64, base,
            nt.OptionalHeader.SizeOfImage, &parsed, &consumed);
        if (parse_result != LLAM_PREEMPT_METADATA_OK || consumed == 0U) {
            saved_errno = parse_result == LLAM_PREEMPT_METADATA_UNSUPPORTED
                              ? ENOTSUP
                              : (errno != 0 ? errno : EINVAL);
            llam_preempt_metadata_destroy(&parsed);
            goto fail;
        }
        for (j = 0U; j < parsed.range_count; ++j) {
            const llam_preempt_metadata_range_t *range = &parsed.ranges[j];

            if ((range->flags & LLAM_PREEMPT_RANGE_ASYNC_SAFE) == 0U) {
                continue;
            }
            if (selected == LLAM_WINDOWS_PE_MAX_RANGES ||
                !llam_windows_pe_executable(held, range->start_pc,
                                            range->end_pc)) {
                saved_errno = selected == LLAM_WINDOWS_PE_MAX_RANGES
                                  ? EOVERFLOW
                                  : ENOTSUP;
                llam_preempt_metadata_destroy(&parsed);
                goto fail;
            }
            ranges[selected].begin_pc = range->start_pc;
            ranges[selected].end_pc = range->end_pc;
            selected += 1U;
        }
        llam_preempt_metadata_destroy(&parsed);
        offset += consumed;
    }
    if (selected == 0U) {
        saved_errno = ENOTSUP;
        goto fail;
    }
    qsort(ranges, selected, sizeof(*ranges),
          llam_windows_pe_range_compare);
    for (i = 1U; i < selected; ++i) {
        if (ranges[i - 1U].end_pc > ranges[i].begin_pc) {
            saved_errno = EINVAL;
            goto fail;
        }
    }
    out->held_module = held;
    out->ranges = ranges;
    out->range_count = selected;
    return 0;

fail:
    free(ranges);
    FreeLibrary(held);
    errno = saved_errno;
    return -1;
}
