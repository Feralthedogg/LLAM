/**
 * @file src/core/runtime_channel_select_fast.c
 * @brief Fast ready probes for channel select.
 *
 * @details
 * The full select waiter state machine lives in runtime_channel_select.c.  This
 * file keeps the cache-hot ready/try path separate so select timeout and ready
 * benchmarks do not drag the parked-waiter code through the same module.
 *
 * @copyright Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: Apache-2.0
 */

#include "runtime_internal.h"

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
        errno = EINVAL;
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

int llam_channel_select_try_ready_batch(llam_select_op_t *ops,
                                        size_t op_count,
                                        size_t start,
                                        size_t *selected_index) {
    llam_channel_t *channels[LLAM_CHANNEL_SELECT_INLINE_OPS];
    size_t i;

    if (op_count > LLAM_CHANNEL_SELECT_INLINE_OPS) {
        return LLAM_SELECT_TRY_FALLBACK;
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
