/**
 * @file src/core/runtime_broker_ring_dispatch.c
 * @brief Broker shared-memory ring operation dispatcher.
 *
 * @details
 * Session lookup and raw queue primitives live in runtime_broker_ring.c. This
 * file owns execution of one validated submission and publication of its
 * completion, including fail-closed clearing of client-visible output ranges.
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

static int llam_broker_ring_session_complete_reserve(const llam_broker_ring_t *ring,
                                                     llam_broker_ring_session_t *session,
                                                     uint64_t *out_tail) {
    uint64_t head;
    uint64_t tail;

    if (LLAM_UNLIKELY(ring == NULL || session == NULL || out_tail == NULL)) {
        errno = EINVAL;
        return -1;
    }
    tail = session->complete_tail;
    head = atomic_load_explicit(&ring->complete_head.value, memory_order_acquire);
    if (LLAM_UNLIKELY(!llam_broker_ring_window_valid(head, tail))) {
        errno = EINVAL;
        return -1;
    }
    if (LLAM_UNLIKELY(tail - head >= LLAM_BROKER_RING_CAP)) {
        errno = EAGAIN;
        return -1;
    }
    *out_tail = tail;
    return 0;
}

static int llam_broker_ring_session_submit_pop(llam_broker_ring_t *ring,
                                               llam_broker_ring_session_t *session,
                                               llam_broker_ring_submission_t *out_entry) {
    uint64_t head;
    uint64_t tail;

    if (LLAM_UNLIKELY(!llam_broker_ring_valid(ring) || session == NULL || out_entry == NULL)) {
        errno = EINVAL;
        return -1;
    }
    head = session->submit_head;
    tail = atomic_load_explicit(&ring->submit_tail.value, memory_order_acquire);
    if (LLAM_UNLIKELY(!llam_broker_ring_window_valid(head, tail))) {
        errno = EINVAL;
        return -1;
    }
    if (LLAM_UNLIKELY(head == tail)) {
        errno = EAGAIN;
        return -1;
    }
    *out_entry = ring->submissions[head & LLAM_BROKER_RING_MASK];
    session->submit_head = head + 1U;
    return 0;
}

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

static void llam_broker_ring_session_complete_publish(llam_broker_ring_t *ring,
                                                      llam_broker_ring_session_t *session,
                                                      uint64_t tail,
                                                      const llam_broker_ring_completion_t *entry) {
    ring->completions[tail & LLAM_BROKER_RING_MASK] = *entry;
    session->complete_tail = tail + 1U;
    atomic_store_explicit(&ring->complete_tail.value, session->complete_tail, memory_order_release);
    llam_broker_ring_broker_stat_add(ring, LLAM_BROKER_RING_BROKER_STAT_COMPLETE_TAIL_PUBLISHES, 1U);
}

static int llam_broker_ring_data_range(uint64_t offset, uint64_t length, size_t *out_offset, size_t *out_length) {
    uint64_t end;

    if (LLAM_UNLIKELY(out_offset == NULL || out_length == NULL || length == 0U)) {
        errno = EINVAL;
        return -1;
    }
    end = offset + length;
    if (LLAM_UNLIKELY(end < offset || end > LLAM_BROKER_RING_DATA_BYTES || length > (uint64_t)SIZE_MAX)) {
        errno = EINVAL;
        return -1;
    }
    *out_offset = (size_t)offset;
    *out_length = (size_t)length;
    return 0;
}

static void llam_broker_ring_clear_output(llam_broker_ring_t *ring, size_t offset, size_t length) {
    /*
     * The shared data area is client-visible. Once a range has been validated,
     * failed output-producing operations clear it before publishing the failure
     * completion so stale successful output cannot be mistaken for fresh data.
     */
    memset(ring->data + offset, 0, length);
}

static void llam_broker_ring_clear_output_suffix(llam_broker_ring_t *ring,
                                                 size_t offset,
                                                 size_t requested,
                                                 size_t produced) {
    /*
     * A successful short read/receive still leaves client-visible bytes in the
     * rest of the requested output window. Clear the suffix so clients that
     * mishandle result0 cannot observe stale data from an earlier operation.
     */
    if (produced < requested) {
        memset(ring->data + offset + produced, 0, requested - produced);
    }
}

static void llam_broker_ring_completion_fail(llam_broker_ring_completion_t *completion, int error_code) {
    /*
     * Ring completions are the only status channel visible to an untrusted
     * client. Normalize failed operations through one helper so new op handlers
     * do not accidentally publish success-looking stale result fields.
     */
    completion->status = -1;
    completion->error_code = error_code != 0 ? error_code : EINVAL;
}

static void llam_broker_ring_completion_fail_errno(llam_broker_ring_completion_t *completion) {
    llam_broker_ring_completion_fail(completion, errno);
}

int llam_broker_ring_serve_locked_session(llam_broker_t *broker,
                                          llam_broker_ring_t *ring,
                                          llam_broker_ring_session_t *session) {
    llam_broker_ring_submission_t submission;
    llam_broker_ring_completion_t completion;
    size_t ring_offset;
    size_t length;
    uint64_t completion_tail;
    uint64_t serve_start_ns;
    uint64_t serve_end_ns;

    /*
     * The caller owns an active broker op and the broker lock. Reserve the
     * queue slots and mark busy before unlocking so forget/destroy paths see a
     * pinned session for the entire blocking broker operation.
     */
    llam_broker_ring_broker_stat_add(ring, LLAM_BROKER_RING_BROKER_STAT_SERVE_CALLS, 1U);
    serve_start_ns = llam_now_ns();
    if (llam_broker_ring_session_complete_reserve(ring, session, &completion_tail) != 0) {
        if (errno == EAGAIN) {
            llam_broker_ring_broker_stat_add(ring, LLAM_BROKER_RING_BROKER_STAT_COMPLETE_FULL, 1U);
        }
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        return -1;
    }
    if (llam_broker_ring_session_submit_pop(ring, session, &submission) != 0) {
        if (errno == EAGAIN) {
            llam_broker_ring_broker_stat_add(ring, LLAM_BROKER_RING_BROKER_STAT_SUBMIT_EMPTY, 1U);
        }
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        return -1;
    }
    session->busy = true;
    llam_broker_unlock(broker);

    memset(&completion, 0, sizeof(completion));
    completion.request_id = submission.request_id;
    switch ((llam_broker_ring_op_t)submission.op) {
    case LLAM_BROKER_RING_OP_NOP:
        completion.status = 0;
        break;
    case LLAM_BROKER_RING_OP_CAP_VALIDATE:
        if (llam_broker_validate_cap(broker, &submission.token, submission.arg0) == 0) {
            completion.status = 0;
            completion.result0 = 1U;
        } else {
            llam_broker_ring_completion_fail_errno(&completion);
        }
        break;
    case LLAM_BROKER_RING_OP_CAP_ATTENUATE:
        if (llam_broker_ring_data_range(submission.arg2,
                                        sizeof(llam_capability_token_t),
                                        &ring_offset,
                                        &length) == 0) {
            llam_capability_token_t attenuated;

            if (llam_broker_attenuate_cap(broker, &submission.token, submission.arg0, &attenuated) == 0) {
                memcpy(ring->data + ring_offset, &attenuated, sizeof(attenuated));
                completion.status = 0;
                completion.result0 = (uint64_t)length;
            } else {
                llam_broker_ring_clear_output(ring, ring_offset, length);
                llam_broker_ring_completion_fail_errno(&completion);
            }
        } else {
            llam_broker_ring_completion_fail_errno(&completion);
        }
        break;
    case LLAM_BROKER_RING_OP_CAP_REVOKE:
        if (llam_broker_ring_data_range(submission.arg2,
                                        sizeof(llam_capability_token_t),
                                        &ring_offset,
                                        &length) == 0) {
            llam_capability_token_t replacement;

            if (llam_broker_revoke_object_cap(broker, &submission.token, submission.arg0, &replacement) == 0) {
                memcpy(ring->data + ring_offset, &replacement, sizeof(replacement));
                completion.status = 0;
                completion.result0 = (uint64_t)length;
            } else {
                llam_broker_ring_clear_output(ring, ring_offset, length);
                llam_broker_ring_completion_fail_errno(&completion);
            }
        } else {
            llam_broker_ring_completion_fail_errno(&completion);
        }
        break;
    case LLAM_BROKER_RING_OP_BUFFER_READ:
        if (llam_broker_ring_data_range(submission.arg2, submission.arg1, &ring_offset, &length) == 0) {
            if (llam_broker_read_buffer(broker,
                                        &submission.token,
                                        submission.arg0,
                                        ring->data + ring_offset,
                                        length) == 0) {
                completion.status = 0;
                completion.result0 = (uint64_t)length;
            } else {
                llam_broker_ring_clear_output(ring, ring_offset, length);
                llam_broker_ring_completion_fail_errno(&completion);
            }
        } else {
            llam_broker_ring_completion_fail_errno(&completion);
        }
        break;
    case LLAM_BROKER_RING_OP_BUFFER_WRITE:
        if (llam_broker_ring_data_range(submission.arg2, submission.arg1, &ring_offset, &length) == 0 &&
            llam_broker_write_buffer(broker,
                                     &submission.token,
                                     submission.arg0,
                                     ring->data + ring_offset,
                                     length) == 0) {
            completion.status = 0;
            completion.result0 = (uint64_t)length;
        } else {
            llam_broker_ring_completion_fail_errno(&completion);
        }
        break;
    case LLAM_BROKER_RING_OP_DESCRIPTOR_READ:
        if (llam_broker_ring_data_range(submission.arg2, submission.arg1, &ring_offset, &length) == 0) {
            ssize_t nread = llam_broker_read_handle(broker, &submission.token, ring->data + ring_offset, length);

            if (nread >= 0) {
                llam_broker_ring_clear_output_suffix(ring, ring_offset, length, (size_t)nread);
                completion.status = 0;
                completion.result0 = (uint64_t)nread;
            } else {
                llam_broker_ring_clear_output(ring, ring_offset, length);
                llam_broker_ring_completion_fail_errno(&completion);
            }
        } else {
            llam_broker_ring_completion_fail_errno(&completion);
        }
        break;
    case LLAM_BROKER_RING_OP_DESCRIPTOR_WRITE:
        if (llam_broker_ring_data_range(submission.arg2, submission.arg1, &ring_offset, &length) == 0) {
            ssize_t nwritten = llam_broker_write_handle(broker, &submission.token, ring->data + ring_offset, length);

            if (nwritten >= 0) {
                completion.status = 0;
                completion.result0 = (uint64_t)nwritten;
            } else {
                llam_broker_ring_completion_fail_errno(&completion);
            }
        } else {
            llam_broker_ring_completion_fail_errno(&completion);
        }
        break;
    case LLAM_BROKER_RING_OP_CHANNEL_SEND:
        if (llam_broker_ring_data_range(submission.arg2, submission.arg1, &ring_offset, &length) == 0 &&
            llam_broker_channel_send(broker, &submission.token, ring->data + ring_offset, length) == 0) {
            completion.status = 0;
            completion.result0 = (uint64_t)length;
        } else {
            llam_broker_ring_completion_fail_errno(&completion);
        }
        break;
    case LLAM_BROKER_RING_OP_CHANNEL_RECV:
        if (llam_broker_ring_data_range(submission.arg2, submission.arg1, &ring_offset, &length) == 0) {
            ssize_t nread = llam_broker_channel_recv(broker, &submission.token, ring->data + ring_offset, length);

            if (nread >= 0) {
                llam_broker_ring_clear_output_suffix(ring, ring_offset, length, (size_t)nread);
                completion.status = 0;
                completion.result0 = (uint64_t)nread;
            } else {
                llam_broker_ring_clear_output(ring, ring_offset, length);
                llam_broker_ring_completion_fail_errno(&completion);
            }
        } else {
            llam_broker_ring_completion_fail_errno(&completion);
        }
        break;
    case LLAM_BROKER_RING_OP_CHANNEL_CLOSE:
        if (llam_broker_channel_close(broker, &submission.token) == 0) {
            completion.status = 0;
        } else {
            llam_broker_ring_completion_fail_errno(&completion);
        }
        break;
    case LLAM_BROKER_RING_OP_TASK_SPAWN:
        if (llam_broker_ring_data_range(submission.arg2,
                                        sizeof(llam_capability_token_t),
                                        &ring_offset,
                                        &length) == 0) {
            llam_capability_token_t task_token;

            if (LLAM_UNLIKELY(submission.arg0 > (uint64_t)UINT32_MAX)) {
                llam_broker_ring_clear_output(ring, ring_offset, length);
                llam_broker_ring_completion_fail(&completion, EINVAL);
            } else if (llam_broker_spawn_task(broker,
                                              (uint32_t)submission.arg0,
                                              submission.arg1,
                                              LLAM_CAP_RIGHT_JOIN | LLAM_CAP_RIGHT_DETACH,
                                              &task_token) == 0) {
                memcpy(ring->data + ring_offset, &task_token, sizeof(task_token));
                completion.status = 0;
                completion.result0 = (uint64_t)length;
            } else {
                llam_broker_ring_clear_output(ring, ring_offset, length);
                llam_broker_ring_completion_fail_errno(&completion);
            }
        } else {
            llam_broker_ring_completion_fail_errno(&completion);
        }
        break;
    case LLAM_BROKER_RING_OP_TASK_JOIN:
        if (llam_broker_join_task(broker, &submission.token, &completion.result0) == 0) {
            completion.status = 0;
        } else {
            llam_broker_ring_completion_fail_errno(&completion);
        }
        break;
    case LLAM_BROKER_RING_OP_TASK_DETACH:
        if (llam_broker_detach_task(broker, &submission.token) == 0) {
            completion.status = 0;
        } else {
            llam_broker_ring_completion_fail_errno(&completion);
        }
        break;
    default:
        llam_broker_ring_completion_fail(&completion, EINVAL);
        break;
    }

    if (llam_broker_lock(broker) != 0) {
        llam_broker_end_op(broker);
        return -1;
    }
    llam_broker_ring_session_complete_publish(ring, session, completion_tail, &completion);
    llam_broker_ring_session_submit_publish(ring, session);
    serve_end_ns = llam_now_ns();
    if (serve_end_ns >= serve_start_ns) {
        uint64_t latency_ns = serve_end_ns - serve_start_ns;

        llam_broker_ring_broker_stat_add(ring, LLAM_BROKER_RING_BROKER_STAT_SERVE_LATENCY_NS_TOTAL, latency_ns);
        llam_broker_ring_broker_stat_max(ring, LLAM_BROKER_RING_BROKER_STAT_SERVE_LATENCY_NS_MAX, latency_ns);
    }
    llam_broker_ring_broker_stat_add(ring, LLAM_BROKER_RING_BROKER_STAT_SERVE_SUCCESS, 1U);
    session->busy = false;
    llam_broker_unlock(broker);
    llam_broker_end_op(broker);
    return 0;
}
