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

/* Two concurrently captured Gs through the actual LLAM one-M dispatcher. */
#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

#include "runtime_internal.h"
#include "windows_async_probe.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAIR_CONTEXT_FLAGS (CONTEXT_ALL | CONTEXT_XSTATE)
#define PAIR_SUPPORTED_XSTATE UINT64_C(0x807)

extern void win_llam_async_pair_leaf(void *record);
extern const unsigned char win_llam_async_pair_leaf_begin[];
extern const unsigned char win_llam_async_pair_leaf_end[];

typedef struct pair_leaf_state {
    atomic_uint release;
    uint32_t pad0;
    uint64_t tag;
    uint64_t observed_rax;
    uint64_t observed_rbx;
    uint64_t observed_ymm0[4];
    uint64_t observed_ymm6[4];
    uint32_t observed_mxcsr;
    uint16_t observed_cw;
    uint16_t saved_cw;
    uint32_t saved_mxcsr;
    uint32_t desired_mxcsr;
    uint16_t desired_cw;
} pair_leaf_state_t;

_Static_assert(offsetof(pair_leaf_state_t, tag) == 8U, "leaf tag offset");
_Static_assert(offsetof(pair_leaf_state_t, observed_rax) == 16U,
               "leaf RAX offset");
_Static_assert(offsetof(pair_leaf_state_t, observed_rbx) == 24U,
               "leaf RBX offset");
_Static_assert(offsetof(pair_leaf_state_t, observed_ymm0) == 32U,
               "leaf YMM0 offset");
_Static_assert(offsetof(pair_leaf_state_t, observed_ymm6) == 64U,
               "leaf YMM6 offset");
_Static_assert(offsetof(pair_leaf_state_t, observed_mxcsr) == 96U,
               "leaf MXCSR offset");
_Static_assert(offsetof(pair_leaf_state_t, observed_cw) == 100U,
               "leaf CW offset");
_Static_assert(offsetof(pair_leaf_state_t, saved_cw) == 102U,
               "leaf saved CW offset");
_Static_assert(offsetof(pair_leaf_state_t, saved_mxcsr) == 104U,
               "leaf saved MXCSR offset");
_Static_assert(offsetof(pair_leaf_state_t, desired_mxcsr) == 108U,
               "leaf desired MXCSR offset");
_Static_assert(offsetof(pair_leaf_state_t, desired_cw) == 112U,
               "leaf desired CW offset");

typedef struct pair_context {
    void *buffer;
    DWORD length;
    PCONTEXT context;
} pair_context_t;

struct pair_state;
typedef struct pair_task {
    struct pair_state *owner;
    unsigned index;
    _Alignas(16) pair_leaf_state_t leaf;
    llam_windows_async_probe_t probe;
    llam_task_t *public_handle;
    pair_context_t captured;
    pair_context_t redirected;
    llam_ctx_t original_g_ctx;
    atomic_uint entered;
    atomic_uint finished;
    atomic_uint error;
    bool controller_pin_held;
} pair_task_t;

typedef struct pair_state {
    llam_runtime_t *runtime;
    HANDLE target;
    ULONG_PTR native_low;
    ULONG_PTR native_high;
    DWORD64 features;
    uint32_t scheduler_mxcsr;
    uint16_t scheduler_cw;
    pair_task_t tasks[3];
    atomic_uint controller_result;
    atomic_uint mutations;
    char stop_phase[20];
    int second_run_result;
} pair_state_t;

static pair_state_t *g_pair_oracle;

static void pair_fail(const char *where, unsigned code) {
    fprintf(stderr, "llam-async-pair: %s failed: %u\n", where, code);
    ExitProcess(1U);
}

static uint32_t pair_read_mxcsr(void) {
    uint32_t value;

    __asm__ volatile("stmxcsr %0" : "=m"(value));
    return value;
}

static uint16_t pair_read_cw(void) {
    uint16_t value;

    __asm__ volatile("fnstcw %0" : "=m"(value));
    return value;
}

static int pair_native_oracle(void) {
    ULONG_PTR low = 0U;
    ULONG_PTR high = 0U;
    volatile uint32_t marker = UINT32_C(0x6c6c616d);
    pair_state_t *state = g_pair_oracle;

    GetCurrentThreadStackLimits(&low, &high);
    if (state == NULL || (uintptr_t)&marker < low ||
        (uintptr_t)&marker >= high ||
        pair_read_mxcsr() != state->scheduler_mxcsr ||
        pair_read_cw() != state->scheduler_cw) {
        return -1;
    }
    return 42;
}

static int pair_context_init(pair_context_t *storage, DWORD64 features) {
    DWORD length = 0U;
    PCONTEXT context = NULL;

    memset(storage, 0, sizeof(*storage));
    SetLastError(ERROR_SUCCESS);
    if (InitializeContext(NULL, PAIR_CONTEXT_FLAGS, &context, &length) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
        length < sizeof(CONTEXT)) {
        return -1;
    }
    storage->buffer = calloc(1U, length);
    if (storage->buffer == NULL) {
        return -1;
    }
    storage->length = length;
    if (!InitializeContext(storage->buffer, PAIR_CONTEXT_FLAGS,
                           &storage->context, &length) ||
        storage->context == NULL || length > storage->length ||
        !SetXStateFeaturesMask(storage->context, features)) {
        return -1;
    }
    return 0;
}

static bool pair_wait_phase(pair_task_t *task, unsigned expected,
                            DWORD timeout_ms) {
    ULONGLONG deadline = GetTickCount64() + timeout_ms;

    while (atomic_load_explicit(&task->probe.phase,
                                memory_order_acquire) != expected) {
        if (atomic_load_explicit(&task->probe.phase,
                                 memory_order_acquire) ==
                LLAM_WINDOWS_ASYNC_PROBE_FAILED ||
            GetTickCount64() >= deadline) {
            return false;
        }
        Sleep(1U);
    }
    return true;
}

static bool pair_observed_state_ok(const pair_task_t *task) {
    const pair_leaf_state_t *leaf = &task->leaf;
    uint64_t high = ~leaf->tag;
    const uint64_t *vectors[2] = {leaf->observed_ymm0,
                                  leaf->observed_ymm6};
    unsigned vector;

    if (leaf->observed_rax != leaf->tag ||
        leaf->observed_rbx != leaf->tag ||
        leaf->observed_mxcsr != leaf->desired_mxcsr ||
        leaf->observed_cw != leaf->desired_cw) {
        return false;
    }
    for (vector = 0U; vector < 2U; ++vector) {
        if (vectors[vector][0] != leaf->tag ||
            vectors[vector][1] != leaf->tag ||
            vectors[vector][2] != high ||
            vectors[vector][3] != high) {
            return false;
        }
    }
    return true;
}

static void pair_task_entry(void *opaque) {
    pair_task_t *task = (pair_task_t *)opaque;
    int expected_errno = task->index == 0U ? 111 : 222;
    DWORD expected_last_error = task->index == 0U ? 333U : 444U;

    errno = expected_errno;
    SetLastError(expected_last_error);
    atomic_store_explicit(&task->entered, 1U, memory_order_release);
    win_llam_async_pair_leaf(&task->leaf);
    if (errno != expected_errno ||
        GetLastError() != expected_last_error ||
        !pair_observed_state_ok(task)) {
        atomic_store_explicit(&task->error, 1U, memory_order_release);
    }
    atomic_store_explicit(&task->finished, 1U, memory_order_release);
}

static bool pair_capture(pair_state_t *state, pair_task_t *task) {
    llam_shard_t *shard = &state->runtime->shards[0];
    llam_worker_t *worker = &shard->primary_worker;
    ULONGLONG deadline = GetTickCount64() + 4000U;

    while (GetTickCount64() < deadline) {
        DWORD previous;
        uintptr_t pc;
        uintptr_t rsp;
        DWORD64 valid_features = 0U;

        if (atomic_load_explicit(&task->entered, memory_order_acquire) == 0U ||
            atomic_load_explicit(&worker->active_g,
                                 memory_order_acquire) != task->probe.task) {
            Sleep(1U);
            continue;
        }
        previous = SuspendThread(state->target);
        if (previous != 0U) {
            if (previous != (DWORD)-1) {
                (void)ResumeThread(state->target);
            }
            return false;
        }
        if (!GetThreadContext(state->target, task->captured.context)) {
            (void)ResumeThread(state->target);
            return false;
        }
        pc = (uintptr_t)task->captured.context->Rip;
        rsp = (uintptr_t)task->captured.context->Rsp;
        if (pc < (uintptr_t)win_llam_async_pair_leaf_begin ||
            pc >= (uintptr_t)win_llam_async_pair_leaf_end) {
            if (ResumeThread(state->target) != 1U) {
                return false;
            }
            Sleep(1U);
            continue;
        }
        if (atomic_load_explicit(&worker->active_g,
                                 memory_order_acquire) != task->probe.task ||
            atomic_load_explicit(&worker->execution_state,
                                 memory_order_acquire) !=
                LLAM_WORKER_EXEC_MANAGED_SAFE ||
            rsp < (uintptr_t)task->probe.task->stack_base ||
            rsp >= (uintptr_t)task->probe.task->stack_base +
                       task->probe.task->stack_size ||
            shard->scheduler_ctx.rsp < state->native_low ||
            shard->scheduler_ctx.rsp >= state->native_high ||
            shard->scheduler_ctx.simd_flags != 0U ||
            !GetXStateFeaturesMask(task->captured.context,
                                   &valid_features) ||
            (valid_features & UINT64_C(0x7)) != UINT64_C(0x7) ||
            (valid_features & ~state->features) != 0U ||
            !CopyContext(task->redirected.context,
                         task->captured.context->ContextFlags,
                         task->captured.context)) {
            (void)ResumeThread(state->target);
            return false;
        }
        /* The controller retains the exact task storage past remote resume. */
        if (!llam_task_scan_ref_try_acquire(state->runtime,
                                             task->probe.task)) {
            (void)ResumeThread(state->target);
            return false;
        }
        task->controller_pin_held = true;
        task->original_g_ctx = task->probe.task->ctx;
        task->probe.worker = worker;
        task->probe.captured_rsp = rsp;
        task->probe.capture_run_generation = atomic_load_explicit(
            &worker->run_generation, memory_order_acquire);
        task->redirected.context->Rip =
            (DWORD64)(uintptr_t)llam_windows_async_restore_scheduler;
        task->redirected.context->Rcx =
            (DWORD64)(uintptr_t)&shard->scheduler_ctx;
        task->redirected.context->EFlags &= ~0x400U;
        atomic_store_explicit(&task->probe.phase,
                              LLAM_WINDOWS_ASYNC_PROBE_CAPTURED,
                              memory_order_release);
        atomic_fetch_add_explicit(&state->mutations, 1U,
                                  memory_order_relaxed);
        if (!SetThreadContext(state->target, task->redirected.context) ||
            ResumeThread(state->target) != 1U) {
            return false;
        }
        return pair_wait_phase(task, LLAM_WINDOWS_ASYNC_PROBE_READY, 4000U);
    }
    return false;
}

static bool pair_restore(pair_state_t *state, pair_task_t *task,
                          bool corrupt_rbx, bool stop_after_restore) {
    llam_worker_t *worker = task->probe.worker;
    DWORD previous;
    uintptr_t pc;

    Sleep(1U); /* Accept only the settled post-save spin interior. */
    previous = SuspendThread(state->target);
    if (previous != 0U) {
        fprintf(stderr, "llam-async-pair: gate suspend previous=%lu error=%lu\n",
                (unsigned long)previous, (unsigned long)GetLastError());
        fflush(stderr);
        if (previous != (DWORD)-1) {
            (void)ResumeThread(state->target);
        }
        return false;
    }
    if (!GetThreadContext(state->target, task->redirected.context)) {
        fprintf(stderr, "llam-async-pair: gate GetThreadContext failed error=%lu flags=0x%lx\n",
                (unsigned long)GetLastError(),
                (unsigned long)task->redirected.context->ContextFlags);
        fflush(stderr);
        (void)ResumeThread(state->target);
        return false;
    }
    pc = (uintptr_t)task->redirected.context->Rip;
    if (pc < (uintptr_t)llam_windows_async_gate_spin_begin ||
        pc >= (uintptr_t)llam_windows_async_gate_spin_end ||
        atomic_load_explicit(&worker->active_g,
                             memory_order_acquire) != task->probe.task ||
        atomic_load_explicit(&worker->run_generation,
                             memory_order_acquire) !=
            task->probe.restore_run_generation ||
        task->probe.restore_run_generation <=
            task->probe.capture_run_generation ||
        task->probe.native_oracle_result != 42 ||
        memcmp(&task->original_g_ctx, &task->probe.task->ctx,
               sizeof(task->original_g_ctx)) != 0) {
        fprintf(stderr, "llam-async-pair: restore refusal G=%u pc=%p gate=[%p,%p) active=%p expected=%p run=%llu expected_run=%llu oracle=%d ctx_equal=%d\n",
                task->index, (void *)pc,
                (void *)llam_windows_async_gate_spin_begin,
                (void *)llam_windows_async_gate_spin_end,
                (void *)atomic_load_explicit(&worker->active_g,
                                             memory_order_acquire),
                (void *)task->probe.task,
                (unsigned long long)atomic_load_explicit(
                    &worker->run_generation, memory_order_acquire),
                (unsigned long long)task->probe.restore_run_generation,
                task->probe.native_oracle_result,
                memcmp(&task->original_g_ctx, &task->probe.task->ctx,
                       sizeof(task->original_g_ctx)) == 0);
        fflush(stderr);
        (void)ResumeThread(state->target);
        return false;
    }
    if (corrupt_rbx) {
        task->captured.context->Rbx ^= 1U;
    }
    atomic_store_explicit(&task->leaf.release, 1U, memory_order_release);
    atomic_store_explicit(&task->probe.phase,
                          LLAM_WINDOWS_ASYNC_PROBE_RESTORED,
                          memory_order_release);
    atomic_fetch_add_explicit(&state->mutations, 1U,
                              memory_order_relaxed);
    if (!SetThreadContext(state->target, task->captured.context) ||
        ResumeThread(state->target) != 1U) {
        return false;
    }
    if (stop_after_restore) {
        llam_request_stop(state->runtime);
    }
    return pair_wait_phase(task, LLAM_WINDOWS_ASYNC_PROBE_FINISHED, 4000U);
}

static DWORD WINAPI pair_controller(void *opaque) {
    pair_state_t *state = (pair_state_t *)opaque;
    bool serviced[2] = {false, false};
    unsigned count;
    bool corrupt = GetEnvironmentVariableA("LLAM_PAIR_CORRUPT_RBX",
                                            NULL, 0U) != 0U;

    if (!pair_capture(state, &state->tasks[0])) {
        goto failed;
    }
    if (strcmp(state->stop_phase, "capture1") == 0) {
        llam_request_stop(state->runtime);
    }
    if (!pair_capture(state, &state->tasks[1])) {
        goto failed;
    }
    if (state->tasks[0].captured.context == state->tasks[1].captured.context ||
        state->tasks[0].captured.buffer == state->tasks[1].captured.buffer ||
        atomic_load_explicit(&state->tasks[0].probe.phase,
                             memory_order_acquire) <
            LLAM_WINDOWS_ASYNC_PROBE_READY ||
        atomic_load_explicit(&state->tasks[0].probe.phase,
                             memory_order_acquire) >
            LLAM_WINDOWS_ASYNC_PROBE_GATE ||
        atomic_load_explicit(&state->tasks[1].probe.phase,
                             memory_order_acquire) <
            LLAM_WINDOWS_ASYNC_PROBE_READY ||
        atomic_load_explicit(&state->tasks[1].probe.phase,
                             memory_order_acquire) >
            LLAM_WINDOWS_ASYNC_PROBE_GATE) {
        goto failed;
    }
    for (count = 0U; count < 2U; ++count) {
        ULONGLONG deadline = GetTickCount64() + 4000U;
        int selected = -1;

        while (GetTickCount64() < deadline && selected < 0) {
            unsigned index;

            for (index = 0U; index < 2U; ++index) {
                if (!serviced[index] &&
                    atomic_load_explicit(&state->tasks[index].probe.phase,
                                         memory_order_acquire) ==
                        LLAM_WINDOWS_ASYNC_PROBE_GATE) {
                    selected = (int)index;
                    break;
                }
            }
            if (selected < 0) {
                Sleep(1U);
            }
        }
        if (selected >= 0 && count == 0U &&
            strcmp(state->stop_phase, "gate") == 0) {
            llam_request_stop(state->runtime);
        }
        if (selected < 0 ||
            !pair_restore(state, &state->tasks[selected],
                          corrupt && selected == 0,
                          count == 0U &&
                              strcmp(state->stop_phase, "postrestore") == 0)) {
            goto failed;
        }
        {
            pair_task_t *task = &state->tasks[selected];
            volatile uint64_t final_context_access;

            if (task->index == 0U) {
                ULONGLONG deadline = GetTickCount64() + 1000U;

                /* Wait for the real detached-task reclaimer to hit our pin. */
                while (atomic_load_explicit(&task->probe.reclaim_wait_seen,
                                            memory_order_acquire) == 0U &&
                       GetTickCount64() < deadline) {
                    Sleep(1U);
                }
                if (atomic_load_explicit(&task->probe.reclaim_wait_seen,
                                         memory_order_acquire) == 0U) {
                    goto failed;
                }
            }
            final_context_access = task->captured.context->Rip;
            (void)final_context_access;
            if (!task->controller_pin_held ||
                !llam_task_scan_ref_release(state->runtime,
                                             task->probe.task)) {
                goto failed;
            }
            task->controller_pin_held = false;
            /* Do not dereference task storage after dropping the last pin. */
        }
        serviced[selected] = true;
    }
    atomic_store_explicit(&state->controller_result, 1U,
                          memory_order_release);
    return 0U;

failed:
    fprintf(stderr, "llam-async-pair: controller failed phases=%u,%u active=%p mutations=%u\n",
            atomic_load_explicit(&state->tasks[0].probe.phase,
                                 memory_order_acquire),
            atomic_load_explicit(&state->tasks[1].probe.phase,
                                 memory_order_acquire),
            (void *)atomic_load_explicit(
                &state->runtime->shards[0].primary_worker.active_g,
                memory_order_acquire),
            atomic_load_explicit(&state->mutations,
                                 memory_order_relaxed));
    fflush(stderr);
    atomic_store_explicit(&state->controller_result, 2U,
                          memory_order_release);
    return 1U;
}

static DWORD WINAPI pair_new_caller_run(void *opaque) {
    pair_state_t *state = (pair_state_t *)opaque;

    state->second_run_result = llam_run();
    return state->second_run_result == 0 ? 0U : 1U;
}

static void pair_check_new_caller_rejects_old(pair_state_t *state) {
    pair_task_t *task = &state->tasks[2];
    llam_worker_t *worker = &state->runtime->shards[0].primary_worker;
    uint64_t old_incarnation = atomic_load_explicit(
        &worker->binding_incarnation, memory_order_acquire);
    uint64_t old_run = atomic_load_explicit(
        &worker->run_generation, memory_order_acquire);
    uint64_t old_return = state->tasks[0].probe.restore_run_generation;
    pair_context_t inspection;
    HANDLE new_caller;
    bool inspected = false;
    unsigned attempt;
    unsigned mutations_before = atomic_load_explicit(&state->mutations,
                                                      memory_order_relaxed);

    memset(task, 0, sizeof(*task));
    task->owner = state;
    task->index = 2U;
    task->leaf.tag = UINT64_C(0x0fedcba987654321);
    task->leaf.desired_mxcsr = 0x3f80U;
    task->leaf.desired_cw = 0x0b7fU;
    atomic_init(&task->leaf.release, 0U);
    atomic_init(&task->entered, 0U);
    atomic_init(&task->finished, 0U);
    atomic_init(&task->error, 0U);
    if (pair_context_init(&inspection, state->features) != 0) {
        pair_fail("new-caller CONTEXT init", GetLastError());
    }
    task->public_handle = llam_spawn(pair_task_entry, task, NULL);
    task->probe.task = llam_task_resolve_public_handle(task->public_handle);
    if (task->public_handle == NULL || task->probe.task == NULL) {
        pair_fail("new-caller spawn/resolve", (unsigned)errno);
    }
    llam_task_end_public_op(task->probe.task);
    new_caller = CreateThread(NULL, 0U, pair_new_caller_run, state, 0U, NULL);
    if (new_caller == NULL) {
        pair_fail("new-caller CreateThread", GetLastError());
    }
    for (attempt = 0U; attempt < 100U; ++attempt) {
        DWORD previous;
        uintptr_t pc;
        uint64_t current_incarnation;
        uint64_t current_run;

        if (atomic_load_explicit(&task->entered, memory_order_acquire) == 0U ||
            atomic_load_explicit(&worker->active_g,
                                 memory_order_acquire) != task->probe.task) {
            Sleep(1U);
            continue;
        }
        previous = SuspendThread(new_caller);
        if (previous != 0U) {
            if (previous != (DWORD)-1) {
                (void)ResumeThread(new_caller);
            }
            pair_fail("new-caller suspension", GetLastError());
        }
        if (!GetThreadContext(new_caller, inspection.context)) {
            (void)ResumeThread(new_caller);
            pair_fail("new-caller GetThreadContext", GetLastError());
        }
        pc = (uintptr_t)inspection.context->Rip;
        if (pc < (uintptr_t)win_llam_async_pair_leaf_begin ||
            pc >= (uintptr_t)win_llam_async_pair_leaf_end) {
            if (ResumeThread(new_caller) != 1U) {
                pair_fail("new-caller skip resume", GetLastError());
            }
            Sleep(1U);
            continue;
        }
        current_incarnation = atomic_load_explicit(
            &worker->binding_incarnation, memory_order_acquire);
        current_run = atomic_load_explicit(&worker->run_generation,
                                           memory_order_acquire);
        if (current_incarnation <= old_incarnation ||
            current_run <= old_run || current_run == old_return ||
            atomic_load_explicit(&worker->active_g,
                                 memory_order_acquire) != task->probe.task ||
            atomic_load_explicit(&state->mutations,
                                 memory_order_relaxed) != mutations_before) {
            (void)ResumeThread(new_caller);
            pair_fail("stale binding/return identity admitted",
                      ERROR_INVALID_DATA);
        }
        if (ResumeThread(new_caller) != 1U) {
            pair_fail("new-caller resume", GetLastError());
        }
        inspected = true;
        break;
    }
    if (!inspected) {
        pair_fail("new-caller safe leaf timeout", WAIT_TIMEOUT);
    }
    atomic_store_explicit(&task->leaf.release, 1U, memory_order_release);
    if (WaitForSingleObject(new_caller, 5000U) != WAIT_OBJECT_0 ||
        state->second_run_result != 0 ||
        llam_join(task->public_handle) != 0 ||
        atomic_load_explicit(&task->finished,
                             memory_order_acquire) != 1U ||
        atomic_load_explicit(&task->error, memory_order_acquire) != 0U ||
        atomic_load_explicit(&state->mutations,
                             memory_order_relaxed) != mutations_before) {
        pair_fail("new-caller run/stale isolation", ERROR_INVALID_DATA);
    }
    CloseHandle(new_caller);
    free(inspection.buffer);
}

int main(void) {
    PROCESS_MITIGATION_USER_SHADOW_STACK_POLICY policy;
    llam_runtime_opts_t options;
    pair_state_t state;
    HANDLE controller;
    DWORD64 features;
    unsigned index;
    int run_result;

    SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX);
    memset(&policy, 0, sizeof(policy));
    if (!GetProcessMitigationPolicy(GetCurrentProcess(),
                                    ProcessUserShadowStackPolicy, &policy,
                                    sizeof(policy))) {
        puts("llam-async-pair: mitigation policy unavailable; skip");
        return 77;
    }
    features = GetEnabledXStateFeatures();
    if (policy.EnableUserShadowStack || policy.AuditUserShadowStack ||
        policy.EnableUserShadowStackStrictMode ||
        policy.SetContextIpValidation || policy.AuditSetContextIpValidation ||
        policy.SetContextIpValidationRelaxedMode ||
        (features & UINT64_C(0x7)) != UINT64_C(0x7) ||
        (features & ~PAIR_SUPPORTED_XSTATE) != 0U) {
        puts("llam-async-pair: unqualified mitigation/XSTATE; skip");
        return 77;
    }
    memset(&state, 0, sizeof(state));
    state.features = features;
    if (GetEnvironmentVariableA("LLAM_PAIR_STOP_PHASE", state.stop_phase,
                                sizeof(state.stop_phase)) >=
        sizeof(state.stop_phase)) {
        pair_fail("stop phase length", ERROR_INVALID_PARAMETER);
    }
    atomic_init(&state.controller_result, 0U);
    atomic_init(&state.mutations, 0U);
    for (index = 0U; index < 2U; ++index) {
        pair_task_t *task = &state.tasks[index];

        task->owner = &state;
        task->index = index;
        task->leaf.tag = index == 0U ? UINT64_C(0x1122334455667788)
                                     : UINT64_C(0x8877665544332211);
        task->leaf.desired_mxcsr = index == 0U ? 0x3f80U : 0x5f80U;
        task->leaf.desired_cw = index == 0U ? 0x0b7fU : 0x077fU;
        atomic_init(&task->leaf.release, 0U);
        atomic_init(&task->probe.phase, LLAM_WINDOWS_ASYNC_PROBE_IDLE);
        atomic_init(&task->probe.reclaim_wait_seen, 0U);
        atomic_init(&task->entered, 0U);
        atomic_init(&task->finished, 0U);
        atomic_init(&task->error, 0U);
        task->probe.native_oracle = pair_native_oracle;
        task->probe.check_return_state = true;
        if (pair_context_init(&task->captured, features) != 0 ||
            pair_context_init(&task->redirected, features) != 0) {
            pair_fail("CONTEXT init", GetLastError());
        }
    }
    state.tasks[0].probe.defer_requeue = true;
    state.tasks[1].probe.release_peer = &state.tasks[0].probe;
    if (llam_runtime_opts_init(&options, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        pair_fail("runtime options", (unsigned)errno);
    }
    options.deterministic = 1U;
    options.preempt_mode = LLAM_PREEMPT_OFF;
    if (llam_runtime_init_compat_ex(&options, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        pair_fail("runtime init", (unsigned)errno);
    }
    state.runtime = llam_runtime_default_storage();
    if (state.runtime == NULL || state.runtime->active_shards != 1U) {
        pair_fail("one-M runtime", ERROR_INVALID_DATA);
    }
    g_pair_oracle = &state;
    state.scheduler_mxcsr = pair_read_mxcsr();
    state.scheduler_cw = pair_read_cw();
    for (index = 0U; index < 2U; ++index) {
        pair_task_t *task = &state.tasks[index];

        task->public_handle = llam_spawn(pair_task_entry, task, NULL);
        task->probe.task = llam_task_resolve_public_handle(task->public_handle);
        if (task->public_handle == NULL || task->probe.task == NULL) {
            pair_fail("spawn/resolve", (unsigned)errno);
        }
        llam_task_end_public_op(task->probe.task);
    }
    if (llam_detach(state.tasks[0].public_handle) != 0) {
        pair_fail("detach first G", (unsigned)errno);
    }
    state.tasks[0].public_handle = NULL;
    GetCurrentThreadStackLimits(&state.native_low, &state.native_high);
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                         GetCurrentProcess(), &state.target,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                             THREAD_SET_CONTEXT | SYNCHRONIZE,
                         FALSE, 0U)) {
        pair_fail("DuplicateHandle", GetLastError());
    }
    llam_windows_async_probe_install_pair(&state.tasks[0].probe,
                                          &state.tasks[1].probe);
    controller = CreateThread(NULL, 0U, pair_controller, &state, 0U, NULL);
    if (controller == NULL) {
        pair_fail("CreateThread", GetLastError());
    }
    run_result = llam_run();
    if (WaitForSingleObject(controller, 5000U) != WAIT_OBJECT_0 ||
        atomic_load_explicit(&state.controller_result,
                             memory_order_acquire) != 1U ||
        run_result != 0 ||
        atomic_load_explicit(&state.mutations,
                             memory_order_acquire) != 4U) {
        pair_fail("controller/run", ERROR_INVALID_DATA);
    }
    for (index = 0U; index < 2U; ++index) {
        pair_task_t *task = &state.tasks[index];

        if ((task->public_handle != NULL &&
             llam_join(task->public_handle) != 0) ||
            atomic_load_explicit(&task->finished,
                                 memory_order_acquire) != 1U ||
            atomic_load_explicit(&task->error,
                                 memory_order_acquire) != 0U ||
            task->probe.native_oracle_result != 42 ||
            task->probe.scheduler_state_checks != 1U ||
            (index == 0U &&
             atomic_load_explicit(&task->probe.reclaim_wait_seen,
                                  memory_order_acquire) != 1U)) {
            pair_fail("two-G state oracle", ERROR_INVALID_DATA);
        }
    }
    llam_windows_async_probe_install_pair(NULL, NULL);
    pair_check_new_caller_rejects_old(&state);
    llam_runtime_shutdown();
    CloseHandle(controller);
    CloseHandle(state.target);
    for (index = 0U; index < 2U; ++index) {
        free(state.tasks[index].captured.buffer);
        free(state.tasks[index].redirected.buffer);
    }
    puts("llam-async-pair: two simultaneous capsules and full-state restoration passed");
    return 0;
}
