/**
 * @file src/io/windows/runtime_io_watch_windows_internal.h
 * @brief Internal declarations shared by the Windows IOCP request backend files.
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

#ifndef LLAM_IO_WINDOWS_RUNTIME_IO_WATCH_WINDOWS_INTERNAL_H
#define LLAM_IO_WINDOWS_RUNTIME_IO_WATCH_WINDOWS_INTERNAL_H

#include "../runtime_io_api_internal.h"

#include "runtime_windows_iocp.h"

#include <mswsock.h>
#include <stdint.h>

#define LLAM_WINDOWS_IOCP_BATCH_MAX 128U
#define LLAM_WINDOWS_ACCEPT_ADDR_BYTES ((DWORD)(sizeof(struct sockaddr_storage) + 16U))
#define LLAM_WINDOWS_IO_OP_MAGIC UINT64_C(0x4c4c414d57494f31)
#define LLAM_WINDOWS_POLL_BACKEND_RECV 1U
#define LLAM_WINDOWS_POLL_BACKEND_RECVFROM_PEEK 2U
#define LLAM_WINDOWS_POLL_BACKEND_SEND 3U

typedef struct llam_windows_fd_assoc {
    llam_fd_t fd;
    struct llam_windows_fd_assoc *next;
} llam_windows_fd_assoc_t;

typedef struct llam_windows_io_op {
    OVERLAPPED overlapped;
    WSABUF wsabuf;
    llam_io_req_t *req;
    llam_node_t *node;
    SOCKET accept_socket;
    uint64_t magic;
    unsigned kind;
    unsigned poll_backend;
    short poll_events;
    DWORD poll_flags;
    char poll_byte;
    struct sockaddr_storage poll_from;
    int poll_from_len;
    char accept_buffer[LLAM_WINDOWS_ACCEPT_ADDR_BYTES * 2U];
    struct llam_windows_io_op *next_free;
} llam_windows_io_op_t;

typedef struct llam_windows_accept_socket_entry {
    SOCKET socket_fd;
    SOCKET listener_fd;
    struct llam_windows_accept_socket_entry *next;
} llam_windows_accept_socket_entry_t;

int llam_windows_associate_fd(llam_node_t *node, llam_fd_t fd);
int llam_windows_load_acceptex(llam_node_t *node, SOCKET socket_fd, LPFN_ACCEPTEX *fn_out);
int llam_windows_load_connectex(llam_node_t *node, SOCKET socket_fd, LPFN_CONNECTEX *fn_out);
int llam_windows_bind_connect_socket(SOCKET socket_fd, const struct sockaddr *addr);
bool llam_windows_socket_info(llam_fd_t fd, int *family_out, int *socket_type_out);
bool llam_windows_iocp_poll_supported(llam_fd_t fd, short events);

SOCKET llam_windows_accept_socket_acquire(llam_node_t *node, SOCKET listener);
void llam_windows_accept_socket_pool_warm(llam_node_t *node, SOCKET listener, unsigned target_free);
void llam_windows_accept_socket_pool_destroy(llam_node_t *node);
llam_windows_io_op_t *llam_windows_io_op_create(llam_node_t *node, llam_io_req_t *req);
void llam_windows_io_op_free(llam_windows_io_op_t *op);
void llam_windows_io_op_pool_destroy(llam_node_t *node);
void llam_windows_complete_req(llam_node_t *node, llam_io_req_t *req, int res, bool decrement_pending);
void llam_windows_complete_submit_error(llam_node_t *node, llam_io_req_t *req, int err);
void llam_windows_process_controls(llam_node_t *node);
void llam_windows_process_submissions(llam_node_t *node);
void llam_windows_drain_completions(llam_node_t *node, DWORD timeout_ms);

#endif
