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

#include <errno.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

static void *blocking_callback(void *arg) {
    core_state_t *state = arg;

    atomic_fetch_add_explicit(&state->blocking_calls, 1U, memory_order_relaxed);
    return arg;
}

static void *blocking_null_callback(void *arg) {
    (void)arg;
    return NULL;
}

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

static void detached_task(void *arg) {
    core_state_t *state = arg;

    llam_yield();
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static int test_preinit_contracts(void) {
    llam_runtime_stats_t stats;

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
    errno = 0;
    if (llam_task_set_class(999U) != -1 || errno != EINVAL) {
        return test_fail("llam_task_set_class invalid class did not fail with EINVAL");
    }
    errno = 0;
    if (llam_task_set_class(LLAM_TASK_CLASS_DEFAULT) != -1 || errno != ENOTSUP) {
        return test_fail("llam_task_set_class outside task did not fail with ENOTSUP");
    }

    errno = 0;
    if (llam_runtime_collect_stats(NULL) != -1 || errno != EINVAL) {
        return test_fail("llam_runtime_collect_stats(NULL) did not fail with EINVAL");
    }
    errno = 0;
    if (llam_runtime_collect_stats_ex(&stats, 0U) != -1 || errno != EINVAL) {
        return test_fail("llam_runtime_collect_stats_ex zero size did not fail with EINVAL");
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

int main(void) {
    if (test_preinit_contracts() != 0 ||
        test_runtime_lifecycle_and_task_contracts() != 0 ||
        test_request_stop_returns_success() != 0 ||
        test_detach_contract() != 0 ||
        test_ex_option_prefixes() != 0 ||
        test_errno_is_task_local_across_switches() != 0 ||
        test_concurrent_join_contract() != 0) {
        return 1;
    }
    printf("[test_runtime_core] ok\n");
    return 0;
}
