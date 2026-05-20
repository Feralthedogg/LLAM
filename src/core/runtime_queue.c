/**
 * @file src/core/runtime_queue.c
 * @brief Scheduler queue operations for runnable and injected task work.
 *
 * @details
 * This translation unit owns the queue policy between runnable task producers
 * and scheduler shards:
 *  - overflow queue handoff when bounded shard queues are full,
 *  - pressure detection from overflow/blocking backlog,
 *  - runnable shard selection for migrating or waking tasks,
 *  - hot-vs-normal lane selection,
 *  - inject queue draining for remote producers,
 *  - opaque-blocking redirect activation and teardown.
 *
 * Queue lanes:
 *  - inject_q receives cross-thread or remote-shard handoff work.
 *  - hot_q gives short-term priority to latency-sensitive or recently woken
 *    tasks.
 *  - norm_q is the normal runnable lane and may use the lock-free Chase-Lev
 *    implementation.
 *  - overflow_q is runtime-global backpressure storage when a bounded queue is
 *    full.
 *
 * Locking model:
 *  - `*_locked` functions require the caller to hold the relevant shard lock.
 *  - overflow_q is protected by rt->overflow_lock.
 *  - inject_depth is a release/acquire hint so idle workers can avoid taking
 *    shard->lock when there is clearly no inject work.
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

/** @brief Overflow, pressure, and local runnable-selection queue policy. */

#include "runtime_internal.h"

/**
 * @brief Move a task to the runtime-global overflow queue.
 *
 * Overflow is a safety valve for bounded per-shard queues.  The task remains
 * runnable; it is simply held in global storage until a worker has capacity to
 * take it back.
 *
 * @param rt Runtime instance that owns the overflow queue.
 * @param task Runnable task to enqueue.
 *
 * @note This function wakes all shards because any online worker may be able
 *       to drain overflow work.
 */
void llam_enqueue_overflow_task(llam_runtime_t *rt, llam_task_t *task) {
    if (rt == NULL || task == NULL || !rt->overflow_lock_initialized) {
        return;
    }

    task->queue_next = NULL;
    task->queue_prev = NULL;
    pthread_mutex_lock(&rt->overflow_lock);
    llam_queue_push_tail(&rt->overflow_q, task);
    atomic_store(&rt->overflow_depth, rt->overflow_q.depth);
    pthread_mutex_unlock(&rt->overflow_lock);
    llam_wake_all_shards(rt);
}

/**
 * @brief Take one runnable task from the runtime-global overflow queue.
 *
 * @param rt Runtime instance that owns the overflow queue.
 *
 * @return Task removed from overflow storage, or NULL if no overflow task is
 *         currently available.
 */
llam_task_t *llam_take_overflow_task(llam_runtime_t *rt) {
    llam_task_t *task;

    if (rt == NULL || !rt->overflow_lock_initialized) {
        return NULL;
    }
    if (atomic_load_explicit(&rt->overflow_depth, memory_order_acquire) == 0U) {
        return NULL;
    }

    pthread_mutex_lock(&rt->overflow_lock);
    task = llam_queue_pop_head(&rt->overflow_q);
    atomic_store(&rt->overflow_depth, rt->overflow_q.depth);
    pthread_mutex_unlock(&rt->overflow_lock);
    return task;
}

/**
 * @brief Read the current overflow queue depth.
 *
 * @param rt Runtime instance.
 * @return Current overflow depth, or 0 if overflow storage is unavailable.
 */
unsigned llam_runtime_overflow_depth(llam_runtime_t *rt) {
    if (rt == NULL || !rt->overflow_lock_initialized) {
        return 0U;
    }
    return atomic_load(&rt->overflow_depth);
}

/**
 * @brief Report whether the scheduler is under global queue or blocking pressure.
 *
 * Pressure is a coarse policy signal used to bias queue selection away from
 * latency-only behavior and toward draining backlog.  It combines overflow
 * queue depth with pending opaque/blocking jobs.
 *
 * @param rt Runtime instance.
 * @return true when pressure-sensitive scheduling should be enabled.
 */
bool llam_runtime_pressure_signal(llam_runtime_t *rt) {
    unsigned overflow_depth;
    unsigned overflow_threshold;
    unsigned online_shards;
    unsigned block_pending = 0U;

    if (rt == NULL) {
        return false;
    }

    overflow_depth = llam_runtime_overflow_depth(rt);
    if (rt->block_worker_count > 0U) {
        block_pending = atomic_load(&rt->block_pending);
    }
    if (overflow_depth == 0U && block_pending == 0U) {
        return false;
    }

    online_shards = llam_max_unsigned(1U, llam_runtime_online_shards(rt));
    overflow_threshold = llam_max_unsigned(4U, online_shards * 2U);
    if (overflow_depth >= overflow_threshold) {
        return true;
    }

    if (block_pending > rt->block_worker_count * 2U) {
        return true;
    }

    return false;
}

/**
 * @brief Compute a best-effort load score for a shard.
 *
 * The score is intentionally simple and cheap: queued runnable tasks plus the
 * current task if one is running.  Opaque redirect activity adds a large
 * penalty so new work avoids a shard that is compensating for blocked code.
 *
 * @param shard Scheduler shard to sample.
 * @return Approximate runnable load for shard selection.
 */
unsigned llam_snapshot_shard_load(llam_shard_t *shard) {
    unsigned load;

    pthread_mutex_lock(&shard->lock);
    load = shard->inject_q.depth + shard->hot_q.depth + llam_norm_queue_depth(shard);
    if (atomic_load_explicit(&shard->current, memory_order_acquire) != NULL) {
        load += 1U;
    }
    if (shard->opaque_redirect_active) {
        load += LLAM_INJECT_QUEUE_CAP;
    }
    pthread_mutex_unlock(&shard->lock);
    return load;
}

/**
 * @brief Pick the best shard to receive a runnable task.
 *
 * Selection rules:
 *  - Pinned tasks stay on their home shard.
 *  - Prefer the task's home shard when it is online and lightly loaded.
 *  - Consider last-running and current-origin shards to improve locality.
 *  - Under pressure, allow more aggressive cross-node balancing.
 *  - Add penalties for cross-node movement and latency-critical migration.
 *
 * @param rt Runtime instance.
 * @param task Task that is becoming runnable.
 *
 * @return Target shard id. Returns 0 only for invalid runtime/task input.
 */
unsigned llam_pick_runnable_shard(llam_runtime_t *rt, llam_task_t *task) {
    unsigned home_id;
    unsigned last_id;
    unsigned origin_id;
    unsigned best_id;
    unsigned best_load;
    bool pressure;
    unsigned i;
    bool best_found = false;

    if (rt == NULL || task == NULL || rt->active_shards == 0U) {
        return 0U;
    }

    home_id = task->home_shard < rt->active_shards ? task->home_shard : task->last_shard % rt->active_shards;
    if ((task->flags & LLAM_TASK_FLAG_PINNED) != 0U || rt->active_shards < 2U) {
        return home_id;
    }

    last_id = task->last_shard < rt->active_shards ? task->last_shard : home_id;
    origin_id = g_llam_tls_shard != NULL && g_llam_tls_shard->id < rt->active_shards ? g_llam_tls_shard->id : home_id;
    best_id = home_id;
    best_load = UINT_MAX;
    if (llam_shard_accepts_new_work(&rt->shards[home_id])) {
        best_id = home_id;
        best_load = llam_snapshot_shard_load(&rt->shards[home_id]);
        best_found = true;
    } else {
        for (i = 0; i < rt->active_shards; ++i) {
            if (!llam_shard_accepts_new_work(&rt->shards[i])) {
                continue;
            }
            best_id = i;
            best_load = llam_snapshot_shard_load(&rt->shards[i]);
            best_found = true;
            break;
        }
    }
    if (!best_found) {
        return home_id;
    }
    pressure = llam_runtime_pressure_signal(rt);

    if (!pressure && best_load < 8U) {
        return best_id;
    }

    if (last_id != best_id && llam_shard_accepts_new_work(&rt->shards[last_id])) {
        unsigned load = llam_snapshot_shard_load(&rt->shards[last_id]);

        if (rt->shards[last_id].node_index == rt->shards[home_id].node_index && load + 2U < best_load) {
            best_id = last_id;
            best_load = load;
        }
    }

    if (origin_id != best_id && llam_shard_accepts_new_work(&rt->shards[origin_id])) {
        unsigned load = llam_snapshot_shard_load(&rt->shards[origin_id]);

        if (rt->shards[origin_id].node_index == rt->shards[home_id].node_index && load + 2U < best_load) {
            best_id = origin_id;
            best_load = load;
        }
    }

    for (i = 0; i < rt->active_shards; ++i) {
        unsigned load;
        unsigned penalty = 0U;

        if (i == best_id) {
            continue;
        }
        if (!llam_shard_accepts_new_work(&rt->shards[i])) {
            continue;
        }

        if (!pressure && rt->shards[i].node_index != rt->shards[home_id].node_index) {
            continue;
        }

        load = llam_snapshot_shard_load(&rt->shards[i]);
        if (rt->shards[i].node_index != rt->shards[home_id].node_index) {
            penalty += pressure ? 2U : 8U;
        }
        if (i != last_id) {
            penalty += 1U;
        }
        if (atomic_load_explicit(&task->task_class, memory_order_acquire) == (unsigned)LLAM_TASK_CLASS_LATENCY && i != home_id) {
            penalty += 2U;
        }

        if (load + penalty < best_load) {
            best_id = i;
            best_load = load + penalty;
        }
    }

    return best_id;
}

/**
 * @brief Decide whether a task should enter the hot runnable lane.
 *
 * Hot queueing gives recently woken or explicitly latency-sensitive work a
 * short priority boost, but pressure and hot-queue dominance demote ordinary
 * work back to the normal lane to avoid starvation.
 *
 * @param shard Shard whose queues are being updated.
 * @param task Task being enqueued.
 * @param hot_requested Caller preference for hot lane placement.
 * @param pressure Current runtime pressure signal.
 *
 * @return true to use hot_q, false to use the normal lane.
 *
 * @note Caller must hold @p shard->lock.
 */
bool llam_should_enqueue_hot_locked(llam_shard_t *shard,
                                  const llam_task_t *task,
                                  bool hot_requested,
                                  bool pressure) {
    unsigned norm_depth;

    /* Pressure can still demote hot work so the normal lane does not starve. */
    if (task != NULL && atomic_load_explicit(&task->task_class, memory_order_acquire) == (unsigned)LLAM_TASK_CLASS_LATENCY) {
        return true;
    }

    if (!hot_requested) {
        return false;
    }

    if (pressure) {
        return false;
    }

    norm_depth = llam_norm_queue_depth(shard);
    if (shard->hot_q.depth >= norm_depth + 8U) {
        return false;
    }

    return true;
}

/**
 * @brief Compute how long the scheduler may keep draining hot_q.
 *
 * @param shard Shard whose hot/normal queues are being balanced.
 * @param pressure Current runtime pressure signal.
 *
 * @return Maximum consecutive hot tasks before normal work must be considered.
 *
 * @note Caller must hold @p shard->lock.
 */
unsigned llam_hot_streak_cap_locked(llam_shard_t *shard, bool pressure) {
    unsigned cap = pressure ? 2U : 8U;
    unsigned norm_depth = llam_norm_queue_depth(shard);

    if (norm_depth > shard->hot_q.depth && norm_depth > 0U) {
        cap = pressure ? 1U : 4U;
    }

    return cap;
}

/**
 * @brief Push a task into a bounded shard queue or spill it to overflow.
 *
 * @param shard Owner shard for metrics and inject-depth hints.
 * @param queue Queue to receive the task.
 * @param capacity Maximum queue depth.
 * @param task Runnable task to enqueue.
 *
 * @return true if the task fit in @p queue, false if it was moved to overflow.
 *
 * @note Caller must hold @p shard->lock.
 */
bool llam_queue_push_bounded_locked(llam_shard_t *shard,
                                  llam_queue_t *queue,
                                  unsigned capacity,
                                  llam_task_t *task) {
    if (queue->depth < capacity) {
        llam_queue_push_tail(queue, task);
        if (queue == &shard->inject_q) {
            atomic_store_explicit(&shard->inject_depth, queue->depth, memory_order_release);
        }
        return true;
    }

    shard->metrics.queue_overflows += 1U;
    llam_enqueue_overflow_task(shard->runtime, task);
    return false;
}

/**
 * @brief Select a shard to receive redirected work while another shard is opaque-blocked.
 *
 * @param blocked Shard entering opaque blocking compensation.
 *
 * @return Target shard id, or UINT_MAX when no safe redirect target exists.
 *
 * @note Internal helper.  The chosen target must still be revalidated under
 *       that target's lock before enqueueing.
 */
static unsigned llam_pick_opaque_redirect_target_id(llam_shard_t *blocked) {
    llam_runtime_t *rt;
    unsigned start;
    unsigned i;

    if (blocked == NULL || blocked->runtime == NULL) {
        return UINT_MAX;
    }
    rt = blocked->runtime;
    if (rt->active_shards <= 1U) {
        return UINT_MAX;
    }
    start = (blocked->id + 1U) % rt->active_shards;
    for (i = 0U; i < rt->active_shards; ++i) {
        unsigned id = (start + i) % rt->active_shards;
        llam_shard_t *candidate = &rt->shards[id];

        if (candidate == blocked || !llam_shard_accepts_new_work(candidate)) {
            continue;
        }
        return id;
    }
    return UINT_MAX;
}

/**
 * @brief Enqueue work onto the redirect target of an opaque-blocked shard.
 *
 * @param blocked Shard whose local queues are being redirected.
 * @param task Runnable task to move away from @p blocked.
 * @param hot true to preserve/force hot-lane preference on the target.
 *
 * @return true if redirect handling accepted the task. This includes spill to
 *         the runtime overflow queue when the target inject queue is full.
 *
 * @note Caller must hold @p blocked->lock.  This function tries the target
 *       lock without blocking to avoid deadlocking two shard locks.
 */
bool llam_enqueue_opaque_redirect_task_locked(llam_shard_t *blocked, llam_task_t *task, bool hot) {
    llam_runtime_t *rt;
    llam_shard_t *target;
    unsigned target_id;
    bool queued;

    if (blocked == NULL || task == NULL || blocked->runtime == NULL) {
        return false;
    }
    rt = blocked->runtime;
    target_id = blocked->opaque_redirect_target_id;
    if (target_id >= rt->active_shards || target_id == blocked->id) {
        return false;
    }
    target = &rt->shards[target_id];
    if (!llam_shard_accepts_new_work(target)) {
        return false;
    }
    if (pthread_mutex_trylock(&target->lock) != 0) {
        return false;
    }
    if (target->opaque_redirect_active) {
        pthread_mutex_unlock(&target->lock);
        return false;
    }

    task->enqueue_hot = hot ? 1U : 0U;
    queued = llam_queue_push_bounded_locked(target, &target->inject_q, LLAM_INJECT_QUEUE_CAP, task);
    if (queued) {
        target->metrics.inject_enqueues += 1U;
    }
    pthread_mutex_unlock(&target->lock);
    if (queued) {
        llam_kick_shard(target);
    }
    return true;
}

/**
 * @brief Drain remote inject work into local runnable lanes.
 *
 * Remote producers enqueue into inject_q so they do not need owner-only normal
 * queue access.  The owner shard drains a bounded batch into hot_q or norm_q
 * according to current pressure and task priority.
 *
 * @param shard Owner shard whose inject queue is drained.
 */
void llam_drain_inject_queue(llam_shard_t *shard) {
    unsigned drained = 0;
    bool pressure;

    if (atomic_load_explicit(&shard->inject_depth, memory_order_acquire) == 0U) {
        return;
    }

    pressure = llam_runtime_pressure_signal(shard->runtime);

    pthread_mutex_lock(&shard->lock);
    if (shard->inject_q.depth == 0U) {
        atomic_store_explicit(&shard->inject_depth, 0U, memory_order_release);
        pthread_mutex_unlock(&shard->lock);
        return;
    }
    while (drained < LLAM_INJECT_DRAIN_BUDGET) {
        llam_task_t *task = llam_queue_pop_head(&shard->inject_q);
        bool prefer_hot;

        if (task == NULL) {
            break;
        }

        prefer_hot = llam_should_enqueue_hot_locked(shard, task, task->enqueue_hot != 0U, pressure);
        if (prefer_hot) {
            if (llam_queue_push_bounded_locked(shard, &shard->hot_q, LLAM_HOT_QUEUE_CAP, task)) {
                shard->metrics.hot_enqueues += 1U;
            }
        } else {
            (void)llam_norm_queue_push_owner_locked(shard, task);
        }
        task->enqueue_hot = 0U;
        drained += 1U;
    }
    atomic_store_explicit(&shard->inject_depth, shard->inject_q.depth, memory_order_release);
    pthread_mutex_unlock(&shard->lock);
}

/**
 * @brief Move every task from a queue to the shard's opaque redirect target.
 *
 * @param shard Shard whose queue is being flushed.
 * @param queue Queue to empty.
 * @param force_hot true to force target hot-lane preference.
 *
 * @note Caller must hold @p shard->lock.
 */
static void llam_flush_queue_to_redirect_locked(llam_shard_t *shard, llam_queue_t *queue, bool force_hot) {
    for (;;) {
        llam_task_t *task = llam_queue_pop_head(queue);
        bool hot;

        if (task == NULL) {
            break;
        }
        hot = force_hot || task->enqueue_hot != 0U;
        shard->metrics.migrations += 1U;
        if (!llam_enqueue_opaque_redirect_task_locked(shard, task, hot)) {
            task->enqueue_hot = 0U;
            llam_enqueue_overflow_task(shard->runtime, task);
        }
    }
    if (queue == &shard->inject_q) {
        atomic_store_explicit(&shard->inject_depth, 0U, memory_order_release);
    }
}

/**
 * @brief Activate opaque-blocking redirect mode for a shard.
 *
 * When a task enters an opaque blocking region, the original worker may be
 * unavailable for normal scheduling.  Redirect mode migrates queued runnable
 * work to another shard so unrelated tasks can continue making progress.
 *
 * @param shard Shard entering redirect mode.
 * @param current_task Task that triggered opaque blocking, if known.
 *
 * @note Caller must hold @p shard->lock.
 */
void llam_activate_opaque_redirect_locked(llam_shard_t *shard, llam_task_t *current_task) {
    if (shard->opaque_redirect_active) {
        return;
    }

    shard->opaque_redirect_active = true;
    shard->opaque_redirect_target_id = llam_pick_opaque_redirect_target_id(shard);
    llam_flush_queue_to_redirect_locked(shard, &shard->inject_q, false);
    llam_flush_queue_to_redirect_locked(shard, &shard->hot_q, true);
    for (;;) {
        llam_task_t *task = llam_norm_queue_pop_owner_locked(shard);

        if (task == NULL) {
            break;
        }
        task->enqueue_hot = 0U;
        shard->metrics.migrations += 1U;
        if (!llam_enqueue_opaque_redirect_task_locked(shard, task, false)) {
            llam_enqueue_overflow_task(shard->runtime, task);
        }
    }
    if (current_task != NULL) {
        llam_trace_shard(shard,
                       current_task,
                       LLAM_TRACE_STATE,
                       LLAM_TASK_STATE_RUNNING,
                       LLAM_TASK_STATE_BLOCKED_OPAQUE,
                       LLAM_WAIT_BLOCKING);
    }
}

/**
 * @brief Deactivate opaque-blocking redirect mode for a shard.
 *
 * @param shard Shard whose redirect state should be cleared.
 *
 * @note Caller must hold @p shard->lock.
 */
void llam_deactivate_opaque_redirect_locked(llam_shard_t *shard) {
    shard->opaque_redirect_active = false;
    shard->opaque_redirect_target_id = UINT_MAX;
}

/**
 * @brief Take the next local runnable task while honoring hot/normal fairness.
 *
 * @param shard Shard whose local runnable lanes are inspected.
 * @param pressure Current runtime pressure signal.
 *
 * @return Runnable task, or NULL if no local task is available.
 */
llam_task_t *llam_take_local_task_with_pressure(llam_shard_t *shard, bool pressure) {
    llam_task_t *task;

    pthread_mutex_lock(&shard->lock);
    if (shard->hot_q.depth > 0U) {
        bool take_norm = false;

        if (shard->hot_streak >= (pressure ? 1U : 4U)) {
            unsigned norm_depth = llam_norm_queue_depth(shard);

            if (norm_depth > 0U) {
                unsigned hot_cap = pressure ? 2U : 8U;

                if (norm_depth > shard->hot_q.depth) {
                    hot_cap = pressure ? 1U : 4U;
                }
                take_norm = shard->hot_streak >= hot_cap;
            }
        }

        if (take_norm) {
            shard->hot_streak = 0U;
            task = llam_norm_queue_pop_owner_locked(shard);
        } else {
            shard->hot_streak += 1U;
            task = llam_queue_pop_head(&shard->hot_q);
        }
    } else {
        shard->hot_streak = 0U;
        task = llam_norm_queue_pop_owner_locked(shard);
    }
    pthread_mutex_unlock(&shard->lock);
    return task;
}

/**
 * @brief Take the next local runnable task using the current runtime pressure signal.
 *
 * @param shard Shard whose local runnable lanes are inspected.
 * @return Runnable task, or NULL if no local task is available.
 */
llam_task_t *llam_take_local_task(llam_shard_t *shard) {
    return llam_take_local_task_with_pressure(shard, llam_runtime_pressure_signal(shard->runtime));
}
