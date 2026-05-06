/**
 * @file src/io/linux/runtime_io_watch_linux_lookup.c
 * @brief Linux watch lookup and registration helpers for fd-based operations.
 *
 * @details
 * Poll and receive watches include device/inode identity in addition to fd
 * number. That prevents stale watch state from satisfying a new file/socket
 * that reused the same numeric fd after close/open churn.
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
 * @brief Capture stable identity fields for an fd.
 *
 * @param fd     File descriptor to inspect.
 * @param st_dev Optional device id output.
 * @param st_ino Optional inode id output.
 * @return true on successful @c fstat.
 */
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

/**
 * @brief Find an existing poll watch while watch_lock is held.
 *
 * @param node   Node whose poll watch table is searched.
 * @param fd     File descriptor.
 * @param events Requested poll event mask.
 * @return Matching watch, or NULL.
 */
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

/**
 * @brief Find an existing accept watch while watch_lock is held.
 *
 * @param node Node whose accept watch table is searched.
 * @param fd   Listener descriptor.
 * @return Matching watch, or NULL.
 */
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

/**
 * @brief Find an existing receive watch while watch_lock is held.
 *
 * @param node   Node whose receive watch table is searched.
 * @param fd     File descriptor.
 * @param st_dev Captured device id.
 * @param st_ino Captured inode id.
 * @return Matching watch, or NULL.
 */
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

/**
 * @brief Find or create a poll watch while watch_lock is held.
 *
 * @param node   Node that will own the watch.
 * @param fd     File descriptor.
 * @param events Poll event mask.
 * @return Watch on success, NULL on fstat/allocation failure.
 */
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
    // live_transferred is set during watch migration once the target owns all
    // waiters/ready data but the source may still receive a final CQE.
    watch->live_transferred = false;
    watch->next = node->poll_watches;
    node->poll_watches = watch;
    return watch;
}

/**
 * @brief Find or create an accept watch while watch_lock is held.
 *
 * @param node Node that will own the watch.
 * @param fd   Listener descriptor.
 * @return Watch on success, NULL on allocation failure.
 */
llam_accept_watch_t *llam_get_or_create_accept_watch_locked(llam_node_t *node, int fd) {
    llam_accept_watch_t *watch = llam_find_accept_watch_locked(node, fd);

    if (watch != NULL) {
        return watch;
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

/**
 * @brief Find or create a receive watch while watch_lock is held.
 *
 * @param node Node that will own the watch.
 * @param fd   File descriptor.
 * @return Watch on success, NULL on fstat/allocation failure.
 */
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

/**
 * @brief Destroy and unlink a receive watch while watch_lock is held.
 *
 * @param node  Node owning the watch.
 * @param watch Watch to destroy.
 */
void llam_destroy_recv_watch_locked(llam_node_t *node, llam_recv_watch_t *watch) {
    llam_recv_watch_t **cursor = &node->recv_watches;

    while (*cursor != NULL) {
        if (*cursor == watch) {
            *cursor = watch->next;
            while (watch->ready_head != NULL) {
                llam_recv_ready_t *next = watch->ready_head->next;

                llam_release_recv_ready(node->runtime, node, watch->ready_head);
                watch->ready_head = next;
            }
            free(watch);
            return;
        }
        cursor = &(*cursor)->next;
    }
}

/**
 * @brief Destroy an idle receive watch if no activation/deactivation is pending.
 *
 * @param node  Node owning the watch.
 * @param watch Candidate watch.
 */
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

/**
 * @brief Append a deferred accept completion.
 *
 * @return true on success.
 */
bool llam_accept_watch_completion_push(llam_accept_watch_completion_t **head,
                                            llam_accept_watch_completion_t **tail,
                                            llam_io_req_t *req,
                                            int result) {
    llam_accept_watch_completion_t *completion = calloc(1, sizeof(*completion));

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

/**
 * @brief Complete and free a list of deferred accept completions.
 *
 * @param node Node used for completion accounting.
 * @param head Completion list.
 */
void llam_accept_watch_completion_drain(llam_node_t *node, llam_accept_watch_completion_t *head) {
    while (head != NULL) {
        llam_accept_watch_completion_t *next = head->next;

        llam_io_complete_req(node, head->req, head->result, 0U, false);
        free(head);
        head = next;
    }
}

/**
 * @brief Append a deferred poll completion group.
 *
 * @return true on success or when @p waiters is NULL.
 */
bool llam_poll_watch_completion_push(llam_poll_watch_completion_t **head,
                                          llam_poll_watch_completion_t **tail,
                                          llam_io_req_t *waiters,
                                          int revents) {
    llam_poll_watch_completion_t *completion;

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

/**
 * @brief Complete and free deferred poll waiter groups.
 *
 * @param node Node used for completion accounting.
 * @param head Completion list.
 */
void llam_poll_watch_completion_drain(llam_node_t *node, llam_poll_watch_completion_t *head) {
    while (head != NULL) {
        llam_poll_watch_completion_t *next_group = head->next;
        llam_io_req_t *waiters = head->waiters;

        while (waiters != NULL) {
            llam_io_req_t *next = waiters->next;

            waiters->next = NULL;
            llam_io_complete_req(node, waiters, head->revents, 0U, false);
            waiters = next;
        }
        free(head);
        head = next_group;
    }
}

/**
 * @brief Append a deferred receive completion.
 *
 * @return true on success.
 */
bool llam_recv_watch_completion_push(llam_recv_watch_completion_t **head,
                                          llam_recv_watch_completion_t **tail,
                                          llam_io_req_t *req,
                                          size_t size,
                                          unsigned short bid,
                                          bool has_buffer,
                                          unsigned node_index) {
    llam_recv_watch_completion_t *completion = calloc(1, sizeof(*completion));

    if (completion == NULL) {
        return false;
    }
    completion->req = req;
    completion->size = size;
    completion->bid = bid;
    completion->has_buffer = has_buffer;
    completion->node_index = has_buffer ? node_index : UINT_MAX;
    if (*tail != NULL) {
        (*tail)->next = completion;
    } else {
        *head = completion;
    }
    *tail = completion;
    return true;
}

/**
 * @brief Complete and free deferred receive completions.
 *
 * @param rt            Runtime used to resolve provided-buffer owner nodes.
 * @param fallback_node Node used when no owner node is encoded.
 * @param head          Completion list.
 */
void llam_recv_watch_completion_drain(llam_runtime_t *rt,
                                           llam_node_t *fallback_node,
                                           llam_recv_watch_completion_t *head) {
    while (head != NULL) {
        llam_recv_watch_completion_t *next = head->next;
        llam_node_t *owner = fallback_node;
        unsigned flags = 0U;

        if (head->has_buffer && rt != NULL && head->node_index < rt->active_nodes) {
            owner = &rt->nodes[head->node_index];
            // Recreate the CQE buffer flag shape expected by llam_io_complete_req
            // so owned-buffer attachment follows the normal completion path.
            flags = IORING_CQE_F_BUFFER | ((unsigned)head->bid << IORING_CQE_BUFFER_SHIFT);
        }
        llam_io_complete_req(owner, head->req, (int)head->size, flags, false);
        free(head);
        head = next;
    }
}

/**
 * @brief Remove a matching queued control operation while watch_lock is held.
 *
 * @param node   Node whose control queue is searched.
 * @param kind   Control operation kind.
 * @param target Target pointer.
 * @return true if an operation was removed.
 */
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
