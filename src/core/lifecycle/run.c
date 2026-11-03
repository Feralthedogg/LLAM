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
 * See LICENSES/Apache-2.0.txt.
 */

#include "runtime_internal.h"
#include "managed_scheduler.h"
#if LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
#include "windows_async_lifetime.h"
#endif
#if defined(__linux__) && defined(__x86_64__) && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
#include "async_preempt_abi.h"
#endif

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
    int rc;
    unsigned i;

    if (llam_runtime_begin_public_op(rt, &pinned_runtime) != 0) {
        return -1;
    }
    rt = pinned_runtime;
    if (g_llam_tls_worker != NULL) {
        llam_runtime_end_public_op(pinned_runtime);
        errno = EBUSY;
        return -1;
    }
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
    if (rt->execution_lease_mode == LLAM_EXECUTION_LEASE_REGISTERED) {
        rc = llam_managed_runtime_run(rt);
        if (rc == 0 && !atomic_load_explicit(&rt->shutdown_requested, memory_order_acquire) &&
            !atomic_load_explicit(&rt->draining, memory_order_acquire))
            atomic_store_explicit(&rt->stop_requested, false, memory_order_release);
        /* No runtime access after completion: destroy may now free storage. */
        atomic_store_explicit(&rt->exec_started, false, memory_order_release);
        return rc;
    }
    if (llam_async_context_run_begin(rt) != 0) {
        atomic_store_explicit(&rt->exec_started, false, memory_order_release);
        return -1;
    }
    /*
     * Shard zero is driven by this borrowed caller thread. Publish its worker
     * binding before auxiliary shards start so no producer or watchdog can
     * observe an ownerless shard that is already eligible to run. The watchdog
     * derives its target from this binding, and the scheduler loop recognizes
     * it without consuming a second incarnation.
     */
    llam_scheduler_bind_identity(&rt->shards[0],
                                 &rt->shards[0].primary_worker,
                                 &rt->shards[0].scheduler_ctx);
    if (atomic_load_explicit(&rt->fatal_errno, memory_order_acquire) != 0) {
        rc = atomic_load_explicit(&rt->fatal_errno, memory_order_acquire);
        i = 1U;
        goto start_failed;
    }
    if (llam_async_context_driver_ready(rt) != 0) {
        rc = errno;
        i = 1U;
        goto start_failed;
    }
    for (i = 1; i < rt->active_shards; ++i) {
        rc = 0;
#if LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64 && \
    defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT) && defined(LLAM_ENABLE_TEST_HOOKS)
        if (llam_windows_async_lifetime_test_take_worker_failure(
                LLAM_WINDOWS_ASYNC_TEST_FAIL_THREAD_START, i)) {
            rc = EAGAIN;
        }
#endif
#if defined(LLAM_ENABLE_TEST_HOOKS) && defined(__linux__) && defined(__x86_64__) && \
    defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
        if (atomic_load_explicit(&rt->async_preempt_enabled, memory_order_acquire) != 0U &&
            llam_async_preempt_test_take_worker_failure(LLAM_ASYNC_TEST_FAIL_THREAD_START, i)) {
            rc = EAGAIN;
        }
#endif
        if (rc == 0) {
            rc = pthread_create(&rt->shards[i].thread, NULL, llam_shard_worker_main, &rt->shards[i]);
        }
        if (rc != 0) {
start_failed:
            // pthread_create returns the error code directly; errno may be
            // stale.  Stop and join any shards already started in this run
            // before exposing the failure to the caller.
            /* Release pre-dispatch waiters; live NEW Gs cannot drain themselves. */
            llam_async_context_run_abort(rt, rc);
            llam_record_fatal(rt, rc);
            llam_request_stop(rt);
            while (i > 1U) {
                --i;
                if (rt->shards[i].thread_started) {
                    pthread_join(rt->shards[i].thread, NULL);
                    rt->shards[i].thread_started = false;
                }
            }
            if (!llam_async_context_driver_close(rt)) {
                abort();
            }
            llam_scheduler_unbind_identity(&rt->shards[0],
                                           &rt->shards[0].primary_worker);
            llam_scheduler_clear_identity();
            llam_async_context_run_end(rt);
            atomic_store_explicit(&rt->exec_started, false, memory_order_release);
            errno = rc;
            return -1;
        }
        rt->shards[i].thread_started = true;
    }

    if (llam_async_context_run_commit(rt) != 0) {
        rc = errno;
        i = rt->active_shards;
        goto start_failed;
    }
    llam_scheduler_loop(&rt->shards[0]);

    for (i = 1; i < rt->active_shards; ++i) {
        if (rt->shards[i].thread_started) {
            pthread_join(rt->shards[i].thread, NULL);
            rt->shards[i].thread_started = false;
        }
    }
    llam_async_context_run_end(rt);

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
    if (!atomic_load_explicit(&rt->draining, memory_order_acquire))
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
    LLAM_NATIVE_API_GUARD(-1);

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
    LLAM_NATIVE_API_GUARD(-1);

    /*
     * The no-handle API remains the default-runtime entry point for unmanaged
     * host threads, but from a managed task it must target that task's owner
     * runtime. Otherwise explicit-runtime tasks that call the legacy stop
     * wrapper accidentally poke the default runtime and leave their own
     * scheduler running.
     */
    return llam_runtime_request_stop_rt(llam_runtime_current_owner());
}
