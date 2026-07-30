/**
 * @file src/core/wait/wait_tracking.c
 * @brief Wait wake, park, and join reinjection transitions.
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

static void llam_wake_handoff_record_attempt(llam_shard_t *shard) {
    llam_task_t *current = g_llam_tls_task;
    bool sample;

    if (shard == NULL) {
        return;
    }
    sample = llam_runtime_should_record_handoff_stats(shard);
    if (current != NULL) {
        current->handoff_sample_current = sample;
    } else {
        shard->autotune_handoff_sample_current = sample;
    }
    if (sample) {
        shard->metrics.wake_handoff_attempts += 1U;
    }
}

static void llam_wake_handoff_record_hit(llam_shard_t *shard) {
    llam_task_t *current = g_llam_tls_task;
    bool sample;

    if (shard == NULL) {
        return;
    }
    sample = current != NULL ? current->handoff_sample_current : shard->autotune_handoff_sample_current;
    if (sample) {
        if (current != NULL) {
            current->handoff_sample_current = false;
        } else {
            shard->autotune_handoff_sample_current = false;
        }
        shard->metrics.wake_handoff_hits += 1U;
    }
}

static void llam_wake_handoff_record_fail(
    llam_shard_t *shard,
    llam_handoff_result_class_t reason) {
    llam_task_t *current = g_llam_tls_task;
    bool sample;

    if (shard == NULL) {
        return;
    }
    sample = current != NULL ? current->handoff_sample_current : shard->autotune_handoff_sample_current;
    if (!sample) {
        return;
    }
    if (current != NULL) {
        current->handoff_sample_current = false;
    } else {
        shard->autotune_handoff_sample_current = false;
    }

    switch (reason) {
    case LLAM_HANDOFF_RESULT_CONTEXT:
        shard->metrics.wake_handoff_fail_context += 1U;
        break;
    case LLAM_HANDOFF_RESULT_POLICY:
        shard->metrics.wake_handoff_fail_policy += 1U;
        break;
    case LLAM_HANDOFF_RESULT_BUDGET:
        shard->metrics.wake_handoff_fail_policy += 1U;
        shard->metrics.wake_handoff_fail_budget += 1U;
        break;
    case LLAM_HANDOFF_RESULT_RACE:
        shard->metrics.wake_handoff_fail_race += 1U;
        break;
    case LLAM_HANDOFF_RESULT_NONE:
    case LLAM_HANDOFF_RESULT_NO_WORK:
    case LLAM_HANDOFF_RESULT_SELF:
    case LLAM_HANDOFF_RESULT_PUSH:
    default:
        break;
    }
}

static llam_handoff_result_class_t llam_wake_handoff_precheck(
    llam_runtime_t *rt,
    llam_task_t *task,
    unsigned parked_shard) {
    llam_shard_t *shard = g_llam_tls_shard;
    llam_task_t *current = g_llam_tls_task;
    llam_handoff_policy_input_t input;
    llam_handoff_reject_t reject;

    if (rt == NULL || task == NULL || shard == NULL || current == NULL ||
        rt->active_shards == 0U || parked_shard >= rt->active_shards ||
        g_llam_tls_scheduler_ctx != &shard->scheduler_ctx) {
        return LLAM_HANDOFF_RESULT_CONTEXT;
    }
    if (shard->runtime != rt || shard->id != parked_shard || current == task) {
        return LLAM_HANDOFF_RESULT_CONTEXT;
    }

    input.runtime = rt;
    input.shard = shard;
    input.current = current;
    input.next = task;
    input.target_id = parked_shard;
    input.target_deadline_active = llam_task_wait_deadline_active(task);
    input.honor_timer_allowance = true;
    input.require_lockfree_queue = true;
    reject = llam_direct_handoff_policy(&input);
    if (reject == LLAM_HANDOFF_REJECT_LIVE_LIMIT ||
        reject == LLAM_HANDOFF_REJECT_BUDGET) {
        shard->direct_handoff_streak = 0U;
    }
    return llam_handoff_reject_classify(reject);
}


/**
 * @brief Wake a wait-node task, optionally fusing the wake with a direct handoff.
 *
 * @param node          Wait node whose task should be reinjected.
 * @param hot           Whether to prefer hot-lane enqueue.
 * @param reason        Wait reason being resolved.
 * @param allow_handoff Whether a same-shard task-to-task handoff may be tried.
 * @return true when the current task switched directly to the woken task.
 */
bool llam_wake_wait_node_and_maybe_handoff(llam_wait_node_t *node,
                                           bool hot,
                                           llam_wait_reason_t reason,
                                           bool allow_handoff) {
    llam_runtime_t *rt;
    llam_task_t *task;
    unsigned parked_shard;

    if (node == NULL || node->task == NULL) {
        return false;
    }
    task = node->task;
    rt = llam_wait_task_runtime(task);
    if (rt == NULL) {
        return false;
    }
    if (node->select_state != NULL) {
        if (!llam_channel_select_node_should_wake(node)) {
            return false;
        }
    } else if (!llam_wait_node_prepare_wake(node)) {
        return false;
    }

    parked_shard = task->parked_shard;
    if (allow_handoff && rt->wake_handoff_enabled != 0U) {
        llam_shard_t *stats_shard = g_llam_tls_shard;
        llam_handoff_result_class_t fail_reason =
            llam_wake_handoff_precheck(rt, task, parked_shard);

        llam_wake_handoff_record_attempt(stats_shard);
        if (fail_reason == LLAM_HANDOFF_RESULT_NONE) {
            if (llam_reinject_task_on_shard_and_yield_current(rt,
                                                              task,
                                                              parked_shard,
                                                              hot,
                                                              LLAM_TRACE_WAKE,
                                                              reason)) {
                llam_wake_handoff_record_hit(stats_shard);
                return true;
            }
            fail_reason = LLAM_HANDOFF_RESULT_RACE;
        }
        llam_wake_handoff_record_fail(stats_shard, fail_reason);
    }

    llam_reinject_task_on_shard(rt, task, parked_shard, hot, LLAM_TRACE_WAKE, reason);
    return false;
}

/**
 * @brief Wake the task referenced by a wait node.
 *
 * @param node   Wait node whose task should be reinjected.
 * @param hot    Whether to prefer hot-lane enqueue.
 * @param reason Wait reason being resolved.
 */
void llam_wake_wait_node(llam_wait_node_t *node, bool hot, llam_wait_reason_t reason) {
    (void)llam_wake_wait_node_and_maybe_handoff(node, hot, reason, false);
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
        g_llam_tls_task == NULL || !llam_task_may_run_on_shard(waiter, target) ||
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
    llam_task_clear_wait_tracking_or_abort(waiter);
    waiter->state = LLAM_TASK_STATE_RUNNABLE;
    waiter->wait_reason = LLAM_WAIT_NONE;
    waiter->enqueue_hot = 0U;
    waiter->last_runnable_ns = llam_runtime_should_stamp_runnable_latency(target) ? llam_now_ns() : 0U;
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
