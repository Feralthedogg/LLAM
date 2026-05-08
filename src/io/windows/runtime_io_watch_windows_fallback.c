/**
 * @file src/io/windows/runtime_io_watch_windows_fallback.c
 * @brief Unsupported Windows watch fallback stubs.
 *
 * @details
 * The Windows IOCP backend owns one-shot request completions. Multishot watch
 * primitives are intentionally unavailable here until a native readiness-watch
 * design is added; the public I/O API routes unsupported masks through the
 * direct/blocking fallback path before reaching these stubs.
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
