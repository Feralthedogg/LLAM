/**
 * @file src/io/linux/watch/linux_state.c
 * @brief Linux/io_uring watch waiter and buffered-readiness state helpers.
 *
 * @details
 * These helpers mutate watch-local queues while the owning node watch lock is
 * held. Shared queue mechanics live in watch.c; this file keeps the
 * Linux-specific accept/receive ready-list ownership rules close to the backend
 * code that consumes them.
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

#include "io/linux/runtime_io_watch_linux_internal.h"

/**
 * @brief Pop one accept waiter from a watch.
 *
 * @param watch Accept watch.
 * @return Waiter request, or NULL.
 */
llam_io_req_t *llam_accept_watch_pop_waiter(llam_accept_watch_t *watch) {
    llam_io_req_t *req = watch->wait_head;

    if (req == NULL) {
        return NULL;
    }
    watch->wait_head = req->next;
    if (watch->wait_head == NULL) {
        watch->wait_tail = NULL;
    }
    req->next = NULL;
    return req;
}

/**
 * @brief Pop one receive waiter from a watch.
 *
 * @param watch Receive watch.
 * @return Waiter request, or NULL.
 */
llam_io_req_t *llam_recv_watch_pop_waiter(llam_recv_watch_t *watch) {
    llam_io_req_t *req = watch->wait_head;

    if (req == NULL) {
        return NULL;
    }
    watch->wait_head = req->next;
    if (watch->wait_head == NULL) {
        watch->wait_tail = NULL;
    }
    req->next = NULL;
    return req;
}

/**
 * @brief Queue an accepted fd as buffered readiness.
 *
 * @param watch Accept watch.
 * @param fd    Accepted fd whose ownership transfers to the watch.
 * @return true on success, false if allocation failed.
 */
bool llam_accept_watch_push_ready_owned(llam_accept_watch_t *watch, int fd) {
    llam_accept_ready_t *ready = calloc(1, sizeof(*ready));

    if (ready == NULL) {
        return false;
    }

    ready->fd = fd;
    if (watch->ready_tail != NULL) {
        watch->ready_tail->next = ready;
    } else {
        watch->ready_head = ready;
    }
    watch->ready_tail = ready;
    watch->ready_depth += 1U;
    return true;
}

/**
 * @brief Queue an accepted fd or close it if buffering fails.
 *
 * @param watch Accept watch.
 * @param fd    Accepted fd.
 */
void llam_accept_watch_push_ready(llam_accept_watch_t *watch, int fd) {
    if (!llam_accept_watch_push_ready_owned(watch, fd)) {
        close(fd);
    }
}

/**
 * @brief Pop one buffered accepted fd from a watch.
 *
 * @param watch Accept watch.
 * @return Accepted fd, or -1 when no ready fd exists.
 */
int llam_accept_watch_pop_ready(llam_accept_watch_t *watch) {
    llam_accept_ready_t *ready = watch->ready_head;
    int fd;

    if (ready == NULL) {
        return -1;
    }

    watch->ready_head = ready->next;
    if (watch->ready_head == NULL) {
        watch->ready_tail = NULL;
    }
    watch->ready_depth -= 1U;
    fd = ready->fd;
    free(ready);
    return fd;
}

/**
 * @brief Release a buffered receive-ready entry.
 *
 * @param rt            Runtime used to resolve provided-buffer owner node.
 * @param fallback_node Node used when the ready entry has no owner index.
 * @param ready         Ready entry to release.
 */
void llam_release_recv_ready(llam_runtime_t *rt, llam_node_t *fallback_node, llam_recv_ready_t *ready) {
    llam_node_t *owner = fallback_node;

    if (ready == NULL) {
        return;
    }
    if (ready->has_buffer && rt != NULL && ready->node_index < rt->active_nodes) {
        owner = &rt->nodes[ready->node_index];
    }
    if (ready->has_buffer && owner != NULL) {
        // Provided buffers must return to the node whose ring owns the buffer
        // group, which can differ after live watch migration.
        (void)llam_node_recycle_recv_buffer(owner, ready->bid);
    }
    free(ready->copy_data);
    free(ready);
}

/**
 * @brief Buffer receive readiness for a later waiter.
 *
 * @param watch         Receive watch.
 * @param size          Received byte count.
 * @param bid           Provided-buffer id, if any.
 * @param has_buffer    Whether @p bid is valid.
 * @param node_index    Node that owns @p bid.
 * @param copy_data     Optional copied payload data.
 * @param copy_capacity Capacity of @p copy_data.
 * @return true on success, false on allocation failure.
 */
bool llam_recv_watch_push_ready(llam_recv_watch_t *watch,
                                     size_t size,
                                     unsigned short bid,
                                     bool has_buffer,
                                     unsigned node_index,
                                     unsigned char *copy_data,
                                     size_t copy_capacity) {
    llam_recv_ready_t *ready = calloc(1, sizeof(*ready));

    if (ready == NULL) {
        // copy_data ownership was passed to this function; release on failure.
        free(copy_data);
        return false;
    }

    ready->size = size;
    ready->bid = bid;
    ready->node_index = has_buffer ? node_index : UINT_MAX;
    ready->copy_capacity = copy_capacity;
    ready->copy_data = copy_data;
    ready->has_buffer = has_buffer;
    if (watch->ready_tail != NULL) {
        watch->ready_tail->next = ready;
    } else {
        watch->ready_head = ready;
    }
    watch->ready_tail = ready;
    watch->ready_depth += 1U;
    return true;
}

/**
 * @brief Pop buffered receive readiness from a watch.
 *
 * Output pointers are optional. If @p copy_data_out is provided, ownership of
 * copied payload memory transfers to the caller.
 */
bool llam_recv_watch_pop_ready(llam_recv_watch_t *watch,
                             size_t *size_out,
                             unsigned short *bid_out,
                             bool *has_buffer_out,
                             unsigned *node_index_out,
                             unsigned char **copy_data_out,
                             size_t *copy_capacity_out) {
    return llam_recv_watch_pop_ready_shared(watch,
                                           size_out,
                                           bid_out,
                                           has_buffer_out,
                                           node_index_out,
                                           copy_data_out,
                                           copy_capacity_out);
}
