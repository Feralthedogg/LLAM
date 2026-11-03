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

/* Deterministic configuration and delayed-controller lifetime boundaries. */
#include "runtime_internal.h"
#include "windows_async_lifetime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void win_llam_async_leaf(void);
extern const unsigned char win_llam_async_leaf_begin[];
extern const unsigned char win_llam_async_leaf_end[];

atomic_uint g_win_llam_async_release = ATOMIC_VAR_INIT(0U);

typedef struct config_state {
    llam_runtime_t *runtime;
    llam_task_t *spawn_handle;
    atomic_uint observed;
    atomic_uint release;
    atomic_uint destroy_started;
    atomic_uint destroy_done;
    atomic_uint leaf_entered;
    atomic_uint leaf_done;
    int enable_result;
    int enable_error;
    int spawn_error;
    int run_result;
    llam_task_t *foreign_spawn_handle;
    int foreign_spawn_error;
    DWORD run_thread_id;
    llam_windows_async_preempt_range_t public_range;
    llam_windows_async_preempt_opts_t public_opts;
    int managed_enable_result;
    int managed_enable_error;
} config_state_t;

static void config_fail(const char *where) {
    fprintf(stderr, "llam-async-config: %s failed (errno=%d)\n", where, errno);
    ExitProcess(1U);
}

static void config_wait_at_least(atomic_uint *value, unsigned minimum,
                                 const char *where) {
    ULONGLONG deadline = GetTickCount64() + 4000U;

    while (atomic_load_explicit(value, memory_order_acquire) < minimum &&
           GetTickCount64() < deadline) {
        Sleep(1U);
    }
    if (atomic_load_explicit(value, memory_order_acquire) < minimum) {
        config_fail(where);
    }
}

static void config_wait(atomic_uint *value, const char *where) {
    config_wait_at_least(value, 1U, where);
}

static void config_join(HANDLE thread, const char *where) {
    DWORD exit_code = 1U;

    if (thread == NULL || WaitForSingleObject(thread, 4000U) != WAIT_OBJECT_0 ||
        !GetExitCodeThread(thread, &exit_code) || exit_code != 0U) {
        config_fail(where);
    }
    CloseHandle(thread);
}

static void config_never_run(void *opaque) {
    (void)opaque;
    abort();
}

static DWORD WINAPI config_enable_thread(void *opaque) {
    config_state_t *state = (config_state_t *)opaque;

    state->enable_result = llam_windows_async_lifetime_enable(state->runtime);
    state->enable_error = errno;
    return 0U;
}

static DWORD WINAPI config_public_enable_thread(void *opaque) {
    config_state_t *state = (config_state_t *)opaque;

    state->enable_result =
        llam_runtime_enable_experimental_windows_async_preempt_ex(
            state->runtime, &state->public_opts, sizeof(state->public_opts));
    state->enable_error = errno;
    return 0U;
}

static DWORD WINAPI config_spawn_thread(void *opaque) {
    config_state_t *state = (config_state_t *)opaque;

    state->spawn_handle = llam_runtime_spawn_ex(state->runtime,
                                                config_never_run, NULL,
                                                NULL, 0U);
    state->spawn_error = errno;
    return 0U;
}

static DWORD WINAPI config_destroy_thread(void *opaque) {
    config_state_t *state = (config_state_t *)opaque;

    atomic_store_explicit(&state->destroy_started, 1U, memory_order_release);
    llam_runtime_destroy(state->runtime);
    atomic_store_explicit(&state->destroy_done, 1U, memory_order_release);
    return 0U;
}

static void config_hold_task(void *opaque) {
    config_state_t *state = (config_state_t *)opaque;

    atomic_store_explicit(&state->observed, 1U, memory_order_release);
    while (atomic_load_explicit(&state->release,
                                memory_order_acquire) == 0U) {
        if (llam_sleep_ns(1000000U) != 0) {
            abort();
        }
    }
}

static void config_leaf_task(void *opaque) {
    config_state_t *state = (config_state_t *)opaque;

    (void)atomic_fetch_add_explicit(&state->leaf_entered, 1U,
                                    memory_order_release);
    win_llam_async_leaf();
    (void)atomic_fetch_add_explicit(&state->leaf_done, 1U,
                                    memory_order_release);
}

static DWORD WINAPI config_run_thread(void *opaque) {
    config_state_t *state = (config_state_t *)opaque;

    state->run_thread_id = GetCurrentThreadId();
    state->run_result = llam_runtime_run_handle(state->runtime);
    return 0U;
}

static DWORD WINAPI config_delayed_monitor(void *opaque) {
    config_state_t *state = (config_state_t *)opaque;
    ULONGLONG deadline = GetTickCount64() + 6000U;

    while (GetTickCount64() < deadline) {
        llam_windows_async_stats_t stats = {0};

        if (atomic_load_explicit(&state->leaf_entered,
                                 memory_order_acquire) >= 2U &&
            llam_windows_async_lifetime_stats(state->runtime, &stats) &&
            stats.captures != 0U && stats.restores != 0U &&
            stats.direct_witnesses != 0U) {
            atomic_store_explicit(&g_win_llam_async_release, 1U,
                                  memory_order_release);
            return 0U;
        }
        Sleep(1U);
    }
    config_fail("delayed controller automatic restore/witness");
    return 1U;
}

static void config_foreign_task(void *opaque) {
    config_state_t *state = (config_state_t *)opaque;

    errno = 0;
    state->foreign_spawn_handle = llam_runtime_spawn_ex(
        state->runtime, config_never_run, NULL, NULL, 0U);
    state->foreign_spawn_error = errno;
}

static void config_managed_public_enable_task(void *opaque) {
    config_state_t *state = (config_state_t *)opaque;

    errno = 0;
    state->managed_enable_result =
        llam_runtime_enable_experimental_windows_async_preempt_ex(
            state->runtime, &state->public_opts, sizeof(state->public_opts));
    state->managed_enable_error = errno;
}

static void config_create_impl(config_state_t *state, uint32_t preempt_mode,
                               bool register_range) {
    llam_runtime_opts_t options;

    memset(state, 0, sizeof(*state));
    atomic_init(&state->observed, 0U);
    atomic_init(&state->release, 0U);
    atomic_init(&state->destroy_started, 0U);
    atomic_init(&state->destroy_done, 0U);
    atomic_init(&state->leaf_entered, 0U);
    atomic_init(&state->leaf_done, 0U);
    if (llam_runtime_opts_init(&options, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        config_fail("options");
    }
    options.deterministic = 1U;
    options.preempt_mode = preempt_mode;
    options.preempt_quantum_ns = 1000000U;
    if (llam_runtime_create(&options, LLAM_RUNTIME_OPTS_CURRENT_SIZE,
                            &state->runtime) != 0 || state->runtime == NULL) {
        config_fail("create");
    }
    if (register_range &&
        llam_windows_async_lifetime_register_safe_range(
            state->runtime, (uintptr_t)win_llam_async_leaf_begin,
            (uintptr_t)win_llam_async_leaf_end) != 0) {
        config_fail("register safe range");
    }
}

static void config_create_mode(config_state_t *state, uint32_t preempt_mode) {
    config_create_impl(state, preempt_mode, true);
}

static void config_create_public(config_state_t *state) {
    config_create_impl(state, LLAM_PREEMPT_STRICT, false);
    state->public_range.begin_pc =
        (uint64_t)(uintptr_t)win_llam_async_leaf_begin;
    state->public_range.end_pc =
        (uint64_t)(uintptr_t)win_llam_async_leaf_end;
    state->public_opts.flags =
        LLAM_WINDOWS_ASYNC_PREEMPT_ACK_PROCESS_FAILFAST;
    state->public_opts.ranges = &state->public_range;
    state->public_opts.range_count = 1U;
}

static void config_create(config_state_t *state) {
    config_create_mode(state, LLAM_PREEMPT_OFF);
}

static void config_check_spawn_capsule(config_state_t *state, bool expected) {
    llam_task_t *task;

    task = llam_task_resolve_public_handle(state->spawn_handle);
    if (task == NULL) {
        config_fail("resolve spawned task");
    }
    if ((task->async_preempt_task != NULL) != expected) {
        config_fail("spawn capsule state");
    }
    llam_task_end_public_op(task);
}

static void config_public_check_empty(config_state_t *state) {
    if (state->runtime->async_preempt_ranges != NULL ||
        state->runtime->async_preempt_range_count != 0U ||
        state->runtime->async_preempt_control != NULL ||
        atomic_load_explicit(&state->runtime->async_preempt_enabled,
                             memory_order_acquire) != 0U) {
        config_fail("public refusal left partial configuration");
    }
}

static void config_spawn_first(void) {
    config_state_t state;
    HANDLE spawner;

    config_create(&state);
    llam_windows_async_test_host_spawn_gate(state.runtime,
                                             &state.observed, &state.release);
    spawner = CreateThread(NULL, 0U, config_spawn_thread, &state, 0U, NULL);
    if (spawner == NULL) {
        config_fail("create spawn thread");
    }
    config_wait(&state.observed, "spawn reservation gate");
    errno = 0;
    if (llam_windows_async_lifetime_enable(state.runtime) != -1 ||
        errno != EBUSY || state.runtime->async_preempt_control != NULL ||
        atomic_load_explicit(&state.runtime->async_preempt_enabled,
                             memory_order_acquire) != 0U) {
        config_fail("enable crossed admitted spawn");
    }
    atomic_store_explicit(&state.release, 1U, memory_order_release);
    config_join(spawner, "spawn thread");
    llam_windows_async_test_host_spawn_gate(NULL, NULL, NULL);
    if (state.spawn_handle == NULL) {
        errno = state.spawn_error;
        config_fail("spawn result");
    }
    config_check_spawn_capsule(&state, false);
    errno = 0;
    if (llam_windows_async_lifetime_enable(state.runtime) != -1 ||
        errno != EBUSY) {
        config_fail("enable accepted existing task");
    }
    llam_runtime_destroy(state.runtime);
}

static void config_enable_first(void) {
    config_state_t state;
    HANDLE enabler;
    HANDLE spawner;

    config_create(&state);
    llam_windows_async_lifetime_test_enable_gate(state.runtime,
                                                 &state.observed,
                                                 &state.release);
    enabler = CreateThread(NULL, 0U, config_enable_thread, &state, 0U, NULL);
    if (enabler == NULL) {
        config_fail("create enable thread");
    }
    config_wait(&state.observed, "enable commit gate");
    if (!atomic_load_explicit(&state.runtime->exec_started,
                              memory_order_acquire) ||
        atomic_load_explicit(&state.runtime->async_preempt_enabled,
                             memory_order_acquire) != 0U) {
        config_fail("configuration admission state");
    }
    errno = 0;
    if (llam_runtime_run_handle(state.runtime) != -1 || errno != EINVAL) {
        config_fail("run crossed configuration");
    }
    spawner = CreateThread(NULL, 0U, config_spawn_thread, &state, 0U, NULL);
    if (spawner == NULL) {
        config_fail("create spawn thread");
    }
    atomic_store_explicit(&state.release, 1U, memory_order_release);
    config_join(enabler, "enable thread");
    config_join(spawner, "spawn thread");
    llam_windows_async_lifetime_test_enable_gate(NULL, NULL, NULL);
    if (state.enable_result != 0 || state.spawn_handle == NULL) {
        errno = state.enable_result != 0 ? state.enable_error : state.spawn_error;
        config_fail("enable-then-spawn result");
    }
    config_check_spawn_capsule(&state, true);
    if (llam_windows_async_lifetime_charged_bytes(state.runtime) == 0U) {
        config_fail("capsule accounting");
    }
    llam_runtime_destroy(state.runtime);
}

static void config_enable_destroy(void) {
    config_state_t state;
    HANDLE enabler;
    HANDLE destroyer;
    ULONGLONG deadline;

    config_create(&state);
    llam_windows_async_lifetime_test_enable_gate(state.runtime,
                                                 &state.observed,
                                                 &state.release);
    enabler = CreateThread(NULL, 0U, config_enable_thread, &state, 0U, NULL);
    if (enabler == NULL) {
        config_fail("create enable thread");
    }
    config_wait(&state.observed, "enable/destroy gate");
    destroyer = CreateThread(NULL, 0U, config_destroy_thread, &state,
                             0U, NULL);
    if (destroyer == NULL) {
        config_fail("create destroy thread");
    }
    deadline = GetTickCount64() + 4000U;
    while (!atomic_load_explicit(&state.runtime->destroy_claimed,
                                  memory_order_acquire) &&
           GetTickCount64() < deadline) {
        Sleep(1U);
    }
    if (!atomic_load_explicit(&state.runtime->destroy_claimed,
                              memory_order_acquire) ||
        atomic_load_explicit(&state.destroy_done,
                             memory_order_acquire) != 0U) {
        config_fail("destroy did not wait for configuration pin");
    }
    atomic_store_explicit(&state.release, 1U, memory_order_release);
    config_join(enabler, "enable/destroy enabler");
    config_join(destroyer, "enable/destroy destroyer");
    llam_windows_async_lifetime_test_enable_gate(NULL, NULL, NULL);
    if (state.enable_result != -1 || state.enable_error != EBUSY ||
        atomic_load_explicit(&state.destroy_done,
                             memory_order_acquire) != 1U) {
        errno = state.enable_error;
        config_fail("destroy admission outcome");
    }
    /* Registry lookup rejects a retired pointer without dereferencing it. */
    errno = 0;
    if (llam_windows_async_lifetime_enable(state.runtime) != -1 ||
        errno != EINVAL) {
        config_fail("retired runtime enable");
    }
    errno = 0;
    if (llam_windows_async_lifetime_register_safe_range(
            state.runtime, (uintptr_t)win_llam_async_leaf_begin,
            (uintptr_t)win_llam_async_leaf_end) != -1 || errno != EINVAL) {
        config_fail("retired runtime register");
    }
}

static void config_run_first(void) {
    config_state_t state;
    llam_task_t *held_task;
    HANDLE runner;

    config_create(&state);
    held_task = llam_runtime_spawn_ex(state.runtime, config_hold_task,
                                      &state, NULL, 0U);
    if (held_task == NULL || llam_detach(held_task) != 0) {
        config_fail("run-first task spawn/detach");
    }
    runner = CreateThread(NULL, 0U, config_run_thread, &state, 0U, NULL);
    if (runner == NULL) {
        config_fail("create run thread");
    }
    config_wait(&state.observed, "active run task");
    errno = 0;
    if (llam_windows_async_lifetime_enable(state.runtime) != -1 ||
        errno != EBUSY) {
        config_fail("enable crossed active run");
    }
    errno = 0;
    if (llam_windows_async_lifetime_register_safe_range(
            state.runtime, (uintptr_t)win_llam_async_leaf_begin,
            (uintptr_t)win_llam_async_leaf_end) != -1 || errno != EBUSY) {
        config_fail("register crossed active run");
    }
    atomic_store_explicit(&state.release, 1U, memory_order_release);
    config_join(runner, "run-first runner");
    if (state.run_result != 0 ||
        llam_windows_async_lifetime_enable(state.runtime) != 0) {
        config_fail("enable after prior run drained");
    }
    state.spawn_handle = llam_runtime_spawn_ex(state.runtime,
                                               config_never_run, NULL,
                                               NULL, 0U);
    if (state.spawn_handle == NULL) {
        config_fail("post-enable spawn");
    }
    config_check_spawn_capsule(&state, true);
    llam_runtime_destroy(state.runtime);
}

static void config_foreign_trylock(void) {
    config_state_t state;
    llam_runtime_opts_t options;
    llam_runtime_t *source_runtime = NULL;
    llam_task_t *source_task;
    HANDLE enabler;

    config_create(&state);
    if (llam_runtime_opts_init(&options, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        config_fail("foreign runtime options");
    }
    options.deterministic = 1U;
    options.preempt_mode = LLAM_PREEMPT_OFF;
    if (llam_runtime_create(&options, LLAM_RUNTIME_OPTS_CURRENT_SIZE,
                            &source_runtime) != 0) {
        config_fail("foreign runtime create");
    }
    source_task = llam_runtime_spawn_ex(source_runtime, config_foreign_task,
                                        &state, NULL, 0U);
    if (source_task == NULL || llam_detach(source_task) != 0) {
        config_fail("foreign source task");
    }
    llam_windows_async_lifetime_test_enable_gate(state.runtime,
                                                 &state.observed,
                                                 &state.release);
    enabler = CreateThread(NULL, 0U, config_enable_thread, &state, 0U, NULL);
    if (enabler == NULL) {
        config_fail("foreign enable thread");
    }
    config_wait(&state.observed, "foreign enable gate");
    if (llam_runtime_run_handle(source_runtime) != 0 ||
        state.foreign_spawn_handle != NULL ||
        state.foreign_spawn_error != EBUSY ||
        llam_runtime_has_live_tasks(state.runtime)) {
        config_fail("foreign managed spawn fell through trylock refusal");
    }
    atomic_store_explicit(&state.release, 1U, memory_order_release);
    config_join(enabler, "foreign enable thread");
    llam_windows_async_lifetime_test_enable_gate(NULL, NULL, NULL);
    if (state.enable_result != 0) {
        errno = state.enable_error;
        config_fail("foreign target enable");
    }
    state.spawn_handle = llam_runtime_spawn_ex(state.runtime,
                                               config_never_run, NULL,
                                               NULL, 0U);
    if (state.spawn_handle == NULL) {
        config_fail("foreign target post-enable spawn");
    }
    config_check_spawn_capsule(&state, true);
    llam_runtime_destroy(state.runtime);
    llam_runtime_destroy(source_runtime);
}

static void config_delayed_controller(void) {
    config_state_t state;
    llam_windows_async_stats_t stats = {0};
    atomic_uint ctrl_observed = ATOMIC_VAR_INIT(0U);
    atomic_uint ctrl_release = ATOMIC_VAR_INIT(0U);
    atomic_uint ctrl_ticks = ATOMIC_VAR_INIT(0U);
    llam_task_t *task;
    HANDLE first_runner;
    HANDLE monitor;
    int second_run_result;

    llam_windows_async_test_ctrl_entry_gate(&ctrl_observed, &ctrl_release,
                                             &ctrl_ticks);
    config_create_mode(&state, LLAM_PREEMPT_STRICT);
    config_wait(&ctrl_observed, "controller entry gate");
    if (!state.runtime->ctrl_thread_started ||
        !atomic_load_explicit(&state.runtime->initialized,
                              memory_order_acquire) ||
        atomic_load_explicit(&ctrl_ticks, memory_order_acquire) != 0U ||
        llam_windows_async_lifetime_enable(state.runtime) != 0) {
        config_fail("enable before delayed controller first tick");
    }
    task = llam_runtime_spawn_ex(state.runtime, config_leaf_task,
                                 &state, NULL, 0U);
    if (task == NULL || llam_detach(task) != 0) {
        config_fail("delayed first task");
    }
    first_runner = CreateThread(NULL, 0U, config_run_thread,
                                &state, 0U, NULL);
    if (first_runner == NULL) {
        config_fail("delayed first runner");
    }
    config_wait(&state.leaf_entered, "first G before controller tick");
    if (!llam_windows_async_lifetime_stats(state.runtime, &stats) ||
        stats.capture_attempts != 0U || stats.captures != 0U ||
        stats.restores != 0U || stats.binding_ops != 0U ||
        stats.captured_bank_active != 0U ||
        stats.pending_restore_active != 0U ||
        stats.execution_debt != 1U ||
        atomic_load_explicit(&ctrl_ticks, memory_order_acquire) != 0U) {
        config_fail("controller admitted capture before entry");
    }
    atomic_store_explicit(&g_win_llam_async_release, 1U,
                          memory_order_release);
    config_join(first_runner, "first run without controller service");
    if (state.run_result != 0 || state.run_thread_id == GetCurrentThreadId() ||
        atomic_load_explicit(&state.leaf_done,
                             memory_order_acquire) != 1U ||
        !llam_windows_async_lifetime_stats(state.runtime, &stats) ||
        stats.capture_attempts != 0U || stats.captures != 0U ||
        stats.binding_ops != 0U || stats.captured_bank_active != 0U ||
        stats.pending_restore_active != 0U ||
        stats.execution_debt != 0U || stats.retire_count != 0U ||
        stats.finalizer_count != 0U ||
        llam_windows_async_lifetime_charged_bytes(state.runtime) != 0U ||
        atomic_load_explicit(&state.runtime->exec_started,
                             memory_order_acquire)) {
        config_fail("first run did not drain while controller delayed");
    }

    atomic_store_explicit(&ctrl_release, 1U, memory_order_release);
    config_wait_at_least(&ctrl_ticks, 3U, "delayed controller idle ticks");
    if (!llam_windows_async_lifetime_stats(state.runtime, &stats) ||
        stats.capture_attempts != 0U || stats.captures != 0U ||
        stats.restores != 0U || stats.binding_ops != 0U ||
        stats.captured_bank_active != 0U ||
        stats.pending_restore_active != 0U) {
        config_fail("late controller used retired binding");
    }

    atomic_store_explicit(&g_win_llam_async_release, 0U,
                          memory_order_release);
    task = llam_runtime_spawn_ex(state.runtime, config_leaf_task,
                                 &state, NULL, 0U);
    if (task == NULL || llam_detach(task) != 0) {
        config_fail("delayed second task");
    }
    monitor = CreateThread(NULL, 0U, config_delayed_monitor,
                           &state, 0U, NULL);
    if (monitor == NULL) {
        config_fail("delayed second-run monitor");
    }
    second_run_result = llam_runtime_run_handle(state.runtime);
    config_join(monitor, "delayed second-run monitor");
    if (second_run_result != 0 ||
        atomic_load_explicit(&state.leaf_done,
                             memory_order_acquire) != 2U ||
        !llam_windows_async_lifetime_stats(state.runtime, &stats) ||
        stats.captures == 0U || stats.restores == 0U ||
        stats.direct_witnesses == 0U || stats.binding_ops != 0U ||
        stats.captured_bank_active != 0U ||
        stats.pending_restore_active != 0U ||
        stats.execution_debt != 0U || stats.retire_count != 0U ||
        stats.finalizer_count != 0U ||
        llam_windows_async_lifetime_charged_bytes(state.runtime) != 0U) {
        config_fail("fresh run after delayed controller did not drain");
    }
    llam_windows_async_test_ctrl_entry_gate(NULL, NULL, NULL);
    llam_runtime_destroy(state.runtime);
}

static void config_public_spawn_first(void) {
    config_state_t state;
    HANDLE spawner;

    config_create_public(&state);
    llam_windows_async_test_host_spawn_gate(state.runtime,
                                             &state.observed, &state.release);
    spawner = CreateThread(NULL, 0U, config_spawn_thread, &state, 0U, NULL);
    if (spawner == NULL) {
        config_fail("public spawn-first thread");
    }
    config_wait(&state.observed, "public spawn reservation gate");
    errno = 0;
    if (llam_runtime_enable_experimental_windows_async_preempt_ex(
            state.runtime, &state.public_opts,
            sizeof(state.public_opts)) != -1 || errno != EBUSY) {
        config_fail("public enable crossed admitted spawn");
    }
    config_public_check_empty(&state);
    atomic_store_explicit(&state.release, 1U, memory_order_release);
    config_join(spawner, "public spawn-first spawner");
    llam_windows_async_test_host_spawn_gate(NULL, NULL, NULL);
    if (state.spawn_handle == NULL) {
        errno = state.spawn_error;
        config_fail("public spawn-first result");
    }
    config_check_spawn_capsule(&state, false);
    llam_runtime_destroy(state.runtime);
}

static void config_public_enable_first(void) {
    config_state_t state;
    HANDLE enabler;
    HANDLE spawner;

    config_create_public(&state);
    llam_windows_async_lifetime_test_enable_gate(state.runtime,
                                                 &state.observed,
                                                 &state.release);
    enabler = CreateThread(NULL, 0U, config_public_enable_thread,
                           &state, 0U, NULL);
    if (enabler == NULL) {
        config_fail("public enable-first thread");
    }
    config_wait(&state.observed, "public enable commit gate");
    errno = 0;
    if (llam_runtime_run_handle(state.runtime) != -1 || errno != EINVAL ||
        atomic_load_explicit(&state.runtime->async_preempt_enabled,
                             memory_order_acquire) != 0U) {
        config_fail("public run crossed incomplete enable");
    }
    spawner = CreateThread(NULL, 0U, config_spawn_thread,
                           &state, 0U, NULL);
    if (spawner == NULL) {
        config_fail("public enable-first spawner");
    }
    atomic_store_explicit(&state.release, 1U, memory_order_release);
    config_join(enabler, "public enable-first enabler");
    config_join(spawner, "public enable-first spawner");
    llam_windows_async_lifetime_test_enable_gate(NULL, NULL, NULL);
    if (state.enable_result != 0 || state.spawn_handle == NULL) {
        errno = state.enable_result != 0 ? state.enable_error : state.spawn_error;
        config_fail("public enable-first result");
    }
    config_check_spawn_capsule(&state, true);
    llam_runtime_destroy(state.runtime);
}

static void config_public_enable_destroy(void) {
    config_state_t state;
    HANDLE enabler;
    HANDLE destroyer;
    ULONGLONG deadline;

    config_create_public(&state);
    llam_windows_async_lifetime_test_enable_gate(state.runtime,
                                                 &state.observed,
                                                 &state.release);
    enabler = CreateThread(NULL, 0U, config_public_enable_thread,
                           &state, 0U, NULL);
    if (enabler == NULL) {
        config_fail("public enable/destroy enabler");
    }
    config_wait(&state.observed, "public enable/destroy gate");
    destroyer = CreateThread(NULL, 0U, config_destroy_thread,
                             &state, 0U, NULL);
    if (destroyer == NULL) {
        config_fail("public enable/destroy destroyer");
    }
    deadline = GetTickCount64() + 4000U;
    while (!atomic_load_explicit(&state.runtime->destroy_claimed,
                                  memory_order_acquire) &&
           GetTickCount64() < deadline) {
        Sleep(1U);
    }
    if (!atomic_load_explicit(&state.runtime->destroy_claimed,
                              memory_order_acquire) ||
        atomic_load_explicit(&state.destroy_done,
                             memory_order_acquire) != 0U) {
        config_fail("public destroy did not wait for enable pin");
    }
    atomic_store_explicit(&state.release, 1U, memory_order_release);
    config_join(enabler, "public enable/destroy enabler completion");
    config_join(destroyer, "public enable/destroy destroyer completion");
    llam_windows_async_lifetime_test_enable_gate(NULL, NULL, NULL);
    if (state.enable_result != -1 || state.enable_error != EBUSY) {
        errno = state.enable_error;
        config_fail("public enable/destroy result");
    }
}

static void config_public_managed_refusal(void) {
    config_state_t state;
    llam_runtime_opts_t options;
    llam_runtime_t *source = NULL;
    llam_task_t *task;

    config_create_public(&state);
    if (llam_runtime_opts_init(&options, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        config_fail("public managed source options");
    }
    options.deterministic = 1U;
    if (llam_runtime_create(&options, LLAM_RUNTIME_OPTS_CURRENT_SIZE,
                            &source) != 0) {
        config_fail("public managed source create");
    }
    task = llam_runtime_spawn_ex(source, config_managed_public_enable_task,
                                 &state, NULL, 0U);
    if (task == NULL || llam_detach(task) != 0 ||
        llam_runtime_run_handle(source) != 0 ||
        state.managed_enable_result != -1 ||
        state.managed_enable_error != EBUSY) {
        config_fail("managed caller entered public configuration gate");
    }
    config_public_check_empty(&state);
    if (llam_runtime_enable_experimental_windows_async_preempt_ex(
            state.runtime, &state.public_opts,
            sizeof(state.public_opts)) != 0) {
        config_fail("host retry after managed refusal");
    }
    llam_runtime_destroy(source);
    llam_runtime_destroy(state.runtime);
}

static void config_start_failure(void) {
    llam_runtime_opts_t options;
    llam_runtime_t *failed_runtime = (llam_runtime_t *)(uintptr_t)1U;
    config_state_t state;

    if (llam_runtime_opts_init(&options, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        config_fail("failure options");
    }
    options.deterministic = 1U;
    options.preempt_mode = LLAM_PREEMPT_OFF;
    llam_runtime_test_fail_ctrl_thread_start();
    if (llam_runtime_create(&options, LLAM_RUNTIME_OPTS_CURRENT_SIZE,
                            &failed_runtime) != -1 || failed_runtime != NULL) {
        config_fail("injected control startup failure");
    }
    /* The failed init must tear down every partly initialized lock and worker;
     * a fresh runtime must still be able to register and enable normally. */
    config_create(&state);
    if (llam_windows_async_lifetime_enable(state.runtime) != 0) {
        config_fail("recreate after control startup failure");
    }
    llam_runtime_destroy(state.runtime);
}

int main(void) {
    char mode[32] = {0};

    if (GetEnvironmentVariableA("LLAM_ASYNC_CONFIG_MODE", mode,
                                sizeof(mode)) == 0U) {
        return 1;
    }
    if (strcmp(mode, "spawn-first") == 0) {
        config_spawn_first();
    } else if (strcmp(mode, "enable-first") == 0) {
        config_enable_first();
    } else if (strcmp(mode, "enable-destroy") == 0) {
        config_enable_destroy();
    } else if (strcmp(mode, "run-first") == 0) {
        config_run_first();
    } else if (strcmp(mode, "foreign-trylock") == 0) {
        config_foreign_trylock();
    } else if (strcmp(mode, "delayed-controller") == 0) {
        config_delayed_controller();
    } else if (strcmp(mode, "public-spawn-first") == 0) {
        config_public_spawn_first();
    } else if (strcmp(mode, "public-enable-first") == 0) {
        config_public_enable_first();
    } else if (strcmp(mode, "public-enable-destroy") == 0) {
        config_public_enable_destroy();
    } else if (strcmp(mode, "public-managed-refusal") == 0) {
        config_public_managed_refusal();
    } else if (strcmp(mode, "start-failure") == 0) {
        config_start_failure();
    } else {
        return 1;
    }
    printf("llam-async-config: %s passed\n", mode);
    return 0;
}
