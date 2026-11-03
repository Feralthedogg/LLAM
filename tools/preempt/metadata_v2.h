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

#ifndef LLAM_PREEMPT_METADATA_V2_H
#define LLAM_PREEMPT_METADATA_V2_H

#include "metadata.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Wire sizes, never sizeof(C struct). All integers are little endian. Address
 * fields are signed 64-bit displacements from the field's loaded address. Tables
 * are contiguous in this order; this avoids offset aliases and parser ambiguity.
 * Each record describes one linked image contribution, not a trusted publisher. */
#define LLAM_LLPM2_VERSION 2U
#define LLAM_LLPM2_HEADER_BYTES 128U
#define LLAM_LLPM2_ENTRY_BYTES 96U
#define LLAM_LLPM2_RANGE_BYTES 32U
#define LLAM_LLPM2_CALL_BYTES 48U
#define LLAM_LLPM2_MAX_ENTRIES 4096U
#define LLAM_LLPM2_MAX_RANGES 65536U
#define LLAM_LLPM2_MAX_CALLS 65536U
#define LLAM_LLPM2_MAX_BYTES (8U * 1024U * 1024U)
#define LLAM_LLPM2_SECTION ".llam.managed"
#define LLAM_LLPM2_PROFILE_LLVM23 UINT64_C(0x4c4c414d17010001)

enum llam_llpm2_native_abi {
    LLAM_LLPM2_ABI_SYSV = 1,
    LLAM_LLPM2_ABI_DARWIN = 2,
    LLAM_LLPM2_ABI_WIN64 = 3
};

enum llam_llpm2_flag {
    LLAM_LLPM2_REQUIRED = 1U,
    LLAM_LLPM2_MACHINE_VERIFIED = 2U,
    LLAM_LLPM2_LINK_VERIFIED = 4U,
    LLAM_LLPM2_IMMUTABLE_TEXT = 8U,
    LLAM_LLPM2_IMAGE_ANCHOR = 16U,
    LLAM_LLPM2_GUARD_BINDINGS = 32U
};

enum llam_llpm2_class {
    LLAM_LLPM2_LEGACY = 0,
    LLAM_LLPM2_PORTABLE = 1,
    LLAM_LLPM2_NATIVE_AFFINE = 2
};

enum llam_llpm2_effect {
    LLAM_LLPM2_EFFECT_MANAGED = 0,
    LLAM_LLPM2_EFFECT_RUNTIME_CRITICAL = 1,
    LLAM_LLPM2_EFFECT_FOREIGN_FAST = 2,
    LLAM_LLPM2_EFFECT_FOREIGN_BLOCKING = 3,
    LLAM_LLPM2_EFFECT_NATIVE_AFFINE = 4,
    LLAM_LLPM2_EFFECT_UNKNOWN = 5
};

enum llam_llpm2_range_kind {
    LLAM_LLPM2_SAFE = 1,
    LLAM_LLPM2_UNSAFE_RUNTIME = 2,
    LLAM_LLPM2_UNSAFE_FOREIGN = 3,
    LLAM_LLPM2_UNSAFE_STACK = 4,
    LLAM_LLPM2_UNSAFE_NATIVE_ENTRY = 5,
    LLAM_LLPM2_UNSAFE_EXCLUSIVE = 6,
    LLAM_LLPM2_UNSAFE_NO_PREEMPT = 7
};

enum llam_llpm2_entry_flag {
    LLAM_LLPM2_ENTRY_INTERNAL = 1U,
    LLAM_LLPM2_ENTRY_NO_PREEMPT = 2U,
    LLAM_LLPM2_ENTRY_NORETURN = 4U,
    LLAM_LLPM2_ENTRY_NATIVE_BOUNDARY = 8U,
    LLAM_LLPM2_ENTRY_RUNTIME_BRIDGE = 16U
};

enum llam_llpm2_call_flag {
    LLAM_LLPM2_CALL_INDIRECT = 1U,
    LLAM_LLPM2_CALL_TAIL = 2U,
    LLAM_LLPM2_CALL_NATIVE_THUNK = 4U,
    LLAM_LLPM2_CALL_GUARD_ENTER = 8U,
    LLAM_LLPM2_CALL_GUARD_LEAVE = 16U,
    LLAM_LLPM2_CALL_FOREIGN_BODY = 32U,
    LLAM_LLPM2_CALL_RUNTIME_BODY = 64U
};

enum llam_llpm2_isa {
    LLAM_LLPM2_ISA_BASE = 1U,
    LLAM_LLPM2_ISA_FP_SIMD = 2U,
    LLAM_LLPM2_ISA_AVX = 4U,
    LLAM_LLPM2_ISA_AVX512 = 8U,
    LLAM_LLPM2_ISA_LSE = 16U
};

typedef struct llam_llpm2_header {
    uint16_t architecture;
    uint16_t native_abi;
    uint16_t managed_major;
    uint16_t managed_minor;
    uint16_t runtime_major;
    uint16_t runtime_minor;
    uint32_t required_flags;
    uint32_t optional_flags;
    unsigned char build_id[16];
    uint64_t toolchain_id;
    uint64_t executable_identity;
    unsigned char anchor_id[16]; /* Unique compiler contribution; loader symbol identity. */
} llam_llpm2_header_t;

typedef struct llam_llpm2_entry {
    uintptr_t native_entry;
    uintptr_t managed_entry;
    uintptr_t code_begin;
    uintptr_t code_end;
    uint32_t execution_class;
    uint32_t effects; /* Bitset of llam_llpm2_effect, including MANAGED. */
    uint64_t required_isa;
    uint32_t frame_profile;
    uint32_t first_range;
    uint32_t range_count;
    uint32_t first_call;
    uint32_t call_count;
    uint32_t flags;
    uint64_t type_id;
    uintptr_t guard_bindings; /* Read-only, eagerly relocated enter/leave pointer pair. */
    uintptr_t native_end;     /* Zero for entries without a generated native thunk. */
} llam_llpm2_entry_t;

typedef struct llam_llpm2_range {
    uint32_t entry_index;
    uint16_t kind;
    uintptr_t begin;
    uintptr_t end;
    uint32_t stack_alignment_log2;
} llam_llpm2_range_t;

typedef struct llam_llpm2_call {
    uint32_t entry_index;
    uint16_t effect;
    uint16_t flags;
    uintptr_t site;
    uintptr_t continuation;
    uintptr_t native_target; /* Guard/runtime: immutable pointer slot; foreign: zero; direct: native entry. */
    uint64_t type_id;
    uint64_t contract_id; /* Runtime body only: stable ID in managed_runtime_contracts.def. */
} llam_llpm2_call_t;

typedef struct llam_llpm2 {
    llam_llpm2_header_t header;
    size_t entry_count;
    size_t range_count;
    size_t call_count;
    llam_llpm2_entry_t *entries;
    llam_llpm2_range_t *ranges;
    llam_llpm2_call_t *calls;
} llam_llpm2_t;

typedef struct llam_llpm2_requirements {
    uint16_t architecture; /* Zero accepts any known architecture. */
    uint16_t native_abi;   /* Zero accepts any known native ABI. */
    uint16_t runtime_major;
    uint16_t runtime_minor;
    uint64_t toolchain_id; /* Zero performs structural parsing only. */
    uint64_t available_isa;
    bool required;
    bool require_sealed;
} llam_llpm2_requirements_t;

/* Exact scalar C ABI identity for an SDK runtime contract, or zero when the
 * contract/target is unsupported. No loaded function address is trusted here. */
uint64_t llam_llpm2_runtime_type_id(uint64_t contract_id, uint16_t architecture, uint16_t native_abi);
uint16_t llam_llpm2_runtime_effect(uint64_t contract_id);

/* mapping_address is the address of bytes in the final image, independent of
 * the parser's buffer address (offline tools copy sections). executable bounds
 * are optional for structural parsing; loaded-image admission must additionally
 * validate each address against OS-reported executable sections and loader pins.
 * Outputs are empty and consumed is zero on malformed input. Unsupported bounded
 * records report their size so an optional caller can skip them deliberately. */
llam_preempt_metadata_result_t llam_llpm2_parse(
    const void *bytes, size_t available, uintptr_t mapping_address,
    uintptr_t executable_base, size_t executable_size,
    const llam_llpm2_requirements_t *requirements,
    llam_llpm2_t *out, size_t *consumed);

void llam_llpm2_destroy(llam_llpm2_t *metadata);

/* Encoder accepts resolved addresses. Output is untouched on invalid input or
 * insufficient space. It validates the same contracts as the parser. */
int llam_llpm2_encode(const llam_llpm2_t *metadata, uintptr_t mapping_address,
                      void *destination, size_t capacity, size_t *written);

/* Entry ranges are sorted and disjoint. Missing PCs, unsafe ranges, native
 * thunks and unknown entries always return NULL. No allocation or locks. */
const llam_llpm2_range_t *llam_llpm2_find_safe(
    const llam_llpm2_t *metadata, size_t entry_index, uintptr_t pc);

#ifdef __cplusplus
}
#endif
#endif /* LLAM_PREEMPT_METADATA_V2_H */
