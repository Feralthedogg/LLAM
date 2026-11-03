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

/* Async-off control through LLAM's actual Windows task stack and switch path. */
#define _WIN32_WINNT 0x0A00
#include "llam/runtime.h"

#include <windows.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#define PROBE_CANARY_A UINT64_C(0x83b9f3ab2a41c7d5)
#define PROBE_CANARY_B UINT64_C(0x5e1c904e72ab6483)

extern int win_probe_local_exception(void);

typedef struct exception_state {
    int g_before_yield;
    int g_after_yield;
    unsigned g_canary_ok;
} exception_state_t;

static void exception_task(void *argument) {
    exception_state_t *state = (exception_state_t *)argument;
    volatile uint64_t canary_a = PROBE_CANARY_A;
    volatile uint64_t canary_b = PROBE_CANARY_B;

    state->g_before_yield = win_probe_local_exception();
    llam_yield();
    state->g_after_yield = win_probe_local_exception();
    state->g_canary_ok = canary_a == PROBE_CANARY_A &&
                         canary_b == PROBE_CANARY_B;
}

int main(void) {
    llam_runtime_opts_t options;
    exception_state_t state = {0};
    llam_task_t *task;
    volatile uint64_t m_canary = PROBE_CANARY_A;
    int m_before;
    int m_after;

    SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX);
    (void)setvbuf(stdout, NULL, _IONBF, 0);
    m_before = win_probe_local_exception();
    printf("llam-g-exception-control: native_before=%d\n", m_before);
    if (m_before != 42 ||
        llam_runtime_opts_init(&options, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return 1;
    }
    options.deterministic = 1U;
    options.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    if (llam_runtime_init_ex(&options, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        fprintf(stderr, "llam-g-exception-control: runtime init failed: %d\n", errno);
        return 1;
    }
    task = llam_spawn(exception_task, &state, NULL);
    if (task == NULL || llam_run() != 0 || llam_join(task) != 0) {
        fprintf(stderr, "llam-g-exception-control: run failed: %d\n", errno);
        llam_runtime_shutdown();
        return 1;
    }
    llam_runtime_shutdown();
    m_after = win_probe_local_exception();
    printf("llam-g-exception-control: G_before=%d G_after=%d G_canary=%u "
           "native_after=%d M_canary=%u\n",
           state.g_before_yield, state.g_after_yield,
           state.g_canary_ok, m_after, m_canary == PROBE_CANARY_A);
    return state.g_before_yield == 42 && state.g_after_yield == 42 &&
           state.g_canary_ok != 0U && m_after == 42 &&
           m_canary == PROBE_CANARY_A ? 0 : 1;
}
