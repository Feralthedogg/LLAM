/**
 * @file src/core/api/blocking_api.c
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
 * SPDX-License-Identifier: Apache-2.0
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * See LICENSE.
 */

#include "runtime_internal.h"

#define LLAM_OPAQUE_HANDOFF_SPIN_ITERS_DEFAULT 16U
#define LLAM_OPAQUE_REDIRECT_FASTPATH_DEFAULT 0U

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
    static atomic_int cached = -1;
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        const char *env = llam_env_get("LLAM_OPAQUE_HANDOFF_SPIN_ITERS");

        value = (int)LLAM_OPAQUE_HANDOFF_SPIN_ITERS_DEFAULT;
        if (env != NULL && env[0] != '\0') {
            char *end = NULL;
            unsigned long parsed;

            if (!llam_ascii_is_space((unsigned char)env[0]) && env[0] != '-' && env[0] != '+') {
                errno = 0;
                parsed = strtoul(env, &end, 10);
                if (errno == 0 && end != env && *end == '\0') {
                    if (parsed > 65535UL) {
                        parsed = 65535UL;
                    }
                    value = (int)parsed;
                }
            }
        }
        atomic_store_explicit(&cached, value, memory_order_release);
    }
    return (unsigned)value;
}

/**
 * @brief Check whether opaque blocking should prefer queue redirect.
 *
 * Helper handoff is the default because it preserves local scheduling affinity
 * and avoids redirect-tail variance in short opaque blocking regions. Redirect
 * remains available through the environment for workloads that prefer keeping
 * every primary worker out of foreign blocking calls.
 *
 * @return @c true when redirect should be attempted before helper handoff.
 */
static bool llam_opaque_redirect_fastpath_enabled(void) {
    static atomic_int cached = -1;
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        const char *env = llam_env_get("LLAM_OPAQUE_REDIRECT_FASTPATH");

        value = (int)LLAM_OPAQUE_REDIRECT_FASTPATH_DEFAULT;
        if (env != NULL && env[0] != '\0') {
            value = llam_env_flag_value(env, LLAM_OPAQUE_REDIRECT_FASTPATH_DEFAULT) != 0U ? 1 : 0;
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
    static atomic_int cached = -1;
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        const char *env = llam_env_get("LLAM_OPAQUE_TIMING");

        value = llam_env_flag_value(env, 0U) != 0U ? 1 : 0;
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
 * Queued callbacks may be cancelled before a worker starts them. Once a worker
 * is executing user code, cancellation is reported only after the callback
 * returns so the caller cannot outlive or free callback arguments prematurely.
 * After the task resumes, any registered cancellation waiter is removed, the
 * wait bookkeeping is cleared, and the callback result or wake error is
 * returned to the caller.
 *
 * @param fn  Blocking callback to execute.
 * @param arg User argument passed to @p fn.
 *
 * @return Callback result on success, or @c NULL on allocation failure,
 *         cancellation, invalid callback, or a callback that legitimately
 *         returns @c NULL. @c errno carries the disambiguating status.
 */
int llam_call_blocking_result(llam_blocking_fn fn, void *arg, void **out) {
    LLAM_NATIVE_API_GUARD(-1);

    llam_runtime_t *rt;
    llam_task_t *task = g_llam_tls_task;
    llam_worker_t *worker = g_llam_tls_worker;
    llam_cancel_lock_set_t cancel_locks;
    llam_block_job_t *job;
    llam_wait_node_t *node;
    bool async_reserved = false;
    int wake_error;

    llam_task_safepoint();

    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }
    /*
     * Result-style APIs must leave no stale callback result behind on any
     * submission failure.  NULL is also a valid successful callback value, so
     * callers distinguish success solely through the integer return code.
     */
    *out = NULL;
    if (fn == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (task == NULL || g_llam_tls_shard == NULL) {
        errno = 0;
        *out = fn(arg);
        return 0;
    }
    rt = task->owner_runtime != NULL ? task->owner_runtime : g_llam_tls_shard->runtime;
    if (rt == NULL) {
        errno = EINVAL;
        return -1;
    }
    /*
     * Runtime stop is a cancellation boundary for managed waits.  Reject new
     * blocking submissions after stop is visible so shutdown can let idle block
     * workers exit without racing a late task that parks on a job nobody will run.
     */
    if (atomic_load_explicit(&rt->stop_requested, memory_order_acquire) ||
        atomic_load_explicit(&rt->shutdown_requested, memory_order_acquire)) {
        errno = ECANCELED;
        return -1;
    }

    if (llam_async_preempt_block_reserve(rt, &async_reserved) != 0) {
        return -1;
    }
    job = llam_block_job_alloc(rt);
    if (job == NULL) {
        if (async_reserved) {
            llam_async_preempt_block_release(rt);
        }
        return -1;
    }
    /* Caller owns this prepared job until publication; every rollback uses its finalizer. */
    job->async_slot_reserved = async_reserved;
    node = llam_sync_wait_node_acquire(g_llam_tls_shard);
    if (node == NULL) {
        llam_block_job_release(rt, job);
        errno = ENOMEM;
        return -1;
    }

    job->fn = fn;
    job->arg = arg;
    job->task = task;
    job->wait_node = node;
    /*
     * Blocking workers complete on OS helper threads and write the callback
     * result back into the parked task just before reinjection.  Hold the same
     * short reclaim guard used by cancellation/timer scans so detached task
     * reclamation cannot recycle the task object while a helper still owns this
     * raw task pointer.
     */
    if (!llam_task_scan_ref_try_acquire(rt, task)) {
        int saved_errno = errno;

        llam_sync_wait_node_release(g_llam_tls_shard, node);
        llam_block_job_release(rt, job);
        errno = saved_errno;
        return -1;
    }
    job->holds_task_ref = true;
    /*
     * Allocation already initialized the atomic state object.  Publish QUEUED
     * with an atomic store so the worker/cancel state machine never observes a
     * byte-cleared or uninitialized atomic after job recycling.
     */
    atomic_store_explicit(&job->state, LLAM_BLOCK_JOB_QUEUED, memory_order_release);

    task->blocking_result = NULL;
    task->blocking_errno = 0;
    llam_task_ensure_listed(task);
    if (!llam_task_set_block_tracking(task, job, g_llam_tls_shard->id)) {
        int saved_errno = errno;

        llam_sync_wait_node_release(g_llam_tls_shard, node);
        llam_block_job_release(rt, job);
        errno = saved_errno;
        return -1;
    }
    if (llam_cancel_token_lock_task(task, &cancel_locks) != 0) {
        int saved_errno = errno;
        task->state = LLAM_TASK_STATE_RUNNING;
        task->wait_reason = LLAM_WAIT_NONE;
        llam_task_clear_wait_tracking_or_abort(task);
        llam_sync_wait_node_release(g_llam_tls_shard, node);
        llam_block_job_release(rt, job);
        errno = saved_errno;
        return -1;
    }
    /* Keep every source locked through queue commit: cancellation must never
     * observe a registered blocking waiter before its job is actually queued. */
    llam_cancel_token_register_task_locked(task);

    pthread_mutex_lock(&rt->block_lock);
    bool admission_closed = atomic_load_explicit(&rt->stop_requested, memory_order_acquire) ||
                            atomic_load_explicit(&rt->shutdown_requested, memory_order_acquire);
    bool at_capacity = rt->max_blocking_jobs != 0U &&
                       atomic_load_explicit(&rt->block_pending, memory_order_relaxed) >= rt->max_blocking_jobs;
    if (admission_closed || at_capacity || !llam_runtime_note_block_pending(rt, 1U)) {
        /* Queue commit and the last block worker's exit decision share this lock. */
        int saved_errno = admission_closed ? ECANCELED : (at_capacity ? EAGAIN : errno);

        pthread_mutex_unlock(&rt->block_lock);
        llam_cancel_token_unregister_task_locked(task);
        llam_cancel_token_unlock_task(&cancel_locks);
        task->state = LLAM_TASK_STATE_RUNNING;
        task->wait_reason = LLAM_WAIT_NONE;
        llam_task_clear_wait_tracking_or_abort(task);
        llam_sync_wait_node_release(g_llam_tls_shard, node);
        llam_block_job_release(rt, job);
        errno = saved_errno;
        return -1;
    }
    job->next = NULL;
    if (rt->block_tail != NULL) {
        rt->block_tail->next = job;
    } else {
        rt->block_head = job;
    }
    rt->block_tail = job;
    /* Queue state still lives under the mutex; futex only handles worker sleep/wake. */
    atomic_fetch_add_explicit(&rt->block_wake_seq, 1U, memory_order_release);
#if !defined(__linux__) && !LLAM_PLATFORM_WINDOWS
    if (rt->block_cv_initialized) {
        pthread_cond_signal(&rt->block_cv);
    }
#endif
    pthread_mutex_unlock(&rt->block_lock);
#if defined(__linux__)
    (void)llam_linux_futex_wake_private(&rt->block_wake_seq, 1U);
#elif LLAM_PLATFORM_WINDOWS
    WakeByAddressSingle((PVOID)&rt->block_wake_seq);
#endif
    llam_cancel_token_unlock_task(&cancel_locks);

    g_llam_tls_shard->metrics.blocking_calls += 1U;
    g_llam_tls_shard->metrics.parks += 1U;
    llam_trace_shard(g_llam_tls_shard,
                   task,
                   LLAM_TRACE_BLOCK_SUBMIT,
                   LLAM_TASK_STATE_RUNNING,
                   LLAM_TASK_STATE_PARKED,
                   LLAM_WAIT_BLOCKING);
    if (llam_wait_node_should_park(node)) {
        /* Retire execution publication before handing the task to a wait. */
        if (llam_worker_matches_shard(worker, g_llam_tls_shard)) {
            pthread_mutex_lock(&g_llam_tls_shard->lock);
            llam_worker_clear_active_g(worker);
            pthread_mutex_unlock(&g_llam_tls_shard->lock);
        }
        llam_task_sample_live_stack(task);
        llam_switch_task_to_scheduler(task,
                                    g_llam_tls_scheduler_ctx != NULL ? g_llam_tls_scheduler_ctx : &g_llam_tls_shard->scheduler_ctx);
    }
    llam_cancel_token_unregister_task(task);
    llam_task_clear_wait_tracking_or_abort(task);
    llam_sync_wait_node_release(g_llam_tls_shard, node);
    wake_error = llam_consume_task_wake_error(task);
    if (wake_error != 0) {
        llam_thread_errno_store(wake_error);
        return -1;
    }
    llam_thread_errno_store(task->blocking_errno);
    *out = task->blocking_result;
    g_llam_tls_shard->metrics.blocking_completions += 1U;
    return 0;
}

void *llam_call_blocking(llam_blocking_fn fn, void *arg) {
    LLAM_NATIVE_API_GUARD(NULL);

    void *result = NULL;

    if (llam_call_blocking_result(fn, arg, &result) != 0) {
        return NULL;
    }
    return result;
}

/*
 * A destructive shutdown can win after a helper transfer has retired the
 * primary publication but before the helper owner is granted. Restore the
 * running task and its worker-local active-G publication as one
 * opaque-lock -> shard-lock boundary so the failed enter remains an ordinary
 * cancellation rather than a partially parked task. Callers hold
 * `shard->opaque_lock` while invoking it.
 */
static void llam_restore_running_after_lease_abort_locked(llam_shard_t *shard,
                                                          llam_task_t *task,
                                                          llam_worker_t *worker) {
    if (shard == NULL || task == NULL) {
        return;
    }
    pthread_mutex_lock(&shard->lock);
    task->state = LLAM_TASK_STATE_RUNNING;
    task->wait_reason = LLAM_WAIT_NONE;
    if (worker != NULL && worker->compat_shard == shard) {
        atomic_store_explicit(&worker->last_safepoint_ns,
                              llam_now_ns(),
                              memory_order_release);
        /* Publish only after the restored task boundary is complete. */
        llam_worker_begin_g_run(worker, task);
        llam_worker_publish_active_g(worker, task);
    }
    llam_trace_shard(shard,
                     task,
                     LLAM_TRACE_STATE,
                     LLAM_TASK_STATE_BLOCKED_OPAQUE,
                     LLAM_TASK_STATE_RUNNING,
                     LLAM_WAIT_BLOCKING);
    pthread_mutex_unlock(&shard->lock);
}

/**
 * @brief Recognize a helper M that is already relinquishing P through redirect.
 *
 * @details
 * The primary can resume from its initial helper activation after the helper
 * has started another managed task and that task has entered foreign blocking.
 * In that window the lease is redirected with no executor, even though the
 * primary's outer helper-backed detach has not yet published its token. Accept
 * only the helper's exact live redirect claimant, with P absent and either a
 * matching published token or an in-progress runtime-critical detach/return.
 */
static bool llam_foreign_helper_redirect_inflight_locked(
    llam_shard_t *shard,
    uint64_t primary_transfer_sequence) {
    llam_worker_t *helper;
    const llam_foreign_token_t *token;
    llam_task_t *active_task;
    uint64_t incarnation;
    unsigned execution_state;
    unsigned critical_depth;
    unsigned preempt_depth;

    if (shard == NULL || primary_transfer_sequence == 0U) {
        return false;
    }
    helper = &shard->opaque_helper_worker;
    token = &helper->foreign_token;
    active_task = llam_worker_load_active_g(helper);
    incarnation = atomic_load_explicit(&helper->incarnation, memory_order_acquire);
    execution_state = atomic_load_explicit(&helper->execution_state,
                                           memory_order_acquire);
    /* Publication precedes dropping the guards. Read critical depth first so
     * observing its final zero also orders the earlier preemption release. */
    critical_depth = atomic_load_explicit(&helper->runtime_critical_depth,
                                          memory_order_acquire);
    preempt_depth = atomic_load_explicit(&helper->preempt_disable_depth,
                                         memory_order_acquire);
    if (shard->legacy_lease.state != LLAM_LEASE_REDIRECTED_NO_EXECUTOR ||
        shard->legacy_lease.owner != NULL ||
        shard->legacy_lease.return_worker != helper ||
        shard->legacy_lease.return_incarnation != incarnation ||
        shard->legacy_lease.transfer_sequence == 0U ||
        shard->legacy_lease.transfer_sequence == primary_transfer_sequence ||
        atomic_load_explicit(&helper->current_p, memory_order_acquire) != NULL ||
        incarnation == 0U ||
        atomic_load_explicit(&helper->binding_incarnation, memory_order_acquire) !=
            incarnation ||
        !atomic_load_explicit(&helper->native_thread_live, memory_order_acquire)) {
        return false;
    }

    if (execution_state == LLAM_WORKER_EXEC_RUNTIME_UNSAFE &&
        critical_depth != 0U && preempt_depth != 0U) {
        bool in_progress_task_valid =
            active_task == NULL ||
            (active_task->owner_runtime == shard->runtime &&
             (atomic_load_explicit(&active_task->state, memory_order_acquire) ==
                  LLAM_TASK_STATE_RUNNING ||
              atomic_load_explicit(&active_task->state, memory_order_acquire) ==
                  LLAM_TASK_STATE_BLOCKED_OPAQUE));

        return (token->state == LLAM_FOREIGN_TOKEN_INACTIVE &&
                in_progress_task_valid) ||
               (token->state == LLAM_FOREIGN_TOKEN_RETURNING &&
                token->worker == helper && token->processor == &shard->processor &&
                token->mode == LLAM_FOREIGN_TOKEN_MODE_REDIRECT &&
                token->generation != 0U &&
                token->generation == helper->foreign_token_generation &&
                token->worker_incarnation == incarnation &&
                token->worker_run_generation != 0U &&
                token->worker_run_generation != UINT64_MAX &&
                token->worker_run_generation ==
                    atomic_load_explicit(&helper->run_generation, memory_order_acquire) &&
                token->task != NULL &&
                token->task_run_generation != 0U &&
                token->task_run_generation != UINT64_MAX &&
                token->task_run_generation ==
                    atomic_load_explicit(&token->task->run_generation, memory_order_acquire) &&
                token->task->owner_runtime == shard->runtime &&
                token->lease_projection_epoch != UINT64_MAX &&
                token->p_generation != 0U &&
                token->p_generation != UINT64_MAX &&
                token->transfer_sequence == shard->legacy_lease.transfer_sequence &&
                (active_task == NULL || active_task == token->task) &&
                atomic_load_explicit(&token->task->state, memory_order_acquire) ==
                    LLAM_TASK_STATE_BLOCKED_OPAQUE &&
                atomic_load_explicit(&token->task->opaque_blocking_depth,
                                     memory_order_acquire) == 0U &&
                token->task->opaque_uses_redirect &&
                !token->task->opaque_uses_helper);
    }

    if (execution_state == LLAM_WORKER_EXEC_FOREIGN &&
        active_task == NULL &&
        token->state == LLAM_FOREIGN_TOKEN_ACTIVE && token->worker == helper &&
        token->processor == &shard->processor &&
        token->mode == LLAM_FOREIGN_TOKEN_MODE_REDIRECT && token->generation != 0U &&
        token->generation == helper->foreign_token_generation &&
        token->worker_incarnation == incarnation &&
        token->worker_run_generation != 0U &&
        token->worker_run_generation != UINT64_MAX &&
        token->worker_run_generation ==
            atomic_load_explicit(&helper->run_generation, memory_order_acquire) &&
        token->task != NULL &&
        token->task_run_generation != 0U &&
        token->task_run_generation != UINT64_MAX &&
        token->task_run_generation ==
            atomic_load_explicit(&token->task->run_generation, memory_order_acquire) &&
        token->task->owner_runtime == shard->runtime &&
        token->lease_projection_epoch != UINT64_MAX && token->p_generation != 0U &&
        token->p_generation != UINT64_MAX &&
        token->transfer_sequence == shard->legacy_lease.transfer_sequence &&
        atomic_load_explicit(&token->task->state, memory_order_acquire) ==
            LLAM_TASK_STATE_BLOCKED_OPAQUE &&
        atomic_load_explicit(&token->task->opaque_blocking_depth, memory_order_acquire) ==
            1U &&
        token->task->opaque_uses_redirect && !token->task->opaque_uses_helper &&
        /* A fully published token is valid through the ordered guard-release
         * tail: (critical, preempt) is (1,1), (1,0), then (0,0). This grants
         * no execution authority; P remains absent and the exact token above
         * still identifies the helper's live redirect. */
        critical_depth <= 1U && preempt_depth <= critical_depth) {
        return true;
    }
    return false;
}

static bool llam_foreign_token_publish(llam_shard_t *shard,
                                       llam_worker_t *worker,
                                       llam_task_t *task,
                                       const llam_processor_owner_ticket_t *entry_ticket,
                                       uint64_t worker_run_generation,
                                       uint64_t task_run_generation,
                                       uint64_t transfer_sequence,
                                       llam_foreign_token_mode_t mode) {
    llam_legacy_lease_t *lease;
    llam_foreign_token_t *token;
    uint64_t worker_incarnation;
    bool lease_handoff_ready;
    bool published = false;

    if (shard == NULL || worker == NULL || task == NULL || entry_ticket == NULL ||
        transfer_sequence == 0U || mode == LLAM_FOREIGN_TOKEN_MODE_NONE ||
        !shard->opaque_lock_initialized || !shard->lock_initialized ||
        pthread_mutex_lock(&shard->opaque_lock) != 0) {
        return false;
    }
    if (pthread_mutex_lock(&shard->lock) != 0) {
        pthread_mutex_unlock(&shard->opaque_lock);
        return false;
    }

    lease = &shard->legacy_lease;
    token = &worker->foreign_token;
    worker_incarnation = atomic_load_explicit(&worker->incarnation,
                                               memory_order_acquire);
    if (mode == LLAM_FOREIGN_TOKEN_MODE_HELPER) {
        llam_worker_t *helper = &shard->opaque_helper_worker;
        uint64_t helper_incarnation =
            atomic_load_explicit(&helper->incarnation, memory_order_acquire);

        lease_handoff_ready =
            (lease->state == LLAM_LEASE_HELPER_OWNED &&
             lease->owner == helper &&
             lease->owner_incarnation == helper_incarnation &&
             atomic_load_explicit(&helper->current_p, memory_order_acquire) ==
                 &shard->processor) ||
            (atomic_load_explicit(&shard->primary_worker.current_p,
                                  memory_order_acquire) == NULL &&
             llam_foreign_helper_redirect_inflight_locked(shard,
                                                            transfer_sequence));
    } else {
        lease_handoff_ready = lease->state == LLAM_LEASE_REDIRECTED_NO_EXECUTOR &&
                              lease->owner == NULL && lease->return_worker == worker &&
                              lease->return_incarnation == worker_incarnation &&
                              lease->transfer_sequence == transfer_sequence &&
                              atomic_load_explicit(&shard->primary_worker.current_p,
                                                   memory_order_acquire) == NULL &&
                              atomic_load_explicit(&shard->opaque_helper_worker.current_p,
                                                   memory_order_acquire) == NULL;
    }
    if (entry_ticket->processor == &shard->processor &&
        entry_ticket->owner_m == worker &&
        entry_ticket->worker_incarnation == worker_incarnation &&
        entry_ticket->p_generation != 0U && entry_ticket->p_generation != UINT64_MAX &&
        entry_ticket->lease_state != LLAM_LEASE_REDIRECTED_NO_EXECUTOR &&
        task->owner_runtime == shard->runtime &&
        atomic_load_explicit(&worker->run_generation, memory_order_acquire) ==
            worker_run_generation &&
        atomic_load_explicit(&task->run_generation, memory_order_acquire) ==
            task_run_generation &&
        atomic_load_explicit(&task->state, memory_order_acquire) ==
            LLAM_TASK_STATE_BLOCKED_OPAQUE &&
        atomic_load_explicit(&task->opaque_blocking_depth, memory_order_acquire) == 0U &&
        llam_worker_load_active_g(worker) == NULL &&
        atomic_load_explicit(&worker->current_p, memory_order_acquire) == NULL &&
        atomic_load_explicit(&worker->execution_state, memory_order_acquire) ==
            LLAM_WORKER_EXEC_RUNTIME_UNSAFE &&
        atomic_load_explicit(&worker->runtime_critical_depth, memory_order_acquire) != 0U &&
        atomic_load_explicit(&worker->preempt_disable_depth, memory_order_acquire) != 0U &&
        atomic_load_explicit(&worker->binding_incarnation, memory_order_acquire) ==
            worker_incarnation &&
        atomic_load_explicit(&worker->native_thread_live, memory_order_acquire) &&
        pthread_equal(worker->native_thread, pthread_self()) != 0 &&
        token->state == LLAM_FOREIGN_TOKEN_INACTIVE && lease_handoff_ready &&
        ((mode == LLAM_FOREIGN_TOKEN_MODE_HELPER && task->opaque_uses_helper &&
          !task->opaque_uses_redirect) ||
         (mode == LLAM_FOREIGN_TOKEN_MODE_REDIRECT && task->opaque_uses_redirect &&
          !task->opaque_uses_helper)) &&
        worker->foreign_token_generation != UINT64_MAX) {
        uint64_t token_generation = worker->foreign_token_generation + 1U;

        memset(token, 0, sizeof(*token));
        token->worker = worker;
        token->processor = &shard->processor;
        token->task = task;
        token->generation = token_generation;
        token->worker_incarnation = worker_incarnation;
        token->worker_run_generation = worker_run_generation;
        token->task_run_generation = task_run_generation;
        token->lease_projection_epoch = entry_ticket->lease_projection_epoch;
        token->p_generation = entry_ticket->p_generation;
        token->transfer_sequence = transfer_sequence;
        token->state = LLAM_FOREIGN_TOKEN_ACTIVE;
        token->mode = mode;
        worker->foreign_token_generation = token_generation;
        atomic_store_explicit(&task->opaque_blocking_depth, 1U, memory_order_release);
        atomic_store_explicit(&worker->execution_state,
                              LLAM_WORKER_EXEC_FOREIGN,
                              memory_order_release);
        atomic_fetch_add_explicit(&shard->metrics.foreign_p_detaches,
                                  1U,
                                  memory_order_relaxed);
        if (mode == LLAM_FOREIGN_TOKEN_MODE_HELPER) {
            atomic_fetch_add_explicit(&shard->metrics.relief_worker_activations,
                                      1U,
                                      memory_order_relaxed);
        }
        published = true;
    }

    pthread_mutex_unlock(&shard->lock);
    pthread_mutex_unlock(&shard->opaque_lock);
    return published;
}

static bool llam_foreign_token_validate_locked(llam_shard_t *shard,
                                              llam_worker_t *worker,
                                              llam_task_t *task,
                                              llam_foreign_token_state_t expected_state) {
    const llam_foreign_token_t *token;
    const llam_legacy_lease_t *lease;
    uint64_t worker_incarnation;
    bool lease_matches;

    if (shard == NULL || worker == NULL || task == NULL) {
        return false;
    }
    token = &worker->foreign_token;
    lease = &shard->legacy_lease;
    worker_incarnation = atomic_load_explicit(&worker->incarnation,
                                              memory_order_acquire);
    if (token->state != expected_state || token->worker != worker || token->task != task ||
        token->processor != &shard->processor || token->generation == 0U ||
        token->generation != worker->foreign_token_generation ||
        token->worker_incarnation != worker_incarnation || worker_incarnation == 0U ||
        token->worker_run_generation == 0U ||
        token->worker_run_generation == UINT64_MAX ||
        token->worker_run_generation !=
            atomic_load_explicit(&worker->run_generation, memory_order_acquire) ||
        token->task_run_generation == 0U || token->task_run_generation == UINT64_MAX ||
        token->lease_projection_epoch == UINT64_MAX || token->p_generation == 0U ||
        token->p_generation == UINT64_MAX || token->transfer_sequence == 0U ||
        token->mode == LLAM_FOREIGN_TOKEN_MODE_NONE ||
        task->owner_runtime != shard->runtime ||
        atomic_load_explicit(&task->run_generation, memory_order_acquire) !=
            token->task_run_generation ||
        atomic_load_explicit(&task->state, memory_order_acquire) !=
            LLAM_TASK_STATE_BLOCKED_OPAQUE ||
        llam_worker_load_active_g(worker) != NULL ||
        !atomic_load_explicit(&worker->native_thread_live, memory_order_acquire) ||
        atomic_load_explicit(&worker->binding_incarnation, memory_order_acquire) !=
            worker_incarnation || worker->compat_shard != shard ||
        worker->associated_processor != &shard->processor ||
        g_llam_tls_shard != shard || g_llam_tls_processor != &shard->processor ||
        g_llam_tls_worker != worker || g_llam_tls_task != task ||
        pthread_equal(worker->native_thread, pthread_self()) == 0) {
        return false;
    }
    if (expected_state == LLAM_FOREIGN_TOKEN_ACTIVE &&
        (atomic_load_explicit(&worker->current_p, memory_order_acquire) != NULL ||
         atomic_load_explicit(&worker->execution_state, memory_order_acquire) !=
             LLAM_WORKER_EXEC_FOREIGN ||
         atomic_load_explicit(&task->opaque_blocking_depth,
                              memory_order_acquire) == 0U)) {
        return false;
    }
    if (expected_state == LLAM_FOREIGN_TOKEN_RETURNING &&
        (atomic_load_explicit(&worker->execution_state, memory_order_acquire) !=
             LLAM_WORKER_EXEC_RUNTIME_UNSAFE ||
         atomic_load_explicit(&worker->runtime_critical_depth,
                              memory_order_acquire) == 0U ||
         atomic_load_explicit(&worker->preempt_disable_depth,
                              memory_order_acquire) == 0U)) {
        return false;
    }

    if (token->mode == LLAM_FOREIGN_TOKEN_MODE_HELPER) {
        lease_matches =
            lease->state == LLAM_LEASE_HELPER_OWNED &&
            lease->owner == &shard->opaque_helper_worker &&
            lease->owner_incarnation ==
                atomic_load_explicit(&shard->opaque_helper_worker.binding_incarnation,
                                     memory_order_acquire) &&
            atomic_load_explicit(&shard->opaque_helper_worker.current_p,
                                 memory_order_acquire) == &shard->processor;
        if (!lease_matches && lease->state == LLAM_LEASE_TRANSFER_TO_PRIMARY) {
            lease_matches = lease->return_worker == worker &&
                            lease->return_incarnation == worker_incarnation;
        }
        if (!lease_matches && lease->state == LLAM_LEASE_STOPPING) {
            lease_matches = lease->return_worker == worker &&
                            lease->return_incarnation == worker_incarnation;
            if (!lease_matches) {
                llam_worker_t *helper = &shard->opaque_helper_worker;
                uint64_t helper_incarnation = atomic_load_explicit(
                    &helper->incarnation,
                    memory_order_acquire);
                bool unclaimed_helper_stop =
                    lease->owner == helper &&
                    lease->owner_incarnation == helper_incarnation &&
                    lease->return_worker == NULL &&
                    lease->return_incarnation == 0U &&
                    atomic_load_explicit(&helper->current_p,
                                         memory_order_acquire) == &shard->processor;

                lease_matches = unclaimed_helper_stop;
            }
        }
    } else {
        lease_matches = lease->transfer_sequence == token->transfer_sequence &&
                        (lease->state == LLAM_LEASE_REDIRECTED_NO_EXECUTOR ||
                         lease->state == LLAM_LEASE_TRANSFER_TO_PRIMARY ||
                         lease->state == LLAM_LEASE_STOPPING) &&
                        lease->return_worker == worker &&
                        lease->return_incarnation == worker_incarnation;
    }
    return lease_matches;
}

static bool llam_foreign_token_begin_return(llam_shard_t *shard,
                                            llam_worker_t *worker,
                                            llam_task_t *task) {
    bool valid = false;
    bool critical_entered = false;
    bool preempt_disabled = false;

    if (shard == NULL || worker == NULL || task == NULL ||
        !shard->opaque_lock_initialized || !shard->lock_initialized ||
        pthread_mutex_lock(&shard->opaque_lock) != 0) {
        return false;
    }
    if (pthread_mutex_lock(&shard->lock) != 0) {
        pthread_mutex_unlock(&shard->opaque_lock);
        return false;
    }
    if (llam_foreign_token_validate_locked(shard,
                                           worker,
                                           task,
                                           LLAM_FOREIGN_TOKEN_ACTIVE)) {
        critical_entered = llam_worker_runtime_critical_enter(worker);
        preempt_disabled = critical_entered && llam_worker_preempt_disable(worker);
        if (preempt_disabled) {
            worker->foreign_token.state = LLAM_FOREIGN_TOKEN_RETURNING;
            atomic_store_explicit(&task->opaque_blocking_depth,
                                  0U,
                                  memory_order_release);
            valid = true;
        } else if (critical_entered) {
            (void)llam_worker_runtime_critical_leave(worker);
            atomic_store_explicit(&worker->execution_state,
                                  LLAM_WORKER_EXEC_FOREIGN,
                                  memory_order_release);
        }
    }
    pthread_mutex_unlock(&shard->lock);
    pthread_mutex_unlock(&shard->opaque_lock);
    return valid;
}

/**
 * @brief Validate the exact acknowledged STOPPING return used only for unwind.
 *
 * @details
 * Shutdown deliberately suppresses ordinary execution tickets. A worker that
 * has already acknowledged its matching return may nevertheless resume the
 * blocked G long enough to unwind the foreign call and observe cancellation.
 * This is narrower than an execution ticket: it requires the same M, token,
 * processor projection, and transfer ACK, and is only valid after shutdown.
 */
static bool llam_foreign_token_stopping_return_valid_pair_locked(
    llam_shard_t *shard,
    const llam_worker_t *worker,
    const llam_task_t *task,
    const llam_foreign_token_t *token,
    uint64_t worker_incarnation) {
    const llam_legacy_lease_t *lease;
    llam_processor_queue_access_t stopping_access = {0};
    bool projection_valid;
    bool return_claim_valid;

    if (shard == NULL || worker == NULL || task == NULL || token == NULL ||
        shard->runtime == NULL ||
        !atomic_load_explicit(&shard->runtime->shutdown_requested,
                              memory_order_acquire)) {
        return false;
    }
    lease = &shard->legacy_lease;
    projection_valid =
        llam_processor_queue_access_snapshot_pair_locked(shard, &stopping_access) &&
        stopping_access.processor == &shard->processor &&
        stopping_access.actor_m == worker &&
        stopping_access.actor_incarnation == worker_incarnation &&
        stopping_access.projected_owner_m == worker &&
        stopping_access.projected_worker_incarnation == worker_incarnation &&
        stopping_access.lease_state == LLAM_LEASE_STOPPING &&
        stopping_access.owner_state == LLAM_PROCESSOR_OWNER_STOPPING &&
        stopping_access.lease_projection_epoch == lease->epoch &&
        stopping_access.p_generation != 0U &&
        stopping_access.p_generation != UINT64_MAX;
    return_claim_valid =
        (lease->return_worker == worker &&
         lease->return_incarnation == worker_incarnation) ||
        (lease->return_worker == NULL && lease->return_incarnation == 0U);
    return lease->state == LLAM_LEASE_STOPPING && lease->owner == worker &&
           lease->owner_incarnation == worker_incarnation &&
           return_claim_valid &&
           lease->transfer_sequence != 0U &&
           token->transfer_sequence != 0U &&
           (token->mode == LLAM_FOREIGN_TOKEN_MODE_HELPER ||
            (token->mode == LLAM_FOREIGN_TOKEN_MODE_REDIRECT &&
             lease->transfer_sequence == token->transfer_sequence)) &&
           lease->handback_ack_sequence == lease->transfer_sequence &&
           projection_valid &&
           atomic_load_explicit(&worker->current_p, memory_order_acquire) ==
               &shard->processor &&
           llam_worker_load_active_g(worker) == NULL &&
           atomic_load_explicit(&worker->binding_incarnation, memory_order_acquire) ==
               worker_incarnation &&
           atomic_load_explicit(&worker->native_thread_live, memory_order_acquire) &&
           token->state == LLAM_FOREIGN_TOKEN_RETURNING && token->worker == worker &&
           token->processor == &shard->processor && token->task == task &&
           token->generation != 0U &&
           token->generation == worker->foreign_token_generation &&
           token->worker_incarnation == worker_incarnation &&
           token->worker_run_generation ==
               atomic_load_explicit(&worker->run_generation, memory_order_acquire) &&
           token->task_run_generation ==
               atomic_load_explicit(&task->run_generation, memory_order_acquire) &&
           task->owner_runtime == shard->runtime &&
           atomic_load_explicit(&task->state, memory_order_acquire) ==
               LLAM_TASK_STATE_BLOCKED_OPAQUE &&
           atomic_load_explicit(&task->opaque_blocking_depth, memory_order_acquire) == 0U &&
           atomic_load_explicit(&worker->execution_state, memory_order_acquire) ==
               LLAM_WORKER_EXEC_RUNTIME_UNSAFE &&
           atomic_load_explicit(&worker->runtime_critical_depth, memory_order_acquire) !=
               0U &&
           atomic_load_explicit(&worker->preempt_disable_depth, memory_order_acquire) != 0U;
}

static void llam_foreign_token_resume_after_failed_return(llam_shard_t *shard,
                                                          llam_worker_t *worker,
                                                          llam_task_t *task) {
    if (shard == NULL || worker == NULL || task == NULL ||
        pthread_mutex_lock(&shard->opaque_lock) != 0) {
        return;
    }
    pthread_mutex_lock(&shard->lock);
    if (worker->foreign_token.state == LLAM_FOREIGN_TOKEN_RETURNING &&
        worker->foreign_token.worker == worker &&
        worker->foreign_token.worker_run_generation ==
            atomic_load_explicit(&worker->run_generation, memory_order_acquire) &&
        worker->foreign_token.task_run_generation ==
            atomic_load_explicit(&task->run_generation, memory_order_acquire)) {
        worker->foreign_token.state = LLAM_FOREIGN_TOKEN_ACTIVE;
        atomic_store_explicit(&task->opaque_blocking_depth, 1U, memory_order_release);
        if (atomic_load_explicit(&worker->preempt_disable_depth,
                                 memory_order_acquire) != 0U) {
            (void)llam_worker_preempt_enable(worker);
        }
        if (atomic_load_explicit(&worker->runtime_critical_depth,
                                 memory_order_acquire) != 0U) {
            (void)llam_worker_runtime_critical_leave(worker);
        }
        atomic_store_explicit(&worker->execution_state,
                              LLAM_WORKER_EXEC_FOREIGN,
                              memory_order_release);
    }
    pthread_mutex_unlock(&shard->lock);
    pthread_mutex_unlock(&shard->opaque_lock);
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
    LLAM_NATIVE_API_GUARD(-1);

    llam_task_t *task = g_llam_tls_task;
    llam_shard_t *shard = g_llam_tls_shard;
    llam_worker_t *worker = g_llam_tls_worker;
    bool helper_active = false;
    bool from_primary = false;
    bool prefer_redirect = false;
    bool wake_redirect_workers = false;
    bool helper_requested = false;
    bool lease_abort = false;
    bool task_publication_cleared = false;
    bool task_publication_restored = false;
    bool timing = false;
    bool outermost = false;
    bool runtime_critical_entered = false;
    bool preempt_disabled = false;
    llam_processor_owner_ticket_t entry_ticket = {0};
    llam_relief_defer_reason_t relief_reason = LLAM_RELIEF_DEFER_NONE;
    uint64_t worker_run_generation = 0U;
    uint64_t task_run_generation = 0U;
    llam_processor_queue_access_t redirect_release_access;
    uint64_t worker_incarnation = 0U;
    uint64_t transfer_sequence = 0U;
    uint64_t enter_wait_ns = 0U;
    uint64_t enter_wait_start_ns = 0U;

    /* Keep the shard productive while this task blocks in foreign code. */
    if (task == NULL || shard == NULL) {
        return 0;
    }
    if (shard->runtime != NULL &&
        shard->runtime->execution_lease_mode == LLAM_EXECUTION_LEASE_REGISTERED) {
        /* Registered native-affine detach has no adapter yet. Callback offload
         * parks G through the scheduler without mutating the legacy lease. */
        errno = ENOTSUP;
        return -1;
    }
#if (defined(__linux__) && defined(__x86_64__)) || \
    (LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)) || LLAM_ASYNC_CONTEXT_SHARED
    if (shard->runtime != NULL &&
        atomic_load_explicit(&shard->runtime->async_preempt_enabled,
                             memory_order_acquire) != 0U) {
        /* Inline native work could strand this M's affine Gs; use callback offload.
         * Windows must reject redirect as well as helper takeover, before
         * changing P ownership or admitting an unqualified native M. */
        errno = ENOTSUP;
        return -1;
    }
#endif
    if (worker != NULL && worker->compat_shard == shard) {
        atomic_store_explicit(&worker->last_safepoint_ns,
                              llam_now_ns(),
                              memory_order_release);
    }

    if (task->opaque_blocking_depth == 0U) {
        outermost = true;
        if (!llam_processor_relief_snapshot(shard,
                                            worker,
                                            task,
                                            &entry_ticket,
                                            &relief_reason)) {
            if (relief_reason == LLAM_RELIEF_DEFER_UNKNOWN_STATE) {
                atomic_fetch_add_explicit(&shard->metrics.relief_deferred_unknown,
                                          1U,
                                          memory_order_relaxed);
            } else if (relief_reason == LLAM_RELIEF_DEFER_STALE_P_TICKET) {
                atomic_fetch_add_explicit(&shard->metrics.relief_deferred_stale_ticket,
                                          1U,
                                          memory_order_relaxed);
            } else {
                atomic_fetch_add_explicit(&shard->metrics.relief_deferred_unsafe,
                                          1U,
                                          memory_order_relaxed);
            }
            errno = atomic_load_explicit(&shard->runtime->shutdown_requested,
                                         memory_order_acquire) != 0U
                        ? ECANCELED
                        : EBUSY;
            return -1;
        }
        worker_run_generation = atomic_load_explicit(&worker->run_generation,
                                                      memory_order_acquire);
        task_run_generation = atomic_load_explicit(&task->run_generation,
                                                   memory_order_acquire);
        runtime_critical_entered = llam_worker_runtime_critical_enter(worker);
        preempt_disabled = runtime_critical_entered &&
                           llam_worker_preempt_disable(worker);
        if (!preempt_disabled) {
            if (runtime_critical_entered) {
                (void)llam_worker_runtime_critical_leave(worker);
            }
            atomic_store_explicit(&worker->execution_state,
                                  LLAM_WORKER_EXEC_MANAGED_SAFE,
                                  memory_order_release);
            errno = EOVERFLOW;
            return -1;
        }
        timing = llam_opaque_timing_enabled();
        task->opaque_block_started_ns = timing ? llam_now_ns() : 0U;
        task->opaque_uses_helper = false;
        task->opaque_uses_redirect = false;
        from_primary = llam_tls_is_primary_scheduler_for(shard);
        if (worker != NULL) {
            worker_incarnation = atomic_load_explicit(&worker->incarnation,
                                                      memory_order_acquire);
        }
        prefer_redirect = from_primary &&
                          shard->runtime->active_shards > 1U &&
                          (llam_opaque_redirect_fastpath_enabled() || g_llam_tls_opaque_redirect_hint != 0U);

        /* Reserve the transfer while the current primary binding is live. */
        pthread_mutex_lock(&shard->opaque_lock);
        if (from_primary && !prefer_redirect &&
            llam_legacy_lease_request_helper_locked(shard,
                                                    &shard->primary_worker,
                                                    &transfer_sequence)) {
            helper_requested = true;
            shard->primary_thread = pthread_self();
        }
        pthread_mutex_unlock(&shard->opaque_lock);

        if (helper_requested) {
            /* The gate is intentionally outside both scheduler locks. */
            llam_legacy_lease_test_pause(LLAM_LEASE_TEST_POINT_AFTER_TRANSFER_REQUEST);
            pthread_mutex_lock(&shard->opaque_lock);
            if (timing) {
                enter_wait_start_ns = llam_now_ns();
            }
            if (llam_ensure_opaque_helper_locked(shard) != 0) {
                if (atomic_load_explicit(&shard->runtime->shutdown_requested,
                                         memory_order_acquire)) {
                    /* Hard shutdown cancels the reservation before publication. */
                    lease_abort = llam_legacy_lease_rollback_helper_locked(
                        shard,
                        &shard->primary_worker,
                        worker_incarnation,
                        transfer_sequence);
                } else {
                    (void)llam_legacy_lease_rollback_helper_locked(
                        shard,
                        &shard->primary_worker,
                        worker_incarnation,
                        transfer_sequence);
                }
                helper_requested = false;
            }
            pthread_mutex_unlock(&shard->opaque_lock);
            if (timing && enter_wait_start_ns != 0U) {
                uint64_t wait_end_ns = llam_now_ns();

                if (wait_end_ns >= enter_wait_start_ns) {
                    enter_wait_ns = wait_end_ns - enter_wait_start_ns;
                }
            }
        }

        if (helper_requested) {
            /* Finish task/active-G cleanup before clearing the lease owner. */
            pthread_mutex_lock(&shard->opaque_lock);
            pthread_mutex_lock(&shard->lock);
            task->state = LLAM_TASK_STATE_BLOCKED_OPAQUE;
            task->wait_reason = LLAM_WAIT_BLOCKING;
            if (worker != NULL && worker->compat_shard == shard) {
                llam_worker_clear_active_g(worker);
            }
            task_publication_cleared = true;
            llam_trace_shard(shard,
                           task,
                           LLAM_TRACE_STATE,
                           LLAM_TASK_STATE_RUNNING,
                           LLAM_TASK_STATE_BLOCKED_OPAQUE,
                           LLAM_WAIT_BLOCKING);
            pthread_mutex_unlock(&shard->lock);
            if (!llam_legacy_lease_clear_owner_locked(shard,
                                                      &shard->primary_worker,
                                                      worker_incarnation,
                                                      transfer_sequence)) {
                if (atomic_load_explicit(&shard->runtime->shutdown_requested,
                                         memory_order_acquire)) {
                    lease_abort = llam_legacy_lease_rollback_helper_locked(
                        shard,
                        &shard->primary_worker,
                        worker_incarnation,
                        transfer_sequence);
                    if (lease_abort && task_publication_cleared) {
                        llam_restore_running_after_lease_abort_locked(shard,
                                                                      task,
                                                                      worker);
                        task_publication_restored = true;
                    }
                }
                pthread_mutex_unlock(&shard->opaque_lock);
                helper_requested = false;
            } else {
                pthread_mutex_unlock(&shard->opaque_lock);
            }
            if (helper_requested) {
                llam_legacy_lease_test_pause(LLAM_LEASE_TEST_POINT_AFTER_OWNER_CLEAR);
                pthread_mutex_lock(&shard->opaque_lock);
                if (!llam_legacy_lease_grant_helper_locked(
                        shard, &shard->opaque_helper_worker, transfer_sequence)) {
                    if (atomic_load_explicit(&shard->runtime->shutdown_requested,
                                             memory_order_acquire)) {
                        lease_abort = llam_legacy_lease_rollback_helper_locked(
                            shard,
                            &shard->primary_worker,
                            worker_incarnation,
                            transfer_sequence);
                        if (lease_abort && task_publication_cleared) {
                            llam_restore_running_after_lease_abort_locked(shard,
                                                                          task,
                                                                          worker);
                            task_publication_restored = true;
                        }
                    } else {
                        (void)llam_legacy_lease_rollback_helper_locked(
                            shard,
                            &shard->primary_worker,
                            worker_incarnation,
                            transfer_sequence);
                    }
                    helper_requested = false;
                }
                pthread_mutex_unlock(&shard->opaque_lock);
            }
            if (helper_requested) {
                llam_legacy_lease_test_pause(LLAM_LEASE_TEST_POINT_AFTER_GRANT);
                pthread_mutex_lock(&shard->opaque_lock);
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
                if (!helper_active && shard->opaque_compensation_depth > 0U) {
                    shard->opaque_compensation_depth -= 1U;
                }
                pthread_mutex_unlock(&shard->opaque_lock);
            }
        }

        if (lease_abort) {
            if (task_publication_cleared && !task_publication_restored) {
                pthread_mutex_lock(&shard->opaque_lock);
                llam_restore_running_after_lease_abort_locked(shard, task, worker);
                pthread_mutex_unlock(&shard->opaque_lock);
            }
            (void)llam_worker_preempt_enable(worker);
            (void)llam_worker_runtime_critical_leave(worker);
            atomic_store_explicit(&worker->execution_state,
                                  atomic_load_explicit(&worker->current_p,
                                                       memory_order_acquire) ==
                                              &shard->processor &&
                                          llam_worker_load_active_g(worker) == task
                                      ? LLAM_WORKER_EXEC_MANAGED_SAFE
                                      : LLAM_WORKER_EXEC_STOPPING,
                                  memory_order_release);
            task->opaque_block_started_ns = 0U;
            task->opaque_uses_helper = false;
            task->opaque_uses_redirect = false;
            errno = ECANCELED;
            return -1;
        }

        if (!helper_active) {
            pthread_mutex_lock(&shard->opaque_lock);
            /* Failed helper startup and preferred redirect both use a token. */
            if (!llam_legacy_lease_enter_redirect_locked(shard,
                                                         worker,
                                                         worker_incarnation)) {
                pthread_mutex_unlock(&shard->opaque_lock);
                (void)llam_worker_preempt_enable(worker);
                (void)llam_worker_runtime_critical_leave(worker);
                atomic_store_explicit(&worker->execution_state,
                                      LLAM_WORKER_EXEC_STOPPING,
                                      memory_order_release);
                errno = ECANCELED;
                return -1;
            }
            transfer_sequence = shard->legacy_lease.transfer_sequence;
            shard->opaque_redirect_depth += 1U;
            if (shard->opaque_redirect_depth > shard->opaque_redirect_depth_peak) {
                shard->opaque_redirect_depth_peak = shard->opaque_redirect_depth;
            }
            wake_redirect_workers = shard->runtime->active_shards > 1U;
            pthread_mutex_unlock(&shard->opaque_lock);
            llam_legacy_lease_test_pause(LLAM_LEASE_TEST_POINT_AFTER_OWNER_CLEAR);

            pthread_mutex_lock(&shard->opaque_lock);
            pthread_mutex_lock(&shard->lock);
            if (!llam_processor_queue_access_snapshot_pair_locked(
                    shard, &redirect_release_access)) {
                pthread_mutex_unlock(&shard->lock);
                pthread_mutex_unlock(&shard->opaque_lock);
                (void)llam_worker_preempt_enable(worker);
                (void)llam_worker_runtime_critical_leave(worker);
                atomic_store_explicit(&worker->execution_state,
                                      LLAM_WORKER_EXEC_STOPPING,
                                      memory_order_release);
                errno = ECANCELED;
                return -1;
            }
            task->state = LLAM_TASK_STATE_BLOCKED_OPAQUE;
            task->wait_reason = LLAM_WAIT_BLOCKING;
            if (worker != NULL && worker->compat_shard == shard) {
                llam_worker_clear_active_g(worker);
            }
            task->opaque_uses_redirect = true;
            if (shard->opaque_redirect_depth == 1U) {
                shard->metrics.opaque_redirect_activations += 1U;
            }
            llam_activate_opaque_redirect_locked(shard,
                                                  &redirect_release_access,
                                                  task);
            pthread_mutex_unlock(&shard->lock);
            pthread_mutex_unlock(&shard->opaque_lock);
            if (wake_redirect_workers) {
                llam_wake_opaque_redirect_worker(shard->runtime, shard);
            }
        } else {
            task->state = LLAM_TASK_STATE_BLOCKED_OPAQUE;
            task->wait_reason = LLAM_WAIT_BLOCKING;
            task->opaque_uses_helper = true;
            shard->metrics.opaque_compensations += 1U;
            if (timing) {
                pthread_mutex_lock(&shard->lock);
                shard->metrics.opaque_enter_wait_ns += enter_wait_ns;
                shard->metrics.opaque_enter_wait_samples += 1U;
                if (enter_wait_ns > shard->metrics.opaque_enter_wait_max_ns) {
                    shard->metrics.opaque_enter_wait_max_ns = enter_wait_ns;
                }
                pthread_mutex_unlock(&shard->lock);
            }
        }
    }
    if (outermost) {
        llam_foreign_token_mode_t mode = task->opaque_uses_helper
                                             ? LLAM_FOREIGN_TOKEN_MODE_HELPER
                                             : LLAM_FOREIGN_TOKEN_MODE_REDIRECT;

        if (mode == LLAM_FOREIGN_TOKEN_MODE_HELPER) {
            llam_legacy_lease_test_pause(LLAM_LEASE_TEST_POINT_BEFORE_HELPER_FOREIGN_PUBLISH);
        }
        if (!llam_foreign_token_publish(shard,
                                        worker,
                                        task,
                                        &entry_ticket,
                                        worker_run_generation,
                                        task_run_generation,
                                        transfer_sequence,
                                        mode)) {
            atomic_fetch_add_explicit(&shard->metrics.relief_deferred_stale_ticket,
                                      1U,
                                      memory_order_relaxed);
            (void)llam_worker_preempt_enable(worker);
            (void)llam_worker_runtime_critical_leave(worker);
            llam_record_fatal(shard->runtime, EPROTO);
            errno = ECANCELED;
            return -1;
        }
        if (mode == LLAM_FOREIGN_TOKEN_MODE_REDIRECT) {
            llam_legacy_lease_test_pause(LLAM_LEASE_TEST_POINT_AFTER_REDIRECT_FOREIGN_PUBLISH);
        }
        (void)llam_worker_preempt_enable(worker);
        (void)llam_worker_runtime_critical_leave(worker);
        return 0;
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
    LLAM_NATIVE_API_GUARD(-1);

    llam_task_t *task = g_llam_tls_task;
    llam_shard_t *shard = g_llam_tls_shard;
    llam_worker_t *worker = g_llam_tls_worker;
    unsigned depth;

    if (task == NULL || shard == NULL) {
        return 0;
    }
    if (shard->runtime != NULL &&
        shard->runtime->execution_lease_mode == LLAM_EXECUTION_LEASE_REGISTERED) {
        errno = ENOTSUP;
        return -1;
    }
    depth = atomic_load_explicit(&task->opaque_blocking_depth, memory_order_acquire);
    if (depth == 0U) {
        errno = EINVAL;
        return -1;
    }
    if (depth > 1U) {
        bool nested_valid = false;

        if (worker != NULL && shard->opaque_lock_initialized &&
            shard->lock_initialized &&
            pthread_mutex_lock(&shard->opaque_lock) == 0) {
            if (pthread_mutex_lock(&shard->lock) == 0) {
                nested_valid = llam_foreign_token_validate_locked(
                    shard,
                    worker,
                    task,
                    LLAM_FOREIGN_TOKEN_ACTIVE);
                if (nested_valid) {
                    atomic_store_explicit(&task->opaque_blocking_depth,
                                          depth - 1U,
                                          memory_order_release);
                }
                pthread_mutex_unlock(&shard->lock);
            }
            pthread_mutex_unlock(&shard->opaque_lock);
        }
        if (!nested_valid) {
            errno = ECANCELED;
            return -1;
        }
        return 0;
    }
    if (!llam_foreign_token_begin_return(shard, worker, task)) {
        errno = ECANCELED;
        return -1;
    }
    {
        bool used_helper = task->opaque_uses_helper;
        bool used_redirect = task->opaque_uses_redirect;
        bool from_opaque_helper = llam_tls_is_opaque_helper_for(shard);
        bool deactivate_redirect = false;
        bool handback_acknowledged = false;
        bool timing = llam_opaque_timing_enabled();
        uint64_t worker_incarnation = worker != NULL
                                          ? atomic_load_explicit(&worker->incarnation,
                                                                 memory_order_acquire)
                                          : 0U;
        uint64_t return_sequence = 0U;
        bool return_requested = false;
        bool leave_validation_failed = false;
        uint64_t now_ns = timing ? llam_now_ns() : 0U;
        uint64_t block_ns = 0U;
        uint64_t leave_wait_ns = 0U;
        uint64_t leave_wait_start_ns = 0U;

        if (timing && task->opaque_block_started_ns > 0U && now_ns >= task->opaque_block_started_ns) {
            block_ns = now_ns - task->opaque_block_started_ns;
        }
        task->opaque_block_started_ns = 0U;
        /* The RETURNING token still exposes this mode to the primary's
         * concurrent detach. Retire it with the token under both locks. */
        task->opaque_block_count += 1U;
        if (timing) {
            task->last_opaque_block_ns = block_ns;
        }
        if (timing && block_ns > task->max_opaque_block_ns) {
            task->max_opaque_block_ns = block_ns;
        }

        llam_legacy_lease_test_pause(LLAM_LEASE_TEST_POINT_BEFORE_FOREIGN_RETURN_REQUEST);

        if (used_helper) {
            if (timing && !from_opaque_helper) {
                leave_wait_start_ns = llam_now_ns();
            }
            /* Retire the primary's worker-local task before requesting ACK. */
            if (worker != NULL && worker->compat_shard == shard) {
                pthread_mutex_lock(&shard->lock);
                llam_worker_clear_active_g(worker);
                pthread_mutex_unlock(&shard->lock);
            }
            pthread_mutex_lock(&shard->opaque_lock);
            uint64_t primary_incarnation = atomic_load_explicit(
                &shard->primary_worker.incarnation,
                memory_order_acquire);

            return_requested = llam_legacy_lease_request_return_locked(
                shard,
                &shard->primary_worker,
                primary_incarnation,
                &return_sequence);
            if (shard->opaque_compensation_depth > 0U) {
                shard->opaque_compensation_depth -= 1U;
            }
            llam_opaque_wake_signal(shard);
#if defined(__linux__)
            if (shard->opaque_helper_thread_started &&
                atomic_load_explicit(&shard->opaque_helper_opaque_wait,
                                     memory_order_acquire) == 0U) {
                llam_kick_shard(shard);
            }
#endif
            pthread_mutex_unlock(&shard->opaque_lock);

            /* The helper publishes its ACK only after clearing its active task. */
            if (!from_opaque_helper) {
                for (;;) {
                    bool helper_active;
                    bool helper_failed;

                    pthread_mutex_lock(&shard->opaque_lock);
                    helper_active = shard->opaque_helper_active;
                    helper_failed = shard->opaque_helper_failed;
                    if (!helper_active || helper_failed) {
                        pthread_mutex_unlock(&shard->opaque_lock);
                        break;
                    }
                    llam_opaque_spin_until_helper_hint(shard, 0U);
                    if (!shard->opaque_helper_active || shard->opaque_helper_failed) {
                        pthread_mutex_unlock(&shard->opaque_lock);
                        break;
                    }
                    llam_opaque_wake_wait(shard);
                    pthread_mutex_unlock(&shard->opaque_lock);
                }
            }
            pthread_mutex_lock(&shard->opaque_lock);
            handback_acknowledged = return_requested &&
                                    llam_legacy_lease_handback_acknowledged_locked(
                                        shard,
                                        &shard->primary_worker,
                                        primary_incarnation,
                                        return_sequence);
            if (!handback_acknowledged &&
                llam_legacy_lease_teardown_return_locked(shard,
                                                         &shard->primary_worker,
                                                         primary_incarnation)) {
                handback_acknowledged = true;
            }
            pthread_mutex_unlock(&shard->opaque_lock);
            leave_validation_failed = !handback_acknowledged;
            if (timing && leave_wait_start_ns != 0U) {
                uint64_t wait_end_ns = llam_now_ns();

                if (wait_end_ns >= leave_wait_start_ns) {
                    leave_wait_ns = wait_end_ns - leave_wait_start_ns;
                }
            }
        }

        if (used_redirect) {
            /* A helper redirect must clear its local active task before ACK. */
            if (worker != NULL && worker->compat_shard == shard) {
                pthread_mutex_lock(&shard->lock);
                llam_worker_clear_active_g(worker);
                pthread_mutex_unlock(&shard->lock);
            }
            pthread_mutex_lock(&shard->opaque_lock);
            if (shard->opaque_redirect_depth > 0U) {
                shard->opaque_redirect_depth -= 1U;
                deactivate_redirect = shard->opaque_redirect_depth == 0U;
            }
            if (deactivate_redirect) {
                return_requested = llam_legacy_lease_request_return_locked(
                    shard,
                    worker,
                    worker_incarnation,
                    &return_sequence);
                if (return_requested) {
                    handback_acknowledged =
                        llam_legacy_lease_complete_handback_locked(shard,
                                                                   worker,
                                                                   return_sequence);
                }
                if (!handback_acknowledged &&
                    llam_legacy_lease_teardown_return_locked(shard,
                                                             worker,
                                                             worker_incarnation)) {
                    handback_acknowledged = true;
                }
                leave_validation_failed = !handback_acknowledged;
            }
            pthread_mutex_unlock(&shard->opaque_lock);
            if (handback_acknowledged) {
                llam_legacy_lease_test_pause(LLAM_LEASE_TEST_POINT_AFTER_HANDBACK_ACK);
            }
        }

        if (leave_validation_failed) {
            /* Keep the task on the return boundary; do not publish a false owner. */
            if (used_redirect && deactivate_redirect) {
                pthread_mutex_lock(&shard->opaque_lock);
                shard->opaque_redirect_depth += 1U;
                pthread_mutex_unlock(&shard->opaque_lock);
            }
            llam_foreign_token_resume_after_failed_return(shard, worker, task);
            errno = ECANCELED;
            return -1;
        }

        if (pthread_mutex_lock(&shard->opaque_lock) != 0) {
            leave_validation_failed = true;
        } else if (pthread_mutex_lock(&shard->lock) != 0) {
            pthread_mutex_unlock(&shard->opaque_lock);
            leave_validation_failed = true;
        } else {
            llam_processor_owner_ticket_t current_ticket = {0};
            llam_foreign_token_t *token = worker != NULL
                                                ? &worker->foreign_token
                                                : NULL;
            bool token_return_tuple_valid =
                token != NULL && token->state == LLAM_FOREIGN_TOKEN_RETURNING &&
                token->worker == worker && token->processor == &shard->processor &&
                token->generation == worker->foreign_token_generation &&
                token->worker_incarnation == worker_incarnation &&
                token->worker_run_generation ==
                    atomic_load_explicit(&worker->run_generation,
                                         memory_order_acquire) &&
                token->task_run_generation ==
                    atomic_load_explicit(&task->run_generation,
                                         memory_order_acquire) &&
                atomic_load_explicit(&worker->execution_state,
                                     memory_order_acquire) ==
                    LLAM_WORKER_EXEC_RUNTIME_UNSAFE &&
                atomic_load_explicit(&worker->runtime_critical_depth,
                                     memory_order_acquire) != 0U &&
                atomic_load_explicit(&worker->preempt_disable_depth,
                                     memory_order_acquire) != 0U;
            bool ordinary_return_ticket_valid =
                llam_processor_owner_ticket_snapshot_pair_locked(shard,
                                                                  &current_ticket) &&
                current_ticket.owner_m == worker &&
                current_ticket.worker_incarnation == worker_incarnation;
            bool stopping_return_valid =
                llam_foreign_token_stopping_return_valid_pair_locked(shard,
                                                                      worker,
                                                                      task,
                                                                      token,
                                                                      worker_incarnation);
            bool return_tuple_valid = token_return_tuple_valid &&
                                      (ordinary_return_ticket_valid ||
                                       stopping_return_valid);

            if (!return_tuple_valid) {
                pthread_mutex_unlock(&shard->lock);
                pthread_mutex_unlock(&shard->opaque_lock);
                leave_validation_failed = true;
            } else {
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
                if (!llam_worker_preempt_enable(worker) ||
                    !llam_worker_runtime_critical_leave(worker)) {
                    llam_record_fatal(shard->runtime, EINVAL);
                    pthread_mutex_unlock(&shard->lock);
                    pthread_mutex_unlock(&shard->opaque_lock);
                    leave_validation_failed = true;
                } else {
                    memset(&worker->foreign_token, 0,
                           sizeof(worker->foreign_token));
                    task->opaque_uses_helper = false;
                    task->opaque_uses_redirect = false;
                    task->state = LLAM_TASK_STATE_RUNNING;
                    task->wait_reason = LLAM_WAIT_NONE;
                    if (worker != NULL && worker->compat_shard == shard) {
                        atomic_store_explicit(&worker->last_safepoint_ns,
                                              timing && now_ns != 0U
                                                  ? now_ns
                                                  : llam_now_ns(),
                                              memory_order_release);
                        /* Publish only after a fresh P ticket or exact stop-return is validated. */
                        llam_worker_begin_g_run(worker, task);
                        llam_worker_publish_active_g(worker, task);
                    }
                    atomic_fetch_add_explicit(&shard->metrics.foreign_p_reacquires,
                                              1U,
                                              memory_order_relaxed);
                    llam_trace_shard(shard,
                                     task,
                                     LLAM_TRACE_STATE,
                                     LLAM_TASK_STATE_BLOCKED_OPAQUE,
                                     LLAM_TASK_STATE_RUNNING,
                                     LLAM_WAIT_BLOCKING);
                    pthread_mutex_unlock(&shard->lock);
                    pthread_mutex_unlock(&shard->opaque_lock);
                    leave_validation_failed = false;
                }
            }
        }
        if (leave_validation_failed) {
            if (used_redirect && deactivate_redirect) {
                pthread_mutex_lock(&shard->opaque_lock);
                shard->opaque_redirect_depth += 1U;
                pthread_mutex_unlock(&shard->opaque_lock);
            }
            llam_foreign_token_resume_after_failed_return(shard, worker, task);
            errno = ECANCELED;
            return -1;
        }
        (void)handback_acknowledged;
    }
    return 0;
}
