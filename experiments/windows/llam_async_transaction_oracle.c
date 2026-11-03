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

/* Explicit production capture/restore transaction, without the testhook gate. */
#include "runtime_internal.h"
#include "windows_async_lifetime.h"

#include <errno.h>
#include <stdio.h>

extern void win_llam_async_leaf(void);
extern const unsigned char win_llam_async_leaf_begin[];
extern const unsigned char win_llam_async_leaf_end[];
extern void win_llam_async_dual_leaf(void);
extern const unsigned char win_llam_async_dual_safe_begin[];
extern const unsigned char win_llam_async_dual_safe_end[];

atomic_uint g_win_llam_async_release = ATOMIC_VAR_INIT(0U);
atomic_uint g_win_llam_async_dual_release = ATOMIC_VAR_INIT(0U);
atomic_uint g_win_llam_async_dual_stage = ATOMIC_VAR_INIT(0U);

typedef struct transaction_state {
    llam_runtime_t *runtime;
    llam_task_t *task;
    llam_channel_t *peer_gate;
    atomic_uint entered;
    atomic_uint peer_progress;
    atomic_uint finished;
    atomic_uint task_error;
    atomic_uint controller_done;
    atomic_uint captured;
    atomic_uint restored;
    atomic_uint requested_cycle;
    atomic_uint auto_cycles;
    bool stop_at_gate;
    bool chain_mode;
    bool unsafe_witness;
    bool automatic;
    bool automatic_stop;
} transaction_state_t;

static void transaction_fail(const char *where) {
    fprintf(stderr, "llam-async-transaction: %s failed\n", where);
    ExitProcess(1U);
}

static void transaction_task(void *opaque) {
    transaction_state_t *state = (transaction_state_t *)opaque;

    errno = 123;
    SetLastError(456U);
    atomic_store_explicit(&state->entered, 1U, memory_order_release);
    if (state->unsafe_witness) {
        win_llam_async_dual_leaf();
    } else {
        win_llam_async_leaf();
    }
    if (errno != 123 || GetLastError() != 456U ||
        atomic_load_explicit(&state->peer_progress,
                             memory_order_acquire) <
            (state->automatic_stop ? 0U : state->chain_mode ? 4U : 1U)) {
        atomic_store_explicit(&state->task_error, 1U,
                              memory_order_release);
    }
    atomic_store_explicit(&state->finished, 1U, memory_order_release);
}

static void transaction_peer(void *opaque) {
    transaction_state_t *state = (transaction_state_t *)opaque;

    if (state->automatic) {
        unsigned cycle;

        for (cycle = 1U; cycle <= 4U; ++cycle) {
            void *value = NULL;

            if (llam_channel_recv_result(state->peer_gate, &value) != 0 ||
                value != (void *)(uintptr_t)cycle) {
                atomic_store_explicit(&state->task_error, 1U,
                                      memory_order_release);
                return;
            }
            atomic_store_explicit(&state->peer_progress, cycle,
                                  memory_order_release);
        }
        return;
    }
    if (!state->chain_mode) {
        atomic_fetch_add_explicit(&state->peer_progress, 1U,
                                  memory_order_release);
        return;
    }
    for (;;) {
        unsigned requested = atomic_load_explicit(&state->requested_cycle,
                                                  memory_order_acquire);
        unsigned completed = atomic_load_explicit(&state->peer_progress,
                                                  memory_order_acquire);

        if (requested > completed) {
            atomic_store_explicit(&state->peer_progress, requested,
                                  memory_order_release);
            if (requested == 4U) {
                return;
            }
        }
        llam_yield();
    }
}

/* This monitor never invokes capture, restore, witness, or scheduler hooks. */
static DWORD transaction_automatic_monitor(transaction_state_t *state) {
    ULONGLONG deadline = GetTickCount64() + 6000U;
    uint64_t previous_run = 0U;
    unsigned cycle;

    while (GetTickCount64() < deadline) {
        if (atomic_load_explicit(&state->entered,
                                 memory_order_acquire) != 0U) {
            previous_run = atomic_load_explicit(
                &state->task->run_generation, memory_order_acquire);
            break;
        }
        Sleep(1U);
    }
    if (previous_run == 0U) {
        transaction_fail("automatic first dispatch");
    }
    if (state->automatic_stop) {
        llam_windows_async_stats_t stats = {0};

        while (GetTickCount64() < deadline) {
            if (!llam_windows_async_lifetime_stats(state->runtime, &stats)) {
                transaction_fail("stop capture statistics");
            }
            if (stats.captures != 0U) {
                break;
            }
            Sleep(1U);
        }
        if (stats.captures == 0U) {
            transaction_fail("automatic stop capture");
        }
        llam_request_stop(state->runtime);
        while (GetTickCount64() < deadline) {
            if (!llam_windows_async_lifetime_stats(state->runtime, &stats)) {
                transaction_fail("stop restore statistics");
            }
            if (stats.restores != 0U) {
                break;
            }
            Sleep(1U);
        }
        if (stats.restores == 0U) {
            transaction_fail("automatic stop restore debt");
        }
        atomic_store_explicit(&state->auto_cycles, 1U,
                              memory_order_release);
        atomic_store_explicit(&g_win_llam_async_release, 1U,
                              memory_order_release);
        atomic_store_explicit(&state->controller_done, 1U,
                              memory_order_release);
        return 0U;
    }
    for (cycle = 1U; cycle <= 4U; ++cycle) {
        uint64_t run_generation = previous_run;

        if (llam_channel_try_send(state->peer_gate,
                                  (void *)(uintptr_t)cycle) != 0) {
            transaction_fail("automatic peer wake");
        }

        while (GetTickCount64() < deadline) {
            run_generation = atomic_load_explicit(
                &state->task->run_generation, memory_order_acquire);
            if (run_generation > previous_run &&
                atomic_load_explicit(&state->peer_progress,
                                     memory_order_acquire) >= cycle) {
                break;
            }
            Sleep(1U);
        }
        if (run_generation <= previous_run ||
            atomic_load_explicit(&state->peer_progress,
                                 memory_order_acquire) < cycle) {
            transaction_fail("automatic repeated dispatch/peer progress");
        }
        previous_run = run_generation;
        atomic_store_explicit(&state->auto_cycles, cycle,
                              memory_order_release);
    }
    atomic_store_explicit(&g_win_llam_async_release, 1U,
                          memory_order_release);
    atomic_store_explicit(&state->controller_done, 1U,
                          memory_order_release);
    return 0U;
}

static DWORD WINAPI transaction_controller(void *opaque) {
    transaction_state_t *state = (transaction_state_t *)opaque;
    llam_windows_async_operation_t op;
    llam_windows_async_bank_t *previous_bank = NULL;
    llam_ctx_t original_g_ctx;
    ULONGLONG deadline;
    unsigned cycle;
    unsigned cycles = state->chain_mode ? 4U : 1U;

    if (state->automatic) {
        return transaction_automatic_monitor(state);
    }
    deadline = GetTickCount64() + 4000U;
    while (atomic_load_explicit(&state->entered,
                                memory_order_acquire) == 0U &&
           GetTickCount64() < deadline) {
        Sleep(1U);
    }
    if (atomic_load_explicit(&state->entered,
                             memory_order_acquire) == 0U) {
        transaction_fail("entered");
    }
    original_g_ctx = state->task->ctx;
    for (cycle = 1U; cycle <= cycles; ++cycle) {
        bool captured = false;
        bool restored = false;
        bool witnessed = false;

        memset(&op, 0, sizeof(op));
        atomic_store_explicit(&state->requested_cycle, cycle,
                              memory_order_release);
        deadline = GetTickCount64() + 4000U;
        while (GetTickCount64() < deadline) {
            llam_windows_async_capture_result_t result;

            if (llam_worker_load_active_g(
                    &state->runtime->shards[0].primary_worker) != state->task) {
                Sleep(1U);
                continue;
            }
            result = llam_windows_async_lifetime_try_capture_primary(
                state->runtime, &op);
            if (result == LLAM_WINDOWS_ASYNC_CAPTURED) {
                captured = true;
                break;
            }
            Sleep(1U);
        }
        if (!captured || op.task != state->task ||
            (state->chain_mode && previous_bank == op.bank)) {
            transaction_fail("capture/alternating bank");
        }
        previous_bank = op.bank;
        atomic_fetch_add_explicit(&state->captured, 1U,
                                  memory_order_release);
        deadline = GetTickCount64() + 4000U;
        while (atomic_load_explicit(&op.bank->phase,
                                    memory_order_acquire) !=
                   LLAM_WINDOWS_ASYNC_BANK_GATE &&
               GetTickCount64() < deadline) {
            Sleep(1U);
        }
        if (atomic_load_explicit(&op.bank->phase,
                                 memory_order_acquire) !=
                LLAM_WINDOWS_ASYNC_BANK_GATE ||
            atomic_load_explicit(&state->peer_progress,
                                 memory_order_acquire) < cycle) {
            transaction_fail("fresh native restore gate/peer progress");
        }
        if (state->stop_at_gate) {
            llam_request_stop(state->runtime);
        }
        if (state->unsafe_witness) {
            atomic_store_explicit(&g_win_llam_async_dual_release, 1U,
                                  memory_order_release);
        } else if (!state->chain_mode) {
            atomic_store_explicit(&g_win_llam_async_release, 1U,
                                  memory_order_release);
        }
        while (GetTickCount64() < deadline) {
            if (llam_windows_async_lifetime_try_restore(&op)) {
                restored = true;
                break;
            }
            Sleep(1U);
        }
        if (!restored) {
            transaction_fail("restore");
        }
        atomic_fetch_add_explicit(&state->restored, 1U,
                                  memory_order_release);
        deadline = GetTickCount64() + 4000U;
        if (state->chain_mode || state->unsafe_witness) {
            if (state->unsafe_witness) {
                while (atomic_load_explicit(&g_win_llam_async_dual_stage,
                                            memory_order_acquire) != 2U &&
                       GetTickCount64() < deadline) {
                    Sleep(1U);
                }
                if (atomic_load_explicit(&g_win_llam_async_dual_stage,
                                         memory_order_acquire) != 2U) {
                    transaction_fail("entered unsafe G interval");
                }
            }
            while (GetTickCount64() < deadline) {
                if (llam_windows_async_lifetime_try_observe_running(&op)) {
                    if (state->unsafe_witness &&
                        (uintptr_t)op.bank->redirected.context->Rip <
                            (uintptr_t)win_llam_async_dual_safe_end) {
                        transaction_fail("unsafe witness PC");
                    }
                    witnessed = true;
                    break;
                }
                if (atomic_load_explicit(&op.bank->phase,
                                         memory_order_acquire) ==
                    LLAM_WINDOWS_ASYNC_BANK_OBSERVED) {
                    transaction_fail("unexpected scheduler return");
                }
                Sleep(1U);
            }
        } else {
            while (atomic_load_explicit(&op.bank->phase,
                                        memory_order_acquire) !=
                       LLAM_WINDOWS_ASYNC_BANK_OBSERVED &&
                   GetTickCount64() < deadline) {
                Sleep(1U);
            }
            witnessed = atomic_load_explicit(&op.bank->phase,
                                             memory_order_acquire) ==
                        LLAM_WINDOWS_ASYNC_BANK_OBSERVED;
        }
        if (!witnessed) {
            transaction_fail("restore witness");
        }
        llam_windows_async_lifetime_op_finish(&op);
        if (state->chain_mode &&
            memcmp(&original_g_ctx, &state->task->ctx,
                   sizeof(original_g_ctx)) != 0) {
            transaction_fail("cooperative G context changed");
        }
    }
    if (state->chain_mode) {
        atomic_store_explicit(&g_win_llam_async_release, 1U,
                              memory_order_release);
    }
    if (state->unsafe_witness) {
        atomic_store_explicit(&g_win_llam_async_dual_release, 3U,
                              memory_order_release);
    }
    atomic_store_explicit(&state->controller_done, 1U,
                          memory_order_release);
    return 0U;
}

int main(void) {
    llam_runtime_opts_t options;
    transaction_state_t state;
    llam_task_t *handle;
    llam_task_t *peer;
    HANDLE controller;
    int run_result;
    llam_windows_async_stats_t stats;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.entered, 0U);
    atomic_init(&state.peer_progress, 0U);
    atomic_init(&state.finished, 0U);
    atomic_init(&state.task_error, 0U);
    atomic_init(&state.controller_done, 0U);
    atomic_init(&state.captured, 0U);
    atomic_init(&state.restored, 0U);
    atomic_init(&state.requested_cycle, 0U);
    atomic_init(&state.auto_cycles, 0U);
    state.stop_at_gate =
        GetEnvironmentVariableA("LLAM_TRANSACTION_STOP_GATE", NULL, 0U) != 0U;
    state.chain_mode =
        GetEnvironmentVariableA("LLAM_TRANSACTION_CHAIN", NULL, 0U) != 0U;
    state.unsafe_witness =
        GetEnvironmentVariableA("LLAM_TRANSACTION_UNSAFE_WITNESS", NULL, 0U) != 0U;
    state.automatic =
        GetEnvironmentVariableA("LLAM_TRANSACTION_AUTOMATIC", NULL, 0U) != 0U;
    state.automatic_stop =
        GetEnvironmentVariableA("LLAM_TRANSACTION_AUTOMATIC_STOP", NULL, 0U) != 0U;
    state.automatic = state.automatic || state.automatic_stop;
    if (llam_runtime_opts_init(&options, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return 1;
    }
    options.deterministic = 1U;
    options.preempt_mode = state.automatic_stop
                               ? LLAM_PREEMPT_STRICT
                               : state.automatic ? LLAM_PREEMPT_AUTO
                                                 : LLAM_PREEMPT_OFF;
    if (state.automatic && !state.automatic_stop) {
        options.preempt_quantum_ns = 1000000U;
    }
    if (llam_runtime_init_ex(&options, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return 1;
    }
    state.runtime = llam_runtime_default_storage();
    if (llam_windows_async_lifetime_register_safe_range(
            state.runtime,
            (uintptr_t)(state.unsafe_witness
                            ? win_llam_async_dual_safe_begin
                            : win_llam_async_leaf_begin),
            (uintptr_t)(state.unsafe_witness
                            ? win_llam_async_dual_safe_end
                            : win_llam_async_leaf_end)) != 0 ||
        llam_windows_async_lifetime_enable(state.runtime) != 0) {
        if (errno == ENOTSUP) {
            llam_runtime_shutdown();
            return 77;
        }
        transaction_fail("enable");
    }
    if (state.automatic) {
        state.peer_gate = llam_channel_create(1U);
        if (state.peer_gate == NULL) {
            transaction_fail("peer channel");
        }
    }
    handle = llam_spawn(transaction_task, &state, NULL);
    peer = state.automatic_stop
               ? NULL
               : llam_spawn(transaction_peer, &state, NULL);
    if (handle == NULL || (!state.automatic_stop && peer == NULL)) {
        transaction_fail("spawn");
    }
    state.task = llam_task_resolve_public_handle(handle);
    if (state.task == NULL) {
        transaction_fail("resolve handle");
    }
    llam_task_end_public_op(state.task);
    if (llam_detach(handle) != 0) {
        transaction_fail("detach");
    }
    controller = CreateThread(NULL, 0U, transaction_controller,
                              &state, 0U, NULL);
    if (controller == NULL) {
        transaction_fail("controller thread");
    }
    run_result = llam_run();
    if (run_result != 0 ||
        WaitForSingleObject(controller, 5000U) != WAIT_OBJECT_0 ||
        atomic_load_explicit(&state.controller_done,
                             memory_order_acquire) != 1U ||
        (!state.automatic &&
         (atomic_load_explicit(&state.captured,
                               memory_order_acquire) !=
              (state.chain_mode ? 4U : 1U) ||
          atomic_load_explicit(&state.restored,
                               memory_order_acquire) !=
              (state.chain_mode ? 4U : 1U))) ||
        (state.automatic &&
         atomic_load_explicit(&state.auto_cycles,
                              memory_order_acquire) !=
             (state.automatic_stop ? 1U : 4U)) ||
        atomic_load_explicit(&state.finished,
                             memory_order_acquire) != 1U ||
        atomic_load_explicit(&state.task_error,
                             memory_order_acquire) != 0U ||
        (peer != NULL && llam_join(peer) != 0) ||
        llam_windows_async_lifetime_charged_bytes(state.runtime) != 0U) {
        transaction_fail("run/drain");
    }
    if (state.automatic &&
        (!llam_windows_async_lifetime_stats(state.runtime, &stats) ||
         stats.captures < (state.automatic_stop ? 1U : 4U) ||
         stats.restores < (state.automatic_stop ? 1U : 4U) ||
         (!state.automatic_stop && stats.direct_witnesses < 3U) ||
         stats.capture_attempts > 64U)) {
        transaction_fail("automatic operation accounting");
    }
    CloseHandle(controller);
    if (state.peer_gate != NULL &&
        (llam_channel_close(state.peer_gate) != 0 ||
         llam_channel_destroy(state.peer_gate) != 0)) {
        transaction_fail("peer channel cleanup");
    }
    llam_runtime_shutdown();
    puts(state.automatic_stop
             ? "llam-async-transaction: watchdog restore debt survived stop"
             : state.automatic
             ? "llam-async-transaction: watchdog repeated preemption passed"
             : state.chain_mode
             ? "llam-async-transaction: four no-yield capture/witness cycles passed"
             : state.unsafe_witness
                   ? "llam-async-transaction: read-only unsafe-PC witness passed"
                   : "llam-async-transaction: production capture/restore passed");
    return 0;
}
