/**
 * @file src/io/linux/watch/linux_segment_complete.c
 * @brief Native segment retirement and terminal request completion.
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
