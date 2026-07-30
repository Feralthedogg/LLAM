/**
 * @file src/io/api/issue_watch.c
 * @brief Shared multishot watch issue transitions.
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

#include "io/runtime_io_api_internal.h"

int llam_issue_multishot_poll(llam_io_req_t *req) {
    llam_runtime_t *rt;
    llam_shard_t *shard = g_llam_tls_shard;
    llam_node_t *node;
    llam_poll_watch_t *watch;
    bool kick = false;
    short immediate_revents = 0;
    int immediate_rc;

    if (req == NULL || shard == NULL || g_llam_tls_task == NULL) {
        errno = EINVAL;
        return -1;
    }
    rt = llam_io_request_runtime(req);
    if (rt == NULL || shard->runtime != rt) {
        errno = EXDEV;
        return -1;
    }

    /* Multishot is reserved for indefinite watch-style waits; timed waits stay on the one-shot path. */
    node = &rt->nodes[llam_multishot_owner_node_index(rt, shard->io_node_index, req->fd)];
    if (!node->ring_ready || !node->supports_multishot_poll) {
        errno = EAGAIN;
        return -1;
    }
    atomic_store_explicit(&req->attached_node_index,
                          node->index,
                          memory_order_release);

    llam_fd_watch_lifecycle_lock();
    pthread_mutex_lock(&node->watch_lock);
    watch = llam_get_or_create_poll_watch_locked(node, req->fd, req->poll_events);
    if (watch == NULL) {
        int saved_errno = errno;

        pthread_mutex_unlock(&node->watch_lock);
        llam_fd_watch_lifecycle_unlock();
        return llam_fail_io_setup_req(req, saved_errno != 0 ? saved_errno : ENOMEM);
    }
    if (watch->migrate_target_node_index != UINT_MAX && watch->migrate_target_node_index != node->index) {
        watch->migrate_target_node_index = UINT_MAX;
        watch->live_transferred = false;
    }

    if (watch->sticky_revents != 0) {
        req->result = 1;
        req->error_code = 0;
        req->poll_revents = watch->sticky_revents;
        watch->sticky_revents = 0;
        if (watch->deactivate_queued &&
            llam_drop_node_control_locked(node, LLAM_IO_CONTROL_POLL_DEACTIVATE, watch)) {
            watch->deactivate_queued = false;
        }
        pthread_mutex_unlock(&node->watch_lock);
        llam_fd_watch_lifecycle_unlock();
        return 0;
    }

    /*
     * A multishot poll CQE wakes the waiters that were already attached to the
     * watch. Poll readiness is level-triggered, so a waiter that arrives just
     * after that CQE must not park behind an already-readable fd.
     */
    immediate_rc = llam_platform_poll_now(req->fd, req->poll_events, &immediate_revents);
    if (immediate_rc != 0) {
        int saved_errno = errno;

        pthread_mutex_unlock(&node->watch_lock);
        llam_fd_watch_lifecycle_unlock();
        if (immediate_rc > 0) {
            req->result = 1;
            req->error_code = 0;
            req->poll_revents = immediate_revents;
            return 0;
        }
        req->result = -1;
        req->error_code = saved_errno;
        req->poll_revents = 0;
        errno = saved_errno;
        return -1;
    }

    req->poll_watch = watch;
    req->accept_watch = NULL;
    req->recv_watch = NULL;
    if (llam_prepare_io_wait(req, LLAM_IO_WAIT_MODE_POLL_WATCH, 0U) != 0) {
        int saved_errno = errno;

        pthread_mutex_unlock(&node->watch_lock);
        llam_fd_watch_lifecycle_unlock();
        return llam_fail_io_setup_req(req, saved_errno);
    }
    if (watch->deactivate_queued) {
        if (!llam_drop_node_control_locked(node, LLAM_IO_CONTROL_POLL_DEACTIVATE, watch)) {
            pthread_mutex_unlock(&node->watch_lock);
            llam_fd_watch_lifecycle_unlock();
            llam_cleanup_io_wait_setup(g_llam_tls_task, req);
            return llam_fail_io_setup_req(req, EAGAIN);
        }
        watch->deactivate_queued = false;
    }
    if (!watch->active && !watch->activating) {
        if (llam_node_queue_control_locked(node, LLAM_IO_CONTROL_POLL_ACTIVATE, watch) != 0) {
            int saved_errno = errno != 0 ? errno : ENOMEM;

            pthread_mutex_unlock(&node->watch_lock);
            llam_fd_watch_lifecycle_unlock();
            llam_cleanup_io_wait_setup(g_llam_tls_task, req);
            return llam_fail_io_setup_req(req, saved_errno);
        }
        watch->activating = true;
        kick = true;
    }
    llam_poll_watch_enqueue_waiter(watch, req);
    pthread_mutex_unlock(&node->watch_lock);
    llam_fd_watch_lifecycle_unlock();
    if (kick) {
        llam_kick_node(node);
    }

    return llam_park_io_req(req, false, 0U, NULL);
}

/**
 * @brief Issue an indefinite multishot accept watch or consume a ready accept.
 *
 * @param req Accept request with listener fd initialized.
 *
 * @return 0 on immediate or parked completion, -1 when multishot accept is not
 *         available or setup fails.
 */
int llam_issue_multishot_accept(llam_io_req_t *req) {
    llam_runtime_t *rt;
    llam_shard_t *shard = g_llam_tls_shard;
    llam_node_t *node;
    llam_accept_watch_t *watch;
    int ready_fd;
    bool kick = false;

    if (req == NULL || shard == NULL || g_llam_tls_task == NULL) {
        errno = EINVAL;
        return -1;
    }
    rt = llam_io_request_runtime(req);
    if (rt == NULL || shard->runtime != rt) {
        errno = EXDEV;
        return -1;
    }

    /* Accept multishot is only safe when the runtime owns buffering for the watch lifetime. */
    node = &rt->nodes[llam_multishot_owner_node_index(rt, shard->io_node_index, req->fd)];
    if (!node->ring_ready || !node->supports_multishot_accept) {
        errno = EAGAIN;
        return -1;
    }
    atomic_store_explicit(&req->attached_node_index,
                          node->index,
                          memory_order_release);

    llam_fd_watch_lifecycle_lock();
    pthread_mutex_lock(&node->watch_lock);
    watch = llam_get_or_create_accept_watch_locked(node, req->fd);
    if (watch == NULL) {
        pthread_mutex_unlock(&node->watch_lock);
        llam_fd_watch_lifecycle_unlock();
        return llam_fail_io_setup_req(req, ENOMEM);
    }
    if (watch->migrate_target_node_index != UINT_MAX && watch->migrate_target_node_index != node->index) {
        watch->migrate_target_node_index = UINT_MAX;
        watch->live_transferred = false;
    }

    ready_fd = llam_accept_watch_pop_ready(watch);
    if (ready_fd >= 0) {
        req->result = ready_fd;
        req->error_code = 0;
        pthread_mutex_unlock(&node->watch_lock);
        llam_fd_watch_lifecycle_unlock();
        return 0;
    }

    /*
     * A terminal target CQE can arrive before its already-submitted cancel CQE.
     * Do not let a new accept activation reuse the watch state while that older
     * control can still clear active/pending ownership for a different backend
     * generation.  A queued (not yet submitted) cancel is safe to withdraw only
     * while the original target is still active; otherwise use the one-shot
     * fallback until the control CQE retires it.
     */
    req->poll_watch = NULL;
    req->accept_watch = watch;
    req->recv_watch = NULL;
    if (llam_prepare_io_wait(req, LLAM_IO_WAIT_MODE_ACCEPT_WATCH, 0U) != 0) {
        int saved_errno = errno;

        pthread_mutex_unlock(&node->watch_lock);
        llam_fd_watch_lifecycle_unlock();
        return llam_fail_io_setup_req(req, saved_errno);
    }
    if (watch->deactivate_queued) {
        if (!watch->active ||
            !llam_drop_node_control_locked(node, LLAM_IO_CONTROL_ACCEPT_DEACTIVATE, watch)) {
            pthread_mutex_unlock(&node->watch_lock);
            llam_fd_watch_lifecycle_unlock();
            llam_cleanup_io_wait_setup(g_llam_tls_task, req);
            return llam_fail_io_setup_req(req, EAGAIN);
        }
        watch->deactivate_queued = false;
    }

    if (!watch->active && !watch->activating) {
        if (llam_node_queue_control_locked(node, LLAM_IO_CONTROL_ACCEPT_ACTIVATE, watch) != 0) {
            int saved_errno = errno != 0 ? errno : ENOMEM;

            pthread_mutex_unlock(&node->watch_lock);
            llam_fd_watch_lifecycle_unlock();
            llam_cleanup_io_wait_setup(g_llam_tls_task, req);
            return llam_fail_io_setup_req(req, saved_errno);
        }
        watch->activating = true;
        kick = true;
    }
    llam_accept_watch_enqueue_waiter(watch, req);
    pthread_mutex_unlock(&node->watch_lock);
    llam_fd_watch_lifecycle_unlock();
    if (kick) {
        llam_kick_node(node);
    }

    return llam_park_io_req(req, false, 0U, NULL);
}

/**
 * @brief Issue an indefinite multishot recv/read watch for owned-buffer I/O.
 *
 * Ready data may already be buffered by the watch.  In that case this function
 * attaches either backend-provided storage, copied storage, or inline fallback
 * storage to the request-owned buffer before returning immediately.
 *
 * @param req Receive request with fd and owned_buffer initialized.
 *
 * @return 0 on immediate or parked completion, -1 when multishot recv is not
 *         available or setup fails.
 */
int llam_issue_multishot_recv(llam_io_req_t *req) {
    llam_runtime_t *rt;
    llam_shard_t *shard = g_llam_tls_shard;
    llam_node_t *node;
    llam_recv_watch_t *watch;
    size_t ready_size = 0U;
    unsigned short ready_bid = 0U;
    bool ready_has_buffer = false;
    unsigned ready_node_index = UINT_MAX;
    unsigned char *ready_copy_data = NULL;
    size_t ready_copy_capacity = 0U;
    bool kick = false;

    if (req == NULL || shard == NULL || g_llam_tls_task == NULL || req->owned_buffer == NULL) {
        errno = EINVAL;
        return -1;
    }
    rt = llam_io_request_runtime(req);
    if (rt == NULL || shard->runtime != rt) {
        errno = EXDEV;
        return -1;
    }

    node = &rt->nodes[llam_multishot_owner_node_index(rt, shard->io_node_index, req->fd)];
    if (!node->ring_ready || !node->supports_multishot_recv) {
        errno = EAGAIN;
        return -1;
    }
    atomic_store_explicit(&req->attached_node_index,
                          node->index,
                          memory_order_release);

    llam_fd_watch_lifecycle_lock();
    pthread_mutex_lock(&node->watch_lock);
    watch = llam_get_or_create_recv_watch_locked(node, req->fd);
    if (watch == NULL) {
        int saved_errno = errno;

        pthread_mutex_unlock(&node->watch_lock);
        llam_fd_watch_lifecycle_unlock();
        return llam_fail_io_setup_req(req, saved_errno != 0 ? saved_errno : ENOMEM);
    }
    if (watch->migrate_target_node_index != UINT_MAX && watch->migrate_target_node_index != node->index) {
        watch->migrate_target_node_index = UINT_MAX;
        watch->live_transferred = false;
    }
    if (watch->active && atomic_load_explicit(&node->pending_ops, memory_order_acquire) == 0U) {
        watch->active = false;
        watch->deactivate_queued = false;
    }

    if (llam_recv_watch_pop_ready(watch,
                                &ready_size,
                                &ready_bid,
                                &ready_has_buffer,
                                &ready_node_index,
                                &ready_copy_data,
                                &ready_copy_capacity)) {
        if (ready_has_buffer &&
            ready_node_index < rt->active_nodes &&
            rt->nodes[ready_node_index].recv_buf_storage != NULL) {
            llam_node_t *ready_node = &rt->nodes[ready_node_index];

            req->owned_buffer->provided_storage = true;
            req->owned_buffer->provided_node_index = ready_node->index;
            req->owned_buffer->provided_bid = ready_bid;
            req->owned_buffer->data = ready_node->recv_buf_storage + ((size_t)ready_bid * LLAM_IO_BUFFER_INLINE_BYTES);
            req->owned_buffer->capacity = LLAM_IO_BUFFER_INLINE_BYTES;
            req->owned_buffer->external_storage = false;
        } else if (ready_copy_data != NULL) {
            req->owned_buffer->provided_storage = false;
            req->owned_buffer->provided_bid = 0U;
            req->owned_buffer->data = ready_copy_data;
            req->owned_buffer->capacity = ready_copy_capacity != 0U ? ready_copy_capacity : ready_size;
            req->owned_buffer->external_storage = true;
            req->use_provided_buffer = false;
        } else {
            req->owned_buffer->provided_storage = false;
            req->owned_buffer->provided_bid = 0U;
            req->owned_buffer->data = req->owned_buffer->inline_data;
            req->owned_buffer->capacity = LLAM_IO_BUFFER_INLINE_BYTES;
            req->owned_buffer->external_storage = false;
            req->use_provided_buffer = false;
        }
        req->owned_buffer->size = ready_size;
        req->result = (ssize_t)ready_size;
        req->error_code = 0;
        req->provided_bid = ready_has_buffer ? ready_bid : 0U;
        llam_maybe_destroy_recv_watch_locked(node, watch);
        pthread_mutex_unlock(&node->watch_lock);
        llam_fd_watch_lifecycle_unlock();
        return 0;
    }

    req->poll_watch = NULL;
    req->accept_watch = NULL;
    req->recv_watch = watch;
    if (llam_prepare_io_wait(req, LLAM_IO_WAIT_MODE_RECV_WATCH, 0U) != 0) {
        int saved_errno = errno;

        pthread_mutex_unlock(&node->watch_lock);
        llam_fd_watch_lifecycle_unlock();
        return llam_fail_io_setup_req(req, saved_errno);
    }
    if (watch->deactivate_queued) {
        if (!watch->active || !llam_drop_node_control_locked(node, LLAM_IO_CONTROL_RECV_DEACTIVATE, watch)) {
            pthread_mutex_unlock(&node->watch_lock);
            llam_fd_watch_lifecycle_unlock();
            llam_cleanup_io_wait_setup(g_llam_tls_task, req);
            return llam_fail_io_setup_req(req, EAGAIN);
        }
        watch->deactivate_queued = false;
    }

    if (!watch->active && !watch->activating) {
        if (llam_node_queue_control_locked(node, LLAM_IO_CONTROL_RECV_ACTIVATE, watch) != 0) {
            int saved_errno = errno != 0 ? errno : ENOMEM;

            pthread_mutex_unlock(&node->watch_lock);
            llam_fd_watch_lifecycle_unlock();
            llam_cleanup_io_wait_setup(g_llam_tls_task, req);
            return llam_fail_io_setup_req(req, saved_errno);
        }
        watch->activating = true;
        kick = true;
    }
    llam_recv_watch_enqueue_waiter(watch, req);
    pthread_mutex_unlock(&node->watch_lock);
    llam_fd_watch_lifecycle_unlock();
    if (kick) {
        llam_kick_node(node);
    }

    return llam_park_io_req(req, false, 0U, NULL);
}
