/**
 * @file src/io/linux/watch/linux_segment_submit.c
 * @brief Atomic native batch SQE preparation and local submit failure.
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
