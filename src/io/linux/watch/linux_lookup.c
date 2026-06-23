/**
 * @file src/io/linux/watch/linux_lookup.c
 * @brief Linux watch receive cleanup and completion helpers for fd watches.
 *
 * @details
 * Common lookup/creation code lives in @c watch_lookup.c. This file
 * keeps Linux-specific receive-ready release semantics because io_uring
 * provided buffers must be recycled to their owner node.
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
