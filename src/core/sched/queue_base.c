/**
 * @file src/core/sched/queue_base.c
 * @brief Low-level bounded queue primitives used by scheduler queues.
 *
 * @details
 * These helpers provide intrusive FIFO/LIFO task queues, wait queues, and
 * dynamic-worker online-state predicates. Callers hold the appropriate shard,
 * primitive, or runtime lock unless the function explicitly reads atomics only.
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
 * @brief Push a task at the tail of an intrusive task queue.
 *
 * @param queue Queue to mutate.
 * @param task  Task to append.
 */
void llam_queue_push_tail(llam_queue_t *queue, llam_task_t *task) {
    task->queue_next = NULL;
    task->queue_prev = queue->tail;
    if (queue->tail != NULL) {
        queue->tail->queue_next = task;
    } else {
        queue->head = task;
    }
    queue->tail = task;
    queue->depth += 1;
}

/**
 * @brief Push a wait node at the tail of a FIFO wait queue.
 *
 * @param queue Wait queue to mutate.
 * @param node  Wait node to append.
 */
void llam_wait_queue_push_tail(llam_wait_queue_t *queue, llam_wait_node_t *node) {
    node->next = NULL;
    if (queue->tail != NULL) {
        queue->tail->next = node;
    } else {
        queue->head = node;
    }
    queue->tail = node;
    queue->depth += 1U;
}

/**
 * @brief Pop the head node from a FIFO wait queue.
 *
 * @param queue Wait queue to mutate.
 * @return Removed wait node, or NULL if empty.
 */
llam_wait_node_t *llam_wait_queue_pop_head(llam_wait_queue_t *queue) {
    llam_wait_node_t *node = queue->head;

    if (node == NULL) {
        return NULL;
    }

    queue->head = node->next;
    if (queue->head == NULL) {
        queue->tail = NULL;
    }
    node->next = NULL;
    queue->depth -= 1U;
    return node;
}

/**
 * @brief Remove a specific wait node from a FIFO wait queue.
 *
 * @param queue Queue to search.
 * @param node  Node to remove.
 * @return true if the node was found and removed.
 */
bool llam_wait_queue_remove(llam_wait_queue_t *queue, llam_wait_node_t *node) {
    llam_wait_node_t *prev = NULL;
    llam_wait_node_t *cur;

    /*
     * Timeout/cancel handlers can race with the normal primitive wake path.
     * When the normal wake already consumed ownership, wait tracking is cleared
     * before the timer path reaches the primitive lock.  Treat that as "not in
     * this queue" rather than dereferencing a stale NULL owner.
     */
    if (queue == NULL || node == NULL) {
        return false;
    }
    cur = queue->head;

    while (cur != NULL) {
        if (cur == node) {
            if (prev != NULL) {
                prev->next = cur->next;
            } else {
                queue->head = cur->next;
            }
            if (queue->tail == cur) {
                queue->tail = prev;
            }
            cur->next = NULL;
            queue->depth -= 1U;
            return true;
        }
        prev = cur;
        cur = cur->next;
    }

    return false;
}

/**
 * @brief Pop the head task from an intrusive task queue.
 *
 * @param queue Queue to mutate.
 * @return Removed task, or NULL if empty.
 */
llam_task_t *llam_queue_pop_head(llam_queue_t *queue) {
    llam_task_t *task = queue->head;

    if (task == NULL) {
        return NULL;
    }

    queue->head = task->queue_next;
    if (queue->head != NULL) {
        queue->head->queue_prev = NULL;
    } else {
        queue->tail = NULL;
    }
    task->queue_next = NULL;
    task->queue_prev = NULL;
    queue->depth -= 1;
    return task;
}

/**
 * @brief Pop the tail task from an intrusive task queue.
 *
 * @param queue Queue to mutate.
 * @return Removed task, or NULL if empty.
 */
llam_task_t *llam_queue_pop_tail(llam_queue_t *queue) {
    llam_task_t *task = queue->tail;

    if (task == NULL) {
        return NULL;
    }

    queue->tail = task->queue_prev;
    if (queue->tail != NULL) {
        queue->tail->queue_next = NULL;
    } else {
        queue->head = NULL;
    }
    task->queue_next = NULL;
    task->queue_prev = NULL;
    queue->depth -= 1;
    return task;
}

/**
 * @brief Check whether the optional lock-free normal queue is enabled.
 *
 * @param rt Runtime to inspect.
 * @return true when lock-free normal queues are active.
 */
bool llam_lockfree_normq_enabled(const llam_runtime_t *rt) {
    return rt != NULL && rt->experimental_lockfree_normq != 0U;
}

/**
 * @brief Check whether watchdog/merge logic has paused stealing globally.
 *
 * @param rt Runtime to inspect.
 * @return true if stealing should currently pause.
 */
bool llam_runtime_steal_pause_active(const llam_runtime_t *rt) {
    return rt != NULL && atomic_load_explicit(&((llam_runtime_t *)rt)->steal_pause_active, memory_order_acquire) != 0U;
}

/**
 * @brief Check whether a shard is paused for merge/rehome work.
 *
 * @param shard Shard to inspect.
 * @return true if new work should not be pushed to the shard.
 */
bool llam_shard_merge_pause_requested(const llam_shard_t *shard) {
    return shard != NULL && atomic_load_explicit(&((llam_shard_t *)shard)->merge_pause_requested, memory_order_acquire) != 0U;
}

/**
 * @brief Check whether a shard can accept newly runnable work.
 *
 * @param shard Shard to inspect.
 * @return true when online and not merge-paused.
 */
bool llam_shard_accepts_new_work(const llam_shard_t *shard) {
    return llam_shard_is_online(shard) && !llam_shard_merge_pause_requested(shard);
}

/**
 * @brief Check whether a shard is currently online.
 *
 * @param shard Shard to inspect.
 * @return true when the shard should participate in scheduling.
 */
bool llam_shard_is_online(const llam_shard_t *shard) {
    if (shard == NULL || shard->runtime == NULL) {
        return false;
    }
    if (shard->runtime->experimental_dynamic_shards == 0U) {
        return true;
    }
    return atomic_load_explicit(&((llam_shard_t *)shard)->online, memory_order_acquire) != 0U;
}

/**
 * @brief Return the current number of online shards.
 *
 * @param rt Runtime to inspect.
 * @return Active shard count, or dynamic online count when enabled.
 */
unsigned llam_runtime_online_shards(const llam_runtime_t *rt) {
    if (rt == NULL) {
        return 0U;
    }
    if (rt->experimental_dynamic_shards == 0U) {
        return rt->active_shards;
    }
    return atomic_load_explicit(&((llam_runtime_t *)rt)->online_shards, memory_order_acquire);
}

/**
 * @brief Return the dynamic online-shard floor.
 *
 * @param rt Runtime to inspect.
 * @return Minimum shard count kept online.
 */
unsigned llam_runtime_online_shards_floor(const llam_runtime_t *rt) {
    if (rt == NULL) {
        return 0U;
    }
    if (rt->experimental_dynamic_shards == 0U) {
        return rt->active_shards;
    }
    return rt->dynamic_online_floor;
}

/**
 * @brief Update observed min/max online shard counts.
 *
 * @param rt     Runtime to update.
 * @param online Newly observed online shard count.
 */
void llam_runtime_note_online_shards(llam_runtime_t *rt, unsigned online) {
    unsigned observed;

    if (rt == NULL || rt->experimental_dynamic_shards == 0U) {
        return;
    }

    observed = atomic_load_explicit(&rt->online_shards_min, memory_order_acquire);
    while (observed == 0U || online < observed) {
        // The first non-zero observation initializes the minimum; later smaller
        // observations update it with a normal CAS loop.
        if (atomic_compare_exchange_weak_explicit(&rt->online_shards_min,
                                                  &observed,
                                                  online,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            break;
        }
    }

    observed = atomic_load_explicit(&rt->online_shards_max, memory_order_acquire);
    while (online > observed) {
        if (atomic_compare_exchange_weak_explicit(&rt->online_shards_max,
                                                  &observed,
                                                  online,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            break;
        }
    }
}

/**
 * @brief Return the minimum online shard count observed.
 *
 * @param rt Runtime to inspect.
 * @return Minimum observed online shard count.
 */
unsigned llam_runtime_online_shards_min(const llam_runtime_t *rt) {
    unsigned online;

    if (rt == NULL) {
        return 0U;
    }
    if (rt->experimental_dynamic_shards == 0U) {
        return rt->active_shards;
    }
    online = atomic_load_explicit(&((llam_runtime_t *)rt)->online_shards_min, memory_order_acquire);
    return online > 0U ? online : llam_runtime_online_shards(rt);
}

/**
 * @brief Return the maximum online shard count observed.
 *
 * @param rt Runtime to inspect.
 * @return Maximum observed online shard count.
 */
unsigned llam_runtime_online_shards_max(const llam_runtime_t *rt) {
    if (rt == NULL) {
        return 0U;
    }
    if (rt->experimental_dynamic_shards == 0U) {
        return rt->active_shards;
    }
    return atomic_load_explicit(&((llam_runtime_t *)rt)->online_shards_max, memory_order_acquire);
}
