/**
 * @file src/engine/scheduler/block.c
 * @brief Blocking compensation engine for tasks that enter opaque blocking regions.
 *
 * @details
 * Blocking workers execute callbacks submitted by ::llam_call_blocking. The
 * scheduler task that submitted the job is parked while a background worker runs
 * the callback, captures its result and errno, and reinjects the task. Jobs may
 * be cancelled before or during execution through an atomic state transition.
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

#if defined(LLAM_ENABLE_TEST_HOOKS)
static atomic_uint g_llam_block_pool_test_create_calls;
static atomic_uint g_llam_block_pool_test_fail_create_on;

void llam_block_pool_test_fail_create_on(unsigned call_index) {
    atomic_store_explicit(&g_llam_block_pool_test_fail_create_on,
                          call_index,
                          memory_order_release);
}

void llam_block_pool_test_reset_create_hook(void) {
    atomic_store_explicit(&g_llam_block_pool_test_create_calls, 0U, memory_order_release);
    atomic_store_explicit(&g_llam_block_pool_test_fail_create_on, 0U, memory_order_release);
}

unsigned llam_block_pool_test_create_calls(void) {
    return atomic_load_explicit(&g_llam_block_pool_test_create_calls,
                                memory_order_acquire);
}
#endif

/**
 * @brief Create one blocking worker without publishing a failed thread handle.
 *
 * @details
 * The caller holds @c rt->block_lock. The temporary handle is copied into the
 * confirmed prefix only after @c pthread_create succeeds, so partial
 * initialization and shutdown never join an indeterminate output value.
 */
static int llam_block_pool_create_one_locked(llam_runtime_t *rt) {
    pthread_t thread;
    unsigned started;
    int rc;

    if (rt == NULL || rt->block_threads == NULL || rt->block_worker_count == 0U) {
        errno = EINVAL;
        return -1;
    }
    started = atomic_load_explicit(&rt->block_threads_started, memory_order_acquire);
    if (started >= rt->block_worker_count) {
        errno = EAGAIN;
        return -1;
    }

#if defined(LLAM_ENABLE_TEST_HOOKS)
    {
        unsigned call_index =
            atomic_fetch_add_explicit(&g_llam_block_pool_test_create_calls,
                                      1U,
                                      memory_order_acq_rel) +
            1U;
        unsigned fail_on =
            atomic_load_explicit(&g_llam_block_pool_test_fail_create_on,
                                 memory_order_acquire);

        if (fail_on != 0U && call_index == fail_on) {
            atomic_fetch_add_explicit(&rt->block_thread_create_failures,
                                      1U,
                                      memory_order_relaxed);
            errno = EAGAIN;
            return -1;
        }
    }
#endif

    rc = pthread_create(&thread, NULL, llam_block_worker_main, rt);
    if (rc != 0) {
        atomic_fetch_add_explicit(&rt->block_thread_create_failures,
                                  1U,
                                  memory_order_relaxed);
        errno = rc;
        return -1;
    }
    rt->block_threads[started] = thread;
    atomic_store_explicit(&rt->block_threads_started,
                          started + 1U,
                          memory_order_release);
    return 0;
}

/**
 * @brief Start exactly the resolved initialization floor for the blocking pool.
 */
int llam_block_pool_start_min(llam_runtime_t *rt) {
    unsigned target;
    int rc = 0;

    if (rt == NULL || !rt->block_lock_initialized) {
        errno = EINVAL;
        return -1;
    }
    target = rt->resource_plan.blocking_min;
    if (target > rt->block_worker_count) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&rt->block_lock);
    while (atomic_load_explicit(&rt->block_threads_started, memory_order_acquire) <
           target) {
        if (llam_block_pool_create_one_locked(rt) != 0) {
            rc = -1;
            break;
        }
    }
    pthread_mutex_unlock(&rt->block_lock);
    return rc;
}

/**
 * @brief Grow the blocking pool by at most one worker for newly published load.
 *
 * @details
 * The caller holds @c rt->block_lock and has already reserved one pending-job
 * credit, but has not linked the job into the worker-visible queue. A failed
 * first worker creation is fatal to that submission because no executor can
 * drain it. Once at least one worker is confirmed, later growth failures are
 * best-effort: the existing worker can still drain the queue.
 */
int llam_block_pool_ensure_capacity_locked(llam_runtime_t *rt,
                                           unsigned pending_after_enqueue) {
    unsigned started;
    int saved_errno;

    if (rt == NULL || !rt->block_lock_initialized ||
        rt->block_threads == NULL || rt->block_worker_count == 0U) {
        errno = EINVAL;
        return -1;
    }
    started = atomic_load_explicit(&rt->block_threads_started, memory_order_acquire);
    if (pending_after_enqueue <= started || started >= rt->block_worker_count) {
        return 0;
    }
    if (llam_block_pool_create_one_locked(rt) == 0) {
        return 0;
    }

    saved_errno = errno != 0 ? errno : EAGAIN;
    started = atomic_load_explicit(&rt->block_threads_started, memory_order_acquire);
    if (started == 0U) {
        errno = saved_errno;
        return -1;
    }
    return 0;
}

bool llam_runtime_note_block_pending(llam_runtime_t *rt, unsigned amount) {
    unsigned pending;

    if (amount == 0U) {
        return true;
    }
    if (rt == NULL) {
        errno = EINVAL;
        return false;
    }

    pending = atomic_load_explicit(&rt->block_pending, memory_order_acquire);
    for (;;) {
        if (UINT_MAX - pending < amount) {
            /*
             * block_pending drives compensation pressure and shutdown drain
             * diagnostics.  A saturated counter must reject new jobs instead
             * of wrapping to zero and hiding queued blocking work.
             */
            llam_record_fatal(rt, EOVERFLOW);
            errno = EOVERFLOW;
            return false;
        }
        if (atomic_compare_exchange_weak_explicit(&rt->block_pending,
                                                  &pending,
                                                  pending + amount,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            return true;
        }
    }
}

bool llam_runtime_complete_block_pending(llam_runtime_t *rt, unsigned amount) {
    unsigned pending;

    if (amount == 0U) {
        return true;
    }
    if (rt == NULL) {
        errno = EINVAL;
        return false;
    }

    pending = atomic_load_explicit(&rt->block_pending, memory_order_acquire);
    while (pending >= amount) {
        if (atomic_compare_exchange_weak_explicit(&rt->block_pending,
                                                  &pending,
                                                  pending - amount,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            return true;
        }
    }

    /*
     * A completion without a matching pending credit means a stale worker entry
     * or corrupted queue state reached the blocking pool.  Preserve the counter
     * so later diagnostics still see the original state.
     */
    llam_record_fatal(rt, EINVAL);
    errno = EINVAL;
    return false;
}

bool llam_runtime_note_block_active(llam_runtime_t *rt, unsigned amount) {
    unsigned active;

    if (amount == 0U) {
        return true;
    }
    if (rt == NULL) {
        errno = EINVAL;
        return false;
    }

    active = atomic_load_explicit(&rt->block_active, memory_order_acquire);
    for (;;) {
        if (UINT_MAX - active < amount) {
            /*
             * Active blocking workers are sampled by diagnostics and watchdog
             * scaling.  Saturation indicates corrupted accounting; reject the
             * job before running user code so the counter cannot wrap to zero.
             */
            llam_record_fatal(rt, EOVERFLOW);
            errno = EOVERFLOW;
            return false;
        }
        if (atomic_compare_exchange_weak_explicit(&rt->block_active,
                                                  &active,
                                                  active + amount,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            llam_atomic_update_peak(&rt->block_active_peak, active + amount);
            return true;
        }
    }
}

bool llam_runtime_complete_block_active(llam_runtime_t *rt, unsigned amount) {
    unsigned active;

    if (amount == 0U) {
        return true;
    }
    if (rt == NULL) {
        errno = EINVAL;
        return false;
    }

    active = atomic_load_explicit(&rt->block_active, memory_order_acquire);
    while (active >= amount) {
        if (atomic_compare_exchange_weak_explicit(&rt->block_active,
                                                  &active,
                                                  active - amount,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            return true;
        }
    }

    /*
     * A completion without an active-worker credit means a stale completion
     * raced through the worker path or the active counter was corrupted.
     */
    llam_record_fatal(rt, EINVAL);
    errno = EINVAL;
    return false;
}

/**
 * @brief Main loop for a blocking-worker thread.
 *
 * Workers sleep until a block job is queued or shutdown is requested. A queued
 * job must transition from @c LLAM_BLOCK_JOB_QUEUED to
 * @c LLAM_BLOCK_JOB_RUNNING before execution; cancelled jobs fail that transition
 * and are released without running the callback.
 *
 * @param arg Runtime pointer.
 *
 * @return Always @c NULL.
 */
void *llam_block_worker_main(void *arg) {
    llam_runtime_t *rt = arg;

    atomic_fetch_add_explicit(&rt->block_threads_entered, 1U, memory_order_relaxed);
    atomic_fetch_add_explicit(&rt->block_threads_live, 1U, memory_order_release);
    llam_tune_block_worker_thread();

    for (;;) {
        llam_block_job_t *job;

        pthread_mutex_lock(&rt->block_lock);
        while (rt->block_head == NULL &&
               !atomic_load_explicit(&rt->shutdown_requested, memory_order_acquire)) {
#if defined(__linux__)
            unsigned wait_seq = atomic_load_explicit(&rt->block_wake_seq, memory_order_acquire);

            /* Sleep on a monotonic wake sequence so producers can signal without condvar bookkeeping. */
            pthread_mutex_unlock(&rt->block_lock);
            (void)llam_linux_futex_wait_private(&rt->block_wake_seq, wait_seq);
            pthread_mutex_lock(&rt->block_lock);
#elif LLAM_PLATFORM_WINDOWS
            unsigned wait_seq = atomic_load_explicit(&rt->block_wake_seq, memory_order_acquire);

            pthread_mutex_unlock(&rt->block_lock);
            (void)WaitOnAddress((volatile VOID *)&rt->block_wake_seq, &wait_seq, sizeof(wait_seq), INFINITE);
            pthread_mutex_lock(&rt->block_lock);
#else
            pthread_cond_wait(&rt->block_cv, &rt->block_lock);
#endif
        }
        if (rt->block_head == NULL) {
            if (atomic_load_explicit(&rt->shutdown_requested, memory_order_acquire)) {
                pthread_mutex_unlock(&rt->block_lock);
                break;
            }
            /*
             * Natural llam_run() drain briefly raises stop_requested to wake
             * scheduler workers, but block workers are runtime-lifetime
             * helpers. Only runtime_shutdown() may terminate them; otherwise
             * loop instead of treating an empty queue as a real job. This also
             * makes the worker robust to spurious condvar wakes on POSIX.
             */
            pthread_mutex_unlock(&rt->block_lock);
            continue;
        }

        job = rt->block_head;
        rt->block_head = job->next;
        if (rt->block_head == NULL) {
            rt->block_tail = NULL;
        }
        pthread_mutex_unlock(&rt->block_lock);

        {
            unsigned expected = LLAM_BLOCK_JOB_QUEUED;

            if (!atomic_compare_exchange_strong(&job->state, &expected, LLAM_BLOCK_JOB_RUNNING)) {
                (void)llam_runtime_complete_block_pending(rt, 1U);
                llam_block_job_release(rt, job);
                continue;
            }
        }

        if (!llam_runtime_note_block_active(rt, 1U)) {
            int error_code = errno != 0 ? errno : EOVERFLOW;

            (void)llam_runtime_complete_block_pending(rt, 1U);
            atomic_store_explicit(&job->result, NULL, memory_order_release);
            atomic_store_explicit(&job->error_code, error_code, memory_order_release);
            atomic_store_explicit(&job->state, LLAM_BLOCK_JOB_ABORTED, memory_order_release);
            if (job->task != NULL) {
                job->task->blocking_result = NULL;
                job->task->blocking_errno = error_code;
                atomic_store_explicit(&job->task->wake_error_code, error_code, memory_order_release);
            }
            if (job->wait_node != NULL) {
                job->wait_node->error_code = error_code;
            }
            if (job->task != NULL &&
                (job->wait_node == NULL || llam_wait_node_prepare_wake(job->wait_node))) {
                llam_reinject_task_on_shard(rt,
                                          job->task,
                                          job->task->parked_shard,
                                          true,
                                          LLAM_TRACE_WAKE,
                                          LLAM_WAIT_BLOCKING);
            }
            llam_block_job_release(rt, job);
            errno = error_code;
            continue;
        }
        errno = 0;
        /*
         * Runtime dumps can sample an active block job while the helper is
         * finishing user code. Publish result/errno atomically so diagnostics
         * never race with callback completion.
         */
        atomic_store_explicit(&job->result, job->fn(job->arg), memory_order_release);
        atomic_store_explicit(&job->error_code, errno, memory_order_release);
        (void)llam_runtime_complete_block_active(rt, 1U);
        (void)llam_runtime_complete_block_pending(rt, 1U);

        {
            unsigned expected = LLAM_BLOCK_JOB_RUNNING;

            if (!atomic_compare_exchange_strong(&job->state, &expected, LLAM_BLOCK_JOB_FINISHED)) {
                /*
                 * Cancellation can mark a RUNNING callback as ABORTED, but it
                 * cannot safely stop user code already executing on this
                 * worker.  Wake the task only after the callback has returned
                 * so the caller cannot free or reuse callback-owned state while
                 * the worker is still touching it.
                 */
                if (expected == LLAM_BLOCK_JOB_ABORTED) {
                    atomic_store_explicit(&job->task->wake_error_code, ECANCELED, memory_order_release);
                    if (job->wait_node != NULL) {
                        job->wait_node->error_code = ECANCELED;
                    }
                    if (job->wait_node == NULL || llam_wait_node_prepare_wake(job->wait_node)) {
                        llam_reinject_task_on_shard(rt,
                                                  job->task,
                                                  job->task->parked_shard,
                                                  true,
                                                  LLAM_TRACE_WAKE,
                                                  LLAM_WAIT_CANCEL);
                    }
                }
                llam_block_job_release(rt, job);
                continue;
            }
        }

        job->task->blocking_result = atomic_load_explicit(&job->result, memory_order_acquire);
        job->task->blocking_errno = atomic_load_explicit(&job->error_code, memory_order_acquire);
        if (job->wait_node != NULL) {
            job->wait_node->error_code = 0;
        }
        if (job->wait_node == NULL || llam_wait_node_prepare_wake(job->wait_node)) {
            llam_reinject_task_on_shard(rt,
                                      job->task,
                                      job->task->parked_shard,
                                      true,
                                      LLAM_TRACE_BLOCK_COMPLETE,
                                      LLAM_WAIT_BLOCKING);
        }
        llam_block_job_release(rt, job);
    }

    atomic_fetch_sub_explicit(&rt->block_threads_live, 1U, memory_order_release);
    atomic_fetch_add_explicit(&rt->block_threads_exited, 1U, memory_order_release);
    return NULL;
}
