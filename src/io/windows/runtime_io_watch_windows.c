/**
 * @file src/io/windows/runtime_io_watch_windows.c
 * @brief Windows IOCP worker loop and backend cleanup.
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
 * Control packets and unsupported watch stubs live in dedicated sibling files
 * so this file stays focused on worker lifetime and node cleanup.
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

void *llam_io_worker_main(void *arg) {
    llam_node_t *node = arg;
    llam_runtime_t *rt = node->runtime;

    llam_tune_io_worker_thread(node);

    for (;;) {
        unsigned pending;

        llam_windows_process_controls(node);
        llam_windows_process_submissions(node);
        pending = atomic_load_explicit(&node->pending_ops, memory_order_acquire);
        if (atomic_load_explicit(&rt->shutdown_requested, memory_order_acquire) &&
            pending == 0U) {
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
    if (node->windows_assoc_lock_initialized) {
        pthread_mutex_lock(&node->windows_assoc_lock);
    }
    assoc = node->windows_fd_assoc_head;
    while (assoc != NULL) {
        llam_windows_fd_assoc_t *next = assoc->next;

        free(assoc);
        assoc = next;
    }
    node->windows_fd_assoc_head = NULL;
    if (node->windows_assoc_lock_initialized) {
        pthread_mutex_unlock(&node->windows_assoc_lock);
    }
    llam_windows_accept_socket_pool_destroy(node);
    llam_windows_io_op_pool_destroy(node);
    node->windows_acceptex = NULL;
    node->windows_connectex = NULL;
    if (node->windows_iocp_handle != NULL) {
        llam_windows_iocp_close(node->windows_iocp_handle);
        node->windows_iocp_handle = NULL;
    }
}
