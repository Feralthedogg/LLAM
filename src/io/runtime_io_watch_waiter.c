/**
 * @file src/io/runtime_io_watch_waiter.c
 * @brief Shared watch waiter, ready-splice, and activation helpers.
 *
 * @details
 * Watch wait lists are protected by each node's watch lock.  The helpers here
 * perform only list mutation and activation bookkeeping; platform-specific
 * readiness payload ownership remains in backend files.
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

#include "runtime_internal.h"

static void llam_watch_push_waiter_front(llam_io_req_t **head,
                                         llam_io_req_t **tail,
                                         llam_io_req_t *req) {
    if (head == NULL || tail == NULL || req == NULL) {
        return;
    }

    req->next = *head;
    *head = req;
    if (*tail == NULL) {
        *tail = req;
    }
}

static void llam_watch_enqueue_waiter_common(llam_io_req_t **head,
                                             llam_io_req_t **tail,
                                             llam_io_req_t *req) {
    if (head == NULL || tail == NULL || req == NULL) {
        return;
    }

    req->next = NULL;
    if (*tail != NULL) {
        (*tail)->next = req;
    } else {
        *head = req;
    }
    *tail = req;
}

static bool llam_watch_remove_waiter_common(llam_io_req_t **head,
                                            llam_io_req_t **tail,
                                            llam_io_req_t *req) {
    llam_io_req_t *prev = NULL;
    llam_io_req_t *cur;

    if (head == NULL || tail == NULL || req == NULL) {
        return false;
    }

    cur = *head;
    while (cur != NULL) {
        if (cur == req) {
            if (prev != NULL) {
                prev->next = cur->next;
            } else {
                *head = cur->next;
            }
            if (*tail == cur) {
                *tail = prev;
            }
            cur->next = NULL;
            return true;
        }
        prev = cur;
        cur = cur->next;
    }
    return false;
}

llam_io_req_t *llam_poll_watch_take_waiters(llam_poll_watch_t *watch) {
    llam_io_req_t *waiters;

    if (watch == NULL) {
        return NULL;
    }

    waiters = watch->wait_head;
    watch->wait_head = NULL;
    watch->wait_tail = NULL;
    return waiters;
}

void llam_poll_watch_push_waiters_front(llam_poll_watch_t *watch, llam_io_req_t *waiters) {
    llam_io_req_t *tail = waiters;

    if (watch == NULL || waiters == NULL) {
        return;
    }

    while (tail->next != NULL) {
        tail = tail->next;
    }
    tail->next = watch->wait_head;
    watch->wait_head = waiters;
    if (watch->wait_tail == NULL) {
        watch->wait_tail = tail;
    }
}

void llam_poll_watch_enqueue_waiter(llam_poll_watch_t *watch, llam_io_req_t *req) {
    if (watch != NULL) {
        llam_watch_enqueue_waiter_common(&watch->wait_head, &watch->wait_tail, req);
    }
}

bool llam_poll_watch_remove_waiter(llam_poll_watch_t *watch, llam_io_req_t *req) {
    return watch != NULL &&
           llam_watch_remove_waiter_common(&watch->wait_head, &watch->wait_tail, req);
}

void llam_accept_watch_enqueue_waiter(llam_accept_watch_t *watch, llam_io_req_t *req) {
    if (watch != NULL) {
        llam_watch_enqueue_waiter_common(&watch->wait_head, &watch->wait_tail, req);
    }
}

bool llam_accept_watch_remove_waiter(llam_accept_watch_t *watch, llam_io_req_t *req) {
    return watch != NULL &&
           llam_watch_remove_waiter_common(&watch->wait_head, &watch->wait_tail, req);
}

void llam_recv_watch_enqueue_waiter(llam_recv_watch_t *watch, llam_io_req_t *req) {
    if (watch != NULL) {
        llam_watch_enqueue_waiter_common(&watch->wait_head, &watch->wait_tail, req);
    }
}

bool llam_recv_watch_remove_waiter(llam_recv_watch_t *watch, llam_io_req_t *req) {
    return watch != NULL &&
           llam_watch_remove_waiter_common(&watch->wait_head, &watch->wait_tail, req);
}

void llam_accept_watch_push_waiter_front(llam_accept_watch_t *watch, llam_io_req_t *req) {
    if (watch != NULL) {
        llam_watch_push_waiter_front(&watch->wait_head, &watch->wait_tail, req);
    }
}

void llam_recv_watch_push_waiter_front(llam_recv_watch_t *watch, llam_io_req_t *req) {
    if (watch != NULL) {
        llam_watch_push_waiter_front(&watch->wait_head, &watch->wait_tail, req);
    }
}

void llam_accept_watch_splice_ready(llam_accept_watch_t *target, llam_accept_watch_t *source) {
    if (target == NULL || source == NULL || source->ready_head == NULL) {
        return;
    }

    if (target->ready_tail != NULL) {
        target->ready_tail->next = source->ready_head;
    } else {
        target->ready_head = source->ready_head;
    }
    target->ready_tail = source->ready_tail;
    target->ready_depth += source->ready_depth;
    source->ready_head = NULL;
    source->ready_tail = NULL;
    source->ready_depth = 0U;
}

void llam_recv_watch_splice_ready(llam_recv_watch_t *target, llam_recv_watch_t *source) {
    if (target == NULL || source == NULL || source->ready_head == NULL) {
        return;
    }

    if (target->ready_tail != NULL) {
        target->ready_tail->next = source->ready_head;
    } else {
        target->ready_head = source->ready_head;
    }
    target->ready_tail = source->ready_tail;
    target->ready_depth += source->ready_depth;
    source->ready_head = NULL;
    source->ready_tail = NULL;
    source->ready_depth = 0U;
}

bool llam_recv_watch_pop_ready_shared(llam_recv_watch_t *watch,
                                      size_t *size_out,
                                      unsigned short *bid_out,
                                      bool *has_buffer_out,
                                      unsigned *node_index_out,
                                      unsigned char **copy_data_out,
                                      size_t *copy_capacity_out) {
    llam_recv_ready_t *ready;

    if (watch == NULL) {
        return false;
    }

    ready = watch->ready_head;
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
        /*
         * Copied Darwin payloads move to the caller. Linux provided-buffer
         * entries leave this field NULL, so the same ownership rule is shared.
         */
        *copy_data_out = ready->copy_data;
        ready->copy_data = NULL;
    }
    free(ready->copy_data);
    if (copy_capacity_out != NULL) {
        *copy_capacity_out = ready->copy_capacity;
    }
    free(ready);
    return true;
}

static bool llam_watch_disarm_if_empty_locked_common(llam_node_t *node,
                                                     bool wait_empty,
                                                     bool *active,
                                                     bool *activating,
                                                     bool *deactivate_queued,
                                                     llam_io_control_kind_t activate_kind,
                                                     llam_io_control_kind_t deactivate_kind,
                                                     void *target,
                                                     bool *kick_node) {
    if (kick_node != NULL) {
        *kick_node = false;
    }
    if (node == NULL || active == NULL || activating == NULL ||
        deactivate_queued == NULL || target == NULL || !wait_empty) {
        return true;
    }

    if (*activating && llam_drop_node_control_locked(node, activate_kind, target)) {
        /*
         * Cancel removed the final waiter before the worker consumed the
         * activation control.  Dropping it avoids backend work that no task can
         * observe and prevents run-drain hangs on idle descriptors.
         */
        *activating = false;
    } else if ((*active || *activating) && !*deactivate_queued) {
        *deactivate_queued = true;
        if (llam_node_queue_control_locked(node, deactivate_kind, target) != 0) {
            *deactivate_queued = false;
            return false;
        }
        if (kick_node != NULL) {
            *kick_node = true;
        }
    }
    return true;
}

bool llam_poll_watch_disarm_if_empty_locked(llam_node_t *node, llam_poll_watch_t *watch, bool *kick_node) {
    if (watch == NULL) {
        return true;
    }
    return llam_watch_disarm_if_empty_locked_common(node,
                                                   watch->wait_head == NULL,
                                                   &watch->active,
                                                   &watch->activating,
                                                   &watch->deactivate_queued,
                                                   LLAM_IO_CONTROL_POLL_ACTIVATE,
                                                   LLAM_IO_CONTROL_POLL_DEACTIVATE,
                                                   watch,
                                                   kick_node);
}

bool llam_accept_watch_disarm_if_empty_locked(llam_node_t *node, llam_accept_watch_t *watch, bool *kick_node) {
    if (watch == NULL) {
        return true;
    }
    return llam_watch_disarm_if_empty_locked_common(node,
                                                   watch->wait_head == NULL,
                                                   &watch->active,
                                                   &watch->activating,
                                                   &watch->deactivate_queued,
                                                   LLAM_IO_CONTROL_ACCEPT_ACTIVATE,
                                                   LLAM_IO_CONTROL_ACCEPT_DEACTIVATE,
                                                   watch,
                                                   kick_node);
}

bool llam_recv_watch_disarm_if_empty_locked(llam_node_t *node, llam_recv_watch_t *watch, bool *kick_node) {
    if (watch == NULL) {
        return true;
    }
    return llam_watch_disarm_if_empty_locked_common(node,
                                                   watch->wait_head == NULL,
                                                   &watch->active,
                                                   &watch->activating,
                                                   &watch->deactivate_queued,
                                                   LLAM_IO_CONTROL_RECV_ACTIVATE,
                                                   LLAM_IO_CONTROL_RECV_DEACTIVATE,
                                                   watch,
                                                   kick_node);
}

bool llam_remove_watch_waiter_after_abort(llam_node_t *node,
                                          llam_io_req_t *req,
                                          unsigned mode,
                                          bool clear_wait_mode) {
    bool removed = false;
    bool kick_node = false;

    if (node == NULL || req == NULL) {
        return false;
    }

    pthread_mutex_lock(&node->watch_lock);
    if (mode == LLAM_IO_WAIT_MODE_POLL_WATCH && req->poll_watch != NULL) {
        removed = llam_poll_watch_remove_waiter(req->poll_watch, req);
        if (removed) {
            (void)llam_poll_watch_disarm_if_empty_locked(node, req->poll_watch, &kick_node);
        }
    } else if (mode == LLAM_IO_WAIT_MODE_ACCEPT_WATCH && req->accept_watch != NULL) {
        removed = llam_accept_watch_remove_waiter(req->accept_watch, req);
        if (removed) {
            (void)llam_accept_watch_disarm_if_empty_locked(node, req->accept_watch, &kick_node);
        }
    } else if (mode == LLAM_IO_WAIT_MODE_RECV_WATCH && req->recv_watch != NULL) {
        removed = llam_recv_watch_remove_waiter(req->recv_watch, req);
        if (removed) {
            (void)llam_recv_watch_disarm_if_empty_locked(node, req->recv_watch, &kick_node);
        }
    }
    if (removed && clear_wait_mode) {
        atomic_store_explicit(&req->wait_mode, LLAM_IO_WAIT_MODE_NONE, memory_order_release);
        atomic_store_explicit(&req->inflight_owner_shard, UINT_MAX, memory_order_release);
    }
    pthread_mutex_unlock(&node->watch_lock);
    if (kick_node) {
        llam_kick_node(node);
    }
    return removed;
}

bool llam_arm_watch_locked_common(llam_node_t *node,
                                  bool *active,
                                  bool *activating,
                                  bool *deactivate_queued,
                                  llam_io_control_kind_t deactivate_kind,
                                  llam_io_control_kind_t activate_kind,
                                  void *target,
                                  bool *kick_node) {
    if (node == NULL || active == NULL || activating == NULL ||
        deactivate_queued == NULL || target == NULL) {
        return false;
    }

    if (!*active && !*activating && *deactivate_queued) {
        /*
         * A new waiter can arrive before the backend worker consumes a queued
         * deactivate. Drop that stale control node and keep using the watch.
         */
        if (!llam_drop_node_control_locked(node, deactivate_kind, target)) {
            return false;
        }
        *deactivate_queued = false;
    }
    if (!*active && !*activating) {
        if (llam_node_queue_control_locked(node, activate_kind, target) != 0) {
            return false;
        }
        *activating = true;
        if (kick_node != NULL) {
            *kick_node = true;
        }
    }
    return true;
}

bool llam_arm_poll_watch_locked(llam_node_t *node, llam_poll_watch_t *watch, bool *kick_node) {
    if (node == NULL || watch == NULL) {
        return false;
    }
    return llam_arm_watch_locked_common(node,
                                        &watch->active,
                                        &watch->activating,
                                        &watch->deactivate_queued,
                                        LLAM_IO_CONTROL_POLL_DEACTIVATE,
                                        LLAM_IO_CONTROL_POLL_ACTIVATE,
                                        watch,
                                        kick_node);
}

bool llam_arm_accept_watch_locked(llam_node_t *node, llam_accept_watch_t *watch, bool *kick_node) {
    if (node == NULL || watch == NULL) {
        return false;
    }
    return llam_arm_watch_locked_common(node,
                                        &watch->active,
                                        &watch->activating,
                                        &watch->deactivate_queued,
                                        LLAM_IO_CONTROL_ACCEPT_DEACTIVATE,
                                        LLAM_IO_CONTROL_ACCEPT_ACTIVATE,
                                        watch,
                                        kick_node);
}

bool llam_arm_recv_watch_locked(llam_node_t *node, llam_recv_watch_t *watch, bool *kick_node) {
    if (node == NULL || watch == NULL) {
        return false;
    }
    return llam_arm_watch_locked_common(node,
                                        &watch->active,
                                        &watch->activating,
                                        &watch->deactivate_queued,
                                        LLAM_IO_CONTROL_RECV_DEACTIVATE,
                                        LLAM_IO_CONTROL_RECV_ACTIVATE,
                                        watch,
                                        kick_node);
}
