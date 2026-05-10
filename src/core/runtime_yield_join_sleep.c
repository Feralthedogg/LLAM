/**
 * @file src/core/runtime_yield_join_sleep.c
 * @brief Task yield, join, timed join, and sleep API implementation.
 *
 * @details
 * These APIs are the common task-facing parking operations. Managed tasks park
 * by recording wait metadata, publishing a state transition, and switching back
 * to the scheduler context. Calls made outside a managed task fall back to
 * process-level blocking where that is meaningful, such as timed joins and
 * sleeps.
 *
 * Cancellation and deadline handling are deliberately integrated with the wait
 * metadata so ::llam_cancel_task_wait and ::llam_timeout_task_wait can remove the
 * task from the correct wait structure before reinjecting it as runnable.
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

/** @brief Sentinel used to mark an explicit user yield without sampling a real timestamp. */
#define LLAM_RECENT_EXPLICIT_YIELD UINT64_MAX

#if defined(__linux__) || defined(__APPLE__) || LLAM_RUNTIME_BACKEND_WINDOWS
#define LLAM_DIRECT_OWNER_HANDOFF 1
#else
#define LLAM_DIRECT_OWNER_HANDOFF 0
#endif

typedef enum llam_yield_direct_fail {
    LLAM_YIELD_DIRECT_FAIL_NONE = 0,
    LLAM_YIELD_DIRECT_FAIL_CONTEXT,
    LLAM_YIELD_DIRECT_FAIL_POLICY,
    LLAM_YIELD_DIRECT_FAIL_NO_WORK,
    LLAM_YIELD_DIRECT_FAIL_SELF,
    LLAM_YIELD_DIRECT_FAIL_PUSH,
} llam_yield_direct_fail_t;

static void llam_yield_direct_record_attempt(llam_shard_t *shard) {
    if (shard != NULL &&
        shard->runtime != NULL &&
        shard->runtime->direct_handoff_stats_enabled != 0U) {
        shard->metrics.yield_direct_attempts += 1U;
    }
}

static void llam_yield_direct_record_hit(llam_shard_t *shard, bool fast) {
    if (shard == NULL ||
        shard->runtime == NULL ||
        shard->runtime->direct_handoff_stats_enabled == 0U) {
        return;
    }
    if (fast) {
        shard->metrics.yield_direct_fast_hits += 1U;
    } else {
        shard->metrics.yield_direct_locked_hits += 1U;
    }
}

static void llam_yield_direct_record_fail(llam_shard_t *shard, llam_yield_direct_fail_t reason) {
    if (shard == NULL ||
        shard->runtime == NULL ||
        shard->runtime->direct_handoff_stats_enabled == 0U) {
        return;
    }
    switch (reason) {
    case LLAM_YIELD_DIRECT_FAIL_CONTEXT:
        shard->metrics.yield_direct_fail_context += 1U;
        break;
    case LLAM_YIELD_DIRECT_FAIL_POLICY:
        shard->metrics.yield_direct_fail_policy += 1U;
        break;
    case LLAM_YIELD_DIRECT_FAIL_NO_WORK:
        shard->metrics.yield_direct_fail_no_work += 1U;
        break;
    case LLAM_YIELD_DIRECT_FAIL_SELF:
        shard->metrics.yield_direct_fail_self += 1U;
        break;
    case LLAM_YIELD_DIRECT_FAIL_PUSH:
        shard->metrics.yield_direct_fail_push += 1U;
        break;
    case LLAM_YIELD_DIRECT_FAIL_NONE:
    default:
        break;
    }
}

/**
 * @brief Resolve whether yields may hand off directly to local work.
 *
 * Mode 1 enables handoff-marked yields such as channel/I/O handoffs. Mode 2
 * also enables ordinary user yields, which is useful for experiments but can
 * starve timer-heavy fanout if the policy guards are relaxed too far.
 */
static unsigned llam_direct_yield_handoff_mode(const llam_runtime_t *rt) {
    static atomic_int cached = -1;
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    (void)rt;
    if (value < 0) {
        const char *env = llam_env_get("LLAM_YIELD_DIRECT_HANDOFF");

        if (env != NULL && env[0] != '\0') {
            if (strcmp(env, "0") == 0) {
                value = 0;
            } else if (strcmp(env, "2") == 0 || strcmp(env, "full") == 0 || strcmp(env, "ordinary") == 0) {
                value = 2;
            } else {
                value = 1;
            }
        } else {
#if LLAM_RUNTIME_BACKEND_WINDOWS
            value = rt != NULL && rt->profile == LLAM_RUNTIME_PROFILE_RELEASE_FAST ? 2 : 0;
#elif defined(__linux__)
            /*
             * Linux owner-lane exchange is cheap enough to use for ordinary
             * yields as well. The runtime burst guard bounds long handoff
             * chains so timer-heavy fanout can still return to the scheduler.
             */
            value = rt != NULL && rt->profile != LLAM_RUNTIME_PROFILE_DEBUG_SAFE ? 2 : 0;
#else
            value = rt != NULL && rt->profile == LLAM_RUNTIME_PROFILE_RELEASE_FAST ? 2 : 0;
#endif
        }
        atomic_store_explicit(&cached, value, memory_order_release);
    }
    return (unsigned)value;
}

/**
 * @brief Direct-yield fast path for the lock-free same-shard owner lane.
 *
 * @details
 * When a primary scheduler task yields to local work, the shard mutex is not
 * needed: the current task is the single owner of the Chase-Lev bottom and FIFO
 * yield lane. This removes one lock/unlock pair from tight spawn/yield bursts.
 */
static bool llam_yield_to_local_runnable_unlocked(llam_yield_direct_fail_t *fail_reason) {
#if LLAM_DIRECT_OWNER_HANDOFF
    llam_shard_t *shard = g_llam_tls_shard;
    llam_task_t *current = g_llam_tls_task;
    llam_task_t *next;
    llam_runtime_t *rt;
    bool push_failed = false;
    int caller_errno = errno;

    if (shard == NULL || current == NULL || g_llam_tls_scheduler_ctx != &shard->scheduler_ctx) {
        if (fail_reason != NULL) {
            *fail_reason = LLAM_YIELD_DIRECT_FAIL_CONTEXT;
        }
        return false;
    }
    rt = shard->runtime;
    if (rt == NULL ||
        rt->trace_events_enabled != 0U ||
        rt->run_timing_enabled != 0U ||
        rt->wake_latency_metrics_enabled != 0U ||
        !llam_lockfree_normq_enabled(rt) ||
        !llam_shard_accepts_new_work(shard) ||
        shard->opaque_redirect_active ||
        (rt->direct_handoff_allow_timers == 0U &&
         atomic_load_explicit(&shard->timer_count, memory_order_acquire) != 0U)) {
        if (fail_reason != NULL) {
            *fail_reason = LLAM_YIELD_DIRECT_FAIL_POLICY;
        }
        return false;
    }
    if (rt->direct_handoff_live_limit != 0U &&
        llam_runtime_live_tasks(rt) > rt->direct_handoff_live_limit) {
        shard->direct_handoff_streak = 0U;
        if (fail_reason != NULL) {
            *fail_reason = LLAM_YIELD_DIRECT_FAIL_POLICY;
        }
        return false;
    }
    if (rt->direct_handoff_burst != 0U && shard->direct_handoff_streak >= rt->direct_handoff_burst) {
        shard->direct_handoff_streak = 0U;
        if (fail_reason != NULL) {
            *fail_reason = LLAM_YIELD_DIRECT_FAIL_POLICY;
        }
        return false;
    }
    current->forced_yield_budget = g_llam_runtime.forced_yield_every;
    current->state = LLAM_TASK_STATE_RUNNABLE;
    current->wait_reason = LLAM_WAIT_NONE;
    current->last_yield_ns = g_llam_tls_io_handoff_yield != 0U ? 0U : LLAM_RECENT_EXPLICIT_YIELD;
    if (!llam_norm_queue_exchange_yield_unlocked(shard, current, &next, &push_failed)) {
        if (fail_reason != NULL) {
            *fail_reason = push_failed ? LLAM_YIELD_DIRECT_FAIL_PUSH : LLAM_YIELD_DIRECT_FAIL_NO_WORK;
        }
        return false;
    }
    if (next == current) {
        if (fail_reason != NULL) {
            *fail_reason = LLAM_YIELD_DIRECT_FAIL_SELF;
        }
        return false;
    }

    next->state = LLAM_TASK_STATE_RUNNING;
    next->wait_reason = LLAM_WAIT_NONE;
    next->last_shard = shard->id;
    next->last_started_ns = 0U;
    atomic_store_explicit(&shard->current, next, memory_order_release);
    g_llam_tls_task = next;

    shard->metrics.yields += 1U;
    shard->metrics.ctx_switches += 1U;
    shard->direct_handoff_streak += 1U;
    errno = caller_errno;
    llam_switch_task_to_task_hot(current, next);
    return true;
#else
    if (fail_reason != NULL) {
        *fail_reason = LLAM_YIELD_DIRECT_FAIL_POLICY;
    }
    return false;
#endif
}

/**
 * @brief Try the ordinary-yield direct path selected by the platform policy.
 *
 * Platforms with a lock-free owner-lane exchange should fall back to the normal
 * scheduler path after a failed fast attempt instead of taking the shard lock
 * just to rediscover the same no-work or timer guard. Other platforms keep the
 * older locked direct path when the user explicitly opts in.
 */
static bool llam_yield_try_direct_handoff(llam_shard_t *shard) {
#if LLAM_DIRECT_OWNER_HANDOFF
    llam_yield_direct_fail_t fail_reason = LLAM_YIELD_DIRECT_FAIL_NONE;

    llam_yield_direct_record_attempt(shard);
    if (llam_yield_to_local_runnable_unlocked(&fail_reason)) {
        llam_yield_direct_record_hit(shard, true);
        return true;
    }
    if (fail_reason == LLAM_YIELD_DIRECT_FAIL_NONE) {
        fail_reason = LLAM_YIELD_DIRECT_FAIL_NO_WORK;
    }
    if (shard != NULL) {
        shard->direct_handoff_streak = 0U;
    }
    llam_yield_direct_record_fail(shard, fail_reason);
    return false;
#else
    (void)shard;
    return llam_yield_to_local_runnable();
#endif
}

/**
 * @brief Yield the current managed task back to its scheduler shard.
 *
 * The task is requeued as runnable on the local normal queue and execution
 * switches to the current scheduler context. Calls outside a runtime task are
 * no-ops because there is no cooperative scheduler frame to return to.
 */
void llam_yield(void) {
    llam_shard_t *shard = g_llam_tls_shard;
    llam_task_t *task = g_llam_tls_task;
    unsigned direct_mode;
    int caller_errno = errno;

    if (shard == NULL || task == NULL) {
        return;
    }
    direct_mode = llam_direct_yield_handoff_mode(shard->runtime);
    if (direct_mode != 0U &&
        (direct_mode >= 2U || g_llam_tls_io_handoff_yield != 0U) &&
        llam_yield_try_direct_handoff(shard)) {
        return;
    }

    task->forced_yield_budget = g_llam_runtime.forced_yield_every;
    task->state = LLAM_TASK_STATE_RUNNABLE;
    task->wait_reason = LLAM_WAIT_NONE;
    task->last_yield_ns = g_llam_tls_io_handoff_yield != 0U ? 0U : LLAM_RECENT_EXPLICIT_YIELD;

    pthread_mutex_lock(&shard->lock);
    (void)llam_norm_queue_push_yield_locked(shard, task);
    shard->metrics.yields += 1U;
    llam_trace_shard(shard, task, LLAM_TRACE_STATE, LLAM_TASK_STATE_RUNNING, LLAM_TASK_STATE_RUNNABLE, LLAM_WAIT_YIELD);
    pthread_mutex_unlock(&shard->lock);

    llam_task_sample_live_stack(task);
    errno = caller_errno;
    llam_switch_task_to_scheduler(task, g_llam_tls_scheduler_ctx != NULL ? g_llam_tls_scheduler_ctx : &shard->scheduler_ctx);
}

/**
 * @brief Yield directly to another local runnable task when safe.
 *
 * This is a narrower handoff path for latency-sensitive internal producers that
 * already know local work exists.  It avoids a scheduler round trip but keeps
 * the same queue discipline as a normal yield by placing the current task on the
 * normal FIFO side before switching to the selected peer.
 *
 * @return true when a direct task-to-task handoff happened.
 */
bool llam_yield_to_local_runnable(void) {
    llam_shard_t *shard = g_llam_tls_shard;
    llam_task_t *current = g_llam_tls_task;
    llam_task_t *next = NULL;
    llam_yield_direct_fail_t fail_reason = LLAM_YIELD_DIRECT_FAIL_NONE;
    int caller_errno = errno;

    llam_yield_direct_record_attempt(shard);
    if (llam_yield_to_local_runnable_unlocked(&fail_reason)) {
        llam_yield_direct_record_hit(shard, true);
        return true;
    }
#if LLAM_DIRECT_OWNER_HANDOFF
    if (fail_reason == LLAM_YIELD_DIRECT_FAIL_NONE) {
        fail_reason = LLAM_YIELD_DIRECT_FAIL_NO_WORK;
    }
    if (shard != NULL) {
        shard->direct_handoff_streak = 0U;
    }
    llam_yield_direct_record_fail(shard, fail_reason);
    return false;
#endif

    if (shard == NULL || current == NULL || shard->runtime == NULL) {
        llam_yield_direct_record_fail(shard, LLAM_YIELD_DIRECT_FAIL_CONTEXT);
        return false;
    }
    if (llam_direct_yield_handoff_mode(shard->runtime) == 0U) {
        llam_yield_direct_record_fail(shard, LLAM_YIELD_DIRECT_FAIL_POLICY);
        return false;
    }
    if (shard->runtime->run_timing_enabled != 0U || shard->runtime->wake_latency_metrics_enabled != 0U ||
        !llam_shard_accepts_new_work(shard)) {
        llam_yield_direct_record_fail(shard, LLAM_YIELD_DIRECT_FAIL_POLICY);
        return false;
    }

    pthread_mutex_lock(&shard->lock);
    if (shard->opaque_redirect_active ||
        shard->inject_q.depth > 0U ||
        (shard->runtime->direct_handoff_allow_timers == 0U && shard->timers != NULL) ||
        shard->norm_q.depth >= LLAM_NORM_QUEUE_CAP) {
        pthread_mutex_unlock(&shard->lock);
        llam_yield_direct_record_fail(shard, LLAM_YIELD_DIRECT_FAIL_POLICY);
        return false;
    }
    if (shard->hot_q.depth > 0U) {
        next = llam_queue_pop_head(&shard->hot_q);
    }
    if (next == NULL) {
        next = llam_norm_queue_pop_owner_locked(shard);
    }
    if (next == NULL || next == current) {
        pthread_mutex_unlock(&shard->lock);
        llam_yield_direct_record_fail(shard, next == current ? LLAM_YIELD_DIRECT_FAIL_SELF : LLAM_YIELD_DIRECT_FAIL_NO_WORK);
        return false;
    }

    current->forced_yield_budget = g_llam_runtime.forced_yield_every;
    current->state = LLAM_TASK_STATE_RUNNABLE;
    current->wait_reason = LLAM_WAIT_NONE;
    current->last_yield_ns = g_llam_tls_io_handoff_yield != 0U ? 0U : LLAM_RECENT_EXPLICIT_YIELD;
    if (!llam_norm_queue_push_yield_locked(shard, current)) {
        (void)llam_norm_queue_push_owner_locked(shard, next);
        pthread_mutex_unlock(&shard->lock);
        llam_yield_direct_record_fail(shard, LLAM_YIELD_DIRECT_FAIL_PUSH);
        return false;
    }

    next->state = LLAM_TASK_STATE_RUNNING;
    next->wait_reason = LLAM_WAIT_NONE;
    next->last_shard = shard->id;
    next->last_started_ns = 0U;
    atomic_store_explicit(&shard->current, next, memory_order_release);
    g_llam_tls_task = next;

    shard->metrics.yields += 1U;
    shard->metrics.ctx_switches += 1U;
    llam_yield_direct_record_hit(shard, false);
    if (shard->runtime->trace_events_enabled != 0U) {
        llam_trace_shard(shard, next, LLAM_TRACE_STATE, LLAM_TASK_STATE_RUNNABLE, LLAM_TASK_STATE_RUNNING, LLAM_WAIT_NONE);
        llam_trace_shard(shard, current, LLAM_TRACE_STATE, LLAM_TASK_STATE_RUNNING, LLAM_TASK_STATE_RUNNABLE, LLAM_WAIT_YIELD);
    }
    pthread_mutex_unlock(&shard->lock);

    errno = caller_errno;
    llam_switch_task_to_task_hot(current, next);
    return true;
}

/**
 * @brief Parked-join direct handoff into local runnable work.
 *
 * @details
 * Spawn/join bursts commonly park the parent immediately after publishing a
 * batch of child tasks.  Once the parent is on a join wait list it no longer
 * needs to run until its target completes, so this path can switch directly to
 * another local runnable task and skip one scheduler round trip.
 *
 * @return true when the current parked joiner switched directly to local work.
 */
static bool llam_join_try_local_handoff(llam_shard_t *shard, llam_task_t *current) {
#if LLAM_DIRECT_OWNER_HANDOFF
    llam_task_t *next;
    llam_runtime_t *rt;
    int caller_errno = errno;

    if (shard == NULL || current == NULL || g_llam_tls_scheduler_ctx != &shard->scheduler_ctx) {
        return false;
    }
    rt = shard->runtime;
    if (rt == NULL ||
        rt->trace_events_enabled != 0U ||
        rt->run_timing_enabled != 0U ||
        rt->wake_latency_metrics_enabled != 0U ||
        !llam_lockfree_normq_enabled(rt) ||
        !llam_shard_accepts_new_work(shard) ||
        shard->opaque_redirect_active ||
        atomic_load_explicit(&shard->timer_count, memory_order_acquire) != 0U) {
        return false;
    }
    if (rt->direct_handoff_live_limit != 0U &&
        llam_runtime_live_tasks(rt) > rt->direct_handoff_live_limit) {
        shard->direct_handoff_streak = 0U;
        return false;
    }
    if (rt->direct_handoff_burst != 0U && shard->direct_handoff_streak >= rt->direct_handoff_burst) {
        shard->direct_handoff_streak = 0U;
        return false;
    }

    next = llam_norm_queue_pop_owner_unlocked(shard);
    if (next == NULL || next == current) {
        return false;
    }

    next->state = LLAM_TASK_STATE_RUNNING;
    next->wait_reason = LLAM_WAIT_NONE;
    next->last_shard = shard->id;
    next->last_started_ns = 0U;
    atomic_store_explicit(&shard->current, next, memory_order_release);
    g_llam_tls_task = next;

    shard->metrics.ctx_switches += 1U;
    shard->direct_handoff_streak += 1U;
    errno = caller_errno;
    llam_switch_task_to_task_hot(current, next);
    return true;
#else
    llam_task_t *next = NULL;
    int caller_errno = errno;

    if (shard == NULL || current == NULL || shard->runtime == NULL) {
        return false;
    }
    if (llam_direct_yield_handoff_mode(shard->runtime) == 0U) {
        return false;
    }
    if (shard->runtime->run_timing_enabled != 0U ||
        shard->runtime->wake_latency_metrics_enabled != 0U ||
        !llam_shard_accepts_new_work(shard)) {
        return false;
    }

    pthread_mutex_lock(&shard->lock);
    if (shard->opaque_redirect_active ||
        shard->inject_q.depth > 0U ||
        shard->timers != NULL) {
        pthread_mutex_unlock(&shard->lock);
        return false;
    }
    if (shard->hot_q.depth > 0U) {
        next = llam_queue_pop_head(&shard->hot_q);
    }
    if (next == NULL) {
        next = llam_norm_queue_pop_owner_locked(shard);
    }
    if (next == NULL || next == current) {
        pthread_mutex_unlock(&shard->lock);
        return false;
    }

    next->state = LLAM_TASK_STATE_RUNNING;
    next->wait_reason = LLAM_WAIT_NONE;
    next->last_shard = shard->id;
    next->last_started_ns = 0U;
    atomic_store_explicit(&shard->current, next, memory_order_release);
    g_llam_tls_task = next;

    shard->metrics.ctx_switches += 1U;
    if (shard->runtime->trace_events_enabled != 0U) {
        llam_trace_shard(shard, next, LLAM_TRACE_STATE, LLAM_TASK_STATE_RUNNABLE, LLAM_TASK_STATE_RUNNING, LLAM_WAIT_NONE);
    }
    pthread_mutex_unlock(&shard->lock);

    errno = caller_errno;
    llam_switch_task_to_task_hot(current, next);
    return true;
#endif
}

/**
 * @brief Reclaim an already-completed task before taking the join mutex.
 *
 * @details
 * The scheduler publishes @c reclaim_ready after a task has released its stack.
 * A join caller that observes that flag can consume the handle immediately,
 * including timed joins whose deadline has already expired. Completion wins over
 * timeout because no parking or waiting is required.
 */
static bool llam_join_try_completed_fast(llam_runtime_t *rt, llam_task_t *task) {
    if (rt == NULL || task == NULL) {
        return false;
    }
    if (atomic_load_explicit(&task->reclaim_ready, memory_order_acquire) == 0U) {
        return false;
    }
    llam_try_reclaim_joined_task(rt, task);
    return true;
}

/**
 * @brief Join a task, optionally bounded by an absolute deadline.
 *
 * Managed callers park on the target task's join waiter list and resume when
 * the target exits, the deadline fires, cancellation wins, or a fatal runtime
 * error is observed. Non-managed callers cannot park cooperatively, so they poll
 * the target state with a short nanosleep interval.
 *
 * @param task        Task to join.
 * @param has_deadline Whether @p deadline_ns should be enforced.
 * @param deadline_ns Absolute monotonic deadline in nanoseconds.
 *
 * @return 0 when the target has completed.
 * @return -1 with @c errno set to @c EINVAL, @c EDEADLK, @c ETIMEDOUT,
 *         @c ECANCELED, or a fatal runtime error.
 */
int llam_join_impl(llam_task_t *task, bool has_deadline, uint64_t deadline_ns) {
    llam_runtime_t *rt = &g_llam_runtime;
    llam_task_t *self = g_llam_tls_task;
    int caller_errno = errno;

    if (task == NULL || !rt->initialized) {
        errno = EINVAL;
        return -1;
    }

    if (atomic_load_explicit(&task->detached, memory_order_acquire) != 0U) {
        errno = EINVAL;
        return -1;
    }

    if (self == NULL) {
        while (task->state != LLAM_TASK_STATE_DEAD && atomic_load(&rt->fatal_errno) == 0) {
            struct timespec ts;

            if (has_deadline && llam_deadline_passed(deadline_ns)) {
                errno = ETIMEDOUT;
                return -1;
            }
            ts.tv_sec = 0;
            ts.tv_nsec = 1000000L;
            nanosleep(&ts, NULL);
        }
        if (atomic_load(&rt->fatal_errno) != 0) {
            errno = atomic_load(&rt->fatal_errno);
            return -1;
        }
        llam_try_reclaim_joined_task(rt, task);
        errno = caller_errno;
        return 0;
    }

    if (self == task) {
        errno = EDEADLK;
        return -1;
    }

    if (llam_join_try_completed_fast(rt, task)) {
        errno = caller_errno;
        return 0;
    }

    pthread_mutex_lock(&task->lock);
    if (atomic_load_explicit(&task->detached, memory_order_acquire) != 0U) {
        pthread_mutex_unlock(&task->lock);
        errno = EINVAL;
        return -1;
    }
    if (task->state == LLAM_TASK_STATE_DEAD) {
        pthread_mutex_unlock(&task->lock);
        llam_try_reclaim_joined_task(rt, task);
        errno = caller_errno;
        return 0;
    }
    if (task->join_waiter_count > 0U) {
        pthread_mutex_unlock(&task->lock);
        errno = EBUSY;
        return -1;
    }
    if (has_deadline && llam_deadline_passed(deadline_ns)) {
        pthread_mutex_unlock(&task->lock);
        errno = ETIMEDOUT;
        return -1;
    }
    llam_task_ensure_listed(self);
    llam_task_set_join_tracking(self, task, g_llam_tls_shard->id);
    self->state = LLAM_TASK_STATE_PARKED;
    self->wait_reason = LLAM_WAIT_JOIN;
    if (has_deadline && llam_arm_task_wait_deadline(self, g_llam_tls_shard, deadline_ns) != 0) {
        pthread_mutex_unlock(&task->lock);
        self->state = LLAM_TASK_STATE_RUNNING;
        self->wait_reason = LLAM_WAIT_NONE;
        llam_task_clear_wait_tracking(self);
        return -1;
    }
    if (self->cancel_token != NULL && llam_cancel_token_register_task(self) != 0) {
        llam_disarm_task_wait_deadline(self);
        pthread_mutex_unlock(&task->lock);
        self->state = LLAM_TASK_STATE_RUNNING;
        self->wait_reason = LLAM_WAIT_NONE;
        llam_task_clear_wait_tracking(self);
        return -1;
    }
    self->wait_next = task->join_waiters;
    task->join_waiters = self;
    task->join_waiter_count += 1U;
    atomic_store_explicit(&task->join_waiter_hint, task->join_waiter_count, memory_order_release);
    pthread_mutex_unlock(&task->lock);

    g_llam_tls_shard->metrics.joins += 1U;
    g_llam_tls_shard->metrics.parks += 1U;
    llam_trace_shard(g_llam_tls_shard, self, LLAM_TRACE_STATE, LLAM_TASK_STATE_RUNNING, LLAM_TASK_STATE_PARKED, LLAM_WAIT_JOIN);
    llam_task_sample_live_stack(self);
    errno = caller_errno;
    if (!has_deadline && self->cancel_token == NULL && llam_join_try_local_handoff(g_llam_tls_shard, self)) {
        /* Resumed by the scheduler after the join target wakes this parked task. */
    } else {
        llam_switch_task_to_scheduler(self,
                                    g_llam_tls_scheduler_ctx != NULL ? g_llam_tls_scheduler_ctx : &g_llam_tls_shard->scheduler_ctx);
    }
    if (self->cancel_registered) {
        llam_cancel_token_unregister_task(self);
    }
    llam_task_clear_wait_tracking(self);
    {
        int wake_error = llam_consume_task_wake_error(self);

        if (wake_error != 0) {
            errno = wake_error;
            return -1;
        }
    }
    llam_try_reclaim_joined_task(rt, task);
    errno = caller_errno;
    return 0;
}

/**
 * @brief Join a task without a timeout.
 *
 * @param task Task to join.
 *
 * @return 0 on completion, or -1 with @c errno set on failure.
 */
int llam_join(llam_task_t *task) {
    return llam_join_impl(task, false, 0U);
}

/**
 * @brief Join a task until an absolute deadline.
 *
 * @param task        Task to join.
 * @param deadline_ns Absolute monotonic deadline in nanoseconds.
 *
 * @return 0 on completion.
 * @return -1 with @c errno set to @c ETIMEDOUT when the deadline expires, or
 *         another error propagated from ::llam_join_impl.
 */
int llam_join_until(llam_task_t *task, uint64_t deadline_ns) {
    return llam_join_impl(task, true, deadline_ns);
}

/**
 * @brief Detach a task handle so completion no longer requires join.
 *
 * @param task Task to detach.
 *
 * @return 0 on detach.
 * @return -1 with @c errno set to @c EINVAL for invalid input or @c EBUSY
 *         when a join waiter already owns completion.
 */
int llam_detach(llam_task_t *task) {
    llam_runtime_t *rt = &g_llam_runtime;
    unsigned expected = 0U;
    bool reclaim_after_unlock = false;

    llam_task_safepoint();

    if (task == NULL || !rt->initialized) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&task->lock);
    if (atomic_load_explicit(&task->detached, memory_order_acquire) != 0U) {
        pthread_mutex_unlock(&task->lock);
        errno = EINVAL;
        return -1;
    }
    if (task->join_waiter_count > 0U) {
        pthread_mutex_unlock(&task->lock);
        errno = EBUSY;
        return -1;
    }
    atomic_store_explicit(&task->detached, 1U, memory_order_release);
    if (atomic_load_explicit(&task->reclaim_ready, memory_order_acquire) != 0U &&
        atomic_compare_exchange_strong_explicit(&task->reclaim_claimed,
                                                &expected,
                                                1U,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        reclaim_after_unlock = true;
    }
    pthread_mutex_unlock(&task->lock);

    if (reclaim_after_unlock) {
        llam_reclaim_claimed_task(rt, task);
    }
    return 0;
}

/**
 * @brief Sleep until an absolute monotonic deadline.
 *
 * Managed tasks are inserted into the shard timer heap and parked. Non-managed
 * callers use @c nanosleep in a loop so this API remains useful before or
 * outside scheduler execution.
 *
 * @param deadline_ns Absolute monotonic deadline in nanoseconds.
 *
 * @return 0 when the deadline has passed.
 * @return -1 with @c errno set on invalid runtime state, allocation failure,
 *         cancellation, or unexpected @c nanosleep failure outside the runtime.
 */
int llam_sleep_until(uint64_t deadline_ns) {
    llam_task_t *task = g_llam_tls_task;
    llam_shard_t *shard = g_llam_tls_shard;
    int caller_errno = errno;
    int rc;
    bool traced_sleep = false;

    llam_task_safepoint();

    if (!g_llam_runtime.initialized) {
        errno = EINVAL;
        return -1;
    }

    if (task == NULL || shard == NULL) {
        for (;;) {
            uint64_t now_ns = llam_now_ns();
            struct timespec ts;

            if (deadline_ns <= now_ns) {
                errno = caller_errno;
                return 0;
            }
            ts.tv_sec = (time_t)((deadline_ns - now_ns) / 1000000000ULL);
            ts.tv_nsec = (long)((deadline_ns - now_ns) % 1000000000ULL);
            rc = nanosleep(&ts, NULL);
            if (rc == 0) {
                errno = caller_errno;
                return 0;
            }
            if (errno != EINTR) {
                return -1;
            }
        }
    }

    if (deadline_ns <= llam_now_ns()) {
        errno = caller_errno;
        return 0;
    }

    llam_task_ensure_listed(task);
    llam_task_set_sleep_tracking(task, shard->id);
    task->deadline_ns = deadline_ns;
    task->state = LLAM_TASK_STATE_PARKED;
    task->wait_reason = LLAM_WAIT_SLEEP;

    pthread_mutex_lock(&shard->lock);
    llam_timer_insert_locked(shard, task);
    if (task->active_timer != NULL && task->cancel_token == NULL) {
        llam_trace_shard(shard, task, LLAM_TRACE_STATE, LLAM_TASK_STATE_RUNNING, LLAM_TASK_STATE_PARKED, LLAM_WAIT_SLEEP);
        traced_sleep = true;
    }
    pthread_mutex_unlock(&shard->lock);
    if (task->active_timer == NULL) {
        task->state = LLAM_TASK_STATE_RUNNING;
        task->wait_reason = LLAM_WAIT_NONE;
        task->deadline_ns = 0U;
        llam_task_clear_wait_tracking(task);
        errno = ENOMEM;
        return -1;
    }
    if (task->cancel_token != NULL && llam_cancel_token_register_task(task) != 0) {
        pthread_mutex_lock(&shard->lock);
        (void)llam_timer_remove_locked(shard, task);
        pthread_mutex_unlock(&shard->lock);
        task->state = LLAM_TASK_STATE_RUNNING;
        task->wait_reason = LLAM_WAIT_NONE;
        task->deadline_ns = 0U;
        llam_task_clear_wait_tracking(task);
        return -1;
    }
    shard->metrics.sleeps += 1U;
    shard->metrics.parks += 1U;
    if (!traced_sleep) {
        pthread_mutex_lock(&shard->lock);
        llam_trace_shard(shard, task, LLAM_TRACE_STATE, LLAM_TASK_STATE_RUNNING, LLAM_TASK_STATE_PARKED, LLAM_WAIT_SLEEP);
        pthread_mutex_unlock(&shard->lock);
    }

    llam_task_sample_live_stack(task);
    errno = caller_errno;
    llam_switch_task_to_scheduler(task, g_llam_tls_scheduler_ctx != NULL ? g_llam_tls_scheduler_ctx : &shard->scheduler_ctx);
    if (task->cancel_registered) {
        llam_cancel_token_unregister_task(task);
    }
    llam_task_clear_wait_tracking(task);
    if (llam_consume_task_wake_error(task) != 0) {
        errno = ECANCELED;
        return -1;
    }
    errno = caller_errno;
    return 0;
}

/**
 * @brief Sleep for a relative duration in nanoseconds.
 *
 * @param duration_ns Relative duration in nanoseconds.
 *
 * @return 0 on success, or -1 with @c errno set by ::llam_sleep_until.
 */
int llam_sleep_ns(uint64_t duration_ns) {
    return llam_sleep_until(llam_now_ns() + duration_ns);
}
