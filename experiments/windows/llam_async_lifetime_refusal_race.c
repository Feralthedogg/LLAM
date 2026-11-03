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

/* A DEAD G racing a pre-mutation refusal must not recycle its FREE bank early. */
#include "runtime_internal.h"
#include "windows_async_lifetime.h"

#include <stdio.h>

typedef struct refusal_state {
    llam_runtime_t *runtime;
    llam_task_t *task;
    atomic_uint entered;
    atomic_uint release;
    atomic_uint run_completed;
    atomic_uint deferred_seen;
    atomic_uint controller_done;
} refusal_state_t;

static void refusal_fail(const char *where) {
    fprintf(stderr, "llam-async-refusal: %s failed\n", where);
    ExitProcess(1U);
}

static void refusal_task(void *opaque) {
    refusal_state_t *state = (refusal_state_t *)opaque;

    atomic_store_explicit(&state->entered, 1U, memory_order_release);
    while (atomic_load_explicit(&state->release,
                                memory_order_acquire) == 0U) {
        YieldProcessor();
    }
}

static DWORD WINAPI refusal_controller(void *opaque) {
    refusal_state_t *state = (refusal_state_t *)opaque;
    llam_windows_async_operation_t op;
    llam_shard_t *shard = &state->runtime->shards[0];
    ULONGLONG deadline = GetTickCount64() + 4000U;

    memset(&op, 0, sizeof(op));
    while (GetTickCount64() < deadline) {
        if (atomic_load_explicit(&state->entered,
                                 memory_order_acquire) != 0U &&
            llam_windows_async_lifetime_op_begin(state->runtime, &op)) {
            break;
        }
        Sleep(1U);
    }
    if (GetTickCount64() >= deadline || op.task != state->task ||
        op.bank->phase != LLAM_WINDOWS_ASYNC_BANK_CAPTURING ||
        op.bank->external_ops != 1U) {
        refusal_fail("operation reservation");
    }

    /* No suspension or context mutation: G can finish before policy refusal. */
    atomic_store_explicit(&state->release, 1U, memory_order_release);
    deadline = GetTickCount64() + 4000U;
    while ((atomic_load_explicit(&shard->windows_async_retire_count,
                                 memory_order_acquire) == 0U ||
            llam_runtime_has_live_tasks(state->runtime)) &&
           GetTickCount64() < deadline) {
        Sleep(1U);
    }
    if (atomic_load_explicit(&shard->windows_async_retire_count,
                             memory_order_acquire) == 0U ||
        llam_runtime_has_live_tasks(state->runtime) ||
        atomic_load_explicit(&state->task->reclaim_ready,
                             memory_order_acquire) != 0U ||
        state->task->stack_mapping == NULL ||
        llam_windows_async_lifetime_charged_bytes(state->runtime) == 0U ||
        atomic_load_explicit(&state->run_completed,
                             memory_order_acquire) != 0U) {
        refusal_fail("retirement before refusal cleanup");
    }
    atomic_store_explicit(&state->deferred_seen, 1U,
                          memory_order_release);
    llam_windows_async_lifetime_op_abort(&op);
    atomic_store_explicit(&state->controller_done, 1U,
                          memory_order_release);
    return 0U;
}

int main(void) {
    llam_runtime_opts_t options;
    refusal_state_t state;
    llam_task_t *handle;
    HANDLE controller;
    int run_result;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.entered, 0U);
    atomic_init(&state.release, 0U);
    atomic_init(&state.run_completed, 0U);
    atomic_init(&state.deferred_seen, 0U);
    atomic_init(&state.controller_done, 0U);
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
        refusal_fail("enable");
    }
    handle = llam_spawn(refusal_task, &state, NULL);
    if (handle == NULL) {
        refusal_fail("spawn");
    }
    state.task = llam_task_resolve_public_handle(handle);
    if (state.task == NULL) {
        refusal_fail("resolve handle");
    }
    llam_task_end_public_op(state.task);
    if (llam_detach(handle) != 0) {
        refusal_fail("detach");
    }
    controller = CreateThread(NULL, 0U, refusal_controller, &state, 0U, NULL);
    if (controller == NULL) {
        refusal_fail("controller create");
    }
    run_result = llam_run();
    atomic_store_explicit(&state.run_completed, 1U, memory_order_release);
    if (run_result != 0 ||
        WaitForSingleObject(controller, 5000U) != WAIT_OBJECT_0 ||
        atomic_load_explicit(&state.deferred_seen,
                             memory_order_acquire) != 1U ||
        atomic_load_explicit(&state.controller_done,
                             memory_order_acquire) != 1U ||
        llam_windows_async_lifetime_charged_bytes(state.runtime) != 0U ||
        atomic_load_explicit(&state.runtime->shards[0].windows_async_execution_debt,
                             memory_order_acquire) != 0U) {
        refusal_fail("refusal/run drain");
    }
    CloseHandle(controller);
    llam_runtime_shutdown();
    puts("llam-async-refusal: completed G retained through bank refusal");
    return 0;
}
