/**
 * @file src/core/runtime_sync.c
 * @brief Shared synchronization and wait-list primitives used by mutexes, condvars, and channels.
 *
 * @details
 * Synchronization primitives share a small wait-node abstraction. The current
 * task's embedded wait node handles the common one-wait-at-a-time case without
 * allocation; heap wait nodes are used only for nested or exceptional ownership
 * patterns.
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

enum {
    LLAM_WAIT_NODE_UNARMED = 0U,
    LLAM_WAIT_NODE_ARMED = 1U,
    LLAM_WAIT_NODE_COMPLETED_INLINE = 2U,
    LLAM_WAIT_NODE_COMPLETED_QUEUED = 3U
};

/**
 * @brief Require that the caller is running inside a managed task context.
 *
 * @return 0 when runtime, task, and shard TLS are all present.
 * @return -1 with @c errno set to @c ENOTSUP otherwise.
 */
int llam_require_task_context(void) {
    llam_runtime_t *rt;

    if (g_llam_tls_task == NULL || g_llam_tls_shard == NULL) {
        errno = ENOTSUP;
        return -1;
    }
    rt = g_llam_tls_task->owner_runtime;
    if (rt == NULL || g_llam_tls_shard->runtime != rt ||
        !atomic_load_explicit(&rt->initialized, memory_order_acquire)) {
        errno = ENOTSUP;
        return -1;
    }
    return 0;
}

/**
 * @brief Reinitialize a wait node for reuse.
 *
 * @details
 * Wait nodes contain atomic wake state used by producers and parked waiters.
 * Reset fields explicitly so embedded and heap-backed node recycling never
 * writes over an already initialized atomic object with a byte clear.
 *
 * @param node        Wait node to reset.
 * @param owner_runtime Runtime that owns the wait node.
 * @param owner_shard   Allocation-owner shard, or @c UINT_MAX for embedded nodes.
 */
void llam_wait_node_reset(llam_wait_node_t *node, llam_runtime_t *owner_runtime, unsigned owner_shard) {
    if (node == NULL) {
        return;
    }

    node->owner_runtime = owner_runtime;
    node->task = NULL;
    node->next = NULL;
    node->alloc_next = NULL;
    node->value = NULL;
    node->select_state = NULL;
    node->error_code = 0;
    node->select_kind = 0U;
    node->scalar_value = 0;
    node->owner_shard = owner_shard;
    /*
     * This helper also resets embedded nodes immediately after a producer has
     * completed them.  Use atomic stores, not atomic_init(), because the latter
     * is only valid for one-time initialization before any concurrent access.
     */
    atomic_store_explicit(&node->wake_armed, 0U, memory_order_release);
    atomic_store_explicit(&node->wake_completed, 0U, memory_order_release);
    atomic_store_explicit(&node->wake_queued, 0U, memory_order_release);
}

/**
 * @brief Add a popped-waiter destroy guard without unsigned wraparound.
 *
 * @details
 * Channel close and condition signal/broadcast remove waiters from their FIFO
 * queues before those waiters resume and consume the result.  The owning object
 * therefore keeps a small in-flight counter so destroy fails with @c EBUSY
 * while a popped waiter still holds a lifetime reference.  Saturation is a
 * corrupted-invariant condition: preserve the non-zero counter and mark the
 * runtime fatal instead of wrapping to zero and letting destroy recycle the
 * object underneath the waiter.
 *
 * @param rt      Runtime that owns the guarded object.
 * @param counter In-flight waiter counter to increment.
 * @param amount  Number of waiters to add.
 *
 * @return @c true on success, @c false with @c errno set on invalid input or
 *         saturation.
 */
bool llam_sync_note_inflight_waiter(llam_runtime_t *rt, atomic_uint *counter, unsigned amount) {
    unsigned current;

    if (amount == 0U) {
        return true;
    }
    if (rt == NULL || counter == NULL) {
        errno = EINVAL;
        if (rt != NULL) {
            llam_record_fatal(rt, EINVAL);
        }
        return false;
    }

    current = atomic_load_explicit(counter, memory_order_acquire);
    for (;;) {
        if (UINT_MAX - current < amount) {
            llam_record_fatal(rt, EOVERFLOW);
            errno = EOVERFLOW;
            return false;
        }
        if (atomic_compare_exchange_weak_explicit(counter,
                                                  &current,
                                                  current + amount,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            return true;
        }
    }
}

/**
 * @brief Drop a popped-waiter destroy guard without unsigned underflow.
 *
 * @details
 * A completion without a matching in-flight waiter means a stale waiter cleanup
 * or corrupted primitive state reached the consume path.  Preserve the counter
 * and record a fatal diagnostic rather than wrapping to @c UINT_MAX.
 *
 * @param rt      Runtime that owns the guarded object.
 * @param counter In-flight waiter counter to decrement.
 * @param amount  Number of waiters to remove.
 *
 * @return @c true on success, @c false with @c errno set on invalid input or
 *         underflow.
 */
bool llam_sync_complete_inflight_waiter(llam_runtime_t *rt, atomic_uint *counter, unsigned amount) {
    unsigned current;

    if (amount == 0U) {
        return true;
    }
    if (rt == NULL || counter == NULL) {
        errno = EINVAL;
        if (rt != NULL) {
            llam_record_fatal(rt, EINVAL);
        }
        return false;
    }

    current = atomic_load_explicit(counter, memory_order_acquire);
    while (current >= amount) {
        if (atomic_compare_exchange_weak_explicit(counter,
                                                  &current,
                                                  current - amount,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            return true;
        }
    }

    llam_record_fatal(rt, EINVAL);
    errno = EINVAL;
    return false;
}

/**
 * @brief Acquire a wait node for a synchronization primitive.
 *
 * The current task's embedded wait node is used when it is free. Otherwise a
 * shard-local heap wait node is allocated.
 *
 * @param shard Shard requesting the wait node.
 *
 * @return Wait node on success, or @c NULL on allocation failure.
 */
llam_wait_node_t *llam_sync_wait_node_acquire(llam_shard_t *shard) {
    llam_task_t *task = g_llam_tls_task;
    llam_wait_node_t *node;

    if (task != NULL &&
        atomic_load_explicit(&task->active_wait_node, memory_order_acquire) == NULL) {
        node = &task->embedded_wait_node;
        llam_wait_node_reset(node, task->owner_runtime, UINT_MAX);
        node->task = task;
        return node;
    }

    node = llam_wait_node_alloc(shard);
    if (node != NULL) {
        node->task = task;
        node->owner_shard = shard != NULL ? shard->id : UINT_MAX;
    }
    return node;
}

/**
 * @brief Release a wait node acquired by ::llam_sync_wait_node_acquire.
 *
 * Embedded nodes are zeroed in place. Heap nodes are returned to the shard-local
 * allocator.
 *
 * @param shard Shard owning the allocator.
 * @param node  Wait node to release; may be @c NULL.
 */
void llam_sync_wait_node_release(llam_shard_t *shard, llam_wait_node_t *node) {
    if (node == NULL) {
        return;
    }
    if (node->task != NULL && node == &node->task->embedded_wait_node) {
        llam_wait_node_reset(node, node->task->owner_runtime, UINT_MAX);
        return;
    }
    llam_wait_node_free(shard, node);
}

/**
 * @brief Publish wait-node completion and decide whether to enqueue the task.
 *
 * @details
 * A producer can complete a wait node after it has been queued but before the
 * waiter has committed to parking. In that case the producer records
 * completion but must not enqueue the still-running task.
 *
 * @c wake_armed is a compact state machine, not a boolean. The waiter commits
 * with a 0->1 compare/exchange; producers complete with either 0->2 for an
 * inline wake or 1->3 for a queued wake. That single atomic state prevents the
 * lost-wakeup window where separate armed/completed stores can both miss each
 * other under cross-worker races. Task state is deliberately not sampled here:
 * it is scheduler/diagnostic state, not the wait-node synchronization flag.
 *
 * @param node Wait node being completed by a synchronization primitive.
 * @return true when the caller should reinject @p node->task, false when the
 *         waiter will consume the completion inline.
 */
bool llam_wait_node_prepare_wake(llam_wait_node_t *node) {
    unsigned state;

    if (node == NULL || node->task == NULL) {
        return false;
    }

    state = atomic_load_explicit(&node->wake_armed, memory_order_acquire);
    for (;;) {
        switch (state) {
        case LLAM_WAIT_NODE_UNARMED:
            if (atomic_compare_exchange_weak_explicit(&node->wake_armed,
                                                      &state,
                                                      LLAM_WAIT_NODE_COMPLETED_INLINE,
                                                      memory_order_acq_rel,
                                                      memory_order_acquire)) {
                atomic_store_explicit(&node->wake_completed, 1U, memory_order_release);
                return false;
            }
            break;
        case LLAM_WAIT_NODE_ARMED:
            if (atomic_compare_exchange_weak_explicit(&node->wake_armed,
                                                      &state,
                                                      LLAM_WAIT_NODE_COMPLETED_QUEUED,
                                                      memory_order_acq_rel,
                                                      memory_order_acquire)) {
                atomic_store_explicit(&node->wake_queued, 1U, memory_order_release);
                atomic_store_explicit(&node->wake_completed, 1U, memory_order_release);
                return true;
            }
            break;
        default:
            return false;
        }
    }
}

/**
 * @brief Return whether a producer already completed this wait node.
 *
 * Cancellation is registered after a waiter is visible to peers.  If a peer
 * completes the node in that small window, completion must win over a
 * pre-cancelled token because the operation's value/ownership has already
 * crossed the primitive boundary.
 */
bool llam_wait_node_completed(const llam_wait_node_t *node) {
    return node != NULL &&
           atomic_load_explicit(&node->wake_completed, memory_order_acquire) != 0U;
}

/**
 * @brief Arm a wait node and decide whether the current task must park.
 *
 * @details
 * If completion already happened before arming, no runnable queue entry exists
 * and the caller should skip parking.  If completion raced after arming, the
 * producer recorded a queued wake; the caller must park to consume it.
 *
 * @param node Wait node owned by the current task.
 * @return true when the caller should park, false when completion is already
 *         available inline.
 */
bool llam_wait_node_should_park(llam_wait_node_t *node) {
    unsigned state = LLAM_WAIT_NODE_UNARMED;

    if (node == NULL) {
        return false;
    }

    if (atomic_compare_exchange_strong_explicit(&node->wake_armed,
                                                &state,
                                                LLAM_WAIT_NODE_ARMED,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        return true;
    }

    if (state == LLAM_WAIT_NODE_COMPLETED_QUEUED ||
        atomic_load_explicit(&node->wake_queued, memory_order_acquire) != 0U) {
        return true;
    }
    if (node->task != NULL) {
        node->task->state = LLAM_TASK_STATE_RUNNING;
        node->task->wait_reason = LLAM_WAIT_NONE;
    }
    return false;
}

/**
 * @brief Wake and remove every waiter from a wait queue.
 *
 * @param queue      Queue to drain.
 * @param error_code Completion error stored in each wait node.
 * @param reason     Trace/wake reason associated with the wakeup.
 */
void llam_wake_wait_queue_all(llam_wait_queue_t *queue, int error_code, llam_wait_reason_t reason) {
    llam_wait_node_t *node;

    while ((node = llam_wait_queue_pop_head(queue)) != NULL) {
        node->error_code = error_code;
        llam_wake_wait_node(node, true, reason);
    }
}
