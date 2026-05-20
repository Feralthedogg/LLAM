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

static llam_io_req_t *llam_task_swap_active_io_req(llam_task_t *task, llam_io_req_t *req) {
    llam_io_req_t *old_req;

    if (task == NULL) {
        return NULL;
    }

    old_req = atomic_exchange_explicit(&task->active_io_req, req, memory_order_acq_rel);
    if (g_llam_runtime.initialized) {
        if (old_req == NULL && req != NULL) {
            // active_io_waiters tracks tasks, not request objects.
            atomic_fetch_add_explicit(&g_llam_runtime.active_io_waiters, 1U, memory_order_acq_rel);
        } else if (old_req != NULL && req == NULL) {
            atomic_fetch_sub_explicit(&g_llam_runtime.active_io_waiters, 1U, memory_order_acq_rel);
        }
    }
    return old_req;
}

llam_io_req_t *llam_task_active_io_req_load(const llam_task_t *task) {
    if (task == NULL) {
        return NULL;
    }
    return atomic_load_explicit(&((llam_task_t *)task)->active_io_req, memory_order_acquire);
}

llam_block_job_t *llam_task_active_block_job_load(const llam_task_t *task) {
    if (task == NULL) {
        return NULL;
    }
    return atomic_load_explicit(&((llam_task_t *)task)->active_block_job, memory_order_acquire);
}

/**
 * @brief Clear all wait ownership fields on a task.
 *
 * @param task Task whose wait tracking should be reset.
 */
void llam_task_clear_wait_tracking(llam_task_t *task) {
    if (task == NULL) {
        return;
    }

    task->active_wait_node = NULL;
    task->active_wait_queue = NULL;
    task->active_wait_queue_lock = NULL;
    task->active_select_state = NULL;
    /*
     * Completion, cancellation, and dynamic rehome can run on different OS
     * threads.  Use an exchange so only the first resolver that observes the
     * active I/O owner decrements global I/O waiter accounting.
     */
    (void)llam_task_swap_active_io_req(task, NULL);
    /*
     * Blocking-job completion/cancellation can clear this field from a helper
     * OS thread while watchdog and timeout diagnostics sample it. Keep the
     * ownership handoff atomic just like active I/O request tracking.
     */
    atomic_store_explicit(&task->active_block_job, NULL, memory_order_release);
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
void llam_task_set_wait_node_tracking(llam_task_t *task,
                                           llam_wait_node_t *node,
                                           llam_wait_queue_t *queue,
                                           pthread_mutex_t *queue_lock,
                                           unsigned parked_shard) {
    task->active_wait_node = node;
    task->active_wait_queue = queue;
    task->active_wait_queue_lock = queue_lock;
    task->active_select_state = NULL;
    (void)llam_task_swap_active_io_req(task, NULL);
    atomic_store_explicit(&task->active_block_job, NULL, memory_order_release);
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
void llam_task_set_join_tracking(llam_task_t *task, llam_task_t *target, unsigned parked_shard) {
    task->active_wait_node = NULL;
    task->active_wait_queue = NULL;
    task->active_wait_queue_lock = NULL;
    task->active_select_state = NULL;
    (void)llam_task_swap_active_io_req(task, NULL);
    atomic_store_explicit(&task->active_block_job, NULL, memory_order_release);
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
void llam_task_set_sleep_tracking(llam_task_t *task, unsigned parked_shard) {
    task->active_wait_node = NULL;
    task->active_wait_queue = NULL;
    task->active_wait_queue_lock = NULL;
    task->active_select_state = NULL;
    (void)llam_task_swap_active_io_req(task, NULL);
    atomic_store_explicit(&task->active_block_job, NULL, memory_order_release);
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
void llam_task_set_io_tracking(llam_task_t *task, llam_io_req_t *req, unsigned parked_shard) {
    if (task == NULL) {
        return;
    }
    task->active_wait_node = NULL;
    task->active_wait_queue = NULL;
    task->active_wait_queue_lock = NULL;
    task->active_select_state = NULL;
    (void)llam_task_swap_active_io_req(task, req);
    atomic_store_explicit(&task->active_block_job, NULL, memory_order_release);
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
void llam_task_set_block_tracking(llam_task_t *task, llam_block_job_t *job, unsigned parked_shard) {
    task->active_wait_node = NULL;
    task->active_wait_queue = NULL;
    task->active_wait_queue_lock = NULL;
    task->active_select_state = NULL;
    (void)llam_task_swap_active_io_req(task, NULL);
    atomic_store_explicit(&task->active_block_job, job, memory_order_release);
    task->join_target = NULL;
    task->parked_shard = parked_shard;
    task->wake_error_code = 0;
}

/**
 * @brief Check whether an absolute deadline has passed.
 *
 * @param deadline_ns Absolute deadline in ::llam_now_ns units.
 * @return true when the deadline is now or in the past.
 */
bool llam_deadline_passed(uint64_t deadline_ns) {
    return deadline_ns <= llam_now_ns();
}

static llam_shard_t *llam_task_deadline_shard(llam_task_t *task) {
    if (task == NULL || g_llam_runtime.active_shards == 0U) {
        return NULL;
    }

    if (task->parked_shard < g_llam_runtime.active_shards) {
        return &g_llam_runtime.shards[task->parked_shard];
    }

    /*
     * Teardown, rehome, and partially failed setup paths can leave parked_shard
     * defensive rather than authoritative.  last_shard still maps the task to a
     * valid shard for timer cleanup.
     */
    return &g_llam_runtime.shards[task->last_shard % g_llam_runtime.active_shards];
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
int llam_arm_task_wait_deadline(llam_task_t *task, llam_shard_t *shard, uint64_t deadline_ns) {
    bool inserted;

    if (task == NULL || shard == NULL) {
        errno = EINVAL;
        return -1;
    }

    task->deadline_ns = deadline_ns;
    pthread_mutex_lock(&shard->lock);
    llam_timer_insert_locked(shard, task);
    /*
     * Decide allocation success while the timer heap lock is still held.  A
     * very short deadline can be popped by the watchdog immediately after this
     * lock is released; that also clears active_timer, but it is completion, not
     * allocation failure.
     */
    inserted = task->active_timer != NULL;
    pthread_mutex_unlock(&shard->lock);
    if (!inserted) {
        task->deadline_ns = 0U;
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

/**
 * @brief Check whether a task still owns an active deadline timer.
 *
 * @details
 * Timer expiry clears @c active_timer under the shard timer lock, including
 * from watchdog/helper pthreads. Readers must use the same lock; otherwise a
 * producer wake can race with timer expiry while deciding whether a direct
 * handoff is safe.
 *
 * @param task Task whose deadline ownership should be checked.
 * @return true if the task currently has an active timer node.
 */
bool llam_task_wait_deadline_active(llam_task_t *task) {
    llam_shard_t *shard = llam_task_deadline_shard(task);
    bool active = false;

    if (shard == NULL) {
        return false;
    }

    pthread_mutex_lock(&shard->lock);
    active = task->active_timer != NULL;
    pthread_mutex_unlock(&shard->lock);
    return active;
}

/**
 * @brief Remove a task's active wait deadline, if one exists.
 *
 * @param task Task whose timer should be removed.
 */
void llam_disarm_task_wait_deadline(llam_task_t *task) {
    llam_shard_t *shard = llam_task_deadline_shard(task);

    if (shard == NULL) {
        return;
    }
    pthread_mutex_lock(&shard->lock);
    if (task->active_timer != NULL) {
        (void)llam_timer_remove_locked(shard, task);
    }
    pthread_mutex_unlock(&shard->lock);
}

/**
 * @brief Register a task as a cancellation-token waiter.
 *
 * @param task Task with an optional cancel token.
 * @return 0 on success/no token, -1 with ECANCELED if already canceled.
 */
int llam_cancel_token_register_task(llam_task_t *task) {
    llam_cancel_token_t *token;

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
void llam_cancel_token_unregister_task(llam_task_t *task) {
    llam_cancel_token_t *token;
    llam_task_t *cur;
    bool linked = false;

    if (task == NULL || task->cancel_token == NULL) {
        return;
    }

    token = task->cancel_token;
    pthread_mutex_lock(&token->lock);
    if (task->cancel_registered) {
        for (cur = token->waiters; cur != NULL; cur = cur->cancel_next) {
            if (cur == task) {
                linked = true;
                break;
            }
        }
        if (linked) {
            if (task->cancel_prev != NULL) {
                task->cancel_prev->cancel_next = task->cancel_next;
            } else {
                token->waiters = task->cancel_next;
            }
            if (task->cancel_next != NULL) {
                task->cancel_next->cancel_prev = task->cancel_prev;
            }
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
int llam_consume_task_wake_error(llam_task_t *task) {
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
    unsigned shard_id;

    if (req == NULL || g_llam_runtime.active_shards == 0U || g_llam_runtime.active_nodes == 0U) {
        return -1;
    }
    if (req->attached_node_index < g_llam_runtime.active_nodes) {
        return (int)req->attached_node_index;
    }

    // Requests that have not attached to a node yet route through their owner
    // shard's assigned I/O node.
    shard_id = req->owner_shard < g_llam_runtime.active_shards ? req->owner_shard : (req->owner_shard % g_llam_runtime.active_shards);
    return (int)g_llam_runtime.shards[shard_id].io_node_index;
}

/**
 * @brief Abort a task's active I/O wait if it can be synchronously detached.
 *
 * @param task   Task parked on I/O.
 * @param reason Abort reason.
 * @return true if the task was detached and reinjected immediately.
 */
bool llam_abort_io_wait(llam_task_t *task, llam_io_abort_reason_t reason) {
    llam_io_req_t *req;
    int node_index;
    llam_node_t *node;
    llam_shard_t *shard;
    unsigned mode;
    bool removed = false;
    unsigned parked_shard;

    if (task == NULL || g_llam_runtime.active_shards == 0U) {
        return false;
    }

    req = llam_task_active_io_req_load(task);
    if (req == NULL) {
        return false;
    }
    node_index = llam_io_req_node_index(req);
    if (node_index < 0) {
        return false;
    }
    node = &g_llam_runtime.nodes[node_index];
    shard = &g_llam_runtime.shards[task->parked_shard % g_llam_runtime.active_shards];
    mode = atomic_load(&req->wait_mode);

    if (mode == LLAM_IO_WAIT_MODE_SUBMIT_QUEUE) {
        // Still queued for submission: remove directly from node submit queue.
        pthread_mutex_lock(&node->submit_lock);
        removed = llam_remove_node_submit_locked(node, req);
        if (removed) {
            atomic_store(&req->wait_mode, LLAM_IO_WAIT_MODE_NONE);
            atomic_store(&req->inflight_owner_shard, UINT_MAX);
        }
        pthread_mutex_unlock(&node->submit_lock);
        if (removed) {
            atomic_fetch_sub(&node->pending_ops, 1U);
        }
    } else if (mode == LLAM_IO_WAIT_MODE_POLL_WATCH && req->poll_watch != NULL) {
        pthread_mutex_lock(&node->watch_lock);
        removed = llam_poll_watch_remove_waiter(req->poll_watch, req);
        if (removed) {
            atomic_store(&req->wait_mode, LLAM_IO_WAIT_MODE_NONE);
            atomic_store(&req->inflight_owner_shard, UINT_MAX);
        }
        pthread_mutex_unlock(&node->watch_lock);
    } else if (mode == LLAM_IO_WAIT_MODE_ACCEPT_WATCH && req->accept_watch != NULL) {
        pthread_mutex_lock(&node->watch_lock);
        removed = llam_accept_watch_remove_waiter(req->accept_watch, req);
        if (removed) {
            atomic_store(&req->wait_mode, LLAM_IO_WAIT_MODE_NONE);
            atomic_store(&req->inflight_owner_shard, UINT_MAX);
        }
        pthread_mutex_unlock(&node->watch_lock);
    } else if (mode == LLAM_IO_WAIT_MODE_RECV_WATCH && req->recv_watch != NULL) {
        pthread_mutex_lock(&node->watch_lock);
        removed = llam_recv_watch_remove_waiter(req->recv_watch, req);
        if (removed) {
            atomic_store(&req->wait_mode, LLAM_IO_WAIT_MODE_NONE);
            atomic_store(&req->inflight_owner_shard, UINT_MAX);
        }
        pthread_mutex_unlock(&node->watch_lock);
    } else if (mode == LLAM_IO_WAIT_MODE_INFLIGHT) {
        // In-flight operations require backend cancellation. Completion will do
        // the actual wake once the backend acknowledges or reports the request.
        atomic_store(&req->abort_reason, (unsigned)reason);
        if (atomic_exchange(&req->cancel_queued, 1U) == 0U) {
            if (llam_node_queue_control(node, LLAM_IO_CONTROL_REQ_CANCEL, req) != 0) {
                /*
                 * The backend still owns the request.  Leave the waiter parked
                 * and allow a later abort attempt or natural completion instead
                 * of pretending the request was detached.
                 */
                atomic_store(&req->cancel_queued, 0U);
            }
        }
        return false;
    }

    if (!removed) {
        return false;
    }

    parked_shard = task->parked_shard;
    llam_io_set_abort_result(req, reason);
    llam_account_io_abort_wake(shard, reason);
    /*
     * Keep the wait ownership intact until the generic reinject path runs.
     * It still needs the original parked_shard to remove any active deadline
     * timer from the correct shard before clearing task tracking.
     */
    llam_reinject_task_on_shard(&g_llam_runtime,
                              task,
                              parked_shard,
                              true,
                              LLAM_TRACE_WAKE,
                              llam_io_abort_wait_reason(reason));
    return true;
}

/**
 * @brief Wake the task referenced by a wait node.
 *
 * @param node   Wait node whose task should be reinjected.
 * @param hot    Whether to prefer hot-lane enqueue.
 * @param reason Wait reason being resolved.
 */
void llam_wake_wait_node(llam_wait_node_t *node, bool hot, llam_wait_reason_t reason) {
    if (node == NULL || node->task == NULL) {
        return;
    }
    if (node->select_state != NULL) {
        if (!llam_channel_select_node_should_wake(node)) {
            return;
        }
    } else if (!llam_wait_node_prepare_wake(node)) {
        return;
    }
    llam_reinject_task(&g_llam_runtime, node->task, hot, LLAM_TRACE_WAKE, reason);
}

/**
 * @brief Park the current managed task and switch back to its scheduler.
 *
 * @param reason Wait reason recorded on the task.
 * @param kind   Trace event kind.
 */
void llam_park_current_task(llam_wait_reason_t reason, llam_trace_kind_t kind) {
    llam_shard_t *shard = g_llam_tls_shard;
    llam_task_t *task = g_llam_tls_task;
    llam_ctx_t *scheduler_ctx;

    if (shard == NULL || task == NULL) {
        return;
    }

    scheduler_ctx = g_llam_tls_scheduler_ctx != NULL ? g_llam_tls_scheduler_ctx : &shard->scheduler_ctx;
    llam_task_ensure_listed(task);
    task->state = LLAM_TASK_STATE_PARKED;
    task->wait_reason = reason;
    shard->metrics.parks += 1U;
    llam_trace_shard(shard, task, kind, LLAM_TASK_STATE_RUNNING, LLAM_TASK_STATE_PARKED, reason);
    llam_task_sample_live_stack(task);
    // The scheduler resumes after this context switch and will later reinject
    // the task when its wait completes.
    llam_switch_task_to_scheduler(task, scheduler_ctx);
}

/**
 * @brief Requeue the common same-shard single join waiter without generic wake routing.
 *
 * @details
 * Spawn/join fanout overwhelmingly wakes exactly one waiter on the same shard
 * that parked it.  The generic reinject path must handle migration, pressure,
 * timers, and cross-worker kicks; this path keeps those semantics out of the
 * hot join wake when they are provably unnecessary.
 *
 * @param rt     Runtime that owns the waiter.
 * @param waiter Single waiter removed from a completed task's join list.
 * @return true when the waiter was published locally.
 */
static bool llam_reinject_single_local_join_waiter(llam_runtime_t *rt, llam_task_t *waiter) {
    llam_shard_t *target;
    llam_task_state_id_t from;

    if (rt == NULL || waiter == NULL || waiter->wait_next != NULL || waiter->parked_shard >= rt->active_shards) {
        return false;
    }
    target = &rt->shards[waiter->parked_shard];
    if (g_llam_tls_shard != target ||
        g_llam_tls_task == NULL ||
        !llam_shard_accepts_new_work(target) ||
        llam_task_wait_deadline_active(waiter)) {
        return false;
    }

    pthread_mutex_lock(&target->lock);
    if (target->opaque_redirect_active) {
        pthread_mutex_unlock(&target->lock);
        return false;
    }
    /*
     * The local fast path can still reject the waiter above.  Do not clear wait
     * ownership until this path has committed; the generic reinject fallback
     * needs the original parked_shard and join tracking intact.
     */
    from = waiter->state;
    llam_task_clear_wait_tracking(waiter);
    waiter->state = LLAM_TASK_STATE_RUNNABLE;
    waiter->wait_reason = LLAM_WAIT_NONE;
    waiter->enqueue_hot = 0U;
    waiter->last_runnable_ns = rt->wake_latency_metrics_enabled != 0U ? llam_now_ns() : 0U;
    if (llam_queue_push_bounded_locked(target, &target->hot_q, LLAM_HOT_QUEUE_CAP, waiter)) {
        target->metrics.hot_enqueues += 1U;
    }
    target->metrics.wakes += 1U;
    target->metrics.wake_reason_hist[LLAM_WAIT_JOIN] += 1U;
    llam_trace_shard(target, waiter, LLAM_TRACE_WAKE, from, LLAM_TASK_STATE_RUNNABLE, LLAM_WAIT_JOIN);
    pthread_mutex_unlock(&target->lock);
    return true;
}

/**
 * @brief Wake every task waiting to join a completed task.
 *
 * @param rt   Runtime owning the tasks.
 * @param task Completed task whose join waiters should be woken.
 */
void llam_reinject_join_waiters(llam_runtime_t *rt, llam_task_t *task) {
    llam_task_t *waiters;

    pthread_mutex_lock(&task->lock);
    waiters = task->join_waiters;
    // Preserve the exit-time waiter count for reclamation ownership.
    task->join_waiter_count_at_exit = task->join_waiter_count;
    task->join_waiters = NULL;
    task->join_waiter_count = 0U;
    atomic_store_explicit(&task->join_waiter_hint, 0U, memory_order_release);
    pthread_mutex_unlock(&task->lock);

    if (waiters == NULL) {
        return;
    }

    while (waiters != NULL) {
        llam_task_t *next = waiters->wait_next;
        waiters->wait_next = NULL;
        if (!llam_reinject_single_local_join_waiter(rt, waiters)) {
            llam_reinject_task_on_shard(rt, waiters, waiters->parked_shard, true, LLAM_TRACE_WAKE, LLAM_WAIT_JOIN);
        }
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
bool llam_join_waiter_remove_locked(llam_task_t *target, llam_task_t *waiter) {
    llam_task_t *prev = NULL;
    llam_task_t *cur = target->join_waiters;

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
            atomic_store_explicit(&target->join_waiter_hint, target->join_waiter_count, memory_order_release);
            return true;
        }
        prev = cur;
        cur = cur->wait_next;
    }

    return false;
}
