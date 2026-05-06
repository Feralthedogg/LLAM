/**
 * @file src/core/runtime_norm_queue.c
 * @brief Normal-priority runnable queue implementation and optional lock-free path.
 *
 * @details
 * The normal runnable lane can use either the traditional shard-locked FIFO
 * queue or an optional Chase-Lev deque. The deque lets the owner push/pop at the
 * bottom while thieves steal from the top, reducing lock contention in
 * work-stealing benchmarks.
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
 * @brief Return the best-effort normal queue depth for a shard.
 *
 * @param shard Shard to inspect.
 * @return Current normal-lane depth, or 0 for NULL.
 */
unsigned llam_norm_queue_depth(const llam_shard_t *shard) {
    return shard != NULL ? atomic_load_explicit(&((llam_shard_t *)shard)->norm_depth, memory_order_acquire) : 0U;
}

/**
 * @brief Initialize a Chase-Lev normal runnable deque.
 *
 * Chase-Lev discipline: the owner shard mutates bottom, thieves race only on top.
 *
 * @param deque Deque storage to initialize.
 */
void llam_cldeque_init(llam_cldeque_t *deque) {
    size_t i;

    if (deque == NULL) {
        return;
    }
    atomic_init(&deque->top, 0U);
    atomic_init(&deque->bottom, 0U);
    for (i = 0; i < LLAM_NORM_QUEUE_CAP; ++i) {
        deque->buffer[i] = NULL;
    }
}

/**
 * @brief Push a task onto the owner side of a Chase-Lev deque.
 *
 * @param deque Deque owned by the current shard.
 * @param task  Task to enqueue.
 * @return true on success, false if the bounded deque is full.
 */
bool llam_cldeque_push_bottom(llam_cldeque_t *deque, llam_task_t *task) {
    size_t bottom;
    size_t top;

    if (deque == NULL || task == NULL) {
        return false;
    }

    bottom = atomic_load_explicit(&deque->bottom, memory_order_relaxed);
    top = atomic_load_explicit(&deque->top, memory_order_acquire);
    if (bottom - top >= LLAM_NORM_QUEUE_CAP) {
        return false;
    }

    deque->buffer[bottom & (LLAM_NORM_QUEUE_CAP - 1U)] = task;
    // Publish the task pointer before moving bottom so thieves never observe an
    // initialized slot as available without the payload.
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&deque->bottom, bottom + 1U, memory_order_relaxed);
    return true;
}

/**
 * @brief Pop a task from the owner side of a Chase-Lev deque.
 *
 * @param deque Deque owned by the current shard.
 * @return Task on success, or NULL on empty/lost last-item race.
 */
static llam_task_t *llam_cldeque_pop_bottom(llam_cldeque_t *deque) {
    size_t bottom;
    size_t top;
    llam_task_t *task;

    if (deque == NULL) {
        return NULL;
    }

    bottom = atomic_load_explicit(&deque->bottom, memory_order_relaxed);
    if (bottom == 0U) {
        return NULL;
    }

    bottom -= 1U;
    atomic_store_explicit(&deque->bottom, bottom, memory_order_relaxed);
    atomic_thread_fence(memory_order_seq_cst);
    top = atomic_load_explicit(&deque->top, memory_order_relaxed);
    if (top > bottom) {
        atomic_store_explicit(&deque->bottom, top, memory_order_relaxed);
        return NULL;
    }

    task = deque->buffer[bottom & (LLAM_NORM_QUEUE_CAP - 1U)];
    if (top == bottom) {
        size_t expected = top;

        // Last item race: owner and thief contend on top. The CAS winner owns
        // the task; the loser reports an empty pop.
        if (!atomic_compare_exchange_strong_explicit(&deque->top,
                                                     &expected,
                                                     top + 1U,
                                                     memory_order_seq_cst,
                                                     memory_order_relaxed)) {
            task = NULL;
        }
        atomic_store_explicit(&deque->bottom, top + 1U, memory_order_relaxed);
    }

    if (task != NULL) {
        deque->buffer[bottom & (LLAM_NORM_QUEUE_CAP - 1U)] = NULL;
    }
    return task;
}

/**
 * @brief Steal a task from the top of another shard's Chase-Lev deque.
 *
 * @param deque Victim deque.
 * @return Stolen task on success, or NULL on empty/lost race.
 */
static llam_task_t *llam_cldeque_steal_top(llam_cldeque_t *deque) {
    size_t top;
    size_t bottom;
    llam_task_t *task;

    if (deque == NULL) {
        return NULL;
    }

    top = atomic_load_explicit(&deque->top, memory_order_acquire);
    atomic_thread_fence(memory_order_seq_cst);
    bottom = atomic_load_explicit(&deque->bottom, memory_order_acquire);
    if (top >= bottom) {
        return NULL;
    }

    task = deque->buffer[top & (LLAM_NORM_QUEUE_CAP - 1U)];
    if (task == NULL) {
        return NULL;
    }

    {
        size_t expected = top;

        if (!atomic_compare_exchange_strong_explicit(&deque->top,
                                                     &expected,
                                                     top + 1U,
                                                     memory_order_seq_cst,
                                                     memory_order_relaxed)) {
            return NULL;
        }
    }

    deque->buffer[top & (LLAM_NORM_QUEUE_CAP - 1U)] = NULL;
    return task;
}

/**
 * @brief Push a task into the owner normal lane while the shard lock is held.
 *
 * @param shard Owner shard.
 * @param task  Task to enqueue.
 * @return true on queue success, false if overflow handling was needed.
 */
bool llam_norm_queue_push_owner_locked(llam_shard_t *shard, llam_task_t *task) {
    bool pushed;

    if (shard == NULL || task == NULL) {
        return false;
    }

    if (llam_lockfree_normq_enabled(shard->runtime)) {
        pushed = llam_cldeque_push_bottom(&shard->norm_cldeque, task);
    } else {
        pushed = llam_queue_push_bounded_locked(shard, &shard->norm_q, LLAM_NORM_QUEUE_CAP, task);
    }

    if (pushed) {
        atomic_fetch_add_explicit(&shard->norm_depth, 1U, memory_order_release);
        shard->metrics.norm_enqueues += 1U;
        return true;
    }

    if (llam_lockfree_normq_enabled(shard->runtime)) {
        shard->metrics.queue_overflows += 1U;
        // Lock-free deque overflow spills to the runtime overflow queue so the
        // task is not lost when the bounded deque is saturated.
        llam_enqueue_overflow_task(shard->runtime, task);
    }
    return false;
}

/**
 * @brief Enqueue a yielding current task with FIFO behavior.
 *
 * @param shard Owner shard.
 * @param task  Yielding task.
 * @return true on success.
 */
bool llam_norm_queue_push_yield_locked(llam_shard_t *shard, llam_task_t *task) {
    bool pushed;

    if (shard == NULL || task == NULL) {
        return false;
    }
    if (!llam_lockfree_normq_enabled(shard->runtime)) {
        return llam_norm_queue_push_owner_locked(shard, task);
    }

    /*
     * Owner pops the Chase-Lev deque LIFO. Yielded tasks need FIFO behavior so
     * a cooperative handoff can run older peer work before resuming itself.
     */
    pushed = llam_queue_push_bounded_locked(shard, &shard->norm_q, LLAM_NORM_QUEUE_CAP, task);
    if (pushed) {
        atomic_fetch_add_explicit(&shard->norm_depth, 1U, memory_order_release);
        shard->metrics.norm_enqueues += 1U;
    }
    return pushed;
}

/**
 * @brief Pop the next normal-lane task for the owner shard.
 *
 * @param shard Owner shard.
 * @return Runnable task, or NULL when the normal lane is empty.
 */
llam_task_t *llam_norm_queue_pop_owner_locked(llam_shard_t *shard) {
    llam_task_t *task;

    if (shard == NULL) {
        return NULL;
    }

    if (llam_lockfree_normq_enabled(shard->runtime)) {
        task = llam_cldeque_pop_bottom(&shard->norm_cldeque);
        if (task == NULL) {
            task = llam_queue_pop_head(&shard->norm_q);
        }
    } else {
        task = llam_queue_pop_head(&shard->norm_q);
    }

    if (task != NULL) {
        atomic_fetch_sub_explicit(&shard->norm_depth, 1U, memory_order_release);
    }
    return task;
}

/**
 * @brief Try to steal a normal-lane task from another shard.
 *
 * @param victim Victim shard.
 * @return Stolen task, or NULL if stealing is unavailable or lost a race.
 */
llam_task_t *llam_norm_queue_steal(llam_shard_t *victim) {
    llam_task_t *task;

    if (victim == NULL || !llam_lockfree_normq_enabled(victim->runtime)) {
        return NULL;
    }

    task = llam_cldeque_steal_top(&victim->norm_cldeque);
    if (task != NULL) {
        atomic_fetch_sub_explicit(&victim->norm_depth, 1U, memory_order_release);
    }
    return task;
}
