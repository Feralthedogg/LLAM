/**
 * @file src/core/runtime_wait_tracking.c
 * @brief Runtime accounting for tasks blocked on waits or I/O.
 *
 * @details
 * A parked task records exactly which wait structure owns it: a synchronization
 * wait node, join target, sleep timer, I/O request, or blocking job. This file
 * centralizes those tracking transitions so timeout, cancellation, completion,
 * and reinjection paths can safely detach a task before making it runnable.
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
 * @brief Clear all wait ownership fields on a task.
 *
 * @param task Task whose wait tracking should be reset.
 */
void nm_task_clear_wait_tracking(nm_task_t *task) {
    if (task == NULL) {
        return;
    }

    if (task->active_io_req != NULL && g_nm_runtime.initialized) {
        // active_io_waiters tracks tasks, not requests. Decrement once when the
        // task leaves any I/O wait state.
        atomic_fetch_sub_explicit(&g_nm_runtime.active_io_waiters, 1U, memory_order_acq_rel);
    }

    task->active_wait_node = NULL;
    task->active_wait_queue = NULL;
    task->active_wait_queue_lock = NULL;
    task->active_io_req = NULL;
    task->active_block_job = NULL;
    task->join_target = NULL;
    task->parked_shard = task->last_shard;
}

/**
 * @brief Track that a task is parked on a synchronization wait node.
 *
 * @param task         Parked task.
 * @param node         Wait node owned by a primitive.
 * @param queue        Wait queue containing @p node.
 * @param queue_lock   Mutex protecting @p queue.
 * @param parked_shard Shard where the task parked.
 */
void nm_task_set_wait_node_tracking(nm_task_t *task,
                                           nm_wait_node_t *node,
                                           nm_wait_queue_t *queue,
                                           pthread_mutex_t *queue_lock,
                                           unsigned parked_shard) {
    task->active_wait_node = node;
    task->active_wait_queue = queue;
    task->active_wait_queue_lock = queue_lock;
    task->active_io_req = NULL;
    task->active_block_job = NULL;
    task->join_target = NULL;
    task->parked_shard = parked_shard;
    task->wake_error_code = 0;
}

/**
 * @brief Track that a task is parked waiting for another task to exit.
 *
 * @param task         Parked waiter.
 * @param target       Join target.
 * @param parked_shard Shard where the waiter parked.
 */
void nm_task_set_join_tracking(nm_task_t *task, nm_task_t *target, unsigned parked_shard) {
    task->active_wait_node = NULL;
    task->active_wait_queue = NULL;
    task->active_wait_queue_lock = NULL;
    task->active_io_req = NULL;
    task->active_block_job = NULL;
    task->join_target = target;
    task->parked_shard = parked_shard;
    task->wake_error_code = 0;
}

/**
 * @brief Track that a task is parked only on a sleep/deadline timer.
 *
 * @param task         Parked task.
 * @param parked_shard Shard where the task parked.
 */
void nm_task_set_sleep_tracking(nm_task_t *task, unsigned parked_shard) {
    task->active_wait_node = NULL;
    task->active_wait_queue = NULL;
    task->active_wait_queue_lock = NULL;
    task->active_io_req = NULL;
    task->active_block_job = NULL;
    task->join_target = NULL;
    task->parked_shard = parked_shard;
    task->wake_error_code = 0;
}

/**
 * @brief Track that a task is parked on an I/O request.
 *
 * @param task         Parked task.
 * @param req          Active I/O request.
 * @param parked_shard Shard where the task parked.
 */
void nm_task_set_io_tracking(nm_task_t *task, nm_io_req_t *req, unsigned parked_shard) {
    if (task == NULL) {
        return;
    }
    if (task->active_io_req == NULL && g_nm_runtime.initialized) {
        // Count the transition into I/O wait once per task.
        atomic_fetch_add_explicit(&g_nm_runtime.active_io_waiters, 1U, memory_order_acq_rel);
    }
    task->active_wait_node = NULL;
    task->active_wait_queue = NULL;
    task->active_wait_queue_lock = NULL;
    task->active_io_req = req;
    task->active_block_job = NULL;
    task->join_target = NULL;
    task->parked_shard = parked_shard;
    task->wake_error_code = 0;
}

/**
 * @brief Track that a task is parked on a runtime blocking job.
 *
 * @param task         Parked task.
 * @param job          Blocking job being executed by a helper.
 * @param parked_shard Shard where the task parked.
 */
void nm_task_set_block_tracking(nm_task_t *task, nm_block_job_t *job, unsigned parked_shard) {
    task->active_wait_node = NULL;
    task->active_wait_queue = NULL;
    task->active_wait_queue_lock = NULL;
    task->active_io_req = NULL;
    task->active_block_job = job;
    task->join_target = NULL;
    task->parked_shard = parked_shard;
    task->wake_error_code = 0;
}

/**
 * @brief Check whether an absolute deadline has passed.
 *
 * @param deadline_ns Absolute deadline in ::nm_now_ns units.
 * @return true when the deadline is now or in the past.
 */
bool nm_deadline_passed(uint64_t deadline_ns) {
    return deadline_ns <= nm_now_ns();
}

/**
 * @brief Arm a deadline timer for a parked task.
 *
 * Timed waits all funnel through the same timer node so timeout and cancellation
 * share one wake path.
 *
 * @param task        Task being parked.
 * @param shard       Timer owner shard.
 * @param deadline_ns Absolute wake deadline.
 * @return 0 on success, -1 on invalid args or timer allocation failure.
 */
int nm_arm_task_wait_deadline(nm_task_t *task, nm_shard_t *shard, uint64_t deadline_ns) {
    if (task == NULL || shard == NULL) {
        errno = EINVAL;
        return -1;
    }

    task->deadline_ns = deadline_ns;
    pthread_mutex_lock(&shard->lock);
    nm_timer_insert_locked(shard, task);
    pthread_mutex_unlock(&shard->lock);
    if (task->active_timer == NULL) {
        task->deadline_ns = 0U;
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

/**
 * @brief Remove a task's active wait deadline, if one exists.
 *
 * @param task Task whose timer should be removed.
 */
void nm_disarm_task_wait_deadline(nm_task_t *task) {
    nm_shard_t *shard;

    if (task == NULL || task->active_timer == NULL || g_nm_runtime.active_shards == 0U) {
        return;
    }

    if (task->parked_shard < g_nm_runtime.active_shards) {
        shard = &g_nm_runtime.shards[task->parked_shard];
    } else {
        // Fallback for defensive cleanup when parked_shard was not set or has
        // become invalid due to teardown/rehome state.
        shard = &g_nm_runtime.shards[task->last_shard % g_nm_runtime.active_shards];
    }

    pthread_mutex_lock(&shard->lock);
    if (task->active_timer != NULL) {
        (void)nm_timer_remove_locked(shard, task);
    }
    pthread_mutex_unlock(&shard->lock);
}

/**
 * @brief Register a task as a cancellation-token waiter.
 *
 * @param task Task with an optional cancel token.
 * @return 0 on success/no token, -1 with ECANCELED if already canceled.
 */
int nm_cancel_token_register_task(nm_task_t *task) {
    nm_cancel_token_t *token;

    if (task == NULL || task->cancel_token == NULL) {
        return 0;
    }

    token = task->cancel_token;
    pthread_mutex_lock(&token->lock);
    if (token->cancelled) {
        pthread_mutex_unlock(&token->lock);
        errno = ECANCELED;
        return -1;
    }

    if (!task->cancel_registered) {
        // Intrusive task links avoid allocating separate cancel wait nodes.
        task->cancel_prev = NULL;
        task->cancel_next = token->waiters;
        if (token->waiters != NULL) {
            token->waiters->cancel_prev = task;
        }
        token->waiters = task;
        task->cancel_registered = true;
    }
    pthread_mutex_unlock(&token->lock);
    return 0;
}

/**
 * @brief Remove a task from its cancellation-token waiter list.
 *
 * @param task Task to unregister.
 */
void nm_cancel_token_unregister_task(nm_task_t *task) {
    nm_cancel_token_t *token;

    if (task == NULL || task->cancel_token == NULL || !task->cancel_registered) {
        return;
    }

    token = task->cancel_token;
    pthread_mutex_lock(&token->lock);
    if (task->cancel_registered) {
        if (task->cancel_prev != NULL) {
            task->cancel_prev->cancel_next = task->cancel_next;
        } else {
            token->waiters = task->cancel_next;
        }
        if (task->cancel_next != NULL) {
            task->cancel_next->cancel_prev = task->cancel_prev;
        }
        task->cancel_prev = NULL;
        task->cancel_next = NULL;
        task->cancel_registered = false;
    }
    pthread_mutex_unlock(&token->lock);
}

/**
 * @brief Consume and clear a task wake error code.
 *
 * @param task Task to inspect.
 * @return Stored wake error, or 0.
 */
int nm_consume_task_wake_error(nm_task_t *task) {
    int error_code = 0;

    if (task == NULL) {
        return 0;
    }

    error_code = task->wake_error_code;
    task->wake_error_code = 0;
    return error_code;
}

/**
 * @brief Fill an I/O request with the result for timeout/cancel aborts.
 *
 * @param req    Request being aborted.
 * @param reason Abort reason.
 */
void nm_io_set_abort_result(nm_io_req_t *req, nm_io_abort_reason_t reason) {
    if (req == NULL) {
        return;
    }

    req->poll_revents = 0;
    if (reason == NM_IO_ABORT_TIMEOUT && req->kind == NM_IO_KIND_POLL) {
        // poll(2) reports timeout as a successful zero-result completion.
        req->result = 0;
        req->error_code = 0;
        return;
    }

    req->result = -1;
    req->error_code = reason == NM_IO_ABORT_TIMEOUT ? ETIMEDOUT : ECANCELED;
}

/**
 * @brief Map an I/O abort reason to the scheduler wait reason used for wakeup.
 *
 * @param reason I/O abort reason.
 * @return Wait reason for metrics/tracing.
 */
nm_wait_reason_t nm_io_abort_wait_reason(nm_io_abort_reason_t reason) {
    switch (reason) {
    case NM_IO_ABORT_CANCEL:
        return NM_WAIT_CANCEL;
    case NM_IO_ABORT_TIMEOUT:
        return NM_WAIT_TIMEOUT;
    default:
        return NM_WAIT_IO;
    }
}

/**
 * @brief Account timeout/cancel wake metrics on a shard.
 *
 * @param shard  Shard that will wake the task.
 * @param reason I/O abort reason.
 */
void nm_account_io_abort_wake(nm_shard_t *shard, nm_io_abort_reason_t reason) {
    if (shard == NULL) {
        return;
    }

    pthread_mutex_lock(&shard->lock);
    if (reason == NM_IO_ABORT_CANCEL) {
        shard->metrics.cancel_wakes += 1U;
    } else if (reason == NM_IO_ABORT_TIMEOUT) {
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
int nm_io_req_node_index(const nm_io_req_t *req) {
    unsigned shard_id;

    if (req == NULL || g_nm_runtime.active_shards == 0U || g_nm_runtime.active_nodes == 0U) {
        return -1;
    }
    if (req->attached_node_index < g_nm_runtime.active_nodes) {
        return (int)req->attached_node_index;
    }

    // Requests that have not attached to a node yet route through their owner
    // shard's assigned I/O node.
    shard_id = req->owner_shard < g_nm_runtime.active_shards ? req->owner_shard : (req->owner_shard % g_nm_runtime.active_shards);
    return (int)g_nm_runtime.shards[shard_id].io_node_index;
}

/**
 * @brief Abort a task's active I/O wait if it can be synchronously detached.
 *
 * @param task   Task parked on I/O.
 * @param reason Abort reason.
 * @return true if the task was detached and reinjected immediately.
 */
bool nm_abort_io_wait(nm_task_t *task, nm_io_abort_reason_t reason) {
    nm_io_req_t *req;
    int node_index;
    nm_node_t *node;
    nm_shard_t *shard;
    unsigned mode;
    bool removed = false;

    if (task == NULL || task->active_io_req == NULL || g_nm_runtime.active_shards == 0U) {
        return false;
    }

    req = task->active_io_req;
    node_index = nm_io_req_node_index(req);
    if (node_index < 0) {
        return false;
    }
    node = &g_nm_runtime.nodes[node_index];
    shard = &g_nm_runtime.shards[task->parked_shard % g_nm_runtime.active_shards];
    mode = atomic_load(&req->wait_mode);

    if (mode == NM_IO_WAIT_MODE_SUBMIT_QUEUE) {
        // Still queued for submission: remove directly from node submit queue.
        pthread_mutex_lock(&node->submit_lock);
        removed = nm_remove_node_submit_locked(node, req);
        if (removed) {
            atomic_store(&req->wait_mode, NM_IO_WAIT_MODE_NONE);
            atomic_store(&req->inflight_owner_shard, UINT_MAX);
        }
        pthread_mutex_unlock(&node->submit_lock);
        if (removed) {
            atomic_fetch_sub(&node->pending_ops, 1U);
        }
    } else if (mode == NM_IO_WAIT_MODE_POLL_WATCH && req->poll_watch != NULL) {
        pthread_mutex_lock(&node->watch_lock);
        removed = nm_poll_watch_remove_waiter(req->poll_watch, req);
        if (removed) {
            atomic_store(&req->wait_mode, NM_IO_WAIT_MODE_NONE);
            atomic_store(&req->inflight_owner_shard, UINT_MAX);
        }
        pthread_mutex_unlock(&node->watch_lock);
    } else if (mode == NM_IO_WAIT_MODE_ACCEPT_WATCH && req->accept_watch != NULL) {
        pthread_mutex_lock(&node->watch_lock);
        removed = nm_accept_watch_remove_waiter(req->accept_watch, req);
        if (removed) {
            atomic_store(&req->wait_mode, NM_IO_WAIT_MODE_NONE);
            atomic_store(&req->inflight_owner_shard, UINT_MAX);
        }
        pthread_mutex_unlock(&node->watch_lock);
    } else if (mode == NM_IO_WAIT_MODE_RECV_WATCH && req->recv_watch != NULL) {
        pthread_mutex_lock(&node->watch_lock);
        removed = nm_recv_watch_remove_waiter(req->recv_watch, req);
        if (removed) {
            atomic_store(&req->wait_mode, NM_IO_WAIT_MODE_NONE);
            atomic_store(&req->inflight_owner_shard, UINT_MAX);
        }
        pthread_mutex_unlock(&node->watch_lock);
    } else if (mode == NM_IO_WAIT_MODE_INFLIGHT) {
        // In-flight operations require backend cancellation. Completion will do
        // the actual wake once the backend acknowledges or reports the request.
        atomic_store(&req->abort_reason, (unsigned)reason);
        if (atomic_exchange(&req->cancel_queued, 1U) == 0U) {
            (void)nm_node_queue_control(node, NM_IO_CONTROL_REQ_CANCEL, req);
        }
        return false;
    }

    if (!removed) {
        return false;
    }

    nm_io_set_abort_result(req, reason);
    nm_account_io_abort_wake(shard, reason);
    nm_task_clear_wait_tracking(task);
    nm_reinject_task_on_shard(&g_nm_runtime,
                              task,
                              task->parked_shard,
                              true,
                              NM_TRACE_WAKE,
                              nm_io_abort_wait_reason(reason));
    return true;
}

/**
 * @brief Wake the task referenced by a wait node.
 *
 * @param node   Wait node whose task should be reinjected.
 * @param hot    Whether to prefer hot-lane enqueue.
 * @param reason Wait reason being resolved.
 */
void nm_wake_wait_node(nm_wait_node_t *node, bool hot, nm_wait_reason_t reason) {
    if (node == NULL || node->task == NULL) {
        return;
    }
    nm_reinject_task(&g_nm_runtime, node->task, hot, NM_TRACE_WAKE, reason);
}

/**
 * @brief Park the current managed task and switch back to its scheduler.
 *
 * @param reason Wait reason recorded on the task.
 * @param kind   Trace event kind.
 */
void nm_park_current_task(nm_wait_reason_t reason, nm_trace_kind_t kind) {
    nm_shard_t *shard = g_nm_tls_shard;
    nm_task_t *task = g_nm_tls_task;
    nm_ctx_t *scheduler_ctx;

    if (shard == NULL || task == NULL) {
        return;
    }

    scheduler_ctx = g_nm_tls_scheduler_ctx != NULL ? g_nm_tls_scheduler_ctx : &shard->scheduler_ctx;
    task->state = NM_TASK_STATE_PARKED;
    task->wait_reason = reason;
    shard->metrics.parks += 1U;
    nm_trace_shard(shard, task, kind, NM_TASK_STATE_RUNNING, NM_TASK_STATE_PARKED, reason);
    nm_task_sample_live_stack(task);
    // The scheduler resumes after this context switch and will later reinject
    // the task when its wait completes.
    nm_ctx_switch(&task->ctx, scheduler_ctx);
}

/**
 * @brief Wake every task waiting to join a completed task.
 *
 * @param rt   Runtime owning the tasks.
 * @param task Completed task whose join waiters should be woken.
 */
void nm_reinject_join_waiters(nm_runtime_t *rt, nm_task_t *task) {
    nm_task_t *waiters;

    pthread_mutex_lock(&task->lock);
    waiters = task->join_waiters;
    // Preserve the exit-time waiter count for reclamation ownership.
    task->join_waiter_count_at_exit = task->join_waiter_count;
    task->join_waiters = NULL;
    task->join_waiter_count = 0U;
    pthread_mutex_unlock(&task->lock);

    while (waiters != NULL) {
        nm_task_t *next = waiters->wait_next;
        waiters->wait_next = NULL;
        nm_reinject_task_on_shard(rt, waiters, waiters->parked_shard, true, NM_TRACE_WAKE, NM_WAIT_JOIN);
        waiters = next;
    }
}

/**
 * @brief Remove a specific join waiter while the target task lock is held.
 *
 * @param target Join target.
 * @param waiter Waiter task to remove.
 * @return true if the waiter was found and removed.
 */
bool nm_join_waiter_remove_locked(nm_task_t *target, nm_task_t *waiter) {
    nm_task_t *prev = NULL;
    nm_task_t *cur = target->join_waiters;

    while (cur != NULL) {
        if (cur == waiter) {
            if (prev != NULL) {
                prev->wait_next = cur->wait_next;
            } else {
                target->join_waiters = cur->wait_next;
            }
            cur->wait_next = NULL;
            if (target->join_waiter_count > 0U) {
                target->join_waiter_count -= 1U;
            }
            return true;
        }
        prev = cur;
        cur = cur->wait_next;
    }

    return false;
}
