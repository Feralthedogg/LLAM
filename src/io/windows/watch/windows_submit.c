/**
 * @file src/io/windows/watch/windows_submit.c
 * @brief Windows IOCP request submission path.
 *
 * @details
 * Submission pins the descriptor association under the lifecycle lock and
 * copies its generation into the overlapped operation before the backend can
 * own it. A helper result of @c -1 means no completion packet owns the request,
 * @c 0 transfers lifetime to IOCP, and @c 1 reports synchronous success on a
 * handle configured to suppress that packet. Completion revalidates the saved
 * generation, so reuse of the same numeric HANDLE or SOCKET cannot redirect an
 * older operation to a replacement association.
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

#include "io/windows/runtime_io_watch_windows_internal.h"

static bool llam_windows_req_is_handle_rw(const llam_io_req_t *req) {
    return req != NULL &&
           (req->kind == LLAM_IO_KIND_HANDLE_READ ||
            req->kind == LLAM_IO_KIND_HANDLE_WRITE ||
            req->kind == LLAM_IO_KIND_HANDLE_PREAD ||
            req->kind == LLAM_IO_KIND_HANDLE_PWRITE);
}

static bool llam_windows_op_skips_completion_on_success(
    const llam_windows_io_op_t *op) {
    if (op == NULL || op->association == NULL) {
        return false;
    }
    return op->association->skip_completion_on_success != 0U;
}

/*
 * Submission helpers return:
 *   1: operation completed synchronously and this handle suppresses IOCP posts
 *   0: operation is pending or will still post a normal completion
 *  -1: submission failed before the backend owns the request
 */
static int llam_windows_submit_rw(llam_node_t *node, llam_io_req_t *req, llam_windows_io_op_t *op, bool write_op) {
    SOCKET socket_fd;
    DWORD transferred = 0;
    DWORD flags = (DWORD)(write_op ? 0 : req->recv_flags);
    int rc;

    (void)node;
    if (op == NULL || op->association == NULL ||
        !op->association->is_socket ||
        req->count > (size_t)ULONG_MAX) {
        errno = EINVAL;
        return -1;
    }
    socket_fd = (SOCKET)op->association->authority;
    op->wsabuf.buf = (CHAR *)req->buf;
    op->wsabuf.len = (ULONG)req->count;
    if (write_op) {
        rc = WSASend(socket_fd, &op->wsabuf, 1, &transferred, 0, &op->overlapped, NULL);
    } else {
        rc = WSARecv(socket_fd, &op->wsabuf, 1, &transferred, &flags, &op->overlapped, NULL);
    }
    if (rc == 0) {
        op->immediate_bytes = transferred;
        return llam_windows_op_skips_completion_on_success(op) ? 1 : 0;
    }
    {
        int err = WSAGetLastError();

        if (err == WSA_IO_PENDING) {
            return 0;
        }
        errno = llam_windows_wsa_error_to_errno(err);
        return -1;
    }
}

static int llam_windows_submit_handle_rw(llam_node_t *node, llam_io_req_t *req, llam_windows_io_op_t *op, bool write_op) {
    HANDLE handle;
    DWORD transferred = 0;
    BOOL ok;

    (void)node;
    if (op == NULL || op->association == NULL ||
        op->association->is_socket ||
        req->count > (size_t)ULONG_MAX ||
        LLAM_HANDLE_IS_INVALID(req->handle)) {
        errno = EINVAL;
        return -1;
    }
    handle = (HANDLE)op->association->authority;
    if (req->kind == LLAM_IO_KIND_HANDLE_PREAD || req->kind == LLAM_IO_KIND_HANDLE_PWRITE) {
        llam_windows_set_overlapped_offset(&op->overlapped, req->offset);
    }
    if (write_op) {
        ok = WriteFile(handle, req->buf, (DWORD)req->count, &transferred, &op->overlapped);
    } else {
        ok = ReadFile(handle, req->buf, (DWORD)req->count, &transferred, &op->overlapped);
    }
    if (ok) {
        op->immediate_bytes = transferred;
        return llam_windows_op_skips_completion_on_success(op) ? 1 : 0;
    }
    {
        DWORD err = GetLastError();

        if (err == ERROR_IO_PENDING) {
            return 0;
        }
        errno = llam_windows_system_error_to_errno(err);
        return -1;
    }
}

static int llam_windows_submit_accept(llam_node_t *node, llam_io_req_t *req, llam_windows_io_op_t *op) {
    LPFN_ACCEPTEX acceptex = NULL;
    SOCKET listener;
    DWORD bytes = 0;
    unsigned accept_prepost;

    if (op == NULL || op->association == NULL ||
        !op->association->is_socket) {
        errno = EINVAL;
        return -1;
    }
    listener = (SOCKET)op->association->authority;
    if (llam_windows_load_acceptex(node, listener, &acceptex) != 0) {
        return -1;
    }
    accept_prepost = node != NULL ? node->windows_accept_prepost : 0U;
    if (accept_prepost > 0U) {
        llam_windows_accept_socket_pool_warm(node, listener, accept_prepost);
    }
    op->accept_socket = llam_windows_accept_socket_acquire(node, listener);
    if (op->accept_socket == INVALID_SOCKET) {
        errno = llam_windows_wsa_error_to_errno(WSAGetLastError());
        return -1;
    }
    req->fd_result = LLAM_INVALID_FD;
    if (!acceptex(listener,
                  op->accept_socket,
                  op->accept_buffer,
                  0,
                  LLAM_WINDOWS_ACCEPT_ADDR_BYTES,
                  LLAM_WINDOWS_ACCEPT_ADDR_BYTES,
                  &bytes,
                  &op->overlapped)) {
        int err = WSAGetLastError();

        if (err == WSA_IO_PENDING) {
            return 0;
        }
        errno = llam_windows_wsa_error_to_errno(err);
        return -1;
    }
    op->immediate_bytes = bytes;
    return llam_windows_op_skips_completion_on_success(op) ? 1 : 0;
}

static int llam_windows_submit_connect(llam_node_t *node, llam_io_req_t *req, llam_windows_io_op_t *op) {
    LPFN_CONNECTEX connectex = NULL;
    SOCKET socket_fd;
    DWORD bytes = 0;

    if (op == NULL || op->association == NULL ||
        !op->association->is_socket) {
        errno = EINVAL;
        return -1;
    }
    socket_fd = (SOCKET)op->association->authority;
    if (llam_windows_load_connectex(node, socket_fd, &connectex) != 0) {
        return -1;
    }
    if (llam_windows_bind_connect_socket(socket_fd, req->addr) != 0) {
        return -1;
    }
    if (!connectex(socket_fd, req->addr, (int)req->addr_len, NULL, 0, &bytes, &op->overlapped)) {
        int err = WSAGetLastError();

        if (err == WSA_IO_PENDING) {
            return 0;
        }
        errno = llam_windows_wsa_error_to_errno(err);
        return -1;
    }
    op->immediate_bytes = bytes;
    return llam_windows_op_skips_completion_on_success(op) ? 1 : 0;
}

static int llam_windows_submit_poll(llam_node_t *node, llam_io_req_t *req, llam_windows_io_op_t *op) {
    SOCKET socket_fd;
    DWORD transferred = 0;
    int socket_type = 0;
    int rc;

    (void)node;
    if (op == NULL || op->association == NULL ||
        !op->association->is_socket) {
        errno = EINVAL;
        return -1;
    }
    socket_fd = (SOCKET)op->association->authority;
    if (!llam_windows_iocp_poll_supported(
            (llam_fd_t)socket_fd, req->poll_events) ||
        !llam_windows_socket_info(
            (llam_fd_t)socket_fd, NULL, &socket_type)) {
        errno = EOPNOTSUPP;
        return -1;
    }
    op->poll_events = req->poll_events;
    op->wsabuf.buf = &op->poll_byte;
    if ((req->poll_events & POLLOUT) != 0) {
        if (socket_type != SOCK_STREAM) {
            errno = EOPNOTSUPP;
            return -1;
        }
        op->poll_backend = LLAM_WINDOWS_POLL_BACKEND_SEND;
        op->wsabuf.len = 0U;
        rc = WSASend(socket_fd, &op->wsabuf, 1, &transferred, 0, &op->overlapped, NULL);
    } else if (socket_type == SOCK_DGRAM) {
        op->poll_backend = LLAM_WINDOWS_POLL_BACKEND_RECVFROM_PEEK;
        op->poll_flags = MSG_PEEK;
        op->poll_from_len = (int)sizeof(op->poll_from);
        op->wsabuf.len = 1U;
        rc = WSARecvFrom(socket_fd,
                         &op->wsabuf,
                         1,
                         &transferred,
                         &op->poll_flags,
                         (struct sockaddr *)&op->poll_from,
                         &op->poll_from_len,
                         &op->overlapped,
                         NULL);
    } else {
        op->poll_backend = LLAM_WINDOWS_POLL_BACKEND_RECV;
        op->poll_flags = MSG_PEEK;
        op->wsabuf.len = 1U;
        rc = WSARecv(socket_fd, &op->wsabuf, 1, &transferred, &op->poll_flags, &op->overlapped, NULL);
    }
    if (rc == 0) {
        op->immediate_bytes = transferred;
        return llam_windows_op_skips_completion_on_success(op) ? 1 : 0;
    }
    {
        int err = WSAGetLastError();

        if (err == WSA_IO_PENDING) {
            return 0;
        }
        errno = llam_windows_wsa_error_to_errno(err);
        return -1;
    }
}

static void llam_windows_submit_req(llam_node_t *node, llam_io_req_t *req) {
    llam_windows_fd_assoc_t *association;
    llam_windows_io_op_t *op;
    int rc = -1;
    int saved_errno;

    if (llam_io_req_abort_requested(req)) {
        /*
         * The worker owns the request but no OVERLAPPED operation has been
         * submitted yet, so cancellation can complete synchronously without
         * calling CancelIoEx or waiting for an IOCP packet.
         */
        llam_windows_complete_req(node, req, -ECANCELED, true);
        return;
    }

    /*
     * HANDLE/SOCKET values can be recycled immediately after close.  Keep the
     * process-wide lifecycle gate from lazy IOCP association through the
     * actual OVERLAPPED system call, so public close cannot invalidate and
     * recycle the numeric value in between those two steps.  Association uses
     * the same recursive gate internally and keeps the lock order lifecycle ->
     * windows_assoc_lock.
     */
    llam_fd_watch_lifecycle_lock();

    /*
     * Associate the descriptor/handle lazily on first use.  This keeps public
     * direct paths cheap while still allowing backend submissions to complete on
     * the node's IOCP once the request leaves the caller thread.
     */
    if (llam_windows_req_is_handle_rw(req)) {
        if (llam_windows_associate_handle(node, req->handle) != 0) {
            int associate_errno = errno;

            llam_fd_watch_lifecycle_unlock();
            errno = associate_errno;
            llam_windows_complete_submit_error(node, req, errno);
            return;
        }
    } else if (llam_windows_associate_fd(node, req->fd) != 0) {
        int associate_errno = errno;

        llam_fd_watch_lifecycle_unlock();
        errno = associate_errno;
        llam_windows_complete_submit_error(node, req, errno);
        return;
    }

    association = llam_windows_fd_assoc_pin(
        node,
        llam_windows_req_is_handle_rw(req) ? (uintptr_t)req->handle : (uintptr_t)req->fd);
    if (association == NULL) {
        int association_errno = errno;

        llam_fd_watch_lifecycle_unlock();
        llam_windows_complete_submit_error(node, req, association_errno);
        return;
    }
    op = llam_windows_io_op_create(node, req);
    if (op == NULL) {
        int allocation_errno = errno;

        llam_windows_fd_assoc_unpin(node, association);
        llam_fd_watch_lifecycle_unlock();
        errno = allocation_errno;
        llam_windows_complete_submit_error(node, req, errno);
        return;
    }
    op->association = association;
    op->association_generation = association->generation;

    switch (req->kind) {
    case LLAM_IO_KIND_READ:
        rc = llam_windows_submit_rw(node, req, op, false);
        break;
    case LLAM_IO_KIND_WRITE:
        rc = llam_windows_submit_rw(node, req, op, true);
        break;
    case LLAM_IO_KIND_HANDLE_READ:
        rc = llam_windows_submit_handle_rw(node, req, op, false);
        break;
    case LLAM_IO_KIND_HANDLE_WRITE:
        rc = llam_windows_submit_handle_rw(node, req, op, true);
        break;
    case LLAM_IO_KIND_HANDLE_PREAD:
        rc = llam_windows_submit_handle_rw(node, req, op, false);
        break;
    case LLAM_IO_KIND_HANDLE_PWRITE:
        rc = llam_windows_submit_handle_rw(node, req, op, true);
        break;
    case LLAM_IO_KIND_ACCEPT:
        rc = llam_windows_submit_accept(node, req, op);
        break;
    case LLAM_IO_KIND_CONNECT:
        rc = llam_windows_submit_connect(node, req, op);
        break;
    case LLAM_IO_KIND_POLL:
        rc = llam_windows_submit_poll(node, req, op);
        break;
    default:
        errno = EINVAL;
        rc = -1;
        break;
    }

    if (rc >= 0) {
        llam_fd_watch_lifecycle_unlock();
        atomic_fetch_add_explicit(&node->submit_calls, 1U, memory_order_relaxed);
        atomic_fetch_add_explicit(&node->submit_syscalls, 1U, memory_order_relaxed);
        if (rc > 0) {
            llam_windows_complete_immediate_op(op, op->immediate_bytes);
        }
        return;
    }

    saved_errno = errno != 0 ? errno : EIO;
    req->platform_data = NULL;
    llam_windows_io_op_free(op);
    llam_fd_watch_lifecycle_unlock();
    llam_windows_complete_submit_error(node, req, saved_errno);
}

void llam_windows_process_submissions(llam_node_t *node) {
    llam_io_req_t *reqs;
    unsigned submitted = 0U;

    if (node == NULL) {
        return;
    }

    reqs = llam_take_node_submissions(node);

    while (reqs != NULL) {
        llam_io_req_t *next = reqs->next;

        reqs->next = NULL;
        llam_windows_submit_req(node, reqs);
        reqs = next;
        submitted += 1U;
    }
    if (submitted > 0U) {
        atomic_fetch_add_explicit(&node->submit_batches, 1U, memory_order_relaxed);
        atomic_fetch_add_explicit(&node->submit_entries, submitted, memory_order_relaxed);
        llam_atomic_update_peak(&node->max_submit_batch, submitted);
    }
}
