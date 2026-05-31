/**
 * @file src/core/runtime_broker_ring_ops.c
 * @brief Execute one validated broker ring submission.
 *
 * @details
 * Ring op execution is split from batch dispatch so fail-closed output clearing
 * and per-op capability checks are audited in one focused file. The caller owns
 * cursor validation and completion publication.
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

void llam_broker_ring_clear_submission_output(llam_broker_ring_t *ring,
                                              const llam_broker_ring_submission_t *submission) {
    uint64_t length;
    size_t ring_offset;
    size_t range_length;

    if (ring == NULL || submission == NULL) {
        return;
    }

    switch ((llam_broker_ring_op_t)submission->op) {
    case LLAM_BROKER_RING_OP_CAP_ATTENUATE:
    case LLAM_BROKER_RING_OP_CAP_REVOKE:
    case LLAM_BROKER_RING_OP_TASK_SPAWN:
        length = sizeof(llam_capability_token_t);
        break;
    case LLAM_BROKER_RING_OP_BUFFER_READ:
    case LLAM_BROKER_RING_OP_DESCRIPTOR_READ:
    case LLAM_BROKER_RING_OP_CHANNEL_RECV:
        length = submission->arg1;
        break;
    default:
        return;
    }

    if (llam_broker_ring_data_range(submission->arg2, length, &ring_offset, &range_length) == 0) {
        llam_broker_ring_clear_output(ring, ring_offset, range_length);
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

void llam_broker_ring_execute_submission(llam_broker_t *broker,
                                         llam_broker_ring_t *ring,
                                         const llam_broker_ring_submission_t *submission,
                                         llam_broker_ring_completion_t *completion) {
    size_t ring_offset;
    size_t length;

    memset(completion, 0, sizeof(*completion));
    completion->request_id = submission->request_id;
    switch ((llam_broker_ring_op_t)submission->op) {
    case LLAM_BROKER_RING_OP_NOP:
        completion->status = 0;
        break;
    case LLAM_BROKER_RING_OP_CAP_VALIDATE:
        if (llam_broker_validate_cap(broker, &submission->token, submission->arg0) == 0) {
            completion->status = 0;
            completion->result0 = 1U;
        } else {
            llam_broker_ring_completion_fail_errno(completion);
        }
        break;
    case LLAM_BROKER_RING_OP_CAP_ATTENUATE:
        if (llam_broker_ring_data_range(submission->arg2,
                                        sizeof(llam_capability_token_t),
                                        &ring_offset,
                                        &length) == 0) {
            llam_capability_token_t attenuated;

            if (llam_broker_attenuate_cap(broker, &submission->token, submission->arg0, &attenuated) == 0) {
                memcpy(ring->data + ring_offset, &attenuated, sizeof(attenuated));
                completion->status = 0;
                completion->result0 = (uint64_t)length;
            } else {
                llam_broker_ring_clear_output(ring, ring_offset, length);
                llam_broker_ring_completion_fail_errno(completion);
            }
        } else {
            llam_broker_ring_completion_fail_errno(completion);
        }
        break;
    case LLAM_BROKER_RING_OP_CAP_REVOKE:
        if (llam_broker_ring_data_range(submission->arg2,
                                        sizeof(llam_capability_token_t),
                                        &ring_offset,
                                        &length) == 0) {
            llam_capability_token_t replacement;

            if (llam_broker_revoke_object_cap(broker, &submission->token, submission->arg0, &replacement) == 0) {
                memcpy(ring->data + ring_offset, &replacement, sizeof(replacement));
                completion->status = 0;
                completion->result0 = (uint64_t)length;
            } else {
                llam_broker_ring_clear_output(ring, ring_offset, length);
                llam_broker_ring_completion_fail_errno(completion);
            }
        } else {
            llam_broker_ring_completion_fail_errno(completion);
        }
        break;
    case LLAM_BROKER_RING_OP_BUFFER_READ:
        if (llam_broker_ring_data_range(submission->arg2, submission->arg1, &ring_offset, &length) == 0) {
            if (llam_broker_read_buffer(broker,
                                        &submission->token,
                                        submission->arg0,
                                        ring->data + ring_offset,
                                        length) == 0) {
                completion->status = 0;
                completion->result0 = (uint64_t)length;
            } else {
                llam_broker_ring_clear_output(ring, ring_offset, length);
                llam_broker_ring_completion_fail_errno(completion);
            }
        } else {
            llam_broker_ring_completion_fail_errno(completion);
        }
        break;
    case LLAM_BROKER_RING_OP_BUFFER_WRITE:
        if (llam_broker_ring_data_range(submission->arg2, submission->arg1, &ring_offset, &length) == 0 &&
            llam_broker_write_buffer(broker,
                                     &submission->token,
                                     submission->arg0,
                                     ring->data + ring_offset,
                                     length) == 0) {
            completion->status = 0;
            completion->result0 = (uint64_t)length;
        } else {
            llam_broker_ring_completion_fail_errno(completion);
        }
        break;
    case LLAM_BROKER_RING_OP_DESCRIPTOR_READ:
        if (llam_broker_ring_data_range(submission->arg2, submission->arg1, &ring_offset, &length) == 0) {
            ssize_t nread = llam_broker_read_handle(broker, &submission->token, ring->data + ring_offset, length);

            if (nread >= 0) {
                llam_broker_ring_clear_output_suffix(ring, ring_offset, length, (size_t)nread);
                completion->status = 0;
                completion->result0 = (uint64_t)nread;
            } else {
                llam_broker_ring_clear_output(ring, ring_offset, length);
                llam_broker_ring_completion_fail_errno(completion);
            }
        } else {
            llam_broker_ring_completion_fail_errno(completion);
        }
        break;
    case LLAM_BROKER_RING_OP_DESCRIPTOR_WRITE:
        if (llam_broker_ring_data_range(submission->arg2, submission->arg1, &ring_offset, &length) == 0) {
            ssize_t nwritten = llam_broker_write_handle(broker, &submission->token, ring->data + ring_offset, length);

            if (nwritten >= 0) {
                completion->status = 0;
                completion->result0 = (uint64_t)nwritten;
            } else {
                llam_broker_ring_completion_fail_errno(completion);
            }
        } else {
            llam_broker_ring_completion_fail_errno(completion);
        }
        break;
    case LLAM_BROKER_RING_OP_CHANNEL_SEND:
        if (llam_broker_ring_data_range(submission->arg2, submission->arg1, &ring_offset, &length) == 0 &&
            llam_broker_channel_send(broker, &submission->token, ring->data + ring_offset, length) == 0) {
            completion->status = 0;
            completion->result0 = (uint64_t)length;
        } else {
            llam_broker_ring_completion_fail_errno(completion);
        }
        break;
    case LLAM_BROKER_RING_OP_CHANNEL_RECV:
        if (llam_broker_ring_data_range(submission->arg2, submission->arg1, &ring_offset, &length) == 0) {
            ssize_t nread = llam_broker_channel_recv(broker, &submission->token, ring->data + ring_offset, length);

            if (nread >= 0) {
                llam_broker_ring_clear_output_suffix(ring, ring_offset, length, (size_t)nread);
                completion->status = 0;
                completion->result0 = (uint64_t)nread;
            } else {
                llam_broker_ring_clear_output(ring, ring_offset, length);
                llam_broker_ring_completion_fail_errno(completion);
            }
        } else {
            llam_broker_ring_completion_fail_errno(completion);
        }
        break;
    case LLAM_BROKER_RING_OP_CHANNEL_CLOSE:
        if (llam_broker_channel_close(broker, &submission->token) == 0) {
            completion->status = 0;
        } else {
            llam_broker_ring_completion_fail_errno(completion);
        }
        break;
    case LLAM_BROKER_RING_OP_TASK_SPAWN:
        if (llam_broker_ring_data_range(submission->arg2,
                                        sizeof(llam_capability_token_t),
                                        &ring_offset,
                                        &length) == 0) {
            llam_capability_token_t task_token;

            if (LLAM_UNLIKELY(submission->arg0 > (uint64_t)UINT32_MAX)) {
                llam_broker_ring_clear_output(ring, ring_offset, length);
                llam_broker_ring_completion_fail(completion, EINVAL);
            } else if (llam_broker_spawn_task(broker,
                                              (uint32_t)submission->arg0,
                                              submission->arg1,
                                              LLAM_CAP_RIGHT_JOIN | LLAM_CAP_RIGHT_DETACH,
                                              &task_token) == 0) {
                memcpy(ring->data + ring_offset, &task_token, sizeof(task_token));
                completion->status = 0;
                completion->result0 = (uint64_t)length;
            } else {
                llam_broker_ring_clear_output(ring, ring_offset, length);
                llam_broker_ring_completion_fail_errno(completion);
            }
        } else {
            llam_broker_ring_completion_fail_errno(completion);
        }
        break;
    case LLAM_BROKER_RING_OP_TASK_JOIN:
        if (llam_broker_join_task(broker, &submission->token, &completion->result0) == 0) {
            completion->status = 0;
        } else {
            llam_broker_ring_completion_fail_errno(completion);
        }
        break;
    case LLAM_BROKER_RING_OP_TASK_DETACH:
        if (llam_broker_detach_task(broker, &submission->token) == 0) {
            completion->status = 0;
        } else {
            llam_broker_ring_completion_fail_errno(completion);
        }
        break;
    default:
        llam_broker_ring_completion_fail(completion, EINVAL);
        break;
    }
}
