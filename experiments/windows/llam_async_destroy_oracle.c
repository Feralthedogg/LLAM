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

/* Public host destroy held across one real automatic Windows restore. */
#include "runtime_internal.h"
#include "windows_async_lifetime.h"

#include <stdio.h>
#include <stdlib.h>

extern void win_llam_async_leaf(void);
extern const unsigned char win_llam_async_leaf_begin[];
extern const unsigned char win_llam_async_leaf_end[];

atomic_uint g_win_llam_async_release = ATOMIC_VAR_INIT(0U);

typedef struct destroy_state {
    llam_runtime_t *runtime;
    llam_task_t *task;
    atomic_uint entered;
    atomic_uint task_done;
    atomic_uint gate_observed;
    atomic_uint gate_release;
    atomic_uint destroy_started;
    atomic_uint destroy_done;
    atomic_uint run_done;
    atomic_int run_result;
    atomic_uint first_run_done;
    atomic_int first_run_result;
    atomic_uint release_first_runner;
    DWORD first_thread_id;
    uint64_t captures_baseline;
    uint64_t restores_baseline;
    uint64_t witnesses_baseline;
    unsigned phase;
} destroy_state_t;

static void destroy_fail(const char *where) {
    fprintf(stderr, "llam-async-destroy: %s failed\n", where);
    ExitProcess(1U);
}

static bool destroy_has_bank_phase(const destroy_state_t *state,
                                   unsigned phase) {
    const llam_windows_async_capsule_t *capsule = state->task->async_preempt_task;
    unsigned i;

    if (capsule == NULL) {
        return false;
    }
    for (i = 0U; i < 2U; ++i) {
        if (atomic_load_explicit(&capsule->banks[i].phase,
                                 memory_order_acquire) == phase) {
            return true;
        }
    }
    return false;
}

static void destroy_task(void *opaque) {
    destroy_state_t *state = (destroy_state_t *)opaque;

    atomic_store_explicit(&state->entered, 1U, memory_order_release);
    win_llam_async_leaf();
    (void)atomic_fetch_add_explicit(&state->task_done, 1U,
                                    memory_order_release);
}

static DWORD WINAPI destroy_first_runner(void *opaque) {
    destroy_state_t *state = (destroy_state_t *)opaque;
    int result;

    state->first_thread_id = GetCurrentThreadId();
    result = llam_runtime_run_handle(state->runtime);
    atomic_store_explicit(&state->first_run_result, result,
                          memory_order_release);
    atomic_store_explicit(&state->first_run_done, 1U,
                          memory_order_release);
    while (atomic_load_explicit(&state->release_first_runner,
                                memory_order_acquire) == 0U) {
        Sleep(1U);
    }
    return result == 0 ? 0U : 1U;
}

static DWORD WINAPI destroy_runner(void *opaque) {
    destroy_state_t *state = (destroy_state_t *)opaque;
    int result = llam_runtime_run_handle(state->runtime);

    atomic_store_explicit(&state->run_result, result,
                          memory_order_release);
    atomic_store_explicit(&state->run_done, 1U,
                          memory_order_release);
    return result == 0 ? 0U : 1U;
}

static DWORD WINAPI destroy_host(void *opaque) {
    destroy_state_t *state = (destroy_state_t *)opaque;

    atomic_store_explicit(&state->destroy_started, 1U,
                          memory_order_release);
    llam_runtime_destroy(state->runtime);
    atomic_store_explicit(&state->destroy_done, 1U,
                          memory_order_release);
    return 0U;
}

static void destroy_wait_gate(destroy_state_t *state) {
    ULONGLONG deadline = GetTickCount64() + 4000U;

    while (GetTickCount64() < deadline) {
        if (state->phase == 0U) {
            if (llam_lease_test_gate_reached(
                    LLAM_LEASE_TEST_AFTER_TASK_STATE_PREPARE)) {
                return;
            }
        } else if (atomic_load_explicit(&state->gate_observed,
                                        memory_order_acquire) != 0U) {
            return;
        }
        Sleep(1U);
    }
    destroy_fail("phase gate not reached");
}

static void destroy_validate_phase(destroy_state_t *state) {
    llam_windows_async_stats_t stats;
    unsigned expected_bank_phase;

    if (!llam_windows_async_lifetime_stats(state->runtime, &stats)) {
        destroy_fail("phase statistics");
    }
    if (state->phase == 0U) {
        if (stats.captures != 0U || stats.binding_ops != 0U ||
            stats.pending_restore_active != 0U ||
            stats.execution_debt != 0U) {
            destroy_fail("pre-dispatch acquired async debt");
        }
        return;
    }
    expected_bank_phase = state->phase == 1U
                              ? LLAM_WINDOWS_ASYNC_BANK_READY
                              : state->phase == 2U
                                    ? LLAM_WINDOWS_ASYNC_BANK_GATE
                                    : LLAM_WINDOWS_ASYNC_BANK_RESTORING;
    if (stats.captures <= state->captures_baseline ||
        stats.binding_ops != 1U ||
        stats.execution_debt != 1U ||
        stats.pending_restore_active != (state->phase == 3U ? 1U : 0U) ||
        stats.restores != state->restores_baseline +
                              (state->phase == 3U ? 1U : 0U) ||
        !destroy_has_bank_phase(state, expected_bank_phase)) {
        destroy_fail("unexpected admitted phase/debt");
    }
}

static void destroy_wait_stop(destroy_state_t *state) {
    ULONGLONG deadline = GetTickCount64() + 3000U;

    while (GetTickCount64() < deadline &&
           !atomic_load_explicit(&state->runtime->stop_requested,
                                 memory_order_acquire)) {
        if (atomic_load_explicit(&state->destroy_done,
                                 memory_order_acquire) != 0U) {
            destroy_fail("destroy finished before held run");
        }
        Sleep(1U);
    }
    if (!atomic_load_explicit(&state->runtime->stop_requested,
                              memory_order_acquire) ||
        atomic_load_explicit(&state->destroy_done,
                             memory_order_acquire) != 0U) {
        destroy_fail("destroy did not wait on run");
    }
}

static void destroy_wait_direct_witness(destroy_state_t *state) {
    ULONGLONG deadline = GetTickCount64() + 4000U;

    while (GetTickCount64() < deadline) {
        llam_windows_async_stats_t stats;

        if (!llam_windows_async_lifetime_stats(state->runtime, &stats)) {
            destroy_fail("witness statistics");
        }
        if (stats.direct_witnesses > state->witnesses_baseline &&
            stats.binding_ops == 0U &&
            stats.pending_restore_active == 0U) {
            return;
        }
        Sleep(1U);
    }
    destroy_fail("destroy abandoned restored-G witness");
}

static void destroy_finish_first_run(destroy_state_t *state) {
    ULONGLONG deadline = GetTickCount64() + 4000U;
    llam_windows_async_stats_t stats = {0};

    while (GetTickCount64() < deadline) {
        if (!llam_windows_async_lifetime_stats(state->runtime, &stats)) {
            destroy_fail("first-run statistics");
        }
        if (stats.captures != 0U && stats.restores != 0U &&
            stats.direct_witnesses != 0U && stats.binding_ops == 0U &&
            stats.pending_restore_active == 0U) {
            break;
        }
        Sleep(1U);
    }
    if (stats.direct_witnesses == 0U || stats.binding_ops != 0U ||
        stats.pending_restore_active != 0U) {
        destroy_fail("first-run automatic witness");
    }
    atomic_store_explicit(&g_win_llam_async_release, 1U,
                          memory_order_release);
    while (atomic_load_explicit(&state->first_run_done,
                                memory_order_acquire) == 0U &&
           GetTickCount64() < deadline) {
        Sleep(1U);
    }
    if (atomic_load_explicit(&state->first_run_done,
                             memory_order_acquire) != 1U ||
        atomic_load_explicit(&state->first_run_result,
                             memory_order_acquire) != 0 ||
        atomic_load_explicit(&state->task_done,
                             memory_order_acquire) != 1U ||
        !llam_windows_async_lifetime_stats(state->runtime, &stats) ||
        stats.binding_ops != 0U || stats.captured_bank_active != 0U ||
        stats.pending_restore_active != 0U ||
        stats.execution_debt != 0U || stats.retire_count != 0U ||
        stats.finalizer_count != 0U ||
        llam_windows_async_lifetime_charged_bytes(state->runtime) != 0U) {
        destroy_fail("first-run drain");
    }
    state->captures_baseline = stats.captures;
    state->restores_baseline = stats.restores;
    state->witnesses_baseline = stats.direct_witnesses;
}

int main(void) {
    llam_runtime_opts_t options;
    destroy_state_t state;
    llam_task_t *public_task;
    HANDLE runner;
    HANDLE first_runner = NULL;
    HANDLE destroyer;
    char mode[8] = {0};
    uint64_t first_incarnation = 0U;
    uint64_t first_worker_run = 0U;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.entered, 0U);
    atomic_init(&state.task_done, 0U);
    atomic_init(&state.gate_observed, 0U);
    atomic_init(&state.gate_release, 0U);
    atomic_init(&state.destroy_started, 0U);
    atomic_init(&state.destroy_done, 0U);
    atomic_init(&state.run_done, 0U);
    atomic_init(&state.run_result, -1);
    atomic_init(&state.first_run_done, 0U);
    atomic_init(&state.first_run_result, -1);
    atomic_init(&state.release_first_runner, 0U);
    if (GetEnvironmentVariableA("LLAM_ASYNC_DESTROY_PHASE", mode,
                                sizeof(mode)) != 1U ||
        mode[0] < '0' || mode[0] > '4') {
        return 1;
    }
    state.phase = (unsigned)(mode[0] - '0');
    if (llam_runtime_opts_init(&options, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return 1;
    }
    options.deterministic = 1U;
    options.preempt_mode = LLAM_PREEMPT_STRICT;
    options.preempt_quantum_ns = 1000000U;
    if (llam_runtime_create(&options, LLAM_RUNTIME_OPTS_CURRENT_SIZE,
                            &state.runtime) != 0 ||
        llam_windows_async_lifetime_register_safe_range(
            state.runtime, (uintptr_t)win_llam_async_leaf_begin,
            (uintptr_t)win_llam_async_leaf_end) != 0 ||
        llam_windows_async_lifetime_enable(state.runtime) != 0) {
        if (errno == ENOTSUP && state.runtime != NULL) {
            llam_runtime_destroy(state.runtime);
            return 77;
        }
        destroy_fail("runtime create/enable");
    }
    public_task = llam_runtime_spawn_ex(state.runtime, destroy_task,
                                        &state, NULL, 0U);
    if (public_task == NULL) {
        destroy_fail("spawn");
    }
    state.task = llam_task_resolve_public_handle(public_task);
    if (state.task == NULL) {
        destroy_fail("resolve task");
    }
    llam_task_end_public_op(state.task);
    if (llam_detach(public_task) != 0) {
        destroy_fail("detach");
    }
    if (state.phase == 4U) {
        first_runner = CreateThread(NULL, 0U, destroy_first_runner,
                                    &state, 0U, NULL);
        if (first_runner == NULL) {
            destroy_fail("first runner thread");
        }
        destroy_finish_first_run(&state);
        first_incarnation = atomic_load_explicit(
            &state.runtime->shards[0].primary_worker.incarnation,
            memory_order_acquire);
        first_worker_run = atomic_load_explicit(
            &state.runtime->shards[0].primary_worker.run_generation,
            memory_order_acquire);
        if (first_incarnation == 0U || first_worker_run == 0U ||
            state.first_thread_id == 0U) {
            destroy_fail("first binding identity");
        }
        atomic_store_explicit(&g_win_llam_async_release, 0U,
                              memory_order_release);
        atomic_store_explicit(&state.entered, 0U,
                              memory_order_release);
        atomic_store_explicit(&state.task_done, 0U,
                              memory_order_release);
        state.phase = 3U;
        public_task = llam_runtime_spawn_ex(state.runtime, destroy_task,
                                            &state, NULL, 0U);
        if (public_task == NULL) {
            destroy_fail("second-run spawn");
        }
        state.task = llam_task_resolve_public_handle(public_task);
        if (state.task == NULL) {
            destroy_fail("second-run task resolve");
        }
        llam_task_end_public_op(state.task);
        if (llam_detach(public_task) != 0) {
            destroy_fail("second-run detach");
        }
    }
    if (state.phase == 0U) {
        llam_lease_test_set_gate(
            LLAM_LEASE_TEST_AFTER_TASK_STATE_PREPARE, true);
    } else {
        llam_windows_async_lifetime_test_phase_gate(
            state.task, state.phase, &state.gate_observed,
            &state.gate_release);
    }
    runner = CreateThread(NULL, 0U, destroy_runner, &state, 0U, NULL);
    if (runner == NULL) {
        destroy_fail("runner thread");
    }
    destroy_wait_gate(&state);
    destroy_validate_phase(&state);
    if (first_runner != NULL &&
        (GetThreadId(runner) == state.first_thread_id ||
         atomic_load_explicit(
             &state.runtime->shards[0].primary_worker.incarnation,
             memory_order_acquire) <= first_incarnation ||
         atomic_load_explicit(
             &state.runtime->shards[0].primary_worker.run_generation,
             memory_order_acquire) <= first_worker_run)) {
        destroy_fail("second borrowed M identity");
    }
    destroyer = CreateThread(NULL, 0U, destroy_host, &state, 0U, NULL);
    if (destroyer == NULL) {
        destroy_fail("destroy thread");
    }
    destroy_wait_stop(&state);
    if (state.phase == 0U) {
        atomic_store_explicit(&g_win_llam_async_release, 1U,
                              memory_order_release);
        llam_lease_test_set_gate(
            LLAM_LEASE_TEST_AFTER_TASK_STATE_PREPARE, false);
    } else {
        atomic_store_explicit(&state.gate_release, 1U,
                              memory_order_release);
        if (state.phase == 3U) {
            destroy_wait_direct_witness(&state);
        }
        atomic_store_explicit(&g_win_llam_async_release, 1U,
                              memory_order_release);
    }
    if (WaitForSingleObject(runner, 5000U) != WAIT_OBJECT_0 ||
        WaitForSingleObject(destroyer, 5000U) != WAIT_OBJECT_0 ||
        atomic_load_explicit(&state.run_done,
                             memory_order_acquire) != 1U ||
        atomic_load_explicit(&state.run_result,
                             memory_order_acquire) != 0 ||
        atomic_load_explicit(&state.destroy_done,
                             memory_order_acquire) != 1U ||
        (state.phase != 0U &&
         atomic_load_explicit(&state.task_done,
                              memory_order_acquire) != 1U)) {
        destroy_fail("run/destroy drain");
    }
    llam_windows_async_lifetime_test_phase_gate(NULL, 0U, NULL, NULL);
    if (first_runner != NULL) {
        atomic_store_explicit(&state.release_first_runner, 1U,
                              memory_order_release);
        if (WaitForSingleObject(first_runner, 5000U) != WAIT_OBJECT_0) {
            destroy_fail("first runner exit");
        }
        CloseHandle(first_runner);
    }
    CloseHandle(runner);
    CloseHandle(destroyer);
    puts(first_incarnation != 0U
             ? "llam-async-destroy: second native M rebind and destroy passed"
             : "llam-async-destroy: public destroy overlapped admitted phase");
    return 0;
}
