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

/** @brief Mix a 64-bit watch identity component. */
uint64_t llam_hash_watch_identity_u64(uint64_t value) {
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31;
    return value;
}

/** @brief Update per-shard in-flight I/O waiter accounting. */
void llam_shard_note_inflight_io_waiter(unsigned owner_shard, int delta) {
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

/** @brief Append a request to a node submit queue while submit_lock is held. */
void llam_queue_node_submit_locked(llam_node_t *node, llam_io_req_t *req) {
    req->next = NULL;
    if (node->submit_tail != NULL) {
        node->submit_tail->next = req;
    } else {
        node->submit_head = req;
    }
    node->submit_tail = req;
}

/** @brief Remove a request from a node submit queue while submit_lock is held. */
bool llam_remove_node_submit_locked(llam_node_t *node, llam_io_req_t *req) {
    llam_io_req_t *prev = NULL;
    llam_io_req_t *cur = node->submit_head;

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

/** @brief Detach all queued submissions from a node. */
llam_io_req_t *llam_take_node_submissions(llam_node_t *node) {
    llam_io_req_t *head;

    pthread_mutex_lock(&node->submit_lock);
    head = node->submit_head;
    node->submit_head = NULL;
    node->submit_tail = NULL;
    pthread_mutex_unlock(&node->submit_lock);
    return head;
}

/** @brief Append a poll waiter while watch_lock is held. */
void llam_poll_watch_enqueue_waiter(llam_poll_watch_t *watch, llam_io_req_t *req) {
    req->next = NULL;
    if (watch->wait_tail != NULL) {
        watch->wait_tail->next = req;
    } else {
        watch->wait_head = req;
    }
    watch->wait_tail = req;
}

/** @brief Remove a poll waiter while watch_lock is held. */
bool llam_poll_watch_remove_waiter(llam_poll_watch_t *watch, llam_io_req_t *req) {
    llam_io_req_t *prev = NULL;
    llam_io_req_t *cur = watch->wait_head;

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

/** @brief Detach all poll waiters from a watch. */
llam_io_req_t *llam_poll_watch_take_waiters(llam_poll_watch_t *watch) {
    llam_io_req_t *head = watch->wait_head;

    watch->wait_head = NULL;
    watch->wait_tail = NULL;
    return head;
}

/** @brief Append an accept waiter while watch_lock is held. */
void llam_accept_watch_enqueue_waiter(llam_accept_watch_t *watch, llam_io_req_t *req) {
    req->next = NULL;
    if (watch->wait_tail != NULL) {
        watch->wait_tail->next = req;
    } else {
        watch->wait_head = req;
    }
    watch->wait_tail = req;
}

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

/** @brief Remove an accept waiter while watch_lock is held. */
bool llam_accept_watch_remove_waiter(llam_accept_watch_t *watch, llam_io_req_t *req) {
    llam_io_req_t *prev = NULL;
    llam_io_req_t *cur = watch->wait_head;

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

/** @brief Capture device/inode identity for fd reuse protection. */
bool llam_capture_recv_watch_identity(int fd, dev_t *st_dev, ino_t *st_ino) {
    struct stat st;

    if (fstat(fd, &st) != 0) {
        return false;
    }
    if (st_dev != NULL) {
        *st_dev = st.st_dev;
    }
    if (st_ino != NULL) {
        *st_ino = st.st_ino;
    }
    return true;
}

/** @brief Find a poll watch while watch_lock is held. */
llam_poll_watch_t *llam_find_poll_watch_locked(llam_node_t *node, int fd, short events) {
    llam_poll_watch_t *watch = node->poll_watches;
    dev_t st_dev = 0;
    ino_t st_ino = 0;

    if (!llam_capture_recv_watch_identity(fd, &st_dev, &st_ino)) {
        return NULL;
    }
    while (watch != NULL) {
        if (watch->fd == fd && watch->events == events && watch->st_dev == st_dev && watch->st_ino == st_ino) {
            return watch;
        }
        watch = watch->next;
    }
    return NULL;
}

/** @brief Find an accept watch while watch_lock is held. */
llam_accept_watch_t *llam_find_accept_watch_locked(llam_node_t *node, int fd) {
    llam_accept_watch_t *watch = node->accept_watches;

    while (watch != NULL) {
        if (watch->fd == fd) {
            return watch;
        }
        watch = watch->next;
    }
    return NULL;
}

/** @brief Find a receive watch while watch_lock is held. */
llam_recv_watch_t *llam_find_recv_watch_locked(llam_node_t *node, int fd, dev_t st_dev, ino_t st_ino) {
    llam_recv_watch_t *watch = node->recv_watches;

    while (watch != NULL) {
        if (watch->fd == fd && watch->st_dev == st_dev && watch->st_ino == st_ino) {
            return watch;
        }
        watch = watch->next;
    }
    return NULL;
}

/** @brief Find or create a poll watch while watch_lock is held. */
llam_poll_watch_t *llam_get_or_create_poll_watch_locked(llam_node_t *node, int fd, short events) {
    llam_poll_watch_t *watch = node->poll_watches;
    dev_t st_dev = 0;
    ino_t st_ino = 0;

    if (!llam_capture_recv_watch_identity(fd, &st_dev, &st_ino)) {
        return NULL;
    }
    while (watch != NULL) {
        if (watch->fd == fd && watch->events == events && watch->st_dev == st_dev && watch->st_ino == st_ino) {
            return watch;
        }
        watch = watch->next;
    }

    watch = calloc(1, sizeof(*watch));
    if (watch == NULL) {
        return NULL;
    }
    watch->fd = fd;
    watch->st_dev = st_dev;
    watch->st_ino = st_ino;
    watch->events = events;
    watch->migrate_target_node_index = UINT_MAX;
    // Marks source watches whose live readiness should be forwarded while a
    // target node is taking ownership.
    watch->live_transferred = false;
    watch->next = node->poll_watches;
    node->poll_watches = watch;
    return watch;
}

/** @brief Find or create an accept watch while watch_lock is held. */
llam_accept_watch_t *llam_get_or_create_accept_watch_locked(llam_node_t *node, int fd) {
    llam_accept_watch_t *watch = node->accept_watches;

    while (watch != NULL) {
        if (watch->fd == fd) {
            return watch;
        }
        watch = watch->next;
    }

    watch = calloc(1, sizeof(*watch));
    if (watch == NULL) {
        return NULL;
    }
    watch->fd = fd;
    watch->migrate_target_node_index = UINT_MAX;
    watch->live_transferred = false;
    watch->next = node->accept_watches;
    node->accept_watches = watch;
    return watch;
}

/** @brief Find or create a receive watch while watch_lock is held. */
llam_recv_watch_t *llam_get_or_create_recv_watch_locked(llam_node_t *node, int fd) {
    llam_recv_watch_t *watch;
    dev_t st_dev = 0;
    ino_t st_ino = 0;

    if (!llam_capture_recv_watch_identity(fd, &st_dev, &st_ino)) {
        return NULL;
    }
    watch = llam_find_recv_watch_locked(node, fd, st_dev, st_ino);
    if (watch != NULL) {
        return watch;
    }

    watch = calloc(1, sizeof(*watch));
    if (watch == NULL) {
        return NULL;
    }
    watch->fd = fd;
    watch->st_dev = st_dev;
    watch->st_ino = st_ino;
    watch->migrate_target_node_index = UINT_MAX;
    watch->live_transferred = false;
    watch->next = node->recv_watches;
    node->recv_watches = watch;
    return watch;
}

/** @brief Destroy and unlink a poll watch while watch_lock is held. */
void llam_destroy_poll_watch_locked(llam_node_t *node, llam_poll_watch_t *watch) {
    llam_poll_watch_t **cursor = &node->poll_watches;

    while (*cursor != NULL) {
        if (*cursor == watch) {
            *cursor = watch->next;
            free(watch);
            return;
        }
        cursor = &(*cursor)->next;
    }
}

/** @brief Destroy and unlink an accept watch while closing buffered accepted fds. */
void llam_destroy_accept_watch_locked(llam_node_t *node, llam_accept_watch_t *watch) {
    llam_accept_watch_t **cursor = &node->accept_watches;

    while (*cursor != NULL) {
        if (*cursor == watch) {
            *cursor = watch->next;
            while (watch->ready_head != NULL) {
                llam_accept_ready_t *next = watch->ready_head->next;

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

/** @brief Destroy a receive watch only when it has no active backend/user state. */
void llam_maybe_destroy_recv_watch_locked(llam_node_t *node, llam_recv_watch_t *watch) {
    if (watch == NULL) {
        return;
    }
    if (watch->active || watch->activating || watch->deactivate_queued) {
        return;
    }
    if (watch->wait_head != NULL || watch->ready_head != NULL) {
        return;
    }
    llam_destroy_recv_watch_locked(node, watch);
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
