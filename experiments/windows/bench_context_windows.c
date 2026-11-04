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

/* Short Windows scheduler/context ceiling diagnostic; no I/O in the timed run. */
#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>

#include "llam/runtime.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONTEXT_ROUNDS 4U
#define CONTEXT_YIELDS_PER_TASK 10000U

typedef struct context_work {
    volatile unsigned *completed;
} context_work_t;

static void context_fail(const char *where, int code) {
    fprintf(stderr, "context-win: %s failed: %d\n", where, code);
    ExitProcess(1U);
}

static void context_task(void *argument) {
    context_work_t *work = (context_work_t *)argument;
    unsigned index;

    for (index = 0U; index < CONTEXT_YIELDS_PER_TASK; ++index) {
        llam_yield();
        ++*work->completed;
    }
}

static uint64_t context_qpc_ns(LARGE_INTEGER ticks, LARGE_INTEGER frequency) {
    return (uint64_t)((long double)ticks.QuadPart * 1000000000.0L /
                      (long double)frequency.QuadPart);
}

static void context_round(unsigned tasks, unsigned round) {
    llam_runtime_opts_t options;
    llam_runtime_stats_t stats;
    llam_task_t *handles[2] = {NULL, NULL};
    context_work_t work;
    volatile unsigned completed = 0U;
    LARGE_INTEGER frequency;
    LARGE_INTEGER start;
    LARGE_INTEGER end;
    uint64_t elapsed_ns;
    unsigned index;

    work.completed = &completed;
    if (!QueryPerformanceFrequency(&frequency) ||
        llam_runtime_opts_init(&options, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        context_fail("options/QPF", errno);
    }
    options.deterministic = 1U;
    options.preempt_mode = LLAM_PREEMPT_OFF;
    options.experimental_flags = 0U;
    if (llam_runtime_init_compat_ex(&options, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        context_fail("runtime init", errno);
    }
    for (index = 0U; index < tasks; ++index) {
        handles[index] = llam_spawn(context_task, &work, NULL);
        if (handles[index] == NULL) {
            context_fail("spawn", errno);
        }
    }
    QueryPerformanceCounter(&start);
    if (llam_run() != 0) {
        context_fail("run", errno);
    }
    QueryPerformanceCounter(&end);
    if (llam_runtime_collect_stats(&stats) != 0) {
        context_fail("collect", errno);
    }
    for (index = 0U; index < tasks; ++index) {
        if (llam_join(handles[index]) != 0) {
            context_fail("join", errno);
        }
    }
    if (completed != tasks * CONTEXT_YIELDS_PER_TASK) {
        context_fail("yield accounting", ERROR_INVALID_DATA);
    }
    elapsed_ns = context_qpc_ns(end, frequency) -
                 context_qpc_ns(start, frequency);
    printf("context-win: tasks=%u round=%u warmup=%u yields=%u elapsed_ns=%llu "
           "ns_per_yield=%.1f ctx_switches=%llu\n",
           tasks, round, round == 0U, tasks * CONTEXT_YIELDS_PER_TASK,
           (unsigned long long)elapsed_ns,
           (double)elapsed_ns / (double)(tasks * CONTEXT_YIELDS_PER_TASK),
           (unsigned long long)stats.ctx_switches);
    llam_runtime_shutdown();
}

int main(void) {
    unsigned round;

    SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX);
    if (!SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS)) {
        context_fail("SetPriorityClass", (int)GetLastError());
    }
    for (round = 0U; round < CONTEXT_ROUNDS; ++round) {
        context_round(1U, round);
        context_round(2U, round);
    }
    return 0;
}
