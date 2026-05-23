/**
 * @file src/internal/runtime_public_slot.h
 * @brief Slot+generation public handle table helpers.
 *
 * @details
 * Runtime objects that can be destroyed or cached behind opaque public handles
 * use these helpers to reject stale, consumed, or forged handles before any
 * object storage is dereferenced.
 *
 * @copyright Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LLAM_RUNTIME_PUBLIC_SLOT_H
#define LLAM_RUNTIME_PUBLIC_SLOT_H

#define LLAM_PUBLIC_HANDLE_MAX_SLOTS ((size_t)UINT32_MAX)
#define LLAM_PUBLIC_SLOT_FREE_NONE ((size_t)0U)
#define LLAM_PUBLIC_SLOT_MAX_GENERATION UINT32_MAX

typedef struct llam_public_slot {
    void *object;
    uint32_t generation;
    size_t next_free_plus_one;
} llam_public_slot_t;

_Static_assert(sizeof(llam_public_slot_t) <= 24U, "public handle slots must stay cache-compact");

typedef struct llam_public_slot_table {
    llam_public_slot_t *slots;
    size_t count;
    size_t capacity;
    size_t free_head_plus_one;
} llam_public_slot_table_t;

static inline uint32_t llam_public_slot_next_generation(uint32_t generation) {
    generation += 1U;
    generation += (uint32_t)(generation == 0U);
    return generation;
}

static inline uintptr_t llam_public_slot_encode_handle(size_t slot, uint32_t generation, unsigned shift) {
    uintptr_t slot_word = (uintptr_t)slot + 1U;

    return generation != 0U ? (slot_word << shift) | (uintptr_t)generation : 0U;
}

static inline bool llam_public_slot_decode_handle(uintptr_t raw,
                                                  unsigned shift,
                                                  size_t *out_slot,
                                                  uint32_t *out_generation) {
    uintptr_t slot_word;
    uint32_t generation;

    if (LLAM_UNLIKELY(out_slot == NULL || out_generation == NULL)) {
        return false;
    }
    slot_word = raw >> shift;
    generation = (uint32_t)raw;
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

static inline int llam_public_slot_reserve(llam_public_slot_table_t *table,
                                           void *object,
                                           size_t initial_capacity,
                                           size_t *out_slot,
                                           uint32_t *out_generation) {
    size_t slot;

    if (LLAM_UNLIKELY(table == NULL || object == NULL || out_slot == NULL || out_generation == NULL)) {
        errno = EINVAL;
        return -1;
    }
    while (LLAM_LIKELY(table->free_head_plus_one != LLAM_PUBLIC_SLOT_FREE_NONE)) {
        llam_public_slot_t *entry;

        slot = table->free_head_plus_one - 1U;
        entry = &table->slots[slot];
        table->free_head_plus_one = entry->next_free_plus_one;
        entry->next_free_plus_one = LLAM_PUBLIC_SLOT_FREE_NONE;
        if (LLAM_UNLIKELY(llam_public_slot_generation_exhausted(entry->generation))) {
            entry->object = NULL;
            continue;
        }
        entry->generation = llam_public_slot_next_generation(entry->generation);
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
    table->slots[slot].generation = 1U;
    table->slots[slot].next_free_plus_one = LLAM_PUBLIC_SLOT_FREE_NONE;
    *out_slot = slot;
    *out_generation = 1U;
    return 0;
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

static inline int llam_public_slot_reactivate(llam_public_slot_table_t *table,
                                              size_t slot,
                                              void *object,
                                              uint32_t *out_generation) {
    llam_public_slot_t *entry;

    if (LLAM_UNLIKELY(table == NULL || object == NULL || out_generation == NULL || slot >= table->count)) {
        errno = EINVAL;
        return -1;
    }
    entry = &table->slots[slot];
    if (LLAM_UNLIKELY(entry->object != object)) {
        errno = EINVAL;
        return -1;
    }
    if (LLAM_UNLIKELY(llam_public_slot_generation_exhausted(entry->generation))) {
        errno = EOVERFLOW;
        return -1;
    }
    entry->generation = llam_public_slot_next_generation(entry->generation);
    *out_generation = entry->generation;
    return 0;
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

static inline void llam_public_active_op_begin(_Atomic size_t *active_ops) {
    (void)atomic_fetch_add_explicit(active_ops, 1U, memory_order_relaxed);
}

static inline void llam_public_active_op_end(_Atomic size_t *active_ops) {
    size_t previous;

    if (active_ops == NULL) {
        return;
    }
    previous = atomic_fetch_sub_explicit(active_ops, 1U, memory_order_relaxed);
    if (LLAM_UNLIKELY(previous == 0U)) {
        (void)atomic_fetch_add_explicit(active_ops, 1U, memory_order_relaxed);
    }
}

static inline size_t llam_public_active_op_count(const _Atomic size_t *active_ops) {
    return active_ops != NULL ? atomic_load_explicit(active_ops, memory_order_relaxed) : 0U;
}

#endif
