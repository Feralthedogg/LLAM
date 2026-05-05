/**
 * @file src/io/runtime_io_api_issue.c
 * @brief I/O request issue path, watch lookup, and readiness submission logic.
 *
 * @details
 * This translation unit is the handoff point between task-facing I/O APIs and
 * the platform I/O backend:
 *  - task bootstrap/exit helpers used by fiber entry code,
 *  - common I/O wait setup and cleanup,
 *  - indefinite multishot poll/accept/recv watch paths,
 *  - one-shot backend submit queue issue path.
 *
 * I/O wait lifecycle:
 *  1) Fill an embedded or allocated ::nm_io_req_t.
 *  2) Attach it to a node submit queue or a shared watch.
 *  3) Park the current task with deadline/cancellation tracking.
 *  4) Resume when the backend completion, cancellation, or timeout wakes it.
 *  5) Clear wait tracking and report the request result/error.
 *
 * Race-handling notes:
 *  - Watch operations recheck immediate readiness before parking so level
 *    triggered readiness cannot be missed between CQE delivery and waiter
 *    insertion.
 *  - Cleanup removes requests from whichever wait mode was successfully
 *    published before the task parked.
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

#include "runtime_io_api_internal.h"

/**
 * @brief Finish the current task and switch back to the scheduler context.
 *
 * This is the terminal path for task fibers.  It marks the task dead, wakes
 * join waiters, decrements the runtime live-task count, and requests runtime
 * stop when the last task exits.
 *
 * @note This function does not return.  It assumes g_nm_tls_task and
 *       g_nm_tls_shard identify the running task and owner shard.
 */
void nm_task_exit_internal(void) {
    nm_runtime_t *rt = &g_nm_runtime;
    nm_task_t *task = g_nm_tls_task;

    if (task == NULL || g_nm_tls_shard == NULL) {
        abort();
    }

    task->state = NM_TASK_STATE_DEAD;
    task->wait_reason = NM_WAIT_NONE;
    nm_trace_shard(g_nm_tls_shard, task, NM_TRACE_STATE, NM_TASK_STATE_RUNNING, NM_TASK_STATE_DEAD, NM_WAIT_NONE);
    nm_reinject_join_waiters(rt, task);

    if (atomic_fetch_sub(&rt->live_tasks, 1U) == 1U) {
        nm_request_stop(rt);
    }

    nm_task_sample_live_stack(task);
    nm_ctx_switch(&task->ctx,
                  g_nm_tls_scheduler_ctx != NULL ? g_nm_tls_scheduler_ctx : &g_nm_tls_shard->scheduler_ctx);
    abort();
}

/**
 * @brief Enter a task fiber, run its user callback, and finalize task exit.
 *
 * @param task Task object whose entry/arg fields are already initialized.
 *
 * @note Called from the architecture-specific fiber bootstrap path.
 */
void nm_task_bootstrap(nm_task_t *task) {
    g_nm_tls_task = task;
    if (g_nm_runtime.run_timing_enabled != 0U || g_nm_runtime.profile == NM_RUNTIME_PROFILE_DEBUG_SAFE) {
        atomic_store_explicit(&g_nm_tls_shard->last_safepoint_ns, nm_now_ns(), memory_order_relaxed);
    }
    task->entry(task->arg);
    nm_task_exit_internal();
}

/**
 * @brief Abort after detecting an invalid fiber stack alignment.
 *
 * @param rsp Stack pointer value observed by the assembly bootstrap.
 *
 * @note This is a hard-fail diagnostic for ABI violations in context setup.
 */
void nm_fiber_alignment_violation(uint64_t rsp) {
    dprintf(STDERR_FILENO,
            "nm: fiber bootstrap stack misaligned rsp=0x%llx\n",
            (unsigned long long)rsp);
    abort();
}

/**
 * @brief Roll back I/O wait setup after cancellation, timeout, or setup failure.
 *
 * The request may have been published to a submit queue or attached to a watch
 * before the failure is observed.  This function removes it from the published
 * location, clears cancellation/deadline state, and returns the task to RUNNING.
 *
 * @param task Task that owns the request.
 * @param req I/O request to detach and reset.
 *
 * @note Safe to call with NULL arguments; in that case it does nothing.
 */
void nm_cleanup_io_wait_setup(nm_task_t *task, nm_io_req_t *req) {
    int node_index;

    if (task == NULL || req == NULL) {
        return;
    }

    if (task->cancel_registered) {
        nm_cancel_token_unregister_task(task);
    }
    nm_disarm_task_wait_deadline(task);
    node_index = nm_io_req_node_index(req);
    if (node_index >= 0) {
        nm_node_t *node = &g_nm_runtime.nodes[node_index];
        unsigned mode = atomic_load(&req->wait_mode);

        if (mode == NM_IO_WAIT_MODE_SUBMIT_QUEUE) {
            bool removed;

            pthread_mutex_lock(&node->submit_lock);
            removed = nm_remove_node_submit_locked(node, req);
            pthread_mutex_unlock(&node->submit_lock);
            if (removed) {
                atomic_fetch_sub(&node->pending_ops, 1U);
            }
        } else if (mode == NM_IO_WAIT_MODE_POLL_WATCH && req->poll_watch != NULL) {
            pthread_mutex_lock(&node->watch_lock);
            (void)nm_poll_watch_remove_waiter(req->poll_watch, req);
            pthread_mutex_unlock(&node->watch_lock);
        } else if (mode == NM_IO_WAIT_MODE_ACCEPT_WATCH && req->accept_watch != NULL) {
            pthread_mutex_lock(&node->watch_lock);
            (void)nm_accept_watch_remove_waiter(req->accept_watch, req);
            pthread_mutex_unlock(&node->watch_lock);
        } else if (mode == NM_IO_WAIT_MODE_RECV_WATCH && req->recv_watch != NULL) {
            pthread_mutex_lock(&node->watch_lock);
            (void)nm_recv_watch_remove_waiter(req->recv_watch, req);
            pthread_mutex_unlock(&node->watch_lock);
        }
    }
    atomic_store(&req->wait_mode, NM_IO_WAIT_MODE_NONE);
    atomic_store(&req->abort_reason, NM_IO_ABORT_NONE);
    atomic_store(&req->cancel_queued, 0U);
    atomic_store(&req->inflight_owner_shard, UINT_MAX);
    req->poll_watch = NULL;
    req->accept_watch = NULL;
    req->recv_watch = NULL;
    req->deadline_ns = 0U;
    task->state = NM_TASK_STATE_RUNNING;
    task->wait_reason = NM_WAIT_NONE;
    nm_task_clear_wait_tracking(task);
}

/**
 * @brief Park the current task on an I/O request until it completes.
 *
 * The caller must publish @p req to a submit queue or watch before entering
 * this function.  The park path arms optional timeout/cancellation tracking,
 * kicks the backend node if requested, and switches back to the scheduler.
 *
 * @param req Request that represents the pending I/O operation.
 * @param has_deadline true when @p deadline_ns should be armed.
 * @param deadline_ns Absolute deadline in runtime monotonic nanoseconds.
 * @param wake_node Optional I/O node to kick after publishing the request.
 *
 * @return 0 when the request completed successfully, -1 when the wake reason
 *         carried an errno-style error.
 */
int nm_park_io_req(nm_io_req_t *req, bool has_deadline, uint64_t deadline_ns, nm_node_t *wake_node) {
    nm_shard_t *shard = g_nm_tls_shard;
    nm_task_t *task = g_nm_tls_task;

    if (shard == NULL || task == NULL) {
        errno = EINVAL;
        return -1;
    }

    nm_task_set_io_tracking(task, req, shard->id);
    req->task = task;
    req->result = -1;
    req->error_code = 0;
    req->owner_shard = shard->id;
    req->submit_ts_ns = nm_now_ns();
    req->deadline_ns = has_deadline ? deadline_ns : 0U;

    task->state = NM_TASK_STATE_PARKED;
    task->wait_reason = NM_WAIT_IO;
    shard->metrics.io_submits += 1U;
    shard->metrics.parks += 1U;
    nm_trace_shard(shard, task, NM_TRACE_IO_SUBMIT, NM_TASK_STATE_RUNNING, NM_TASK_STATE_PARKED, NM_WAIT_IO);
    if (has_deadline && nm_arm_task_wait_deadline(task, shard, deadline_ns) != 0) {
        nm_cleanup_io_wait_setup(task, req);
        return -1;
    }
    if (task->cancel_token != NULL && nm_cancel_token_register_task(task) != 0) {
        nm_cleanup_io_wait_setup(task, req);
        return -1;
    }
    if (wake_node != NULL) {
        nm_kick_node(wake_node);
    }
    nm_task_sample_live_stack(task);
    nm_ctx_switch(&task->ctx, g_nm_tls_scheduler_ctx != NULL ? g_nm_tls_scheduler_ctx : &shard->scheduler_ctx);
    if (task->cancel_registered) {
        nm_cancel_token_unregister_task(task);
    }
    nm_task_clear_wait_tracking(task);
    shard->metrics.io_completions += 1U;
    errno = req->error_code;
    return req->error_code == 0 ? 0 : -1;
}

/**
 * @brief Issue an indefinite multishot poll watch or complete immediately.
 *
 * Selection rules:
 *  - Use the fd's multishot owner node so all waiters for the same fd share
 *    one watch.
 *  - Consume sticky readiness from a previous CQE before parking.
 *  - Drop stale deactivation controls when a new waiter arrives.
 *  - Recheck readiness with a nonblocking poll before inserting the waiter to
 *    avoid missing level-triggered readiness.
 *
 * @param req Poll request. `fd`, `poll_events`, and task ownership fields must
 *            already be initialized by the caller.
 *
 * @return 0 on immediate or parked completion, -1 when the request must fall
 *         back to another path or fails with errno set.
 */
int nm_issue_multishot_poll(nm_io_req_t *req) {
    nm_runtime_t *rt = &g_nm_runtime;
    nm_shard_t *shard = g_nm_tls_shard;
    nm_node_t *node;
    nm_poll_watch_t *watch;
    bool kick = false;
    short immediate_revents = 0;
    int immediate_rc;

    if (req == NULL || shard == NULL || g_nm_tls_task == NULL) {
        errno = EINVAL;
        return -1;
    }

    /* Multishot is reserved for indefinite watch-style waits; timed waits stay on the one-shot path. */
    node = &rt->nodes[nm_multishot_owner_node_index(rt, shard->io_node_index, req->fd)];
    if (!node->ring_ready || !node->supports_multishot_poll) {
        errno = EAGAIN;
        return -1;
    }
    req->attached_node_index = node->index;

    pthread_mutex_lock(&node->watch_lock);
    watch = nm_get_or_create_poll_watch_locked(node, req->fd, req->poll_events);
    if (watch == NULL) {
        int saved_errno = errno;

        pthread_mutex_unlock(&node->watch_lock);
        errno = saved_errno != 0 ? saved_errno : ENOMEM;
        return -1;
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
            nm_drop_node_control_locked(node, NM_IO_CONTROL_POLL_DEACTIVATE, watch)) {
            watch->deactivate_queued = false;
        }
        pthread_mutex_unlock(&node->watch_lock);
        return 0;
    }

    if (watch->deactivate_queued) {
        if (!nm_drop_node_control_locked(node, NM_IO_CONTROL_POLL_DEACTIVATE, watch)) {
            pthread_mutex_unlock(&node->watch_lock);
            errno = EAGAIN;
            return -1;
        }
        watch->deactivate_queued = false;
    }

    /*
     * A multishot poll CQE wakes the waiters that were already attached to the
     * watch. Poll readiness is level-triggered, so a waiter that arrives just
     * after that CQE must not park behind an already-readable fd.
     */
    immediate_rc = nm_platform_poll_now(req->fd, req->poll_events, &immediate_revents);
    if (immediate_rc != 0) {
        int saved_errno = errno;

        pthread_mutex_unlock(&node->watch_lock);
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

    if (!watch->active && !watch->activating) {
        if (nm_node_queue_control_locked(node, NM_IO_CONTROL_POLL_ACTIVATE, watch) != 0) {
            pthread_mutex_unlock(&node->watch_lock);
            errno = ENOMEM;
            return -1;
        }
        watch->activating = true;
        kick = true;
    }
    nm_poll_watch_enqueue_waiter(watch, req);
    pthread_mutex_unlock(&node->watch_lock);
    if (kick) {
        nm_kick_node(node);
    }

    atomic_store(&req->wait_mode, NM_IO_WAIT_MODE_POLL_WATCH);
    atomic_store(&req->abort_reason, NM_IO_ABORT_NONE);
    atomic_store(&req->cancel_queued, 0U);
    req->poll_watch = watch;
    req->accept_watch = NULL;
    return nm_park_io_req(req, false, 0U, NULL);
}

/**
 * @brief Issue an indefinite multishot accept watch or consume a ready accept.
 *
 * @param req Accept request with listener fd initialized.
 *
 * @return 0 on immediate or parked completion, -1 when multishot accept is not
 *         available or setup fails.
 */
int nm_issue_multishot_accept(nm_io_req_t *req) {
    nm_runtime_t *rt = &g_nm_runtime;
    nm_shard_t *shard = g_nm_tls_shard;
    nm_node_t *node;
    nm_accept_watch_t *watch;
    int ready_fd;
    bool kick = false;

    if (req == NULL || shard == NULL || g_nm_tls_task == NULL) {
        errno = EINVAL;
        return -1;
    }

    /* Accept multishot is only safe when the runtime owns buffering for the watch lifetime. */
    node = &rt->nodes[nm_multishot_owner_node_index(rt, shard->io_node_index, req->fd)];
    if (!node->ring_ready || !node->supports_multishot_accept) {
        errno = EAGAIN;
        return -1;
    }
    req->attached_node_index = node->index;

    pthread_mutex_lock(&node->watch_lock);
    watch = nm_get_or_create_accept_watch_locked(node, req->fd);
    if (watch == NULL) {
        pthread_mutex_unlock(&node->watch_lock);
        errno = ENOMEM;
        return -1;
    }
    if (watch->migrate_target_node_index != UINT_MAX && watch->migrate_target_node_index != node->index) {
        watch->migrate_target_node_index = UINT_MAX;
        watch->live_transferred = false;
    }

    ready_fd = nm_accept_watch_pop_ready(watch);
    if (ready_fd >= 0) {
        req->result = ready_fd;
        req->error_code = 0;
        pthread_mutex_unlock(&node->watch_lock);
        return 0;
    }

    if (!watch->active && !watch->activating) {
        if (nm_node_queue_control_locked(node, NM_IO_CONTROL_ACCEPT_ACTIVATE, watch) != 0) {
            pthread_mutex_unlock(&node->watch_lock);
            errno = ENOMEM;
            return -1;
        }
        watch->activating = true;
        kick = true;
    }
    nm_accept_watch_enqueue_waiter(watch, req);
    pthread_mutex_unlock(&node->watch_lock);
    if (kick) {
        nm_kick_node(node);
    }

    atomic_store(&req->wait_mode, NM_IO_WAIT_MODE_ACCEPT_WATCH);
    atomic_store(&req->abort_reason, NM_IO_ABORT_NONE);
    atomic_store(&req->cancel_queued, 0U);
    req->poll_watch = NULL;
    req->accept_watch = watch;
    req->recv_watch = NULL;
    return nm_park_io_req(req, false, 0U, NULL);
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
int nm_issue_multishot_recv(nm_io_req_t *req) {
    nm_runtime_t *rt = &g_nm_runtime;
    nm_shard_t *shard = g_nm_tls_shard;
    nm_node_t *node;
    nm_recv_watch_t *watch;
    size_t ready_size = 0U;
    unsigned short ready_bid = 0U;
    bool ready_has_buffer = false;
    unsigned ready_node_index = UINT_MAX;
    unsigned char *ready_copy_data = NULL;
    size_t ready_copy_capacity = 0U;
    bool kick = false;

    if (req == NULL || shard == NULL || g_nm_tls_task == NULL || req->owned_buffer == NULL) {
        errno = EINVAL;
        return -1;
    }

    node = &rt->nodes[nm_multishot_owner_node_index(rt, shard->io_node_index, req->fd)];
    if (!node->ring_ready || !node->supports_multishot_recv) {
        errno = EAGAIN;
        return -1;
    }
    req->attached_node_index = node->index;

    pthread_mutex_lock(&node->watch_lock);
    watch = nm_get_or_create_recv_watch_locked(node, req->fd);
    if (watch == NULL) {
        int saved_errno = errno;

        pthread_mutex_unlock(&node->watch_lock);
        errno = saved_errno != 0 ? saved_errno : ENOMEM;
        return -1;
    }
    if (watch->migrate_target_node_index != UINT_MAX && watch->migrate_target_node_index != node->index) {
        watch->migrate_target_node_index = UINT_MAX;
        watch->live_transferred = false;
    }
    if (watch->active && atomic_load_explicit(&node->pending_ops, memory_order_acquire) == 0U) {
        watch->active = false;
        watch->deactivate_queued = false;
    }

    if (nm_recv_watch_pop_ready(watch,
                                &ready_size,
                                &ready_bid,
                                &ready_has_buffer,
                                &ready_node_index,
                                &ready_copy_data,
                                &ready_copy_capacity)) {
        if (ready_has_buffer &&
            ready_node_index < rt->active_nodes &&
            rt->nodes[ready_node_index].recv_buf_storage != NULL) {
            nm_node_t *ready_node = &rt->nodes[ready_node_index];

            req->owned_buffer->provided_storage = true;
            req->owned_buffer->provided_node_index = ready_node->index;
            req->owned_buffer->provided_bid = ready_bid;
            req->owned_buffer->data = ready_node->recv_buf_storage + ((size_t)ready_bid * NM_IO_BUFFER_INLINE_BYTES);
            req->owned_buffer->capacity = NM_IO_BUFFER_INLINE_BYTES;
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
            req->owned_buffer->capacity = NM_IO_BUFFER_INLINE_BYTES;
            req->owned_buffer->external_storage = false;
            req->use_provided_buffer = false;
        }
        req->owned_buffer->size = ready_size;
        req->result = (ssize_t)ready_size;
        req->error_code = 0;
        req->provided_bid = ready_has_buffer ? ready_bid : 0U;
        nm_maybe_destroy_recv_watch_locked(node, watch);
        pthread_mutex_unlock(&node->watch_lock);
        return 0;
    }

    if (watch->deactivate_queued) {
        if (!watch->active || !nm_drop_node_control_locked(node, NM_IO_CONTROL_RECV_DEACTIVATE, watch)) {
            pthread_mutex_unlock(&node->watch_lock);
            errno = EAGAIN;
            return -1;
        }
        watch->deactivate_queued = false;
    }

    if (!watch->active && !watch->activating) {
        if (nm_node_queue_control_locked(node, NM_IO_CONTROL_RECV_ACTIVATE, watch) != 0) {
            pthread_mutex_unlock(&node->watch_lock);
            errno = ENOMEM;
            return -1;
        }
        watch->activating = true;
        kick = true;
    }
    nm_recv_watch_enqueue_waiter(watch, req);
    pthread_mutex_unlock(&node->watch_lock);
    if (kick) {
        nm_kick_node(node);
    }

    atomic_store(&req->wait_mode, NM_IO_WAIT_MODE_RECV_WATCH);
    atomic_store(&req->abort_reason, NM_IO_ABORT_NONE);
    atomic_store(&req->cancel_queued, 0U);
    req->poll_watch = NULL;
    req->accept_watch = NULL;
    req->recv_watch = watch;
    return nm_park_io_req(req, false, 0U, NULL);
}

/**
 * @brief Issue a one-shot I/O request through the current shard's I/O node.
 *
 * This is the fallback/general path for operations that cannot use an
 * indefinite shared watch.  The request is queued under node->submit_lock,
 * counted as pending backend work, and then the owning task parks until the
 * backend completion path wakes it.
 *
 * @param req I/O request to submit.
 * @param has_deadline true when @p deadline_ns should be armed.
 * @param deadline_ns Absolute deadline in runtime monotonic nanoseconds.
 *
 * @return 0 on completion, -1 when the backend cannot accept the request or
 *         completion reports an error.
 */
int nm_issue_io(nm_io_req_t *req, bool has_deadline, uint64_t deadline_ns) {
    nm_runtime_t *rt = &g_nm_runtime;
    nm_shard_t *shard = g_nm_tls_shard;
    nm_task_t *task = g_nm_tls_task;
    nm_node_t *node;

    if (task == NULL || shard == NULL) {
        return 0;
    }

    node = &rt->nodes[shard->io_node_index];
    if (!node->ring_ready ||
        (req->kind == NM_IO_KIND_READ && req->use_recv_op ? !node->supports_recv : !nm_node_supports_kind(node, req->kind))) {
        node->unsupported_ops += 1U;
        shard->metrics.io_fallbacks += 1U;
        errno = EAGAIN;
        return -1;
    }

    req->task = task;
    req->result = -1;
    req->error_code = 0;
    req->owner_shard = shard->id;
    req->attached_node_index = node->index;
    req->submit_ts_ns = nm_now_ns();
    atomic_store(&req->wait_mode, NM_IO_WAIT_MODE_SUBMIT_QUEUE);
    atomic_store(&req->abort_reason, NM_IO_ABORT_NONE);
    atomic_store(&req->cancel_queued, 0U);
    req->poll_watch = NULL;
    req->accept_watch = NULL;
    req->recv_watch = NULL;

    pthread_mutex_lock(&node->submit_lock);
    nm_queue_node_submit_locked(node, req);
    pthread_mutex_unlock(&node->submit_lock);
    atomic_fetch_add(&node->pending_ops, 1U);
    if (nm_park_io_req(req, has_deadline, deadline_ns, node) != 0) {
        return -1;
    }
    return 0;
}
