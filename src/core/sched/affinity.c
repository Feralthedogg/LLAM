/**
 * @file src/core/sched/affinity.c
 * @brief Hard logical-shard affinity validation and containment.
 *
 * @details
 * This translation unit centralizes the execution contract for pinned tasks:
 *  - resolve the immutable home shard and fail closed on corrupt ownership,
 *  - validate every candidate logical execution shard,
 *  - return rejected runnable tasks to their home inject queue,
 *  - filter direct-handoff candidates before a task-to-task switch commits,
 *  - contain an invalid final dispatch without executing user code.
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

unsigned llam_task_required_shard(const llam_runtime_t *rt,
                                  const llam_task_t *task) {
    if (rt == NULL || task == NULL) {
        return UINT_MAX;
    }
    if (task->owner_runtime != rt) {
        llam_record_fatal_deferred((llam_runtime_t *)rt, EPROTO);
        return UINT_MAX;
    }
    if ((task->flags & LLAM_TASK_FLAG_PINNED) == 0U) {
        return UINT_MAX;
    }
    if (rt->shards == NULL || rt->active_shards == 0U ||
        task->home_shard >= rt->active_shards) {
        llam_record_fatal_deferred((llam_runtime_t *)rt, EPROTO);
        return UINT_MAX;
    }
    return task->home_shard;
}

bool llam_task_may_run_on_shard(const llam_task_t *task,
                                const llam_shard_t *shard) {
    unsigned required;

    if (task == NULL || shard == NULL || shard->runtime == NULL ||
        task->owner_runtime != shard->runtime) {
        if (shard != NULL && shard->runtime != NULL && task != NULL) {
            llam_record_fatal_deferred(shard->runtime, EPROTO);
        }
        return false;
    }
    if ((task->flags & LLAM_TASK_FLAG_PINNED) == 0U) {
        return true;
    }
    required = llam_task_required_shard(shard->runtime, task);
    return required != UINT_MAX && required == shard->id;
}

bool llam_requeue_task_to_required_shard(llam_runtime_t *rt,
                                         llam_task_t *task,
                                         bool hot) {
    unsigned required;
    llam_shard_t *target;

    if (rt == NULL || task == NULL) {
        return false;
    }
    required = llam_task_required_shard(rt, task);
    if (required == UINT_MAX) {
        return false;
    }
    target = &rt->shards[required];
    pthread_mutex_lock(&target->lock);
    if (rt->experimental_dynamic_shards != 0U &&
        atomic_load_explicit(&target->online, memory_order_relaxed) == 0U) {
        atomic_store_explicit(&target->online, 1U, memory_order_release);
        llam_runtime_note_online_shards(
            rt,
            atomic_fetch_add_explicit(&rt->online_shards,
                                      1U,
                                      memory_order_acq_rel) +
                1U);
    }
    task->enqueue_hot = hot ? 1U : 0U;
    if (llam_queue_push_bounded_locked(
            target, &target->inject_q, LLAM_INJECT_QUEUE_CAP, task)) {
        target->metrics.inject_enqueues += 1U;
    }
    pthread_mutex_unlock(&target->lock);
    llam_kick_shard(target);
    return true;
}

llam_task_t *llam_take_handoff_task_unlocked(llam_shard_t *shard) {
    llam_task_t *task;

    for (;;) {
        task = llam_norm_queue_pop_owner_unlocked(shard);
        if (task == NULL || llam_task_may_run_on_shard(task, shard)) {
            return task;
        }
        if (!llam_requeue_task_to_required_shard(
                shard->runtime, task, task->enqueue_hot != 0U)) {
            llam_enqueue_overflow_task(shard->runtime, task);
        }
    }
}

llam_task_t *llam_take_handoff_task_locked(llam_shard_t *shard) {
    llam_task_t *task;

    for (;;) {
        task = llam_queue_pop_head(&shard->hot_q);
        if (task == NULL) {
            task = llam_norm_queue_pop_owner_locked(shard);
        }
        if (task == NULL || llam_task_may_run_on_shard(task, shard)) {
            return task;
        }
        task->enqueue_hot = 0U;
        llam_enqueue_overflow_task(shard->runtime, task);
    }
}

/**
 * @brief Fail a corrupt task that has no valid logical execution shard.
 *
 * This mirrors terminal task bookkeeping but never executes user code. It is
 * reserved for impossible ownership corruption such as an invalid pinned home.
 */
static void llam_fail_undispatchable_task(llam_shard_t *shard,
                                          llam_task_t *task) {
    llam_runtime_t *rt = shard != NULL ? shard->runtime : NULL;
    llam_task_state_id_t from;

    if (rt == NULL || task == NULL || task->owner_runtime != rt) {
        llam_record_fatal(rt, EPROTO);
        return;
    }
    llam_record_fatal(rt, EPROTO);
    from = task->state;
    pthread_mutex_lock(&task->lock);
    task->state = LLAM_TASK_STATE_DEAD;
    task->wait_reason = LLAM_WAIT_NONE;
    atomic_store_explicit(&task->completed, 1U, memory_order_release);
    pthread_mutex_unlock(&task->lock);
    llam_trace_shard(shard,
                     task,
                     LLAM_TRACE_STATE,
                     from,
                     LLAM_TASK_STATE_DEAD,
                     LLAM_WAIT_NONE);
    llam_reinject_join_waiters(rt, task);
    if (llam_runtime_note_task_dead(rt, task)) {
        llam_request_stop(rt);
    }
    llam_task_release_stack(task);
    llam_task_mark_reclaim_ready(task);
    llam_try_reclaim_detached_task(rt, task);
}

bool llam_prepare_task_dispatch(llam_shard_t *shard, llam_task_t *task) {
    if (llam_task_may_run_on_shard(task, shard)) {
        return true;
    }
    if (task != NULL && shard != NULL &&
        task->owner_runtime == shard->runtime &&
        (task->flags & LLAM_TASK_FLAG_PINNED) != 0U &&
        llam_requeue_task_to_required_shard(
            shard->runtime, task, task->enqueue_hot != 0U)) {
        return false;
    }
    llam_fail_undispatchable_task(shard, task);
    return false;
}
