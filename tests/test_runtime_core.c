/**
 * @file tests/test_runtime_core.c
 * @brief Core runtime lifecycle, spawn, introspection, blocking, and stats tests.
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
#include "runtime_internal.h"

#include <errno.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if LLAM_PLATFORM_POSIX
#include <pthread.h>
#include <unistd.h>
#endif

typedef struct core_state {
    atomic_uint failures;
    atomic_uint ran;
    atomic_uint blocking_calls;
    unsigned expected_flags;
    llam_task_class_t expected_class;
    int first_errno;
    char first_case[128];
} core_state_t;

typedef struct errno_task_args {
    core_state_t *state;
    int yield_errno;
    int sleep_errno;
} errno_task_args_t;

typedef struct double_join_state {
    core_state_t *state;
    llam_task_t *target;
    atomic_uint target_done;
    atomic_uint joined;
    atomic_uint busy;
} double_join_state_t;

typedef struct owner_diag_state {
    core_state_t core;
    llam_runtime_t fake_runtime;
    llam_channel_t *channel;
    llam_mutex_t *mutex;
    llam_cond_t *cond;
    llam_cancel_token_t *token;
    llam_task_group_t *group;
    llam_task_t *target;
    int payload;
} owner_diag_state_t;

#if LLAM_PLATFORM_POSIX
typedef struct dump_blocking_state {
    core_state_t core;
    atomic_uint blocking_started;
    atomic_uint release_blocking;
    atomic_uint task_done;
} dump_blocking_state_t;
#endif

#if LLAM_ARCH_AARCH64 && !LLAM_PLATFORM_WINDOWS
typedef struct aarch64_simd_state {
    core_state_t *state;
    uint64_t expected_d8_bits;
} aarch64_simd_state_t;
#endif

static int test_fail(const char *message) {
    fprintf(stderr, "[test_runtime_core] %s\n", message);
    return 1;
}

static int test_fail_errno(const char *message) {
    fprintf(stderr, "[test_runtime_core] %s: errno=%d (%s)\n", message, errno, strerror(errno));
    return 1;
}

static void task_fail(core_state_t *state, const char *where, int err) {
    if (atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed) == 0U) {
        state->first_errno = err;
        (void)snprintf(state->first_case, sizeof(state->first_case), "%s", where);
    }
}

static void expect_runtime_owner_mismatch(core_state_t *state, const char *where, int rc) {
    if (rc != -1 || errno != EXDEV) {
        task_fail(state, where, errno);
    }
}

#if LLAM_ARCH_AARCH64 && !LLAM_PLATFORM_WINDOWS
__attribute__((always_inline)) static inline void aarch64_set_d8_bits(uint64_t value) {
    __asm__ volatile("fmov d8, %0" : : "r"(value) : "v8");
}

__attribute__((always_inline)) static inline uint64_t aarch64_get_d8_bits(void) {
    uint64_t value;

    __asm__ volatile("fmov %0, d8" : "=r"(value));
    return value;
}
#endif

static void *blocking_callback(void *arg) {
    core_state_t *state = arg;

    atomic_fetch_add_explicit(&state->blocking_calls, 1U, memory_order_relaxed);
    return arg;
}

static void *blocking_null_callback(void *arg) {
    (void)arg;
    return NULL;
}

#if LLAM_PLATFORM_POSIX
static void *dump_blocking_callback(void *arg) {
    dump_blocking_state_t *state = arg;

    atomic_store_explicit(&state->blocking_started, 1U, memory_order_release);
    while (atomic_load_explicit(&state->release_blocking, memory_order_acquire) == 0U) {
        usleep(1000);
    }
    return arg;
}

static void dump_blocking_task(void *arg) {
    dump_blocking_state_t *state = arg;
    void *result = NULL;

    if (llam_call_blocking_result(dump_blocking_callback, state, &result) != 0 ||
        result != state) {
        task_fail(&state->core, "blocking job for concurrent runtime dump failed", errno);
        return;
    }
    atomic_store_explicit(&state->task_done, 1U, memory_order_release);
}

static void *dump_run_thread(void *arg) {
    dump_blocking_state_t *state = arg;

    if (llam_run() != 0) {
        task_fail(&state->core, "llam_run concurrent dump thread failed", errno);
    }
    return NULL;
}
#endif

static void inspect_task(void *arg) {
    core_state_t *state = arg;
    llam_task_t *self = llam_current_task();
    const char *state_name;
    void *blocking_result;

    if (self == NULL) {
        task_fail(state, "llam_current_task returned NULL", EINVAL);
        return;
    }
    if (llam_task_id(self) == 0U) {
        task_fail(state, "llam_task_id returned 0", EINVAL);
        return;
    }
    state_name = llam_task_state_name(self);
    if (state_name == NULL || strcmp(state_name, "UNKNOWN") == 0) {
        task_fail(state, "llam_task_state_name returned invalid name", EINVAL);
        return;
    }
    if (llam_task_class(self) != state->expected_class) {
        task_fail(state, "initial task class mismatch", EINVAL);
        return;
    }
    if ((llam_task_flags(self) & state->expected_flags) != state->expected_flags) {
        task_fail(state, "task flags mismatch", EINVAL);
        return;
    }

    if (llam_task_set_class(LLAM_TASK_CLASS_LATENCY) != 0) {
        task_fail(state, "llam_task_set_class failed", errno);
        return;
    }
    if (llam_task_class(self) != LLAM_TASK_CLASS_LATENCY) {
        task_fail(state, "llam_task_set_class did not update current task", EINVAL);
        return;
    }

    llam_task_safepoint();
    llam_yield();
    if (llam_sleep_ns(0U) != 0) {
        task_fail(state, "llam_sleep_ns(0)", errno);
        return;
    }
    if (llam_call_blocking(blocking_callback, state) != state) {
        task_fail(state, "llam_call_blocking", errno);
        return;
    }
    blocking_result = (void *)state;
    if (llam_call_blocking_result(blocking_null_callback, state, &blocking_result) != 0 ||
        blocking_result != NULL) {
        task_fail(state, "llam_call_blocking_result NULL callback", errno);
        return;
    }
    errno = 0;
    if (llam_call_blocking_result(blocking_callback, state, NULL) != -1 || errno != EINVAL) {
        task_fail(state, "llam_call_blocking_result NULL out", errno);
        return;
    }

    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void direct_yield_no_work_task(void *arg) {
    core_state_t *state = arg;
    llam_task_t *self = llam_current_task();

    if (self == NULL) {
        task_fail(state, "direct yield current task missing", EINVAL);
        return;
    }
    /*
     * This exercises the internal try-handoff API used by I/O and channel fast
     * paths.  With no local peer runnable, the call must fail without changing
     * the still-executing task from RUNNING to RUNNABLE.
     */
    if (llam_yield_to_local_runnable()) {
        task_fail(state, "direct yield unexpectedly found local work", EINVAL);
        return;
    }
    if (strcmp(llam_task_state_name(self), "RUNNING") != 0) {
        task_fail(state, "failed direct yield mutated current task state", EINVAL);
        return;
    }
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void errno_isolation_task(void *arg) {
    errno_task_args_t *args = arg;
    core_state_t *state = args->state;
    unsigned i;

    for (i = 0U; i < 16U; ++i) {
        errno = args->yield_errno;
        llam_yield();
        if (errno != args->yield_errno) {
            task_fail(state, "task-local errno was not preserved across yield", errno);
            return;
        }

        errno = args->sleep_errno;
        if (llam_sleep_ns(100000U) != 0) {
            task_fail(state, "llam_sleep_ns in errno isolation task", errno);
            return;
        }
        if (errno != args->sleep_errno) {
            task_fail(state, "task-local errno was not preserved across sleep", errno);
            return;
        }
    }

    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void double_join_target_task(void *arg) {
    double_join_state_t *state = arg;

    if (llam_sleep_ns(20ULL * 1000ULL * 1000ULL) != 0) {
        task_fail(state->state, "double join target sleep", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->target_done, 1U, memory_order_relaxed);
}

static void double_join_waiter_task(void *arg) {
    double_join_state_t *state = arg;

    llam_yield();
    if (llam_join(state->target) == 0) {
        atomic_fetch_add_explicit(&state->joined, 1U, memory_order_relaxed);
        return;
    }
    if (errno == EBUSY) {
        atomic_fetch_add_explicit(&state->busy, 1U, memory_order_relaxed);
        return;
    }
    task_fail(state->state, "double join unexpected errno", errno);
}

static void request_stop_task(void *arg) {
    core_state_t *state = arg;

    if (llam_runtime_request_stop() != 0) {
        task_fail(state, "llam_runtime_request_stop in task", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void shutdown_from_task_task(void *arg) {
    core_state_t *state = arg;
    int saved_errno = E2BIG;

    /*
     * Managed tasks must never run full singleton teardown from their scheduler
     * stack.  The public shutdown entry point should degrade to request_stop
     * here, preserve errno, and let the host thread destroy resources later.
     */
    errno = saved_errno;
    llam_runtime_shutdown();
    if (errno != saved_errno) {
        task_fail(state, "managed shutdown clobbered errno", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void detached_task(void *arg) {
    core_state_t *state = arg;

    llam_yield();
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

#if LLAM_ARCH_AARCH64 && !LLAM_PLATFORM_WINDOWS
static void aarch64_simd_preservation_task(void *arg) {
    aarch64_simd_state_t *simd = arg;
    unsigned i;

    aarch64_set_d8_bits(simd->expected_d8_bits);
    for (i = 0U; i < 64U; ++i) {
        llam_yield();
        if (aarch64_get_d8_bits() != simd->expected_d8_bits) {
            task_fail(simd->state, "AArch64 d8 was not preserved across yield", EINVAL);
            return;
        }
    }

    atomic_fetch_add_explicit(&simd->state->ran, 1U, memory_order_relaxed);
}
#endif

static int test_preinit_contracts(void) {
    llam_runtime_stats_t stats;
    llam_task_local_key_t key = LLAM_TASK_LOCAL_INVALID_KEY;

    if (llam_runtime_default() == NULL) {
        return test_fail("llam_runtime_default returned NULL");
    }
    if (llam_current_task() != NULL) {
        return test_fail("llam_current_task outside runtime was not NULL");
    }
    if (llam_task_id(NULL) != 0U) {
        return test_fail("llam_task_id(NULL) did not return 0");
    }
    if (strcmp(llam_task_state_name(NULL), "UNKNOWN") != 0) {
        return test_fail("llam_task_state_name(NULL) did not return UNKNOWN");
    }
    if (llam_task_class(NULL) != LLAM_TASK_CLASS_DEFAULT) {
        return test_fail("llam_task_class(NULL) did not return default class");
    }
    if (llam_task_flags(NULL) != 0U) {
        return test_fail("llam_task_flags(NULL) did not return 0");
    }
    llam_task_safepoint();
    errno = 0;
    if (llam_task_set_class(999U) != -1 || errno != EINVAL) {
        return test_fail("llam_task_set_class invalid class did not fail with EINVAL");
    }
    errno = 0;
    if (llam_task_set_class(LLAM_TASK_CLASS_DEFAULT) != -1 || errno != ENOTSUP) {
        return test_fail("llam_task_set_class outside task did not fail with ENOTSUP");
    }
    if (llam_task_local_key_create(&key) != 0) {
        return test_fail_errno("llam_task_local_key_create outside runtime failed");
    }
    errno = 0;
    if (llam_task_local_get(key) != NULL || errno != ENOTSUP) {
        (void)llam_task_local_key_delete(key);
        return test_fail("llam_task_local_get outside task did not fail with ENOTSUP");
    }
    errno = 0;
    if (llam_task_local_set(key, &stats) != -1 || errno != ENOTSUP) {
        (void)llam_task_local_key_delete(key);
        return test_fail("llam_task_local_set outside task did not fail with ENOTSUP");
    }
    if (llam_task_local_key_delete(key) != 0) {
        return test_fail_errno("llam_task_local_key_delete failed");
    }
    errno = 0;
    if (llam_task_local_key_delete(key) != -1 || errno != EINVAL) {
        return test_fail("llam_task_local_key_delete inactive key did not fail with EINVAL");
    }

    errno = 0;
    if (llam_runtime_collect_stats(NULL) != -1 || errno != EINVAL) {
        return test_fail("llam_runtime_collect_stats(NULL) did not fail with EINVAL");
    }
    errno = 0;
    if (llam_runtime_collect_stats_ex(&stats, 0U) != -1 || errno != EINVAL) {
        return test_fail("llam_runtime_collect_stats_ex zero size did not fail with EINVAL");
    }
    errno = 0;
    if (llam_runtime_write_stats_json(-1) != -1 || errno != EINVAL) {
        return test_fail("llam_runtime_write_stats_json invalid fd did not fail with EINVAL");
    }
    memset(&stats, 0xA5, sizeof(stats));
    errno = 0;
    if (llam_runtime_init_ex((const llam_runtime_opts_t *)(const void *)&stats, 0U) != -1 ||
        errno != EINVAL) {
        return test_fail("llam_runtime_init_ex zero-sized opts did not fail with EINVAL");
    }
    {
        llam_runtime_opts_t invalid_opts;

        memset(&invalid_opts, 0, sizeof(invalid_opts));
        invalid_opts.profile = 999U;
        errno = 0;
        if (llam_runtime_init_ex(&invalid_opts, sizeof(invalid_opts)) != -1 || errno != EINVAL) {
            return test_fail("llam_runtime_init_ex invalid profile did not fail with EINVAL");
        }
        memset(&invalid_opts, 0, sizeof(invalid_opts));
        invalid_opts.profile = LLAM_RUNTIME_PROFILE_BALANCED;
        invalid_opts.preempt_mode = 999U;
        errno = 0;
        if (llam_runtime_init_ex(&invalid_opts, sizeof(invalid_opts)) != -1 || errno != EINVAL) {
            return test_fail("llam_runtime_init_ex invalid preempt mode did not fail with EINVAL");
        }
    }
    errno = 0;
    if (llam_spawn(NULL, NULL, NULL) != NULL || errno != EINVAL) {
        return test_fail("llam_spawn before init did not fail with EINVAL");
    }
    errno = 0;
    if (llam_spawn_ex(inspect_task, NULL, (const llam_spawn_opts_t *)(const void *)&stats, 0U) != NULL ||
        errno != EINVAL) {
        return test_fail("llam_spawn_ex zero-sized opts did not fail with EINVAL");
    }
    errno = 0;
    if (llam_run() != -1 || errno != EINVAL) {
        return test_fail("llam_run before init did not fail with EINVAL");
    }
    errno = 0;
    if (llam_runtime_request_stop() != -1 || errno != EINVAL) {
        return test_fail("llam_runtime_request_stop before init did not fail with EINVAL");
    }
    errno = 0;
    if (llam_detach(NULL) != -1 || errno != EINVAL) {
        return test_fail("llam_detach(NULL) before init did not fail with EINVAL");
    }
    errno = 0;
    if (llam_sleep_ns(0U) != -1 || errno != EINVAL) {
        return test_fail("llam_sleep_ns before init did not fail with EINVAL");
    }
    return 0;
}

static int test_runtime_handle_api(void) {
    core_state_t state;
    llam_runtime_opts_t runtime_opts;
    llam_runtime_stats_t stats;
    llam_runtime_t *runtime = NULL;
    llam_runtime_t *second_runtime = NULL;
    llam_runtime_t *fake_runtime = (llam_runtime_t *)(void *)&state;
    llam_task_t *task;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.blocking_calls, 0U);

    memset(&runtime_opts, 0, sizeof(runtime_opts));
    runtime_opts.deterministic = 1U;
    runtime_opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;

    if (llam_runtime_create(&runtime_opts, sizeof(runtime_opts), &runtime) != 0) {
        return test_fail_errno("llam_runtime_create failed");
    }
    if (runtime == NULL || runtime == llam_runtime_default()) {
        llam_runtime_destroy(runtime);
        return test_fail("llam_runtime_create returned unexpected handle");
    }
    errno = 0;
    if (llam_runtime_run_handle(NULL) != -1 || errno != EINVAL) {
        llam_runtime_destroy(runtime);
        return test_fail("llam_runtime_run_handle(NULL) did not fail with EINVAL");
    }
    errno = 0;
    if (llam_runtime_run_handle(fake_runtime) != -1 || errno != EINVAL) {
        llam_runtime_destroy(runtime);
        return test_fail("llam_runtime_run_handle(non-default) did not fail with EINVAL");
    }
    errno = 0;
    if (llam_runtime_request_stop_rt(fake_runtime) != -1 || errno != EINVAL) {
        llam_runtime_destroy(runtime);
        return test_fail("llam_runtime_request_stop_rt(non-default) did not fail with EINVAL");
    }
    errno = 0;
    if (llam_runtime_collect_stats_ex_rt(fake_runtime, &stats, sizeof(stats)) != -1 || errno != EINVAL) {
        llam_runtime_destroy(runtime);
        return test_fail("llam_runtime_collect_stats_ex_rt(non-default) did not fail with EINVAL");
    }
    errno = 0;
    if (llam_runtime_write_stats_json_rt(fake_runtime, 1) != -1 || errno != EINVAL) {
        llam_runtime_destroy(runtime);
        return test_fail("llam_runtime_write_stats_json_rt(non-default) did not fail with EINVAL");
    }
    if (llam_runtime_collect_stats_ex_rt(runtime, &stats, sizeof(stats)) != 0) {
        llam_runtime_destroy(runtime);
        return test_fail_errno("llam_runtime_collect_stats_ex_rt(default) failed");
    }
    if (llam_runtime_create(&runtime_opts, sizeof(runtime_opts), &second_runtime) != 0 ||
        second_runtime == NULL ||
        second_runtime == runtime ||
        second_runtime == llam_runtime_default()) {
        llam_runtime_destroy(runtime);
        return test_fail("second llam_runtime_create did not return an independent handle");
    }
    llam_runtime_destroy(second_runtime);
    second_runtime = NULL;
    task = llam_runtime_spawn_ex(runtime, detached_task, &state, NULL, 0U);
    if (task == NULL) {
        llam_runtime_destroy(runtime);
        return test_fail_errno("llam_spawn for runtime handle failed");
    }
    if (llam_runtime_run_handle(runtime) != 0) {
        llam_runtime_destroy(runtime);
        return test_fail_errno("llam_runtime_run_handle failed");
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U) {
        llam_runtime_destroy(runtime);
        return test_fail("runtime handle task did not run");
    }
    if (llam_join(task) != 0) {
        llam_runtime_destroy(runtime);
        return test_fail_errno("llam_join for runtime handle task failed");
    }
    llam_runtime_destroy(runtime);
    return 0;
}

static int test_runtime_lifecycle_and_task_contracts(void) {
    core_state_t state;
    llam_runtime_opts_t runtime_opts;
    llam_spawn_opts_t spawn_opts;
    llam_runtime_stats_t stats;
    llam_runtime_stats_t prefix_stats;
    llam_task_t *task;
    unsigned expected_flags = LLAM_SPAWN_F_PINNED | LLAM_SPAWN_F_NO_PREEMPT;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.blocking_calls, 0U);
    state.expected_flags = expected_flags;
    state.expected_class = LLAM_TASK_CLASS_BATCH;

    memset(&runtime_opts, 0, sizeof(runtime_opts));
    runtime_opts.deterministic = 1U;
    runtime_opts.forced_yield_every = 2U;
    runtime_opts.experimental_flags = LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    runtime_opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;

    if (llam_runtime_init(&runtime_opts) != 0) {
        return test_fail_errno("llam_runtime_init failed");
    }
    errno = 0;
    if (llam_runtime_init(&runtime_opts) != -1 || errno != EBUSY) {
        llam_runtime_shutdown();
        return test_fail("second llam_runtime_init did not fail with EBUSY");
    }
    errno = 0;
    if (llam_spawn(NULL, NULL, NULL) != NULL || errno != EINVAL) {
        llam_runtime_shutdown();
        return test_fail("llam_spawn(NULL) after init did not fail with EINVAL");
    }
    {
        llam_spawn_opts_t invalid_spawn_opts;

        memset(&invalid_spawn_opts, 0, sizeof(invalid_spawn_opts));
        invalid_spawn_opts.task_class = 999U;
        errno = 0;
        if (llam_spawn_ex(inspect_task, &state, &invalid_spawn_opts, sizeof(invalid_spawn_opts)) != NULL ||
            errno != EINVAL) {
            llam_runtime_shutdown();
            return test_fail("llam_spawn_ex invalid task class did not fail with EINVAL");
        }
        memset(&invalid_spawn_opts, 0, sizeof(invalid_spawn_opts));
        invalid_spawn_opts.stack_class = 999U;
        errno = 0;
        if (llam_spawn_ex(inspect_task, &state, &invalid_spawn_opts, sizeof(invalid_spawn_opts)) != NULL ||
            errno != EINVAL) {
            llam_runtime_shutdown();
            return test_fail("llam_spawn_ex invalid stack class did not fail with EINVAL");
        }
    }

    memset(&spawn_opts, 0, sizeof(spawn_opts));
    spawn_opts.task_class = LLAM_TASK_CLASS_BATCH;
    spawn_opts.stack_class = LLAM_STACK_CLASS_LARGE;
    spawn_opts.flags = expected_flags;
    task = llam_spawn(inspect_task, &state, &spawn_opts);
    if (task == NULL) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_spawn inspect_task failed");
    }
    if (llam_task_id(task) == 0U ||
        llam_task_class(task) != LLAM_TASK_CLASS_BATCH ||
        (llam_task_flags(task) & expected_flags) != expected_flags) {
        llam_runtime_shutdown();
        return test_fail("spawned task metadata was not observable before run");
    }

    if (llam_run() != 0) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_run failed");
    }
    if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
        fprintf(stderr,
                "[test_runtime_core] task failed at %s errno=%d (%s)\n",
                state.first_case,
                state.first_errno,
                strerror(state.first_errno));
        llam_runtime_shutdown();
        return 1;
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.blocking_calls, memory_order_relaxed) != 1U) {
        llam_runtime_shutdown();
        return test_fail("task or blocking callback did not run exactly once");
    }
    if (llam_runtime_collect_stats_ex(&stats, sizeof(stats)) != 0) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_runtime_collect_stats_ex failed");
    }
    if (stats.active_workers == 0U || stats.active_nodes == 0U || stats.yields == 0U ||
        stats.blocking_calls == 0U || stats.blocking_completions == 0U) {
        llam_runtime_shutdown();
        return test_fail("runtime stats did not record expected activity");
    }
    memset(&prefix_stats, 0xA5, sizeof(prefix_stats));
    if (llam_runtime_collect_stats_ex(&prefix_stats, offsetof(llam_runtime_stats_t, active_nodes)) != 0) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_runtime_collect_stats_ex prefix failed");
    }
    if (prefix_stats.ctx_switches == 0U || prefix_stats.yields == 0U ||
        prefix_stats.active_nodes != 0xA5A5A5A5U) {
        llam_runtime_shutdown();
        return test_fail("runtime stats prefix copy was not bounded");
    }
    if (llam_runtime_collect_stats(&stats) != 0) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_runtime_collect_stats wrapper failed");
    }
#if LLAM_PLATFORM_POSIX
    {
        int pipe_fds[2];
        char json[4096];
        ssize_t nread;

        if (pipe(pipe_fds) != 0) {
            llam_runtime_shutdown();
            return test_fail_errno("pipe for stats json failed");
        }
        if (llam_runtime_write_stats_json(pipe_fds[1]) != 0) {
            int saved_errno = errno;

            close(pipe_fds[0]);
            close(pipe_fds[1]);
            llam_runtime_shutdown();
            errno = saved_errno;
            return test_fail_errno("llam_runtime_write_stats_json failed");
        }
        close(pipe_fds[1]);
        nread = read(pipe_fds[0], json, sizeof(json) - 1U);
        close(pipe_fds[0]);
        if (nread <= 0) {
            llam_runtime_shutdown();
            return test_fail("stats json read produced no data");
        }
        json[nread] = '\0';
        if (json[0] != '{' ||
            strstr(json, "\"ctx_switches\":") == NULL ||
            strstr(json, "\"active_workers\":") == NULL ||
            strstr(json, "\"io_submit_syscalls\":") == NULL ||
            strstr(json, "\"yield_direct_attempts\":") == NULL ||
            strstr(json, "\"yield_direct_fail_push\":") == NULL) {
            llam_runtime_shutdown();
            return test_fail("stats json did not contain expected fields");
        }
        {
            const char *idle_spin_fallbacks = strstr(json, "\"idle_spin_fallbacks\":");

            if (idle_spin_fallbacks == NULL ||
                strstr(idle_spin_fallbacks + 1, "\"idle_spin_fallbacks\":") != NULL) {
                llam_runtime_shutdown();
                return test_fail("stats json field set is missing or duplicated");
            }
        }
    }
    {
        char path[] = "/tmp/llam-runtime-dump-XXXXXX";
        int dump_fd = mkstemp(path);
        off_t dump_size;
        char *dump = NULL;
        ssize_t dump_read;

        if (dump_fd < 0) {
            llam_runtime_shutdown();
            return test_fail_errno("mkstemp for runtime dump failed");
        }
        (void)unlink(path);
        llam_dump_runtime_state(dump_fd);
        dump_size = lseek(dump_fd, 0, SEEK_END);
        if (dump_size <= 0 || dump_size > (off_t)(1024U * 1024U) ||
            lseek(dump_fd, 0, SEEK_SET) < 0) {
            int saved_errno = errno;

            close(dump_fd);
            llam_runtime_shutdown();
            errno = saved_errno;
            return test_fail_errno("runtime dump sizing failed");
        }
        dump = malloc((size_t)dump_size + 1U);
        if (dump == NULL) {
            close(dump_fd);
            llam_runtime_shutdown();
            return test_fail_errno("runtime dump allocation failed");
        }
        dump_read = read(dump_fd, dump, (size_t)dump_size);
        close(dump_fd);
        if (dump_read != dump_size) {
            free(dump);
            llam_runtime_shutdown();
            return test_fail("runtime dump read was incomplete");
        }
        dump[dump_read] = '\0';
        if (strstr(dump, "lifecycle:") == NULL ||
            strstr(dump, "stop_requested=") == NULL ||
            strstr(dump, "active_io_waiters=") == NULL ||
            strstr(dump, "io_queues(") == NULL ||
            strstr(dump, "inflight_io_waiters=") == NULL ||
            strstr(dump, "wait_owner=") == NULL ||
            strstr(dump, "io_req=") == NULL ||
            strstr(dump, "block_job=") == NULL) {
            free(dump);
            llam_runtime_shutdown();
            return test_fail("runtime dump did not contain ownership diagnostics");
        }
        free(dump);
    }
#endif

    llam_runtime_shutdown();
    return 0;
}

static int test_request_stop_returns_success(void) {
    core_state_t state;
    llam_runtime_opts_t runtime_opts;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.blocking_calls, 0U);

    memset(&runtime_opts, 0, sizeof(runtime_opts));
    runtime_opts.deterministic = 1U;
    runtime_opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;

    if (llam_runtime_init(&runtime_opts) != 0) {
        return test_fail_errno("llam_runtime_init for request_stop failed");
    }
    if (llam_spawn(request_stop_task, &state, NULL) == NULL) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_spawn request_stop task failed");
    }
    if (llam_run() != 0) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_run did not treat request_stop as success");
    }
    if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U ||
        atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U) {
        llam_runtime_shutdown();
        return test_fail("request_stop task did not complete cleanly");
    }

    llam_runtime_shutdown();
    return 0;
}

static int test_shutdown_from_task_requests_stop(void) {
    core_state_t state;
    llam_runtime_opts_t runtime_opts;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.blocking_calls, 0U);

    memset(&runtime_opts, 0, sizeof(runtime_opts));
    runtime_opts.deterministic = 1U;
    runtime_opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;

    if (llam_runtime_init(&runtime_opts) != 0) {
        return test_fail_errno("llam_runtime_init for task shutdown failed");
    }
    if (llam_spawn(shutdown_from_task_task, &state, NULL) == NULL) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_spawn task shutdown task failed");
    }
    if (llam_run() != 0) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_run did not complete after task shutdown");
    }
    if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U ||
        atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U) {
        llam_runtime_shutdown();
        return test_fail("task shutdown did not degrade to stop cleanly");
    }

    llam_runtime_shutdown();
    return 0;
}

#if LLAM_PLATFORM_POSIX
static int test_runtime_dump_while_blocking_job_active(void) {
    dump_blocking_state_t state;
    llam_runtime_opts_t runtime_opts;
    pthread_t runner;
    bool runner_started = false;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.core.failures, 0U);
    atomic_init(&state.core.ran, 0U);
    atomic_init(&state.core.blocking_calls, 0U);
    atomic_init(&state.blocking_started, 0U);
    atomic_init(&state.release_blocking, 0U);
    atomic_init(&state.task_done, 0U);

    memset(&runtime_opts, 0, sizeof(runtime_opts));
    runtime_opts.deterministic = 1U;
    runtime_opts.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;

    if (llam_runtime_init(&runtime_opts) != 0) {
        return test_fail_errno("llam_runtime_init for concurrent dump failed");
    }
    if (llam_spawn(dump_blocking_task, &state, NULL) == NULL) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_spawn concurrent dump task failed");
    }
    if (pthread_create(&runner, NULL, dump_run_thread, &state) != 0) {
        llam_runtime_shutdown();
        return test_fail_errno("pthread_create concurrent dump runner failed");
    }
    runner_started = true;

    for (unsigned i = 0U; i < 5000U &&
                          atomic_load_explicit(&state.blocking_started, memory_order_acquire) == 0U;
         ++i) {
        usleep(1000);
    }
    if (atomic_load_explicit(&state.blocking_started, memory_order_acquire) == 0U) {
        atomic_store_explicit(&state.release_blocking, 1U, memory_order_release);
        if (runner_started) {
            (void)pthread_join(runner, NULL);
        }
        llam_runtime_shutdown();
        return test_fail("blocking callback did not start before concurrent dump timeout");
    }

    /*
     * Runtime dumps are diagnostic reads that can run from another OS thread
     * while a blocking helper owns task wait state.  This loop keeps that
     * cross-thread ownership visible under TSan and guards against regressing
     * wait-owner fields back to plain data-racy pointers.
     */
    for (unsigned i = 0U; i < 64U; ++i) {
        FILE *dump = tmpfile();

        if (i == 32U) {
            atomic_store_explicit(&state.release_blocking, 1U, memory_order_release);
        }
        if (dump == NULL) {
            atomic_store_explicit(&state.release_blocking, 1U, memory_order_release);
            if (runner_started) {
                (void)pthread_join(runner, NULL);
            }
            llam_runtime_shutdown();
            return test_fail_errno("tmpfile for concurrent runtime dump failed");
        }
        llam_dump_runtime_state(fileno(dump));
        fclose(dump);
    }

    atomic_store_explicit(&state.release_blocking, 1U, memory_order_release);
    if (runner_started && pthread_join(runner, NULL) != 0) {
        llam_runtime_shutdown();
        return test_fail_errno("pthread_join concurrent dump runner failed");
    }
    if (atomic_load_explicit(&state.core.failures, memory_order_relaxed) != 0U ||
        atomic_load_explicit(&state.task_done, memory_order_acquire) != 1U) {
        fprintf(stderr,
                "[test_runtime_core] concurrent dump task failed at %s errno=%d (%s)\n",
                state.core.first_case,
                state.core.first_errno,
                strerror(state.core.first_errno));
        llam_runtime_shutdown();
        return 1;
    }

    llam_runtime_shutdown();
    return 0;
}
#endif

static int test_detach_contract(void) {
    core_state_t state;
    llam_runtime_opts_t runtime_opts;
    llam_task_t *before_run;
    llam_task_t *after_run;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.blocking_calls, 0U);

    memset(&runtime_opts, 0, sizeof(runtime_opts));
    runtime_opts.deterministic = 1U;
    runtime_opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;

    if (llam_runtime_init(&runtime_opts) != 0) {
        return test_fail_errno("llam_runtime_init for detach failed");
    }
    before_run = llam_spawn(detached_task, &state, NULL);
    after_run = llam_spawn(detached_task, &state, NULL);
    if (before_run == NULL || after_run == NULL) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_spawn detach tasks failed");
    }
    if (llam_detach(before_run) != 0) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_detach before run failed");
    }
    if (llam_run() != 0) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_run detach failed");
    }
    if (llam_detach(after_run) != 0) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_detach after run failed");
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 2U) {
        llam_runtime_shutdown();
        return test_fail("detached tasks did not both complete");
    }

    llam_runtime_shutdown();
    return 0;
}

static void owner_diag_target_task(void *arg) {
    core_state_t *state = arg;

    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void owner_diag_task(void *arg) {
    owner_diag_state_t *state = arg;
    llam_runtime_t *owner;
    llam_channel_t *raw_channel;
    llam_mutex_t *raw_mutex;
    llam_cond_t *raw_cond;
    llam_cancel_token_t *raw_token;
    llam_task_group_t *raw_group;
    void *out = NULL;
    llam_select_op_t op;
    size_t selected = 0U;

    raw_channel = llam_channel_resolve_public_handle(state->channel);
    if (raw_channel == NULL) {
        task_fail(&state->core, "channel owner diagnostic resolve", errno);
        return;
    }
    owner = raw_channel->owner_runtime;
    raw_channel->owner_runtime = &state->fake_runtime;
    expect_runtime_owner_mismatch(&state->core,
                                  "channel try send cross-runtime",
                                  llam_channel_try_send(state->channel, &state->payload));
    expect_runtime_owner_mismatch(&state->core,
                                  "channel try recv cross-runtime",
                                  llam_channel_try_recv_result(state->channel, &out));
    expect_runtime_owner_mismatch(&state->core,
                                  "channel close cross-runtime",
                                  llam_channel_close(state->channel));
    expect_runtime_owner_mismatch(&state->core,
                                  "channel destroy cross-runtime",
                                  llam_channel_destroy(state->channel));
    memset(&op, 0, sizeof(op));
    op.kind = LLAM_SELECT_OP_RECV;
    op.channel = state->channel;
    op.recv_out = &out;
    expect_runtime_owner_mismatch(&state->core,
                                  "channel select cross-runtime",
                                  llam_channel_select(&op, 1U, 0U, &selected));
    raw_channel->owner_runtime = owner;
    llam_channel_end_public_op(raw_channel);

    raw_mutex = llam_mutex_resolve_public_handle(state->mutex);
    if (raw_mutex == NULL) {
        task_fail(&state->core, "mutex owner diagnostic resolve", errno);
        return;
    }
    owner = raw_mutex->owner_runtime;
    raw_mutex->owner_runtime = &state->fake_runtime;
    expect_runtime_owner_mismatch(&state->core,
                                  "mutex trylock cross-runtime",
                                  llam_mutex_trylock(state->mutex));
    expect_runtime_owner_mismatch(&state->core,
                                  "mutex destroy cross-runtime",
                                  llam_mutex_destroy(state->mutex));
    raw_mutex->owner_runtime = owner;
    llam_mutex_end_public_op(raw_mutex);

    raw_cond = llam_cond_resolve_public_handle(state->cond);
    if (raw_cond == NULL) {
        task_fail(&state->core, "cond owner diagnostic resolve", errno);
        return;
    }
    owner = raw_cond->owner_runtime;
    raw_cond->owner_runtime = &state->fake_runtime;
    expect_runtime_owner_mismatch(&state->core,
                                  "cond signal cross-runtime",
                                  llam_cond_signal(state->cond));
    expect_runtime_owner_mismatch(&state->core,
                                  "cond broadcast cross-runtime",
                                  llam_cond_broadcast(state->cond));
    expect_runtime_owner_mismatch(&state->core,
                                  "cond destroy cross-runtime",
                                  llam_cond_destroy(state->cond));
    raw_cond->owner_runtime = owner;
    llam_cond_end_public_op(raw_cond);

    if (llam_cancel_token_retain_task_ref(state->token, &raw_token) != 0) {
        task_fail(&state->core, "cancel token owner diagnostic resolve", errno);
        return;
    }
    owner = raw_token->owner_runtime;
    raw_token->owner_runtime = &state->fake_runtime;
    expect_runtime_owner_mismatch(&state->core,
                                  "cancel token query cross-runtime",
                                  llam_cancel_token_is_cancelled(state->token));
    expect_runtime_owner_mismatch(&state->core,
                                  "cancel token cancel cross-runtime",
                                  llam_cancel_token_cancel(state->token));
    expect_runtime_owner_mismatch(&state->core,
                                  "cancel token destroy cross-runtime",
                                  llam_cancel_token_destroy(state->token));
    raw_token->owner_runtime = owner;
    llam_cancel_token_release_task_ref(raw_token);

    raw_group = llam_task_group_resolve_public_handle(state->group);
    if (raw_group == NULL) {
        task_fail(&state->core, "task group owner diagnostic resolve", errno);
        return;
    }
    owner = raw_group->owner_runtime;
    raw_group->owner_runtime = &state->fake_runtime;
    expect_runtime_owner_mismatch(&state->core,
                                  "task group cancel cross-runtime",
                                  llam_task_group_cancel(state->group));
    expect_runtime_owner_mismatch(&state->core,
                                  "task group join cross-runtime",
                                  llam_task_group_join_until(state->group, 0U));
    expect_runtime_owner_mismatch(&state->core,
                                  "task group destroy cross-runtime",
                                  llam_task_group_destroy(state->group));
    raw_group->owner_runtime = owner;
    llam_task_group_end_public_op(raw_group);

}

static int test_runtime_owner_mismatch_diagnostics(void) {
    owner_diag_state_t state;
    llam_runtime_opts_t runtime_opts;
    llam_runtime_t *owner;
    llam_task_t *diag_task = NULL;
    bool ran_runtime = false;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.core.failures, 0U);
    atomic_init(&state.core.ran, 0U);
    atomic_init(&state.core.blocking_calls, 0U);
    state.payload = 42;

    memset(&runtime_opts, 0, sizeof(runtime_opts));
    runtime_opts.deterministic = 1U;
    runtime_opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;

    if (llam_runtime_init(&runtime_opts) != 0) {
        return test_fail_errno("llam_runtime_init for owner diagnostics failed");
    }
    state.channel = llam_channel_create(1U);
    state.mutex = llam_mutex_create();
    state.cond = llam_cond_create();
    state.token = llam_cancel_token_create();
    state.group = llam_task_group_create();
    if (state.channel == NULL ||
        state.mutex == NULL ||
        state.cond == NULL ||
        state.token == NULL ||
        state.group == NULL) {
        rc = test_fail_errno("object create for owner diagnostics failed");
        goto cleanup;
    }
    state.target = llam_spawn(owner_diag_target_task, &state.core, NULL);
    if (state.target == NULL) {
        rc = test_fail_errno("spawn for owner diagnostics failed");
        goto cleanup;
    }
    {
        llam_task_t *raw_target = llam_task_resolve_public_handle(state.target);

        if (raw_target == NULL) {
            rc = test_fail_errno("resolve task for owner diagnostics failed");
            goto cleanup;
        }
        owner = raw_target->owner_runtime;
        raw_target->owner_runtime = &state.fake_runtime;
        expect_runtime_owner_mismatch(&state.core,
                                      "task join cross-runtime",
                                      llam_join_until(state.target, 0U));
        expect_runtime_owner_mismatch(&state.core,
                                      "task detach cross-runtime",
                                      llam_detach(state.target));
        raw_target->owner_runtime = owner;
        llam_task_end_public_op(raw_target);
    }
    if (atomic_load_explicit(&state.core.failures, memory_order_relaxed) != 0U) {
        rc = test_fail("task owner diagnostics did not fail with EXDEV");
        goto cleanup;
    }
    diag_task = llam_spawn(owner_diag_task, &state, NULL);
    if (diag_task == NULL) {
        rc = test_fail_errno("spawn for owner diagnostics failed");
        goto cleanup;
    }
    if (llam_run() != 0) {
        rc = test_fail_errno("llam_run for owner diagnostics failed");
        goto cleanup;
    }
    ran_runtime = true;
    if (llam_join(diag_task) != 0) {
        rc = test_fail_errno("llam_join for owner diagnostics failed");
        goto cleanup;
    }
    diag_task = NULL;
    if (llam_join(state.target) != 0) {
        rc = test_fail_errno("llam_join target for owner diagnostics failed");
        goto cleanup;
    }
    state.target = NULL;
    if (atomic_load_explicit(&state.core.failures, memory_order_relaxed) != 0U) {
        fprintf(stderr,
                "[test_runtime_core] owner diagnostics failed at %s errno=%d (%s)\n",
                state.core.first_case,
                state.core.first_errno,
                strerror(state.core.first_errno));
        goto cleanup;
    }
    if (atomic_load_explicit(&state.core.ran, memory_order_relaxed) != 1U) {
        rc = test_fail("owner diagnostics target task did not run");
        goto cleanup;
    }
    rc = 0;

cleanup:
    if (diag_task != NULL) {
        if (ran_runtime) {
            (void)llam_join(diag_task);
        } else {
            (void)llam_detach(diag_task);
        }
    }
    if (state.target != NULL) {
        if (ran_runtime) {
            (void)llam_join(state.target);
        } else {
            (void)llam_detach(state.target);
        }
    }
    if (state.group != NULL) {
        (void)llam_task_group_destroy(state.group);
    }
    if (state.token != NULL) {
        (void)llam_cancel_token_destroy(state.token);
    }
    if (state.cond != NULL) {
        (void)llam_cond_destroy(state.cond);
    }
    if (state.mutex != NULL) {
        (void)llam_mutex_destroy(state.mutex);
    }
    if (state.channel != NULL) {
        (void)llam_channel_destroy(state.channel);
    }
    llam_runtime_shutdown();
    return rc;
}

static int test_ex_option_prefixes(void) {
    core_state_t state;
    llam_runtime_opts_t runtime_opts;
    llam_spawn_opts_t spawn_opts;
    llam_task_t *task;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.blocking_calls, 0U);
    state.expected_flags = 0U;
    state.expected_class = LLAM_TASK_CLASS_LATENCY;

    memset(&runtime_opts, 0, sizeof(runtime_opts));
    runtime_opts.deterministic = 1U;
    runtime_opts.forced_yield_every = 1U;
    runtime_opts.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    if (llam_runtime_init_ex(&runtime_opts, offsetof(llam_runtime_opts_t, experimental_flags)) != 0) {
        return test_fail_errno("llam_runtime_init_ex prefix opts failed");
    }

    memset(&spawn_opts, 0, sizeof(spawn_opts));
    spawn_opts.task_class = LLAM_TASK_CLASS_LATENCY;
    spawn_opts.stack_class = LLAM_STACK_CLASS_LARGE;
    spawn_opts.flags = LLAM_SPAWN_F_PINNED;
    task = llam_spawn_ex(inspect_task, &state, &spawn_opts, offsetof(llam_spawn_opts_t, flags));
    if (task == NULL) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_spawn_ex prefix opts failed");
    }
    if (llam_task_class(task) != LLAM_TASK_CLASS_LATENCY ||
        (llam_task_flags(task) & LLAM_SPAWN_F_PINNED) != 0U) {
        llam_runtime_shutdown();
        return test_fail("llam_spawn_ex did not honor prefix-sized spawn opts");
    }
    if (llam_run() != 0) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_run for ex option prefixes failed");
    }
    if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
        fprintf(stderr,
                "[test_runtime_core] task failed at %s errno=%d (%s)\n",
                state.first_case,
                state.first_errno,
                strerror(state.first_errno));
        llam_runtime_shutdown();
        return 1;
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.blocking_calls, memory_order_relaxed) != 1U) {
        llam_runtime_shutdown();
        return test_fail("ex option prefix task did not run exactly once");
    }

    llam_runtime_shutdown();
    return 0;
}

static int test_errno_is_task_local_across_switches(void) {
    core_state_t state;
    errno_task_args_t task_a;
    errno_task_args_t task_b;
    llam_runtime_opts_t runtime_opts;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.blocking_calls, 0U);

    task_a.state = &state;
    task_a.yield_errno = ECHILD;
    task_a.sleep_errno = EADDRINUSE;
    task_b.state = &state;
    task_b.yield_errno = ENAMETOOLONG;
    task_b.sleep_errno = EALREADY;

    memset(&runtime_opts, 0, sizeof(runtime_opts));
    runtime_opts.deterministic = 1U;
    runtime_opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;

    if (llam_runtime_init(&runtime_opts) != 0) {
        return test_fail_errno("llam_runtime_init for errno isolation failed");
    }
    if (llam_spawn(errno_isolation_task, &task_a, NULL) == NULL ||
        llam_spawn(errno_isolation_task, &task_b, NULL) == NULL) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_spawn errno isolation task failed");
    }
    if (llam_run() != 0) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_run errno isolation failed");
    }
    if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
        fprintf(stderr,
                "[test_runtime_core] task failed at %s errno=%d (%s)\n",
                state.first_case,
                state.first_errno,
                strerror(state.first_errno));
        llam_runtime_shutdown();
        return 1;
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 2U) {
        llam_runtime_shutdown();
        return test_fail("errno isolation tasks did not both complete");
    }

    llam_runtime_shutdown();
    return 0;
}

static int test_direct_yield_failure_keeps_task_running(void) {
    core_state_t state;
    llam_runtime_opts_t runtime_opts;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.blocking_calls, 0U);

    memset(&runtime_opts, 0, sizeof(runtime_opts));
    runtime_opts.deterministic = 1U;
    runtime_opts.experimental_flags = LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    runtime_opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;

    if (llam_runtime_init(&runtime_opts) != 0) {
        return test_fail_errno("llam_runtime_init for direct yield failure failed");
    }
    if (llam_spawn(direct_yield_no_work_task, &state, NULL) == NULL) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_spawn direct yield no-work task failed");
    }
    if (llam_run() != 0) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_run direct yield failure failed");
    }
    if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
        fprintf(stderr,
                "[test_runtime_core] task failed at %s errno=%d (%s)\n",
                state.first_case,
                state.first_errno,
                strerror(state.first_errno));
        llam_runtime_shutdown();
        return 1;
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U) {
        llam_runtime_shutdown();
        return test_fail("direct yield no-work task did not run exactly once");
    }

    llam_runtime_shutdown();
    return 0;
}

#if LLAM_ARCH_AARCH64 && !LLAM_PLATFORM_WINDOWS
static int test_aarch64_simd_is_preserved_across_switches(void) {
    core_state_t state;
    aarch64_simd_state_t task_a;
    aarch64_simd_state_t task_b;
    llam_runtime_opts_t runtime_opts;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.blocking_calls, 0U);

    task_a.state = &state;
    task_a.expected_d8_bits = UINT64_C(0x3ff123456789abcd);
    task_b.state = &state;
    task_b.expected_d8_bits = UINT64_C(0x400fedcba9876543);

    memset(&runtime_opts, 0, sizeof(runtime_opts));
    runtime_opts.deterministic = 1U;
    runtime_opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;

    if (llam_runtime_init(&runtime_opts) != 0) {
        return test_fail_errno("llam_runtime_init for AArch64 SIMD preservation failed");
    }
    if (llam_spawn(aarch64_simd_preservation_task, &task_a, NULL) == NULL ||
        llam_spawn(aarch64_simd_preservation_task, &task_b, NULL) == NULL) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_spawn AArch64 SIMD preservation task failed");
    }
    if (llam_run() != 0) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_run AArch64 SIMD preservation failed");
    }
    if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
        fprintf(stderr,
                "[test_runtime_core] task failed at %s errno=%d (%s)\n",
                state.first_case,
                state.first_errno,
                strerror(state.first_errno));
        llam_runtime_shutdown();
        return 1;
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 2U) {
        llam_runtime_shutdown();
        return test_fail("AArch64 SIMD preservation tasks did not both complete");
    }

    llam_runtime_shutdown();
    return 0;
}
#endif

static int test_concurrent_join_contract(void) {
    core_state_t core;
    double_join_state_t state;
    llam_runtime_opts_t runtime_opts;

    memset(&core, 0, sizeof(core));
    atomic_init(&core.failures, 0U);
    atomic_init(&core.ran, 0U);
    atomic_init(&core.blocking_calls, 0U);

    memset(&state, 0, sizeof(state));
    state.state = &core;
    atomic_init(&state.target_done, 0U);
    atomic_init(&state.joined, 0U);
    atomic_init(&state.busy, 0U);

    memset(&runtime_opts, 0, sizeof(runtime_opts));
    runtime_opts.deterministic = 1U;
    runtime_opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;

    if (llam_runtime_init(&runtime_opts) != 0) {
        return test_fail_errno("llam_runtime_init for concurrent join failed");
    }
    state.target = llam_spawn(double_join_target_task, &state, NULL);
    if (state.target == NULL ||
        llam_spawn(double_join_waiter_task, &state, NULL) == NULL ||
        llam_spawn(double_join_waiter_task, &state, NULL) == NULL) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_spawn concurrent join tasks failed");
    }
    if (llam_run() != 0) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_run concurrent join failed");
    }
    if (atomic_load_explicit(&core.failures, memory_order_relaxed) != 0U) {
        fprintf(stderr,
                "[test_runtime_core] task failed at %s errno=%d (%s)\n",
                core.first_case,
                core.first_errno,
                strerror(core.first_errno));
        llam_runtime_shutdown();
        return 1;
    }
    if (atomic_load_explicit(&state.target_done, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.joined, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.busy, memory_order_relaxed) != 1U) {
        llam_runtime_shutdown();
        return test_fail("concurrent join did not produce one success and one EBUSY");
    }

    llam_runtime_shutdown();
    return 0;
}

#if LLAM_PLATFORM_POSIX
typedef struct init_race_barrier {
    pthread_mutex_t lock;
    pthread_cond_t cv;
    unsigned count;
    unsigned generation;
} init_race_barrier_t;

typedef struct init_race_state {
    init_race_barrier_t *barrier;
    atomic_uint successes;
    atomic_uint busy_failures;
    atomic_uint unexpected_failures;
} init_race_state_t;

static void init_race_barrier_wait(init_race_barrier_t *barrier) {
    unsigned generation;

    pthread_mutex_lock(&barrier->lock);
    generation = barrier->generation;
    barrier->count += 1U;
    if (barrier->count == 2U) {
        barrier->count = 0U;
        barrier->generation += 1U;
        pthread_cond_broadcast(&barrier->cv);
    } else {
        while (generation == barrier->generation) {
            pthread_cond_wait(&barrier->cv, &barrier->lock);
        }
    }
    pthread_mutex_unlock(&barrier->lock);
}

static void *init_race_thread(void *arg) {
    init_race_state_t *state = arg;
    llam_runtime_t *runtime = NULL;

    init_race_barrier_wait(state->barrier);
    if (llam_runtime_create(NULL, 0U, &runtime) == 0) {
        /*
         * Explicit runtime handles are independent. Destroy the handle inside
         * the creating thread so this race test does not leave heap runtimes in
         * the live registry while the next round starts.
         */
        llam_runtime_destroy(runtime);
        atomic_fetch_add_explicit(&state->successes, 1U, memory_order_relaxed);
    } else if (errno == EBUSY) {
        atomic_fetch_add_explicit(&state->busy_failures, 1U, memory_order_relaxed);
    } else {
        atomic_fetch_add_explicit(&state->unexpected_failures, 1U, memory_order_relaxed);
    }
    return NULL;
}

typedef struct init_shutdown_race_state {
    init_race_barrier_t *barrier;
    atomic_uint init_done;
    int init_rc;
    int init_errno;
} init_shutdown_race_state_t;

typedef struct init_stats_race_state {
    atomic_uint start;
    atomic_uint stop;
    atomic_uint failures;
    atomic_uint snapshots;
} init_stats_race_state_t;

typedef struct trace_race_state {
    llam_shard_t *shard;
    atomic_uint start;
} trace_race_state_t;

typedef struct run_race_state {
    init_race_barrier_t *barrier;
    atomic_uint *attempting;
    int rc;
    int err;
} run_race_state_t;

typedef struct run_hold_state {
    atomic_uint started;
    atomic_uint release;
} run_hold_state_t;

typedef struct spawn_race_state {
    atomic_uint ready;
    atomic_uint start;
    atomic_uint failures;
} spawn_race_state_t;

static char *test_dup_env_value(const char *name) {
    const char *value = getenv(name);
    size_t bytes;
    char *copy;

    if (value == NULL) {
        return NULL;
    }
    bytes = strlen(value) + 1U;
    copy = malloc(bytes);
    if (copy != NULL) {
        memcpy(copy, value, bytes);
    }
    return copy;
}

static void test_restore_env_value(const char *name, char *value) {
    if (value != NULL) {
        setenv(name, value, 1);
        free(value);
    } else {
        unsetenv(name);
    }
}

static void *init_stats_race_stats_thread(void *arg) {
    init_stats_race_state_t *state = arg;

    while (atomic_load_explicit(&state->start, memory_order_acquire) == 0U) {
    }
    while (atomic_load_explicit(&state->stop, memory_order_acquire) == 0U) {
        llam_runtime_stats_t stats;

        /*
         * Stats are part of the embedding boundary.  A host monitoring thread
         * may call this while another thread is still constructing the
         * singleton; the call must either wait for a stable snapshot or return
         * a valid pre-init snapshot, never walk partially published arrays.
         */
        if (llam_runtime_collect_stats_ex(&stats, LLAM_RUNTIME_STATS_CURRENT_SIZE) != 0) {
            atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
        } else {
            atomic_fetch_add_explicit(&state->snapshots, 1U, memory_order_relaxed);
        }
        usleep(100);
    }
    return NULL;
}

static int run_profile_yield_stats(uint32_t profile, llam_runtime_stats_t *stats) {
    core_state_t state;
    llam_runtime_opts_t opts;
    llam_task_t *task;

    if (stats == NULL) {
        return test_fail("profile yield stats output was NULL");
    }
    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.blocking_calls, 0U);
    memset(&opts, 0, sizeof(opts));
    opts.deterministic = 1U;
    opts.profile = profile;

    if (llam_runtime_init_ex(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return test_fail_errno("runtime init for profile yield stats failed");
    }
    task = llam_spawn(detached_task, &state, NULL);
    if (task == NULL) {
        llam_runtime_shutdown();
        return test_fail_errno("spawn for profile yield stats failed");
    }
    if (llam_run() != 0 || llam_join(task) != 0) {
        llam_runtime_shutdown();
        return test_fail_errno("run/join for profile yield stats failed");
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U) {
        llam_runtime_shutdown();
        return test_fail("profile yield stats task did not run");
    }
    memset(stats, 0, sizeof(*stats));
    if (llam_runtime_collect_stats_ex(stats, LLAM_RUNTIME_STATS_CURRENT_SIZE) != 0) {
        llam_runtime_shutdown();
        return test_fail_errno("collect stats for profile yield stats failed");
    }
    llam_runtime_shutdown();
    return 0;
}

static int test_direct_yield_auto_policy_is_profile_scoped(void) {
    char *saved_stats = test_dup_env_value("LLAM_DIRECT_HANDOFF_STATS");
    char *saved_handoff = test_dup_env_value("LLAM_YIELD_DIRECT_HANDOFF");
    llam_runtime_stats_t fast_stats;
    llam_runtime_stats_t debug_stats;
    int rc = 1;

    /*
     * The auto direct-yield policy depends on the current runtime profile.
     * Running release-fast first must not cache direct handoff on for a later
     * debug-safe runtime in the same host process.
     */
    if (setenv("LLAM_DIRECT_HANDOFF_STATS", "1", 1) != 0 ||
        unsetenv("LLAM_YIELD_DIRECT_HANDOFF") != 0) {
        rc = test_fail_errno("setenv for direct-yield profile policy failed");
        goto cleanup_env;
    }
    if (run_profile_yield_stats(LLAM_RUNTIME_PROFILE_RELEASE_FAST, &fast_stats) != 0 ||
        run_profile_yield_stats(LLAM_RUNTIME_PROFILE_DEBUG_SAFE, &debug_stats) != 0) {
        goto cleanup_env;
    }
    if (fast_stats.yield_direct_attempts == 0U) {
        rc = test_fail("release-fast runtime did not exercise direct-yield auto policy");
        goto cleanup_env;
    }
    if (debug_stats.yield_direct_attempts != 0U) {
        fprintf(stderr,
                "[test_runtime_core] debug-safe direct-yield attempts leaked from previous profile: %llu\n",
                (unsigned long long)debug_stats.yield_direct_attempts);
        rc = 1;
        goto cleanup_env;
    }
    rc = 0;

cleanup_env:
    test_restore_env_value("LLAM_DIRECT_HANDOFF_STATS", saved_stats);
    test_restore_env_value("LLAM_YIELD_DIRECT_HANDOFF", saved_handoff);
    return rc;
}

static void *init_shutdown_race_init_thread(void *arg) {
    init_shutdown_race_state_t *state = arg;
    llam_runtime_opts_t opts;

    memset(&opts, 0, sizeof(opts));
    opts.deterministic = 0U;
    opts.profile = LLAM_RUNTIME_PROFILE_BALANCED;
    init_race_barrier_wait(state->barrier);
    errno = 0;
    state->init_rc = llam_runtime_init_ex(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE);
    state->init_errno = errno;
    atomic_store_explicit(&state->init_done, 1U, memory_order_release);
    return NULL;
}

static void *init_shutdown_race_shutdown_thread(void *arg) {
    init_shutdown_race_state_t *state = arg;

    init_race_barrier_wait(state->barrier);
    while (atomic_load_explicit(&state->init_done, memory_order_acquire) == 0U) {
        llam_runtime_shutdown();
    }
    llam_runtime_shutdown();
    return NULL;
}

static void *trace_race_thread(void *arg) {
    trace_race_state_t *state = arg;

    while (atomic_load_explicit(&state->start, memory_order_acquire) == 0U) {
    }
    for (unsigned i = 0U; i < 20000U; ++i) {
        llam_trace_shard(state->shard,
                         NULL,
                         LLAM_TRACE_IDLE,
                         LLAM_TASK_STATE_RUNNABLE,
                         LLAM_TASK_STATE_PARKED,
                         LLAM_WAIT_NONE);
    }
    return NULL;
}

static void *run_race_thread(void *arg) {
    run_race_state_t *state = arg;

    init_race_barrier_wait(state->barrier);
    atomic_fetch_add_explicit(state->attempting, 1U, memory_order_release);
    errno = 0;
    state->rc = llam_run();
    state->err = errno;
    return NULL;
}

static void run_hold_task(void *arg) {
    run_hold_state_t *state = arg;

    atomic_store_explicit(&state->started, 1U, memory_order_release);
    while (atomic_load_explicit(&state->release, memory_order_acquire) == 0U) {
        llam_yield();
    }
}

static void spawn_race_noop_task(void *arg) {
    (void)arg;
}

static void *spawn_race_thread(void *arg) {
    spawn_race_state_t *state = arg;

    atomic_fetch_add_explicit(&state->ready, 1U, memory_order_release);
    while (atomic_load_explicit(&state->start, memory_order_acquire) == 0U) {
    }
    for (unsigned i = 0U; i < 2000U; ++i) {
        llam_task_t *task = llam_spawn(spawn_race_noop_task, NULL, NULL);

        if (task == NULL) {
            atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
            continue;
        }
        if (llam_detach(task) != 0) {
            atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
        }
    }
    return NULL;
}

static int test_concurrent_runtime_init_contract(void) {
    /*
     * Explicit runtime handles may be constructed concurrently. The lifecycle
     * gate still protects each partially initialized runtime object, but it
     * must not collapse independent handles back into singleton EBUSY behavior.
     */
    for (unsigned round = 0U; round < 128U; ++round) {
        init_race_barrier_t barrier = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0U, 0U};
        init_race_state_t state;
        pthread_t first;
        pthread_t second;
        int first_rc;
        int second_rc;

        memset(&state, 0, sizeof(state));
        state.barrier = &barrier;
        atomic_init(&state.successes, 0U);
        atomic_init(&state.busy_failures, 0U);
        atomic_init(&state.unexpected_failures, 0U);
        first_rc = pthread_create(&first, NULL, init_race_thread, &state);
        second_rc = first_rc == 0
                        ? pthread_create(&second, NULL, init_race_thread, &state)
                        : first_rc;
        if (first_rc != 0 || second_rc != 0) {
            if (first_rc == 0) {
                pthread_join(first, NULL);
            }
            errno = first_rc != 0 ? first_rc : second_rc;
            llam_runtime_shutdown();
            return test_fail_errno("pthread_create for concurrent runtime init failed");
        }
        pthread_join(first, NULL);
        pthread_join(second, NULL);
        if (atomic_load_explicit(&state.successes, memory_order_relaxed) != 2U ||
            atomic_load_explicit(&state.busy_failures, memory_order_relaxed) != 0U ||
            atomic_load_explicit(&state.unexpected_failures, memory_order_relaxed) != 0U) {
            fprintf(stderr,
                    "[test_runtime_core] concurrent runtime init race round=%u successes=%u busy=%u unexpected=%u\n",
                    round,
                    atomic_load_explicit(&state.successes, memory_order_relaxed),
                    atomic_load_explicit(&state.busy_failures, memory_order_relaxed),
                    atomic_load_explicit(&state.unexpected_failures, memory_order_relaxed));
            return 1;
        }
    }
    return 0;
}

static int test_concurrent_init_shutdown_contract(void) {
    /*
     * Shutdown is public and idempotent, so a host thread must not be able to
     * tear down partially published initialization storage while another host
     * thread still consumes it.  The prewarm knobs make the init window wide
     * enough to catch the race on unprotected builds.
     */
    char *saved_timer_prewarm = test_dup_env_value("LLAM_TIMER_HEAP_PREWARM");
    char *saved_stack_prewarm = test_dup_env_value("LLAM_STACK_CACHE_PREWARM");

    if (setenv("LLAM_TIMER_HEAP_PREWARM", "4096", 1) != 0 ||
        setenv("LLAM_STACK_CACHE_PREWARM", "64", 1) != 0) {
        test_restore_env_value("LLAM_TIMER_HEAP_PREWARM", saved_timer_prewarm);
        test_restore_env_value("LLAM_STACK_CACHE_PREWARM", saved_stack_prewarm);
        return test_fail_errno("setenv for init/shutdown race failed");
    }

    for (unsigned round = 0U; round < 64U; ++round) {
        init_race_barrier_t barrier = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0U, 0U};
        init_shutdown_race_state_t state;
        pthread_t init_thread;
        pthread_t shutdown_thread;
        int init_thread_rc;
        int shutdown_thread_rc;

        memset(&state, 0, sizeof(state));
        state.barrier = &barrier;
        atomic_init(&state.init_done, 0U);
        init_thread_rc = pthread_create(&init_thread, NULL, init_shutdown_race_init_thread, &state);
        shutdown_thread_rc = init_thread_rc == 0
                                 ? pthread_create(&shutdown_thread,
                                                  NULL,
                                                  init_shutdown_race_shutdown_thread,
                                                  &state)
                                 : init_thread_rc;
        if (init_thread_rc != 0 || shutdown_thread_rc != 0) {
            if (init_thread_rc == 0) {
                pthread_join(init_thread, NULL);
            }
            errno = init_thread_rc != 0 ? init_thread_rc : shutdown_thread_rc;
            llam_runtime_shutdown();
            test_restore_env_value("LLAM_TIMER_HEAP_PREWARM", saved_timer_prewarm);
            test_restore_env_value("LLAM_STACK_CACHE_PREWARM", saved_stack_prewarm);
            return test_fail_errno("pthread_create for init/shutdown race failed");
        }
        pthread_join(init_thread, NULL);
        pthread_join(shutdown_thread, NULL);
        if (state.init_rc != 0 && state.init_errno != EBUSY) {
            fprintf(stderr,
                    "[test_runtime_core] concurrent init/shutdown race round=%u rc=%d errno=%d (%s)\n",
                    round,
                    state.init_rc,
                    state.init_errno,
                    strerror(state.init_errno));
            llam_runtime_shutdown();
            test_restore_env_value("LLAM_TIMER_HEAP_PREWARM", saved_timer_prewarm);
            test_restore_env_value("LLAM_STACK_CACHE_PREWARM", saved_stack_prewarm);
            return 1;
        }
        llam_runtime_shutdown();
    }

    test_restore_env_value("LLAM_TIMER_HEAP_PREWARM", saved_timer_prewarm);
    test_restore_env_value("LLAM_STACK_CACHE_PREWARM", saved_stack_prewarm);
    return 0;
}

static int test_concurrent_init_stats_contract(void) {
    /*
     * Regression coverage for diagnostics racing the construction window.
     * active_shards is configured before the shard array is allocated; stats
     * must not trust that count unless lifecycle serialization and pointer
     * availability both say the array is safe to walk.
     */
    char *saved_timer_prewarm = test_dup_env_value("LLAM_TIMER_HEAP_PREWARM");
    char *saved_stack_prewarm = test_dup_env_value("LLAM_STACK_CACHE_PREWARM");

    if (setenv("LLAM_TIMER_HEAP_PREWARM", "8192", 1) != 0 ||
        setenv("LLAM_STACK_CACHE_PREWARM", "128", 1) != 0) {
        test_restore_env_value("LLAM_TIMER_HEAP_PREWARM", saved_timer_prewarm);
        test_restore_env_value("LLAM_STACK_CACHE_PREWARM", saved_stack_prewarm);
        return test_fail_errno("setenv for init/stats race failed");
    }

    for (unsigned round = 0U; round < 64U; ++round) {
        init_stats_race_state_t state;
        llam_runtime_opts_t opts;
        pthread_t stats_thread;
        int thread_rc;

        memset(&state, 0, sizeof(state));
        memset(&opts, 0, sizeof(opts));
        atomic_init(&state.start, 0U);
        atomic_init(&state.stop, 0U);
        atomic_init(&state.failures, 0U);
        atomic_init(&state.snapshots, 0U);
        opts.deterministic = 0U;
        opts.profile = LLAM_RUNTIME_PROFILE_BALANCED;

        thread_rc = pthread_create(&stats_thread, NULL, init_stats_race_stats_thread, &state);
        if (thread_rc != 0) {
            errno = thread_rc;
            llam_runtime_shutdown();
            test_restore_env_value("LLAM_TIMER_HEAP_PREWARM", saved_timer_prewarm);
            test_restore_env_value("LLAM_STACK_CACHE_PREWARM", saved_stack_prewarm);
            return test_fail_errno("pthread_create for init/stats race failed");
        }
        atomic_store_explicit(&state.start, 1U, memory_order_release);
        if (llam_runtime_init_ex(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
            atomic_store_explicit(&state.stop, 1U, memory_order_release);
            pthread_join(stats_thread, NULL);
            test_restore_env_value("LLAM_TIMER_HEAP_PREWARM", saved_timer_prewarm);
            test_restore_env_value("LLAM_STACK_CACHE_PREWARM", saved_stack_prewarm);
            return test_fail_errno("llam_runtime_init for init/stats race failed");
        }
        atomic_store_explicit(&state.stop, 1U, memory_order_release);
        pthread_join(stats_thread, NULL);
        if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
            fprintf(stderr,
                    "[test_runtime_core] init/stats race round=%u stats_failures=%u snapshots=%u\n",
                    round,
                    atomic_load_explicit(&state.failures, memory_order_relaxed),
                    atomic_load_explicit(&state.snapshots, memory_order_relaxed));
            llam_runtime_shutdown();
            test_restore_env_value("LLAM_TIMER_HEAP_PREWARM", saved_timer_prewarm);
            test_restore_env_value("LLAM_STACK_CACHE_PREWARM", saved_stack_prewarm);
            return 1;
        }
        llam_runtime_shutdown();
    }

    test_restore_env_value("LLAM_TIMER_HEAP_PREWARM", saved_timer_prewarm);
    test_restore_env_value("LLAM_STACK_CACHE_PREWARM", saved_stack_prewarm);
    return 0;
}

static int test_concurrent_trace_ring_contract(void) {
    /*
     * Trace events can be emitted by peer wake paths, I/O completions,
     * watchdogs, and shard owners.  Force concurrent producers onto one shard
     * so TSan-backed runs catch the diagnostic ring regressing to plain writes.
     */
    enum { TRACE_RACE_THREADS = 4U, TRACE_RACE_ITERS = 20000U };
    char *saved_trace = test_dup_env_value("LLAM_TRACE_EVENTS");
    pthread_t threads[TRACE_RACE_THREADS];
    bool joined[TRACE_RACE_THREADS];
    trace_race_state_t state;
    llam_runtime_opts_t opts;
    unsigned started = 0U;

    memset(joined, 0, sizeof(joined));
    memset(&state, 0, sizeof(state));
    memset(&opts, 0, sizeof(opts));
    atomic_init(&state.start, 0U);
    if (setenv("LLAM_TRACE_EVENTS", "1", 1) != 0) {
        test_restore_env_value("LLAM_TRACE_EVENTS", saved_trace);
        return test_fail_errno("setenv for trace race failed");
    }
    opts.deterministic = 1U;
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_runtime_init(&opts) != 0) {
        test_restore_env_value("LLAM_TRACE_EVENTS", saved_trace);
        return test_fail_errno("llam_runtime_init for trace race failed");
    }
    state.shard = &g_llam_runtime.shards[0];
    for (started = 0U; started < TRACE_RACE_THREADS; ++started) {
        int rc = pthread_create(&threads[started], NULL, trace_race_thread, &state);

        if (rc != 0) {
            errno = rc;
            atomic_store_explicit(&state.start, 1U, memory_order_release);
            for (unsigned i = 0U; i < started; ++i) {
                if (!joined[i]) {
                    (void)pthread_join(threads[i], NULL);
                    joined[i] = true;
                }
            }
            llam_runtime_shutdown();
            test_restore_env_value("LLAM_TRACE_EVENTS", saved_trace);
            return test_fail_errno("pthread_create for trace race failed");
        }
    }
    atomic_store_explicit(&state.start, 1U, memory_order_release);
    for (unsigned i = 0U; i < started; ++i) {
        if (pthread_join(threads[i], NULL) != 0) {
            llam_runtime_shutdown();
            test_restore_env_value("LLAM_TRACE_EVENTS", saved_trace);
            return test_fail_errno("pthread_join for trace race failed");
        }
        joined[i] = true;
    }
    if (atomic_load_explicit(&state.shard->trace_head, memory_order_acquire) <
        TRACE_RACE_THREADS * TRACE_RACE_ITERS) {
        llam_runtime_shutdown();
        test_restore_env_value("LLAM_TRACE_EVENTS", saved_trace);
        return test_fail("concurrent trace ring lost reservations");
    }
    llam_runtime_shutdown();
    test_restore_env_value("LLAM_TRACE_EVENTS", saved_trace);
    return 0;
}

static int test_concurrent_run_contract(void) {
    /*
     * llam_run() is the single scheduler driver for shard zero.  Concurrent
     * unmanaged callers must produce one runner and one EINVAL failure; two
     * successful callers would execute the same shard state at the same time.
     */
    for (unsigned round = 0U; round < 128U; ++round) {
        init_race_barrier_t barrier = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0U, 0U};
        atomic_uint attempting;
        run_hold_state_t hold;
        run_race_state_t first = {&barrier, &attempting, 123, 0};
        run_race_state_t second = {&barrier, &attempting, 123, 0};
        llam_runtime_opts_t opts;
        pthread_t first_thread;
        pthread_t second_thread;
        int first_rc;
        int second_rc;

        atomic_init(&attempting, 0U);
        atomic_init(&hold.started, 0U);
        atomic_init(&hold.release, 0U);
        memset(&opts, 0, sizeof(opts));
        opts.deterministic = 1U;
        opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
        if (llam_runtime_init(&opts) != 0) {
            return test_fail_errno("llam_runtime_init for concurrent run failed");
        }
        if (llam_spawn(run_hold_task, &hold, NULL) == NULL) {
            llam_runtime_shutdown();
            return test_fail_errno("llam_spawn for concurrent run hold task failed");
        }
        first_rc = pthread_create(&first_thread, NULL, run_race_thread, &first);
        second_rc = first_rc == 0
                        ? pthread_create(&second_thread, NULL, run_race_thread, &second)
                        : first_rc;
        if (first_rc != 0 || second_rc != 0) {
            if (first_rc == 0) {
                pthread_join(first_thread, NULL);
            }
            errno = first_rc != 0 ? first_rc : second_rc;
            llam_runtime_shutdown();
            return test_fail_errno("pthread_create for concurrent run failed");
        }
        for (unsigned i = 0U; i < 10000U &&
                              atomic_load_explicit(&hold.started, memory_order_acquire) == 0U;
             ++i) {
            usleep(100);
        }
        for (unsigned i = 0U; i < 10000U &&
                              atomic_load_explicit(&attempting, memory_order_acquire) < 2U;
             ++i) {
            usleep(100);
        }
        atomic_store_explicit(&hold.release, 1U, memory_order_release);
        pthread_join(first_thread, NULL);
        pthread_join(second_thread, NULL);
        if (first.rc == 0 && second.rc == 0) {
            fprintf(stderr,
                    "[test_runtime_core] concurrent run double-success round=%u\n",
                    round);
            llam_runtime_shutdown();
            return 1;
        }
        if ((first.rc != 0 && first.err != EINVAL) ||
            (second.rc != 0 && second.err != EINVAL)) {
            fprintf(stderr,
                    "[test_runtime_core] concurrent run unexpected result round=%u first=(%d,%d) second=(%d,%d)\n",
                    round,
                    first.rc,
                    first.err,
                    second.rc,
                    second.err);
            llam_runtime_shutdown();
            return 1;
        }
        llam_runtime_shutdown();
    }
    return 0;
}

static int test_concurrent_spawn_contract(void) {
    /*
     * Unmanaged embedders can submit work from multiple OS threads before a
     * scheduler driver starts.  Keep the placement cursor race-covered so TSan
     * catches regressions in shard selection.
     */
    enum { SPAWN_RACE_THREADS = 8U };
    pthread_t threads[SPAWN_RACE_THREADS];
    spawn_race_state_t state;
    llam_runtime_opts_t opts;
    unsigned started = 0U;

    memset(&state, 0, sizeof(state));
    memset(&opts, 0, sizeof(opts));
    atomic_init(&state.ready, 0U);
    atomic_init(&state.start, 0U);
    atomic_init(&state.failures, 0U);
    opts.deterministic = 0U;
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_runtime_init(&opts) != 0) {
        return test_fail_errno("llam_runtime_init for concurrent spawn failed");
    }
    for (started = 0U; started < SPAWN_RACE_THREADS; ++started) {
        int rc = pthread_create(&threads[started], NULL, spawn_race_thread, &state);

        if (rc != 0) {
            errno = rc;
            atomic_store_explicit(&state.start, 1U, memory_order_release);
            for (unsigned i = 0U; i < started; ++i) {
                pthread_join(threads[i], NULL);
            }
            llam_runtime_shutdown();
            return test_fail_errno("pthread_create for concurrent spawn failed");
        }
    }
    while (atomic_load_explicit(&state.ready, memory_order_acquire) != SPAWN_RACE_THREADS) {
    }
    atomic_store_explicit(&state.start, 1U, memory_order_release);
    for (unsigned i = 0U; i < started; ++i) {
        if (pthread_join(threads[i], NULL) != 0) {
            llam_runtime_shutdown();
            return test_fail_errno("pthread_join for concurrent spawn failed");
        }
    }
    if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
        llam_runtime_shutdown();
        return test_fail("concurrent unmanaged spawn produced failures");
    }
    if (llam_run() != 0) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_run for concurrent spawn failed");
    }
    llam_runtime_shutdown();
    return 0;
}
#endif

int main(void) {
    if (test_preinit_contracts() != 0 ||
        test_direct_yield_auto_policy_is_profile_scoped() != 0 ||
        test_runtime_handle_api() != 0 ||
        test_runtime_lifecycle_and_task_contracts() != 0 ||
        test_request_stop_returns_success() != 0 ||
        test_shutdown_from_task_requests_stop() != 0 ||
        test_runtime_owner_mismatch_diagnostics() != 0 ||
#if LLAM_PLATFORM_POSIX
        test_runtime_dump_while_blocking_job_active() != 0 ||
        test_concurrent_runtime_init_contract() != 0 ||
        test_concurrent_init_shutdown_contract() != 0 ||
        test_concurrent_init_stats_contract() != 0 ||
        test_concurrent_trace_ring_contract() != 0 ||
        test_concurrent_run_contract() != 0 ||
        test_concurrent_spawn_contract() != 0 ||
#endif
        test_detach_contract() != 0 ||
        test_ex_option_prefixes() != 0 ||
        test_errno_is_task_local_across_switches() != 0 ||
        test_direct_yield_failure_keeps_task_running() != 0 ||
#if LLAM_ARCH_AARCH64 && !LLAM_PLATFORM_WINDOWS
        test_aarch64_simd_is_preserved_across_switches() != 0 ||
#endif
        test_concurrent_join_contract() != 0) {
        return 1;
    }
    printf("[test_runtime_core] ok\n");
    return 0;
}
