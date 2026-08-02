/**
 * @file src/core/task/stack_cache_lists.c
 * @brief Fiber stack-cache list ownership and local reuse.
 *
 * @details
 * A retained mapping is linked first through its preferred shard and then
 * through the runtime fallback cache. List detach and byte-account removal
 * share one owning lock; platform VM transitions are delegated to the
 * authority module and never occur under these locks.
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

/** Runtime-wide cached default stack limit. */
#define LLAM_STACK_CACHE_DEFAULT_LIMIT 4096U
/** Runtime-wide cached large stack limit. */
#define LLAM_STACK_CACHE_LARGE_LIMIT 512U
/** Runtime-wide cached huge stack limit. */
#define LLAM_STACK_CACHE_HUGE_LIMIT 128U
/** Per-shard cached default stack limit in normal profiles. */
#define LLAM_STACK_CACHE_LOCAL_DEFAULT_LIMIT 256U
/** Per-shard cached default stack limit in release-fast profile. */
#define LLAM_STACK_CACHE_LOCAL_DEFAULT_RELEASE_FAST_LIMIT 512U
/** Per-shard cached large stack limit. */
#define LLAM_STACK_CACHE_LOCAL_LARGE_LIMIT 64U
/** Per-shard cached huge stack limit. */
#define LLAM_STACK_CACHE_LOCAL_HUGE_LIMIT 16U

/**
 * @brief Resolve a public stack class from its exact usable size.
 */
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

/**
 * @brief Select the runtime-wide cache head for a stack size.
 *
 * @param rt         Runtime cache owner.
 * @param stack_size Exact stack payload size.
 * @return Address of the matching list head, or NULL for unsupported sizes.
 */
static llam_stack_cache_entry_t **llam_runtime_stack_cache_head(llam_runtime_t *rt, size_t stack_size) {
    if (rt == NULL) {
        return NULL;
    }
    if (stack_size == llam_stack_bytes(LLAM_STACK_CLASS_DEFAULT)) {
        return &rt->stack_cache_default;
    }
    if (stack_size == llam_stack_bytes(LLAM_STACK_CLASS_LARGE)) {
        return &rt->stack_cache_large;
    }
    if (stack_size == llam_stack_bytes(LLAM_STACK_CLASS_HUGE)) {
        return &rt->stack_cache_huge;
    }
    return NULL;
}

/**
 * @brief Select the runtime-wide cache count field for a stack size.
 *
 * @param rt         Runtime cache owner.
 * @param stack_size Exact stack payload size.
 * @return Address of the matching count field, or NULL for unsupported sizes.
 */
static unsigned *llam_runtime_stack_cache_count(llam_runtime_t *rt, size_t stack_size) {
    if (rt == NULL) {
        return NULL;
    }
    if (stack_size == llam_stack_bytes(LLAM_STACK_CLASS_DEFAULT)) {
        return &rt->stack_cache_default_count;
    }
    if (stack_size == llam_stack_bytes(LLAM_STACK_CLASS_LARGE)) {
        return &rt->stack_cache_large_count;
    }
    if (stack_size == llam_stack_bytes(LLAM_STACK_CLASS_HUGE)) {
        return &rt->stack_cache_huge_count;
    }
    return NULL;
}

/**
 * @brief Return the runtime-wide cache limit for a stack size.
 *
 * @param stack_size Exact stack payload size.
 * @return Maximum cached mappings for the class, or 0 for unsupported sizes.
 */
static unsigned llam_runtime_stack_cache_limit(size_t stack_size) {
    if (stack_size == llam_stack_bytes(LLAM_STACK_CLASS_DEFAULT)) {
        return LLAM_STACK_CACHE_DEFAULT_LIMIT;
    }
    if (stack_size == llam_stack_bytes(LLAM_STACK_CLASS_LARGE)) {
        return LLAM_STACK_CACHE_LARGE_LIMIT;
    }
    if (stack_size == llam_stack_bytes(LLAM_STACK_CLASS_HUGE)) {
        return LLAM_STACK_CACHE_HUGE_LIMIT;
    }
    return 0U;
}

/**
 * @brief Select the per-shard cache head for a stack size.
 *
 * @param shard      Shard cache owner.
 * @param stack_size Exact stack payload size.
 * @return Address of the matching list head, or NULL for unsupported sizes.
 */
static llam_stack_cache_entry_t **llam_shard_stack_cache_head(llam_shard_t *shard, size_t stack_size) {
    if (shard == NULL) {
        return NULL;
    }
    if (stack_size == llam_stack_bytes(LLAM_STACK_CLASS_DEFAULT)) {
        return &shard->stack_cache_default;
    }
    if (stack_size == llam_stack_bytes(LLAM_STACK_CLASS_LARGE)) {
        return &shard->stack_cache_large;
    }
    if (stack_size == llam_stack_bytes(LLAM_STACK_CLASS_HUGE)) {
        return &shard->stack_cache_huge;
    }
    return NULL;
}

/**
 * @brief Select the per-shard cache count field for a stack size.
 *
 * @param shard      Shard cache owner.
 * @param stack_size Exact stack payload size.
 * @return Address of the matching count field, or NULL for unsupported sizes.
 */
static unsigned *llam_shard_stack_cache_count(llam_shard_t *shard, size_t stack_size) {
    if (shard == NULL) {
        return NULL;
    }
    if (stack_size == llam_stack_bytes(LLAM_STACK_CLASS_DEFAULT)) {
        return &shard->stack_cache_default_count;
    }
    if (stack_size == llam_stack_bytes(LLAM_STACK_CLASS_LARGE)) {
        return &shard->stack_cache_large_count;
    }
    if (stack_size == llam_stack_bytes(LLAM_STACK_CLASS_HUGE)) {
        return &shard->stack_cache_huge_count;
    }
    return NULL;
}

/**
 * @brief Return the per-shard stack-cache limit for a stack size.
 *
 * @param shard      Shard cache owner.
 * @param stack_size Exact stack payload size.
 * @return Maximum cached mappings for the class, or 0 for unsupported sizes.
 */
static unsigned llam_shard_stack_cache_limit(const llam_shard_t *shard, size_t stack_size) {
    const llam_runtime_t *rt = shard != NULL ? shard->runtime : NULL;

    if (stack_size == llam_stack_bytes(LLAM_STACK_CLASS_DEFAULT)) {
        return rt != NULL && rt->profile == LLAM_RUNTIME_PROFILE_RELEASE_FAST
                   ? LLAM_STACK_CACHE_LOCAL_DEFAULT_RELEASE_FAST_LIMIT
                   : LLAM_STACK_CACHE_LOCAL_DEFAULT_LIMIT;
    }
    if (stack_size == llam_stack_bytes(LLAM_STACK_CLASS_LARGE)) {
        return LLAM_STACK_CACHE_LOCAL_LARGE_LIMIT;
    }
    if (stack_size == llam_stack_bytes(LLAM_STACK_CLASS_HUGE)) {
        return LLAM_STACK_CACHE_LOCAL_HUGE_LIMIT;
    }
    return 0U;
}

/**
 * @brief Allocate or reuse a runtime-wide stack-cache entry.
 *
 * @param rt Runtime cache owner; its stack cache lock must already be held.
 * @return Cleared entry object on success, or NULL on allocation failure.
 */
static llam_stack_cache_entry_t *llam_runtime_stack_cache_entry_alloc_locked(llam_runtime_t *rt) {
    llam_stack_cache_entry_t *entry;

    if (rt == NULL) {
        return NULL;
    }
    entry = rt->stack_cache_entry_free;
    if (entry != NULL) {
        // Entry metadata is cached separately from stack mappings so pushing a
        // stack back into the cache does not require a fresh malloc.
        rt->stack_cache_entry_free = entry->next;
        memset(entry, 0, sizeof(*entry));
        entry->heap_allocated = true;
        return entry;
    }
    entry = calloc(1, sizeof(*entry));
    if (entry != NULL) {
        entry->heap_allocated = true;
    }
    return entry;
}

/**
 * @brief Return a metadata entry to the runtime-wide entry cache.
 *
 * @param rt    Runtime cache owner; its stack cache lock must already be held.
 * @param entry Entry metadata object to recycle.
 */
static void llam_runtime_stack_cache_entry_free_locked(llam_runtime_t *rt, llam_stack_cache_entry_t *entry) {
    if (rt == NULL || entry == NULL) {
        return;
    }
    memset(entry, 0, sizeof(*entry));
    entry->heap_allocated = true;
    entry->next = rt->stack_cache_entry_free;
    rt->stack_cache_entry_free = entry;
}

/**
 * @brief Allocate or reuse a shard-local stack-cache entry.
 *
 * @param shard Shard cache owner; its stack cache lock must already be held.
 * @return Cleared entry object on success, or NULL on allocation failure.
 */
static llam_stack_cache_entry_t *llam_shard_stack_cache_entry_alloc(llam_shard_t *shard) {
    llam_stack_cache_entry_t *entry;

    if (shard == NULL) {
        return NULL;
    }
    entry = shard->stack_cache_entry_free;
    if (entry != NULL) {
        shard->stack_cache_entry_free = entry->next;
        memset(entry, 0, sizeof(*entry));
        entry->heap_allocated = true;
        return entry;
    }
    entry = calloc(1, sizeof(*entry));
    if (entry != NULL) {
        entry->heap_allocated = true;
    }
    return entry;
}

/**
 * @brief Return a metadata entry to the shard-local entry cache.
 *
 * @param shard Shard cache owner; its stack cache lock must already be held.
 * @param entry Entry metadata object to recycle.
 */
static void llam_shard_stack_cache_entry_free(llam_shard_t *shard, llam_stack_cache_entry_t *entry) {
    if (shard == NULL || entry == NULL) {
        return;
    }
    memset(entry, 0, sizeof(*entry));
    entry->heap_allocated = true;
    entry->next = shard->stack_cache_entry_free;
    shard->stack_cache_entry_free = entry;
}

/**
 * @brief Validate retained metadata before transferring a mapping to a task.
 */
static bool llam_stack_cache_entry_valid(
    llam_runtime_t *rt,
    size_t requested_stack_size,
    llam_runtime_t *owner_runtime,
    void *mapping,
    size_t mapping_size,
    void *stack_base,
    size_t stack_size,
    uint64_t committed_bytes,
    uint64_t last_return_ns,
    uint32_t stack_class,
    uint32_t state) {
    uint32_t requested_class;
    long raw_page_size = llam_page_size();
    size_t page_size;

    if (rt == NULL || raw_page_size <= 0 ||
        !llam_stack_cache_class_for_size(requested_stack_size,
                                         &requested_class)) {
        return false;
    }
    page_size = (size_t)raw_page_size;
    if (requested_stack_size > SIZE_MAX - page_size) {
        return false;
    }
    return owner_runtime == rt &&
           mapping != NULL &&
           mapping != MAP_FAILED &&
           mapping_size == requested_stack_size + page_size &&
           stack_base == (unsigned char *)mapping + page_size &&
           stack_size == requested_stack_size &&
           last_return_ns != 0U &&
           stack_class == requested_class &&
           ((state == LLAM_STACK_CACHE_ENTRY_READY &&
             committed_bytes == (uint64_t)stack_size) ||
            (state == LLAM_STACK_CACHE_ENTRY_DISCARDED &&
             committed_bytes == 0U));
}

/**
 * @brief Finish a cache pop after the owning list lock has been released.
 */
static bool llam_stack_cache_finish_pop(
    llam_runtime_t *rt,
    size_t requested_stack_size,
    llam_stack_cache_entry_t *entry,
    void **mapping_out,
    size_t *mapping_size_out,
    void **stack_base_out) {
    int saved_errno;

    if (entry == NULL) {
        return false;
    }
    if (!llam_stack_cache_entry_valid(rt,
                                      requested_stack_size,
                                      entry->owner_runtime,
                                      entry->mapping,
                                      entry->mapping_size,
                                      entry->stack_base,
                                      entry->stack_size,
                                      entry->committed_bytes,
                                      entry->last_return_ns,
                                      entry->stack_class,
                                      entry->state)) {
        llam_record_fatal_deferred(rt, EINVAL);
        (void)llam_stack_cache_release_detached_entry(rt, entry);
        errno = EINVAL;
        return false;
    }
    if (entry->state == LLAM_STACK_CACHE_ENTRY_DISCARDED &&
        llam_stack_vm_reactivate(entry->stack_base,
                                 entry->stack_size) != 0) {
        saved_errno = errno != 0 ? errno : EIO;
        atomic_fetch_add_explicit(
            &rt->stack_cache_secure_return_failures,
            1U,
            memory_order_relaxed);
        (void)llam_stack_cache_release_detached_entry(rt, entry);
        errno = saved_errno;
        return false;
    }
    *mapping_out = entry->mapping;
    *mapping_size_out = entry->mapping_size;
    *stack_base_out = entry->stack_base;
    return true;
}

/**
 * @brief Pop a cached stack mapping from the runtime-wide cache.
 *
 * @param rt               Runtime cache owner.
 * @param stack_size       Exact stack payload size.
 * @param mapping_out      Receives mapping base pointer.
 * @param mapping_size_out Receives mapping size including guard page.
 * @param stack_base_out   Receives usable stack base after guard page.
 * @return true when a cached mapping was returned, false on cache miss.
 */
static bool llam_runtime_stack_cache_pop(llam_runtime_t *rt,
                                         size_t stack_size,
                                         void **mapping_out,
                                         size_t *mapping_size_out,
                                         void **stack_base_out) {
    llam_stack_cache_entry_t **head;
    unsigned *count;
    llam_stack_cache_entry_t *entry;
    bool popped;

    if (mapping_out == NULL || mapping_size_out == NULL ||
        stack_base_out == NULL) {
        return false;
    }
    *mapping_out = NULL;
    *mapping_size_out = 0U;
    *stack_base_out = NULL;
    if (rt == NULL || !rt->stack_cache_lock_initialized) {
        return false;
    }

    head = llam_runtime_stack_cache_head(rt, stack_size);
    count = llam_runtime_stack_cache_count(rt, stack_size);
    if (head == NULL || count == NULL) {
        return false;
    }

    pthread_mutex_lock(&rt->stack_cache_lock);
    entry = *head;
    if (entry != NULL) {
        *head = entry->next;
        entry->next = NULL;
        if (*count > 0U) {
            *count -= 1U;
        }
    }
    pthread_mutex_unlock(&rt->stack_cache_lock);
    if (entry == NULL) {
        return false;
    }

    popped = llam_stack_cache_finish_pop(rt,
                                         stack_size,
                                         entry,
                                         mapping_out,
                                         mapping_size_out,
                                         stack_base_out);
    if (!popped) {
        return false;
    }
    llam_stack_cache_account_remove(rt,
                                    entry->mapping_size,
                                    entry->committed_bytes);
    pthread_mutex_lock(&rt->stack_cache_lock);
    llam_runtime_stack_cache_entry_free_locked(rt, entry);
    pthread_mutex_unlock(&rt->stack_cache_lock);
    return true;
}

/**
 * @brief Pop a cached stack mapping from a shard-local cache.
 *
 * @param shard            Shard cache owner.
 * @param stack_size       Exact stack payload size.
 * @param mapping_out      Receives mapping base pointer.
 * @param mapping_size_out Receives mapping size including guard page.
 * @param stack_base_out   Receives usable stack base after guard page.
 * @return true when a cached mapping was returned, false on cache miss.
 */
static bool llam_shard_stack_cache_pop(llam_shard_t *shard,
                                       size_t stack_size,
                                       void **mapping_out,
                                       size_t *mapping_size_out,
                                       void **stack_base_out) {
    llam_stack_cache_entry_t **head;
    unsigned *count;
    llam_stack_cache_entry_t *entry;
    llam_runtime_t *rt;
    bool popped;

    if (mapping_out == NULL || mapping_size_out == NULL ||
        stack_base_out == NULL) {
        return false;
    }
    *mapping_out = NULL;
    *mapping_size_out = 0U;
    *stack_base_out = NULL;
    if (shard == NULL || shard->runtime == NULL) {
        return false;
    }
    rt = shard->runtime;
    head = llam_shard_stack_cache_head(shard, stack_size);
    count = llam_shard_stack_cache_count(shard, stack_size);
    if (head == NULL || count == NULL) {
        return false;
    }

    pthread_mutex_lock(&shard->stack_cache_lock);
    entry = *head;
    if (entry == NULL) {
        pthread_mutex_unlock(&shard->stack_cache_lock);
        return false;
    }

    *head = entry->next;
    entry->next = NULL;
    if (*count > 0U) {
        *count -= 1U;
    }
    pthread_mutex_unlock(&shard->stack_cache_lock);

    popped = llam_stack_cache_finish_pop(rt,
                                         stack_size,
                                         entry,
                                         mapping_out,
                                         mapping_size_out,
                                         stack_base_out);
    if (!popped) {
        return false;
    }
    llam_stack_cache_account_remove(rt,
                                    entry->mapping_size,
                                    entry->committed_bytes);
    pthread_mutex_lock(&shard->stack_cache_lock);
    llam_shard_stack_cache_entry_free(shard, entry);
    pthread_mutex_unlock(&shard->stack_cache_lock);
    return true;
}

/**
 * @brief Populate retained-mapping metadata before list publication.
 */
static void llam_stack_cache_entry_init(
    llam_stack_cache_entry_t *entry,
    llam_runtime_t *rt,
    void *mapping,
    size_t mapping_size,
    void *stack_base,
    size_t stack_size,
    uint64_t committed_bytes,
    uint64_t last_return_ns,
    uint32_t stack_class,
    uint32_t state) {
    memset(entry, 0, sizeof(*entry));
    entry->owner_runtime = rt;
    entry->mapping = mapping;
    entry->mapping_size = mapping_size;
    entry->stack_base = stack_base;
    entry->stack_size = stack_size;
    entry->committed_bytes = committed_bytes;
    entry->last_return_ns = last_return_ns;
    entry->stack_class = stack_class;
    entry->state = state;
    entry->heap_allocated = true;
}

/**
 * @brief Publish an already-authorized mapping to the runtime fallback cache.
 *
 * The caller owns VM cleanup and authority rollback when publication fails.
 */
static bool llam_runtime_stack_cache_publish(
    llam_runtime_t *rt,
    void *mapping,
    size_t mapping_size,
    void *stack_base,
    size_t stack_size,
    uint64_t committed_bytes,
    uint64_t last_return_ns,
    uint32_t stack_class,
    uint32_t state) {
    llam_stack_cache_entry_t **head;
    unsigned *count;
    unsigned limit;
    llam_stack_cache_entry_t *entry;

    if (mapping == NULL || mapping_size == 0U ||
        stack_base == NULL || stack_size == 0U ||
        rt == NULL || !rt->stack_cache_lock_initialized) {
        return false;
    }

    head = llam_runtime_stack_cache_head(rt, stack_size);
    count = llam_runtime_stack_cache_count(rt, stack_size);
    limit = llam_runtime_stack_cache_limit(stack_size);
    if (head == NULL || count == NULL || limit == 0U) {
        return false;
    }

    pthread_mutex_lock(&rt->stack_cache_lock);
    if (*count >= limit) {
        pthread_mutex_unlock(&rt->stack_cache_lock);
        return false;
    }

    entry = llam_runtime_stack_cache_entry_alloc_locked(rt);
    if (entry == NULL) {
        pthread_mutex_unlock(&rt->stack_cache_lock);
        return false;
    }
    llam_stack_cache_entry_init(entry,
                                rt,
                                mapping,
                                mapping_size,
                                stack_base,
                                stack_size,
                                committed_bytes,
                                last_return_ns,
                                stack_class,
                                state);
    entry->next = *head;
    *head = entry;
    *count += 1U;
    pthread_mutex_unlock(&rt->stack_cache_lock);
    return true;
}

/**
 * @brief Publish an already-authorized mapping to a shard-local cache.
 *
 * The caller owns VM cleanup and authority rollback when publication fails.
 */
static bool llam_shard_stack_cache_publish(
    llam_shard_t *shard,
    void *mapping,
    size_t mapping_size,
    void *stack_base,
    size_t stack_size,
    uint64_t committed_bytes,
    uint64_t last_return_ns,
    uint32_t stack_class,
    uint32_t state) {
    llam_stack_cache_entry_t **head;
    unsigned *count;
    unsigned limit;
    llam_stack_cache_entry_t *entry;

    if (shard == NULL || shard->runtime == NULL ||
        mapping == NULL || mapping_size == 0U ||
        stack_base == NULL || stack_size == 0U) {
        return false;
    }
    head = llam_shard_stack_cache_head(shard, stack_size);
    count = llam_shard_stack_cache_count(shard, stack_size);
    limit = llam_shard_stack_cache_limit(shard, stack_size);
    if (head == NULL || count == NULL || limit == 0U) {
        return false;
    }
    pthread_mutex_lock(&shard->stack_cache_lock);
    if (*count >= limit) {
        pthread_mutex_unlock(&shard->stack_cache_lock);
        return false;
    }
    entry = llam_shard_stack_cache_entry_alloc(shard);
    if (entry == NULL) {
        pthread_mutex_unlock(&shard->stack_cache_lock);
        return false;
    }
    llam_stack_cache_entry_init(entry,
                                shard->runtime,
                                mapping,
                                mapping_size,
                                stack_base,
                                stack_size,
                                committed_bytes,
                                last_return_ns,
                                stack_class,
                                state);
    entry->next = *head;
    *head = entry;
    *count += 1U;
    pthread_mutex_unlock(&shard->stack_cache_lock);
    return true;
}

/**
 * @brief Pop from the preferred shard, then the runtime fallback cache.
 */
bool llam_stack_cache_pop_mapping(llam_runtime_t *rt,
                                  llam_shard_t *preferred_shard,
                                  size_t stack_size,
                                  void **mapping_out,
                                  size_t *mapping_size_out,
                                  void **stack_base_out) {
    if (rt == NULL) {
        return false;
    }
    if (preferred_shard != NULL &&
        preferred_shard->runtime == rt &&
        llam_shard_stack_cache_pop(preferred_shard,
                                   stack_size,
                                   mapping_out,
                                   mapping_size_out,
                                   stack_base_out)) {
        return true;
    }
    return llam_runtime_stack_cache_pop(rt,
                                        stack_size,
                                        mapping_out,
                                        mapping_size_out,
                                        stack_base_out);
}

/**
 * @brief Publish authorized metadata to a preferred shard or runtime fallback.
 */
bool llam_stack_cache_publish_mapping(llam_runtime_t *rt,
                                      llam_shard_t *preferred_shard,
                                      void *mapping,
                                      size_t mapping_size,
                                      void *stack_base,
                                      size_t stack_size,
                                      uint64_t committed_bytes,
                                      uint64_t last_return_ns,
                                      uint32_t stack_class,
                                      uint32_t state) {
    if (rt == NULL) {
        return false;
    }
    if (preferred_shard != NULL &&
        preferred_shard->runtime == rt &&
        llam_shard_stack_cache_publish(preferred_shard,
                                       mapping,
                                       mapping_size,
                                       stack_base,
                                       stack_size,
                                       committed_bytes,
                                       last_return_ns,
                                       stack_class,
                                       state)) {
        return true;
    }
    return llam_runtime_stack_cache_publish(rt,
                                            mapping,
                                            mapping_size,
                                            stack_base,
                                            stack_size,
                                            committed_bytes,
                                            last_return_ns,
                                            stack_class,
                                            state);
}
