/**
 * @file src/io/linux/runtime_io_watch_linux_submit.c
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

/** @brief Return true for request kinds this backend can encode as an SQE. */
static bool llam_linux_io_req_kind_supported(llam_io_kind_t kind) {
    switch (kind) {
    case LLAM_IO_KIND_READ:
    case LLAM_IO_KIND_WRITE:
    case LLAM_IO_KIND_PREAD:
    case LLAM_IO_KIND_PWRITE:
    case LLAM_IO_KIND_ACCEPT:
    case LLAM_IO_KIND_CONNECT:
    case LLAM_IO_KIND_POLL:
        return true;
    default:
        return false;
    }
}

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
void llam_io_submit_one(llam_node_t *node, llam_io_req_t *req) {
    llam_io_kind_t kind = req->kind;
    struct io_uring_sqe *sqe;

    if (!llam_linux_io_req_kind_supported(kind)) {
        /*
         * Unsupported requests complete locally.  Reject before SQE acquisition
         * so the ring tail never advances for an entry that will not be
         * initialized below.
         */
        atomic_fetch_add_explicit(&node->unsupported_ops, 1U, memory_order_relaxed);
        llam_io_complete_req(node, req, -EINVAL, 0U, true);
        return;
    }

    if ((kind == LLAM_IO_KIND_READ ||
         kind == LLAM_IO_KIND_WRITE ||
         kind == LLAM_IO_KIND_PREAD ||
         kind == LLAM_IO_KIND_PWRITE) &&
        req->count > (size_t)UINT_MAX) {
        /*
         * io_uring read/write SQEs carry length as unsigned int.  The public
         * API normally rejects this before parking, but keep the backend guard
         * so internal or future request paths cannot silently truncate.  This
         * must run before io_uring_get_sqe(): consuming an SQE and then
         * completing locally leaves an uninitialized entry visible to the next
         * ring submission.
         */
        llam_io_complete_req(node, req, -EINVAL, 0U, true);
        return;
    }
    if (llam_io_req_abort_requested(req)) {
        /*
         * A cancel/timeout can arrive after the node worker detached this
         * request from submit_lock but before an SQE exists.  Finish it here so
         * no later cancel SQE is needed to release the parked task.
         */
        llam_io_complete_req(node, req, -ECANCELED, 0U, true);
        return;
    }

    sqe = io_uring_get_sqe(&node->ring);

    if (sqe == NULL) {
        int rc = llam_node_submit_ring(node);
        if (rc < 0) {
            llam_io_complete_req(node, req, rc, 0U, true);
            return;
        }
        sqe = io_uring_get_sqe(&node->ring);
    }

    if (sqe == NULL) {
        llam_io_complete_req(node, req, -EAGAIN, 0U, true);
        return;
    }

    switch (kind) {
    case LLAM_IO_KIND_READ:
        if (req->use_provided_buffer && node->supports_provided_buffers && req->count <= LLAM_IO_BUFFER_INLINE_BYTES) {
            io_uring_prep_recv(sqe, req->fd, NULL, (unsigned)req->count, req->recv_flags);
            llam_io_uring_sqe_set_buf_group_compat(sqe, node->recv_buf_group);
            io_uring_sqe_set_flags(sqe, IOSQE_BUFFER_SELECT);
        } else if (req->use_recv_op) {
            io_uring_prep_recv(sqe, req->fd, req->buf, (unsigned)req->count, req->recv_flags);
        } else {
            io_uring_prep_read(sqe, req->fd, req->buf, (unsigned)req->count, (unsigned long long)-1);
        }
        break;
    case LLAM_IO_KIND_WRITE:
        io_uring_prep_write(sqe, req->fd, req->buf, (unsigned)req->count, (unsigned long long)-1);
        break;
    case LLAM_IO_KIND_PREAD:
        io_uring_prep_read(sqe, req->fd, req->buf, (unsigned)req->count, req->offset);
        break;
    case LLAM_IO_KIND_PWRITE:
        io_uring_prep_write(sqe, req->fd, req->buf, (unsigned)req->count, req->offset);
        break;
    case LLAM_IO_KIND_ACCEPT:
        io_uring_prep_accept(sqe, req->fd, req->addr, req->addrlen, 0);
        break;
    case LLAM_IO_KIND_CONNECT:
        io_uring_prep_connect(sqe, req->fd, req->addr, req->addr_len);
        break;
    case LLAM_IO_KIND_POLL:
        io_uring_prep_poll_add(sqe, req->fd, (unsigned)req->poll_events);
        break;
    default:
        return;
    }

    io_uring_sqe_set_data64(sqe, llam_io_udata_encode(req, LLAM_IO_UDATA_REQ));
}

/**
 * @brief Queue deactivation controls for all active watches during shutdown.
 *
 * The worker uses this when runtime stop is requested so multishot poll,
 * accept, and recv watches are asked to deactivate before the node exits.
 *
 * @param node Node whose watch lists should be drained.
 */
void llam_io_queue_shutdown_controls(llam_node_t *node) {
    bool kicked = false;

    pthread_mutex_lock(&node->watch_lock);
    for (llam_poll_watch_t *watch = node->poll_watches; watch != NULL; watch = watch->next) {
        if (watch->active && !watch->deactivate_queued) {
            watch->deactivate_queued = true;
            if (llam_node_queue_control_locked(node, LLAM_IO_CONTROL_POLL_DEACTIVATE, watch) == 0) {
                kicked = true;
            } else {
                // Allocation failure must not leave the watch in a permanently
                // queued-looking state; a later shutdown pass can retry.
                watch->deactivate_queued = false;
            }
        }
    }
    for (llam_accept_watch_t *watch = node->accept_watches; watch != NULL; watch = watch->next) {
        if (watch->active && !watch->deactivate_queued) {
            watch->deactivate_queued = true;
            if (llam_node_queue_control_locked(node, LLAM_IO_CONTROL_ACCEPT_DEACTIVATE, watch) == 0) {
                kicked = true;
            } else {
                watch->deactivate_queued = false;
            }
        }
    }
    for (llam_recv_watch_t *watch = node->recv_watches; watch != NULL; watch = watch->next) {
        if (watch->active && !watch->deactivate_queued) {
            watch->deactivate_queued = true;
            if (llam_node_queue_control_locked(node, LLAM_IO_CONTROL_RECV_DEACTIVATE, watch) == 0) {
                kicked = true;
            } else {
                watch->deactivate_queued = false;
            }
        }
    }
    pthread_mutex_unlock(&node->watch_lock);
    if (kicked) {
        llam_kick_node(node);
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
void llam_io_submit_batch(llam_node_t *node) {
    llam_io_control_op_t *controls = llam_take_node_controls(node);
    llam_io_req_t *reqs = llam_take_node_submissions(node);
    unsigned submitted = 0U;

    while (controls != NULL) {
        llam_io_control_op_t *next = controls->next;

        controls->next = NULL;
        llam_io_submit_control_op(node, controls);
        controls = next;
        submitted += 1U;
    }

    while (reqs != NULL) {
        llam_io_req_t *next = reqs->next;

        reqs->next = NULL;
        llam_io_submit_one(node, reqs);
        reqs = next;
        submitted += 1U;
    }

    if (submitted > 0U) {
        int rc = llam_node_submit_ring(node);
        if (rc < 0) {
            llam_record_fatal(node->runtime, -rc);
        }
        atomic_fetch_add_explicit(&node->submit_batches, 1U, memory_order_relaxed);
        atomic_fetch_add_explicit(&node->submit_entries, submitted, memory_order_relaxed);
        llam_atomic_update_peak(&node->max_submit_batch, submitted);
    }
}
