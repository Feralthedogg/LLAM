/**
 * @file src/io/linux/watch/linux_segment.c
 * @brief io_uring encoding and CQE reduction for compiled LEIR segments.
 *
 * @details
 * This file contains only value validation, SQE preparation, and completion
 * state reduction.  Queue ownership and task wakeup are layered on top so the
 * semantic core can be tested without creating a kernel ring.
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

#include "io/linux/runtime_io_watch_linux_internal.h"

_Static_assert(
    _Alignof(llam_linux_native_token_t) >= 8U,
    "native segment tokens must keep three tag bits clear");
_Static_assert(
    sizeof(llam_linux_native_token_t) % 8U == 0U,
    "native segment token stride must preserve alignment");

static bool llam_linux_native_count_is_valid(unsigned op_count) {
    return op_count == 1U ||
           op_count == 2U ||
           op_count == 4U ||
           op_count == 8U;
}

static bool llam_linux_native_kind_is_valid(uint16_t kind) {
    return kind == LLAM_LINUX_NATIVE_OP_RECV ||
           kind == LLAM_LINUX_NATIVE_OP_SEND;
}

static bool llam_linux_native_mode_is_valid(
    llam_linux_native_segment_mode_t mode) {
    return mode == LLAM_LINUX_NATIVE_SEGMENT_LINK ||
           mode == LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP;
}

int llam_linux_native_segment_configure(
    llam_linux_native_segment_t *segment,
    const llam_linux_native_op_t *ops,
    unsigned op_count,
    llam_linux_native_segment_mode_t mode) {
    llam_linux_native_op_t
        copied[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned i;

    if (segment == NULL ||
        ops == NULL ||
        !llam_linux_native_count_is_valid(op_count) ||
        !llam_linux_native_mode_is_valid(mode)) {
        errno = EINVAL;
        return -1;
    }
    for (i = 0U; i < op_count; i += 1U) {
        if (!llam_linux_native_kind_is_valid(ops[i].kind) ||
            ops[i].fd < 0 ||
            ops[i].buffer == NULL ||
            ops[i].length == 0U ||
            (ops[i].kind == LLAM_LINUX_NATIVE_OP_RECV &&
             i + 1U < op_count)) {
            errno = EINVAL;
            return -1;
        }
    }

    memcpy(copied, ops, op_count * sizeof(ops[0]));
    memset(segment, 0, sizeof(*segment));
    memcpy(segment->ops, copied, op_count * sizeof(copied[0]));
    segment->op_count = op_count;
    segment->first_error_index = UINT_MAX;
    segment->mode = mode;
    for (i = 0U; i < op_count; i += 1U) {
        segment->tokens[i].owner = segment;
        segment->tokens[i].operation_index = (uint16_t)i;
        segment->cancel_tokens[i].owner = segment;
        segment->cancel_tokens[i].operation_index =
            (uint16_t)i;
    }
    atomic_init(&segment->state, LLAM_LINUX_NATIVE_SEGMENT_IDLE);
    atomic_init(&segment->semantic_claimed, 0U);
    atomic_init(&segment->target_retired, 0U);
    atomic_init(&segment->terminal_claimed, 0U);
    return 0;
}

void llam_linux_native_segment_prepare_sqe(
    const llam_linux_native_segment_t *segment,
    unsigned index,
    struct io_uring_sqe *sqe) {
    const llam_linux_native_op_t *op;
    unsigned flags = 0U;

    if (segment == NULL ||
        sqe == NULL ||
        index >= segment->op_count) {
        return;
    }
    op = &segment->ops[index];
    if (op->kind == LLAM_LINUX_NATIVE_OP_RECV) {
        io_uring_prep_recv(
            sqe,
            op->fd,
            op->buffer,
            op->length,
            0);
    } else {
        io_uring_prep_send(
            sqe,
            op->fd,
            op->buffer,
            op->length,
            MSG_NOSIGNAL);
    }

    if (index + 1U < segment->op_count) {
        flags = IOSQE_IO_LINK;
        if (segment->mode ==
            LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP) {
            flags |= IOSQE_CQE_SKIP_SUCCESS;
        }
    }
    io_uring_sqe_set_flags(sqe, flags);
    io_uring_sqe_set_data64(
        sqe,
        llam_io_udata_encode(
            (void *)&segment->tokens[index],
            LLAM_IO_UDATA_NATIVE_SEGMENT));
}

static int llam_linux_native_result_error(
    const llam_linux_native_op_t *op,
    int result) {
    if (result < 0) {
        if (result == INT_MIN) {
            return EIO;
        }
        return -result;
    }
    if ((uint32_t)result != op->length) {
        return EMSGSIZE;
    }
    return 0;
}

static void llam_linux_native_record_error(
    llam_linux_native_segment_t *segment,
    unsigned index,
    int error) {
    bool current_is_cancel;
    bool error_is_cancel;

    if (error == 0) {
        return;
    }
    current_is_cancel = segment->first_error == ECANCELED;
    error_is_cancel = error == ECANCELED;
    if (segment->first_error == 0 ||
        (current_is_cancel && !error_is_cancel) ||
        (current_is_cancel == error_is_cancel &&
         index < segment->first_error_index)) {
        segment->first_error = error;
        segment->first_error_index = index;
    }
}

static unsigned llam_linux_native_missing_cqes(
    uint64_t observed_mask) {
    unsigned highest = 0U;
    unsigned missing = 0U;
    unsigned i;

    if (observed_mask == 0U) {
        return 0U;
    }
    for (i = 0U; i < LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS; i += 1U) {
        if ((observed_mask & (UINT64_C(1) << i)) != 0U) {
            highest = i;
        }
    }
    for (i = 0U; i < highest; i += 1U) {
        if ((observed_mask & (UINT64_C(1) << i)) == 0U) {
            missing += 1U;
        }
    }
    return missing;
}

llam_linux_native_cqe_action_t
llam_linux_native_segment_apply_cqe(
    llam_linux_native_segment_t *segment,
    const llam_linux_native_token_t *token,
    int result,
    int *terminal_result_out) {
    llam_linux_native_cqe_action_t action =
        LLAM_LINUX_NATIVE_CQE_CONTINUE;
    uint64_t bit;
    uint64_t old_mask;
    unsigned old_missing = 0U;
    unsigned new_missing = 0U;
    unsigned state;
    unsigned index;
    int error;

    if (segment == NULL ||
        token == NULL ||
        terminal_result_out == NULL ||
        token->owner != segment ||
        token->generation == 0U ||
        token->generation != segment->generation ||
        token->operation_index >= segment->op_count) {
        return LLAM_LINUX_NATIVE_CQE_FATAL;
    }

    state = atomic_load_explicit(
        &segment->state, memory_order_acquire);
    if (state != LLAM_LINUX_NATIVE_SEGMENT_INFLIGHT &&
        state != LLAM_LINUX_NATIVE_SEGMENT_RETIRING) {
        return LLAM_LINUX_NATIVE_CQE_FATAL;
    }
    index = token->operation_index;
    bit = UINT64_C(1) << index;
    old_mask = segment->observed_operation_mask;
    if ((old_mask & bit) != 0U ||
        (segment->mode ==
             LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP &&
         index + 1U < segment->op_count &&
         result >= 0)) {
        return LLAM_LINUX_NATIVE_CQE_FATAL;
    }
    if (segment->mode ==
        LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP) {
        old_missing =
            llam_linux_native_missing_cqes(old_mask);
    }
    segment->observed_operation_mask = old_mask | bit;
    if (segment->mode ==
        LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP) {
        new_missing = llam_linux_native_missing_cqes(
            segment->observed_operation_mask);
        if (new_missing >= old_missing) {
            segment->suppressed_success_cqes +=
                new_missing - old_missing;
        } else {
            segment->suppressed_success_cqes -=
                old_missing - new_missing;
        }
    }

    error = llam_linux_native_result_error(
        &segment->ops[index], result);
    segment->observed_cqes += 1U;
    segment->completed_cqes += 1U;
    llam_linux_native_record_error(segment, index, error);

    if (error != 0) {
        unsigned was_claimed = atomic_exchange_explicit(
            &segment->semantic_claimed,
            1U,
            memory_order_acq_rel);

        segment->semantic_result = -segment->first_error;
        *terminal_result_out = segment->semantic_result;
        if (was_claimed == 0U) {
            atomic_store_explicit(
                &segment->state,
                LLAM_LINUX_NATIVE_SEGMENT_RETIRING,
                memory_order_release);
            action = LLAM_LINUX_NATIVE_CQE_SEMANTIC;
        }
    }

    if (index + 1U == segment->op_count) {
        unsigned expected = 0U;

        if (!atomic_compare_exchange_strong_explicit(
                &segment->terminal_claimed,
                &expected,
                1U,
                memory_order_acq_rel,
                memory_order_acquire)) {
            return LLAM_LINUX_NATIVE_CQE_FATAL;
        }
        atomic_store_explicit(
            &segment->target_retired,
            1U,
            memory_order_release);
        if (atomic_exchange_explicit(
                &segment->semantic_claimed,
                1U,
                memory_order_acq_rel) == 0U) {
            segment->semantic_result =
                segment->first_error != 0
                    ? -segment->first_error
                    : result;
        } else if (segment->first_error != 0) {
            segment->semantic_result =
                -segment->first_error;
        }
        *terminal_result_out = segment->semantic_result;
        atomic_store_explicit(
            &segment->state,
            LLAM_LINUX_NATIVE_SEGMENT_RETIRED,
            memory_order_release);
        return segment->first_error != 0
            ? LLAM_LINUX_NATIVE_CQE_RETIRED_ERROR
            : LLAM_LINUX_NATIVE_CQE_RETIRED_OK;
    }
    return action;
}

static bool llam_linux_native_batch_count_is_valid(
    unsigned segment_count) {
    return segment_count >= 1U &&
           segment_count <=
               LLAM_LINUX_NATIVE_BATCH_MAX_SEGMENTS;
}

static int llam_linux_native_batch_validate(
    llam_node_t *node,
    llam_linux_native_batch_t *batch,
    llam_io_req_t *req) {
    llam_runtime_t *runtime;
    unsigned i;
    unsigned j;

    if (node == NULL || batch == NULL || req == NULL) {
        return EINVAL;
    }
    runtime = node->runtime;
    if (runtime == NULL ||
        batch->owner_runtime == NULL ||
        req->owner_runtime == NULL ||
        !llam_linux_native_batch_count_is_valid(
            batch->segment_count)) {
        return EINVAL;
    }
    if (batch->owner_runtime != runtime ||
        req->owner_runtime != runtime) {
        llam_record_fatal_deferred(runtime, EXDEV);
        return EXDEV;
    }
    if (atomic_load_explicit(
            &batch->state, memory_order_acquire) !=
            LLAM_LINUX_NATIVE_BATCH_IDLE ||
        atomic_load_explicit(
            &batch->cancel_state, memory_order_acquire) !=
            LLAM_LINUX_NATIVE_CANCEL_NONE) {
        return EBUSY;
    }
    for (i = 0U; i < batch->segment_count; i += 1U) {
        llam_linux_native_segment_t *segment =
            batch->segments[i];

        if (segment == NULL ||
            segment->owner_runtime == NULL ||
            segment->generation == 0U ||
            segment->op_count == 0U ||
            segment->op_count >
                LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS) {
            return EINVAL;
        }
        if (segment->owner_runtime != runtime) {
            llam_record_fatal_deferred(runtime, EXDEV);
            return EXDEV;
        }
        if (atomic_load_explicit(
                &segment->state,
                memory_order_acquire) !=
                LLAM_LINUX_NATIVE_SEGMENT_IDLE) {
            return EBUSY;
        }
        if (segment->mode ==
                LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP &&
            (node->linux_ring_features &
             IORING_FEAT_CQE_SKIP) == 0U) {
            return ENOTSUP;
        }
        for (j = 0U; j < i; j += 1U) {
            if (batch->segments[j] == segment) {
                return EINVAL;
            }
        }
    }
    return 0;
}

static void llam_linux_native_segment_activate(
    llam_node_t *node,
    llam_linux_native_batch_t *batch,
    llam_linux_native_segment_t *segment) {
    unsigned i;

    segment->owner_node = node;
    segment->req = batch->req;
    segment->next = NULL;
    segment->batch = batch;
    segment->completed_cqes = 0U;
    segment->observed_operation_mask = 0U;
    segment->observed_cancel_mask = 0U;
    segment->first_error = 0;
    segment->first_error_index = UINT_MAX;
    segment->semantic_result = 0;
    atomic_store_explicit(
        &segment->semantic_claimed, 0U, memory_order_release);
    atomic_store_explicit(
        &segment->target_retired, 0U, memory_order_release);
    atomic_store_explicit(
        &segment->terminal_claimed, 0U, memory_order_release);
    for (i = 0U; i < segment->op_count; i += 1U) {
        segment->tokens[i].owner = segment;
        segment->tokens[i].generation = segment->generation;
        segment->tokens[i].operation_index = (uint16_t)i;
        segment->cancel_tokens[i].owner = segment;
        segment->cancel_tokens[i].generation =
            segment->generation;
        segment->cancel_tokens[i].operation_index =
            (uint16_t)i;
    }
    segment->activations += 1U;
    segment->logical_operations += segment->op_count;
}

/**
 * @brief Publish one bounded native batch through one pending owner.
 */
bool llam_linux_native_batch_enqueue(
    llam_node_t *node,
    llam_linux_native_batch_t *batch,
    llam_io_req_t *req) {
    unsigned claimed_segments = 0U;
    unsigned expected;
    unsigned i;
    int error;

    error = llam_linux_native_batch_validate(
        node, batch, req);
    if (error != 0) {
        errno = error;
        return false;
    }

    pthread_mutex_lock(&node->submit_lock);
    if (atomic_load_explicit(
            &req->wait_mode,
            memory_order_acquire) !=
            LLAM_IO_WAIT_MODE_SUBMIT_QUEUE ||
        atomic_load_explicit(
            &req->linux_native_batch,
            memory_order_acquire) != NULL) {
        error = EBUSY;
        goto reject;
    }
    if (llam_io_req_abort_requested(req)) {
        error = ECANCELED;
        goto reject;
    }
    for (i = 0U; i < batch->segment_count; i += 1U) {
        expected = LLAM_LINUX_NATIVE_SEGMENT_IDLE;
        if (!atomic_compare_exchange_strong_explicit(
                &batch->segments[i]->state,
                &expected,
                LLAM_LINUX_NATIVE_SEGMENT_QUEUED,
                memory_order_acq_rel,
                memory_order_acquire)) {
            error = EBUSY;
            goto rollback_segments;
        }
        claimed_segments += 1U;
    }
    expected = LLAM_LINUX_NATIVE_BATCH_IDLE;
    if (!atomic_compare_exchange_strong_explicit(
            &batch->state,
            &expected,
            LLAM_LINUX_NATIVE_BATCH_QUEUED,
            memory_order_acq_rel,
            memory_order_acquire)) {
        error = EBUSY;
        goto rollback_segments;
    }
    if (!llam_node_note_pending_ops(node, 1U)) {
        error = errno != 0 ? errno : EOVERFLOW;
        atomic_store_explicit(
            &batch->state,
            LLAM_LINUX_NATIVE_BATCH_IDLE,
            memory_order_release);
        goto rollback_segments;
    }

    batch->owner_node = node;
    batch->req = req;
    batch->next = NULL;
    batch->cancel_next = NULL;
    batch->retired_segments = 0U;
    batch->cancel_sqes_prepared = 0U;
    batch->cancel_cqes_observed = 0U;
    batch->terminal_result = 0;
    batch->first_error_segment = UINT_MAX;
    atomic_store_explicit(
        &batch->terminal_claimed, 0U, memory_order_release);
    atomic_store_explicit(
        &batch->cancel_state,
        LLAM_LINUX_NATIVE_CANCEL_NONE,
        memory_order_release);
    atomic_store_explicit(
        &batch->cancel_requested, 0U, memory_order_release);
    for (i = 0U; i < batch->segment_count; i += 1U) {
        llam_linux_native_segment_activate(
            node, batch, batch->segments[i]);
    }
    batch->segments[0]->queue_publications += 1U;
    atomic_store_explicit(
        &req->attached_node_index,
        node->index,
        memory_order_release);
    atomic_store_explicit(
        &req->linux_native_batch,
        batch,
        memory_order_release);
    if (node->native_batch_tail != NULL) {
        node->native_batch_tail->next = batch;
    } else {
        node->native_batch_head = batch;
    }
    node->native_batch_tail = batch;
    pthread_mutex_unlock(&node->submit_lock);
    llam_kick_node(node);
    return true;

rollback_segments:
    while (claimed_segments > 0U) {
        claimed_segments -= 1U;
        atomic_store_explicit(
            &batch->segments[claimed_segments]->state,
            LLAM_LINUX_NATIVE_SEGMENT_IDLE,
            memory_order_release);
    }
reject:
    pthread_mutex_unlock(&node->submit_lock);
    errno = error;
    return false;
}

/**
 * @brief Detach every queued native batch and publish one waiter per ticket.
 */
llam_linux_native_batch_t *
llam_linux_native_batch_take_all(llam_node_t *node) {
    llam_linux_native_batch_t *head;
    llam_linux_native_batch_t *cursor;

    if (node == NULL) {
        return NULL;
    }
    pthread_mutex_lock(&node->submit_lock);
    head = node->native_batch_head;
    node->native_batch_head = NULL;
    node->native_batch_tail = NULL;
    cursor = head;
    while (cursor != NULL) {
        llam_io_req_t *req = cursor->req;
        bool valid =
            req != NULL &&
            cursor->owner_node == node &&
            req->owner_runtime == node->runtime &&
            atomic_load_explicit(
                &cursor->state,
                memory_order_acquire) ==
                LLAM_LINUX_NATIVE_BATCH_QUEUED &&
            atomic_load_explicit(
                &req->wait_mode,
                memory_order_acquire) ==
                LLAM_IO_WAIT_MODE_SUBMIT_QUEUE;
        unsigned i;

        for (i = 0U;
             valid && i < cursor->segment_count;
             i += 1U) {
            llam_linux_native_segment_t *segment =
                cursor->segments[i];

            valid =
                segment != NULL &&
                segment->batch == cursor &&
                segment->req == req &&
                segment->owner_node == node &&
                atomic_load_explicit(
                    &segment->state,
                    memory_order_acquire) ==
                    LLAM_LINUX_NATIVE_SEGMENT_QUEUED;
        }
        if (!valid) {
            llam_record_fatal_deferred(
                node->runtime, EPROTO);
        } else {
            unsigned owner_shard = atomic_load_explicit(
                &req->owner_shard, memory_order_acquire);

            atomic_store_explicit(
                &req->inflight_owner_shard,
                owner_shard,
                memory_order_release);
            atomic_store_explicit(
                &req->wait_mode,
                LLAM_IO_WAIT_MODE_INFLIGHT,
                memory_order_release);
            llam_shard_note_inflight_io_waiter(
                req->owner_runtime, owner_shard, 1);
        }
        cursor = cursor->next;
    }
    pthread_mutex_unlock(&node->submit_lock);
    return head;
}

static void llam_linux_native_batch_complete_local(
    llam_node_t *node,
    llam_linux_native_batch_t *batch,
    int error) {
    unsigned i;

    if (node == NULL ||
        batch == NULL ||
        batch->req == NULL ||
        batch->segment_count == 0U ||
        error <= 0) {
        if (node != NULL) {
            llam_record_fatal_deferred(
                node->runtime, EINVAL);
        }
        return;
    }
    if (atomic_load_explicit(
            &batch->terminal_claimed,
            memory_order_acquire) != 0U) {
        llam_record_fatal_deferred(node->runtime, EPROTO);
        return;
    }
    for (i = 0U; i < batch->segment_count; i += 1U) {
        llam_linux_native_segment_t *segment =
            batch->segments[i];

        segment->first_error = error;
        segment->first_error_index = UINT_MAX;
        segment->semantic_result = -error;
        atomic_store_explicit(
            &segment->semantic_claimed,
            1U,
            memory_order_release);
        atomic_store_explicit(
            &segment->target_retired,
            1U,
            memory_order_release);
        atomic_store_explicit(
            &segment->terminal_claimed,
            1U,
            memory_order_release);
        atomic_store_explicit(
            &segment->state,
            LLAM_LINUX_NATIVE_SEGMENT_RETIRED,
            memory_order_release);
    }
    batch->retired_segments = batch->segment_count;
    batch->first_error_segment = 0U;
    batch->terminal_result = -error;
    atomic_store_explicit(
        &batch->state,
        LLAM_LINUX_NATIVE_BATCH_RETIRING,
        memory_order_release);
    llam_linux_native_batch_maybe_complete(node, batch);
}

/**
 * @brief Encode every complete segment in one ticket without partial publish.
 *
 * @return Number of prepared operation SQEs, or zero after local completion.
 */
unsigned llam_linux_native_batch_submit_one(
    llam_node_t *node,
    llam_linux_native_batch_t *batch) {
    unsigned total_ops = 0U;
    unsigned start_tail;
    unsigned i;
    unsigned j;

    if (node == NULL ||
        batch == NULL ||
        batch->req == NULL ||
        batch->owner_node != node ||
        batch->req->owner_runtime != node->runtime ||
        !llam_linux_native_batch_count_is_valid(
            batch->segment_count) ||
        atomic_load_explicit(
            &batch->state,
            memory_order_acquire) !=
            LLAM_LINUX_NATIVE_BATCH_QUEUED) {
        if (node != NULL) {
            llam_record_fatal_deferred(
                node->runtime, EINVAL);
        }
        return 0U;
    }
    for (i = 0U; i < batch->segment_count; i += 1U) {
        llam_linux_native_segment_t *segment =
            batch->segments[i];

        if (segment == NULL ||
            segment->batch != batch ||
            segment->owner_node != node ||
            segment->req != batch->req ||
            atomic_load_explicit(
                &segment->state,
                memory_order_acquire) !=
                LLAM_LINUX_NATIVE_SEGMENT_QUEUED ||
            UINT_MAX - total_ops < segment->op_count) {
            llam_record_fatal_deferred(
                node->runtime, EPROTO);
            return 0U;
        }
        total_ops += segment->op_count;
    }
    if (!node->ring_ready ||
        node->linux_submit_terminal) {
        int error = atomic_load_explicit(
            &node->runtime->fatal_errno,
            memory_order_acquire);

        llam_linux_native_batch_complete_local(
            node, batch, error != 0 ? error : EAGAIN);
        return 0U;
    }
    if (io_uring_sq_space_left(&node->ring) < total_ops) {
        int rc = llam_node_submit_ring(node);

        if (rc < 0 ||
            io_uring_sq_space_left(&node->ring) <
                total_ops) {
            llam_linux_native_batch_complete_local(
                node, batch, EAGAIN);
            return 0U;
        }
    }

    start_tail = node->ring.sq.sqe_tail;
    atomic_store_explicit(
        &batch->state,
        LLAM_LINUX_NATIVE_BATCH_INFLIGHT,
        memory_order_release);
    for (i = 0U; i < batch->segment_count; i += 1U) {
        atomic_store_explicit(
            &batch->segments[i]->state,
            LLAM_LINUX_NATIVE_SEGMENT_INFLIGHT,
            memory_order_release);
    }
    for (i = 0U; i < batch->segment_count; i += 1U) {
        llam_linux_native_segment_t *segment =
            batch->segments[i];

        for (j = 0U; j < segment->op_count; j += 1U) {
            struct io_uring_sqe *sqe =
                io_uring_get_sqe(&node->ring);

            if (sqe == NULL) {
                node->ring.sq.sqe_tail = start_tail;
                llam_linux_native_batch_complete_local(
                    node, batch, EAGAIN);
                return 0U;
            }
            llam_linux_native_segment_prepare_sqe(
                segment, j, sqe);
        }
    }
    for (i = 0U; i < batch->segment_count; i += 1U) {
        batch->segments[i]->prepared_sqes +=
            batch->segments[i]->op_count;
    }
    return total_ops;
}

static unsigned llam_linux_native_batch_segment_index(
    const llam_linux_native_batch_t *batch,
    const llam_linux_native_segment_t *segment) {
    unsigned i;

    for (i = 0U; i < batch->segment_count; i += 1U) {
        if (batch->segments[i] == segment) {
            return i;
        }
    }
    return UINT_MAX;
}

static void llam_linux_native_batch_retire_segment(
    llam_node_t *node,
    llam_linux_native_batch_t *batch,
    llam_linux_native_segment_t *segment,
    int terminal_result) {
    unsigned segment_index;

    segment_index = llam_linux_native_batch_segment_index(
        batch, segment);
    if (segment_index == UINT_MAX ||
        batch->retired_segments >= batch->segment_count) {
        llam_record_fatal_deferred(node->runtime, EPROTO);
        return;
    }
    if (terminal_result < 0 &&
        (batch->first_error_segment == UINT_MAX ||
         segment_index < batch->first_error_segment)) {
        batch->first_error_segment = segment_index;
        batch->terminal_result = terminal_result;
    }
    batch->retired_segments += 1U;
    if (batch->retired_segments < batch->segment_count) {
        atomic_store_explicit(
            &batch->state,
            LLAM_LINUX_NATIVE_BATCH_RETIRING,
            memory_order_release);
        return;
    }
    llam_linux_native_batch_maybe_complete(node, batch);
}

void llam_linux_native_batch_maybe_complete(
    llam_node_t *node,
    llam_linux_native_batch_t *batch) {
    llam_linux_native_batch_t *expected_batch;
    llam_io_req_t *req;
    unsigned expected = 0U;
    unsigned cancel_state;
    unsigned i;
    int terminal_result;

    if (node == NULL ||
        node->runtime == NULL ||
        batch == NULL ||
        batch->owner_node != node ||
        batch->owner_runtime != node->runtime ||
        batch->req == NULL ||
        batch->segment_count == 0U ||
        batch->retired_segments != batch->segment_count) {
        return;
    }
    pthread_mutex_lock(&node->submit_lock);
    if (batch->retired_segments != batch->segment_count ||
        atomic_load_explicit(
            &batch->state, memory_order_acquire) ==
            LLAM_LINUX_NATIVE_BATCH_RETIRED) {
        pthread_mutex_unlock(&node->submit_lock);
        return;
    }
    cancel_state = atomic_load_explicit(
        &batch->cancel_state, memory_order_acquire);
    if (cancel_state != LLAM_LINUX_NATIVE_CANCEL_NONE &&
        (cancel_state != LLAM_LINUX_NATIVE_CANCEL_RETIRED ||
         batch->cancel_cqes_observed !=
             batch->cancel_sqes_prepared)) {
        pthread_mutex_unlock(&node->submit_lock);
        return;
    }
    for (i = 0U; i < batch->segment_count; i += 1U) {
        if (atomic_load_explicit(
                &batch->segments[i]->target_retired,
                memory_order_acquire) == 0U ||
            atomic_load_explicit(
                &batch->segments[i]->state,
                memory_order_acquire) !=
                LLAM_LINUX_NATIVE_SEGMENT_RETIRED) {
            llam_record_fatal_deferred(
                node->runtime, EPROTO);
            pthread_mutex_unlock(&node->submit_lock);
            return;
        }
    }
    if (batch->first_error_segment == UINT_MAX) {
        batch->terminal_result =
            batch->segments[
                batch->segment_count - 1U]->semantic_result;
    }
    if (!atomic_compare_exchange_strong_explicit(
            &batch->terminal_claimed,
            &expected,
            1U,
            memory_order_acq_rel,
            memory_order_acquire)) {
        pthread_mutex_unlock(&node->submit_lock);
        return;
    }
    expected_batch = batch;
    if (!atomic_compare_exchange_strong_explicit(
            &batch->req->linux_native_batch,
            &expected_batch,
            NULL,
            memory_order_acq_rel,
            memory_order_acquire)) {
        llam_record_fatal_deferred(node->runtime, EPROTO);
        pthread_mutex_unlock(&node->submit_lock);
        return;
    }
    atomic_store_explicit(
        &batch->state,
        LLAM_LINUX_NATIVE_BATCH_RETIRED,
        memory_order_release);
    for (i = 0U; i < batch->segment_count; i += 1U) {
        batch->segments[i]->batch = NULL;
    }
    batch->segments[0]->terminal_wakes += 1U;
    req = batch->req;
    terminal_result = batch->terminal_result;
    pthread_mutex_unlock(&node->submit_lock);
    llam_io_complete_req(
        node,
        req,
        terminal_result,
        0U,
        true);
}

/**
 * @brief Reduce one segment CQE and release its batch only after all tails.
 */
void llam_linux_native_segment_handle_cqe(
    llam_node_t *node,
    llam_linux_native_token_t *token,
    int result) {
    llam_linux_native_segment_t *segment;
    llam_linux_native_batch_t *batch;
    llam_linux_native_cqe_action_t action;
    int terminal_result = -EIO;
    unsigned batch_state;

    if (node == NULL || node->runtime == NULL) {
        return;
    }
    if (token == NULL || token->owner == NULL) {
        llam_record_fatal_deferred(node->runtime, EPROTO);
        return;
    }
    segment = token->owner;
    batch = segment->batch;
    if (segment->owner_runtime != node->runtime) {
        llam_record_fatal_deferred(node->runtime, EXDEV);
        return;
    }
    if (batch == NULL ||
        segment->req == NULL ||
        segment->owner_node != node) {
        llam_record_fatal_deferred(node->runtime, EPROTO);
        return;
    }
    if (batch->owner_runtime != node->runtime ||
        batch->owner_node != node ||
        batch->req != segment->req ||
        segment->req->owner_runtime != node->runtime) {
        llam_record_fatal_deferred(node->runtime, EXDEV);
        return;
    }
    batch_state = atomic_load_explicit(
        &batch->state, memory_order_acquire);
    if (batch_state != LLAM_LINUX_NATIVE_BATCH_INFLIGHT &&
        batch_state != LLAM_LINUX_NATIVE_BATCH_RETIRING) {
        llam_record_fatal_deferred(node->runtime, EPROTO);
        return;
    }

    action = llam_linux_native_segment_apply_cqe(
        segment, token, result, &terminal_result);
    if (action == LLAM_LINUX_NATIVE_CQE_CONTINUE) {
        return;
    }
    if (action == LLAM_LINUX_NATIVE_CQE_SEMANTIC) {
        atomic_store_explicit(
            &batch->state,
            LLAM_LINUX_NATIVE_BATCH_RETIRING,
            memory_order_release);
        return;
    }
    if (action == LLAM_LINUX_NATIVE_CQE_FATAL) {
        llam_record_fatal_deferred(node->runtime, EPROTO);
        return;
    }

    llam_linux_native_batch_retire_segment(
        node, batch, segment, terminal_result);
}
