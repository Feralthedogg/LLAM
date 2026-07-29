/**
 * @file src/core/task/stack_cache_trim.c
 * @brief Bounded live stack-cache trim and memory-pressure policy.
 *
 * @details
 * Trim requests serialize through one runtime-owned mutex. Each cache owner is
 * visited independently: a bounded batch is detached while its list lock is
 * held, then platform VM release happens after that lock is released. Exact
 * byte authority is removed only after release succeeds.
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

/** Maximum mappings detached while one cache-owner lock is held. */
#define LLAM_STACK_CACHE_TRIM_BATCH 32U

typedef enum llam_stack_cache_trim_reason {
    LLAM_STACK_CACHE_TRIM_MANUAL = 0,
    LLAM_STACK_CACHE_TRIM_PRESSURE = 1,
    LLAM_STACK_CACHE_TRIM_HIGH_WATER = 2,
    LLAM_STACK_CACHE_TRIM_IDLE = 3,
} llam_stack_cache_trim_reason_t;

typedef struct llam_stack_cache_trim_batch {
    llam_stack_cache_entry_t *entries[LLAM_STACK_CACHE_TRIM_BATCH];
    uint64_t mapping_bytes;
    unsigned count;
} llam_stack_cache_trim_batch_t;

/**
 * @brief Test whether one retained mapping may be selected by this trim.
 */
static bool llam_stack_cache_trim_entry_eligible(
    const llam_stack_cache_entry_t *entry,
    bool idle_only,
    uint64_t idle_cutoff_ns) {
    if (entry == NULL) {
        return false;
    }
    return !idle_only ||
           (entry->last_return_ns != 0U &&
            entry->last_return_ns <= idle_cutoff_ns);
}

/**
 * @brief Detach the oldest eligible suffix from one size-class list.
 *
 * The list is newest-first in normal operation. A first pass counts eligible
 * entries and a second pass removes only the last bounded subset, preserving
 * hotter entries without requiring tail pointers or allocation under lock.
 */
static unsigned llam_stack_cache_trim_detach_list_locked(
    llam_runtime_t *rt,
    llam_stack_cache_entry_t **head,
    unsigned *list_count,
    size_t nominal_mapping_size,
    uint64_t target_bytes,
    bool idle_only,
    uint64_t idle_cutoff_ns,
    llam_stack_cache_trim_batch_t *batch) {
    llam_stack_cache_entry_t *entry;
    llam_stack_cache_entry_t **link;
    uint64_t cached_bytes;
    uint64_t bytes_needed;
    uint64_t mappings_needed;
    unsigned eligible_count = 0U;
    unsigned available;
    unsigned take_count;
    unsigned skip_count;
    unsigned detached = 0U;

    if (rt == NULL || head == NULL || list_count == NULL ||
        nominal_mapping_size == 0U || batch == NULL ||
        batch->count >= LLAM_STACK_CACHE_TRIM_BATCH) {
        return 0U;
    }
    cached_bytes = atomic_load_explicit(&rt->stack_cache_cached_bytes,
                                        memory_order_acquire);
    if (cached_bytes < batch->mapping_bytes) {
        llam_record_fatal_deferred(rt, EOVERFLOW);
        return 0U;
    }
    cached_bytes -= batch->mapping_bytes;
    if (cached_bytes <= target_bytes) {
        return 0U;
    }
    for (entry = *head; entry != NULL; entry = entry->next) {
        if (llam_stack_cache_trim_entry_eligible(entry,
                                                 idle_only,
                                                 idle_cutoff_ns)) {
            eligible_count += 1U;
        }
    }
    if (eligible_count == 0U) {
        return 0U;
    }

    bytes_needed = cached_bytes - target_bytes;
    mappings_needed =
        bytes_needed / (uint64_t)nominal_mapping_size;
    if (bytes_needed % (uint64_t)nominal_mapping_size != 0U) {
        mappings_needed += 1U;
    }
    available = LLAM_STACK_CACHE_TRIM_BATCH - batch->count;
    take_count = eligible_count < available ? eligible_count : available;
    if ((uint64_t)take_count > mappings_needed) {
        take_count = (unsigned)mappings_needed;
    }
    if (take_count == 0U) {
        return 0U;
    }
    skip_count = eligible_count - take_count;

    link = head;
    while (*link != NULL && detached < take_count) {
        entry = *link;
        if (!llam_stack_cache_trim_entry_eligible(entry,
                                                  idle_only,
                                                  idle_cutoff_ns)) {
            link = &entry->next;
            continue;
        }
        if (skip_count != 0U) {
            skip_count -= 1U;
            link = &entry->next;
            continue;
        }
        if ((uint64_t)entry->mapping_size >
            UINT64_MAX - batch->mapping_bytes) {
            llam_record_fatal_deferred(rt, EOVERFLOW);
            break;
        }

        *link = entry->next;
        entry->next = NULL;
        if (*list_count == 0U) {
            llam_record_fatal_deferred(rt, EOVERFLOW);
        } else {
            *list_count -= 1U;
        }
        if (entry->owner_runtime != rt) {
            llam_record_fatal_deferred(rt, EINVAL);
        }
        batch->entries[batch->count++] = entry;
        batch->mapping_bytes += (uint64_t)entry->mapping_size;
        detached += 1U;
        if (cached_bytes < (uint64_t)entry->mapping_size) {
            llam_record_fatal_deferred(rt, EOVERFLOW);
            break;
        }
        cached_bytes -= (uint64_t)entry->mapping_size;
        if (cached_bytes <= target_bytes) {
            break;
        }
    }
    return detached;
}

/**
 * @brief Detach one bounded batch from a cache owner under its one list lock.
 */
static unsigned llam_stack_cache_trim_detach_owner_locked(
    llam_runtime_t *rt,
    llam_stack_cache_entry_t **default_head,
    unsigned *default_count,
    llam_stack_cache_entry_t **large_head,
    unsigned *large_count,
    llam_stack_cache_entry_t **huge_head,
    unsigned *huge_count,
    uint64_t target_bytes,
    bool idle_only,
    uint64_t idle_cutoff_ns,
    llam_stack_cache_trim_batch_t *batch) {
    size_t page_size = (size_t)llam_page_size();
    unsigned detached = 0U;

    detached += llam_stack_cache_trim_detach_list_locked(
        rt,
        huge_head,
        huge_count,
        llam_stack_bytes(LLAM_STACK_CLASS_HUGE) + page_size,
        target_bytes,
        idle_only,
        idle_cutoff_ns,
        batch);
    detached += llam_stack_cache_trim_detach_list_locked(
        rt,
        large_head,
        large_count,
        llam_stack_bytes(LLAM_STACK_CLASS_LARGE) + page_size,
        target_bytes,
        idle_only,
        idle_cutoff_ns,
        batch);
    detached += llam_stack_cache_trim_detach_list_locked(
        rt,
        default_head,
        default_count,
        llam_stack_bytes(LLAM_STACK_CLASS_DEFAULT) + page_size,
        target_bytes,
        idle_only,
        idle_cutoff_ns,
        batch);
    return detached;
}

/** Retain a detached mapping whose platform release did not complete. */
static void llam_stack_cache_trim_defer_release(
    llam_runtime_t *rt,
    llam_stack_cache_entry_t *entry) {
    if (rt == NULL || entry == NULL) {
        return;
    }
    entry->next = rt->stack_cache_release_pending;
    rt->stack_cache_release_pending = entry;
}

/** Destroy metadata only after the mapping no longer exists. */
static void llam_stack_cache_trim_destroy_entry(
    llam_stack_cache_entry_t *entry) {
    if (entry == NULL) {
        return;
    }
    if (entry->heap_allocated) {
        free(entry);
    } else {
        memset(entry, 0, sizeof(*entry));
    }
}

bool llam_stack_cache_release_detached_entry(
    llam_runtime_t *rt,
    llam_stack_cache_entry_t *entry) {
    int lock_rc;
    int saved_errno;
    bool released = false;

    if (rt == NULL || entry == NULL ||
        !rt->stack_cache_trim_lock_initialized) {
        errno = EINVAL;
        return false;
    }
    lock_rc = pthread_mutex_lock(&rt->stack_cache_trim_lock);
    if (lock_rc != 0) {
        errno = lock_rc;
        return false;
    }
    if (entry->owner_runtime != rt ||
        entry->mapping == NULL ||
        entry->mapping_size == 0U) {
        llam_record_fatal_deferred(rt, EINVAL);
        saved_errno = EINVAL;
        llam_stack_cache_trim_defer_release(rt, entry);
    } else if (llam_stack_mapping_release(rt,
                                          entry->mapping,
                                          entry->mapping_size)) {
        llam_stack_cache_account_remove(rt,
                                        entry->mapping_size,
                                        entry->committed_bytes);
        llam_stack_cache_trim_destroy_entry(entry);
        saved_errno = 0;
        released = true;
    } else {
        saved_errno = errno != 0 ? errno : EIO;
        llam_stack_cache_trim_defer_release(rt, entry);
    }
    pthread_mutex_unlock(&rt->stack_cache_trim_lock);
    if (!released) {
        errno = saved_errno;
    }
    return released;
}

/**
 * @brief Release one detached batch without holding a cache-owner lock.
 */
static int llam_stack_cache_trim_release_batch(
    llam_runtime_t *rt,
    llam_stack_cache_trim_batch_t *batch,
    uint64_t *released_bytes) {
    int result = 0;
    int result_errno = 0;

    for (unsigned index = 0U; index < batch->count; ++index) {
        llam_stack_cache_entry_t *entry = batch->entries[index];
        uint64_t mapping_bytes =
            entry != NULL ? (uint64_t)entry->mapping_size : 0U;
        bool mapping_released = false;

        if (entry == NULL || entry->owner_runtime != rt ||
            entry->mapping == NULL || entry->mapping_size == 0U) {
            if (entry != NULL) {
                llam_record_fatal_deferred(rt, EINVAL);
                llam_stack_cache_trim_defer_release(rt, entry);
            }
            if (result == 0) {
                result = -1;
                result_errno = EINVAL;
            }
        } else if (llam_stack_mapping_release(rt,
                                              entry->mapping,
                                              entry->mapping_size)) {
            llam_stack_cache_account_remove(rt,
                                            entry->mapping_size,
                                            entry->committed_bytes);
            mapping_released = true;
            if (*released_bytes > UINT64_MAX - mapping_bytes) {
                if (result == 0) {
                    result = -1;
                    result_errno = EOVERFLOW;
                }
            } else {
                *released_bytes += mapping_bytes;
            }
        } else {
            int release_errno = errno != 0 ? errno : EIO;

            llam_stack_cache_trim_defer_release(rt, entry);
            if (result == 0) {
                result = -1;
                result_errno = release_errno;
            }
        }
        if (mapping_released) {
            llam_stack_cache_trim_destroy_entry(entry);
        }
        batch->entries[index] = NULL;
    }
    batch->count = 0U;
    batch->mapping_bytes = 0U;
    if (result != 0) {
        errno = result_errno;
    }
    return result;
}

/**
 * @brief Retry one bounded batch of previously failed platform releases.
 *
 * The caller holds the runtime trim mutex. Entries remain charged to the
 * runtime-wide byte authority while detached and pending.
 */
static int llam_stack_cache_trim_retry_pending(
    llam_runtime_t *rt,
    uint64_t *released_bytes) {
    llam_stack_cache_trim_batch_t batch;

    memset(&batch, 0, sizeof(batch));
    while (rt->stack_cache_release_pending != NULL &&
           batch.count < LLAM_STACK_CACHE_TRIM_BATCH) {
        llam_stack_cache_entry_t *entry =
            rt->stack_cache_release_pending;

        rt->stack_cache_release_pending = entry->next;
        entry->next = NULL;
        batch.entries[batch.count++] = entry;
        if ((uint64_t)entry->mapping_size >
            UINT64_MAX - batch.mapping_bytes) {
            llam_record_fatal_deferred(rt, EOVERFLOW);
        } else {
            batch.mapping_bytes += (uint64_t)entry->mapping_size;
        }
    }
    return llam_stack_cache_trim_release_batch(rt,
                                               &batch,
                                               released_bytes);
}

/**
 * @brief Detach and release one batch from one shard cache.
 */
static int llam_stack_cache_trim_shard(
    llam_runtime_t *rt,
    llam_shard_t *shard,
    uint64_t target_bytes,
    bool idle_only,
    uint64_t idle_cutoff_ns,
    uint64_t *released_bytes,
    bool *made_progress) {
    llam_stack_cache_trim_batch_t batch;
    int lock_rc;

    memset(&batch, 0, sizeof(batch));
    lock_rc = pthread_mutex_lock(&shard->stack_cache_lock);
    if (lock_rc != 0) {
        errno = lock_rc;
        return -1;
    }
    (void)llam_stack_cache_trim_detach_owner_locked(
        rt,
        &shard->stack_cache_default,
        &shard->stack_cache_default_count,
        &shard->stack_cache_large,
        &shard->stack_cache_large_count,
        &shard->stack_cache_huge,
        &shard->stack_cache_huge_count,
        target_bytes,
        idle_only,
        idle_cutoff_ns,
        &batch);
    pthread_mutex_unlock(&shard->stack_cache_lock);

    if (batch.count != 0U) {
        *made_progress = true;
    }
    return llam_stack_cache_trim_release_batch(rt,
                                               &batch,
                                               released_bytes);
}

/**
 * @brief Detach and release one batch from the runtime fallback cache.
 */
static int llam_stack_cache_trim_runtime(
    llam_runtime_t *rt,
    uint64_t target_bytes,
    bool idle_only,
    uint64_t idle_cutoff_ns,
    uint64_t *released_bytes,
    bool *made_progress) {
    llam_stack_cache_trim_batch_t batch;
    int lock_rc;

    memset(&batch, 0, sizeof(batch));
    lock_rc = pthread_mutex_lock(&rt->stack_cache_lock);
    if (lock_rc != 0) {
        errno = lock_rc;
        return -1;
    }
    (void)llam_stack_cache_trim_detach_owner_locked(
        rt,
        &rt->stack_cache_default,
        &rt->stack_cache_default_count,
        &rt->stack_cache_large,
        &rt->stack_cache_large_count,
        &rt->stack_cache_huge,
        &rt->stack_cache_huge_count,
        target_bytes,
        idle_only,
        idle_cutoff_ns,
        &batch);
    pthread_mutex_unlock(&rt->stack_cache_lock);

    if (batch.count != 0U) {
        *made_progress = true;
    }
    return llam_stack_cache_trim_release_batch(rt,
                                               &batch,
                                               released_bytes);
}

/**
 * @brief Execute a serialized trim request.
 *
 * The caller must hold @c stack_cache_trim_lock. The round limit is derived
 * from the initial retained mapping count, so active producers cannot keep a
 * host thread inside a best-effort trim forever.
 */
int llam_stack_cache_trim_internal(llam_runtime_t *rt,
                                   uint64_t target_bytes,
                                   bool idle_only,
                                   uint64_t idle_cutoff_ns,
                                   uint64_t *released_bytes) {
    uint64_t initial_mappings;
    uint64_t round_limit;

    if (released_bytes != NULL) {
        *released_bytes = 0U;
    }
    if (rt == NULL || released_bytes == NULL ||
        !rt->stack_cache_lock_initialized ||
        !rt->stack_cache_trim_lock_initialized) {
        errno = EINVAL;
        return -1;
    }
    initial_mappings =
        atomic_load_explicit(&rt->stack_cache_cached_mappings,
                             memory_order_acquire);
    round_limit =
        initial_mappings / LLAM_STACK_CACHE_TRIM_BATCH + 2U;

    for (uint64_t round = 0U; round < round_limit; ++round) {
        bool made_progress = false;

        if (rt->stack_cache_release_pending != NULL) {
            if (llam_stack_cache_trim_retry_pending(
                    rt,
                    released_bytes) != 0) {
                return -1;
            }
            if (rt->stack_cache_release_pending != NULL) {
                continue;
            }
        }
        if (atomic_load_explicit(&rt->stack_cache_cached_bytes,
                                 memory_order_acquire) <= target_bytes) {
            break;
        }
        for (unsigned shard_id = 0U;
             shard_id < rt->active_shards;
             ++shard_id) {
            if (llam_stack_cache_trim_shard(
                    rt,
                    &rt->shards[shard_id],
                    target_bytes,
                    idle_only,
                    idle_cutoff_ns,
                    released_bytes,
                    &made_progress) != 0) {
                return -1;
            }
            if (atomic_load_explicit(&rt->stack_cache_cached_bytes,
                                     memory_order_acquire) <= target_bytes) {
                break;
            }
        }
        if (atomic_load_explicit(&rt->stack_cache_cached_bytes,
                                 memory_order_acquire) > target_bytes &&
            llam_stack_cache_trim_runtime(rt,
                                          target_bytes,
                                          idle_only,
                                          idle_cutoff_ns,
                                          released_bytes,
                                          &made_progress) != 0) {
            return -1;
        }
        if (!made_progress) {
            break;
        }
    }
    return 0;
}

/**
 * @brief Acquire runtime trim authority and record one request.
 */
static int llam_stack_cache_trim_request(
    llam_runtime_t *rt,
    uint64_t target_bytes,
    bool idle_only,
    uint64_t idle_cutoff_ns,
    llam_stack_cache_trim_reason_t reason,
    uint64_t *released_bytes) {
    int result;
    int result_errno = 0;
    int lock_rc;

    if (released_bytes != NULL) {
        *released_bytes = 0U;
    }
    if (rt == NULL || released_bytes == NULL ||
        !rt->stack_cache_trim_lock_initialized) {
        errno = EINVAL;
        return -1;
    }
    lock_rc = pthread_mutex_lock(&rt->stack_cache_trim_lock);
    if (lock_rc != 0) {
        errno = lock_rc;
        return -1;
    }
    if (reason == LLAM_STACK_CACHE_TRIM_HIGH_WATER &&
        atomic_load_explicit(&rt->stack_cache_cached_bytes,
                             memory_order_acquire) <=
            rt->resource_plan.stack_cache_high_watermark_bytes) {
        pthread_mutex_unlock(&rt->stack_cache_trim_lock);
        return 0;
    }
    atomic_fetch_add_explicit(&rt->stack_cache_trim_requests,
                              1U,
                              memory_order_relaxed);
    result = llam_stack_cache_trim_internal(rt,
                                            target_bytes,
                                            idle_only,
                                            idle_cutoff_ns,
                                            released_bytes);
    if (result != 0) {
        result_errno = errno;
    }
    pthread_mutex_unlock(&rt->stack_cache_trim_lock);
    if (result != 0) {
        errno = result_errno;
    }
    return result;
}

/**
 * @brief Apply automatic high-water and idle maintenance after publication.
 */
void llam_stack_cache_maintain(llam_runtime_t *rt, uint64_t now_ns) {
    uint64_t released_bytes;
    uint64_t high;
    uint64_t idle_ns;
    uint_fast64_t last_scan;

    if (rt == NULL || !rt->stack_cache_trim_lock_initialized) {
        return;
    }
    high = rt->resource_plan.stack_cache_high_watermark_bytes;
    if (atomic_load_explicit(&rt->stack_cache_cached_bytes,
                             memory_order_acquire) > high) {
        (void)llam_stack_cache_trim_request(
            rt,
            rt->resource_plan.stack_cache_low_watermark_bytes,
            false,
            0U,
            LLAM_STACK_CACHE_TRIM_HIGH_WATER,
            &released_bytes);
    }

    idle_ns = rt->resource_plan.stack_cache_idle_ns;
    if (idle_ns == 0U || now_ns <= idle_ns) {
        return;
    }
    last_scan =
        atomic_load_explicit(&rt->stack_cache_last_idle_scan_ns,
                             memory_order_acquire);
    for (;;) {
        if (now_ns < (uint64_t)last_scan ||
            now_ns - (uint64_t)last_scan < idle_ns) {
            return;
        }
        if (atomic_compare_exchange_weak_explicit(
                &rt->stack_cache_last_idle_scan_ns,
                &last_scan,
                now_ns,
                memory_order_acq_rel,
                memory_order_acquire)) {
            break;
        }
    }
    (void)llam_stack_cache_trim_request(
        rt,
        0U,
        true,
        now_ns - idle_ns,
        LLAM_STACK_CACHE_TRIM_IDLE,
        &released_bytes);
}

/**
 * @brief Shared public trim entry point with active-operation pinning.
 */
static int llam_runtime_stack_cache_trim_public(
    llam_runtime_t *runtime,
    uint64_t target_bytes,
    llam_stack_cache_trim_reason_t reason,
    uint64_t *released_bytes) {
    llam_runtime_t *pinned_runtime = NULL;
    int result;
    int result_errno = 0;

    if (released_bytes != NULL) {
        *released_bytes = 0U;
    }
    if (runtime == NULL || released_bytes == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (llam_runtime_begin_public_op(runtime, &pinned_runtime) != 0) {
        return -1;
    }
    if (!atomic_load_explicit(&pinned_runtime->initialized,
                              memory_order_acquire) ||
        target_bytes >
            pinned_runtime->resource_plan.stack_cache_budget_bytes) {
        llam_runtime_end_public_op(pinned_runtime);
        errno = EINVAL;
        return -1;
    }
    result = llam_stack_cache_trim_request(
        pinned_runtime,
        target_bytes,
        false,
        0U,
        reason,
        released_bytes);
    if (result != 0) {
        result_errno = errno;
    }
    llam_runtime_end_public_op(pinned_runtime);
    if (result != 0) {
        errno = result_errno;
    }
    return result;
}

int llam_runtime_stack_cache_trim_ex(llam_runtime_t *runtime,
                                     uint64_t target_bytes,
                                     uint64_t *released_bytes) {
    return llam_runtime_stack_cache_trim_public(
        runtime,
        target_bytes,
        LLAM_STACK_CACHE_TRIM_MANUAL,
        released_bytes);
}

int llam_runtime_notify_memory_pressure(llam_runtime_t *runtime) {
    uint64_t released_bytes;

    return llam_runtime_stack_cache_trim_public(
        runtime,
        0U,
        LLAM_STACK_CACHE_TRIM_PRESSURE,
        &released_bytes);
}
