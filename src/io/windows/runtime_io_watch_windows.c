/**
 * @file src/io/windows/runtime_io_watch_windows.c
 * @brief Windows IOCP request backend for one-shot socket operations.
 *
 * @details
 * Windows does not have a kqueue/io_uring-style poll primitive, so this backend
 * wires the completion-native socket operations directly:
 *  - WSARecv for read/recv requests;
 *  - WSASend for write requests;
 *  - AcceptEx for accept requests;
 *  - ConnectEx for connect requests.
 *
 * One-shot readiness polls are mapped only where the Windows socket operation
 * has poll-compatible semantics:
 *  - TCP POLLOUT uses zero-byte WSASend;
 *  - UDP POLLIN uses WSARecvFrom(MSG_PEEK) so the datagram is not consumed.
 *
 * TCP POLLIN readiness stays on the public poll fallback path because repeated
 * overlapped stream-readiness probes are not stable enough across Windows 10/11
 * loopback workloads.
 * Unsupported masks and multi-direction polls continue through fallback paths.
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

static void llam_shard_note_inflight_io_waiter(unsigned owner_shard, int delta) {
    llam_runtime_t *rt = &g_llam_runtime;

    if (delta == 0 || owner_shard >= rt->active_shards) {
        return;
    }
    if (delta > 0) {
        atomic_fetch_add_explicit(&rt->shards[owner_shard].inflight_io_waiters, (unsigned)delta, memory_order_acq_rel);
    } else {
        atomic_fetch_sub_explicit(&rt->shards[owner_shard].inflight_io_waiters, (unsigned)(-delta), memory_order_acq_rel);
    }
}

bool llam_io_req_transfer_inflight_owner(llam_io_req_t *req, unsigned from_shard, unsigned to_shard) {
    unsigned expected;

    if (req == NULL || from_shard == to_shard || from_shard >= g_llam_runtime.active_shards ||
        to_shard >= g_llam_runtime.active_shards) {
        return false;
    }

    expected = from_shard;
    if (!atomic_compare_exchange_strong_explicit(&req->inflight_owner_shard,
                                                 &expected,
                                                 to_shard,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        return false;
    }

    llam_shard_note_inflight_io_waiter(from_shard, -1);
    llam_shard_note_inflight_io_waiter(to_shard, 1);
    return true;
}

void llam_queue_node_submit_locked(llam_node_t *node, llam_io_req_t *req) {
    if (node == NULL || req == NULL) {
        return;
    }
    req->next = NULL;
    if (node->submit_tail != NULL) {
        node->submit_tail->next = req;
    } else {
        node->submit_head = req;
    }
    node->submit_tail = req;
}

bool llam_remove_node_submit_locked(llam_node_t *node, llam_io_req_t *req) {
    llam_io_req_t *prev = NULL;
    llam_io_req_t *cur;

    if (node == NULL || req == NULL) {
        return false;
    }

    cur = node->submit_head;
    while (cur != NULL) {
        if (cur == req) {
            if (prev != NULL) {
                prev->next = cur->next;
            } else {
                node->submit_head = cur->next;
            }
            if (node->submit_tail == cur) {
                node->submit_tail = prev;
            }
            cur->next = NULL;
            return true;
        }
        prev = cur;
        cur = cur->next;
    }
    return false;
}

static llam_io_req_t *llam_take_node_submissions(llam_node_t *node) {
    llam_io_req_t *head;
    llam_io_req_t *cursor;

    pthread_mutex_lock(&node->submit_lock);
    head = node->submit_head;
    node->submit_head = NULL;
    node->submit_tail = NULL;
    cursor = head;
    while (cursor != NULL) {
        atomic_store_explicit(&cursor->inflight_owner_shard, cursor->owner_shard, memory_order_release);
        atomic_store(&cursor->wait_mode, LLAM_IO_WAIT_MODE_INFLIGHT);
        llam_shard_note_inflight_io_waiter(cursor->owner_shard, 1);
        cursor = cursor->next;
    }
    pthread_mutex_unlock(&node->submit_lock);
    return head;
}

static llam_io_control_op_t *llam_take_node_controls(llam_node_t *node) {
    llam_io_control_op_t *head;

    pthread_mutex_lock(&node->watch_lock);
    head = node->control_head;
    node->control_head = NULL;
    node->control_tail = NULL;
    pthread_mutex_unlock(&node->watch_lock);
    return head;
}

int llam_node_queue_control_locked(llam_node_t *node, llam_io_control_kind_t kind, void *target) {
    llam_io_control_op_t *op;

    if (node == NULL) {
        errno = EINVAL;
        return -1;
    }
    op = calloc(1, sizeof(*op));
    if (op == NULL) {
        errno = ENOMEM;
        return -1;
    }
    op->kind = kind;
    op->target = target;
    if (node->control_tail != NULL) {
        node->control_tail->next = op;
    } else {
        node->control_head = op;
    }
    node->control_tail = op;
    return 0;
}

int llam_node_queue_control(llam_node_t *node, llam_io_control_kind_t kind, void *target) {
    int rc;

    if (node == NULL) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&node->watch_lock);
    rc = llam_node_queue_control_locked(node, kind, target);
    pthread_mutex_unlock(&node->watch_lock);
    if (rc == 0) {
        llam_kick_node(node);
    }
    return rc;
}

bool llam_drop_node_control_locked(llam_node_t *node, llam_io_control_kind_t kind, const void *target) {
    llam_io_control_op_t *prev = NULL;
    llam_io_control_op_t *cur;

    if (node == NULL) {
        return false;
    }
    cur = node->control_head;
    while (cur != NULL) {
        if (cur->kind == kind && cur->target == target) {
            if (prev != NULL) {
                prev->next = cur->next;
            } else {
                node->control_head = cur->next;
            }
            if (node->control_tail == cur) {
                node->control_tail = prev;
            }
            free(cur);
            return true;
        }
        prev = cur;
        cur = cur->next;
    }
    return false;
}

static int llam_windows_associate_fd(llam_node_t *node, llam_fd_t fd) {
    llam_windows_fd_assoc_t *assoc;
    HANDLE handle;
    DWORD error_code;

    if (node == NULL || node->windows_iocp_handle == NULL || LLAM_FD_IS_INVALID(fd)) {
        errno = EINVAL;
        return -1;
    }
    for (assoc = node->windows_fd_assoc_head; assoc != NULL; assoc = assoc->next) {
        if (assoc->fd == fd) {
            return 0;
        }
    }

    handle = CreateIoCompletionPort((HANDLE)(uintptr_t)fd, (HANDLE)node->windows_iocp_handle, 0, 0);
    if (handle == NULL) {
        error_code = GetLastError();
        if (error_code != ERROR_INVALID_PARAMETER) {
            errno = error_code == ERROR_NOT_ENOUGH_MEMORY ? ENOMEM : EINVAL;
            return -1;
        }
    }

    assoc = calloc(1, sizeof(*assoc));
    if (assoc == NULL) {
        errno = ENOMEM;
        return -1;
    }
    assoc->fd = fd;
    assoc->next = node->windows_fd_assoc_head;
    node->windows_fd_assoc_head = assoc;
    return 0;
}

static int llam_windows_load_acceptex(llam_node_t *node, SOCKET socket_fd, LPFN_ACCEPTEX *fn_out) {
    GUID guid = WSAID_ACCEPTEX;
    LPFN_ACCEPTEX fn = NULL;
    DWORD bytes = 0;

    if (node->windows_acceptex != NULL) {
        *fn_out = (LPFN_ACCEPTEX)node->windows_acceptex;
        return 0;
    }
    if (WSAIoctl(socket_fd,
                 SIO_GET_EXTENSION_FUNCTION_POINTER,
                 &guid,
                 sizeof(guid),
                 &fn,
                 sizeof(fn),
                 &bytes,
                 NULL,
                 NULL) != 0) {
        errno = llam_windows_wsa_error_to_errno(WSAGetLastError());
        return -1;
    }
    node->windows_acceptex = (void *)fn;
    *fn_out = fn;
    return 0;
}

static int llam_windows_load_connectex(llam_node_t *node, SOCKET socket_fd, LPFN_CONNECTEX *fn_out) {
    GUID guid = WSAID_CONNECTEX;
    LPFN_CONNECTEX fn = NULL;
    DWORD bytes = 0;

    if (node->windows_connectex != NULL) {
        *fn_out = (LPFN_CONNECTEX)node->windows_connectex;
        return 0;
    }
    if (WSAIoctl(socket_fd,
                 SIO_GET_EXTENSION_FUNCTION_POINTER,
                 &guid,
                 sizeof(guid),
                 &fn,
                 sizeof(fn),
                 &bytes,
                 NULL,
                 NULL) != 0) {
        errno = llam_windows_wsa_error_to_errno(WSAGetLastError());
        return -1;
    }
    node->windows_connectex = (void *)fn;
    *fn_out = fn;
    return 0;
}

static SOCKET llam_windows_create_accept_socket(SOCKET listener) {
    struct sockaddr_storage local_addr;
    int local_len = (int)sizeof(local_addr);
    int family = AF_INET;
    int socket_type = SOCK_STREAM;
    int opt_len = (int)sizeof(socket_type);

    memset(&local_addr, 0, sizeof(local_addr));
    if (getsockname(listener, (struct sockaddr *)&local_addr, &local_len) == 0) {
        family = ((struct sockaddr *)&local_addr)->sa_family;
    }
    (void)getsockopt(listener, SOL_SOCKET, SO_TYPE, &socket_type, &opt_len);
    return WSASocket(family, socket_type, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
}

static SOCKET llam_windows_accept_socket_acquire(llam_node_t *node, SOCKET listener) {
    llam_windows_accept_socket_entry_t *prev = NULL;
    llam_windows_accept_socket_entry_t *entry;
    SOCKET socket_fd;

    if (node == NULL) {
        return llam_windows_create_accept_socket(listener);
    }

    entry = node->windows_accept_socket_free;
    while (entry != NULL) {
        if (entry->listener_fd == listener) {
            if (prev != NULL) {
                prev->next = entry->next;
            } else {
                node->windows_accept_socket_free = entry->next;
            }
            if (node->windows_accept_socket_free_count > 0U) {
                node->windows_accept_socket_free_count -= 1U;
            }
            socket_fd = entry->socket_fd;
            free(entry);
            return socket_fd;
        }
        prev = entry;
        entry = entry->next;
    }

    return llam_windows_create_accept_socket(listener);
}

static int llam_windows_accept_socket_pool_push(llam_node_t *node, SOCKET listener, SOCKET socket_fd) {
    llam_windows_accept_socket_entry_t *entry;
    unsigned max_free;

    if (node == NULL || socket_fd == INVALID_SOCKET) {
        errno = EINVAL;
        return -1;
    }
    max_free = node->windows_accept_socket_free_max != 0U ? node->windows_accept_socket_free_max : 1U;
    if (node->windows_accept_socket_free_count >= max_free) {
        (void)closesocket(socket_fd);
        return 0;
    }

    entry = calloc(1, sizeof(*entry));
    if (entry == NULL) {
        (void)closesocket(socket_fd);
        errno = ENOMEM;
        return -1;
    }
    entry->socket_fd = socket_fd;
    entry->listener_fd = listener;
    entry->next = node->windows_accept_socket_free;
    node->windows_accept_socket_free = entry;
    node->windows_accept_socket_free_count += 1U;
    return 0;
}

static void llam_windows_accept_socket_pool_warm(llam_node_t *node, SOCKET listener, unsigned target_free) {
    unsigned created = 0U;

    if (node == NULL || target_free == 0U || node->windows_accept_socket_free_count >= target_free) {
        return;
    }
    while (node->windows_accept_socket_free_count < target_free && created < target_free) {
        SOCKET socket_fd = llam_windows_create_accept_socket(listener);

        if (socket_fd == INVALID_SOCKET) {
            return;
        }
        if (llam_windows_accept_socket_pool_push(node, listener, socket_fd) != 0) {
            return;
        }
        created += 1U;
    }
}

static void llam_windows_accept_socket_pool_destroy(llam_node_t *node) {
    llam_windows_accept_socket_entry_t *entry;

    if (node == NULL) {
        return;
    }
    entry = node->windows_accept_socket_free;
    while (entry != NULL) {
        llam_windows_accept_socket_entry_t *next = entry->next;

        if (entry->socket_fd != INVALID_SOCKET) {
            (void)closesocket(entry->socket_fd);
        }
        free(entry);
        entry = next;
    }
    node->windows_accept_socket_free = NULL;
    node->windows_accept_socket_free_count = 0U;
}

static int llam_windows_bind_connect_socket(SOCKET socket_fd, const struct sockaddr *addr) {
    int rc;

    if (addr == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (addr->sa_family == AF_INET) {
        struct sockaddr_in local_addr;

        memset(&local_addr, 0, sizeof(local_addr));
        local_addr.sin_family = AF_INET;
        rc = bind(socket_fd, (const struct sockaddr *)&local_addr, (int)sizeof(local_addr));
    } else if (addr->sa_family == AF_INET6) {
        struct sockaddr_in6 local_addr;

        memset(&local_addr, 0, sizeof(local_addr));
        local_addr.sin6_family = AF_INET6;
        rc = bind(socket_fd, (const struct sockaddr *)&local_addr, (int)sizeof(local_addr));
    } else {
        errno = EAFNOSUPPORT;
        return -1;
    }

    if (rc == 0) {
        return 0;
    }
    {
        int err = WSAGetLastError();

        if (err == WSAEINVAL || err == WSAEADDRINUSE) {
            return 0;
        }
        errno = llam_windows_wsa_error_to_errno(err);
        return -1;
    }
}

static bool llam_windows_socket_info(llam_fd_t fd, int *family_out, int *socket_type_out) {
    struct sockaddr_storage local_addr;
    int local_len = (int)sizeof(local_addr);
    int socket_type = 0;
    int socket_type_len = (int)sizeof(socket_type);

    if (family_out != NULL) {
        *family_out = AF_UNSPEC;
    }
    if (socket_type_out != NULL) {
        *socket_type_out = 0;
    }
    if (LLAM_FD_IS_INVALID(fd)) {
        return false;
    }
    memset(&local_addr, 0, sizeof(local_addr));
    if (getsockname(fd, (struct sockaddr *)&local_addr, &local_len) != 0) {
        return false;
    }
    if (((struct sockaddr *)&local_addr)->sa_family != AF_INET &&
        ((struct sockaddr *)&local_addr)->sa_family != AF_INET6) {
        return false;
    }
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, (char *)&socket_type, &socket_type_len) != 0) {
        return false;
    }
    if (family_out != NULL) {
        *family_out = ((struct sockaddr *)&local_addr)->sa_family;
    }
    if (socket_type_out != NULL) {
        *socket_type_out = socket_type;
    }
    return true;
}

bool llam_windows_iocp_poll_supported(llam_fd_t fd, short events) {
    int socket_type = 0;
    short unsupported = (short)(events & ~(POLLIN | POLLOUT | POLLHUP | POLLERR));
    bool wants_read = (events & POLLIN) != 0;
    bool wants_write = (events & POLLOUT) != 0;

    if (unsupported != 0 || (!wants_read && !wants_write) || (wants_read && wants_write)) {
        return false;
    }
    if (!llam_windows_socket_info(fd, NULL, &socket_type)) {
        return false;
    }
    if (socket_type == SOCK_STREAM) {
        return wants_write;
    }
    return socket_type == SOCK_DGRAM && wants_read;
}

static llam_windows_io_op_t *llam_windows_io_op_create(llam_node_t *node, llam_io_req_t *req) {
    llam_windows_io_op_t *op = NULL;

    if (node != NULL && node->windows_io_op_free != NULL) {
        op = node->windows_io_op_free;
        node->windows_io_op_free = op->next_free;
        if (node->windows_io_op_free_count > 0U) {
            node->windows_io_op_free_count -= 1U;
        }
        memset(op, 0, sizeof(*op));
    } else {
        op = calloc(1, sizeof(*op));
        if (op == NULL) {
            errno = ENOMEM;
            return NULL;
        }
    }
    op->req = req;
    op->node = node;
    op->accept_socket = INVALID_SOCKET;
    op->magic = LLAM_WINDOWS_IO_OP_MAGIC;
    op->kind = (unsigned)req->kind;
    req->platform_data = op;
    return op;
}

static void llam_windows_io_op_free(llam_windows_io_op_t *op) {
    llam_node_t *node;
    unsigned max_free;

    if (op == NULL) {
        return;
    }
    node = op->node;
    op->magic = 0U;
    if (op->accept_socket != INVALID_SOCKET) {
        (void)closesocket(op->accept_socket);
        op->accept_socket = INVALID_SOCKET;
    }
    if (node != NULL) {
        max_free = node->windows_io_op_free_max != 0U ? node->windows_io_op_free_max : 64U;
        if (node->windows_io_op_free_count < max_free) {
            memset(op, 0, sizeof(*op));
            op->accept_socket = INVALID_SOCKET;
            op->next_free = node->windows_io_op_free;
            node->windows_io_op_free = op;
            node->windows_io_op_free_count += 1U;
            return;
        }
    }
    free(op);
}

static void llam_windows_io_op_pool_destroy(llam_node_t *node) {
    llam_windows_io_op_t *op;

    if (node == NULL) {
        return;
    }
    op = node->windows_io_op_free;
    while (op != NULL) {
        llam_windows_io_op_t *next = op->next_free;

        if (op->accept_socket != INVALID_SOCKET) {
            (void)closesocket(op->accept_socket);
        }
        free(op);
        op = next;
    }
    node->windows_io_op_free = NULL;
    node->windows_io_op_free_count = 0U;
}

static void llam_windows_complete_req(llam_node_t *node, llam_io_req_t *req, int res, bool decrement_pending) {
    llam_io_abort_reason_t abort_reason;
    llam_wait_reason_t wake_reason = LLAM_WAIT_IO;
    unsigned inflight_owner = UINT_MAX;
    unsigned wait_mode;

    if (decrement_pending) {
        atomic_fetch_sub(&node->pending_ops, 1U);
    }
    wait_mode = atomic_load_explicit(&req->wait_mode, memory_order_acquire);
    if (wait_mode == LLAM_IO_WAIT_MODE_INFLIGHT) {
        inflight_owner = atomic_exchange_explicit(&req->inflight_owner_shard, UINT_MAX, memory_order_acq_rel);
        if (inflight_owner < node->runtime->active_shards) {
            llam_shard_note_inflight_io_waiter(inflight_owner, -1);
        }
    } else {
        atomic_store_explicit(&req->inflight_owner_shard, UINT_MAX, memory_order_release);
    }
    abort_reason = (llam_io_abort_reason_t)atomic_exchange(&req->abort_reason, LLAM_IO_ABORT_NONE);
    atomic_store(&req->wait_mode, LLAM_IO_WAIT_MODE_NONE);
    atomic_store(&req->cancel_queued, 0U);
    req->platform_data = NULL;
    req->poll_watch = NULL;
    req->accept_watch = NULL;
    req->recv_watch = NULL;

    if (res >= 0) {
        if (req->kind == LLAM_IO_KIND_POLL) {
            req->result = res != 0 ? 1 : 0;
        } else {
            req->result = req->kind == LLAM_IO_KIND_ACCEPT && !LLAM_FD_IS_INVALID(req->fd_result) ? (ssize_t)req->fd_result : res;
        }
        req->error_code = 0;
        req->poll_revents = req->kind == LLAM_IO_KIND_POLL ? (short)res : 0;
        if (req->owned_buffer != NULL && req->kind == LLAM_IO_KIND_READ) {
            req->owned_buffer->size = (size_t)res;
        }
    } else if (res == -ECANCELED && abort_reason != LLAM_IO_ABORT_NONE) {
        llam_io_set_abort_result(req, abort_reason);
        wake_reason = llam_io_abort_wait_reason(abort_reason);
    } else {
        req->result = -1;
        req->error_code = -res;
        req->poll_revents = 0;
        if (req->kind == LLAM_IO_KIND_ACCEPT) {
            req->fd_result = LLAM_INVALID_FD;
        }
    }

    if (req->owner_shard < node->runtime->active_shards) {
        llam_shard_t *shard = &node->runtime->shards[req->owner_shard];
        uint64_t now_ns = llam_now_ns();

        pthread_mutex_lock(&shard->lock);
        if (wake_reason == LLAM_WAIT_CANCEL) {
            shard->metrics.cancel_wakes += 1U;
        } else if (wake_reason == LLAM_WAIT_TIMEOUT) {
            shard->metrics.timeout_wakes += 1U;
        }
        if (now_ns >= req->submit_ts_ns) {
            shard->metrics.io_completion_latency_ns += now_ns - req->submit_ts_ns;
            shard->metrics.io_completion_samples += 1U;
        }
        pthread_mutex_unlock(&shard->lock);
    }

    llam_reinject_task_on_shard(node->runtime,
                              req->task,
                              req->owner_shard,
                              true,
                              LLAM_TRACE_IO_COMPLETE,
                              wake_reason);
}

static void llam_windows_complete_submit_error(llam_node_t *node, llam_io_req_t *req, int err) {
    if (err <= 0) {
        err = EIO;
    }
    llam_windows_complete_req(node, req, -err, true);
}

static int llam_windows_submit_rw(llam_node_t *node, llam_io_req_t *req, llam_windows_io_op_t *op, bool write_op) {
    DWORD transferred = 0;
    DWORD flags = (DWORD)(write_op ? 0 : req->recv_flags);
    int rc;

    if (req->count > (size_t)ULONG_MAX) {
        errno = EINVAL;
        return -1;
    }
    op->wsabuf.buf = (CHAR *)req->buf;
    op->wsabuf.len = (ULONG)req->count;
    if (write_op) {
        rc = WSASend(req->fd, &op->wsabuf, 1, &transferred, 0, &op->overlapped, NULL);
    } else {
        rc = WSARecv(req->fd, &op->wsabuf, 1, &transferred, &flags, &op->overlapped, NULL);
    }
    if (rc == 0) {
        return 0;
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

static int llam_windows_submit_accept(llam_node_t *node, llam_io_req_t *req, llam_windows_io_op_t *op) {
    LPFN_ACCEPTEX acceptex = NULL;
    DWORD bytes = 0;
    unsigned accept_prepost;

    if (llam_windows_load_acceptex(node, req->fd, &acceptex) != 0) {
        return -1;
    }
    accept_prepost = node != NULL ? node->windows_accept_prepost : 0U;
    if (accept_prepost > 0U) {
        llam_windows_accept_socket_pool_warm(node, req->fd, accept_prepost);
    }
    op->accept_socket = llam_windows_accept_socket_acquire(node, req->fd);
    if (op->accept_socket == INVALID_SOCKET) {
        errno = llam_windows_wsa_error_to_errno(WSAGetLastError());
        return -1;
    }
    req->fd_result = LLAM_INVALID_FD;
    if (!acceptex(req->fd,
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
    return 0;
}

static int llam_windows_submit_connect(llam_node_t *node, llam_io_req_t *req, llam_windows_io_op_t *op) {
    LPFN_CONNECTEX connectex = NULL;
    DWORD bytes = 0;

    if (llam_windows_load_connectex(node, req->fd, &connectex) != 0) {
        return -1;
    }
    if (llam_windows_bind_connect_socket(req->fd, req->addr) != 0) {
        return -1;
    }
    if (!connectex(req->fd, req->addr, (int)req->addr_len, NULL, 0, &bytes, &op->overlapped)) {
        int err = WSAGetLastError();

        if (err == WSA_IO_PENDING) {
            return 0;
        }
        errno = llam_windows_wsa_error_to_errno(err);
        return -1;
    }
    return 0;
}

static int llam_windows_submit_poll(llam_node_t *node, llam_io_req_t *req, llam_windows_io_op_t *op) {
    DWORD transferred = 0;
    int socket_type = 0;
    int rc;

    (void)node;
    if (!llam_windows_iocp_poll_supported(req->fd, req->poll_events) ||
        !llam_windows_socket_info(req->fd, NULL, &socket_type)) {
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
        rc = WSASend(req->fd, &op->wsabuf, 1, &transferred, 0, &op->overlapped, NULL);
    } else if (socket_type == SOCK_DGRAM) {
        op->poll_backend = LLAM_WINDOWS_POLL_BACKEND_RECVFROM_PEEK;
        op->poll_flags = MSG_PEEK;
        op->poll_from_len = (int)sizeof(op->poll_from);
        op->wsabuf.len = 1U;
        rc = WSARecvFrom(req->fd,
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
        rc = WSARecv(req->fd, &op->wsabuf, 1, &transferred, &op->poll_flags, &op->overlapped, NULL);
    }
    if (rc == 0) {
        return 0;
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
    llam_windows_io_op_t *op;
    int rc = -1;
    int saved_errno;

    if (llam_windows_associate_fd(node, req->fd) != 0) {
        llam_windows_complete_submit_error(node, req, errno);
        return;
    }

    op = llam_windows_io_op_create(node, req);
    if (op == NULL) {
        llam_windows_complete_submit_error(node, req, errno);
        return;
    }

    switch (req->kind) {
    case LLAM_IO_KIND_READ:
        rc = llam_windows_submit_rw(node, req, op, false);
        break;
    case LLAM_IO_KIND_WRITE:
        rc = llam_windows_submit_rw(node, req, op, true);
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

    if (rc == 0) {
        node->submit_calls += 1U;
        node->submit_syscalls += 1U;
        return;
    }

    saved_errno = errno != 0 ? errno : EIO;
    req->platform_data = NULL;
    llam_windows_io_op_free(op);
    llam_windows_complete_submit_error(node, req, saved_errno);
}

static void llam_windows_process_submissions(llam_node_t *node) {
    llam_io_req_t *reqs = llam_take_node_submissions(node);
    unsigned submitted = 0U;

    while (reqs != NULL) {
        llam_io_req_t *next = reqs->next;

        reqs->next = NULL;
        llam_windows_submit_req(node, reqs);
        reqs = next;
        submitted += 1U;
    }
    if (submitted > 0U) {
        node->submit_batches += 1U;
        node->submit_entries += submitted;
        if (submitted > node->max_submit_batch) {
            node->max_submit_batch = submitted;
        }
    }
}

static void llam_windows_process_control(llam_node_t *node, llam_io_control_op_t *op) {
    llam_io_req_t *req;
    llam_windows_io_op_t *io_op;

    if (op == NULL) {
        return;
    }
    if (op->kind != LLAM_IO_CONTROL_REQ_CANCEL) {
        free(op);
        return;
    }

    req = op->target;
    if (req != NULL && atomic_load_explicit(&req->wait_mode, memory_order_acquire) == LLAM_IO_WAIT_MODE_INFLIGHT) {
        io_op = req->platform_data;
        if (io_op != NULL && io_op->magic == LLAM_WINDOWS_IO_OP_MAGIC) {
            (void)CancelIoEx((HANDLE)(uintptr_t)req->fd, &io_op->overlapped);
        }
    }
    free(op);
    (void)node;
}

static void llam_windows_process_controls(llam_node_t *node) {
    llam_io_control_op_t *controls = llam_take_node_controls(node);

    while (controls != NULL) {
        llam_io_control_op_t *next = controls->next;

        controls->next = NULL;
        llam_windows_process_control(node, controls);
        controls = next;
    }
}

static void llam_windows_copy_accept_addr(llam_windows_io_op_t *op) {
    llam_io_req_t *req = op->req;
    struct sockaddr *local_addr = NULL;
    struct sockaddr *remote_addr = NULL;
    int local_len = 0;
    int remote_len = 0;

    if (req->addr == NULL || req->addrlen == NULL) {
        return;
    }

    GetAcceptExSockaddrs(op->accept_buffer,
                         0,
                         LLAM_WINDOWS_ACCEPT_ADDR_BYTES,
                         LLAM_WINDOWS_ACCEPT_ADDR_BYTES,
                         &local_addr,
                         &local_len,
                         &remote_addr,
                         &remote_len);
    (void)local_addr;
    (void)local_len;
    if (remote_addr == NULL || remote_len <= 0) {
        *req->addrlen = 0;
        return;
    }
    if (*req->addrlen > 0) {
        size_t copy_len = (size_t)*req->addrlen < (size_t)remote_len ? (size_t)*req->addrlen : (size_t)remote_len;

        memcpy(req->addr, remote_addr, copy_len);
    }
    *req->addrlen = (socklen_t)remote_len;
}

static int llam_windows_finalize_accept(llam_windows_io_op_t *op) {
    llam_io_req_t *req = op->req;

    if (setsockopt(op->accept_socket,
                   SOL_SOCKET,
                   SO_UPDATE_ACCEPT_CONTEXT,
                   (const char *)&req->fd,
                   (int)sizeof(req->fd)) != 0) {
        return llam_windows_wsa_error_to_errno(WSAGetLastError());
    }
    llam_windows_copy_accept_addr(op);
    req->fd_result = op->accept_socket;
    op->accept_socket = INVALID_SOCKET;
    return 0;
}

static int llam_windows_finalize_connect(llam_windows_io_op_t *op) {
    llam_io_req_t *req = op->req;

    if (setsockopt(req->fd, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0) != 0) {
        return llam_windows_wsa_error_to_errno(WSAGetLastError());
    }
    return 0;
}

static void llam_windows_handle_completion(const llam_windows_iocp_completion_t *completion) {
    llam_windows_io_op_t *op = (llam_windows_io_op_t *)completion->overlapped;
    llam_io_req_t *req;
    llam_node_t *node;
    DWORD transferred;
    DWORD flags = 0;
    int result;
    int err = 0;

    if (completion->overlapped == 0U) {
        return;
    }
    if (op == NULL || op->magic != LLAM_WINDOWS_IO_OP_MAGIC || op->req == NULL || op->node == NULL) {
        return;
    }

    req = op->req;
    node = op->node;
    transferred = completion->bytes;
    if (!WSAGetOverlappedResult(req->fd, &op->overlapped, &transferred, FALSE, &flags)) {
        err = llam_windows_wsa_error_to_errno(WSAGetLastError());
    }

    if (req->kind == LLAM_IO_KIND_POLL && err == EMSGSIZE &&
        op->poll_backend == LLAM_WINDOWS_POLL_BACKEND_RECVFROM_PEEK) {
        err = 0;
    }

    if (err == 0 && req->kind == LLAM_IO_KIND_ACCEPT) {
        err = llam_windows_finalize_accept(op);
        result = 0;
    } else if (err == 0 && req->kind == LLAM_IO_KIND_CONNECT) {
        err = llam_windows_finalize_connect(op);
        result = 0;
    } else if (err == 0 && req->kind == LLAM_IO_KIND_POLL) {
        result = op->poll_backend == LLAM_WINDOWS_POLL_BACKEND_SEND ? POLLOUT : POLLIN;
    } else {
        result = (int)transferred;
    }

    if (err != 0) {
        if (req->kind == LLAM_IO_KIND_ACCEPT) {
            req->fd_result = LLAM_INVALID_FD;
        }
        llam_windows_complete_req(node, req, -err, true);
    } else {
        llam_windows_complete_req(node, req, result, true);
    }
    llam_windows_io_op_free(op);
}

static void llam_windows_drain_completions(llam_node_t *node, DWORD timeout_ms) {
    llam_windows_iocp_completion_t entries[LLAM_WINDOWS_IOCP_BATCH_MAX];
    size_t batch = node->windows_completion_batch != 0U ? node->windows_completion_batch : 64U;
    unsigned rounds = 0U;

    if (batch > LLAM_WINDOWS_IOCP_BATCH_MAX) {
        batch = LLAM_WINDOWS_IOCP_BATCH_MAX;
    }
    for (;;) {
        size_t count = 0U;
        size_t i;

        if (llam_windows_iocp_drain(node->windows_iocp_handle, entries, batch, timeout_ms, &count) != 0) {
            if (errno != ETIMEDOUT) {
                llam_record_fatal(node->runtime, errno != 0 ? errno : EIO);
            }
            return;
        }
        node->last_cq_depth = (unsigned)count;
        if ((unsigned)count > node->max_cq_depth) {
            node->max_cq_depth = (unsigned)count;
        }
        for (i = 0U; i < count; ++i) {
            if (entries[i].overlapped == 0U && entries[i].key == LLAM_WINDOWS_IOCP_WAKE_KEY) {
                llam_drain_node_wake(node);
                continue;
            }
            llam_windows_handle_completion(&entries[i]);
        }
        rounds += 1U;
        if (count < batch || rounds >= 4U) {
            return;
        }
        timeout_ms = 0U;
    }
}

void *llam_io_worker_main(void *arg) {
    llam_node_t *node = arg;
    llam_runtime_t *rt = node->runtime;

    llam_tune_io_worker_thread(node);

    for (;;) {
        unsigned pending;

        llam_windows_process_controls(node);
        llam_windows_process_submissions(node);
        pending = atomic_load_explicit(&node->pending_ops, memory_order_acquire);
        if (atomic_load_explicit(&rt->stop_requested, memory_order_acquire) && pending == 0U) {
            break;
        }
        llam_windows_drain_completions(node, INFINITE);
    }

    return NULL;
}

void llam_windows_iocp_cleanup_node(llam_node_t *node) {
    llam_windows_fd_assoc_t *assoc;

    if (node == NULL) {
        return;
    }
    assoc = node->windows_fd_assoc_head;
    while (assoc != NULL) {
        llam_windows_fd_assoc_t *next = assoc->next;

        free(assoc);
        assoc = next;
    }
    node->windows_fd_assoc_head = NULL;
    llam_windows_accept_socket_pool_destroy(node);
    llam_windows_io_op_pool_destroy(node);
    node->windows_acceptex = NULL;
    node->windows_connectex = NULL;
    if (node->windows_iocp_handle != NULL) {
        llam_windows_iocp_close(node->windows_iocp_handle);
        node->windows_iocp_handle = NULL;
    }
}

void llam_accept_watch_enqueue_waiter(llam_accept_watch_t *watch, llam_io_req_t *req) {
    (void)watch;
    (void)req;
}

int llam_accept_watch_pop_ready(llam_accept_watch_t *watch) {
    (void)watch;
    errno = EAGAIN;
    return -1;
}

bool llam_accept_watch_remove_waiter(llam_accept_watch_t *watch, llam_io_req_t *req) {
    (void)watch;
    (void)req;
    return false;
}

void llam_poll_watch_enqueue_waiter(llam_poll_watch_t *watch, llam_io_req_t *req) {
    (void)watch;
    (void)req;
}

bool llam_poll_watch_remove_waiter(llam_poll_watch_t *watch, llam_io_req_t *req) {
    (void)watch;
    (void)req;
    return false;
}

void llam_recv_watch_enqueue_waiter(llam_recv_watch_t *watch, llam_io_req_t *req) {
    (void)watch;
    (void)req;
}

bool llam_recv_watch_remove_waiter(llam_recv_watch_t *watch, llam_io_req_t *req) {
    (void)watch;
    (void)req;
    return false;
}

bool llam_recv_watch_pop_ready(llam_recv_watch_t *watch,
                             size_t *size_out,
                             unsigned short *bid_out,
                             bool *has_buffer_out,
                             unsigned *node_index_out,
                             unsigned char **copy_data_out,
                             size_t *copy_capacity_out) {
    (void)watch;
    if (size_out != NULL) {
        *size_out = 0U;
    }
    if (bid_out != NULL) {
        *bid_out = 0U;
    }
    if (has_buffer_out != NULL) {
        *has_buffer_out = false;
    }
    if (node_index_out != NULL) {
        *node_index_out = 0U;
    }
    if (copy_data_out != NULL) {
        *copy_data_out = NULL;
    }
    if (copy_capacity_out != NULL) {
        *copy_capacity_out = 0U;
    }
    errno = EAGAIN;
    return false;
}

llam_accept_watch_t *llam_get_or_create_accept_watch_locked(llam_node_t *node, llam_fd_t fd) {
    (void)node;
    (void)fd;
    errno = ENOSYS;
    return NULL;
}

llam_poll_watch_t *llam_get_or_create_poll_watch_locked(llam_node_t *node, llam_fd_t fd, short events) {
    (void)node;
    (void)fd;
    (void)events;
    errno = ENOSYS;
    return NULL;
}

llam_recv_watch_t *llam_get_or_create_recv_watch_locked(llam_node_t *node, llam_fd_t fd) {
    (void)node;
    (void)fd;
    errno = ENOSYS;
    return NULL;
}

void llam_maybe_destroy_recv_watch_locked(llam_node_t *node, llam_recv_watch_t *watch) {
    (void)node;
    (void)watch;
}

bool llam_io_rehome_idle_watch_state(llam_node_t *source, llam_node_t *target) {
    (void)source;
    (void)target;
    return false;
}

bool llam_io_rehome_marked_watch_state(llam_node_t *source, llam_node_t *target) {
    (void)source;
    (void)target;
    return false;
}

unsigned llam_multishot_owner_node_index(llam_runtime_t *rt, unsigned fallback_node_index, llam_fd_t fd) {
    (void)rt;
    (void)fd;
    return fallback_node_index;
}
