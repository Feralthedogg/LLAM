/**
 * @file src/core/runtime_spawn.c
 * @brief Task creation, spawn option normalization, and initial runnable queue insertion.
 *
 * @details
 * This translation unit implements the internal spawn path:
 *  - ::llam_spawn
 *
 * High-level behavior:
 *  - Validates runtime state and the task entry point.
 *  - Normalizes optional spawn settings into task class, stack class, flags,
 *    deadline, and cancellation-token state.
 *  - Allocates a task object and stack from the selected shard allocator.
 *  - Publishes the task in the runtime task list and marks it live.
 *  - Inserts the task into the correct runnable lane: local normal queue,
 *    remote inject queue, or opaque-blocking redirect queue.
 *
 * Scheduling notes:
 *  - Spawned tasks start on a chosen home shard and may later migrate unless
 *    pinned.
 *  - Local spawns avoid an immediate wake unless fanout reaches the configured
 *    wake interval.
 *  - Dynamic shards are brought online lazily when spawn targets a parked
 *    shard.
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
 * @brief Decide whether a local spawn burst should wake other scheduler shards.
 *
 * Local spawns normally avoid cross-worker wakeups because the current worker
 * can continue draining its own queue.  This helper reintroduces fanout only
 * after the target normal queue reaches a configured depth multiple.
 *
 * @param rt Runtime instance.
 * @param target Shard receiving the newly spawned task.
 *
 * @return true if all shards should be woken for fanout, false otherwise.
 *
 * @note Internal helper.  The caller does not need to hold @p target->lock;
 *       ::llam_norm_queue_depth provides the queue-depth snapshot used here.
 */
static bool llam_spawn_should_wake_fanout(llam_runtime_t *rt, llam_shard_t *target) {
    unsigned interval;
    unsigned depth;

    if (rt == NULL || target == NULL || rt->active_shards <= 1U) {
        return false;
    }
    interval = rt->spawn_fanout_wake_interval;
    if (interval == 0U) {
        return false;
    }
    depth = llam_norm_queue_depth(target);
    return depth >= interval && (depth % interval) == 0U;
}

/** @brief Return true when @p task_class is a supported public task class. */
static bool llam_public_task_class_valid(uint32_t task_class) {
    return task_class == (uint32_t)LLAM_TASK_CLASS_LATENCY ||
           task_class == (uint32_t)LLAM_TASK_CLASS_DEFAULT ||
           task_class == (uint32_t)LLAM_TASK_CLASS_BATCH;
}

/** @brief Return true when @p stack_class is a supported public stack class. */
static bool llam_public_stack_class_valid(uint32_t stack_class) {
    return stack_class == (uint32_t)LLAM_STACK_CLASS_DEFAULT ||
           stack_class == (uint32_t)LLAM_STACK_CLASS_LARGE ||
           stack_class == (uint32_t)LLAM_STACK_CLASS_HUGE;
}

/**
 * @brief Create a new runtime task and place it on a runnable queue.
 *
 * Algorithm outline:
 *  1) Validate runtime initialization and the task entry function.
 *  2) Resolve optional spawn policy into task class, stack class, flags,
 *     deadline, and cancellation-token ownership.
 *  3) Pick a home shard, allocate the task object, allocate its fiber stack,
 *     and initialize scheduler-visible state.
 *  4) Publish the task in the global task list and increment live task count.
 *  5) Enqueue the task on the best queue for the current context:
 *       - opaque redirect queue when the target shard is blocked in opaque I/O,
 *       - inject queue for remote lock-free normal queue handoff,
 *       - owner normal queue for local owner-side enqueue.
 *  6) Wake the target shard or fan out wakeups when local spawn pressure
 *     warrants it.
 *
 * @param fn Task entry point. Must be non-NULL.
 * @param arg Opaque argument passed to @p fn when the task starts.
 * @param opts Optional spawn options. NULL selects default task/stack classes.
 *
 * @return Newly allocated task handle on success, or NULL with errno set on
 *         failure.
 *
 * @note This is the ABI-stable implementation used by the canonical
 *       `llam_spawn` convenience wrapper.  It assumes the runtime global has
 *       already been initialized.
 */
llam_task_t *llam_spawn_ex(llam_task_fn fn, void *arg, const llam_spawn_opts_t *opts, size_t opts_size) {
    llam_runtime_t *rt = &g_llam_runtime;
    llam_spawn_opts_t opts_storage;
    llam_task_t *task;
    unsigned shard_id;
    llam_stack_class_t stack_class = LLAM_STACK_CLASS_DEFAULT;
    llam_task_class_t task_class = LLAM_TASK_CLASS_DEFAULT;
    llam_shard_t *target;
    size_t opts_copy_size;
    bool local_spawn;
    bool wake_fanout = false;

    llam_task_safepoint();

    if (opts != NULL) {
        if (opts_size == 0U) {
            errno = EINVAL;
            return NULL;
        }
        memset(&opts_storage, 0, sizeof(opts_storage));
        opts_copy_size = opts_size < sizeof(opts_storage) ? opts_size : sizeof(opts_storage);
        memcpy(&opts_storage, opts, opts_copy_size);
        opts = &opts_storage;
        if (!llam_public_task_class_valid(opts->task_class) ||
            !llam_public_stack_class_valid(opts->stack_class)) {
            errno = EINVAL;
            return NULL;
        }
    }

    if (!rt->initialized || fn == NULL) {
        errno = EINVAL;
        return NULL;
    }

    if (opts != NULL) {
        stack_class = (llam_stack_class_t)opts->stack_class;
        task_class = (llam_task_class_t)opts->task_class;
    }

    shard_id = llam_pick_spawn_shard(rt);
    target = &rt->shards[shard_id];
    local_spawn = g_llam_tls_shard == target && g_llam_tls_task != NULL;
    task = llam_task_alloc(target);
    if (task == NULL) {
        return NULL;
    }

    task->id = atomic_fetch_add(&rt->next_task_id, 1U) + 1U;
    task->state = LLAM_TASK_STATE_NEW;
    task->wait_reason = LLAM_WAIT_NONE;
    task->flags = opts != NULL ? opts->flags : 0U;
    if ((task->flags & LLAM_TASK_FLAG_LATENCY_CRITICAL) != 0U) {
        task_class = LLAM_TASK_CLASS_LATENCY;
    }
    task->task_class = task_class;
    task->deadline_ns = opts != NULL ? opts->deadline_ns : 0U;
    task->cancel_token = opts != NULL ? opts->cancel_token : NULL;
    if (task->cancel_token != NULL) {
        pthread_mutex_lock(&task->cancel_token->lock);
        task->cancel_token->refcount += 1U;
        pthread_mutex_unlock(&task->cancel_token->lock);
    }
    task->entry = fn;
    task->arg = arg;
    task->forced_yield_budget = rt->forced_yield_every;
    task->home_shard = shard_id;
    task->last_shard = shard_id;
    atomic_init(&task->preempt_requested, 0U);
    atomic_init(&task->reclaim_ready, 0U);
    atomic_init(&task->reclaim_claimed, 0U);
    atomic_init(&task->detached, 0U);
    task->join_waiter_count_at_exit = 0U;

    if (llam_alloc_task_stack(task, stack_class) != 0) {
        int saved_errno = errno;
        llam_task_allocator_free(task);
        errno = saved_errno;
        return NULL;
    }

    task->last_runnable_ns = rt->wake_latency_metrics_enabled != 0U ? llam_now_ns() : 0U;

    llam_add_task_to_list(rt, task);
    atomic_fetch_add(&rt->live_tasks, 1U);

    pthread_mutex_lock(&target->lock);
    if (rt->experimental_dynamic_shards != 0U &&
        atomic_load_explicit(&target->online, memory_order_relaxed) == 0U) {
        atomic_store_explicit(&target->online, 1U, memory_order_release);
        llam_runtime_note_online_shards(rt, atomic_fetch_add_explicit(&rt->online_shards, 1U, memory_order_acq_rel) + 1U);
    }
    task->state = LLAM_TASK_STATE_RUNNABLE;
    if (target->opaque_redirect_active) {
        target->metrics.migrations += 1U;
        task->enqueue_hot = 0U;
        if (!llam_enqueue_opaque_redirect_task_locked(target, task, false)) {
            llam_enqueue_overflow_task(rt, task);
        }
    } else if (llam_lockfree_normq_enabled(rt) && (g_llam_tls_shard != target || g_llam_tls_task == NULL)) {
        task->enqueue_hot = 0U;
        if (llam_queue_push_bounded_locked(target, &target->inject_q, LLAM_INJECT_QUEUE_CAP, task)) {
            target->metrics.inject_enqueues += 1U;
        }
    } else {
        (void)llam_norm_queue_push_owner_locked(target, task);
    }
    if (local_spawn) {
        wake_fanout = llam_spawn_should_wake_fanout(rt, target);
    }
    llam_trace_shard(target, task, LLAM_TRACE_STATE, LLAM_TASK_STATE_NEW, LLAM_TASK_STATE_RUNNABLE, LLAM_WAIT_NONE);
    pthread_mutex_unlock(&target->lock);
    if (!local_spawn) {
        llam_kick_shard(target);
    } else if (wake_fanout) {
        llam_wake_all_shards(rt);
    }
    return task;
}

llam_task_t *llam_spawn(llam_task_fn fn, void *arg, const llam_spawn_opts_t *opts) {
    return llam_spawn_ex(fn, arg, opts, opts != NULL ? sizeof(*opts) : 0U);
}
