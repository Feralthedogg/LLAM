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

/** Maximum process-quarantine mappings retried by one lifecycle opportunity. */
#define LLAM_STACK_CACHE_QUARANTINE_RETRY_BATCH 32U

/*
 * A runtime handle cannot remain alive solely to own a stack mapping whose
 * platform release failed during shutdown. Transfer its existing heap metadata
 * to this process-owned quarantine instead. The lock only protects list and
 * counter publication; platform VM calls always happen after it is released.
 */
static atomic_flag g_llam_stack_cache_quarantine_lock = ATOMIC_FLAG_INIT;
static llam_stack_cache_entry_t *g_llam_stack_cache_quarantine;
static llam_stack_cache_entry_t *g_llam_stack_cache_quarantine_tail;
static uint64_t g_llam_stack_cache_quarantine_bytes;
static uint64_t g_llam_stack_cache_quarantine_mappings;

static void llam_stack_cache_quarantine_lock(void) {
    while (atomic_flag_test_and_set_explicit(
               &g_llam_stack_cache_quarantine_lock,
               memory_order_acquire)) {
        llam_pause_cpu();
    }
}

static void llam_stack_cache_quarantine_unlock(void) {
    atomic_flag_clear_explicit(&g_llam_stack_cache_quarantine_lock,
                               memory_order_release);
}

/** Transfer one heap-owned mapping entry to process-lifetime authority. */
static bool llam_stack_cache_quarantine_entry(
    llam_stack_cache_entry_t *entry) {
    if (entry == NULL || !entry->heap_allocated ||
        entry->mapping == NULL || entry->mapping_size == 0U) {
        return false;
    }

    entry->owner_runtime = NULL;
    llam_stack_cache_quarantine_lock();
    if (g_llam_stack_cache_quarantine_tail == NULL) {
        g_llam_stack_cache_quarantine = entry;
    } else {
        g_llam_stack_cache_quarantine_tail->next = entry;
    }
    entry->next = NULL;
    g_llam_stack_cache_quarantine_tail = entry;
    if (g_llam_stack_cache_quarantine_mappings != UINT64_MAX) {
        g_llam_stack_cache_quarantine_mappings += 1U;
    }
    if ((uint64_t)entry->mapping_size >
        UINT64_MAX - g_llam_stack_cache_quarantine_bytes) {
        g_llam_stack_cache_quarantine_bytes = UINT64_MAX;
    } else {
        g_llam_stack_cache_quarantine_bytes +=
            (uint64_t)entry->mapping_size;
    }
    llam_stack_cache_quarantine_unlock();
    return true;
}

void llam_stack_cache_quarantine_snapshot(uint64_t *mapping_bytes,
                                          uint64_t *mapping_count) {
    if (mapping_bytes != NULL) {
        *mapping_bytes = 0U;
    }
    if (mapping_count != NULL) {
        *mapping_count = 0U;
    }
    if (mapping_bytes == NULL || mapping_count == NULL) {
        return;
    }

    llam_stack_cache_quarantine_lock();
    *mapping_bytes = g_llam_stack_cache_quarantine_bytes;
    *mapping_count = g_llam_stack_cache_quarantine_mappings;
    llam_stack_cache_quarantine_unlock();
}

void llam_stack_cache_quarantine_retry(void) {
    llam_stack_cache_entry_t
        *entries[LLAM_STACK_CACHE_QUARANTINE_RETRY_BATCH];
    llam_stack_cache_entry_t *failed_head = NULL;
    llam_stack_cache_entry_t *failed_tail = NULL;
    uint64_t failed_bytes = 0U;
    uint64_t failed_mappings = 0U;
    unsigned count = 0U;
    int saved_errno = errno;

    llam_stack_cache_quarantine_lock();
    while (g_llam_stack_cache_quarantine != NULL &&
           count < LLAM_STACK_CACHE_QUARANTINE_RETRY_BATCH) {
        llam_stack_cache_entry_t *entry =
            g_llam_stack_cache_quarantine;

        g_llam_stack_cache_quarantine = entry->next;
        entry->next = NULL;
        if (g_llam_stack_cache_quarantine_mappings != 0U) {
            g_llam_stack_cache_quarantine_mappings -= 1U;
        }
        if (g_llam_stack_cache_quarantine_bytes >=
            (uint64_t)entry->mapping_size) {
            g_llam_stack_cache_quarantine_bytes -=
                (uint64_t)entry->mapping_size;
        } else {
            g_llam_stack_cache_quarantine_bytes = 0U;
        }
        entries[count++] = entry;
    }
    if (g_llam_stack_cache_quarantine == NULL) {
        g_llam_stack_cache_quarantine_tail = NULL;
    }
    llam_stack_cache_quarantine_unlock();

    for (unsigned index = 0U; index < count; ++index) {
        llam_stack_cache_entry_t *entry = entries[index];

        if (llam_stack_mapping_release(NULL,
                                       entry->mapping,
                                       entry->mapping_size)) {
            free(entry);
        } else {
            entry->next = NULL;
            if (failed_tail == NULL) {
                failed_head = entry;
            } else {
                failed_tail->next = entry;
            }
            failed_tail = entry;
            if (failed_mappings != UINT64_MAX) {
                failed_mappings += 1U;
            }
            if ((uint64_t)entry->mapping_size >
                UINT64_MAX - failed_bytes) {
                failed_bytes = UINT64_MAX;
            } else {
                failed_bytes += (uint64_t)entry->mapping_size;
            }
        }
    }
    if (failed_head != NULL) {
        llam_stack_cache_quarantine_lock();
        if (g_llam_stack_cache_quarantine_tail == NULL) {
            g_llam_stack_cache_quarantine = failed_head;
        } else {
            g_llam_stack_cache_quarantine_tail->next = failed_head;
        }
        g_llam_stack_cache_quarantine_tail = failed_tail;
        if (failed_mappings >
            UINT64_MAX - g_llam_stack_cache_quarantine_mappings) {
            g_llam_stack_cache_quarantine_mappings = UINT64_MAX;
        } else {
            g_llam_stack_cache_quarantine_mappings += failed_mappings;
        }
        if (failed_bytes >
            UINT64_MAX - g_llam_stack_cache_quarantine_bytes) {
            g_llam_stack_cache_quarantine_bytes = UINT64_MAX;
        } else {
            g_llam_stack_cache_quarantine_bytes += failed_bytes;
        }
        llam_stack_cache_quarantine_unlock();
    }
    errno = saved_errno;
}

#if defined(LLAM_ENABLE_TEST_HOOKS)
static llam_test_stack_cache_account_observer_fn
    g_llam_stack_cache_account_observer;
static void *g_llam_stack_cache_account_observer_context;

void llam_runtime_test_reset_stack_cache_account_hook(void) {
    g_llam_stack_cache_account_observer = NULL;
    g_llam_stack_cache_account_observer_context = NULL;
}

void llam_runtime_test_set_stack_cache_account_observer(
    llam_test_stack_cache_account_observer_fn observer,
    void *context) {
    g_llam_stack_cache_account_observer_context = context;
    g_llam_stack_cache_account_observer = observer;
}
#endif

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

/** Serialize the short authoritative tuple update without taking a list lock. */
static void llam_stack_cache_account_write_begin(llam_runtime_t *rt) {
    unsigned expected;

    for (;;) {
        expected = 0U;
        if (atomic_compare_exchange_weak_explicit(
                &rt->stack_cache_account_writer,
                &expected,
                1U,
                memory_order_acquire,
                memory_order_relaxed)) {
            break;
        }
        llam_pause_cpu();
    }
    atomic_fetch_add_explicit(&rt->stack_cache_account_seq,
                              1U,
                              memory_order_acq_rel);
}

/** Publish one complete authoritative tuple update. */
static void llam_stack_cache_account_write_end(llam_runtime_t *rt) {
    atomic_fetch_add_explicit(&rt->stack_cache_account_seq,
                              1U,
                              memory_order_release);
    atomic_store_explicit(&rt->stack_cache_account_writer,
                          0U,
                          memory_order_release);
}

void llam_stack_cache_account_snapshot(const llam_runtime_t *rt,
                                       uint64_t *cached_bytes,
                                       uint64_t *cached_mappings,
                                       uint64_t *committed_bytes) {
    uint_fast64_t start_seq;
    uint_fast64_t end_seq;

    if (cached_bytes != NULL) {
        *cached_bytes = 0U;
    }
    if (cached_mappings != NULL) {
        *cached_mappings = 0U;
    }
    if (committed_bytes != NULL) {
        *committed_bytes = 0U;
    }
    if (rt == NULL || cached_bytes == NULL || cached_mappings == NULL ||
        committed_bytes == NULL) {
        return;
    }
    for (;;) {
        start_seq =
            atomic_load_explicit(&rt->stack_cache_account_seq,
                                 memory_order_acquire);
        if ((start_seq & 1U) != 0U) {
            llam_pause_cpu();
            continue;
        }
        *cached_bytes =
            atomic_load_explicit(&rt->stack_cache_cached_bytes,
                                 memory_order_relaxed);
        *cached_mappings =
            atomic_load_explicit(&rt->stack_cache_cached_mappings,
                                 memory_order_relaxed);
        *committed_bytes =
            atomic_load_explicit(&rt->stack_cache_committed_bytes,
                                 memory_order_relaxed);
        end_seq =
            atomic_load_explicit(&rt->stack_cache_account_seq,
                                 memory_order_acquire);
        if (start_seq == end_seq) {
            return;
        }
    }
}

/** Reserve runtime-wide mapping-byte authority before publication. */
static bool llam_stack_cache_account_reserve(llam_runtime_t *rt,
                                             size_t mapping_size,
                                             uint64_t committed_bytes) {
    uint64_t current;
    uint64_t current_committed;
    uint64_t current_mappings;
    uint64_t budget;
    uint64_t bytes;
    bool valid = true;

    if (rt == NULL || mapping_size == 0U) {
        return false;
    }
    bytes = (uint64_t)mapping_size;
    budget = rt->resource_plan.stack_cache_budget_bytes;
    llam_stack_cache_account_write_begin(rt);
    current =
        atomic_load_explicit(&rt->stack_cache_cached_bytes,
                             memory_order_relaxed);
    current_mappings =
        atomic_load_explicit(&rt->stack_cache_cached_mappings,
                             memory_order_relaxed);
    current_committed =
        atomic_load_explicit(&rt->stack_cache_committed_bytes,
                             memory_order_relaxed);
    if (current > budget || bytes > budget - current) {
        valid = false;
    } else if (current_mappings == UINT64_MAX ||
               committed_bytes > UINT64_MAX - current_committed) {
        valid = false;
        llam_record_fatal_deferred(rt, EOVERFLOW);
    } else {
        atomic_store_explicit(&rt->stack_cache_cached_bytes,
                              current + bytes,
                              memory_order_relaxed);
#if defined(LLAM_ENABLE_TEST_HOOKS)
        if (g_llam_stack_cache_account_observer != NULL) {
            g_llam_stack_cache_account_observer(
                g_llam_stack_cache_account_observer_context);
        }
#endif
        atomic_store_explicit(&rt->stack_cache_cached_mappings,
                              current_mappings + 1U,
                              memory_order_relaxed);
        atomic_store_explicit(&rt->stack_cache_committed_bytes,
                              current_committed + committed_bytes,
                              memory_order_relaxed);
    }
    llam_stack_cache_account_write_end(rt);
    if (!valid) {
        atomic_fetch_add_explicit(&rt->stack_cache_budget_rejections,
                                  1U,
                                  memory_order_relaxed);
    }
    return valid;
}

void llam_stack_cache_account_remove(llam_runtime_t *rt,
                                     size_t mapping_size,
                                     uint64_t committed_bytes) {
    uint64_t current_bytes;
    uint64_t current_committed;
    uint64_t current_mappings;
    bool valid = true;

    if (rt == NULL || mapping_size == 0U) {
        return;
    }
    llam_stack_cache_account_write_begin(rt);
    current_bytes =
        atomic_load_explicit(&rt->stack_cache_cached_bytes,
                             memory_order_relaxed);
    current_mappings =
        atomic_load_explicit(&rt->stack_cache_cached_mappings,
                             memory_order_relaxed);
    current_committed =
        atomic_load_explicit(&rt->stack_cache_committed_bytes,
                             memory_order_relaxed);
    if (current_bytes < (uint64_t)mapping_size ||
        current_mappings == 0U ||
        current_committed < committed_bytes) {
        valid = false;
    } else {
        atomic_store_explicit(&rt->stack_cache_cached_bytes,
                              current_bytes - (uint64_t)mapping_size,
                              memory_order_relaxed);
        atomic_store_explicit(&rt->stack_cache_cached_mappings,
                              current_mappings - 1U,
                              memory_order_relaxed);
        atomic_store_explicit(&rt->stack_cache_committed_bytes,
                              current_committed - committed_bytes,
                              memory_order_relaxed);
    }
    llam_stack_cache_account_write_end(rt);
    if (!valid) {
        llam_record_fatal_deferred(rt, EOVERFLOW);
    }
}

bool llam_stack_mapping_release(llam_runtime_t *rt,
                                void *mapping,
                                size_t mapping_size) {
    if (mapping == NULL || mapping_size == 0U) {
        return false;
    }
    if (llam_stack_vm_release(mapping, mapping_size) == 0) {
        if (rt != NULL) {
            atomic_fetch_add_explicit(&rt->stack_cache_released_bytes,
                                      (uint64_t)mapping_size,
                                      memory_order_relaxed);
        }
        return true;
    }
    return false;
}

void llam_stack_mapping_release_or_quarantine(llam_runtime_t *rt,
                                              void *mapping,
                                              size_t mapping_size) {
    llam_stack_cache_entry_t *entry;
    int release_errno;

    if (mapping == NULL || mapping_size == 0U) {
        llam_record_fatal_deferred(rt, EINVAL);
        return;
    }
    if (llam_stack_mapping_release(rt, mapping, mapping_size)) {
        return;
    }
    release_errno = errno != 0 ? errno : EIO;
    entry = calloc(1U, sizeof(*entry));
    if (entry == NULL) {
        /*
         * There is no safe continuation without either the VM release or
         * metadata that retains the last mapping pointer. Fail closed and let
         * process teardown reclaim the address space.
         */
        llam_record_fatal_deferred(rt, ENOMEM);
        abort();
    }
    entry->mapping = mapping;
    entry->mapping_size = mapping_size;
    entry->heap_allocated = true;
    if (!llam_stack_cache_quarantine_entry(entry)) {
        llam_record_fatal_deferred(rt, EINVAL);
        abort();
    }
    llam_record_fatal_deferred(rt, release_errno);
    errno = release_errno;
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
        llam_stack_mapping_release_or_quarantine(rt,
                                                 mapping,
                                                 mapping_size);
        return false;
    }
    page_size = (size_t)raw_page_size;
    if (stack_size > SIZE_MAX - page_size ||
        mapping_size != stack_size + page_size ||
        stack_base != (unsigned char *)mapping + page_size) {
        llam_record_fatal_deferred(rt, EINVAL);
        llam_stack_mapping_release_or_quarantine(rt,
                                                 mapping,
                                                 mapping_size);
        return false;
    }
    if ((rt->resource_plan.stack_cache_flags &
         LLAM_RUNTIME_STACK_CACHE_F_DISABLED) != 0U ||
        rt->resource_plan.stack_cache_budget_bytes == 0U) {
        llam_stack_mapping_release_or_quarantine(rt,
                                                 mapping,
                                                 mapping_size);
        return false;
    }
    if (!llam_stack_cache_prepare_return(rt,
                                         stack_base,
                                         stack_size,
                                         &state,
                                         &committed_bytes)) {
        llam_stack_mapping_release_or_quarantine(rt,
                                                 mapping,
                                                 mapping_size);
        return false;
    }
    if (!llam_stack_cache_account_reserve(rt,
                                          mapping_size,
                                          committed_bytes)) {
        llam_stack_mapping_release_or_quarantine(rt,
                                                 mapping,
                                                 mapping_size);
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
        llam_stack_mapping_release_or_quarantine(rt,
                                                 mapping,
                                                 mapping_size);
    } else {
        llam_stack_cache_maintain(rt, last_return_ns);
    }
    return published;
}

/** Drain and release cached stack mappings or metadata entries. */
static void llam_stack_cache_drain_list(llam_runtime_t *rt,
                                        llam_stack_cache_entry_t *entry) {
    while (entry != NULL) {
        llam_stack_cache_entry_t *next = entry->next;
        bool heap_allocated = entry->heap_allocated;
        bool destroy_entry = true;

        if (entry->mapping != NULL && entry->mapping_size != 0U) {
            if (entry->owner_runtime != rt) {
                llam_record_fatal_deferred(rt, EINVAL);
            }
            if (llam_stack_mapping_release(rt,
                                           entry->mapping,
                                           entry->mapping_size)) {
                llam_stack_cache_account_remove(
                    rt,
                    entry->mapping_size,
                    entry->committed_bytes);
            } else if (llam_stack_cache_quarantine_entry(entry)) {
                int release_errno = errno != 0 ? errno : EIO;

                llam_stack_cache_account_remove(
                    rt,
                    entry->mapping_size,
                    entry->committed_bytes);
                llam_record_fatal_deferred(rt, release_errno);
                destroy_entry = false;
            } else {
                /*
                 * Mapping-bearing cache entries are heap metadata by
                 * construction. If corruption violates that invariant, retain
                 * the object rather than knowingly erasing the last pointer.
                 */
                llam_record_fatal_deferred(rt, EINVAL);
                destroy_entry = false;
            }
        }
        if (destroy_entry && heap_allocated) {
            free(entry);
        } else if (destroy_entry) {
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
    llam_stack_cache_entry_t *lists[5];

    if (rt == NULL) {
        return;
    }
    lists[0] = rt->stack_cache_default;
    lists[1] = rt->stack_cache_large;
    lists[2] = rt->stack_cache_huge;
    lists[3] = rt->stack_cache_entry_free;
    lists[4] = rt->stack_cache_release_pending;
    rt->stack_cache_default = NULL;
    rt->stack_cache_large = NULL;
    rt->stack_cache_huge = NULL;
    rt->stack_cache_entry_free = NULL;
    rt->stack_cache_release_pending = NULL;
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
