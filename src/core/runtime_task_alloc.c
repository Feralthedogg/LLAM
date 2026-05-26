/**
 * @file src/core/runtime_task_alloc.c
 * @brief Task object allocation, reuse, and task-id assignment.
 *
 * @details
 * Task objects are owned by the shard allocator that created them. The owner
 * worker allocates and recycles normal tasks from its local free list without
 * locking. Host-control spawns use a separate external cache so unmanaged
 * public runtime handles never race the owner free-list hot path. Foreign
 * shards return normal tasks through the owner's atomic remote-free list, which
 * is drained at quiescent scheduler points.
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
 * @param owner_runtime Runtime that owns the task object.
 * @param shard_id      Allocation-owner shard id to publish after reset.
 */
static void llam_task_reset_reused(llam_task_t *task, llam_runtime_t *owner_runtime, unsigned shard_id) {
    bool lock_initialized;
    unsigned i;

    if (task == NULL) {
        return;
    }

    lock_initialized = task->lock_initialized;
    task->owner_runtime = owner_runtime;
    task->id = 0U;
    atomic_store_explicit(&task->active_ops, 0U, memory_order_release);
    atomic_init(&task->state, (unsigned)LLAM_TASK_STATE_NEW);
    task->wait_reason = LLAM_WAIT_NONE;
    task->flags = 0U;
    task->home_shard = 0U;
    task->live_shard = 0U;
    task->last_shard = 0U;
    task->parked_shard = 0U;
    atomic_init(&task->task_class, (unsigned)LLAM_TASK_CLASS_DEFAULT);
    atomic_init(&task->base_task_class, (unsigned)LLAM_TASK_CLASS_DEFAULT);
    memset((char *)task + offsetof(llam_task_t, deadline_ns),
           0,
           offsetof(llam_task_t, ctx) - offsetof(llam_task_t, deadline_ns));
    memset((char *)task + offsetof(llam_task_t, stack_mapping),
           0,
           offsetof(llam_task_t, lock) - offsetof(llam_task_t, stack_mapping));
    atomic_init(&task->task_listed, 0U);
    atomic_init(&task->scan_refs, 0U);
    task->all_next = NULL;
    task->all_prev = NULL;
    task->alloc_next = NULL;
    task->queue_next = NULL;
    task->queue_prev = NULL;
    task->join_waiters = NULL;
    task->join_waiter_count = 0U;
    atomic_init(&task->join_waiter_hint, 0U);
    task->join_target = NULL;
    task->wait_next = NULL;
    task->cancel_next = NULL;
    task->cancel_prev = NULL;
    llam_wait_node_reset(&task->embedded_wait_node, owner_runtime, UINT_MAX);
    for (i = 0U; i < LLAM_TASK_EMBEDDED_SELECT_NODES; ++i) {
        llam_wait_node_reset(&task->embedded_select_nodes[i], owner_runtime, UINT_MAX);
    }
    memset((char *)task + offsetof(llam_task_t, active_wait_node),
           0,
           offsetof(llam_task_t, embedded_io_req) - offsetof(llam_task_t, active_wait_node));
    llam_io_req_reset(&task->embedded_io_req, owner_runtime, UINT_MAX, UINT_MAX);
    /*
     * active_io_req is an atomic ownership boundary between I/O completion and
     * dynamic rehome. Reinitialize it explicitly instead of byte-clearing the
     * tail of the task object, which also contains other atomics.
     */
    atomic_init(&task->active_io_req, NULL);
    atomic_init(&task->active_block_job, NULL);
    task->task_locals = NULL;
    task->cancel_registered = false;
    task->enqueue_hot = 0U;
    task->last_runnable_ns = 0U;
    task->last_yield_ns = 0U;
    task->last_started_ns = 0U;
    task->last_run_ns = 0U;
    task->total_run_ns = 0U;
    task->opaque_block_started_ns = 0U;
    task->last_opaque_block_ns = 0U;
    task->max_opaque_block_ns = 0U;
    task->opaque_block_count = 0U;
    task->blocking_result = NULL;
    task->saved_errno = 0;
    task->blocking_errno = 0;
    task->wake_error_code = 0;
    atomic_init(&task->opaque_blocking_depth, 0U);
    task->opaque_uses_helper = false;
    task->opaque_uses_redirect = false;
    task->safepoint_tick = 0U;
    task->preempt_poll_tick = 0U;
    task->last_stack_used = 0U;
    task->stack_high_water = 0U;
    memset(&task->embedded_timer_node, 0, sizeof(task->embedded_timer_node));
    task->embedded_timer_node.owner_runtime = owner_runtime;
    task->active_timer = NULL;
    atomic_init(&task->preempt_requested, 0U);
    atomic_init(&task->completed, 0U);
    atomic_init(&task->reclaim_ready, 0U);
    atomic_init(&task->reclaim_claimed, 0U);
    atomic_init(&task->join_claimed, 0U);
    atomic_init(&task->detached, 0U);
    task->join_waiter_count_at_exit = 0U;
    task->forced_yield_budget = 0U;
    task->lock_initialized = lock_initialized;
    task->alloc_owner_shard = shard_id;
}

/**
 * @brief Allocate a task for an unmanaged host-control spawn.
 *
 * @details
 * Host threads can spawn into an explicit live runtime while the owner worker is
 * concurrently recycling tasks.  To keep the managed-task spawn path lock-free,
 * host-control tasks use a separate mutex-protected external cache instead of
 * touching the owner shard's normal task_free list.
 *
 * @param shard Target shard whose runtime owns the task.
 * @return Task object on success, or NULL on allocation failure.
 */
static llam_task_t *llam_task_alloc_external(llam_shard_t *shard) {
    llam_task_t *task;
    int rc;

    llam_allocator_lock(&shard->allocator);
    task = shard->allocator.task_external_free;
    if (task != NULL) {
        shard->allocator.task_external_free = task->alloc_next;
        shard->allocator.task_reuses += 1U;
    }
    llam_allocator_unlock(&shard->allocator);
    if (task != NULL) {
        return task;
    }

    task = calloc(1, sizeof(*task));
    if (task == NULL) {
        return NULL;
    }
    rc = pthread_mutex_init(&task->lock, NULL);
    if (rc != 0) {
        free(task);
        errno = rc;
        return NULL;
    }
    task->lock_initialized = true;
    task->alloc_owner_shard = shard->id;
    task->alloc_external_pool = true;
    if (llam_allocator_record_chunk(&shard->allocator,
                                    task,
                                    sizeof(*task),
                                    false,
                                    LLAM_ALLOC_CHUNK_TASK,
                                    1U) != 0) {
        pthread_mutex_destroy(&task->lock);
        free(task);
        errno = ENOMEM;
        return NULL;
    }
    llam_task_register_public_slab(task, 1U);
    llam_allocator_lock(&shard->allocator);
    shard->allocator.task_allocs += 1U;
    llam_allocator_unlock(&shard->allocator);
    return task;
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
    bool local_owner;

    if (shard == NULL) {
        errno = EINVAL;
        return NULL;
    }

    local_owner = g_llam_tls_shard == shard;
    if (!local_owner) {
        task = llam_task_alloc_external(shard);
        if (task == NULL) {
            return NULL;
        }
        llam_task_reset_reused(task, shard->runtime, shard->id);
        task->alloc_external_pool = true;
        if (llam_task_activate_public_handle(task) != 0) {
            llam_task_allocator_free(task);
            return NULL;
        }
        return task;
    }

    for (;;) {
        task = shard->allocator.task_free;
        if (task != NULL) {
            shard->allocator.task_free = task->alloc_next;
            shard->allocator.task_reuses += 1U;
            break;
        }
        // Empty cache: grow one slab and retry so callers see normal allocation
        // semantics instead of needing to understand slab management.
        if (llam_allocator_grow_task_slab(shard) != 0) {
            return NULL;
        }
    }
    llam_task_reset_reused(task, shard->runtime, shard->id);
    task->alloc_external_pool = false;
    if (llam_task_activate_public_handle(task) != 0) {
        llam_task_allocator_free(task);
        return NULL;
    }
    return task;
}

/**
 * @brief Return a task object to its allocation-owner shard.
 *
 * @param task Task object previously returned by ::llam_task_alloc.
 */
void llam_task_allocator_free(llam_task_t *task) {
    llam_runtime_t *rt = task != NULL ? task->owner_runtime : NULL;
    llam_shard_t *owner;
    llam_task_t *head;

    if (task == NULL || rt == NULL || task->alloc_owner_shard >= rt->active_shards) {
        return;
    }

    llam_task_local_clear(task);
    owner = &rt->shards[task->alloc_owner_shard];
    task->alloc_next = NULL;
    if (task->alloc_external_pool) {
        llam_allocator_lock(&owner->allocator);
        task->alloc_next = owner->allocator.task_external_free;
        owner->allocator.task_external_free = task;
        owner->allocator.task_frees += 1U;
        llam_allocator_unlock(&owner->allocator);
        return;
    }
    if (g_llam_tls_shard == owner) {
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
