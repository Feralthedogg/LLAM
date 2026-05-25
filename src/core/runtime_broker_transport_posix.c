/**
 * @file src/core/runtime_broker_transport_posix.c
 * @brief POSIX broker transport descriptor-passing helpers.
 *
 * @details
 * The control wire format never treats a numeric fd field as authority. POSIX
 * descriptor grants are accepted only when the kernel attaches an fd through
 * SCM_RIGHTS ancillary data on the Unix-domain control socket.
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

#if !LLAM_PLATFORM_WINDOWS

#include <sys/stat.h>
#include <unistd.h>

static int llam_broker_fd_transport_id(int fd, uintptr_t *out_transport_id) {
    struct stat info;
    uint64_t mixed;

    if (LLAM_UNLIKELY(fd < 0 || out_transport_id == NULL)) {
        errno = EINVAL;
        return -1;
    }
    if (fstat(fd, &info) != 0) {
        return -1;
    }
    /*
     * A local-control session is an OS object, not just an integer fd. Mixing
     * the socket inode into the subject-table key prevents stale `serve_one`
     * state from following a later connection that reuses the same fd number.
     */
    mixed = llam_public_slot_mix64((uint64_t)(uintptr_t)fd);
    mixed ^= llam_public_slot_mix64((uint64_t)info.st_dev);
    mixed ^= llam_public_slot_mix64((uint64_t)info.st_ino);
    if (mixed == 0U) {
        mixed = llam_public_slot_mix64((uint64_t)(uintptr_t)fd ^ UINT64_C(0x9e3779b97f4a7c15));
    }
    *out_transport_id = (uintptr_t)mixed;
    return 0;
}

static int llam_broker_serve_one_fd_subject(llam_broker_t *broker,
                                            int fd,
                                            uint64_t subject_id,
                                            bool *out_should_close) {
    llam_broker_wire_request_t request;
    llam_broker_wire_response_t response;
    int descriptor_fd = -1;
    llam_handle_t response_descriptor = LLAM_INVALID_HANDLE;

    if (out_should_close != NULL) {
        *out_should_close = false;
    }
    if (LLAM_UNLIKELY(fd < 0)) {
        errno = EINVAL;
        return -1;
    }
    if (llam_broker_read_request_fd(fd, &request, &descriptor_fd) != 0) {
        return -1;
    }
    if (llam_broker_begin_op_subject(broker, subject_id) != 0) {
        if (descriptor_fd >= 0) {
            close(descriptor_fd);
        }
        return -1;
    }
    llam_broker_process_request_with_descriptors(broker,
                                                 &request,
                                                 &response,
                                                 out_should_close,
                                                 (llam_handle_t)descriptor_fd,
                                                 &response_descriptor);
    if (!llam_handle_is_invalid(response_descriptor)) {
        if (llam_broker_write_response_with_descriptor(fd, &response, (int)response_descriptor) != 0) {
            int saved_errno = errno;

            if (request.op == (uint32_t)LLAM_BROKER_WIRE_OP_CREATE_RING &&
                response.status == 0 &&
                response.result2 != 0U) {
                (void)llam_broker_ring_forget_session(broker, response.result2, subject_id);
            }
            close((int)response_descriptor);
            llam_broker_end_op(broker);
            errno = saved_errno;
            return -1;
        }
        close((int)response_descriptor);
    } else if (llam_broker_write_response_fd(fd, &response) != 0) {
        int saved_errno = errno;

        if (request.op == (uint32_t)LLAM_BROKER_WIRE_OP_CREATE_RING &&
            response.status == 0 &&
            response.result2 != 0U) {
            (void)llam_broker_ring_forget_session(broker, response.result2, subject_id);
        }
        llam_broker_end_op(broker);
        errno = saved_errno;
        return -1;
    }
    llam_broker_end_op(broker);
    return 0;
}

int llam_broker_serve_one_fd(llam_broker_t *broker, int fd, bool *out_should_close) {
    uintptr_t transport_id;
    uint64_t subject_id;
    int rc;

    if (llam_broker_fd_transport_id(fd, &transport_id) != 0) {
        return -1;
    }
    if (llam_broker_transport_subject(broker, transport_id, &subject_id) != 0) {
        return -1;
    }
    rc = llam_broker_serve_one_fd_subject(broker, fd, subject_id, out_should_close);

    if (rc != 0 || (out_should_close != NULL && *out_should_close)) {
        llam_broker_forget_transport_subject(broker, transport_id);
    }
    return rc;
}

int llam_broker_serve_fd(llam_broker_t *broker, int fd) {
    bool should_close = false;
    uintptr_t transport_id;
    uint64_t subject_id;
    int rc = 0;

    if (llam_broker_fd_transport_id(fd, &transport_id) != 0) {
        return -1;
    }
    if (llam_broker_transport_subject(broker, transport_id, &subject_id) != 0) {
        return -1;
    }
    while (!should_close) {
        if (llam_broker_serve_one_fd_subject(broker, fd, subject_id, &should_close) != 0) {
            rc = -1;
            break;
        }
    }
    llam_broker_forget_transport_subject(broker, transport_id);
    return rc;
}

int llam_broker_serve_one_handle(llam_broker_t *broker, llam_handle_t handle, bool *out_should_close) {
    return llam_broker_serve_one_fd(broker, (int)handle, out_should_close);
}

int llam_broker_serve_handle(llam_broker_t *broker, llam_handle_t handle) {
    return llam_broker_serve_fd(broker, (int)handle);
}

int llam_broker_request_handle(llam_handle_t handle,
                               const llam_broker_wire_request_t *request,
                               llam_broker_wire_response_t *response) {
    return llam_broker_request_fd((int)handle, request, response);
}

int llam_broker_request_handle_with_descriptor(llam_handle_t handle,
                                               const llam_broker_wire_request_t *request,
                                               llam_handle_t descriptor_handle,
                                               llam_broker_wire_response_t *response) {
    return llam_broker_request_fd_with_descriptor((int)handle, request, (int)descriptor_handle, response);
}

#endif
