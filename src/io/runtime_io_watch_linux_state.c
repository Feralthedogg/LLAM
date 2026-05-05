/**
 * @file src/io/runtime_io_watch_linux_state.c
 * @brief Linux watch state queues, wait-list ownership, and ready-buffer bookkeeping.
 *
 * @details
 * Linux multishot watches keep waiters and buffered readiness under a node's
 * @c watch_lock. Submission queue state is protected by @c submit_lock. Helpers
 * in this file do only list ownership manipulation; backend submission and CQE
 * interpretation live in companion files.
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

#include "runtime_io_watch_linux_internal.h"

/**
 * @brief Append a request to a node's submit queue while submit_lock is held.
 *
 * @param node Destination I/O node.
 * @param req  Request to enqueue.
 */
void nm_queue_node_submit_locked(nm_node_t *node, nm_io_req_t *req) {
    req->next = NULL;
    if (node->submit_tail != NULL) {
        node->submit_tail->next = req;
    } else {
        node->submit_head = req;
    }
    node->submit_tail = req;
}

/**
 * @brief Remove a request from a node's submit queue while submit_lock is held.
 *
 * @param node Node whose queue is searched.
 * @param req  Request to remove.
 * @return true if the request was removed before submission.
 */
bool nm_remove_node_submit_locked(nm_node_t *node, nm_io_req_t *req) {
    nm_io_req_t *prev = NULL;
    nm_io_req_t *cur = node->submit_head;

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

/**
 * @brief Detach all pending submissions and mark them in-flight.
 *
 * @param node Node whose submit queue should be drained.
 * @return Head of detached request list.
 */
nm_io_req_t *nm_take_node_submissions(nm_node_t *node) {
    nm_io_req_t *head;
    nm_io_req_t *cursor;

    pthread_mutex_lock(&node->submit_lock);
    head = node->submit_head;
    node->submit_head = NULL;
    node->submit_tail = NULL;
    cursor = head;
    while (cursor != NULL) {
        // Ownership changes from submit queue to backend in-flight state before
        // the list is returned to the submitter thread.
        atomic_store_explicit(&cursor->inflight_owner_shard, cursor->owner_shard, memory_order_release);
        atomic_store(&cursor->wait_mode, NM_IO_WAIT_MODE_INFLIGHT);
        nm_shard_note_inflight_io_waiter(cursor->owner_shard, 1);
        cursor = cursor->next;
    }
    pthread_mutex_unlock(&node->submit_lock);
    return head;
}

/** @brief Append a poll waiter while watch_lock is held. */
void nm_poll_watch_enqueue_waiter(nm_poll_watch_t *watch, nm_io_req_t *req) {
    req->next = NULL;
    if (watch->wait_tail != NULL) {
        watch->wait_tail->next = req;
    } else {
        watch->wait_head = req;
    }
    watch->wait_tail = req;
}

/**
 * @brief Remove a poll waiter while watch_lock is held.
 *
 * @return true if removed.
 */
bool nm_poll_watch_remove_waiter(nm_poll_watch_t *watch, nm_io_req_t *req) {
    nm_io_req_t *prev = NULL;
    nm_io_req_t *cur = watch->wait_head;

    while (cur != NULL) {
        if (cur == req) {
            if (prev != NULL) {
                prev->next = cur->next;
            } else {
                watch->wait_head = cur->next;
            }
            if (watch->wait_tail == cur) {
                watch->wait_tail = prev;
            }
            cur->next = NULL;
            return true;
        }
        prev = cur;
        cur = cur->next;
    }
    return false;
}

/**
 * @brief Detach all poll waiters from a watch.
 *
 * @param watch Poll watch whose waiters are taken.
 * @return Detached waiter list.
 */
nm_io_req_t *nm_poll_watch_take_waiters(nm_poll_watch_t *watch) {
    nm_io_req_t *head = watch->wait_head;

    watch->wait_head = NULL;
    watch->wait_tail = NULL;
    return head;
}

/**
 * @brief Destroy and unlink a poll watch while watch_lock is held.
 *
 * @param node  Node owning the watch list.
 * @param watch Watch to remove.
 */
void nm_destroy_poll_watch_locked(nm_node_t *node, nm_poll_watch_t *watch) {
    nm_poll_watch_t **cursor = &node->poll_watches;

    while (*cursor != NULL) {
        if (*cursor == watch) {
            *cursor = watch->next;
            free(watch);
            return;
        }
        cursor = &(*cursor)->next;
    }
}

/** @brief Append an accept waiter while watch_lock is held. */
void nm_accept_watch_enqueue_waiter(nm_accept_watch_t *watch, nm_io_req_t *req) {
    req->next = NULL;
    if (watch->wait_tail != NULL) {
        watch->wait_tail->next = req;
    } else {
        watch->wait_head = req;
    }
    watch->wait_tail = req;
}

/**
 * @brief Remove an accept waiter while watch_lock is held.
 *
 * @return true if removed.
 */
bool nm_accept_watch_remove_waiter(nm_accept_watch_t *watch, nm_io_req_t *req) {
    nm_io_req_t *prev = NULL;
    nm_io_req_t *cur = watch->wait_head;

    while (cur != NULL) {
        if (cur == req) {
            if (prev != NULL) {
                prev->next = cur->next;
            } else {
                watch->wait_head = cur->next;
            }
            if (watch->wait_tail == cur) {
                watch->wait_tail = prev;
            }
            cur->next = NULL;
            return true;
        }
        prev = cur;
        cur = cur->next;
    }
    return false;
}

/** @brief Append a receive waiter while watch_lock is held. */
void nm_recv_watch_enqueue_waiter(nm_recv_watch_t *watch, nm_io_req_t *req) {
    req->next = NULL;
    if (watch->wait_tail != NULL) {
        watch->wait_tail->next = req;
    } else {
        watch->wait_head = req;
    }
    watch->wait_tail = req;
}

/**
 * @brief Remove a receive waiter while watch_lock is held.
 *
 * @return true if removed.
 */
bool nm_recv_watch_remove_waiter(nm_recv_watch_t *watch, nm_io_req_t *req) {
    nm_io_req_t *prev = NULL;
    nm_io_req_t *cur = watch->wait_head;

    while (cur != NULL) {
        if (cur == req) {
            if (prev != NULL) {
                prev->next = cur->next;
            } else {
                watch->wait_head = cur->next;
            }
            if (watch->wait_tail == cur) {
                watch->wait_tail = prev;
            }
            cur->next = NULL;
            return true;
        }
        prev = cur;
        cur = cur->next;
    }
    return false;
}

/**
 * @brief Pop one accept waiter from a watch.
 *
 * @param watch Accept watch.
 * @return Waiter request, or NULL.
 */
nm_io_req_t *nm_accept_watch_pop_waiter(nm_accept_watch_t *watch) {
    nm_io_req_t *req = watch->wait_head;

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
nm_io_req_t *nm_recv_watch_pop_waiter(nm_recv_watch_t *watch) {
    nm_io_req_t *req = watch->wait_head;

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
bool nm_accept_watch_push_ready_owned(nm_accept_watch_t *watch, int fd) {
    nm_accept_ready_t *ready = calloc(1, sizeof(*ready));

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
void nm_accept_watch_push_ready(nm_accept_watch_t *watch, int fd) {
    if (!nm_accept_watch_push_ready_owned(watch, fd)) {
        close(fd);
    }
}

/**
 * @brief Pop one buffered accepted fd from a watch.
 *
 * @param watch Accept watch.
 * @return Accepted fd, or -1 when no ready fd exists.
 */
int nm_accept_watch_pop_ready(nm_accept_watch_t *watch) {
    nm_accept_ready_t *ready = watch->ready_head;
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
 * @brief Destroy and unlink an accept watch while watch_lock is held.
 *
 * Buffered accepted fds are closed because their ownership never reached user
 * code.
 */
void nm_destroy_accept_watch_locked(nm_node_t *node, nm_accept_watch_t *watch) {
    nm_accept_watch_t **cursor = &node->accept_watches;

    while (*cursor != NULL) {
        if (*cursor == watch) {
            *cursor = watch->next;
            while (watch->ready_head != NULL) {
                nm_accept_ready_t *next = watch->ready_head->next;

                close(watch->ready_head->fd);
                free(watch->ready_head);
                watch->ready_head = next;
            }
            free(watch);
            return;
        }
        cursor = &(*cursor)->next;
    }
}

/**
 * @brief Release a buffered receive-ready entry.
 *
 * @param rt            Runtime used to resolve provided-buffer owner node.
 * @param fallback_node Node used when the ready entry has no owner index.
 * @param ready         Ready entry to release.
 */
void nm_release_recv_ready(nm_runtime_t *rt, nm_node_t *fallback_node, nm_recv_ready_t *ready) {
    nm_node_t *owner = fallback_node;

    if (ready == NULL) {
        return;
    }
    if (ready->has_buffer && rt != NULL && ready->node_index < rt->active_nodes) {
        owner = &rt->nodes[ready->node_index];
    }
    if (ready->has_buffer && owner != NULL) {
        // Provided buffers must return to the node whose ring owns the buffer
        // group, which can differ after live watch migration.
        (void)nm_node_recycle_recv_buffer(owner, ready->bid);
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
bool nm_recv_watch_push_ready(nm_recv_watch_t *watch,
                                     size_t size,
                                     unsigned short bid,
                                     bool has_buffer,
                                     unsigned node_index,
                                     unsigned char *copy_data,
                                     size_t copy_capacity) {
    nm_recv_ready_t *ready = calloc(1, sizeof(*ready));

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
bool nm_recv_watch_pop_ready(nm_recv_watch_t *watch,
                             size_t *size_out,
                             unsigned short *bid_out,
                             bool *has_buffer_out,
                             unsigned *node_index_out,
                             unsigned char **copy_data_out,
                             size_t *copy_capacity_out) {
    nm_recv_ready_t *ready = watch->ready_head;

    if (ready == NULL) {
        return false;
    }

    watch->ready_head = ready->next;
    if (watch->ready_head == NULL) {
        watch->ready_tail = NULL;
    }
    watch->ready_depth -= 1U;
    if (size_out != NULL) {
        *size_out = ready->size;
    }
    if (bid_out != NULL) {
        *bid_out = ready->bid;
    }
    if (has_buffer_out != NULL) {
        *has_buffer_out = ready->has_buffer;
    }
    if (node_index_out != NULL) {
        *node_index_out = ready->node_index;
    }
    if (copy_data_out != NULL) {
        // Transfer copy_data ownership to caller; otherwise it is released when
        // the ready entry is freed.
        *copy_data_out = ready->copy_data;
        ready->copy_data = NULL;
    }
    if (copy_capacity_out != NULL) {
        *copy_capacity_out = ready->copy_capacity;
    }
    free(ready);
    return true;
}
