/**
 * @file src/io/darwin/runtime_io_watch_darwin_state.c
 * @brief Darwin/kqueue watch state queues and wait-list ownership helpers.
 *
 * @details
 * Darwin watch state mirrors the Linux multishot watch model but uses kqueue
 * EV_DISPATCH events and copied receive buffers instead of io_uring provided
 * buffers. Watch and ready queues are protected by the node's @c watch_lock.
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

#include "runtime_io_watch_darwin_internal.h"

/** @brief Pop one buffered accepted fd from an accept watch. */
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

/** @brief Pop one accept waiter from a watch. */
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

/** @brief Buffer an accepted fd, transferring ownership to the watch. */
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

/** @brief Buffer an accepted fd or close it if buffering fails. */
void llam_accept_watch_push_ready(llam_accept_watch_t *watch, int fd) {
    if (!llam_accept_watch_push_ready_owned(watch, fd)) {
        close(fd);
    }
}

/** @brief Pop one receive waiter from a watch. */
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
 * @brief Copy and buffer receive readiness for a future waiter.
 *
 * Darwin does not use io_uring provided buffers, so readiness cached by a watch
 * owns a heap copy of the received payload.
 */
bool llam_recv_watch_push_ready_copy(llam_recv_watch_t *watch, const unsigned char *data, size_t size) {
    llam_recv_ready_t *ready = calloc(1, sizeof(*ready));

    if (ready == NULL) {
        return false;
    }
    if (size > 0U) {
        ready->copy_data = malloc(size);
        if (ready->copy_data == NULL) {
            free(ready);
            return false;
        }
        memcpy(ready->copy_data, data, size);
        ready->copy_capacity = size;
    }
    ready->size = size;
    ready->bid = 0U;
    ready->node_index = UINT_MAX;
    ready->has_buffer = false;
    if (watch->ready_tail != NULL) {
        watch->ready_tail->next = ready;
    } else {
        watch->ready_head = ready;
    }
    watch->ready_tail = ready;
    watch->ready_depth += 1U;
    return true;
}

/** @brief Destroy and unlink a receive watch while freeing buffered payload copies. */
void llam_destroy_recv_watch_locked(llam_node_t *node, llam_recv_watch_t *watch) {
    llam_recv_watch_t **cursor = &node->recv_watches;

    while (*cursor != NULL) {
        if (*cursor == watch) {
            *cursor = watch->next;
            while (watch->ready_head != NULL) {
                llam_recv_ready_t *next = watch->ready_head->next;

                free(watch->ready_head->copy_data);
                free(watch->ready_head);
                watch->ready_head = next;
            }
            free(watch);
            return;
        }
        cursor = &(*cursor)->next;
    }
}

/** @brief Append a deferred accept completion. */
bool llam_darwin_accept_completion_push(llam_darwin_accept_completion_t **head,
                                             llam_darwin_accept_completion_t **tail,
                                             llam_io_req_t *req,
                                             int result) {
    llam_darwin_accept_completion_t *completion = calloc(1, sizeof(*completion));

    if (completion == NULL) {
        return false;
    }
    completion->req = req;
    completion->result = result;
    if (*tail != NULL) {
        (*tail)->next = completion;
    } else {
        *head = completion;
    }
    *tail = completion;
    return true;
}

/** @brief Append a deferred poll completion group. */
bool llam_darwin_poll_completion_push(llam_darwin_poll_completion_t **head,
                                           llam_darwin_poll_completion_t **tail,
                                           llam_io_req_t *waiters,
                                           int revents) {
    llam_darwin_poll_completion_t *completion;

    if (waiters == NULL) {
        return true;
    }
    completion = calloc(1, sizeof(*completion));
    if (completion == NULL) {
        return false;
    }
    completion->waiters = waiters;
    completion->revents = revents;
    if (*tail != NULL) {
        (*tail)->next = completion;
    } else {
        *head = completion;
    }
    *tail = completion;
    return true;
}

/** @brief Complete and free deferred poll completion groups. */
void llam_darwin_poll_completion_drain(llam_node_t *node, llam_darwin_poll_completion_t *head) {
    while (head != NULL) {
        llam_darwin_poll_completion_t *next_group = head->next;
        llam_io_req_t *waiters = head->waiters;

        while (waiters != NULL) {
            llam_io_req_t *next = waiters->next;

            waiters->next = NULL;
            llam_io_complete_req(node, waiters, head->revents, false);
            waiters = next;
        }
        free(head);
        head = next_group;
    }
}

/** @brief Append a deferred receive completion that already owns copied data. */
bool llam_darwin_recv_completion_push(llam_darwin_recv_completion_t **head,
                                           llam_darwin_recv_completion_t **tail,
                                           llam_io_req_t *req,
                                           size_t size,
                                           unsigned char *copy_data,
                                           size_t copy_capacity) {
    llam_darwin_recv_completion_t *completion = calloc(1, sizeof(*completion));

    if (completion == NULL) {
        return false;
    }
    completion->req = req;
    completion->size = size;
    completion->copy_data = copy_data;
    completion->copy_capacity = copy_capacity;
    if (*tail != NULL) {
        (*tail)->next = completion;
    } else {
        *head = completion;
    }
    *tail = completion;
    return true;
}

/** @brief Copy payload and append a deferred receive completion. */
bool llam_darwin_recv_completion_push_copy(llam_darwin_recv_completion_t **head,
                                                llam_darwin_recv_completion_t **tail,
                                                llam_io_req_t *req,
                                                const unsigned char *data,
                                                size_t size) {
    unsigned char *copy = NULL;

    if (size > 0U) {
        copy = malloc(size);
        if (copy == NULL) {
            return false;
        }
        memcpy(copy, data, size);
    }
    if (!llam_darwin_recv_completion_push(head, tail, req, size, copy, size)) {
        free(copy);
        return false;
    }
    return true;
}

/** @brief Complete and free deferred receive completions. */
void llam_darwin_recv_completion_drain(llam_node_t *node, llam_darwin_recv_completion_t *head) {
    while (head != NULL) {
        llam_darwin_recv_completion_t *next = head->next;
        llam_io_req_t *req = head->req;
        int result = (int)head->size;

        if (req == NULL || req->owned_buffer == NULL ||
            !llam_darwin_assign_owned_buffer(req,
                                           head->copy_data,
                                           head->size,
                                           head->copy_data,
                                           head->copy_capacity)) {
            free(head->copy_data);
            head->copy_data = NULL;
            if (req != NULL) {
                req->use_provided_buffer = false;
                llam_io_complete_req(node, req, -ENOMEM, false);
            }
        } else {
            req->use_provided_buffer = false;
            // Ownership of copy_data moved into req->owned_buffer.
            head->copy_data = NULL;
            llam_io_complete_req(node, req, result, false);
        }
        free(head->copy_data);
        free(head);
        head = next;
    }
}

/** @brief Remove a matching queued control operation while watch_lock is held. */
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
