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

#if LLAM_PLATFORM_DARWIN
#include <dlfcn.h>
#include <libproc.h>
#include <limits.h>
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>

#define LLAM_DARWIN_IMAGE_MAX_COMMANDS (1024U * 1024U)

static bool copy_image(uintptr_t address, void *out, size_t bytes) {
    mach_vm_size_t copied = 0U;
    return bytes != 0U && bytes <= UINTPTR_MAX - address &&
        mach_vm_read_overwrite(mach_task_self(), address, bytes, (mach_vm_address_t)out, &copied) == KERN_SUCCESS &&
        copied == bytes;
}

static bool image_address(uintptr_t base, uint64_t text_vmaddr, uint64_t vmaddr, size_t bytes,
                            uintptr_t *out) {
    if (vmaddr < text_vmaddr || vmaddr - text_vmaddr > UINTPTR_MAX - base) return false;
    uintptr_t value = base + (uintptr_t)(vmaddr - text_vmaddr);
    if (bytes > UINTPTR_MAX - value) return false;
    *out = value;
    return true;
}

static bool region_readonly(uintptr_t address, size_t bytes) {
    if (bytes == 0U || bytes > UINTPTR_MAX - address) return false;
    uintptr_t end = address + bytes;
    while (address < end) {
        mach_vm_address_t region = address;
        mach_vm_size_t size = 0U;
        natural_t depth = 0U;
        vm_region_submap_info_data_64_t info = {0};
        for (;;) {
            mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
            if (depth > 16U || mach_vm_region_recurse(mach_task_self(), &region, &size, &depth,
                    (vm_region_recurse_info_t)&info, &count) != KERN_SUCCESS ||
                region > address || size == 0U || size > UINTPTR_MAX - region || region + size <= address) return false;
            if (!info.is_submap) break;
            ++depth;
        }
        if ((info.protection & VM_PROT_READ) == 0 || (info.protection & VM_PROT_WRITE) != 0) return false;
        address = region + size < end ? (uintptr_t)(region + size) : end;
    }
    return true;
}

bool llam_managed_image_readonly(const llam_managed_image_t *image, uintptr_t address, size_t bytes) {
    if (!llam_managed_image_contains(image, address, bytes)) return false;
    for (size_t i = 0U; i < image->section_count; ++i) {
        const llam_managed_image_section_t *s = &image->sections[i];
        if (s->readonly_after_relocation && address >= s->begin && bytes <= s->size &&
            address - s->begin <= s->size - bytes) return region_readonly(address, bytes);
    }
    return false;
}

void llam_managed_image_close(llam_managed_image_t *image) {
    if (image == NULL) return;
    if (image->native_pin != NULL) (void)dlclose(image->native_pin);
    memset(image, 0, sizeof(*image));
}

uintptr_t llam_managed_image_symbol(const llam_managed_image_t *image, const char *name) {
    return image != NULL && image->native_pin != NULL ? (uintptr_t)dlsym(image->native_pin, name) : 0U;
}

static int parse_commands(llam_managed_image_t *image, const struct mach_header_64 *header,
                           const unsigned char *commands) {
    uint64_t text_vmaddr = UINT64_MAX;
    size_t cursor = 0U;
    unsigned uuid_count = 0U;
    unsigned metadata_count = 0U;
    bool has_metadata = false;
    unsigned text_segments = 0U;
    uintptr_t segment_begins[LLAM_MANAGED_IMAGE_MAX_SECTIONS];
    uintptr_t segment_ends[LLAM_MANAGED_IMAGE_MAX_SECTIONS];
    size_t segment_count = 0U;
    for (uint32_t i = 0U; i < header->ncmds; ++i) {
        struct load_command command;
        if (cursor > header->sizeofcmds || sizeof(command) > header->sizeofcmds - cursor) goto invalid;
        memcpy(&command, commands + cursor, sizeof(command));
        if (command.cmdsize < sizeof(command) || (command.cmdsize & 3U) != 0U ||
            command.cmdsize > header->sizeofcmds - cursor) goto invalid;
        if (command.cmd == LC_SEGMENT_64) {
            struct segment_command_64 segment;
            if (command.cmdsize < sizeof(segment)) goto invalid;
            memcpy(&segment, commands + cursor, sizeof(segment));
            if (segment.nsects > (command.cmdsize - sizeof(segment)) / sizeof(struct section_64)) goto invalid;
            for (uint32_t j = 0U; j < segment.nsects; ++j) {
                struct section_64 section;
                memcpy(&section, commands + cursor + sizeof(segment) + j * sizeof(section), sizeof(section));
                if (memcmp(section.sectname, "__llam_managed", 15U) == 0) has_metadata = true;
            }
            if (segment.fileoff == 0U && segment.filesize >= sizeof(*header) &&
                (segment.initprot & VM_PROT_READ) != 0) {
                ++text_segments;
                text_vmaddr = segment.vmaddr;
            }
        }
        cursor += command.cmdsize;
    }
    if (cursor != header->sizeofcmds) goto invalid;
    if (!has_metadata) { errno = ENOENT; return -1; }
    if (text_segments != 1U || text_vmaddr == UINT64_MAX) goto invalid;
    cursor = 0U;
    for (uint32_t i = 0U; i < header->ncmds; ++i) {
        struct load_command command;
        memcpy(&command, commands + cursor, sizeof(command));
        if (command.cmd == LC_UUID) {
            struct uuid_command uuid;
            if (command.cmdsize != sizeof(uuid) || ++uuid_count != 1U) goto invalid;
            memcpy(&uuid, commands + cursor, sizeof(uuid));
            memcpy(image->build_id, uuid.uuid, 16U);
        } else if (command.cmd == LC_SEGMENT_64) {
            struct segment_command_64 segment;
            memcpy(&segment, commands + cursor, sizeof(segment));
            if (segment.nsects > LLAM_MANAGED_IMAGE_MAX_SECTIONS ||
                segment.nsects > (command.cmdsize - sizeof(segment)) / sizeof(struct section_64)) goto invalid;
            if (segment.filesize > segment.vmsize || segment.filesize > UINT64_MAX - segment.fileoff ||
                (segment.fileoff == 0U && segment.filesize != 0U &&
                 segment.filesize < sizeof(*header) + (uint64_t)header->sizeofcmds)) goto invalid;
            if ((segment.initprot & (VM_PROT_WRITE | VM_PROT_EXECUTE)) == (VM_PROT_WRITE | VM_PROT_EXECUTE)) goto invalid;
            if (segment.initprot != 0 && segment.vmsize != 0U) {
                uintptr_t begin;
                if (!image_address(image->base, text_vmaddr, segment.vmaddr, segment.vmsize, &begin)) goto invalid;
                if (segment_count == LLAM_MANAGED_IMAGE_MAX_SECTIONS) goto invalid;
                uintptr_t end = begin + (uintptr_t)segment.vmsize;
                for (size_t j = 0U; j < segment_count; ++j) {
                    if (begin < segment_ends[j] && segment_begins[j] < end) goto invalid;
                }
                segment_begins[segment_count] = begin;
                segment_ends[segment_count++] = end;
                size_t extent = (size_t)(begin - image->base) + (size_t)segment.vmsize;
                if (extent > image->size) image->size = extent;
            }
            for (uint32_t j = 0U; j < segment.nsects; ++j) {
                struct section_64 section;
                memcpy(&section, commands + cursor + sizeof(segment) + j * sizeof(section), sizeof(section));
                if (section.size == 0U) continue;
                if (image->section_count == LLAM_MANAGED_IMAGE_MAX_SECTIONS || section.addr < segment.vmaddr ||
                    section.size > segment.vmsize || section.addr - segment.vmaddr > segment.vmsize - section.size ||
                    memcmp(section.segname, segment.segname, sizeof(section.segname)) != 0 ||
                    (segment.initprot & VM_PROT_READ) == 0) goto invalid;
                uint32_t type = section.flags & SECTION_TYPE;
                bool zero_fill = type == S_ZEROFILL || type == S_GB_ZEROFILL || type == S_THREAD_LOCAL_ZEROFILL;
                uint64_t displacement = section.addr - segment.vmaddr;
                if (!zero_fill && (displacement > segment.filesize || section.size > segment.filesize - displacement ||
                    displacement > UINT64_MAX - segment.fileoff || section.offset != segment.fileoff + displacement)) goto invalid;
                uintptr_t address;
                if (!image_address(image->base, text_vmaddr, section.addr, section.size, &address)) goto invalid;
                bool executable = (section.flags & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS)) != 0U;
                bool readonly = (segment.initprot & VM_PROT_WRITE) == 0 ||
                    (memcmp(segment.segname, "__DATA_CONST", 12U) == 0 && (segment.flags & SG_READ_ONLY) != 0U);
                if (executable && (zero_fill || (segment.initprot & (VM_PROT_READ | VM_PROT_EXECUTE)) !=
                    (VM_PROT_READ | VM_PROT_EXECUTE) || !readonly)) goto invalid;
                image->sections[image->section_count++] = (llam_managed_image_section_t){
                    .begin = address, .size = (size_t)section.size,
                    .executable = executable, .readonly_after_relocation = readonly
                };
                if (memcmp(section.sectname, "__llam_managed", 15U) == 0) {
                    if (++metadata_count != 1U || executable || !readonly ||
                        (section.flags & SECTION_TYPE) != S_REGULAR || section.size > LLAM_LLPM2_MAX_BYTES) goto invalid;
                    image->metadata = address;
                    image->metadata_size = (size_t)section.size;
                }
            }
        }
        cursor += command.cmdsize;
    }
    if (metadata_count == 0U) { errno = ENOENT; return -1; }
    if (uuid_count != 1U) goto invalid;
    return 0;
invalid:
    errno = EINVAL;
    return -1;
}

static bool qualified_header(const struct mach_header_64 *header) {
    bool native_cpu = (LLAM_ARCH_AARCH64 && header->cputype == CPU_TYPE_ARM64) ||
        (LLAM_ARCH_X86_64 && header->cputype == CPU_TYPE_X86_64);
    return header->magic == MH_MAGIC_64 && native_cpu &&
        (header->filetype == MH_EXECUTE || header->filetype == MH_DYLIB || header->filetype == MH_BUNDLE) &&
        header->ncmds <= 4096U && header->sizeofcmds >= sizeof(struct load_command) &&
        header->sizeofcmds <= LLAM_DARWIN_IMAGE_MAX_COMMANDS;
}

static int refresh_layout(llam_managed_image_t *image) {
    struct mach_header_64 header;
    unsigned char *commands;
    if (!copy_image(image->base, &header, sizeof(header)) || !qualified_header(&header) ||
        sizeof(header) > UINTPTR_MAX - image->base) {
        errno = EINVAL;
        return -1;
    }
    commands = malloc(header.sizeofcmds);
    if (commands == NULL) return -1;
    if (!copy_image(image->base + sizeof(header), commands, header.sizeofcmds)) {
        free(commands);
        errno = EINVAL;
        return -1;
    }
    image->size = 0U;
    image->section_count = 0U;
    image->metadata = 0U;
    image->metadata_size = 0U;
    image->architecture = header.cputype == CPU_TYPE_ARM64 ? LLAM_PREEMPT_METADATA_ARCH_AARCH64 : LLAM_PREEMPT_METADATA_ARCH_X86_64;
    image->native_abi = LLAM_LLPM2_ABI_DARWIN;
    memset(image->build_id, 0, sizeof(image->build_id));
    int result = parse_commands(image, &header, commands);
    free(commands);
    return result;
}

int llam_managed_image_open(const void *address, bool caller_reference, llam_managed_image_t *out) {
    llam_managed_image_t image = {0};
    Dl_info info;
    struct mach_header_64 header;
    unsigned char first[LLAM_LLPM2_HEADER_BYTES];
    unsigned char *commands = NULL;
    char path[PATH_MAX] = {0};
    int error = EINVAL;
    if (out != NULL) memset(out, 0, sizeof(*out));
    if (address == NULL || out == NULL) goto failed;
    if (dladdr(address, &info) == 0 || info.dli_fbase == NULL) {
        error = caller_reference ? EINVAL : ESRCH;
        goto failed;
    }
    image.base = (uintptr_t)info.dli_fbase;
    if (!copy_image(image.base, &header, sizeof(header))) { error = caller_reference ? EINVAL : ESRCH; goto failed; }
    if (!qualified_header(&header)) goto failed;
    image.architecture = header.cputype == CPU_TYPE_ARM64 ? LLAM_PREEMPT_METADATA_ARCH_AARCH64 : LLAM_PREEMPT_METADATA_ARCH_X86_64;
    image.native_abi = LLAM_LLPM2_ABI_DARWIN;
    commands = malloc(header.sizeofcmds);
    if (commands == NULL) { error = ENOMEM; goto failed; }
    if (sizeof(header) > UINTPTR_MAX - image.base) goto failed;
    if (!copy_image(image.base + sizeof(header), commands, header.sizeofcmds)) {
        error = caller_reference ? EINVAL : ESRCH;
        goto failed;
    }
    if (parse_commands(&image, &header, commands) != 0) { error = errno; goto failed; }
    free(commands);
    commands = NULL;
    if (image.metadata_size < sizeof(first)) goto failed;
    if (!copy_image(image.metadata, first, sizeof(first))) { error = caller_reference ? EINVAL : ESRCH; goto failed; }
    if (caller_reference) {
        if (info.dli_fname == NULL || strnlen(info.dli_fname, sizeof(path)) == sizeof(path)) goto failed;
        memcpy(path, info.dli_fname, strlen(info.dli_fname) + 1U);
    } else {
        /* Copy the vnode path through the kernel. Borrowed dyld filename
         * pointers may disappear between an auto snapshot and dlopen. */
        int length = proc_regionfilename(getpid(), image.base, path, sizeof(path));
        if (length <= 0 || (size_t)length >= sizeof(path)) { error = ESRCH; goto failed; }
        path[length] = '\0';
    }
    image.native_pin = dlopen(path, RTLD_NOW | RTLD_NOLOAD | RTLD_LOCAL | RTLD_FIRST);
    if (image.native_pin == NULL && image.base == (uintptr_t)_dyld_get_image_header(0U)) {
        image.native_pin = dlopen(NULL, RTLD_NOW | RTLD_FIRST);
    }
    if (image.native_pin == NULL) { error = caller_reference ? ENOTSUP : ESRCH; goto failed; }
    char anchor[55];
    llam_managed_image_anchor_name(first + 112U, anchor);
    uintptr_t metadata = image.metadata;
    if (llam_managed_image_symbol(&image, anchor) != metadata) goto failed;
    Dl_info retained;
    if (dladdr((const void *)metadata, &retained) == 0 || (uintptr_t)retained.dli_fbase != image.base) {
        error = caller_reference ? EINVAL : ESRCH;
        goto failed;
    }
    /* An auto snapshot can race an unload/reload at the same address. Refresh
     * every section extent after acquiring the exact anchor's independent pin;
     * no direct mapped read uses stale pre-pin layout information. */
    if (refresh_layout(&image) != 0) { error = errno; goto failed; }
    if (image.metadata != metadata ||
        !llam_managed_image_readonly(&image, image.metadata, image.metadata_size)) goto failed;
    /* The exact independently retained anchor now owns all direct mapped reads. */
    *out = image;
    return 0;
failed:
    free(commands);
    llam_managed_image_close(&image);
    errno = error;
    return -1;
}

int llam_managed_image_discover(int (*visitor)(const void *, void *), void *opaque) {
    if (visitor == NULL) { errno = EINVAL; return -1; }
    uint32_t count = _dyld_image_count();
    if (count > 4096U) { errno = E2BIG; return -1; }
    uintptr_t *addresses = calloc(count != 0U ? count : 1U, sizeof(*addresses));
    if (addresses == NULL) return -1;
    for (uint32_t i = 0U; i < count; ++i) addresses[i] = (uintptr_t)_dyld_get_image_header(i);
    int result = 0, error = 0;
    for (uint32_t i = 0U; i < count; ++i) {
        if (addresses[i] == 0U) continue;
        bool duplicate = false;
        for (uint32_t j = 0U; j < i; ++j) if (addresses[j] == addresses[i]) duplicate = true;
        if (duplicate) continue;
        llam_managed_image_t image;
        if (llam_managed_image_open((const void *)addresses[i], false, &image) != 0) {
            if (errno == ENOENT || errno == ESRCH) continue;
            result = -1; error = errno; break;
        }
        result = visitor((const void *)image.metadata, opaque);
        error = errno;
        llam_managed_image_close(&image);
        if (result != 0) break;
    }
    free(addresses);
    if (result != 0) errno = error;
    return result;
}
#endif /* LLAM_PLATFORM_DARWIN */
