/**
 * @file src/engine/runtime_block.c
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
                atomic_fetch_sub(&rt->block_pending, 1U);
                llam_block_job_release(rt, job);
                continue;
            }
        }

        {
            unsigned active = atomic_fetch_add(&rt->block_active, 1U) + 1U;

            llam_atomic_update_peak(&rt->block_active_peak, active);
        }
        errno = 0;
        /*
         * Runtime dumps can sample an active block job while the helper is
         * finishing user code. Publish result/errno atomically so diagnostics
         * never race with callback completion.
         */
        atomic_store_explicit(&job->result, job->fn(job->arg), memory_order_release);
        atomic_store_explicit(&job->error_code, errno, memory_order_release);
        atomic_fetch_sub(&rt->block_active, 1U);
        atomic_fetch_sub(&rt->block_pending, 1U);

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

    return NULL;
}
