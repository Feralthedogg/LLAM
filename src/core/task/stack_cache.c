/**
 * @file src/core/task/stack_cache.c
 * @brief Runtime-wide stack-cache byte and VM-state authority.
 *
 * @details
 * Cache return reserves a runtime-wide byte budget before list publication.
 * Security transitions and platform release happen without a cache-list lock.
 * List mechanics are isolated in stack_cache_lists.c.
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

#include "runtime_internal.h"

/** Resolve a public stack class from its exact usable size. */
static bool llam_stack_cache_class_for_size(size_t stack_size,
                                            uint32_t *stack_class_out) {
    if (stack_class_out == NULL) {
        return false;
    }
    if (stack_size == llam_stack_bytes(LLAM_STACK_CLASS_DEFAULT)) {
        *stack_class_out = (uint32_t)LLAM_STACK_CLASS_DEFAULT;
        return true;
    }
    if (stack_size == llam_stack_bytes(LLAM_STACK_CLASS_LARGE)) {
        *stack_class_out = (uint32_t)LLAM_STACK_CLASS_LARGE;
        return true;
    }
    if (stack_size == llam_stack_bytes(LLAM_STACK_CLASS_HUGE)) {
        *stack_class_out = (uint32_t)LLAM_STACK_CLASS_HUGE;
        return true;
    }
    return false;
}

/** Subtract an exact cache counter without unsigned wraparound. */
static bool llam_stack_cache_counter_sub(atomic_uint_fast64_t *counter,
                                         uint64_t value) {
    uint_fast64_t current;

    if (counter == NULL) {
        return false;
    }
    current = atomic_load_explicit(counter, memory_order_acquire);
    for (;;) {
        if (current < value) {
            return false;
        }
        if (atomic_compare_exchange_weak_explicit(counter,
                                                  &current,
                                                  current - value,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            return true;
        }
    }
}

/** Reserve runtime-wide mapping-byte authority before publication. */
static bool llam_stack_cache_account_reserve(llam_runtime_t *rt,
                                             size_t mapping_size,
                                             uint64_t committed_bytes) {
    uint_fast64_t current;
    uint64_t budget;
    uint64_t bytes;

    if (rt == NULL || mapping_size == 0U) {
        return false;
    }
    bytes = (uint64_t)mapping_size;
    budget = rt->resource_plan.stack_cache_budget_bytes;
    current = atomic_load_explicit(&rt->stack_cache_cached_bytes,
                                   memory_order_acquire);
    for (;;) {
        if (current > budget || bytes > budget - current) {
            atomic_fetch_add_explicit(&rt->stack_cache_budget_rejections,
                                      1U,
                                      memory_order_relaxed);
            return false;
        }
        if (atomic_compare_exchange_weak_explicit(
                &rt->stack_cache_cached_bytes,
                &current,
                current + bytes,
                memory_order_acq_rel,
                memory_order_acquire)) {
            break;
        }
    }
    atomic_fetch_add_explicit(&rt->stack_cache_cached_mappings,
                              1U,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&rt->stack_cache_committed_bytes,
                              committed_bytes,
                              memory_order_relaxed);
    return true;
}

void llam_stack_cache_account_remove(llam_runtime_t *rt,
                                     size_t mapping_size,
                                     uint64_t committed_bytes) {
    bool valid;

    if (rt == NULL || mapping_size == 0U) {
        return;
    }
    valid = llam_stack_cache_counter_sub(&rt->stack_cache_committed_bytes,
                                         committed_bytes);
    valid = llam_stack_cache_counter_sub(&rt->stack_cache_cached_mappings,
                                         1U) &&
            valid;
    valid = llam_stack_cache_counter_sub(&rt->stack_cache_cached_bytes,
                                         (uint64_t)mapping_size) &&
            valid;
    if (!valid) {
        llam_record_fatal_deferred(rt, EOVERFLOW);
    }
}

void llam_stack_mapping_release(llam_runtime_t *rt,
                                void *mapping,
                                size_t mapping_size) {
    if (mapping == NULL || mapping_size == 0U) {
        return;
    }
    if (llam_stack_vm_release(mapping, mapping_size) == 0 && rt != NULL) {
        atomic_fetch_add_explicit(&rt->stack_cache_released_bytes,
                                  (uint64_t)mapping_size,
                                  memory_order_relaxed);
    }
}

/** Establish the configured security and commit state before publication. */
static bool llam_stack_cache_prepare_return(
    llam_runtime_t *rt,
    void *stack_base,
    size_t stack_size,
    uint32_t *state_out,
    uint64_t *committed_bytes_out) {
    uint32_t flags;

    if (rt == NULL || stack_base == NULL || stack_size == 0U ||
        state_out == NULL || committed_bytes_out == NULL) {
        return false;
    }
    flags = rt->resource_plan.stack_cache_flags;
    *state_out = LLAM_STACK_CACHE_ENTRY_READY;
    *committed_bytes_out = (uint64_t)stack_size;

    if ((flags & LLAM_RUNTIME_STACK_CACHE_F_SECURE_SCRUB) != 0U &&
        llam_stack_vm_secure_zero(stack_base, stack_size) != 0) {
        atomic_fetch_add_explicit(
            &rt->stack_cache_secure_return_failures,
            1U,
            memory_order_relaxed);
        return false;
    }
    if ((flags & LLAM_RUNTIME_STACK_CACHE_F_DISCARD_ON_RETURN) != 0U) {
        if (llam_stack_vm_discard(stack_base, stack_size) != 0) {
            atomic_fetch_add_explicit(
                &rt->stack_cache_secure_return_failures,
                1U,
                memory_order_relaxed);
            return false;
        }
        *state_out = LLAM_STACK_CACHE_ENTRY_DISCARDED;
        *committed_bytes_out = 0U;
        atomic_fetch_add_explicit(&rt->stack_cache_discarded_bytes,
                                  (uint64_t)stack_size,
                                  memory_order_relaxed);
    }
    return true;
}

bool llam_stack_cache_return_mapping(llam_runtime_t *rt,
                                     llam_shard_t *preferred_shard,
                                     void *mapping,
                                     size_t mapping_size,
                                     void *stack_base,
                                     size_t stack_size) {
    uint64_t committed_bytes;
    uint64_t last_return_ns;
    uint32_t stack_class;
    uint32_t state;
    long raw_page_size = llam_page_size();
    size_t page_size;
    bool published;

    if (mapping == NULL || mapping_size == 0U) {
        return false;
    }
    if (rt == NULL || raw_page_size <= 0 ||
        stack_base == NULL || stack_size == 0U ||
        !llam_stack_cache_class_for_size(stack_size, &stack_class)) {
        llam_stack_mapping_release(rt, mapping, mapping_size);
        return false;
    }
    page_size = (size_t)raw_page_size;
    if (stack_size > SIZE_MAX - page_size ||
        mapping_size != stack_size + page_size ||
        stack_base != (unsigned char *)mapping + page_size) {
        llam_record_fatal_deferred(rt, EINVAL);
        llam_stack_mapping_release(rt, mapping, mapping_size);
        return false;
    }
    if ((rt->resource_plan.stack_cache_flags &
         LLAM_RUNTIME_STACK_CACHE_F_DISABLED) != 0U ||
        rt->resource_plan.stack_cache_budget_bytes == 0U) {
        llam_stack_mapping_release(rt, mapping, mapping_size);
        return false;
    }
    if (!llam_stack_cache_prepare_return(rt,
                                         stack_base,
                                         stack_size,
                                         &state,
                                         &committed_bytes)) {
        llam_stack_mapping_release(rt, mapping, mapping_size);
        return false;
    }
    if (!llam_stack_cache_account_reserve(rt,
                                          mapping_size,
                                          committed_bytes)) {
        llam_stack_mapping_release(rt, mapping, mapping_size);
        return false;
    }

    last_return_ns = llam_now_ns();
    if (last_return_ns == 0U) {
        last_return_ns = 1U;
    }
    published = llam_stack_cache_publish_mapping(rt,
                                                 preferred_shard,
                                                 mapping,
                                                 mapping_size,
                                                 stack_base,
                                                 stack_size,
                                                 committed_bytes,
                                                 last_return_ns,
                                                 stack_class,
                                                 state);
    if (!published) {
        llam_stack_cache_account_remove(rt,
                                        mapping_size,
                                        committed_bytes);
        llam_stack_mapping_release(rt, mapping, mapping_size);
    }
    return published;
}

/** Drain and release cached stack mappings or metadata entries. */
static void llam_stack_cache_drain_list(llam_runtime_t *rt,
                                        llam_stack_cache_entry_t *entry) {
    while (entry != NULL) {
        llam_stack_cache_entry_t *next = entry->next;
        bool heap_allocated = entry->heap_allocated;

        if (entry->mapping != NULL && entry->mapping_size != 0U) {
            if (entry->owner_runtime != rt) {
                llam_record_fatal_deferred(rt, EINVAL);
            }
            llam_stack_cache_account_remove(rt,
                                            entry->mapping_size,
                                            entry->committed_bytes);
            llam_stack_mapping_release(rt,
                                       entry->mapping,
                                       entry->mapping_size);
        }
        if (heap_allocated) {
            free(entry);
        } else {
            memset(entry, 0, sizeof(*entry));
        }
        entry = next;
    }
}

void llam_shard_drain_stack_cache(llam_shard_t *shard) {
    llam_stack_cache_entry_t *lists[4];

    if (shard == NULL) {
        return;
    }
    lists[0] = shard->stack_cache_default;
    lists[1] = shard->stack_cache_large;
    lists[2] = shard->stack_cache_huge;
    lists[3] = shard->stack_cache_entry_free;
    shard->stack_cache_default = NULL;
    shard->stack_cache_large = NULL;
    shard->stack_cache_huge = NULL;
    shard->stack_cache_entry_free = NULL;
    shard->stack_cache_default_count = 0U;
    shard->stack_cache_large_count = 0U;
    shard->stack_cache_huge_count = 0U;
    for (unsigned i = 0U;
         i < (unsigned)(sizeof(lists) / sizeof(lists[0]));
         ++i) {
        llam_stack_cache_drain_list(shard->runtime, lists[i]);
    }
}

void llam_runtime_drain_stack_cache(llam_runtime_t *rt) {
    llam_stack_cache_entry_t *lists[4];

    if (rt == NULL) {
        return;
    }
    lists[0] = rt->stack_cache_default;
    lists[1] = rt->stack_cache_large;
    lists[2] = rt->stack_cache_huge;
    lists[3] = rt->stack_cache_entry_free;
    rt->stack_cache_default = NULL;
    rt->stack_cache_large = NULL;
    rt->stack_cache_huge = NULL;
    rt->stack_cache_entry_free = NULL;
    rt->stack_cache_default_count = 0U;
    rt->stack_cache_large_count = 0U;
    rt->stack_cache_huge_count = 0U;
    for (unsigned i = 0U;
         i < (unsigned)(sizeof(lists) / sizeof(lists[0]));
         ++i) {
        llam_stack_cache_drain_list(rt, lists[i]);
    }
}

int llam_runtime_prewarm_stack_cache(llam_runtime_t *rt,
                                     uint64_t total,
                                     bool exact,
                                     uint64_t *achieved) {
    uint64_t completed = 0U;
    size_t stack_size;
    int saved_errno = errno;

    if (achieved != NULL) {
        *achieved = 0U;
    }
    if (rt == NULL || !rt->stack_cache_lock_initialized ||
        rt->shards == NULL || rt->active_shards == 0U || achieved == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (total > LLAM_RUNTIME_MAX_STACK_PREWARM) {
        errno = E2BIG;
        return -1;
    }

    stack_size = llam_stack_bytes(LLAM_STACK_CLASS_DEFAULT);
    for (unsigned shard_id = 0U; shard_id < rt->active_shards; ++shard_id) {
        uint64_t share =
            llam_runtime_prewarm_share(total, rt->active_shards, shard_id);

        for (uint64_t i = 0U; i < share; ++i) {
            void *mapping;
            void *stack_base;
            size_t mapping_size;
            bool cached;

#if defined(LLAM_ENABLE_TEST_HOOKS)
            if (!llam_runtime_test_prewarm_allocation_permitted(
                    LLAM_TEST_PREWARM_STACK,
                    1U)) {
                errno = ENOMEM;
                goto exhausted;
            }
#endif
            if (llam_stack_vm_map(stack_size,
                                  &mapping,
                                  &mapping_size,
                                  &stack_base) != 0) {
                goto exhausted;
            }
            cached = llam_stack_cache_return_mapping(
                rt,
                &rt->shards[shard_id],
                mapping,
                mapping_size,
                stack_base,
                stack_size);
            if (!cached) {
                errno = ENOMEM;
                goto exhausted;
            }
            completed += 1U;
        }
    }

    *achieved = completed;
    errno = saved_errno;
    return 0;

exhausted:
    *achieved = completed;
    if (exact) {
        return -1;
    }
    errno = saved_errno;
    return 0;
}
