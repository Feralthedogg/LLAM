/**
 * @file src/core/runtime_broker_transport_windows_fd_stubs.c
 * @brief POSIX fd broker transport stubs for Windows builds.
 *
 * @details
 * Windows broker transport uses named pipes and HANDLE duplication. These
 * functions satisfy the cross-platform broker transport surface while making
 * unsupported Unix-domain fd paths fail explicitly with ENOTSUP.
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

#if LLAM_PLATFORM_WINDOWS

int llam_broker_read_request_fd(int fd, llam_broker_wire_request_t *request, int *out_descriptor_fd) {
    (void)fd;
    (void)request;
    (void)out_descriptor_fd;
    errno = ENOTSUP;
    return -1;
}

int llam_broker_read_response_fd(int fd, llam_broker_wire_response_t *response, int *out_descriptor_fd) {
    (void)fd;
    (void)response;
    (void)out_descriptor_fd;
    errno = ENOTSUP;
    return -1;
}

int llam_broker_write_request_with_descriptor(int fd,
                                              const llam_broker_wire_request_t *request,
                                              int descriptor_fd) {
    (void)fd;
    (void)request;
    (void)descriptor_fd;
    errno = ENOTSUP;
    return -1;
}

int llam_broker_write_response_with_descriptor(int fd,
                                               const llam_broker_wire_response_t *response,
                                               int descriptor_fd) {
    (void)fd;
    (void)response;
    (void)descriptor_fd;
    errno = ENOTSUP;
    return -1;
}

int llam_broker_serve_fd(llam_broker_t *broker, int fd) {
    (void)broker;
    (void)fd;
    errno = ENOTSUP;
    return -1;
}

int llam_broker_request_fd(int fd,
                           const llam_broker_wire_request_t *request,
                           llam_broker_wire_response_t *response) {
    (void)fd;
    (void)request;
    (void)response;
    errno = ENOTSUP;
    return -1;
}

int llam_broker_request_fd_with_descriptor(int fd,
                                           const llam_broker_wire_request_t *request,
                                           int descriptor_fd,
                                           llam_broker_wire_response_t *response) {
    (void)fd;
    (void)request;
    (void)descriptor_fd;
    (void)response;
    errno = ENOTSUP;
    return -1;
}

int llam_broker_request_fd_with_response_descriptor(int fd,
                                                    const llam_broker_wire_request_t *request,
                                                    llam_broker_wire_response_t *response,
                                                    int *out_descriptor_fd) {
    (void)fd;
    (void)request;
    (void)response;
    (void)out_descriptor_fd;
    errno = ENOTSUP;
    return -1;
}

int llam_broker_listen_unix(const char *path, int *out_fd) {
    (void)path;
    (void)out_fd;
    errno = ENOTSUP;
    return -1;
}

int llam_broker_connect_unix(const char *path, int *out_fd) {
    (void)path;
    (void)out_fd;
    errno = ENOTSUP;
    return -1;
}

int llam_broker_accept_one(int listen_fd, int *out_fd) {
    (void)listen_fd;
    (void)out_fd;
    errno = ENOTSUP;
    return -1;
}

void llam_broker_close_fd(int fd) {
    (void)fd;
}

#endif
