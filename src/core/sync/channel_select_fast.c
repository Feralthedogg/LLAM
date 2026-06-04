/**
 * @file src/core/sync/channel_select_fast.c
 * @brief Fast ready probes for channel select.
 *
 * @details
 * The full select waiter state machine lives in channel_select.c.  This
 * file keeps the cache-hot ready/try path separate so select timeout and ready
 * benchmarks do not drag the parked-waiter code through the same module.
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

static int llam_channel_select_try_one_resolved(llam_select_op_t *op, llam_channel_t *channel);

int llam_channel_select_validate_op(const llam_select_op_t *op) {
    if (op == NULL || op->channel == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (op->kind == LLAM_SELECT_OP_RECV) {
        if (op->recv_out == NULL) {
            errno = EINVAL;
            return -1;
        }
        return 0;
    }
    if (op->kind == LLAM_SELECT_OP_SEND) {
        return 0;
    }
    errno = EINVAL;
    return -1;
}

int llam_channel_select_try_ready_large(llam_select_op_t *ops,
                                        size_t op_count,
                                        size_t start,
                                        size_t *selected_index) {
    llam_channel_t **channels;
    size_t i;
    int rc = LLAM_SELECT_TRY_NOT_READY;

    /*
     * Large selects cannot use the fixed inline array. Resolve and pin every
     * operand before any ready probe has side effects, then keep those pins
     * until the whole immediate pass finishes. This mirrors the inline batch
     * contract and keeps destroy from reclaiming a tail operand while an early
     * ready/fallback operation is still being evaluated.
     */
    channels = calloc(op_count, sizeof(*channels));
    if (channels == NULL) {
        errno = ENOMEM;
        return -1;
    }
    if (llam_channel_resolve_public_handles_for_select(ops, op_count, channels) != 0) {
        free(channels);
        return -1;
    }

    for (i = 0U; i < op_count; ++i) {
        size_t index = start + i;
        int selected;

        if (index >= op_count) {
            index -= op_count;
        }

        selected = llam_channel_select_try_one_resolved(&ops[index], channels[index]);
        if (selected == LLAM_SELECT_TRY_FALLBACK) {
            /*
             * Peer-waiter transfers need the public try operation, but the
             * whole select set stays pinned while it runs so a concurrent
             * destroy cannot invalidate a not-yet-scanned operand.
             */
            selected = llam_channel_select_try_one(&ops[index]);
        }
        if (selected < 0) {
            rc = -1;
            break;
        }
        if (selected > 0) {
            *selected_index = index;
            rc = LLAM_SELECT_TRY_SELECTED;
            break;
        }
    }

    llam_channel_end_public_select_ops(channels, op_count);
    free(channels);
    return rc;
}

static int llam_channel_select_try_one_resolved(llam_select_op_t *op, llam_channel_t *channel) {
    void *value;

    pthread_mutex_lock(&channel->lock);
    if (op->kind == LLAM_SELECT_OP_RECV) {
        if (channel->count > 0U) {
            if (channel->send_waiters.head != NULL) {
                pthread_mutex_unlock(&channel->lock);
                return LLAM_SELECT_TRY_FALLBACK;
            }
            if (channel->capacity == 1U) {
                value = channel->buffer[0];
            } else {
                value = channel->buffer[channel->head];
                channel->head = (channel->head + 1U) & channel->mask;
            }
            channel->count -= 1U;
            pthread_mutex_unlock(&channel->lock);
            *op->recv_out = value;
            return LLAM_SELECT_TRY_SELECTED;
        }
        if (channel->send_waiters.head != NULL) {
            pthread_mutex_unlock(&channel->lock);
            return LLAM_SELECT_TRY_FALLBACK;
        }
        if (channel->closed) {
            pthread_mutex_unlock(&channel->lock);
            *op->recv_out = NULL;
            op->result_errno = EPIPE;
            return LLAM_SELECT_TRY_SELECTED;
        }
        pthread_mutex_unlock(&channel->lock);
        return LLAM_SELECT_TRY_NOT_READY;
    }

    if (channel->closed) {
        pthread_mutex_unlock(&channel->lock);
        op->result_errno = EPIPE;
        return LLAM_SELECT_TRY_SELECTED;
    }
    if (channel->recv_waiters.head != NULL) {
        pthread_mutex_unlock(&channel->lock);
        return LLAM_SELECT_TRY_FALLBACK;
    }
    if (channel->count < channel->capacity) {
        if (channel->capacity == 1U) {
            channel->buffer[0] = op->send_value;
        } else {
            channel->buffer[channel->tail] = op->send_value;
            channel->tail = (channel->tail + 1U) & channel->mask;
        }
        channel->count += 1U;
        pthread_mutex_unlock(&channel->lock);
        return LLAM_SELECT_TRY_SELECTED;
    }
    pthread_mutex_unlock(&channel->lock);
    return LLAM_SELECT_TRY_NOT_READY;
}

int llam_channel_select_try_one_fast(llam_select_op_t *op) {
    llam_channel_t *channel;
    int rc;

    channel = llam_channel_resolve_public_handle(op->channel);
    if (channel == NULL) {
        return -1;
    }
    if (llam_runtime_check_object_owner(channel->owner_runtime) != 0) {
        llam_channel_end_public_op(channel);
        return -1;
    }
    rc = llam_channel_select_try_one_resolved(op, channel);
    llam_channel_end_public_op(channel);
    return rc;
}

static int llam_channel_select_try_ready_pair_registry(llam_select_op_t *ops, size_t *selected_index) {
#if !LLAM_RUNTIME_DISABLE_OWNER_CHECKS
    llam_runtime_t *current_owner = llam_runtime_tls_owner_fast();
#endif
    llam_channel_t *first;
    llam_channel_t *second;
    int selected;

#if !LLAM_RUNTIME_DISABLE_OWNER_CHECKS
    if (LLAM_UNLIKELY(current_owner == NULL)) {
        current_owner = llam_runtime_current_owner();
    }
#endif

    /*
     * The two-op ready/timeout path is the dominant select shape in benchmarks
     * and in Go-style channel races.  Validate both public handles before
     * touching either channel so a stale later operand still rejects the whole
     * call.  Holding the registry lock until the quick probes finish keeps the
     * objects alive without taking active-op pins on both channels.
     */
    llam_channel_public_registry_lock();
    first = llam_channel_resolve_public_handle_locked_unpinned(ops[0].channel);
    if (LLAM_UNLIKELY(first == NULL)) {
        llam_channel_public_registry_unlock();
        errno = EINVAL;
        return -1;
    }
#if !LLAM_RUNTIME_DISABLE_OWNER_CHECKS
    if (LLAM_UNLIKELY(first->owner_runtime == NULL || first->owner_runtime != current_owner)) {
        llam_channel_public_registry_unlock();
        errno = first->owner_runtime == NULL ? EINVAL : EXDEV;
        return -1;
    }
#else
    if (LLAM_UNLIKELY(first->owner_runtime == NULL)) {
        llam_channel_public_registry_unlock();
        errno = EINVAL;
        return -1;
    }
#endif
    if (LLAM_UNLIKELY(llam_public_active_op_is_saturated(
            llam_public_active_op_count(&first->active_ops)))) {
        llam_channel_public_registry_unlock();
        errno = EBUSY;
        return -1;
    }

    second = llam_channel_resolve_public_handle_locked_unpinned(ops[1].channel);
    if (LLAM_UNLIKELY(second == NULL)) {
        llam_channel_public_registry_unlock();
        errno = EINVAL;
        return -1;
    }
#if !LLAM_RUNTIME_DISABLE_OWNER_CHECKS
    if (LLAM_UNLIKELY(second->owner_runtime == NULL || second->owner_runtime != current_owner)) {
        llam_channel_public_registry_unlock();
        errno = second->owner_runtime == NULL ? EINVAL : EXDEV;
        return -1;
    }
#else
    if (LLAM_UNLIKELY(second->owner_runtime == NULL)) {
        llam_channel_public_registry_unlock();
        errno = EINVAL;
        return -1;
    }
#endif
    if (LLAM_UNLIKELY(llam_public_active_op_is_saturated(
            llam_public_active_op_count(&second->active_ops)))) {
        llam_channel_public_registry_unlock();
        errno = EBUSY;
        return -1;
    }
    if (LLAM_UNLIKELY(first == second)) {
        llam_channel_public_registry_unlock();
        return LLAM_SELECT_TRY_FALLBACK;
    }

    selected = llam_channel_select_try_one_resolved(&ops[0], first);
    if (selected < 0 || selected == LLAM_SELECT_TRY_FALLBACK) {
        llam_channel_public_registry_unlock();
        return selected;
    }
    if (selected > 0) {
        *selected_index = 0U;
        llam_channel_public_registry_unlock();
        return LLAM_SELECT_TRY_SELECTED;
    }

    selected = llam_channel_select_try_one_resolved(&ops[1], second);
    if (selected < 0 || selected == LLAM_SELECT_TRY_FALLBACK) {
        llam_channel_public_registry_unlock();
        return selected;
    }
    if (selected > 0) {
        *selected_index = 1U;
        llam_channel_public_registry_unlock();
        return LLAM_SELECT_TRY_SELECTED;
    }

    llam_channel_public_registry_unlock();
    return LLAM_SELECT_TRY_NOT_READY;
}

int llam_channel_select_try_ready_batch(llam_select_op_t *ops,
                                        size_t op_count,
                                        size_t start,
                                        size_t *selected_index) {
    llam_channel_t *channels[LLAM_CHANNEL_SELECT_INLINE_OPS];
    size_t i;

    if (op_count > LLAM_CHANNEL_SELECT_INLINE_OPS) {
        return LLAM_SELECT_TRY_FALLBACK;
    }
    if (op_count == 2U && start == 0U) {
        return llam_channel_select_try_ready_pair_registry(ops, selected_index);
    }
    if (llam_channel_resolve_public_handles_for_select(ops, op_count, channels) != 0) {
        return -1;
    }
    for (i = 0U; i < op_count; ++i) {
        size_t index = start + i;
        int selected;

        if (index >= op_count) {
            index -= op_count;
        }
        selected = llam_channel_select_try_one_resolved(&ops[index], channels[index]);
        if (selected < 0) {
            llam_channel_end_public_select_ops(channels, op_count);
            return -1;
        }
        if (selected == LLAM_SELECT_TRY_FALLBACK) {
            llam_channel_end_public_select_ops(channels, op_count);
            return LLAM_SELECT_TRY_FALLBACK;
        }
        if (selected > 0) {
            *selected_index = index;
            llam_channel_end_public_select_ops(channels, op_count);
            return LLAM_SELECT_TRY_SELECTED;
        }
    }
    llam_channel_end_public_select_ops(channels, op_count);
    return LLAM_SELECT_TRY_NOT_READY;
}

int llam_channel_select_try_one(llam_select_op_t *op) {
    int rc;

    op->result_errno = 0;
    switch (op->kind) {
    case LLAM_SELECT_OP_RECV:
        rc = llam_channel_try_recv_result(op->channel, op->recv_out);
        break;
    case LLAM_SELECT_OP_SEND:
        rc = llam_channel_try_send(op->channel, op->send_value);
        break;
    default:
        errno = EINVAL;
        return -1;
    }
    if (rc == 0) {
        return 1;
    }
    if (errno == EAGAIN || errno == ETIMEDOUT) {
        return 0;
    }
    if (errno == EPIPE || errno == ECANCELED) {
        op->result_errno = errno;
        return 1;
    }
    return -1;
}
