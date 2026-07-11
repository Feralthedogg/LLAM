/**
 * @file src/io/watch/close.c
 * @brief Descriptor-close cleanup for shared multishot watch state.
 *
 * @details
 * Public close is the descriptor-generation lifetime boundary.  A successful
 * close detaches every waiter that still names the old generation and makes
 * exactly one backend teardown request for each active watch.  Backend-owned
 * watch storage is retained until its terminal event/control path proves that
 * no raw kernel user-data pointer remains.
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

#if LLAM_RUNTIME_BACKEND_LINUX
#include "io/linux/runtime_io_watch_linux_internal.h"
#elif LLAM_RUNTIME_BACKEND_KQUEUE
#include "io/darwin/runtime_io_watch_darwin_internal.h"
#endif

#if !LLAM_PLATFORM_WINDOWS

typedef struct llam_closed_watch_waiters {
    llam_io_req_t *head;
    llam_io_req_t *tail;
} llam_closed_watch_waiters_t;

static void llam_closed_watch_waiters_append(llam_closed_watch_waiters_t *detached,
                                              llam_io_req_t **head,
                                              llam_io_req_t **tail) {
    llam_io_req_t *list_head;
    llam_io_req_t *list_tail;

    if (detached == NULL || head == NULL || tail == NULL || *head == NULL) {
        return;
    }
    list_head = *head;
    list_tail = *tail;
    *head = NULL;
    *tail = NULL;
    if (list_tail == NULL) {
        list_tail = list_head;
        while (list_tail->next != NULL) {
            list_tail = list_tail->next;
        }
    }
    if (detached->tail != NULL) {
        detached->tail->next = list_head;
    } else {
        detached->head = list_head;
    }
    detached->tail = list_tail;
}

static void llam_complete_closed_watch_waiters(llam_node_t *node,
                                                llam_closed_watch_waiters_t *detached) {
    llam_io_req_t *req;

    if (node == NULL || detached == NULL) {
        return;
    }
    req = detached->head;
    detached->head = NULL;
    detached->tail = NULL;
    while (req != NULL) {
        llam_io_req_t *next = req->next;

        req->next = NULL;
#if LLAM_RUNTIME_BACKEND_LINUX
        llam_io_complete_req(node, req, -EBADF, 0U, false);
#elif LLAM_RUNTIME_BACKEND_KQUEUE
        llam_io_complete_req(node, req, -EBADF, false);
#else
        /* Supported POSIX builds use one of the native completion paths above. */
        req->result = -1;
        req->error_code = EBADF;
        atomic_store_explicit(&req->wait_mode, LLAM_IO_WAIT_MODE_NONE, memory_order_release);
#endif
        req = next;
    }
}

static void llam_note_close_control_error(int *first_error) {
    if (first_error != NULL && *first_error == 0) {
        *first_error = errno != 0 ? errno : ENOMEM;
    }
}

static void llam_close_poll_watch_locked(llam_node_t *node,
                                         llam_poll_watch_t *watch,
                                         llam_closed_watch_waiters_t *detached,
                                         bool *kick_node,
                                         int *first_error) {
    watch->accepts_waiters = false;
    watch->migrate_target_node_index = UINT_MAX;
    watch->live_transferred = false;
    watch->sticky_revents = 0;
    llam_closed_watch_waiters_append(detached, &watch->wait_head, &watch->wait_tail);

    if (watch->activating &&
        llam_drop_node_control_locked(node, LLAM_IO_CONTROL_POLL_ACTIVATE, watch)) {
        watch->activating = false;
    }
    if ((watch->active || watch->activating) && !watch->deactivate_queued) {
        watch->deactivate_queued = true;
        if (llam_node_queue_control_locked(node, LLAM_IO_CONTROL_POLL_DEACTIVATE, watch) == 0) {
            *kick_node = true;
        } else {
            watch->deactivate_queued = false;
            llam_note_close_control_error(first_error);
        }
    }
}

static void llam_close_accept_watch_locked(llam_node_t *node,
                                           llam_accept_watch_t *watch,
                                           llam_closed_watch_waiters_t *detached,
                                           bool *kick_node,
                                           int *first_error) {
    watch->accepts_waiters = false;
    watch->migrate_target_node_index = UINT_MAX;
    watch->live_transferred = false;
    llam_closed_watch_waiters_append(detached, &watch->wait_head, &watch->wait_tail);

    if (watch->activating &&
        llam_drop_node_control_locked(node, LLAM_IO_CONTROL_ACCEPT_ACTIVATE, watch)) {
        watch->activating = false;
    }
    if ((watch->active || watch->activating) && !watch->deactivate_queued) {
        watch->deactivate_queued = true;
        if (llam_node_queue_control_locked(node, LLAM_IO_CONTROL_ACCEPT_DEACTIVATE, watch) == 0) {
            *kick_node = true;
        } else {
            watch->deactivate_queued = false;
            llam_note_close_control_error(first_error);
        }
    }
}

static void llam_close_recv_watch_locked(llam_node_t *node,
                                         llam_recv_watch_t *watch,
                                         llam_closed_watch_waiters_t *detached,
                                         bool *kick_node,
                                         int *first_error) {
    watch->accepts_waiters = false;
    watch->migrate_target_node_index = UINT_MAX;
    watch->live_transferred = false;
    llam_closed_watch_waiters_append(detached, &watch->wait_head, &watch->wait_tail);

    if (watch->activating &&
        llam_drop_node_control_locked(node, LLAM_IO_CONTROL_RECV_ACTIVATE, watch)) {
        watch->activating = false;
    }
    if ((watch->active || watch->activating) && !watch->deactivate_queued) {
        watch->deactivate_queued = true;
        if (llam_node_queue_control_locked(node, LLAM_IO_CONTROL_RECV_DEACTIVATE, watch) == 0) {
            *kick_node = true;
        } else {
            watch->deactivate_queued = false;
            llam_note_close_control_error(first_error);
        }
    }
}

static void llam_purge_closed_fd_watches_locked(llam_node_t *node,
                                                llam_fd_t fd,
                                                llam_closed_watch_waiters_t *detached,
                                                bool *kick_node,
                                                int *first_error) {
    llam_poll_watch_t **poll_cursor = &node->poll_watches;
    llam_accept_watch_t **accept_cursor = &node->accept_watches;
    llam_recv_watch_t **recv_cursor = &node->recv_watches;

    while (*poll_cursor != NULL) {
        llam_poll_watch_t *watch = *poll_cursor;

        if (watch->fd != fd) {
            poll_cursor = &watch->next;
            continue;
        }
        llam_close_poll_watch_locked(node, watch, detached, kick_node, first_error);
        if (!watch->active && !watch->activating && !watch->deactivate_queued) {
            llam_destroy_poll_watch_locked(node, watch);
            if (*poll_cursor == watch) {
                poll_cursor = &watch->next;
            }
            continue;
        }
        poll_cursor = &watch->next;
    }

    while (*accept_cursor != NULL) {
        llam_accept_watch_t *watch = *accept_cursor;

        if (watch->fd != fd) {
            accept_cursor = &watch->next;
            continue;
        }
        llam_close_accept_watch_locked(node, watch, detached, kick_node, first_error);
        if (!watch->active && !watch->activating && !watch->deactivate_queued) {
            llam_destroy_accept_watch_locked(node, watch);
            if (*accept_cursor == watch) {
                accept_cursor = &watch->next;
            }
            continue;
        }
        accept_cursor = &watch->next;
    }

    while (*recv_cursor != NULL) {
        llam_recv_watch_t *watch = *recv_cursor;

        if (watch->fd != fd) {
            recv_cursor = &watch->next;
            continue;
        }
        llam_close_recv_watch_locked(node, watch, detached, kick_node, first_error);
        if (!watch->active && !watch->activating && !watch->deactivate_queued) {
            llam_destroy_recv_watch_locked(node, watch);
            if (*recv_cursor == watch) {
                recv_cursor = &watch->next;
            }
            continue;
        }
        recv_cursor = &watch->next;
    }
}

int llam_forget_closed_fd_watch_state(llam_runtime_t *rt, llam_fd_t fd) {
    int first_error = 0;

    if (rt == NULL || rt->nodes == NULL || LLAM_FD_IS_INVALID(fd)) {
        return 0;
    }

    for (unsigned i = 0U; i < rt->active_nodes; ++i) {
        llam_node_t *node = &rt->nodes[i];
        llam_closed_watch_waiters_t detached = {0};
        bool kick_node = false;

        if (!node->watch_lock_initialized) {
            continue;
        }
        pthread_mutex_lock(&node->watch_lock);
        llam_purge_closed_fd_watches_locked(node,
                                            fd,
                                            &detached,
                                            &kick_node,
                                            &first_error);
        pthread_mutex_unlock(&node->watch_lock);

        if (kick_node) {
            llam_kick_node(node);
        }
        llam_complete_closed_watch_waiters(node, &detached);
    }

    if (first_error != 0) {
        errno = first_error;
        return -1;
    }
    return 0;
}

#endif
