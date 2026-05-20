/**
 * @file src/engine/runtime_watchdog_merge.c
 * @brief Watchdog metric merge logic across workers and runtime nodes.
 *
 * @details
 * Merge helpers support dynamic shard offlining. They choose a low-load target,
 * coordinate pause handshakes, validate that queued tasks are safe to migrate,
 * rehome task ownership, and move runnable queues into the target shard.
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

#include "runtime_watchdog_internal.h"

/**
 * @brief Choose the best target shard for merging an offlined source shard.
 *
 * Targets sharing the same I/O node are preferred, then targets sharing the same
 * locality node, then any accepting shard with the lowest watchdog load.
 *
 * @param rt     Runtime containing the shard set.
 * @param source Shard being considered for offlining.
 *
 * @return Merge target, or @c NULL when no accepting target exists.
 */
llam_shard_t *llam_runtime_pick_merge_target(llam_runtime_t *rt, llam_shard_t *source) {
    llam_shard_t *best_same_io = NULL;
    llam_shard_t *best_same_node = NULL;
    llam_shard_t *best_any = NULL;
    unsigned best_same_io_load = UINT_MAX;
    unsigned best_same_node_load = UINT_MAX;
    unsigned best_any_load = UINT_MAX;
    unsigned i;

    if (rt == NULL || source == NULL) {
        return NULL;
    }

    for (i = 0U; i < rt->active_shards; ++i) {
        llam_shard_t *candidate = &rt->shards[i];
        unsigned load;

        if (candidate == source || !llam_shard_accepts_new_work(candidate)) {
            continue;
        }

        load = llam_watchdog_snapshot_shard_load(candidate);
        if (candidate->io_node_index == source->io_node_index) {
            if (load < best_same_io_load) {
                best_same_io = candidate;
                best_same_io_load = load;
            }
            continue;
        }
        if (candidate->node_index == source->node_index) {
            if (load < best_same_node_load) {
                best_same_node = candidate;
                best_same_node_load = load;
            }
            continue;
        }
        if (load < best_any_load) {
            best_any = candidate;
            best_any_load = load;
        }
    }

    if (best_same_io != NULL) {
        return best_same_io;
    }
    if (best_same_node != NULL) {
        return best_same_node;
    }
    return best_any;
}

/**
 * @brief Sleep briefly while waiting for a watchdog pause handshake.
 */
void llam_watchdog_pause_briefly(void) {
    struct timespec ts;

    ts.tv_sec = 0;
    ts.tv_nsec = 100000L;
    nanosleep(&ts, NULL);
}

/**
 * @brief Enable or disable runtime-wide stealing pause.
 *
 * @param rt     Runtime to update.
 * @param active Whether stealing should pause.
 */
void llam_runtime_set_steal_pause(llam_runtime_t *rt, bool active) {
    unsigned i;

    if (rt == NULL) {
        return;
    }

    for (i = 0U; i < rt->active_shards; ++i) {
        atomic_store_explicit(&rt->shards[i].steal_pause_ack, 0U, memory_order_release);
    }
    atomic_store_explicit(&rt->steal_pause_active, active ? 1U : 0U, memory_order_release);
    llam_wake_all_shards(rt);
}

/**
 * @brief Wait for every online shard to acknowledge the stealing pause.
 *
 * @param rt          Runtime to inspect.
 * @param deadline_ns Absolute monotonic deadline.
 *
 * @return @c true when all online shards acknowledged before the deadline.
 */
bool llam_runtime_wait_steal_pause_ack(llam_runtime_t *rt, uint64_t deadline_ns) {
    if (rt == NULL) {
        return false;
    }

    while (llam_now_ns() < deadline_ns) {
        unsigned i;
        bool all_ack = true;

        for (i = 0U; i < rt->active_shards; ++i) {
            llam_shard_t *shard = &rt->shards[i];

            if (!llam_shard_is_online(shard)) {
                continue;
            }
            if (atomic_load_explicit(&shard->steal_pause_ack, memory_order_acquire) == 0U) {
                all_ack = false;
                break;
            }
        }
        if (all_ack) {
            return true;
        }
        llam_watchdog_pause_briefly();
    }
    return false;
}

/**
 * @brief Request that a shard pause at a merge-safe scheduler point.
 *
 * @param shard Shard to pause.
 */
void llam_shard_request_merge_pause(llam_shard_t *shard) {
    if (shard == NULL) {
        return;
    }
    atomic_store_explicit(&shard->merge_pause_ack, 0U, memory_order_release);
    atomic_store_explicit(&shard->merge_pause_requested, 1U, memory_order_release);
    llam_kick_shard(shard);
}

/**
 * @brief Release a previously requested merge pause.
 *
 * @param shard Shard to release.
 */
void llam_shard_release_merge_pause(llam_shard_t *shard) {
    if (shard == NULL) {
        return;
    }
    atomic_store_explicit(&shard->merge_pause_requested, 0U, memory_order_release);
    atomic_store_explicit(&shard->merge_pause_ack, 0U, memory_order_release);
    llam_kick_shard(shard);
}

/**
 * @brief Wait for a shard to acknowledge merge pause.
 *
 * @param shard       Shard being paused.
 * @param deadline_ns Absolute monotonic deadline.
 *
 * @return @c true when the shard acknowledged before the deadline.
 */
bool llam_shard_wait_merge_pause_ack(llam_shard_t *shard, uint64_t deadline_ns) {
    if (shard == NULL) {
        return false;
    }

    while (llam_now_ns() < deadline_ns) {
        if (!llam_shard_is_online(shard)) {
            return false;
        }
        if (atomic_load_explicit(&shard->merge_pause_ack, memory_order_acquire) != 0U) {
            return true;
        }
        llam_watchdog_pause_briefly();
    }
    return false;
}

/**
 * @brief Check whether a locked shard is empty enough to mark offline.
 *
 * @param shard Locked shard to inspect.
 *
 * @return @c true when no current task, queues, or redirect state remain.
 */
bool llam_shard_can_offline_locked(const llam_shard_t *shard) {
    return shard != NULL &&
           atomic_load_explicit(&shard->current, memory_order_acquire) == NULL &&
           atomic_load_explicit(&((llam_shard_t *)shard)->timer_callbacks_active, memory_order_acquire) == 0U &&
           shard->inject_q.depth == 0U &&
           shard->hot_q.depth == 0U &&
           llam_norm_queue_depth(shard) == 0U &&
           !shard->opaque_redirect_active;
}

/**
 * @brief Check whether a locked shard is safe to start merging.
 *
 * @param shard Locked shard to inspect.
 *
 * @return @c true when no current task or opaque redirect is active.
 */
bool llam_shard_can_start_merge_locked(const llam_shard_t *shard) {
    return shard != NULL &&
           atomic_load_explicit(&shard->current, memory_order_acquire) == NULL &&
           !shard->opaque_redirect_active;
}

/**
 * @brief Check whether a runnable task can move from source to target.
 *
 * @param source Source shard.
 * @param target Target shard.
 * @param task   Task to inspect.
 *
 * @return @c true when the task is runnable, unpinned, and not waiting.
 */
static bool llam_task_can_merge_runnable(const llam_shard_t *source, const llam_shard_t *target, const llam_task_t *task) {
    if (source == NULL || target == NULL || task == NULL) {
        return false;
    }
    if ((task->flags & LLAM_TASK_FLAG_PINNED) != 0U) {
        return false;
    }
    if (task->state != LLAM_TASK_STATE_RUNNABLE || task->wait_reason != LLAM_WAIT_NONE) {
        return false;
    }
    return true;
}

/**
 * @brief Validate every task in a queue for merge migration.
 *
 * @param source Source shard.
 * @param target Target shard.
 * @param queue  Queue to inspect.
 *
 * @return @c true when every queued task is mergeable.
 */
static bool llam_queue_can_merge_runnable(const llam_shard_t *source, const llam_shard_t *target, const llam_queue_t *queue) {
    const llam_task_t *task;

    if (source == NULL || target == NULL || queue == NULL) {
        return false;
    }

    for (task = queue->head; task != NULL; task = task->queue_next) {
        if (!llam_task_can_merge_runnable(source, target, task)) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Rewrite a task's home shard when its owner is being offlined.
 *
 * @param source Source shard being merged.
 * @param target Target shard receiving ownership.
 * @param task   Task to update.
 */
void llam_merge_rehome_task(llam_shard_t *source, llam_shard_t *target, llam_task_t *task) {
    if (source == NULL || target == NULL || task == NULL) {
        return;
    }
    if (task->home_shard == source->id) {
        task->home_shard = target->id;
    }
}

/**
 * @brief Move one runnable task into the target inject queue.
 *
 * @param source Source shard.
 * @param target Target shard.
 * @param task   Task to migrate.
 *
 * @return @c true after the task has been accounted as migrated.
 */
static bool llam_merge_task_to_target_locked(llam_shard_t *source, llam_shard_t *target, llam_task_t *task) {
    if (source == NULL || target == NULL || task == NULL) {
        return false;
    }

    llam_merge_rehome_task(source, target, task);
    if (llam_queue_push_bounded_locked(target, &target->inject_q, LLAM_INJECT_QUEUE_CAP, task)) {
        target->metrics.inject_enqueues += 1U;
    }
    source->metrics.migrations += 1U;
    return true;
}

/**
 * @brief Merge the lock-free normal queue while stealing is paused.
 *
 * Tasks are staged first so validation failure can restore the source queue
 * without partially migrating unsafe entries.
 *
 * @param source       Source shard.
 * @param target       Target shard.
 * @param migrated_out Optional migrated-count output.
 *
 * @return @c true on success.
 */
static bool llam_merge_lockfree_normq_locked(llam_shard_t *source, llam_shard_t *target, unsigned *migrated_out) {
    llam_task_t *staged[LLAM_NORM_QUEUE_CAP];
    unsigned staged_count = 0U;
    unsigned i;

    if (migrated_out != NULL) {
        *migrated_out = 0U;
    }
    if (source == NULL || target == NULL || source == target) {
        return false;
    }
    if (!llam_lockfree_normq_enabled(source->runtime)) {
        return true;
    }
    if (!llam_runtime_steal_pause_active(source->runtime) ||
        !llam_shard_merge_pause_requested(source) ||
        atomic_load_explicit(&source->merge_pause_ack, memory_order_acquire) == 0U) {
        return llam_norm_queue_depth(source) == 0U;
    }

    while (staged_count < LLAM_NORM_QUEUE_CAP) {
        llam_task_t *task = llam_norm_queue_pop_owner_locked(source);

        if (task == NULL) {
            break;
        }
        staged[staged_count++] = task;
        if (!llam_task_can_merge_runnable(source, target, task)) {
            for (i = staged_count; i-- > 0U;) {
                if (llam_norm_queue_push_owner_locked(source, staged[i])) {
                    source->metrics.norm_enqueues -= 1U;
                }
            }
            return false;
        }
    }

    for (i = 0U; i < staged_count; ++i) {
        (void)llam_merge_task_to_target_locked(source, target, staged[i]);
    }
    if (migrated_out != NULL) {
        *migrated_out = staged_count;
    }
    return true;
}

/**
 * @brief Move all mergeable runnable queues from source to target.
 *
 * The caller must hold the relevant shard locks and have established any
 * required pause handshakes.
 *
 * @param source       Source shard being offlined.
 * @param target       Target shard receiving tasks.
 * @param migrated_out Optional migrated-count output.
 *
 * @return @c true when all runnable queues were migrated safely.
 */
bool llam_merge_runnable_queues_locked(llam_shard_t *source, llam_shard_t *target, unsigned *migrated_out) {
    unsigned migrated = 0U;
    unsigned lockfree_norm_migrated = 0U;

    if (migrated_out != NULL) {
        *migrated_out = 0U;
    }
    if (source == NULL || target == NULL || source == target) {
        return false;
    }
    if (!llam_queue_can_merge_runnable(source, target, &source->inject_q) ||
        !llam_queue_can_merge_runnable(source, target, &source->hot_q)) {
        return false;
    }
    if (!llam_lockfree_normq_enabled(source->runtime) && !llam_queue_can_merge_runnable(source, target, &source->norm_q)) {
        return false;
    }

    for (;;) {
        llam_task_t *task = llam_queue_pop_head(&source->inject_q);

        if (task == NULL) {
            break;
        }
        (void)llam_merge_task_to_target_locked(source, target, task);
        migrated += 1U;
    }
    atomic_store_explicit(&source->inject_depth, source->inject_q.depth, memory_order_release);
    for (;;) {
        llam_task_t *task = llam_queue_pop_head(&source->hot_q);

        if (task == NULL) {
            break;
        }
        (void)llam_merge_task_to_target_locked(source, target, task);
        migrated += 1U;
    }
    if (!llam_lockfree_normq_enabled(source->runtime)) {
        for (;;) {
            llam_task_t *task = llam_queue_pop_head(&source->norm_q);

            if (task == NULL) {
                break;
            }
            atomic_fetch_sub_explicit(&source->norm_depth, 1U, memory_order_release);
            (void)llam_merge_task_to_target_locked(source, target, task);
            migrated += 1U;
        }
    } else if (!llam_merge_lockfree_normq_locked(source, target, &lockfree_norm_migrated)) {
        return false;
    }
    migrated += lockfree_norm_migrated;

    if (migrated_out != NULL) {
        *migrated_out = migrated;
    }
    return true;
}
