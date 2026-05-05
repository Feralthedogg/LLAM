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

/**
 * @brief Allocate a bounded runtime channel.
 *
 * @param capacity Number of pointer slots in the channel buffer.
 *
 * @return New channel on success, or @c NULL with @c errno set.
 */
nm_channel_t *nm_channel_create(size_t capacity) {
    nm_channel_t *channel;

    if (capacity == 0U) {
        errno = EINVAL;
        return NULL;
    }

    if (capacity == 1U) {
        channel = nm_channel_cache_acquire();
        if (channel != NULL) {
            return channel;
        }
    }

    channel = calloc(1, sizeof(*channel));
    if (channel == NULL) {
        return NULL;
    }

    channel->buffer = calloc(capacity, sizeof(*channel->buffer));
    if (channel->buffer == NULL) {
        free(channel);
        return NULL;
    }
    channel->capacity = capacity;

    if (pthread_mutex_init(&channel->lock, NULL) != 0) {
        free(channel->buffer);
        free(channel);
        errno = ENOMEM;
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
static bool nm_channel_should_handoff_to_waiter(const nm_wait_node_t *node, bool has_deadline) {
    if (g_nm_runtime.channel_local_handoff_enabled == 0U || has_deadline) {
        return false;
    }
    if (node == NULL || node->task == NULL || g_nm_tls_shard == NULL || g_nm_tls_task == NULL) {
        return false;
    }
    if (node->task->parked_shard != g_nm_tls_shard->id || node->task->last_shard != g_nm_tls_shard->id) {
        return false;
    }
    return true;
}

/**
 * @brief Yield after a channel handoff without marking it as a normal user yield.
 */
static void nm_channel_handoff_yield(void) {
    g_nm_tls_io_handoff_yield += 1U;
    nm_yield();
    g_nm_tls_io_handoff_yield -= 1U;
}

/**
 * @brief Wake a channel waiter on its parked shard.
 *
 * @param node   Wait node to wake.
 * @param reason Wait reason used for tracing and metrics.
 */
static void nm_channel_wake_waiter(nm_wait_node_t *node, nm_wait_reason_t reason) {
    if (node == NULL || node->task == NULL) {
        return;
    }
    nm_reinject_task_on_shard(&g_nm_runtime,
                              node->task,
                              node->task->parked_shard,
                              true,
                              NM_TRACE_WAKE,
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
static bool nm_channel_wake_waiter_and_handoff(nm_wait_node_t *node, nm_wait_reason_t reason) {
    if (node == NULL || node->task == NULL || g_nm_tls_shard == NULL || g_nm_tls_task == NULL) {
        return false;
    }
    return nm_reinject_task_on_shard_and_yield_current(&g_nm_runtime,
                                                       node->task,
                                                       node->task->parked_shard,
                                                       true,
                                                       NM_TRACE_WAKE,
                                                       reason);
}

/**
 * @brief Destroy a runtime channel.
 *
 * The caller must ensure no tasks are currently waiting on the channel.
 *
 * @param channel Channel to destroy; may be @c NULL.
 */
void nm_channel_destroy(nm_channel_t *channel) {
    if (channel == NULL) {
        return;
    }

    if (nm_channel_cache_release(channel)) {
        return;
    }

    pthread_mutex_destroy(&channel->lock);
    free(channel->buffer);
    free(channel);
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
int nm_channel_send_impl(nm_channel_t *channel, void *value, bool has_deadline, uint64_t deadline_ns) {
    nm_wait_node_t *receiver;
    nm_wait_node_t *node;
    int rc;

    nm_task_safepoint();

    if (channel == NULL || !g_nm_runtime.initialized || g_nm_tls_task == NULL || g_nm_tls_shard == NULL) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&channel->lock);
    if (channel->closed) {
        pthread_mutex_unlock(&channel->lock);
        errno = EPIPE;
        return -1;
    }

    receiver = nm_wait_queue_pop_head(&channel->recv_waiters);
    if (receiver != NULL) {
        bool handoff = nm_channel_should_handoff_to_waiter(receiver, has_deadline);

        receiver->value = value;
        receiver->error_code = 0;
        pthread_mutex_unlock(&channel->lock);
        if (handoff && nm_channel_wake_waiter_and_handoff(receiver, NM_WAIT_CHANNEL_RECV)) {
            return 0;
        }
        nm_channel_wake_waiter(receiver, NM_WAIT_CHANNEL_RECV);
        if (handoff) {
            nm_channel_handoff_yield();
        }
        return 0;
    }

    if (channel->count < channel->capacity) {
        if (channel->capacity == 1U) {
            channel->buffer[0] = value;
        } else {
            channel->buffer[channel->tail] = value;
            channel->tail = (channel->tail + 1U) % channel->capacity;
        }
        channel->count += 1U;
        pthread_mutex_unlock(&channel->lock);
        return 0;
    }
    if (has_deadline && nm_deadline_passed(deadline_ns)) {
        pthread_mutex_unlock(&channel->lock);
        errno = ETIMEDOUT;
        return -1;
    }

    node = nm_sync_wait_node_acquire(g_nm_tls_shard);
    if (node == NULL) {
        pthread_mutex_unlock(&channel->lock);
        errno = ENOMEM;
        return -1;
    }

    node->task = g_nm_tls_task;
    node->owner_shard = g_nm_tls_shard->id;
    node->value = value;
    nm_wait_queue_push_tail(&channel->send_waiters, node);
    pthread_mutex_unlock(&channel->lock);
    nm_task_set_wait_node_tracking(g_nm_tls_task, node, &channel->send_waiters, &channel->lock, g_nm_tls_shard->id);
    g_nm_tls_task->state = NM_TASK_STATE_PARKED;
    g_nm_tls_task->wait_reason = NM_WAIT_CHANNEL_SEND;
    if (has_deadline && nm_arm_task_wait_deadline(g_nm_tls_task, g_nm_tls_shard, deadline_ns) != 0) {
        pthread_mutex_lock(&channel->lock);
        (void)nm_wait_queue_remove(&channel->send_waiters, node);
        pthread_mutex_unlock(&channel->lock);
        g_nm_tls_task->state = NM_TASK_STATE_RUNNING;
        g_nm_tls_task->wait_reason = NM_WAIT_NONE;
        nm_task_clear_wait_tracking(g_nm_tls_task);
        nm_sync_wait_node_release(g_nm_tls_shard, node);
        return -1;
    }
    if (g_nm_tls_task->cancel_token != NULL && nm_cancel_token_register_task(g_nm_tls_task) != 0) {
        nm_disarm_task_wait_deadline(g_nm_tls_task);
        pthread_mutex_lock(&channel->lock);
        (void)nm_wait_queue_remove(&channel->send_waiters, node);
        pthread_mutex_unlock(&channel->lock);
        g_nm_tls_task->state = NM_TASK_STATE_RUNNING;
        g_nm_tls_task->wait_reason = NM_WAIT_NONE;
        nm_task_clear_wait_tracking(g_nm_tls_task);
        nm_sync_wait_node_release(g_nm_tls_shard, node);
        return -1;
    }

    nm_park_current_task(NM_WAIT_CHANNEL_SEND, NM_TRACE_STATE);
    if (g_nm_tls_task->cancel_registered) {
        nm_cancel_token_unregister_task(g_nm_tls_task);
    }
    nm_task_clear_wait_tracking(g_nm_tls_task);
    rc = node->error_code;
    nm_sync_wait_node_release(g_nm_tls_shard, node);
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
int nm_channel_send(nm_channel_t *channel, void *value) {
    return nm_channel_send_impl(channel, value, false, 0U);
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
int nm_channel_send_until(nm_channel_t *channel, void *value, uint64_t deadline_ns) {
    return nm_channel_send_impl(channel, value, true, deadline_ns);
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
 * @return Received pointer on success, or @c NULL with @c errno set on error.
 */
static void *nm_channel_recv_impl(nm_channel_t *channel, bool has_deadline, uint64_t deadline_ns) {
    nm_wait_node_t *sender;
    nm_wait_node_t *node;
    void *value;
    int rc;

    nm_task_safepoint();

    if (channel == NULL || !g_nm_runtime.initialized || g_nm_tls_task == NULL || g_nm_tls_shard == NULL) {
        errno = EINVAL;
        return NULL;
    }

    pthread_mutex_lock(&channel->lock);
    if (channel->count > 0U) {
        if (channel->capacity == 1U) {
            value = channel->buffer[0];
        } else {
            value = channel->buffer[channel->head];
            channel->head = (channel->head + 1U) % channel->capacity;
        }
        channel->count -= 1U;

        sender = nm_wait_queue_pop_head(&channel->send_waiters);
        if (sender != NULL) {
            if (channel->capacity == 1U) {
                channel->buffer[0] = sender->value;
            } else {
                channel->buffer[channel->tail] = sender->value;
                channel->tail = (channel->tail + 1U) % channel->capacity;
            }
            channel->count += 1U;
            sender->error_code = 0;
        }
        pthread_mutex_unlock(&channel->lock);
        if (sender != NULL) {
            nm_channel_wake_waiter(sender, NM_WAIT_CHANNEL_SEND);
        }
        return value;
    }

    sender = nm_wait_queue_pop_head(&channel->send_waiters);
    if (sender != NULL) {
        value = sender->value;
        sender->error_code = 0;
        pthread_mutex_unlock(&channel->lock);
        nm_channel_wake_waiter(sender, NM_WAIT_CHANNEL_SEND);
        return value;
    }

    if (channel->closed) {
        pthread_mutex_unlock(&channel->lock);
        errno = EPIPE;
        return NULL;
    }
    if (has_deadline && nm_deadline_passed(deadline_ns)) {
        pthread_mutex_unlock(&channel->lock);
        errno = ETIMEDOUT;
        return NULL;
    }

    node = nm_sync_wait_node_acquire(g_nm_tls_shard);
    if (node == NULL) {
        pthread_mutex_unlock(&channel->lock);
        errno = ENOMEM;
        return NULL;
    }

    node->task = g_nm_tls_task;
    node->owner_shard = g_nm_tls_shard->id;
    nm_wait_queue_push_tail(&channel->recv_waiters, node);
    pthread_mutex_unlock(&channel->lock);
    nm_task_set_wait_node_tracking(g_nm_tls_task, node, &channel->recv_waiters, &channel->lock, g_nm_tls_shard->id);
    g_nm_tls_task->state = NM_TASK_STATE_PARKED;
    g_nm_tls_task->wait_reason = NM_WAIT_CHANNEL_RECV;
    if (has_deadline && nm_arm_task_wait_deadline(g_nm_tls_task, g_nm_tls_shard, deadline_ns) != 0) {
        pthread_mutex_lock(&channel->lock);
        (void)nm_wait_queue_remove(&channel->recv_waiters, node);
        pthread_mutex_unlock(&channel->lock);
        g_nm_tls_task->state = NM_TASK_STATE_RUNNING;
        g_nm_tls_task->wait_reason = NM_WAIT_NONE;
        nm_task_clear_wait_tracking(g_nm_tls_task);
        nm_sync_wait_node_release(g_nm_tls_shard, node);
        return NULL;
    }
    if (g_nm_tls_task->cancel_token != NULL && nm_cancel_token_register_task(g_nm_tls_task) != 0) {
        nm_disarm_task_wait_deadline(g_nm_tls_task);
        pthread_mutex_lock(&channel->lock);
        (void)nm_wait_queue_remove(&channel->recv_waiters, node);
        pthread_mutex_unlock(&channel->lock);
        g_nm_tls_task->state = NM_TASK_STATE_RUNNING;
        g_nm_tls_task->wait_reason = NM_WAIT_NONE;
        nm_task_clear_wait_tracking(g_nm_tls_task);
        nm_sync_wait_node_release(g_nm_tls_shard, node);
        errno = ECANCELED;
        return NULL;
    }

    nm_park_current_task(NM_WAIT_CHANNEL_RECV, NM_TRACE_STATE);
    if (g_nm_tls_task->cancel_registered) {
        nm_cancel_token_unregister_task(g_nm_tls_task);
    }
    nm_task_clear_wait_tracking(g_nm_tls_task);
    value = node->value;
    rc = node->error_code;
    nm_sync_wait_node_release(g_nm_tls_shard, node);
    if (rc != 0) {
        errno = rc;
        return NULL;
    }
    return value;
}

/**
 * @brief Receive a pointer from a channel without a timeout.
 *
 * @param channel Channel to receive from.
 *
 * @return Received pointer on success, or @c NULL with @c errno set.
 */
void *nm_channel_recv(nm_channel_t *channel) {
    return nm_channel_recv_impl(channel, false, 0U);
}

/**
 * @brief Receive a pointer from a channel until an absolute deadline.
 *
 * @param channel     Channel to receive from.
 * @param deadline_ns Absolute monotonic deadline in nanoseconds.
 *
 * @return Received pointer on success, or @c NULL with @c errno set.
 */
void *nm_channel_recv_until(nm_channel_t *channel, uint64_t deadline_ns) {
    return nm_channel_recv_impl(channel, true, deadline_ns);
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
int nm_channel_close(nm_channel_t *channel) {
    if (channel == NULL) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&channel->lock);
    if (channel->closed) {
        pthread_mutex_unlock(&channel->lock);
        return 0;
    }
    channel->closed = true;
    nm_wake_wait_queue_all(&channel->send_waiters, EPIPE, NM_WAIT_CHANNEL_SEND);
    nm_wake_wait_queue_all(&channel->recv_waiters, EPIPE, NM_WAIT_CHANNEL_RECV);
    pthread_mutex_unlock(&channel->lock);
    return 0;
}
