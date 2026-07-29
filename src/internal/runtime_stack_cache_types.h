// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Feralthedogg

/**
 * @file src/internal/runtime_stack_cache_types.h
 * @brief Private retained-stack metadata shared by cache modules.
 */

#ifndef LLAM_RUNTIME_STACK_CACHE_TYPES_H
#define LLAM_RUNTIME_STACK_CACHE_TYPES_H

/** @brief VM state of a retained stack-cache mapping. */
typedef enum llam_stack_cache_entry_state {
    LLAM_STACK_CACHE_ENTRY_READY = 0,
    LLAM_STACK_CACHE_ENTRY_DISCARDED = 1,
} llam_stack_cache_entry_state_t;

/** @brief Metadata for a cached stack mapping and its usable stack range. */
struct llam_stack_cache_entry {
    llam_runtime_t *owner_runtime;
    void *mapping;
    size_t mapping_size;
    void *stack_base;
    size_t stack_size;
    uint64_t committed_bytes;
    uint64_t last_return_ns;
    uint32_t stack_class;
    uint32_t state;
    llam_stack_cache_entry_t *next;
    bool heap_allocated;
};

#endif
