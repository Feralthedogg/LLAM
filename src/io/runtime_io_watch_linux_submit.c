/**
 * @file src/io/runtime_io_watch_linux_submit.c
 * @brief Linux io_uring SQE preparation and batch submission logic.
 *
 * @details
 * The Linux I/O worker drains control operations and request submissions into
 * the node ring in batches. Individual SQE preparation is kept in this file so
 * backend feature choices, provided-buffer selection, and unsupported-kind
 * completion stay localized.
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
 * @brief Prepare one io_uring SQE for a runtime I/O request.
 *
 * If the submission queue is full, the current ring is submitted once and the
 * SQE acquisition is retried. Permanent SQE unavailability completes the request
 * with @c -EAGAIN so the caller's wake path remains uniform.
 *
 * @param node Node owning the io_uring ring.
 * @param req  Request to submit.
 */
void nm_io_submit_one(nm_node_t *node, nm_io_req_t *req) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&node->ring);

    if (sqe == NULL) {
        int rc = nm_node_submit_ring(node);
        if (rc < 0) {
            nm_io_complete_req(node, req, rc, 0U, true);
            return;
        }
        sqe = io_uring_get_sqe(&node->ring);
    }

    if (sqe == NULL) {
        nm_io_complete_req(node, req, -EAGAIN, 0U, true);
        return;
    }

    switch (req->kind) {
    case NM_IO_KIND_READ:
        if (req->use_provided_buffer && node->supports_provided_buffers && req->count <= NM_IO_BUFFER_INLINE_BYTES) {
            io_uring_prep_recv(sqe, req->fd, NULL, (unsigned)req->count, req->recv_flags);
            nm_io_uring_sqe_set_buf_group_compat(sqe, node->recv_buf_group);
            io_uring_sqe_set_flags(sqe, IOSQE_BUFFER_SELECT);
        } else if (req->use_recv_op) {
            io_uring_prep_recv(sqe, req->fd, req->buf, (unsigned)req->count, req->recv_flags);
        } else {
            io_uring_prep_read(sqe, req->fd, req->buf, (unsigned)req->count, (unsigned long long)-1);
        }
        break;
    case NM_IO_KIND_WRITE:
        io_uring_prep_write(sqe, req->fd, req->buf, (unsigned)req->count, (unsigned long long)-1);
        break;
    case NM_IO_KIND_ACCEPT:
        io_uring_prep_accept(sqe, req->fd, req->addr, req->addrlen, 0);
        break;
    case NM_IO_KIND_CONNECT:
        io_uring_prep_connect(sqe, req->fd, req->addr, req->addr_len);
        break;
    case NM_IO_KIND_POLL:
        io_uring_prep_poll_add(sqe, req->fd, (unsigned)req->poll_events);
        break;
    default:
        node->unsupported_ops += 1U;
        nm_io_complete_req(node, req, -EINVAL, 0U, true);
        return;
    }

    io_uring_sqe_set_data64(sqe, nm_io_udata_encode(req, NM_IO_UDATA_REQ));
}

/**
 * @brief Queue deactivation controls for all active watches during shutdown.
 *
 * The worker uses this when runtime stop is requested so multishot poll,
 * accept, and recv watches are asked to deactivate before the node exits.
 *
 * @param node Node whose watch lists should be drained.
 */
void nm_io_queue_shutdown_controls(nm_node_t *node) {
    bool kicked = false;

    pthread_mutex_lock(&node->watch_lock);
    for (nm_poll_watch_t *watch = node->poll_watches; watch != NULL; watch = watch->next) {
        if (watch->active && !watch->deactivate_queued) {
            watch->deactivate_queued = true;
            if (nm_node_queue_control_locked(node, NM_IO_CONTROL_POLL_DEACTIVATE, watch) == 0) {
                kicked = true;
            }
        }
    }
    for (nm_accept_watch_t *watch = node->accept_watches; watch != NULL; watch = watch->next) {
        if (watch->active && !watch->deactivate_queued) {
            watch->deactivate_queued = true;
            if (nm_node_queue_control_locked(node, NM_IO_CONTROL_ACCEPT_DEACTIVATE, watch) == 0) {
                kicked = true;
            }
        }
    }
    for (nm_recv_watch_t *watch = node->recv_watches; watch != NULL; watch = watch->next) {
        if (watch->active && !watch->deactivate_queued) {
            watch->deactivate_queued = true;
            if (nm_node_queue_control_locked(node, NM_IO_CONTROL_RECV_DEACTIVATE, watch) == 0) {
                kicked = true;
            }
        }
    }
    pthread_mutex_unlock(&node->watch_lock);
    if (kicked) {
        nm_kick_node(node);
    }
}

/**
 * @brief Submit all pending node control operations and I/O requests.
 *
 * Controls are submitted before regular requests so watch deactivation and
 * migration state changes take effect promptly. Metrics record batch count,
 * total entries, and maximum observed batch size.
 *
 * @param node Node whose pending queues should be flushed into the ring.
 */
void nm_io_submit_batch(nm_node_t *node) {
    nm_io_control_op_t *controls = nm_take_node_controls(node);
    nm_io_req_t *reqs = nm_take_node_submissions(node);
    unsigned submitted = 0U;

    while (controls != NULL) {
        nm_io_control_op_t *next = controls->next;

        controls->next = NULL;
        nm_io_submit_control_op(node, controls);
        controls = next;
        submitted += 1U;
    }

    while (reqs != NULL) {
        nm_io_req_t *next = reqs->next;

        reqs->next = NULL;
        nm_io_submit_one(node, reqs);
        reqs = next;
        submitted += 1U;
    }

    if (submitted > 0U) {
        int rc = nm_node_submit_ring(node);
        if (rc < 0) {
            nm_record_fatal(node->runtime, -rc);
        }
        node->submit_batches += 1U;
        node->submit_entries += submitted;
        if (submitted > node->max_submit_batch) {
            node->max_submit_batch = submitted;
        }
    }
}
