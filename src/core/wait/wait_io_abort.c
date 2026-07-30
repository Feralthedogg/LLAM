/**
 * @file src/core/wait/wait_io_abort.c
 * @brief Cancellation and timeout transitions for active I/O waits.
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

#include "runtime_internal.h"

/**
 * @brief Consume and clear a task wake error code.
 *
 * @param task Task to inspect.
 * @return Stored wake error, or 0.
 */
int llam_consume_task_wake_error(llam_task_t *task) {
    int error_code = 0;

    if (task == NULL) {
        return 0;
    }

    error_code = atomic_exchange_explicit(&task->wake_error_code, 0, memory_order_acq_rel);
    return error_code;
}

/**
 * @brief Fill an I/O request with the result for timeout/cancel aborts.
 *
 * @param req    Request being aborted.
 * @param reason Abort reason.
 */
void llam_io_set_abort_result(llam_io_req_t *req, llam_io_abort_reason_t reason) {
    if (req == NULL) {
        return;
    }

    req->poll_revents = 0;
    if (reason == LLAM_IO_ABORT_TIMEOUT && req->kind == LLAM_IO_KIND_POLL) {
        // poll(2) reports timeout as a successful zero-result completion.
        req->result = 0;
        req->error_code = 0;
        return;
    }

    req->result = -1;
    if (reason == LLAM_IO_ABORT_ERROR) {
        if (req->error_code == 0) {
            req->error_code = EIO;
        }
    } else {
        req->error_code = reason == LLAM_IO_ABORT_TIMEOUT ? ETIMEDOUT : ECANCELED;
    }
}

/**
 * @brief Map an I/O abort reason to the scheduler wait reason used for wakeup.
 *
 * @param reason I/O abort reason.
 * @return Wait reason for metrics/tracing.
 */
llam_wait_reason_t llam_io_abort_wait_reason(llam_io_abort_reason_t reason) {
    switch (reason) {
    case LLAM_IO_ABORT_CANCEL:
        return LLAM_WAIT_CANCEL;
    case LLAM_IO_ABORT_TIMEOUT:
        return LLAM_WAIT_TIMEOUT;
    case LLAM_IO_ABORT_ERROR:
        return LLAM_WAIT_IO;
    default:
        return LLAM_WAIT_IO;
    }
}

/**
 * @brief Account timeout/cancel wake metrics on a shard.
 *
 * @param shard  Shard that will wake the task.
 * @param reason I/O abort reason.
 */
void llam_account_io_abort_wake(llam_shard_t *shard, llam_io_abort_reason_t reason) {
    if (shard == NULL) {
        return;
    }

    pthread_mutex_lock(&shard->lock);
    if (reason == LLAM_IO_ABORT_CANCEL) {
        shard->metrics.cancel_wakes += 1U;
    } else if (reason == LLAM_IO_ABORT_TIMEOUT) {
        shard->metrics.timeout_wakes += 1U;
    }
    pthread_mutex_unlock(&shard->lock);
}

/**
 * @brief Resolve the platform I/O node currently responsible for a request.
 *
 * @param req Request to inspect.
 * @return Node index on success, -1 if no node can be resolved.
 */
int llam_io_req_node_index(const llam_io_req_t *req) {
    llam_runtime_t *rt;
    unsigned attached_node_index;
    unsigned shard_id;

    rt = req != NULL ? req->owner_runtime : NULL;
    if (req == NULL || rt == NULL || rt->active_shards == 0U || rt->active_nodes == 0U) {
        return -1;
    }
    attached_node_index = atomic_load_explicit(&req->attached_node_index,
                                               memory_order_acquire);
    if (attached_node_index < rt->active_nodes) {
        return (int)attached_node_index;
    }

    // Requests that have not attached to a node yet route through their owner
    // shard's assigned I/O node.
    shard_id = atomic_load_explicit(&req->owner_shard, memory_order_acquire);
    shard_id = shard_id < rt->active_shards
                   ? shard_id
                   : (shard_id % rt->active_shards);
    return (int)rt->shards[shard_id].io_node_index;
}

/** @brief Revalidate the exact task wait and recyclable request activation. */
static bool llam_io_abort_owner_matches(const llam_task_t *task,
                                        const llam_io_req_t *req,
                                        const llam_runtime_t *rt,
                                        uint64_t operation_generation,
                                        uint64_t wait_generation) {
    return task != NULL && req != NULL && rt != NULL &&
           operation_generation != 0U &&
           req->owner_runtime == rt && req->task == task &&
           llam_task_active_io_req_load(task) == req &&
           atomic_load_explicit(&task->active_io_generation,
                                memory_order_acquire) == operation_generation &&
           atomic_load_explicit(&task->wait_generation,
                                memory_order_acquire) == wait_generation &&
           atomic_load_explicit(&task->state,
                                memory_order_acquire) == LLAM_TASK_STATE_PARKED &&
           atomic_load_explicit(&task->wait_reason,
                                memory_order_acquire) == LLAM_WAIT_IO &&
           atomic_load_explicit(&req->operation_generation,
                                memory_order_acquire) == operation_generation;
}

/**
 * @brief Abort a task's active I/O wait if it can be synchronously detached.
 *
 * @param task   Task parked on I/O.
 * @param reason Abort reason.
 * @param wait_generation Exact task wait epoch owned by the abort producer.
 * @return true if the task was detached and reinjected immediately.
 */
static bool llam_abort_io_wait_impl(llam_task_t *task,
                                    llam_io_abort_reason_t reason,
                                    uint64_t wait_generation,
                                    bool resolver_held) {
    llam_runtime_t *rt = llam_wait_task_runtime(task);
    llam_io_req_t *req;
    uint64_t operation_generation;
    int node_index;
    llam_node_t *node = NULL;
    llam_shard_t *shard;
    unsigned mode;
    bool removed = false;
    bool detached = false;
    unsigned parked_shard;

    if (task == NULL || rt == NULL || rt->active_shards == 0U ||
        wait_generation == 0U || wait_generation == UINT64_MAX) {
        if (resolver_held && task != NULL) {
            llam_task_wait_resolver_end(task);
        }
        return false;
    }

    /*
     * Pin immediately after the published pointer load.  The embedded request
     * may otherwise complete, reset, and represent a later operation at the
     * same address before cancellation reaches the control queue.
     */
    req = llam_task_active_io_req_load(task);
    if (req == NULL || !llam_io_req_lifetime_try_acquire(req)) {
        if (resolver_held) {
            llam_task_wait_resolver_end(task);
        }
        return false;
    }
    operation_generation = (uint64_t)atomic_load_explicit(&req->operation_generation,
                                                           memory_order_acquire);
    for (;;) {
        unsigned attached_node_index;

        if (!llam_io_abort_owner_matches(task,
                                         req,
                                         rt,
                                         operation_generation,
                                         wait_generation)) {
            goto release_req;
        }
        mode = atomic_load_explicit(&req->wait_mode, memory_order_acquire);
        node_index = llam_io_req_node_index(req);
        if (node_index < 0 || (unsigned)node_index >= rt->active_nodes) {
            goto release_req;
        }
        node = &rt->nodes[(unsigned)node_index];

        if (mode == LLAM_IO_WAIT_MODE_SUBMIT_QUEUE) {
#if LLAM_RUNTIME_BACKEND_LINUX && LLAM_BUILD_RESEARCH
            struct llam_linux_native_batch *native_batch =
                atomic_load_explicit(
                    &req->linux_native_batch,
                    memory_order_acquire);

            if (native_batch != NULL) {
                removed =
                    llam_linux_native_batch_abort_queued(
                        node, native_batch, req);
                if (removed) {
                    break;
                }
                if (atomic_load_explicit(
                        &req->linux_native_batch,
                        memory_order_acquire) ==
                        native_batch &&
                    atomic_load_explicit(
                        &req->wait_mode,
                        memory_order_acquire) ==
                        LLAM_IO_WAIT_MODE_SUBMIT_QUEUE) {
                    atomic_store_explicit(
                        &req->abort_reason,
                        (unsigned)reason,
                        memory_order_release);
                    llam_record_fatal_deferred(rt, EPROTO);
                    goto release_req;
                }
                continue;
            }
#endif
            unsigned detached_node_index = UINT_MAX;
            llam_io_submit_detach_result_t submit_result;

            /*
             * The shared helper excludes every cross-node evacuation and
             * resolves the exact queue. The resolver gate keeps task/wait
             * generations stable for the duration of that locked lookup.
             */
            submit_result = llam_detach_submit_req_current(
                req,
                &detached_node_index);
            if (submit_result == LLAM_IO_SUBMIT_DETACH_OWNER_CHANGED) {
                continue;
            }
            if (submit_result == LLAM_IO_SUBMIT_DETACH_REMOVED &&
                detached_node_index < rt->active_nodes) {
                node = &rt->nodes[detached_node_index];
                removed = true;
                break;
            }
            /*
             * Registered cancellation only observes published waits, so a
             * still-SUBMIT request absent from every locked queue is corrupt.
             * Preserve an abort latch for setup publication and stop closed.
             */
            atomic_store_explicit(&req->abort_reason,
                                  (unsigned)reason,
                                  memory_order_release);
            goto release_req;
        }

        attached_node_index = atomic_load_explicit(&req->attached_node_index,
                                                   memory_order_acquire);
        if (attached_node_index < rt->active_nodes &&
            attached_node_index != (unsigned)node_index) {
            continue;
        }
        if (mode == LLAM_IO_WAIT_MODE_POLL_WATCH ||
            mode == LLAM_IO_WAIT_MODE_ACCEPT_WATCH ||
            mode == LLAM_IO_WAIT_MODE_RECV_WATCH) {
            removed = llam_remove_watch_waiter_after_abort(node,
                                                           req,
                                                           mode,
                                                           true);
            if (removed) {
                break;
            }
            if (llam_io_abort_owner_matches(task,
                                            req,
                                            rt,
                                            operation_generation,
                                            wait_generation) &&
                (atomic_load_explicit(&req->wait_mode,
                                      memory_order_acquire) != mode ||
                 atomic_load_explicit(&req->attached_node_index,
                                      memory_order_acquire) !=
                     attached_node_index)) {
                continue;
            }
            /* A completion that already detached the waiter owns the wake. */
            goto release_req;
        }
        if (mode == LLAM_IO_WAIT_MODE_INFLIGHT) {
#if LLAM_RUNTIME_BACKEND_LINUX && LLAM_BUILD_RESEARCH
            struct llam_linux_native_batch *native_batch =
                atomic_load_explicit(
                    &req->linux_native_batch,
                    memory_order_acquire);
#endif

            if (!llam_io_abort_owner_matches(task,
                                             req,
                                             rt,
                                             operation_generation,
                                             wait_generation) ||
                atomic_load_explicit(&req->wait_mode,
                                     memory_order_acquire) !=
                    LLAM_IO_WAIT_MODE_INFLIGHT ||
                atomic_load_explicit(&req->attached_node_index,
                                     memory_order_acquire) !=
                    attached_node_index) {
                continue;
            }
#if LLAM_RUNTIME_BACKEND_LINUX && LLAM_BUILD_RESEARCH
            if (native_batch != NULL) {
                atomic_store_explicit(
                    &req->abort_reason,
                    (unsigned)reason,
                    memory_order_release);
                (void)llam_linux_native_batch_request_cancel(
                    node, native_batch, req);
                goto release_req;
            }
#endif
            /*
             * Backend ownership is stable after the submit-lock transition.
             * Queue cancellation on that current node; the control owns its
             * own request/task pins until completion or teardown.
             */
            atomic_store_explicit(&req->abort_reason,
                                  (unsigned)reason,
                                  memory_order_release);
            if (atomic_exchange_explicit(&req->cancel_queued,
                                         1U,
                                         memory_order_acq_rel) == 0U &&
                llam_node_queue_control(node,
                                        LLAM_IO_CONTROL_REQ_CANCEL,
                                        req) != 0) {
                atomic_store_explicit(&req->cancel_queued,
                                      0U,
                                      memory_order_release);
            }
            goto release_req;
        }
        goto release_req;
    }

    llam_io_set_abort_result(req, reason);
    parked_shard = atomic_load_explicit(&task->parked_shard,
                                        memory_order_acquire);
    shard = &rt->shards[parked_shard % rt->active_shards];
    if (resolver_held) {
        llam_task_wait_resolver_end(task);
        resolver_held = false;
    }
    llam_account_io_abort_wake(shard, reason);
    /*
     * Keep the wait ownership intact until the generic reinject path runs.
     * It still needs the original parked_shard to remove any active deadline
     * timer from the correct shard before clearing task tracking.
     */
    llam_reinject_task_on_shard(rt,
                              task,
                              parked_shard,
                              true,
                              LLAM_TRACE_WAKE,
                              llam_io_abort_wait_reason(reason));
    detached = true;

release_req:
    if (resolver_held) {
        llam_task_wait_resolver_end(task);
    }
    /* A queued control has acquired its own ref before this transient is put. */
    (void)llam_io_req_lifetime_release(req);
    return detached;
}

bool llam_abort_io_wait(llam_task_t *task,
                        llam_io_abort_reason_t reason,
                        uint64_t wait_generation) {
    return llam_abort_io_wait_impl(task, reason, wait_generation, false);
}

bool llam_abort_io_wait_claimed(llam_task_t *task,
                                llam_io_abort_reason_t reason,
                                uint64_t wait_generation) {
    return llam_abort_io_wait_impl(task, reason, wait_generation, true);
}
