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

#include "runtime_io_watch_windows_internal.h"

static llam_io_control_op_t *llam_take_node_controls(llam_node_t *node) {
    llam_io_control_op_t *head;

    pthread_mutex_lock(&node->watch_lock);
    head = node->control_head;
    node->control_head = NULL;
    node->control_tail = NULL;
    pthread_mutex_unlock(&node->watch_lock);
    return head;
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
