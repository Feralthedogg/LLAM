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
 * See LICENSES/Apache-2.0.txt.
 */

/*
 * A deliberately single-task, test-only production-dispatcher oracle. This
 * does not turn on Windows async preemption in ordinary builds. The controller
 * and CONTEXT/XSTATE allocation are owned by the native conformance test.
 */
#include "runtime_internal.h"
#include "windows_async_probe.h"

/* Static atomic pointers start null; MSVC rejects ATOMIC_VAR_INIT(NULL) here. */
static _Atomic(llam_windows_async_probe_t *) g_windows_async_probe;
static _Atomic(llam_windows_async_probe_t *) g_windows_async_probe_second;

void llam_windows_async_probe_install(llam_windows_async_probe_t *probe) {
    atomic_store_explicit(&g_windows_async_probe_second, NULL,
                          memory_order_release);
    atomic_store_explicit(&g_windows_async_probe, probe, memory_order_release);
}

void llam_windows_async_probe_install_pair(llam_windows_async_probe_t *first,
                                           llam_windows_async_probe_t *second) {
    atomic_store_explicit(&g_windows_async_probe_second, second,
                          memory_order_release);
    atomic_store_explicit(&g_windows_async_probe, first, memory_order_release);
}

static llam_windows_async_probe_t *llam_windows_async_probe_for(
    const llam_task_t *task) {
    llam_windows_async_probe_t *probe = atomic_load_explicit(
        &g_windows_async_probe, memory_order_acquire);

    if (probe != NULL && probe->task == task) {
        return probe;
    }
    probe = atomic_load_explicit(&g_windows_async_probe_second,
                                 memory_order_acquire);
    return probe != NULL && probe->task == task ? probe : NULL;
}

void llam_windows_async_probe_record_scheduler_errno(llam_task_t *task,
                                                      int scheduler_errno) {
    llam_windows_async_probe_t *probe = llam_windows_async_probe_for(task);

    if (probe != NULL) {
        probe->scheduler_errno_before = scheduler_errno;
    }
}

static int llam_windows_async_probe_call_native_oracle(
    llam_windows_async_probe_t *probe) {
    int scheduler_errno = llam_thread_errno_load();
    DWORD scheduler_last_error = GetLastError();
    int result = probe->native_oracle != NULL ? probe->native_oracle() : 42;

    llam_thread_errno_store(scheduler_errno);
    SetLastError(scheduler_last_error);
    return result;
}

/* Called at the first native return seam, before scheduler errno is restored. */
void llam_windows_async_probe_arrival(llam_task_t *task) {
    llam_windows_async_probe_t *probe = llam_windows_async_probe_for(task);
    unsigned phase;

    if (probe == NULL) {
        return;
    }
    phase = atomic_load_explicit(&probe->phase, memory_order_acquire);
    if (phase == LLAM_WINDOWS_ASYNC_PROBE_RESTORED) {
        atomic_store_explicit(&probe->phase,
                              LLAM_WINDOWS_ASYNC_PROBE_FINISHED,
                              memory_order_release);
        return;
    }
    if (phase != LLAM_WINDOWS_ASYNC_PROBE_CAPTURED) {
        return;
    }
    if (g_llam_tls_worker != probe->worker ||
        atomic_load_explicit(&probe->worker->run_generation,
                             memory_order_acquire) !=
            probe->capture_run_generation) {
        atomic_store_explicit(&probe->phase,
                              LLAM_WINDOWS_ASYNC_PROBE_FAILED,
                              memory_order_release);
        return;
    }

    /* The controller cannot read these target-local values from CONTEXT. */
    probe->captured_last_error = GetLastError();
    probe->captured_errno = llam_thread_errno_load();
    task->saved_errno = probe->captured_errno;
    probe->arrival_run_generation = probe->capture_run_generation;
    atomic_store_explicit(&probe->phase, LLAM_WINDOWS_ASYNC_PROBE_ARRIVED,
                          memory_order_release);
}

/* The gate saves a new, live scheduler frame and never returns on G restore. */
bool llam_windows_async_probe_resume(llam_ctx_t *scheduler_ctx,
                                     llam_task_t *task) {
#if defined(_MSC_VER)
    /* The test oracle's GNU assembly gate is unavailable in the MSVC backend. */
    (void)scheduler_ctx;
    (void)task;
    return false;
#else
    llam_windows_async_probe_t *probe = llam_windows_async_probe_for(task);

    if (probe == NULL ||
        atomic_load_explicit(&probe->phase, memory_order_acquire) !=
            LLAM_WINDOWS_ASYNC_PROBE_READY) {
        return false;
    }
    if (g_llam_tls_worker != probe->worker ||
        atomic_load_explicit(&probe->worker->run_generation,
                             memory_order_acquire) <=
            probe->capture_run_generation) {
        atomic_store_explicit(&probe->phase,
                              LLAM_WINDOWS_ASYNC_PROBE_FAILED,
                              memory_order_release);
        abort();
    }
    probe->restore_run_generation = atomic_load_explicit(
        &probe->worker->run_generation, memory_order_acquire);
    SetLastError(probe->captured_last_error);
    llam_thread_errno_store(probe->captured_errno);
    llam_windows_async_save_gate(scheduler_ctx, &probe->phase);
    return true; /* Reached only after a later genuine G return. */
#endif
}

/* Called only after the runtime has cleared active-G on the native stack. */
void llam_windows_async_probe_scheduler_return(llam_shard_t *shard,
                                               llam_task_t *task) {
    llam_windows_async_probe_t *probe = llam_windows_async_probe_for(task);
    unsigned phase;

    if (probe == NULL) {
        return;
    }
    phase = atomic_load_explicit(&probe->phase, memory_order_acquire);
    if (phase == LLAM_WINDOWS_ASYNC_PROBE_FINISHED &&
        probe->check_return_state) {
        if (llam_thread_errno_load() != probe->scheduler_errno_before ||
            llam_windows_async_probe_call_native_oracle(probe) != 42) {
            atomic_store_explicit(&probe->phase,
                                  LLAM_WINDOWS_ASYNC_PROBE_FAILED,
                                  memory_order_release);
            abort();
        }
        probe->scheduler_state_checks += 1U;
        return;
    }
    if (phase != LLAM_WINDOWS_ASYNC_PROBE_ARRIVED) {
        return;
    }
    if (llam_thread_errno_load() != probe->scheduler_errno_before) {
        atomic_store_explicit(&probe->phase,
                              LLAM_WINDOWS_ASYNC_PROBE_FAILED,
                              memory_order_release);
        abort();
    }
    if (probe->native_oracle != NULL) {
        probe->native_oracle_result =
            llam_windows_async_probe_call_native_oracle(probe);
        if (probe->native_oracle_result != 42) {
            atomic_store_explicit(&probe->phase,
                                  LLAM_WINDOWS_ASYNC_PROBE_FAILED,
                                  memory_order_release);
            abort();
        }
    }
    atomic_store_explicit(&probe->phase, LLAM_WINDOWS_ASYNC_PROBE_READY,
                          memory_order_release);
#if defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
    if (task->async_preempt_task != NULL &&
        atomic_load_explicit(&shard->runtime->async_preempt_enabled,
                             memory_order_acquire) != 0U) {
        /* Production return processing already owns READY/requeue. */
        return;
    }
#endif
    if (probe->defer_requeue) {
        return;
    }
    if (probe->release_peer != NULL &&
        atomic_load_explicit(&probe->release_peer->phase,
                             memory_order_acquire) ==
            LLAM_WINDOWS_ASYNC_PROBE_READY) {
        llam_reinject_task_on_shard(shard->runtime, probe->release_peer->task,
                                    shard->id, false, LLAM_TRACE_STATE,
                                    LLAM_WAIT_NONE);
    }
    llam_reinject_task_on_shard(shard->runtime, task, shard->id, false,
                                LLAM_TRACE_STATE, LLAM_WAIT_NONE);
}

uintptr_t llam_windows_async_probe_stack_rsp(llam_task_t *task) {
    llam_windows_async_probe_t *probe = llam_windows_async_probe_for(task);

    if (probe != NULL &&
        atomic_load_explicit(&probe->phase, memory_order_acquire) ==
            LLAM_WINDOWS_ASYNC_PROBE_ARRIVED) {
        return probe->captured_rsp;
    }
    return (uintptr_t)task->ctx.rsp;
}

void llam_windows_async_probe_note_reclaim_wait(llam_task_t *task) {
    llam_windows_async_probe_t *probe = llam_windows_async_probe_for(task);

    if (probe != NULL) {
        atomic_store_explicit(&probe->reclaim_wait_seen, 1U,
                              memory_order_release);
    }
}
