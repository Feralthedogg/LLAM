/**
 * @file src/internal/runtime_public_slot.h
 * @brief Slot+generation public handle table helpers.
 *
 * @details
 * Runtime objects that can be destroyed or cached behind opaque public handles
 * use these helpers to reject stale, consumed, forged, or wrong-family handles
 * before any object storage is dereferenced.
 *
 * @copyright Copyright 2026 Feralthedogg
 *
 * @par License
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef LLAM_RUNTIME_PUBLIC_SLOT_H
#define LLAM_RUNTIME_PUBLIC_SLOT_H

#include <limits.h>

#if defined(__linux__)
#include <sys/syscall.h>
#endif

#define LLAM_PUBLIC_HANDLE_MAX_SLOTS ((size_t)UINT32_MAX)
#define LLAM_PUBLIC_SLOT_FREE_NONE ((size_t)0U)
#define LLAM_PUBLIC_SLOT_MAX_GENERATION UINT32_MAX
#define LLAM_PUBLIC_HANDLE_FAMILY_BITS 4U
#define LLAM_PUBLIC_HANDLE_FAMILY_MASK ((uint32_t)((1U << LLAM_PUBLIC_HANDLE_FAMILY_BITS) - 1U))
#define LLAM_PUBLIC_HANDLE_FAMILY_MAX_EPOCH (UINT32_MAX >> LLAM_PUBLIC_HANDLE_FAMILY_BITS)

#define LLAM_PUBLIC_HANDLE_FAMILY_TASK 1U
#define LLAM_PUBLIC_HANDLE_FAMILY_MUTEX 2U
#define LLAM_PUBLIC_HANDLE_FAMILY_COND 3U
#define LLAM_PUBLIC_HANDLE_FAMILY_CHANNEL 4U
#define LLAM_PUBLIC_HANDLE_FAMILY_CANCEL_TOKEN 5U
#define LLAM_PUBLIC_HANDLE_FAMILY_TASK_GROUP 6U
#define LLAM_PUBLIC_HANDLE_FAMILY_IO_BUFFER 7U
#define LLAM_PUBLIC_HANDLE_EPOCH_MASK LLAM_PUBLIC_HANDLE_FAMILY_MAX_EPOCH
#define LLAM_PUBLIC_SLOT_WORD_BITS ((unsigned)(sizeof(uintptr_t) * CHAR_BIT))
#define LLAM_PUBLIC_ACTIVE_OP_RESERVED_THRESHOLD ((SIZE_MAX / 2U) - 1U)
#define LLAM_PUBLIC_ACTIVE_OP_BEGIN_LIMIT (LLAM_PUBLIC_ACTIVE_OP_RESERVED_THRESHOLD - 1U)
#define LLAM_PUBLIC_ACTIVE_OP_BUSY_SENTINEL SIZE_MAX

typedef struct llam_public_slot {
    void *object;
    /*
     * generation is the sealed public token. epoch is the monotonic internal
     * counter used for ABA protection. The affine seal fields define a
     * slot-stable permutation over the public token space, so no stale
     * generation can repeat before the internal epoch is exhausted.
     */
    uint32_t generation;
    uint32_t epoch;
    uint32_t seal_multiplier;
    uint32_t seal_addend;
    size_t next_free_plus_one;
} llam_public_slot_t;

_Static_assert(sizeof(llam_public_slot_t) <= 32U, "public handle slots must stay cache-compact");

typedef struct llam_public_slot_table {
    llam_public_slot_t *slots;
    size_t count;
    size_t capacity;
    size_t free_head_plus_one;
    /* Cold-path table key mixed from OS entropy and the creating runtime. */
    uint64_t handle_secret;
} llam_public_slot_table_t;

static inline uint32_t llam_public_slot_next_generation(uint32_t generation) {
    generation += 1U;
    generation += (uint32_t)(generation == 0U);
    return generation;
}

static inline uintptr_t llam_public_slot_encode_handle(size_t slot, uint32_t generation, unsigned shift) {
    uintptr_t slot_word = (uintptr_t)slot + 1U;

    if (LLAM_UNLIKELY(generation == 0U ||
                      shift == 0U ||
                      shift >= LLAM_PUBLIC_SLOT_WORD_BITS ||
                      slot_word == 0U ||
                      (shift > 0U && slot_word > (UINTPTR_MAX >> shift)) ||
                      (shift > 0U && ((uintptr_t)generation >> shift) != 0U))) {
        return 0U;
    }
    return generation != 0U ? (slot_word << shift) | (uintptr_t)generation : 0U;
}

static inline bool llam_public_slot_decode_handle(uintptr_t raw,
                                                  unsigned shift,
                                                  size_t *out_slot,
                                                  uint32_t *out_generation) {
    uintptr_t slot_word;
    uintptr_t generation_bits;
    uint32_t generation;

    if (LLAM_UNLIKELY(out_slot == NULL || out_generation == NULL)) {
        return false;
    }
    if (LLAM_UNLIKELY(shift == 0U || shift >= LLAM_PUBLIC_SLOT_WORD_BITS)) {
        return false;
    }
    slot_word = raw >> shift;
    generation_bits = shift < 32U ? raw & (((uintptr_t)1U << shift) - 1U) : raw & (uintptr_t)UINT32_MAX;
    if (LLAM_UNLIKELY(shift > 32U &&
                      (raw & ((((uintptr_t)1U << shift) - 1U) & ~(uintptr_t)UINT32_MAX)) != 0U)) {
        return false;
    }
    generation = (uint32_t)generation_bits;
    if (LLAM_UNLIKELY(slot_word == 0U || generation == 0U)) {
        return false;
    }
    *out_slot = (size_t)(slot_word - 1U);
    *out_generation = generation;
    return true;
}

static inline bool llam_public_slot_generation_exhausted(uint32_t generation) {
    return generation == LLAM_PUBLIC_SLOT_MAX_GENERATION;
}

static inline bool llam_public_slot_family_valid(uint32_t family) {
    return family != 0U && family <= LLAM_PUBLIC_HANDLE_FAMILY_MASK;
}

/*
 * Family-tagged generations reserve the low bits for the object family. This
 * keeps independent task/channel/mutex/etc. slot tables from accepting each
 * other's encoded handle values when a C FFI caller passes the wrong opaque
 * type. The remaining high bits store a sealed verifier token, not the raw
 * epoch; stale handles keep ABA protection without exposing a predictable next
 * generation.
 */
static inline uint32_t llam_public_slot_family_generation(uint32_t epoch, uint32_t family) {
    return (uint32_t)(epoch << LLAM_PUBLIC_HANDLE_FAMILY_BITS) | family;
}

#include "runtime_public_slot_seal.h"

static inline int llam_public_slot_refresh_family_generation(llam_public_slot_table_t *table,
                                                             llam_public_slot_t *entry,
                                                             size_t slot,
                                                             void *object,
                                                             uint32_t family,
                                                             uint64_t owner_secret) {
    uint32_t previous_generation;

    if (LLAM_UNLIKELY(table == NULL ||
                      entry == NULL ||
                      object == NULL ||
                      !llam_public_slot_family_valid(family))) {
        errno = EINVAL;
        return -1;
    }
    previous_generation = entry->generation;
    llam_public_slot_prepare_affine_seal(table, entry, slot, object, owner_secret);
    if (LLAM_UNLIKELY(!llam_public_slot_affine_multiplier_valid(entry->seal_multiplier) ||
                      entry->seal_addend >= LLAM_PUBLIC_HANDLE_EPOCH_MASK)) {
        /*
         * A malformed seal can make the affine step repeat the previous public
         * token until epoch exhaustion.  Treat damaged slot metadata as a
         * lifecycle-corruption diagnostic instead of spending a long time in
         * the retry loop below.
         */
        errno = EINVAL;
        return -1;
    }
    while (entry->epoch < LLAM_PUBLIC_HANDLE_FAMILY_MAX_EPOCH) {
        entry->epoch += 1U;
        entry->generation = llam_public_slot_next_affine_generation(previous_generation,
                                                                    family,
                                                                    entry->seal_multiplier,
                                                                    entry->seal_addend);
        if (LLAM_LIKELY(entry->generation != previous_generation)) {
            return 0;
        }
    }
    /*
     * Family slots use a bijection over the public token domain, so a collision
     * before epoch exhaustion means the seal state itself is corrupt.
     */
    errno = EOVERFLOW;
    return -1;
}

static inline int llam_public_slot_reserve_impl(llam_public_slot_table_t *table,
                                                void *object,
                                                size_t initial_capacity,
                                                uint32_t family,
                                                uint64_t owner_secret,
                                                size_t *out_slot,
                                                uint32_t *out_generation) {
    size_t slot;

    if (LLAM_UNLIKELY(table == NULL ||
                      object == NULL ||
                      out_slot == NULL ||
                      out_generation == NULL ||
                      (family != 0U && !llam_public_slot_family_valid(family)))) {
        errno = EINVAL;
        return -1;
    }
    while (LLAM_LIKELY(table->free_head_plus_one != LLAM_PUBLIC_SLOT_FREE_NONE)) {
        llam_public_slot_t *entry;

        slot = table->free_head_plus_one - 1U;
        entry = &table->slots[slot];
        table->free_head_plus_one = entry->next_free_plus_one;
        entry->next_free_plus_one = LLAM_PUBLIC_SLOT_FREE_NONE;
        if (LLAM_UNLIKELY(family != 0U
                              ? entry->epoch >= LLAM_PUBLIC_HANDLE_FAMILY_MAX_EPOCH
                              : llam_public_slot_generation_exhausted(entry->generation))) {
            entry->object = NULL;
            continue;
        }
        if (family != 0U) {
            if (llam_public_slot_refresh_family_generation(table, entry, slot, object, family, owner_secret) != 0) {
                entry->object = NULL;
                continue;
            }
        } else {
            entry->generation = llam_public_slot_next_generation(entry->generation);
            entry->epoch = entry->generation;
            entry->seal_multiplier = 0U;
            entry->seal_addend = 0U;
        }
        entry->object = object;
        *out_slot = slot;
        *out_generation = entry->generation;
        return 0;
    }

    if (LLAM_UNLIKELY(table->count >= LLAM_PUBLIC_HANDLE_MAX_SLOTS)) {
        errno = ENOMEM;
        return -1;
    }
    if (LLAM_UNLIKELY(table->count == table->capacity)) {
        llam_public_slot_t *slots;
        size_t new_capacity = table->capacity != 0U ? table->capacity * 2U : initial_capacity;

        if (new_capacity == 0U) {
            new_capacity = 64U;
        }
        if (new_capacity > LLAM_PUBLIC_HANDLE_MAX_SLOTS) {
            new_capacity = LLAM_PUBLIC_HANDLE_MAX_SLOTS;
        }
        if (LLAM_UNLIKELY(new_capacity < table->count || new_capacity > SIZE_MAX / sizeof(*slots))) {
            errno = ENOMEM;
            return -1;
        }
        slots = realloc(table->slots, new_capacity * sizeof(*slots));
        if (LLAM_UNLIKELY(slots == NULL)) {
            errno = ENOMEM;
            return -1;
        }
        memset(slots + table->capacity, 0, (new_capacity - table->capacity) * sizeof(*slots));
        table->slots = slots;
        table->capacity = new_capacity;
    }

    slot = table->count++;
    table->slots[slot].object = object;
    table->slots[slot].epoch = family != 0U ? 0U : 1U;
    table->slots[slot].seal_multiplier = 0U;
    table->slots[slot].seal_addend = 0U;
    if (family != 0U) {
        if (llam_public_slot_refresh_family_generation(table,
                                                       &table->slots[slot],
                                                       slot,
                                                       object,
                                                       family,
                                                       owner_secret) != 0) {
            table->slots[slot].object = NULL;
            return -1;
        }
    } else {
        table->slots[slot].generation = 1U;
    }
    table->slots[slot].next_free_plus_one = LLAM_PUBLIC_SLOT_FREE_NONE;
    *out_slot = slot;
    *out_generation = table->slots[slot].generation;
    return 0;
}

static inline int llam_public_slot_reserve(llam_public_slot_table_t *table,
                                           void *object,
                                           size_t initial_capacity,
                                           size_t *out_slot,
                                           uint32_t *out_generation) {
    return llam_public_slot_reserve_impl(table, object, initial_capacity, 0U, 0U, out_slot, out_generation);
}

static inline int llam_public_slot_reserve_family_secret(llam_public_slot_table_t *table,
                                                         void *object,
                                                         size_t initial_capacity,
                                                         uint32_t family,
                                                         uint64_t owner_secret,
                                                         size_t *out_slot,
                                                         uint32_t *out_generation) {
    return llam_public_slot_reserve_impl(table, object, initial_capacity, family, owner_secret, out_slot, out_generation);
}

static inline int llam_public_slot_reserve_family(llam_public_slot_table_t *table,
                                                  void *object,
                                                  size_t initial_capacity,
                                                  uint32_t family,
                                                  size_t *out_slot,
                                                  uint32_t *out_generation) {
    return llam_public_slot_reserve_family_secret(table, object, initial_capacity, family, 0U, out_slot, out_generation);
}

static inline void *llam_public_slot_resolve(const llam_public_slot_table_t *table,
                                             size_t slot,
                                             uint32_t generation) {
    const llam_public_slot_t *entry;
    void *object;

    if (LLAM_UNLIKELY(table == NULL || generation == 0U || slot >= table->count)) {
        return NULL;
    }
    entry = &table->slots[slot];
    object = entry->object;
    return LLAM_LIKELY(entry->generation == generation) ? object : NULL;
}

static inline void *llam_public_slot_resolve_encoded(const llam_public_slot_table_t *table,
                                                     uintptr_t raw,
                                                     unsigned shift,
                                                     size_t *out_slot,
                                                     uint32_t *out_generation) {
    size_t slot;
    uint32_t generation;
    void *object;

    if (!llam_public_slot_decode_handle(raw, shift, &slot, &generation)) {
        return NULL;
    }
    object = llam_public_slot_resolve(table, slot, generation);
    if (object != NULL) {
        if (out_slot != NULL) {
            *out_slot = slot;
        }
        if (out_generation != NULL) {
            *out_generation = generation;
        }
    }
    return object;
}

static inline uint32_t llam_public_slot_generation(const llam_public_slot_table_t *table, size_t slot) {
    if (LLAM_UNLIKELY(table == NULL || slot >= table->count || table->slots[slot].object == NULL)) {
        return 0U;
    }
    return table->slots[slot].generation;
}

static inline int llam_public_slot_reactivate_impl(llam_public_slot_table_t *table,
                                                   size_t slot,
                                                   void *object,
                                                   uint32_t family,
                                                   uint64_t owner_secret,
                                                   uint32_t *out_generation) {
    llam_public_slot_t *entry;

    if (LLAM_UNLIKELY(table == NULL ||
                      object == NULL ||
                      out_generation == NULL ||
                      slot >= table->count ||
                      (family != 0U && !llam_public_slot_family_valid(family)))) {
        errno = EINVAL;
        return -1;
    }
    entry = &table->slots[slot];
    if (LLAM_UNLIKELY(entry->object != object)) {
        errno = EINVAL;
        return -1;
    }
    if (LLAM_UNLIKELY(family != 0U
                          ? entry->epoch >= LLAM_PUBLIC_HANDLE_FAMILY_MAX_EPOCH
                          : llam_public_slot_generation_exhausted(entry->generation))) {
        errno = EOVERFLOW;
        return -1;
    }
    if (family != 0U) {
        if (llam_public_slot_refresh_family_generation(table, entry, slot, object, family, owner_secret) != 0) {
            return -1;
        }
    } else {
        entry->generation = llam_public_slot_next_generation(entry->generation);
        entry->epoch = entry->generation;
        entry->seal_multiplier = 0U;
        entry->seal_addend = 0U;
    }
    *out_generation = entry->generation;
    return 0;
}

static inline int llam_public_slot_reactivate(llam_public_slot_table_t *table,
                                              size_t slot,
                                              void *object,
                                              uint32_t *out_generation) {
    return llam_public_slot_reactivate_impl(table, slot, object, 0U, 0U, out_generation);
}

static inline int llam_public_slot_reactivate_family_secret(llam_public_slot_table_t *table,
                                                            size_t slot,
                                                            void *object,
                                                            uint32_t family,
                                                            uint64_t owner_secret,
                                                            uint32_t *out_generation) {
    return llam_public_slot_reactivate_impl(table, slot, object, family, owner_secret, out_generation);
}

static inline int llam_public_slot_reactivate_family(llam_public_slot_table_t *table,
                                                     size_t slot,
                                                     void *object,
                                                     uint32_t family,
                                                     uint32_t *out_generation) {
    return llam_public_slot_reactivate_family_secret(table, slot, object, family, 0U, out_generation);
}

static inline void llam_public_slot_release(llam_public_slot_table_t *table,
                                            size_t slot,
                                            void *object,
                                            uint32_t generation) {
    llam_public_slot_t *entry;

    if (LLAM_UNLIKELY(table == NULL || slot >= table->count)) {
        return;
    }
    entry = &table->slots[slot];
    if (LLAM_UNLIKELY(entry->object != object || entry->generation != generation)) {
        return;
    }
    entry->object = NULL;
    entry->next_free_plus_one = table->free_head_plus_one;
    table->free_head_plus_one = slot + 1U;
}

static inline void llam_public_active_op_init(_Atomic size_t *active_ops) {
    atomic_init(active_ops, 0U);
}

static inline int llam_public_active_op_try_begin(_Atomic size_t *active_ops) {
    size_t current;
    size_t previous;

    if (active_ops == NULL) {
        return 0;
    }
    current = atomic_load_explicit(active_ops, memory_order_relaxed);
    if (LLAM_UNLIKELY(current >= LLAM_PUBLIC_ACTIVE_OP_BEGIN_LIMIT)) {
        /*
         * The final low-half value and the high half are reserved sentinel
         * space. A begin from the last valid count would increment into that
         * range, and a later end could decrement it back into an apparently valid
         * count. Publish the permanent busy sentinel before doing arithmetic.
         */
        atomic_store_explicit(active_ops, LLAM_PUBLIC_ACTIVE_OP_BUSY_SENTINEL, memory_order_relaxed);
        errno = EBUSY;
        return -1;
    }
    previous = atomic_fetch_add_explicit(active_ops, 1U, memory_order_relaxed);
    if (LLAM_LIKELY(previous < LLAM_PUBLIC_ACTIVE_OP_BEGIN_LIMIT)) {
        return 0;
    }
    atomic_store_explicit(active_ops, LLAM_PUBLIC_ACTIVE_OP_BUSY_SENTINEL, memory_order_relaxed);
    errno = EBUSY;
    return -1;
}

static inline void llam_public_active_op_begin(_Atomic size_t *active_ops) {
    (void)llam_public_active_op_try_begin(active_ops);
}

static inline void llam_public_active_op_end(_Atomic size_t *active_ops) {
    size_t current;
    size_t previous;

    if (active_ops == NULL) {
        return;
    }
    current = atomic_load_explicit(active_ops, memory_order_relaxed);
    if (current == 0U) {
        return;
    }
    if (LLAM_UNLIKELY(current >= LLAM_PUBLIC_ACTIVE_OP_RESERVED_THRESHOLD)) {
        /*
         * Saturated/corrupt counters are permanent EBUSY sentinels. Do not use
         * fetch_sub on that state: SIZE_MAX - 1 would look like a huge but
         * different active count instead of the canonical sentinel.
         */
        atomic_store_explicit(active_ops, LLAM_PUBLIC_ACTIVE_OP_BUSY_SENTINEL, memory_order_relaxed);
        return;
    }
    previous = atomic_fetch_sub_explicit(active_ops, 1U, memory_order_relaxed);
    if (LLAM_LIKELY(previous > 0U && previous < LLAM_PUBLIC_ACTIVE_OP_RESERVED_THRESHOLD)) {
        return;
    }
    if (previous == 0U) {
        (void)atomic_fetch_add_explicit(active_ops, 1U, memory_order_relaxed);
        return;
    }
    atomic_store_explicit(active_ops, LLAM_PUBLIC_ACTIVE_OP_BUSY_SENTINEL, memory_order_relaxed);
}

static inline size_t llam_public_active_op_count(const _Atomic size_t *active_ops) {
    return active_ops != NULL ? atomic_load_explicit(active_ops, memory_order_relaxed) : 0U;
}

static inline bool llam_public_active_op_is_saturated(size_t active_ops) {
    return active_ops >= LLAM_PUBLIC_ACTIVE_OP_RESERVED_THRESHOLD;
}

#endif
