/**
 * @file src/io/linux/watch/cqe.c
 * @brief Linux io_uring completion processing and request wakeup paths.
 *
 * @details
 * Completion user data encodes either a one-shot request, a multishot watch, or
 * a control operation. Watch completions detach waiters under @c watch_lock,
 * then complete/wake tasks outside the lock to avoid running scheduler wake
 * logic while holding backend state locks.
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
 * @brief Queue a deactivate control and roll back the watch flag on failure.
 *
 * @details
 * CQE handling marks a multishot watch as "deactivation queued" before the
 * worker observes the cancellation completion. If the control allocation fails,
 * leaving that flag set would hide the watch from future deactivate attempts.
 */
static bool llam_queue_deactivate_control_locked(llam_node_t *node,
                                                 llam_io_control_kind_t kind,
                                                 void *watch,
                                                 bool *deactivate_queued) {
    *deactivate_queued = true;
    if (llam_node_queue_control_locked(node, kind, watch) == 0) {
        return true;
    }
    *deactivate_queued = false;
    return false;
}

/**
 * @brief Dispatch one io_uring completion queue entry.
 *
 * @param node Node whose ring produced the CQE.
 * @param cqe  Completion entry; consumed by this function.
 */
void llam_io_handle_cqe(llam_node_t *node, struct io_uring_cqe *cqe) {
    uint64_t user_data = cqe->user_data;
    int res = cqe->res;
    unsigned cqe_flags = cqe->flags;
    unsigned tag = llam_io_udata_tag(user_data);

    io_uring_cqe_seen(&node->ring, cqe);
    // Low bits in user_data identify the object type without needing separate
    // completion queues per operation kind.
    switch (tag) {
        case LLAM_IO_UDATA_REQ: {
            llam_io_req_t *req = llam_io_udata_ptr(user_data);

            if (req != NULL) {
                llam_io_complete_req(node, req, res, cqe_flags, true);
            }
            break;
        }
        case LLAM_IO_UDATA_POLL_WATCH: {
            llam_poll_watch_t *watch = llam_io_udata_ptr(user_data);
            llam_io_req_t *waiters = NULL;
            bool release_pending = false;
            bool queue_activate = false;
            bool kick_deactivate = false;
            unsigned live_target = UINT_MAX;
            int live_fd = -1;
            short live_events = 0;
            bool was_activating;

            if (watch == NULL) {
                break;
            }

            pthread_mutex_lock(&node->watch_lock);
            was_activating = watch->activating;
            watch->activating = false;
            if (res < 0) {
                if (res == -ECANCELED && watch->wait_head != NULL) {
                    // A cancel racing with new waiters re-arms the watch so
                    // waiters do not get stranded after a transient teardown.
                    watch->sticky_revents = 0;
                    if (watch->active) {
                        watch->active = false;
                        release_pending = true;
                    }
                    watch->deactivate_queued = false;
                    if (was_activating) {
                        watch->activating = true;
                    } else if (llam_node_queue_control_locked(node, LLAM_IO_CONTROL_POLL_ACTIVATE, watch) == 0) {
                        watch->activating = true;
                        queue_activate = true;
                    } else {
                        waiters = llam_poll_watch_take_waiters(watch);
                        res = -ENOMEM;
                    }
                } else {
                    waiters = llam_poll_watch_take_waiters(watch);
                    watch->sticky_revents = 0;
                    if (watch->active) {
                        watch->active = false;
                        release_pending = true;
                    }
                    watch->deactivate_queued = false;
                    if (res == -EINVAL || res == -EOPNOTSUPP || res == -ENOSYS) {
                        // Kernel does not support this multishot path; future
                        // calls should use the fallback issue path.
                        node->supports_multishot_poll = false;
                    }
                }
            } else {
                if ((cqe_flags & IORING_CQE_F_MORE) == 0U && watch->active) {
                    watch->active = false;
                    release_pending = true;
                }
                waiters = llam_poll_watch_take_waiters(watch);
                if (waiters == NULL) {
                    if (watch->migrate_target_node_index != UINT_MAX &&
                        watch->migrate_target_node_index != node->index) {
                        // No local waiters remain, but a target node is taking
                        // over the live watch. Forward readiness before tearing
                        // down the source watch.
                        live_target = watch->migrate_target_node_index;
                        live_fd = watch->fd;
                        live_events = watch->events;
                        if (watch->active && !watch->deactivate_queued) {
                            kick_deactivate = llam_queue_deactivate_control_locked(
                                node, LLAM_IO_CONTROL_POLL_DEACTIVATE, watch, &watch->deactivate_queued);
                        }
                    } else {
                        // Sticky readiness lets a later waiter consume an event
                        // that arrived before it parked.
                        watch->sticky_revents = (short)res;
                        if (watch->active && !watch->deactivate_queued) {
                            kick_deactivate = llam_queue_deactivate_control_locked(
                                node, LLAM_IO_CONTROL_POLL_DEACTIVATE, watch, &watch->deactivate_queued);
                        }
                    }
                }
            }
            pthread_mutex_unlock(&node->watch_lock);

            if (release_pending) {
                (void)llam_node_complete_pending_ops(node, 1U);
            }
            if (queue_activate) {
                llam_kick_node(node);
            }
            if (live_target != UINT_MAX &&
                !llam_forward_live_poll_watch_event(node, live_fd, live_events, live_target, (short)res)) {
                bool fallback_kick = false;

                pthread_mutex_lock(&node->watch_lock);
                watch = llam_find_poll_watch_locked(node, live_fd, live_events);
                if (watch != NULL && watch->wait_head == NULL) {
                    watch->sticky_revents = (short)res;
                    if (watch->active && !watch->deactivate_queued) {
                        watch->deactivate_queued = true;
                        if (llam_node_queue_control_locked(node, LLAM_IO_CONTROL_POLL_DEACTIVATE, watch) == 0) {
                            fallback_kick = true;
                        } else {
                            watch->deactivate_queued = false;
                        }
                    }
                }
                pthread_mutex_unlock(&node->watch_lock);
                if (fallback_kick) {
                    llam_kick_node(node);
                }
            }
            if (kick_deactivate) {
                llam_kick_node(node);
            }
            while (waiters != NULL) {
                llam_io_req_t *next = waiters->next;

                waiters->next = NULL;
                llam_io_complete_req(node, waiters, res, cqe_flags, false);
                waiters = next;
            }
            break;
        }
        case LLAM_IO_UDATA_ACCEPT_WATCH: {
            llam_accept_watch_t *watch = llam_io_udata_ptr(user_data);
            llam_io_req_t *waiter = NULL;
            llam_io_req_t *waiters = NULL;
            bool release_pending = false;
            bool kick_deactivate = false;
            unsigned live_target = UINT_MAX;
            int live_fd = -1;
            bool live_consumed = false;

            if (watch == NULL) {
                break;
            }

            pthread_mutex_lock(&node->watch_lock);
            watch->activating = false;
            if (res < 0) {
                // Multishot accept failed: wake all waiters with the backend
                // error and disable the feature for recoverable capability
                // failures.
                waiters = watch->wait_head;
                watch->wait_head = NULL;
                watch->wait_tail = NULL;
                if (watch->active) {
                    watch->active = false;
                    release_pending = true;
                }
                watch->deactivate_queued = false;
                if (res == -EINVAL || res == -EOPNOTSUPP || res == -ENOSYS) {
                    node->supports_multishot_accept = false;
                }
            } else {
                if ((cqe_flags & IORING_CQE_F_MORE) == 0U && watch->active) {
                    watch->active = false;
                    release_pending = true;
                }
                waiter = llam_accept_watch_pop_waiter(watch);
                if (waiter == NULL) {
                    if (watch->migrate_target_node_index != UINT_MAX &&
                        watch->migrate_target_node_index != node->index) {
                        // Accepted fd ownership must move exactly once. The
                        // forward path consumes it; fallback stores or closes it.
                        live_target = watch->migrate_target_node_index;
                        live_fd = watch->fd;
                        if (watch->active && !watch->deactivate_queued) {
                            kick_deactivate = llam_queue_deactivate_control_locked(
                                node, LLAM_IO_CONTROL_ACCEPT_DEACTIVATE, watch, &watch->deactivate_queued);
                        }
                    } else {
                        llam_accept_watch_push_ready(watch, res);
                        if (watch->active && !watch->deactivate_queued) {
                            kick_deactivate = llam_queue_deactivate_control_locked(
                                node, LLAM_IO_CONTROL_ACCEPT_DEACTIVATE, watch, &watch->deactivate_queued);
                        }
                    }
                }
            }
            pthread_mutex_unlock(&node->watch_lock);

            if (release_pending) {
                (void)llam_node_complete_pending_ops(node, 1U);
            }
            if (live_target != UINT_MAX) {
                if (llam_forward_live_accept_watch_ready(node, live_fd, live_target, res)) {
                    live_consumed = true;
                } else {
                    pthread_mutex_lock(&node->watch_lock);
                    watch = llam_find_accept_watch_locked(node, live_fd);
                    if (watch != NULL && watch->wait_head == NULL) {
                        llam_accept_watch_push_ready(watch, res);
                        live_consumed = true;
                    }
                    pthread_mutex_unlock(&node->watch_lock);
                    if (!live_consumed) {
                        close(res);
                        live_consumed = true;
                    }
                }
            }
            if (kick_deactivate) {
                llam_kick_node(node);
            }
            if (waiter != NULL) {
                llam_io_complete_req(node, waiter, res, cqe_flags, false);
            }
            while (waiters != NULL) {
                llam_io_req_t *next = waiters->next;

                waiters->next = NULL;
                llam_io_complete_req(node, waiters, res, cqe_flags, false);
                waiters = next;
            }
            break;
        }
        case LLAM_IO_UDATA_RECV_WATCH: {
            llam_recv_watch_t *watch = llam_io_udata_ptr(user_data);
            llam_io_req_t *waiter = NULL;
            llam_io_req_t *waiters = NULL;
            bool release_pending = false;
            bool kick_deactivate = false;
            bool queued_ready = false;
            bool has_buffer = false;
            unsigned short bid = 0U;
            unsigned live_target = UINT_MAX;
            unsigned live_node_index = UINT_MAX;
            int live_fd = -1;
            dev_t live_st_dev = 0;
            ino_t live_st_ino = 0;

            if (watch == NULL) {
                break;
            }

            if ((cqe_flags & IORING_CQE_F_BUFFER) != 0U) {
                bid = (unsigned short)(cqe_flags >> IORING_CQE_BUFFER_SHIFT);
                has_buffer = bid < node->recv_buf_entries;
                if (has_buffer) {
                    atomic_fetch_add_explicit(&node->provided_buf_acquires, 1U, memory_order_relaxed);
                }
            }
            if (res == -ENOBUFS) {
                /*
                 * recv_multishot depends on provided buffers. Some kernels can
                 * accept the SQE and later report that no buffer is available;
                 * treat that as a backend miss so callers retry on one-shot I/O.
                 */
                res = -EAGAIN;
            }
            if (res > 0 && !has_buffer) {
                // A positive multishot recv without a provided buffer is not
                // usable for the owned-buffer API, so treat it as backend error.
                res = -EIO;
            }

            pthread_mutex_lock(&node->watch_lock);
            watch->activating = false;
            if (res < 0) {
                waiters = watch->wait_head;
                watch->wait_head = NULL;
                watch->wait_tail = NULL;
                if (watch->active) {
                    watch->active = false;
                    release_pending = true;
                }
                watch->deactivate_queued = false;
                if (res == -EAGAIN || res == -EINVAL || res == -EOPNOTSUPP || res == -ENOSYS) {
                    node->supports_multishot_recv = false;
                }
            } else {
                if ((cqe_flags & IORING_CQE_F_MORE) == 0U && watch->active) {
                    watch->active = false;
                    release_pending = true;
                }
                waiter = llam_recv_watch_pop_waiter(watch);
                if (waiter == NULL) {
                    if (watch->migrate_target_node_index != UINT_MAX &&
                        watch->migrate_target_node_index != node->index) {
                        // Transfer readiness to the target node if migration is
                        // in progress and no source waiter consumes it.
                        live_target = watch->migrate_target_node_index;
                        live_node_index = node->index;
                        live_fd = watch->fd;
                        live_st_dev = watch->st_dev;
                        live_st_ino = watch->st_ino;
                        if (watch->active && !watch->deactivate_queued) {
                            kick_deactivate = llam_queue_deactivate_control_locked(
                                node, LLAM_IO_CONTROL_RECV_DEACTIVATE, watch, &watch->deactivate_queued);
                        }
                    } else {
                        queued_ready = llam_recv_watch_push_ready(watch, (size_t)res, bid, has_buffer, node->index, NULL, 0U);
                        if (watch->active && !watch->deactivate_queued) {
                            kick_deactivate = llam_queue_deactivate_control_locked(
                                node, LLAM_IO_CONTROL_RECV_DEACTIVATE, watch, &watch->deactivate_queued);
                        }
                    }
                } else if (watch->active && watch->wait_head == NULL && !watch->deactivate_queued) {
                    // Owned recv is demand-driven, so tear the watch down once
                    // no waiter remains.
                    kick_deactivate = llam_queue_deactivate_control_locked(
                        node, LLAM_IO_CONTROL_RECV_DEACTIVATE, watch, &watch->deactivate_queued);
                }
            }
            if (!kick_deactivate) {
                llam_maybe_destroy_recv_watch_locked(node, watch);
            }
            pthread_mutex_unlock(&node->watch_lock);

            if (release_pending) {
                (void)llam_node_complete_pending_ops(node, 1U);
            }
            if (live_target != UINT_MAX) {
                if (!llam_forward_live_recv_watch_ready(node,
                                                     live_fd,
                                                     live_st_dev,
                                                     live_st_ino,
                                                     live_target,
                                                     (size_t)res,
                                                     bid,
                                                     has_buffer,
                                                     live_node_index)) {
                    pthread_mutex_lock(&node->watch_lock);
                    watch = llam_find_recv_watch_locked(node, live_fd, live_st_dev, live_st_ino);
                    if (watch != NULL && watch->wait_head == NULL) {
                        queued_ready = llam_recv_watch_push_ready(watch, (size_t)res, bid, has_buffer, live_node_index, NULL, 0U);
                    }
                    pthread_mutex_unlock(&node->watch_lock);
                } else {
                    queued_ready = true;
                }
            }
            if (kick_deactivate) {
                llam_kick_node(node);
            }
            if (waiter != NULL) {
                if (has_buffer && waiter->owned_buffer != NULL && node->recv_buf_storage != NULL) {
                    // Hand the selected provided buffer to the waiting owned
                    // buffer wrapper without copying payload bytes.
                    waiter->owned_buffer->provided_storage = true;
                    waiter->owned_buffer->provided_node_index = node->index;
                    waiter->owned_buffer->provided_bid = bid;
                    waiter->owned_buffer->data =
                        node->recv_buf_storage + ((size_t)bid * LLAM_IO_BUFFER_INLINE_BYTES);
                    waiter->owned_buffer->capacity = LLAM_IO_BUFFER_INLINE_BYTES;
                    waiter->owned_buffer->size = (size_t)res;
                    waiter->provided_bid = bid;
                } else if (waiter->owned_buffer != NULL) {
                    waiter->owned_buffer->provided_storage = false;
                    waiter->owned_buffer->provided_bid = 0U;
                    waiter->owned_buffer->size = (size_t)(res >= 0 ? res : 0);
                    waiter->use_provided_buffer = false;
                }
                llam_io_complete_req(node, waiter, res, cqe_flags, false);
            } else if (!queued_ready && has_buffer) {
                // No waiter and no ready queue took ownership; return the
                // provided buffer immediately.
                (void)llam_node_recycle_recv_buffer(node, bid);
            }
            while (waiters != NULL) {
                llam_io_req_t *next = waiters->next;

                waiters->next = NULL;
                llam_io_complete_req(node, waiters, res, cqe_flags, false);
                waiters = next;
            }
            break;
        }
        case LLAM_IO_UDATA_CONTROL: {
            llam_io_control_op_t *op = llam_io_udata_ptr(user_data);

            if (op != NULL) {
                unsigned poll_migrate_target = UINT_MAX;
                unsigned accept_migrate_target = UINT_MAX;
                unsigned recv_migrate_target = UINT_MAX;
                bool kick_target = false;

                pthread_mutex_lock(&node->watch_lock);
                // Control completions finalize deactivate/cancel state and then
                // may trigger migration finalization outside the lock.
                switch (op->kind) {
                case LLAM_IO_CONTROL_POLL_DEACTIVATE: {
                    llam_poll_watch_t *watch = op->target;

                    if (watch != NULL) {
                        watch->deactivate_queued = false;
                        if (watch->active) {
                            watch->active = false;
                            (void)llam_node_complete_pending_ops(node, 1U);
                        }
                        if (watch->migrate_target_node_index != UINT_MAX) {
                            poll_migrate_target = watch->migrate_target_node_index;
                        }
                    }
                    break;
                }
                case LLAM_IO_CONTROL_ACCEPT_DEACTIVATE: {
                    llam_accept_watch_t *watch = op->target;

                    if (watch != NULL) {
                        watch->deactivate_queued = false;
                        if (watch->active) {
                            watch->active = false;
                            (void)llam_node_complete_pending_ops(node, 1U);
                        }
                        if (watch->migrate_target_node_index != UINT_MAX) {
                            accept_migrate_target = watch->migrate_target_node_index;
                        }
                    }
                    break;
                }
                case LLAM_IO_CONTROL_RECV_DEACTIVATE: {
                    llam_recv_watch_t *watch = op->target;

                    if (watch != NULL) {
                        watch->deactivate_queued = false;
                        if (watch->active) {
                            watch->active = false;
                            (void)llam_node_complete_pending_ops(node, 1U);
                        }
                        if (watch->migrate_target_node_index != UINT_MAX) {
                            recv_migrate_target = watch->migrate_target_node_index;
                        } else {
                            llam_maybe_destroy_recv_watch_locked(node, watch);
                        }
                    }
                    break;
                }
                default:
                    break;
                }
                pthread_mutex_unlock(&node->watch_lock);
                if (poll_migrate_target != UINT_MAX) {
                    (void)llam_finalize_poll_watch_migration(node, op->target, poll_migrate_target, &kick_target);
                } else if (accept_migrate_target != UINT_MAX) {
                    (void)llam_finalize_accept_watch_migration(node, op->target, accept_migrate_target, &kick_target);
                } else if (recv_migrate_target != UINT_MAX) {
                    (void)llam_finalize_recv_watch_migration(node, op->target, recv_migrate_target, &kick_target);
                }
                if (kick_target) {
                    unsigned target_index = poll_migrate_target != UINT_MAX ? poll_migrate_target :
                                            (accept_migrate_target != UINT_MAX ? accept_migrate_target : recv_migrate_target);

                    if (target_index < node->runtime->active_nodes) {
                        llam_kick_node(&node->runtime->nodes[target_index]);
                    }
                }
                free(op);
            }
            break;
        }
        default:
            break;
    }
}
