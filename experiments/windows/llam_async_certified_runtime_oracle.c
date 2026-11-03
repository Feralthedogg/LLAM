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

/* Bounded R8 compiler-generated LLPM -> public enable -> automatic G oracle. */
#include "runtime_internal.h"
#include "windows_async_lifetime.h"

#include <stdio.h>
#include <string.h>

#define R8_ITERATIONS UINT64_C(120000000)
#define R8_LOOP_CODE_BYTES 0x45U
#define R8_SCALAR_CODE_BYTES 8U

typedef unsigned long long (*r8_loop_fn_t)(unsigned long long);

typedef struct r8_state {
    llam_runtime_t *runtime;
    r8_loop_fn_t loop;
    uint64_t iterations;
    uint64_t expected;
    uint64_t actual;
    uintptr_t compute_key;
    uintptr_t peer_key;
    atomic_uint compute_done;
    atomic_uint peer_progress;
    atomic_uint gate_observed;
    atomic_uint gate_release;
    atomic_uint adversary_done;
    atomic_uint destroy_done;
    HANDLE destroy_thread;
    bool destroy_root;
    bool capture_tail;
} r8_state_t;

static void r8_fail(const char *where) {
    fprintf(stderr, "llam-r8-runtime: %s failed (errno=%d)\n", where, errno);
    ExitProcess(1U);
}

static uintptr_t r8_symbol(HMODULE module, const char *name) {
    FARPROC procedure = GetProcAddress(module, name);
    uintptr_t address = 0U;

    _Static_assert(sizeof(procedure) == sizeof(address), "PE pointer size");
    if (procedure != NULL) {
        memcpy(&address, &procedure, sizeof(address));
    }
    if (address == 0U) {
        r8_fail("GetProcAddress");
    }
    return address;
}

static r8_loop_fn_t r8_loop_function(uintptr_t address) {
    r8_loop_fn_t function = NULL;

    _Static_assert(sizeof(function) == sizeof(address), "function pointer size");
    memcpy(&function, &address, sizeof(function));
    return function;
}

static bool r8_is_mapped(HMODULE module, uintptr_t address) {
    HMODULE observed = NULL;

    return GetModuleHandleExA(
               GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
               (LPCSTR)(const void *)address, &observed) &&
           observed == module;
}

/* Separate, unregistered reference; every shift/add is defined unsigned. */
static uint64_t r8_reference(uint64_t iterations) {
    uint64_t value = 1U;
    uint64_t index;

    for (index = 0U; index < iterations; ++index) {
        value = ((value << 7U) ^ (value >> 3U)) + index;
    }
    return value;
}

static void r8_compute_task(void *opaque) {
    r8_state_t *state = (r8_state_t *)opaque;

    state->actual = state->loop(state->iterations);
    if (atomic_load_explicit(&state->runtime->async_preempt_enabled,
                             memory_order_acquire) != 0U) {
        llam_windows_async_lifetime_test_note_task(
            state->runtime, LLAM_WINDOWS_ASYNC_TEST_RETURN, 0U);
    }
    atomic_store_explicit(&state->compute_done, 1U, memory_order_release);
}

static void r8_peer_task(void *opaque) {
    r8_state_t *state = (r8_state_t *)opaque;
    size_t last_capture_index = 0U;

    while (atomic_load_explicit(&state->compute_done,
                                memory_order_acquire) == 0U) {
        llam_windows_async_test_event_t
            events[LLAM_WINDOWS_ASYNC_TEST_TRACE_CAPACITY];
        size_t count = llam_windows_async_lifetime_test_trace(
            state->runtime, events, LLAM_WINDOWS_ASYNC_TEST_TRACE_CAPACITY);
        size_t index;
        uint64_t latest_generation = 0U;

        for (index = last_capture_index; index < count; ++index) {
            if (events[index].kind == LLAM_WINDOWS_ASYNC_TEST_CAPTURE &&
                events[index].task_key == state->compute_key) {
                latest_generation = events[index].bank_generation;
            }
        }
        last_capture_index = count;
        if (latest_generation != 0U) {
            /* This note is diagnostic only. It does not wake or schedule Gs. */
            llam_windows_async_lifetime_test_note_task(
                state->runtime, LLAM_WINDOWS_ASYNC_TEST_PEER,
                latest_generation);
            atomic_fetch_add_explicit(&state->peer_progress, 1U,
                                      memory_order_relaxed);
        }
        llam_yield();
    }
}

static DWORD WINAPI r8_deadline(void *opaque) {
    HANDLE done = (HANDLE)opaque;

    if (WaitForSingleObject(done, 6000U) == WAIT_TIMEOUT) {
        fputs("llam-r8-runtime: six-second subprocess deadline\n", stderr);
        ExitProcess(124U);
    }
    return 0U;
}

static void r8_check_trace(const r8_state_t *state,
                           uintptr_t loop_begin,
                           uintptr_t loop_end) {
    llam_windows_async_test_event_t
        events[LLAM_WINDOWS_ASYNC_TEST_TRACE_CAPACITY];
    size_t count = llam_windows_async_lifetime_test_trace(
        state->runtime, events, LLAM_WINDOWS_ASYNC_TEST_TRACE_CAPACITY);
    size_t capture[2] = {SIZE_MAX, SIZE_MAX};
    size_t peer[2] = {SIZE_MAX, SIZE_MAX};
    size_t restore[2] = {SIZE_MAX, SIZE_MAX};
    size_t witness[2] = {SIZE_MAX, SIZE_MAX};
    size_t returned = SIZE_MAX;
    size_t found = 0U;
    size_t index;
    size_t cycle;

    if (llam_windows_async_lifetime_test_trace_overflow(state->runtime)) {
        r8_fail("test trace overflow");
    }
    for (index = 0U; index < count; ++index) {
        if (events[index].kind == LLAM_WINDOWS_ASYNC_TEST_CAPTURE &&
            events[index].task_key == state->compute_key &&
            events[index].captured_pc >= loop_begin &&
            events[index].captured_pc < loop_end && found < 2U) {
            capture[found++] = index;
        }
        if (events[index].kind == LLAM_WINDOWS_ASYNC_TEST_RETURN &&
            events[index].task_key == state->compute_key) {
            returned = index;
        }
    }
    if (found != 2U || returned == SIZE_MAX ||
        capture[0] >= capture[1] || capture[1] >= returned ||
        events[capture[0]].bank_generation ==
            events[capture[1]].bank_generation) {
        r8_fail("two captures of one finite invocation");
    }
    for (cycle = 0U; cycle < 2U; ++cycle) {
        uint64_t generation = events[capture[cycle]].bank_generation;

        for (index = capture[cycle] + 1U; index < count; ++index) {
            if (events[index].bank_generation != generation) {
                continue;
            }
            if (events[index].kind == LLAM_WINDOWS_ASYNC_TEST_PEER &&
                events[index].task_key == state->peer_key &&
                peer[cycle] == SIZE_MAX) {
                peer[cycle] = index;
            } else if (events[index].kind ==
                           LLAM_WINDOWS_ASYNC_TEST_RESTORE &&
                       events[index].task_key == state->compute_key &&
                       restore[cycle] == SIZE_MAX) {
                restore[cycle] = index;
            } else if (events[index].kind ==
                           LLAM_WINDOWS_ASYNC_TEST_WITNESS &&
                       events[index].task_key == state->compute_key &&
                       witness[cycle] == SIZE_MAX) {
                witness[cycle] = index;
            }
        }
        if (peer[cycle] == SIZE_MAX || restore[cycle] == SIZE_MAX ||
            witness[cycle] == SIZE_MAX ||
            !(capture[cycle] < peer[cycle] &&
              peer[cycle] < restore[cycle] &&
              restore[cycle] < witness[cycle] &&
              witness[cycle] < returned) ||
            events[capture[cycle]].bank_key == 0U ||
            events[restore[cycle]].bank_key !=
                events[capture[cycle]].bank_key ||
            events[witness[cycle]].bank_key !=
                events[capture[cycle]].bank_key ||
            events[restore[cycle]].capture_task_run !=
                events[capture[cycle]].capture_task_run ||
            events[witness[cycle]].capture_task_run !=
                events[capture[cycle]].capture_task_run ||
            events[restore[cycle]].capture_worker_run !=
                events[capture[cycle]].capture_worker_run ||
            events[witness[cycle]].capture_worker_run !=
                events[capture[cycle]].capture_worker_run ||
            events[restore[cycle]].restore_task_run <=
                events[capture[cycle]].capture_task_run ||
            events[restore[cycle]].restore_worker_run <=
                events[capture[cycle]].capture_worker_run ||
            events[witness[cycle]].restore_task_run !=
                events[restore[cycle]].restore_task_run ||
            events[witness[cycle]].restore_worker_run !=
                events[restore[cycle]].restore_worker_run) {
            r8_fail("capture/peer/restore/direct-witness chain");
        }
    }
    if (witness[0] >= capture[1] ||
        atomic_load_explicit(&state->peer_progress,
                             memory_order_acquire) < 2U) {
        r8_fail("repeat chain and peer progress");
    }
    printf("llam-r8-runtime: chain1=%zu/%zu/%zu/%zu chain2=%zu/%zu/%zu/%zu banks=%llu,%llu rip=%p,%p expected=%llu actual=%llu\n",
           capture[0], peer[0], restore[0], witness[0],
           capture[1], peer[1], restore[1], witness[1],
           (unsigned long long)events[capture[0]].bank_generation,
           (unsigned long long)events[capture[1]].bank_generation,
           (void *)events[capture[0]].captured_pc,
           (void *)events[capture[1]].captured_pc,
           (unsigned long long)state->expected,
           (unsigned long long)state->actual);
}

static void r8_join(llam_task_t *handle) {
    if (handle == NULL || llam_join(handle) != 0) {
        r8_fail("join");
    }
}

static DWORD WINAPI r8_destroy_root(void *opaque) {
    r8_state_t *state = opaque;
    llam_runtime_destroy(state->runtime);
    atomic_store_explicit(&state->destroy_done, 1U, memory_order_release);
    return 0U;
}

static DWORD WINAPI r8_root_observer(void *opaque) {
    r8_state_t *state = opaque;
    uint64_t deadline = GetTickCount64() + 5000U;
    while (GetTickCount64() < deadline) {
        if (atomic_load_explicit(&state->gate_observed, memory_order_acquire) != 0U) {
            llam_shard_t *origin = &state->runtime->shards[0];
            llam_task_t *task = (llam_task_t *)state->compute_key;
            llam_windows_async_stats_t stats;
            bool matched = false;
            pthread_mutex_lock(&origin->opaque_lock);
            pthread_mutex_lock(&origin->lock);
            /* M or capture-controller pause retains the real continuation;
             * this diagnostic borrows it, never adds a lifetime reference. */
            llam_windows_async_capsule_t *capsule = task->async_preempt_task;
            if (capsule == NULL) r8_fail("root capsule disappeared");
            for (unsigned i = 0U; i < 2U; ++i) {
                llam_windows_async_bank_t *bank = &capsule->banks[i];
                unsigned phase = atomic_load_explicit(&bank->phase, memory_order_acquire);
                if (state->capture_tail) {
                    matched |= phase == LLAM_WINDOWS_ASYNC_BANK_GATE && bank->external_ops == 1U &&
                        bank->restore_task_run > bank->capture_task_run &&
                        atomic_load_explicit(&bank->arrival_complete, memory_order_acquire) == 1U;
                } else {
                    matched |= phase == LLAM_WINDOWS_ASYNC_BANK_READY && bank->external_ops == 0U;
                }
            }
            pthread_mutex_unlock(&origin->lock);
            pthread_mutex_unlock(&origin->opaque_lock);
            if (matched && llam_windows_async_lifetime_stats(state->runtime, &stats) &&
                stats.execution_debt == 1U && stats.captured_bank_active == 0U &&
                stats.binding_ops == (state->capture_tail ? 1U : 0U) &&
                stats.pending_restore_active == (state->capture_tail ? 1U : 0U)) {
                printf("llam-r8-root: %s ops=%llu pending=%llu debt=%llu before release\n",
                    state->capture_tail ? "late capture at fresh GATE" : "READY without controller operation",
                    (unsigned long long)stats.binding_ops,
                    (unsigned long long)stats.pending_restore_active,
                    (unsigned long long)stats.execution_debt);
                if (state->destroy_root) {
                    state->destroy_thread = CreateThread(NULL, 0U, r8_destroy_root, state, 0U, NULL);
                    if (state->destroy_thread == NULL) r8_fail("root public destroy thread");
                    while (!atomic_load_explicit(&state->runtime->shutdown_requested, memory_order_acquire)) {
                        if (GetTickCount64() >= deadline) r8_fail("root public destroy publication");
                        Sleep(1U);
                    }
                } else if (!state->capture_tail && llam_runtime_request_stop_rt(state->runtime) != 0) {
                    r8_fail("root public stop");
                }
                atomic_store_explicit(&state->adversary_done, 1U, memory_order_release);
                atomic_store_explicit(&state->gate_release, 1U, memory_order_release);
                return 0U; /* No runtime or task access after the root releases. */
            }
        }
        Sleep(1U);
    }
    r8_fail("root adversary coverage");
    return 1U;
}

static void r8_check_mixed_trace(const r8_state_t *first, const r8_state_t *second) {
    llam_windows_async_test_event_t events[LLAM_WINDOWS_ASYNC_TEST_TRACE_CAPACITY];
    size_t count = llam_windows_async_lifetime_test_trace(
        first->runtime, events, LLAM_WINDOWS_ASYNC_TEST_TRACE_CAPACITY);
    size_t first_capture = SIZE_MAX, second_capture = SIZE_MAX;
    size_t first_restore = SIZE_MAX, second_return = SIZE_MAX;

    for (size_t i = 0U; i < count; ++i) {
        if (events[i].task_key == first->compute_key) {
            if (events[i].kind == LLAM_WINDOWS_ASYNC_TEST_CAPTURE && first_capture == SIZE_MAX) {
                first_capture = i;
            } else if (events[i].kind == LLAM_WINDOWS_ASYNC_TEST_RESTORE && first_restore == SIZE_MAX) {
                first_restore = i;
            }
        } else if (events[i].task_key == second->compute_key) {
            if (events[i].kind == LLAM_WINDOWS_ASYNC_TEST_CAPTURE && second_capture == SIZE_MAX) {
                second_capture = i;
            } else if (events[i].kind == LLAM_WINDOWS_ASYNC_TEST_RETURN && second_return == SIZE_MAX) {
                second_return = i;
            }
        }
    }
    if (!(first_capture < second_capture && second_capture < first_restore &&
          second_capture < second_return)) {
        fprintf(stderr, "llam-r8-mixed: capture1=%zu capture2=%zu restore1=%zu return2=%zu count=%zu; "
                "natural peer completion is not no-yield liveness\n",
                first_capture, second_capture, first_restore, second_return, count);
        r8_fail("same-M two-certified-leaf automatic overlap");
    }
    printf("llam-r8-mixed: capture1=%zu capture2=%zu restore1=%zu return2=%zu; "
           "both finite invocations exact, no manual capture or rescue\n",
           first_capture, second_capture, first_restore, second_return);
}

static void r8_run(HMODULE module,
                   llam_windows_async_pe_range_set_t *set,
                   const char *mode,
                   uintptr_t loop_begin,
                   uintptr_t ordinary_begin) {
    llam_runtime_opts_t options;
    llam_windows_async_preempt_opts_t async_options = {0};
    llam_windows_async_stats_t stats = {0};
    llam_runtime_t *runtime = NULL;
    llam_task_t *compute;
    llam_task_t *peer = NULL;
    llam_task_t *resolved;
    r8_state_t state = {0};
    r8_state_t second = {0};
    HANDLE deadline_done;
    HANDLE deadline_thread;
    HANDLE observer = NULL;
    bool positive = strcmp(mode, "positive") == 0;
    bool disabled = strcmp(mode, "disabled") == 0;
    bool ordinary = strcmp(mode, "ordinary") == 0;
    bool mixed = strcmp(mode, "mixed") == 0;
    bool ready_stop = strcmp(mode, "ready-stop") == 0;
    bool ready_destroy = strcmp(mode, "ready-destroy") == 0;
    bool capture_late = strcmp(mode, "capture-late") == 0;
    bool adversary = ready_stop || ready_destroy || capture_late;
    bool automatic = positive || ordinary || mixed || adversary;
    int result;

    if (!automatic && !disabled) {
        r8_fail("run mode");
    }
    state.loop = r8_loop_function(ordinary ? ordinary_begin : loop_begin);
    state.iterations = ordinary ? 7U : R8_ITERATIONS;
    state.expected = ordinary ? 16U : r8_reference(state.iterations);
    atomic_init(&state.compute_done, 0U);
    atomic_init(&state.peer_progress, 0U);
    atomic_init(&state.gate_observed, 0U);
    atomic_init(&state.gate_release, 0U);
    atomic_init(&state.adversary_done, 0U);
    atomic_init(&state.destroy_done, 0U);
    state.destroy_root = ready_destroy;
    state.capture_tail = capture_late;
    if (mixed) {
        second.loop = state.loop;
        second.iterations = state.iterations + UINT64_C(7919);
        second.expected = r8_reference(second.iterations);
        atomic_init(&second.compute_done, 0U);
        atomic_init(&second.peer_progress, 0U);
    }
    if (llam_runtime_opts_init(&options, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        r8_fail("runtime options");
    }
    options.deterministic = 1U;
    options.preempt_mode = adversary ? LLAM_PREEMPT_STRICT : LLAM_PREEMPT_AUTO;
    options.preempt_quantum_ns = 1000000U;
    if (llam_runtime_create(&options, LLAM_RUNTIME_OPTS_CURRENT_SIZE,
                            &runtime) != 0) {
        r8_fail("runtime create");
    }
    state.runtime = runtime;
    second.runtime = runtime;
    if (automatic) {
        async_options.flags =
            LLAM_WINDOWS_ASYNC_PREEMPT_ACK_PROCESS_FAILFAST;
        async_options.ranges = set->ranges;
        async_options.range_count = set->range_count;
        result = llam_runtime_enable_experimental_windows_async_preempt_ex(
            runtime, &async_options, sizeof(async_options));
        if (result != 0 && errno == ENOTSUP) {
            llam_runtime_destroy(runtime);
            llam_windows_async_pe_release_ranges(set);
            FreeLibrary(module);
            ExitProcess(77U);
        }
        if (result != 0 || !FreeLibrary(module)) {
            r8_fail("public expert enable");
        }
        llam_windows_async_pe_release_ranges(set);
        if (!r8_is_mapped(module, loop_begin)) {
            r8_fail("backend image pin");
        }
    }
    compute = llam_runtime_spawn_ex(runtime, r8_compute_task, &state,
                                    NULL, 0U);
    if (compute == NULL) {
        r8_fail("compute spawn");
    }
    resolved = llam_task_resolve_public_handle(compute);
    if (resolved == NULL) {
        r8_fail("compute handle resolve");
    }
    state.compute_key = (uintptr_t)resolved;
    llam_task_end_public_op(resolved);
    if (positive || mixed) {
        peer = llam_runtime_spawn_ex(runtime, mixed ? r8_compute_task : r8_peer_task,
                                     mixed ? &second : &state,
                                     NULL, 0U);
        if (peer == NULL) {
            r8_fail("peer spawn");
        }
        resolved = llam_task_resolve_public_handle(peer);
        if (resolved == NULL) {
            r8_fail("peer handle resolve");
        }
        state.peer_key = (uintptr_t)resolved;
        second.compute_key = state.peer_key;
        llam_task_end_public_op(resolved);
    }
    deadline_done = CreateEventA(NULL, TRUE, FALSE, NULL);
    deadline_thread = deadline_done != NULL
                          ? CreateThread(NULL, 0U, r8_deadline,
                                         deadline_done, 0U, NULL)
                          : NULL;
    if (deadline_thread == NULL) {
        r8_fail("deadline setup");
    }
    if (adversary) {
        llam_windows_async_lifetime_test_phase_gate((llam_task_t *)state.compute_key,
            capture_late ? LLAM_WINDOWS_ASYNC_TEST_BEFORE_CAPTURE_HANDOFF : LLAM_WINDOWS_ASYNC_TEST_READY,
            &state.gate_observed, &state.gate_release);
        observer = CreateThread(NULL, 0U, r8_root_observer, &state, 0U, NULL);
        if (observer == NULL) r8_fail("root observer");
    }
    result = llam_runtime_run_handle(runtime);
    if (observer != NULL) {
        if (WaitForSingleObject(observer, 1000U) != WAIT_OBJECT_0 ||
            atomic_load_explicit(&state.adversary_done, memory_order_acquire) != 1U) {
            r8_fail("root observer completion");
        }
        CloseHandle(observer);
    }
    if (result != 0 ||
        atomic_load_explicit(&state.compute_done,
                             memory_order_acquire) != 1U ||
        state.actual != state.expected) {
        r8_fail("finite arithmetic result/run");
    }
    if (ready_destroy) {
        if (WaitForSingleObject(state.destroy_thread, 1000U) != WAIT_OBJECT_0 ||
            atomic_load_explicit(&state.destroy_done, memory_order_acquire) != 1U) {
            r8_fail("root destroy completion");
        }
        CloseHandle(state.destroy_thread);
        goto runtime_released; /* Public destroy consumed runtime and task handles. */
    }
    if (mixed) {
        if (atomic_load_explicit(&second.compute_done, memory_order_acquire) != 1U ||
            second.actual != second.expected) {
            r8_fail("second finite arithmetic result");
        }
        r8_check_mixed_trace(&state, &second);
    }
    if (positive || mixed || adversary) {
        if (!llam_windows_async_lifetime_stats(runtime, &stats) ||
            stats.captures < (adversary ? 1U : 2U) || stats.restores != stats.captures ||
            stats.direct_witnesses < (adversary ? 1U : 2U) || stats.binding_ops != 0U ||
            stats.captured_bank_active != 0U ||
            stats.pending_restore_active != 0U ||
            stats.execution_debt != 0U ||
            llam_windows_async_lifetime_charged_bytes(runtime) != 0U) {
            fprintf(stderr,
                    "llam-r8-runtime: captures=%llu restores=%llu witnesses=%llu attempts=%llu binding=%llu bank=%llu pending=%llu debt=%llu bytes=%llu\n",
                    (unsigned long long)stats.captures,
                    (unsigned long long)stats.restores,
                    (unsigned long long)stats.direct_witnesses,
                    (unsigned long long)stats.capture_attempts,
                    (unsigned long long)stats.binding_ops,
                    (unsigned long long)stats.captured_bank_active,
                    (unsigned long long)stats.pending_restore_active,
                    (unsigned long long)stats.execution_debt,
                    (unsigned long long)llam_windows_async_lifetime_charged_bytes(runtime));
            r8_fail("automatic operation/drain statistics");
        }
        if (positive) r8_check_trace(&state, loop_begin, loop_begin + R8_LOOP_CODE_BYTES);
    } else if (ordinary) {
        if (!llam_windows_async_lifetime_stats(runtime, &stats) ||
            stats.captures != 0U || stats.restores != 0U ||
            stats.direct_witnesses != 0U || stats.binding_ops != 0U ||
            llam_windows_async_lifetime_charged_bytes(runtime) != 0U) {
            r8_fail("ordinary function captured");
        }
    } else if (llam_windows_async_lifetime_stats(runtime, &stats) ||
               atomic_load_explicit(&runtime->async_preempt_enabled,
                                    memory_order_acquire) != 0U) {
        r8_fail("disabled backend mutated context");
    }
    r8_join(compute);
    if (peer != NULL) {
        r8_join(peer);
    }
    llam_runtime_destroy(runtime);
runtime_released:
    if (disabled) {
        llam_windows_async_pe_release_ranges(set);
        if (!FreeLibrary(module)) {
            r8_fail("disabled caller module release");
        }
    }
    if (r8_is_mapped(module, loop_begin)) {
        r8_fail("runtime/module final unload");
    }
    SetEvent(deadline_done);
    if (WaitForSingleObject(deadline_thread, 1000U) != WAIT_OBJECT_0) {
        r8_fail("deadline thread join");
    }
    CloseHandle(deadline_thread);
    CloseHandle(deadline_done);
    printf("llam-r8-runtime: %s passed iterations=%llu expected=%llu actual=%llu\n",
           mode, (unsigned long long)state.iterations,
           (unsigned long long)state.expected,
           (unsigned long long)state.actual);
}

static void r8_precommit_failure(HMODULE module,
                                 llam_windows_async_pe_range_set_t *set,
                                 uintptr_t loop_begin) {
    llam_runtime_opts_t options;
    llam_windows_async_preempt_opts_t async_options = {0};
    llam_windows_async_preempt_range_t ranges[2];
    llam_runtime_t *runtime = NULL;
    void *private_page;

    if (llam_runtime_opts_init(&options, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        r8_fail("failure-control options");
    }
    options.deterministic = 1U;
    options.preempt_mode = LLAM_PREEMPT_AUTO;
    if (llam_runtime_create(&options, LLAM_RUNTIME_OPTS_CURRENT_SIZE,
                            &runtime) != 0) {
        r8_fail("failure-control runtime create");
    }
    private_page = VirtualAlloc(NULL, 4096U, MEM_RESERVE | MEM_COMMIT,
                                PAGE_EXECUTE_READ);
    if (private_page == NULL) {
        r8_fail("failure-control private page");
    }
    ranges[0] = set->ranges[0];
    ranges[1].begin_pc = (uint64_t)(uintptr_t)private_page;
    ranges[1].end_pc = ranges[1].begin_pc + 16U;
    ranges[1].flags = 0U;
    ranges[1].reserved0 = 0U;
    async_options.flags = LLAM_WINDOWS_ASYNC_PREEMPT_ACK_PROCESS_FAILFAST;
    async_options.ranges = ranges;
    async_options.range_count = 2U;
    errno = 0;
    if (llam_runtime_enable_experimental_windows_async_preempt_ex(
            runtime, &async_options, sizeof(async_options)) != -1 ||
        errno != ENOTSUP ||
        atomic_load_explicit(&runtime->async_preempt_enabled,
                             memory_order_acquire) != 0U ||
        runtime->async_preempt_control != NULL ||
        runtime->async_preempt_ranges != NULL ||
        runtime->async_preempt_range_count != 0U) {
        r8_fail("prepublication input rejection");
    }
    VirtualFree(private_page, 0U, MEM_RELEASE);
    llam_windows_async_pe_release_ranges(set);
    if (!FreeLibrary(module) || r8_is_mapped(module, loop_begin)) {
        r8_fail("precommit module pin unwind");
    }
    llam_runtime_destroy(runtime);
    puts("llam-r8-runtime: precommit failure/unload passed");
}

int main(int argc, char **argv) {
    llam_windows_async_pe_range_set_t set = {0};
    HMODULE module;
    uintptr_t loop_begin;
    uintptr_t scalar_begin;
    uintptr_t ordinary_begin;
    const char *mode;

    if (argc != 3 || argv[1] == NULL || argv[2] == NULL) {
        return 2;
    }
    mode = argv[1];
    module = LoadLibraryA(argv[2]);
    if (module == NULL) {
        r8_fail("LoadLibrary certified DLL");
    }
    loop_begin = r8_symbol(module, "llam_r8_cpu_loop_v1");
    scalar_begin = r8_symbol(module, "llam_win_good64");
    ordinary_begin = r8_symbol(module, "llam_transport_ordinary");
    if (llam_windows_async_pe_collect_ranges(module, &set) != 0 ||
        set.held_module != module || set.ranges == NULL ||
        set.range_count != 2U ||
        set.ranges[0].begin_pc != loop_begin ||
        set.ranges[0].end_pc != loop_begin + R8_LOOP_CODE_BYTES ||
        set.ranges[1].begin_pc != scalar_begin ||
        set.ranges[1].end_pc != scalar_begin + R8_SCALAR_CODE_BYTES ||
        (ordinary_begin >= set.ranges[0].begin_pc &&
         ordinary_begin < set.ranges[0].end_pc) ||
        (ordinary_begin >= set.ranges[1].begin_pc &&
         ordinary_begin < set.ranges[1].end_pc)) {
        r8_fail("exact compiler-certified collection");
    }
    if (strcmp(mode, "positive") == 0 ||
        strcmp(mode, "mixed") == 0 ||
        strcmp(mode, "ready-stop") == 0 ||
        strcmp(mode, "ready-destroy") == 0 ||
        strcmp(mode, "capture-late") == 0 ||
        strcmp(mode, "disabled") == 0 ||
        strcmp(mode, "ordinary") == 0) {
        r8_run(module, &set, mode, loop_begin, ordinary_begin);
    } else if (strcmp(mode, "precommit") == 0) {
        r8_precommit_failure(module, &set, loop_begin);
    } else {
        llam_windows_async_pe_release_ranges(&set);
        FreeLibrary(module);
        return 2;
    }
    return 0;
}
