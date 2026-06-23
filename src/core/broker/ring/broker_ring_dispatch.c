/**
 * @file src/core/broker/ring/broker_ring_dispatch.c
 * @brief Broker shared-memory ring operation dispatcher.
 *
 * @details
 * Session lookup lives in broker_ring.c, raw SPSC cursor movement lives
 * in broker_ring_queue.c, and per-op authority checks live in
 * broker_ring_ops.c. This file only reserves immutable submission
 * batches, coordinates lock release/reacquire around execution, and publishes
 * completions after validating broker-private cursors.
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
#include "runtime_broker_ring.h"

#include <string.h>

static void llam_broker_ring_session_submit_publish(llam_broker_ring_t *ring,
                                                    const llam_broker_ring_session_t *session) {
    /*
     * The session cursor is broker-private. Publish it only after the request
     * has produced a completion so the shared cache line is not dirtied during
     * every dispatch prologue and future multi-request batches can flush once.
     */
    atomic_store_explicit(&ring->submit_head.value, session->submit_head, memory_order_release);
    llam_broker_ring_broker_stat_add(ring, LLAM_BROKER_RING_BROKER_STAT_SUBMIT_HEAD_PUBLISHES, 1U);
}

static int llam_broker_ring_session_reserve_batch(llam_broker_ring_t *ring,
                                                  llam_broker_ring_session_t *session,
                                                  size_t max_requests,
                                                  llam_broker_ring_submission_t *submissions,
                                                  size_t *out_count,
                                                  uint64_t *out_complete_tail) {
    uint64_t submit_head;
    uint64_t submit_tail;
    uint64_t complete_head;
    uint64_t complete_tail;
    uint64_t published_submit_head;
    uint64_t published_complete_tail;
    uint64_t submit_available;
    uint64_t complete_space;
    size_t count;
    size_t i;

    if (LLAM_UNLIKELY(!llam_broker_ring_valid(ring) ||
                      session == NULL ||
                      submissions == NULL ||
                      out_count == NULL ||
                      out_complete_tail == NULL ||
                      max_requests == 0U)) {
        errno = EINVAL;
        return -1;
    }
    if (max_requests > LLAM_BROKER_RING_SERVE_BATCH_MAX) {
        max_requests = LLAM_BROKER_RING_SERVE_BATCH_MAX;
    }

    submit_head = session->submit_head;
    published_submit_head = atomic_load_explicit(&ring->submit_head.value, memory_order_acquire);
    if (LLAM_UNLIKELY(published_submit_head != submit_head)) {
        /*
         * Broker progress cursors are private, but the published cursor must
         * mirror them. A mismatch means the shared mapping was reset or
         * corrupted; continuing could skip fresh submissions or replay stale
         * completion slots. Fail closed rather than trying to resynchronize
         * from client-writable state.
         */
        errno = EINVAL;
        return -1;
    }
    submit_tail = atomic_load_explicit(&ring->submit_tail.value, memory_order_acquire);
    if (LLAM_UNLIKELY(!llam_broker_ring_window_valid(submit_head, submit_tail))) {
        errno = EINVAL;
        return -1;
    }
    submit_available = submit_tail - submit_head;
    if (LLAM_UNLIKELY(submit_available == 0U)) {
        llam_broker_ring_broker_stat_add(ring, LLAM_BROKER_RING_BROKER_STAT_SUBMIT_EMPTY, 1U);
        errno = EAGAIN;
        return -1;
    }

    complete_tail = session->complete_tail;
    published_complete_tail = atomic_load_explicit(&ring->complete_tail.value, memory_order_acquire);
    if (LLAM_UNLIKELY(published_complete_tail != complete_tail)) {
        /*
         * The client owns complete_head, but complete_tail is broker-published.
         * If it no longer matches the broker-private cursor, publishing a new
         * tail can expose stale or zeroed completion entries as successful
         * responses.
         */
        errno = EINVAL;
        return -1;
    }
    complete_head = atomic_load_explicit(&ring->complete_head.value, memory_order_acquire);
    if (LLAM_UNLIKELY(!llam_broker_ring_window_valid(complete_head, complete_tail))) {
        errno = EINVAL;
        return -1;
    }
    complete_space = LLAM_BROKER_RING_CAP - (complete_tail - complete_head);
    if (LLAM_UNLIKELY(complete_space == 0U)) {
        llam_broker_ring_broker_stat_add(ring, LLAM_BROKER_RING_BROKER_STAT_COMPLETE_FULL, 1U);
        errno = EAGAIN;
        return -1;
    }

    count = max_requests;
    if ((uint64_t)count > submit_available) {
        count = (size_t)submit_available;
    }
    if ((uint64_t)count > complete_space) {
        count = (size_t)complete_space;
    }
    for (i = 0U; i < count; ++i) {
        submissions[i] = ring->submissions[(submit_head + (uint64_t)i) & LLAM_BROKER_RING_MASK];
    }
    *out_count = count;
    *out_complete_tail = complete_tail;
    return 0;
}

static int llam_broker_ring_session_validate_publish_locked(const llam_broker_ring_t *ring,
                                                            const llam_broker_ring_session_t *session,
                                                            uint64_t completion_tail) {
    uint64_t published_submit_head;
    uint64_t published_complete_tail;

    if (LLAM_UNLIKELY(!llam_broker_ring_valid(ring) || session == NULL)) {
        errno = EINVAL;
        return -1;
    }

    published_submit_head = atomic_load_explicit(&ring->submit_head.value, memory_order_acquire);
    published_complete_tail = atomic_load_explicit(&ring->complete_tail.value, memory_order_acquire);
    if (LLAM_UNLIKELY(published_submit_head != session->submit_head ||
                      published_complete_tail != completion_tail)) {
        /*
         * Operations can block outside the broker lock. Re-check broker-owned
         * published cursors before exposing completions so a client cannot
         * reset or corrupt the ring while an operation is in flight and then
         * receive a completion that masks the tampering.
         */
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static bool llam_broker_ring_spawned_task_token(const llam_broker_ring_t *ring,
                                                const llam_broker_ring_submission_t *submission,
                                                const llam_broker_ring_completion_t *completion,
                                                llam_capability_token_t *out_token) {
    uint64_t end;

    if (out_token != NULL) {
        memset(out_token, 0, sizeof(*out_token));
    }
    if (ring == NULL ||
        submission == NULL ||
        completion == NULL ||
        out_token == NULL ||
        submission->op != LLAM_BROKER_RING_OP_TASK_SPAWN ||
        completion->status != 0 ||
        submission->arg2 > (uint64_t)LLAM_BROKER_RING_DATA_BYTES ||
        sizeof(*out_token) > LLAM_BROKER_RING_DATA_BYTES) {
        return false;
    }
    end = submission->arg2 + (uint64_t)sizeof(*out_token);
    if (end < submission->arg2 || end > (uint64_t)LLAM_BROKER_RING_DATA_BYTES) {
        return false;
    }
    memcpy(out_token, ring->data + (size_t)submission->arg2, sizeof(*out_token));
    return out_token->family == LLAM_BROKER_CAP_FAMILY_TASK;
}

static size_t llam_broker_ring_authority_safe_batch_count(const llam_broker_ring_submission_t *submissions,
                                                          size_t count) {
    size_t i;

    if (submissions == NULL) {
        return 0U;
    }
    for (i = 0U; i < count; ++i) {
        if (submissions[i].op == LLAM_BROKER_RING_OP_TASK_SPAWN) {
            return i == 0U ? 1U : i;
        }
    }
    return count;
}

static void llam_broker_ring_rollback_unpublished_tasks(llam_broker_t *broker,
                                                        const llam_capability_token_t *tokens,
                                                        const bool *created,
                                                        size_t count) {
    size_t i;

    for (i = 0U; i < count; ++i) {
        llam_broker_wire_request_t request;
        llam_broker_wire_response_t response;

        if (!created[i]) {
            continue;
        }
        memset(&request, 0, sizeof(request));
        memset(&response, 0, sizeof(response));
        request.op = (uint32_t)LLAM_BROKER_WIRE_OP_TASK_SPAWN;
        response.status = 0;
        response.token = tokens[i];
        llam_broker_rollback_created_response(broker, &request, &response, 0U);
    }
}

int llam_broker_ring_serve_locked_session_batch(llam_broker_t *broker,
                                                llam_broker_ring_t *ring,
                                                llam_broker_ring_session_t *session,
                                                size_t max_requests,
                                                size_t *out_served) {
    llam_broker_ring_submission_t submissions[LLAM_BROKER_RING_SERVE_BATCH_MAX];
    llam_broker_ring_completion_t completions[LLAM_BROKER_RING_SERVE_BATCH_MAX];
    llam_capability_token_t created_task_tokens[LLAM_BROKER_RING_SERVE_BATCH_MAX];
    bool created_task[LLAM_BROKER_RING_SERVE_BATCH_MAX];
    llam_broker_ring_mapping_t poisoned_mapping;
    uint64_t completion_tail;
    uint64_t serve_start_ns;
    uint64_t serve_end_ns;
    size_t count;
    size_t i;

    if (LLAM_UNLIKELY(out_served == NULL)) {
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        errno = EINVAL;
        return -1;
    }
    *out_served = 0U;
    serve_start_ns = llam_now_ns();
    if (llam_broker_ring_session_reserve_batch(ring,
                                               session,
                                               max_requests,
                                               submissions,
                                               &count,
                                               &completion_tail) != 0) {
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        return -1;
    }
    /*
     * Copy submissions while the broker table lock is still held, then mark the
     * session busy. The client owns the shared mapping and may mutate slots
     * after publishing them; the local copy is the broker's immutable worklist.
     */
    count = llam_broker_ring_authority_safe_batch_count(submissions, count);
    if (count == 0U) {
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        errno = EINVAL;
        return -1;
    }
    session->busy = true;
    llam_broker_unlock(broker);

    memset(created_task_tokens, 0, sizeof(created_task_tokens));
    memset(created_task, 0, sizeof(created_task));
    for (i = 0U; i < count; ++i) {
        llam_broker_ring_execute_submission(broker, ring, &submissions[i], &completions[i]);
        created_task[i] = llam_broker_ring_spawned_task_token(ring,
                                                              &submissions[i],
                                                              &completions[i],
                                                              &created_task_tokens[i]);
    }

    if (llam_broker_lock(broker) != 0) {
        llam_broker_end_op(broker);
        return -1;
    }
    if (llam_broker_ring_session_validate_publish_locked(ring, session, completion_tail) != 0) {
        int saved_errno = errno;
        bool unmap_mapping;

        for (i = 0U; i < count; ++i) {
            llam_broker_ring_clear_submission_output(ring, &submissions[i]);
        }
        /*
         * A cursor mismatch after execution means the data plane is no longer
         * trustworthy. Drop the broker-private session so stale cursors cannot
         * replay the already-executed submission on a later serve attempt.
         */
        unmap_mapping = llam_broker_ring_session_take_mapping(session, &poisoned_mapping);
        llam_broker_unlock(broker);
        llam_broker_ring_rollback_unpublished_tasks(broker, created_task_tokens, created_task, count);
        if (unmap_mapping) {
            llam_broker_ring_unmap(&poisoned_mapping);
        }
        llam_broker_end_op(broker);
        errno = saved_errno;
        return -1;
    }
    for (i = 0U; i < count; ++i) {
        ring->completions[(completion_tail + (uint64_t)i) & LLAM_BROKER_RING_MASK] = completions[i];
    }
    session->submit_head += (uint64_t)count;
    session->complete_tail = completion_tail + (uint64_t)count;
    atomic_store_explicit(&ring->complete_tail.value, session->complete_tail, memory_order_release);
    llam_broker_ring_broker_stat_add(ring, LLAM_BROKER_RING_BROKER_STAT_COMPLETE_TAIL_PUBLISHES, 1U);
    llam_broker_ring_session_submit_publish(ring, session);
    serve_end_ns = llam_now_ns();
    if (serve_end_ns >= serve_start_ns) {
        uint64_t latency_ns = serve_end_ns - serve_start_ns;

        llam_broker_ring_broker_stat_add(ring, LLAM_BROKER_RING_BROKER_STAT_SERVE_LATENCY_NS_TOTAL, latency_ns);
        llam_broker_ring_broker_stat_max(ring, LLAM_BROKER_RING_BROKER_STAT_SERVE_LATENCY_NS_MAX, latency_ns);
    }
    llam_broker_ring_broker_stat_add(ring, LLAM_BROKER_RING_BROKER_STAT_SERVE_CALLS, (uint64_t)count);
    llam_broker_ring_broker_stat_add(ring, LLAM_BROKER_RING_BROKER_STAT_SERVE_SUCCESS, (uint64_t)count);
    *out_served = count;
    session->busy = false;
    llam_broker_unlock(broker);
    llam_broker_end_op(broker);
    return 0;
}

int llam_broker_ring_serve_locked_session(llam_broker_t *broker,
                                          llam_broker_ring_t *ring,
                                          llam_broker_ring_session_t *session) {
    size_t served;

    return llam_broker_ring_serve_locked_session_batch(broker, ring, session, 1U, &served);
}
