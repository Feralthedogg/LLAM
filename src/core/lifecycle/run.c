/**
 * @file src/core/lifecycle/run.c
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

#if defined(LLAM_ENABLE_TEST_HOOKS)
static atomic_uint g_llam_shard_create_test_calls;
static atomic_uint g_llam_shard_create_test_fail_on;

void llam_runtime_test_fail_shard_create_on(unsigned call_index) {
    atomic_store_explicit(&g_llam_shard_create_test_fail_on,
                          call_index,
                          memory_order_release);
}

void llam_runtime_test_reset_shard_create_hook(void) {
    atomic_store_explicit(&g_llam_shard_create_test_calls,
                          0U,
                          memory_order_release);
    atomic_store_explicit(&g_llam_shard_create_test_fail_on,
                          0U,
                          memory_order_release);
}

unsigned llam_runtime_test_shard_create_calls(void) {
    return atomic_load_explicit(&g_llam_shard_create_test_calls,
                                memory_order_acquire);
}
#endif

static int llam_runtime_create_shard_thread(llam_shard_t *shard) {
    pthread_t thread;
    int rc;

#if defined(LLAM_ENABLE_TEST_HOOKS)
    {
        unsigned call_index =
            atomic_fetch_add_explicit(&g_llam_shard_create_test_calls,
                                      1U,
                                      memory_order_acq_rel) +
            1U;
        unsigned fail_on =
            atomic_load_explicit(&g_llam_shard_create_test_fail_on,
                                 memory_order_acquire);

        if (fail_on != 0U && call_index == fail_on) {
            return EAGAIN;
        }
    }
#endif
    rc = pthread_create(&thread, NULL, llam_shard_worker_main, shard);
    if (rc == 0) {
        shard->thread = thread;
    }
    return rc;
}

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
    llam_runtime_t *pinned_runtime = NULL;
    bool expected_started = false;
    int result = 0;
    int result_errno = 0;
    unsigned i;

    if (llam_runtime_begin_public_op(rt, &pinned_runtime) != 0) {
        return -1;
    }
    rt = pinned_runtime;
    if (!atomic_load_explicit(&rt->initialized, memory_order_acquire)) {
        llam_runtime_end_public_op(pinned_runtime);
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
        llam_runtime_end_public_op(pinned_runtime);
        errno = EINVAL;
        return -1;
    }
    /*
     * exec_started now protects runtime-owned scheduler state from teardown;
     * release the public-op pin so shutdown can request stop and wait on the
     * run token instead of being blocked for the full scheduler lifetime.
     */
    llam_runtime_end_public_op(pinned_runtime);
    if (llam_runtime_capture_driver_affinity(rt) != 0) {
        result = -1;
        result_errno = errno;
        goto finish;
    }
    /* active_shards is the immutable configured capacity from the resource plan. */
    for (i = 1; i < rt->active_shards; ++i) {
        int create_rc = llam_runtime_create_shard_thread(&rt->shards[i]);

        if (create_rc != 0) {
            // pthread_create returns the error code directly; errno may be
            // stale.  Stop and join any shards already started in this run
            // before exposing the failure to the caller.
            llam_record_fatal(rt, create_rc);
            llam_request_stop(rt);
            while (i > 1U) {
                --i;
                if (rt->shards[i].thread_started) {
                    pthread_join(rt->shards[i].thread, NULL);
                    rt->shards[i].thread_started = false;
                }
            }
            result = -1;
            result_errno = create_rc;
            goto finish;
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
        result = -1;
        result_errno = atomic_load(&rt->fatal_errno);
    } else {
        /*
         * Natural drain uses the same stop flag that explicit runtime-stop
         * requests use to wake idle workers out of their scheduler loops. Once
         * a run has completed cleanly, clear that internal drain signal so
         * embedders can spawn more work and call llam_run() again.
         */
        atomic_store_explicit(&rt->stop_requested, false, memory_order_release);
    }

finish:
    if (llam_runtime_restore_driver_affinity(rt) != 0 && result == 0) {
        result = -1;
        result_errno = errno;
    }
    /*
     * Publish run completion only after the final runtime-state access above.
     * Host-side destroy waits on this flag before freeing explicit runtime
     * storage, so clearing it earlier would expose a small UAF window.
     */
    atomic_store_explicit(&rt->exec_started, false, memory_order_release);
    if (result != 0) {
        errno = result_errno != 0 ? result_errno : EIO;
    }
    return result;
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
    llam_runtime_t *pinned_runtime = NULL;

    if (llam_runtime_begin_public_op(rt, &pinned_runtime) != 0) {
        return -1;
    }
    rt = pinned_runtime;
    if (!atomic_load_explicit(&rt->initialized, memory_order_acquire)) {
        llam_runtime_end_public_op(pinned_runtime);
        errno = EINVAL;
        return -1;
    }
    llam_request_stop(rt);
    llam_runtime_end_public_op(pinned_runtime);
    return 0;
}

int llam_runtime_request_stop(void) {
    /*
     * The no-handle API remains the default-runtime entry point for unmanaged
     * host threads, but from a managed task it must target that task's owner
     * runtime. Otherwise explicit-runtime tasks that call the legacy stop
     * wrapper accidentally poke the default runtime and leave their own
     * scheduler running.
     */
    return llam_runtime_request_stop_rt(llam_runtime_current_owner());
}
