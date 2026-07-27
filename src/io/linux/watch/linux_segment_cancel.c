/**
 * @file src/io/linux/watch/linux_segment_cancel.c
 * @brief Allocation-free cancellation for native Linux segment batches.
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
    _Alignof(llam_linux_native_cancel_token_t) >= 8U,
    "native cancel tokens must keep three tag bits clear");
_Static_assert(
    sizeof(llam_linux_native_cancel_token_t) % 8U == 0U,
    "native cancel token stride must preserve alignment");

static bool llam_linux_native_batch_matches_request(
    const llam_node_t *node,
    const llam_linux_native_batch_t *batch,
    const llam_io_req_t *req) {
    return node != NULL &&
           node->runtime != NULL &&
           batch != NULL &&
           req != NULL &&
           batch->owner_runtime == node->runtime &&
           batch->owner_node == node &&
           batch->req == req &&
           req->owner_runtime == node->runtime &&
           atomic_load_explicit(
               &((llam_io_req_t *)req)->linux_native_batch,
               memory_order_acquire) == batch;
}

bool llam_linux_native_batch_abort_queued(
    llam_node_t *node,
    llam_linux_native_batch_t *batch,
    llam_io_req_t *req) {
    llam_linux_native_batch_t *previous = NULL;
    llam_linux_native_batch_t *cursor;
    unsigned i;

    if (!llam_linux_native_batch_matches_request(
            node, batch, req)) {
        return false;
    }
    pthread_mutex_lock(&node->submit_lock);
    if (!llam_linux_native_batch_matches_request(
            node, batch, req) ||
        atomic_load_explicit(
            &batch->state, memory_order_acquire) !=
            LLAM_LINUX_NATIVE_BATCH_QUEUED ||
        atomic_load_explicit(
            &req->wait_mode, memory_order_acquire) !=
            LLAM_IO_WAIT_MODE_SUBMIT_QUEUE) {
        pthread_mutex_unlock(&node->submit_lock);
        return false;
    }
    cursor = node->native_batch_head;
    while (cursor != NULL && cursor != batch) {
        previous = cursor;
        cursor = cursor->next;
    }
    if (cursor != batch ||
        !llam_node_complete_pending_ops(node, 1U)) {
        pthread_mutex_unlock(&node->submit_lock);
        return false;
    }
    if (previous != NULL) {
        previous->next = batch->next;
    } else {
        node->native_batch_head = batch->next;
    }
    if (node->native_batch_tail == batch) {
        node->native_batch_tail = previous;
    }
    batch->next = NULL;
    batch->retired_segments = batch->segment_count;
    batch->first_error_segment = 0U;
    batch->terminal_result = -ECANCELED;
    atomic_store_explicit(
        &batch->terminal_claimed, 1U, memory_order_release);
    atomic_store_explicit(
        &batch->cancel_requested, 0U, memory_order_release);
    atomic_store_explicit(
        &batch->cancel_state,
        LLAM_LINUX_NATIVE_CANCEL_NONE,
        memory_order_release);
    for (i = 0U; i < batch->segment_count; i += 1U) {
        llam_linux_native_segment_t *segment =
            batch->segments[i];

        if (segment == NULL ||
            segment->batch != batch ||
            segment->req != req ||
            segment->owner_node != node ||
            atomic_load_explicit(
                &segment->state, memory_order_acquire) !=
                LLAM_LINUX_NATIVE_SEGMENT_QUEUED) {
            llam_record_fatal_deferred(
                node->runtime, EPROTO);
            continue;
        }
        segment->first_error = ECANCELED;
        segment->first_error_index = UINT_MAX;
        segment->semantic_result = -ECANCELED;
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
        segment->batch = NULL;
    }
    atomic_store_explicit(
        &batch->state,
        LLAM_LINUX_NATIVE_BATCH_RETIRED,
        memory_order_release);
    atomic_store_explicit(
        &req->linux_native_batch, NULL, memory_order_release);
    atomic_store_explicit(
        &req->wait_mode,
        LLAM_IO_WAIT_MODE_NONE,
        memory_order_release);
    atomic_store_explicit(
        &req->attached_node_index, UINT_MAX, memory_order_release);
    pthread_mutex_unlock(&node->submit_lock);
    return true;
}

bool llam_linux_native_batch_request_cancel(
    llam_node_t *node,
    llam_linux_native_batch_t *batch,
    llam_io_req_t *req) {
    unsigned batch_state;
    unsigned cancel_state;
    bool queued = false;

    if (!llam_linux_native_batch_matches_request(
            node, batch, req)) {
        return false;
    }
    pthread_mutex_lock(&node->submit_lock);
    if (!llam_linux_native_batch_matches_request(
            node, batch, req) ||
        atomic_load_explicit(
            &req->wait_mode, memory_order_acquire) !=
            LLAM_IO_WAIT_MODE_INFLIGHT) {
        pthread_mutex_unlock(&node->submit_lock);
        return false;
    }
    batch_state = atomic_load_explicit(
        &batch->state, memory_order_acquire);
    if (batch_state != LLAM_LINUX_NATIVE_BATCH_QUEUED &&
        batch_state != LLAM_LINUX_NATIVE_BATCH_INFLIGHT &&
        batch_state != LLAM_LINUX_NATIVE_BATCH_RETIRING) {
        pthread_mutex_unlock(&node->submit_lock);
        return false;
    }
    cancel_state = atomic_load_explicit(
        &batch->cancel_state, memory_order_acquire);
    if (cancel_state == LLAM_LINUX_NATIVE_CANCEL_NONE) {
        atomic_store_explicit(
            &batch->cancel_requested, 1U, memory_order_release);
        atomic_store_explicit(
            &batch->cancel_state,
            LLAM_LINUX_NATIVE_CANCEL_QUEUED,
            memory_order_release);
        batch->cancel_next = NULL;
        if (node->native_cancel_tail != NULL) {
            node->native_cancel_tail->cancel_next = batch;
        } else {
            node->native_cancel_head = batch;
        }
        node->native_cancel_tail = batch;
        queued = true;
    } else if (
        cancel_state != LLAM_LINUX_NATIVE_CANCEL_QUEUED &&
        cancel_state != LLAM_LINUX_NATIVE_CANCEL_SUBMITTED &&
        cancel_state != LLAM_LINUX_NATIVE_CANCEL_RETIRED) {
        pthread_mutex_unlock(&node->submit_lock);
        return false;
    }
    pthread_mutex_unlock(&node->submit_lock);
    if (queued) {
        llam_kick_node(node);
    }
    return true;
}

llam_linux_native_batch_t *
llam_linux_native_cancel_take_all(llam_node_t *node) {
    llam_linux_native_batch_t *head;

    if (node == NULL) {
        return NULL;
    }
    pthread_mutex_lock(&node->submit_lock);
    head = node->native_cancel_head;
    node->native_cancel_head = NULL;
    node->native_cancel_tail = NULL;
    pthread_mutex_unlock(&node->submit_lock);
    return head;
}

static unsigned llam_linux_native_batch_operation_count(
    const llam_linux_native_batch_t *batch) {
    unsigned total = 0U;
    unsigned i;

    for (i = 0U; i < batch->segment_count; i += 1U) {
        if (batch->segments[i] == NULL ||
            UINT_MAX - total <
                batch->segments[i]->op_count) {
            return 0U;
        }
        total += batch->segments[i]->op_count;
    }
    return total;
}

static void llam_linux_native_cancel_requeue(
    llam_node_t *node,
    llam_linux_native_batch_t *batch) {
    pthread_mutex_lock(&node->submit_lock);
    batch->cancel_next = NULL;
    if (node->native_cancel_tail != NULL) {
        node->native_cancel_tail->cancel_next = batch;
    } else {
        node->native_cancel_head = batch;
    }
    node->native_cancel_tail = batch;
    pthread_mutex_unlock(&node->submit_lock);
    llam_kick_node(node);
}

static void llam_linux_native_cancel_retire_unsubmitted(
    llam_node_t *node,
    llam_linux_native_batch_t *batch) {
    batch->cancel_sqes_prepared = 0U;
    batch->cancel_cqes_observed = 0U;
    atomic_store_explicit(
        &batch->cancel_state,
        LLAM_LINUX_NATIVE_CANCEL_RETIRED,
        memory_order_release);
    llam_linux_native_batch_maybe_complete(node, batch);
}

unsigned llam_linux_native_batch_submit_cancel(
    llam_node_t *node,
    llam_linux_native_batch_t *batch) {
    unsigned total_ops;
    unsigned start_tail;
    unsigned i;
    unsigned j;

    if (node == NULL ||
        batch == NULL ||
        !llam_linux_native_batch_matches_request(
            node, batch, batch->req) ||
        atomic_load_explicit(
            &batch->cancel_state, memory_order_acquire) !=
            LLAM_LINUX_NATIVE_CANCEL_QUEUED) {
        if (node != NULL && batch != NULL &&
            atomic_load_explicit(
                &batch->state, memory_order_acquire) !=
                LLAM_LINUX_NATIVE_BATCH_RETIRED) {
            llam_record_fatal_deferred(
                node->runtime, EPROTO);
        }
        return 0U;
    }
    total_ops =
        llam_linux_native_batch_operation_count(batch);
    if (total_ops == 0U) {
        llam_record_fatal_deferred(node->runtime, EPROTO);
        return 0U;
    }
    if (!node->ring_ready || node->linux_submit_terminal) {
        llam_linux_native_cancel_retire_unsubmitted(
            node, batch);
        return 0U;
    }
    if (io_uring_sq_space_left(&node->ring) < total_ops) {
        int rc = llam_node_submit_ring(node);

        if (rc < 0 && node->linux_submit_terminal) {
            llam_record_fatal_deferred(
                node->runtime, -rc);
            llam_linux_native_cancel_retire_unsubmitted(
                node, batch);
            return 0U;
        }
        if (rc < 0 ||
            io_uring_sq_space_left(&node->ring) < total_ops) {
            llam_linux_native_cancel_requeue(node, batch);
            return 0U;
        }
    }

    start_tail = node->ring.sq.sqe_tail;
    for (i = 0U; i < batch->segment_count; i += 1U) {
        llam_linux_native_segment_t *segment =
            batch->segments[i];

        for (j = 0U; j < segment->op_count; j += 1U) {
            struct io_uring_sqe *sqe =
                io_uring_get_sqe(&node->ring);

            if (sqe == NULL) {
                node->ring.sq.sqe_tail = start_tail;
                llam_linux_native_cancel_requeue(
                    node, batch);
                return 0U;
            }
            io_uring_prep_cancel64(
                sqe,
                llam_io_udata_encode(
                    &segment->tokens[j],
                    LLAM_IO_UDATA_NATIVE_SEGMENT),
                0);
            io_uring_sqe_set_data64(
                sqe,
                llam_io_udata_encode(
                    &segment->cancel_tokens[j],
                    LLAM_IO_UDATA_NATIVE_CANCEL));
        }
    }
    batch->cancel_sqes_prepared = total_ops;
    batch->cancel_cqes_observed = 0U;
    atomic_store_explicit(
        &batch->cancel_state,
        LLAM_LINUX_NATIVE_CANCEL_SUBMITTED,
        memory_order_release);
    return total_ops;
}

void llam_linux_native_cancel_handle_cqe(
    llam_node_t *node,
    llam_linux_native_cancel_token_t *token,
    int result) {
    llam_linux_native_segment_t *segment;
    llam_linux_native_batch_t *batch;
    uint64_t bit;
    unsigned index;
    bool result_is_valid;

    if (node == NULL || node->runtime == NULL ||
        token == NULL || token->owner == NULL) {
        if (node != NULL) {
            llam_record_fatal_deferred(
                node->runtime, EPROTO);
        }
        return;
    }
    segment = token->owner;
    batch = segment->batch;
    if (batch == NULL ||
        segment->owner_runtime != node->runtime ||
        segment->owner_node != node ||
        segment->generation == 0U ||
        token->generation != segment->generation ||
        token->operation_index >= segment->op_count ||
        batch->owner_runtime != node->runtime ||
        batch->owner_node != node ||
        batch->req != segment->req ||
        atomic_load_explicit(
            &batch->cancel_state, memory_order_acquire) !=
            LLAM_LINUX_NATIVE_CANCEL_SUBMITTED) {
        llam_record_fatal_deferred(node->runtime, EPROTO);
        return;
    }
    index = token->operation_index;
    bit = UINT64_C(1) << index;
    if ((segment->observed_cancel_mask & bit) != 0U) {
        llam_record_fatal_deferred(node->runtime, EPROTO);
        return;
    }
    segment->observed_cancel_mask |= bit;
    batch->cancel_cqes_observed += 1U;
    result_is_valid =
        result == 0 ||
        result == -ENOENT ||
        result == -EALREADY;
    if (!result_is_valid ||
        batch->cancel_cqes_observed >
            batch->cancel_sqes_prepared) {
        llam_record_fatal_deferred(node->runtime, EPROTO);
    }
    if (batch->cancel_cqes_observed ==
        batch->cancel_sqes_prepared) {
        atomic_store_explicit(
            &batch->cancel_state,
            LLAM_LINUX_NATIVE_CANCEL_RETIRED,
            memory_order_release);
        llam_linux_native_batch_maybe_complete(
            node, batch);
    }
}
