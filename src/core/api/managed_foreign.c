/*
 * Copyright 2026 Feralthedogg
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
#include "managed_async_adapter.h"
#include "managed_scheduler.h"
#if defined(LLAM_ENABLE_TEST_HOOKS)
#include "runtime_test_managed_foreign.h"

static _Atomic(llam_managed_foreign_test_gate_t *) g_managed_foreign_test_gate;

void llam_managed_foreign_test_set_gate(llam_managed_foreign_test_gate_t *gate) {
    atomic_store_explicit(&g_managed_foreign_test_gate, gate, memory_order_release);
}

static void llam_managed_foreign_test_point(llam_task_t *task, unsigned point) {
    llam_managed_foreign_test_gate_t *gate = atomic_load_explicit(&g_managed_foreign_test_gate,
                                                                 memory_order_acquire);
    if (gate != NULL && gate->runtime == task->owner_runtime && gate->observe != NULL)
        gate->observe(task, point, gate->data);
}
#endif

static void llam_managed_foreign_reopen(llam_worker_t *worker, llam_task_t *task) {
    if (g_llam_tls_worker != worker || g_llam_tls_task != task ||
        llam_worker_load_active_g(worker) != task ||
        atomic_load_explicit(&worker->preempt_disable_depth, memory_order_acquire) != 1U ||
        !llam_worker_preempt_enable(worker)) abort();
    llam_worker_publish_active_g(worker, task);
}

static int llam_call_native_affine_impl(llam_blocking_fn fn, void *arg, void **out, bool cleanup) {

    int incoming_errno = errno;
#if LLAM_PLATFORM_WINDOWS
    DWORD incoming_last_error = GetLastError();
#endif
    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }
    *out = NULL;
    if (fn == NULL) {
        errno = EINVAL;
        return -1;
    }
    llam_task_t *task = g_llam_tls_task;
    llam_worker_t *worker = g_llam_tls_worker;
    if (task == NULL || worker == NULL || task->owner_runtime == NULL ||
        task->managed_execution_class != LLAM_MANAGED_EXECUTION_NATIVE_AFFINE ||
        task->managed_execution_worker != worker ||
        worker->role != LLAM_WORKER_ROLE_NATIVE_AFFINE) {
        errno = ENOTSUP;
        return -1;
    }
    llam_runtime_t *runtime = task->owner_runtime;
    for (;;) {
        if (!cleanup && (atomic_load_explicit(&runtime->stop_requested, memory_order_acquire) ||
                         atomic_load_explicit(&runtime->shutdown_requested, memory_order_acquire) ||
                         llam_task_cancel_token_is_cancelled(task))) {
            errno = ECANCELED;
            return -1;
        }
        if (!llam_worker_has_safe_managed_g(worker, g_llam_tls_processor, task) ||
            !llam_worker_preempt_disable(worker)) {
            errno = ENOTSUP;
            return -1;
        }
        /* Close capture admission before taking the native service locks in
         * the quiescence query. An earlier publisher must finish under those
         * locks and is visible to the query; a later publisher observes this
         * disable depth through the same lock handoff and cannot reserve an op.
         * Retain the gate until callback completion and fresh P reacquisition. */
        if (llam_managed_async_task_quiescent(task)) {
#if defined(LLAM_ENABLE_TEST_HOOKS)
            llam_managed_foreign_test_point(task, LLAM_MANAGED_FOREIGN_TEST_QUIESCENT);
#endif
            break;
        }
#if defined(LLAM_ENABLE_TEST_HOOKS)
        llam_managed_foreign_test_point(task, LLAM_MANAGED_FOREIGN_TEST_PENDING);
#endif
        llam_managed_foreign_reopen(worker, task);
        /* A real scheduler return retires prior restore observations/readers.
         * Never spin or enter native callback code while those obligations can
         * still require this M to service a suspended continuation. */
        llam_yield();
    }

    llam_managed_foreign_token_t token = {0};
    if (llam_managed_foreign_release(task, &token) != 0) {
        int saved_errno = errno;
        llam_managed_foreign_reopen(worker, task);
        errno = saved_errno;
        return -1;
    }
    /* Release commits the P handoff and hides active G before native effects.
     * These latch-only guards exclude callback reentry; they own no P locks. */
    llam_managed_native_enter();
    errno = incoming_errno;
#if LLAM_PLATFORM_WINDOWS
    SetLastError(incoming_last_error);
#endif
    void *result = fn(arg);
    int callback_errno = errno;
#if LLAM_PLATFORM_WINDOWS
    DWORD callback_last_error = GetLastError();
#endif
    llam_managed_native_leave();
#if LLAM_PLATFORM_DARWIN && LLAM_ASYNC_CONTEXT_SHARED
    /* The callback runs without saved native continuations. Reapply the
     * binding's asynchronous-signal policy before any managed PC can resume. */
    llam_async_shared_rearm_native_signals(worker);
#endif

    /* Callback completion cannot be converted to an early cancellation return:
     * reacquire a fresh execute/drain lease on this same M before exposing the
     * result or allowing any managed instruction to continue. */
    llam_managed_foreign_reacquire(&token);
    llam_managed_foreign_reopen(worker, task);
    *out = result;
    errno = callback_errno;
#if LLAM_PLATFORM_WINDOWS
    SetLastError(callback_last_error);
#endif
    return 0;
}

int llam_call_native_affine_result(llam_blocking_fn fn, void *arg, void **out) {
    LLAM_NATIVE_API_GUARD(-1);
    return llam_call_native_affine_impl(fn, arg, out, false);
}

void llam_call_native_affine_cleanup(llam_blocking_fn fn, void *arg) {
    void *ignored = NULL;
    /* Registration admitted this task's execution class. Failing this internal
     * invariant must not silently discard an accepted ownership obligation. */
    if (llam_call_native_affine_impl(fn, arg, &ignored, true) != 0) abort();
}
