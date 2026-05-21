/**
 * @file src/core/runtime_channel.c
 * @brief Runtime-aware channel implementation for pointer messages.
 *
 * @details
 * Channels are bounded FIFO queues carrying pointer-sized messages. Senders and
 * receivers park on separate wait queues when the buffer is full or empty. A
 * capacity-one cache handles the common rendezvous/single-slot case without
 * repeated heap allocation.
 *
 * Local handoff is an optimization for same-shard send-to-receiver wakeups: the
 * producer can yield immediately after waking a local receiver, reducing latency
 * for ping-pong workloads without affecting timed waits.
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

static _Thread_local unsigned g_llam_tls_channel_safepoint_ops;

/**
 * @brief Round a requested channel capacity to the internal ring size.
 *
 * Power-of-two ring sizes let hot send/recv paths advance indices with a
 * bitmask instead of an integer modulo while preserving the exact public
 * bounded capacity separately.
 */
static size_t llam_channel_round_capacity(size_t capacity) {
    size_t rounded = 1U;

    if (capacity == 0U) {
        return 0U;
    }
    if (capacity > (SIZE_MAX / 2U) + 1U) {
        return 0U;
    }
    while (rounded < capacity) {
        if (rounded > SIZE_MAX / 2U) {
            return 0U;
        }
        rounded <<= 1U;
    }
    return rounded;
}

/**
 * @brief Allocate a bounded runtime channel.
 *
 * @param capacity Number of pointer slots in the channel buffer.
 *
 * @return New channel on success, or @c NULL with @c errno set.
 */
llam_channel_t *llam_channel_create(size_t capacity) {
    llam_channel_t *channel;
    size_t ring_capacity;
    int rc;

    if (capacity == 0U) {
        errno = EINVAL;
        return NULL;
    }
    ring_capacity = llam_channel_round_capacity(capacity);
    if (ring_capacity == 0U) {
        errno = ENOMEM;
        return NULL;
    }

    if (capacity == 1U) {
        channel = llam_channel_cache_acquire();
        if (channel != NULL) {
            return channel;
        }
    }

    channel = calloc(1, sizeof(*channel));
    if (channel == NULL) {
        return NULL;
    }

    channel->owner_runtime = llam_runtime_owner_for_new_object();
    channel->buffer = calloc(ring_capacity, sizeof(*channel->buffer));
    if (channel->buffer == NULL) {
        free(channel);
        return NULL;
    }
    channel->capacity = capacity;
    channel->ring_capacity = ring_capacity;
    channel->mask = ring_capacity - 1U;

    rc = pthread_mutex_init(&channel->lock, NULL);
    if (rc != 0) {
        free(channel->buffer);
        free(channel);
        // Preserve the exact pthread failure so callers can distinguish
        // transient resource limits from ordinary heap exhaustion.
        errno = rc;
        return NULL;
    }

    return channel;
}

/**
 * @brief Decide whether a sender should hand off execution to a receiver.
 *
 * Handoff is limited to non-timed waits on the same shard. Timed waits avoid the
 * optimization because timeout ownership adds more wakeup races to resolve.
 *
 * @param node         Receiver wait node being woken.
 * @param has_deadline Whether the send operation is deadline-bound.
 *
 * @return @c true when local handoff is eligible.
 */
static bool llam_channel_should_handoff_to_waiter(const llam_wait_node_t *node, bool has_deadline) {
    if (g_llam_runtime.channel_local_handoff_enabled == 0U || has_deadline) {
        return false;
    }
    if (node == NULL || node->task == NULL || g_llam_tls_shard == NULL || g_llam_tls_task == NULL) {
        return false;
    }
    /*
     * The wait node is visible to producers before the waiter finishes
     * publishing task wait-tracking fields.  Use node->owner_shard, which is set
     * before enqueue, rather than racing on task->parked_shard/last_shard for
     * this optimization-only decision.
     */
    if (node->owner_shard != g_llam_tls_shard->id) {
        return false;
    }
    return true;
}

/**
 * @brief Yield after a channel handoff without marking it as a normal user yield.
 */
static void llam_channel_handoff_yield(void) {
    g_llam_tls_io_handoff_yield += 1U;
    llam_yield();
    g_llam_tls_io_handoff_yield -= 1U;
}

/**
 * @brief Throttle cooperative safepoints in pure buffered channel hot paths.
 *
 * Buffered channel send/recv loops can transfer many slots without parking.
 * Running the full safepoint path on every successful transfer adds overhead
 * without improving fairness for yield-heavy workloads, so release profiles
 * batch those safepoints while forced-yield diagnostics keep exact behavior.
 */
static void llam_channel_hot_safepoint(void) {
    unsigned interval = g_llam_runtime.channel_safepoint_interval;

    if (interval <= 1U || g_llam_runtime.forced_yield_every != 0U) {
        llam_task_safepoint();
        return;
    }
    g_llam_tls_channel_safepoint_ops += 1U;
    if (g_llam_tls_channel_safepoint_ops >= interval) {
        g_llam_tls_channel_safepoint_ops = 0U;
        llam_task_safepoint();
    }
}

/**
 * @brief Cheaply test whether a buffered channel send can hand off locally.
 *
 * The full direct-yield path remains the correctness gate. This hint only
 * avoids failed handoff attempts in single-task buffered loops.
 */
static bool llam_channel_has_local_runnable_hint(void) {
    llam_shard_t *shard = g_llam_tls_shard;
    size_t top;
    size_t bottom;

    if (shard == NULL) {
        return false;
    }
    if (shard->norm_q.depth != 0U) {
        return true;
    }
    top = atomic_load_explicit(&shard->norm_cldeque.top, memory_order_acquire);
    bottom = atomic_load_explicit(&shard->norm_cldeque.bottom, memory_order_acquire);
    return bottom > top;
}

static bool llam_channel_buffered_handoff_enabled(void) {
#if LLAM_RUNTIME_BACKEND_WINDOWS
    return g_llam_runtime.channel_local_handoff_enabled != 0U &&
           llam_channel_has_local_runnable_hint();
#else
    return false;
#endif
}

/**
 * @brief Mark a popped waiter as still owning a channel lifetime reference.
 *
 * @details
 * Closing a channel removes waiters from its queues before they resume.  Keep a
 * small in-flight count so destroy cannot recycle/free the channel while a
 * close/send/recv wake result is still waiting to be consumed by the task.
 */
static void llam_channel_waiter_popped(llam_channel_t *channel, llam_wait_node_t *node) {
    if (node != NULL && node->select_state == NULL) {
        node->scalar_value = 1;
    }
    if (channel != NULL) {
        atomic_fetch_add_explicit(&channel->inflight_waiters, 1U, memory_order_acq_rel);
    }
}

/**
 * @brief Release a channel waiter lifetime reference after the waiter resumes.
 *
 * @param channel Channel that supplied the wait completion.
 */
void llam_channel_waiter_consumed(llam_channel_t *channel) {
    if (channel != NULL) {
        atomic_fetch_sub_explicit(&channel->inflight_waiters, 1U, memory_order_acq_rel);
    }
}

/**
 * @brief Wake a channel waiter on its parked shard.
 *
 * @param node   Wait node to wake.
 * @param reason Wait reason used for tracing and metrics.
 */
static void llam_channel_wake_waiter(llam_wait_node_t *node, llam_wait_reason_t reason) {
    if (node == NULL || node->task == NULL) {
        return;
    }
    if (node->select_state != NULL) {
        if (!llam_channel_select_node_should_wake(node)) {
            return;
        }
    } else if (!llam_wait_node_prepare_wake(node)) {
        return;
    }
    llam_reinject_task_on_shard(&g_llam_runtime,
                              node->task,
                              node->task->parked_shard,
                              true,
                              LLAM_TRACE_WAKE,
                              reason);
}

/**
 * @brief Wake a channel waiter and yield the current task when possible.
 *
 * @param node   Wait node to wake.
 * @param reason Wait reason used for tracing and metrics.
 *
 * @return @c true when the current task yielded directly to the waiter.
 */
static bool llam_channel_wake_waiter_and_handoff(llam_wait_node_t *node, llam_wait_reason_t reason) {
    if (node == NULL || node->task == NULL || g_llam_tls_shard == NULL || g_llam_tls_task == NULL) {
        return false;
    }
    if (node->select_state != NULL) {
        if (!llam_channel_select_node_should_wake(node)) {
            return false;
        }
    } else if (!llam_wait_node_prepare_wake(node)) {
        return false;
    }
    if (llam_reinject_task_on_shard_and_yield_current(&g_llam_runtime,
                                                     node->task,
                                                     node->task->parked_shard,
                                                     true,
                                                     LLAM_TRACE_WAKE,
                                                     reason)) {
        return true;
    }
    if (node->select_state == NULL) {
        llam_reinject_task_on_shard(&g_llam_runtime,
                                  node->task,
                                  node->task->parked_shard,
                                  true,
                                  LLAM_TRACE_WAKE,
                                  reason);
        return true;
    }
    return false;
}

/**
 * @brief Pop the next receiver waiter that has not already been selected elsewhere.
 *
 * Multi-channel select waiters may appear in several queues at once. A stale
 * node means another channel already completed that select; skip it and keep
 * looking for a live waiter.
 */
static llam_wait_node_t *llam_channel_pop_live_receiver(llam_channel_t *channel, void *value) {
    llam_wait_node_t *receiver;

    while ((receiver = llam_wait_queue_pop_head(&channel->recv_waiters)) != NULL) {
        if (receiver->select_state != NULL) {
            if (!llam_channel_select_complete_node(receiver, value, 0)) {
                continue;
            }
        } else {
            receiver->value = value;
            receiver->error_code = 0;
        }
        llam_channel_waiter_popped(channel, receiver);
        return receiver;
    }
    return NULL;
}

/**
 * @brief Pop the next sender waiter that has not already been selected elsewhere.
 */
static llam_wait_node_t *llam_channel_pop_live_sender(llam_channel_t *channel, void **out_value) {
    llam_wait_node_t *sender;

    while ((sender = llam_wait_queue_pop_head(&channel->send_waiters)) != NULL) {
        void *value = sender->value;

        if (sender->select_state != NULL) {
            if (!llam_channel_select_complete_node(sender, value, 0)) {
                continue;
            }
        } else {
            sender->error_code = 0;
        }
        *out_value = value;
        llam_channel_waiter_popped(channel, sender);
        return sender;
    }
    return NULL;
}

/**
 * @brief Wake all channel waiters, completing select waiters at most once.
 */
static void llam_channel_wake_all_waiters(llam_channel_t *channel,
                                          llam_wait_queue_t *queue,
                                          int error_code,
                                          llam_wait_reason_t reason) {
    llam_wait_node_t *node;

    while ((node = llam_wait_queue_pop_head(queue)) != NULL) {
        if (node->select_state != NULL) {
            if (!llam_channel_select_complete_node(node, NULL, error_code)) {
                continue;
            }
        } else {
            node->error_code = error_code;
        }
        llam_channel_waiter_popped(channel, node);
        llam_wake_wait_node(node, true, reason);
    }
}

/**
 * @brief Destroy a runtime channel.
 *
 * The caller must ensure no tasks are currently waiting on the channel.
 *
 * @param channel Channel to destroy.
 *
 * @return 0 on success, or -1 with @c errno set to @c EINVAL or @c EBUSY.
 */
int llam_channel_destroy(llam_channel_t *channel) {
    if (channel == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (llam_runtime_check_object_owner(channel->owner_runtime) != 0) {
        return -1;
    }

    pthread_mutex_lock(&channel->lock);
    if (channel->count != 0U ||
        atomic_load_explicit(&channel->inflight_waiters, memory_order_acquire) != 0U ||
        channel->send_waiters.head != NULL ||
        channel->send_waiters.depth != 0U ||
        channel->recv_waiters.head != NULL ||
        channel->recv_waiters.depth != 0U) {
        pthread_mutex_unlock(&channel->lock);
        errno = EBUSY;
        return -1;
    }
    pthread_mutex_unlock(&channel->lock);

    if (llam_channel_cache_release(channel)) {
        return 0;
    }

    pthread_mutex_destroy(&channel->lock);
    free(channel->buffer);
    free(channel);
    return 0;
}

/**
 * @brief Send a pointer through a channel, optionally with a deadline.
 *
 * A waiting receiver is satisfied directly before buffered storage is used. If
 * the channel is full, the current task parks on the sender queue with optional
 * timeout and cancellation tracking.
 *
 * @param channel     Channel to send on.
 * @param value       Pointer value to send.
 * @param has_deadline Whether @p deadline_ns is active.
 * @param deadline_ns Absolute monotonic deadline in nanoseconds.
 *
 * @return 0 on success, or -1 with @c errno set.
 */
static int llam_channel_send_impl(llam_channel_t *channel, void *value, bool has_deadline, uint64_t deadline_ns) {
    llam_shard_t *shard;
    llam_task_t *task;
    llam_wait_node_t *receiver;
    llam_wait_node_t *node;
    int rc;

    if (channel == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (llam_runtime_require_object_owner(channel->owner_runtime) != 0) {
        return -1;
    }
    task = g_llam_tls_task;
    shard = g_llam_tls_shard;

    pthread_mutex_lock(&channel->lock);
    if (channel->closed) {
        pthread_mutex_unlock(&channel->lock);
        errno = EPIPE;
        return -1;
    }

    receiver = llam_channel_pop_live_receiver(channel, value);
    if (receiver != NULL) {
        bool handoff = llam_channel_should_handoff_to_waiter(receiver, has_deadline);

        pthread_mutex_unlock(&channel->lock);
        if (handoff && llam_channel_wake_waiter_and_handoff(receiver, LLAM_WAIT_CHANNEL_RECV)) {
            return 0;
        }
        llam_channel_wake_waiter(receiver, LLAM_WAIT_CHANNEL_RECV);
        if (handoff) {
            llam_channel_handoff_yield();
        }
        return 0;
    }

    if (channel->count < channel->capacity) {
        bool buffered_handoff = !has_deadline &&
                                channel->capacity == 1U &&
                                llam_channel_buffered_handoff_enabled();

        if (channel->capacity == 1U) {
            channel->buffer[0] = value;
        } else {
            channel->buffer[channel->tail] = value;
            channel->tail = (channel->tail + 1U) & channel->mask;
        }
        channel->count += 1U;
        pthread_mutex_unlock(&channel->lock);
        if (buffered_handoff &&
            llam_channel_has_local_runnable_hint() &&
            llam_yield_to_local_runnable()) {
            return 0;
        }
        llam_channel_hot_safepoint();
        return 0;
    }
    if (has_deadline && llam_deadline_passed(deadline_ns)) {
        pthread_mutex_unlock(&channel->lock);
        errno = ETIMEDOUT;
        return -1;
    }

    node = llam_sync_wait_node_acquire(shard);
    if (node == NULL) {
        pthread_mutex_unlock(&channel->lock);
        errno = ENOMEM;
        return -1;
    }

    node->task = task;
    node->owner_shard = shard->id;
    node->value = value;
    node->error_code = task->cancel_token != NULL ? ECANCELED : 0;
    llam_wait_queue_push_tail(&channel->send_waiters, node);
    pthread_mutex_unlock(&channel->lock);
    llam_task_ensure_listed(task);
    llam_task_set_wait_node_tracking(task, node, &channel->send_waiters, &channel->lock, shard->id);
    task->state = LLAM_TASK_STATE_PARKED;
    task->wait_reason = LLAM_WAIT_CHANNEL_SEND;
    if (has_deadline && llam_arm_task_wait_deadline(task, shard, deadline_ns) != 0) {
        bool removed;

        if (llam_wait_node_completed(node)) {
            goto wait_ready;
        }
        pthread_mutex_lock(&channel->lock);
        removed = llam_wait_queue_remove(&channel->send_waiters, node);
        pthread_mutex_unlock(&channel->lock);
        if (!removed) {
            /* A receiver already popped the node; park/consume that completion. */
            goto wait_ready;
        }
        task->state = LLAM_TASK_STATE_RUNNING;
        task->wait_reason = LLAM_WAIT_NONE;
        llam_task_clear_wait_tracking(task);
        llam_sync_wait_node_release(shard, node);
        return -1;
    }
    if (task->cancel_token != NULL && llam_cancel_token_register_task(task) != 0) {
        bool removed;

        if (llam_wait_node_completed(node)) {
            goto wait_ready;
        }
        llam_disarm_task_wait_deadline(task);
        pthread_mutex_lock(&channel->lock);
        removed = llam_wait_queue_remove(&channel->send_waiters, node);
        pthread_mutex_unlock(&channel->lock);
        if (!removed) {
            /* A receiver already popped the node; park/consume that completion. */
            goto wait_ready;
        }
        task->state = LLAM_TASK_STATE_RUNNING;
        task->wait_reason = LLAM_WAIT_NONE;
        llam_task_clear_wait_tracking(task);
        llam_sync_wait_node_release(shard, node);
        return -1;
    }

wait_ready:
    if (llam_wait_node_should_park(node)) {
        llam_park_current_task(LLAM_WAIT_CHANNEL_SEND, LLAM_TRACE_STATE);
    }
    if (has_deadline) {
        // A receiver may consume this node before deadline arming completes;
        // clear any timer the wake path could not have seen yet.
        llam_disarm_task_wait_deadline(task);
    }
    llam_cancel_token_unregister_task(task);
    llam_task_clear_wait_tracking(task);
    rc = node->error_code;
    if (node->scalar_value != 0) {
        llam_channel_waiter_consumed(channel);
    }
    llam_sync_wait_node_release(shard, node);
    if (rc != 0) {
        errno = rc;
        return -1;
    }
    return 0;
}

/**
 * @brief Send a pointer through a channel without a timeout.
 *
 * @param channel Channel to send on.
 * @param value   Pointer value to send.
 *
 * @return 0 on success, or -1 with @c errno set.
 */
int llam_channel_send(llam_channel_t *channel, void *value) {
    return llam_channel_send_impl(channel, value, false, 0U);
}

/**
 * @brief Try to send a pointer through a channel without parking.
 *
 * @param channel Channel to send on.
 * @param value   Pointer value to send.
 *
 * @return 0 on success, or -1 with @c errno set.
 */
int llam_channel_try_send(llam_channel_t *channel, void *value) {
    llam_wait_node_t *receiver;

    if (channel == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (!g_llam_runtime.initialized) {
        errno = ENOTSUP;
        return -1;
    }
    if (llam_runtime_check_object_owner(channel->owner_runtime) != 0) {
        return -1;
    }

    pthread_mutex_lock(&channel->lock);
    if (channel->closed) {
        pthread_mutex_unlock(&channel->lock);
        errno = EPIPE;
        return -1;
    }

    receiver = llam_channel_pop_live_receiver(channel, value);
    if (receiver != NULL) {
        pthread_mutex_unlock(&channel->lock);
        llam_channel_wake_waiter(receiver, LLAM_WAIT_CHANNEL_RECV);
        return 0;
    }

    if (channel->count < channel->capacity) {
        if (channel->capacity == 1U) {
            channel->buffer[0] = value;
        } else {
            channel->buffer[channel->tail] = value;
            channel->tail = (channel->tail + 1U) & channel->mask;
        }
        channel->count += 1U;
        pthread_mutex_unlock(&channel->lock);
        llam_channel_hot_safepoint();
        return 0;
    }

    pthread_mutex_unlock(&channel->lock);
    errno = EAGAIN;
    return -1;
}

/**
 * @brief Send a pointer through a channel until an absolute deadline.
 *
 * @param channel     Channel to send on.
 * @param value       Pointer value to send.
 * @param deadline_ns Absolute monotonic deadline in nanoseconds.
 *
 * @return 0 on success, or -1 with @c errno set.
 */
int llam_channel_send_until(llam_channel_t *channel, void *value, uint64_t deadline_ns) {
    return llam_channel_send_impl(channel, value, true, deadline_ns);
}

/**
 * @brief Receive a pointer from a channel, optionally with a deadline.
 *
 * Buffered values are consumed first. If senders are parked, one sender is used
 * either to refill the buffer or to satisfy the receive directly. Empty channels
 * park the current task on the receiver queue unless closed or timed out.
 *
 * @param channel      Channel to receive from.
 * @param has_deadline Whether @p deadline_ns is active.
 * @param deadline_ns  Absolute monotonic deadline in nanoseconds.
 *
 * @return 0 on success, or -1 with @c errno set on error.
 */
static int llam_channel_recv_result_impl(llam_channel_t *channel,
                                         bool has_deadline,
                                         uint64_t deadline_ns,
                                         void **out) {
    llam_shard_t *shard;
    llam_task_t *task;
    llam_wait_node_t *sender;
    llam_wait_node_t *node;
    void *value;
    void *refill_value;
    int rc;

    if (channel == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (llam_runtime_require_object_owner(channel->owner_runtime) != 0) {
        return -1;
    }
    task = g_llam_tls_task;
    shard = g_llam_tls_shard;
    *out = NULL;

    pthread_mutex_lock(&channel->lock);
    if (channel->count > 0U) {
        if (channel->capacity == 1U) {
            value = channel->buffer[0];
        } else {
            value = channel->buffer[channel->head];
            channel->head = (channel->head + 1U) & channel->mask;
        }
        channel->count -= 1U;

        sender = llam_channel_pop_live_sender(channel, &refill_value);
        if (sender != NULL) {
            if (channel->capacity == 1U) {
                channel->buffer[0] = refill_value;
            } else {
                channel->buffer[channel->tail] = refill_value;
                channel->tail = (channel->tail + 1U) & channel->mask;
            }
            channel->count += 1U;
        }
        pthread_mutex_unlock(&channel->lock);
        if (sender != NULL) {
            llam_channel_wake_waiter(sender, LLAM_WAIT_CHANNEL_SEND);
        } else if (has_deadline ||
                   channel->capacity != 1U ||
                   g_llam_runtime.channel_local_handoff_enabled == 0U) {
            llam_channel_hot_safepoint();
        }
        *out = value;
        return 0;
    }

    sender = llam_channel_pop_live_sender(channel, &value);
    if (sender != NULL) {
        pthread_mutex_unlock(&channel->lock);
        llam_channel_wake_waiter(sender, LLAM_WAIT_CHANNEL_SEND);
        *out = value;
        return 0;
    }

    if (channel->closed) {
        pthread_mutex_unlock(&channel->lock);
        errno = EPIPE;
        return -1;
    }
    if (has_deadline && llam_deadline_passed(deadline_ns)) {
        pthread_mutex_unlock(&channel->lock);
        errno = ETIMEDOUT;
        return -1;
    }

    node = llam_sync_wait_node_acquire(shard);
    if (node == NULL) {
        pthread_mutex_unlock(&channel->lock);
        errno = ENOMEM;
        return -1;
    }

    node->task = task;
    node->owner_shard = shard->id;
    node->error_code = task->cancel_token != NULL ? ECANCELED : 0;
    llam_wait_queue_push_tail(&channel->recv_waiters, node);
    pthread_mutex_unlock(&channel->lock);
    llam_task_ensure_listed(task);
    llam_task_set_wait_node_tracking(task, node, &channel->recv_waiters, &channel->lock, shard->id);
    task->state = LLAM_TASK_STATE_PARKED;
    task->wait_reason = LLAM_WAIT_CHANNEL_RECV;
    if (has_deadline && llam_arm_task_wait_deadline(task, shard, deadline_ns) != 0) {
        bool removed;

        if (llam_wait_node_completed(node)) {
            goto wait_ready;
        }
        pthread_mutex_lock(&channel->lock);
        removed = llam_wait_queue_remove(&channel->recv_waiters, node);
        pthread_mutex_unlock(&channel->lock);
        if (!removed) {
            /* A sender/close path already popped the node; consume that result. */
            goto wait_ready;
        }
        task->state = LLAM_TASK_STATE_RUNNING;
        task->wait_reason = LLAM_WAIT_NONE;
        llam_task_clear_wait_tracking(task);
        llam_sync_wait_node_release(shard, node);
        return -1;
    }
    if (task->cancel_token != NULL && llam_cancel_token_register_task(task) != 0) {
        bool removed;

        if (llam_wait_node_completed(node)) {
            goto wait_ready;
        }
        llam_disarm_task_wait_deadline(task);
        pthread_mutex_lock(&channel->lock);
        removed = llam_wait_queue_remove(&channel->recv_waiters, node);
        pthread_mutex_unlock(&channel->lock);
        if (!removed) {
            /* A sender/close path already popped the node; consume that result. */
            goto wait_ready;
        }
        task->state = LLAM_TASK_STATE_RUNNING;
        task->wait_reason = LLAM_WAIT_NONE;
        llam_task_clear_wait_tracking(task);
        llam_sync_wait_node_release(shard, node);
        errno = ECANCELED;
        return -1;
    }

wait_ready:
    if (llam_wait_node_should_park(node)) {
        llam_park_current_task(LLAM_WAIT_CHANNEL_RECV, LLAM_TRACE_STATE);
    }
    if (has_deadline) {
        // A sender may satisfy this node before deadline arming completes;
        // clear any timer the wake path could not have seen yet.
        llam_disarm_task_wait_deadline(task);
    }
    llam_cancel_token_unregister_task(task);
    llam_task_clear_wait_tracking(task);
    value = node->value;
    rc = node->error_code;
    if (node->scalar_value != 0) {
        llam_channel_waiter_consumed(channel);
    }
    llam_sync_wait_node_release(shard, node);
    if (rc != 0) {
        errno = rc;
        return -1;
    }
    *out = value;
    return 0;
}

/**
 * @brief Receive a pointer from a channel without a timeout.
 *
 * @param channel Channel to receive from.
 * @param out     Destination for the received pointer.
 *
 * @return 0 on success, or -1 with @c errno set.
 */
int llam_channel_recv_result(llam_channel_t *channel, void **out) {
    return llam_channel_recv_result_impl(channel, false, 0U, out);
}

/**
 * @brief Receive a pointer from a channel until an absolute deadline.
 *
 * @param channel     Channel to receive from.
 * @param deadline_ns Absolute monotonic deadline in nanoseconds.
 * @param out         Destination for the received pointer.
 *
 * @return 0 on success, or -1 with @c errno set.
 */
int llam_channel_recv_until_result(llam_channel_t *channel, uint64_t deadline_ns, void **out) {
    return llam_channel_recv_result_impl(channel, true, deadline_ns, out);
}

/**
 * @brief Try to receive a pointer from a channel without parking.
 *
 * @param channel Channel to receive from.
 * @param out     Destination for the received pointer.
 *
 * @return 0 on success, or -1 with @c errno set.
 */
int llam_channel_try_recv_result(llam_channel_t *channel, void **out) {
    llam_wait_node_t *sender;
    void *value;
    void *refill_value;

    if (channel == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (llam_runtime_check_object_owner(channel->owner_runtime) != 0) {
        return -1;
    }
    *out = NULL;

    pthread_mutex_lock(&channel->lock);
    if (!g_llam_runtime.initialized) {
        /*
         * Channels can outlive the scheduler singleton.  Host cleanup code may
         * still need to drain already-buffered values before destroy; do that
         * without touching parked senders, which require a live runtime to wake.
         */
        if (channel->count > 0U) {
            if (channel->capacity == 1U) {
                value = channel->buffer[0];
            } else {
                value = channel->buffer[channel->head];
                channel->head = (channel->head + 1U) & channel->mask;
            }
            channel->count -= 1U;
            pthread_mutex_unlock(&channel->lock);
            *out = value;
            return 0;
        }
        if (channel->closed) {
            pthread_mutex_unlock(&channel->lock);
            errno = EPIPE;
            return -1;
        }
        pthread_mutex_unlock(&channel->lock);
        errno = ENOTSUP;
        return -1;
    }

    if (channel->count > 0U) {
        if (channel->capacity == 1U) {
            value = channel->buffer[0];
        } else {
            value = channel->buffer[channel->head];
            channel->head = (channel->head + 1U) & channel->mask;
        }
        channel->count -= 1U;

        sender = llam_channel_pop_live_sender(channel, &refill_value);
        if (sender != NULL) {
            if (channel->capacity == 1U) {
                channel->buffer[0] = refill_value;
            } else {
                channel->buffer[channel->tail] = refill_value;
                channel->tail = (channel->tail + 1U) & channel->mask;
            }
            channel->count += 1U;
        }
        pthread_mutex_unlock(&channel->lock);
        if (sender != NULL) {
            llam_channel_wake_waiter(sender, LLAM_WAIT_CHANNEL_SEND);
        } else if (channel->capacity != 1U ||
                   g_llam_runtime.channel_local_handoff_enabled == 0U) {
            llam_channel_hot_safepoint();
        }
        *out = value;
        return 0;
    }

    sender = llam_channel_pop_live_sender(channel, &value);
    if (sender != NULL) {
        pthread_mutex_unlock(&channel->lock);
        llam_channel_wake_waiter(sender, LLAM_WAIT_CHANNEL_SEND);
        *out = value;
        return 0;
    }

    if (channel->closed) {
        pthread_mutex_unlock(&channel->lock);
        errno = EPIPE;
        return -1;
    }

    pthread_mutex_unlock(&channel->lock);
    errno = EAGAIN;
    return -1;
}

/**
 * @brief Receive a pointer from a channel without a timeout.
 *
 * @param channel Channel to receive from.
 *
 * @return Received pointer on success, or @c NULL with @c errno set.
 */
void *llam_channel_recv(llam_channel_t *channel) {
    llam_shard_t *shard;
    llam_task_t *task;
    llam_wait_node_t *sender;
    llam_wait_node_t *node;
    void *value;
    void *refill_value;
    int rc;

    if (channel == NULL) {
        errno = EINVAL;
        return NULL;
    }
    if (llam_runtime_require_object_owner(channel->owner_runtime) != 0) {
        return NULL;
    }
    task = g_llam_tls_task;
    shard = g_llam_tls_shard;

    pthread_mutex_lock(&channel->lock);
    if (channel->count > 0U) {
        if (channel->capacity == 1U) {
            value = channel->buffer[0];
        } else {
            value = channel->buffer[channel->head];
            channel->head = (channel->head + 1U) & channel->mask;
        }
        channel->count -= 1U;

        sender = llam_channel_pop_live_sender(channel, &refill_value);
        if (sender != NULL) {
            if (channel->capacity == 1U) {
                channel->buffer[0] = refill_value;
            } else {
                channel->buffer[channel->tail] = refill_value;
                channel->tail = (channel->tail + 1U) & channel->mask;
            }
            channel->count += 1U;
        }
        pthread_mutex_unlock(&channel->lock);
        if (sender != NULL) {
            llam_channel_wake_waiter(sender, LLAM_WAIT_CHANNEL_SEND);
        } else if (channel->capacity != 1U ||
                   g_llam_runtime.channel_local_handoff_enabled == 0U) {
            llam_channel_hot_safepoint();
        }
        if (value == NULL) {
            errno = 0;
        }
        return value;
    }

    sender = llam_channel_pop_live_sender(channel, &value);
    if (sender != NULL) {
        pthread_mutex_unlock(&channel->lock);
        llam_channel_wake_waiter(sender, LLAM_WAIT_CHANNEL_SEND);
        if (value == NULL) {
            errno = 0;
        }
        return value;
    }

    if (channel->closed) {
        pthread_mutex_unlock(&channel->lock);
        errno = EPIPE;
        return NULL;
    }

    node = llam_sync_wait_node_acquire(shard);
    if (node == NULL) {
        pthread_mutex_unlock(&channel->lock);
        errno = ENOMEM;
        return NULL;
    }

    node->task = task;
    node->owner_shard = shard->id;
    node->error_code = task->cancel_token != NULL ? ECANCELED : 0;
    llam_wait_queue_push_tail(&channel->recv_waiters, node);
    pthread_mutex_unlock(&channel->lock);
    llam_task_ensure_listed(task);
    llam_task_set_wait_node_tracking(task, node, &channel->recv_waiters, &channel->lock, shard->id);
    task->state = LLAM_TASK_STATE_PARKED;
    task->wait_reason = LLAM_WAIT_CHANNEL_RECV;
    if (task->cancel_token != NULL && llam_cancel_token_register_task(task) != 0) {
        bool removed;

        if (llam_wait_node_completed(node)) {
            goto wait_ready;
        }
        pthread_mutex_lock(&channel->lock);
        removed = llam_wait_queue_remove(&channel->recv_waiters, node);
        pthread_mutex_unlock(&channel->lock);
        if (!removed) {
            /* A sender/close path already popped the node; consume that result. */
            goto wait_ready;
        }
        task->state = LLAM_TASK_STATE_RUNNING;
        task->wait_reason = LLAM_WAIT_NONE;
        llam_task_clear_wait_tracking(task);
        llam_sync_wait_node_release(shard, node);
        errno = ECANCELED;
        return NULL;
    }

wait_ready:
    if (llam_wait_node_should_park(node)) {
        llam_park_current_task(LLAM_WAIT_CHANNEL_RECV, LLAM_TRACE_STATE);
    }
    llam_cancel_token_unregister_task(task);
    llam_task_clear_wait_tracking(task);
    value = node->value;
    rc = node->error_code;
    if (node->scalar_value != 0) {
        llam_channel_waiter_consumed(channel);
    }
    llam_sync_wait_node_release(shard, node);
    if (rc != 0) {
        errno = rc;
        return NULL;
    }
    if (value == NULL) {
        errno = 0;
    }
    return value;
}

/**
 * @brief Receive a pointer from a channel until an absolute deadline.
 *
 * @param channel     Channel to receive from.
 * @param deadline_ns Absolute monotonic deadline in nanoseconds.
 *
 * @return Received pointer on success, or @c NULL with @c errno set.
 */
void *llam_channel_recv_until(llam_channel_t *channel, uint64_t deadline_ns) {
    void *value = NULL;

    if (llam_channel_recv_until_result(channel, deadline_ns, &value) != 0) {
        return NULL;
    }
    if (value == NULL) {
        errno = 0;
    }
    return value;
}

/**
 * @brief Close a channel and wake all blocked senders/receivers.
 *
 * Closing is idempotent. Waiters wake with @c EPIPE.
 *
 * @param channel Channel to close.
 *
 * @return 0 on success, or -1 with @c errno set to @c EINVAL.
 */
int llam_channel_close(llam_channel_t *channel) {
    if (channel == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (llam_runtime_check_object_owner(channel->owner_runtime) != 0) {
        return -1;
    }

    pthread_mutex_lock(&channel->lock);
    if (channel->closed) {
        pthread_mutex_unlock(&channel->lock);
        return 0;
    }
    channel->closed = true;
    llam_channel_wake_all_waiters(channel, &channel->send_waiters, EPIPE, LLAM_WAIT_CHANNEL_SEND);
    llam_channel_wake_all_waiters(channel, &channel->recv_waiters, EPIPE, LLAM_WAIT_CHANNEL_RECV);
    pthread_mutex_unlock(&channel->lock);
    return 0;
}
