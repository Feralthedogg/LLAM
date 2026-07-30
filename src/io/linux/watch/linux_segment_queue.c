/**
 * @file src/io/linux/watch/linux_segment_queue.c
 * @brief Native batch validation, activation, and submit-queue ownership.
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

bool llam_linux_native_batch_count_is_valid(
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
        if (segment->op_count > 1U &&
            !node->linux_submit_all) {
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
    segment->atomic_submission = node->linux_submit_all;
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
