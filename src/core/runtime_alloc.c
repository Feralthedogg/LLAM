/**
 * @file src/core/runtime_alloc.c
 * @brief Runtime allocator front-end and shared slab-growth policy.
 *
 * @details
 * This translation unit owns allocation policy that is shared by the shard-local
 * object caches:
 *  - slab storage selection (calloc vs. mmap + optional huge-page advice),
 *  - chunk bookkeeping for allocator teardown,
 *  - runtime-wide blocking-job pooling,
 *  - and shard slab growth for task, wait, timer, I/O request, and I/O buffer
 *    objects.
 *
 * Fast allocation/free paths live in the object-specific files. The functions
 * here are intentionally colder: they are called when a cache is empty, during
 * prewarm, or while shutting down.
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

/** Minimum allocation size where the experimental huge-allocation path is used. */
#define NM_HUGE_ALLOC_MIN_BYTES (2U * 1024U * 1024U)

/**
 * @brief Return the I/O buffer slab width for a shard.
 *
 * @param shard Shard whose runtime profile/config should be consulted.
 * @return Huge slab count when experimental huge allocation is requested,
 *         otherwise the normal buffer slab count.
 */
static unsigned nm_io_buffer_slab_count(const nm_shard_t *shard) {
    if (shard != NULL && shard->runtime != NULL && shard->runtime->experimental_huge_alloc_requested != 0U) {
        return NM_IO_BUFFER_HUGE_SLAB_COUNT;
    }
    return NM_IO_BUFFER_SLAB_COUNT;
}

/**
 * @brief Release storage allocated by ::nm_alloc_slab_storage.
 *
 * @param storage Base pointer returned by the slab allocator.
 * @param bytes   Allocation size, required for @c munmap.
 * @param mmapped Whether @p storage came from @c mmap instead of @c calloc.
 */
static void nm_release_alloc_storage(void *storage, size_t bytes, bool mmapped) {
    if (storage == NULL) {
        return;
    }
    if (mmapped) {
        (void)munmap(storage, bytes);
    } else {
        free(storage);
    }
}

/**
 * @brief Allocate zeroed storage for a slab.
 *
 * Large slabs may use anonymous @c mmap so Linux can back them with transparent
 * huge pages when the runtime's experimental huge-allocation option is enabled.
 * Smaller slabs use @c calloc to keep the normal path simple and portable.
 *
 * @param rt          Runtime whose allocation options are used (may be NULL).
 * @param bytes       Requested payload size.
 * @param alloc_bytes Receives the actual allocation size.
 * @param mmapped     Receives whether @c munmap is required for release.
 * @return Zero-initialized storage on success, or NULL on failure with @c errno
 *         preserved from the failing allocator where possible.
 */
static void *nm_alloc_slab_storage(nm_runtime_t *rt, size_t bytes, size_t *alloc_bytes, bool *mmapped) {
    if (alloc_bytes == NULL || mmapped == NULL) {
        errno = EINVAL;
        return NULL;
    }

    if (rt != NULL && rt->experimental_huge_alloc_requested != 0U && bytes >= NM_HUGE_ALLOC_MIN_BYTES) {
        long page_size = nm_page_size();
        size_t mapped_bytes = nm_align_up(bytes, (size_t)page_size);
        void *storage = mmap(NULL, mapped_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

        if (storage != MAP_FAILED) {
#ifdef MADV_HUGEPAGE
            // Huge-page advice is opportunistic. Failure should not make slab
            // growth fail because the mapping is still valid regular memory.
            if (madvise(storage, mapped_bytes, MADV_HUGEPAGE) == 0) {
                rt->experimental_huge_alloc_active = 1U;
            }
#endif
            *alloc_bytes = mapped_bytes;
            *mmapped = true;
            return storage;
        }
    }

    {
        void *storage = calloc(1, bytes);

        if (storage == NULL) {
            return NULL;
        }
        *alloc_bytes = bytes;
        *mmapped = false;
        return storage;
    }
}

/**
 * @brief Add a newly allocated slab/chunk to allocator teardown bookkeeping.
 *
 * The allocator records every backing allocation so ::nm_allocator_destroy can
 * release slabs in one pass. Task chunks carry extra metadata because each task
 * embeds a pthread mutex that must be destroyed before the backing storage is
 * unmapped/freed.
 *
 * @param allocator  Allocator that owns the storage.
 * @param storage    Allocation base pointer.
 * @param bytes      Allocation size.
 * @param mmapped    True if @p storage must be released with @c munmap.
 * @param item_kind  Chunk kind used for type-specific destruction.
 * @param item_count Number of typed items in the chunk when needed.
 * @return 0 on success, -1 on allocation failure.
 */
int nm_allocator_record_chunk(nm_allocator_t *allocator,
                              void *storage,
                              size_t bytes,
                              bool mmapped,
                              unsigned item_kind,
                              unsigned item_count) {
    nm_alloc_chunk_t *chunk = calloc(1, sizeof(*chunk));

    if (chunk == NULL) {
        return -1;
    }

    {
        int rc = pthread_mutex_trylock(&allocator->lock);

        if (rc == 0) {
            allocator->lock_acquires += 1U;
        } else {
            if (rc == EBUSY) {
                allocator->lock_contentions += 1U;
            }
            pthread_mutex_lock(&allocator->lock);
            allocator->lock_acquires += 1U;
        }
    }
    chunk->storage = storage;
    chunk->bytes = bytes;
    chunk->item_kind = item_kind;
    chunk->item_count = item_count;
    chunk->mmapped = mmapped;
    chunk->next = allocator->chunks;
    allocator->chunks = chunk;
    pthread_mutex_unlock(&allocator->lock);
    return 0;
}

/**
 * @brief Lock a shard allocator and record the acquisition metric.
 *
 * @param allocator Allocator to lock.
 */
void nm_allocator_lock(nm_allocator_t *allocator) {
    pthread_mutex_lock(&allocator->lock);
    allocator->lock_acquires += 1U;
}

/**
 * @brief Unlock a shard allocator.
 *
 * @param allocator Allocator to unlock.
 */
void nm_allocator_unlock(nm_allocator_t *allocator) {
    pthread_mutex_unlock(&allocator->lock);
}

/**
 * @brief Initialize an allocator embedded in a shard.
 *
 * @param allocator Allocator storage to initialize.
 */
void nm_allocator_init(nm_allocator_t *allocator) {
    memset(allocator, 0, sizeof(*allocator));
    (void)pthread_mutex_init(&allocator->lock, NULL);
    atomic_store(&allocator->remote_free_pending, 0U);
    atomic_store(&allocator->task_remote_free, NULL);
    atomic_store(&allocator->wait_remote_free, NULL);
    atomic_store(&allocator->timer_remote_free, NULL);
    atomic_store(&allocator->io_req_remote_free, NULL);
    atomic_store(&allocator->io_buffer_remote_free, NULL);
    atomic_store(&allocator->local_epoch, 0U);
}

/**
 * @brief Destroy a shard allocator and release every recorded slab.
 *
 * @param allocator Allocator to tear down.
 *
 * @note Task slabs require per-task mutex destruction before the backing memory
 *       is released. Other slab kinds contain only plain storage owned by the
 *       runtime allocator.
 */
void nm_allocator_destroy(nm_allocator_t *allocator) {
    nm_alloc_chunk_t *chunk = allocator->chunks;

    while (chunk != NULL) {
        nm_alloc_chunk_t *next = chunk->next;
        if (chunk->item_kind == NM_ALLOC_CHUNK_TASK && chunk->storage != NULL) {
            nm_task_t *tasks = chunk->storage;

            // Task locks are initialized when the slab is grown; they must be
            // destroyed even for task objects that never reached user code.
            for (unsigned i = 0U; i < chunk->item_count; ++i) {
                if (tasks[i].lock_initialized) {
                    pthread_mutex_destroy(&tasks[i].lock);
                    tasks[i].lock_initialized = false;
                }
            }
        }
        nm_release_alloc_storage(chunk->storage, chunk->bytes, chunk->mmapped);
        free(chunk);
        chunk = next;
    }

    pthread_mutex_destroy(&allocator->lock);
}

/**
 * @brief Grow the runtime-wide blocking-job free list.
 *
 * Blocking jobs are shared by all shards because they represent handoff to the
 * blocking worker pool rather than ownership by a specific scheduler shard.
 *
 * @param rt Runtime whose blocking-job pool should grow.
 * @return 0 on success, -1 on allocation failure.
 */
static int nm_block_job_grow_pool(nm_runtime_t *rt) {
    size_t alloc_bytes = 0U;
    bool mmapped = false;
    nm_block_job_t *items = nm_alloc_slab_storage(rt, NM_BLOCK_JOB_SLAB_COUNT * sizeof(*items), &alloc_bytes, &mmapped);
    nm_alloc_chunk_t *chunk;
    nm_block_job_t *head;
    nm_block_job_t *tail;
    nm_block_job_t *old_head;
    unsigned i;

    if (items == NULL) {
        return -1;
    }

    chunk = calloc(1, sizeof(*chunk));
    if (chunk == NULL) {
        nm_release_alloc_storage(items, alloc_bytes, mmapped);
        return -1;
    }

    chunk->storage = items;
    chunk->bytes = alloc_bytes;
    chunk->mmapped = mmapped;
    pthread_mutex_lock(&rt->block_lock);
    chunk->next = rt->block_job_chunks;
    rt->block_job_chunks = chunk;
    pthread_mutex_unlock(&rt->block_lock);

    // Build the new slab as a local list, then publish the whole batch with one
    // CAS so racing producers can continue using the global free list.
    head = NULL;
    tail = &items[0];
    for (i = 0; i < NM_BLOCK_JOB_SLAB_COUNT; ++i) {
        items[i].next = head;
        head = &items[i];
    }

    do {
        old_head = atomic_load_explicit(&rt->block_job_free, memory_order_acquire);
        tail->next = old_head;
    } while (!atomic_compare_exchange_weak_explicit(&rt->block_job_free,
                                                    &old_head,
                                                    head,
                                                    memory_order_acq_rel,
                                                    memory_order_acquire));

    return 0;
}

/**
 * @brief Allocate a runtime-wide blocking-job object.
 *
 * @param rt Runtime that owns the blocking-job pool.
 * @return Cleared job object on success, or NULL on failure.
 *
 * @note This path is lock-free after the pool has free entries. It grows the
 *       pool lazily when contention drains the free list.
 */
nm_block_job_t *nm_block_job_alloc(nm_runtime_t *rt) {
    nm_block_job_t *job;
    nm_block_job_t *next;

    if (rt == NULL) {
        errno = EINVAL;
        return NULL;
    }

    for (;;) {
        job = atomic_load_explicit(&rt->block_job_free, memory_order_acquire);
        if (job == NULL) {
            if (nm_block_job_grow_pool(rt) != 0) {
                return NULL;
            }
            continue;
        }
        next = job->next;
        if (atomic_compare_exchange_weak_explicit(&rt->block_job_free,
                                                  &job,
                                                  next,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            memset(job, 0, sizeof(*job));
            return job;
        }
    }
}

/**
 * @brief Return a blocking-job object to the runtime-wide free list.
 *
 * @param rt  Runtime that owns @p job.
 * @param job Job object to recycle.
 */
void nm_block_job_release(nm_runtime_t *rt, nm_block_job_t *job) {
    nm_block_job_t *head;

    if (rt == NULL || job == NULL) {
        return;
    }

    do {
        head = atomic_load_explicit(&rt->block_job_free, memory_order_acquire);
        job->next = head;
    } while (!atomic_compare_exchange_weak_explicit(&rt->block_job_free,
                                                    &head,
                                                    job,
                                                    memory_order_acq_rel,
                                                    memory_order_acquire));
}

/**
 * @brief Grow a shard's task-object slab.
 *
 * @param shard Shard whose task free list should receive the new objects.
 * @return 0 on success, -1 on allocation or mutex-initialization failure.
 */
int nm_allocator_grow_task_slab(nm_shard_t *shard) {
    size_t alloc_bytes = 0U;
    bool mmapped = false;
    nm_task_t *items = nm_alloc_slab_storage(shard->runtime,
                                             NM_TASK_SLAB_COUNT * sizeof(*items),
                                             &alloc_bytes,
                                             &mmapped);
    unsigned i;

    if (items == NULL) {
        nm_allocator_lock(&shard->allocator);
        shard->allocator.slab_grow_failures += 1U;
        nm_allocator_unlock(&shard->allocator);
        return -1;
    }
    // Task locks are part of the object contract, so initialize every item
    // before exposing the slab to the allocator free list.
    for (i = 0; i < NM_TASK_SLAB_COUNT; ++i) {
        int rc = pthread_mutex_init(&items[i].lock, NULL);

        if (rc != 0) {
            while (i > 0U) {
                i -= 1U;
                pthread_mutex_destroy(&items[i].lock);
            }
            nm_release_alloc_storage(items, alloc_bytes, mmapped);
            nm_allocator_lock(&shard->allocator);
            shard->allocator.slab_grow_failures += 1U;
            nm_allocator_unlock(&shard->allocator);
            errno = rc;
            return -1;
        }
        items[i].lock_initialized = true;
    }
    if (nm_allocator_record_chunk(&shard->allocator,
                                  items,
                                  alloc_bytes,
                                  mmapped,
                                  NM_ALLOC_CHUNK_TASK,
                                  NM_TASK_SLAB_COUNT) != 0) {
        for (i = 0; i < NM_TASK_SLAB_COUNT; ++i) {
            pthread_mutex_destroy(&items[i].lock);
            items[i].lock_initialized = false;
        }
        nm_release_alloc_storage(items, alloc_bytes, mmapped);
        nm_allocator_lock(&shard->allocator);
        shard->allocator.slab_grow_failures += 1U;
        nm_allocator_unlock(&shard->allocator);
        return -1;
    }

    // Publish all items only after bookkeeping succeeds; otherwise teardown
    // would not know how to release this backing allocation.
    nm_allocator_lock(&shard->allocator);
    shard->allocator.slab_grows += 1U;
    for (i = 0; i < NM_TASK_SLAB_COUNT; ++i) {
        items[i].alloc_owner_shard = shard->id;
        items[i].alloc_next = shard->allocator.task_free;
        shard->allocator.task_free = &items[i];
        shard->allocator.task_allocs += 1U;
    }
    nm_allocator_unlock(&shard->allocator);
    return 0;
}

/**
 * @brief Prewarm task allocators across all active shards.
 *
 * The prewarm target is controlled by @c LLAM_TASK_CACHE_PREWARM and capped to
 * avoid accidentally reserving unbounded task memory during process startup.
 *
 * @param rt Runtime whose shard task caches should be prefilled.
 */
void nm_runtime_prewarm_task_allocators(nm_runtime_t *rt) {
    const char *value;
    unsigned target = 128U;
    unsigned shard_id;
    unsigned slabs;

    if (rt == NULL || rt->shards == NULL || rt->active_shards == 0U) {
        return;
    }
    value = nm_env_get("LLAM_TASK_CACHE_PREWARM");
    if (value != NULL && value[0] != '\0') {
        char *end = NULL;
        unsigned long parsed = strtoul(value, &end, 10);

        if (end != value) {
            if (parsed > 4096UL) {
                parsed = 4096UL;
            }
            target = (unsigned)parsed;
        }
    }
    if (target == 0U) {
        return;
    }
    slabs = (target + NM_TASK_SLAB_COUNT - 1U) / NM_TASK_SLAB_COUNT;
    // Prewarm evenly per shard so first-use latency does not concentrate on
    // shard zero during spawn-heavy startup benchmarks.
    for (shard_id = 0U; shard_id < rt->active_shards; ++shard_id) {
        unsigned i;

        for (i = 0U; i < slabs; ++i) {
            if (nm_allocator_grow_task_slab(&rt->shards[shard_id]) != 0) {
                return;
            }
        }
    }
}

/**
 * @brief Grow a shard's wait-node slab.
 *
 * @param shard Shard whose wait-node cache should grow.
 * @return 0 on success, -1 on allocation failure.
 */
int nm_allocator_grow_wait_slab(nm_shard_t *shard) {
    size_t alloc_bytes = 0U;
    bool mmapped = false;
    nm_wait_node_t *items = nm_alloc_slab_storage(shard->runtime,
                                                  NM_WAIT_NODE_SLAB_COUNT * sizeof(*items),
                                                  &alloc_bytes,
                                                  &mmapped);
    unsigned i;

    if (items == NULL) {
        nm_allocator_lock(&shard->allocator);
        shard->allocator.slab_grow_failures += 1U;
        nm_allocator_unlock(&shard->allocator);
        return -1;
    }
    if (nm_allocator_record_chunk(&shard->allocator, items, alloc_bytes, mmapped, NM_ALLOC_CHUNK_GENERIC, 0U) != 0) {
        nm_release_alloc_storage(items, alloc_bytes, mmapped);
        nm_allocator_lock(&shard->allocator);
        shard->allocator.slab_grow_failures += 1U;
        nm_allocator_unlock(&shard->allocator);
        return -1;
    }

    nm_allocator_lock(&shard->allocator);
    shard->allocator.slab_grows += 1U;
    for (i = 0; i < NM_WAIT_NODE_SLAB_COUNT; ++i) {
        items[i].owner_shard = shard->id;
        items[i].alloc_next = shard->allocator.wait_free;
        shard->allocator.wait_free = &items[i];
        shard->allocator.wait_allocs += 1U;
    }
    nm_allocator_unlock(&shard->allocator);
    return 0;
}

/**
 * @brief Grow a shard's timer-node slab.
 *
 * @param shard Shard whose timer-node cache should grow.
 * @return 0 on success, -1 on allocation failure.
 */
int nm_allocator_grow_timer_slab(nm_shard_t *shard) {
    size_t alloc_bytes = 0U;
    bool mmapped = false;
    nm_timer_node_t *items = nm_alloc_slab_storage(shard->runtime,
                                                   NM_TIMER_NODE_SLAB_COUNT * sizeof(*items),
                                                   &alloc_bytes,
                                                   &mmapped);
    unsigned i;

    if (items == NULL) {
        nm_allocator_lock(&shard->allocator);
        shard->allocator.slab_grow_failures += 1U;
        nm_allocator_unlock(&shard->allocator);
        return -1;
    }
    if (nm_allocator_record_chunk(&shard->allocator, items, alloc_bytes, mmapped, NM_ALLOC_CHUNK_GENERIC, 0U) != 0) {
        nm_release_alloc_storage(items, alloc_bytes, mmapped);
        nm_allocator_lock(&shard->allocator);
        shard->allocator.slab_grow_failures += 1U;
        nm_allocator_unlock(&shard->allocator);
        return -1;
    }

    nm_allocator_lock(&shard->allocator);
    shard->allocator.slab_grows += 1U;
    for (i = 0; i < NM_TIMER_NODE_SLAB_COUNT; ++i) {
        items[i].owner_shard = shard->id;
        items[i].alloc_next = shard->allocator.timer_free;
        shard->allocator.timer_free = &items[i];
        shard->allocator.timer_allocs += 1U;
    }
    nm_allocator_unlock(&shard->allocator);
    return 0;
}

/**
 * @brief Grow a shard's I/O request slab.
 *
 * @param shard Shard whose I/O request cache should grow.
 * @return 0 on success, -1 on allocation failure.
 */
int nm_allocator_grow_io_req_slab(nm_shard_t *shard) {
    size_t alloc_bytes = 0U;
    bool mmapped = false;
    nm_io_req_t *items = nm_alloc_slab_storage(shard->runtime,
                                               NM_IO_REQ_SLAB_COUNT * sizeof(*items),
                                               &alloc_bytes,
                                               &mmapped);
    unsigned i;

    if (items == NULL) {
        nm_allocator_lock(&shard->allocator);
        shard->allocator.slab_grow_failures += 1U;
        nm_allocator_unlock(&shard->allocator);
        return -1;
    }
    if (nm_allocator_record_chunk(&shard->allocator, items, alloc_bytes, mmapped, NM_ALLOC_CHUNK_GENERIC, 0U) != 0) {
        nm_release_alloc_storage(items, alloc_bytes, mmapped);
        nm_allocator_lock(&shard->allocator);
        shard->allocator.slab_grow_failures += 1U;
        nm_allocator_unlock(&shard->allocator);
        return -1;
    }

    nm_allocator_lock(&shard->allocator);
    shard->allocator.slab_grows += 1U;
    for (i = 0; i < NM_IO_REQ_SLAB_COUNT; ++i) {
        // Requests track both logical owner and allocation owner because live
        // I/O can migrate/rehome while the backing object still returns to the
        // original slab owner.
        items[i].owner_shard = shard->id;
        items[i].alloc_owner_shard = shard->id;
        items[i].attached_node_index = UINT_MAX;
        atomic_init(&items[i].inflight_owner_shard, UINT_MAX);
        items[i].alloc_next = shard->allocator.io_req_free;
        shard->allocator.io_req_free = &items[i];
        shard->allocator.io_req_allocs += 1U;
    }
    nm_allocator_unlock(&shard->allocator);
    return 0;
}

/**
 * @brief Grow a shard's I/O buffer wrapper slab.
 *
 * @param shard Shard whose I/O buffer cache should grow.
 * @return 0 on success, -1 on allocation failure.
 */
int nm_allocator_grow_io_buffer_slab(nm_shard_t *shard) {
    size_t alloc_bytes = 0U;
    bool mmapped = false;
    unsigned slab_count = nm_io_buffer_slab_count(shard);
    nm_io_buffer_t *items = nm_alloc_slab_storage(shard->runtime,
                                                  (size_t)slab_count * sizeof(*items),
                                                  &alloc_bytes,
                                                  &mmapped);
    unsigned i;

    if (items == NULL) {
        nm_allocator_lock(&shard->allocator);
        shard->allocator.slab_grow_failures += 1U;
        nm_allocator_unlock(&shard->allocator);
        return -1;
    }
    if (nm_allocator_record_chunk(&shard->allocator, items, alloc_bytes, mmapped, NM_ALLOC_CHUNK_GENERIC, 0U) != 0) {
        nm_release_alloc_storage(items, alloc_bytes, mmapped);
        nm_allocator_lock(&shard->allocator);
        shard->allocator.slab_grow_failures += 1U;
        nm_allocator_unlock(&shard->allocator);
        return -1;
    }

    nm_allocator_lock(&shard->allocator);
    shard->allocator.slab_grows += 1U;
    for (i = 0; i < slab_count; ++i) {
        // Buffer objects start with inline storage. Larger request payloads may
        // attach external storage later and release it before cache return.
        items[i].alloc_owner_shard = shard->id;
        items[i].data = items[i].inline_data;
        items[i].capacity = NM_IO_BUFFER_INLINE_BYTES;
        items[i].alloc_next = shard->allocator.io_buffer_free;
        shard->allocator.io_buffer_free = &items[i];
        shard->allocator.io_buffer_allocs += 1U;
    }
    nm_allocator_unlock(&shard->allocator);
    return 0;
}
