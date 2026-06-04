/**
 * @file src/core/task/task_stack.c
 * @brief Fiber stack allocation, guard handling, and stack class sizing.
 *
 * @details
 * This translation unit owns stack mapping lifetime. Fiber stacks are allocated
 * with a guard page, cached first in the task's home shard, and then in a
 * runtime-wide fallback cache. Keeping stacks warm avoids repeated @c mmap /
 * @c mprotect cost in spawn-heavy workloads while preserving guard-page
 * protection for overflow diagnostics.
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
    void *mapping = NULL;
    size_t mapping_size = 0U;
    void *stack_base = NULL;

    if (mapping_out != NULL) {
        *mapping_out = NULL;
    }
    if (stack_base_out != NULL) {
        *stack_base_out = NULL;
    }
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
        // Metadata is recycled while the stack mapping is transferred to the
        // caller; the mapping itself remains live and protected.
        *head = entry->next;
        if (*count > 0U) {
            *count -= 1U;
        }
        mapping = entry->mapping;
        mapping_size = entry->mapping_size;
        stack_base = entry->stack_base;
        llam_runtime_stack_cache_entry_free_locked(rt, entry);
    }
    pthread_mutex_unlock(&rt->stack_cache_lock);
    if (entry == NULL) {
        return false;
    }

    if (mapping_out != NULL) {
        *mapping_out = mapping;
    }
    if (mapping_size_out != NULL) {
        *mapping_size_out = mapping_size;
    }
    if (stack_base_out != NULL) {
        *stack_base_out = stack_base;
    }
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
    void *mapping;
    size_t mapping_size;
    void *stack_base;

    if (mapping_out != NULL) {
        *mapping_out = NULL;
    }
    if (stack_base_out != NULL) {
        *stack_base_out = NULL;
    }
    if (shard == NULL) {
        return false;
    }
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
    if (*count > 0U) {
        *count -= 1U;
    }
    mapping = entry->mapping;
    mapping_size = entry->mapping_size;
    stack_base = entry->stack_base;
    llam_shard_stack_cache_entry_free(shard, entry);
    pthread_mutex_unlock(&shard->stack_cache_lock);

    if (mapping_out != NULL) {
        *mapping_out = mapping;
    }
    if (mapping_size_out != NULL) {
        *mapping_size_out = mapping_size;
    }
    if (stack_base_out != NULL) {
        *stack_base_out = stack_base;
    }
    return true;
}

/**
 * @brief Push a stack mapping into the runtime-wide fallback cache.
 *
 * @param rt            Runtime cache owner.
 * @param mapping       Mapping base pointer.
 * @param mapping_size  Mapping size including guard page.
 * @param stack_base    Usable stack base after guard page.
 * @param stack_size    Usable stack size.
 * @param entry_storage Optional caller-owned metadata entry to reuse.
 */
static void llam_runtime_stack_cache_push(llam_runtime_t *rt,
                                        void *mapping,
                                        size_t mapping_size,
                                        void *stack_base,
                                        size_t stack_size,
                                        llam_stack_cache_entry_t *entry_storage) {
    llam_stack_cache_entry_t **head;
    unsigned *count;
    unsigned limit;
    llam_stack_cache_entry_t *entry;

    if (mapping == NULL || mapping_size == 0U || stack_base == NULL || stack_size == 0U ||
        rt == NULL || !rt->stack_cache_lock_initialized) {
        if (mapping != NULL && mapping_size != 0U) {
            // Invalid/unavailable cache owner: release the mapping rather than
            // leaking a stack after task teardown.
            (void)munmap(mapping, mapping_size);
        }
        return;
    }

    head = llam_runtime_stack_cache_head(rt, stack_size);
    count = llam_runtime_stack_cache_count(rt, stack_size);
    limit = llam_runtime_stack_cache_limit(stack_size);
    if (head == NULL || count == NULL || limit == 0U) {
        (void)munmap(mapping, mapping_size);
        return;
    }

    pthread_mutex_lock(&rt->stack_cache_lock);
    if (*count >= limit) {
        pthread_mutex_unlock(&rt->stack_cache_lock);
        if (entry_storage != NULL) {
            memset(entry_storage, 0, sizeof(*entry_storage));
        }
        // Cache is full. Returning to the OS bounds memory usage for bursty
        // workloads that temporarily allocate many stacks.
        (void)munmap(mapping, mapping_size);
        return;
    }

    entry = entry_storage;
    if (entry != NULL) {
        memset(entry, 0, sizeof(*entry));
    } else {
        entry = llam_runtime_stack_cache_entry_alloc_locked(rt);
        if (entry == NULL) {
            pthread_mutex_unlock(&rt->stack_cache_lock);
            (void)munmap(mapping, mapping_size);
            return;
        }
    }
    entry->mapping = mapping;
    entry->mapping_size = mapping_size;
    entry->stack_base = stack_base;
    entry->stack_size = stack_size;
    entry->next = *head;
    *head = entry;
    *count += 1U;
    pthread_mutex_unlock(&rt->stack_cache_lock);
}

/**
 * @brief Push a stack mapping into a shard-local cache.
 *
 * @param shard        Shard cache owner.
 * @param mapping      Mapping base pointer.
 * @param mapping_size Mapping size including guard page.
 * @param stack_base   Usable stack base after guard page.
 * @param stack_size   Usable stack size.
 * @return true if the mapping was cached, false if the caller must fall back.
 */
static bool llam_shard_stack_cache_push(llam_shard_t *shard,
                                      void *mapping,
                                      size_t mapping_size,
                                      void *stack_base,
                                      size_t stack_size) {
    llam_stack_cache_entry_t **head;
    unsigned *count;
    unsigned limit;
    llam_stack_cache_entry_t *entry;

    if (shard == NULL || mapping == NULL || mapping_size == 0U || stack_base == NULL || stack_size == 0U) {
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
    entry->mapping = mapping;
    entry->mapping_size = mapping_size;
    entry->stack_base = stack_base;
    entry->stack_size = stack_size;
    entry->next = *head;
    *head = entry;
    *count += 1U;
    pthread_mutex_unlock(&shard->stack_cache_lock);
    return true;
}

/**
 * @brief Drain and release a list of cached stack mappings or metadata entries.
 *
 * @param entry List head.
 */
static void llam_stack_cache_drain_list(llam_stack_cache_entry_t *entry) {
    while (entry != NULL) {
        llam_stack_cache_entry_t *next = entry->next;
        bool heap_allocated = entry->heap_allocated;

        if (entry->mapping != NULL && entry->mapping_size != 0U) {
            (void)munmap(entry->mapping, entry->mapping_size);
        }
        if (heap_allocated) {
            free(entry);
        } else {
            // Stack entries embedded in other objects are only cleared; heap
            // entries are owned by the cache and freed above.
            memset(entry, 0, sizeof(*entry));
        }
        entry = next;
    }
}

/**
 * @brief Drain all stack-cache lists owned by a shard.
 *
 * @param shard Shard whose stack caches should be emptied.
 */
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
    for (unsigned i = 0U; i < (unsigned)(sizeof(lists) / sizeof(lists[0])); ++i) {
        llam_stack_cache_drain_list(lists[i]);
    }
}

/**
 * @brief Drain all runtime-wide stack-cache lists.
 *
 * @param rt Runtime whose fallback stack caches should be emptied.
 */
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
    for (unsigned i = 0U; i < (unsigned)(sizeof(lists) / sizeof(lists[0])); ++i) {
        llam_stack_cache_drain_list(lists[i]);
    }
}

/**
 * @brief Preallocate default-class stacks into shard/runtime caches.
 *
 * @param rt Runtime whose stack cache should be warmed.
 */
void llam_runtime_prewarm_stack_cache(llam_runtime_t *rt) {
    const char *value;
    unsigned target = 128U;
    unsigned i;
    long page_size;
    size_t stack_size;
    size_t mapping_size;

    if (rt == NULL || !rt->stack_cache_lock_initialized) {
        return;
    }
    if (rt->profile == LLAM_RUNTIME_PROFILE_RELEASE_FAST) {
        target = 256U;
    }
    value = llam_env_get("LLAM_STACK_CACHE_PREWARM");
    if (value != NULL && value[0] != '\0') {
        char *end = NULL;
        unsigned long parsed;

        if (!llam_ascii_is_space((unsigned char)value[0]) && value[0] != '-' && value[0] != '+') {
            errno = 0;
            parsed = strtoul(value, &end, 10);
            if (errno == 0 && end != value && *end == '\0') {
                if (parsed > LLAM_STACK_CACHE_DEFAULT_LIMIT) {
                    parsed = LLAM_STACK_CACHE_DEFAULT_LIMIT;
                }
                target = (unsigned)parsed;
            }
        }
    }
    if (target == 0U) {
        return;
    }

    page_size = llam_page_size();
    stack_size = llam_stack_bytes(LLAM_STACK_CLASS_DEFAULT);
    mapping_size = stack_size + (size_t)page_size;
    for (i = 0U; i < target; ++i) {
        void *mapping = mmap(NULL,
                             mapping_size,
                             PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK,
                             -1,
                             0);

        if (mapping == MAP_FAILED) {
            return;
        }
        if (mprotect(mapping, (size_t)page_size, PROT_NONE) != 0) {
            (void)munmap(mapping, mapping_size);
            return;
        }
        // Prefer per-shard warm stacks so early spawns avoid both global cache
        // locking and fresh mmap/mprotect work.
        if (rt->shards == NULL || rt->active_shards == 0U ||
            !llam_shard_stack_cache_push(&rt->shards[i % rt->active_shards],
                                       mapping,
                                       mapping_size,
                                       (char *)mapping + page_size,
                                       stack_size)) {
            llam_runtime_stack_cache_push(rt, mapping, mapping_size, (char *)mapping + page_size, stack_size, NULL);
        }
    }
}

/**
 * @brief Allocate and initialize a fiber stack for a task.
 *
 * @param task        Task receiving stack/context state.
 * @param stack_class Requested stack size class.
 * @return 0 on success, -1 on allocation or context setup failure.
 */
int llam_alloc_task_stack(llam_task_t *task, llam_stack_class_t stack_class) {
    long page_size = llam_page_size();
    size_t stack_size = llam_stack_bytes(stack_class);
    size_t mapping_size = stack_size + (size_t)page_size;
    void *mapping;
    llam_shard_t *cache_shard = g_llam_tls_shard;
    llam_runtime_t *rt;

    if (task == NULL) {
        errno = EINVAL;
        return -1;
    }
    rt = task->owner_runtime;
    if (rt == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (cache_shard == NULL && task->home_shard < rt->active_shards) {
        cache_shard = &rt->shards[task->home_shard];
    }

    // Lookup order is local shard cache, then runtime fallback cache, then a
    // fresh guarded mmap. This preserves locality without failing if the owner
    // shard has no warm stack available.
    if (!llam_shard_stack_cache_pop(cache_shard, stack_size, &mapping, &mapping_size, &task->stack_base) &&
        !llam_runtime_stack_cache_pop(rt, stack_size, &mapping, &mapping_size, &task->stack_base)) {
        mapping = mmap(NULL,
                       mapping_size,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK,
                       -1,
                       0);
        if (mapping == MAP_FAILED) {
            return -1;
        }

        if (mprotect(mapping, (size_t)page_size, PROT_NONE) != 0) {
            int saved_errno = errno;
            munmap(mapping, mapping_size);
            errno = saved_errno;
            return -1;
        }
        task->stack_base = (char *)mapping + page_size;
    }
    task->stack_mapping = mapping;
    task->mapping_size = mapping_size;
    task->stack_size = stack_size;
    if (llam_ctx_init_fp_state(&task->ctx, task->owner_runtime) != 0) {
        int saved_errno = errno;

        munmap(task->stack_mapping, task->mapping_size);
        task->stack_mapping = NULL;
        task->mapping_size = 0U;
        task->stack_base = NULL;
        task->stack_size = 0U;
        errno = saved_errno;
        return -1;
    }
#if LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64
    if (rt->windows_unsafe_skip_task_simd != 0U) {
        /*
         * Opt-in ceiling mode for benchmark/runtime profiles that guarantee
         * managed tasks do not depend on ABI-preserved XMM6-XMM15 state across
         * cooperative switches.
         */
        task->ctx.simd_flags = LLAM_CTX_SIMD_F_SKIP_SAVE | LLAM_CTX_SIMD_F_SKIP_RESTORE;
    }
#endif
#if ((LLAM_PLATFORM_LINUX || LLAM_PLATFORM_DARWIN || LLAM_PLATFORM_BSD) && LLAM_ARCH_X86_64) || \
    (LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64)
    {
        uintptr_t stack_top = (uintptr_t)task->stack_base + task->stack_size;
        uint64_t *sp;

        stack_top &= ~(uintptr_t)0xFUL;
        sp = (uint64_t *)stack_top;
        // Hand-written x86-64 bootstrap: the assembly switch restores callee
        // saved registers, then returns into llam_fiber_bootstrap with r12=task.
        *--sp = (uint64_t)(uintptr_t)llam_task_exit_internal;
        *--sp = (uint64_t)(uintptr_t)llam_fiber_bootstrap;

        task->ctx.rsp = (uint64_t)(uintptr_t)sp;
        task->ctx.rbx = 0;
        task->ctx.rbp = 0;
#if LLAM_PLATFORM_WINDOWS
        task->ctx.rsi = 0;
        task->ctx.rdi = 0;
#endif
        task->ctx.r12 = (uint64_t)(uintptr_t)task;
        task->ctx.r13 = 0;
        task->ctx.r14 = 0;
        task->ctx.r15 = 0;
    }
#else
    // Non-x86 ports delegate initial context construction to the platform
    // context implementation.
    if (llam_ctx_make_task(&task->ctx, task->stack_base, task->stack_size, task) != 0) {
        int saved_errno = errno;

        llam_ctx_destroy_fp_state(&task->ctx);
        munmap(task->stack_mapping, task->mapping_size);
        task->stack_mapping = NULL;
        task->mapping_size = 0U;
        task->stack_base = NULL;
        task->stack_size = 0U;
        errno = saved_errno;
        return -1;
    }
#endif
    return 0;
}

/**
 * @brief Release a task's stack, preferably into a stack cache.
 *
 * @param task Task whose stack mapping should be detached and recycled.
 */
void llam_task_release_stack(llam_task_t *task) {
    void *mapping;
    size_t mapping_size;
    void *stack_base;
    size_t stack_size;
    llam_shard_t *cache_shard = NULL;
    pthread_mutex_t *diagnostic_lock = NULL;
    llam_runtime_t *rt;

    if (task == NULL) {
        return;
    }

    rt = task->owner_runtime;
    if (rt != NULL && task->alloc_owner_shard < rt->active_shards) {
        /*
         * Runtime dumps walk alloc-owner diagnostic lists under this lock. Keep
         * stack pointer/size detachment serialized with those snapshots; the
         * cache insertion itself happens after unlock to avoid nesting cache
         * locks under the diagnostic list lock.
         */
        diagnostic_lock = &rt->shards[task->alloc_owner_shard].lock;
        pthread_mutex_lock(diagnostic_lock);
    }
    if (task->stack_mapping == NULL || task->mapping_size == 0U || task->stack_base == NULL || task->stack_size == 0U) {
        if (diagnostic_lock != NULL) {
            pthread_mutex_unlock(diagnostic_lock);
        }
        return;
    }
    mapping = task->stack_mapping;
    mapping_size = task->mapping_size;
    stack_base = task->stack_base;
    stack_size = task->stack_size;
    task->stack_mapping = NULL;
    task->mapping_size = 0U;
    task->stack_base = NULL;
    task->stack_size = 0U;
    if (diagnostic_lock != NULL) {
        pthread_mutex_unlock(diagnostic_lock);
    }
    if (rt != NULL && task->home_shard < rt->active_shards) {
        cache_shard = &rt->shards[task->home_shard];
    } else {
        cache_shard = g_llam_tls_shard;
    }
    // Try to preserve home-shard locality first; overflow falls back to the
    // runtime cache, which may finally munmap if all limits are reached.
    if (!llam_shard_stack_cache_push(cache_shard, mapping, mapping_size, stack_base, stack_size)) {
        llam_runtime_stack_cache_push(rt, mapping, mapping_size, stack_base, stack_size, NULL);
    }
}

/**
 * @brief Choose an active shard for a spawn operation.
 *
 * @param rt Runtime whose shards should be considered.
 * @return Shard id selected for the new task.
 */
unsigned llam_pick_spawn_shard(llam_runtime_t *rt) {
    unsigned start_id;
    unsigned shard_id;
    unsigned ticket;
    unsigned i;

    if (g_llam_tls_shard != NULL &&
        g_llam_tls_shard->runtime == rt &&
        g_llam_tls_shard->id < rt->active_shards) {
        return g_llam_tls_shard->id;
    }

    /*
     * Unmanaged host threads may call spawn entry points concurrently. The
     * ticket is only a placement hint, but it still must be atomic to avoid
     * corrupting runtime-local placement state under multi-threaded embedders.
     */
    ticket = atomic_fetch_add_explicit(&rt->next_spawn_shard, 1U, memory_order_relaxed);
    start_id = ticket % rt->active_shards;
    shard_id = start_id;
    // Round-robin across shards that currently accept work; this avoids pushing
    // new tasks into shards that are draining or marked offline.
    for (i = 0; i < rt->active_shards; ++i) {
        unsigned candidate = (start_id + i) % rt->active_shards;

        if (llam_shard_accepts_new_work(&rt->shards[candidate])) {
            return candidate;
        }
    }

    return shard_id;
}
