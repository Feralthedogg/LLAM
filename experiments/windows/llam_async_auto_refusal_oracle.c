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

/* Automatic requester negative controls; no fixture context mutation. */
#include "runtime_internal.h"
#include "windows_async_lifetime.h"

#include <errno.h>
#include <stdio.h>

extern void win_llam_async_dual_leaf(void);
extern const unsigned char win_llam_async_dual_safe_begin[];
extern const unsigned char win_llam_async_dual_safe_end[];

atomic_uint g_win_llam_async_dual_release = ATOMIC_VAR_INIT(0U);
atomic_uint g_win_llam_async_dual_stage = ATOMIC_VAR_INIT(0U);

typedef struct auto_refusal_state {
    llam_runtime_t *runtime;
    atomic_uint entered;
    atomic_uint done;
    atomic_uint task_error;
    atomic_uint monitor_done;
    bool empty_registry;
    bool no_preempt;
} auto_refusal_state_t;

static void auto_refusal_fail(const char *where) {
    fprintf(stderr, "llam-async-auto-refusal: %s failed\n", where);
    ExitProcess(1U);
}

static void auto_refusal_task(void *opaque) {
    auto_refusal_state_t *state = (auto_refusal_state_t *)opaque;

    errno = 123;
    SetLastError(456U);
    atomic_store_explicit(&state->entered, 1U, memory_order_release);
    win_llam_async_dual_leaf();
    if (errno != 123 || GetLastError() != 456U) {
        atomic_store_explicit(&state->task_error, 1U,
                              memory_order_release);
    }
    atomic_store_explicit(&state->done, 1U, memory_order_release);
}

static DWORD WINAPI auto_refusal_monitor(void *opaque) {
    auto_refusal_state_t *state = (auto_refusal_state_t *)opaque;
    ULONGLONG deadline = GetTickCount64() + 2000U;
    llam_windows_async_stats_t stats;
    uint64_t initial_attempts;

    while (GetTickCount64() < deadline &&
           (atomic_load_explicit(&state->entered,
                                 memory_order_acquire) == 0U ||
            atomic_load_explicit(&g_win_llam_async_dual_stage,
                                 memory_order_acquire) !=
                ((state->empty_registry || state->no_preempt) ? 1U : 2U))) {
        Sleep(1U);
    }
    if (GetTickCount64() >= deadline ||
        !llam_windows_async_lifetime_stats(state->runtime, &stats)) {
        auto_refusal_fail("entered/probe statistics");
    }
    if (state->empty_registry || state->no_preempt) {
        Sleep(60U);
        if (!llam_windows_async_lifetime_stats(state->runtime, &stats) ||
            stats.capture_attempts != 0U || stats.captures != 0U ||
            stats.restores != 0U) {
            auto_refusal_fail("empty registry admitted a probe");
        }
    } else {
        while (GetTickCount64() < deadline) {
            if (!llam_windows_async_lifetime_stats(state->runtime, &stats)) {
                auto_refusal_fail("unsafe probe statistics");
            }
            if (stats.unsafe_pc_refusals != 0U) {
                break;
            }
            Sleep(1U);
        }
        if (stats.unsafe_pc_refusals == 0U || stats.captures != 0U ||
            stats.restores != 0U) {
            auto_refusal_fail("unsafe RIP was not refused");
        }
        initial_attempts = stats.capture_attempts;
        Sleep(60U);
        if (!llam_windows_async_lifetime_stats(state->runtime, &stats) ||
            stats.capture_attempts < initial_attempts ||
            stats.capture_attempts - initial_attempts > 16U ||
            stats.captures != 0U || stats.restores != 0U) {
            auto_refusal_fail("unsafe retry storm or context mutation");
        }
    }
    atomic_store_explicit(&g_win_llam_async_dual_release, 3U,
                          memory_order_release);
    atomic_store_explicit(&state->monitor_done, 1U,
                          memory_order_release);
    return 0U;
}

int main(void) {
    llam_runtime_opts_t options;
    auto_refusal_state_t state;
    llam_task_t *handle;
    llam_spawn_opts_t spawn_opts;
    HANDLE monitor;
    llam_windows_async_stats_t stats;
    int result;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.entered, 0U);
    atomic_init(&state.done, 0U);
    atomic_init(&state.task_error, 0U);
    atomic_init(&state.monitor_done, 0U);
    state.empty_registry =
        GetEnvironmentVariableA("LLAM_AUTO_EMPTY", NULL, 0U) != 0U;
    state.no_preempt =
        GetEnvironmentVariableA("LLAM_AUTO_NO_PREEMPT", NULL, 0U) != 0U;
    if (llam_runtime_opts_init(&options, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return 1;
    }
    options.deterministic = 1U;
    options.preempt_mode = LLAM_PREEMPT_STRICT;
    options.preempt_quantum_ns = 1000000U;
    if (llam_runtime_init_compat_ex(&options, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return 1;
    }
    state.runtime = llam_runtime_default_storage();
    if (!state.empty_registry &&
        llam_windows_async_lifetime_register_safe_range(
            state.runtime, (uintptr_t)win_llam_async_dual_safe_begin,
            (uintptr_t)win_llam_async_dual_safe_end) != 0) {
        auto_refusal_fail("register safe range");
    }
    if (llam_windows_async_lifetime_enable(state.runtime) != 0) {
        if (errno == ENOTSUP) {
            llam_runtime_shutdown();
            return 77;
        }
        auto_refusal_fail("enable");
    }
    if (!state.empty_registry && !state.no_preempt) {
        atomic_store_explicit(&g_win_llam_async_dual_release, 1U,
                              memory_order_release);
    }
    memset(&spawn_opts, 0, sizeof(spawn_opts));
    spawn_opts.task_class = (uint32_t)LLAM_TASK_CLASS_DEFAULT;
    spawn_opts.stack_class = (uint32_t)LLAM_STACK_CLASS_DEFAULT;
    spawn_opts.flags = state.no_preempt ? LLAM_SPAWN_F_NO_PREEMPT : 0U;
    handle = llam_spawn(auto_refusal_task, &state, &spawn_opts);
    if (handle == NULL || llam_detach(handle) != 0) {
        auto_refusal_fail("spawn/detach");
    }
    monitor = CreateThread(NULL, 0U, auto_refusal_monitor,
                           &state, 0U, NULL);
    if (monitor == NULL) {
        auto_refusal_fail("monitor thread");
    }
    result = llam_run();
    if (result != 0 ||
        WaitForSingleObject(monitor, 5000U) != WAIT_OBJECT_0 ||
        atomic_load_explicit(&state.monitor_done,
                             memory_order_acquire) != 1U ||
        atomic_load_explicit(&state.done,
                             memory_order_acquire) != 1U ||
        atomic_load_explicit(&state.task_error,
                             memory_order_acquire) != 0U ||
        llam_windows_async_lifetime_charged_bytes(state.runtime) != 0U ||
        !llam_windows_async_lifetime_stats(state.runtime, &stats) ||
        stats.captures != 0U || stats.restores != 0U) {
        auto_refusal_fail("run/drain");
    }
    CloseHandle(monitor);
    llam_runtime_shutdown();
    puts(state.no_preempt
             ? "llam-async-auto-refusal: NO_PREEMPT made zero probes"
             : state.empty_registry
             ? "llam-async-auto-refusal: empty registry made zero probes"
             : "llam-async-auto-refusal: unsafe RIP refused without mutation");
    return 0;
}
