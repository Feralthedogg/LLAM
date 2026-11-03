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

#if LLAM_PLATFORM_WINDOWS
#include <psapi.h>

#define LLAM_PE_MAX_HEADER_BYTES (1024U * 1024U)
#define LLAM_PE_MAX_SYMBOLS 65536U
#define LLAM_PE_MAX_IMPORTS 4096U
#define LLAM_PE_MAX_MODULES 4096U
#define LLAM_PE_MAX_SYMBOL_NAME 256U

typedef enum image_access {
    IMAGE_ACCESS_READABLE,
    IMAGE_ACCESS_READONLY,
    IMAGE_ACCESS_EXECUTABLE
} image_access_t;

typedef struct pe_headers {
    IMAGE_NT_HEADERS64 nt;
    uintptr_t section_table;
} pe_headers_t;

static bool image_add(uintptr_t base, size_t offset, uintptr_t *out) {
    if (offset > UINTPTR_MAX - base) return false;
    *out = base + offset;
    return true;
}

static bool mapped_range(HMODULE module, uintptr_t address, size_t bytes, image_access_t access) {
    uintptr_t end;
    if (module == NULL || bytes == 0U || !image_add(address, bytes, &end)) return false;
    while (address < end) {
        MEMORY_BASIC_INFORMATION info;
        if (VirtualQuery((const void *)address, &info, sizeof(info)) != sizeof(info) ||
            info.State != MEM_COMMIT || info.Type != MEM_IMAGE || info.AllocationBase != module ||
            (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0U) return false;
        DWORD protection = info.Protect & 0xffU;
        bool readable = protection == PAGE_READONLY || protection == PAGE_READWRITE ||
            protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_READ ||
            protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
        if (!readable || (access == IMAGE_ACCESS_READONLY &&
                protection != PAGE_READONLY && protection != PAGE_EXECUTE_READ) ||
            (access == IMAGE_ACCESS_EXECUTABLE && protection != PAGE_EXECUTE_READ)) return false;
        uintptr_t region = (uintptr_t)info.BaseAddress;
        uintptr_t region_end;
        if (region > address || !image_add(region, info.RegionSize, &region_end) || region_end <= address) return false;
        address = region_end < end ? region_end : end;
    }
    return true;
}

static bool copy_image(HMODULE module, uintptr_t address, void *out, size_t bytes) {
    SIZE_T copied = 0U;
    /* The loader reference prevents unload; the OS copy also fails cleanly if
     * another thread changes page protection between query and copy. */
    return mapped_range(module, address, bytes, IMAGE_ACCESS_READONLY) &&
        ReadProcessMemory(GetCurrentProcess(), (const void *)address, out, bytes, &copied) && copied == bytes;
}

bool llam_managed_image_readonly(const llam_managed_image_t *image, uintptr_t address, size_t bytes) {
    if (image == NULL || image->native_pin == NULL || bytes == 0U ||
        !llam_managed_image_contains(image, address, bytes)) return false;
    for (size_t i = 0U; i < image->section_count; ++i) {
        const llam_managed_image_section_t *section = &image->sections[i];
        if (section->readonly_after_relocation && address >= section->begin && bytes <= section->size &&
            address - section->begin <= section->size - bytes) {
            return mapped_range((HMODULE)image->native_pin, address, bytes, IMAGE_ACCESS_READONLY);
        }
    }
    return false;
}

static bool copy_readonly(const llam_managed_image_t *image, uintptr_t address, void *out, size_t bytes) {
    return llam_managed_image_readonly(image, address, bytes) &&
        copy_image((HMODULE)image->native_pin, address, out, bytes);
}

static bool copy_rva(const llam_managed_image_t *image, DWORD rva, void *out, size_t bytes) {
    uintptr_t address;
    return image_add(image->base, rva, &address) && copy_readonly(image, address, out, bytes);
}

static bool read_headers(HMODULE module, pe_headers_t *out) {
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS64 nt = {0};
    uintptr_t base = (uintptr_t)module;
    uintptr_t nt_address;
    uintptr_t optional_address;
    const size_t prefix = offsetof(IMAGE_NT_HEADERS64, OptionalHeader);
    const size_t minimum_optional = offsetof(IMAGE_OPTIONAL_HEADER64, DataDirectory);
    if (!copy_image(module, base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew < (LONG)sizeof(dos) || (DWORD)dos.e_lfanew > LLAM_PE_MAX_HEADER_BYTES ||
        !image_add(base, (size_t)dos.e_lfanew, &nt_address) ||
        !copy_image(module, nt_address, &nt, prefix) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || nt.FileHeader.NumberOfSections == 0U ||
        nt.FileHeader.NumberOfSections > LLAM_MANAGED_IMAGE_MAX_SECTIONS ||
        nt.FileHeader.SizeOfOptionalHeader < minimum_optional ||
        !image_add(nt_address, prefix, &optional_address)) return false;
    size_t optional_bytes = nt.FileHeader.SizeOfOptionalHeader;
    if (optional_bytes > sizeof(nt.OptionalHeader)) optional_bytes = sizeof(nt.OptionalHeader);
    if (!copy_image(module, optional_address, &nt.OptionalHeader, optional_bytes) ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.OptionalHeader.NumberOfRvaAndSizes > IMAGE_NUMBEROF_DIRECTORY_ENTRIES ||
        nt.OptionalHeader.NumberOfRvaAndSizes >
            (nt.FileHeader.SizeOfOptionalHeader - minimum_optional) / sizeof(IMAGE_DATA_DIRECTORY) ||
        nt.OptionalHeader.SizeOfHeaders == 0U || nt.OptionalHeader.SizeOfHeaders > LLAM_PE_MAX_HEADER_BYTES ||
        nt.OptionalHeader.SizeOfHeaders > nt.OptionalHeader.SizeOfImage ||
        nt.OptionalHeader.SizeOfImage > UINTPTR_MAX - base) return false;
    size_t table_offset = (size_t)dos.e_lfanew + prefix + nt.FileHeader.SizeOfOptionalHeader;
    if (table_offset > nt.OptionalHeader.SizeOfHeaders || nt.FileHeader.NumberOfSections >
        (nt.OptionalHeader.SizeOfHeaders - table_offset) / sizeof(IMAGE_SECTION_HEADER) ||
        !image_add(base, table_offset, &out->section_table)) return false;
    out->nt = nt;
    return true;
}

static bool image_directory(const llam_managed_image_t *image, const pe_headers_t *headers,
                            unsigned index, IMAGE_DATA_DIRECTORY *out) {
    if (headers->nt.OptionalHeader.NumberOfRvaAndSizes <= index) return false;
    *out = headers->nt.OptionalHeader.DataDirectory[index];
    uintptr_t address;
    return out->VirtualAddress != 0U && out->Size != 0U &&
        image_add(image->base, out->VirtualAddress, &address) &&
        llam_managed_image_readonly(image, address, out->Size);
}

static bool compare_image_name(const llam_managed_image_t *image, DWORD rva, const char *name,
                               size_t length, int *order) {
    unsigned char bytes[LLAM_PE_MAX_SYMBOL_NAME + 1U];
    uintptr_t address;
    if (!image_add(image->base, rva, &address)) return false;
    for (size_t i = 0U; i < image->section_count; ++i) {
        const llam_managed_image_section_t *section = &image->sections[i];
        if (address < section->begin || address - section->begin >= section->size) continue;
        size_t available = section->size - (address - section->begin);
        size_t count = available < length + 1U ? available : length + 1U;
        if (!copy_readonly(image, address, bytes, count)) return false;
        for (size_t j = 0U; j < count; ++j) {
            if (bytes[j] != (unsigned char)name[j]) {
                *order = bytes[j] < (unsigned char)name[j] ? -1 : 1;
                return true;
            }
            if (bytes[j] == 0U) { *order = 0; return true; }
        }
        return false;
    }
    return false;
}

uintptr_t llam_managed_image_symbol(const llam_managed_image_t *image, const char *name) {
    pe_headers_t headers;
    IMAGE_DATA_DIRECTORY directory;
    IMAGE_EXPORT_DIRECTORY exports;
    if (image == NULL || image->native_pin == NULL || name == NULL) return 0U;
    size_t length = strnlen(name, LLAM_PE_MAX_SYMBOL_NAME + 1U);
    if (length == 0U || length > LLAM_PE_MAX_SYMBOL_NAME ||
        !read_headers((HMODULE)image->native_pin, &headers) ||
        !image_directory(image, &headers, IMAGE_DIRECTORY_ENTRY_EXPORT, &directory) ||
        directory.Size < sizeof(exports) || !copy_rva(image, directory.VirtualAddress, &exports, sizeof(exports)) ||
        exports.NumberOfNames > exports.NumberOfFunctions || exports.NumberOfFunctions > LLAM_PE_MAX_SYMBOLS) return 0U;
    uintptr_t names, ordinals, functions;
    if (exports.NumberOfNames == 0U || !image_add(image->base, exports.AddressOfNames, &names) ||
        !image_add(image->base, exports.AddressOfNameOrdinals, &ordinals) ||
        !image_add(image->base, exports.AddressOfFunctions, &functions) ||
        !llam_managed_image_readonly(image, names, (size_t)exports.NumberOfNames * sizeof(DWORD)) ||
        !llam_managed_image_readonly(image, ordinals, (size_t)exports.NumberOfNames * sizeof(WORD)) ||
        !llam_managed_image_readonly(image, functions, (size_t)exports.NumberOfFunctions * sizeof(DWORD))) return 0U;
    size_t low = 0U, high = exports.NumberOfNames;
    while (low < high) {
        size_t middle = low + (high - low) / 2U;
        DWORD name_rva;
        int order;
        if (!copy_readonly(image, names + middle * sizeof(name_rva), &name_rva, sizeof(name_rva)) ||
            !compare_image_name(image, name_rva, name, length, &order)) return 0U;
        if (order < 0) { low = middle + 1U; continue; }
        if (order > 0) { high = middle; continue; }
        WORD ordinal;
        DWORD rva;
        if (!copy_readonly(image, ordinals + middle * sizeof(ordinal), &ordinal, sizeof(ordinal)) ||
            ordinal >= exports.NumberOfFunctions ||
            !copy_readonly(image, functions + ordinal * sizeof(rva), &rva, sizeof(rva)) || rva == 0U ||
            ((uint64_t)rva >= directory.VirtualAddress &&
             (uint64_t)rva - directory.VirtualAddress < directory.Size)) return 0U;
        uintptr_t address;
        return image_add(image->base, rva, &address) && llam_managed_image_contains(image, address, 1U) &&
            mapped_range((HMODULE)image->native_pin, address, 1U, IMAGE_ACCESS_READABLE) ? address : 0U;
    }
    return 0U;
}

static bool collect_build_id(llam_managed_image_t *image, const pe_headers_t *headers,
                             const IMAGE_SECTION_HEADER *sections) {
    IMAGE_DATA_DIRECTORY directory;
    if (!image_directory(image, headers, IMAGE_DIRECTORY_ENTRY_DEBUG, &directory) ||
        directory.Size % sizeof(IMAGE_DEBUG_DIRECTORY) != 0U ||
        directory.Size / sizeof(IMAGE_DEBUG_DIRECTORY) > LLAM_MANAGED_IMAGE_MAX_SECTIONS) return false;
    unsigned found = 0U;
    for (size_t offset = 0U; offset < directory.Size; offset += sizeof(IMAGE_DEBUG_DIRECTORY)) {
        IMAGE_DEBUG_DIRECTORY debug;
        if (!copy_rva(image, directory.VirtualAddress + (DWORD)offset, &debug, sizeof(debug))) return false;
        if (debug.Type != IMAGE_DEBUG_TYPE_CODEVIEW || debug.SizeOfData < 24U) continue;
        unsigned char codeview[24];
        if (!copy_rva(image, debug.AddressOfRawData, codeview, sizeof(codeview))) return false;
        if (memcmp(codeview, "RSDS", 4U) != 0) continue;
        if (++found != 1U) return false;
        bool file_matches = false;
        for (size_t i = 0U; i < headers->nt.FileHeader.NumberOfSections; ++i) {
            const IMAGE_SECTION_HEADER *section = &sections[i];
            if (debug.AddressOfRawData < section->VirtualAddress) continue;
            uint64_t delta = (uint64_t)debug.AddressOfRawData - section->VirtualAddress;
            if (delta <= section->SizeOfRawData && debug.SizeOfData <= section->SizeOfRawData - delta &&
                (uint64_t)section->PointerToRawData + delta == debug.PointerToRawData) file_matches = true;
        }
        if (!file_matches) return false;
        memcpy(image->build_id, codeview + 4U, sizeof(image->build_id));
    }
    return found == 1U;
}

static int collect_sections(llam_managed_image_t *image, const pe_headers_t *headers) {
    IMAGE_SECTION_HEADER sections[LLAM_MANAGED_IMAGE_MAX_SECTIONS];
    size_t count = headers->nt.FileHeader.NumberOfSections;
    if (!copy_image((HMODULE)image->native_pin, headers->section_table, sections, count * sizeof(*sections))) goto invalid;
    unsigned metadata_count = 0U;
    for (size_t i = 0U; i < count; ++i) {
        if (memcmp(sections[i].Name, ".llamm\0\0", IMAGE_SIZEOF_SHORT_NAME) == 0) ++metadata_count;
    }
    /* Native-only modules need not satisfy the managed compiler profile.
     * Discovery must not reject an unrelated native DLL for writable text. */
    if (metadata_count == 0U) { errno = ENOENT; return -1; }
    if (metadata_count != 1U) goto invalid;
    for (size_t i = 0U; i < count; ++i) {
        const IMAGE_SECTION_HEADER *section = &sections[i];
        size_t size = section->Misc.VirtualSize != 0U ? section->Misc.VirtualSize : section->SizeOfRawData;
        bool metadata = memcmp(section->Name, ".llamm\0\0", IMAGE_SIZEOF_SHORT_NAME) == 0;
        if (size == 0U) { if (metadata) goto invalid; else continue; }
        uintptr_t address;
        if (section->VirtualAddress < headers->nt.OptionalHeader.SizeOfHeaders ||
            !image_add(image->base, section->VirtualAddress, &address) ||
            !llam_managed_image_contains(image, address, size)) goto invalid;
        bool executable = (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0U;
        bool readonly = (section->Characteristics & IMAGE_SCN_MEM_READ) != 0U &&
            (section->Characteristics & (IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_DISCARDABLE)) == 0U;
        if (executable && (!readonly || size > section->SizeOfRawData ||
            !mapped_range((HMODULE)image->native_pin, address, size, IMAGE_ACCESS_EXECUTABLE))) goto invalid;
        for (size_t j = 0U; j < image->section_count; ++j) {
            const llam_managed_image_section_t *previous = &image->sections[j];
            if (address < previous->begin + previous->size && previous->begin < address + size) goto invalid;
        }
        image->sections[image->section_count++] = (llam_managed_image_section_t){
            .begin = address, .size = size, .executable = executable, .readonly_after_relocation = readonly
        };
        if (metadata) {
            if (executable || !readonly || size < LLAM_LLPM2_HEADER_BYTES || size > LLAM_LLPM2_MAX_BYTES ||
                size > section->SizeOfRawData ||
                (section->Characteristics & IMAGE_SCN_CNT_INITIALIZED_DATA) == 0U ||
                !llam_managed_image_readonly(image, address, size)) goto invalid;
            image->metadata = address;
            image->metadata_size = size;
        }
    }
    if (!collect_build_id(image, headers, sections)) goto invalid;
    return 0;
invalid:
    errno = EINVAL;
    return -1;
}

void llam_managed_image_close(llam_managed_image_t *image) {
    if (image == NULL) return;
    if (image->native_pin != NULL) (void)FreeLibrary((HMODULE)image->native_pin);
    memset(image, 0, sizeof(*image));
}

int llam_managed_image_open(const void *address, bool caller_reference, llam_managed_image_t *out) {
    llam_managed_image_t image = {0};
    HMODULE held = NULL;
    pe_headers_t headers;
    int error = EINVAL;
    (void)caller_reference;
    if (out != NULL) memset(out, 0, sizeof(*out));
    if (address == NULL || out == NULL) goto failed;
    /* FROM_ADDRESS increments the loader reference before any mapped read.
     * Auto snapshots may be stale; every descriptor below comes from this pin. */
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)address, &held) || held == NULL) {
        DWORD loader_error = GetLastError();
        error = loader_error == ERROR_MOD_NOT_FOUND ? ESRCH :
            (loader_error == ERROR_NOT_ENOUGH_MEMORY || loader_error == ERROR_OUTOFMEMORY ? ENOMEM : EIO);
        goto failed;
    }
    image.native_pin = held;
    image.base = (uintptr_t)held;
    if (!read_headers(held, &headers)) goto failed;
    image.size = headers.nt.OptionalHeader.SizeOfImage;
    image.architecture = LLAM_PREEMPT_METADATA_ARCH_X86_64;
    image.native_abi = LLAM_LLPM2_ABI_WIN64;
    if (!llam_managed_image_contains(&image, (uintptr_t)address, 1U) ||
        !mapped_range(held, (uintptr_t)address, 1U, IMAGE_ACCESS_READABLE)) goto failed;
    if (collect_sections(&image, &headers) != 0) { error = errno; goto failed; }
    unsigned char anchor_id[16];
    char anchor[55];
    if (!copy_readonly(&image, image.metadata + 112U, anchor_id, sizeof(anchor_id))) goto failed;
    llam_managed_image_anchor_name(anchor_id, anchor);
    if (llam_managed_image_symbol(&image, anchor) != image.metadata) goto failed;
    *out = image;
    return 0;
failed:
    llam_managed_image_close(&image);
    errno = error;
    return -1;
}

bool llam_managed_image_import_matches(const llam_managed_image_t *image, uintptr_t actual,
                                       uintptr_t expected, const char *symbol_name) {
    if (image == NULL || expected == 0U || symbol_name == NULL) return false;
    size_t name_length = strnlen(symbol_name, LLAM_PE_MAX_SYMBOL_NAME + 1U);
    if (name_length == 0U || name_length > LLAM_PE_MAX_SYMBOL_NAME) return false;
    if (actual == expected) return true;
    unsigned char code[6];
    if (!llam_managed_image_executable(image, actual, sizeof(code)) ||
        !mapped_range((HMODULE)image->native_pin, actual, sizeof(code), IMAGE_ACCESS_EXECUTABLE) ||
        !copy_readonly(image, actual, code, sizeof(code)) || code[0] != 0xffU || code[1] != 0x25U) return false;
    int32_t displacement;
    memcpy(&displacement, code + 2U, sizeof(displacement));
    uintptr_t slot = actual + sizeof(code);
    if (displacement < 0) {
        uintptr_t distance = (uintptr_t)-(int64_t)displacement;
        if (distance > slot) return false;
        slot -= distance;
    } else if (!image_add(slot, (size_t)displacement, &slot)) return false;
    uintptr_t target;
    pe_headers_t headers;
    IMAGE_DATA_DIRECTORY imports, iat;
    if (!copy_readonly(image, slot, &target, sizeof(target)) || target != expected ||
        llam_managed_image_executable(image, slot, sizeof(target)) ||
        !read_headers((HMODULE)image->native_pin, &headers) ||
        !image_directory(image, &headers, IMAGE_DIRECTORY_ENTRY_IMPORT, &imports) ||
        !image_directory(image, &headers, IMAGE_DIRECTORY_ENTRY_IAT, &iat) ||
        imports.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR) > LLAM_PE_MAX_IMPORTS ||
        slot < image->base + iat.VirtualAddress || sizeof(target) > iat.Size ||
        slot - (image->base + iat.VirtualAddress) > iat.Size - sizeof(target)) return false;
    unsigned matches = 0U;
    size_t inspected = 0U;
    for (size_t offset = 0U; sizeof(IMAGE_IMPORT_DESCRIPTOR) <= imports.Size - offset;
         offset += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        IMAGE_IMPORT_DESCRIPTOR descriptor;
        if (!copy_rva(image, imports.VirtualAddress + (DWORD)offset, &descriptor, sizeof(descriptor))) return false;
        if (descriptor.OriginalFirstThunk == 0U && descriptor.TimeDateStamp == 0U &&
            descriptor.ForwarderChain == 0U && descriptor.Name == 0U && descriptor.FirstThunk == 0U) return matches == 1U;
        uintptr_t first;
        if (descriptor.OriginalFirstThunk == 0U || descriptor.FirstThunk == 0U || descriptor.Name == 0U ||
            !image_add(image->base, descriptor.FirstThunk, &first) || slot < first ||
            (slot - first) % sizeof(uint64_t) != 0U) continue;
        size_t index = (slot - first) / sizeof(uint64_t);
        if (index >= LLAM_PE_MAX_SYMBOLS || index + 1U > LLAM_PE_MAX_SYMBOLS - inspected) return false;
        inspected += index + 1U;
        uintptr_t lookup;
        if (!image_add(image->base, descriptor.OriginalFirstThunk, &lookup) ||
            !llam_managed_image_readonly(image, lookup, (index + 1U) * sizeof(uint64_t))) continue;
        uint64_t imported = 0U;
        bool terminated = false;
        for (size_t i = 0U; i <= index; ++i) {
            if (!copy_readonly(image, lookup + i * sizeof(imported), &imported, sizeof(imported))) return false;
            if (imported == 0U) { terminated = true; break; }
        }
        if (terminated || (imported & IMAGE_ORDINAL_FLAG64) != 0U || imported > UINT32_MAX - sizeof(WORD)) continue;
        int order;
        if (!compare_image_name(image, (DWORD)imported + sizeof(WORD), symbol_name, name_length, &order)) return false;
        if (order == 0 && ++matches > 1U) return false;
    }
    return false;
}

bool llam_managed_image_guard_matches(const llam_managed_image_t *image, uintptr_t actual,
                                      uintptr_t expected, unsigned guard_index) {
    static const char *const guard_names[] = {"llam_managed_native_enter", "llam_managed_native_leave"};
    return guard_index < 2U && llam_managed_image_import_matches(image, actual, expected, guard_names[guard_index]);
}

bool llam_managed_image_same_provider(uintptr_t first, uintptr_t second) {
    MEMORY_BASIC_INFORMATION left, right;
    if (first == 0U || second == 0U ||
        VirtualQuery((const void *)first, &left, sizeof(left)) != sizeof(left) ||
        VirtualQuery((const void *)second, &right, sizeof(right)) != sizeof(right) ||
        left.State != MEM_COMMIT || right.State != MEM_COMMIT ||
        left.Type != MEM_IMAGE || right.Type != MEM_IMAGE || left.AllocationBase == NULL ||
        left.AllocationBase != right.AllocationBase ||
        ((left.Protect | right.Protect) & (PAGE_GUARD | PAGE_NOACCESS)) != 0U) return false;
    DWORD left_access = left.Protect & 0xffU;
    DWORD right_access = right.Protect & 0xffU;
    /* These are expected runtime entry PCs, whose owner retains their image.
     * Mapping identity is checked here; this helper does not acquire ownership. */
    return (left_access == PAGE_EXECUTE || left_access == PAGE_EXECUTE_READ) &&
        (right_access == PAGE_EXECUTE || right_access == PAGE_EXECUTE_READ);
}

int llam_managed_image_discover(int (*visitor)(const void *, void *), void *opaque) {
    if (visitor == NULL) { errno = EINVAL; return -1; }
    HMODULE *modules = calloc(LLAM_PE_MAX_MODULES, sizeof(*modules));
    if (modules == NULL) { errno = ENOMEM; return -1; }
    DWORD needed = 0U;
    if (!EnumProcessModules(GetCurrentProcess(), modules, LLAM_PE_MAX_MODULES * sizeof(*modules), &needed) ||
        needed % sizeof(*modules) != 0U || needed > LLAM_PE_MAX_MODULES * sizeof(*modules)) {
        free(modules);
        errno = needed > LLAM_PE_MAX_MODULES * sizeof(*modules) ? E2BIG : EAGAIN;
        return -1;
    }
    int result = 0;
    int error = 0;
    for (size_t i = 0U; i < needed / sizeof(*modules); ++i) {
        llam_managed_image_t image;
        if (llam_managed_image_open(modules[i], false, &image) != 0) {
            if (errno == ENOENT || errno == ESRCH) continue;
            error = errno;
            result = -1;
            break;
        }
        /* Enumeration has finished: callbacks run without the loader lock and
         * with this independent reference holding the entire metadata image. */
        result = visitor((const void *)image.metadata, opaque);
        error = errno;
        llam_managed_image_close(&image);
        if (result != 0) break;
    }
    free(modules);
    if (result != 0) errno = error;
    return result;
}
#endif /* LLAM_PLATFORM_WINDOWS */
