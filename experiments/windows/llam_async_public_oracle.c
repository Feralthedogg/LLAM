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

/* Production-linked public opt-in and all-or-nothing failure oracle. */
#include "runtime_internal.h"
#include "windows_async_lifetime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void win_llam_async_leaf(void);
extern const unsigned char win_llam_async_leaf_begin[];
extern const unsigned char win_llam_async_leaf_end[];

atomic_uint g_win_llam_async_release = ATOMIC_VAR_INIT(0U);

typedef struct public_state {
    llam_runtime_t *runtime;
    atomic_uint entered;
    atomic_uint done;
} public_state_t;

static void public_fail(const char *where) {
    fprintf(stderr, "llam-async-public: %s failed (errno=%d)\n", where, errno);
    ExitProcess(1U);
}

static void public_create(public_state_t *state, uint32_t mode) {
    llam_runtime_opts_t options;

    memset(state, 0, sizeof(*state));
    atomic_init(&state->entered, 0U);
    atomic_init(&state->done, 0U);
    if (llam_runtime_opts_init(&options, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        public_fail("runtime options");
    }
    options.deterministic = 1U;
    options.preempt_mode = mode;
    options.preempt_quantum_ns = 1000000U;
    if (llam_runtime_create(&options, LLAM_RUNTIME_OPTS_CURRENT_SIZE,
                            &state->runtime) != 0 || state->runtime == NULL) {
        public_fail("runtime create");
    }
}

static llam_windows_async_preempt_range_t public_leaf_range(void) {
    llam_windows_async_preempt_range_t range = {0};

    range.begin_pc = (uint64_t)(uintptr_t)win_llam_async_leaf_begin;
    range.end_pc = (uint64_t)(uintptr_t)win_llam_async_leaf_end;
    return range;
}

static llam_windows_async_preempt_opts_t public_options(
    const llam_windows_async_preempt_range_t *ranges,
    uint64_t count) {
    llam_windows_async_preempt_opts_t options = {0};

    options.flags = LLAM_WINDOWS_ASYNC_PREEMPT_ACK_PROCESS_FAILFAST;
    options.ranges = ranges;
    options.range_count = count;
    return options;
}

static void public_check_empty(const public_state_t *state) {
    if (state->runtime->async_preempt_ranges != NULL ||
        state->runtime->async_preempt_range_count != 0U ||
        state->runtime->async_preempt_control != NULL ||
        state->runtime->windows_async_range_generation != 0U ||
        atomic_load_explicit(&state->runtime->async_preempt_enabled,
                             memory_order_acquire) != 0U ||
        atomic_load_explicit(
            &state->runtime->preempt_capability.registered_safe_ranges,
            memory_order_acquire) != 0U) {
        public_fail("failed opt-in left partial configuration");
    }
}

static void public_task(void *opaque) {
    public_state_t *state = (public_state_t *)opaque;

    atomic_store_explicit(&state->entered, 1U, memory_order_release);
    win_llam_async_leaf();
    atomic_store_explicit(&state->done, 1U, memory_order_release);
}

static void public_quick_task(void *opaque) {
    (void)opaque;
}

static DWORD WINAPI public_monitor(void *opaque) {
    public_state_t *state = (public_state_t *)opaque;
    ULONGLONG deadline = GetTickCount64() + 6000U;

    while (GetTickCount64() < deadline) {
        llam_windows_async_stats_t stats = {0};

        if (atomic_load_explicit(&state->entered,
                                 memory_order_acquire) != 0U &&
            llam_windows_async_lifetime_stats(state->runtime, &stats) &&
            stats.captures != 0U && stats.restores != 0U &&
            stats.direct_witnesses != 0U) {
            atomic_store_explicit(&g_win_llam_async_release, 1U,
                                  memory_order_release);
            return 0U;
        }
        Sleep(1U);
    }
    public_fail("automatic capture/restore/witness");
    return 1U;
}

static void public_join(HANDLE thread, const char *where) {
    DWORD exit_code = 1U;

    if (thread == NULL || WaitForSingleObject(thread, 4000U) != WAIT_OBJECT_0 ||
        !GetExitCodeThread(thread, &exit_code) || exit_code != 0U) {
        public_fail(where);
    }
    CloseHandle(thread);
}

static int public_valid(void) {
    public_state_t state;
    llam_windows_async_preempt_range_t range = public_leaf_range();
    llam_windows_async_preempt_opts_t options = public_options(&range, 1U);
    llam_windows_async_stats_t stats = {0};
    llam_task_t *task;
    HANDLE monitor;
    int result;

    public_create(&state, LLAM_PREEMPT_STRICT);
    result = llam_runtime_enable_experimental_windows_async_preempt_ex(
        state.runtime, &options, sizeof(options));
    if (result != 0 && errno == ENOTSUP) {
        llam_runtime_destroy(state.runtime);
        return 77;
    }
    if (result != 0 ||
        state.runtime->async_preempt_range_count != 1U ||
        state.runtime->async_preempt_ranges[0].begin !=
            (uintptr_t)win_llam_async_leaf_begin ||
        atomic_load_explicit(&state.runtime->preempt_capability.async_state,
                             memory_order_acquire) != LLAM_PREEMPT_ASYNC_ACTIVE) {
        public_fail("public enable/capability publication");
    }
    /* The runtime copied the caller-owned array before publishing enable. */
    memset(&range, 0, sizeof(range));
    task = llam_runtime_spawn_ex(state.runtime, public_task,
                                 &state, NULL, 0U);
    if (task == NULL || llam_detach(task) != 0) {
        public_fail("public enabled spawn");
    }
    monitor = CreateThread(NULL, 0U, public_monitor, &state, 0U, NULL);
    if (monitor == NULL) {
        public_fail("monitor thread");
    }
    result = llam_runtime_run_handle(state.runtime);
    public_join(monitor, "monitor thread completion");
    if (result != 0 ||
        atomic_load_explicit(&state.done, memory_order_acquire) != 1U ||
        !llam_windows_async_lifetime_stats(state.runtime, &stats) ||
        stats.captures == 0U || stats.restores == 0U ||
        stats.direct_witnesses == 0U || stats.binding_ops != 0U ||
        stats.captured_bank_active != 0U ||
        stats.pending_restore_active != 0U ||
        stats.execution_debt != 0U || stats.retire_count != 0U ||
        stats.finalizer_count != 0U ||
        llam_windows_async_lifetime_charged_bytes(state.runtime) != 0U) {
        public_fail("public enabled run drain");
    }
    llam_runtime_destroy(state.runtime);
    return 0;
}

static int public_negatives(void) {
    public_state_t state;
    public_state_t disabled;
    public_state_t already_run;
    llam_windows_async_preempt_range_t ranges[2] = {0};
    llam_windows_async_preempt_opts_t options;
    llam_task_t *prior_task;
    void *private_code;

    public_create(&state, LLAM_PREEMPT_STRICT);
    ranges[0] = public_leaf_range();
    options = public_options(ranges, 1U);
    options.flags = 0U;
    errno = 0;
    if (llam_runtime_enable_experimental_windows_async_preempt_ex(
            state.runtime, &options, sizeof(options)) != -1 ||
        errno != EINVAL) {
        public_fail("missing process-failfast acknowledgement");
    }
    public_check_empty(&state);
    options.flags = LLAM_WINDOWS_ASYNC_PREEMPT_ACK_PROCESS_FAILFAST;
    errno = 0;
    if (llam_runtime_enable_experimental_windows_async_preempt_ex(
            state.runtime, &options, sizeof(options) - 1U) != -1 ||
        errno != EINVAL) {
        public_fail("short public options prefix");
    }
    public_check_empty(&state);
    options.flags |= UINT32_C(2);
    errno = 0;
    if (llam_runtime_enable_experimental_windows_async_preempt_ex(
            state.runtime, &options, sizeof(options)) != -1 ||
        errno != EINVAL) {
        public_fail("unknown public option flag");
    }
    public_check_empty(&state);
    options.flags = LLAM_WINDOWS_ASYNC_PREEMPT_ACK_PROCESS_FAILFAST;
    ranges[0].flags = 1U;
    errno = 0;
    if (llam_runtime_enable_experimental_windows_async_preempt_ex(
            state.runtime, &options, sizeof(options)) != -1 ||
        errno != EINVAL) {
        public_fail("unknown certified range flag");
    }
    public_check_empty(&state);
    ranges[0].flags = 0U;
    ranges[0].end_pc = ranges[0].begin_pc;
    errno = 0;
    if (llam_runtime_enable_experimental_windows_async_preempt_ex(
            state.runtime, &options, sizeof(options)) != -1 ||
        errno != EINVAL) {
        public_fail("empty certified range");
    }
    public_check_empty(&state);
    ranges[0] = public_leaf_range();
    options.range_count = 129U;
    errno = 0;
    if (llam_runtime_enable_experimental_windows_async_preempt_ex(
            state.runtime, &options, sizeof(options)) != -1 ||
        errno != EOVERFLOW) {
        public_fail("oversize certified range count");
    }
    public_check_empty(&state);
    options.range_count = 2U;
    ranges[1] = ranges[0];
    errno = 0;
    if (llam_runtime_enable_experimental_windows_async_preempt_ex(
            state.runtime, &options, sizeof(options)) != -1 ||
        errno != EINVAL) {
        public_fail("overlapping certified ranges");
    }
    public_check_empty(&state);
    private_code = VirtualAlloc(NULL, 4096U, MEM_RESERVE | MEM_COMMIT,
                                PAGE_EXECUTE_READWRITE);
    if (private_code == NULL) {
        public_fail("private executable allocation");
    }
    ranges[1].begin_pc = (uint64_t)(uintptr_t)private_code;
    ranges[1].end_pc = ranges[1].begin_pc + 16U;
    options.range_count = 2U;
    errno = 0;
    if (llam_runtime_enable_experimental_windows_async_preempt_ex(
            state.runtime, &options, sizeof(options)) != -1 ||
        errno != ENOTSUP) {
        public_fail("MEM_PRIVATE executable rejection");
    }
    public_check_empty(&state);
    VirtualFree(private_code, 0U, MEM_RELEASE);

    options.range_count = 1U;
    public_create(&disabled, LLAM_PREEMPT_OFF);
    errno = 0;
    if (llam_runtime_enable_experimental_windows_async_preempt_ex(
            disabled.runtime, &options, sizeof(options)) != -1 ||
        errno != ENOTSUP) {
        public_fail("OFF policy rejection");
    }
    public_check_empty(&disabled);
    llam_runtime_destroy(disabled.runtime);

    public_create(&already_run, LLAM_PREEMPT_STRICT);
    prior_task = llam_runtime_spawn_ex(already_run.runtime,
                                       public_quick_task, NULL, NULL, 0U);
    if (prior_task == NULL || llam_detach(prior_task) != 0 ||
        llam_runtime_run_handle(already_run.runtime) != 0) {
        public_fail("prior ordinary run setup");
    }
    errno = 0;
    if (llam_runtime_enable_experimental_windows_async_preempt_ex(
            already_run.runtime, &options, sizeof(options)) != -1 ||
        errno != EBUSY) {
        public_fail("public opt-in accepted a previously driven runtime");
    }
    public_check_empty(&already_run);
    llam_runtime_destroy(already_run.runtime);

    if (llam_runtime_enable_experimental_windows_async_preempt_ex(
            state.runtime, &options, sizeof(options)) != 0) {
        public_fail("retry after atomic rejection");
    }
    errno = 0;
    if (llam_runtime_enable_experimental_windows_async_preempt_ex(
            state.runtime, &options, sizeof(options)) != -1 ||
        errno != EALREADY) {
        public_fail("second public enable was not EALREADY");
    }
    llam_runtime_destroy(state.runtime);
    return 0;
}

int main(void) {
    char mode[24] = {0};
    int result;

    if (GetEnvironmentVariableA("LLAM_ASYNC_PUBLIC_MODE", mode,
                                sizeof(mode)) == 0U) {
        return 1;
    }
    if (strcmp(mode, "valid") == 0) {
        result = public_valid();
    } else if (strcmp(mode, "negatives") == 0) {
        result = public_negatives();
    } else {
        return 1;
    }
    if (result == 0) {
        printf("llam-async-public: %s passed\n", mode);
    }
    return result;
}
