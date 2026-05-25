/**
 * @file src/io/darwin/runtime_io_watch_darwin_control.c
 * @brief Darwin/kqueue control-operation registration and cancellation helpers.
 *
 * @details
 * Darwin control operations turn watch/request state changes into kevent
 * registrations and deletions. API threads enqueue control nodes; the I/O node
 * worker applies them so kqueue state is mutated from one place.
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

/** @brief Enqueue a control operation and wake the node worker. */
int llam_node_queue_control(llam_node_t *node, llam_io_control_kind_t kind, void *target) {
    int rc;

    pthread_mutex_lock(&node->watch_lock);
    rc = llam_node_queue_control_locked(node, kind, target);
    pthread_mutex_unlock(&node->watch_lock);
    if (rc == 0) {
        llam_kick_node(node);
    }
    return rc;
}

/** @brief Pop buffered receive readiness, optionally transferring copy ownership. */
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

/** @brief Detach all queued control operations from a node. */
llam_io_control_op_t *llam_take_node_controls(llam_node_t *node) {
    llam_io_control_op_t *head;

    pthread_mutex_lock(&node->watch_lock);
    head = node->control_head;
    node->control_head = NULL;
    node->control_tail = NULL;
    pthread_mutex_unlock(&node->watch_lock);
    return head;
}

/** @brief Apply one or more kevent changes, retrying EINTR. */
int llam_darwin_kevent_apply(llam_node_t *node, struct kevent *changes, int change_count) {
    int rc;

    do {
        rc = kevent(node->event_fd, changes, change_count, NULL, 0, NULL);
    } while (rc < 0 && errno == EINTR);
    return rc;
}

/** @brief Add/delete kqueue filters for a poll watch. */
int llam_darwin_poll_watch_change(llam_node_t *node, llam_poll_watch_t *watch, uint16_t flags) {
    struct kevent changes[2];
    int change_count = 0;

    if ((watch->events & (POLLIN | POLLPRI)) != 0) {
        EV_SET(&changes[change_count++],
               (uintptr_t)watch->fd,
               EVFILT_READ,
               flags,
               0U,
               0,
               (void *)(uintptr_t)llam_io_udata_encode(watch, LLAM_IO_UDATA_POLL_WATCH));
    }
    if ((watch->events & POLLOUT) != 0) {
        EV_SET(&changes[change_count++],
               (uintptr_t)watch->fd,
               EVFILT_WRITE,
               flags,
               0U,
               0,
               (void *)(uintptr_t)llam_io_udata_encode(watch, LLAM_IO_UDATA_POLL_WATCH));
    }
    if (change_count == 0) {
        errno = EINVAL;
        return -1;
    }
    return llam_darwin_kevent_apply(node, changes, change_count);
}

/** @brief Add/delete the kqueue filter for an accept watch. */
int llam_darwin_accept_watch_change(llam_node_t *node, llam_accept_watch_t *watch, uint16_t flags) {
    struct kevent change;

    EV_SET(&change,
           (uintptr_t)watch->fd,
           EVFILT_READ,
           flags,
           0U,
           0,
           (void *)(uintptr_t)llam_io_udata_encode(watch, LLAM_IO_UDATA_ACCEPT_WATCH));
    return llam_darwin_kevent_apply(node, &change, 1);
}

/** @brief Add/delete the kqueue filter for a receive watch. */
int llam_darwin_recv_watch_change(llam_node_t *node, llam_recv_watch_t *watch, uint16_t flags) {
    struct kevent change;

    EV_SET(&change,
           (uintptr_t)watch->fd,
           EVFILT_READ,
           flags,
           0U,
           0,
           (void *)(uintptr_t)llam_io_udata_encode(watch, LLAM_IO_UDATA_RECV_WATCH));
    return llam_darwin_kevent_apply(node, &change, 1);
}

/** @brief Apply one kqueue filter change for a one-shot request. */
int llam_darwin_req_change_one(llam_node_t *node, llam_io_req_t *req, int16_t filter, uint16_t flags) {
    struct kevent change;

    EV_SET(&change,
           (uintptr_t)req->fd,
           filter,
           (uint16_t)(flags | EV_UDATA_SPECIFIC),
           0U,
           0,
           (void *)(uintptr_t)llam_io_udata_encode(req, LLAM_IO_UDATA_REQ));
    return llam_darwin_kevent_apply(node, &change, 1);
}

/** @brief Register kqueue readiness for a one-shot request. */
int llam_darwin_req_register(llam_node_t *node, llam_io_req_t *req) {
    int rc = 0;

    switch (req->kind) {
    case LLAM_IO_KIND_READ:
    case LLAM_IO_KIND_ACCEPT:
        rc = llam_darwin_req_change_one(node, req, EVFILT_READ, EV_ADD | EV_ONESHOT | EV_CLEAR);
        break;
    case LLAM_IO_KIND_WRITE:
    case LLAM_IO_KIND_CONNECT:
        rc = llam_darwin_req_change_one(node, req, EVFILT_WRITE, EV_ADD | EV_ONESHOT | EV_CLEAR);
        break;
    case LLAM_IO_KIND_POLL:
        if ((req->poll_events & (POLLIN | POLLPRI)) != 0) {
            rc = llam_darwin_req_change_one(node, req, EVFILT_READ, EV_ADD | EV_ONESHOT | EV_CLEAR);
            if (rc != 0) {
                return -1;
            }
        }
        if ((req->poll_events & POLLOUT) != 0) {
            rc = llam_darwin_req_change_one(node, req, EVFILT_WRITE, EV_ADD | EV_ONESHOT | EV_CLEAR);
            if (rc != 0) {
                return -1;
            }
        }
        if ((req->poll_events & (POLLIN | POLLPRI | POLLOUT)) == 0) {
            errno = EINVAL;
            return -1;
        }
        break;
    default:
        errno = EOPNOTSUPP;
        return -1;
    }
    return rc;
}

/** @brief Delete kqueue readiness entries for a one-shot request. */
void llam_darwin_req_delete(llam_node_t *node, llam_io_req_t *req) {
    int saved_errno = errno;

    // Preserve caller errno because delete is often best-effort cleanup after an
    // operation has already produced its real result.
    switch (req->kind) {
    case LLAM_IO_KIND_READ:
    case LLAM_IO_KIND_ACCEPT:
        if (llam_darwin_req_change_one(node, req, EVFILT_READ, EV_DELETE) != 0 && errno != ENOENT) {
            llam_record_fatal(node->runtime, errno);
        }
        break;
    case LLAM_IO_KIND_WRITE:
    case LLAM_IO_KIND_CONNECT:
        if (llam_darwin_req_change_one(node, req, EVFILT_WRITE, EV_DELETE) != 0 && errno != ENOENT) {
            llam_record_fatal(node->runtime, errno);
        }
        break;
    case LLAM_IO_KIND_POLL:
        if ((req->poll_events & (POLLIN | POLLPRI)) != 0 &&
            llam_darwin_req_change_one(node, req, EVFILT_READ, EV_DELETE) != 0 &&
            errno != ENOENT) {
            llam_record_fatal(node->runtime, errno);
        }
        if ((req->poll_events & POLLOUT) != 0 &&
            llam_darwin_req_change_one(node, req, EVFILT_WRITE, EV_DELETE) != 0 &&
            errno != ENOENT) {
            llam_record_fatal(node->runtime, errno);
        }
        break;
    default:
        break;
    }
    errno = saved_errno;
}
