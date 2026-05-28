/**
 * @file src/core/runtime_broker_transport_ops.c
 * @brief Broker wire-operation dispatcher.
 *
 * @details
 * Transport files own byte movement over sockets or pipes. This file owns the
 * fixed control-message semantics so POSIX and Windows transports cannot drift.
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
#include "runtime_broker.h"
#include "runtime_broker_ring.h"

#include <string.h>

static void llam_broker_response_init_empty(llam_broker_wire_response_t *response) {
    memset(response, 0, sizeof(*response));
    response->magic = LLAM_BROKER_WIRE_MAGIC;
    response->version = LLAM_BROKER_WIRE_VERSION;
}

static void llam_broker_response_bind_broker(llam_broker_t *broker, llam_broker_wire_response_t *response) {
    response->runtime_id = broker != NULL && broker->runtime != NULL ? broker->runtime->runtime_id : 0U;
    response->revocation_epoch = broker != NULL ? atomic_load_explicit(&broker->revocation_epoch, memory_order_acquire) : 0U;
}

static void llam_broker_response_error(llam_broker_wire_response_t *response, int error_code) {
    llam_broker_mark_response_failure_clear_outputs(response, error_code != 0 ? error_code : EINVAL);
}

static void llam_broker_close_unclaimed_descriptor(llam_handle_t descriptor_handle) {
    if (!llam_handle_is_invalid(descriptor_handle)) {
        /*
         * Descriptor/HANDLE authority is accepted only by the explicit register
         * operation. Any malformed or wrong-op request that arrives with an
         * attached OS handle must close it immediately so a malicious transport
         * peer cannot leak broker descriptors by sending unusable headers.
         */
        llam_broker_close_handle(descriptor_handle);
    }
}

static int llam_broker_transport_rights(uint64_t requested, uint64_t allowed, uint64_t *out_rights) {
    uint64_t rights = requested;

    /*
     * Client-writable grant requests must be explicit. Treating rights==0 as
     * "all allowed rights" makes a zero-initialized or truncated request mint
     * maximum authority instead of failing closed.
     */
    if (LLAM_UNLIKELY(out_rights == NULL || rights == 0U || (rights & ~allowed) != 0U)) {
        errno = EACCES;
        return -1;
    }
    *out_rights = rights;
    return 0;
}

static int llam_broker_transport_create_grant(llam_broker_t *broker,
                                              const llam_broker_wire_request_t *request,
                                              bool is_channel,
                                              llam_capability_token_t *out_token) {
    uint64_t max_units = is_channel ? LLAM_BROKER_CHANNEL_CAPACITY : LLAM_BROKER_BUFFER_MAX_BYTES;
    uint64_t allowed = is_channel ? LLAM_BROKER_CHANNEL_TRANSPORT_RIGHTS : LLAM_BROKER_BUFFER_TRANSPORT_RIGHTS;
    uint64_t rights = 0U;
    size_t units;

    if (LLAM_UNLIKELY(request == NULL ||
                      out_token == NULL ||
                      request->slot == 0U ||
                      request->slot > max_units ||
                      request->slot > (uint64_t)SIZE_MAX)) {
        errno = EINVAL;
        return -1;
    }
    if (llam_broker_transport_rights(request->rights, allowed, &rights) != 0) {
        return -1;
    }
    units = (size_t)request->slot;
    if (is_channel) {
        return llam_broker_create_channel(broker, units, rights, out_token);
    }
    return llam_broker_register_buffer(broker, NULL, units, rights, out_token);
}

static bool llam_broker_wire_length_valid(uint64_t length) {
    return length > 0U &&
           length <= LLAM_BROKER_WIRE_DATA_BYTES &&
           length <= (uint64_t)SIZE_MAX;
}

static int llam_broker_transport_join_task(llam_broker_t *broker,
                                           const llam_capability_token_t *token,
                                           uint64_t *out_result0) {
    bool drive_allowed = false;
    int saved_errno;

    if (llam_broker_join_task(broker, token, out_result0) == 0) {
        return 0;
    }
    saved_errno = errno;
    if (saved_errno != EAGAIN) {
        errno = saved_errno;
        return -1;
    }
    /* Drive only quick predefined tasks; long sleeps stay retryable EAGAIN so
     * one client cannot pin the broker serve thread for attacker-chosen time. */
    if (llam_broker_task_join_runtime_drive_allowed(broker, token, &drive_allowed) != 0) {
        return -1;
    }
    if (!drive_allowed) {
        errno = EAGAIN;
        return -1;
    }
    if (LLAM_UNLIKELY(broker == NULL || broker->runtime == NULL)) {
        errno = EAGAIN;
        return -1;
    }
    if (llam_runtime_run_handle(broker->runtime) != 0) {
        return -1;
    }
    return llam_broker_join_task(broker, token, out_result0);
}

static size_t llam_broker_transport_serve_ring_batch_size(const llam_broker_wire_request_t *request) {
    uint64_t requested;

    /* length==0 keeps old one-request serving; nonzero values are bounded so
     * untrusted clients cannot force oversized stack batches. */
    requested = request != NULL ? request->length : 0U;
    if (requested == 0U) {
        return 1U;
    }
    if (requested > (uint64_t)LLAM_BROKER_RING_SERVE_BATCH_MAX) {
        return LLAM_BROKER_RING_SERVE_BATCH_MAX;
    }
    return (size_t)requested;
}

void llam_broker_process_request_with_descriptors(llam_broker_t *broker,
                                                  const llam_broker_wire_request_t *request,
                                                  llam_broker_wire_response_t *response,
                                                  bool *out_should_close,
                                                  llam_handle_t descriptor_handle,
                                                  llam_handle_t *out_response_descriptor) {
    int saved_errno = 0;

    if (out_response_descriptor != NULL) {
        *out_response_descriptor = LLAM_INVALID_HANDLE;
    }
    if (LLAM_UNLIKELY(response == NULL)) {
        llam_broker_close_unclaimed_descriptor(descriptor_handle);
        errno = EINVAL;
        return;
    }
    llam_broker_response_init_empty(response);
    if (out_should_close != NULL) {
        *out_should_close = false;
    }
    if (LLAM_UNLIKELY(request == NULL)) {
        llam_broker_close_unclaimed_descriptor(descriptor_handle);
        llam_broker_response_error(response, EINVAL);
        return;
    }
    if (request->magic != LLAM_BROKER_WIRE_MAGIC || request->version != LLAM_BROKER_WIRE_VERSION) {
        llam_broker_close_unclaimed_descriptor(descriptor_handle);
        llam_broker_response_error(response, EINVAL);
        return;
    }
    if (!llam_handle_is_invalid(descriptor_handle) &&
        request->op != (uint32_t)LLAM_BROKER_WIRE_OP_REGISTER_DESCRIPTOR) {
        llam_broker_close_unclaimed_descriptor(descriptor_handle);
        llam_broker_response_error(response, EINVAL);
        return;
    }
    if (LLAM_UNLIKELY(llam_broker_begin_op(broker) != 0)) {
        saved_errno = errno != 0 ? errno : EINVAL;
        /* Match transport serve active-op semantics: reject new work after
         * destroy starts, but let nested accepted requests finish. */
        llam_broker_close_unclaimed_descriptor(descriptor_handle);
        llam_broker_response_error(response, saved_errno);
        return;
    }
    llam_broker_response_bind_broker(broker, response);

    switch ((llam_broker_wire_op_t)request->op) {
    case LLAM_BROKER_WIRE_OP_PING:
        response->status = 0;
        break;
    case LLAM_BROKER_WIRE_OP_ISSUE_CAP:
        /*
         * Raw minting is a trusted in-process helper only. Exposing it on the
         * client-writable control transport would let a client request broader
         * rights for any object id/generation it can observe.
         */
        llam_broker_response_error(response, EACCES);
        break;
    case LLAM_BROKER_WIRE_OP_VALIDATE_CAP:
        if (llam_broker_validate_cap(broker, &request->token, request->required_rights) == 0) {
            response->status = 0;
        } else {
            saved_errno = errno;
            llam_broker_response_error(response, saved_errno);
        }
        break;
    case LLAM_BROKER_WIRE_OP_ATTENUATE_CAP:
        if (llam_broker_attenuate_cap(broker, &request->token, request->rights, &response->token) == 0) {
            response->status = 0;
        } else {
            saved_errno = errno;
            llam_broker_response_error(response, saved_errno);
        }
        break;
    case LLAM_BROKER_WIRE_OP_REVOKE_CAP:
        if (llam_broker_revoke_object_cap(broker, &request->token, request->rights, &response->token) == 0) {
            response->status = 0;
        } else {
            saved_errno = errno;
            llam_broker_response_error(response, saved_errno);
        }
        break;
    case LLAM_BROKER_WIRE_OP_CREATE_BUFFER:
        if (llam_broker_transport_create_grant(broker, request, false, &response->token) == 0) {
            response->status = 0;
        } else {
            saved_errno = errno;
            llam_broker_response_error(response, saved_errno);
        }
        break;
    case LLAM_BROKER_WIRE_OP_CREATE_CHANNEL:
        if (llam_broker_transport_create_grant(broker, request, true, &response->token) == 0) {
            response->status = 0;
        } else {
            saved_errno = errno;
            llam_broker_response_error(response, saved_errno);
        }
        break;
    case LLAM_BROKER_WIRE_OP_CREATE_RING:
        if (LLAM_UNLIKELY(out_response_descriptor == NULL)) {
            llam_broker_response_error(response, ENOTSUP);
            break;
        }
        if (llam_broker_transport_create_ring(broker, out_response_descriptor, &response->result1) == 0) {
            response->status = 0;
            response->result0 = response->result1;
            response->result1 = LLAM_BROKER_RING_CAP;
            response->result2 = response->result0;
        } else {
            saved_errno = errno;
            llam_broker_response_error(response, saved_errno);
        }
        break;
    case LLAM_BROKER_WIRE_OP_SERVE_RING:
        {
            size_t served = 0U;

            if (llam_broker_ring_serve_session_batch(broker,
                                                     request->slot,
                                                     llam_broker_current_subject(broker),
                                                     llam_broker_transport_serve_ring_batch_size(request),
                                                     &served) == 0) {
                response->status = 0;
                response->result0 = (uint64_t)served;
            } else {
                saved_errno = errno;
                llam_broker_response_error(response, saved_errno);
            }
        }
        break;
    case LLAM_BROKER_WIRE_OP_BUFFER_READ:
        if (!llam_broker_wire_length_valid(request->length)) {
            llam_broker_response_error(response, EINVAL);
            break;
        }
        if (llam_broker_read_buffer(broker,
                                    &request->token,
                                    request->offset,
                                    response->data,
                                    (size_t)request->length) == 0) {
            response->status = 0;
            response->result0 = request->length;
        } else {
            saved_errno = errno;
            llam_broker_response_error(response, saved_errno);
        }
        break;
    case LLAM_BROKER_WIRE_OP_BUFFER_WRITE:
        if (!llam_broker_wire_length_valid(request->length)) {
            llam_broker_response_error(response, EINVAL);
            break;
        }
        if (llam_broker_write_buffer(broker,
                                     &request->token,
                                     request->offset,
                                     request->data,
                                     (size_t)request->length) == 0) {
            response->status = 0;
            response->result0 = request->length;
        } else {
            saved_errno = errno;
            llam_broker_response_error(response, saved_errno);
        }
        break;
    case LLAM_BROKER_WIRE_OP_CHANNEL_SEND:
        if (!llam_broker_wire_length_valid(request->length)) {
            llam_broker_response_error(response, EINVAL);
            break;
        }
        if (llam_broker_channel_send(broker,
                                     &request->token,
                                     request->data,
                                     (size_t)request->length) == 0) {
            response->status = 0;
            response->result0 = request->length;
        } else {
            saved_errno = errno;
            llam_broker_response_error(response, saved_errno);
        }
        break;
    case LLAM_BROKER_WIRE_OP_CHANNEL_RECV:
        if (!llam_broker_wire_length_valid(request->length)) {
            llam_broker_response_error(response, EINVAL);
            break;
        }
        {
            ssize_t nread = llam_broker_channel_recv(broker,
                                                     &request->token,
                                                     response->data,
                                                     (size_t)request->length);

            if (nread >= 0) {
                response->status = 0;
                response->result0 = (uint64_t)nread;
            } else {
                saved_errno = errno;
                llam_broker_response_error(response, saved_errno);
            }
        }
        break;
    case LLAM_BROKER_WIRE_OP_CHANNEL_CLOSE:
        if (llam_broker_channel_close(broker, &request->token) == 0) {
            response->status = 0;
        } else {
            saved_errno = errno;
            llam_broker_response_error(response, saved_errno);
        }
        break;
    case LLAM_BROKER_WIRE_OP_REGISTER_DESCRIPTOR:
        {
            uint64_t rights = 0U;

            if (LLAM_UNLIKELY(llam_handle_is_invalid(descriptor_handle))) {
                llam_broker_response_error(response, EINVAL);
                break;
            }
            if (llam_broker_transport_rights(request->rights,
                                             LLAM_BROKER_DESCRIPTOR_TRANSPORT_RIGHTS,
                                             &rights) != 0) {
                saved_errno = errno;
                llam_broker_close_handle(descriptor_handle);
                llam_broker_response_error(response, saved_errno);
                break;
            }
            /* Only SCM_RIGHTS/DuplicateHandle transfers authority; numeric wire
             * descriptor fields are never trusted as broker-owned handles. */
            if (llam_broker_register_handle(broker, descriptor_handle, rights, true, &response->token) == 0) {
                response->status = 0;
            } else {
                saved_errno = errno;
                llam_broker_close_handle(descriptor_handle);
                llam_broker_response_error(response, saved_errno);
            }
        }
        break;
    case LLAM_BROKER_WIRE_OP_DESCRIPTOR_READ:
        if (!llam_broker_wire_length_valid(request->length)) {
            llam_broker_response_error(response, EINVAL);
            break;
        }
        {
            ssize_t nread = llam_broker_read_handle(broker,
                                                    &request->token,
                                                    response->data,
                                                    (size_t)request->length);

            if (nread >= 0) {
                response->status = 0;
                response->result0 = (uint64_t)nread;
            } else {
                saved_errno = errno;
                llam_broker_response_error(response, saved_errno);
            }
        }
        break;
    case LLAM_BROKER_WIRE_OP_DESCRIPTOR_WRITE:
        if (!llam_broker_wire_length_valid(request->length)) {
            llam_broker_response_error(response, EINVAL);
            break;
        }
        {
            ssize_t nwritten = llam_broker_write_handle(broker,
                                                        &request->token,
                                                        request->data,
                                                        (size_t)request->length);

            if (nwritten >= 0) {
                response->status = 0;
                response->result0 = (uint64_t)nwritten;
            } else {
                saved_errno = errno;
                llam_broker_response_error(response, saved_errno);
            }
        }
        break;
    case LLAM_BROKER_WIRE_OP_TASK_SPAWN:
        {
            uint64_t rights = 0U;

            if (llam_broker_transport_rights(request->rights,
                                             LLAM_CAP_RIGHT_JOIN | LLAM_CAP_RIGHT_DETACH,
                                             &rights) != 0) {
                saved_errno = errno;
                llam_broker_response_error(response, saved_errno);
                break;
            }
            if (LLAM_UNLIKELY(request->slot > (uint64_t)UINT32_MAX)) {
                llam_broker_response_error(response, EINVAL);
                break;
            }
            if (llam_broker_spawn_task(broker,
                                       (uint32_t)request->slot,
                                       request->offset,
                                       rights,
                                       &response->token) == 0) {
                response->status = 0;
            } else {
                saved_errno = errno;
                llam_broker_response_error(response, saved_errno);
            }
        }
        break;
    case LLAM_BROKER_WIRE_OP_TASK_JOIN:
        if (llam_broker_transport_join_task(broker, &request->token, &response->result0) == 0) {
            response->status = 0;
        } else {
            saved_errno = errno;
            llam_broker_response_error(response, saved_errno);
        }
        break;
    case LLAM_BROKER_WIRE_OP_TASK_DETACH:
        if (llam_broker_detach_task(broker, &request->token) == 0) {
            response->status = 0;
        } else {
            saved_errno = errno;
            llam_broker_response_error(response, saved_errno);
        }
        break;
    case LLAM_BROKER_WIRE_OP_REVOKE_ALL:
        /* Global revocation is broker-management authority, not a client
         * transport capability. */
        llam_broker_response_error(response, EACCES);
        break;
    case LLAM_BROKER_WIRE_OP_STOP:
        response->status = 0;
        if (out_should_close != NULL) {
            *out_should_close = true;
        }
        break;
    default:
        llam_broker_response_error(response, EINVAL);
        break;
    }
    llam_broker_end_op(broker);
}
