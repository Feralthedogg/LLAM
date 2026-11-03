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

#include "metadata_v2.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define LLPM2_FLAGS_KNOWN UINT32_C(63)
#define LLPM2_EFFECTS_KNOWN UINT32_C(63)
#define LLPM2_ISA_KNOWN UINT64_C(31)
#define LLPM2_ENTRY_FLAGS_KNOWN UINT32_C(31)
#define LLPM2_CALL_FLAGS_KNOWN UINT16_C(127)
#define LLPM2_SEAL_FLAGS (LLAM_LLPM2_MACHINE_VERIFIED | \
                         LLAM_LLPM2_LINK_VERIFIED | LLAM_LLPM2_IMMUTABLE_TEXT | LLAM_LLPM2_IMAGE_ANCHOR)

static uint16_t read16(const unsigned char *p) {
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8U));
}

static uint32_t read32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) |
           ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
}

static uint64_t read64(const unsigned char *p) {
    return (uint64_t)read32(p) | ((uint64_t)read32(p + 4U) << 32U);
}

static void write16(unsigned char *p, uint16_t value) {
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8U);
}

static void write32(unsigned char *p, uint32_t value) {
    for (unsigned i = 0U; i < 4U; ++i) p[i] = (unsigned char)(value >> (i * 8U));
}

static void write64(unsigned char *p, uint64_t value) {
    for (unsigned i = 0U; i < 8U; ++i) p[i] = (unsigned char)(value >> (i * 8U));
}

static bool resolve64(uintptr_t base, uint64_t displacement, uintptr_t *out) {
    if ((displacement >> 63U) == 0U) {
        if (displacement > UINTPTR_MAX - base) return false;
        *out = base + (uintptr_t)displacement;
    } else {
        uint64_t magnitude = UINT64_C(0) - displacement;
        if (magnitude > base) return false;
        *out = base - (uintptr_t)magnitude;
    }
    return true;
}

static bool encode_relative(uintptr_t address, uintptr_t field, uint64_t *out) {
    if (address >= field) {
        uint64_t distance = address - field;
        if (distance > INT64_MAX) return false;
        *out = distance;
    } else {
        uint64_t distance = field - address;
        if (distance > (UINT64_C(1) << 63U)) return false;
        *out = UINT64_C(0) - distance;
    }
    return true;
}

static bool known_header(const llam_llpm2_header_t *h) {
    return (h->architecture == LLAM_PREEMPT_METADATA_ARCH_X86_64 ||
            h->architecture == LLAM_PREEMPT_METADATA_ARCH_AARCH64) &&
           h->native_abi >= LLAM_LLPM2_ABI_SYSV && h->native_abi <= LLAM_LLPM2_ABI_WIN64 &&
           !(h->native_abi == LLAM_LLPM2_ABI_WIN64 &&
             h->architecture != LLAM_PREEMPT_METADATA_ARCH_X86_64) &&
           h->managed_major == 1U && h->managed_minor == 0U &&
           (h->required_flags & ~LLPM2_FLAGS_KNOWN) == 0U;
}

static bool in_image(uintptr_t pc, uintptr_t base, size_t size, bool endpoint) {
    if (size == 0U) return base == 0U;
    return pc >= base && (endpoint ? pc - base <= size : pc - base < size);
}

static bool instruction_aligned(uintptr_t pc, const llam_llpm2_header_t *h) {
    return h->architecture != LLAM_PREEMPT_METADATA_ARCH_AARCH64 || (pc & 3U) == 0U;
}

static uint64_t guard_type_id(const llam_llpm2_header_t *h) {
    const char signature[] = "void ():0:0:0";
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0U; i + 1U < sizeof(signature); ++i) {
        unsigned char byte = (unsigned char)signature[i];
        if (i == sizeof(signature) - 4U) byte = (unsigned char)('0' + h->architecture);
        if (i == sizeof(signature) - 2U) byte = (unsigned char)('0' + h->native_abi);
        hash = (hash ^ byte) * UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t hash_text(uint64_t hash, const char *text) {
    for (; *text != '\0'; ++text) hash = (hash ^ (unsigned char)*text) * UINT64_C(1099511628211);
    return hash;
}

static const char *runtime_type(char code, uint16_t abi) {
    switch (code) {
    case 'v': return "void";
    case 'p': return "ptr";
    case 'i': return "i32";
    case 'q': return "i64";
    case 'f': return abi == LLAM_LLPM2_ABI_WIN64 ? "i64" : "i32";
    case 'h': return abi == LLAM_LLPM2_ABI_WIN64 ? "ptr" : "i32";
    default: return NULL;
    }
}

uint64_t llam_llpm2_runtime_type_id(uint64_t contract_id, uint16_t architecture, uint16_t native_abi) {
    const char *signature;
    if ((architecture != LLAM_PREEMPT_METADATA_ARCH_X86_64 &&
         architecture != LLAM_PREEMPT_METADATA_ARCH_AARCH64) ||
        native_abi < LLAM_LLPM2_ABI_SYSV || native_abi > LLAM_LLPM2_ABI_WIN64 ||
        (native_abi == LLAM_LLPM2_ABI_WIN64 && architecture != LLAM_PREEMPT_METADATA_ARCH_X86_64)) return 0U;
    switch (contract_id) {
#define LLAM_RUNTIME_CONTRACT(ID, Name, Signature, Effect) case ID: signature = Signature; break;
#include "managed_runtime_contracts.def"
#undef LLAM_RUNTIME_CONTRACT
    default: return 0U;
    }
    uint64_t hash = hash_text(UINT64_C(14695981039346656037), runtime_type(signature[0], native_abi));
    hash = hash_text(hash, " (");
    for (const char *parameter = signature + 1U; *parameter != '\0'; ++parameter) {
        if (parameter != signature + 1U) hash = hash_text(hash, ", ");
        hash = hash_text(hash, runtime_type(*parameter, native_abi));
    }
    char suffix[] = "):0:0:0";
    suffix[4] = (char)('0' + architecture);
    suffix[6] = (char)('0' + native_abi);
    hash = hash_text(hash, suffix);
    return hash == 0U ? 1U : hash;
}

uint16_t llam_llpm2_runtime_effect(uint64_t contract_id) {
    switch (contract_id) {
#define LLAM_RUNTIME_CONTRACT(ID, Name, Signature, Effect) case ID: return LLAM_LLPM2_EFFECT_##Effect;
#include "managed_runtime_contracts.def"
#undef LLAM_RUNTIME_CONTRACT
    default: return LLAM_LLPM2_EFFECT_UNKNOWN;
    }
}

const llam_llpm2_range_t *llam_llpm2_find_safe(
    const llam_llpm2_t *m, size_t entry_index, uintptr_t pc) {
    if (m == NULL || entry_index >= m->entry_count) return NULL;
    const llam_llpm2_entry_t *e = &m->entries[entry_index];
    if (e->execution_class == LLAM_LLPM2_LEGACY ||
        (e->flags & LLAM_LLPM2_ENTRY_NO_PREEMPT) != 0U ||
        e->first_range > m->range_count ||
        e->range_count > m->range_count - e->first_range) return NULL;
    size_t low = e->first_range;
    size_t high = low + e->range_count;
    while (low < high) {
        size_t mid = low + (high - low) / 2U;
        const llam_llpm2_range_t *r = &m->ranges[mid];
        if (pc < r->begin) high = mid;
        else if (pc >= r->end) low = mid + 1U;
        else return r->kind == LLAM_LLPM2_SAFE ? r : NULL;
    }
    return NULL;
}

static llam_preempt_metadata_result_t validate(const llam_llpm2_t *m,
                                               uintptr_t base, size_t size) {
    const llam_llpm2_header_t *h = &m->header;
    unsigned build_nonzero = 0U;
    unsigned anchor_nonzero = 0U;
    size_t next_range = 0U;
    size_t next_call = 0U;
    if (m->entry_count == 0U || m->entry_count > LLAM_LLPM2_MAX_ENTRIES ||
        m->range_count > LLAM_LLPM2_MAX_RANGES || m->call_count > LLAM_LLPM2_MAX_CALLS ||
        m->entries == NULL || (m->range_count != 0U && m->ranges == NULL) ||
        (m->call_count != 0U && m->calls == NULL) ||
        (size != 0U && (base == 0U || size > UINTPTR_MAX - base))) {
        return LLAM_PREEMPT_METADATA_INVALID;
    }
    for (size_t i = 0U; i < sizeof(h->build_id); ++i) build_nonzero |= h->build_id[i];
    for (size_t i = 0U; i < sizeof(h->anchor_id); ++i) anchor_nonzero |= h->anchor_id[i];
    if (build_nonzero == 0U || h->toolchain_id == 0U || h->runtime_major == 0U) {
        return LLAM_PREEMPT_METADATA_INVALID;
    }
    if (((h->required_flags & LLAM_LLPM2_IMAGE_ANCHOR) != 0U) != (anchor_nonzero != 0U)) {
        return LLAM_PREEMPT_METADATA_INVALID;
    }
    if (!known_header(h)) return LLAM_PREEMPT_METADATA_UNSUPPORTED;
    for (size_t i = 0U; i < m->entry_count; ++i) {
        const llam_llpm2_entry_t *e = &m->entries[i];
        if (e->code_begin >= e->code_end || e->native_entry == 0U || e->type_id == 0U ||
            !in_image(e->native_entry, base, size, false) ||
            !in_image(e->code_begin, base, size, false) ||
            !in_image(e->code_end, base, size, true) ||
            !instruction_aligned(e->native_entry, h) ||
            !instruction_aligned(e->code_begin, h) || !instruction_aligned(e->code_end, h) ||
            (i != 0U && m->entries[i - 1U].code_end > e->code_begin) ||
            e->first_range != next_range || e->range_count > m->range_count - next_range ||
            e->first_call != next_call || e->call_count > m->call_count - next_call) {
            return LLAM_PREEMPT_METADATA_INVALID;
        }
        if (e->execution_class > LLAM_LLPM2_NATIVE_AFFINE ||
            (e->effects & ~LLPM2_EFFECTS_KNOWN) != 0U ||
            (e->flags & ~LLPM2_ENTRY_FLAGS_KNOWN) != 0U ||
            (e->required_isa & ~LLPM2_ISA_KNOWN) != 0U || e->frame_profile != 1U) {
            return LLAM_PREEMPT_METADATA_UNSUPPORTED;
        }
        if ((((h->required_flags & LLAM_LLPM2_GUARD_BINDINGS) != 0U) != (e->guard_bindings != 0U)) ||
            (e->guard_bindings != 0U &&
             ((e->guard_bindings & 7U) != 0U || !in_image(e->guard_bindings, base, size, false) ||
              e->guard_bindings > UINTPTR_MAX - 16U || !in_image(e->guard_bindings + 16U, base, size, true))) ||
            (e->native_end != 0U &&
             (e->native_end <= e->native_entry || !instruction_aligned(e->native_end, h) ||
              !in_image(e->native_end, base, size, true) || e->guard_bindings == 0U ||
              (e->native_entry < e->code_end && e->native_end > e->code_begin) ||
              (e->flags & LLAM_LLPM2_ENTRY_INTERNAL) != 0U))) {
            return LLAM_PREEMPT_METADATA_INVALID;
        }
        bool native_boundary = (e->flags & LLAM_LLPM2_ENTRY_NATIVE_BOUNDARY) != 0U;
        bool runtime_bridge = (e->flags & LLAM_LLPM2_ENTRY_RUNTIME_BRIDGE) != 0U;
        if (native_boundary && (e->execution_class != LLAM_LLPM2_LEGACY ||
            (e->flags & LLAM_LLPM2_ENTRY_INTERNAL) == 0U || e->native_end != 0U ||
            e->guard_bindings == 0U || e->native_entry != e->code_begin)) {
            return LLAM_PREEMPT_METADATA_INVALID;
        }
        if (runtime_bridge && (e->flags != (LLAM_LLPM2_ENTRY_INTERNAL | LLAM_LLPM2_ENTRY_RUNTIME_BRIDGE) ||
            e->execution_class != LLAM_LLPM2_LEGACY || e->native_entry != e->code_begin ||
            (e->effects != (UINT32_C(1) << LLAM_LLPM2_EFFECT_RUNTIME_CRITICAL) &&
             e->effects != (UINT32_C(1) << LLAM_LLPM2_EFFECT_NATIVE_AFFINE)) ||
            e->guard_bindings != 0U || e->native_end != 0U || e->range_count != 0U || e->call_count != 1U)) {
            return LLAM_PREEMPT_METADATA_INVALID;
        }
        if (e->execution_class == LLAM_LLPM2_LEGACY) {
            if (e->managed_entry != 0U) return LLAM_PREEMPT_METADATA_INVALID;
        } else if (e->managed_entry < e->code_begin || e->managed_entry >= e->code_end ||
                   !instruction_aligned(e->managed_entry, h) ||
                   (e->effects & (UINT32_C(1) << LLAM_LLPM2_EFFECT_MANAGED)) == 0U ||
                   (e->effects & (UINT32_C(1) << LLAM_LLPM2_EFFECT_UNKNOWN)) != 0U ||
                   (e->required_isa & LLAM_LLPM2_ISA_BASE) == 0U ||
                   e->guard_bindings == 0U ||
                   ((e->flags & LLAM_LLPM2_ENTRY_INTERNAL) == 0U && e->native_end == 0U) ||
                   (e->native_entry == e->managed_entry &&
                    (e->flags & LLAM_LLPM2_ENTRY_INTERNAL) == 0U)) {
            return LLAM_PREEMPT_METADATA_INVALID;
        }
        if (e->execution_class == LLAM_LLPM2_PORTABLE &&
            (e->effects & (UINT32_C(1) << LLAM_LLPM2_EFFECT_NATIVE_AFFINE)) != 0U) {
            return LLAM_PREEMPT_METADATA_INVALID;
        }
        uintptr_t previous_end = e->code_begin;
        for (size_t j = next_range; j < next_range + e->range_count; ++j) {
            const llam_llpm2_range_t *r = &m->ranges[j];
            if (r->entry_index != i || r->begin < previous_end || r->begin >= r->end ||
                r->end > e->code_end || !instruction_aligned(r->begin, h) ||
                !instruction_aligned(r->end, h)) return LLAM_PREEMPT_METADATA_INVALID;
            if (r->kind < LLAM_LLPM2_SAFE || r->kind > LLAM_LLPM2_UNSAFE_NO_PREEMPT) {
                return LLAM_PREEMPT_METADATA_UNSUPPORTED;
            }
            if (r->stack_alignment_log2 > 16U ||
                (r->kind == LLAM_LLPM2_SAFE &&
                 (r->stack_alignment_log2 < (h->architecture ==
                   LLAM_PREEMPT_METADATA_ARCH_AARCH64 ? 4U : 3U) ||
                  e->execution_class == LLAM_LLPM2_LEGACY ||
                  (e->flags & LLAM_LLPM2_ENTRY_NO_PREEMPT) != 0U))) {
                return LLAM_PREEMPT_METADATA_INVALID;
            }
            previous_end = r->end;
        }
        uintptr_t previous_site = e->code_begin;
        bool in_thunk = false;
        unsigned thunk_phase = 0U;
        for (size_t j = next_call; j < next_call + e->call_count; ++j) {
            const llam_llpm2_call_t *c = &m->calls[j];
            bool thunk = (c->flags & LLAM_LLPM2_CALL_NATIVE_THUNK) != 0U;
            bool runtime_body = (c->flags & LLAM_LLPM2_CALL_RUNTIME_BODY) != 0U;
            unsigned guard = c->flags & (LLAM_LLPM2_CALL_GUARD_ENTER | LLAM_LLPM2_CALL_GUARD_LEAVE);
            bool first_in_region = j == next_call || (thunk && !in_thunk);
            if (thunk && !in_thunk) previous_site = e->native_entry;
            if (in_thunk && !thunk) return LLAM_PREEMPT_METADATA_INVALID;
            in_thunk = thunk;
            uintptr_t call_begin = thunk ? e->native_entry : e->code_begin;
            uintptr_t call_end = thunk && !native_boundary ? e->native_end : e->code_end;
            if (c->entry_index != i || c->site < previous_site || c->site < call_begin || c->site >= call_end ||
                (!first_in_region && c->site <= previous_site) ||
                c->continuation <= c->site || c->continuation > call_end ||
                !instruction_aligned(c->site, h) || !instruction_aligned(c->continuation, h) ||
                c->type_id == 0U ||
                (guard == 0U && !runtime_body &&
                 (((c->flags & LLAM_LLPM2_CALL_INDIRECT) != 0U) != (c->native_target == 0U))) ||
                (guard != 0U &&
                 (!thunk || c->effect != LLAM_LLPM2_EFFECT_RUNTIME_CRITICAL ||
                  (c->flags & LLAM_LLPM2_CALL_INDIRECT) == 0U || e->guard_bindings == 0U ||
                  c->type_id != guard_type_id(h) ||
                  (guard != LLAM_LLPM2_CALL_GUARD_ENTER && guard != LLAM_LLPM2_CALL_GUARD_LEAVE) ||
                  c->native_target != e->guard_bindings + (guard == LLAM_LLPM2_CALL_GUARD_LEAVE ? 8U : 0U))) ||
                (guard == 0U && c->native_target != 0U && !in_image(c->native_target, base, size, false)) ||
                llam_llpm2_find_safe(m, i, c->site) != NULL) {
                return LLAM_PREEMPT_METADATA_INVALID;
            }
            if (c->effect > LLAM_LLPM2_EFFECT_UNKNOWN ||
                (c->flags & ~LLPM2_CALL_FLAGS_KNOWN) != 0U) {
                return LLAM_PREEMPT_METADATA_UNSUPPORTED;
            }
            if (runtime_body != runtime_bridge || (!runtime_body && c->contract_id != 0U) ||
                (runtime_body && (c->flags != (LLAM_LLPM2_CALL_INDIRECT | LLAM_LLPM2_CALL_RUNTIME_BODY) ||
                 c->effect != llam_llpm2_runtime_effect(c->contract_id) || c->native_target == 0U ||
                 (c->native_target & 7U) != 0U || c->native_target > UINTPTR_MAX - 8U ||
                 !in_image(c->native_target + 8U, base, size, true) || c->type_id != e->type_id ||
                 c->type_id != llam_llpm2_runtime_type_id(c->contract_id, h->architecture, h->native_abi)))) {
                return LLAM_PREEMPT_METADATA_INVALID;
            }
            if (c->flags != 0U && c->flags != LLAM_LLPM2_CALL_NATIVE_THUNK &&
                c->flags != (LLAM_LLPM2_CALL_INDIRECT | LLAM_LLPM2_CALL_NATIVE_THUNK | LLAM_LLPM2_CALL_GUARD_ENTER) &&
                c->flags != (LLAM_LLPM2_CALL_INDIRECT | LLAM_LLPM2_CALL_NATIVE_THUNK | LLAM_LLPM2_CALL_GUARD_LEAVE) &&
                c->flags != (LLAM_LLPM2_CALL_INDIRECT | LLAM_LLPM2_CALL_NATIVE_THUNK | LLAM_LLPM2_CALL_FOREIGN_BODY) &&
                c->flags != (LLAM_LLPM2_CALL_INDIRECT | LLAM_LLPM2_CALL_RUNTIME_BODY)) {
                return LLAM_PREEMPT_METADATA_INVALID;
            }
            if ((c->flags & LLAM_LLPM2_CALL_FOREIGN_BODY) != 0U &&
                (!native_boundary || !thunk || guard != 0U || c->native_target != 0U ||
                 c->effect == LLAM_LLPM2_EFFECT_MANAGED ||
                 c->type_id != e->type_id ||
                 (c->flags & LLAM_LLPM2_CALL_INDIRECT) == 0U)) return LLAM_PREEMPT_METADATA_INVALID;
            if ((e->effects & (UINT32_C(1) << c->effect)) == 0U ||
                (e->execution_class != LLAM_LLPM2_LEGACY &&
                 c->effect == LLAM_LLPM2_EFFECT_UNKNOWN)) return LLAM_PREEMPT_METADATA_INVALID;
            if (thunk) {
                if (guard == LLAM_LLPM2_CALL_GUARD_ENTER && thunk_phase == 0U) thunk_phase = 1U;
                else if (guard == 0U && c->effect == LLAM_LLPM2_EFFECT_MANAGED &&
                         c->native_target == e->native_entry && c->type_id == e->type_id && thunk_phase == 1U) {
                    thunk_phase = 2U;
                } else if (native_boundary && (c->flags & LLAM_LLPM2_CALL_FOREIGN_BODY) != 0U &&
                           thunk_phase == 1U) {
                    thunk_phase = 2U;
                } else if (guard == LLAM_LLPM2_CALL_GUARD_LEAVE && thunk_phase == 2U &&
                           (e->flags & LLAM_LLPM2_ENTRY_NORETURN) == 0U) thunk_phase = 3U;
                else return LLAM_PREEMPT_METADATA_INVALID;
            }
            previous_site = c->site;
        }
        if ((e->native_end != 0U || native_boundary) &&
            thunk_phase != ((e->flags & LLAM_LLPM2_ENTRY_NORETURN) != 0U ? 2U : 3U)) {
            return LLAM_PREEMPT_METADATA_INVALID;
        }
        next_range += e->range_count;
        next_call += e->call_count;
    }
    if (next_range != m->range_count || next_call != m->call_count) {
        return LLAM_PREEMPT_METADATA_INVALID;
    }
    /* A native thunk may lie outside its body's extent, but cannot alias ANY
     * safe interval. Internal entries are explicitly unavailable to native callers. */
    for (size_t i = 0U; i < m->entry_count; ++i) {
        const llam_llpm2_entry_t *e = &m->entries[i];
        if ((e->flags & LLAM_LLPM2_ENTRY_INTERNAL) != 0U) continue;
        size_t low = 0U, high = m->entry_count;
        while (low < high) {
            size_t mid = low + (high - low) / 2U;
            const llam_llpm2_entry_t *other = &m->entries[mid];
            if (e->native_entry < other->code_begin) high = mid;
            else if (e->native_entry >= other->code_end) low = mid + 1U;
            else {
                if (llam_llpm2_find_safe(m, mid, e->native_entry) != NULL) {
                    return LLAM_PREEMPT_METADATA_INVALID;
                }
                break;
            }
        }
    }
    return LLAM_PREEMPT_METADATA_OK;
}

void llam_llpm2_destroy(llam_llpm2_t *m) {
    if (m == NULL) return;
    free(m->calls);
    free(m->ranges);
    free(m->entries);
    memset(m, 0, sizeof(*m));
}

static llam_preempt_metadata_result_t unsupported(
    const llam_llpm2_requirements_t *r, uint32_t flags, size_t bytes, size_t *consumed) {
    if (consumed != NULL) *consumed = bytes;
    return ((r != NULL && r->required) || (flags & LLAM_LLPM2_REQUIRED) != 0U)
        ? LLAM_PREEMPT_METADATA_UNSUPPORTED : LLAM_PREEMPT_METADATA_IGNORED_OPTIONAL;
}

llam_preempt_metadata_result_t llam_llpm2_parse(
    const void *opaque, size_t available, uintptr_t address,
    uintptr_t base, size_t size, const llam_llpm2_requirements_t *requirements,
    llam_llpm2_t *out, size_t *consumed) {
    const unsigned char *bytes = opaque;
    llam_llpm2_t m = {0};
    llam_preempt_metadata_result_t result;
    if (out != NULL) memset(out, 0, sizeof(*out));
    if (consumed != NULL) *consumed = 0U;
    if (out == NULL || (bytes == NULL && available != 0U)) return LLAM_PREEMPT_METADATA_INVALID;
    if (available == 0U) return LLAM_PREEMPT_METADATA_ABSENT;
    if (available < LLAM_LLPM2_HEADER_BYTES ||
        read32(bytes) != LLAM_PREEMPT_METADATA_MAGIC) return LLAM_PREEMPT_METADATA_INVALID;
    uint64_t total = read64(bytes + 8U);
    uint32_t flags = read32(bytes + 48U);
    if (total < LLAM_LLPM2_HEADER_BYTES || total > available ||
        total > LLAM_LLPM2_MAX_BYTES || total > UINTPTR_MAX - address) {
        return LLAM_PREEMPT_METADATA_INVALID;
    }
    if (read16(bytes + 4U) != LLAM_LLPM2_VERSION ||
        read16(bytes + 6U) != LLAM_LLPM2_HEADER_BYTES ||
        read16(bytes + 28U) != LLAM_LLPM2_ENTRY_BYTES ||
        read16(bytes + 30U) != LLAM_LLPM2_RANGE_BYTES ||
        read16(bytes + 32U) != LLAM_LLPM2_CALL_BYTES || read16(bytes + 46U) != 1U) {
        return unsupported(requirements, flags, (size_t)total, consumed);
    }
    m.entry_count = read32(bytes + 16U);
    m.range_count = read32(bytes + 20U);
    m.call_count = read32(bytes + 24U);
    if (m.entry_count == 0U || m.entry_count > LLAM_LLPM2_MAX_ENTRIES ||
        m.range_count > LLAM_LLPM2_MAX_RANGES || m.call_count > LLAM_LLPM2_MAX_CALLS) {
        return LLAM_PREEMPT_METADATA_INVALID;
    }
    size_t range_offset = LLAM_LLPM2_HEADER_BYTES + m.entry_count * LLAM_LLPM2_ENTRY_BYTES;
    size_t call_offset = range_offset + m.range_count * LLAM_LLPM2_RANGE_BYTES;
    size_t expected_bytes = call_offset + m.call_count * LLAM_LLPM2_CALL_BYTES;
    if (total != expected_bytes || read64(bytes + 80U) != LLAM_LLPM2_HEADER_BYTES ||
        read64(bytes + 88U) != range_offset || read64(bytes + 96U) != call_offset) {
        return LLAM_PREEMPT_METADATA_INVALID;
    }
    m.header = (llam_llpm2_header_t){
        .architecture = read16(bytes + 34U), .native_abi = read16(bytes + 36U),
        .managed_major = read16(bytes + 38U), .managed_minor = read16(bytes + 40U),
        .runtime_major = read16(bytes + 42U), .runtime_minor = read16(bytes + 44U),
        .required_flags = flags, .optional_flags = read32(bytes + 52U),
        .toolchain_id = read64(bytes + 72U), .executable_identity = read64(bytes + 104U)
    };
    memcpy(m.header.build_id, bytes + 56U, sizeof(m.header.build_id));
    memcpy(m.header.anchor_id, bytes + 112U, sizeof(m.header.anchor_id));
    if (!known_header(&m.header) ||
        (requirements != NULL &&
         ((requirements->architecture != 0U && requirements->architecture != m.header.architecture) ||
          (requirements->native_abi != 0U && requirements->native_abi != m.header.native_abi) ||
          (requirements->runtime_major != 0U &&
           (requirements->runtime_major != m.header.runtime_major ||
            requirements->runtime_minor < m.header.runtime_minor)) ||
          (requirements->toolchain_id != 0U && requirements->toolchain_id != m.header.toolchain_id) ||
          (requirements->require_sealed &&
           ((flags & LLPM2_SEAL_FLAGS) != LLPM2_SEAL_FLAGS || m.header.executable_identity == 0U))))) {
        return unsupported(requirements, flags, (size_t)total, consumed);
    }
    m.entries = calloc(m.entry_count, sizeof(*m.entries));
    if (m.range_count != 0U) m.ranges = calloc(m.range_count, sizeof(*m.ranges));
    if (m.call_count != 0U) m.calls = calloc(m.call_count, sizeof(*m.calls));
    if (m.entries == NULL || (m.range_count != 0U && m.ranges == NULL) ||
        (m.call_count != 0U && m.calls == NULL)) {
        llam_llpm2_destroy(&m);
        errno = ENOMEM;
        return LLAM_PREEMPT_METADATA_INVALID;
    }
    for (size_t i = 0U; i < m.entry_count; ++i) {
        size_t offset = LLAM_LLPM2_HEADER_BYTES + i * LLAM_LLPM2_ENTRY_BYTES;
        const unsigned char *p = bytes + offset;
        llam_llpm2_entry_t *e = &m.entries[i];
        if (!resolve64(address + offset, read64(p), &e->native_entry) ||
            !resolve64(address + offset + 16U, read64(p + 16U), &e->code_begin) ||
            !resolve64(address + offset + 24U, read64(p + 24U), &e->code_end)) goto invalid;
        if ((flags & LLAM_LLPM2_GUARD_BINDINGS) != 0U) {
            if (!resolve64(address + offset + 80U, read64(p + 80U), &e->guard_bindings)) goto invalid;
        } else if (read64(p + 80U) != 0U) goto invalid;
        if (read64(p + 88U) != 0U &&
            !resolve64(address + offset + 88U, read64(p + 88U), &e->native_end)) goto invalid;
        e->execution_class = read32(p + 32U);
        if (e->execution_class == LLAM_LLPM2_LEGACY) {
            if (read64(p + 8U) != 0U) goto invalid;
        } else if (!resolve64(address + offset + 8U, read64(p + 8U), &e->managed_entry)) goto invalid;
        e->effects = read32(p + 36U);
        e->required_isa = read64(p + 40U);
        e->frame_profile = read32(p + 48U);
        e->first_range = read32(p + 52U);
        e->range_count = read32(p + 56U);
        e->first_call = read32(p + 60U);
        e->call_count = read32(p + 64U);
        e->flags = read32(p + 68U);
        e->type_id = read64(p + 72U);
    }
    for (size_t i = 0U; i < m.range_count; ++i) {
        size_t offset = range_offset + i * LLAM_LLPM2_RANGE_BYTES;
        const unsigned char *p = bytes + offset;
        llam_llpm2_range_t *range = &m.ranges[i];
        range->entry_index = read32(p);
        range->kind = read16(p + 4U);
        range->stack_alignment_log2 = read32(p + 24U);
        if (read16(p + 6U) != 0U || read32(p + 28U) != 0U ||
            !resolve64(address + offset + 8U, read64(p + 8U), &range->begin) ||
            !resolve64(address + offset + 16U, read64(p + 16U), &range->end)) goto invalid;
    }
    for (size_t i = 0U; i < m.call_count; ++i) {
        size_t offset = call_offset + i * LLAM_LLPM2_CALL_BYTES;
        const unsigned char *p = bytes + offset;
        llam_llpm2_call_t *call = &m.calls[i];
        call->entry_index = read32(p);
        call->effect = read16(p + 4U);
        call->flags = read16(p + 6U);
        call->type_id = read64(p + 32U);
        call->contract_id = read64(p + 40U);
        if (!resolve64(address + offset + 8U, read64(p + 8U), &call->site) ||
            !resolve64(address + offset + 16U, read64(p + 16U), &call->continuation)) goto invalid;
        if ((call->flags & LLAM_LLPM2_CALL_INDIRECT) != 0U &&
            (call->flags & (LLAM_LLPM2_CALL_GUARD_ENTER | LLAM_LLPM2_CALL_GUARD_LEAVE |
                            LLAM_LLPM2_CALL_RUNTIME_BODY)) == 0U) {
            if (read64(p + 24U) != 0U) goto invalid;
        } else if (!resolve64(address + offset + 24U, read64(p + 24U), &call->native_target)) goto invalid;
    }
    result = validate(&m, base, size);
    if (result == LLAM_PREEMPT_METADATA_UNSUPPORTED) goto not_supported;
    if (result != LLAM_PREEMPT_METADATA_OK) goto invalid;
    if (requirements != NULL && requirements->available_isa != 0U) {
        for (size_t i = 0U; i < m.entry_count; ++i) {
            if ((m.entries[i].required_isa & ~requirements->available_isa) != 0U) goto not_supported;
        }
    }
    *out = m;
    if (consumed != NULL) *consumed = (size_t)total;
    return LLAM_PREEMPT_METADATA_OK;
not_supported:
    llam_llpm2_destroy(&m);
    return unsupported(requirements, flags, (size_t)total, consumed);
invalid:
    llam_llpm2_destroy(&m);
    return LLAM_PREEMPT_METADATA_INVALID;
}

static bool put_address(unsigned char *bytes, size_t offset, uintptr_t mapping,
                         uintptr_t address) {
    uint64_t displacement;
    if (!encode_relative(address, mapping + offset, &displacement)) return false;
    write64(bytes + offset, displacement);
    return true;
}

int llam_llpm2_encode(const llam_llpm2_t *m, uintptr_t mapping,
                      void *destination, size_t capacity, size_t *written) {
    if (written != NULL) *written = 0U;
    if (m == NULL || destination == NULL || validate(m, 0U, 0U) != LLAM_PREEMPT_METADATA_OK) {
        errno = EINVAL;
        return -1;
    }
    size_t range_offset = LLAM_LLPM2_HEADER_BYTES + m->entry_count * LLAM_LLPM2_ENTRY_BYTES;
    size_t call_offset = range_offset + m->range_count * LLAM_LLPM2_RANGE_BYTES;
    size_t total = call_offset + m->call_count * LLAM_LLPM2_CALL_BYTES;
    if (total > capacity || total > UINTPTR_MAX - mapping) {
        errno = ENOSPC;
        return -1;
    }
    /* Staging is deliberately cold: failed relative-address validation must not
     * leave a partially published certificate in the caller's mapped section. */
    unsigned char *bytes = calloc(1U, total);
    if (bytes == NULL) return -1;
    write32(bytes, LLAM_PREEMPT_METADATA_MAGIC);
    write16(bytes + 4U, LLAM_LLPM2_VERSION);
    write16(bytes + 6U, LLAM_LLPM2_HEADER_BYTES);
    write64(bytes + 8U, total);
    write32(bytes + 16U, (uint32_t)m->entry_count);
    write32(bytes + 20U, (uint32_t)m->range_count);
    write32(bytes + 24U, (uint32_t)m->call_count);
    write16(bytes + 28U, LLAM_LLPM2_ENTRY_BYTES);
    write16(bytes + 30U, LLAM_LLPM2_RANGE_BYTES);
    write16(bytes + 32U, LLAM_LLPM2_CALL_BYTES);
    write16(bytes + 34U, m->header.architecture);
    write16(bytes + 36U, m->header.native_abi);
    write16(bytes + 38U, m->header.managed_major);
    write16(bytes + 40U, m->header.managed_minor);
    write16(bytes + 42U, m->header.runtime_major);
    write16(bytes + 44U, m->header.runtime_minor);
    write16(bytes + 46U, 1U);
    write32(bytes + 48U, m->header.required_flags);
    write32(bytes + 52U, m->header.optional_flags);
    memcpy(bytes + 56U, m->header.build_id, sizeof(m->header.build_id));
    write64(bytes + 72U, m->header.toolchain_id);
    write64(bytes + 80U, LLAM_LLPM2_HEADER_BYTES);
    write64(bytes + 88U, range_offset);
    write64(bytes + 96U, call_offset);
    write64(bytes + 104U, m->header.executable_identity);
    memcpy(bytes + 112U, m->header.anchor_id, sizeof(m->header.anchor_id));
    for (size_t i = 0U; i < m->entry_count; ++i) {
        const llam_llpm2_entry_t *e = &m->entries[i];
        size_t offset = LLAM_LLPM2_HEADER_BYTES + i * LLAM_LLPM2_ENTRY_BYTES;
        unsigned char *p = bytes + offset;
        if (!put_address(bytes, offset, mapping, e->native_entry) ||
            (e->execution_class != LLAM_LLPM2_LEGACY &&
             !put_address(bytes, offset + 8U, mapping, e->managed_entry)) ||
            !put_address(bytes, offset + 16U, mapping, e->code_begin) ||
            !put_address(bytes, offset + 24U, mapping, e->code_end)) goto unencodable;
        write32(p + 32U, e->execution_class);
        write32(p + 36U, e->effects);
        write64(p + 40U, e->required_isa);
        write32(p + 48U, e->frame_profile);
        write32(p + 52U, e->first_range);
        write32(p + 56U, e->range_count);
        write32(p + 60U, e->first_call);
        write32(p + 64U, e->call_count);
        write32(p + 68U, e->flags);
        write64(p + 72U, e->type_id);
        if ((m->header.required_flags & LLAM_LLPM2_GUARD_BINDINGS) != 0U &&
            !put_address(bytes, offset + 80U, mapping, e->guard_bindings)) goto unencodable;
        if (e->native_end != 0U && !put_address(bytes, offset + 88U, mapping, e->native_end)) goto unencodable;
    }
    for (size_t i = 0U; i < m->range_count; ++i) {
        const llam_llpm2_range_t *r = &m->ranges[i];
        size_t offset = range_offset + i * LLAM_LLPM2_RANGE_BYTES;
        write32(bytes + offset, r->entry_index);
        write16(bytes + offset + 4U, r->kind);
        write32(bytes + offset + 24U, r->stack_alignment_log2);
        if (!put_address(bytes, offset + 8U, mapping, r->begin) ||
            !put_address(bytes, offset + 16U, mapping, r->end)) goto unencodable;
    }
    for (size_t i = 0U; i < m->call_count; ++i) {
        const llam_llpm2_call_t *c = &m->calls[i];
        size_t offset = call_offset + i * LLAM_LLPM2_CALL_BYTES;
        write32(bytes + offset, c->entry_index);
        write16(bytes + offset + 4U, c->effect);
        write16(bytes + offset + 6U, c->flags);
        write64(bytes + offset + 32U, c->type_id);
        write64(bytes + offset + 40U, c->contract_id);
        if (!put_address(bytes, offset + 8U, mapping, c->site) ||
            !put_address(bytes, offset + 16U, mapping, c->continuation) ||
            (((c->flags & LLAM_LLPM2_CALL_INDIRECT) == 0U ||
              (c->flags & (LLAM_LLPM2_CALL_GUARD_ENTER | LLAM_LLPM2_CALL_GUARD_LEAVE |
                           LLAM_LLPM2_CALL_RUNTIME_BODY)) != 0U) &&
             !put_address(bytes, offset + 24U, mapping, c->native_target))) goto unencodable;
    }
    memcpy(destination, bytes, total);
    free(bytes);
    if (written != NULL) *written = total;
    return 0;
unencodable:
    free(bytes);
    errno = EOVERFLOW;
    return -1;
}
