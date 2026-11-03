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

/* Production range lookup against the actual stopped RIP, without mutation. */
#include "runtime_internal.h"
#include "windows_async_lifetime.h"

#include <stdio.h>

extern void win_llam_async_safe_regions(void);
extern const unsigned char win_llam_async_safe_begin[];
extern const unsigned char win_llam_async_safe_end[];

atomic_uint g_win_llam_safe_region_stage = ATOMIC_VAR_INIT(0U);
atomic_uint g_win_llam_safe_region_release = ATOMIC_VAR_INIT(0U);

typedef struct safe_pc_state {
    llam_runtime_t *runtime;
    llam_task_t *task;
    atomic_uint release_c;
    atomic_uint controller_done;
    atomic_uint probes;
    bool empty_registry;
} safe_pc_state_t;

static void safe_pc_fail(const char *where) {
    fprintf(stderr, "llam-async-safe-pc: %s failed\n", where);
    ExitProcess(1U);
}

static void safe_pc_task(void *opaque) {
    safe_pc_state_t *state = (safe_pc_state_t *)opaque;

    win_llam_async_safe_regions();
    atomic_store_explicit(&g_win_llam_safe_region_stage, 4U,
                          memory_order_release);
    while (atomic_load_explicit(&state->release_c,
                                memory_order_acquire) == 0U) {
        YieldProcessor();
    }
}

static bool safe_pc_probe(safe_pc_state_t *state, unsigned region) {
    llam_windows_async_operation_t op;
    ULONGLONG deadline = GetTickCount64() + 4000U;
    DWORD previous;
    uintptr_t rip;
    uintptr_t lo = (uintptr_t)win_llam_async_safe_begin;
    uintptr_t hi = (uintptr_t)win_llam_async_safe_end;
    bool allowed;

    memset(&op, 0, sizeof(op));
    while (GetTickCount64() < deadline) {
        if (atomic_load_explicit(&g_win_llam_safe_region_stage,
                                 memory_order_acquire) == region &&
            llam_windows_async_lifetime_op_begin(state->runtime, &op)) {
            break;
        }
        Sleep(1U);
    }
    if (GetTickCount64() >= deadline || op.task != state->task) {
        return false;
    }
    previous = SuspendThread(op.native_thread);
    if (previous != 0U) {
        if (previous != (DWORD)-1) {
            (void)ResumeThread(op.native_thread);
        }
        return false;
    }
    if (!GetThreadContext(op.native_thread, op.bank->captured.context)) {
        (void)ResumeThread(op.native_thread);
        return false;
    }
    rip = (uintptr_t)op.bank->captured.context->Rip;
    allowed = llam_windows_async_lifetime_pc_allowed(&op, rip);
    if (region == 1U && (rip >= lo || allowed)) {
        (void)ResumeThread(op.native_thread);
        return false;
    }
    if (region == 2U) {
        llam_windows_async_operation_t stale = op;

        stale.range_generation = op.range_generation > 0U
                                     ? op.range_generation - 1U
                                     : UINT64_MAX;
        if (rip < lo || rip >= hi ||
            allowed == state->empty_registry ||
            llam_windows_async_lifetime_pc_allowed(&stale, rip) ||
            llam_windows_async_lifetime_pc_allowed(&op, lo - 1U) ||
            llam_windows_async_lifetime_pc_allowed(&op, hi) ||
            (state->empty_registry &&
             llam_windows_async_lifetime_op_capture_committed(&op))) {
            (void)ResumeThread(op.native_thread);
            return false;
        }
    }
    if (region == 3U && (rip < hi || allowed)) {
        (void)ResumeThread(op.native_thread);
        return false;
    }
    if (region == 4U && allowed) {
        (void)ResumeThread(op.native_thread);
        return false;
    }
    if (region != 2U &&
        llam_windows_async_lifetime_op_capture_committed(&op)) {
        (void)ResumeThread(op.native_thread);
        return false;
    }
    if (ResumeThread(op.native_thread) != 1U) {
        return false;
    }
    llam_windows_async_lifetime_op_abort(&op);
    (void)atomic_fetch_add_explicit(&state->probes, 1U,
                                    memory_order_release);
    return true;
}

static DWORD WINAPI safe_pc_controller(void *opaque) {
    safe_pc_state_t *state = (safe_pc_state_t *)opaque;
    unsigned region;

    for (region = 1U; region <= 4U; ++region) {
        if (!safe_pc_probe(state, region)) {
            safe_pc_fail("stopped RIP qualification");
        }
        if (region < 4U) {
            (void)atomic_fetch_or_explicit(&g_win_llam_safe_region_release,
                                           1U << (region - 1U),
                                           memory_order_release);
        } else {
            atomic_store_explicit(&state->release_c, 1U,
                                  memory_order_release);
        }
    }
    atomic_store_explicit(&state->controller_done, 1U,
                          memory_order_release);
    return 0U;
}

int main(void) {
    llam_runtime_opts_t options;
    safe_pc_state_t state;
    llam_task_t *handle;
    HANDLE controller;
    bool empty_registry;
    int run_result;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.release_c, 0U);
    atomic_init(&state.controller_done, 0U);
    atomic_init(&state.probes, 0U);
    empty_registry = GetEnvironmentVariableA("LLAM_SAFEPC_EMPTY", NULL, 0U) != 0U;
    state.empty_registry = empty_registry;
    if (llam_runtime_opts_init(&options, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return 1;
    }
    options.deterministic = 1U;
    options.preempt_mode = LLAM_PREEMPT_OFF;
    if (llam_runtime_init_ex(&options, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return 1;
    }
    state.runtime = llam_runtime_default_storage();
    if (!empty_registry) {
        uintptr_t lo = (uintptr_t)win_llam_async_safe_begin;
        uintptr_t hi = (uintptr_t)win_llam_async_safe_end;

        if (llam_windows_async_lifetime_register_safe_range(
                state.runtime, lo, hi) != 0) {
            safe_pc_fail("register");
        }
        errno = 0;
        if (llam_windows_async_lifetime_register_safe_range(
                state.runtime, lo + 1U, hi) == 0 || errno != EINVAL) {
            safe_pc_fail("overlap refusal");
        }
    }
    if (llam_windows_async_lifetime_enable(state.runtime) != 0) {
        if (errno == ENOTSUP) {
            llam_runtime_shutdown();
            return 77;
        }
        safe_pc_fail("enable");
    }
    handle = llam_spawn(safe_pc_task, &state, NULL);
    if (handle == NULL) {
        safe_pc_fail("spawn");
    }
    state.task = llam_task_resolve_public_handle(handle);
    if (state.task == NULL) {
        safe_pc_fail("resolve handle");
    }
    llam_task_end_public_op(state.task);
    if (llam_detach(handle) != 0) {
        safe_pc_fail("detach");
    }
    controller = CreateThread(NULL, 0U, safe_pc_controller,
                              &state, 0U, NULL);
    if (controller == NULL) {
        safe_pc_fail("controller thread");
    }
    run_result = llam_run();
    if (run_result != 0 ||
        WaitForSingleObject(controller, 5000U) != WAIT_OBJECT_0 ||
        atomic_load_explicit(&state.controller_done,
                             memory_order_acquire) != 1U ||
        atomic_load_explicit(&state.probes,
                             memory_order_acquire) != 4U ||
        llam_windows_async_lifetime_charged_bytes(state.runtime) != 0U) {
        safe_pc_fail("run/retirement");
    }
    CloseHandle(controller);
    llam_runtime_shutdown();
    puts(empty_registry
             ? "llam-async-safe-pc: empty registry refused all regions"
             : "llam-async-safe-pc: actual stopped RIP admitted only in safe range");
    return 0;
}
