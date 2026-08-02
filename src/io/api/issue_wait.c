/**
 * @file src/io/api/issue_wait.c
 * @brief I/O wait preparation, cancellation, cleanup, and park transitions.
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

#if LLAM_RUNTIME_BACKEND_LINUX && LLAM_BUILD_RESEARCH
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
llam_runtime_t *llam_io_request_runtime(const llam_io_req_t *req) {
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
int llam_fail_io_setup_req(llam_io_req_t *req, int error_code) {
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
#if LLAM_RUNTIME_BACKEND_LINUX && LLAM_BUILD_RESEARCH
        {
            llam_linux_native_batch_t *native_batch =
                atomic_load_explicit(
                    &req->linux_native_batch,
                    memory_order_acquire);

            if (native_batch != NULL) {
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
                (void)llam_linux_native_batch_request_cancel(
                    node, native_batch, req);
                return true;
            }
        }
#endif
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
#if LLAM_RUNTIME_BACKEND_LINUX && LLAM_BUILD_RESEARCH
            llam_linux_native_batch_t *native_batch =
                atomic_load_explicit(
                    &req->linux_native_batch,
                    memory_order_acquire);

            if (native_batch != NULL) {
                int node_index = llam_io_req_node_index(req);
                llam_node_t *node;

                if (node_index < 0 ||
                    (unsigned)node_index >= rt->active_nodes) {
                    return false;
                }
                node = &rt->nodes[(unsigned)node_index];
                if (llam_linux_native_batch_abort_queued(
                        node, native_batch, req)) {
                    llam_io_set_abort_result(req, reason);
                    return true;
                }
                if (atomic_load_explicit(
                        &req->linux_native_batch,
                        memory_order_acquire) != native_batch ||
                    atomic_load_explicit(
                        &req->wait_mode,
                        memory_order_acquire) !=
                        LLAM_IO_WAIT_MODE_SUBMIT_QUEUE) {
                    continue;
                }
                atomic_store_explicit(&req->abort_reason,
                                      (unsigned)reason,
                                      memory_order_release);
                llam_record_fatal_deferred(rt, EPROTO);
                if (wait_for_completion != NULL) {
                    *wait_for_completion = true;
                }
                return true;
            }
#endif
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
