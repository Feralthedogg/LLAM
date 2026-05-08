/**
 * @file src/io/windows/runtime_io_watch_windows_pool.c
 * @brief Windows IOCP accept-socket and overlapped-operation pools.
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

SOCKET llam_windows_accept_socket_acquire(llam_node_t *node, SOCKET listener) {
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

void llam_windows_accept_socket_pool_warm(llam_node_t *node, SOCKET listener, unsigned target_free) {
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

void llam_windows_accept_socket_pool_destroy(llam_node_t *node) {
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

llam_windows_io_op_t *llam_windows_io_op_create(llam_node_t *node, llam_io_req_t *req) {
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

void llam_windows_io_op_free(llam_windows_io_op_t *op) {
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

void llam_windows_io_op_pool_destroy(llam_node_t *node) {
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
