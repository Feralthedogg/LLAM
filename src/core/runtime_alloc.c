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
#define LLAM_HUGE_ALLOC_MIN_BYTES (2U * 1024U * 1024U)

/**
 * @brief Return the I/O buffer slab width for a shard.
 *
 * @param shard Shard whose runtime profile/config should be consulted.
 * @return Huge slab count when experimental huge allocation is requested,
 *         otherwise the normal buffer slab count.
 */
static unsigned llam_io_buffer_slab_count(const llam_shard_t *shard) {
    if (shard != NULL && shard->runtime != NULL && shard->runtime->experimental_huge_alloc_requested != 0U) {
        return LLAM_IO_BUFFER_HUGE_SLAB_COUNT;
    }
    return LLAM_IO_BUFFER_SLAB_COUNT;
}

/**
 * @brief Release storage allocated by ::llam_alloc_slab_storage.
 *
 * @param storage Base pointer returned by the slab allocator.
 * @param bytes   Allocation size, required for @c munmap.
 * @param mmapped Whether @p storage came from @c mmap instead of @c calloc.
 */
static void llam_release_alloc_storage(void *storage, size_t bytes, bool mmapped) {
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
static void *llam_alloc_slab_storage(llam_runtime_t *rt, size_t bytes, size_t *alloc_bytes, bool *mmapped) {
    if (alloc_bytes == NULL || mmapped == NULL) {
        errno = EINVAL;
        return NULL;
    }

    if (rt != NULL && rt->experimental_huge_alloc_requested != 0U && bytes >= LLAM_HUGE_ALLOC_MIN_BYTES) {
        long page_size = llam_page_size();
        size_t mapped_bytes = 0U;
        void *storage;

        if (page_size <= 0) {
            errno = EINVAL;
            return NULL;
        }
        if (llam_align_up_checked(bytes, (size_t)page_size, &mapped_bytes) != 0) {
            return NULL;
        }
        storage = mmap(NULL, mapped_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
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
 * The allocator records every backing allocation so ::llam_allocator_destroy can
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
int llam_allocator_record_chunk(llam_allocator_t *allocator,
                              void *storage,
                              size_t bytes,
                              bool mmapped,
                              unsigned item_kind,
                              unsigned item_count) {
    llam_alloc_chunk_t *chunk = calloc(1, sizeof(*chunk));

    if (chunk == NULL) {
        return -1;
    }

    {
        int rc = pthread_mutex_trylock(&allocator->lock);
        bool contended = false;

        if (rc == 0) {
            allocator->lock_acquires += 1U;
        } else {
            if (rc == EBUSY) {
                contended = true;
            }
            pthread_mutex_lock(&allocator->lock);
            /*
             * Keep allocator telemetry under the allocator lock.  The counters
             * are diagnostic only, but concurrent host-thread slab growth can
             * otherwise race while the protected chunk list remains correct.
             */
            if (contended) {
                allocator->lock_contentions += 1U;
            }
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
void llam_allocator_lock(llam_allocator_t *allocator) {
    pthread_mutex_lock(&allocator->lock);
    allocator->lock_acquires += 1U;
}

/**
 * @brief Unlock a shard allocator.
 *
 * @param allocator Allocator to unlock.
 */
void llam_allocator_unlock(llam_allocator_t *allocator) {
    pthread_mutex_unlock(&allocator->lock);
}

/**
 * @brief Initialize an allocator embedded in a shard.
 *
 * @param allocator Allocator storage to initialize.
 */
int llam_allocator_init(llam_allocator_t *allocator) {
    int rc;

    memset(allocator, 0, sizeof(*allocator));
    rc = pthread_mutex_init(&allocator->lock, NULL);
    if (rc != 0) {
        // Allocator locks protect all shard-local slabs.  Startup must fail
        // rather than publishing a shard with an unusable allocator lock.
        errno = rc;
        return -1;
    }
    allocator->lock_initialized = true;
    atomic_store(&allocator->remote_free_pending, 0U);
    atomic_store(&allocator->task_remote_free, NULL);
    atomic_store(&allocator->wait_remote_free, NULL);
    atomic_store(&allocator->timer_remote_free, NULL);
    atomic_store(&allocator->io_req_remote_free, NULL);
    atomic_store(&allocator->io_buffer_remote_free, NULL);
    atomic_store(&allocator->local_epoch, 0U);
    return 0;
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
void llam_allocator_destroy(llam_allocator_t *allocator) {
    llam_alloc_chunk_t *chunk = allocator->chunks;

    while (chunk != NULL) {
        llam_alloc_chunk_t *next = chunk->next;
        bool keep_chunk = false;

        if (chunk->item_kind == LLAM_ALLOC_CHUNK_TASK && chunk->storage != NULL) {
            llam_task_t *tasks = chunk->storage;

            if (llam_task_unregister_public_slab(tasks, chunk->item_count) != 0) {
                /*
                 * The task public-op counter is saturated/corrupted.  Freeing
                 * the slab could turn an outstanding forged/stale handle path
                 * into UAF, so teardown leaks this chunk instead of hanging or
                 * releasing unsafe storage.
                 */
                keep_chunk = true;
            }
            // Task locks are initialized when the slab is grown; they must be
            // destroyed even for task objects that never reached user code.
            if (!keep_chunk) {
                for (unsigned i = 0U; i < chunk->item_count; ++i) {
                    if (tasks[i].lock_initialized) {
                        pthread_mutex_destroy(&tasks[i].lock);
                        tasks[i].lock_initialized = false;
                    }
                }
            }
        }
        if (keep_chunk) {
            chunk = next;
            continue;
        }
        llam_release_alloc_storage(chunk->storage, chunk->bytes, chunk->mmapped);
        free(chunk);
        chunk = next;
    }

    if (allocator->lock_initialized) {
        pthread_mutex_destroy(&allocator->lock);
        allocator->lock_initialized = false;
    }
}

/**
 * @brief Initialize a fresh blocking-job object before first publication.
 *
 * Blocking jobs carry an atomic state observed by producer, worker, timeout,
 * and cancellation paths. Fresh slab objects use @c atomic_init before any
 * thread can observe them.
 *
 * @param job Job object to initialize.
 */
static void llam_block_job_init(llam_block_job_t *job) {
    if (job == NULL) {
        return;
    }

    job->fn = NULL;
    job->arg = NULL;
    atomic_init(&job->result, NULL);
    atomic_init(&job->error_code, 0);
    job->task = NULL;
    job->wait_node = NULL;
    atomic_init(&job->state, LLAM_BLOCK_JOB_ABORTED);
    job->holds_task_ref = false;
    job->next = NULL;
}

/**
 * @brief Reset a blocking-job object for reuse.
 *
 * @details
 * Reused jobs already contain a live atomic state object. Use an atomic store
 * instead of @c atomic_init so recycling remains valid after previous
 * cross-thread state-machine access.
 *
 * @param job Job object to reset.
 */
static void llam_block_job_reset(llam_block_job_t *job) {
    if (job == NULL) {
        return;
    }

    job->fn = NULL;
    job->arg = NULL;
    atomic_store_explicit(&job->result, NULL, memory_order_release);
    atomic_store_explicit(&job->error_code, 0, memory_order_release);
    job->task = NULL;
    job->wait_node = NULL;
    atomic_store_explicit(&job->state, LLAM_BLOCK_JOB_ABORTED, memory_order_release);
    job->holds_task_ref = false;
    job->next = NULL;
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
static int llam_block_job_grow_pool(llam_runtime_t *rt) {
    size_t alloc_bytes = 0U;
    bool mmapped = false;
    llam_block_job_t *items = llam_alloc_slab_storage(rt, LLAM_BLOCK_JOB_SLAB_COUNT * sizeof(*items), &alloc_bytes, &mmapped);
    llam_alloc_chunk_t *chunk;
    llam_block_job_t *head;
    llam_block_job_t *tail;
    unsigned i;

    if (items == NULL) {
        return -1;
    }

    chunk = calloc(1, sizeof(*chunk));
    if (chunk == NULL) {
        llam_release_alloc_storage(items, alloc_bytes, mmapped);
        return -1;
    }

    chunk->storage = items;
    chunk->bytes = alloc_bytes;
    chunk->mmapped = mmapped;
    // Build the new slab as a local list, then publish the whole batch under
    // block_lock. This free list supports both pop and push, so a lock-free
    // Treiber stack would need ABA protection once jobs can be recycled.
    head = NULL;
    tail = &items[0];
    for (i = 0; i < LLAM_BLOCK_JOB_SLAB_COUNT; ++i) {
        llam_block_job_init(&items[i]);
        items[i].next = head;
        head = &items[i];
    }

    pthread_mutex_lock(&rt->block_lock);
    chunk->next = rt->block_job_chunks;
    rt->block_job_chunks = chunk;
    tail->next = atomic_load_explicit(&rt->block_job_free, memory_order_relaxed);
    atomic_store_explicit(&rt->block_job_free, head, memory_order_release);
    pthread_mutex_unlock(&rt->block_lock);

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
llam_block_job_t *llam_block_job_alloc(llam_runtime_t *rt) {
    llam_block_job_t *job;

    if (rt == NULL) {
        errno = EINVAL;
        return NULL;
    }

    for (;;) {
        pthread_mutex_lock(&rt->block_lock);
        job = atomic_load_explicit(&rt->block_job_free, memory_order_acquire);
        if (job != NULL) {
            atomic_store_explicit(&rt->block_job_free, job->next, memory_order_release);
            pthread_mutex_unlock(&rt->block_lock);
            job->next = NULL;
            llam_block_job_reset(job);
            return job;
        }
        pthread_mutex_unlock(&rt->block_lock);

        if (llam_block_job_grow_pool(rt) != 0) {
            return NULL;
        }
    }
}

/**
 * @brief Return a blocking-job object to the runtime-wide free list.
 *
 * @param rt  Runtime that owns @p job.
 * @param job Job object to recycle.
 */
void llam_block_job_release(llam_runtime_t *rt, llam_block_job_t *job) {
    llam_block_job_t *head;
    llam_task_t *task;
    bool release_task_ref;

    if (rt == NULL || job == NULL) {
        return;
    }

    task = job->task;
    release_task_ref = job->holds_task_ref;
    job->holds_task_ref = false;
    job->task = NULL;
    job->wait_node = NULL;
    if (release_task_ref && task != NULL) {
        (void)llam_task_scan_ref_release(rt, task);
    }

    pthread_mutex_lock(&rt->block_lock);
    head = atomic_load_explicit(&rt->block_job_free, memory_order_relaxed);
    job->next = head;
    atomic_store_explicit(&rt->block_job_free, job, memory_order_release);
    pthread_mutex_unlock(&rt->block_lock);
}

/**
 * @brief Grow a shard's task-object slab.
 *
 * @param shard Shard whose task free list should receive the new objects.
 * @return 0 on success, -1 on allocation or mutex-initialization failure.
 */
int llam_allocator_grow_task_slab(llam_shard_t *shard) {
    size_t alloc_bytes = 0U;
    bool mmapped = false;
    llam_task_t *items = llam_alloc_slab_storage(shard->runtime,
                                             LLAM_TASK_SLAB_COUNT * sizeof(*items),
                                             &alloc_bytes,
                                             &mmapped);
    unsigned i;

    if (items == NULL) {
        llam_allocator_lock(&shard->allocator);
        shard->allocator.slab_grow_failures += 1U;
        llam_allocator_unlock(&shard->allocator);
        return -1;
    }
    // Task locks are part of the object contract, so initialize every item
    // before exposing the slab to the allocator free list.
    for (i = 0; i < LLAM_TASK_SLAB_COUNT; ++i) {
        int rc = pthread_mutex_init(&items[i].lock, NULL);

        if (rc != 0) {
            while (i > 0U) {
                i -= 1U;
                pthread_mutex_destroy(&items[i].lock);
            }
            llam_release_alloc_storage(items, alloc_bytes, mmapped);
            llam_allocator_lock(&shard->allocator);
            shard->allocator.slab_grow_failures += 1U;
            llam_allocator_unlock(&shard->allocator);
            errno = rc;
            return -1;
        }
        items[i].lock_initialized = true;
    }
    if (llam_allocator_record_chunk(&shard->allocator,
                                  items,
                                  alloc_bytes,
                                  mmapped,
                                  LLAM_ALLOC_CHUNK_TASK,
                                  LLAM_TASK_SLAB_COUNT) != 0) {
        for (i = 0; i < LLAM_TASK_SLAB_COUNT; ++i) {
            pthread_mutex_destroy(&items[i].lock);
            items[i].lock_initialized = false;
        }
        llam_release_alloc_storage(items, alloc_bytes, mmapped);
        llam_allocator_lock(&shard->allocator);
        shard->allocator.slab_grow_failures += 1U;
        llam_allocator_unlock(&shard->allocator);
        return -1;
    }

    llam_task_register_public_slab(items, LLAM_TASK_SLAB_COUNT);

    // Publish all items only after bookkeeping succeeds; otherwise teardown
    // would not know how to release this backing allocation.
    llam_allocator_lock(&shard->allocator);
    shard->allocator.slab_grows += 1U;
    for (i = 0; i < LLAM_TASK_SLAB_COUNT; ++i) {
        items[i].alloc_owner_shard = shard->id;
        items[i].alloc_external_pool = false;
        items[i].alloc_next = shard->allocator.task_free;
        shard->allocator.task_free = &items[i];
        shard->allocator.task_allocs += 1U;
    }
    llam_allocator_unlock(&shard->allocator);
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
void llam_runtime_prewarm_task_allocators(llam_runtime_t *rt) {
    const char *value;
    unsigned target = 128U;
    unsigned shard_id;
    unsigned slabs;

    if (rt == NULL || rt->shards == NULL || rt->active_shards == 0U) {
        return;
    }
    value = llam_env_get("LLAM_TASK_CACHE_PREWARM");
    if (value != NULL && value[0] != '\0') {
        char *end = NULL;
        unsigned long parsed;

        if (!llam_ascii_is_space((unsigned char)value[0]) && value[0] != '-' && value[0] != '+') {
            errno = 0;
            parsed = strtoul(value, &end, 10);
            if (errno == 0 && end != value && *end == '\0') {
                if (parsed > 4096UL) {
                    parsed = 4096UL;
                }
                target = (unsigned)parsed;
            }
        }
    }
    if (target == 0U) {
        return;
    }
    slabs = (target + LLAM_TASK_SLAB_COUNT - 1U) / LLAM_TASK_SLAB_COUNT;
    // Prewarm evenly per shard so first-use latency does not concentrate on
    // shard zero during spawn-heavy startup benchmarks.
    for (shard_id = 0U; shard_id < rt->active_shards; ++shard_id) {
        unsigned i;

        for (i = 0U; i < slabs; ++i) {
            if (llam_allocator_grow_task_slab(&rt->shards[shard_id]) != 0) {
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
int llam_allocator_grow_wait_slab(llam_shard_t *shard) {
    size_t alloc_bytes = 0U;
    bool mmapped = false;
    llam_wait_node_t *items = llam_alloc_slab_storage(shard->runtime,
                                                  LLAM_WAIT_NODE_SLAB_COUNT * sizeof(*items),
                                                  &alloc_bytes,
                                                  &mmapped);
    unsigned i;

    if (items == NULL) {
        llam_allocator_lock(&shard->allocator);
        shard->allocator.slab_grow_failures += 1U;
        llam_allocator_unlock(&shard->allocator);
        return -1;
    }
    if (llam_allocator_record_chunk(&shard->allocator, items, alloc_bytes, mmapped, LLAM_ALLOC_CHUNK_GENERIC, 0U) != 0) {
        llam_release_alloc_storage(items, alloc_bytes, mmapped);
        llam_allocator_lock(&shard->allocator);
        shard->allocator.slab_grow_failures += 1U;
        llam_allocator_unlock(&shard->allocator);
        return -1;
    }

    llam_allocator_lock(&shard->allocator);
    shard->allocator.slab_grows += 1U;
    for (i = 0; i < LLAM_WAIT_NODE_SLAB_COUNT; ++i) {
        llam_wait_node_reset(&items[i], shard->runtime, shard->id);
        items[i].alloc_next = shard->allocator.wait_free;
        shard->allocator.wait_free = &items[i];
        shard->allocator.wait_allocs += 1U;
    }
    llam_allocator_unlock(&shard->allocator);
    return 0;
}

/**
 * @brief Grow a shard's timer-node slab.
 *
 * @param shard Shard whose timer-node cache should grow.
 * @return 0 on success, -1 on allocation failure.
 */
int llam_allocator_grow_timer_slab(llam_shard_t *shard) {
    size_t alloc_bytes = 0U;
    bool mmapped = false;
    llam_timer_node_t *items = llam_alloc_slab_storage(shard->runtime,
                                                   LLAM_TIMER_NODE_SLAB_COUNT * sizeof(*items),
                                                   &alloc_bytes,
                                                   &mmapped);
    unsigned i;

    if (items == NULL) {
        llam_allocator_lock(&shard->allocator);
        shard->allocator.slab_grow_failures += 1U;
        llam_allocator_unlock(&shard->allocator);
        return -1;
    }
    if (llam_allocator_record_chunk(&shard->allocator, items, alloc_bytes, mmapped, LLAM_ALLOC_CHUNK_GENERIC, 0U) != 0) {
        llam_release_alloc_storage(items, alloc_bytes, mmapped);
        llam_allocator_lock(&shard->allocator);
        shard->allocator.slab_grow_failures += 1U;
        llam_allocator_unlock(&shard->allocator);
        return -1;
    }

    llam_allocator_lock(&shard->allocator);
    shard->allocator.slab_grows += 1U;
    for (i = 0; i < LLAM_TIMER_NODE_SLAB_COUNT; ++i) {
        items[i].owner_runtime = shard->runtime;
        items[i].owner_shard = shard->id;
        items[i].alloc_next = shard->allocator.timer_free;
        shard->allocator.timer_free = &items[i];
        shard->allocator.timer_allocs += 1U;
    }
    llam_allocator_unlock(&shard->allocator);
    return 0;
}

/**
 * @brief Grow a shard's I/O request slab.
 *
 * @param shard Shard whose I/O request cache should grow.
 * @return 0 on success, -1 on allocation failure.
 */
int llam_allocator_grow_io_req_slab(llam_shard_t *shard) {
    size_t alloc_bytes = 0U;
    bool mmapped = false;
    llam_io_req_t *items = llam_alloc_slab_storage(shard->runtime,
                                               LLAM_IO_REQ_SLAB_COUNT * sizeof(*items),
                                               &alloc_bytes,
                                               &mmapped);
    unsigned i;

    if (items == NULL) {
        llam_allocator_lock(&shard->allocator);
        shard->allocator.slab_grow_failures += 1U;
        llam_allocator_unlock(&shard->allocator);
        return -1;
    }
    if (llam_allocator_record_chunk(&shard->allocator, items, alloc_bytes, mmapped, LLAM_ALLOC_CHUNK_GENERIC, 0U) != 0) {
        llam_release_alloc_storage(items, alloc_bytes, mmapped);
        llam_allocator_lock(&shard->allocator);
        shard->allocator.slab_grow_failures += 1U;
        llam_allocator_unlock(&shard->allocator);
        return -1;
    }

    llam_allocator_lock(&shard->allocator);
    shard->allocator.slab_grows += 1U;
    for (i = 0; i < LLAM_IO_REQ_SLAB_COUNT; ++i) {
        // Requests track both logical owner and allocation owner because live
        // I/O can migrate/rehome while the backing object still returns to the
        // original slab owner.
        llam_io_req_reset(&items[i], shard->runtime, shard->id, shard->id);
        items[i].alloc_next = shard->allocator.io_req_free;
        shard->allocator.io_req_free = &items[i];
        shard->allocator.io_req_allocs += 1U;
    }
    llam_allocator_unlock(&shard->allocator);
    return 0;
}

/**
 * @brief Grow a shard's I/O buffer wrapper slab.
 *
 * @param shard Shard whose I/O buffer cache should grow.
 * @return 0 on success, -1 on allocation failure.
 */
int llam_allocator_grow_io_buffer_slab(llam_shard_t *shard) {
    size_t alloc_bytes = 0U;
    bool mmapped = false;
    unsigned slab_count = llam_io_buffer_slab_count(shard);
    llam_io_buffer_t *items = llam_alloc_slab_storage(shard->runtime,
                                                  (size_t)slab_count * sizeof(*items),
                                                  &alloc_bytes,
                                                  &mmapped);
    unsigned i;

    if (items == NULL) {
        llam_allocator_lock(&shard->allocator);
        shard->allocator.slab_grow_failures += 1U;
        llam_allocator_unlock(&shard->allocator);
        return -1;
    }
    if (llam_allocator_record_chunk(&shard->allocator, items, alloc_bytes, mmapped, LLAM_ALLOC_CHUNK_GENERIC, 0U) != 0) {
        llam_release_alloc_storage(items, alloc_bytes, mmapped);
        llam_allocator_lock(&shard->allocator);
        shard->allocator.slab_grow_failures += 1U;
        llam_allocator_unlock(&shard->allocator);
        return -1;
    }

    llam_allocator_lock(&shard->allocator);
    shard->allocator.slab_grows += 1U;
    for (i = 0; i < slab_count; ++i) {
        // Buffer objects start with inline storage. Larger request payloads may
        // attach external storage later and release it before cache return.
        items[i].owner_runtime = shard->runtime;
        items[i].alloc_owner_shard = shard->id;
        items[i].data = items[i].inline_data;
        items[i].capacity = LLAM_IO_BUFFER_INLINE_BYTES;
        items[i].alloc_next = shard->allocator.io_buffer_free;
        shard->allocator.io_buffer_free = &items[i];
        shard->allocator.io_buffer_allocs += 1U;
    }
    llam_allocator_unlock(&shard->allocator);
    return 0;
}
