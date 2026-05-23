/**
 * @file src/core/runtime_run.c
 * @brief Top-level scheduler run loop coordination.
 *
 * @details
 * ::llam_run starts secondary shard workers, runs shard 0 on the calling thread,
 * joins the workers when the scheduler drains, and propagates any fatal runtime
 * error recorded by worker threads. Legacy default-runtime wrappers and
 * explicit handle APIs both delegate to the runtime-owned internal entry point
 * in this file.
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
 * @brief Run the initialized runtime until all scheduled work completes.
 *
 * The caller owns shard 0's scheduler loop. Additional shards are started as
 * pthread workers before shard 0 enters ::llam_scheduler_loop. After the primary
 * scheduler exits, all secondary workers are joined and any recorded fatal error
 * is surfaced through @c errno.
 *
 * @return 0 when the runtime drains normally.
 * @return -1 with @c errno set if the runtime is not initialized, already
 *         running, a worker cannot be started, or a worker records a fatal
 *         runtime error.
 *
 * @see llam_runtime_init
 * @see llam_scheduler_loop
 */
int llam_runtime_run_rt(llam_runtime_t *rt) {
    bool expected_started = false;
    int rc;
    unsigned i;

    if (llam_runtime_check_handle(rt) != 0) {
        return -1;
    }
    if (!atomic_load_explicit(&rt->initialized, memory_order_acquire)) {
        errno = EINVAL;
        return -1;
    }
    /*
     * Each runtime may have only one active scheduler driver. A load then store
     * admits two unmanaged callers that cross the check at the same time, so
     * claim the run token with CAS before touching shard worker state.
     */
    if (!atomic_compare_exchange_strong_explicit(&rt->exec_started,
                                                 &expected_started,
                                                 true,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        errno = EINVAL;
        return -1;
    }
    for (i = 1; i < rt->active_shards; ++i) {
        rc = pthread_create(&rt->shards[i].thread, NULL, llam_shard_worker_main, &rt->shards[i]);
        if (rc != 0) {
            // pthread_create returns the error code directly; errno may be
            // stale.  Stop and join any shards already started in this run
            // before exposing the failure to the caller.
            llam_record_fatal(rt, rc);
            llam_request_stop(rt);
            while (i > 1U) {
                --i;
                if (rt->shards[i].thread_started) {
                    pthread_join(rt->shards[i].thread, NULL);
                    rt->shards[i].thread_started = false;
                }
            }
            atomic_store_explicit(&rt->exec_started, false, memory_order_release);
            errno = rc;
            return -1;
        }
        rt->shards[i].thread_started = true;
    }

    llam_scheduler_loop(&rt->shards[0]);

    for (i = 1; i < rt->active_shards; ++i) {
        if (rt->shards[i].thread_started) {
            pthread_join(rt->shards[i].thread, NULL);
            rt->shards[i].thread_started = false;
        }
    }

    if (atomic_load(&rt->fatal_errno) != 0) {
        errno = atomic_load(&rt->fatal_errno);
        atomic_store_explicit(&rt->exec_started, false, memory_order_release);
        return -1;
    }

    /*
     * Natural drain uses the same stop flag that explicit runtime-stop requests
     * use to wake idle workers out of their scheduler loops.  Once a run has
     * completed cleanly, clear that internal drain signal so embedders can
     * spawn more work and call llam_run() again on the same initialized runtime.
     */
    atomic_store_explicit(&rt->stop_requested, false, memory_order_release);
    /*
     * Publish run completion only after the final runtime-state access above.
     * Host-side destroy waits on this flag before freeing explicit runtime
     * storage, so clearing it earlier would expose a small UAF window.
     */
    atomic_store_explicit(&rt->exec_started, false, memory_order_release);
    return 0;
}

int llam_run(void) {
    return llam_runtime_run_rt(llam_runtime_default_storage());
}

/**
 * @brief Request cooperative runtime stop from any thread.
 *
 * @return 0 on success, or -1 with @c errno set when the runtime is not initialized.
 */
int llam_runtime_request_stop_rt(llam_runtime_t *rt) {
    if (llam_runtime_check_handle(rt) != 0) {
        return -1;
    }
    if (!atomic_load_explicit(&rt->initialized, memory_order_acquire)) {
        errno = EINVAL;
        return -1;
    }
    llam_request_stop(rt);
    return 0;
}

int llam_runtime_request_stop(void) {
    return llam_runtime_request_stop_rt(llam_runtime_default_storage());
}
