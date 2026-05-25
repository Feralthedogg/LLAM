/**
 * @file src/io/darwin/runtime_io_watch_darwin_worker.c
 * @brief Darwin I/O worker loop and kqueue polling lifecycle.
 *
 * @details
 * Each Darwin I/O node owns a kqueue worker. The worker applies queued control
 * operations, submits one-shot requests, waits for kqueue events, handles user
 * wake events, and dispatches tagged watch/request events to specialized
 * handlers.
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

/** @brief Apply one queued Darwin control operation. */
void llam_darwin_process_control(llam_node_t *node, llam_io_control_op_t *op) {
    int rc = 0;

    switch (op->kind) {
    case LLAM_IO_CONTROL_POLL_ACTIVATE: {
        llam_poll_watch_t *watch = op->target;
        llam_io_req_t *waiters = NULL;

        rc = llam_darwin_poll_watch_change(node, watch, EV_ADD | EV_ENABLE | EV_DISPATCH);
        pthread_mutex_lock(&node->watch_lock);
        if (rc == 0) {
            // EV_DISPATCH produces one event then disables the watch until the
            // event handler explicitly re-enables it.
            watch->active = true;
            watch->activating = false;
            watch->deactivate_queued = false;
            atomic_fetch_add(&node->pending_ops, 1U);
        } else {
            watch->active = false;
            watch->activating = false;
            watch->deactivate_queued = false;
            watch->sticky_revents = 0;
            waiters = llam_poll_watch_take_waiters(watch);
        }
        pthread_mutex_unlock(&node->watch_lock);
        while (waiters != NULL) {
            llam_io_req_t *next = waiters->next;

            waiters->next = NULL;
            llam_io_complete_req(node, waiters, -errno, false);
            waiters = next;
        }
        break;
    }
    case LLAM_IO_CONTROL_POLL_DEACTIVATE: {
        llam_poll_watch_t *watch = op->target;
        unsigned migrate_target = UINT_MAX;
        bool kick_target = false;

        rc = llam_darwin_poll_watch_change(node, watch, EV_DELETE);
        pthread_mutex_lock(&node->watch_lock);
        watch->activating = false;
        watch->deactivate_queued = false;
        if (watch->active) {
            watch->active = false;
            atomic_fetch_sub(&node->pending_ops, 1U);
        }
        if (watch->migrate_target_node_index != UINT_MAX) {
            migrate_target = watch->migrate_target_node_index;
        }
        pthread_mutex_unlock(&node->watch_lock);
        if (rc != 0 && errno != ENOENT && errno != EAGAIN) {
            llam_record_fatal(node->runtime, errno);
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

        rc = llam_darwin_accept_watch_change(node, watch, EV_ADD | EV_ENABLE | EV_DISPATCH | EV_CLEAR);
        pthread_mutex_lock(&node->watch_lock);
        if (rc == 0) {
            watch->active = true;
            watch->activating = false;
            watch->deactivate_queued = false;
            atomic_fetch_add(&node->pending_ops, 1U);
        } else {
            watch->active = false;
            watch->activating = false;
            watch->deactivate_queued = false;
            waiters = watch->wait_head;
            watch->wait_head = NULL;
            watch->wait_tail = NULL;
        }
        pthread_mutex_unlock(&node->watch_lock);
        while (waiters != NULL) {
            llam_io_req_t *next = waiters->next;

            waiters->next = NULL;
            llam_io_complete_req(node, waiters, -errno, false);
            waiters = next;
        }
        break;
    }
    case LLAM_IO_CONTROL_ACCEPT_DEACTIVATE: {
        llam_accept_watch_t *watch = op->target;
        unsigned migrate_target = UINT_MAX;
        bool kick_target = false;

        rc = llam_darwin_accept_watch_change(node, watch, EV_DELETE);
        pthread_mutex_lock(&node->watch_lock);
        watch->activating = false;
        watch->deactivate_queued = false;
        if (watch->active) {
            watch->active = false;
            atomic_fetch_sub(&node->pending_ops, 1U);
        }
        if (watch->migrate_target_node_index != UINT_MAX) {
            migrate_target = watch->migrate_target_node_index;
        }
        pthread_mutex_unlock(&node->watch_lock);
        if (rc != 0 && errno != ENOENT) {
            llam_record_fatal(node->runtime, errno);
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

        rc = llam_darwin_recv_watch_change(node, watch, EV_ADD | EV_ENABLE | EV_DISPATCH | EV_CLEAR);
        pthread_mutex_lock(&node->watch_lock);
        if (rc == 0) {
            watch->active = true;
            watch->activating = false;
            watch->deactivate_queued = false;
            atomic_fetch_add(&node->pending_ops, 1U);
        } else {
            watch->active = false;
            watch->activating = false;
            watch->deactivate_queued = false;
            waiters = watch->wait_head;
            watch->wait_head = NULL;
            watch->wait_tail = NULL;
        }
        pthread_mutex_unlock(&node->watch_lock);
        while (waiters != NULL) {
            llam_io_req_t *next = waiters->next;

            waiters->next = NULL;
            llam_io_complete_req(node, waiters, -errno, false);
            waiters = next;
        }
        break;
    }
    case LLAM_IO_CONTROL_RECV_DEACTIVATE: {
        llam_recv_watch_t *watch = op->target;
        unsigned migrate_target = UINT_MAX;
        bool kick_target = false;

        rc = llam_darwin_recv_watch_change(node, watch, EV_DELETE);
        pthread_mutex_lock(&node->watch_lock);
        watch->activating = false;
        watch->deactivate_queued = false;
        if (watch->active) {
            watch->active = false;
            atomic_fetch_sub(&node->pending_ops, 1U);
        }
        if (watch->migrate_target_node_index != UINT_MAX) {
            migrate_target = watch->migrate_target_node_index;
        } else {
            llam_maybe_destroy_recv_watch_locked(node, watch);
        }
        pthread_mutex_unlock(&node->watch_lock);
        if (rc != 0 && errno != ENOENT) {
            llam_record_fatal(node->runtime, errno);
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

                controls->next = NULL;
                llam_darwin_process_control(node, controls);
                free(controls);
                controls = next;
            }
        }
        llam_darwin_process_submissions(node);

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
            // EVFILT_USER/MACHPORT to interrupt this wait.
            ts_ptr = NULL;
        }

        do {
            count = kevent(node->event_fd, NULL, 0, events, (int)LLAM_DARWIN_KEVENT_BATCH, ts_ptr);
        } while (count < 0 && errno == EINTR);
        if (count < 0) {
            llam_record_fatal(rt, errno);
            break;
        }

        for (i = 0; i < (unsigned)count; ++i) {
            uint64_t user_data;
            unsigned tag;

            if (events[i].filter == EVFILT_USER || events[i].filter == EVFILT_MACHPORT) {
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
    }

    return NULL;
}
