/**
 * @file src/io/api/issue.c
 * @brief I/O request issue path, watch lookup, and readiness submission logic.
 *
 * @details
 * This is the handoff point between task-facing I/O APIs and the backend:
 * task bootstrap/exit, wait setup/cleanup, shared watch paths, and one-shot
 * queue issue. Watch operations recheck readiness before parking to avoid
 * missing level-triggered events between CQE delivery and waiter insertion.
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

#if LLAM_RUNTIME_BACKEND_LINUX
#include "io/linux/runtime_io_segment_linux_internal.h"
#endif

#if defined(LLAM_ENABLE_TEST_HOOKS)
static llam_io_park_snapshot_hook_fn
    g_llam_io_park_snapshot_hook;

void llam_io_test_set_park_snapshot_hook(
    llam_io_park_snapshot_hook_fn hook) {
    g_llam_io_park_snapshot_hook = hook;
}

static void llam_io_test_park_snapshot(
    llam_io_req_t *req,
    unsigned observed_wait_mode) {
    if (g_llam_io_park_snapshot_hook != NULL) {
        g_llam_io_park_snapshot_hook(
            req,
            observed_wait_mode);
    }
}
#else
static void llam_io_test_park_snapshot(
    llam_io_req_t *req,
    unsigned observed_wait_mode) {
    (void)req;
    (void)observed_wait_mode;
}
#endif

/**
 * @brief Resolve the runtime that owns an I/O request.
 *
 * @details
 * Request objects are stamped with an owner runtime when they are allocated from
 * a task/shard cache.  Setup and abort paths must use that owner instead of the
 * default runtime so explicit runtime handles can run concurrently without
 * routing completions or cache returns to the wrong backend.
 */
static llam_runtime_t *llam_io_request_runtime(const llam_io_req_t *req) {
    if (req != NULL && req->owner_runtime != NULL) {
        return req->owner_runtime;
    }
    if (g_llam_tls_task != NULL && g_llam_tls_task->owner_runtime != NULL) {
        return g_llam_tls_task->owner_runtime;
    }
    return g_llam_tls_shard != NULL ? g_llam_tls_shard->runtime : NULL;
}

/**
 * @brief Normalize an I/O setup failure onto the request object.
 *
 * Public wrappers sometimes inspect the request result after an attempted
 * multishot watch setup.  Request objects reset to result=0, so every hard
 * setup failure must publish a negative result before returning or callers can
 * mistake ENOMEM/setup failure for a clean timeout/no-readiness result.
 *
 * @param req        Request being prepared; may be NULL for defensive callers.
 * @param error_code Concrete errno value to report.  Zero is normalized to
 *                   EIO because zero is not a valid failure contract.
 *
 * @return Always -1 with @c errno set to the normalized error.
 */
static int llam_fail_io_setup_req(llam_io_req_t *req, int error_code) {
    int saved_errno = error_code != 0 ? error_code : EIO;

    if (req != NULL) {
        req->result = -1;
        req->fd_result = LLAM_INVALID_FD;
        req->error_code = saved_errno;
        req->poll_revents = 0;
    }
    errno = saved_errno;
    return -1;
}

/**
 * @brief Finish the current task and switch back to the scheduler context.
 *
 * This is the terminal path for task fibers.  It marks the task dead, wakes
 * join waiters, decrements shard-local live-task accounting, and requests
 * runtime stop when the last task exits.
 *
 * @note This function does not return.  It assumes g_llam_tls_task and
 *       g_llam_tls_shard identify the running task and owner shard.
 */
void llam_task_exit_internal(void) {
    llam_runtime_t *rt;
    llam_task_t *task = g_llam_tls_task;

    if (LLAM_UNLIKELY(task == NULL ||
                      g_llam_tls_shard == NULL ||
                      task->owner_runtime == NULL ||
                      g_llam_tls_shard->runtime != task->owner_runtime)) {
        abort();
    }
    rt = task->owner_runtime;

    pthread_mutex_lock(&task->lock);
    task->state = LLAM_TASK_STATE_DEAD;
    task->wait_reason = LLAM_WAIT_NONE;
    atomic_store_explicit(&task->completed, 1U, memory_order_release);
    pthread_mutex_unlock(&task->lock);
    llam_trace_shard(g_llam_tls_shard, task, LLAM_TRACE_STATE, LLAM_TASK_STATE_RUNNING, LLAM_TASK_STATE_DEAD, LLAM_WAIT_NONE);
    llam_reinject_join_waiters(rt, task);

    if (llam_runtime_note_task_dead(rt, task)) {
        llam_request_stop(rt);
    }

    llam_task_sample_live_stack(task);
    llam_switch_task_to_scheduler(task,
                                g_llam_tls_scheduler_ctx != NULL ? g_llam_tls_scheduler_ctx : &g_llam_tls_shard->scheduler_ctx);
    abort();
}

/**
 * @brief Enter a task fiber, run its user callback, and finalize task exit.
 *
 * @param task Task object whose entry/arg fields are already initialized.
 *
 * @note Called from the architecture-specific fiber bootstrap path.
 */
void llam_task_bootstrap(llam_task_t *task) {
    llam_runtime_t *rt;

    g_llam_tls_task = task;
    /* Fail closed before corrupt cursors or NULL entry become arbitrary crashes. */
    if (LLAM_UNLIKELY(task == NULL ||
                      task->entry == NULL ||
                      g_llam_tls_shard == NULL ||
                      task->owner_runtime == NULL ||
                      g_llam_tls_shard->runtime != task->owner_runtime)) {
        abort();
    }
    rt = task->owner_runtime;
    llam_task_restore_errno(task);
    if (rt->run_timing_enabled != 0U || rt->profile == LLAM_RUNTIME_PROFILE_DEBUG_SAFE) {
        atomic_store_explicit(&g_llam_tls_shard->last_safepoint_ns, llam_now_ns(), memory_order_relaxed);
    }
    task->entry(task->arg);
    llam_task_exit_internal();
}

/**
 * @brief Abort after detecting an invalid fiber stack alignment.
 *
 * @param rsp Stack pointer value observed by the assembly bootstrap.
 *
 * @note This is a hard-fail diagnostic for ABI violations in context setup.
 */
void llam_fiber_alignment_violation(uint64_t rsp) {
    dprintf(STDERR_FILENO,
            "llam: fiber bootstrap stack misaligned rsp=0x%llx\n",
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
void llam_cleanup_io_wait_setup(llam_task_t *task, llam_io_req_t *req) {
    llam_runtime_t *rt;

    if (task == NULL || req == NULL) {
        return;
    }

    rt = llam_io_request_runtime(req);
    llam_cancel_token_unregister_task(task);
    llam_disarm_task_wait_deadline(task);
    if (rt != NULL) {
        for (;;) {
            unsigned mode = atomic_load_explicit(&req->wait_mode,
                                                 memory_order_acquire);

            if (mode == LLAM_IO_WAIT_MODE_SUBMIT_QUEUE) {
                llam_io_submit_detach_result_t result =
                    llam_detach_submit_req_current(req, NULL);

                if (result == LLAM_IO_SUBMIT_DETACH_OWNER_CHANGED) {
                    continue;
                }
                break;
            }
            if (mode == LLAM_IO_WAIT_MODE_INFLIGHT) {
                /* This cleanup API has no completion handoff result. */
                llam_record_fatal_deferred(rt, EPROTO);
                abort();
            }
            if (mode == LLAM_IO_WAIT_MODE_POLL_WATCH ||
                mode == LLAM_IO_WAIT_MODE_ACCEPT_WATCH ||
                mode == LLAM_IO_WAIT_MODE_RECV_WATCH) {
                int node_index = llam_io_req_node_index(req);

                if (node_index >= 0 &&
                    (unsigned)node_index < rt->active_nodes) {
                    llam_node_t *node = &rt->nodes[(unsigned)node_index];

                    if (!llam_remove_watch_waiter_after_abort(node,
                                                              req,
                                                              mode,
                                                              false)) {
                        unsigned attached_node_index = atomic_load_explicit(
                            &req->attached_node_index,
                            memory_order_acquire);

                        if (atomic_load_explicit(&req->wait_mode,
                                                 memory_order_acquire) != mode ||
                            (attached_node_index < rt->active_nodes &&
                             attached_node_index != (unsigned)node_index) ||
                            (attached_node_index >= rt->active_nodes &&
                             llam_io_req_node_index(req) != node_index)) {
                            continue;
                        }
                    }
                }
            }
            break;
        }
    }
    atomic_store(&req->wait_mode, LLAM_IO_WAIT_MODE_NONE);
    atomic_store(&req->abort_reason, LLAM_IO_ABORT_NONE);
    atomic_store(&req->cancel_queued, 0U);
    atomic_store(&req->inflight_owner_shard, UINT_MAX);
    req->poll_watch = NULL;
    req->accept_watch = NULL;
    req->recv_watch = NULL;
    req->deadline_ns = 0U;
    task->state = LLAM_TASK_STATE_RUNNING;
    task->wait_reason = LLAM_WAIT_NONE;
    llam_task_clear_wait_tracking_or_abort(task);
}

/**
 * @brief Queue backend cancellation for a request that became in-flight during setup.
 *
 * A cancellation token may already be cancelled by the time the task registers
 * after publishing an I/O request. If the backend worker has already taken the
 * request out of the submit queue, the request must stay owned by the backend
 * until its cancellation completion arrives.
 */
static bool llam_abort_inflight_io_setup(llam_io_req_t *req, llam_io_abort_reason_t reason) {
    llam_runtime_t *rt;
    int node_index;

    if (req == NULL) {
        return false;
    }
    rt = llam_io_request_runtime(req);
    if (rt == NULL) {
        return false;
    }
    for (;;) {
        unsigned attached_node_index;
        llam_node_t *node;

        if (atomic_load_explicit(&req->wait_mode,
                                 memory_order_acquire) !=
            LLAM_IO_WAIT_MODE_INFLIGHT) {
            return false;
        }
        node_index = llam_io_req_node_index(req);
        if (node_index < 0 || (unsigned)node_index >= rt->active_nodes) {
            return false;
        }
        attached_node_index = atomic_load_explicit(&req->attached_node_index,
                                                   memory_order_acquire);
        if (attached_node_index < rt->active_nodes &&
            attached_node_index != (unsigned)node_index) {
            continue;
        }
        node = &rt->nodes[(unsigned)node_index];
        atomic_store_explicit(&req->abort_reason,
                              (unsigned)reason,
                              memory_order_release);
        if (atomic_load_explicit(&req->wait_mode,
                                 memory_order_acquire) !=
                LLAM_IO_WAIT_MODE_INFLIGHT ||
            atomic_load_explicit(&req->attached_node_index,
                                 memory_order_acquire) !=
                attached_node_index) {
            continue;
        }
        if (atomic_exchange_explicit(&req->cancel_queued,
                                     1U,
                                     memory_order_acq_rel) == 0U &&
            llam_node_queue_control(node,
                                    LLAM_IO_CONTROL_REQ_CANCEL,
                                    req) != 0) {
            /*
             * The request is already backend-owned. Returning false here would
             * let setup cleanup release storage still used by the backend.
             */
            atomic_store_explicit(&req->cancel_queued,
                                  0U,
                                  memory_order_release);
        }
        return true;
    }
}

/**
 * @brief Abort a request that has already been published during park setup.
 *
 * Once a request is linked into a submit queue or watch, plain setup cleanup is
 * no longer enough: the request must first be detached from that owner, or, if a
 * backend worker already took it, the task must wait for backend completion.
 *
 * @param req                 Published request.
 * @param reason              Abort reason to report if the backend completes a cancel.
 * @param wait_for_completion Set true when backend/completion ownership remains.
 * @return true when the caller can safely either cleanup immediately or keep
 *         parking for completion; false when ownership could not be established.
 */
static bool llam_abort_published_io_setup(llam_io_req_t *req,
                                          llam_io_abort_reason_t reason,
                                          bool *wait_for_completion) {
    llam_runtime_t *rt;

    if (wait_for_completion != NULL) {
        *wait_for_completion = false;
    }
    if (req == NULL) {
        return false;
    }

    rt = llam_io_request_runtime(req);
    if (rt == NULL) {
        return false;
    }
    for (;;) {
        unsigned mode = atomic_load_explicit(&req->wait_mode,
                                             memory_order_acquire);

        if (mode == LLAM_IO_WAIT_MODE_NONE) {
            if (wait_for_completion != NULL) {
                *wait_for_completion = true;
            }
            return true;
        }
        if (mode == LLAM_IO_WAIT_MODE_INFLIGHT) {
            if (!llam_abort_inflight_io_setup(req, reason)) {
                continue;
            }
            if (wait_for_completion != NULL) {
                *wait_for_completion = true;
            }
            return true;
        }
        if (mode == LLAM_IO_WAIT_MODE_SUBMIT_QUEUE) {
            llam_io_submit_detach_result_t result =
                llam_detach_submit_req_current(req, NULL);

            if (result == LLAM_IO_SUBMIT_DETACH_OWNER_CHANGED) {
                continue;
            }
            if (result == LLAM_IO_SUBMIT_DETACH_REMOVED) {
                llam_io_set_abort_result(req, reason);
                return true;
            }
            /* All submit locks proved that no backend queue owns this setup. */
            return false;
        }
        if (mode == LLAM_IO_WAIT_MODE_POLL_WATCH ||
            mode == LLAM_IO_WAIT_MODE_ACCEPT_WATCH ||
            mode == LLAM_IO_WAIT_MODE_RECV_WATCH) {
            int node_index = llam_io_req_node_index(req);

            if (node_index < 0 || (unsigned)node_index >= rt->active_nodes) {
                return false;
            }
            if (llam_remove_watch_waiter_after_abort(
                    &rt->nodes[(unsigned)node_index],
                    req,
                    mode,
                    true)) {
                llam_io_set_abort_result(req, reason);
                return true;
            }
            {
                unsigned attached_node_index = atomic_load_explicit(
                    &req->attached_node_index,
                    memory_order_acquire);

                if (atomic_load_explicit(&req->wait_mode,
                                         memory_order_acquire) != mode ||
                    (attached_node_index < rt->active_nodes &&
                     attached_node_index != (unsigned)node_index) ||
                    (attached_node_index >= rt->active_nodes &&
                     llam_io_req_node_index(req) != node_index)) {
                    continue;
                }
            }
            /* A completion detached this waiter and owns the final wake. */
            if (wait_for_completion != NULL) {
                *wait_for_completion = true;
            }
            return true;
        }
        return false;
    }
}

#if defined(LLAM_ENABLE_TEST_HOOKS)
bool llam_io_test_abort_published_io_setup(llam_io_req_t *req,
                                           llam_io_abort_reason_t reason,
                                           bool *wait_for_completion) {
    return llam_abort_published_io_setup(req, reason, wait_for_completion);
}
#endif

/**
 * @brief Prepare the current task before publishing it to a backend wait owner.
 *
 * Backend completions can race with both shared watch insertion and one-shot
 * submit-queue publication.  Publish the parked task state and initial request
 * result before making @p req visible so an early completion cannot be
 * overwritten by the generic park path.
 *
 * @param req         Request that will be linked to a backend owner.
 * @param wait_mode   Wait mode to publish.
 * @param deadline_ns Absolute deadline to store on the request, or 0.
 *
 * @return 0 on success, -1 with errno set for invalid context.
 */
int llam_prepare_io_wait(
    llam_io_req_t *req,
    llam_io_wait_mode_t wait_mode,
    uint64_t deadline_ns) {
    llam_shard_t *shard = g_llam_tls_shard;
    llam_task_t *task = g_llam_tls_task;

    if (shard == NULL || task == NULL || req == NULL) {
        errno = EINVAL;
        return -1;
    }

    req->task = task;
    req->result = -1;
    req->error_code = 0;
    atomic_store_explicit(&req->owner_shard,
                          shard->id,
                          memory_order_release);
    req->submit_ts_ns = llam_now_ns();
    req->deadline_ns = deadline_ns;
    atomic_store(&req->wait_mode, wait_mode);
    atomic_store(&req->abort_reason, LLAM_IO_ABORT_NONE);
    atomic_store(&req->cancel_queued, 0U);

    llam_task_ensure_listed(task);
    if (!llam_task_set_io_tracking(task, req, shard->id)) {
        int saved_errno = errno;

        atomic_store_explicit(&req->wait_mode,
                              LLAM_IO_WAIT_MODE_NONE,
                              memory_order_release);
        req->task = NULL;
        errno = saved_errno;
        return -1;
    }
    shard->metrics.io_submits += 1U;
    shard->metrics.parks += 1U;
    llam_trace_shard(shard, task, LLAM_TRACE_IO_SUBMIT, LLAM_TASK_STATE_RUNNING, LLAM_TASK_STATE_PARKED, LLAM_WAIT_IO);
    return 0;
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
int llam_park_io_req(llam_io_req_t *req, bool has_deadline, uint64_t deadline_ns, llam_node_t *wake_node) {
    llam_shard_t *shard = g_llam_tls_shard;
    llam_task_t *task = g_llam_tls_task;
    unsigned wait_mode;
    bool already_prepared;

    if (shard == NULL || task == NULL) {
        errno = EINVAL;
        return -1;
    }

    wait_mode = atomic_load_explicit(&req->wait_mode, memory_order_acquire);
    llam_io_test_park_snapshot(req, wait_mode);
    /*
     * One-shot submit and shared-watch paths prepare the task before exposing
     * req to backend threads.  If an immediate backend completion wins before
     * this function runs, wait_mode is already NONE and the completion path has
     * queued this task; do not reinitialize the result or wait ownership. The
     * initial snapshot can become stale while the completion clears task
     * tracking, so the completed-state branch must reload wait_mode.
     */
    already_prepared = (req->task == task &&
                        ((llam_task_active_io_req_load(task) == req &&
                          task->state == LLAM_TASK_STATE_PARKED &&
                          (llam_wait_reason_t)atomic_load_explicit(&task->wait_reason, memory_order_acquire) ==
                              LLAM_WAIT_IO) ||
                         atomic_load_explicit(
                             &req->wait_mode,
                             memory_order_acquire) ==
                             LLAM_IO_WAIT_MODE_NONE));
    if (!already_prepared) {
        req->task = task;
        req->result = -1;
        req->error_code = 0;
        atomic_store_explicit(&req->owner_shard,
                              shard->id,
                              memory_order_release);
        req->submit_ts_ns = llam_now_ns();
        req->deadline_ns = has_deadline ? deadline_ns : 0U;

        llam_task_ensure_listed(task);
        if (!llam_task_set_io_tracking(task, req, shard->id)) {
            int saved_errno = errno;

            atomic_store_explicit(&req->wait_mode,
                                  LLAM_IO_WAIT_MODE_NONE,
                                  memory_order_release);
            req->task = NULL;
            errno = saved_errno;
            return -1;
        }
        shard->metrics.io_submits += 1U;
        shard->metrics.parks += 1U;
        llam_trace_shard(shard, task, LLAM_TRACE_IO_SUBMIT, LLAM_TASK_STATE_RUNNING, LLAM_TASK_STATE_PARKED, LLAM_WAIT_IO);
    }
    if (has_deadline && atomic_load_explicit(&req->wait_mode, memory_order_acquire) != LLAM_IO_WAIT_MODE_NONE) {
        if (llam_arm_task_wait_deadline(task, shard, deadline_ns) != 0) {
            int saved_errno = errno;
            bool wait_for_completion = false;

            req->error_code = saved_errno != 0 ? saved_errno : ENOMEM;
            if (llam_abort_published_io_setup(req, LLAM_IO_ABORT_ERROR, &wait_for_completion) &&
                wait_for_completion) {
                /* Backend completion now owns the final wake/result. */
            } else {
                llam_cleanup_io_wait_setup(task, req);
                errno = saved_errno;
                return -1;
            }
        }
        if (atomic_load_explicit(&req->wait_mode, memory_order_acquire) == LLAM_IO_WAIT_MODE_NONE) {
            llam_disarm_task_wait_deadline(task);
        }
    }
    if (task->cancel_token != NULL && atomic_load_explicit(&req->wait_mode, memory_order_acquire) != LLAM_IO_WAIT_MODE_NONE) {
        if (llam_cancel_token_register_task(task) != 0) {
            int saved_errno = errno;
            bool wait_for_completion = false;

            if (saved_errno == ECANCELED &&
                llam_abort_published_io_setup(req, LLAM_IO_ABORT_CANCEL, &wait_for_completion)) {
                if (wait_for_completion) {
                    /* Backend completion will wake this task with ECANCELED. */
                } else {
                    if (has_deadline) {
                        llam_disarm_task_wait_deadline(task);
                    }
                    llam_cleanup_io_wait_setup(task, req);
                    errno = saved_errno;
                    return -1;
                }
            } else {
                if (has_deadline) {
                    llam_disarm_task_wait_deadline(task);
                }
                llam_cleanup_io_wait_setup(task, req);
                errno = saved_errno;
                return -1;
            }
        }
        if (atomic_load_explicit(&req->wait_mode, memory_order_acquire) == LLAM_IO_WAIT_MODE_NONE) {
            llam_cancel_token_unregister_task(task);
        }
    }
    if (wake_node != NULL) {
        llam_kick_node(wake_node);
    }
    llam_task_sample_live_stack(task);
    llam_switch_task_to_scheduler(task, g_llam_tls_scheduler_ctx != NULL ? g_llam_tls_scheduler_ctx : &shard->scheduler_ctx);
    if (has_deadline) {
        // Fast I/O completion can win the race before deadline setup is fully
        // visible to the wake path.  Disarm defensively after the task resumes.
        llam_disarm_task_wait_deadline(task);
    }
    llam_cancel_token_unregister_task(task);
    llam_task_clear_wait_tracking_or_abort(task);
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
int llam_issue_io(llam_io_req_t *req, bool has_deadline, uint64_t deadline_ns) {
    llam_runtime_t *rt;
    llam_shard_t *shard = g_llam_tls_shard;
    llam_task_t *task = g_llam_tls_task;
    llam_node_t *node;
    bool kind_supported;

    if (task == NULL || shard == NULL) {
        return 0;
    }
    rt = llam_io_request_runtime(req);
    if (rt == NULL || shard->runtime != rt) {
        errno = EXDEV;
        return -1;
    }

    node = &rt->nodes[shard->io_node_index];
    kind_supported =
        req->kind == LLAM_IO_KIND_READ && req->use_recv_op
            ? node->supports_recv
            : req->kind == LLAM_IO_KIND_WRITE && req->use_send_op
                ? node->supports_send
            : llam_node_supports_kind(node, req->kind);
#if LLAM_RUNTIME_BACKEND_KQUEUE
    /*
     * Generic public read/write keeps using its conservative blocking fallback
     * on kqueue. A trusted internal completion sink, however, can use the
     * existing one-shot request backend so it can retain ownership across a
     * composed runtime-effect region.
     */
    if (!kind_supported && req->completion_sink != NULL &&
        (req->kind == LLAM_IO_KIND_READ ||
         req->kind == LLAM_IO_KIND_WRITE)) {
        kind_supported = true;
    }
#endif
    /*
     * Do not inspect a socket through the caller's descriptor before queuing a
     * poll. Windows submissions are generation-bound to a retained duplicate,
     * and ConnectEx/SO_UPDATE_CONNECT_CONTEXT is performed on that association
     * authority. The raw descriptor is still the lookup key, but it is not the
     * authoritative handle for post-connect capability state.
     *
     * llam_windows_submit_poll() validates the event mask, socket family, and
     * type after the association has been revalidated and pinned. Unsupported
     * requests complete with a capability error and retain the normal public
     * blocking-fallback contract.
     */
    if (!node->ring_ready || !kind_supported) {
        atomic_fetch_add_explicit(&node->unsupported_ops, 1U, memory_order_relaxed);
        shard->metrics.io_fallbacks += 1U;
        errno = EAGAIN;
        return -1;
    }

    req->task = task;
    req->result = -1;
    req->error_code = 0;
    atomic_store_explicit(&req->owner_shard,
                          shard->id,
                          memory_order_release);
    atomic_store_explicit(&req->attached_node_index,
                          node->index,
                          memory_order_release);
    req->submit_ts_ns = llam_now_ns();
    atomic_store(&req->abort_reason, LLAM_IO_ABORT_NONE);
    atomic_store(&req->cancel_queued, 0U);
    req->poll_watch = NULL;
    req->accept_watch = NULL;
    req->recv_watch = NULL;
    if (llam_prepare_io_wait(req, LLAM_IO_WAIT_MODE_SUBMIT_QUEUE, has_deadline ? deadline_ns : 0U) != 0) {
        return -1;
    }

    if (!llam_node_submit_io_req(node, req)) {
        int saved_errno = errno;

        llam_cleanup_io_wait_setup(task, req);
        return llam_fail_io_setup_req(req, saved_errno);
    }
    if (llam_park_io_req(req, has_deadline, deadline_ns, node) != 0) {
        return -1;
    }
    return 0;
}

#if LLAM_RUNTIME_BACKEND_LINUX
/**
 * @brief Submit one bounded native batch and park its task exactly once.
 */
int llam_issue_linux_native_batch(
    llam_linux_native_batch_t *batch,
    llam_io_req_t *req) {
    llam_shard_t *shard = g_llam_tls_shard;
    llam_task_t *task = g_llam_tls_task;
    llam_runtime_t *rt;
    llam_node_t *node;
    unsigned request_refs;
    unsigned i;
    int error = EINVAL;

    if (batch == NULL || req == NULL ||
        shard == NULL || task == NULL) {
        return llam_fail_io_setup_req(req, EINVAL);
    }
    rt = task->owner_runtime;
    if (rt == NULL ||
        shard->runtime != rt ||
        batch->owner_runtime != rt ||
        req->owner_runtime != rt) {
        return llam_fail_io_setup_req(req, EXDEV);
    }
    if (shard->id >= rt->active_shards ||
        shard->io_node_index >= rt->active_nodes ||
        rt->nodes == NULL) {
        return llam_fail_io_setup_req(req, EINVAL);
    }
    node = &rt->nodes[shard->io_node_index];
    request_refs = atomic_load_explicit(
        &req->lifetime_refs, memory_order_acquire);
    if (request_refs == 0U || request_refs == UINT_MAX ||
        atomic_load_explicit(
            &req->wait_mode,
            memory_order_acquire) != LLAM_IO_WAIT_MODE_NONE ||
        atomic_load_explicit(
            &batch->state,
            memory_order_acquire) !=
            LLAM_LINUX_NATIVE_BATCH_IDLE ||
        batch->segment_count == 0U ||
        batch->segment_count >
            LLAM_LINUX_NATIVE_BATCH_MAX_SEGMENTS) {
        return llam_fail_io_setup_req(req, EBUSY);
    }
    if (atomic_load_explicit(
            &rt->stop_requested, memory_order_acquire) ||
        atomic_load_explicit(
            &rt->shutdown_requested, memory_order_acquire)) {
        return llam_fail_io_setup_req(req, ESHUTDOWN);
    }
    if (!node->ring_ready || node->linux_submit_terminal) {
        return llam_fail_io_setup_req(req, EAGAIN);
    }
    for (i = 0U; i < batch->segment_count; i += 1U) {
        llam_linux_native_segment_t *segment =
            batch->segments[i];
        unsigned operation_index;

        if (segment == NULL ||
            segment->owner_runtime != rt ||
            segment->generation == 0U ||
            segment->op_count == 0U ||
            segment->op_count >
                LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS ||
            atomic_load_explicit(
                &segment->state,
                memory_order_acquire) !=
                LLAM_LINUX_NATIVE_SEGMENT_IDLE) {
            return llam_fail_io_setup_req(req, EINVAL);
        }
        if (segment->mode ==
                LLAM_LINUX_NATIVE_SEGMENT_LINK_CQE_SKIP &&
            (node->linux_ring_features &
             IORING_FEAT_CQE_SKIP) == 0U) {
            return llam_fail_io_setup_req(req, ENOTSUP);
        }
        for (operation_index = 0U;
             operation_index < segment->op_count;
             operation_index += 1U) {
            if (segment->ops[operation_index].kind ==
                LLAM_LINUX_NATIVE_OP_RECV) {
                if (node->supports_recv) {
                    continue;
                }
                error = EAGAIN;
                break;
            }
            if (segment->ops[operation_index].kind ==
                LLAM_LINUX_NATIVE_OP_SEND) {
                if (node->supports_send) {
                    continue;
                }
                error = EAGAIN;
                break;
            }
            error = EINVAL;
            break;
        }
        if (operation_index != segment->op_count) {
            return llam_fail_io_setup_req(req, error);
        }
    }

    if (llam_prepare_io_wait(
            req,
            LLAM_IO_WAIT_MODE_SUBMIT_QUEUE,
            0U) != 0) {
        return -1;
    }
    if (!llam_linux_native_batch_enqueue(
            node, batch, req)) {
        int saved_errno = errno != 0 ? errno : EIO;

        llam_cleanup_io_wait_setup(task, req);
        return llam_fail_io_setup_req(req, saved_errno);
    }
    batch->segments[0]->task_parks += 1U;
    return llam_park_io_req(req, false, 0U, node);
}

/**
 * @brief Preserve the width-one native API through a stack-owned batch ticket.
 */
int llam_issue_linux_native_segment(
    llam_linux_native_segment_t *segment,
    llam_io_req_t *req) {
    llam_linux_native_batch_t batch;

    if (segment == NULL) {
        return llam_fail_io_setup_req(req, EINVAL);
    }
    memset(&batch, 0, sizeof(batch));
    batch.owner_runtime = segment->owner_runtime;
    batch.segments[0] = segment;
    batch.segment_count = 1U;
    atomic_init(&batch.state, LLAM_LINUX_NATIVE_BATCH_IDLE);
    atomic_init(&batch.terminal_claimed, 0U);
    atomic_init(
        &batch.cancel_state,
        LLAM_LINUX_NATIVE_CANCEL_NONE);
    atomic_init(&batch.cancel_requested, 0U);
    return llam_issue_linux_native_batch(&batch, req);
}
#endif
