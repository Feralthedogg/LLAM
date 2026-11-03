/**
 * @file src/core/registry/handle.c
 * @brief Explicit runtime-handle lifecycle API.
 *
 * @details
 * LLAM 2.x treats explicit handles as the canonical embedding boundary.  The
 * legacy singleton APIs remain wrappers around ::llam_runtime_default, while
 * embedders can create independent heap-backed runtime objects through this
 * translation unit.
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

/**
 * @brief Return the legacy default runtime handle.
 *
 * @details
 * This intentionally does not allocate or retain anything.  It is a stable
 * alias for the process-default runtime used by the legacy convenience APIs.
 */
llam_runtime_t *llam_runtime_default(void) {
    LLAM_NATIVE_API_GUARD(NULL);

    return llam_runtime_default_storage();
}

/**
 * @brief Allocate and initialize an explicit runtime handle.
 *
 * @details
 * Keep the "out is NULL until fully initialized" contract explicit here: FFI
 * callers can retry or clean up safely after any validation/init failure.
 */
int llam_runtime_create(const llam_runtime_opts_t *opts, size_t opts_size, llam_runtime_t **out) {
    if (LLAM_UNLIKELY(llam_native_domain_blocked())) {
        if (out != NULL) *out = NULL;
        errno = ENOTSUP;
        return -1;
    }

    llam_runtime_t *runtime;

    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }
    *out = NULL;

    runtime = calloc(1U, sizeof(*runtime));
    if (runtime == NULL) {
        errno = ENOMEM;
        return -1;
    }

    if (llam_runtime_init_rt(runtime, opts, opts_size, true) != 0) {
        free(runtime);
        return -1;
    }
    *out = runtime;
    return 0;
}

/**
 * @brief Drive the runtime through the explicit-handle entry point.
 *
 * @details
 * The validation happens in ::llam_runtime_run_rt so run/shutdown/stats paths
 * share the same non-default-handle errno contract.
 */
int llam_runtime_request_stop_handle(llam_runtime_t *runtime) {
    LLAM_NATIVE_API_GUARD(-1);
    if (runtime == NULL) { errno = EINVAL; return -1; }
    if ((g_llam_tls_task != NULL || g_llam_tls_shard != NULL) &&
        llam_runtime_current_owner() != runtime) { errno = EXDEV; return -1; }
    llam_runtime_t *pinned = NULL;
    if (llam_runtime_begin_public_op(runtime, &pinned) != 0) return -1;
    llam_request_stop(pinned);
    llam_runtime_end_public_op(pinned);
    return 0;
}

int llam_runtime_run_handle(llam_runtime_t *runtime) {
    LLAM_NATIVE_API_GUARD(-1);

    if (runtime == NULL) {
        errno = EINVAL;
        return -1;
    }
    return llam_runtime_run_rt(runtime);
}

/**
 * @brief Tear down an explicit runtime handle.
 *
 * @details
 * NULL is kept as a convenience alias for legacy shutdown.  Unknown handles are
 * ignored because this function has no errno channel; fallible handle APIs
 * reject them before touching runtime-owned state.
 */
void llam_runtime_destroy(llam_runtime_t *runtime) {
    LLAM_NATIVE_API_GUARD();

    llam_runtime_destroy_rt(runtime);
}

/* The caller holds a public-operation pin. Configuration and worker slot
 * storage are immutable until shutdown; native liveness and counters are atomic.
 * Flags describe this sample, not a reservation against concurrent callers. */
static void llam_runtime_resources_snapshot(llam_runtime_t *rt, llam_runtime_resources_t *out) {
    memset(out, 0, sizeof(*out));
    out->worker_count = rt->active_shards;
    out->blocking_worker_count = rt->block_worker_count;
    /* Sample the run/operation publications before their protected counters;
     * a finished publisher's release must precede a quiescent observation. */
    bool running = atomic_load_explicit(&rt->exec_started, memory_order_acquire);
    size_t operations = atomic_load_explicit(&rt->active_ops, memory_order_acquire);
    out->admitted_tasks = atomic_load_explicit(&rt->admitted_tasks, memory_order_relaxed);
    out->live_tasks = llam_runtime_live_tasks(rt);
    out->blocking_jobs = atomic_load_explicit(&rt->block_pending, memory_order_acquire);
    out->max_tasks = rt->max_tasks;
    out->max_blocking_jobs = rt->max_blocking_jobs;
    out->public_operations = operations > 0U ? operations - 1U : 0U;
    for (size_t i = 0U; i < rt->worker_slot_count; ++i) {
        if (rt->worker_slots[i] != NULL &&
            atomic_load_explicit(&rt->worker_slots[i]->native_thread_live, memory_order_acquire))
            out->bound_native_workers += 1U;
    }
    if (running)
        out->flags |= LLAM_RUNTIME_RESOURCE_F_RUNNING;
    if (atomic_load_explicit(&rt->draining, memory_order_acquire))
        out->flags |= LLAM_RUNTIME_RESOURCE_F_DRAINING;
    if ((out->flags & LLAM_RUNTIME_RESOURCE_F_RUNNING) == 0U) {
        if (out->live_tasks != 0U) out->flags |= LLAM_RUNTIME_RESOURCE_F_NEEDS_RUNNER;
        else if (out->blocking_jobs == 0U && out->public_operations == 0U)
            out->flags |= LLAM_RUNTIME_RESOURCE_F_QUIESCENT;
    }
}

int llam_runtime_get_resources_ex(llam_runtime_t *runtime,
                                   llam_runtime_resources_t *out, size_t out_size) {
    LLAM_NATIVE_API_GUARD(-1);
    if (runtime == NULL || out == NULL || out_size == 0U) { errno = EINVAL; return -1; }
    llam_runtime_t *pinned = NULL;
    if (llam_runtime_begin_public_op(runtime, &pinned) != 0) return -1;
    llam_runtime_resources_t info;
    llam_runtime_resources_snapshot(pinned, &info);
    memcpy(out, &info, out_size < sizeof(info) ? out_size : sizeof(info));
    llam_runtime_end_public_op(pinned);
    return 0;
}

int llam_runtime_drain_until(llam_runtime_t *runtime, uint64_t deadline_ns,
                              llam_runtime_resources_t *out, size_t out_size) {
    LLAM_NATIVE_API_GUARD(-1);
    if (runtime == NULL || (out != NULL && out_size == 0U)) { errno = EINVAL; return -1; }
    if (g_llam_tls_task != NULL || g_llam_tls_worker != NULL) { errno = EBUSY; return -1; }
    llam_runtime_t *pinned = NULL;
    if (llam_runtime_begin_public_op(runtime, &pinned) != 0) return -1;
    llam_runtime_resources_t info;
    llam_runtime_resources_snapshot(pinned, &info);
    int error = 0;
    if ((info.flags & LLAM_RUNTIME_RESOURCE_F_NEEDS_RUNNER) != 0U) {
        error = EBUSY;
    } else {
        /* Release closes future admission. Operations already admitted are
         * retained by their runtime/run pins and counted until they finish. */
        atomic_store_explicit(&pinned->draining, true, memory_order_release);
        llam_request_stop(pinned);
        for (;;) {
            llam_runtime_resources_snapshot(pinned, &info);
            if ((info.flags & LLAM_RUNTIME_RESOURCE_F_QUIESCENT) != 0U) break;
            uint64_t now = llam_now_ns();
            if (now >= deadline_ns) { error = ETIMEDOUT; break; }
            uint64_t remaining = deadline_ns - now;
            struct timespec interval = {0, (long)(remaining < UINT64_C(1000000)
                                                    ? remaining : UINT64_C(1000000))};
            (void)nanosleep(&interval, NULL);
        }
    }
    if (out != NULL) memcpy(out, &info, out_size < sizeof(info) ? out_size : sizeof(info));
    llam_runtime_end_public_op(pinned);
    if (error != 0) { errno = error; return -1; }
    return 0;
}
