/**
 * @file src/core/runtime_blocking_api.c
 * @brief Public blocking/offload API and worker compensation entry points.
 *
 * @details
 * This translation unit covers two separate blocking integration paths:
 *  - ::llam_call_blocking submits a user callback to the runtime's blocking
 *    worker pool, parks the current task, and resumes it when the callback
 *    completes or cancellation wins the race.
 *  - ::llam_enter_blocking and ::llam_leave_blocking bracket opaque foreign code
 *    that blocks the current scheduler worker directly.
 *
 * Opaque blocking is compensated so the shard does not become idle while the
 * task is inside foreign code. The primary compensation path either hands the
 * shard to an opaque helper thread or, on Linux by default, redirects queued
 * work to another runnable shard. Helper handoff keeps local affinity, while
 * redirect avoids paying the helper wake/wait cost on short blocking regions.
 *
 * Runtime knobs:
 *  - @c LLAM_OPAQUE_HANDOFF_SPIN_ITERS limits short spin waits around helper
 *    activation/deactivation.
 *  - @c LLAM_OPAQUE_REDIRECT_FASTPATH controls whether redirect is preferred
 *    over helper handoff.
 *  - @c LLAM_OPAQUE_TIMING enables timing counters for enter, leave, and total
 *    opaque-block duration.
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

#define LLAM_OPAQUE_HANDOFF_SPIN_ITERS_DEFAULT 16U
#if defined(__linux__)
#define LLAM_OPAQUE_REDIRECT_FASTPATH_DEFAULT 1U
#else
#define LLAM_OPAQUE_REDIRECT_FASTPATH_DEFAULT 0U
#endif

/**
 * @brief Return the cached helper-handoff spin count.
 *
 * The value is loaded from @c LLAM_OPAQUE_HANDOFF_SPIN_ITERS on first use and
 * then cached atomically. Clamping keeps the spin budget small enough that a
 * malformed environment value cannot turn handoff into an unbounded busy wait.
 *
 * @return Number of pause-loop iterations to try before sleeping on the helper
 *         condition variable/futex path.
 */
static unsigned llam_opaque_handoff_spin_iters(void) {
    static atomic_int cached = ATOMIC_VAR_INIT(-1);
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        const char *env = llam_env_get("LLAM_OPAQUE_HANDOFF_SPIN_ITERS");

        value = (int)LLAM_OPAQUE_HANDOFF_SPIN_ITERS_DEFAULT;
        if (env != NULL && env[0] != '\0') {
            char *end = NULL;
            unsigned long parsed = strtoul(env, &end, 10);

            if (end != env) {
                if (parsed > 65535UL) {
                    parsed = 65535UL;
                }
                value = (int)parsed;
            }
        }
        atomic_store_explicit(&cached, value, memory_order_release);
    }
    return (unsigned)value;
}

/**
 * @brief Check whether opaque blocking should prefer queue redirect.
 *
 * Linux defaults to redirect because the benchmarked fast path avoids the
 * helper handoff latency for short opaque blocks. Other platforms default to
 * helper handoff unless the environment overrides the policy.
 *
 * @return @c true when redirect should be attempted before helper handoff.
 */
static bool llam_opaque_redirect_fastpath_enabled(void) {
    static atomic_int cached = ATOMIC_VAR_INIT(-1);
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        const char *env = llam_env_get("LLAM_OPAQUE_REDIRECT_FASTPATH");

        value = (int)LLAM_OPAQUE_REDIRECT_FASTPATH_DEFAULT;
        if (env != NULL && env[0] != '\0') {
            value = strcmp(env, "0") != 0 ? 1 : 0;
        }
        atomic_store_explicit(&cached, value, memory_order_release);
    }
    return value != 0;
}

/**
 * @brief Check whether opaque-block timing counters are enabled.
 *
 * Timing is intentionally opt-in because the additional clock reads sit on the
 * enter/leave path that opaque-block microbenchmarks exercise heavily.
 *
 * @return @c true when @c LLAM_OPAQUE_TIMING is set to a non-zero value.
 */
static bool llam_opaque_timing_enabled(void) {
    static atomic_int cached = ATOMIC_VAR_INIT(-1);
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        const char *env = llam_env_get("LLAM_OPAQUE_TIMING");

        value = (env != NULL && env[0] != '\0' && strcmp(env, "0") != 0) ? 1 : 0;
        atomic_store_explicit(&cached, value, memory_order_release);
    }
    return value != 0;
}

/**
 * @brief Wake a shard that can execute work redirected from an opaque blocker.
 *
 * The preferred target recorded on @p blocked is tried first. If it is no
 * longer eligible, the function scans other shards and kicks the first one
 * accepting work. A final broadcast is used only when no single good target is
 * obvious.
 *
 * @param rt      Runtime containing the shard set.
 * @param blocked Shard whose primary worker entered an opaque blocking region.
 */
static void llam_wake_opaque_redirect_worker(llam_runtime_t *rt, llam_shard_t *blocked) {
    unsigned target_id;
    unsigned start;
    unsigned i;

    if (rt == NULL || blocked == NULL || rt->active_shards <= 1U) {
        return;
    }
    target_id = blocked->opaque_redirect_target_id;
    if (target_id < rt->active_shards && target_id != blocked->id &&
        llam_shard_accepts_new_work(&rt->shards[target_id])) {
        llam_kick_shard(&rt->shards[target_id]);
        return;
    }
    start = (blocked->id + 1U) % rt->active_shards;
    for (i = 0U; i < rt->active_shards; ++i) {
        llam_shard_t *candidate = &rt->shards[(start + i) % rt->active_shards];

        if (candidate == blocked || !llam_shard_accepts_new_work(candidate)) {
            continue;
        }
        llam_kick_shard(candidate);
        return;
    }
    llam_wake_all_shards(rt);
}

/**
 * @brief Spin briefly until the helper active hint reaches an expected value.
 *
 * The opaque lock is dropped while spinning so the helper can make progress and
 * publish its state transition. The caller regains the lock before inspecting
 * authoritative helper fields.
 *
 * @param shard    Shard owning the helper state.
 * @param expected Expected value of @c opaque_helper_active_hint.
 */
static void llam_opaque_spin_until_helper_hint(llam_shard_t *shard, unsigned expected) {
    unsigned spins = llam_opaque_handoff_spin_iters();
    unsigned i;

    if (spins == 0U) {
        return;
    }
    pthread_mutex_unlock(&shard->opaque_lock);
    for (i = 0U; i < spins; ++i) {
        if (atomic_load_explicit(&shard->opaque_helper_active_hint, memory_order_acquire) == expected) {
            break;
        }
        llam_pause_cpu();
    }
    pthread_mutex_lock(&shard->opaque_lock);
}

/**
 * @brief Run a blocking callback on the runtime blocking-worker pool.
 *
 * If called outside a managed task, the callback runs synchronously in the
 * caller because there is no scheduler context to park. Inside a managed task,
 * the function allocates a block job, links the task into its cancellation
 * token if present, queues the job under @c block_lock, wakes a blocking worker,
 * and context-switches back to the scheduler until completion.
 *
 * Completion and cancellation share the same wake path. After the task resumes,
 * any registered cancellation waiter is removed, the wait bookkeeping is
 * cleared, and the callback result or wake error is returned to the caller.
 *
 * @param fn  Blocking callback to execute.
 * @param arg User argument passed to @p fn.
 *
 * @return Callback result on success, or @c NULL on allocation failure,
 *         cancellation, invalid callback, or a callback that legitimately
 *         returns @c NULL. @c errno carries the disambiguating status.
 */
int llam_call_blocking_result(llam_blocking_fn fn, void *arg, void **out) {
    llam_runtime_t *rt = &g_llam_runtime;
    llam_task_t *task = g_llam_tls_task;
    llam_cancel_token_t *token;
    llam_block_job_t *job;
    int wake_error;

    llam_task_safepoint();

    if (fn == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }
    *out = NULL;

    if (task == NULL || g_llam_tls_shard == NULL) {
        errno = 0;
        *out = fn(arg);
        return 0;
    }

    job = llam_block_job_alloc(rt);
    if (job == NULL) {
        return -1;
    }

    job->fn = fn;
    job->arg = arg;
    job->task = task;
    atomic_init(&job->state, LLAM_BLOCK_JOB_QUEUED);

    task->blocking_result = NULL;
    task->blocking_errno = 0;
    llam_task_set_block_tracking(task, job, g_llam_tls_shard->id);
    task->state = LLAM_TASK_STATE_PARKED;
    task->wait_reason = LLAM_WAIT_BLOCKING;
    token = task->cancel_token;

    if (token != NULL) {
        pthread_mutex_lock(&token->lock);
        if (token->cancelled) {
            pthread_mutex_unlock(&token->lock);
            task->state = LLAM_TASK_STATE_RUNNING;
            task->wait_reason = LLAM_WAIT_NONE;
            llam_task_clear_wait_tracking(task);
            llam_block_job_release(rt, job);
            errno = ECANCELED;
            return -1;
        }

        if (!task->cancel_registered) {
            task->cancel_prev = NULL;
            task->cancel_next = token->waiters;
            if (token->waiters != NULL) {
                token->waiters->cancel_prev = task;
            }
            token->waiters = task;
            task->cancel_registered = true;
        }
    }

    pthread_mutex_lock(&rt->block_lock);
    job->next = NULL;
    if (rt->block_tail != NULL) {
        rt->block_tail->next = job;
    } else {
        rt->block_head = job;
    }
    rt->block_tail = job;
    atomic_fetch_add(&rt->block_pending, 1U);
    /* Queue state still lives under the mutex; futex only handles worker sleep/wake. */
    atomic_fetch_add_explicit(&rt->block_wake_seq, 1U, memory_order_release);
#if !defined(__linux__)
    if (rt->block_cv_initialized) {
        pthread_cond_signal(&rt->block_cv);
    }
#endif
    pthread_mutex_unlock(&rt->block_lock);
#if defined(__linux__)
    (void)llam_linux_futex_wake_private(&rt->block_wake_seq, 1U);
#endif
    if (token != NULL) {
        pthread_mutex_unlock(&token->lock);
    }

    g_llam_tls_shard->metrics.blocking_calls += 1U;
    g_llam_tls_shard->metrics.parks += 1U;
    llam_trace_shard(g_llam_tls_shard,
                   task,
                   LLAM_TRACE_BLOCK_SUBMIT,
                   LLAM_TASK_STATE_RUNNING,
                   LLAM_TASK_STATE_PARKED,
                   LLAM_WAIT_BLOCKING);
    llam_task_sample_live_stack(task);
    llam_switch_task_to_scheduler(task,
                                g_llam_tls_scheduler_ctx != NULL ? g_llam_tls_scheduler_ctx : &g_llam_tls_shard->scheduler_ctx);
    if (task->cancel_registered) {
        llam_cancel_token_unregister_task(task);
    }
    llam_task_clear_wait_tracking(task);
    wake_error = llam_consume_task_wake_error(task);
    if (wake_error != 0) {
        errno = wake_error;
        return -1;
    }
    errno = task->blocking_errno;
    *out = task->blocking_result;
    g_llam_tls_shard->metrics.blocking_completions += 1U;
    return 0;
}

void *llam_call_blocking(llam_blocking_fn fn, void *arg) {
    void *result = NULL;

    if (llam_call_blocking_result(fn, arg, &result) != 0) {
        return NULL;
    }
    return result;
}

/**
 * @brief Mark the current task as entering an opaque blocking region.
 *
 * The first enter at depth zero performs compensation. When the task is running
 * on the shard's primary scheduler context, the runtime either starts/wakes the
 * opaque helper or activates redirect so other shards can drain work while the
 * primary thread is stuck in foreign code. Nested enters only increase the task
 * depth; compensation is released by the matching outermost ::llam_leave_blocking.
 *
 * @return 0 on success. Calls made outside a managed runtime task are treated
 *         as no-ops and also return 0.
 */
int llam_enter_blocking(void) {
    llam_task_t *task = g_llam_tls_task;
    llam_shard_t *shard = g_llam_tls_shard;
    bool helper_active = false;
    bool redirect_active = false;
    bool from_primary = false;
    bool prefer_redirect = false;
    bool wake_redirect_workers = false;
    bool timing = false;
    uint64_t enter_wait_ns = 0U;
    uint64_t enter_wait_start_ns = 0U;

    llam_task_safepoint();

    /* Keep the shard productive while this task blocks in foreign code. */
    if (task == NULL || shard == NULL) {
        return 0;
    }

    if (task->opaque_blocking_depth == 0U) {
        timing = llam_opaque_timing_enabled();
        task->opaque_block_started_ns = timing ? llam_now_ns() : 0U;
        task->state = LLAM_TASK_STATE_BLOCKED_OPAQUE;
        task->wait_reason = LLAM_WAIT_BLOCKING;
        task->opaque_uses_helper = false;
        task->opaque_uses_redirect = false;
        from_primary = g_llam_tls_scheduler_ctx == &shard->scheduler_ctx;
        prefer_redirect = from_primary &&
                          shard->runtime->active_shards > 1U &&
                          (llam_opaque_redirect_fastpath_enabled() || g_llam_tls_opaque_redirect_hint != 0U);

        pthread_mutex_lock(&shard->opaque_lock);
        if (from_primary && !prefer_redirect) {
            shard->primary_thread = pthread_self();
            if (timing) {
                enter_wait_start_ns = llam_now_ns();
            }
            if (llam_ensure_opaque_helper_locked(shard) == 0) {
                shard->opaque_compensation_depth += 1U;
                if (shard->opaque_compensation_depth > shard->opaque_compensation_depth_peak) {
                    shard->opaque_compensation_depth_peak = shard->opaque_compensation_depth;
                }
                llam_opaque_wake_signal(shard);
                while (!shard->opaque_helper_active && !shard->opaque_helper_failed) {
                    llam_opaque_spin_until_helper_hint(shard, 1U);
                    if (shard->opaque_helper_active || shard->opaque_helper_failed) {
                        break;
                    }
                    llam_opaque_wake_wait(shard);
                }
                helper_active = shard->opaque_helper_active && !shard->opaque_helper_failed;
            }
            if (timing && enter_wait_start_ns != 0U) {
                uint64_t wait_end_ns = llam_now_ns();

                if (wait_end_ns >= enter_wait_start_ns) {
                    enter_wait_ns = wait_end_ns - enter_wait_start_ns;
                }
            }
            if (!helper_active && shard->opaque_compensation_depth > 0U) {
                shard->opaque_compensation_depth -= 1U;
            }
        }
        if (!helper_active) {
            shard->opaque_redirect_depth += 1U;
            if (shard->opaque_redirect_depth > shard->opaque_redirect_depth_peak) {
                shard->opaque_redirect_depth_peak = shard->opaque_redirect_depth;
            }
            redirect_active = true;
            wake_redirect_workers = shard->runtime->active_shards > 1U;
        }
        pthread_mutex_unlock(&shard->opaque_lock);

        pthread_mutex_lock(&shard->lock);
        atomic_store_explicit(&shard->current, NULL, memory_order_release);
        if (helper_active) {
            task->opaque_uses_helper = true;
            shard->metrics.opaque_compensations += 1U;
            if (timing) {
                shard->metrics.opaque_enter_wait_ns += enter_wait_ns;
                shard->metrics.opaque_enter_wait_samples += 1U;
                if (enter_wait_ns > shard->metrics.opaque_enter_wait_max_ns) {
                    shard->metrics.opaque_enter_wait_max_ns = enter_wait_ns;
                }
            }
            llam_trace_shard(shard,
                           task,
                           LLAM_TRACE_STATE,
                           LLAM_TASK_STATE_RUNNING,
                           LLAM_TASK_STATE_BLOCKED_OPAQUE,
                           LLAM_WAIT_BLOCKING);
        }
        if (redirect_active) {
            task->opaque_uses_redirect = true;
            if (shard->opaque_redirect_depth == 1U) {
                shard->metrics.opaque_redirect_activations += 1U;
            }
            llam_activate_opaque_redirect_locked(shard, task);
        }
        pthread_mutex_unlock(&shard->lock);
        if (wake_redirect_workers) {
            llam_wake_opaque_redirect_worker(shard->runtime, shard);
        }
    }
    task->opaque_blocking_depth += 1U;
    return 0;
}

/**
 * @brief Leave a previously entered opaque blocking region.
 *
 * Nested calls decrement only the task-local depth. The outermost leave tears
 * down the compensation selected by ::llam_enter_blocking: helper-backed blocks
 * wait for the helper to yield the shard back, while redirect-backed blocks
 * deactivate redirect when the shard's redirect depth reaches zero. Timing and
 * trace state are published before the task returns to normal running state.
 *
 * @return 0 on success or no-op outside a managed task.
 * @return -1 with @c errno set to @c EINVAL if leave is called without a
 *         matching enter.
 */
int llam_leave_blocking(void) {
    llam_task_t *task = g_llam_tls_task;
    llam_shard_t *shard = g_llam_tls_shard;

    if (task == NULL || shard == NULL) {
        return 0;
    }
    if (task->opaque_blocking_depth == 0U) {
        errno = EINVAL;
        return -1;
    }

    task->opaque_blocking_depth -= 1U;
    if (task->opaque_blocking_depth == 0U) {
        bool helper_started;
        bool used_helper = task->opaque_uses_helper;
        bool used_redirect = task->opaque_uses_redirect;
        bool from_opaque_helper = g_llam_tls_scheduler_ctx == &shard->opaque_scheduler_ctx;
        bool deactivate_redirect = false;
        bool timing = llam_opaque_timing_enabled();
        uint64_t now_ns = timing ? llam_now_ns() : 0U;
        uint64_t block_ns = 0U;
        uint64_t leave_wait_ns = 0U;
        uint64_t leave_wait_start_ns = 0U;

        if (timing && task->opaque_block_started_ns > 0U && now_ns >= task->opaque_block_started_ns) {
            block_ns = now_ns - task->opaque_block_started_ns;
        }
        task->opaque_block_started_ns = 0U;
        task->opaque_uses_helper = false;
        task->opaque_uses_redirect = false;
        task->opaque_block_count += 1U;
        if (timing) {
            task->last_opaque_block_ns = block_ns;
        }
        if (timing && block_ns > task->max_opaque_block_ns) {
            task->max_opaque_block_ns = block_ns;
        }

        pthread_mutex_lock(&shard->opaque_lock);
        helper_started = shard->opaque_helper_thread_started;
        if (used_helper && shard->opaque_compensation_depth > 0U) {
            if (timing && !from_opaque_helper) {
                leave_wait_start_ns = llam_now_ns();
            }
            shard->opaque_compensation_depth -= 1U;
            llam_opaque_wake_signal(shard);
            if (helper_started
#if defined(__linux__)
                && atomic_load_explicit(&shard->opaque_helper_opaque_wait, memory_order_acquire) == 0U
#endif
            ) {
                llam_kick_shard(shard);
            }
            while (!from_opaque_helper && shard->opaque_helper_active && !shard->opaque_helper_failed) {
                llam_opaque_spin_until_helper_hint(shard, 0U);
                if (!shard->opaque_helper_active || shard->opaque_helper_failed) {
                    break;
                }
                llam_opaque_wake_wait(shard);
            }
            if (timing && leave_wait_start_ns != 0U) {
                uint64_t wait_end_ns = llam_now_ns();

                if (wait_end_ns >= leave_wait_start_ns) {
                    leave_wait_ns = wait_end_ns - leave_wait_start_ns;
                }
            }
        }
        if (used_redirect && shard->opaque_redirect_depth > 0U) {
            shard->opaque_redirect_depth -= 1U;
            deactivate_redirect = shard->opaque_redirect_depth == 0U;
        }
        pthread_mutex_unlock(&shard->opaque_lock);

        pthread_mutex_lock(&shard->lock);
        if (timing) {
            shard->metrics.opaque_block_ns += block_ns;
            shard->metrics.opaque_block_samples += 1U;
        }
        if (timing && block_ns > shard->metrics.opaque_block_max_ns) {
            shard->metrics.opaque_block_max_ns = block_ns;
        }
        if (timing && used_helper && !from_opaque_helper) {
            shard->metrics.opaque_leave_wait_ns += leave_wait_ns;
            shard->metrics.opaque_leave_wait_samples += 1U;
            if (leave_wait_ns > shard->metrics.opaque_leave_wait_max_ns) {
                shard->metrics.opaque_leave_wait_max_ns = leave_wait_ns;
            }
        }
        if (deactivate_redirect) {
            llam_deactivate_opaque_redirect_locked(shard);
        }
        atomic_store_explicit(&shard->current, task, memory_order_release);
        task->state = LLAM_TASK_STATE_RUNNING;
        task->wait_reason = LLAM_WAIT_NONE;
        llam_trace_shard(shard,
                       task,
                       LLAM_TRACE_STATE,
                       LLAM_TASK_STATE_BLOCKED_OPAQUE,
                       LLAM_TASK_STATE_RUNNING,
                       LLAM_WAIT_BLOCKING);
        pthread_mutex_unlock(&shard->lock);
        atomic_store_explicit(&shard->last_safepoint_ns,
                              timing && now_ns != 0U ? now_ns : llam_now_ns(),
                              memory_order_relaxed);
    }
    return 0;
}
