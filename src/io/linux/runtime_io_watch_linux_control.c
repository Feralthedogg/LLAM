/**
 * @file src/io/linux/runtime_io_watch_linux_control.c
 * @brief Linux io_uring control-operation submission and watch cancellation helpers.
 *
 * @details
 * Control operations serialize backend mutations that cannot be performed
 * directly from API threads: multishot watch activation/deactivation and
 * cancellation of in-flight requests. Operations are queued under watch_lock and
 * consumed by the node worker when preparing io_uring SQEs.
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
 * @brief Enqueue a control operation while watch_lock is held.
 *
 * @param node   Target node.
 * @param kind   Control operation kind.
 * @param target Watch or request pointer consumed by the worker.
 * @return 0 on success, -1 on allocation failure.
 */
int llam_node_queue_control_locked(llam_node_t *node, llam_io_control_kind_t kind, void *target) {
    llam_io_control_op_t *op = calloc(1, sizeof(*op));

    if (op == NULL) {
        return -1;
    }

    op->kind = kind;
    op->target = target;
    if (node->control_tail != NULL) {
        node->control_tail->next = op;
    } else {
        node->control_head = op;
    }
    node->control_tail = op;
    return 0;
}

/**
 * @brief Enqueue a control operation and wake the node worker.
 *
 * @param node   Target node.
 * @param kind   Control operation kind.
 * @param target Watch or request pointer consumed by the worker.
 * @return 0 on success, -1 on allocation failure.
 */
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

/**
 * @brief Detach all queued control operations from a node.
 *
 * @param node Node whose control queue should be drained.
 * @return Head of detached control list.
 */
llam_io_control_op_t *llam_take_node_controls(llam_node_t *node) {
    llam_io_control_op_t *head;

    pthread_mutex_lock(&node->watch_lock);
    head = node->control_head;
    node->control_head = NULL;
    node->control_tail = NULL;
    pthread_mutex_unlock(&node->watch_lock);
    return head;
}

/**
 * @brief Complete a single Linux I/O request and wake its task.
 *
 * @param node              Node that observed the completion.
 * @param req               Completed request.
 * @param res               io_uring result value.
 * @param cqe_flags         io_uring CQE flags.
 * @param decrement_pending Whether to decrement node pending operation count.
 */
void llam_io_complete_req(llam_node_t *node, llam_io_req_t *req, int res, unsigned cqe_flags, bool decrement_pending) {
    llam_io_abort_reason_t abort_reason;
    llam_wait_reason_t wake_reason = LLAM_WAIT_IO;
    unsigned inflight_owner = UINT_MAX;
    unsigned completion_owner = UINT_MAX;
    unsigned wait_mode;

    if (decrement_pending) {
        atomic_fetch_sub(&node->pending_ops, 1U);
    }
    wait_mode = atomic_load_explicit(&req->wait_mode, memory_order_acquire);
    if (wait_mode == LLAM_IO_WAIT_MODE_INFLIGHT) {
        /*
         * Clear in-flight ownership exactly once so shard pressure accounting
         * is balanced with llam_take_node_submissions().  The exchanged owner
         * is also the authoritative completion target because dynamic rehome
         * can update req->owner_shard concurrently after transferring this
         * atomic owner.
         */
        inflight_owner = atomic_exchange_explicit(&req->inflight_owner_shard, UINT_MAX, memory_order_acq_rel);
        if (inflight_owner < node->runtime->active_shards) {
            llam_shard_note_inflight_io_waiter(req->owner_runtime, inflight_owner, -1);
            completion_owner = inflight_owner;
        }
    } else {
        atomic_store_explicit(&req->inflight_owner_shard, UINT_MAX, memory_order_release);
        completion_owner = req->owner_shard;
    }
    abort_reason = (llam_io_abort_reason_t)atomic_exchange(&req->abort_reason, LLAM_IO_ABORT_NONE);
    atomic_store(&req->wait_mode, LLAM_IO_WAIT_MODE_NONE);
    atomic_store(&req->cancel_queued, 0U);
    req->poll_watch = NULL;
    req->accept_watch = NULL;
    req->recv_watch = NULL;

    if (res >= 0) {
        if (req->use_provided_buffer) {
            if ((cqe_flags & IORING_CQE_F_BUFFER) != 0U && req->owned_buffer != NULL && node->recv_buf_storage != NULL) {
                unsigned short bid = (unsigned short)(cqe_flags >> IORING_CQE_BUFFER_SHIFT);

                if (bid < node->recv_buf_entries) {
                    // Attach the provided buffer directly to the owned-buffer
                    // wrapper. It will be recycled when the user releases it.
                    req->owned_buffer->provided_storage = true;
                    req->owned_buffer->provided_node_index = node->index;
                    req->owned_buffer->provided_bid = bid;
                    req->owned_buffer->data =
                        node->recv_buf_storage + ((size_t)bid * LLAM_IO_BUFFER_INLINE_BYTES);
                    req->owned_buffer->capacity = LLAM_IO_BUFFER_INLINE_BYTES;
                    req->owned_buffer->size = (size_t)res;
                    req->provided_bid = bid;
                    atomic_fetch_add_explicit(&node->provided_buf_acquires, 1U, memory_order_relaxed);
                    req->result = res;
                    req->error_code = 0;
                    req->poll_revents = 0;
                } else {
                    req->result = -1;
                    req->error_code = EIO;
                    req->poll_revents = 0;
                }
            } else {
                req->result = -1;
                req->error_code = EIO;
                req->poll_revents = 0;
            }
        } else {
            req->result = req->kind == LLAM_IO_KIND_POLL ? (res != 0 ? 1 : 0) : res;
            req->error_code = 0;
            req->poll_revents = req->kind == LLAM_IO_KIND_POLL ? (short)res : 0;
            if (req->owned_buffer != NULL && req->kind == LLAM_IO_KIND_READ) {
                req->owned_buffer->size = (size_t)res;
            }
        }
    } else {
        if (res == -ECANCELED && abort_reason != LLAM_IO_ABORT_NONE) {
            llam_io_set_abort_result(req, abort_reason);
            wake_reason = llam_io_abort_wait_reason(abort_reason);
        } else {
            req->result = -1;
            req->error_code = -res;
            req->poll_revents = 0;
        }
    }
    if (completion_owner < node->runtime->active_shards) {
        llam_shard_t *shard = &node->runtime->shards[completion_owner];
        uint64_t now_ns = llam_now_ns();

        pthread_mutex_lock(&shard->lock);
        if (wake_reason == LLAM_WAIT_CANCEL) {
            shard->metrics.cancel_wakes += 1U;
        } else if (wake_reason == LLAM_WAIT_TIMEOUT) {
            shard->metrics.timeout_wakes += 1U;
        }
        if (now_ns >= req->submit_ts_ns) {
            shard->metrics.io_completion_latency_ns += now_ns - req->submit_ts_ns;
            shard->metrics.io_completion_samples += 1U;
        }
        pthread_mutex_unlock(&shard->lock);
    }
    llam_reinject_task_on_shard(node->runtime,
                              req->task,
                              completion_owner,
                              true,
                              LLAM_TRACE_IO_COMPLETE,
                              wake_reason);
}

/** @brief Drop an unsubmitted control op without leaving synthetic queue state behind. */
static void llam_io_fail_control_op(llam_node_t *node, llam_io_control_op_t *op) {
    llam_io_req_t *waiters = NULL;
    int error = -EAGAIN;

    if (op == NULL) {
        return;
    }

    pthread_mutex_lock(&node->watch_lock);
    switch (op->kind) {
    case LLAM_IO_CONTROL_POLL_ACTIVATE: {
        llam_poll_watch_t *watch = op->target;

        if (watch != NULL) {
            watch->activating = false;
            waiters = llam_poll_watch_take_waiters(watch);
            watch->sticky_revents = 0;
        }
        break;
    }
    case LLAM_IO_CONTROL_ACCEPT_ACTIVATE: {
        llam_accept_watch_t *watch = op->target;

        if (watch != NULL) {
            watch->activating = false;
            waiters = watch->wait_head;
            watch->wait_head = NULL;
            watch->wait_tail = NULL;
        }
        break;
    }
    case LLAM_IO_CONTROL_RECV_ACTIVATE: {
        llam_recv_watch_t *watch = op->target;

        if (watch != NULL) {
            watch->activating = false;
            waiters = watch->wait_head;
            watch->wait_head = NULL;
            watch->wait_tail = NULL;
            llam_maybe_destroy_recv_watch_locked(node, watch);
        }
        break;
    }
    case LLAM_IO_CONTROL_POLL_DEACTIVATE: {
        llam_poll_watch_t *watch = op->target;

        if (watch != NULL) {
            watch->deactivate_queued = false;
        }
        break;
    }
    case LLAM_IO_CONTROL_ACCEPT_DEACTIVATE: {
        llam_accept_watch_t *watch = op->target;

        if (watch != NULL) {
            watch->deactivate_queued = false;
        }
        break;
    }
    case LLAM_IO_CONTROL_RECV_DEACTIVATE: {
        llam_recv_watch_t *watch = op->target;

        if (watch != NULL) {
            watch->deactivate_queued = false;
            llam_maybe_destroy_recv_watch_locked(node, watch);
        }
        break;
    }
    case LLAM_IO_CONTROL_REQ_CANCEL: {
        llam_io_req_t *req = op->target;

        if (req != NULL) {
            /*
             * The request remains backend-owned.  Roll back only the queued bit
             * so a later timeout/cancel pass can retry backend cancellation.
             */
            atomic_store_explicit(&req->cancel_queued, 0U, memory_order_release);
        }
        break;
    }
    default:
        break;
    }
    pthread_mutex_unlock(&node->watch_lock);

    while (waiters != NULL) {
        llam_io_req_t *next = waiters->next;

        waiters->next = NULL;
        llam_io_complete_req(node, waiters, error, 0U, false);
        waiters = next;
    }
    free(op);
}

/**
 * @brief Validate a queued control op before consuming an SQE for it.
 *
 * All current Linux control operations require a concrete target object.  Drop
 * malformed control records locally so a defensive failure path cannot leave an
 * uninitialized SQE pending in the ring.
 */
static bool llam_io_control_op_ready(const llam_io_control_op_t *op) {
    if (op == NULL || op->target == NULL) {
        return false;
    }

    switch (op->kind) {
    case LLAM_IO_CONTROL_POLL_ACTIVATE:
    case LLAM_IO_CONTROL_POLL_DEACTIVATE:
    case LLAM_IO_CONTROL_ACCEPT_ACTIVATE:
    case LLAM_IO_CONTROL_ACCEPT_DEACTIVATE:
    case LLAM_IO_CONTROL_RECV_ACTIVATE:
    case LLAM_IO_CONTROL_RECV_DEACTIVATE:
    case LLAM_IO_CONTROL_REQ_CANCEL:
        return true;
    default:
        return false;
    }
}

/**
 * @brief Prepare one queued Linux control operation as an io_uring SQE.
 *
 * @param node Node whose ring receives the SQE.
 * @param op   Control operation; ownership is consumed by this function or by
 *             the resulting control completion.
 */
void llam_io_submit_control_op(llam_node_t *node, llam_io_control_op_t *op) {
    struct io_uring_sqe *sqe;

    if (!llam_io_control_op_ready(op)) {
        llam_io_fail_control_op(node, op);
        return;
    }

    sqe = io_uring_get_sqe(&node->ring);

    if (sqe == NULL) {
        int rc = llam_node_submit_ring(node);
        if (rc < 0) {
            llam_record_fatal(node->runtime, -rc);
            llam_io_fail_control_op(node, op);
            return;
        }
        sqe = io_uring_get_sqe(&node->ring);
    }
    if (sqe == NULL) {
        llam_io_fail_control_op(node, op);
        return;
    }

    switch (op->kind) {
    case LLAM_IO_CONTROL_POLL_ACTIVATE: {
        llam_poll_watch_t *watch = op->target;

        io_uring_prep_poll_multishot(sqe, watch->fd, (unsigned)watch->events);
        io_uring_sqe_set_data64(sqe, llam_io_udata_encode(watch, LLAM_IO_UDATA_POLL_WATCH));
        pthread_mutex_lock(&node->watch_lock);
        // Mark active before submission is visible so a racing deactivation can
        // issue a remove/cancel operation instead of losing the watch.
        watch->active = true;
        watch->activating = false;
        watch->deactivate_queued = false;
        pthread_mutex_unlock(&node->watch_lock);
        atomic_fetch_add(&node->pending_ops, 1U);
        free(op);
        return;
    }
    case LLAM_IO_CONTROL_POLL_DEACTIVATE: {
        llam_poll_watch_t *watch = op->target;

        io_uring_prep_poll_remove(sqe, llam_io_udata_encode(watch, LLAM_IO_UDATA_POLL_WATCH));
        io_uring_sqe_set_data64(sqe, llam_io_udata_encode(op, LLAM_IO_UDATA_CONTROL));
        return;
    }
    case LLAM_IO_CONTROL_ACCEPT_ACTIVATE: {
        llam_accept_watch_t *watch = op->target;

        io_uring_prep_multishot_accept(sqe, watch->fd, NULL, NULL, 0);
        io_uring_sqe_set_data64(sqe, llam_io_udata_encode(watch, LLAM_IO_UDATA_ACCEPT_WATCH));
        pthread_mutex_lock(&node->watch_lock);
        watch->active = true;
        watch->activating = false;
        watch->deactivate_queued = false;
        pthread_mutex_unlock(&node->watch_lock);
        atomic_fetch_add(&node->pending_ops, 1U);
        free(op);
        return;
    }
    case LLAM_IO_CONTROL_ACCEPT_DEACTIVATE: {
        llam_accept_watch_t *watch = op->target;

        io_uring_prep_cancel64(sqe, llam_io_udata_encode(watch, LLAM_IO_UDATA_ACCEPT_WATCH), 0);
        io_uring_sqe_set_data64(sqe, llam_io_udata_encode(op, LLAM_IO_UDATA_CONTROL));
        return;
    }
    case LLAM_IO_CONTROL_RECV_ACTIVATE: {
        llam_recv_watch_t *watch = op->target;

        io_uring_prep_recv_multishot(sqe, watch->fd, NULL, LLAM_IO_BUFFER_INLINE_BYTES, 0);
        llam_io_uring_sqe_set_buf_group_compat(sqe, node->recv_buf_group);
        // Multishot recv uses io_uring provided buffers so completions can hand
        // ownership to an llam_io_buffer_t without copying.
        io_uring_sqe_set_flags(sqe, IOSQE_BUFFER_SELECT);
        io_uring_sqe_set_data64(sqe, llam_io_udata_encode(watch, LLAM_IO_UDATA_RECV_WATCH));
        pthread_mutex_lock(&node->watch_lock);
        watch->active = true;
        watch->activating = false;
        watch->deactivate_queued = false;
        pthread_mutex_unlock(&node->watch_lock);
        atomic_fetch_add(&node->pending_ops, 1U);
        free(op);
        return;
    }
    case LLAM_IO_CONTROL_RECV_DEACTIVATE: {
        llam_recv_watch_t *watch = op->target;

        io_uring_prep_cancel64(sqe, llam_io_udata_encode(watch, LLAM_IO_UDATA_RECV_WATCH), 0);
        io_uring_sqe_set_data64(sqe, llam_io_udata_encode(op, LLAM_IO_UDATA_CONTROL));
        return;
    }
    case LLAM_IO_CONTROL_REQ_CANCEL: {
        llam_io_req_t *req = op->target;

        // Request cancellation targets the encoded request user-data used by the
        // original SQE.
        io_uring_prep_cancel64(sqe, llam_io_udata_encode(req, LLAM_IO_UDATA_REQ), 0);
        io_uring_sqe_set_data64(sqe, llam_io_udata_encode(op, LLAM_IO_UDATA_CONTROL));
        return;
    }
    default:
        free(op);
        return;
    }
}
