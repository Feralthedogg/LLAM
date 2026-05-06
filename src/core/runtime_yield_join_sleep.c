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
    int caller_errno = errno;

    if (shard == NULL || task == NULL) {
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

    llam_task_safepoint();

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
        errno = caller_errno;
        return 0;
    }

    if (self == task) {
        errno = EDEADLK;
        return -1;
    }

    if (!has_deadline && atomic_load_explicit(&task->reclaim_ready, memory_order_acquire) != 0U) {
        llam_try_reclaim_joined_task(rt, task);
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
    self->wait_next = task->join_waiters;
    task->join_waiters = self;
    task->join_waiter_count += 1U;
    pthread_mutex_unlock(&task->lock);
    llam_task_set_join_tracking(self, task, g_llam_tls_shard->id);
    self->state = LLAM_TASK_STATE_PARKED;
    self->wait_reason = LLAM_WAIT_JOIN;
    if (has_deadline && llam_arm_task_wait_deadline(self, g_llam_tls_shard, deadline_ns) != 0) {
        pthread_mutex_lock(&task->lock);
        (void)llam_join_waiter_remove_locked(task, self);
        pthread_mutex_unlock(&task->lock);
        self->state = LLAM_TASK_STATE_RUNNING;
        self->wait_reason = LLAM_WAIT_NONE;
        llam_task_clear_wait_tracking(self);
        return -1;
    }
    if (self->cancel_token != NULL && llam_cancel_token_register_task(self) != 0) {
        llam_disarm_task_wait_deadline(self);
        pthread_mutex_lock(&task->lock);
        (void)llam_join_waiter_remove_locked(task, self);
        pthread_mutex_unlock(&task->lock);
        self->state = LLAM_TASK_STATE_RUNNING;
        self->wait_reason = LLAM_WAIT_NONE;
        llam_task_clear_wait_tracking(self);
        return -1;
    }

    g_llam_tls_shard->metrics.joins += 1U;
    g_llam_tls_shard->metrics.parks += 1U;
    llam_trace_shard(g_llam_tls_shard, self, LLAM_TRACE_STATE, LLAM_TASK_STATE_RUNNING, LLAM_TASK_STATE_PARKED, LLAM_WAIT_JOIN);
    llam_task_sample_live_stack(self);
    errno = caller_errno;
    llam_switch_task_to_scheduler(self,
                                g_llam_tls_scheduler_ctx != NULL ? g_llam_tls_scheduler_ctx : &g_llam_tls_shard->scheduler_ctx);
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
