/**
 * @file src/io/darwin/watch/darwin_worker.c
 * @brief Darwin/BSD I/O worker loop and kqueue polling lifecycle.
 *
 * @details
 * Each kqueue I/O node owns a worker. The worker applies queued control
 * operations, submits one-shot requests, waits for kqueue events, handles user
 * wake events, and dispatches tagged watch/request events to specialized
 * handlers.
 *
 * A returned @c kevent batch contains borrowed @c udata pointers. The worker
 * pins every live watch and every request's parent storage before dispatch, and
 * releases those pins only after the whole batch has been processed. If a
 * request pin cannot be acquired, its event is rewritten to an inert control
 * tag so no handler can dereference partially owned storage.
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

#include "io/darwin/runtime_io_watch_darwin_internal.h"

void llam_darwin_pin_event_batch(llam_node_t *node, struct kevent *events, unsigned count) {
    if (node == NULL || events == NULL || count == 0U) {
        return;
    }

    pthread_mutex_lock(&node->watch_lock);
    for (unsigned i = 0U; i < count; ++i) {
        uint64_t user_data;
        void *ptr;

        if (llam_kqueue_is_worker_wake_event(&events[i])) {
            continue;
        }
        user_data = (uint64_t)(uintptr_t)events[i].udata;
        ptr = llam_io_udata_ptr(user_data);
        switch (llam_io_udata_tag(user_data)) {
        case LLAM_IO_UDATA_POLL_WATCH:
            if (ptr != NULL && !((llam_poll_watch_t *)ptr)->retired) {
                llam_poll_watch_pin_locked(ptr);
            }
            break;
        case LLAM_IO_UDATA_ACCEPT_WATCH:
            if (ptr != NULL && !((llam_accept_watch_t *)ptr)->retired) {
                llam_accept_watch_pin_locked(ptr);
            }
            break;
        case LLAM_IO_UDATA_RECV_WATCH:
            if (ptr != NULL && !((llam_recv_watch_t *)ptr)->retired) {
                llam_recv_watch_pin_locked(ptr);
            }
            break;
        default:
            break;
        }
    }
    pthread_mutex_unlock(&node->watch_lock);

    for (unsigned i = 0U; i < count; ++i) {
        uint64_t user_data;

        if (llam_kqueue_is_worker_wake_event(&events[i])) {
            continue;
        }
        user_data = (uint64_t)(uintptr_t)events[i].udata;
        if (llam_io_udata_tag(user_data) == LLAM_IO_UDATA_REQ) {
            if (!llam_io_req_backend_event_pin(llam_io_udata_ptr(user_data))) {
                /* Suppress an event that could not acquire all parent storage pins. */
                events[i].udata = (void *)(uintptr_t)llam_io_udata_encode(NULL, LLAM_IO_UDATA_CONTROL);
            }
        }
    }
}

void llam_darwin_unpin_event_batch(llam_node_t *node, const struct kevent *events, unsigned count) {
    if (node == NULL || events == NULL || count == 0U) {
        return;
    }

    pthread_mutex_lock(&node->watch_lock);
    for (unsigned i = 0U; i < count; ++i) {
        uint64_t user_data;
        void *ptr;

        if (llam_kqueue_is_worker_wake_event(&events[i])) {
            continue;
        }
        user_data = (uint64_t)(uintptr_t)events[i].udata;
        ptr = llam_io_udata_ptr(user_data);
        switch (llam_io_udata_tag(user_data)) {
        case LLAM_IO_UDATA_POLL_WATCH:
            llam_poll_watch_unpin_locked(node, ptr);
            break;
        case LLAM_IO_UDATA_ACCEPT_WATCH:
            llam_accept_watch_unpin_locked(node, ptr);
            break;
        case LLAM_IO_UDATA_RECV_WATCH:
            llam_recv_watch_unpin_locked(node, ptr);
            break;
        default:
            break;
        }
    }
    pthread_mutex_unlock(&node->watch_lock);

    for (unsigned i = 0U; i < count; ++i) {
        uint64_t user_data;

        if (llam_kqueue_is_worker_wake_event(&events[i])) {
            continue;
        }
        user_data = (uint64_t)(uintptr_t)events[i].udata;
        if (llam_io_udata_tag(user_data) == LLAM_IO_UDATA_REQ) {
            llam_io_req_backend_event_unpin(llam_io_udata_ptr(user_data));
        }
    }
}

/** @brief Apply one queued Darwin control operation. */
static bool llam_darwin_control_guards_fd_lifecycle(const llam_io_control_op_t *op) {
    return op != NULL &&
           (op->kind == LLAM_IO_CONTROL_POLL_ACTIVATE ||
            op->kind == LLAM_IO_CONTROL_POLL_DEACTIVATE ||
            op->kind == LLAM_IO_CONTROL_ACCEPT_ACTIVATE ||
            op->kind == LLAM_IO_CONTROL_ACCEPT_DEACTIVATE ||
            op->kind == LLAM_IO_CONTROL_RECV_ACTIVATE ||
            op->kind == LLAM_IO_CONTROL_RECV_DEACTIVATE);
}

static bool llam_darwin_has_attachable_read_replacement_locked(const llam_node_t *node,
                                                                int fd,
                                                                const void *closed_watch) {
    const llam_poll_watch_t *poll_watch;
    const llam_accept_watch_t *accept_watch;
    const llam_recv_watch_t *recv_watch;

    if (node == NULL) {
        return false;
    }
    for (poll_watch = node->poll_watches; poll_watch != NULL; poll_watch = poll_watch->next) {
        if ((const void *)poll_watch != closed_watch && poll_watch->fd == fd &&
            (poll_watch->events & (POLLIN | POLLPRI)) != 0 &&
            poll_watch->accepts_waiters && !poll_watch->retired && !poll_watch->destroy_pending) {
            return true;
        }
    }
    for (accept_watch = node->accept_watches; accept_watch != NULL; accept_watch = accept_watch->next) {
        if ((const void *)accept_watch != closed_watch && accept_watch->fd == fd &&
            accept_watch->accepts_waiters && !accept_watch->retired && !accept_watch->destroy_pending) {
            return true;
        }
    }
    for (recv_watch = node->recv_watches; recv_watch != NULL; recv_watch = recv_watch->next) {
        if ((const void *)recv_watch != closed_watch && recv_watch->fd == fd &&
            recv_watch->accepts_waiters && !recv_watch->retired && !recv_watch->destroy_pending) {
            return true;
        }
    }
    return false;
}

static bool llam_darwin_has_attachable_write_replacement_locked(const llam_node_t *node,
                                                                 int fd,
                                                                 const void *closed_watch) {
    const llam_poll_watch_t *poll_watch;

    if (node == NULL) {
        return false;
    }
    for (poll_watch = node->poll_watches; poll_watch != NULL; poll_watch = poll_watch->next) {
        if ((const void *)poll_watch != closed_watch && poll_watch->fd == fd &&
            (poll_watch->events & POLLOUT) != 0 && poll_watch->accepts_waiters &&
            !poll_watch->retired && !poll_watch->destroy_pending) {
            return true;
        }
    }
    return false;
}

static bool llam_darwin_closed_fd_identity_matches(int fd, dev_t st_dev, ino_t st_ino) {
    dev_t current_dev = 0;
    ino_t current_ino = 0;

    return llam_capture_fd_watch_identity(fd, &current_dev, &current_ino) &&
           current_dev == st_dev && current_ino == st_ino;
}

static bool llam_darwin_closed_accept_identity_matches(const llam_accept_watch_t *watch) {
    struct sockaddr_storage local_addr;
    socklen_t local_addrlen = (socklen_t)sizeof(local_addr);
    bool has_local_addr = false;

    if (watch == NULL ||
        !llam_darwin_closed_fd_identity_matches(watch->fd, watch->st_dev, watch->st_ino)) {
        return false;
    }
    memset(&local_addr, 0, sizeof(local_addr));
    if (getsockname(watch->fd,
                    (struct sockaddr *)(void *)&local_addr,
                    &local_addrlen) == 0 &&
        local_addrlen <= (socklen_t)sizeof(local_addr)) {
        has_local_addr = true;
    } else {
        local_addrlen = 0U;
    }
    if (watch->has_local_addr != has_local_addr) {
        return false;
    }
    return !has_local_addr ||
           (watch->local_addrlen == local_addrlen &&
            memcmp(&watch->local_addr, &local_addr, (size_t)local_addrlen) == 0);
}

static bool llam_darwin_closed_poll_delete_plan_locked(const llam_node_t *node,
                                                        const llam_poll_watch_t *watch,
                                                        bool *delete_read,
                                                        bool *delete_write) {
    if (delete_read != NULL) {
        *delete_read = false;
    }
    if (delete_write != NULL) {
        *delete_write = false;
    }
    if (watch == NULL ||
        !llam_darwin_closed_fd_identity_matches(watch->fd, watch->st_dev, watch->st_ino)) {
        return false;
    }
    if (delete_read != NULL && (watch->events & (POLLIN | POLLPRI)) != 0) {
        *delete_read = !llam_darwin_has_attachable_read_replacement_locked(node,
                                                                           watch->fd,
                                                                           watch);
    }
    if (delete_write != NULL && (watch->events & POLLOUT) != 0) {
        *delete_write = !llam_darwin_has_attachable_write_replacement_locked(node,
                                                                             watch->fd,
                                                                             watch);
    }
    return (delete_read != NULL && *delete_read) ||
           (delete_write != NULL && *delete_write);
}

static bool llam_darwin_closed_accept_needs_delete_locked(const llam_node_t *node,
                                                           const llam_accept_watch_t *watch) {
    return watch != NULL &&
           !llam_darwin_has_attachable_read_replacement_locked(node, watch->fd, watch) &&
           llam_darwin_closed_accept_identity_matches(watch);
}

static bool llam_darwin_closed_recv_needs_delete_locked(const llam_node_t *node,
                                                         const llam_recv_watch_t *watch) {
    return watch != NULL &&
           !llam_darwin_has_attachable_read_replacement_locked(node, watch->fd, watch) &&
           llam_darwin_closed_fd_identity_matches(watch->fd, watch->st_dev, watch->st_ino);
}

static int llam_darwin_poll_watch_delete_filters(llam_node_t *node,
                                                  llam_poll_watch_t *watch,
                                                  bool delete_read,
                                                  bool delete_write) {
    struct kevent change;

    if (delete_read) {
        int rc;

        EV_SET(&change,
               (uintptr_t)watch->fd,
               EVFILT_READ,
               EV_DELETE,
               0U,
               0,
               (void *)(uintptr_t)llam_io_udata_encode(watch, LLAM_IO_UDATA_POLL_WATCH));
        rc = llam_darwin_kevent_apply(node, &change, 1);
        if (rc != 0 && errno != ENOENT && errno != EBADF) {
            return -1;
        }
    }
    if (delete_write) {
        int rc;

        EV_SET(&change,
               (uintptr_t)watch->fd,
               EVFILT_WRITE,
               EV_DELETE,
               0U,
               0,
               (void *)(uintptr_t)llam_io_udata_encode(watch, LLAM_IO_UDATA_POLL_WATCH));
        rc = llam_darwin_kevent_apply(node, &change, 1);
        if (rc != 0 && errno != ENOENT && errno != EBADF) {
            return -1;
        }
    }
    return 0;
}

static bool llam_darwin_delete_definitively_complete(int rc, int cleanup_error, bool attempted) {
    return !attempted || rc == 0 || cleanup_error == ENOENT || cleanup_error == EBADF;
}

static bool llam_darwin_cleanup_error_retryable(int cleanup_error) {
    return cleanup_error == EAGAIN
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
           || cleanup_error == EWOULDBLOCK
#endif
        ;
}

void llam_darwin_process_control(llam_node_t *node, llam_io_control_op_t *op) {
    int rc = 0;

    switch (op->kind) {
    case LLAM_IO_CONTROL_POLL_ACTIVATE: {
        llam_poll_watch_t *watch = op->target;
        llam_io_req_t *waiters = NULL;
        int activation_error = 0;
        bool eligible;

        pthread_mutex_lock(&node->watch_lock);
        eligible = watch->accepts_waiters;
        pthread_mutex_unlock(&node->watch_lock);
        if (eligible) {
            rc = llam_darwin_poll_watch_change(node, watch, EV_ADD | EV_ENABLE | LLAM_KQUEUE_WATCH_ONESHOT_FLAGS);
        } else {
            errno = EBADF;
            rc = -1;
        }
        activation_error = rc == 0 ? 0 : errno;
        pthread_mutex_lock(&node->watch_lock);
        if (rc == 0) {
            // Kqueue one-shot delivery disables the watch until the event
            // handler explicitly re-enables it.
            watch->active = true;
            watch->activating = false;
            (void)llam_node_note_pending_ops(node, 1U);
        } else {
            watch->active = false;
            watch->activating = false;
            watch->sticky_revents = 0;
            waiters = llam_poll_watch_take_waiters(watch);
            if (!watch->accepts_waiters && !watch->deactivate_queued) {
                llam_destroy_poll_watch_locked(node, watch);
            }
        }
        pthread_mutex_unlock(&node->watch_lock);
        while (waiters != NULL) {
            llam_io_req_t *next = waiters->next;

            waiters->next = NULL;
            llam_io_complete_req(node, waiters, -activation_error, false);
            waiters = next;
        }
        break;
    }
    case LLAM_IO_CONTROL_POLL_DEACTIVATE: {
        llam_poll_watch_t *watch = op->target;
        unsigned migrate_target = UINT_MAX;
        bool kick_target = false;
        bool retry_queued = false;
        bool attempted_delete;
        bool cleanup_complete;
        bool closed;
        bool delete_read = false;
        bool delete_write = false;
        int cleanup_error = 0;
        int retry_error = 0;

        pthread_mutex_lock(&node->watch_lock);
        closed = !watch->accepts_waiters;
        if (closed) {
            attempted_delete = llam_darwin_closed_poll_delete_plan_locked(node,
                                                                           watch,
                                                                           &delete_read,
                                                                           &delete_write);
        } else {
            delete_read = (watch->events & (POLLIN | POLLPRI)) != 0;
            delete_write = (watch->events & POLLOUT) != 0;
            attempted_delete = delete_read || delete_write;
        }
        pthread_mutex_unlock(&node->watch_lock);
        rc = attempted_delete ? llam_darwin_poll_watch_delete_filters(node,
                                                                       watch,
                                                                       delete_read,
                                                                       delete_write) : 0;
        cleanup_error = rc == 0 ? 0 : errno;
        cleanup_complete = llam_darwin_delete_definitively_complete(rc,
                                                                    cleanup_error,
                                                                    attempted_delete);
        pthread_mutex_lock(&node->watch_lock);
        watch->activating = false;
        watch->deactivate_queued = false;
        if (!cleanup_complete) {
            if (llam_darwin_cleanup_error_retryable(cleanup_error)) {
                watch->deactivate_queued = true;
                if (llam_node_queue_control_locked(node,
                                                   LLAM_IO_CONTROL_POLL_DEACTIVATE,
                                                   watch) == 0) {
                    retry_queued = true;
                } else {
                    watch->deactivate_queued = false;
                    retry_error = errno != 0 ? errno : ENOMEM;
                }
            }
            pthread_mutex_unlock(&node->watch_lock);
            if (retry_queued) {
                llam_kick_node(node);
            } else if (retry_error != 0) {
                llam_record_fatal(node->runtime, retry_error);
            } else if (llam_darwin_kevent_cleanup_error_is_fatal(cleanup_error)) {
                llam_record_fatal(node->runtime, cleanup_error);
            }
            break;
        }
        if (watch->active) {
            watch->active = false;
            (void)llam_node_complete_pending_ops(node, 1U);
        }
        if (!closed && watch->migrate_target_node_index != UINT_MAX) {
            migrate_target = watch->migrate_target_node_index;
        }
        if (closed) {
            llam_destroy_poll_watch_locked(node, watch);
        }
        pthread_mutex_unlock(&node->watch_lock);
        if (rc != 0 && cleanup_error != EBADF &&
            llam_darwin_kevent_cleanup_error_is_fatal(cleanup_error)) {
            llam_record_fatal(node->runtime, cleanup_error);
        }
        if (migrate_target != UINT_MAX &&
            llam_finalize_poll_watch_migration(node, watch, migrate_target, &kick_target) &&
            kick_target &&
            migrate_target < node->runtime->active_nodes) {
            llam_kick_node(&node->runtime->nodes[migrate_target]);
        }
        break;
    }
    case LLAM_IO_CONTROL_ACCEPT_ACTIVATE: {
        llam_accept_watch_t *watch = op->target;
        llam_io_req_t *waiters = NULL;
        int activation_error = 0;
        bool eligible;

        pthread_mutex_lock(&node->watch_lock);
        eligible = watch->accepts_waiters;
        pthread_mutex_unlock(&node->watch_lock);
        if (eligible) {
            rc = llam_darwin_accept_watch_change(node, watch, EV_ADD | EV_ENABLE | LLAM_KQUEUE_WATCH_ONESHOT_FLAGS | EV_CLEAR);
        } else {
            errno = EBADF;
            rc = -1;
        }
        activation_error = rc == 0 ? 0 : errno;
        pthread_mutex_lock(&node->watch_lock);
        if (rc == 0) {
            watch->active = true;
            watch->activating = false;
            (void)llam_node_note_pending_ops(node, 1U);
        } else {
            watch->active = false;
            watch->activating = false;
            waiters = watch->wait_head;
            watch->wait_head = NULL;
            watch->wait_tail = NULL;
            if (!watch->accepts_waiters && !watch->deactivate_queued) {
                llam_destroy_accept_watch_locked(node, watch);
            }
        }
        pthread_mutex_unlock(&node->watch_lock);
        while (waiters != NULL) {
            llam_io_req_t *next = waiters->next;

            waiters->next = NULL;
            llam_io_complete_req(node, waiters, -activation_error, false);
            waiters = next;
        }
        break;
    }
    case LLAM_IO_CONTROL_ACCEPT_DEACTIVATE: {
        llam_accept_watch_t *watch = op->target;
        unsigned migrate_target = UINT_MAX;
        bool kick_target = false;
        bool retry_queued = false;
        bool attempted_delete;
        bool cleanup_complete;
        bool closed;
        int cleanup_error = 0;
        int retry_error = 0;

        pthread_mutex_lock(&node->watch_lock);
        closed = !watch->accepts_waiters;
        attempted_delete = !closed || llam_darwin_closed_accept_needs_delete_locked(node, watch);
        pthread_mutex_unlock(&node->watch_lock);
        rc = attempted_delete ? llam_darwin_accept_watch_change(node, watch, EV_DELETE) : 0;
        cleanup_error = rc == 0 ? 0 : errno;
        cleanup_complete = llam_darwin_delete_definitively_complete(rc,
                                                                    cleanup_error,
                                                                    attempted_delete);
        pthread_mutex_lock(&node->watch_lock);
        watch->activating = false;
        watch->deactivate_queued = false;
        if (!cleanup_complete) {
            if (llam_darwin_cleanup_error_retryable(cleanup_error)) {
                watch->deactivate_queued = true;
                if (llam_node_queue_control_locked(node,
                                                   LLAM_IO_CONTROL_ACCEPT_DEACTIVATE,
                                                   watch) == 0) {
                    retry_queued = true;
                } else {
                    watch->deactivate_queued = false;
                    retry_error = errno != 0 ? errno : ENOMEM;
                }
            }
            pthread_mutex_unlock(&node->watch_lock);
            if (retry_queued) {
                llam_kick_node(node);
            } else if (retry_error != 0) {
                llam_record_fatal(node->runtime, retry_error);
            } else if (llam_darwin_kevent_cleanup_error_is_fatal(cleanup_error)) {
                llam_record_fatal(node->runtime, cleanup_error);
            }
            break;
        }
        if (watch->active) {
            watch->active = false;
            (void)llam_node_complete_pending_ops(node, 1U);
        }
        if (!closed && watch->migrate_target_node_index != UINT_MAX) {
            migrate_target = watch->migrate_target_node_index;
        }
        if (closed) {
            llam_destroy_accept_watch_locked(node, watch);
        }
        pthread_mutex_unlock(&node->watch_lock);
        if (rc != 0 && cleanup_error != EBADF &&
            llam_darwin_kevent_cleanup_error_is_fatal(cleanup_error)) {
            llam_record_fatal(node->runtime, cleanup_error);
        }
        if (migrate_target != UINT_MAX &&
            llam_finalize_accept_watch_migration(node, watch, migrate_target, &kick_target) &&
            kick_target &&
            migrate_target < node->runtime->active_nodes) {
            llam_kick_node(&node->runtime->nodes[migrate_target]);
        }
        break;
    }
    case LLAM_IO_CONTROL_RECV_ACTIVATE: {
        llam_recv_watch_t *watch = op->target;
        llam_io_req_t *waiters = NULL;
        int activation_error = 0;
        bool eligible;

        pthread_mutex_lock(&node->watch_lock);
        eligible = watch->accepts_waiters;
        pthread_mutex_unlock(&node->watch_lock);
        if (eligible) {
            rc = llam_darwin_recv_watch_change(node, watch, EV_ADD | EV_ENABLE | LLAM_KQUEUE_WATCH_ONESHOT_FLAGS | EV_CLEAR);
        } else {
            errno = EBADF;
            rc = -1;
        }
        activation_error = rc == 0 ? 0 : errno;
        pthread_mutex_lock(&node->watch_lock);
        if (rc == 0) {
            watch->active = true;
            watch->activating = false;
            (void)llam_node_note_pending_ops(node, 1U);
        } else {
            watch->active = false;
            watch->activating = false;
            waiters = watch->wait_head;
            watch->wait_head = NULL;
            watch->wait_tail = NULL;
            if (!watch->accepts_waiters && !watch->deactivate_queued) {
                llam_destroy_recv_watch_locked(node, watch);
            }
        }
        pthread_mutex_unlock(&node->watch_lock);
        while (waiters != NULL) {
            llam_io_req_t *next = waiters->next;

            waiters->next = NULL;
            llam_io_complete_req(node, waiters, -activation_error, false);
            waiters = next;
        }
        break;
    }
    case LLAM_IO_CONTROL_RECV_DEACTIVATE: {
        llam_recv_watch_t *watch = op->target;
        unsigned migrate_target = UINT_MAX;
        bool kick_target = false;
        bool retry_queued = false;
        bool attempted_delete;
        bool cleanup_complete;
        bool closed;
        int cleanup_error = 0;
        int retry_error = 0;

        pthread_mutex_lock(&node->watch_lock);
        closed = !watch->accepts_waiters;
        attempted_delete = !closed || llam_darwin_closed_recv_needs_delete_locked(node, watch);
        pthread_mutex_unlock(&node->watch_lock);
        rc = attempted_delete ? llam_darwin_recv_watch_change(node, watch, EV_DELETE) : 0;
        cleanup_error = rc == 0 ? 0 : errno;
        cleanup_complete = llam_darwin_delete_definitively_complete(rc,
                                                                    cleanup_error,
                                                                    attempted_delete);
        pthread_mutex_lock(&node->watch_lock);
        watch->activating = false;
        watch->deactivate_queued = false;
        if (!cleanup_complete) {
            if (llam_darwin_cleanup_error_retryable(cleanup_error)) {
                watch->deactivate_queued = true;
                if (llam_node_queue_control_locked(node,
                                                   LLAM_IO_CONTROL_RECV_DEACTIVATE,
                                                   watch) == 0) {
                    retry_queued = true;
                } else {
                    watch->deactivate_queued = false;
                    retry_error = errno != 0 ? errno : ENOMEM;
                }
            }
            pthread_mutex_unlock(&node->watch_lock);
            if (retry_queued) {
                llam_kick_node(node);
            } else if (retry_error != 0) {
                llam_record_fatal(node->runtime, retry_error);
            } else if (llam_darwin_kevent_cleanup_error_is_fatal(cleanup_error)) {
                llam_record_fatal(node->runtime, cleanup_error);
            }
            break;
        }
        if (watch->active) {
            watch->active = false;
            (void)llam_node_complete_pending_ops(node, 1U);
        }
        if (closed) {
            llam_destroy_recv_watch_locked(node, watch);
        } else if (watch->migrate_target_node_index != UINT_MAX) {
            migrate_target = watch->migrate_target_node_index;
        } else {
            llam_maybe_destroy_recv_watch_locked(node, watch);
        }
        pthread_mutex_unlock(&node->watch_lock);
        if (rc != 0 && cleanup_error != EBADF &&
            llam_darwin_kevent_cleanup_error_is_fatal(cleanup_error)) {
            llam_record_fatal(node->runtime, cleanup_error);
        }
        if (migrate_target != UINT_MAX &&
            llam_finalize_recv_watch_migration(node, watch, migrate_target, &kick_target) &&
            kick_target &&
            migrate_target < node->runtime->active_nodes) {
            llam_kick_node(&node->runtime->nodes[migrate_target]);
        }
        break;
    }
    case LLAM_IO_CONTROL_REQ_CANCEL: {
        llam_io_req_t *req = op->target;

        if (req == NULL || atomic_load_explicit(&req->wait_mode, memory_order_acquire) != LLAM_IO_WAIT_MODE_INFLIGHT) {
            break;
        }
        // kqueue cancellation is represented by deleting the registered filter
        // and completing through the normal canceled request path.
        llam_darwin_req_delete(node, req);
        llam_io_complete_req(node, req, -ECANCELED, true);
        break;
    }
    default:
        break;
    }
}

/** @brief Submit all queued one-shot requests for a node. */
void llam_darwin_process_submissions(llam_node_t *node) {
    llam_io_req_t *reqs = llam_take_node_submissions(node);

    while (reqs != NULL) {
        llam_io_req_t *next = reqs->next;

        reqs->next = NULL;
        llam_darwin_submit_req(node, reqs);
        reqs = next;
    }
}

/** @brief Queue deactivation controls for all active watches during shutdown. */
void llam_darwin_queue_shutdown_controls(llam_node_t *node) {
    llam_io_queue_shutdown_controls_common(node);
}

/**
 * @brief Main loop for a Darwin I/O node worker.
 *
 * @param arg Pointer to an ::llam_node_t.
 * @return Always NULL.
 */
void *llam_io_worker_main(void *arg) {
    llam_node_t *node = arg;
    llam_runtime_t *rt = node->runtime;
    bool thread_counted =
        llam_runtime_native_thread_enter(rt, &rt->io_threads_live);

    if (!thread_counted) {
        return NULL;
    }
    llam_tune_io_worker_thread(node);

    for (;;) {
        struct kevent events[LLAM_DARWIN_KEVENT_BATCH];
        struct timespec ts;
        struct timespec *ts_ptr = NULL;
        unsigned pending;
        int count;
        unsigned i;

        if (atomic_load(&rt->stop_requested)) {
            llam_darwin_queue_shutdown_controls(node);
        }

        {
            llam_io_control_op_t *controls = llam_take_node_controls(node);

            while (controls != NULL) {
                llam_io_control_op_t *next = controls->next;
                bool guards_fd = llam_darwin_control_guards_fd_lifecycle(controls);

                controls->next = NULL;
                if (guards_fd) {
                    llam_fd_watch_lifecycle_lock();
                }
                llam_darwin_process_control(node, controls);
                if (guards_fd) {
                    llam_fd_watch_lifecycle_unlock();
                }
                llam_io_control_op_destroy(node, controls);
                controls = next;
            }
        }
        llam_fd_watch_lifecycle_lock();
        llam_darwin_process_submissions(node);
        llam_fd_watch_lifecycle_unlock();

        pending = atomic_load(&node->pending_ops);
        if (atomic_load_explicit(&rt->shutdown_requested, memory_order_acquire) &&
            pending == 0U) {
            break;
        }

        if (pending == 0U) {
            ts.tv_sec = 0;
            ts.tv_nsec = 1000000L;
            ts_ptr = &ts;
        } else {
            // Pending backend work can block indefinitely; wake controls use
            // EVFILT_USER and Darwin EVFILT_MACHPORT to interrupt this wait.
            ts_ptr = NULL;
        }

        do {
            count = kevent(node->event_fd, NULL, 0, events, (int)LLAM_DARWIN_KEVENT_BATCH, ts_ptr);
        } while (count < 0 && errno == EINTR);
        if (count < 0) {
            /*
             * BSD kqueue implementations can transiently report no kernel
             * resources while LLAM rapidly tears down and recreates runtime
             * workers in lifecycle race tests. Treat that like an empty poll
             * iteration instead of poisoning the runtime with a fatal error;
             * back off briefly so a persistent kernel pressure condition does
             * not turn the I/O worker into a spin loop.
             */
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct timespec retry_ts;

                retry_ts.tv_sec = 0;
                retry_ts.tv_nsec = 1000000L;
                (void)nanosleep(&retry_ts, NULL);
                continue;
            }
            llam_record_fatal(rt, errno);
            break;
        }

        llam_darwin_pin_event_batch(node, events, (unsigned)count);
        for (i = 0; i < (unsigned)count; ++i) {
            uint64_t user_data;
            unsigned tag;

            if (llam_kqueue_is_worker_wake_event(&events[i])) {
                llam_drain_node_wake(node);
                continue;
            }
            user_data = (uint64_t)(uintptr_t)events[i].udata;
            tag = llam_io_udata_tag(user_data);

            // Tags mirror the Linux user_data scheme so higher-level completion
            // code can stay backend-agnostic.
            if (tag == LLAM_IO_UDATA_POLL_WATCH) {
                llam_darwin_handle_poll_watch_event(node,
                                                  llam_io_udata_ptr(user_data),
                                                  llam_darwin_poll_revents(&events[i]));
            } else if (tag == LLAM_IO_UDATA_ACCEPT_WATCH) {
                llam_darwin_handle_accept_watch_event(node, llam_io_udata_ptr(user_data));
            } else if (tag == LLAM_IO_UDATA_RECV_WATCH) {
                llam_darwin_handle_recv_watch_event(node, llam_io_udata_ptr(user_data));
            } else if (tag == LLAM_IO_UDATA_REQ) {
                llam_darwin_handle_req_event(node, llam_io_udata_ptr(user_data), &events[i]);
            }
        }
        llam_darwin_unpin_event_batch(node, events, (unsigned)count);
    }

    llam_runtime_native_thread_exit(rt, &rt->io_threads_live);
    return NULL;
}
