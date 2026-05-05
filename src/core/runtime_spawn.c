/**
 * @file src/core/runtime_spawn.c
 * @brief Task creation, spawn option normalization, and initial runnable queue insertion.
 *
 * @details
 * This translation unit implements the internal spawn path:
 *  - ::nm_spawn
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
 *       ::nm_norm_queue_depth provides the queue-depth snapshot used here.
 */
static bool nm_spawn_should_wake_fanout(nm_runtime_t *rt, nm_shard_t *target) {
    unsigned interval;
    unsigned depth;

    if (rt == NULL || target == NULL || rt->active_shards <= 1U) {
        return false;
    }
    interval = rt->spawn_fanout_wake_interval;
    if (interval == 0U) {
        return false;
    }
    depth = nm_norm_queue_depth(target);
    return depth >= interval && (depth % interval) == 0U;
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
 * @note This is the legacy internal implementation used by the canonical
 *       `llam_spawn` wrapper.  It assumes the runtime global has already been
 *       initialized.
 */
nm_task_t *nm_spawn(nm_task_fn fn, void *arg, const nm_spawn_opts_t *opts) {
    nm_runtime_t *rt = &g_nm_runtime;
    nm_task_t *task;
    unsigned shard_id;
    nm_stack_class_t stack_class = NM_STACK_CLASS_DEFAULT;
    nm_task_class_t task_class = NM_TASK_CLASS_DEFAULT;
    nm_shard_t *target;
    bool local_spawn;
    bool wake_fanout = false;

    nm_task_safepoint();

    if (!rt->initialized || fn == NULL) {
        errno = EINVAL;
        return NULL;
    }

    if (opts != NULL) {
        stack_class = opts->stack_class;
        task_class = opts->task_class;
    }

    shard_id = nm_pick_spawn_shard(rt);
    target = &rt->shards[shard_id];
    local_spawn = g_nm_tls_shard == target && g_nm_tls_task != NULL;
    task = nm_task_alloc(target);
    if (task == NULL) {
        return NULL;
    }

    task->id = atomic_fetch_add(&rt->next_task_id, 1U) + 1U;
    task->state = NM_TASK_STATE_NEW;
    task->wait_reason = NM_WAIT_NONE;
    task->flags = opts != NULL ? opts->flags : 0U;
    if ((task->flags & NM_TASK_FLAG_LATENCY_CRITICAL) != 0U) {
        task_class = NM_TASK_CLASS_LATENCY;
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
    task->join_waiter_count_at_exit = 0U;

    if (nm_alloc_task_stack(task, stack_class) != 0) {
        int saved_errno = errno;
        nm_task_allocator_free(task);
        errno = saved_errno;
        return NULL;
    }

    task->last_runnable_ns = rt->wake_latency_metrics_enabled != 0U ? nm_now_ns() : 0U;

    nm_add_task_to_list(rt, task);
    atomic_fetch_add(&rt->live_tasks, 1U);

    pthread_mutex_lock(&target->lock);
    if (rt->experimental_dynamic_shards != 0U &&
        atomic_load_explicit(&target->online, memory_order_relaxed) == 0U) {
        atomic_store_explicit(&target->online, 1U, memory_order_release);
        nm_runtime_note_online_shards(rt, atomic_fetch_add_explicit(&rt->online_shards, 1U, memory_order_acq_rel) + 1U);
    }
    task->state = NM_TASK_STATE_RUNNABLE;
    if (target->opaque_redirect_active) {
        target->metrics.migrations += 1U;
        task->enqueue_hot = 0U;
        if (!nm_enqueue_opaque_redirect_task_locked(target, task, false)) {
            nm_enqueue_overflow_task(rt, task);
        }
    } else if (nm_lockfree_normq_enabled(rt) && (g_nm_tls_shard != target || g_nm_tls_task == NULL)) {
        task->enqueue_hot = 0U;
        if (nm_queue_push_bounded_locked(target, &target->inject_q, NM_INJECT_QUEUE_CAP, task)) {
            target->metrics.inject_enqueues += 1U;
        }
    } else {
        (void)nm_norm_queue_push_owner_locked(target, task);
    }
    if (local_spawn) {
        wake_fanout = nm_spawn_should_wake_fanout(rt, target);
    }
    nm_trace_shard(target, task, NM_TRACE_STATE, NM_TASK_STATE_NEW, NM_TASK_STATE_RUNNABLE, NM_WAIT_NONE);
    pthread_mutex_unlock(&target->lock);
    if (!local_spawn) {
        nm_kick_shard(target);
    } else if (wake_fanout) {
        nm_wake_all_shards(rt);
    }
    return task;
}
