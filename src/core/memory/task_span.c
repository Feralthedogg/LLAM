/**
 * @file src/core/memory/task_span.c
 * @brief Adaptive, reclaimable spans for unmanaged host-task allocation.
 *
 * @copyright Copyright 2026 Feralthedogg
 *
 * @par License
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
 * See LICENSES/Apache-2.0.txt.
 */

#include "runtime_internal.h"

#define LLAM_TASK_SPAN_MAX_ITEMS 16U
#define LLAM_TASK_SPAN_EMPTY_LIMIT 2U

#if defined(LLAM_ENABLE_TEST_HOOKS)
static atomic_uint g_llam_task_span_fail_stage;

void llam_task_span_test_fail_next(unsigned stage) {
    atomic_store_explicit(&g_llam_task_span_fail_stage, stage, memory_order_release);
}

static bool llam_task_span_fail_stage(unsigned stage) {
    unsigned expected = stage;

    return atomic_compare_exchange_strong_explicit(&g_llam_task_span_fail_stage,
                                                  &expected, 0U,
                                                  memory_order_acq_rel, memory_order_acquire);
}
#else
static bool llam_task_span_fail_stage(unsigned stage) {
    (void)stage;
    return false;
}
#endif

enum {
    LLAM_TASK_SPAN_UNLINKED = 0U,
    LLAM_TASK_SPAN_FULL,
    LLAM_TASK_SPAN_PARTIAL,
    LLAM_TASK_SPAN_EMPTY,
    LLAM_TASK_SPAN_RETIRED,
};

/* Lists and occupancy belong to the allocation P, even when another M returns
 * a host task. The existing allocator mutex serializes that entire cold path.
 * Managed task free lists and remote-free publication are unaffected. */
static void llam_task_span_detach(llam_allocator_t *allocator, llam_alloc_chunk_t *span) {
    llam_alloc_chunk_t **head = NULL;
    llam_alloc_chunk_t **tail = NULL;

    if (span->pool_state == LLAM_TASK_SPAN_PARTIAL) {
        head = &allocator->task_external_partial;
    } else if (span->pool_state == LLAM_TASK_SPAN_EMPTY) {
        head = &allocator->task_external_empty;
        tail = &allocator->task_external_empty_tail;
        allocator->task_external_empty_count -= 1U;
    } else if (span->pool_state == LLAM_TASK_SPAN_RETIRED) {
        head = &allocator->task_external_retired;
        tail = &allocator->task_external_retired_tail;
    }
    if (head != NULL) {
        if (span->pool_prev != NULL) {
            span->pool_prev->pool_next = span->pool_next;
        } else {
            *head = span->pool_next;
        }
        if (span->pool_next != NULL) {
            span->pool_next->pool_prev = span->pool_prev;
        } else if (tail != NULL) {
            *tail = span->pool_prev;
        }
    }
    span->pool_next = NULL;
    span->pool_prev = NULL;
    span->pool_state = LLAM_TASK_SPAN_UNLINKED;
}

static void llam_task_span_push_partial(llam_allocator_t *allocator, llam_alloc_chunk_t *span) {
    span->pool_state = LLAM_TASK_SPAN_PARTIAL;
    span->pool_next = allocator->task_external_partial;
    if (span->pool_next != NULL) {
        span->pool_next->pool_prev = span;
    }
    allocator->task_external_partial = span;
}

static void llam_task_span_push_empty(llam_allocator_t *allocator, llam_alloc_chunk_t *span) {
    span->pool_state = LLAM_TASK_SPAN_EMPTY;
    span->pool_next = allocator->task_external_empty;
    if (span->pool_next != NULL) {
        span->pool_next->pool_prev = span;
    } else {
        allocator->task_external_empty_tail = span;
    }
    allocator->task_external_empty = span;
    allocator->task_external_empty_count += 1U;
}

static void llam_task_span_push_retired(llam_allocator_t *allocator, llam_alloc_chunk_t *span) {
    span->pool_state = LLAM_TASK_SPAN_RETIRED;
    span->pool_prev = allocator->task_external_retired_tail;
    if (span->pool_prev != NULL) {
        span->pool_prev->pool_next = span;
    } else {
        allocator->task_external_retired = span;
    }
    allocator->task_external_retired_tail = span;
}

static void llam_task_span_update_head(llam_allocator_t *allocator) {
    llam_alloc_chunk_t *span = allocator->task_external_partial != NULL
                                  ? allocator->task_external_partial
                                  : allocator->task_external_empty;

    allocator->task_external_free = span != NULL ? span->task_free : NULL;
    /* Publish queue readiness after list updates. The shared pending flag
     * keeps the unchanged safepoint fast path asleep when no work exists;
     * the allocator lock, not this hint, protects retirement/lifetime. */
    unsigned pending = allocator->task_external_retired != NULL ? 1U : 0U;
    if (atomic_load_explicit(&allocator->task_external_retire_pending, memory_order_relaxed) != pending) {
        atomic_store_explicit(&allocator->task_external_retire_pending, pending, memory_order_release);
    }
    if (pending != 0U) {
        atomic_store_explicit(&allocator->remote_free_pending, 1U, memory_order_release);
    }
}

/* No reader wait runs on an M. Failed retirement quarantines all span slots
 * until a later call can recheck the pins; they are never offered for reuse. */
static void llam_task_span_collect(llam_allocator_t *allocator,
                                   llam_alloc_chunk_t *span,
                                   llam_alloc_chunk_t **release) {
    int saved_errno = errno;

    llam_task_span_detach(allocator, span);
    if (!llam_task_try_unregister_public_slab(span->storage, span->item_count)) {
        llam_task_span_push_retired(allocator, span);
    } else {
        *span->prev_link = span->next;
        if (span->next != NULL) {
            span->next->prev_link = span->prev_link;
        }
        span->prev_link = NULL;
        span->next = NULL;
        span->pool_next = *release;
        *release = span;
    }
    errno = saved_errno;
}

static void llam_task_span_maintain(llam_allocator_t *allocator,
                                    llam_alloc_chunk_t **release) {
    /* At most one quarantined span and one empty overflow per operation.
     * Busy spans rotate, so one stalled reader cannot starve later spans. */
    if (allocator->task_external_retired != NULL) {
        llam_task_span_collect(allocator, allocator->task_external_retired, release);
    }
    if (allocator->task_external_empty_count > LLAM_TASK_SPAN_EMPTY_LIMIT) {
        llam_task_span_collect(allocator, allocator->task_external_empty_tail, release);
    }
}

static void llam_task_span_destroy(llam_alloc_chunk_t *release) {
    while (release != NULL) {
        llam_alloc_chunk_t *next = release->pool_next;
        llam_task_t *items = release->storage;

        /* Public slots and scan refs are gone. Destruction happens after the
         * allocator lock is dropped, including any task-resource callbacks. */
        for (unsigned i = 0U; i < release->item_count; ++i) {
            llam_task_release_resources(&items[i]);
            if (items[i].lock_initialized) {
                pthread_mutex_destroy(&items[i].lock);
            }
        }
        if (release->mmapped) {
            (void)munmap(items, release->bytes);
        } else {
            free(items);
        }
        free(release);
        release = next;
    }
}

/* Find a dense page multiple without increasing the 16-object span ceiling.
 * Integer cross-products avoid floating-point rounding in the policy. Small
 * heap-backed ramps stay cheap; mature spans can then release real OS pages. */
static void llam_task_span_choose_density(llam_allocator_t *allocator) {
    long page = llam_page_size();
    size_t best_bytes = 0U;
    unsigned best_count = 0U;

    if (page <= 0 || (size_t)page > SIZE_MAX / (4U * LLAM_TASK_SPAN_MAX_ITEMS)) {
        allocator->task_external_max_count = LLAM_TASK_SPAN_MAX_ITEMS;
        return;
    }
    for (unsigned pages = 1U; pages <= 4U; ++pages) {
        size_t bytes = (size_t)page * pages;
        size_t count = bytes / sizeof(llam_task_t);

        if (count > LLAM_TASK_SPAN_MAX_ITEMS) {
            count = LLAM_TASK_SPAN_MAX_ITEMS;
        }
        if (count != 0U && (best_count == 0U || count * best_bytes > (size_t)best_count * bytes)) {
            best_count = (unsigned)count;
            best_bytes = bytes;
        }
    }
    allocator->task_external_max_count = best_count != 0U ? best_count : LLAM_TASK_SPAN_MAX_ITEMS;
    allocator->task_external_dense_bytes = best_bytes;
}

/* Caller holds allocator->lock. Grow geometrically only when no existing span
 * has a reusable slot, starting at one task to preserve single-spawn footprint. */
static llam_alloc_chunk_t *llam_task_span_grow(llam_shard_t *shard) {
    llam_allocator_t *allocator = &shard->processor.allocator;
    unsigned count = allocator->task_external_next_count != 0U
                         ? allocator->task_external_next_count : 1U;
    llam_alloc_chunk_t *span = NULL;
    llam_task_t *items = NULL;
    unsigned initialized = 0U;
    int saved_errno;

    if (allocator->task_external_max_count == 0U) {
        llam_task_span_choose_density(allocator);
    }
    if (count > allocator->task_external_max_count) {
        count = allocator->task_external_max_count;
    }
    if (llam_task_span_fail_stage(1U)) {
        errno = ENOMEM;
        goto failed;
    }
    span = calloc(1U, sizeof(*span));
    if (span == NULL) {
        goto failed;
    }
    if (llam_task_span_fail_stage(2U)) {
        errno = ENOMEM;
        goto failed;
    }
    if (count == allocator->task_external_max_count && allocator->task_external_dense_bytes != 0U) {
        void *mapping = MAP_FAILED;

        if (!llam_task_span_fail_stage(4U)) {
            mapping = mmap(NULL, allocator->task_external_dense_bytes,
                           PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        }

        if (mapping != MAP_FAILED) {
            items = mapping;
            span->mmapped = true;
            span->bytes = allocator->task_external_dense_bytes;
        }
    }
    if (items == NULL) {
        /* A missing mapping capability must not turn a viable host allocation
         * into a failure. The tracked heap fallback keeps lifetime semantics. */
        items = calloc(count, sizeof(*items));
        span->bytes = (size_t)count * sizeof(*items);
    }
    if (items == NULL) {
        goto failed;
    }
    for (unsigned i = 0U; i < count; ++i) {
        int rc;

        if (i == (count > 1U ? 1U : 0U) && llam_task_span_fail_stage(3U)) {
            errno = EAGAIN;
            goto failed;
        }
        rc = pthread_mutex_init(&items[i].lock, NULL);

        if (rc != 0) {
            errno = rc;
            goto failed;
        }
        initialized += 1U;
        items[i].lock_initialized = true;
        items[i].alloc_owner_shard = shard->id;
        items[i].alloc_external_pool = true;
        items[i].alloc_span = span;
        items[i].alloc_next = span->task_free;
        span->task_free = &items[i];
    }
    span->storage = items;
    span->item_kind = LLAM_ALLOC_CHUNK_TASK;
    span->item_count = count;
    span->external_task_span = true;
    /* Global registration never acquires the allocator lock, so this order
     * cannot invert another allocator/registry lock pair. No public handle is
     * activated until checkout has incremented span occupancy. */
    llam_task_register_public_slab(items, count);
    llam_allocator_link_chunk_locked(allocator, span);
    llam_task_span_push_empty(allocator, span);
    allocator->task_external_next_count = count < allocator->task_external_max_count
                                            ? count * 2U : count;
    if (allocator->task_external_next_count > allocator->task_external_max_count) {
        allocator->task_external_next_count = allocator->task_external_max_count;
    }
    allocator->task_allocs += count;
    allocator->slab_grows += 1U;
    return span;
failed:
    saved_errno = errno != 0 ? errno : ENOMEM;
    for (unsigned i = 0U; i < initialized; ++i) {
        pthread_mutex_destroy(&items[i].lock);
    }
    if (span != NULL && span->mmapped) {
        (void)munmap(items, span->bytes);
    } else {
        free(items);
    }
    free(span);
    allocator->slab_grow_failures += 1U;
    errno = saved_errno;
    return NULL;
}

llam_task_t *llam_task_span_alloc_external(llam_shard_t *shard) {
    llam_allocator_t *allocator = &shard->processor.allocator;
    llam_alloc_chunk_t *release = NULL;
    llam_alloc_chunk_t *span;
    llam_task_t *task = NULL;
    bool reuse = true;
    int saved_errno;

    llam_allocator_lock(allocator);
    llam_task_span_maintain(allocator, &release);
    /* Pack into already occupied spans before opening an empty one. */
    span = allocator->task_external_partial != NULL
               ? allocator->task_external_partial : allocator->task_external_empty;
    if (span == NULL) {
        span = llam_task_span_grow(shard);
        reuse = false;
    }
    if (span != NULL) {
        llam_task_span_detach(allocator, span);
        task = span->task_free;
        span->task_free = task->alloc_next;
        span->task_live += 1U;
        if (span->task_free != NULL) {
            llam_task_span_push_partial(allocator, span);
        } else {
            span->pool_state = LLAM_TASK_SPAN_FULL;
        }
        if (reuse) {
            allocator->task_external_reuses += 1U;
        }
    }
    saved_errno = errno;
    llam_task_span_update_head(allocator);
    llam_allocator_unlock(allocator);
    llam_task_span_destroy(release);
    errno = saved_errno;
    return task;
}

void llam_task_span_release_external(llam_shard_t *owner, llam_task_t *task) {
    llam_allocator_t *allocator = &owner->processor.allocator;
    llam_alloc_chunk_t *span = task->alloc_span;
    llam_alloc_chunk_t *release = NULL;
    int saved_errno = errno;

    llam_allocator_lock(allocator);
    if (span == NULL || !span->external_task_span || span->task_live == 0U) {
        llam_allocator_unlock(allocator);
        llam_record_fatal_deferred(owner->runtime, EINVAL);
        return;
    }
    llam_task_span_detach(allocator, span);
    task->alloc_next = span->task_free;
    span->task_free = task;
    span->task_live -= 1U;
    if (span->task_live == 0U) {
        llam_task_span_push_empty(allocator, span);
    } else {
        llam_task_span_push_partial(allocator, span);
    }
    allocator->task_external_frees += 1U;
    llam_task_span_maintain(allocator, &release);
    llam_task_span_update_head(allocator);
    llam_allocator_unlock(allocator);
    llam_task_span_destroy(release);
    errno = saved_errno;
}

/** @brief Progress one quarantined span at an owner safepoint without spinning. */
void llam_task_span_quiescent(llam_shard_t *owner) {
    llam_allocator_t *allocator = &owner->processor.allocator;
    llam_alloc_chunk_t *release = NULL;
    int saved_errno = errno;

    if (atomic_load_explicit(&allocator->task_external_retire_pending, memory_order_acquire) == 0U) {
        return;
    }
    if (pthread_mutex_trylock(&allocator->lock) != 0) {
        atomic_store_explicit(&allocator->remote_free_pending, 1U, memory_order_release);
        return;
    }
    allocator->lock_acquires += 1U;
    llam_task_span_maintain(allocator, &release);
    llam_task_span_update_head(allocator);
    llam_allocator_unlock(allocator);
    llam_task_span_destroy(release);
    errno = saved_errno;
}
