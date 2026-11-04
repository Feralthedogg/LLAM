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

/* Test-only real LLAM dispatcher gate, not a production Windows backend. */
#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

#include "runtime_internal.h"
#include "windows_async_probe.h"
#if defined(LLAM_WINDOWS_ASYNC_LIFETIME_ORACLE)
#include "windows_async_lifetime.h"
#endif

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ORACLE_CONTEXT_FLAGS (CONTEXT_ALL | CONTEXT_XSTATE)
#define ORACLE_SUPPORTED_XSTATE UINT64_C(0x807)

extern void win_llam_async_leaf(void);
extern const unsigned char win_llam_async_leaf_begin[];
extern const unsigned char win_llam_async_leaf_end[];
extern int win_llam_async_native_oracle(void);
extern volatile LONG g_win_llam_async_cleanup_count;

_Atomic(unsigned) g_win_llam_async_release = ATOMIC_VAR_INIT(0U);
static _Atomic(unsigned) g_leaf_entered = ATOMIC_VAR_INIT(0U);
static _Atomic(unsigned) g_companion_progress = ATOMIC_VAR_INIT(0U);
static _Atomic(unsigned) g_task_finished = ATOMIC_VAR_INIT(0U);
static _Atomic(unsigned) g_task_error = ATOMIC_VAR_INIT(0U);
static _Atomic(unsigned) g_second_armed = ATOMIC_VAR_INIT(0U);
#if defined(LLAM_WINDOWS_ASYNC_LIFETIME_ORACLE)
static _Atomic(unsigned) g_rebind_progress = ATOMIC_VAR_INIT(0U);
#endif

typedef struct oracle_context {
    void *buffer;
    DWORD length;
    PCONTEXT context;
} oracle_context_t;

typedef struct oracle_state {
    llam_windows_async_probe_t probe;
    HANDLE target;
    ULONG_PTR native_stack_low;
    ULONG_PTR native_stack_high;
    oracle_context_t captured;
    oracle_context_t redirect;
    DWORD64 features;
    llam_ctx_t original_g_ctx;
    atomic_uint controller_result;
    atomic_uint context_mutations;
    atomic_uint stale_rejections;
#if defined(LLAM_WINDOWS_ASYNC_LIFETIME_ORACLE)
    llam_windows_async_operation_t operation;
    atomic_uint run_completed;
    atomic_uint deferred_seen;
    atomic_uint companion_after_deferred;
    atomic_uint premutation_refusals;
    bool peer_mode;
    bool task_scan_mode;
    bool stop_deferred_mode;
    bool uncertain_capture_mode;
    bool uncertain_restore_mode;
#endif
} oracle_state_t;

static void oracle_fail(const char *where, unsigned code) {
    fprintf(stderr, "llam-async-oracle: %s failed: %u\n", where, code);
    ExitProcess(1U);
}

#if !defined(LLAM_WINDOWS_ASYNC_LIFETIME_ORACLE)
static int oracle_context_init(oracle_context_t *storage, DWORD64 features) {
    DWORD length = 0U;
    PCONTEXT context = NULL;

    memset(storage, 0, sizeof(*storage));
    SetLastError(ERROR_SUCCESS);
    if (InitializeContext(NULL, ORACLE_CONTEXT_FLAGS, &context, &length) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
        length < sizeof(CONTEXT)) {
        return -1;
    }
    storage->buffer = calloc(1U, length);
    if (storage->buffer == NULL) {
        return -1;
    }
    storage->length = length;
    if (!InitializeContext(storage->buffer, ORACLE_CONTEXT_FLAGS,
                           &storage->context, &length) ||
        storage->context == NULL || length > storage->length ||
        !SetXStateFeaturesMask(storage->context, features)) {
        return -1;
    }
    return 0;
}

static int oracle_context_reset(oracle_context_t *storage,
                                DWORD64 features) {
    DWORD length = storage->length;
    PCONTEXT context = NULL;

    if (!InitializeContext(storage->buffer, ORACLE_CONTEXT_FLAGS,
                           &context, &length) ||
        context == NULL || length > storage->length ||
        !SetXStateFeaturesMask(context, features)) {
        return -1;
    }
    storage->context = context;
    return 0;
}
#endif

static BOOL oracle_set_context(oracle_state_t *state, PCONTEXT context) {
    atomic_fetch_add_explicit(&state->context_mutations, 1U,
                              memory_order_relaxed);
    return SetThreadContext(state->target, context);
}

/*
 * A stale external operation may inspect a suspended gate, but must not
 * mutate it. The legitimate controller services the still-spinning gate next.
 */
static bool oracle_reject_stale_gate(oracle_state_t *state,
                                     llam_task_t *claimed_task,
                                     uint64_t claimed_run,
                                     uint64_t claimed_incarnation) {
    llam_windows_async_probe_t *probe = &state->probe;
    llam_worker_t *worker = probe->worker;
    unsigned mutations_before = atomic_load_explicit(
        &state->context_mutations, memory_order_relaxed);
    DWORD previous = SuspendThread(state->target);
    uintptr_t gate_pc;
    bool accepted;

    if (previous != 0U) {
        if (previous != (DWORD)-1) {
            (void)ResumeThread(state->target);
        }
        return false;
    }
    if (!GetThreadContext(state->target, state->redirect.context)) {
        (void)ResumeThread(state->target);
        return false;
    }
    gate_pc = (uintptr_t)state->redirect.context->Rip;
    accepted = gate_pc >= (uintptr_t)llam_windows_async_gate_spin_begin &&
               gate_pc < (uintptr_t)llam_windows_async_gate_spin_end &&
               atomic_load_explicit(&probe->phase, memory_order_acquire) ==
                   LLAM_WINDOWS_ASYNC_PROBE_GATE &&
               claimed_task == probe->task &&
               claimed_run == probe->restore_run_generation &&
               claimed_incarnation == atomic_load_explicit(
                   &worker->binding_incarnation, memory_order_acquire);
    if (ResumeThread(state->target) != 1U || accepted ||
        atomic_load_explicit(&state->context_mutations,
                             memory_order_relaxed) != mutations_before) {
        return false;
    }
    atomic_fetch_add_explicit(&state->stale_rejections, 1U,
                              memory_order_relaxed);
    return true;
}

static void oracle_task(void *opaque) {
    oracle_state_t *state = (oracle_state_t *)opaque;

    errno = 123;
    SetLastError(456U);
    atomic_store_explicit(&g_leaf_entered, 1U, memory_order_release);
    win_llam_async_leaf();
    if (errno != 123 || GetLastError() != 456U) {
        atomic_store_explicit(&g_task_error, 1U, memory_order_release);
    }
    if (atomic_load_explicit(&g_companion_progress,
                             memory_order_acquire) == 0U) {
        atomic_store_explicit(&g_task_error, 2U, memory_order_release);
    }
#if defined(LLAM_WINDOWS_ASYNC_LIFETIME_ORACLE)
    if (state->probe.native_oracle_result != 42) {
        atomic_store_explicit(&g_task_error, 3U, memory_order_release);
    }
    atomic_store_explicit(&g_task_finished, 1U, memory_order_release);
    return;
#endif
    llam_yield();
    while (atomic_load_explicit(&g_second_armed,
                                memory_order_acquire) == 0U) {
        YieldProcessor();
    }
    errno = 321;
    SetLastError(654U);
    atomic_store_explicit(&g_leaf_entered, 1U, memory_order_release);
    win_llam_async_leaf();
    if (errno != 321 || GetLastError() != 654U) {
        atomic_store_explicit(&g_task_error, 4U, memory_order_release);
    }
    if (state->probe.native_oracle_result != 42) {
        atomic_store_explicit(&g_task_error, 3U, memory_order_release);
    }
    atomic_store_explicit(&g_task_finished, 1U, memory_order_release);
}

static void oracle_companion(void *opaque) {
#if defined(LLAM_WINDOWS_ASYNC_LIFETIME_ORACLE)
    oracle_state_t *state = (oracle_state_t *)opaque;

    atomic_fetch_add_explicit(&g_companion_progress, 1U,
                              memory_order_release);
    if (state->peer_mode) {
        while (atomic_load_explicit(&state->deferred_seen,
                                    memory_order_acquire) == 0U) {
            (void)llam_sleep_ns(UINT64_C(1000000));
        }
        atomic_store_explicit(&state->companion_after_deferred, 1U,
                              memory_order_release);
    }
#else
    (void)opaque;
    atomic_fetch_add_explicit(&g_companion_progress, 1U,
                              memory_order_release);
#endif
}

#if defined(LLAM_WINDOWS_ASYNC_LIFETIME_ORACLE)
static void oracle_rebind_task(void *opaque) {
    (void)opaque;
    atomic_fetch_add_explicit(&g_rebind_progress, 1U,
                              memory_order_release);
}

static DWORD WINAPI oracle_rebind_run(void *opaque) {
    (void)opaque;
    return llam_run() == 0 ? 0U : 1U;
}
#endif

static bool oracle_wait_phase(oracle_state_t *state, unsigned expected,
                              DWORD timeout_ms) {
    ULONGLONG deadline = GetTickCount64() + timeout_ms;

#if defined(LLAM_WINDOWS_ASYNC_LIFETIME_ORACLE)
    if (expected == LLAM_WINDOWS_ASYNC_PROBE_GATE) {
        while (atomic_load_explicit(&state->operation.bank->phase,
                                    memory_order_acquire) !=
                   LLAM_WINDOWS_ASYNC_BANK_GATE &&
               GetTickCount64() < deadline) {
            Sleep(1U);
        }
        if (atomic_load_explicit(&state->operation.bank->phase,
                                 memory_order_acquire) !=
            LLAM_WINDOWS_ASYNC_BANK_GATE) {
            return false;
        }
        state->probe.restore_run_generation = atomic_load_explicit(
            &state->probe.worker->run_generation, memory_order_acquire);
        atomic_store_explicit(&state->probe.phase,
                              LLAM_WINDOWS_ASYNC_PROBE_GATE,
                              memory_order_release);
        return true;
    }
#endif
    while (atomic_load_explicit(&state->probe.phase,
                                memory_order_acquire) != expected) {
        if (atomic_load_explicit(&state->probe.phase,
                                 memory_order_acquire) ==
                LLAM_WINDOWS_ASYNC_PROBE_FAILED ||
            GetTickCount64() >= deadline) {
            return false;
        }
        Sleep(1U);
    }
    return true;
}

static DWORD WINAPI oracle_controller(void *opaque) {
    oracle_state_t *state = (oracle_state_t *)opaque;
    llam_windows_async_probe_t *probe = &state->probe;
    llam_shard_t *shard = &state->probe.runtime->shards[0];
    llam_worker_t *worker = &shard->primary_worker;
    ULONGLONG deadline = GetTickCount64() + 4000U;
    DWORD64 valid_features = 0U;
    bool captured = false;
    unsigned stage = 0U;
    uintptr_t last_pc = 0U;
    DWORD failure_error = 0U;
    DWORD suspension_before = (DWORD)-1;
    uint64_t first_capture_generation = 0U;
    unsigned cycle;
#if defined(LLAM_WINDOWS_ASYNC_LIFETIME_ORACLE)
    const unsigned cycle_count = 1U;
#else
    const unsigned cycle_count = 2U;
#endif

    for (cycle = 0U; cycle < cycle_count; ++cycle) {
    deadline = GetTickCount64() + 4000U;
    captured = false;
    stage = 0U;
    last_pc = 0U;
    failure_error = 0U;
    suspension_before = (DWORD)-1;
#if defined(LLAM_WINDOWS_ASYNC_LIFETIME_ORACLE)
    if (cycle == 0U) {
        unsigned rejected = 0U;

        while (rejected < 4U && GetTickCount64() < deadline) {
            DWORD previous;

            if (atomic_load_explicit(&g_leaf_entered,
                                     memory_order_acquire) == 0U ||
                atomic_load_explicit(&worker->active_g,
                                     memory_order_acquire) != probe->task ||
                !llam_windows_async_lifetime_op_begin(state->probe.runtime,
                                                      &state->operation)) {
                Sleep(1U);
                continue;
            }
            state->target = state->operation.native_thread;
            previous = SuspendThread(state->target);
            if (previous != 0U ||
                !GetThreadContext(state->target,
                                  state->operation.bank->captured.context)) {
                if (previous != (DWORD)-1) {
                    (void)ResumeThread(state->target);
                }
                llam_windows_async_lifetime_op_abort(&state->operation);
                goto failed;
            }
            /* A policy refusal before mutation must return this exact bank. */
            if (ResumeThread(state->target) != 1U) {
                goto failed;
            }
            llam_windows_async_lifetime_op_abort(&state->operation);
            ++rejected;
        }
        if (rejected != 4U) {
            goto failed;
        }
        atomic_store_explicit(&state->premutation_refusals,
                              rejected, memory_order_release);
    }
#endif
    while (GetTickCount64() < deadline) {
        DWORD previous;
        uintptr_t pc;
        uintptr_t rsp;

        if (atomic_load_explicit(&g_leaf_entered, memory_order_acquire) == 0U ||
            atomic_load_explicit(&worker->active_g,
                                 memory_order_acquire) != probe->task) {
            Sleep(1U);
            continue;
        }
#if defined(LLAM_WINDOWS_ASYNC_LIFETIME_ORACLE)
        if (!llam_windows_async_lifetime_op_begin(state->probe.runtime,
                                                 &state->operation)) {
            Sleep(1U);
            continue;
        }
        if (state->operation.task != probe->task) {
            llam_windows_async_lifetime_op_abort(&state->operation);
            continue;
        }
        state->target = state->operation.native_thread;
        state->captured.buffer = state->operation.bank->captured.storage;
        state->captured.length = state->operation.bank->captured.length;
        state->captured.context = state->operation.bank->captured.context;
        state->redirect.buffer = state->operation.bank->redirected.storage;
        state->redirect.length = state->operation.bank->redirected.length;
        state->redirect.context = state->operation.bank->redirected.context;
#endif
        stage = 10U;
        previous = SuspendThread(state->target);
        suspension_before = previous;
        if (previous == (DWORD)-1) {
            failure_error = GetLastError();
#if defined(LLAM_WINDOWS_ASYNC_LIFETIME_ORACLE)
            llam_windows_async_lifetime_op_abort(&state->operation);
#endif
            goto failed;
        }
        if (previous != 0U) {
            failure_error = ERROR_BUSY;
            (void)ResumeThread(state->target);
#if defined(LLAM_WINDOWS_ASYNC_LIFETIME_ORACLE)
            llam_windows_async_lifetime_op_abort(&state->operation);
#endif
            goto failed;
        }
        stage = 11U;
        if (!GetThreadContext(state->target, state->captured.context)) {
            failure_error = GetLastError();
            (void)ResumeThread(state->target);
#if defined(LLAM_WINDOWS_ASYNC_LIFETIME_ORACLE)
            llam_windows_async_lifetime_op_abort(&state->operation);
#endif
            goto failed;
        }
        stage = 12U;
        pc = (uintptr_t)state->captured.context->Rip;
        last_pc = pc;
        rsp = (uintptr_t)state->captured.context->Rsp;
        if (pc < (uintptr_t)win_llam_async_leaf_begin ||
            pc >= (uintptr_t)win_llam_async_leaf_end) {
            if (ResumeThread(state->target) != 1U) {
                goto failed;
            }
#if defined(LLAM_WINDOWS_ASYNC_LIFETIME_ORACLE)
            llam_windows_async_lifetime_op_abort(&state->operation);
#endif
            Sleep(1U);
            continue;
        }
        {
            BOOL xmask_ok = GetXStateFeaturesMask(state->captured.context,
                                                   &valid_features);
            BOOL copy_ok = CopyContext(state->redirect.context,
                                       state->captured.context->ContextFlags,
                                       state->captured.context);

        if (atomic_load_explicit(&worker->active_g,
                                 memory_order_acquire) != probe->task ||
            atomic_load_explicit(&worker->execution_state,
                                 memory_order_acquire) !=
                LLAM_WORKER_EXEC_MANAGED_SAFE ||
            rsp < (uintptr_t)probe->task->stack_base ||
            rsp >= (uintptr_t)probe->task->stack_base +
                       probe->task->stack_size ||
            shard->scheduler_ctx.rsp < state->native_stack_low ||
            shard->scheduler_ctx.rsp >= state->native_stack_high ||
            shard->scheduler_ctx.simd_flags != 0U ||
            !xmask_ok ||
            (valid_features & ~state->features) != 0U ||
            !copy_ok) {
            fprintf(stderr, "llam-async-oracle: admission rsp=%p G=[%p,%p) scheduler_rsp=%p M=[%p,%p) simd_flags=%u xmask_ok=%d valid=0x%llx expected=0x%llx copy_ok=%d last_error=%lu\n",
                    (void *)rsp, probe->task->stack_base,
                    (void *)((uintptr_t)probe->task->stack_base +
                             probe->task->stack_size),
                    (void *)(uintptr_t)shard->scheduler_ctx.rsp,
                    (void *)(uintptr_t)state->native_stack_low,
                    (void *)(uintptr_t)state->native_stack_high,
                    shard->scheduler_ctx.simd_flags, (int)xmask_ok,
                    (unsigned long long)valid_features,
                    (unsigned long long)state->features, (int)copy_ok,
                    (unsigned long)GetLastError());
            fflush(stderr);
            (void)ResumeThread(state->target);
#if defined(LLAM_WINDOWS_ASYNC_LIFETIME_ORACLE)
            llam_windows_async_lifetime_op_abort(&state->operation);
#endif
            goto failed;
        }
        }
        state->original_g_ctx = probe->task->ctx;
        stage = 1U;
        probe->worker = worker;
        probe->captured_rsp = rsp;
        probe->capture_run_generation = atomic_load_explicit(
            &worker->run_generation, memory_order_acquire);
        if (cycle == 0U) {
            first_capture_generation = probe->capture_run_generation;
        } else if (probe->capture_run_generation <= first_capture_generation ||
                   atomic_load_explicit(&shard->metrics.yields,
                                        memory_order_relaxed) == 0U) {
            (void)ResumeThread(state->target);
            goto failed;
        }
        state->redirect.context->Rip =
            (DWORD64)(uintptr_t)llam_windows_async_restore_scheduler;
        state->redirect.context->Rcx =
            (DWORD64)(uintptr_t)&shard->scheduler_ctx;
        state->redirect.context->EFlags &= ~0x400U;
#if defined(LLAM_WINDOWS_ASYNC_LIFETIME_ORACLE)
        if (!llam_windows_async_lifetime_op_capture_committed(
                &state->operation)) {
            (void)ResumeThread(state->target);
            llam_windows_async_lifetime_op_abort(&state->operation);
            goto failed;
        }
#endif
        atomic_store_explicit(&probe->phase,
                              LLAM_WINDOWS_ASYNC_PROBE_CAPTURED,
                              memory_order_release);
        if (!oracle_set_context(state, state->redirect.context)) {
            (void)ResumeThread(state->target);
            goto failed;
        }
        if (ResumeThread(state->target) != 1U) {
            goto failed;
        }
#if defined(LLAM_WINDOWS_ASYNC_LIFETIME_ORACLE)
        if (state->uncertain_capture_mode) {
            /* Inject lost certainty after the real remote context mutation. */
            llam_windows_async_lifetime_op_finish(&state->operation);
            oracle_fail("capture fail-fast returned", ERROR_INVALID_DATA);
        }
#endif
        captured = true;
        fprintf(stderr, "llam-async-oracle: cycle=%u capture redirected pc=%p generation=%llu\n",
                cycle, (void *)pc,
                (unsigned long long)probe->capture_run_generation);
        fflush(stderr);
        break;
    }
    if (!captured || !oracle_wait_phase(state, LLAM_WINDOWS_ASYNC_PROBE_GATE,
                                        4000U)) {
        goto failed;
    }
    stage = 2U;
    fprintf(stderr, "llam-async-oracle: cycle=%u native arrival/gate reached generation=%llu oracle=%d\n",
            cycle, (unsigned long long)probe->restore_run_generation,
            probe->native_oracle_result);
    fflush(stderr);
    if (probe->arrival_run_generation != probe->capture_run_generation ||
        probe->restore_run_generation <= probe->capture_run_generation ||
        probe->native_oracle_result != 42 ||
        memcmp(&state->original_g_ctx, &probe->task->ctx,
               sizeof(state->original_g_ctx)) != 0) {
        goto failed;
    }
    Sleep(1U); /* Let the gate settle on its bounded assembly spin labels. */
    if (cycle == 0U) {
        uint64_t incarnation = atomic_load_explicit(
            &worker->binding_incarnation, memory_order_acquire);

        if (!oracle_reject_stale_gate(state, NULL,
                                      probe->restore_run_generation,
                                      incarnation) ||
            !oracle_reject_stale_gate(state, probe->task,
                                      probe->restore_run_generation - 1U,
                                      incarnation) ||
            !oracle_reject_stale_gate(state, probe->task,
                                      probe->restore_run_generation,
                                      incarnation - 1U)) {
            goto failed;
        }
    }
    {
        DWORD previous = SuspendThread(state->target);
        PCONTEXT gate = state->redirect.context;
        uintptr_t gate_pc;

        if (previous != 0U || !GetThreadContext(state->target, gate)) {
            if (previous != (DWORD)-1) {
                (void)ResumeThread(state->target);
            }
            goto failed;
        }
        gate_pc = (uintptr_t)gate->Rip;
        last_pc = gate_pc;
        if (gate_pc < (uintptr_t)llam_windows_async_gate_spin_begin ||
            gate_pc >= (uintptr_t)llam_windows_async_gate_spin_end ||
#if defined(LLAM_WINDOWS_ASYNC_LIFETIME_ORACLE)
            llam_windows_async_lifetime_pc_allowed(&state->operation, gate_pc) ||
#endif
            atomic_load_explicit(&worker->active_g,
                                 memory_order_acquire) != probe->task ||
            atomic_load_explicit(&worker->run_generation,
                                 memory_order_acquire) !=
                probe->restore_run_generation) {
            (void)ResumeThread(state->target);
            goto failed;
        }
#if defined(LLAM_WINDOWS_ASYNC_LIFETIME_ORACLE)
        if (!llam_windows_async_lifetime_op_restoring(
                &state->operation,
                atomic_load_explicit(&probe->task->run_generation,
                                     memory_order_acquire),
                atomic_load_explicit(&worker->run_generation,
                                     memory_order_acquire))) {
            (void)ResumeThread(state->target);
            goto failed;
        }
#endif
        atomic_store_explicit(&g_win_llam_async_release, 1U,
                              memory_order_release);
        atomic_store_explicit(&probe->phase,
                              LLAM_WINDOWS_ASYNC_PROBE_RESTORED,
                              memory_order_release);
        if (!oracle_set_context(state, state->captured.context)) {
            (void)ResumeThread(state->target);
            goto failed;
        }
#if defined(LLAM_WINDOWS_ASYNC_LIFETIME_ORACLE)
        if (state->uncertain_restore_mode) {
            /* Gate context was mutated; target remains suspended here. */
            llam_windows_async_lifetime_op_finish(&state->operation);
            oracle_fail("restore fail-fast returned", ERROR_INVALID_DATA);
        }
#endif
        if (ResumeThread(state->target) != 1U) {
            goto failed;
        }
    }
    stage = 3U;
    fprintf(stderr, "llam-async-oracle: cycle=%u remote G restoration issued\n",
            cycle);
    fflush(stderr);
    if (!oracle_wait_phase(state, LLAM_WINDOWS_ASYNC_PROBE_FINISHED,
                            4000U)) {
        goto failed;
    }
#if defined(LLAM_WINDOWS_ASYNC_LIFETIME_ORACLE)
    {
        ULONGLONG retire_deadline = GetTickCount64() + 4000U;

        while ((atomic_load_explicit(&shard->windows_async_retire_count,
                                     memory_order_acquire) == 0U ||
                atomic_load_explicit(&state->operation.bank->phase,
                                     memory_order_acquire) !=
                    LLAM_WINDOWS_ASYNC_BANK_OBSERVED ||
                (!state->peer_mode &&
                 llam_runtime_has_live_tasks(state->probe.runtime))) &&
               GetTickCount64() < retire_deadline) {
            Sleep(1U);
        }
        if (atomic_load_explicit(&shard->windows_async_retire_count,
                                 memory_order_acquire) == 0U ||
            atomic_load_explicit(&state->operation.bank->phase,
                                 memory_order_acquire) !=
                LLAM_WINDOWS_ASYNC_BANK_OBSERVED ||
            (!state->peer_mode &&
             llam_runtime_has_live_tasks(state->probe.runtime)) ||
            atomic_load_explicit(&probe->task->reclaim_ready,
                                 memory_order_acquire) != 0U ||
            probe->task->stack_mapping == NULL ||
            llam_windows_async_lifetime_charged_bytes(state->probe.runtime) ==
                0U ||
            atomic_load_explicit(&state->run_completed,
                                 memory_order_acquire) != 0U) {
            goto failed;
        }
        atomic_store_explicit(&state->deferred_seen, 1U,
                              memory_order_release);
        fprintf(stderr, "llam-async-lifetime: deferred bank/stack retained; live=%u debt=%u\n",
                llam_runtime_live_tasks(state->probe.runtime),
                atomic_load_explicit(&shard->windows_async_execution_debt,
                                     memory_order_acquire));
        fflush(stderr);
        if (state->peer_mode) {
            ULONGLONG peer_deadline = GetTickCount64() + 4000U;

            while (atomic_load_explicit(&state->companion_after_deferred,
                                        memory_order_acquire) == 0U &&
                   GetTickCount64() < peer_deadline) {
                Sleep(1U);
            }
            if (atomic_load_explicit(&state->companion_after_deferred,
                                     memory_order_acquire) == 0U) {
                goto failed;
            }
        }
        if (state->stop_deferred_mode) {
            llam_request_stop(state->probe.runtime);
        }
        Sleep(10U);
        if (atomic_load_explicit(&state->run_completed,
                                 memory_order_acquire) != 0U ||
            (uintptr_t)state->captured.context->Rip <
                (uintptr_t)win_llam_async_leaf_begin ||
            (uintptr_t)state->captured.context->Rip >=
                (uintptr_t)win_llam_async_leaf_end) {
            goto failed;
        }
        if (state->task_scan_mode &&
            !llam_task_scan_ref_try_acquire(state->probe.runtime,
                                            probe->task)) {
            goto failed;
        }
        llam_windows_async_lifetime_op_finish(&state->operation);
        if (state->task_scan_mode) {
            ULONGLONG scan_deadline = GetTickCount64() + 4000U;

            while ((atomic_load_explicit(&probe->task->reclaim_ready,
                                         memory_order_acquire) == 0U ||
                    atomic_load_explicit(&shard->windows_async_finalizer_count,
                                         memory_order_acquire) == 0U) &&
                   GetTickCount64() < scan_deadline) {
                Sleep(1U);
            }
            if (atomic_load_explicit(&probe->task->reclaim_ready,
                                     memory_order_acquire) == 0U ||
                atomic_load_explicit(&shard->windows_async_finalizer_count,
                                     memory_order_acquire) == 0U ||
                probe->task->stack_mapping != NULL ||
                llam_windows_async_lifetime_charged_bytes(
                    state->probe.runtime) != 0U ||
                atomic_load_explicit(&state->run_completed,
                                     memory_order_acquire) != 0U) {
                goto failed;
            }
            if (!llam_task_scan_ref_release(state->probe.runtime,
                                             probe->task)) {
                goto failed;
            }
        }
    }
#else
    if (cycle == 0U) {
        atomic_store_explicit(&g_win_llam_async_release, 0U,
                              memory_order_release);
        atomic_store_explicit(&g_leaf_entered, 0U, memory_order_release);
        probe->native_oracle_result = 0;
        if (oracle_context_reset(&state->captured, state->features) != 0 ||
            oracle_context_reset(&state->redirect, state->features) != 0) {
            goto failed;
        }
        atomic_store_explicit(&probe->phase,
                              LLAM_WINDOWS_ASYNC_PROBE_IDLE,
                              memory_order_release);
        atomic_store_explicit(&g_second_armed, 1U, memory_order_release);
    }
#endif
    }
    atomic_store_explicit(&state->controller_result, 1U,
                          memory_order_release);
    return 0U;

failed:
    fprintf(stderr, "llam-async-oracle: controller failed stage=%u phase=%u pc=%p error=%lu suspend_before=%lu leaf=%u active=%p task=%p worker_state=%u companion=%u\n",
            stage, atomic_load_explicit(&probe->phase, memory_order_acquire),
            (void *)last_pc, (unsigned long)failure_error,
            (unsigned long)suspension_before,
            atomic_load_explicit(&g_leaf_entered, memory_order_acquire),
            (void *)atomic_load_explicit(&worker->active_g,
                                         memory_order_acquire),
            (void *)probe->task,
            atomic_load_explicit(&worker->execution_state,
                                 memory_order_acquire),
            atomic_load_explicit(&g_companion_progress,
                                 memory_order_acquire));
    fflush(stderr);
    atomic_store_explicit(&state->controller_result, 2U,
                          memory_order_release);
    return 1U;
}

int main(void) {
    PROCESS_MITIGATION_USER_SHADOW_STACK_POLICY policy;
    llam_runtime_opts_t options;
    oracle_state_t state;
    llam_task_t *task;
    llam_task_t *companion;
    HANDLE controller;
    DWORD64 features;
    int run_result;
#if defined(LLAM_WINDOWS_ASYNC_LIFETIME_ORACLE)
    HANDLE rebind_runner;
    llam_task_t *rebind_task_handle;
    DWORD rebind_exit = 1U;
    uint64_t binding_before;
#endif

    SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX);
    memset(&policy, 0, sizeof(policy));
    if (!GetProcessMitigationPolicy(GetCurrentProcess(),
                                    ProcessUserShadowStackPolicy, &policy,
                                    sizeof(policy))) {
        puts("llam-async-oracle: mitigation policy unavailable; skip");
        return 77;
    }
    features = GetEnabledXStateFeatures();
    if (policy.EnableUserShadowStack || policy.AuditUserShadowStack ||
        policy.EnableUserShadowStackStrictMode ||
        policy.SetContextIpValidation || policy.AuditSetContextIpValidation ||
        policy.SetContextIpValidationRelaxedMode ||
        features == 0U || (features & ~ORACLE_SUPPORTED_XSTATE) != 0U) {
        puts("llam-async-oracle: unqualified mitigation/XSTATE; skip");
        return 77;
    }
    memset(&state, 0, sizeof(state));
    state.features = features;
    state.probe.native_oracle = win_llam_async_native_oracle;
    atomic_init(&state.probe.phase, LLAM_WINDOWS_ASYNC_PROBE_IDLE);
    atomic_init(&state.probe.reclaim_wait_seen, 0U);
    atomic_init(&state.controller_result, 0U);
    atomic_init(&state.context_mutations, 0U);
    atomic_init(&state.stale_rejections, 0U);
#if defined(LLAM_WINDOWS_ASYNC_LIFETIME_ORACLE)
    char lifetime_mode[24] = {0};
    char uncertain_mode[24] = {0};
    DWORD mode_length;

    atomic_init(&state.run_completed, 0U);
    atomic_init(&state.deferred_seen, 0U);
    atomic_init(&state.companion_after_deferred, 0U);
    atomic_init(&state.premutation_refusals, 0U);
    mode_length = GetEnvironmentVariableA("LLAM_LIFETIME_MODE",
                                          lifetime_mode,
                                          sizeof(lifetime_mode));
    if (mode_length >= sizeof(lifetime_mode)) {
        oracle_fail("lifetime mode length", ERROR_INVALID_PARAMETER);
    }
    state.peer_mode = strcmp(lifetime_mode, "peer") == 0;
    state.task_scan_mode = strcmp(lifetime_mode, "taskscan") == 0;
    state.stop_deferred_mode = strcmp(lifetime_mode, "stopdeferred") == 0;
    if (lifetime_mode[0] != '\0' && !state.peer_mode &&
        !state.task_scan_mode && !state.stop_deferred_mode) {
        oracle_fail("lifetime mode", ERROR_INVALID_PARAMETER);
    }
    mode_length = GetEnvironmentVariableA("LLAM_LIFETIME_UNCERTAIN_MODE",
                                          uncertain_mode,
                                          sizeof(uncertain_mode));
    if (mode_length >= sizeof(uncertain_mode)) {
        oracle_fail("uncertain mode length", ERROR_INVALID_PARAMETER);
    }
    state.uncertain_capture_mode = strcmp(uncertain_mode, "capture") == 0;
    state.uncertain_restore_mode = strcmp(uncertain_mode, "restore") == 0;
    if (uncertain_mode[0] != '\0' && !state.uncertain_capture_mode &&
        !state.uncertain_restore_mode) {
        oracle_fail("uncertain mode", ERROR_INVALID_PARAMETER);
    }
    if (llam_runtime_opts_init(&options, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
#else
    if (oracle_context_init(&state.captured, features) != 0 ||
        oracle_context_init(&state.redirect, features) != 0 ||
        llam_runtime_opts_init(&options, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
#endif
        oracle_fail("CONTEXT/options init", GetLastError());
    }
    options.deterministic = 1U;
    options.preempt_mode = LLAM_PREEMPT_OFF;
    if (llam_runtime_init_compat_ex(&options, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        oracle_fail("runtime init", (unsigned)errno);
    }
    state.probe.runtime = llam_runtime_default_storage();
    if (state.probe.runtime == NULL ||
        state.probe.runtime->active_shards != 1U ||
        win_llam_async_native_oracle() != 42) {
        oracle_fail("native before-run oracle", ERROR_INVALID_DATA);
    }
#if defined(LLAM_WINDOWS_ASYNC_LIFETIME_ORACLE)
    if (llam_windows_async_lifetime_register_safe_range(
            state.probe.runtime,
            (uintptr_t)win_llam_async_leaf_begin,
            (uintptr_t)win_llam_async_leaf_end) != 0 ||
        llam_windows_async_lifetime_enable(state.probe.runtime) != 0) {
        oracle_fail("lifetime enable", (unsigned)errno);
    }
#endif
    task = llam_spawn(oracle_task, &state, NULL);
    companion = llam_spawn(oracle_companion, &state, NULL);
    if (task == NULL || companion == NULL) {
        oracle_fail("spawn", (unsigned)errno);
    }
    /* Spawn returns an encoded public handle, never dereference it as storage. */
    state.probe.task = llam_task_resolve_public_handle(task);
    if (state.probe.task == NULL) {
        oracle_fail("resolve task handle", (unsigned)errno);
    }
    llam_task_end_public_op(state.probe.task);
#if defined(LLAM_WINDOWS_ASYNC_LIFETIME_ORACLE)
    if (llam_detach(task) != 0) {
        oracle_fail("detach", (unsigned)errno);
    }
#endif
    GetCurrentThreadStackLimits(&state.native_stack_low,
                                &state.native_stack_high);
#if !defined(LLAM_WINDOWS_ASYNC_LIFETIME_ORACLE)
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                         GetCurrentProcess(), &state.target,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                             THREAD_SET_CONTEXT | SYNCHRONIZE,
                         FALSE, 0U)) {
        oracle_fail("DuplicateHandle", GetLastError());
    }
#endif
    llam_windows_async_probe_install(&state.probe);
    controller = CreateThread(NULL, 0U, oracle_controller, &state, 0U, NULL);
    if (controller == NULL) {
        oracle_fail("CreateThread", GetLastError());
    }
    run_result = llam_run();
#if defined(LLAM_WINDOWS_ASYNC_LIFETIME_ORACLE)
    atomic_store_explicit(&state.run_completed, 1U, memory_order_release);
#endif
    if (WaitForSingleObject(controller, 5000U) != WAIT_OBJECT_0 ||
        atomic_load_explicit(&state.controller_result,
                             memory_order_acquire) != 1U ||
        run_result != 0 ||
#if defined(LLAM_WINDOWS_ASYNC_LIFETIME_ORACLE)
        atomic_load_explicit(&state.deferred_seen,
                             memory_order_acquire) != 1U ||
        atomic_load_explicit(&state.premutation_refusals,
                             memory_order_acquire) != 4U ||
        llam_windows_async_lifetime_charged_bytes(state.probe.runtime) != 0U ||
#else
        llam_join(task) != 0 ||
#endif
        llam_join(companion) != 0 ||
        atomic_load_explicit(&g_task_finished,
                             memory_order_acquire) != 1U ||
        atomic_load_explicit(&g_task_error, memory_order_acquire) != 0U ||
        atomic_load_explicit(&state.context_mutations,
                             memory_order_acquire) !=
#if defined(LLAM_WINDOWS_ASYNC_LIFETIME_ORACLE)
            2U ||
#else
            4U ||
#endif
        atomic_load_explicit(&state.stale_rejections,
                             memory_order_acquire) != 3U ||
        win_llam_async_native_oracle() != 42 ||
        g_win_llam_async_cleanup_count !=
#if defined(LLAM_WINDOWS_ASYNC_LIFETIME_ORACLE)
            3L) {
#else
            4L) {
#endif
        oracle_fail("real scheduler oracle", ERROR_INVALID_DATA);
    }
    llam_windows_async_probe_install(NULL);
#if defined(LLAM_WINDOWS_ASYNC_LIFETIME_ORACLE)
    binding_before = atomic_load_explicit(
        &state.probe.runtime->shards[0].primary_worker.incarnation,
        memory_order_acquire);
    rebind_task_handle = llam_spawn(oracle_rebind_task, NULL, NULL);
    rebind_runner = rebind_task_handle != NULL
                        ? CreateThread(NULL, 0U, oracle_rebind_run,
                                       NULL, 0U, NULL)
                        : NULL;
    if (rebind_runner == NULL ||
        WaitForSingleObject(rebind_runner, 5000U) != WAIT_OBJECT_0 ||
        !GetExitCodeThread(rebind_runner, &rebind_exit) ||
        rebind_exit != 0U || llam_join(rebind_task_handle) != 0 ||
        atomic_load_explicit(&g_rebind_progress,
                             memory_order_acquire) != 1U ||
        atomic_load_explicit(
            &state.probe.runtime->shards[0].primary_worker.incarnation,
            memory_order_acquire) <= binding_before ||
        llam_windows_async_lifetime_charged_bytes(
            state.probe.runtime) != 0U) {
        oracle_fail("new-caller binding lifetime", ERROR_INVALID_DATA);
    }
    CloseHandle(rebind_runner);
#endif
    llam_runtime_shutdown();
    CloseHandle(controller);
#if !defined(LLAM_WINDOWS_ASYNC_LIFETIME_ORACLE)
    CloseHandle(state.target);
    free(state.captured.buffer);
    free(state.redirect.buffer);
#endif
    puts("llam-async-oracle: native scheduler capture/restore passed");
    return 0;
}
