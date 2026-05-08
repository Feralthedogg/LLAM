/**
 * @file tests/test_windows_runtime_smoke.c
 * @brief Native Windows scheduler smoke test for context switching and wakeup paths.
 *
 * @copyright Copyright 2026 Feralthedogg
 *
 * @par License
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "llam/runtime.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    WINDOWS_SMOKE_TASKS = 8,
    WINDOWS_SMOKE_YIELDS = 1000,
};

typedef struct windows_smoke_state {
    atomic_uint ran;
    atomic_uint failures;
    atomic_uint blocking_calls;
    int first_errno;
    char first_case[96];
} windows_smoke_state_t;

#if defined(_WIN64) && defined(__GNUC__) && defined(__x86_64__)
#define LLAM_WINDOWS_XMM_SMOKE 1
static inline __attribute__((always_inline)) void windows_smoke_set_xmm6(uint64_t lo, uint64_t hi) {
    uint64_t words[2] = {lo, hi};

    __asm__ volatile("movdqu %0, %%xmm6" : : "m"(words));
}

static inline __attribute__((always_inline)) int windows_smoke_xmm6_matches(uint64_t lo, uint64_t hi) {
    uint64_t words[2];

    __asm__ volatile("movdqu %%xmm6, %0" : "=m"(words));
    return words[0] == lo && words[1] == hi;
}
#else
#define LLAM_WINDOWS_XMM_SMOKE 0
#endif

static int fail(const char *message) {
    fprintf(stderr, "[test_windows_runtime_smoke] %s\n", message);
    return 1;
}

static int fail_errno(const char *message) {
    fprintf(stderr, "[test_windows_runtime_smoke] %s: errno=%d (%s)\n", message, errno, strerror(errno));
    return 1;
}

static void task_fail(windows_smoke_state_t *state, const char *where, int err) {
    if (atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed) == 0U) {
        state->first_errno = err;
        (void)snprintf(state->first_case, sizeof(state->first_case), "%s", where);
    }
}

static void *blocking_callback(void *arg) {
    windows_smoke_state_t *state = arg;

    atomic_fetch_add_explicit(&state->blocking_calls, 1U, memory_order_relaxed);
    return arg;
}

static void smoke_task(void *arg) {
    windows_smoke_state_t *state = arg;
    void *blocking_result = NULL;
#if LLAM_WINDOWS_XMM_SMOKE
    uint64_t xmm_lo = UINT64_C(0x4c4c414d58364c4f) ^ (uint64_t)(uintptr_t)state;
    uint64_t xmm_hi = UINT64_C(0x57494e363453494d) ^ ((uint64_t)WINDOWS_SMOKE_YIELDS << 32);
#endif
    unsigned i;

#if LLAM_WINDOWS_XMM_SMOKE
    windows_smoke_set_xmm6(xmm_lo, xmm_hi);
#endif
    for (i = 0U; i < WINDOWS_SMOKE_YIELDS; ++i) {
        errno = EADDRINUSE;
        llam_yield();
        if (errno != EADDRINUSE) {
            task_fail(state, "errno changed across Windows context switch", errno);
            return;
        }
#if LLAM_WINDOWS_XMM_SMOKE
        if (!windows_smoke_xmm6_matches(xmm_lo, xmm_hi)) {
            task_fail(state, "XMM6 changed across Windows context switch", 0);
            return;
        }
#endif
    }
    if (llam_call_blocking_result(blocking_callback, state, &blocking_result) != 0 ||
        blocking_result != state) {
        task_fail(state, "blocking callback failed", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

int main(void) {
    windows_smoke_state_t state;
    llam_runtime_opts_t runtime_opts;
    llam_runtime_stats_t stats;
    llam_task_t *tasks[WINDOWS_SMOKE_TASKS];
    unsigned i;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.ran, 0U);
    atomic_init(&state.failures, 0U);
    atomic_init(&state.blocking_calls, 0U);

    if (llam_runtime_opts_init(&runtime_opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return fail_errno("llam_runtime_opts_init failed");
    }
    runtime_opts.deterministic = 1U;
    runtime_opts.forced_yield_every = 4U;
    runtime_opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_runtime_init_ex(&runtime_opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return fail_errno("llam_runtime_init_ex failed");
    }

    for (i = 0U; i < WINDOWS_SMOKE_TASKS; ++i) {
        tasks[i] = llam_spawn(smoke_task, &state, NULL);
        if (tasks[i] == NULL) {
            llam_runtime_shutdown();
            return fail_errno("llam_spawn failed");
        }
    }
    if (llam_run() != 0) {
        llam_runtime_shutdown();
        return fail_errno("llam_run failed");
    }
    for (i = 0U; i < WINDOWS_SMOKE_TASKS; ++i) {
        if (llam_join(tasks[i]) != 0) {
            llam_runtime_shutdown();
            return fail_errno("llam_join failed");
        }
    }
    if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
        fprintf(stderr,
                "[test_windows_runtime_smoke] task failed at %s errno=%d (%s)\n",
                state.first_case,
                state.first_errno,
                strerror(state.first_errno));
        llam_runtime_shutdown();
        return 1;
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != WINDOWS_SMOKE_TASKS ||
        atomic_load_explicit(&state.blocking_calls, memory_order_relaxed) != WINDOWS_SMOKE_TASKS) {
        llam_runtime_shutdown();
        return fail("unexpected task/blocking completion count");
    }
    if (llam_runtime_collect_stats(&stats) != 0) {
        llam_runtime_shutdown();
        return fail_errno("llam_runtime_collect_stats failed");
    }
    if (stats.ctx_switches == 0U || stats.yields == 0U || stats.blocking_completions == 0U) {
        llam_runtime_shutdown();
        return fail("runtime stats did not observe scheduler activity");
    }

    llam_runtime_shutdown();
    puts("[test_windows_runtime_smoke] ok");
    return 0;
}
