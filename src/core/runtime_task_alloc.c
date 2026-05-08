/**
 * @file src/core/runtime_task_alloc.c
 * @brief Task object allocation, reuse, and task-id assignment.
 *
 * @details
 * Task objects are owned by the shard allocator that created them. The owner can
 * allocate and recycle tasks from its local free list without locking. Foreign
 * shards return tasks through the owner's atomic remote-free list, which is
 * drained at quiescent scheduler points.
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

/**
 * @brief Reset a recycled task object while preserving embedded permanent state.
 *
 * @details
 * The pthread mutex is initialized once per slab object and embedded wait/I/O
 * objects are cleared at acquisition time.  Avoiding a whole-struct memset keeps
 * spawn-heavy workloads from repeatedly touching cold embedded request storage.
 *
 * @param task     Task object taken from a shard free list.
 * @param shard_id Allocation-owner shard id to publish after reset.
 */
static void llam_task_reset_reused(llam_task_t *task, unsigned shard_id) {
    bool lock_initialized;

    if (task == NULL) {
        return;
    }

    lock_initialized = task->lock_initialized;
    memset(task, 0, offsetof(llam_task_t, ctx));
    memset((char *)task + offsetof(llam_task_t, stack_mapping),
           0,
           offsetof(llam_task_t, lock) - offsetof(llam_task_t, stack_mapping));
    memset((char *)task + offsetof(llam_task_t, task_listed),
           0,
           offsetof(llam_task_t, embedded_wait_node) - offsetof(llam_task_t, task_listed));
    memset((char *)task + offsetof(llam_task_t, active_wait_node),
           0,
           offsetof(llam_task_t, embedded_io_req) - offsetof(llam_task_t, active_wait_node));
    memset((char *)task + offsetof(llam_task_t, active_io_req),
           0,
           sizeof(*task) - offsetof(llam_task_t, active_io_req));
    task->lock_initialized = lock_initialized;
    task->alloc_owner_shard = shard_id;
}

/**
 * @brief Allocate a task object from a shard-local cache.
 *
 * @param shard Shard that should own the returned task object.
 * @return Reinitialized task object on success, or NULL on allocation failure.
 *
 * @note The embedded pthread mutex is preserved across reuse. Task slabs
 *       initialize the mutex once and allocator teardown destroys it.
 */
llam_task_t *llam_task_alloc(llam_shard_t *shard) {
    llam_task_t *task = NULL;

    if (shard == NULL) {
        errno = EINVAL;
        return NULL;
    }

    for (;;) {
        if (g_llam_tls_shard == shard) {
            // Same-shard allocation is the hot path; no other thread mutates the
            // owner-local free list without taking the slow path lock.
            task = shard->allocator.task_free;
            if (task != NULL) {
                shard->allocator.task_free = task->alloc_next;
                shard->allocator.task_reuses += 1U;
                llam_task_reset_reused(task, shard->id);
                return task;
            }
        } else {
            // Cold external/bootstrap allocation. Serialize access to another
            // shard's local free list rather than using the remote-free queue.
            llam_allocator_lock(&shard->allocator);
            task = shard->allocator.task_free;
            if (task != NULL) {
                shard->allocator.task_free = task->alloc_next;
                shard->allocator.task_reuses += 1U;
                llam_allocator_unlock(&shard->allocator);
                llam_task_reset_reused(task, shard->id);
                return task;
            }
            llam_allocator_unlock(&shard->allocator);
        }
        // Empty cache: grow one slab and retry so callers see normal allocation
        // semantics instead of needing to understand slab management.
        if (llam_allocator_grow_task_slab(shard) != 0) {
            return NULL;
        }
    }
}

/**
 * @brief Return a task object to its allocation-owner shard.
 *
 * @param task Task object previously returned by ::llam_task_alloc.
 */
void llam_task_allocator_free(llam_task_t *task) {
    llam_runtime_t *rt = &g_llam_runtime;
    llam_shard_t *owner;
    llam_task_t *head;

    if (task == NULL || task->alloc_owner_shard >= rt->active_shards) {
        return;
    }

    owner = &rt->shards[task->alloc_owner_shard];
    task->alloc_next = NULL;
    if (g_llam_tls_shard != NULL && g_llam_tls_shard->id == owner->id) {
        // Owner shard can recycle directly; this is the common task cleanup path
        // after joined tasks are reclaimed by their home scheduler.
        task->alloc_next = owner->allocator.task_free;
        owner->allocator.task_free = task;
        owner->allocator.task_frees += 1U;
        return;
    }

    // Foreign frees publish onto a lock-free remote list and set one pending
    // flag. The owner drains the list in llam_allocator_quiescent().
    do {
        head = atomic_load(&owner->allocator.task_remote_free);
        task->alloc_next = head;
    } while (!atomic_compare_exchange_weak(&owner->allocator.task_remote_free, &head, task));
    atomic_store_explicit(&owner->allocator.remote_free_pending, 1U, memory_order_release);
}
