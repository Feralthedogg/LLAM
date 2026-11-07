/**
 * @file src/core/task/task_alloc.c
 * @brief Task object allocation, reuse, and task-id assignment.
 *
 * @details
 * Task objects are owned by the processor allocator that created them. The
 * owner worker allocates and recycles normal tasks from its local free list without
 * locking. Host-control spawns use a separate external cache so unmanaged
 * public runtime handles never race the owner free-list hot path. Foreign
 * processors return normal tasks through the owner's atomic remote-free list, which
 * is drained at quiescent scheduler points.
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
 * See LICENSE.
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

    if (task == NULL) {
        return;
    }

    lock_initialized = task->lock_initialized;
    task->owner_runtime = owner_runtime;
    task->admission_counted = false;
    task->id = 0U;
    atomic_store_explicit(&task->active_ops, 0U, memory_order_release);
    atomic_init(&task->state, (unsigned)LLAM_TASK_STATE_NEW);
    task->wait_reason = LLAM_WAIT_NONE;
    atomic_init(&task->run_generation, 0U);
    task->flags = 0U;
    atomic_init(&task->home_shard, 0U);
    task->live_shard = 0U;
    atomic_init(&task->last_shard, 0U);
    task->parked_shard = 0U;
    atomic_init(&task->active_timer_heap_shard, UINT_MAX);
    atomic_init(&task->task_class, (unsigned)LLAM_TASK_CLASS_DEFAULT);
    atomic_init(&task->base_task_class, (unsigned)LLAM_TASK_CLASS_DEFAULT);
    memset((char *)task + offsetof(llam_task_t, deadline_ns),
           0,
           offsetof(llam_task_t, ctx) - offsetof(llam_task_t, deadline_ns));
    memset((char *)task + offsetof(llam_task_t, stack_mapping),
           0,
           offsetof(llam_task_t, lock) - offsetof(llam_task_t, stack_mapping));
    atomic_init(&task->managed_dispatch_owner, NULL);
#if defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT) && \
    ((defined(__linux__) && defined(__x86_64__)) || (LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64) || LLAM_ASYNC_CONTEXT_SHARED)
    task->async_preempt_task = NULL;
#endif
    atomic_init(&task->task_listed, 0U);
    atomic_init(&task->scan_refs, 0U);
#if LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
    task->windows_async_retire_next = NULL;
    task->windows_async_retire_queued = false;
    atomic_init(&task->windows_async_execution_owed, 0U);
    task->windows_async_debt_owner = UINT_MAX;
    task->windows_async_debt_run = 0U;
    task->windows_async_saved_last_error = 0U;
    task->windows_async_internal_yield_depth = 0U;
    task->windows_async_finalizer_owner = UINT_MAX;
    task->windows_async_finalizer_next = NULL;
    task->windows_async_finalizer_queued = false;
#endif
    task->all_next = NULL;
    task->all_prev = NULL;
    task->alloc_next = NULL;
    task->queue_next = NULL;
    task->queue_prev = NULL;
    task->join_waiters = NULL;
    task->join_waiter_count = 0U;
    atomic_init(&task->join_waiter_hint, 0U);
    atomic_init(&task->join_target, NULL);
    task->wait_next = NULL;
    task->group_cancel_token = NULL;
    task->operation_cancel_token = NULL;
    task->operation_deadline_ns = 0U;
    task->request_deadline_ns = 0U;
    task->io_scope_active = false;
    task->scope_completion = NULL;
    task->group_retirement_pending = false;
    task->local_executor = NULL;
    task->local_executor_retired = false;
    memset(&task->application_result, 0, sizeof(task->application_result));
    for (unsigned i = 0U; i < LLAM_CANCEL_SOURCE_COUNT; ++i) {
        task->cancel_links[i].task = task;
        task->cancel_links[i].next = NULL;
        task->cancel_links[i].prev = NULL;
        task->cancel_links[i].wait_generation = 0U;
        task->cancel_links[i].registered = false;
        atomic_init(&task->cancel_links[i].delivering, false);
    }
    llam_wait_node_reset(&task->embedded_wait_node, owner_runtime, UINT_MAX);
    /*
     * Wait ownership fields are atomics because runtime stop/cancel and timeout
     * paths can sample them from other OS threads. Initialize them explicitly;
     * byte-clearing atomic objects would hide the ownership contract.
     */
    atomic_init(&task->active_wait_node, NULL);
    atomic_init(&task->active_wait_queue, NULL);
    atomic_init(&task->active_wait_queue_lock, NULL);
    atomic_init(&task->active_select_state, NULL);
    atomic_init(&task->active_wait_lifetime_ops, NULL);
    atomic_init(&task->wait_resolver_state,
                UINT_MAX - (UINT_MAX >> 1U));
    atomic_init(&task->wait_generation, 0U);
    llam_io_req_reset(&task->embedded_io_req, owner_runtime, UINT_MAX, UINT_MAX);
    /*
     * active_io_req is an atomic ownership boundary between I/O completion and
     * dynamic rehome. Reinitialize it explicitly instead of byte-clearing the
     * tail of the task object, which also contains other atomics.
     */
    atomic_init(&task->active_io_req, NULL);
    atomic_init(&task->active_io_generation, 0U);
    atomic_init(&task->active_block_job, NULL);
    task->task_locals = NULL;
    task->handoff_sample_current = false;
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
    atomic_init(&task->wake_error_code, 0);
    atomic_init(&task->opaque_blocking_depth, 0U);
    task->opaque_uses_helper = false;
    task->opaque_uses_redirect = false;
    task->safepoint_tick = 0U;
    task->preempt_poll_tick = 0U;
    atomic_init(&task->last_stack_used, 0U);
    atomic_init(&task->stack_high_water, 0U);
    memset(&task->embedded_timer_node, 0, sizeof(task->embedded_timer_node));
    task->embedded_timer_node.owner_runtime = owner_runtime;
    task->active_timer = NULL;
    atomic_init(&task->preempt_requested, 0U);
    atomic_init(&task->cleanup_wait_depth, 0U);
    atomic_init(&task->borrowed_drain_depth, 0U);
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
    return llam_task_span_alloc_external(shard);
}

/**
 * @brief Allocate a task object from a processor-local cache.
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

    local_owner = llam_processor_tls_owns_resources(&shard->processor);
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
        task = shard->processor.allocator.task_free;
        if (task != NULL) {
            shard->processor.allocator.task_free = task->alloc_next;
            shard->processor.allocator.task_reuses += 1U;
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

    if (task->admission_counted) {
        task->admission_counted = false;
        /* This counter owns capacity only; task publication/reclamation has its
         * own release/acquire protocol. No storage is published through it. */
        atomic_fetch_sub_explicit(&rt->admitted_tasks, 1U, memory_order_relaxed);
    }
    llam_task_local_clear(task);
    llam_managed_task_release(task);
    llam_scoped_task_retire(task);
    owner = &rt->shards[task->alloc_owner_shard];
    task->alloc_next = NULL;
    if (task->alloc_external_pool) {
        llam_task_span_release_external(owner, task);
        return;
    }
    if (llam_processor_tls_owns_resources(&owner->processor)) {
        task->alloc_next = owner->processor.allocator.task_free;
        owner->processor.allocator.task_free = task;
        owner->processor.allocator.task_frees += 1U;
        return;
    }

    // Foreign frees publish onto a lock-free remote list and set one pending
    // flag. The owner drains the list in llam_allocator_quiescent().
    do {
        head = atomic_load(&owner->processor.allocator.task_remote_free);
        task->alloc_next = head;
    } while (!atomic_compare_exchange_weak(&owner->processor.allocator.task_remote_free, &head, task));
    atomic_store_explicit(&owner->processor.allocator.remote_free_pending, 1U, memory_order_release);
}
