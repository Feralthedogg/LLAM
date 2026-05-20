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

#define LLAM_DIRECT_YIELD_FIFO_FAIRNESS_BURST 8U

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
        atomic_init(&deque->buffer[i], NULL);
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

    // The slot is shared with thieves; publish it atomically before increasing
    // bottom so a thief that observes the new range can acquire the payload.
    atomic_store_explicit(&deque->buffer[bottom & (LLAM_NORM_QUEUE_CAP - 1U)],
                          task,
                          memory_order_release);
    atomic_store_explicit(&deque->bottom, bottom + 1U, memory_order_release);
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

    task = atomic_load_explicit(&deque->buffer[bottom & (LLAM_NORM_QUEUE_CAP - 1U)], memory_order_acquire);
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
        atomic_store_explicit(&deque->buffer[bottom & (LLAM_NORM_QUEUE_CAP - 1U)],
                              NULL,
                              memory_order_release);
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

    task = atomic_load_explicit(&deque->buffer[top & (LLAM_NORM_QUEUE_CAP - 1U)], memory_order_acquire);
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

    atomic_store_explicit(&deque->buffer[top & (LLAM_NORM_QUEUE_CAP - 1U)], NULL, memory_order_release);
    return task;
}

/**
 * @brief Return whether the owner-side deque has runnable work.
 *
 * Direct-yield handoff can otherwise let a pair of FIFO-yielding tasks exchange
 * with each other forever while fresh owner-deque work waits behind them.
 */
static bool llam_cldeque_has_work(llam_cldeque_t *deque) {
    size_t top;
    size_t bottom;

    if (deque == NULL) {
        return false;
    }
    top = atomic_load_explicit(&deque->top, memory_order_acquire);
    bottom = atomic_load_explicit(&deque->bottom, memory_order_acquire);
    return top < bottom;
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
 * @brief Enqueue on the owner side of the lock-free normal lane without taking shard->lock.
 *
 * @details
 * This is only valid from the shard's primary scheduler thread while a managed
 * task is running.  The Chase-Lev owner side is single-writer, so local spawn
 * bursts can avoid a pthread mutex round trip and still publish work safely to
 * thieves through the deque's release fence.
 *
 * @param shard Owner shard.
 * @param task  Task to enqueue.
 * @return true when the task was accepted by the normal lane or overflow queue.
 */
bool llam_norm_queue_push_owner_unlocked(llam_shard_t *shard, llam_task_t *task) {
    if (shard == NULL || task == NULL || !llam_lockfree_normq_enabled(shard->runtime)) {
        return false;
    }

    if (llam_cldeque_push_bottom(&shard->norm_cldeque, task)) {
        atomic_fetch_add_explicit(&shard->norm_depth, 1U, memory_order_release);
        shard->metrics.norm_enqueues += 1U;
        return true;
    }

    shard->metrics.queue_overflows += 1U;
    llam_enqueue_overflow_task(shard->runtime, task);
    return true;
}

/**
 * @brief Pop owner-side normal work without taking shard->lock.
 *
 * @details
 * Valid only on the primary scheduler thread while a managed task is running.
 * The owner is the only writer for the Chase-Lev bottom and FIFO yield lane.
 *
 * @param shard Owner shard.
 * @return Runnable task, or NULL when the owner lanes are empty.
 */
llam_task_t *llam_norm_queue_pop_owner_unlocked(llam_shard_t *shard) {
    llam_task_t *task;

    if (shard == NULL || !llam_lockfree_normq_enabled(shard->runtime)) {
        return NULL;
    }

    task = llam_cldeque_pop_bottom(&shard->norm_cldeque);
    if (task == NULL) {
        task = llam_queue_pop_head(&shard->norm_q);
    }
    if (task != NULL) {
        atomic_fetch_sub_explicit(&shard->norm_depth, 1U, memory_order_release);
    }
    return task;
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
 * @brief Enqueue a yielding task on the owner FIFO lane without shard->lock.
 *
 * @details
 * This mirrors ::llam_norm_queue_push_yield_locked for the single-owner direct
 * handoff path. It deliberately uses FIFO order so yielded tasks do not starve
 * behind fresh owner-side spawn bursts.
 *
 * @param shard Owner shard.
 * @param task  Yielding task.
 * @return true on success.
 */
bool llam_norm_queue_push_yield_unlocked(llam_shard_t *shard, llam_task_t *task) {
    if (shard == NULL || task == NULL || !llam_lockfree_normq_enabled(shard->runtime)) {
        return false;
    }
    if (shard->norm_q.depth >= LLAM_NORM_QUEUE_CAP) {
        return false;
    }

    llam_queue_push_tail(&shard->norm_q, task);
    atomic_fetch_add_explicit(&shard->norm_depth, 1U, memory_order_release);
    shard->metrics.norm_enqueues += 1U;
    return true;
}

/**
 * @brief Exchange the running task with local normal work without changing net depth.
 *
 * @details
 * Direct yield handoff pops one runnable task and queues the current task in
 * its place.  The normal-lane total depth is unchanged, so the hot path can
 * avoid the atomic decrement/increment pair used by separate pop and push
 * helpers.  The caller must be the shard owner thread.
 *
 * @param shard           Owner shard.
 * @param current         Running task to requeue on the FIFO yield lane.
 * @param out_next        Receives the runnable task to switch into.
 * @param out_push_failed Receives whether failure was caused by no FIFO space.
 * @return true when @p current was queued and @p out_next owns a runnable task.
 */
bool llam_norm_queue_exchange_yield_unlocked(llam_shard_t *shard,
                                             llam_task_t *current,
                                             llam_task_t **out_next,
                                             bool *out_push_failed) {
    llam_task_t *next = NULL;
    bool prefer_owner_deque;

    if (out_next != NULL) {
        *out_next = NULL;
    }
    if (out_push_failed != NULL) {
        *out_push_failed = false;
    }
    if (shard == NULL || current == NULL || !llam_lockfree_normq_enabled(shard->runtime)) {
        return false;
    }

    /*
     * Direct yields should prefer older yielded peers before fresh owner-side
     * spawn work. This also keeps tight ping-pong handoffs on the FIFO lane and
     * avoids an unnecessary Chase-Lev owner pop on the common exchange path.
     *
     * That preference must be bounded: if FIFO tasks keep yielding to each
     * other while owner-deque work exists, a task can starve before it reaches
     * its first blocking/I/O operation. Periodically pull from the owner deque
     * to keep direct handoff fair without forcing a scheduler round trip.
     */
    prefer_owner_deque =
        shard->direct_handoff_streak >= LLAM_DIRECT_YIELD_FIFO_FAIRNESS_BURST &&
        llam_cldeque_has_work(&shard->norm_cldeque);
    if (!prefer_owner_deque) {
        next = llam_queue_pop_head(&shard->norm_q);
    }
    if (next == NULL) {
        if (shard->norm_q.depth >= LLAM_NORM_QUEUE_CAP) {
            if (out_push_failed != NULL) {
                *out_push_failed = true;
            }
            return false;
        }
        next = llam_cldeque_pop_bottom(&shard->norm_cldeque);
        if (next == NULL) {
            return false;
        }
    }
    if (prefer_owner_deque) {
        shard->direct_handoff_streak = 0U;
    }
    if (next == current) {
        /*
         * The running task should never already be present in runnable queues.
         * Treat that as a failed direct exchange rather than queueing a second
         * reference and corrupting task ownership.
         */
        if (out_next != NULL) {
            *out_next = current;
        }
        return false;
    }

    llam_queue_push_tail(&shard->norm_q, current);
    shard->metrics.norm_enqueues += 1U;
    if (out_next != NULL) {
        *out_next = next;
    }
    return true;
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
