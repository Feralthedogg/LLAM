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

#if !LLAM_PLATFORM_WINDOWS && !LLAM_PLATFORM_DARWIN
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <limits.h>
#include <unistd.h>

#define LLAM_ELF_MAX_SYMBOLS 262144U
#define LLAM_ELF_MAX_DYNAMIC 65536U
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct elf_anchor {
    uintptr_t begin;
    size_t size;
    unsigned char id[16];
} elf_anchor_t;

typedef struct elf_snapshot {
    uintptr_t wanted;
    llam_managed_image_t image;
    char name[PATH_MAX];
    elf_anchor_t *anchors; /* Prepared before entering the loader callback. */
    size_t anchor_count;
    size_t page_size;
    bool main_image;
    bool found;
    int error;
} elf_snapshot_t;

static bool readable(const llam_managed_image_t *image, uintptr_t address, size_t bytes) {
    if (!llam_managed_image_contains(image, address, bytes)) return false;
    for (size_t i = 0U; i < image->section_count; ++i) {
        const llam_managed_image_section_t *s = &image->sections[i];
        if (address >= s->begin && bytes <= s->size && address - s->begin <= s->size - bytes) return true;
    }
    return false;
}

bool llam_managed_image_readonly(const llam_managed_image_t *image, uintptr_t address, size_t bytes) {
    if (!llam_managed_image_contains(image, address, bytes)) return false;
    for (size_t i = 0U; i < image->section_count; ++i) {
        const llam_managed_image_section_t *s = &image->sections[i];
        if (s->readonly_after_relocation && address >= s->begin && bytes <= s->size &&
            address - s->begin <= s->size - bytes) return true;
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

static bool add_bias(uintptr_t bias, uint64_t value, uintptr_t *out) {
    if (value > UINTPTR_MAX - bias) return false;
    *out = bias + (uintptr_t)value;
    return true;
}

static uintptr_t dynamic_pointer(const llam_managed_image_t *image, uintptr_t bias, uint64_t value) {
    if (value > UINTPTR_MAX) return 0U;
    if (readable(image, (uintptr_t)value, 1U)) return (uintptr_t)value;
    uintptr_t relocated;
    return add_bias(bias, value, &relocated) && readable(image, relocated, 1U) ? relocated : 0U;
}

static bool symbol_count(const llam_managed_image_t *image, uintptr_t sysv, uintptr_t gnu, size_t *out) {
    if (sysv != 0U) {
        uint32_t words[2];
        if (!readable(image, sysv, sizeof(words))) return false;
        memcpy(words, (const void *)sysv, sizeof(words));
        if (words[1] > LLAM_ELF_MAX_SYMBOLS) return false;
        *out = words[1];
        return true;
    }
    uint32_t header[4];
    if (gnu == 0U || !readable(image, gnu, sizeof(header))) return false;
    memcpy(header, (const void *)gnu, sizeof(header));
    uint32_t buckets = header[0], first = header[1], bloom = header[2];
    if (buckets > LLAM_ELF_MAX_SYMBOLS || first > LLAM_ELF_MAX_SYMBOLS ||
        bloom == 0U || bloom > LLAM_ELF_MAX_SYMBOLS) return false;
    uintptr_t bucket_address;
    if (!add_bias(gnu, sizeof(header) + (uint64_t)bloom * 8U, &bucket_address) ||
        !readable(image, bucket_address, (size_t)buckets * 4U)) return false;
    uint32_t last = 0U;
    for (uint32_t i = 0U; i < buckets; ++i) {
        uint32_t value;
        memcpy(&value, (const void *)(bucket_address + i * 4U), sizeof(value));
        if (value != 0U && (value < first || value >= LLAM_ELF_MAX_SYMBOLS)) return false;
        if (value > last) last = value;
    }
    if (last == 0U) { *out = first; return true; }
    uintptr_t chain;
    if (!add_bias(bucket_address, (uint64_t)buckets * 4U + (uint64_t)(last - first) * 4U, &chain)) return false;
    for (size_t i = last; i < LLAM_ELF_MAX_SYMBOLS; ++i) {
        uint32_t value;
        if (!readable(image, chain, sizeof(value))) return false;
        memcpy(&value, (const void *)chain, sizeof(value));
        if ((value & 1U) != 0U) { *out = i + 1U; return true; }
        if (!add_bias(chain, sizeof(value), &chain)) return false;
    }
    return false;
}

static bool collect_anchors(elf_snapshot_t *snapshot, uintptr_t bias, uintptr_t dynamic, size_t bytes,
                            bool *text_relocations) {
    llam_managed_image_t *image = &snapshot->image;
    uintptr_t symbols = 0U, strings = 0U, sysv = 0U, gnu = 0U;
    size_t string_bytes = 0U, entry_bytes = 0U;
    bool terminated = false;
    if (bytes % sizeof(Elf64_Dyn) != 0U || bytes / sizeof(Elf64_Dyn) > LLAM_ELF_MAX_DYNAMIC ||
        !readable(image, dynamic, bytes)) return false;
    for (size_t i = 0U; i < bytes / sizeof(Elf64_Dyn); ++i) {
        Elf64_Dyn entry;
        memcpy(&entry, (const void *)(dynamic + i * sizeof(entry)), sizeof(entry));
        if (entry.d_tag == DT_NULL) { terminated = true; break; }
        switch (entry.d_tag) {
        case DT_SYMTAB: symbols = dynamic_pointer(image, bias, entry.d_un.d_ptr); break;
        case DT_STRTAB: strings = dynamic_pointer(image, bias, entry.d_un.d_ptr); break;
        case DT_HASH: sysv = dynamic_pointer(image, bias, entry.d_un.d_ptr); break;
        case DT_GNU_HASH: gnu = dynamic_pointer(image, bias, entry.d_un.d_ptr); break;
        case DT_STRSZ: string_bytes = entry.d_un.d_val; break;
        case DT_SYMENT: entry_bytes = entry.d_un.d_val; break;
        case DT_TEXTREL: *text_relocations = true; break;
        case DT_FLAGS: if ((entry.d_un.d_val & DF_TEXTREL) != 0U) *text_relocations = true; break;
        default: break;
        }
    }
    size_t count;
    if (!terminated || symbols == 0U || strings == 0U || entry_bytes != sizeof(Elf64_Sym) ||
        string_bytes == 0U || !readable(image, strings, string_bytes) ||
        !symbol_count(image, sysv, gnu, &count) || !readable(image, symbols, count * sizeof(Elf64_Sym))) return false;
    static const char prefix[] = "__llam_managed_anchor_";
    for (size_t i = 0U; i < count; ++i) {
        Elf64_Sym symbol;
        memcpy(&symbol, (const void *)(symbols + i * sizeof(symbol)), sizeof(symbol));
        if (symbol.st_shndx == SHN_UNDEF || symbol.st_name >= string_bytes) continue;
        size_t remaining = string_bytes - symbol.st_name;
        const unsigned char *name = (const unsigned char *)(strings + symbol.st_name);
        if (remaining < 55U || memcmp(name, prefix, sizeof(prefix) - 1U) != 0) continue;
        if (name[54U] != 0U || ELF64_ST_TYPE(symbol.st_info) != STT_OBJECT || symbol.st_size == 0U ||
            symbol.st_size > LLAM_LLPM2_MAX_BYTES || snapshot->anchor_count == LLAM_MANAGED_IMAGE_MAX_RECORDS) return false;
        elf_anchor_t *anchor = &snapshot->anchors[snapshot->anchor_count];
        for (size_t j = 0U; j < 16U; ++j) {
            unsigned byte = 0U;
            for (unsigned k = 0U; k < 2U; ++k) {
                unsigned char ch = name[22U + j * 2U + k];
                unsigned value;
                if (ch >= '0' && ch <= '9') value = ch - '0';
                else if (ch >= 'a' && ch <= 'f') value = ch - 'a' + 10U;
                else return false;
                byte = byte * 16U + value;
            }
            anchor->id[j] = (unsigned char)byte;
        }
        if (!add_bias(bias, symbol.st_value, &anchor->begin) ||
            !llam_managed_image_readonly(image, anchor->begin, symbol.st_size) ||
            llam_managed_image_executable(image, anchor->begin, symbol.st_size)) return false;
        anchor->size = symbol.st_size;
        ++snapshot->anchor_count;
    }
    return true;
}

static bool collect_note(llam_managed_image_t *image, uintptr_t note, size_t size, unsigned *build_ids) {
    if (!readable(image, note, size)) return false;
    size_t offset = 0U;
    while (offset < size) {
        Elf64_Nhdr header;
        if (sizeof(header) > size - offset) return false;
        memcpy(&header, (const void *)(note + offset), sizeof(header));
        offset += sizeof(header);
        uint64_t names = ((uint64_t)header.n_namesz + 3U) & ~UINT64_C(3);
        uint64_t desc = ((uint64_t)header.n_descsz + 3U) & ~UINT64_C(3);
        if (names > size - offset || desc > size - offset - names) return false;
        if (header.n_type == NT_GNU_BUILD_ID && header.n_namesz == 4U &&
            memcmp((const void *)(note + offset), "GNU\0", 4U) == 0) {
            if (++*build_ids != 1U || header.n_descsz < 16U) return false;
            memcpy(image->build_id, (const void *)(note + offset + (size_t)names), 16U);
        }
        offset += (size_t)(names + desc);
    }
    return true;
}

/* Runs under the loader's enumeration lock. It only copies bounded native
 * descriptors into preallocated storage; never allocates, pins or calls dlopen. */
static int snapshot_image(struct dl_phdr_info *info, size_t info_size, void *opaque) {
    elf_snapshot_t *snapshot = opaque;
    llam_managed_image_t *image = &snapshot->image;
    if (info_size < offsetof(struct dl_phdr_info, dlpi_phnum) + sizeof(info->dlpi_phnum) ||
        info->dlpi_phdr == NULL) return 0;
    bool contains = false;
    for (Elf64_Half i = 0U; i < info->dlpi_phnum; ++i) {
        const Elf64_Phdr *p = &info->dlpi_phdr[i];
        uintptr_t begin;
        if (p->p_type == PT_LOAD && add_bias(info->dlpi_addr, p->p_vaddr, &begin) &&
            snapshot->wanted >= begin && snapshot->wanted - begin < p->p_memsz) contains = true;
    }
    if (!contains) return 0;
    snapshot->found = true;
    snapshot->error = EINVAL;
    if (info->dlpi_phnum > LLAM_MANAGED_IMAGE_MAX_SECTIONS) { snapshot->error = E2BIG; return 1; }
    uintptr_t low = UINTPTR_MAX, high = 0U, header_address = 0U;
    uintptr_t dynamic = 0U;
    size_t dynamic_size = 0U;
    bool qualified = true;
    bool text_relocations = false;
    for (Elf64_Half i = 0U; i < info->dlpi_phnum; ++i) {
        const Elf64_Phdr *p = &info->dlpi_phdr[i];
        if (p->p_type != PT_LOAD || p->p_memsz == 0U) continue;
        uintptr_t begin;
        if (!add_bias(info->dlpi_addr, p->p_vaddr, &begin) || p->p_memsz > UINTPTR_MAX - begin) return 1;
        if (begin < low) low = begin;
        if (begin + p->p_memsz > high) high = begin + p->p_memsz;
        if ((p->p_flags & PF_R) == 0U || (p->p_flags & (PF_W | PF_X)) == (PF_W | PF_X) ||
            p->p_filesz > p->p_memsz || p->p_filesz > UINT64_MAX - p->p_offset ||
            ((p->p_flags & PF_X) != 0U && p->p_filesz != p->p_memsz)) qualified = false;
        if (p->p_offset == 0U && p->p_filesz >= sizeof(Elf64_Ehdr)) {
            if (header_address != 0U) qualified = false;
            header_address = begin;
        }
        if ((p->p_flags & PF_R) == 0U) continue;
        for (size_t j = 0U; j < image->section_count; ++j) {
            const llam_managed_image_section_t *prior = &image->sections[j];
            if (begin < prior->begin + prior->size && prior->begin < begin + p->p_memsz) qualified = false;
        }
        image->sections[image->section_count++] = (llam_managed_image_section_t){
            .begin = begin, .size = p->p_memsz, .executable = (p->p_flags & PF_X) != 0U,
            .readonly_after_relocation = (p->p_flags & PF_W) == 0U
        };
    }
    image->base = low;
    image->size = high > low ? high - low : 0U;
    for (Elf64_Half i = 0U; i < info->dlpi_phnum; ++i) {
        const Elf64_Phdr *p = &info->dlpi_phdr[i];
        uintptr_t begin;
        if (!add_bias(info->dlpi_addr, p->p_vaddr, &begin)) return 1;
        if (p->p_type == PT_GNU_RELRO && p->p_memsz != 0U) {
            if (p->p_memsz > UINTPTR_MAX - begin || !readable(image, begin, p->p_memsz)) return 1;
            /* The loader protects only complete pages through the RELRO end.
             * Its unaligned trailing bytes must never authorize a guard slot. */
            uintptr_t protected_end = (begin + p->p_memsz) & ~((uintptr_t)snapshot->page_size - 1U);
            if (protected_end <= begin) continue;
            if (image->section_count == LLAM_MANAGED_IMAGE_MAX_SECTIONS) return 1;
            image->sections[image->section_count++] = (llam_managed_image_section_t){
                .begin = begin, .size = protected_end - begin, .readonly_after_relocation = true
            };
        } else if (p->p_type == PT_DYNAMIC) {
            if (dynamic != 0U) qualified = false;
            dynamic = begin;
            dynamic_size = p->p_memsz;
        }
    }
    if (dynamic == 0U) { snapshot->error = ENOENT; return 1; }
    if (!collect_anchors(snapshot, info->dlpi_addr, dynamic, dynamic_size, &text_relocations)) return 1;
    if (snapshot->anchor_count == 0U) { snapshot->error = ENOENT; return 1; }
    /* Ordinary native images do not promise the LLPM machine, immutable-code
     * or build-ID profile. Enforce it only after finding a loader-visible
     * metadata contribution, while all mapped reads still hold the loader lock. */
    if (!qualified || text_relocations || header_address == 0U ||
        !readable(image, header_address, sizeof(Elf64_Ehdr))) return 1;
    Elf64_Ehdr header;
    memcpy(&header, (const void *)header_address, sizeof(header));
    uintptr_t phdr_address;
    if (memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 || header.e_ident[EI_CLASS] != ELFCLASS64 ||
        header.e_ident[EI_DATA] != ELFDATA2LSB || header.e_ident[EI_VERSION] != EV_CURRENT ||
        header.e_version != EV_CURRENT || header.e_ehsize != sizeof(header) ||
        header.e_phentsize != sizeof(Elf64_Phdr) || header.e_phnum != info->dlpi_phnum ||
        (header.e_type != ET_EXEC && header.e_type != ET_DYN) ||
        !add_bias(header_address, header.e_phoff, &phdr_address) || phdr_address != (uintptr_t)info->dlpi_phdr ||
        !readable(image, phdr_address, (size_t)header.e_phnum * sizeof(Elf64_Phdr))) return 1;
#if LLAM_ARCH_AARCH64
    if (header.e_machine != EM_AARCH64) return 1;
    image->architecture = LLAM_PREEMPT_METADATA_ARCH_AARCH64;
#elif LLAM_ARCH_X86_64
    if (header.e_machine != EM_X86_64) return 1;
    image->architecture = LLAM_PREEMPT_METADATA_ARCH_X86_64;
#else
    snapshot->error = ENOTSUP;
    return 1;
#endif
    image->native_abi = LLAM_LLPM2_ABI_SYSV;
    unsigned build_ids = 0U;
    for (Elf64_Half i = 0U; i < info->dlpi_phnum; ++i) {
        const Elf64_Phdr *p = &info->dlpi_phdr[i];
        uintptr_t begin;
        if (p->p_type == PT_NOTE && (!add_bias(info->dlpi_addr, p->p_vaddr, &begin) ||
            !collect_note(image, begin, p->p_memsz, &build_ids))) return 1;
    }
    if (build_ids != 1U) return 1;
    const char *name = info->dlpi_name != NULL ? info->dlpi_name : "";
    size_t n = 0U;
    while (name[n] != '\0' && n + 1U < sizeof(snapshot->name)) {
        snapshot->name[n] = name[n];
        ++n;
    }
    if (name[n] != '\0') { snapshot->error = ENAMETOOLONG; return 1; }
    snapshot->name[n] = '\0';
    snapshot->main_image = n == 0U;
    snapshot->error = 0;
    return 1;
}

static int anchor_order(const void *a, const void *b) {
    const elf_anchor_t *left = a, *right = b;
    return left->begin < right->begin ? -1 : left->begin != right->begin;
}

static int take_snapshot(uintptr_t address, elf_snapshot_t *snapshot, elf_anchor_t *anchors) {
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->wanted = address;
    snapshot->anchors = anchors;
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0 || ((unsigned long)page_size & ((unsigned long)page_size - 1U)) != 0U) {
        errno = ENOTSUP;
        return -1;
    }
    snapshot->page_size = (size_t)page_size;
    dl_iterate_phdr(snapshot_image, snapshot);
    if (!snapshot->found || snapshot->error != 0) {
        errno = snapshot->found ? snapshot->error : ESRCH;
        return -1;
    }
    qsort(anchors, snapshot->anchor_count, sizeof(*anchors), anchor_order);
    uintptr_t end = anchors[0].begin;
    for (size_t i = 0U; i < snapshot->anchor_count; ++i) {
        if (anchors[i].begin != end || anchors[i].size > UINTPTR_MAX - end) { errno = EINVAL; return -1; }
        end += anchors[i].size;
    }
    snapshot->image.metadata = anchors[0].begin;
    snapshot->image.metadata_size = end - anchors[0].begin;
    if (snapshot->image.metadata_size > LLAM_LLPM2_MAX_BYTES) { errno = E2BIG; return -1; }
    return 0;
}

int llam_managed_image_open(const void *address, bool caller_reference, llam_managed_image_t *out) {
    elf_snapshot_t snapshot;
    elf_anchor_t *anchors = NULL;
    void *pin = NULL;
    int error = EINVAL;
    if (out != NULL) memset(out, 0, sizeof(*out));
    if (address == NULL || out == NULL) goto failed;
    anchors = calloc(LLAM_MANAGED_IMAGE_MAX_RECORDS, sizeof(*anchors));
    if (anchors == NULL) { error = ENOMEM; goto failed; }
    if (take_snapshot((uintptr_t)address, &snapshot, anchors) != 0) {
        error = caller_reference && errno == ESRCH ? EINVAL : errno;
        goto failed;
    }
    char anchor[55];
    llam_managed_image_anchor_name(anchors[0].id, anchor);
    uintptr_t metadata = snapshot.image.metadata;
    if (snapshot.main_image) pin = dlopen(NULL, RTLD_NOW | RTLD_LOCAL);
    else {
        int mode = RTLD_NOW | RTLD_LOCAL;
#if defined(RTLD_NOLOAD)
        mode |= RTLD_NOLOAD;
#else
        if (!caller_reference) { error = ENOTSUP; goto failed; }
#endif
        pin = dlopen(snapshot.name, mode);
    }
    if (pin == NULL) { error = caller_reference ? ENOTSUP : ESRCH; goto failed; }
    if ((uintptr_t)dlsym(pin, anchor) != metadata) goto failed;
    if (take_snapshot(metadata, &snapshot, anchors) != 0) { error = errno; goto failed; }
    if (snapshot.image.metadata != metadata || (uintptr_t)dlsym(pin, anchor) != metadata) goto failed;
    snapshot.image.native_pin = pin;
    *out = snapshot.image;
    free(anchors);
    return 0;
failed:
    free(anchors);
    if (pin != NULL) (void)dlclose(pin);
    errno = error;
    return -1;
}

typedef struct elf_image_addresses {
    uintptr_t values[4096];
    size_t count;
    bool overflow;
} elf_image_addresses_t;

static int collect_image_addresses(struct dl_phdr_info *info, size_t size, void *opaque) {
    elf_image_addresses_t *addresses = opaque;
    if (size < offsetof(struct dl_phdr_info, dlpi_phnum) + sizeof(info->dlpi_phnum) || info->dlpi_phdr == NULL) return 0;
    if (addresses->count == 4096U) { addresses->overflow = true; return 1; }
    for (Elf64_Half i = 0U; i < info->dlpi_phnum; ++i) {
        const Elf64_Phdr *p = &info->dlpi_phdr[i];
        uintptr_t address;
        if (p->p_type == PT_LOAD && p->p_memsz != 0U && add_bias(info->dlpi_addr, p->p_vaddr, &address)) {
            addresses->values[addresses->count++] = address;
            break;
        }
    }
    return 0;
}

int llam_managed_image_discover(int (*visitor)(const void *, void *), void *opaque) {
    if (visitor == NULL) { errno = EINVAL; return -1; }
    elf_image_addresses_t *addresses = calloc(1U, sizeof(*addresses));
    if (addresses == NULL) return -1;
    dl_iterate_phdr(collect_image_addresses, addresses);
    int result = 0, error = 0;
    if (addresses->overflow) { result = -1; error = E2BIG; }
    for (size_t i = 0U; result == 0 && i < addresses->count; ++i) {
        llam_managed_image_t image;
        if (llam_managed_image_open((const void *)addresses->values[i], false, &image) != 0) {
            if (errno == ENOENT || errno == ESRCH) continue;
            result = -1; error = errno; break;
        }
        result = visitor((const void *)image.metadata, opaque);
        error = errno;
        llam_managed_image_close(&image);
    }
    free(addresses);
    if (result != 0) errno = error;
    return result;
}
#endif /* ELF platforms */
