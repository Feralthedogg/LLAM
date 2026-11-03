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

/* Host consuming join races the first reclaim_ready publication. */
#include "runtime_internal.h"
#include "windows_async_lifetime.h"

#include <stdio.h>

typedef struct ready_race_state {
    llam_runtime_t *runtime;
    llam_task_t *task;
    llam_task_t *public_handle;
    atomic_uint observed;
    atomic_uint release;
    atomic_uint monitor_seen;
    atomic_uint join_done;
    atomic_int join_result;
} ready_race_state_t;

static void ready_fail(const char *where) {
    fprintf(stderr, "llam-async-ready-race: %s failed\n", where);
    ExitProcess(1U);
}

static void ready_task(void *opaque) {
    (void)opaque;
}

static DWORD WINAPI ready_joiner(void *opaque) {
    ready_race_state_t *state = (ready_race_state_t *)opaque;
    int result = llam_join(state->public_handle);

    atomic_store_explicit(&state->join_result, result,
                          memory_order_release);
    atomic_store_explicit(&state->join_done, 1U,
                          memory_order_release);
    return result == 0 ? 0U : 1U;
}

static DWORD WINAPI ready_monitor(void *opaque) {
    ready_race_state_t *state = (ready_race_state_t *)opaque;
    ULONGLONG deadline = GetTickCount64() + 4000U;

    while (atomic_load_explicit(&state->observed,
                                memory_order_acquire) == 0U &&
           GetTickCount64() < deadline) {
        Sleep(1U);
    }
    if (atomic_load_explicit(&state->observed,
                             memory_order_acquire) == 0U) {
        atomic_store_explicit(&state->release, 1U,
                              memory_order_release);
        return 1U;
    }
    /* The scheduler's handoff scan ref must keep G alive through its last use. */
    while (atomic_load_explicit(&state->task->reclaim_claimed,
                                memory_order_acquire) == 0U &&
           GetTickCount64() < deadline) {
        Sleep(1U);
    }
    if (atomic_load_explicit(&state->task->reclaim_claimed,
                             memory_order_acquire) == 0U ||
        atomic_load_explicit(&state->task->scan_refs,
                             memory_order_acquire) == 0U ||
        state->task->stack_mapping != NULL ||
        llam_windows_async_lifetime_charged_bytes(state->runtime) != 0U ||
        atomic_load_explicit(&state->join_done,
                             memory_order_acquire) != 0U) {
        atomic_store_explicit(&state->release, 1U,
                              memory_order_release);
        return 1U;
    }
    atomic_store_explicit(&state->monitor_seen, 1U,
                          memory_order_release);
    atomic_store_explicit(&state->release, 1U,
                          memory_order_release);
    return 0U;
}

int main(void) {
    llam_runtime_opts_t options;
    ready_race_state_t state;
    HANDLE joiner;
    HANDLE monitor;
    DWORD join_exit = 1U;
    DWORD monitor_exit = 1U;
    int run_result;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.observed, 0U);
    atomic_init(&state.release, 0U);
    atomic_init(&state.monitor_seen, 0U);
    atomic_init(&state.join_done, 0U);
    atomic_init(&state.join_result, -1);
    if (llam_runtime_opts_init(&options, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return 1;
    }
    options.deterministic = 1U;
    options.preempt_mode = LLAM_PREEMPT_OFF;
    if (llam_runtime_init_ex(&options, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return 1;
    }
    state.runtime = llam_runtime_default_storage();
    if (llam_windows_async_lifetime_enable(state.runtime) != 0) {
        if (errno == ENOTSUP) {
            llam_runtime_shutdown();
            return 77;
        }
        ready_fail("enable");
    }
    state.public_handle = llam_spawn(ready_task, NULL, NULL);
    if (state.public_handle == NULL) {
        ready_fail("spawn");
    }
    state.task = llam_task_resolve_public_handle(state.public_handle);
    if (state.task == NULL) {
        ready_fail("resolve handle");
    }
    llam_task_end_public_op(state.task);
    llam_windows_async_lifetime_test_ready_gate(state.task,
                                                &state.observed,
                                                &state.release);
    joiner = CreateThread(NULL, 0U, ready_joiner, &state, 0U, NULL);
    monitor = CreateThread(NULL, 0U, ready_monitor, &state, 0U, NULL);
    if (joiner == NULL || monitor == NULL) {
        ready_fail("thread create");
    }
    run_result = llam_run();
    if (run_result != 0 ||
        WaitForSingleObject(monitor, 5000U) != WAIT_OBJECT_0 ||
        WaitForSingleObject(joiner, 5000U) != WAIT_OBJECT_0 ||
        !GetExitCodeThread(monitor, &monitor_exit) ||
        !GetExitCodeThread(joiner, &join_exit) ||
        monitor_exit != 0U || join_exit != 0U ||
        atomic_load_explicit(&state.monitor_seen,
                             memory_order_acquire) != 1U ||
        atomic_load_explicit(&state.join_result,
                             memory_order_acquire) != 0 ||
        llam_windows_async_lifetime_charged_bytes(state.runtime) != 0U) {
        ready_fail("join/readiness race");
    }
    llam_windows_async_lifetime_test_ready_gate(NULL, NULL, NULL);
    CloseHandle(monitor);
    CloseHandle(joiner);
    llam_runtime_shutdown();
    puts("llam-async-ready-race: host join waited for scheduler handoff");
    return 0;
}
