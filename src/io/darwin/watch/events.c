/**
 * @file src/io/darwin/watch/events.c
 * @brief Darwin/kqueue event decoding and readiness dispatch.
 *
 * @details
 * Watch events are edge-dispatched by kqueue. Handlers detach waiters under the
 * node watch lock, run any required accept/recv syscalls without holding that
 * lock, re-arm watches when demand remains, and finalize migration when source
 * watches become inactive.
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

/** @brief Handle one poll-watch kqueue readiness event. */
void llam_darwin_handle_poll_watch_event(llam_node_t *node, llam_poll_watch_t *watch, short revents) {
    llam_io_req_t *waiters;
    bool release_pending = false;
    unsigned finalize_target = UINT_MAX;
    bool kick_target = false;

    pthread_mutex_lock(&node->watch_lock);
    if (watch == NULL || !watch->active) {
        pthread_mutex_unlock(&node->watch_lock);
        return;
    }
    waiters = llam_poll_watch_take_waiters(watch);
    if (waiters == NULL) {
        if (watch->migrate_target_node_index != UINT_MAX &&
            watch->migrate_target_node_index != node->index) {
            // No local waiter consumes this edge; forward/finalize it for the
            // target node if a migration is in progress.
            finalize_target = watch->migrate_target_node_index;
        } else {
            // Sticky readiness lets a later waiter consume this event.
            watch->sticky_revents = revents;
        }
    }
    watch->active = false;
    watch->activating = false;
    watch->deactivate_queued = false;
    release_pending = true;
    pthread_mutex_unlock(&node->watch_lock);

    if (release_pending) {
        (void)llam_node_complete_pending_ops(node, 1U);
    }
    if (llam_darwin_poll_watch_change(node, watch, EV_DELETE) != 0 &&
        llam_darwin_kevent_cleanup_error_is_fatal(errno)) {
        llam_record_fatal(node->runtime, errno);
    }
    if (finalize_target != UINT_MAX) {
        if (!llam_forward_live_poll_watch_event(node, watch->fd, watch->events, finalize_target, revents)) {
            pthread_mutex_lock(&node->watch_lock);
            if (watch->wait_head == NULL) {
                watch->sticky_revents = revents;
            }
            pthread_mutex_unlock(&node->watch_lock);
            finalize_target = UINT_MAX;
        }
    }
    if (finalize_target != UINT_MAX &&
        llam_finalize_poll_watch_migration(node, watch, finalize_target, &kick_target) &&
        kick_target &&
        finalize_target < node->runtime->active_nodes) {
        llam_kick_node(&node->runtime->nodes[finalize_target]);
    }

    while (waiters != NULL) {
        llam_io_req_t *next = waiters->next;

        waiters->next = NULL;
        llam_io_complete_req(node, waiters, (int)revents, false);
        waiters = next;
    }
}

/** @brief Drain accept readiness for a Darwin accept watch. */
void llam_darwin_handle_accept_watch_event(llam_node_t *node, llam_accept_watch_t *watch) {
    llam_darwin_accept_completion_t *completion_head = NULL;
    llam_darwin_accept_completion_t *completion_tail = NULL;
    int saved_flags = 0;
    bool restore_flags = false;
    bool rearm = false;
    bool release_pending = false;
    int accept_error = 0;
    unsigned finalize_target = UINT_MAX;
    bool kick_target = false;

    if (watch == NULL) {
        return;
    }

    if (llam_darwin_fd_set_nonblocking(watch->fd, &saved_flags, &restore_flags) != 0) {
        accept_error = errno;
    } else {
        for (;;) {
            llam_io_req_t *waiter;
            int accepted_fd;

            accepted_fd = accept(watch->fd, NULL, NULL);
            if (accepted_fd >= 0) {
                unsigned live_target = UINT_MAX;

                pthread_mutex_lock(&node->watch_lock);
                waiter = llam_accept_watch_pop_waiter(watch);
                if (waiter == NULL) {
                    if (watch->migrate_target_node_index != UINT_MAX &&
                        watch->migrate_target_node_index != node->index) {
                        // Accepted fd ownership transfers to the target watch or fallback storage.
                        live_target = watch->migrate_target_node_index;
                    } else {
                        llam_accept_watch_push_ready(watch, accepted_fd);
                    }
                    pthread_mutex_unlock(&node->watch_lock);
                    if (live_target != UINT_MAX &&
                        !llam_forward_live_accept_watch_ready(node, watch->fd, live_target, accepted_fd)) {
                        pthread_mutex_lock(&node->watch_lock);
                        llam_accept_watch_push_ready(watch, accepted_fd);
                        pthread_mutex_unlock(&node->watch_lock);
                    }
                    continue;
                }
                pthread_mutex_unlock(&node->watch_lock);

                if (!llam_darwin_accept_completion_push(&completion_head, &completion_tail, waiter, accepted_fd)) {
                    close(accepted_fd);
                    accept_error = ENOMEM;
                    break;
                }
                continue;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            accept_error = errno;
            break;
        }
        llam_darwin_fd_restore(watch->fd, saved_flags, restore_flags);
    }

    pthread_mutex_lock(&node->watch_lock);
    if (watch->active) {
        if (accept_error == 0 && watch->wait_head != NULL) {
            rearm = true;
        } else {
            watch->active = false;
            release_pending = true;
            if (watch->migrate_target_node_index != UINT_MAX &&
                watch->migrate_target_node_index != node->index) {
                finalize_target = watch->migrate_target_node_index;
            }
        }
    }
    watch->activating = false;
    watch->deactivate_queued = false;
    pthread_mutex_unlock(&node->watch_lock);

    if (release_pending) {
        (void)llam_node_complete_pending_ops(node, 1U);
    }

    if (accept_error != 0) {
        llam_io_req_t *waiters;

        // A permanent accept error wakes every parked waiter with that errno.
        if (llam_darwin_accept_watch_change(node, watch, EV_DELETE) != 0 &&
            llam_darwin_kevent_cleanup_error_is_fatal(errno)) {
            llam_record_fatal(node->runtime, errno);
        }
        pthread_mutex_lock(&node->watch_lock);
        waiters = watch->wait_head;
        watch->wait_head = NULL;
        watch->wait_tail = NULL;
        pthread_mutex_unlock(&node->watch_lock);
        while (waiters != NULL) {
            llam_io_req_t *next = waiters->next;

            waiters->next = NULL;
            llam_io_complete_req(node, waiters, -accept_error, false);
            waiters = next;
        }
    } else if (rearm) {
        // One-shot kqueue delivery disables the watch after delivery, so
        // re-enable it if waiters remain and no error occurred.
        if (llam_darwin_accept_watch_change(node, watch, EV_ADD | EV_ENABLE | LLAM_KQUEUE_WATCH_ONESHOT_FLAGS | EV_CLEAR) != 0) {
            int rearm_errno = errno;
            llam_io_req_t *waiters;

            pthread_mutex_lock(&node->watch_lock);
            waiters = watch->wait_head;
            watch->wait_head = NULL;
            watch->wait_tail = NULL;
            if (watch->active) {
                watch->active = false;
                (void)llam_node_complete_pending_ops(node, 1U);
            }
            pthread_mutex_unlock(&node->watch_lock);
            while (waiters != NULL) {
                llam_io_req_t *next = waiters->next;

                waiters->next = NULL;
                llam_io_complete_req(node, waiters, -rearm_errno, false);
                waiters = next;
            }
        }
    } else if (llam_darwin_accept_watch_change(node, watch, EV_DELETE) != 0 &&
               llam_darwin_kevent_cleanup_error_is_fatal(errno)) {
        llam_record_fatal(node->runtime, errno);
    }

    if (finalize_target != UINT_MAX &&
        llam_finalize_accept_watch_migration(node, watch, finalize_target, &kick_target) &&
        kick_target &&
        finalize_target < node->runtime->active_nodes) {
        llam_kick_node(&node->runtime->nodes[finalize_target]);
    }

    llam_darwin_complete_accept_completions(node, completion_head);
}

/** @brief Drain receive readiness for a Darwin receive watch. */
void llam_darwin_handle_recv_watch_event(llam_node_t *node, llam_recv_watch_t *watch) {
#if !defined(MSG_DONTWAIT)
    int saved_flags = 0;
    bool restore_flags = false;
#endif
    bool rearm = false;
    bool release_pending = false;
    int recv_error = 0;
    unsigned char packet[LLAM_IO_BUFFER_INLINE_BYTES];
    unsigned finalize_target = UINT_MAX;
    bool kick_target = false;

    if (watch == NULL) {
        return;
    }

#if !defined(MSG_DONTWAIT)
    if (llam_darwin_fd_set_nonblocking(watch->fd, &saved_flags, &restore_flags) != 0) {
        recv_error = errno;
    } else {
#endif
        for (;;) {
            ssize_t received;
            llam_io_req_t *waiter = NULL;
            unsigned live_target = UINT_MAX;

#if defined(MSG_DONTWAIT)
            received = recv(watch->fd, packet, sizeof(packet), MSG_DONTWAIT);
#else
            received = recv(watch->fd, packet, sizeof(packet), 0);
#endif
            if (received >= 0) {
                pthread_mutex_lock(&node->watch_lock);
                waiter = llam_recv_watch_pop_waiter(watch);
                if (waiter == NULL) {
                    if (watch->migrate_target_node_index != UINT_MAX &&
                        watch->migrate_target_node_index != node->index) {
                        // Forward copied packet bytes when migration takes over this fd.
                        live_target = watch->migrate_target_node_index;
                    } else if (!llam_recv_watch_push_ready_copy(watch, packet, (size_t)received)) {
                        recv_error = ENOMEM;
                    }
                    pthread_mutex_unlock(&node->watch_lock);
                    if (live_target != UINT_MAX &&
                        !llam_forward_live_recv_watch_ready(node,
                                                          watch->fd,
                                                          watch->st_dev,
                                                          watch->st_ino,
                                                          live_target,
                                                          packet,
                                                          (size_t)received)) {
                        pthread_mutex_lock(&node->watch_lock);
                        if (!llam_recv_watch_push_ready_copy(watch, packet, (size_t)received)) {
                            recv_error = ENOMEM;
                        }
                        pthread_mutex_unlock(&node->watch_lock);
                    }
                    if (recv_error != 0) {
                        break;
                    }
                    continue;
                }
                pthread_mutex_unlock(&node->watch_lock);

                if (!llam_darwin_assign_owned_buffer(waiter, packet, (size_t)received, NULL, 0U)) {
                    llam_io_complete_req(node, waiter, -ENOMEM, false);
                    recv_error = ENOMEM;
                    break;
                }
                waiter->use_provided_buffer = false;
                llam_io_complete_req(node, waiter, (int)received, false);
                continue;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            recv_error = errno;
            break;
        }
#if !defined(MSG_DONTWAIT)
        llam_darwin_fd_restore(watch->fd, saved_flags, restore_flags);
    }
#endif

    pthread_mutex_lock(&node->watch_lock);
    if (watch->active) {
        if (recv_error == 0 && watch->wait_head != NULL) {
            rearm = true;
        } else {
            watch->active = false;
            release_pending = true;
            if (watch->migrate_target_node_index != UINT_MAX &&
                watch->migrate_target_node_index != node->index) {
                finalize_target = watch->migrate_target_node_index;
            }
        }
    }
    watch->activating = false;
    watch->deactivate_queued = false;
    pthread_mutex_unlock(&node->watch_lock);

    if (release_pending) {
        (void)llam_node_complete_pending_ops(node, 1U);
    }

    if (recv_error != 0) {
        llam_io_req_t *waiters;

        // Error tears down the watch and wakes every waiter with the same errno.
        if (llam_darwin_recv_watch_change(node, watch, EV_DELETE) != 0 &&
            llam_darwin_kevent_cleanup_error_is_fatal(errno)) {
            llam_record_fatal(node->runtime, errno);
        }
        pthread_mutex_lock(&node->watch_lock);
        waiters = watch->wait_head;
        watch->wait_head = NULL;
        watch->wait_tail = NULL;
        pthread_mutex_unlock(&node->watch_lock);
        while (waiters != NULL) {
            llam_io_req_t *next = waiters->next;

            waiters->next = NULL;
            llam_io_complete_req(node, waiters, -recv_error, false);
            waiters = next;
        }
    } else if (rearm) {
        // One-shot kqueue delivery requires explicit re-enable when waiters remain.
        if (llam_darwin_recv_watch_change(node, watch, EV_ADD | EV_ENABLE | LLAM_KQUEUE_WATCH_ONESHOT_FLAGS | EV_CLEAR) != 0) {
            int rearm_errno = errno;
            llam_io_req_t *waiters;

            pthread_mutex_lock(&node->watch_lock);
            waiters = watch->wait_head;
            watch->wait_head = NULL;
            watch->wait_tail = NULL;
            if (watch->active) {
                watch->active = false;
                (void)llam_node_complete_pending_ops(node, 1U);
            }
            pthread_mutex_unlock(&node->watch_lock);
            while (waiters != NULL) {
                llam_io_req_t *next = waiters->next;

                waiters->next = NULL;
                llam_io_complete_req(node, waiters, -rearm_errno, false);
                waiters = next;
            }
        }
    } else if (llam_darwin_recv_watch_change(node, watch, EV_DELETE) != 0 &&
               llam_darwin_kevent_cleanup_error_is_fatal(errno)) {
        llam_record_fatal(node->runtime, errno);
    }

    if (finalize_target != UINT_MAX &&
        llam_finalize_recv_watch_migration(node, watch, finalize_target, &kick_target) &&
        kick_target &&
        finalize_target < node->runtime->active_nodes) {
        llam_kick_node(&node->runtime->nodes[finalize_target]);
    }

    pthread_mutex_lock(&node->watch_lock);
    llam_maybe_destroy_recv_watch_locked(node, watch);
    pthread_mutex_unlock(&node->watch_lock);
}

/** @brief Handle a one-shot request kevent. */
void llam_darwin_handle_req_event(llam_node_t *node, llam_io_req_t *req, const struct kevent *event) {
    int result = 0;
    unsigned wait_mode;

    if (node == NULL || req == NULL) {
        return;
    }

    wait_mode = atomic_load_explicit(&req->wait_mode, memory_order_acquire);
    if (wait_mode != LLAM_IO_WAIT_MODE_INFLIGHT) {
        return;
    }

    if (atomic_load_explicit(&req->cancel_queued, memory_order_acquire) != 0U ||
        atomic_load_explicit(&req->abort_reason, memory_order_acquire) != LLAM_IO_ABORT_NONE) {
        // Cancellation wins over a readiness event; delete the kqueue filter and
        // complete through the normal abort path.
        llam_darwin_req_delete(node, req);
        llam_io_complete_req(node, req, -ECANCELED, true);
        return;
    }

    if (req->kind == LLAM_IO_KIND_POLL) {
        llam_darwin_req_delete(node, req);
        llam_io_complete_req(node, req, (int)llam_darwin_poll_revents(event), true);
        return;
    }
    if (req->kind == LLAM_IO_KIND_CONNECT) {
        int connect_result = 0;

        llam_darwin_req_delete(node, req);
        if (llam_socket_connect_error(req->fd) != 0) {
            connect_result = -errno;
        }
        llam_io_complete_req(node, req, connect_result, true);
        return;
    }

    switch (llam_darwin_try_req_syscall(req, &result)) {
    case 1:
        llam_io_complete_req(node, req, result, true);
        break;
    case 0:
        // Readiness was transient; re-register the one-shot kqueue event.
        if (llam_darwin_req_register(node, req) != 0) {
            llam_io_complete_req(node, req, -errno, true);
        }
        break;
    default:
        llam_io_complete_req(node, req, result, true);
        break;
    }
}

/** @brief Submit one request to the Darwin backend. */
void llam_darwin_submit_req(llam_node_t *node, llam_io_req_t *req) {
    int result = 0;

    if (node == NULL || req == NULL) {
        return;
    }

    if (atomic_load_explicit(&req->wait_mode, memory_order_acquire) != LLAM_IO_WAIT_MODE_INFLIGHT) {
        atomic_store_explicit(&req->inflight_owner_shard, req->owner_shard, memory_order_release);
        atomic_store(&req->wait_mode, LLAM_IO_WAIT_MODE_INFLIGHT);
        llam_shard_note_inflight_io_waiter(req->owner_runtime, req->owner_shard, 1);
    }
    if (llam_io_req_abort_requested(req)) {
        // Cancel can arrive before a kqueue filter exists, so finish locally.
        llam_io_complete_req(node, req, -ECANCELED, true);
        return;
    }

    if (req->kind != LLAM_IO_KIND_POLL) {
        // Try immediate nonblocking completion before registering with kqueue.
        switch (llam_darwin_try_req_syscall(req, &result)) {
        case 1:
            llam_io_complete_req(node, req, result, true);
            return;
        case -1:
            llam_io_complete_req(node, req, result, true);
            return;
        default:
            break;
        }
    }

    if (llam_darwin_req_register(node, req) != 0) {
        llam_io_complete_req(node, req, -errno, true);
    }
}
