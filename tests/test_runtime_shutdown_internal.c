/**
 * @file tests/test_runtime_shutdown_internal.c
 * @brief Internal shutdown and watch-cleanup invariants.
 *
 * @details
 * This test intentionally reaches into private runtime state to exercise
 * teardown paths that are difficult to reach deterministically through the
 * public API.  Leak-enabled sanitizer jobs use this path to catch ownership
 * regressions in queued I/O readiness cleanup.
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

#include "runtime_internal.h"
#include "engine/runtime_watchdog_internal.h"
#include "io/runtime_io_api_internal.h"
#if LLAM_RUNTIME_BACKEND_KQUEUE
#include "io/darwin/runtime_io_watch_darwin_internal.h"
#elif LLAM_RUNTIME_BACKEND_LINUX
#include "io/linux/runtime_io_watch_linux_internal.h"
#elif LLAM_RUNTIME_BACKEND_WINDOWS
#include "io/windows/runtime_io_watch_windows_internal.h"
#endif
#include <errno.h>
#include <limits.h>
#if !LLAM_RUNTIME_BACKEND_WINDOWS
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_stack_cache_cases.inc"
static int fail_errno(const char *message) {
    fprintf(stderr, "test_runtime_shutdown_internal: %s: errno=%d (%s)\n", message, errno, strerror(errno));
    return 1;
}
static int fail_msg(const char *message) {
    fprintf(stderr, "test_runtime_shutdown_internal: %s\n", message);
    return 1;
}
#include "test_stack_vm_cases.inc"
static void *count_block_callback(void *arg) {
    atomic_uint *calls = arg;

    if (calls != NULL) {
        atomic_fetch_add_explicit(calls, 1U, memory_order_relaxed);
    }
    return arg;
}

#if defined(LLAM_ENABLE_TEST_HOOKS)
typedef struct block_pool_failure_state {
    llam_runtime_t *runtime;
    llam_task_t *tasks[2];
    atomic_uint callback_calls;
    atomic_uint callback_started;
    atomic_uint release_callback;
    atomic_uint task_returns;
    atomic_uint failures;
    unsigned confirmed_before_release;
    int first_rc;
    int first_errno;
} block_pool_failure_state_t;

static void *block_pool_failure_callback(void *arg) {
    block_pool_failure_state_t *state = arg;
    struct timespec interval = {0, 1000000L};

    atomic_fetch_add_explicit(&state->callback_calls, 1U, memory_order_relaxed);
    atomic_fetch_add_explicit(&state->callback_started, 1U, memory_order_release);
    while (atomic_load_explicit(&state->release_callback, memory_order_acquire) == 0U) {
        (void)nanosleep(&interval, NULL);
    }
    return state;
}

static void block_pool_first_create_failure_task(void *arg) {
    block_pool_failure_state_t *state = arg;
    void *result = (void *)(uintptr_t)1U;

    errno = 0;
    state->first_rc =
        llam_call_blocking_result(block_pool_failure_callback, state, &result);
    state->first_errno = errno;
    if (state->first_rc != -1 || state->first_errno != EAGAIN || result != NULL) {
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
    }
    atomic_fetch_add_explicit(&state->task_returns, 1U, memory_order_release);
}

static void block_pool_second_create_failure_child(void *arg) {
    block_pool_failure_state_t *state = arg;
    void *result = NULL;

    if (llam_call_blocking_result(block_pool_failure_callback, state, &result) != 0 ||
        result != state) {
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
    }
    atomic_fetch_add_explicit(&state->task_returns, 1U, memory_order_release);
}

static void block_pool_second_create_failure_parent(void *arg) {
    block_pool_failure_state_t *state = arg;
    uint64_t deadline_ns = llam_now_ns() + UINT64_C(5000000000);
    unsigned i;

    for (i = 0U; i < 2U; ++i) {
        state->tasks[i] = llam_runtime_spawn_ex(
            state->runtime,
            block_pool_second_create_failure_child,
            state,
            NULL,
            0U);
        if (state->tasks[i] == NULL) {
            atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
            break;
        }
    }

    while (i == 2U &&
           (atomic_load_explicit(&state->runtime->block_pending, memory_order_acquire) < 2U ||
            atomic_load_explicit(&state->callback_started, memory_order_acquire) < 1U)) {
        if (llam_now_ns() >= deadline_ns) {
            atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
            break;
        }
        llam_yield();
    }
    state->confirmed_before_release =
        atomic_load_explicit(&state->runtime->block_threads_started, memory_order_acquire);
    atomic_store_explicit(&state->release_callback, 1U, memory_order_release);
    for (i = 0U; i < 2U; ++i) {
        if (state->tasks[i] != NULL && llam_join(state->tasks[i]) != 0) {
            atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
        }
        state->tasks[i] = NULL;
    }
}

static void init_block_pool_failure_state(block_pool_failure_state_t *state) {
    memset(state, 0, sizeof(*state));
    atomic_init(&state->callback_calls, 0U);
    atomic_init(&state->callback_started, 0U);
    atomic_init(&state->release_callback, 0U);
    atomic_init(&state->task_returns, 0U);
    atomic_init(&state->failures, 0U);
}

static int init_zero_min_block_pool_runtime(llam_runtime_t **runtime) {
    llam_runtime_opts_t opts;

    if (runtime == NULL ||
        llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    opts.worker_min = 1U;
    opts.worker_count = 1U;
    opts.worker_max = 1U;
    opts.blocking_min = 0U;
    opts.blocking_max = 2U;
    return llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, runtime);
}

static int exercise_first_block_worker_create_failure_rolls_back_submission(void) {
    block_pool_failure_state_t state;
    llam_runtime_t *runtime = NULL;
    llam_task_t *task = NULL;
    int rc = 1;

    init_block_pool_failure_state(&state);
    llam_block_pool_test_reset_create_hook();
    llam_block_pool_test_fail_create_on(1U);
    if (init_zero_min_block_pool_runtime(&runtime) != 0) {
        rc = fail_errno("first-create failure runtime init failed");
        goto cleanup;
    }
    state.runtime = runtime;
    task = llam_runtime_spawn_ex(
        runtime, block_pool_first_create_failure_task, &state, NULL, 0U);
    if (task == NULL) {
        rc = fail_errno("first-create failure task spawn failed");
        goto cleanup;
    }
    if (llam_runtime_run_handle(runtime) != 0 || llam_join(task) != 0) {
        task = NULL;
        rc = fail_errno("first-create failure task did not return");
        goto cleanup;
    }
    task = NULL;

    if (atomic_load_explicit(&state.failures, memory_order_acquire) != 0U ||
        atomic_load_explicit(&state.task_returns, memory_order_acquire) != 1U ||
        atomic_load_explicit(&state.callback_calls, memory_order_acquire) != 0U ||
        atomic_load_explicit(&runtime->block_pending, memory_order_acquire) != 0U ||
        atomic_load_explicit(&runtime->block_threads_started, memory_order_acquire) != 0U ||
        runtime->block_head != NULL || runtime->block_tail != NULL ||
        llam_block_pool_test_create_calls() != 1U) {
        rc = fail_msg("first block-worker create failure published or stranded a job");
        goto cleanup;
    }
    rc = 0;

cleanup:
    atomic_store_explicit(&state.release_callback, 1U, memory_order_release);
    if (task != NULL) {
        (void)llam_detach(task);
    }
    llam_block_pool_test_reset_create_hook();
    llam_runtime_destroy(runtime);
    return rc;
}

static int exercise_second_block_worker_create_failure_uses_existing_worker(void) {
    block_pool_failure_state_t state;
    llam_runtime_stats_t stats;
    llam_runtime_t *runtime = NULL;
    llam_task_t *parent = NULL;
    int rc = 1;

    init_block_pool_failure_state(&state);
    llam_block_pool_test_reset_create_hook();
    llam_block_pool_test_fail_create_on(2U);
    if (init_zero_min_block_pool_runtime(&runtime) != 0) {
        rc = fail_errno("second-create failure runtime init failed");
        goto cleanup;
    }
    state.runtime = runtime;
    parent = llam_runtime_spawn_ex(
        runtime, block_pool_second_create_failure_parent, &state, NULL, 0U);
    if (parent == NULL) {
        rc = fail_errno("second-create failure parent spawn failed");
        goto cleanup;
    }
    if (llam_runtime_run_handle(runtime) != 0 || llam_join(parent) != 0) {
        parent = NULL;
        rc = fail_errno("second-create failure workload did not drain");
        goto cleanup;
    }
    parent = NULL;
    if (llam_runtime_collect_stats_ex_handle(runtime, &stats, sizeof(stats)) != 0) {
        rc = fail_errno("second-create failure stats failed");
        goto cleanup;
    }

    if (atomic_load_explicit(&state.failures, memory_order_acquire) != 0U ||
        atomic_load_explicit(&state.task_returns, memory_order_acquire) != 2U ||
        atomic_load_explicit(&state.callback_calls, memory_order_acquire) != 2U ||
        state.confirmed_before_release != 1U ||
        atomic_load_explicit(&runtime->block_threads_started, memory_order_acquire) != 1U ||
        atomic_load_explicit(&runtime->block_threads_entered, memory_order_acquire) != 1U ||
        atomic_load_explicit(&runtime->block_threads_live, memory_order_acquire) != 1U ||
        atomic_load_explicit(&runtime->block_pending, memory_order_acquire) != 0U ||
        stats.blocking_threads != 1U ||
        llam_block_pool_test_create_calls() != 2U) {
        rc = fail_msg("second block-worker create failure did not drain on the confirmed worker");
        goto cleanup;
    }
    rc = 0;

cleanup:
    atomic_store_explicit(&state.release_callback, 1U, memory_order_release);
    if (parent != NULL) {
        (void)llam_detach(parent);
    }
    llam_block_pool_test_reset_create_hook();
    llam_runtime_destroy(runtime);
    return rc;
}

static int exercise_block_pool_min_partial_failure_unwinds(void) {
    llam_runtime_opts_t opts;
    llam_runtime_t *runtime = NULL;
    int rc = 1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return fail_errno("partial blocking-min opts init failed");
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    opts.worker_min = 1U;
    opts.worker_count = 1U;
    opts.worker_max = 1U;
    opts.blocking_min = 2U;
    opts.blocking_max = 2U;

    llam_block_pool_test_reset_create_hook();
    llam_block_pool_test_fail_create_on(2U);
    errno = 0;
    if (llam_runtime_create(&opts,
                            LLAM_RUNTIME_OPTS_CURRENT_SIZE,
                            &runtime) != -1 ||
        errno != EAGAIN || runtime != NULL ||
        llam_block_pool_test_create_calls() != 2U) {
        rc = fail_msg("partial blocking-min create failure did not unwind confirmed threads");
        goto cleanup;
    }

    /*
     * A clean retry proves that the successful first thread from the failed
     * attempt was joined and its partial runtime handle was unregistered.
     */
    llam_block_pool_test_reset_create_hook();
    if (llam_runtime_create(&opts,
                            LLAM_RUNTIME_OPTS_CURRENT_SIZE,
                            &runtime) != 0) {
        rc = fail_errno("runtime create after partial blocking-min unwind failed");
        goto cleanup;
    }
    rc = 0;

cleanup:
    llam_block_pool_test_reset_create_hook();
    llam_runtime_destroy(runtime);
    return rc;
}

typedef struct affinity_policy_case {
    const char *name;
    unsigned policy;
    int capture_error;
    int apply_error;
    int restore_error;
    int expected_run_rc;
    int expected_errno;
    uint64_t expected_failures;
    unsigned expected_capture_calls;
    unsigned expected_apply_calls;
    unsigned expected_restore_calls;
} affinity_policy_case_t;

static int init_affinity_test_runtime(unsigned policy,
                                      unsigned worker_count,
                                      llam_runtime_t **runtime) {
    llam_runtime_opts_t opts;

    if (runtime == NULL ||
        llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    opts.worker_min = worker_count;
    opts.worker_count = worker_count;
    opts.worker_max = worker_count;
    opts.blocking_min = 0U;
    opts.blocking_max = 1U;
    opts.affinity_policy = policy;
    return llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, runtime);
}

static int run_affinity_policy_case(const affinity_policy_case_t *test_case) {
    llam_runtime_stats_t stats;
    llam_runtime_t *runtime = NULL;
    int run_rc;
    int run_errno;
    int rc = 1;

    llam_runtime_test_reset_affinity_hooks();
    llam_runtime_test_set_affinity_supported(1);
    llam_runtime_test_set_affinity_error(
        LLAM_TEST_AFFINITY_CAPTURE, test_case->capture_error);
    llam_runtime_test_set_affinity_error(
        LLAM_TEST_AFFINITY_APPLY, test_case->apply_error);
    llam_runtime_test_set_affinity_error(
        LLAM_TEST_AFFINITY_RESTORE, test_case->restore_error);

    if (init_affinity_test_runtime(test_case->policy, 1U, &runtime) != 0) {
        fprintf(stderr,
                "test_runtime_shutdown_internal: affinity case '%s' init failed: "
                "errno=%d (%s)\n",
                test_case->name,
                errno,
                strerror(errno));
        goto cleanup;
    }
    errno = 0;
    run_rc = llam_runtime_run_handle(runtime);
    run_errno = errno;
    if (llam_runtime_collect_stats_ex_handle(runtime, &stats, sizeof(stats)) != 0) {
        fprintf(stderr,
                "test_runtime_shutdown_internal: affinity case '%s' stats failed\n",
                test_case->name);
        goto cleanup;
    }

    if (run_rc != test_case->expected_run_rc ||
        (run_rc != 0 && run_errno != test_case->expected_errno) ||
        stats.affinity_failures != test_case->expected_failures ||
        llam_runtime_test_affinity_calls(LLAM_TEST_AFFINITY_CAPTURE) !=
            test_case->expected_capture_calls ||
        llam_runtime_test_affinity_calls(LLAM_TEST_AFFINITY_APPLY) !=
            test_case->expected_apply_calls ||
        llam_runtime_test_affinity_calls(LLAM_TEST_AFFINITY_RESTORE) !=
            test_case->expected_restore_calls) {
        fprintf(stderr,
                "test_runtime_shutdown_internal: affinity case '%s' mismatch: "
                "run=%d/%d failures=%llu calls=%u/%u/%u\n",
                test_case->name,
                run_rc,
                run_errno,
                (unsigned long long)stats.affinity_failures,
                llam_runtime_test_affinity_calls(LLAM_TEST_AFFINITY_CAPTURE),
                llam_runtime_test_affinity_calls(LLAM_TEST_AFFINITY_APPLY),
                llam_runtime_test_affinity_calls(LLAM_TEST_AFFINITY_RESTORE));
        goto cleanup;
    }
    rc = 0;

cleanup:
    llam_runtime_destroy(runtime);
    llam_runtime_test_reset_affinity_hooks();
    return rc;
}

static int exercise_affinity_policy_matrix(void) {
    static const affinity_policy_case_t cases[] = {
        {
            .name = "none skips platform",
            .policy = LLAM_RUNTIME_AFFINITY_NONE,
            .capture_error = EIO,
            .apply_error = EIO,
            .restore_error = EIO,
        },
        {
            .name = "prefer tolerates capture",
            .policy = LLAM_RUNTIME_AFFINITY_PREFER,
            .capture_error = EACCES,
            .expected_failures = 1U,
            .expected_capture_calls = 1U,
        },
        {
            .name = "require rejects capture",
            .policy = LLAM_RUNTIME_AFFINITY_REQUIRE,
            .capture_error = EACCES,
            .expected_run_rc = -1,
            .expected_errno = EACCES,
            .expected_failures = 1U,
            .expected_capture_calls = 1U,
        },
        {
            .name = "prefer tolerates apply",
            .policy = LLAM_RUNTIME_AFFINITY_PREFER,
            .apply_error = EPERM,
            .expected_failures = 1U,
            .expected_capture_calls = 1U,
            .expected_apply_calls = 1U,
            .expected_restore_calls = 1U,
        },
        {
            .name = "require rejects apply",
            .policy = LLAM_RUNTIME_AFFINITY_REQUIRE,
            .apply_error = EPERM,
            .expected_run_rc = -1,
            .expected_errno = EPERM,
            .expected_failures = 1U,
            .expected_capture_calls = 1U,
            .expected_apply_calls = 1U,
            .expected_restore_calls = 1U,
        },
        {
            .name = "prefer tolerates restore",
            .policy = LLAM_RUNTIME_AFFINITY_PREFER,
            .restore_error = EBUSY,
            .expected_failures = 1U,
            .expected_capture_calls = 1U,
            .expected_apply_calls = 1U,
            .expected_restore_calls = 1U,
        },
        {
            .name = "require rejects restore",
            .policy = LLAM_RUNTIME_AFFINITY_REQUIRE,
            .restore_error = EBUSY,
            .expected_run_rc = -1,
            .expected_errno = EBUSY,
            .expected_failures = 1U,
            .expected_capture_calls = 1U,
            .expected_apply_calls = 1U,
            .expected_restore_calls = 1U,
        },
        {
            .name = "prefer success",
            .policy = LLAM_RUNTIME_AFFINITY_PREFER,
            .expected_capture_calls = 1U,
            .expected_apply_calls = 1U,
            .expected_restore_calls = 1U,
        },
        {
            .name = "require success",
            .policy = LLAM_RUNTIME_AFFINITY_REQUIRE,
            .expected_capture_calls = 1U,
            .expected_apply_calls = 1U,
            .expected_restore_calls = 1U,
        },
    };

    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if (run_affinity_policy_case(&cases[i]) != 0) {
            return 1;
        }
    }
    return 0;
}

static int exercise_affinity_unsupported_policy(void) {
    llam_runtime_stats_t stats;
    llam_runtime_t *runtime = NULL;
    int rc = 1;

    llam_runtime_test_reset_affinity_hooks();
    llam_runtime_test_set_affinity_supported(0);
    errno = 0;
    if (init_affinity_test_runtime(
            LLAM_RUNTIME_AFFINITY_REQUIRE, 1U, &runtime) != -1 ||
        errno != ENOTSUP || runtime != NULL) {
        rc = fail_msg("required affinity did not fail init when unsupported");
        goto cleanup;
    }
    if (init_affinity_test_runtime(
            LLAM_RUNTIME_AFFINITY_PREFER, 1U, &runtime) != 0 ||
        llam_runtime_run_handle(runtime) != 0 ||
        llam_runtime_collect_stats_ex_handle(runtime, &stats, sizeof(stats)) != 0) {
        rc = fail_errno("preferred unsupported affinity did not continue");
        goto cleanup;
    }
    if (stats.affinity_failures != 1U ||
        llam_runtime_test_affinity_calls(LLAM_TEST_AFFINITY_CAPTURE) != 0U ||
        llam_runtime_test_affinity_calls(LLAM_TEST_AFFINITY_APPLY) != 0U ||
        llam_runtime_test_affinity_calls(LLAM_TEST_AFFINITY_RESTORE) != 0U) {
        rc = fail_msg("unsupported preferred affinity diagnostics were not exact");
        goto cleanup;
    }
    rc = 0;

cleanup:
    llam_runtime_destroy(runtime);
    llam_runtime_test_reset_affinity_hooks();
    return rc;
}

typedef struct affinity_run_exit_state {
    llam_runtime_t *runtime;
    bool fatal;
} affinity_run_exit_state_t;

static void affinity_run_exit_task(void *arg) {
    affinity_run_exit_state_t *state = arg;

    if (state->fatal) {
        llam_record_fatal(state->runtime, EIO);
    } else {
        (void)llam_runtime_request_stop();
    }
}

static int exercise_affinity_restore_on_task_exit(bool fatal) {
    affinity_run_exit_state_t state;
    llam_runtime_t *runtime = NULL;
    llam_task_t *task = NULL;
    int join_rc;
    int join_errno;
    int run_rc;
    int run_errno;
    int rc = 1;

    llam_runtime_test_reset_affinity_hooks();
    llam_runtime_test_set_affinity_supported(1);
    if (init_affinity_test_runtime(
            LLAM_RUNTIME_AFFINITY_PREFER, 1U, &runtime) != 0) {
        rc = fail_errno("affinity task-exit runtime init failed");
        goto cleanup;
    }
    state.runtime = runtime;
    state.fatal = fatal;
    task = llam_runtime_spawn_ex(runtime, affinity_run_exit_task, &state, NULL, 0U);
    if (task == NULL) {
        rc = fail_errno("affinity task-exit spawn failed");
        goto cleanup;
    }
    errno = 0;
    run_rc = llam_runtime_run_handle(runtime);
    run_errno = errno;
    errno = 0;
    join_rc = llam_join(task);
    join_errno = errno;
    if ((fatal && (join_rc != -1 || join_errno != EIO)) ||
        (!fatal && join_rc != 0)) {
        rc = fail_errno("affinity task-exit join result was unexpected");
        goto cleanup;
    }
    if (!fatal) {
        task = NULL;
    }
    if ((fatal && (run_rc != -1 || run_errno != EIO)) ||
        (!fatal && run_rc != 0) ||
        llam_runtime_test_affinity_calls(LLAM_TEST_AFFINITY_CAPTURE) != 1U ||
        llam_runtime_test_affinity_calls(LLAM_TEST_AFFINITY_APPLY) != 1U ||
        llam_runtime_test_affinity_calls(LLAM_TEST_AFFINITY_RESTORE) != 1U) {
        rc = fail_msg(fatal
                          ? "fatal worker exit did not restore driver affinity"
                          : "cooperative stop did not restore driver affinity");
        goto cleanup;
    }
    rc = 0;

cleanup:
    if (task != NULL) {
        (void)llam_detach(task);
    }
    llam_runtime_destroy(runtime);
    llam_runtime_test_reset_affinity_hooks();
    return rc;
}

static int exercise_affinity_restore_on_worker_create_failure(void) {
    llam_runtime_stats_t stats;
    llam_runtime_t *runtime = NULL;
    unsigned *allowed_cpus = NULL;
    unsigned allowed_cpu_count = llam_count_allowed_cpus(&allowed_cpus);
    int rc = 1;

    free(allowed_cpus);
    if (allowed_cpu_count < 2U) {
        return 0;
    }
    llam_runtime_test_reset_affinity_hooks();
    llam_runtime_test_reset_shard_create_hook();
    llam_runtime_test_set_affinity_supported(1);
    if (init_affinity_test_runtime(
            LLAM_RUNTIME_AFFINITY_PREFER, 2U, &runtime) != 0) {
        rc = fail_errno("affinity worker-create runtime init failed");
        goto cleanup;
    }
    llam_runtime_test_fail_shard_create_on(1U);
    errno = 0;
    if (llam_runtime_run_handle(runtime) != -1 || errno != EAGAIN ||
        llam_runtime_collect_stats_ex_handle(runtime, &stats, sizeof(stats)) != 0 ||
        stats.affinity_failures != 0U ||
        llam_runtime_test_shard_create_calls() != 1U ||
        llam_runtime_test_affinity_calls(LLAM_TEST_AFFINITY_CAPTURE) != 1U ||
        llam_runtime_test_affinity_calls(LLAM_TEST_AFFINITY_APPLY) != 0U ||
        llam_runtime_test_affinity_calls(LLAM_TEST_AFFINITY_RESTORE) != 1U) {
        rc = fail_msg("worker-create failure did not restore driver affinity exactly once");
        goto cleanup;
    }
    rc = 0;

cleanup:
    llam_runtime_test_reset_shard_create_hook();
    llam_runtime_destroy(runtime);
    llam_runtime_test_reset_affinity_hooks();
    return rc;
}

static int exercise_native_thread_counter_saturates(void) {
    llam_runtime_t *runtime = NULL;
    int rc = 1;

    if (init_affinity_test_runtime(
            LLAM_RUNTIME_AFFINITY_NONE, 1U, &runtime) != 0) {
        return fail_errno("native-thread overflow runtime init failed");
    }
    atomic_store_explicit(&runtime->scheduler_threads_live,
                          UINT_MAX,
                          memory_order_release);
    errno = 0;
    if (llam_runtime_native_thread_enter(
            runtime, &runtime->scheduler_threads_live) ||
        errno != EOVERFLOW ||
        atomic_load_explicit(&runtime->scheduler_threads_live,
                             memory_order_acquire) != UINT_MAX ||
        atomic_load_explicit(&runtime->fatal_errno,
                             memory_order_acquire) != EOVERFLOW) {
        rc = fail_msg("native-thread counter overflow did not saturate");
        goto cleanup;
    }
    rc = 0;

cleanup:
    atomic_store_explicit(&runtime->scheduler_threads_live,
                          0U,
                          memory_order_release);
    llam_runtime_destroy(runtime);
    return rc;
}

static int exercise_native_thread_counter_rejects_underflow(void) {
    llam_runtime_t *runtime = NULL;
    int rc = 1;

    if (init_affinity_test_runtime(
            LLAM_RUNTIME_AFFINITY_NONE, 1U, &runtime) != 0) {
        return fail_errno("native-thread underflow runtime init failed");
    }
    llam_runtime_native_thread_exit(runtime,
                                    &runtime->scheduler_threads_live);
    if (atomic_load_explicit(&runtime->scheduler_threads_live,
                             memory_order_acquire) != 0U ||
        atomic_load_explicit(&runtime->fatal_errno,
                             memory_order_acquire) != EINVAL) {
        rc = fail_msg("native-thread counter underflow did not fail closed");
        goto cleanup;
    }
    rc = 0;

cleanup:
    llam_runtime_destroy(runtime);
    return rc;
}
#endif

static int init_runtime(void) {
    llam_runtime_opts_t opts;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    return llam_runtime_init_ex(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE);
}

#if defined(LLAM_ENABLE_TEST_HOOKS) && !LLAM_RUNTIME_BACKEND_WINDOWS
typedef struct blocking_result_disposal_state {
    llam_cancel_token_t *token;
    llam_blocking_result_test_kind_t kind;
    atomic_uint gate_reached;
    atomic_uint connection_ready;
    atomic_uint created;
    atomic_uint release_created;
    atomic_uint discarded;
    uintptr_t created_value;
    uintptr_t discarded_value;
    struct addrinfo *addrinfo_result;
    llam_handle_t handle_result;
    llam_fd_t accept_result;
    int caller_result;
    int caller_errno;
    int gai_error;
    int cancel_result;
    int cancel_errno;
    int listener;
    int client;
    int connect_result;
    int connect_errno;
    struct sockaddr_in listener_address;
} blocking_result_disposal_state_t;

static void blocking_result_disposal_hook(
    llam_blocking_result_test_kind_t kind,
    llam_blocking_result_test_event_t event,
    uintptr_t value,
    void *context) {
    blocking_result_disposal_state_t *state = context;

    if (state == NULL || kind != state->kind) {
        return;
    }
    if (event == LLAM_BLOCKING_RESULT_TEST_DISCARDED) {
        state->discarded_value = value;
        atomic_store_explicit(
            &state->discarded, 1U, memory_order_release);
        return;
    }
    if (kind == LLAM_BLOCKING_RESULT_TEST_ACCEPT &&
        event == LLAM_BLOCKING_RESULT_TEST_BEFORE_CREATE) {
        if (atomic_exchange_explicit(
                &state->gate_reached,
                1U,
                memory_order_acq_rel) != 0U) {
            return;
        }
        while (atomic_load_explicit(
                   &state->connection_ready,
                   memory_order_acquire) == 0U) {
            sched_yield();
        }
        return;
    }
    if (event == LLAM_BLOCKING_RESULT_TEST_CREATED) {
        state->created_value = value;
        atomic_store_explicit(
            &state->created, 1U, memory_order_release);
        if (kind == LLAM_BLOCKING_RESULT_TEST_ACCEPT) {
            while (atomic_load_explicit(
                       &state->release_created,
                       memory_order_acquire) == 0U) {
                sched_yield();
            }
            return;
        }
    } else {
        return;
    }

    if (atomic_exchange_explicit(
            &state->gate_reached,
            1U,
            memory_order_acq_rel) != 0U) {
        return;
    }
    while (atomic_load_explicit(
               &state->release_created,
               memory_order_acquire) == 0U) {
        sched_yield();
    }
}

static void blocking_result_disposal_caller(void *context) {
    blocking_result_disposal_state_t *state = context;

    errno = 0;
    switch (state->kind) {
        case LLAM_BLOCKING_RESULT_TEST_GETADDRINFO:
            state->caller_result = llam_getaddrinfo_result(
                "localhost",
                "80",
                NULL,
                &state->addrinfo_result,
                &state->gai_error);
            break;
        case LLAM_BLOCKING_RESULT_TEST_OPEN:
            state->caller_result = llam_open_async(
                "/dev/null",
                O_RDONLY,
                0U,
                &state->handle_result);
            break;
        case LLAM_BLOCKING_RESULT_TEST_ACCEPT: {
            struct sockaddr_storage peer;
            socklen_t peer_size = sizeof(peer);

            state->accept_result = llam_accept(
                (llam_fd_t)state->listener,
                (struct sockaddr *)&peer,
                &peer_size);
            state->caller_result =
                LLAM_FD_IS_INVALID(state->accept_result)
                    ? -1
                    : 0;
            break;
        }
    }
    state->caller_errno = errno;
}

static void blocking_result_disposal_canceller(void *context) {
    blocking_result_disposal_state_t *state = context;

    while (atomic_load_explicit(
               &state->gate_reached,
               memory_order_acquire) == 0U) {
        llam_yield();
    }
    if (state->kind == LLAM_BLOCKING_RESULT_TEST_ACCEPT) {
        state->client = socket(AF_INET, SOCK_STREAM, 0);
        if (state->client >= 0) {
            errno = 0;
            state->connect_result = connect(
                state->client,
                (const struct sockaddr *)
                    &state->listener_address,
                sizeof(state->listener_address));
            state->connect_errno = errno;
        } else {
            state->connect_result = -1;
            state->connect_errno = errno;
        }
        atomic_store_explicit(
            &state->connection_ready, 1U, memory_order_release);
        if (state->connect_result == 0) {
            while (atomic_load_explicit(
                       &state->created,
                       memory_order_acquire) == 0U) {
                llam_yield();
            }
        }
    }
    errno = 0;
    state->cancel_result =
        llam_cancel_token_cancel(state->token);
    state->cancel_errno = errno;
    atomic_store_explicit(
        &state->release_created, 1U, memory_order_release);
}

static int blocking_result_disposal_listener_init(
    blocking_result_disposal_state_t *state) {
    socklen_t address_size =
        sizeof(state->listener_address);
    int one = 1;

    if (setenv(
            "LLAM_ACCEPT_DIRECT_BLOCKING", "1", 1) != 0) {
        return -1;
    }
    state->listener = socket(AF_INET, SOCK_STREAM, 0);
    if (state->listener < 0) {
        return -1;
    }
    (void)setsockopt(
        state->listener,
        SOL_SOCKET,
        SO_REUSEADDR,
        &one,
        sizeof(one));
    memset(
        &state->listener_address,
        0,
        sizeof(state->listener_address));
    state->listener_address.sin_family = AF_INET;
    state->listener_address.sin_addr.s_addr =
        htonl(INADDR_LOOPBACK);
    if (bind(
            state->listener,
            (const struct sockaddr *)
                &state->listener_address,
            sizeof(state->listener_address)) != 0 ||
        listen(state->listener, 8) != 0 ||
        getsockname(
            state->listener,
            (struct sockaddr *)
                &state->listener_address,
            &address_size) != 0) {
        return -1;
    }
    return 0;
}

static int run_blocking_result_disposal_case(
    llam_blocking_result_test_kind_t kind) {
    blocking_result_disposal_state_t state;
    llam_runtime_opts_t runtime_options;
    llam_spawn_opts_t spawn_options;
    llam_task_t *caller = NULL;
    llam_task_t *canceller = NULL;
    bool runtime_started = false;
    bool resource_still_live = false;
    int failed = 1;

    memset(&state, 0, sizeof(state));
    state.kind = kind;
    state.handle_result = LLAM_INVALID_HANDLE;
    state.accept_result = LLAM_INVALID_FD;
    state.caller_result = -2;
    state.cancel_result = -2;
    state.listener = -1;
    state.client = -1;
    state.connect_result = -2;
    atomic_init(&state.gate_reached, 0U);
    atomic_init(&state.connection_ready, 0U);
    atomic_init(&state.created, 0U);
    atomic_init(&state.release_created, 0U);
    atomic_init(&state.discarded, 0U);
    if (kind == LLAM_BLOCKING_RESULT_TEST_ACCEPT &&
        blocking_result_disposal_listener_init(&state) != 0) {
        goto cleanup;
    }
    state.token = llam_cancel_token_create();
    if (state.token == NULL ||
        llam_runtime_opts_init(
            &runtime_options,
            LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0 ||
        llam_spawn_opts_init(
            &spawn_options,
            LLAM_SPAWN_OPTS_CURRENT_SIZE) != 0) {
        goto cleanup;
    }
    runtime_options.deterministic = 1U;
    runtime_options.forced_yield_every = 1U;
    spawn_options.cancel_token = state.token;
    if (llam_runtime_init(&runtime_options) != 0) {
        goto cleanup;
    }
    runtime_started = true;
    llam_io_test_set_blocking_result_hook(
        blocking_result_disposal_hook, &state);
    caller = llam_spawn(
        blocking_result_disposal_caller,
        &state,
        &spawn_options);
    canceller = llam_spawn(
        blocking_result_disposal_canceller,
        &state,
        NULL);
    if (caller == NULL ||
        canceller == NULL ||
        llam_run() != 0 ||
        llam_join(caller) != 0 ||
        llam_join(canceller) != 0) {
        goto cleanup;
    }
    llam_io_test_set_blocking_result_hook(NULL, NULL);

    if (kind == LLAM_BLOCKING_RESULT_TEST_OPEN ||
        kind == LLAM_BLOCKING_RESULT_TEST_ACCEPT) {
        int descriptor = (int)state.created_value;

        errno = 0;
        resource_still_live =
            descriptor >= 0 &&
            fcntl(descriptor, F_GETFD) != -1;
    }
    if (state.caller_result == -1 &&
        state.caller_errno == ECANCELED &&
        state.cancel_result == 0 &&
        atomic_load_explicit(
            &state.gate_reached,
            memory_order_acquire) != 0U &&
        atomic_load_explicit(
            &state.created,
            memory_order_acquire) != 0U &&
        atomic_load_explicit(
            &state.discarded,
            memory_order_acquire) != 0U &&
        state.discarded_value == state.created_value &&
        !resource_still_live &&
        (kind != LLAM_BLOCKING_RESULT_TEST_ACCEPT ||
         state.connect_result == 0)) {
        failed = 0;
    } else {
        fprintf(
            stderr,
            "blocking result disposal kind=%u "
            "caller=%d/%d cancel=%d/%d connect=%d/%d "
            "gate=%u created=%u discarded=%u live=%u\n",
            (unsigned)kind,
            state.caller_result,
            state.caller_errno,
            state.cancel_result,
            state.cancel_errno,
            state.connect_result,
            state.connect_errno,
            atomic_load_explicit(
                &state.gate_reached,
                memory_order_acquire),
            atomic_load_explicit(
                &state.created,
                memory_order_acquire),
            atomic_load_explicit(
                &state.discarded,
                memory_order_acquire),
            resource_still_live ? 1U : 0U);
    }

cleanup:
    atomic_store_explicit(
        &state.connection_ready, 1U, memory_order_release);
    atomic_store_explicit(
        &state.release_created, 1U, memory_order_release);
    llam_io_test_set_blocking_result_hook(NULL, NULL);
    if (state.addrinfo_result != NULL) {
        freeaddrinfo(state.addrinfo_result);
        state.addrinfo_result = NULL;
    } else if (
        kind == LLAM_BLOCKING_RESULT_TEST_GETADDRINFO &&
        state.created_value != 0U &&
        atomic_load_explicit(
            &state.discarded,
            memory_order_acquire) == 0U) {
        freeaddrinfo(
            (struct addrinfo *)state.created_value);
    }
    if ((kind == LLAM_BLOCKING_RESULT_TEST_OPEN ||
         kind == LLAM_BLOCKING_RESULT_TEST_ACCEPT) &&
        atomic_load_explicit(
            &state.created,
            memory_order_acquire) != 0U) {
        int descriptor = (int)state.created_value;

        errno = 0;
        if (fcntl(descriptor, F_GETFD) != -1) {
            (void)close(descriptor);
        }
    }
    if (runtime_started) {
        llam_runtime_shutdown();
    }
    if (state.token != NULL &&
        llam_cancel_token_destroy(state.token) != 0) {
        failed = 1;
    }
    if (state.client >= 0) {
        (void)close(state.client);
    }
    if (state.listener >= 0) {
        (void)close(state.listener);
    }
    return failed;
}

static int exercise_canceled_blocking_results_are_disposed(void) {
    int failed = 0;

    if (run_blocking_result_disposal_case(
            LLAM_BLOCKING_RESULT_TEST_GETADDRINFO) != 0) {
        failed = 1;
    }
    if (run_blocking_result_disposal_case(
            LLAM_BLOCKING_RESULT_TEST_OPEN) != 0) {
        failed = 1;
    }
    if (run_blocking_result_disposal_case(
            LLAM_BLOCKING_RESULT_TEST_ACCEPT) != 0) {
        failed = 1;
    }
    return failed;
}
#else
static int exercise_canceled_blocking_results_are_disposed(void) {
    return 0;
}
#endif

#if defined(LLAM_ENABLE_TEST_HOOKS) && !LLAM_PLATFORM_WINDOWS && \
    LLAM_BUILD_RESEARCH
typedef enum close_watch_unpublish_kind {
    CLOSE_WATCH_UNPUBLISH_POLL = 0,
    CLOSE_WATCH_UNPUBLISH_ACCEPT = 1,
    CLOSE_WATCH_UNPUBLISH_RECV = 2,
} close_watch_unpublish_kind_t;

typedef struct close_watch_unpublish_state {
    llam_runtime_t runtime;
    llam_node_t node;
    llam_io_req_t req;
    void *watch;
    close_watch_unpublish_kind_t kind;
    atomic_uint hook_reached;
    atomic_uint hook_release;
    int close_result;
} close_watch_unpublish_state_t;

static bool close_watch_unpublish_completion_sink(
    llam_node_t *node,
    llam_io_req_t *req,
    unsigned completion_owner,
    llam_wait_reason_t *wake_reason,
    void *context) {
    (void)node;
    (void)req;
    (void)completion_owner;
    (void)wake_reason;
    (void)context;
    return true;
}

static void close_watch_unpublish_hook(
    llam_node_t *node,
    void *context) {
    close_watch_unpublish_state_t *state = context;

    if (state == NULL || node != &state->node) {
        return;
    }
    atomic_store_explicit(
        &state->hook_reached, 1U, memory_order_release);
    while (atomic_load_explicit(
               &state->hook_release,
               memory_order_acquire) == 0U) {
        sched_yield();
    }
}

static void *close_watch_unpublish_thread(void *context) {
    close_watch_unpublish_state_t *state = context;

    state->close_result =
        llam_forget_closed_fd_watch_state(
            &state->runtime,
            state->req.fd);
    return NULL;
}

static int run_close_watch_unpublish_case(
    close_watch_unpublish_kind_t kind) {
    close_watch_unpublish_state_t state;
    pthread_t closer;
    unsigned mode;
    bool unpublished;
    bool removed = true;
    int failed = 1;

    memset(&state, 0, sizeof(state));
    state.kind = kind;
    state.close_result = -2;
    atomic_init(&state.hook_reached, 0U);
    atomic_init(&state.hook_release, 0U);
    state.runtime.nodes = &state.node;
    state.runtime.active_nodes = 1U;
    state.node.runtime = &state.runtime;
    state.node.index = 0U;
    state.node.watch_lock_initialized = true;
    if (pthread_mutex_init(
            &state.node.watch_lock, NULL) != 0) {
        return 1;
    }
    llam_io_req_reset(
        &state.req,
        &state.runtime,
        UINT_MAX,
        UINT_MAX);
    state.req.fd = (llam_fd_t)(41 + (int)kind);
    state.req.completion_sink =
        close_watch_unpublish_completion_sink;
    atomic_store_explicit(
        &state.req.attached_node_index,
        0U,
        memory_order_release);

    pthread_mutex_lock(&state.node.watch_lock);
    if (kind == CLOSE_WATCH_UNPUBLISH_POLL) {
        llam_poll_watch_t *watch =
            calloc(1U, sizeof(*watch));

        if (watch == NULL) {
            pthread_mutex_unlock(&state.node.watch_lock);
            pthread_mutex_destroy(&state.node.watch_lock);
            return 1;
        }
        state.watch = watch;
        watch->fd = state.req.fd;
        watch->migrate_target_node_index = UINT_MAX;
        watch->accepts_waiters = true;
        watch->activating = true;
        watch->wait_head = &state.req;
        watch->wait_tail = &state.req;
        state.node.poll_watches = watch;
        state.req.kind = LLAM_IO_KIND_POLL;
        state.req.poll_watch = watch;
        atomic_store_explicit(
            &state.req.wait_mode,
            LLAM_IO_WAIT_MODE_POLL_WATCH,
            memory_order_release);
        if (llam_node_queue_control_locked(
                &state.node,
                LLAM_IO_CONTROL_POLL_ACTIVATE,
                watch) != 0) {
            pthread_mutex_unlock(&state.node.watch_lock);
            free(watch);
            pthread_mutex_destroy(&state.node.watch_lock);
            return 1;
        }
    } else if (kind == CLOSE_WATCH_UNPUBLISH_ACCEPT) {
        llam_accept_watch_t *watch =
            calloc(1U, sizeof(*watch));

        if (watch == NULL) {
            pthread_mutex_unlock(&state.node.watch_lock);
            pthread_mutex_destroy(&state.node.watch_lock);
            return 1;
        }
        state.watch = watch;
        watch->fd = state.req.fd;
        watch->migrate_target_node_index = UINT_MAX;
        watch->accepts_waiters = true;
        watch->activating = true;
        watch->wait_head = &state.req;
        watch->wait_tail = &state.req;
        state.node.accept_watches = watch;
        state.req.kind = LLAM_IO_KIND_ACCEPT;
        state.req.accept_watch = watch;
        atomic_store_explicit(
            &state.req.wait_mode,
            LLAM_IO_WAIT_MODE_ACCEPT_WATCH,
            memory_order_release);
        if (llam_node_queue_control_locked(
                &state.node,
                LLAM_IO_CONTROL_ACCEPT_ACTIVATE,
                watch) != 0) {
            pthread_mutex_unlock(&state.node.watch_lock);
            free(watch);
            pthread_mutex_destroy(&state.node.watch_lock);
            return 1;
        }
    } else {
        llam_recv_watch_t *watch =
            calloc(1U, sizeof(*watch));

        if (watch == NULL) {
            pthread_mutex_unlock(&state.node.watch_lock);
            pthread_mutex_destroy(&state.node.watch_lock);
            return 1;
        }
        state.watch = watch;
        watch->fd = state.req.fd;
        watch->migrate_target_node_index = UINT_MAX;
        watch->accepts_waiters = true;
        watch->activating = true;
        watch->wait_head = &state.req;
        watch->wait_tail = &state.req;
        state.node.recv_watches = watch;
        state.req.kind = LLAM_IO_KIND_READ;
        state.req.recv_watch = watch;
        atomic_store_explicit(
            &state.req.wait_mode,
            LLAM_IO_WAIT_MODE_RECV_WATCH,
            memory_order_release);
        if (llam_node_queue_control_locked(
                &state.node,
                LLAM_IO_CONTROL_RECV_ACTIVATE,
                watch) != 0) {
            pthread_mutex_unlock(&state.node.watch_lock);
            free(watch);
            pthread_mutex_destroy(&state.node.watch_lock);
            return 1;
        }
    }
    pthread_mutex_unlock(&state.node.watch_lock);

    llam_io_test_set_close_watch_unlocked_hook(
        close_watch_unpublish_hook, &state);
    if (pthread_create(
            &closer,
            NULL,
            close_watch_unpublish_thread,
            &state) != 0) {
        llam_io_test_set_close_watch_unlocked_hook(
            NULL, NULL);
        return 1;
    }
    for (unsigned attempt = 0U;
         attempt < 1000000U &&
         atomic_load_explicit(
             &state.hook_reached,
             memory_order_acquire) == 0U;
         attempt += 1U) {
        sched_yield();
    }

    mode = atomic_load_explicit(
        &state.req.wait_mode, memory_order_acquire);
    unpublished =
        mode == LLAM_IO_WAIT_MODE_NONE &&
        state.req.poll_watch == NULL &&
        state.req.accept_watch == NULL &&
        state.req.recv_watch == NULL;
    if (unpublished) {
        removed = llam_remove_watch_waiter_after_abort(
            &state.node,
            &state.req,
            kind == CLOSE_WATCH_UNPUBLISH_POLL
                ? LLAM_IO_WAIT_MODE_POLL_WATCH
                : kind == CLOSE_WATCH_UNPUBLISH_ACCEPT
                      ? LLAM_IO_WAIT_MODE_ACCEPT_WATCH
                      : LLAM_IO_WAIT_MODE_RECV_WATCH,
            true);
    }
    atomic_store_explicit(
        &state.hook_release, 1U, memory_order_release);
    (void)pthread_join(closer, NULL);
    llam_io_test_set_close_watch_unlocked_hook(NULL, NULL);

    if (atomic_load_explicit(
            &state.hook_reached,
            memory_order_acquire) != 0U &&
        unpublished &&
        !removed &&
        state.close_result == 0 &&
        state.node.poll_watches == NULL &&
        state.node.accept_watches == NULL &&
        state.node.recv_watches == NULL &&
        state.node.control_head == NULL &&
        state.node.control_tail == NULL) {
        failed = 0;
    } else {
        fprintf(
            stderr,
            "close watch remained published kind=%u "
            "hook=%u mode=%u raw=%p removed=%u close=%d\n",
            (unsigned)kind,
            atomic_load_explicit(
                &state.hook_reached,
                memory_order_acquire),
            mode,
            kind == CLOSE_WATCH_UNPUBLISH_POLL
                ? (void *)state.req.poll_watch
                : kind == CLOSE_WATCH_UNPUBLISH_ACCEPT
                      ? (void *)state.req.accept_watch
                      : (void *)state.req.recv_watch,
            removed ? 1U : 0U,
            state.close_result);
    }
    pthread_mutex_destroy(&state.node.watch_lock);
    return failed;
}

static int exercise_close_unpublishes_detached_watch_waiters(void) {
    int failed = 0;

    if (run_close_watch_unpublish_case(
            CLOSE_WATCH_UNPUBLISH_POLL) != 0) {
        failed = 1;
    }
    if (run_close_watch_unpublish_case(
            CLOSE_WATCH_UNPUBLISH_ACCEPT) != 0) {
        failed = 1;
    }
    if (run_close_watch_unpublish_case(
            CLOSE_WATCH_UNPUBLISH_RECV) != 0) {
        failed = 1;
    }
    return failed;
}
#else
static int exercise_close_unpublishes_detached_watch_waiters(void) {
    return 0;
}
#endif

#if defined(LLAM_ENABLE_TEST_HOOKS)
typedef struct park_completion_race_state {
    ssize_t result;
    int park_rc;
    int park_error;
    int setup_error;
    unsigned hook_calls;
} park_completion_race_state_t;

static void park_completion_snapshot_hook(
    llam_io_req_t *req,
    unsigned observed_wait_mode) {
    park_completion_race_state_t *state;
    unsigned owner_shard;

    if (req == NULL ||
        observed_wait_mode != LLAM_IO_WAIT_MODE_SUBMIT_QUEUE) {
        return;
    }
    state = req->buf;
    if (state == NULL) {
        return;
    }
    state->hook_calls += 1U;
    req->result = 37;
    req->error_code = 0;
    atomic_store_explicit(
        &req->wait_mode,
        LLAM_IO_WAIT_MODE_NONE,
        memory_order_release);
    owner_shard = atomic_load_explicit(
        &req->owner_shard,
        memory_order_acquire);
    llam_reinject_task_on_shard(
        req->owner_runtime,
        req->task,
        owner_shard,
        true,
        LLAM_TRACE_IO_COMPLETE,
        LLAM_WAIT_IO);
}

static void park_completion_race_task(void *opaque) {
    park_completion_race_state_t *state = opaque;
    llam_io_req_t *req = llam_api_io_req_acquire(g_llam_tls_shard);

    if (req == NULL) {
        state->setup_error = errno != 0 ? errno : ENOMEM;
        return;
    }
    req->task = g_llam_tls_task;
    req->buf = state;
    req->result = -1;
    req->error_code = 0;
    atomic_store_explicit(
        &req->owner_shard,
        g_llam_tls_shard->id,
        memory_order_release);
    atomic_store_explicit(
        &req->wait_mode,
        LLAM_IO_WAIT_MODE_SUBMIT_QUEUE,
        memory_order_release);
    if (!llam_task_set_io_tracking(
            g_llam_tls_task,
            req,
            g_llam_tls_shard->id)) {
        state->setup_error = errno != 0 ? errno : EIO;
        llam_api_io_req_release(g_llam_tls_shard, req);
        return;
    }

    llam_io_test_set_park_snapshot_hook(
        park_completion_snapshot_hook);
    errno = 0;
    state->park_rc = llam_park_io_req(
        req,
        false,
        0U,
        NULL);
    state->park_error = errno;
    state->result = req->result;
    llam_io_test_set_park_snapshot_hook(NULL);
    llam_api_io_req_release(g_llam_tls_shard, req);
}

static int exercise_park_completion_preserves_result(void) {
    park_completion_race_state_t state;
    llam_task_t *task;
    int run_rc;

    memset(&state, 0, sizeof(state));
    state.result = -1;
    state.park_rc = -1;
    if (init_runtime() != 0) {
        return fail_errno(
            "runtime init failed for park completion race");
    }
    task = llam_spawn(
        park_completion_race_task,
        &state,
        NULL);
    if (task == NULL) {
        llam_runtime_shutdown();
        return fail_errno(
            "task spawn failed for park completion race");
    }
    run_rc = llam_run();
    llam_io_test_set_park_snapshot_hook(NULL);
    llam_runtime_shutdown();
    if (run_rc != 0 ||
        state.setup_error != 0 ||
        state.hook_calls != 1U ||
        state.park_rc != 0 ||
        state.park_error != 0 ||
        state.result != 37) {
        fprintf(
            stderr,
            "park completion race failed: "
            "run=%d setup=%d hooks=%u park=%d/%d result=%zd\n",
            run_rc,
            state.setup_error,
            state.hook_calls,
            state.park_rc,
            state.park_error,
            state.result);
        return 1;
    }
    return 0;
}
#else
static int exercise_park_completion_preserves_result(void) {
    return 0;
}
#endif

#if LLAM_RUNTIME_BACKEND_KQUEUE || LLAM_RUNTIME_BACKEND_LINUX
static void close_if_valid(int *fd) {
    if (fd != NULL && *fd >= 0) {
        (void)close(*fd);
        *fd = -1;
    }
}

static int make_loopback_listener(int *listener_out) {
    struct sockaddr_in addr;
    int fd;
    int one = 1;

    if (listener_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, (socklen_t)sizeof(one));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0U);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (const struct sockaddr *)(const void *)&addr, (socklen_t)sizeof(addr)) != 0 ||
        listen(fd, 16) != 0) {
        close_if_valid(&fd);
        return -1;
    }
    *listener_out = fd;
    return 0;
}
#endif

static int exercise_recv_ready_copy_payload_shutdown(void) {
    llam_recv_watch_t *watch;
    llam_recv_ready_t *ready;
    unsigned char *payload;

    if (init_runtime() != 0) {
        return fail_errno("runtime init failed");
    }
    if (g_llam_runtime.nodes == NULL || g_llam_runtime.active_nodes == 0U) {
        llam_runtime_shutdown();
        return fail_msg("runtime initialized without an I/O node");
    }

    watch = calloc(1U, sizeof(*watch));
    ready = calloc(1U, sizeof(*ready));
    payload = malloc(4096U);
    if (watch == NULL || ready == NULL || payload == NULL) {
        free(payload);
        free(ready);
        free(watch);
        llam_runtime_shutdown();
        return fail_errno("allocation failed");
    }
    memset(payload, 0x5a, 4096U);

    /*
     * Simulate a copied recv completion that shutdown owns.  If teardown only
     * frees the ready node and forgets copy_data, leak-enabled jobs report it.
     */
    ready->copy_data = payload;
    ready->copy_capacity = 4096U;
    ready->size = 4096U;
    ready->has_buffer = false;
    watch->ready_head = ready;
    watch->ready_tail = ready;
    watch->ready_depth = 1U;

    {
        int lock_rc = pthread_mutex_lock(&g_llam_runtime.nodes[0].watch_lock);

        if (lock_rc != 0) {
            errno = lock_rc;
            free(payload);
            free(ready);
            free(watch);
            llam_runtime_shutdown();
            return fail_errno("watch lock failed");
        }
    }
    watch->next = g_llam_runtime.nodes[0].recv_watches;
    g_llam_runtime.nodes[0].recv_watches = watch;
    {
        int unlock_rc = pthread_mutex_unlock(&g_llam_runtime.nodes[0].watch_lock);

        if (unlock_rc != 0) {
            errno = unlock_rc;
            llam_runtime_shutdown();
            return fail_errno("watch unlock failed");
        }
    }

    llam_runtime_shutdown();
    return 0;
}

static int exercise_recv_ready_pop_without_transfer(void) {
#if LLAM_RUNTIME_BACKEND_KQUEUE || LLAM_RUNTIME_BACKEND_LINUX
    llam_recv_watch_t watch;
    unsigned char data[64];
    size_t size = 0U;

    memset(&watch, 0, sizeof(watch));
    memset(data, 0x33, sizeof(data));
#if LLAM_RUNTIME_BACKEND_KQUEUE
    if (!llam_recv_watch_push_ready_copy(&watch, data, sizeof(data))) {
        return fail_errno("recv ready copy push failed");
    }
#else
    {
        unsigned char *copy = malloc(sizeof(data));

        if (copy == NULL) {
            return fail_errno("recv ready copy allocation failed");
        }
        memcpy(copy, data, sizeof(data));
        if (!llam_recv_watch_push_ready(&watch, sizeof(data), 0U, false, UINT_MAX, copy, sizeof(data))) {
            return fail_errno("recv ready push failed");
        }
    }
#endif

    /*
     * Pop without requesting copy_data ownership.  The watch helper must free
     * the queued payload itself; leak-enabled sanitizer jobs catch regressions.
     */
    if (!llam_recv_watch_pop_ready(&watch, &size, NULL, NULL, NULL, NULL, NULL)) {
        return fail_errno("recv ready pop failed");
    }
    if (size != sizeof(data) || watch.ready_head != NULL || watch.ready_tail != NULL || watch.ready_depth != 0U) {
        return fail_msg("recv ready pop violated queue invariants");
    }
#endif
    return 0;
}

static int exercise_close_purges_accept_watch_ready_fds(void) {
#if LLAM_RUNTIME_BACKEND_KQUEUE || LLAM_RUNTIME_BACKEND_LINUX
    llam_node_t *node;
    llam_accept_watch_t *watch;
    int listener = -1;
    int ready_pipe[2] = {-1, -1};
    int watch_ready_fd = -1;
    int lock_rc;

    if (init_runtime() != 0) {
        return fail_errno("runtime init failed for close watch purge");
    }
    if (g_llam_runtime.nodes == NULL || g_llam_runtime.active_nodes == 0U) {
        llam_runtime_shutdown();
        return fail_msg("runtime initialized without an I/O node for close watch purge");
    }
    if (make_loopback_listener(&listener) != 0) {
        llam_runtime_shutdown();
        return fail_errno("listener setup failed for close watch purge");
    }
    if (pipe(ready_pipe) != 0) {
        close_if_valid(&listener);
        llam_runtime_shutdown();
        return fail_errno("ready fd setup failed for close watch purge");
    }

    node = &g_llam_runtime.nodes[0];
    lock_rc = pthread_mutex_lock(&node->watch_lock);
    if (lock_rc != 0) {
        errno = lock_rc;
        close_if_valid(&ready_pipe[0]);
        close_if_valid(&ready_pipe[1]);
        close_if_valid(&listener);
        llam_runtime_shutdown();
        return fail_errno("watch lock failed for close watch purge");
    }
    watch = llam_get_or_create_accept_watch_locked(node, listener);
    if (watch == NULL || !llam_accept_watch_push_ready_owned(watch, ready_pipe[0])) {
        (void)pthread_mutex_unlock(&node->watch_lock);
        close_if_valid(&ready_pipe[0]);
        close_if_valid(&ready_pipe[1]);
        close_if_valid(&listener);
        llam_runtime_shutdown();
        return fail_errno("accept watch ready setup failed for close watch purge");
    }
    /*
     * The watch owns ready_pipe[0] from this point.  The close-boundary cleanup
     * must release it before the runtime reaches full shutdown.
     */
    watch_ready_fd = ready_pipe[0];
    ready_pipe[0] = -1;
    lock_rc = pthread_mutex_unlock(&node->watch_lock);
    if (lock_rc != 0) {
        errno = lock_rc;
        close_if_valid(&ready_pipe[1]);
        close_if_valid(&listener);
        llam_runtime_shutdown();
        return fail_errno("watch unlock failed for close watch purge");
    }

    if (llam_close(listener) != 0) {
        close_if_valid(&ready_pipe[1]);
        listener = -1;
        llam_runtime_shutdown();
        return fail_errno("llam_close failed during close watch purge");
    }
    listener = -1;
    errno = 0;
    if (fcntl(watch_ready_fd, F_GETFD) != -1 || errno != EBADF) {
        close_if_valid(&watch_ready_fd);
        close_if_valid(&ready_pipe[1]);
        llam_runtime_shutdown();
        return fail_msg("llam_close did not purge accept-watch ready fd");
    }

    close_if_valid(&ready_pipe[1]);
    llam_runtime_shutdown();
#endif
    return 0;
}

static int exercise_host_close_purges_explicit_runtime_accept_watch_ready_fds(void) {
#if LLAM_RUNTIME_BACKEND_KQUEUE || LLAM_RUNTIME_BACKEND_LINUX
    llam_runtime_t *runtime = NULL;
    llam_node_t *node;
    llam_accept_watch_t *watch;
    int listener = -1;
    int ready_pipe[2] = {-1, -1};
    int watch_ready_fd = -1;
    int lock_rc;

    if (llam_runtime_create(NULL, 0U, &runtime) != 0) {
        return fail_errno("explicit runtime init failed for host close watch purge");
    }
    if (runtime == NULL || runtime->nodes == NULL || runtime->active_nodes == 0U) {
        llam_runtime_destroy(runtime);
        return fail_msg("explicit runtime initialized without an I/O node for host close watch purge");
    }
    if (make_loopback_listener(&listener) != 0) {
        llam_runtime_destroy(runtime);
        return fail_errno("listener setup failed for explicit host close watch purge");
    }
    if (pipe(ready_pipe) != 0) {
        close_if_valid(&listener);
        llam_runtime_destroy(runtime);
        return fail_errno("ready fd setup failed for explicit host close watch purge");
    }

    node = &runtime->nodes[0];
    lock_rc = pthread_mutex_lock(&node->watch_lock);
    if (lock_rc != 0) {
        errno = lock_rc;
        close_if_valid(&ready_pipe[0]);
        close_if_valid(&ready_pipe[1]);
        close_if_valid(&listener);
        llam_runtime_destroy(runtime);
        return fail_errno("watch lock failed for explicit host close watch purge");
    }
    watch = llam_get_or_create_accept_watch_locked(node, listener);
    if (watch == NULL || !llam_accept_watch_push_ready_owned(watch, ready_pipe[0])) {
        (void)pthread_mutex_unlock(&node->watch_lock);
        close_if_valid(&ready_pipe[0]);
        close_if_valid(&ready_pipe[1]);
        close_if_valid(&listener);
        llam_runtime_destroy(runtime);
        return fail_errno("accept watch ready setup failed for explicit host close watch purge");
    }
    /*
     * This models an embedder-owned fd closed from a host thread.  There is no
     * TLS task/shard cursor, so close-boundary cleanup must scan live explicit
     * runtimes instead of only the legacy default runtime.
     */
    watch_ready_fd = ready_pipe[0];
    ready_pipe[0] = -1;
    lock_rc = pthread_mutex_unlock(&node->watch_lock);
    if (lock_rc != 0) {
        errno = lock_rc;
        close_if_valid(&ready_pipe[1]);
        close_if_valid(&listener);
        llam_runtime_destroy(runtime);
        return fail_errno("watch unlock failed for explicit host close watch purge");
    }

    if (llam_close(listener) != 0) {
        close_if_valid(&ready_pipe[1]);
        listener = -1;
        llam_runtime_destroy(runtime);
        return fail_errno("llam_close failed during explicit host close watch purge");
    }
    listener = -1;
    errno = 0;
    if (fcntl(watch_ready_fd, F_GETFD) != -1 || errno != EBADF) {
        close_if_valid(&watch_ready_fd);
        close_if_valid(&ready_pipe[1]);
        llam_runtime_destroy(runtime);
        return fail_msg("host llam_close did not purge explicit-runtime accept-watch ready fd");
    }

    close_if_valid(&ready_pipe[1]);
    llam_runtime_destroy(runtime);
#endif
    return 0;
}

#if LLAM_RUNTIME_BACKEND_KQUEUE || LLAM_RUNTIME_BACKEND_LINUX
typedef struct managed_close_state {
    int fd;
    int rc;
    int error;
} managed_close_state_t;

static void managed_close_task(void *arg) {
    managed_close_state_t *state = arg;

    if (state == NULL) {
        return;
    }
    state->rc = llam_close(state->fd);
    state->error = errno;
}
#endif

static int exercise_managed_close_purges_peer_runtime_accept_watch_ready_fds(void) {
#if LLAM_RUNTIME_BACKEND_KQUEUE || LLAM_RUNTIME_BACKEND_LINUX
    llam_runtime_t *closer_runtime = NULL;
    llam_runtime_t *watch_runtime = NULL;
    llam_node_t *node;
    llam_accept_watch_t *watch;
    llam_task_t *task = NULL;
    managed_close_state_t close_state;
    int listener = -1;
    int ready_pipe[2] = {-1, -1};
    int watch_ready_fd = -1;
    int lock_rc;

    memset(&close_state, 0, sizeof(close_state));
    close_state.fd = -1;
    close_state.rc = -1;
    if (llam_runtime_create(NULL, 0U, &closer_runtime) != 0 ||
        llam_runtime_create(NULL, 0U, &watch_runtime) != 0) {
        llam_runtime_destroy(watch_runtime);
        llam_runtime_destroy(closer_runtime);
        return fail_errno("explicit runtime init failed for managed peer close purge");
    }
    if (watch_runtime == NULL || watch_runtime->nodes == NULL || watch_runtime->active_nodes == 0U) {
        llam_runtime_destroy(watch_runtime);
        llam_runtime_destroy(closer_runtime);
        return fail_msg("explicit watch runtime initialized without an I/O node for managed peer close purge");
    }
    if (make_loopback_listener(&listener) != 0) {
        llam_runtime_destroy(watch_runtime);
        llam_runtime_destroy(closer_runtime);
        return fail_errno("listener setup failed for managed peer close purge");
    }
    if (pipe(ready_pipe) != 0) {
        close_if_valid(&listener);
        llam_runtime_destroy(watch_runtime);
        llam_runtime_destroy(closer_runtime);
        return fail_errno("ready fd setup failed for managed peer close purge");
    }

    node = &watch_runtime->nodes[0];
    lock_rc = pthread_mutex_lock(&node->watch_lock);
    if (lock_rc != 0) {
        errno = lock_rc;
        close_if_valid(&ready_pipe[0]);
        close_if_valid(&ready_pipe[1]);
        close_if_valid(&listener);
        llam_runtime_destroy(watch_runtime);
        llam_runtime_destroy(closer_runtime);
        return fail_errno("watch lock failed for managed peer close purge");
    }
    watch = llam_get_or_create_accept_watch_locked(node, listener);
    if (watch == NULL || !llam_accept_watch_push_ready_owned(watch, ready_pipe[0])) {
        (void)pthread_mutex_unlock(&node->watch_lock);
        close_if_valid(&ready_pipe[0]);
        close_if_valid(&ready_pipe[1]);
        close_if_valid(&listener);
        llam_runtime_destroy(watch_runtime);
        llam_runtime_destroy(closer_runtime);
        return fail_errno("accept watch ready setup failed for managed peer close purge");
    }
    /*
     * The fd namespace is process-wide.  A managed task in one runtime can
     * close an embedder-owned descriptor while another runtime still has idle
     * readiness cached for that descriptor number.  close-boundary cleanup must
     * therefore cover every live runtime, not just the task's owner runtime.
     */
    watch_ready_fd = ready_pipe[0];
    ready_pipe[0] = -1;
    lock_rc = pthread_mutex_unlock(&node->watch_lock);
    if (lock_rc != 0) {
        errno = lock_rc;
        close_if_valid(&ready_pipe[1]);
        close_if_valid(&listener);
        llam_runtime_destroy(watch_runtime);
        llam_runtime_destroy(closer_runtime);
        return fail_errno("watch unlock failed for managed peer close purge");
    }

    close_state.fd = listener;
    task = llam_runtime_spawn_ex(closer_runtime, managed_close_task, &close_state, NULL, 0U);
    if (task == NULL ||
        llam_runtime_run_handle(closer_runtime) != 0 ||
        llam_join(task) != 0) {
        close_if_valid(&ready_pipe[1]);
        close_if_valid(&listener);
        llam_runtime_destroy(watch_runtime);
        llam_runtime_destroy(closer_runtime);
        return fail_errno("managed close task failed for peer close purge");
    }
    task = NULL;
    listener = -1;
    if (close_state.rc != 0) {
        errno = close_state.error;
        close_if_valid(&ready_pipe[1]);
        llam_runtime_destroy(watch_runtime);
        llam_runtime_destroy(closer_runtime);
        return fail_errno("managed llam_close failed for peer close purge");
    }

    errno = 0;
    if (fcntl(watch_ready_fd, F_GETFD) != -1 || errno != EBADF) {
        close_if_valid(&watch_ready_fd);
        close_if_valid(&ready_pipe[1]);
        llam_runtime_destroy(watch_runtime);
        llam_runtime_destroy(closer_runtime);
        return fail_msg("managed llam_close did not purge peer-runtime accept-watch ready fd");
    }

    close_if_valid(&ready_pipe[1]);
    llam_runtime_destroy(watch_runtime);
    llam_runtime_destroy(closer_runtime);
#endif
    return 0;
}

static int exercise_closed_watch_generation_is_deferred_and_not_reused(void) {
#if LLAM_PLATFORM_WINDOWS
    return 0;
#else
    llam_runtime_t runtime;
    llam_node_t node;
    llam_poll_watch_t *old_watch = NULL;
    llam_poll_watch_t *replacement = NULL;
    int fds[2] = {-1, -1};
    bool old_pinned = false;
    bool replacement_created = false;
    int rc = -1;

    memset(&runtime, 0, sizeof(runtime));
    memset(&node, 0, sizeof(node));
    runtime.nodes = &node;
    runtime.active_nodes = 1U;
    node.runtime = &runtime;
    if (pipe(fds) != 0 || pthread_mutex_init(&node.watch_lock, NULL) != 0) {
        goto done;
    }
    node.watch_lock_initialized = true;

    llam_fd_watch_lifecycle_lock();
    pthread_mutex_lock(&node.watch_lock);
    old_watch = llam_get_or_create_poll_watch_locked(&node, fds[0], POLLIN);
    if (old_watch != NULL) {
        llam_poll_watch_pin_locked(old_watch);
        old_pinned = true;
    }
    pthread_mutex_unlock(&node.watch_lock);
    llam_fd_watch_lifecycle_unlock();
    if (old_watch == NULL) {
        goto done;
    }

    /* Model the public close boundary before the kernel may recycle the number. */
    llam_fd_watch_lifecycle_lock();
    if (llam_forget_closed_fd_watch_state(&runtime, fds[0]) != 0) {
        llam_fd_watch_lifecycle_unlock();
        goto done;
    }
    llam_fd_watch_lifecycle_unlock();
    if (node.poll_watches != old_watch || !old_watch->destroy_pending ||
        old_watch->accepts_waiters || old_watch->lifetime_refs != 1U) {
        fprintf(stderr, "pinned closed watch was freed or remained attachable\n");
        goto done;
    }

    llam_fd_watch_lifecycle_lock();
    pthread_mutex_lock(&node.watch_lock);
    replacement = llam_get_or_create_poll_watch_locked(&node, fds[0], POLLIN);
    if (replacement != NULL && replacement != old_watch && replacement->accepts_waiters) {
        replacement_created = true;
        llam_poll_watch_unpin_locked(&node, old_watch);
        old_pinned = false;
        old_watch = NULL;
        llam_destroy_poll_watch_locked(&node, replacement);
        replacement = NULL;
    }
    pthread_mutex_unlock(&node.watch_lock);
    llam_fd_watch_lifecycle_unlock();
    if (!replacement_created || node.poll_watches != NULL ||
        node.retired_poll_watches != NULL) {
        fprintf(stderr, "closed watch generation was reused or not reclaimed safely\n");
        goto done;
    }
    rc = 0;

done:
    if (node.watch_lock_initialized) {
        pthread_mutex_lock(&node.watch_lock);
        if (old_pinned && old_watch != NULL) {
            llam_poll_watch_unpin_locked(&node, old_watch);
        }
        while (node.poll_watches != NULL) {
            llam_poll_watch_t *watch = node.poll_watches;

            llam_destroy_poll_watch_locked(&node, watch);
            if (node.poll_watches == watch) {
                break;
            }
        }
        while (node.retired_poll_watches != NULL) {
            llam_poll_watch_t *next = node.retired_poll_watches->next;

            free(node.retired_poll_watches);
            node.retired_poll_watches = next;
        }
        pthread_mutex_unlock(&node.watch_lock);
        pthread_mutex_destroy(&node.watch_lock);
    }
    if (fds[0] >= 0) {
        close(fds[0]);
    }
    if (fds[1] >= 0) {
        close(fds[1]);
    }
    return rc;
#endif
}

static int exercise_active_closed_watch_queues_one_deactivate(void) {
#if LLAM_PLATFORM_WINDOWS
    return 0;
#else
    llam_runtime_t runtime;
    llam_node_t node;
    llam_poll_watch_t *poll_watch = NULL;
    llam_accept_watch_t *accept_watch = NULL;
    llam_recv_watch_t *recv_watch = NULL;
    llam_io_control_op_t *control_head;
    int listener = -1;
    int rc = -1;

    memset(&runtime, 0, sizeof(runtime));
    memset(&node, 0, sizeof(node));
    runtime.nodes = &node;
    runtime.active_nodes = 1U;
    node.runtime = &runtime;
    node.event_fd = -1;
    if (make_loopback_listener(&listener) != 0 || pthread_mutex_init(&node.watch_lock, NULL) != 0) {
        goto done;
    }
    node.watch_lock_initialized = true;

    pthread_mutex_lock(&node.watch_lock);
    poll_watch = llam_get_or_create_poll_watch_locked(&node, listener, POLLIN);
    accept_watch = llam_get_or_create_accept_watch_locked(&node, listener);
    recv_watch = llam_get_or_create_recv_watch_locked(&node, listener);
    if (poll_watch != NULL && accept_watch != NULL && recv_watch != NULL) {
        poll_watch->active = true;
        accept_watch->active = true;
        recv_watch->active = true;
    }
    pthread_mutex_unlock(&node.watch_lock);
    if (poll_watch == NULL || accept_watch == NULL || recv_watch == NULL) {
        goto done;
    }

    llam_fd_watch_lifecycle_lock();
    if (llam_forget_closed_fd_watch_state(&runtime, listener) != 0) {
        llam_fd_watch_lifecycle_unlock();
        goto done;
    }
    llam_fd_watch_lifecycle_unlock();
    control_head = node.control_head;
    if (poll_watch->accepts_waiters || !poll_watch->active || !poll_watch->deactivate_queued ||
        accept_watch->accepts_waiters || !accept_watch->active || !accept_watch->deactivate_queued ||
        recv_watch->accepts_waiters || !recv_watch->active || !recv_watch->deactivate_queued) {
        fprintf(stderr, "active closed watches did not enter teardown state\n");
        goto done;
    }
    {
        unsigned seen = 0U;
        unsigned count = 0U;

        for (llam_io_control_op_t *op = control_head; op != NULL; op = op->next) {
            count += 1U;
            if (op->kind == LLAM_IO_CONTROL_POLL_DEACTIVATE && op->target == poll_watch) {
                seen |= 1U;
            } else if (op->kind == LLAM_IO_CONTROL_ACCEPT_DEACTIVATE && op->target == accept_watch) {
                seen |= 2U;
            } else if (op->kind == LLAM_IO_CONTROL_RECV_DEACTIVATE && op->target == recv_watch) {
                seen |= 4U;
            }
        }
        if (count != 3U || seen != 7U) {
            fprintf(stderr, "active closed watches did not queue exactly one deactivation each\n");
            goto done;
        }
    }

    llam_fd_watch_lifecycle_lock();
    if (llam_forget_closed_fd_watch_state(&runtime, listener) != 0) {
        llam_fd_watch_lifecycle_unlock();
        goto done;
    }
    llam_fd_watch_lifecycle_unlock();
    if (node.control_head != control_head) {
        fprintf(stderr, "repeated close-state purge queued duplicate deactivation\n");
        goto done;
    }
    {
        unsigned count = 0U;

        for (llam_io_control_op_t *op = node.control_head; op != NULL; op = op->next) {
            count += 1U;
        }
        if (count != 3U) {
            fprintf(stderr, "repeated close-state purge changed deactivation count\n");
            goto done;
        }
    }
    rc = 0;

done:
    if (node.watch_lock_initialized) {
        pthread_mutex_lock(&node.watch_lock);
        while (node.control_head != NULL) {
            llam_io_control_op_t *next = node.control_head->next;

            node.control_head->next = NULL;
            llam_io_control_op_destroy(&node, node.control_head);
            node.control_head = next;
        }
        node.control_tail = NULL;
        if (poll_watch != NULL && node.poll_watches == poll_watch) {
            poll_watch->active = false;
            poll_watch->activating = false;
            poll_watch->deactivate_queued = false;
            llam_destroy_poll_watch_locked(&node, poll_watch);
        }
        if (accept_watch != NULL && node.accept_watches == accept_watch) {
            accept_watch->active = false;
            accept_watch->activating = false;
            accept_watch->deactivate_queued = false;
            llam_destroy_accept_watch_locked(&node, accept_watch);
        }
        if (recv_watch != NULL && node.recv_watches == recv_watch) {
            recv_watch->active = false;
            recv_watch->activating = false;
            recv_watch->deactivate_queued = false;
            llam_destroy_recv_watch_locked(&node, recv_watch);
        }
        while (node.retired_poll_watches != NULL) {
            llam_poll_watch_t *next = node.retired_poll_watches->next;

            free(node.retired_poll_watches);
            node.retired_poll_watches = next;
        }
        while (node.retired_accept_watches != NULL) {
            llam_accept_watch_t *next = node.retired_accept_watches->next;

            free(node.retired_accept_watches);
            node.retired_accept_watches = next;
        }
        while (node.retired_recv_watches != NULL) {
            llam_recv_watch_t *next = node.retired_recv_watches->next;

            free(node.retired_recv_watches);
            node.retired_recv_watches = next;
        }
        pthread_mutex_unlock(&node.watch_lock);
        pthread_mutex_destroy(&node.watch_lock);
    }
    close_if_valid(&listener);
    return rc;
#endif
}

static int exercise_closed_watch_reclamation_stays_bounded(void) {
#if LLAM_RUNTIME_BACKEND_KQUEUE || LLAM_RUNTIME_BACKEND_LINUX
    llam_runtime_t runtime;
    llam_node_t node;
    int sockets[2] = {-1, -1};
    int rc = -1;

    memset(&runtime, 0, sizeof(runtime));
    memset(&node, 0, sizeof(node));
    runtime.nodes = &node;
    runtime.active_nodes = 1U;
    node.runtime = &runtime;
    node.event_fd = -1;
    if (pthread_mutex_init(&node.watch_lock, NULL) != 0) {
        return fail_errno("watch lock init failed for bounded reclamation");
    }
    node.watch_lock_initialized = true;

    for (unsigned generation = 0U; generation < 1024U; ++generation) {
        llam_poll_watch_t *poll_watch;
        llam_accept_watch_t *accept_watch;
        llam_recv_watch_t *recv_watch;

        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
            goto done;
        }
        pthread_mutex_lock(&node.watch_lock);
        poll_watch = llam_get_or_create_poll_watch_locked(&node, sockets[0], POLLIN);
        accept_watch = llam_get_or_create_accept_watch_locked(&node, sockets[0]);
        recv_watch = llam_get_or_create_recv_watch_locked(&node, sockets[0]);
        if (poll_watch != NULL && accept_watch != NULL && recv_watch != NULL) {
            /* Model a returned backend batch retaining each pointer across close. */
            llam_poll_watch_pin_locked(poll_watch);
            llam_accept_watch_pin_locked(accept_watch);
            llam_recv_watch_pin_locked(recv_watch);
        }
        pthread_mutex_unlock(&node.watch_lock);
        if (poll_watch == NULL || accept_watch == NULL || recv_watch == NULL) {
            goto done;
        }

        llam_fd_watch_lifecycle_lock();
        if (llam_forget_closed_fd_watch_state(&runtime, sockets[0]) != 0) {
            llam_fd_watch_lifecycle_unlock();
            goto done;
        }
        llam_fd_watch_lifecycle_unlock();

        pthread_mutex_lock(&node.watch_lock);
        llam_poll_watch_unpin_locked(&node, poll_watch);
        llam_accept_watch_unpin_locked(&node, accept_watch);
        llam_recv_watch_unpin_locked(&node, recv_watch);
        if (node.poll_watches != NULL || node.accept_watches != NULL ||
            node.recv_watches != NULL || node.retired_poll_watches != NULL ||
            node.retired_accept_watches != NULL || node.retired_recv_watches != NULL) {
            pthread_mutex_unlock(&node.watch_lock);
            fprintf(stderr, "closed watch quarantine grew at generation %u\n", generation);
            goto done;
        }
        pthread_mutex_unlock(&node.watch_lock);
        close_if_valid(&sockets[0]);
        close_if_valid(&sockets[1]);
    }
    rc = 0;

done:
    close_if_valid(&sockets[0]);
    close_if_valid(&sockets[1]);
    pthread_mutex_destroy(&node.watch_lock);
    return rc;
#else
    return 0;
#endif
}

static int exercise_darwin_closed_live_generation_deletes_knote(void) {
#if LLAM_RUNTIME_BACKEND_KQUEUE
    llam_runtime_t runtime;
    llam_node_t node;
    llam_poll_watch_t *watch = NULL;
    llam_io_control_op_t control;
    struct kevent event;
    struct timespec timeout = {0, 0};
    int fds[2] = {-1, -1};
    int event_count;
    int rc = -1;

    memset(&runtime, 0, sizeof(runtime));
    memset(&node, 0, sizeof(node));
    memset(&control, 0, sizeof(control));
    runtime.nodes = &node;
    runtime.active_nodes = 1U;
    node.runtime = &runtime;
    node.event_fd = -1;
    atomic_init(&node.pending_ops, 1U);
    if (pipe(fds) != 0 || pthread_mutex_init(&node.watch_lock, NULL) != 0) {
        goto done;
    }
    node.watch_lock_initialized = true;
    node.event_fd = kqueue();
    if (node.event_fd < 0) {
        goto done;
    }

    llam_fd_watch_lifecycle_lock();
    pthread_mutex_lock(&node.watch_lock);
    watch = llam_get_or_create_poll_watch_locked(&node, fds[0], POLLIN);
    if (watch != NULL) {
        watch->active = true;
    }
    pthread_mutex_unlock(&node.watch_lock);
    if (watch == NULL ||
        llam_darwin_poll_watch_change(&node,
                                      watch,
                                      EV_ADD | EV_ENABLE | LLAM_KQUEUE_WATCH_ONESHOT_FLAGS) != 0) {
        llam_fd_watch_lifecycle_unlock();
        goto done;
    }

    /*
     * Model platform close failure: close-boundary state was invalidated, but
     * the descriptor and its old-generation knote are still live.  Deactivate
     * must prove that identity and issue EV_DELETE before reclaiming storage.
     */
    pthread_mutex_lock(&node.watch_lock);
    watch->accepts_waiters = false;
    watch->deactivate_queued = true;
    pthread_mutex_unlock(&node.watch_lock);
    control.kind = LLAM_IO_CONTROL_POLL_DEACTIVATE;
    control.target = watch;
    llam_darwin_process_control(&node, &control);
    llam_fd_watch_lifecycle_unlock();
    watch = NULL;

    if (node.poll_watches != NULL ||
        atomic_load_explicit(&node.pending_ops, memory_order_acquire) != 0U) {
        fprintf(stderr, "live closed fd generation was not safely reclaimed\n");
        goto done;
    }
    if (write(fds[1], "x", 1U) != 1) {
        goto done;
    }
    do {
        event_count = kevent(node.event_fd, NULL, 0, &event, 1, &timeout);
    } while (event_count < 0 && errno == EINTR);
    if (event_count != 0) {
        fprintf(stderr, "closed live fd generation retained a dangling knote\n");
        goto done;
    }
    rc = 0;

done:
    if (node.event_fd >= 0) {
        close(node.event_fd);
        node.event_fd = -1;
    }
    if (node.watch_lock_initialized) {
        pthread_mutex_lock(&node.watch_lock);
        while (node.poll_watches != NULL) {
            llam_poll_watch_t *next = node.poll_watches->next;

            node.poll_watches->active = false;
            node.poll_watches->activating = false;
            node.poll_watches->deactivate_queued = false;
            node.poll_watches->lifetime_refs = 0U;
            llam_destroy_poll_watch_locked(&node, node.poll_watches);
            if (node.poll_watches != next) {
                break;
            }
        }
        pthread_mutex_unlock(&node.watch_lock);
        pthread_mutex_destroy(&node.watch_lock);
    }
    close_if_valid(&fds[0]);
    close_if_valid(&fds[1]);
    return rc;
#else
    return 0;
#endif
}

#if LLAM_RUNTIME_BACKEND_KQUEUE
static int exercise_darwin_poll_filter_replacement_case(bool mixed_read_write) {
    llam_runtime_t runtime;
    llam_node_t node;
    llam_poll_watch_t *old_watch = NULL;
    llam_recv_watch_t *read_replacement = NULL;
    llam_io_control_op_t control;
    struct kevent events[4];
    struct timespec timeout = {0, 0};
    int sockets[2] = {-1, -1};
    bool replacement_registered = false;
    int event_count;
    int rc = -1;

    memset(&runtime, 0, sizeof(runtime));
    memset(&node, 0, sizeof(node));
    memset(&control, 0, sizeof(control));
    runtime.nodes = &node;
    runtime.active_nodes = 1U;
    node.runtime = &runtime;
    node.event_fd = -1;
    atomic_init(&node.pending_ops, 1U);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0 ||
        pthread_mutex_init(&node.watch_lock, NULL) != 0) {
        goto done;
    }
    node.watch_lock_initialized = true;
    node.event_fd = kqueue();
    if (node.event_fd < 0) {
        goto done;
    }

    llam_fd_watch_lifecycle_lock();
    pthread_mutex_lock(&node.watch_lock);
    old_watch = llam_get_or_create_poll_watch_locked(&node,
                                                      sockets[0],
                                                      mixed_read_write ? (POLLIN | POLLOUT) : POLLOUT);
    read_replacement = llam_get_or_create_recv_watch_locked(&node, sockets[0]);
    if (old_watch != NULL && read_replacement != NULL) {
        old_watch->active = true;
    }
    pthread_mutex_unlock(&node.watch_lock);
    if (old_watch == NULL || read_replacement == NULL ||
        llam_darwin_poll_watch_change(&node,
                                      old_watch,
                                      EV_ADD | EV_ENABLE | LLAM_KQUEUE_WATCH_ONESHOT_FLAGS) != 0) {
        llam_fd_watch_lifecycle_unlock();
        goto done;
    }
    if (mixed_read_write) {
        if (llam_darwin_recv_watch_change(&node,
                                          read_replacement,
                                          EV_ADD | EV_ENABLE |
                                              LLAM_KQUEUE_WATCH_ONESHOT_FLAGS | EV_CLEAR) != 0) {
            llam_fd_watch_lifecycle_unlock();
            goto done;
        }
        replacement_registered = true;
    }

    pthread_mutex_lock(&node.watch_lock);
    old_watch->accepts_waiters = false;
    old_watch->deactivate_queued = true;
    pthread_mutex_unlock(&node.watch_lock);
    control.kind = LLAM_IO_CONTROL_POLL_DEACTIVATE;
    control.target = old_watch;
    llam_darwin_process_control(&node, &control);
    llam_fd_watch_lifecycle_unlock();
    old_watch = NULL;

    if (node.poll_watches != NULL || node.recv_watches != read_replacement ||
        atomic_load_explicit(&node.pending_ops, memory_order_acquire) != 0U) {
        fprintf(stderr, "filter-specific closed poll teardown retained old state\n");
        goto done;
    }
    if (!mixed_read_write) {
        do {
            event_count = kevent(node.event_fd, NULL, 0, events, 4, &timeout);
        } while (event_count < 0 && errno == EINTR);
        if (event_count != 0) {
            fprintf(stderr, "read-only replacement suppressed required EVFILT_WRITE delete\n");
            goto done;
        }
    } else {
        if (write(sockets[1], "r", 1U) != 1) {
            goto done;
        }
        do {
            event_count = kevent(node.event_fd, NULL, 0, events, 4, &timeout);
        } while (event_count < 0 && errno == EINTR);
        if (event_count != 1 || events[0].filter != EVFILT_READ ||
            llam_io_udata_tag((uint64_t)(uintptr_t)events[0].udata) != LLAM_IO_UDATA_RECV_WATCH ||
            llam_io_udata_ptr((uint64_t)(uintptr_t)events[0].udata) != read_replacement) {
            fprintf(stderr, "mixed poll teardown deleted replacement READ or retained old WRITE\n");
            goto done;
        }
    }
    rc = 0;

done:
    if (replacement_registered && node.event_fd >= 0 && read_replacement != NULL) {
        (void)llam_darwin_recv_watch_change(&node, read_replacement, EV_DELETE);
    }
    if (node.event_fd >= 0) {
        close(node.event_fd);
        node.event_fd = -1;
    }
    if (node.watch_lock_initialized) {
        pthread_mutex_lock(&node.watch_lock);
        while (node.poll_watches != NULL) {
            node.poll_watches->active = false;
            node.poll_watches->activating = false;
            node.poll_watches->deactivate_queued = false;
            node.poll_watches->lifetime_refs = 0U;
            llam_destroy_poll_watch_locked(&node, node.poll_watches);
        }
        while (node.recv_watches != NULL) {
            node.recv_watches->active = false;
            node.recv_watches->activating = false;
            node.recv_watches->deactivate_queued = false;
            node.recv_watches->lifetime_refs = 0U;
            llam_destroy_recv_watch_locked(&node, node.recv_watches);
        }
        pthread_mutex_unlock(&node.watch_lock);
        pthread_mutex_destroy(&node.watch_lock);
    }
    close_if_valid(&sockets[0]);
    close_if_valid(&sockets[1]);
    return rc;
}
#endif

static int exercise_darwin_poll_filter_specific_replacement(void) {
#if LLAM_RUNTIME_BACKEND_KQUEUE
    if (exercise_darwin_poll_filter_replacement_case(false) != 0 ||
        exercise_darwin_poll_filter_replacement_case(true) != 0) {
        return 1;
    }
#endif
    return 0;
}

#if LLAM_RUNTIME_BACKEND_KQUEUE || LLAM_RUNTIME_BACKEND_LINUX
typedef struct close_waiter_state {
    int read_fd;
    atomic_uint waiter_entered;
    int poll_rc;
    int poll_error;
    short poll_revents;
    int close_rc;
    int close_error;
    bool observed_watch_waiter;
} close_waiter_state_t;

static void closed_watch_waiter_task(void *arg) {
    close_waiter_state_t *state = arg;

    atomic_store_explicit(&state->waiter_entered, 1U, memory_order_release);
    errno = 0;
    state->poll_revents = (short)0x7fff;
    state->poll_rc = llam_poll_fd(state->read_fd, POLLIN, -1, &state->poll_revents);
    state->poll_error = errno;
}

static void closed_watch_closer_task(void *arg) {
    close_waiter_state_t *state = arg;

    while (atomic_load_explicit(&state->waiter_entered, memory_order_acquire) == 0U) {
        llam_yield();
    }
    for (unsigned attempt = 0U; attempt < 10000U && !state->observed_watch_waiter; ++attempt) {
        for (unsigned i = 0U; i < g_llam_runtime.active_nodes; ++i) {
            llam_node_t *node = &g_llam_runtime.nodes[i];
            llam_poll_watch_t *watch;

            pthread_mutex_lock(&node->watch_lock);
            watch = node->poll_watches;
            while (watch != NULL) {
                if (watch->fd == state->read_fd && watch->accepts_waiters &&
                    watch->wait_head != NULL) {
                    state->observed_watch_waiter = true;
                    break;
                }
                watch = watch->next;
            }
            pthread_mutex_unlock(&node->watch_lock);
            if (state->observed_watch_waiter) {
                break;
            }
        }
        if (!state->observed_watch_waiter) {
            llam_yield();
        }
    }

    errno = 0;
    state->close_rc = llam_close(state->read_fd);
    state->close_error = errno;
}
#endif

static int exercise_close_completes_parked_watch_waiter(void) {
#if LLAM_RUNTIME_BACKEND_KQUEUE || LLAM_RUNTIME_BACKEND_LINUX
    close_waiter_state_t state;
    llam_task_t *waiter;
    llam_task_t *closer;
    int fds[2] = {-1, -1};
    bool supports_multishot = false;
    int run_rc;

    memset(&state, 0, sizeof(state));
    state.read_fd = -1;
    state.poll_rc = INT_MIN;
    state.close_rc = INT_MIN;
    atomic_init(&state.waiter_entered, 0U);
    if (pipe(fds) != 0) {
        return fail_errno("pipe setup failed for close waiter completion");
    }
    if (init_runtime() != 0) {
        close_if_valid(&fds[0]);
        close_if_valid(&fds[1]);
        return fail_errno("runtime init failed for close waiter completion");
    }
    for (unsigned i = 0U; i < g_llam_runtime.active_nodes; ++i) {
        if (g_llam_runtime.nodes[i].supports_multishot_poll) {
            supports_multishot = true;
            break;
        }
    }
    if (!supports_multishot) {
        llam_runtime_shutdown();
        close_if_valid(&fds[0]);
        close_if_valid(&fds[1]);
        return 0;
    }

    state.read_fd = fds[0];
    waiter = llam_spawn(closed_watch_waiter_task, &state, NULL);
    closer = llam_spawn(closed_watch_closer_task, &state, NULL);
    if (waiter == NULL || closer == NULL) {
        llam_runtime_shutdown();
        close_if_valid(&fds[0]);
        close_if_valid(&fds[1]);
        return fail_errno("task spawn failed for close waiter completion");
    }
    run_rc = llam_run();
    llam_runtime_shutdown();
    fds[0] = -1;
    close_if_valid(&fds[1]);

    if (run_rc != 0 || !state.observed_watch_waiter || state.close_rc != 0 ||
        state.poll_rc != -1 || state.poll_error != EBADF || state.poll_revents != 0) {
        fprintf(stderr,
                "close waiter completion failed: run=%d observed=%u close=%d/%d poll=%d/%d revents=%d\n",
                run_rc,
                state.observed_watch_waiter ? 1U : 0U,
                state.close_rc,
                state.close_error,
                state.poll_rc,
                state.poll_error,
                (int)state.poll_revents);
        return 1;
    }
#endif
    return 0;
}

#if LLAM_RUNTIME_BACKEND_LINUX
static bool io_uring_unavailable_for_direct_internal_test(int rc) {
    int err = -rc;

    return err == EPERM || err == EACCES || err == ENOSYS || err == EOPNOTSUPP;
}
#endif

static int exercise_linux_oversized_submit_preserves_sq_tail(void) {
#if LLAM_RUNTIME_BACKEND_LINUX
    llam_runtime_t runtime;
    llam_node_t node;
    llam_io_req_t req;
    int rc;

    memset(&runtime, 0, sizeof(runtime));
    memset(&node, 0, sizeof(node));
    memset(&req, 0, sizeof(req));

    rc = io_uring_queue_init(4U, &node.ring, 0U);
    if (rc != 0) {
        if (io_uring_unavailable_for_direct_internal_test(rc)) {
            return 0;
        }
        errno = -rc;
        return fail_errno("io_uring init for oversized submit test failed");
    }

    node.runtime = &runtime;
    atomic_init(&node.pending_ops, 1U);
    req.kind = LLAM_IO_KIND_WRITE;
    req.count = (size_t)UINT_MAX + 1U;
    req.owner_runtime = &runtime;
    req.owner_shard = UINT_MAX;
    atomic_init(&req.wait_mode, LLAM_IO_WAIT_MODE_NONE);
    atomic_init(&req.inflight_owner_shard, UINT_MAX);
    atomic_init(&req.abort_reason, LLAM_IO_ABORT_NONE);
    atomic_init(&req.cancel_queued, 0U);

    /*
     * Oversized backend requests are rejected before SQE acquisition.  If the
     * guard consumes an SQE and then completes the request locally, a later
     * ring submit can observe an uninitialized SQE.
     */
    llam_io_submit_one(&node, &req);
    if (req.result != -1 || req.error_code != EINVAL) {
        io_uring_queue_exit(&node.ring);
        return fail_msg("oversized Linux I/O submit did not report EINVAL");
    }
    if (io_uring_sq_ready(&node.ring) != 0U) {
        io_uring_queue_exit(&node.ring);
        return fail_msg("oversized Linux I/O submit consumed an SQE");
    }

    io_uring_queue_exit(&node.ring);
#endif
    return 0;
}

static int exercise_linux_invalid_request_preserves_sq_tail(void) {
#if LLAM_RUNTIME_BACKEND_LINUX
    llam_runtime_t runtime;
    llam_node_t node;
    llam_io_req_t req;
    int rc;

    memset(&runtime, 0, sizeof(runtime));
    memset(&node, 0, sizeof(node));
    memset(&req, 0, sizeof(req));

    rc = io_uring_queue_init(4U, &node.ring, 0U);
    if (rc != 0) {
        if (io_uring_unavailable_for_direct_internal_test(rc)) {
            return 0;
        }
        errno = -rc;
        return fail_errno("io_uring init for invalid request test failed");
    }

    node.runtime = &runtime;
    atomic_init(&node.pending_ops, 1U);
    req.kind = (llam_io_kind_t)UINT_MAX;
    req.owner_runtime = &runtime;
    req.owner_shard = UINT_MAX;
    atomic_init(&req.wait_mode, LLAM_IO_WAIT_MODE_NONE);
    atomic_init(&req.inflight_owner_shard, UINT_MAX);
    atomic_init(&req.abort_reason, LLAM_IO_ABORT_NONE);
    atomic_init(&req.cancel_queued, 0U);

    /*
     * Unsupported request kinds complete locally.  They must be rejected before
     * SQE acquisition, otherwise the ring can later submit a stale entry.
     */
    llam_io_submit_one(&node, &req);
    if (req.result != -1 || req.error_code != EINVAL) {
        io_uring_queue_exit(&node.ring);
        return fail_msg("invalid Linux I/O request did not report EINVAL");
    }
    if (io_uring_sq_ready(&node.ring) != 0U) {
        io_uring_queue_exit(&node.ring);
        return fail_msg("invalid Linux I/O request consumed an SQE");
    }

    io_uring_queue_exit(&node.ring);
#endif
    return 0;
}

static int exercise_linux_invalid_control_preserves_sq_tail(void) {
#if LLAM_RUNTIME_BACKEND_LINUX
    llam_runtime_t runtime;
    llam_node_t node;
    llam_io_control_op_t *op = NULL;
    int rc;

    memset(&runtime, 0, sizeof(runtime));
    memset(&node, 0, sizeof(node));

    rc = pthread_mutex_init(&node.watch_lock, NULL);
    if (rc != 0) {
        errno = rc;
        return fail_errno("watch lock init for invalid control test failed");
    }
    rc = io_uring_queue_init(4U, &node.ring, 0U);
    if (rc != 0) {
        pthread_mutex_destroy(&node.watch_lock);
        if (io_uring_unavailable_for_direct_internal_test(rc)) {
            return 0;
        }
        errno = -rc;
        return fail_errno("io_uring init for invalid control test failed");
    }

    op = calloc(1U, sizeof(*op));
    if (op == NULL) {
        io_uring_queue_exit(&node.ring);
        pthread_mutex_destroy(&node.watch_lock);
        return fail_errno("control op allocation failed");
    }

    node.runtime = &runtime;
    op->kind = (llam_io_control_kind_t)UINT_MAX;
    /*
     * Invalid or malformed control operations are a defensive path, but they
     * still must not burn an SQE before being dropped locally.
     */
    llam_io_submit_control_op(&node, op);
    if (io_uring_sq_ready(&node.ring) != 0U) {
        io_uring_queue_exit(&node.ring);
        pthread_mutex_destroy(&node.watch_lock);
        return fail_msg("invalid Linux control submit consumed an SQE");
    }

    io_uring_queue_exit(&node.ring);
    pthread_mutex_destroy(&node.watch_lock);
#endif
    return 0;
}

#if LLAM_RUNTIME_BACKEND_LINUX
typedef struct linux_submit_fault_state {
    int forced_result;
    unsigned calls;
    unsigned expected;
} linux_submit_fault_state_t;

static int force_one_linux_submit_result(llam_node_t *node,
                                         unsigned expected,
                                         void *arg) {
    linux_submit_fault_state_t *state = arg;

    state->calls += 1U;
    state->expected = expected;
    node->linux_submit_override = NULL;
    node->linux_submit_override_arg = NULL;
    return state->forced_result;
}

static int exercise_linux_staged_cancel_submit_fault(int forced_result,
                                                     bool expect_terminal) {
    llam_runtime_t runtime;
    llam_shard_t shard;
    llam_node_t node;
    llam_task_t task;
    llam_io_req_t *req;
    llam_io_control_op_t *op = NULL;
    llam_io_control_op_t *encoded_op;
    linux_submit_fault_state_t fault;
    struct io_uring_cqe *cqe = NULL;
    struct __kernel_timespec timeout = {.tv_sec = 1, .tv_nsec = 0};
    bool ring_ready = false;
    bool watch_lock_ready = false;
    bool submit_lock_ready = false;
    bool req_activated = false;
    int init_rc;
    int rc = 1;

    memset(&runtime, 0, sizeof(runtime));
    memset(&shard, 0, sizeof(shard));
    memset(&node, 0, sizeof(node));
    memset(&task, 0, sizeof(task));
    memset(&fault, 0, sizeof(fault));
    fault.forced_result = forced_result;

    init_rc = pthread_mutex_init(&node.watch_lock, NULL);
    if (init_rc != 0) {
        errno = init_rc;
        return fail_errno("watch lock init for staged cancel control failed");
    }
    watch_lock_ready = true;
    init_rc = pthread_mutex_init(&node.submit_lock, NULL);
    if (init_rc != 0) {
        errno = init_rc;
        rc = fail_errno("submit lock init for staged cancel control failed");
        goto done;
    }
    submit_lock_ready = true;
    init_rc = io_uring_queue_init(4U, &node.ring, 0U);
    if (init_rc != 0) {
        if (io_uring_unavailable_for_direct_internal_test(init_rc)) {
            rc = 0;
            goto done;
        }
        errno = -init_rc;
        rc = fail_errno("io_uring init for staged cancel control failed");
        goto done;
    }
    ring_ready = true;

    runtime.shards = &shard;
    runtime.active_shards = 1U;
    runtime.nodes = &node;
    runtime.active_nodes = 1U;
    shard.runtime = &runtime;
    shard.id = 0U;
    node.runtime = &runtime;
    node.watch_lock_initialized = true;
    node.submit_lock_initialized = true;
    atomic_init(&node.pending_ops, 0U);
    task.owner_runtime = &runtime;
    atomic_init(&task.scan_refs, 0U);
    atomic_init(&task.active_io_req, NULL);
    req = &task.embedded_io_req;
    llam_io_req_reset(req, &runtime, UINT_MAX, UINT_MAX);
    if (!llam_io_req_lifetime_activate(req)) {
        rc = fail_errno("request activation for staged cancel control failed");
        goto done;
    }
    req_activated = true;
    req->task = &task;
    atomic_store_explicit(&req->wait_mode, LLAM_IO_WAIT_MODE_INFLIGHT, memory_order_release);
    atomic_store_explicit(&req->cancel_queued, 1U, memory_order_release);

    pthread_mutex_lock(&node.watch_lock);
    if (llam_node_queue_control_locked(&node, LLAM_IO_CONTROL_REQ_CANCEL, req) != 0) {
        pthread_mutex_unlock(&node.watch_lock);
        rc = fail_errno("queue staged cancel control failed");
        goto done;
    }
    pthread_mutex_unlock(&node.watch_lock);
    op = llam_take_node_controls(&node);
    if (op == NULL || op->next != NULL) {
        rc = fail_msg("staged cancel control did not detach exactly one op");
        goto done;
    }
    encoded_op = op;

    /*
     * Encode first, then force the normal submit wrapper to report a real
     * negative/short result without flushing the SQ.  This fixes the hostile
     * state precisely: source queue empty, SQE still live, and the only heap
     * owner reachable through encoded user_data plus the backend-control list.
     */
    llam_io_submit_control_op(&node, op);
    op = NULL;
    if (io_uring_sq_ready(&node.ring) != 1U ||
        node.linux_backend_control_head != encoded_op ||
        node.linux_backend_control_tail != encoded_op ||
        !encoded_op->linux_backend_tracked) {
        rc = fail_msg("staged cancel control did not retain a ring SQE");
        goto done;
    }
    if (atomic_load_explicit(&node.pending_ops, memory_order_acquire) != 1U ||
        atomic_load_explicit(&req->lifetime_refs, memory_order_acquire) != 2U ||
        atomic_load_explicit(&task.scan_refs, memory_order_acquire) != 1U) {
        rc = fail_msg("staged cancel control was not tracked through backend ownership");
        goto done;
    }

    node.linux_submit_override = force_one_linux_submit_result;
    node.linux_submit_override_arg = &fault;
    init_rc = llam_node_submit_ring(&node);
    if (init_rc != forced_result || fault.calls != 1U || fault.expected != 1U ||
        node.linux_submit_retry == expect_terminal ||
        node.linux_submit_terminal != expect_terminal ||
        node.linux_backend_control_head != encoded_op ||
        atomic_load_explicit(&node.pending_ops, memory_order_acquire) != 1U ||
        atomic_load_explicit(&req->lifetime_refs, memory_order_acquire) != 2U ||
        atomic_load_explicit(&task.scan_refs, memory_order_acquire) != 1U) {
        rc = fail_msg("faulted Linux control submit lost retry/ownership state");
        goto done;
    }

    if (expect_terminal) {
        /* No kernel reference survives queue_exit; teardown may now roll back. */
        io_uring_queue_exit(&node.ring);
        ring_ready = false;
        llam_linux_retire_backend_controls(&node);
        if (node.linux_backend_control_head != NULL ||
            node.linux_backend_control_tail != NULL ||
            atomic_load_explicit(&node.pending_ops, memory_order_acquire) != 0U ||
            atomic_load_explicit(&req->lifetime_refs, memory_order_acquire) != 1U ||
            atomic_load_explicit(&task.scan_refs, memory_order_acquire) != 0U ||
            atomic_load_explicit(&req->cancel_queued, memory_order_acquire) != 0U ||
            atomic_load_explicit(&req->cancel_submitted, memory_order_acquire) != 0U) {
            rc = fail_msg("terminal Linux ring teardown did not retire encoded control ownership");
            goto done;
        }
    } else {
        /* No newly dequeued work exists; the explicit retry bit drives enter. */
        llam_io_submit_batch(&node);
        if (node.linux_submit_retry || node.linux_submit_terminal) {
            rc = fail_msg("empty Linux batch did not clear submit retry state");
            goto done;
        }
        init_rc = io_uring_wait_cqe_timeout(&node.ring, &cqe, &timeout);
        if (init_rc != 0 || cqe == NULL) {
            errno = init_rc < 0 ? -init_rc : ETIMEDOUT;
            rc = fail_errno("empty Linux batch did not resubmit staged cancel control");
            goto done;
        }
        llam_io_handle_cqe(&node, cqe);
        if (node.linux_backend_control_head != NULL ||
            node.linux_backend_control_tail != NULL ||
            atomic_load_explicit(&node.pending_ops, memory_order_acquire) != 0U ||
            atomic_load_explicit(&req->lifetime_refs, memory_order_acquire) != 1U ||
            atomic_load_explicit(&task.scan_refs, memory_order_acquire) != 0U ||
            atomic_load_explicit(&req->cancel_queued, memory_order_acquire) != 0U ||
            atomic_load_explicit(&req->cancel_submitted, memory_order_acquire) != 0U) {
            rc = fail_msg("cancel control CQE did not balance backend/request/task ownership");
            goto done;
        }
    }

    if (!llam_io_req_lifetime_release(req)) {
        rc = fail_errno("base request release after submit fault failed");
        goto done;
    }
    req_activated = false;
    if (atomic_load_explicit(&node.pending_ops, memory_order_acquire) != 0U ||
        atomic_load_explicit(&task.embedded_io_req.lifetime_refs,
                             memory_order_acquire) != 0U ||
        atomic_load_explicit(&task.scan_refs, memory_order_acquire) != 0U) {
        rc = fail_msg("submit fault terminal left request or pending ownership live");
        goto done;
    }
    rc = 0;

done:
    if (ring_ready) {
        io_uring_queue_exit(&node.ring);
        ring_ready = false;
    }
    if (node.linux_backend_control_head != NULL) {
        llam_linux_retire_backend_controls(&node);
    }
    while (op != NULL) {
        llam_io_control_op_t *next = op->next;

        op->next = NULL;
        llam_io_control_op_destroy(&node, op);
        op = next;
    }
    if (req_activated &&
        atomic_load_explicit(&task.embedded_io_req.lifetime_refs, memory_order_acquire) != 0U) {
        (void)llam_io_req_lifetime_release(&task.embedded_io_req);
    }
    if (submit_lock_ready) {
        pthread_mutex_destroy(&node.submit_lock);
    }
    if (watch_lock_ready) {
        pthread_mutex_destroy(&node.watch_lock);
    }
    return rc;
}

typedef enum linux_watch_lifetime_test_kind {
    LINUX_WATCH_LIFETIME_POLL = 0,
    LINUX_WATCH_LIFETIME_ACCEPT = 1,
    LINUX_WATCH_LIFETIME_RECV = 2,
} linux_watch_lifetime_test_kind_t;

static bool linux_watch_lifetime_test_is_linked(const llam_node_t *node,
                                                linux_watch_lifetime_test_kind_t kind,
                                                const void *watch) {
    switch (kind) {
    case LINUX_WATCH_LIFETIME_POLL:
        return node->poll_watches == watch;
    case LINUX_WATCH_LIFETIME_ACCEPT:
        return node->accept_watches == watch;
    case LINUX_WATCH_LIFETIME_RECV:
        return node->recv_watches == watch;
    default:
        return false;
    }
}

static unsigned linux_watch_lifetime_test_backend_refs(linux_watch_lifetime_test_kind_t kind,
                                                       const void *watch) {
    switch (kind) {
    case LINUX_WATCH_LIFETIME_POLL:
        return ((const llam_poll_watch_t *)watch)->backend_refs;
    case LINUX_WATCH_LIFETIME_ACCEPT:
        return ((const llam_accept_watch_t *)watch)->backend_refs;
    case LINUX_WATCH_LIFETIME_RECV:
        return ((const llam_recv_watch_t *)watch)->backend_refs;
    default:
        return UINT_MAX;
    }
}

static bool linux_watch_lifetime_test_active(linux_watch_lifetime_test_kind_t kind,
                                             const void *watch) {
    switch (kind) {
    case LINUX_WATCH_LIFETIME_POLL:
        return ((const llam_poll_watch_t *)watch)->active;
    case LINUX_WATCH_LIFETIME_ACCEPT:
        return ((const llam_accept_watch_t *)watch)->active;
    case LINUX_WATCH_LIFETIME_RECV:
        return ((const llam_recv_watch_t *)watch)->active;
    default:
        return false;
    }
}

static bool linux_watch_lifetime_test_deactivate_queued(linux_watch_lifetime_test_kind_t kind,
                                                        const void *watch) {
    switch (kind) {
    case LINUX_WATCH_LIFETIME_POLL:
        return ((const llam_poll_watch_t *)watch)->deactivate_queued;
    case LINUX_WATCH_LIFETIME_ACCEPT:
        return ((const llam_accept_watch_t *)watch)->deactivate_queued;
    case LINUX_WATCH_LIFETIME_RECV:
        return ((const llam_recv_watch_t *)watch)->deactivate_queued;
    default:
        return false;
    }
}

static bool linux_watch_lifetime_test_destroy_pending(linux_watch_lifetime_test_kind_t kind,
                                                      const void *watch) {
    switch (kind) {
    case LINUX_WATCH_LIFETIME_POLL:
        return ((const llam_poll_watch_t *)watch)->destroy_pending;
    case LINUX_WATCH_LIFETIME_ACCEPT:
        return ((const llam_accept_watch_t *)watch)->destroy_pending;
    case LINUX_WATCH_LIFETIME_RECV:
        return ((const llam_recv_watch_t *)watch)->destroy_pending;
    default:
        return false;
    }
}

static void linux_watch_lifetime_test_destroy_locked(llam_node_t *node,
                                                     linux_watch_lifetime_test_kind_t kind,
                                                     void *watch) {
    switch (kind) {
    case LINUX_WATCH_LIFETIME_POLL: {
        llam_poll_watch_t *poll_watch = watch;

        poll_watch->retired = false;
        poll_watch->active = false;
        poll_watch->activating = false;
        poll_watch->deactivate_queued = false;
        poll_watch->backend_refs = 0U;
        llam_destroy_poll_watch_locked(node, poll_watch);
        break;
    }
    case LINUX_WATCH_LIFETIME_ACCEPT: {
        llam_accept_watch_t *accept_watch = watch;

        accept_watch->retired = false;
        accept_watch->active = false;
        accept_watch->activating = false;
        accept_watch->deactivate_queued = false;
        accept_watch->backend_refs = 0U;
        llam_destroy_accept_watch_locked(node, accept_watch);
        break;
    }
    case LINUX_WATCH_LIFETIME_RECV: {
        llam_recv_watch_t *recv_watch = watch;

        recv_watch->retired = false;
        recv_watch->active = false;
        recv_watch->activating = false;
        recv_watch->deactivate_queued = false;
        recv_watch->backend_refs = 0U;
        llam_destroy_recv_watch_locked(node, recv_watch);
        break;
    }
    default:
        break;
    }
}

static int exercise_linux_closed_watch_cqe_order_one(linux_watch_lifetime_test_kind_t kind,
                                                     bool cancel_control_first,
                                                     bool retired_terminal_only) {
    llam_runtime_t runtime;
    llam_node_t node;
    llam_io_control_op_t *op = NULL;
    llam_io_control_op_t *encoded_op = NULL;
    struct io_uring_cqe target_cqe;
    struct io_uring_cqe control_cqe;
    struct io_uring_buf_ring *recv_buf_ring = NULL;
    unsigned char *recv_buf_storage = NULL;
    void *watch = NULL;
    int accepted_pipe[2] = {-1, -1};
    bool watch_lock_ready = false;
    bool recv_lock_ready = false;
    bool ring_ready = false;
    bool expect_provided_buffer = false;
    int init_rc;
    int rc = 1;

    memset(&runtime, 0, sizeof(runtime));
    memset(&node, 0, sizeof(node));
    memset(&target_cqe, 0, sizeof(target_cqe));
    memset(&control_cqe, 0, sizeof(control_cqe));
    atomic_init(&runtime.fatal_errno, 0);
    atomic_init(&node.pending_ops, retired_terminal_only ? 1U : 2U);
    atomic_init(&node.provided_buf_acquires, 0U);
    atomic_init(&node.provided_buf_returns, 0U);
    runtime.nodes = &node;
    runtime.active_nodes = 1U;
    node.runtime = &runtime;
    node.index = 0U;
    node.event_fd = -1;

    init_rc = pthread_mutex_init(&node.watch_lock, NULL);
    if (init_rc != 0) {
        errno = init_rc;
        return fail_errno("watch lock init for Linux watch CQE ordering failed");
    }
    watch_lock_ready = true;
    init_rc = pthread_mutex_init(&node.recv_buf_lock, NULL);
    if (init_rc != 0) {
        errno = init_rc;
        rc = fail_errno("recv lock init for Linux watch CQE ordering failed");
        goto done;
    }
    recv_lock_ready = true;
    init_rc = io_uring_queue_init(8U, &node.ring, 0U);
    if (init_rc != 0) {
        if (io_uring_unavailable_for_direct_internal_test(init_rc)) {
            rc = 0;
            goto done;
        }
        errno = -init_rc;
        rc = fail_errno("io_uring init for Linux watch CQE ordering failed");
        goto done;
    }
    ring_ready = true;
    node.ring_ready = true;

    switch (kind) {
    case LINUX_WATCH_LIFETIME_POLL: {
        llam_poll_watch_t *poll_watch = calloc(1U, sizeof(*poll_watch));

        if (poll_watch == NULL) {
            goto done;
        }
        poll_watch->fd = -1;
        poll_watch->events = POLLIN;
        poll_watch->migrate_target_node_index = UINT_MAX;
        poll_watch->active = true;
        poll_watch->deactivate_queued = true;
        poll_watch->backend_refs = 1U;
        node.poll_watches = poll_watch;
        watch = poll_watch;
        target_cqe.user_data = llam_io_udata_encode(watch, LLAM_IO_UDATA_POLL_WATCH);
        target_cqe.res = -ECANCELED;
        break;
    }
    case LINUX_WATCH_LIFETIME_ACCEPT: {
        llam_accept_watch_t *accept_watch = calloc(1U, sizeof(*accept_watch));

        if (accept_watch == NULL || pipe(accepted_pipe) != 0) {
            free(accept_watch);
            goto done;
        }
        accept_watch->fd = -1;
        accept_watch->migrate_target_node_index = UINT_MAX;
        accept_watch->active = true;
        accept_watch->deactivate_queued = true;
        accept_watch->backend_refs = 1U;
        node.accept_watches = accept_watch;
        watch = accept_watch;
        target_cqe.user_data = llam_io_udata_encode(watch, LLAM_IO_UDATA_ACCEPT_WATCH);
        target_cqe.res = accepted_pipe[0];
        break;
    }
    case LINUX_WATCH_LIFETIME_RECV: {
        llam_recv_watch_t *recv_watch = calloc(1U, sizeof(*recv_watch));

        recv_buf_ring = calloc(1U, sizeof(*recv_buf_ring));
        recv_buf_storage = calloc(1U, LLAM_IO_BUFFER_INLINE_BYTES);
        if (recv_watch == NULL || recv_buf_ring == NULL || recv_buf_storage == NULL) {
            free(recv_watch);
            goto done;
        }
        recv_watch->fd = -1;
        recv_watch->migrate_target_node_index = UINT_MAX;
        recv_watch->active = true;
        recv_watch->deactivate_queued = true;
        recv_watch->backend_refs = 1U;
        node.recv_watches = recv_watch;
        node.recv_buf_ring = recv_buf_ring;
        node.recv_buf_storage = recv_buf_storage;
        node.recv_buf_entries = 1U;
        node.recv_buf_mask = 0U;
        node.supports_provided_buffers = true;
#if defined(LLAM_HAVE_IO_URING_BUF_RING_HELPERS)
        expect_provided_buffer = true;
#endif
        watch = recv_watch;
        target_cqe.user_data = llam_io_udata_encode(watch, LLAM_IO_UDATA_RECV_WATCH);
        target_cqe.res = 8;
        target_cqe.flags = IORING_CQE_F_BUFFER;
        break;
    }
    default:
        goto done;
    }

    if (retired_terminal_only) {
        switch (kind) {
        case LINUX_WATCH_LIFETIME_POLL:
            ((llam_poll_watch_t *)watch)->retired = true;
            break;
        case LINUX_WATCH_LIFETIME_ACCEPT:
            ((llam_accept_watch_t *)watch)->retired = true;
            break;
        case LINUX_WATCH_LIFETIME_RECV:
            ((llam_recv_watch_t *)watch)->retired = true;
            break;
        default:
            goto done;
        }
        llam_io_handle_cqe(&node, &target_cqe);
        if (!linux_watch_lifetime_test_is_linked(&node, kind, watch) ||
            linux_watch_lifetime_test_backend_refs(kind, watch) != 0U ||
            linux_watch_lifetime_test_active(kind, watch) ||
            atomic_load_explicit(&node.pending_ops, memory_order_acquire) != 0U) {
            rc = fail_msg("retired Linux terminal CQE did not release backend ownership");
            goto done;
        }
        if (kind == LINUX_WATCH_LIFETIME_ACCEPT) {
            errno = 0;
            if (fcntl(accepted_pipe[0], F_GETFD) != -1 || errno != EBADF) {
                rc = fail_msg("retired Linux accept CQE did not close late accepted fd");
                goto done;
            }
            accepted_pipe[0] = -1;
        } else if (kind == LINUX_WATCH_LIFETIME_RECV && expect_provided_buffer &&
                   (atomic_load_explicit(&node.provided_buf_acquires, memory_order_acquire) != 1U ||
                    atomic_load_explicit(&node.provided_buf_returns, memory_order_acquire) != 1U)) {
            rc = fail_msg("retired Linux recv CQE did not recycle late provided buffer");
            goto done;
        }
        rc = 0;
        goto done;
    }

    op = calloc(1U, sizeof(*op));
    if (op == NULL) {
        goto done;
    }
    op->target = watch;
    op->kind = kind == LINUX_WATCH_LIFETIME_POLL ? LLAM_IO_CONTROL_POLL_DEACTIVATE :
               (kind == LINUX_WATCH_LIFETIME_ACCEPT ? LLAM_IO_CONTROL_ACCEPT_DEACTIVATE :
                                                      LLAM_IO_CONTROL_RECV_DEACTIVATE);
    encoded_op = op;
    llam_linux_track_backend_control(&node, op);
    op = NULL;
    control_cqe.user_data = llam_io_udata_encode(encoded_op, LLAM_IO_UDATA_CONTROL);
    control_cqe.res = 0;

    if (cancel_control_first) {
        llam_io_handle_cqe(&node, &control_cqe);
        encoded_op = NULL;
        if (!linux_watch_lifetime_test_is_linked(&node, kind, watch) ||
            linux_watch_lifetime_test_backend_refs(kind, watch) != 1U ||
            linux_watch_lifetime_test_active(kind, watch) ||
            linux_watch_lifetime_test_deactivate_queued(kind, watch) ||
            !linux_watch_lifetime_test_destroy_pending(kind, watch) ||
            atomic_load_explicit(&node.pending_ops, memory_order_acquire) != 0U) {
            rc = fail_msg("cancel-first Linux watch CQE did not retain terminal backend ownership");
            goto done;
        }
        llam_io_handle_cqe(&node, &target_cqe);
    } else {
        llam_io_handle_cqe(&node, &target_cqe);
        if (!linux_watch_lifetime_test_is_linked(&node, kind, watch) ||
            linux_watch_lifetime_test_backend_refs(kind, watch) != 0U ||
            linux_watch_lifetime_test_active(kind, watch) ||
            !linux_watch_lifetime_test_deactivate_queued(kind, watch) ||
            linux_watch_lifetime_test_destroy_pending(kind, watch) ||
            atomic_load_explicit(&node.pending_ops, memory_order_acquire) != 1U) {
            rc = fail_msg("target-first Linux watch CQE released control-target ownership early");
            goto done;
        }
        llam_io_handle_cqe(&node, &control_cqe);
        encoded_op = NULL;
    }

    if (linux_watch_lifetime_test_is_linked(&node, kind, watch) ||
        node.retired_poll_watches != NULL || node.retired_accept_watches != NULL ||
        node.retired_recv_watches != NULL || node.linux_backend_control_head != NULL ||
        node.linux_backend_control_tail != NULL ||
        atomic_load_explicit(&node.pending_ops, memory_order_acquire) != 0U) {
        bool still_linked = linux_watch_lifetime_test_is_linked(&node, kind, watch);

        fprintf(stderr,
                "Linux watch CQE ordering state kind=%u cancel_first=%u linked=%u pending=%u "
                "controls=%p/%p retired=%p/%p/%p\n",
                (unsigned)kind,
                cancel_control_first ? 1U : 0U,
                still_linked ? 1U : 0U,
                atomic_load_explicit(&node.pending_ops, memory_order_acquire),
                (void *)node.linux_backend_control_head,
                (void *)node.linux_backend_control_tail,
                (void *)node.retired_poll_watches,
                (void *)node.retired_accept_watches,
                (void *)node.retired_recv_watches);
        if (still_linked) {
            unsigned lifetime_refs = kind == LINUX_WATCH_LIFETIME_POLL
                                         ? ((llam_poll_watch_t *)watch)->lifetime_refs
                                         : (kind == LINUX_WATCH_LIFETIME_ACCEPT
                                                ? ((llam_accept_watch_t *)watch)->lifetime_refs
                                                : ((llam_recv_watch_t *)watch)->lifetime_refs);
            bool accepts_waiters = kind == LINUX_WATCH_LIFETIME_POLL
                                       ? ((llam_poll_watch_t *)watch)->accepts_waiters
                                       : (kind == LINUX_WATCH_LIFETIME_ACCEPT
                                              ? ((llam_accept_watch_t *)watch)->accepts_waiters
                                              : ((llam_recv_watch_t *)watch)->accepts_waiters);
            bool retired = kind == LINUX_WATCH_LIFETIME_POLL
                               ? ((llam_poll_watch_t *)watch)->retired
                               : (kind == LINUX_WATCH_LIFETIME_ACCEPT
                                      ? ((llam_accept_watch_t *)watch)->retired
                                      : ((llam_recv_watch_t *)watch)->retired);
            bool activating = kind == LINUX_WATCH_LIFETIME_POLL
                                  ? ((llam_poll_watch_t *)watch)->activating
                                  : (kind == LINUX_WATCH_LIFETIME_ACCEPT
                                         ? ((llam_accept_watch_t *)watch)->activating
                                         : ((llam_recv_watch_t *)watch)->activating);

            fprintf(stderr,
                    "Linux watch CQE ordering watch refs=%u lifetime=%u active=%u activating=%u "
                    "deactivate=%u destroy=%u accepts=%u retired=%u\n",
                    linux_watch_lifetime_test_backend_refs(kind, watch),
                    lifetime_refs,
                    linux_watch_lifetime_test_active(kind, watch) ? 1U : 0U,
                    activating ? 1U : 0U,
                    linux_watch_lifetime_test_deactivate_queued(kind, watch) ? 1U : 0U,
                    linux_watch_lifetime_test_destroy_pending(kind, watch) ? 1U : 0U,
                    accepts_waiters ? 1U : 0U,
                    retired ? 1U : 0U);
        }
        rc = fail_msg("Linux watch CQE ordering did not reclaim and balance final ownership");
        goto done;
    }
    watch = NULL;
    if (kind == LINUX_WATCH_LIFETIME_ACCEPT) {
        errno = 0;
        if (fcntl(accepted_pipe[0], F_GETFD) != -1 || errno != EBADF) {
            rc = fail_msg("late accepted fd was not closed during watch teardown");
            goto done;
        }
        accepted_pipe[0] = -1;
    } else if (kind == LINUX_WATCH_LIFETIME_RECV && expect_provided_buffer &&
               (atomic_load_explicit(&node.provided_buf_acquires, memory_order_acquire) != 1U ||
                atomic_load_explicit(&node.provided_buf_returns, memory_order_acquire) != 1U)) {
        fprintf(stderr,
                "Linux watch recv cleanup state cancel_first=%u acquires=%llu returns=%llu ring_ready=%u "
                "buffers=%u entries=%u\n",
                cancel_control_first ? 1U : 0U,
                (unsigned long long)atomic_load_explicit(&node.provided_buf_acquires,
                                                         memory_order_acquire),
                (unsigned long long)atomic_load_explicit(&node.provided_buf_returns,
                                                         memory_order_acquire),
                node.ring_ready ? 1U : 0U,
                node.supports_provided_buffers ? 1U : 0U,
                node.recv_buf_entries);
        rc = fail_msg("late recv provided buffer was not recycled during watch teardown");
        goto done;
    }
    rc = 0;

done:
    if (ring_ready) {
        io_uring_queue_exit(&node.ring);
        ring_ready = false;
        node.ring_ready = false;
    }
    if (watch_lock_ready) {
        llam_linux_retire_backend_controls(&node);
        llam_linux_retire_backend_watch_refs(&node);
        pthread_mutex_lock(&node.watch_lock);
        if (watch != NULL && linux_watch_lifetime_test_is_linked(&node, kind, watch)) {
            linux_watch_lifetime_test_destroy_locked(&node, kind, watch);
        }
        pthread_mutex_unlock(&node.watch_lock);
    }
    free(op);
    close_if_valid(&accepted_pipe[0]);
    close_if_valid(&accepted_pipe[1]);
    free(recv_buf_ring);
    free(recv_buf_storage);
    if (recv_lock_ready) {
        pthread_mutex_destroy(&node.recv_buf_lock);
    }
    if (watch_lock_ready) {
        pthread_mutex_destroy(&node.watch_lock);
    }
    return rc;
}

static int exercise_linux_closed_watch_cqe_orders(void) {
    for (unsigned kind = LINUX_WATCH_LIFETIME_POLL;
         kind <= LINUX_WATCH_LIFETIME_RECV;
         ++kind) {
        if (exercise_linux_closed_watch_cqe_order_one(
                (linux_watch_lifetime_test_kind_t)kind, false, false) != 0 ||
            exercise_linux_closed_watch_cqe_order_one(
                (linux_watch_lifetime_test_kind_t)kind, true, false) != 0 ||
            exercise_linux_closed_watch_cqe_order_one(
                (linux_watch_lifetime_test_kind_t)kind, false, true) != 0) {
            return 1;
        }
    }
    return 0;
}

enum {
    LINUX_WATCH_REARM_NATURAL_TERMINAL = 0,
    LINUX_WATCH_REARM_TARGET_FIRST = 1,
    LINUX_WATCH_REARM_CONTROL_FIRST = 2,
};

static int exercise_linux_accept_rejects_stale_deactivate_overlap(void) {
    llam_runtime_t runtime;
    llam_node_t node;
    llam_shard_t shard;
    llam_task_t task;
    llam_io_req_t req;
    llam_accept_watch_t *watch = NULL;
    llam_shard_t *saved_shard = g_llam_tls_shard;
    llam_task_t *saved_task = g_llam_tls_task;
    int listener = -1;
    bool lock_ready = false;
    int rc = 1;

    memset(&runtime, 0, sizeof(runtime));
    memset(&node, 0, sizeof(node));
    memset(&shard, 0, sizeof(shard));
    memset(&task, 0, sizeof(task));
    memset(&req, 0, sizeof(req));
    atomic_init(&runtime.fatal_errno, 0);
    runtime.nodes = &node;
    runtime.active_nodes = 1U;
    node.runtime = &runtime;
    node.index = 0U;
    node.event_fd = -1;
    node.ring_ready = true;
    node.supports_multishot_accept = true;
    shard.runtime = &runtime;
    shard.id = 0U;
    shard.io_node_index = 0U;
    task.owner_runtime = &runtime;
    llam_io_req_reset(&req, &runtime, UINT_MAX, UINT_MAX);

    if (make_loopback_listener(&listener) != 0) {
        return fail_errno("listener setup for stale accept deactivate gate failed");
    }
    if (pthread_mutex_init(&node.watch_lock, NULL) != 0) {
        goto done;
    }
    lock_ready = true;
    node.watch_lock_initialized = true;

    pthread_mutex_lock(&node.watch_lock);
    watch = llam_get_or_create_accept_watch_locked(&node, listener);
    if (watch != NULL) {
        /* Model target-terminal-first with its old cancel already ring-backed. */
        watch->active = false;
        watch->deactivate_queued = true;
    }
    pthread_mutex_unlock(&node.watch_lock);
    if (watch == NULL) {
        goto done;
    }
    req.fd = listener;

    g_llam_tls_shard = &shard;
    g_llam_tls_task = &task;
    errno = 0;
    {
        int issue_rc = llam_issue_multishot_accept(&req);
        int issue_error = errno;

        if (issue_rc != -1 || issue_error != EAGAIN) {
            fprintf(stderr,
                    "stale accept deactivate result rc=%d errno=%d active=%u activating=%u "
                    "deactivate=%u queued=%p\n",
                    issue_rc,
                    issue_error,
                    watch->active ? 1U : 0U,
                    watch->activating ? 1U : 0U,
                    watch->deactivate_queued ? 1U : 0U,
                    (void *)node.control_head);
            g_llam_tls_shard = saved_shard;
            g_llam_tls_task = saved_task;
            rc = fail_msg("accept overlapped a ring-backed stale deactivate");
            goto done;
        }
    }
    g_llam_tls_shard = saved_shard;
    g_llam_tls_task = saved_task;
    if (node.control_head != NULL || watch->activating || watch->active ||
        !watch->deactivate_queued) {
        rc = fail_msg("stale accept deactivate gate mutated backend generation state");
        goto done;
    }
    rc = 0;

done:
    g_llam_tls_shard = saved_shard;
    g_llam_tls_task = saved_task;
    node.ring_ready = false;
    if (lock_ready) {
        pthread_mutex_lock(&node.watch_lock);
        if (watch != NULL && node.accept_watches == watch) {
            watch->active = false;
            watch->activating = false;
            watch->deactivate_queued = false;
            watch->backend_refs = 0U;
            watch->lifetime_refs = 0U;
            llam_destroy_accept_watch_locked(&node, watch);
        }
        pthread_mutex_unlock(&node.watch_lock);
        pthread_mutex_destroy(&node.watch_lock);
    }
    close_if_valid(&listener);
    return rc;
}

static int exercise_linux_terminal_watch_rearm_one(linux_watch_lifetime_test_kind_t kind,
                                                   unsigned ordering) {
    llam_runtime_t runtime;
    llam_node_t node;
    llam_io_req_t first;
    llam_io_req_t second;
    llam_io_control_op_t *encoded_op = NULL;
    struct io_uring_cqe target_cqe;
    struct io_uring_cqe control_cqe;
    void *watch = NULL;
    int accepted_pipe[2] = {-1, -1};
    bool watch_lock_ready = false;
    bool ring_ready = false;
    int init_rc;
    int rc = 1;

    memset(&runtime, 0, sizeof(runtime));
    memset(&node, 0, sizeof(node));
    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    memset(&target_cqe, 0, sizeof(target_cqe));
    memset(&control_cqe, 0, sizeof(control_cqe));
    atomic_init(&runtime.fatal_errno, 0);
    runtime.nodes = &node;
    runtime.active_nodes = 1U;
    node.runtime = &runtime;
    node.index = 0U;
    node.event_fd = -1;
    atomic_init(&node.pending_ops,
                ordering == LINUX_WATCH_REARM_NATURAL_TERMINAL ? 1U : 2U);

    init_rc = pthread_mutex_init(&node.watch_lock, NULL);
    if (init_rc != 0) {
        errno = init_rc;
        return fail_errno("watch lock init for terminal rearm failed");
    }
    watch_lock_ready = true;
    node.watch_lock_initialized = true;
    init_rc = io_uring_queue_init(8U, &node.ring, 0U);
    if (init_rc != 0) {
        if (io_uring_unavailable_for_direct_internal_test(init_rc)) {
            rc = 0;
            goto done;
        }
        errno = -init_rc;
        rc = fail_errno("io_uring init for terminal rearm failed");
        goto done;
    }
    ring_ready = true;
    node.ring_ready = true;

    llam_io_req_reset(&first, &runtime, UINT_MAX, UINT_MAX);
    llam_io_req_reset(&second, &runtime, UINT_MAX, UINT_MAX);
    first.result = -101;
    second.result = -202;
    first.next = &second;
    if (kind == LINUX_WATCH_LIFETIME_ACCEPT) {
        llam_accept_watch_t *accept_watch = calloc(1U, sizeof(*accept_watch));

        if (accept_watch == NULL ||
            (ordering == LINUX_WATCH_REARM_NATURAL_TERMINAL && pipe(accepted_pipe) != 0)) {
            free(accept_watch);
            goto done;
        }
        accept_watch->fd = -1;
        accept_watch->migrate_target_node_index =
            ordering == LINUX_WATCH_REARM_NATURAL_TERMINAL ? UINT_MAX : 1U;
        accept_watch->accepts_waiters = true;
        accept_watch->active = true;
        accept_watch->backend_refs = 1U;
        accept_watch->deactivate_queued = ordering != LINUX_WATCH_REARM_NATURAL_TERMINAL;
        accept_watch->wait_head = &first;
        accept_watch->wait_tail = &second;
        node.accept_watches = accept_watch;
        watch = accept_watch;
        first.kind = LLAM_IO_KIND_ACCEPT;
        second.kind = LLAM_IO_KIND_ACCEPT;
        first.accept_watch = accept_watch;
        second.accept_watch = accept_watch;
        atomic_store_explicit(&first.wait_mode, LLAM_IO_WAIT_MODE_ACCEPT_WATCH, memory_order_release);
        atomic_store_explicit(&second.wait_mode, LLAM_IO_WAIT_MODE_ACCEPT_WATCH, memory_order_release);
        target_cqe.user_data = llam_io_udata_encode(watch, LLAM_IO_UDATA_ACCEPT_WATCH);
        target_cqe.res = ordering == LINUX_WATCH_REARM_NATURAL_TERMINAL
                             ? accepted_pipe[0]
                             : -ECANCELED;
    } else if (kind == LINUX_WATCH_LIFETIME_RECV) {
        llam_recv_watch_t *recv_watch = calloc(1U, sizeof(*recv_watch));

        if (recv_watch == NULL) {
            goto done;
        }
        recv_watch->fd = -1;
        recv_watch->migrate_target_node_index =
            ordering == LINUX_WATCH_REARM_NATURAL_TERMINAL ? UINT_MAX : 1U;
        recv_watch->accepts_waiters = true;
        recv_watch->active = true;
        recv_watch->backend_refs = 1U;
        recv_watch->deactivate_queued = ordering != LINUX_WATCH_REARM_NATURAL_TERMINAL;
        recv_watch->wait_head = &first;
        recv_watch->wait_tail = &second;
        node.recv_watches = recv_watch;
        watch = recv_watch;
        first.recv_watch = recv_watch;
        second.recv_watch = recv_watch;
        atomic_store_explicit(&first.wait_mode, LLAM_IO_WAIT_MODE_RECV_WATCH, memory_order_release);
        atomic_store_explicit(&second.wait_mode, LLAM_IO_WAIT_MODE_RECV_WATCH, memory_order_release);
        target_cqe.user_data = llam_io_udata_encode(watch, LLAM_IO_UDATA_RECV_WATCH);
        /* Zero is one datagram/EOF result, never a broadcast to both waiters. */
        target_cqe.res = ordering == LINUX_WATCH_REARM_NATURAL_TERMINAL ? 0 : -ECANCELED;
    } else {
        goto done;
    }

    if (ordering != LINUX_WATCH_REARM_NATURAL_TERMINAL) {
        encoded_op = calloc(1U, sizeof(*encoded_op));
        if (encoded_op == NULL) {
            goto done;
        }
        encoded_op->target = watch;
        encoded_op->kind = kind == LINUX_WATCH_LIFETIME_ACCEPT
                               ? LLAM_IO_CONTROL_ACCEPT_DEACTIVATE
                               : LLAM_IO_CONTROL_RECV_DEACTIVATE;
        llam_linux_track_backend_control(&node, encoded_op);
        control_cqe.user_data = llam_io_udata_encode(encoded_op, LLAM_IO_UDATA_CONTROL);
        control_cqe.res = 0;
    }

    if (ordering == LINUX_WATCH_REARM_CONTROL_FIRST) {
        llam_io_handle_cqe(&node, &control_cqe);
        encoded_op = NULL;
        llam_io_handle_cqe(&node, &target_cqe);
    } else {
        llam_io_handle_cqe(&node, &target_cqe);
        if (ordering == LINUX_WATCH_REARM_TARGET_FIRST) {
            llam_io_handle_cqe(&node, &control_cqe);
            encoded_op = NULL;
        }
    }

    if (!linux_watch_lifetime_test_is_linked(&node, kind, watch) ||
        linux_watch_lifetime_test_backend_refs(kind, watch) != 0U ||
        linux_watch_lifetime_test_active(kind, watch) ||
        linux_watch_lifetime_test_deactivate_queued(kind, watch) ||
        atomic_load_explicit(&node.pending_ops, memory_order_acquire) != 0U ||
        node.control_head == NULL || node.control_head->next != NULL ||
        node.control_head->kind != (kind == LINUX_WATCH_LIFETIME_ACCEPT
                                        ? LLAM_IO_CONTROL_ACCEPT_ACTIVATE
                                        : LLAM_IO_CONTROL_RECV_ACTIVATE)) {
        rc = fail_msg("terminal/control watch ordering did not queue one reactivation");
        goto done;
    }
    if (ordering == LINUX_WATCH_REARM_NATURAL_TERMINAL) {
        bool first_consumed = kind == LINUX_WATCH_LIFETIME_ACCEPT
                                  ? first.result == accepted_pipe[0]
                                  : first.result == 0;
        llam_io_req_t *remaining = kind == LINUX_WATCH_LIFETIME_ACCEPT
                                       ? ((llam_accept_watch_t *)watch)->wait_head
                                       : ((llam_recv_watch_t *)watch)->wait_head;

        if (!first_consumed || second.result != -202 || remaining != &second || second.next != NULL) {
            rc = fail_msg("terminal success was duplicated or failed to preserve remaining waiter");
            goto done;
        }
        {
            llam_io_control_op_t *activation = llam_take_node_controls(&node);

            if (activation == NULL || activation->next != NULL) {
                rc = fail_msg("terminal rearm did not expose exactly one activation control");
                goto done;
            }
            node.linux_submit_terminal = true;
            llam_io_submit_control_op(&node, activation);
            node.linux_submit_terminal = false;
            remaining = kind == LINUX_WATCH_LIFETIME_ACCEPT
                            ? ((llam_accept_watch_t *)watch)->wait_head
                            : node.recv_watches != NULL ? node.recv_watches->wait_head : NULL;
            if (remaining != NULL || second.result != -1 || second.error_code != EAGAIN ||
                (kind == LINUX_WATCH_LIFETIME_ACCEPT
                     ? ((llam_accept_watch_t *)watch)->activating
                     : node.recv_watches != NULL)) {
                rc = fail_msg("failed terminal reactivation did not error-complete remaining waiter");
                goto done;
            }
            if (kind == LINUX_WATCH_LIFETIME_RECV) {
                watch = NULL;
            }
        }
    } else {
        llam_io_req_t *remaining = kind == LINUX_WATCH_LIFETIME_ACCEPT
                                       ? ((llam_accept_watch_t *)watch)->wait_head
                                       : ((llam_recv_watch_t *)watch)->wait_head;

        if (first.result != -101 || second.result != -202 || remaining != &first || first.next != &second) {
            rc = fail_msg("deactivate terminal CQE consumed a waiter instead of rearming");
            goto done;
        }
    }
    rc = 0;

done:
    if (ring_ready) {
        io_uring_queue_exit(&node.ring);
        ring_ready = false;
        node.ring_ready = false;
    }
    if (watch_lock_ready) {
        if (node.linux_backend_control_head != NULL) {
            llam_linux_retire_backend_controls(&node);
        }
        llam_linux_retire_backend_watch_refs(&node);
        pthread_mutex_lock(&node.watch_lock);
        while (node.control_head != NULL) {
            llam_io_control_op_t *next = node.control_head->next;

            node.control_head->next = NULL;
            llam_io_control_op_destroy(&node, node.control_head);
            node.control_head = next;
        }
        node.control_tail = NULL;
        if (watch != NULL && linux_watch_lifetime_test_is_linked(&node, kind, watch)) {
            if (kind == LINUX_WATCH_LIFETIME_ACCEPT) {
                ((llam_accept_watch_t *)watch)->wait_head = NULL;
                ((llam_accept_watch_t *)watch)->wait_tail = NULL;
            } else if (kind == LINUX_WATCH_LIFETIME_RECV) {
                ((llam_recv_watch_t *)watch)->wait_head = NULL;
                ((llam_recv_watch_t *)watch)->wait_tail = NULL;
            }
            linux_watch_lifetime_test_destroy_locked(&node, kind, watch);
        }
        pthread_mutex_unlock(&node.watch_lock);
        pthread_mutex_destroy(&node.watch_lock);
    }
    free(encoded_op);
    close_if_valid(&accepted_pipe[0]);
    close_if_valid(&accepted_pipe[1]);
    return rc;
}

static int exercise_linux_terminal_watch_rearms(void) {
    for (unsigned kind = LINUX_WATCH_LIFETIME_ACCEPT;
         kind <= LINUX_WATCH_LIFETIME_RECV;
         ++kind) {
        for (unsigned ordering = LINUX_WATCH_REARM_NATURAL_TERMINAL;
             ordering <= LINUX_WATCH_REARM_CONTROL_FIRST;
             ++ordering) {
            if (exercise_linux_terminal_watch_rearm_one(
                    (linux_watch_lifetime_test_kind_t)kind, ordering) != 0) {
                return 1;
            }
        }
    }
    return 0;
}

static int exercise_linux_activation_rejects_deactivate_overlap(void) {
    for (unsigned kind = LINUX_WATCH_LIFETIME_POLL;
         kind <= LINUX_WATCH_LIFETIME_RECV;
         ++kind) {
        llam_runtime_t runtime;
        llam_node_t node;
        llam_io_control_op_t *op = NULL;
        void *watch = NULL;
        int init_rc;

        memset(&runtime, 0, sizeof(runtime));
        memset(&node, 0, sizeof(node));
        atomic_init(&runtime.fatal_errno, 0);
        atomic_init(&node.pending_ops, 0U);
        node.runtime = &runtime;
        node.event_fd = -1;
        init_rc = pthread_mutex_init(&node.watch_lock, NULL);
        if (init_rc != 0) {
            errno = init_rc;
            return fail_errno("watch lock init for activation/deactivate overlap failed");
        }
        init_rc = io_uring_queue_init(4U, &node.ring, 0U);
        if (init_rc != 0) {
            pthread_mutex_destroy(&node.watch_lock);
            if (io_uring_unavailable_for_direct_internal_test(init_rc)) {
                return 0;
            }
            errno = -init_rc;
            return fail_errno("io_uring init for activation/deactivate overlap failed");
        }
        node.ring_ready = true;
        if (kind == LINUX_WATCH_LIFETIME_POLL) {
            llam_poll_watch_t *poll_watch = calloc(1U, sizeof(*poll_watch));

            if (poll_watch != NULL) {
                poll_watch->accepts_waiters = true;
                poll_watch->activating = true;
                poll_watch->deactivate_queued = true;
                poll_watch->migrate_target_node_index = UINT_MAX;
                node.poll_watches = poll_watch;
            }
            watch = poll_watch;
        } else if (kind == LINUX_WATCH_LIFETIME_ACCEPT) {
            llam_accept_watch_t *accept_watch = calloc(1U, sizeof(*accept_watch));

            if (accept_watch != NULL) {
                accept_watch->accepts_waiters = true;
                accept_watch->activating = true;
                accept_watch->deactivate_queued = true;
                accept_watch->migrate_target_node_index = UINT_MAX;
                node.accept_watches = accept_watch;
            }
            watch = accept_watch;
        } else {
            llam_recv_watch_t *recv_watch = calloc(1U, sizeof(*recv_watch));

            if (recv_watch != NULL) {
                recv_watch->accepts_waiters = true;
                recv_watch->activating = true;
                recv_watch->deactivate_queued = true;
                recv_watch->migrate_target_node_index = UINT_MAX;
                node.recv_watches = recv_watch;
            }
            watch = recv_watch;
        }
        op = calloc(1U, sizeof(*op));
        if (watch == NULL || op == NULL) {
            free(op);
            io_uring_queue_exit(&node.ring);
            pthread_mutex_lock(&node.watch_lock);
            if (watch != NULL) {
                linux_watch_lifetime_test_destroy_locked(
                    &node, (linux_watch_lifetime_test_kind_t)kind, watch);
            }
            pthread_mutex_unlock(&node.watch_lock);
            pthread_mutex_destroy(&node.watch_lock);
            return fail_errno("activation/deactivate overlap allocation failed");
        }
        op->kind = kind == LINUX_WATCH_LIFETIME_POLL
                       ? LLAM_IO_CONTROL_POLL_ACTIVATE
                       : (kind == LINUX_WATCH_LIFETIME_ACCEPT
                              ? LLAM_IO_CONTROL_ACCEPT_ACTIVATE
                              : LLAM_IO_CONTROL_RECV_ACTIVATE);
        op->target = watch;
        llam_io_submit_control_op(&node, op);
        op = NULL;
        if (io_uring_sq_ready(&node.ring) != 0U ||
            linux_watch_lifetime_test_backend_refs(
                (linux_watch_lifetime_test_kind_t)kind, watch) != 0U ||
            linux_watch_lifetime_test_active((linux_watch_lifetime_test_kind_t)kind, watch) ||
            atomic_load_explicit(&node.pending_ops, memory_order_acquire) != 0U) {
            io_uring_queue_exit(&node.ring);
            pthread_mutex_lock(&node.watch_lock);
            linux_watch_lifetime_test_destroy_locked(
                &node, (linux_watch_lifetime_test_kind_t)kind, watch);
            pthread_mutex_unlock(&node.watch_lock);
            pthread_mutex_destroy(&node.watch_lock);
            return fail_msg("activation crossed an older deactivate generation");
        }
        pthread_mutex_lock(&node.watch_lock);
        linux_watch_lifetime_test_destroy_locked(
            &node, (linux_watch_lifetime_test_kind_t)kind, watch);
        pthread_mutex_unlock(&node.watch_lock);
        watch = NULL;

        if (kind == LINUX_WATCH_LIFETIME_POLL) {
            llam_poll_watch_t *poll_watch = calloc(1U, sizeof(*poll_watch));

            if (poll_watch != NULL) {
                poll_watch->accepts_waiters = false;
                poll_watch->destroy_pending = true;
                poll_watch->activating = true;
                poll_watch->migrate_target_node_index = UINT_MAX;
                node.poll_watches = poll_watch;
            }
            watch = poll_watch;
        } else if (kind == LINUX_WATCH_LIFETIME_ACCEPT) {
            llam_accept_watch_t *accept_watch = calloc(1U, sizeof(*accept_watch));

            if (accept_watch != NULL) {
                accept_watch->accepts_waiters = false;
                accept_watch->destroy_pending = true;
                accept_watch->activating = true;
                accept_watch->migrate_target_node_index = UINT_MAX;
                node.accept_watches = accept_watch;
            }
            watch = accept_watch;
        } else {
            llam_recv_watch_t *recv_watch = calloc(1U, sizeof(*recv_watch));

            if (recv_watch != NULL) {
                recv_watch->accepts_waiters = false;
                recv_watch->destroy_pending = true;
                recv_watch->activating = true;
                recv_watch->migrate_target_node_index = UINT_MAX;
                node.recv_watches = recv_watch;
            }
            watch = recv_watch;
        }
        op = calloc(1U, sizeof(*op));
        if (watch == NULL || op == NULL) {
            free(op);
            io_uring_queue_exit(&node.ring);
            pthread_mutex_lock(&node.watch_lock);
            if (watch != NULL) {
                linux_watch_lifetime_test_destroy_locked(
                    &node, (linux_watch_lifetime_test_kind_t)kind, watch);
            }
            pthread_mutex_unlock(&node.watch_lock);
            pthread_mutex_destroy(&node.watch_lock);
            return fail_errno("closed activation failure allocation failed");
        }
        op->kind = kind == LINUX_WATCH_LIFETIME_POLL
                       ? LLAM_IO_CONTROL_POLL_ACTIVATE
                       : (kind == LINUX_WATCH_LIFETIME_ACCEPT
                              ? LLAM_IO_CONTROL_ACCEPT_ACTIVATE
                              : LLAM_IO_CONTROL_RECV_ACTIVATE);
        op->target = watch;
        llam_io_submit_control_op(&node, op);
        op = NULL;
        if (linux_watch_lifetime_test_is_linked(
                &node, (linux_watch_lifetime_test_kind_t)kind, watch) ||
            io_uring_sq_ready(&node.ring) != 0U ||
            atomic_load_explicit(&node.pending_ops, memory_order_acquire) != 0U) {
            io_uring_queue_exit(&node.ring);
            pthread_mutex_lock(&node.watch_lock);
            if (linux_watch_lifetime_test_is_linked(
                    &node, (linux_watch_lifetime_test_kind_t)kind, watch)) {
                linux_watch_lifetime_test_destroy_locked(
                    &node, (linux_watch_lifetime_test_kind_t)kind, watch);
            }
            pthread_mutex_unlock(&node.watch_lock);
            pthread_mutex_destroy(&node.watch_lock);
            return fail_msg("failed closed activation left a detached watch resident");
        }
        watch = NULL;
        io_uring_queue_exit(&node.ring);
        pthread_mutex_destroy(&node.watch_lock);
    }
    return 0;
}

typedef struct linux_migration_cqe_thread_context {
    llam_node_t *node;
    struct io_uring_cqe *cqe;
    atomic_uint done;
} linux_migration_cqe_thread_context_t;

static void *linux_migration_cqe_thread_main(void *arg) {
    linux_migration_cqe_thread_context_t *context = arg;

    if (context != NULL && context->node != NULL && context->cqe != NULL) {
        llam_io_handle_cqe(context->node, context->cqe);
    }
    if (context != NULL) {
        atomic_store_explicit(&context->done, 1U, memory_order_release);
    }
    return NULL;
}

static int exercise_linux_migration_finalize_holds_watch_pin(void) {
    llam_runtime_t runtime;
    llam_node_t nodes[2];
    llam_accept_watch_t *watch = NULL;
    llam_io_control_op_t *encoded_op = NULL;
    struct io_uring_cqe control_cqe;
    linux_migration_cqe_thread_context_t context;
    pthread_t thread;
    int listener = -1;
    bool locks_ready[2] = {false, false};
    bool ring_ready = false;
    bool lifecycle_locked = false;
    bool thread_started = false;
    bool observed_deferred = false;
    int init_rc;
    int rc = 1;

    memset(&runtime, 0, sizeof(runtime));
    memset(nodes, 0, sizeof(nodes));
    memset(&control_cqe, 0, sizeof(control_cqe));
    memset(&context, 0, sizeof(context));
    atomic_init(&runtime.fatal_errno, 0);
    runtime.nodes = nodes;
    runtime.active_nodes = 2U;
    for (unsigned i = 0U; i < 2U; ++i) {
        nodes[i].runtime = &runtime;
        nodes[i].index = i;
        nodes[i].event_fd = -1;
        atomic_init(&nodes[i].pending_ops, i == 0U ? 1U : 0U);
        init_rc = pthread_mutex_init(&nodes[i].watch_lock, NULL);
        if (init_rc != 0) {
            errno = init_rc;
            goto done;
        }
        locks_ready[i] = true;
        nodes[i].watch_lock_initialized = true;
    }
    init_rc = io_uring_queue_init(8U, &nodes[0].ring, 0U);
    if (init_rc != 0) {
        if (io_uring_unavailable_for_direct_internal_test(init_rc)) {
            rc = 0;
            goto done;
        }
        errno = -init_rc;
        goto done;
    }
    ring_ready = true;
    nodes[0].ring_ready = true;
    if (make_loopback_listener(&listener) != 0) {
        goto done;
    }

    pthread_mutex_lock(&nodes[0].watch_lock);
    watch = llam_get_or_create_accept_watch_locked(&nodes[0], listener);
    if (watch != NULL) {
        watch->active = false;
        watch->deactivate_queued = true;
        watch->backend_refs = 0U;
        watch->migrate_target_node_index = 1U;
    }
    pthread_mutex_unlock(&nodes[0].watch_lock);
    if (watch == NULL) {
        goto done;
    }
    encoded_op = calloc(1U, sizeof(*encoded_op));
    if (encoded_op == NULL) {
        goto done;
    }
    encoded_op->kind = LLAM_IO_CONTROL_ACCEPT_DEACTIVATE;
    encoded_op->target = watch;
    llam_linux_track_backend_control(&nodes[0], encoded_op);
    control_cqe.user_data = llam_io_udata_encode(encoded_op, LLAM_IO_UDATA_CONTROL);
    control_cqe.res = 0;
    context.node = &nodes[0];
    context.cqe = &control_cqe;
    atomic_init(&context.done, 0U);

    llam_fd_watch_lifecycle_lock();
    lifecycle_locked = true;
    init_rc = pthread_create(&thread, NULL, linux_migration_cqe_thread_main, &context);
    if (init_rc != 0) {
        errno = init_rc;
        goto done;
    }
    thread_started = true;
    for (unsigned spin = 0U; spin < 100000U; ++spin) {
        unsigned refs;
        bool deactivate_queued;

        pthread_mutex_lock(&nodes[0].watch_lock);
        refs = watch->lifetime_refs;
        deactivate_queued = watch->deactivate_queued;
        pthread_mutex_unlock(&nodes[0].watch_lock);
        if (!deactivate_queued) {
            observed_deferred = refs == 1U;
            break;
        }
        sched_yield();
    }
    if (!observed_deferred) {
        rc = fail_msg("migration control did not pin watch before deferred finalize");
        goto done;
    }

    if (llam_forget_closed_fd_watch_state(&runtime, listener) != 0) {
        rc = fail_errno("close-state purge during deferred migration failed");
        goto done;
    }
    pthread_mutex_lock(&nodes[0].watch_lock);
    if (nodes[0].accept_watches != watch || watch->lifetime_refs != 1U ||
        !watch->destroy_pending || watch->accepts_waiters) {
        pthread_mutex_unlock(&nodes[0].watch_lock);
        rc = fail_msg("close reclaimed migration watch before deferred finalize released its pin");
        goto done;
    }
    pthread_mutex_unlock(&nodes[0].watch_lock);

    llam_fd_watch_lifecycle_unlock();
    lifecycle_locked = false;
    pthread_join(thread, NULL);
    thread_started = false;
    encoded_op = NULL;
    watch = NULL;
    if (nodes[0].accept_watches != NULL || nodes[1].accept_watches != NULL ||
        nodes[0].linux_backend_control_head != NULL ||
        atomic_load_explicit(&nodes[0].pending_ops, memory_order_acquire) != 0U ||
        atomic_load_explicit(&runtime.fatal_errno, memory_order_acquire) != 0) {
        rc = fail_msg("deferred migration pin did not reclaim and balance after close");
        goto done;
    }
    rc = 0;

done:
    if (lifecycle_locked) {
        llam_fd_watch_lifecycle_unlock();
        lifecycle_locked = false;
    }
    if (thread_started) {
        pthread_join(thread, NULL);
        thread_started = false;
        encoded_op = NULL;
    }
    if (ring_ready) {
        io_uring_queue_exit(&nodes[0].ring);
        ring_ready = false;
        nodes[0].ring_ready = false;
    }
    if (locks_ready[0] && nodes[0].linux_backend_control_head != NULL) {
        llam_linux_retire_backend_controls(&nodes[0]);
        encoded_op = NULL;
    }
    if (locks_ready[0]) {
        llam_linux_retire_backend_watch_refs(&nodes[0]);
        pthread_mutex_lock(&nodes[0].watch_lock);
        if (watch != NULL && nodes[0].accept_watches == watch) {
            watch->active = false;
            watch->activating = false;
            watch->deactivate_queued = false;
            watch->backend_refs = 0U;
            watch->lifetime_refs = 0U;
            llam_destroy_accept_watch_locked(&nodes[0], watch);
        }
        pthread_mutex_unlock(&nodes[0].watch_lock);
    }
    free(encoded_op);
    close_if_valid(&listener);
    for (unsigned i = 0U; i < 2U; ++i) {
        if (locks_ready[i]) {
            pthread_mutex_destroy(&nodes[i].watch_lock);
        }
    }
    return rc;
}

static int exercise_linux_migration_pin_saturation_fails_closed(void) {
    llam_runtime_t runtime;
    llam_node_t nodes[2];
    llam_accept_watch_t *watch = NULL;
    llam_io_control_op_t *encoded_op = NULL;
    struct io_uring_cqe control_cqe;
    bool locks_ready[2] = {false, false};
    bool ring_ready = false;
    int listener = -1;
    int init_rc;
    int rc = 1;

    memset(&runtime, 0, sizeof(runtime));
    memset(nodes, 0, sizeof(nodes));
    memset(&control_cqe, 0, sizeof(control_cqe));
    atomic_init(&runtime.fatal_errno, 0);
    runtime.nodes = nodes;
    runtime.active_nodes = 2U;
    for (unsigned i = 0U; i < 2U; ++i) {
        nodes[i].runtime = &runtime;
        nodes[i].index = i;
        nodes[i].event_fd = -1;
        atomic_init(&nodes[i].pending_ops, i == 0U ? 1U : 0U);
        init_rc = pthread_mutex_init(&nodes[i].watch_lock, NULL);
        if (init_rc != 0) {
            errno = init_rc;
            goto done;
        }
        locks_ready[i] = true;
        nodes[i].watch_lock_initialized = true;
    }
    init_rc = io_uring_queue_init(8U, &nodes[0].ring, 0U);
    if (init_rc != 0) {
        if (io_uring_unavailable_for_direct_internal_test(init_rc)) {
            rc = 0;
            goto done;
        }
        errno = -init_rc;
        goto done;
    }
    ring_ready = true;
    nodes[0].ring_ready = true;
    if (make_loopback_listener(&listener) != 0) {
        goto done;
    }
    pthread_mutex_lock(&nodes[0].watch_lock);
    watch = llam_get_or_create_accept_watch_locked(&nodes[0], listener);
    if (watch != NULL) {
        watch->active = false;
        watch->deactivate_queued = true;
        watch->migrate_target_node_index = 1U;
        watch->lifetime_refs = UINT_MAX;
    }
    pthread_mutex_unlock(&nodes[0].watch_lock);
    if (watch == NULL) {
        goto done;
    }
    encoded_op = calloc(1U, sizeof(*encoded_op));
    if (encoded_op == NULL) {
        goto done;
    }
    encoded_op->kind = LLAM_IO_CONTROL_ACCEPT_DEACTIVATE;
    encoded_op->target = watch;
    llam_linux_track_backend_control(&nodes[0], encoded_op);
    control_cqe.user_data = llam_io_udata_encode(encoded_op, LLAM_IO_UDATA_CONTROL);
    llam_io_handle_cqe(&nodes[0], &control_cqe);
    encoded_op = NULL;
    if (nodes[0].accept_watches != watch || nodes[1].accept_watches != NULL ||
        watch->lifetime_refs != UINT_MAX || watch->deactivate_queued ||
        nodes[0].linux_backend_control_head != NULL ||
        atomic_load_explicit(&nodes[0].pending_ops, memory_order_acquire) != 0U ||
        atomic_load_explicit(&runtime.fatal_errno, memory_order_acquire) != EOVERFLOW) {
        rc = fail_msg("saturated migration pin did not fail closed before deferred dereference");
        goto done;
    }
    rc = 0;

done:
    if (ring_ready) {
        io_uring_queue_exit(&nodes[0].ring);
        nodes[0].ring_ready = false;
    }
    if (locks_ready[0] && nodes[0].linux_backend_control_head != NULL) {
        llam_linux_retire_backend_controls(&nodes[0]);
        encoded_op = NULL;
    }
    if (locks_ready[0]) {
        pthread_mutex_lock(&nodes[0].watch_lock);
        if (watch != NULL && nodes[0].accept_watches == watch) {
            watch->active = false;
            watch->activating = false;
            watch->deactivate_queued = false;
            watch->backend_refs = 0U;
            watch->lifetime_refs = 0U;
            watch->migrate_target_node_index = UINT_MAX;
            llam_destroy_accept_watch_locked(&nodes[0], watch);
        }
        pthread_mutex_unlock(&nodes[0].watch_lock);
    }
    free(encoded_op);
    close_if_valid(&listener);
    for (unsigned i = 0U; i < 2U; ++i) {
        if (locks_ready[i]) {
            pthread_mutex_destroy(&nodes[i].watch_lock);
        }
    }
    return rc;
}

static int exercise_linux_activation_terminal_teardown_one(linux_watch_lifetime_test_kind_t kind) {
    llam_runtime_t runtime;
    llam_node_t node;
    llam_io_control_op_t *op = NULL;
    linux_submit_fault_state_t fault;
    void *watch = NULL;
    bool watch_lock_ready = false;
    bool submit_lock_ready = false;
    bool ring_ready = false;
    int init_rc;
    int rc = 1;

    memset(&runtime, 0, sizeof(runtime));
    memset(&node, 0, sizeof(node));
    memset(&fault, 0, sizeof(fault));
    fault.forced_result = -EBADF;
    atomic_init(&runtime.fatal_errno, 0);
    atomic_init(&node.pending_ops, 0U);
    runtime.nodes = &node;
    runtime.active_nodes = 1U;
    node.runtime = &runtime;
    node.event_fd = -1;

    init_rc = pthread_mutex_init(&node.watch_lock, NULL);
    if (init_rc != 0) {
        errno = init_rc;
        return fail_errno("watch lock init for Linux activation teardown failed");
    }
    watch_lock_ready = true;
    init_rc = pthread_mutex_init(&node.submit_lock, NULL);
    if (init_rc != 0) {
        errno = init_rc;
        rc = fail_errno("submit lock init for Linux activation teardown failed");
        goto done;
    }
    submit_lock_ready = true;
    init_rc = io_uring_queue_init(4U, &node.ring, 0U);
    if (init_rc != 0) {
        if (io_uring_unavailable_for_direct_internal_test(init_rc)) {
            rc = 0;
            goto done;
        }
        errno = -init_rc;
        rc = fail_errno("io_uring init for Linux activation teardown failed");
        goto done;
    }
    ring_ready = true;
    node.ring_ready = true;

    switch (kind) {
    case LINUX_WATCH_LIFETIME_POLL: {
        llam_poll_watch_t *poll_watch = calloc(1U, sizeof(*poll_watch));

        if (poll_watch == NULL) {
            goto done;
        }
        poll_watch->fd = -1;
        poll_watch->events = POLLIN;
        poll_watch->accepts_waiters = true;
        poll_watch->activating = true;
        node.poll_watches = poll_watch;
        watch = poll_watch;
        break;
    }
    case LINUX_WATCH_LIFETIME_ACCEPT: {
        llam_accept_watch_t *accept_watch = calloc(1U, sizeof(*accept_watch));

        if (accept_watch == NULL) {
            goto done;
        }
        accept_watch->fd = -1;
        accept_watch->accepts_waiters = true;
        accept_watch->activating = true;
        node.accept_watches = accept_watch;
        watch = accept_watch;
        break;
    }
    case LINUX_WATCH_LIFETIME_RECV: {
        llam_recv_watch_t *recv_watch = calloc(1U, sizeof(*recv_watch));

        if (recv_watch == NULL) {
            goto done;
        }
        recv_watch->fd = -1;
        recv_watch->accepts_waiters = true;
        recv_watch->activating = true;
        node.recv_watches = recv_watch;
        watch = recv_watch;
        break;
    }
    default:
        goto done;
    }

    op = calloc(1U, sizeof(*op));
    if (op == NULL) {
        goto done;
    }
    op->target = watch;
    op->kind = kind == LINUX_WATCH_LIFETIME_POLL ? LLAM_IO_CONTROL_POLL_ACTIVATE :
               (kind == LINUX_WATCH_LIFETIME_ACCEPT ? LLAM_IO_CONTROL_ACCEPT_ACTIVATE :
                                                      LLAM_IO_CONTROL_RECV_ACTIVATE);
    llam_io_submit_control_op(&node, op);
    op = NULL;
    if (io_uring_sq_ready(&node.ring) != 1U ||
        linux_watch_lifetime_test_backend_refs(kind, watch) != 1U ||
        !linux_watch_lifetime_test_active(kind, watch) ||
        atomic_load_explicit(&node.pending_ops, memory_order_acquire) != 1U) {
        rc = fail_msg("Linux activation SQE did not acquire backend watch ownership");
        goto done;
    }

    node.linux_submit_override = force_one_linux_submit_result;
    node.linux_submit_override_arg = &fault;
    init_rc = llam_node_submit_ring(&node);
    if (init_rc != -EBADF || fault.calls != 1U || fault.expected != 1U ||
        !node.linux_submit_terminal) {
        rc = fail_msg("Linux activation terminal submit fault was not retained for teardown");
        goto done;
    }
    io_uring_queue_exit(&node.ring);
    ring_ready = false;
    node.ring_ready = false;
    llam_linux_retire_backend_controls(&node);
    llam_linux_retire_backend_watch_refs(&node);
    if (!linux_watch_lifetime_test_is_linked(&node, kind, watch) ||
        linux_watch_lifetime_test_backend_refs(kind, watch) != 0U ||
        linux_watch_lifetime_test_active(kind, watch) ||
        atomic_load_explicit(&node.pending_ops, memory_order_acquire) != 0U ||
        node.retired_poll_watches != NULL || node.retired_accept_watches != NULL ||
        node.retired_recv_watches != NULL) {
        rc = fail_msg("Linux ring teardown did not retire activation watch ownership");
        goto done;
    }
    pthread_mutex_lock(&node.watch_lock);
    linux_watch_lifetime_test_destroy_locked(&node, kind, watch);
    pthread_mutex_unlock(&node.watch_lock);
    watch = NULL;
    rc = 0;

done:
    if (ring_ready) {
        io_uring_queue_exit(&node.ring);
        ring_ready = false;
        node.ring_ready = false;
    }
    if (watch_lock_ready) {
        llam_linux_retire_backend_controls(&node);
        llam_linux_retire_backend_watch_refs(&node);
        pthread_mutex_lock(&node.watch_lock);
        if (watch != NULL && linux_watch_lifetime_test_is_linked(&node, kind, watch)) {
            linux_watch_lifetime_test_destroy_locked(&node, kind, watch);
        }
        pthread_mutex_unlock(&node.watch_lock);
    }
    free(op);
    if (submit_lock_ready) {
        pthread_mutex_destroy(&node.submit_lock);
    }
    if (watch_lock_ready) {
        pthread_mutex_destroy(&node.watch_lock);
    }
    return rc;
}

static int exercise_linux_activation_terminal_teardown(void) {
    for (unsigned kind = LINUX_WATCH_LIFETIME_POLL;
         kind <= LINUX_WATCH_LIFETIME_RECV;
         ++kind) {
        if (exercise_linux_activation_terminal_teardown_one(
                (linux_watch_lifetime_test_kind_t)kind) != 0) {
            return 1;
        }
    }
    return 0;
}
#endif

static int exercise_linux_staged_cancel_control_retries_and_retires(void) {
#if LLAM_RUNTIME_BACKEND_LINUX
    if (exercise_linux_staged_cancel_submit_fault(-EAGAIN, false) != 0) {
        return 1;
    }
    if (exercise_linux_staged_cancel_submit_fault(0, false) != 0) {
        return 1;
    }
    if (exercise_linux_staged_cancel_submit_fault(-EBADF, true) != 0) {
        return 1;
    }
#endif
    return 0;
}

static int exercise_linux_wait_cqe_interrupt_policy(void) {
#if LLAM_RUNTIME_BACKEND_LINUX
    /*
     * io_uring_wait_cqe_timeout can be interrupted by process signals.  The
     * worker should simply retry later; treating EINTR like a backend failure
     * poisons otherwise healthy runtimes under profilers or signal-heavy tests.
     */
    if (llam_linux_wait_cqe_error_is_fatal(EINTR)) {
        return fail_msg("Linux CQ wait EINTR was treated as fatal");
    }
    if (llam_linux_wait_cqe_error_is_fatal(ETIME)) {
        return fail_msg("Linux CQ wait timeout was treated as fatal");
    }
    if (!llam_linux_wait_cqe_error_is_fatal(EIO)) {
        return fail_msg("Linux CQ wait EIO was not treated as fatal");
    }
#endif
    return 0;
}

static void free_control_ops(llam_node_t *node) {
    while (node != NULL && node->control_head != NULL) {
        llam_io_control_op_t *next = node->control_head->next;

        free(node->control_head);
        node->control_head = next;
    }
    if (node != NULL) {
        node->control_tail = NULL;
    }
}

static int exercise_empty_poll_watch_cancel_disarms_backend_work(void) {
    llam_node_t node;
    llam_poll_watch_t watch;
    llam_io_req_t req;
    bool kick_node = true;
    int rc;

    memset(&node, 0, sizeof(node));
    memset(&watch, 0, sizeof(watch));
    memset(&req, 0, sizeof(req));

    rc = pthread_mutex_init(&node.watch_lock, NULL);
    if (rc != 0) {
        errno = rc;
        return fail_errno("watch lock init for empty poll watch cancel test failed");
    }

    rc = pthread_mutex_lock(&node.watch_lock);
    if (rc != 0) {
        pthread_mutex_destroy(&node.watch_lock);
        errno = rc;
        return fail_errno("watch lock for empty poll watch cancel test failed");
    }
    watch.wait_head = &req;
    watch.wait_tail = &req;
    watch.activating = true;
    if (llam_node_queue_control_locked(&node, LLAM_IO_CONTROL_POLL_ACTIVATE, &watch) != 0) {
        (void)pthread_mutex_unlock(&node.watch_lock);
        pthread_mutex_destroy(&node.watch_lock);
        return fail_errno("queue poll activate for empty watch cancel test failed");
    }
    if (!llam_poll_watch_remove_waiter(&watch, &req) ||
        !llam_poll_watch_disarm_if_empty_locked(&node, &watch, &kick_node)) {
        (void)pthread_mutex_unlock(&node.watch_lock);
        free_control_ops(&node);
        pthread_mutex_destroy(&node.watch_lock);
        return fail_msg("empty activating poll watch was not disarmed after final waiter cancel");
    }
    if (watch.activating || watch.active || watch.deactivate_queued ||
        node.control_head != NULL || kick_node) {
        (void)pthread_mutex_unlock(&node.watch_lock);
        free_control_ops(&node);
        pthread_mutex_destroy(&node.watch_lock);
        return fail_msg("empty activating poll watch left backend work after final waiter cancel");
    }
    rc = pthread_mutex_unlock(&node.watch_lock);
    if (rc != 0) {
        pthread_mutex_destroy(&node.watch_lock);
        errno = rc;
        return fail_errno("watch unlock after activating poll watch cancel test failed");
    }

    memset(&watch, 0, sizeof(watch));
    kick_node = false;
    rc = pthread_mutex_lock(&node.watch_lock);
    if (rc != 0) {
        pthread_mutex_destroy(&node.watch_lock);
        errno = rc;
        return fail_errno("watch relock for active poll watch cancel test failed");
    }
    watch.active = true;
    /*
     * If the worker already consumed activation, cancel cannot drop it from the
     * control queue.  The empty watch must instead enqueue deactivate so the
     * active backend registration is drained before runtime shutdown.
     */
    if (!llam_poll_watch_disarm_if_empty_locked(&node, &watch, &kick_node)) {
        (void)pthread_mutex_unlock(&node.watch_lock);
        free_control_ops(&node);
        pthread_mutex_destroy(&node.watch_lock);
        return fail_msg("active empty poll watch was not queued for deactivate");
    }
    if (!watch.deactivate_queued || node.control_head == NULL ||
        node.control_head->kind != LLAM_IO_CONTROL_POLL_DEACTIVATE ||
        node.control_head->target != &watch || !kick_node) {
        (void)pthread_mutex_unlock(&node.watch_lock);
        free_control_ops(&node);
        pthread_mutex_destroy(&node.watch_lock);
        return fail_msg("active empty poll watch did not queue deactivate after final waiter cancel");
    }
    rc = pthread_mutex_unlock(&node.watch_lock);
    if (rc != 0) {
        free_control_ops(&node);
        pthread_mutex_destroy(&node.watch_lock);
        errno = rc;
        return fail_errno("watch unlock after active poll watch cancel test failed");
    }

    free_control_ops(&node);
    pthread_mutex_destroy(&node.watch_lock);
    return 0;
}

static int exercise_completion_drops_stale_cancel_control(void) {
#if LLAM_RUNTIME_BACKEND_KQUEUE || LLAM_RUNTIME_BACKEND_LINUX || LLAM_RUNTIME_BACKEND_WINDOWS
    llam_node_t *node;
    llam_io_req_t req;
    int lock_rc;

    if (init_runtime() != 0) {
        return fail_errno("runtime init failed for cancel-control completion race");
    }
    if (g_llam_runtime.nodes == NULL || g_llam_runtime.active_nodes == 0U) {
        llam_runtime_shutdown();
        return fail_msg("runtime initialized without an I/O node for cancel-control completion race");
    }

    node = &g_llam_runtime.nodes[0];
    memset(&req, 0, sizeof(req));
    llam_io_req_reset(&req, &g_llam_runtime, UINT_MAX, UINT_MAX);
    if (!llam_io_req_lifetime_activate(&req)) {
        llam_runtime_shutdown();
        return fail_errno("activate request for cancel-control completion race failed");
    }
    req.kind = LLAM_IO_KIND_READ;
    req.owner_shard = UINT_MAX;
    atomic_store_explicit(&req.wait_mode, LLAM_IO_WAIT_MODE_INFLIGHT, memory_order_release);
    atomic_store_explicit(&req.cancel_queued, 1U, memory_order_release);

    lock_rc = pthread_mutex_lock(&node->watch_lock);
    if (lock_rc != 0) {
        errno = lock_rc;
        llam_runtime_shutdown();
        return fail_errno("watch lock failed for cancel-control completion race");
    }
    if (llam_node_queue_control_locked(node, LLAM_IO_CONTROL_REQ_CANCEL, &req) != 0) {
        (void)pthread_mutex_unlock(&node->watch_lock);
        llam_runtime_shutdown();
        return fail_errno("queue cancel control failed for completion race");
    }
    lock_rc = pthread_mutex_unlock(&node->watch_lock);
    if (lock_rc != 0) {
        errno = lock_rc;
        llam_runtime_shutdown();
        return fail_errno("watch unlock failed for cancel-control completion race");
    }

    /*
     * This models natural I/O completion winning before the queued cancel
     * control is processed. Completion must unlink that control before the
     * waiting task can resume and release the request storage. On Linux this
     * also prevents a stale io_uring cancel SQE from targeting a later request
     * that reuses the same embedded request address.
     */
#if LLAM_RUNTIME_BACKEND_KQUEUE
    llam_io_complete_req(node, &req, 0, false);
#elif LLAM_RUNTIME_BACKEND_LINUX
    llam_io_complete_req(node, &req, 0, 0U, false);
#else
    llam_windows_complete_req(node, &req, 0, false);
#endif

    lock_rc = pthread_mutex_lock(&node->watch_lock);
    if (lock_rc != 0) {
        errno = lock_rc;
        llam_runtime_shutdown();
        return fail_errno("watch relock failed for cancel-control completion race");
    }
    if (node->control_head != NULL || node->control_tail != NULL) {
        (void)pthread_mutex_unlock(&node->watch_lock);
        llam_runtime_shutdown();
        return fail_msg("completion left a stale cancel control queued");
    }
    lock_rc = pthread_mutex_unlock(&node->watch_lock);
    if (lock_rc != 0) {
        errno = lock_rc;
        llam_runtime_shutdown();
        return fail_errno("watch final unlock failed for cancel-control completion race");
    }
    if (atomic_load_explicit(&req.cancel_queued, memory_order_acquire) != 0U ||
        atomic_load_explicit(&req.wait_mode, memory_order_acquire) != LLAM_IO_WAIT_MODE_NONE) {
        llam_runtime_shutdown();
        return fail_msg("completion did not clear cancel/wait ownership");
    }

    llam_runtime_shutdown();
#endif
    return 0;
}

static int exercise_completion_rejects_foreign_runtime_request(void) {
#if LLAM_RUNTIME_BACKEND_KQUEUE || LLAM_RUNTIME_BACKEND_LINUX || LLAM_RUNTIME_BACKEND_WINDOWS
    llam_runtime_t foreign_runtime;
    llam_node_t *node;
    llam_io_req_t req;

    if (init_runtime() != 0) {
        return fail_errno("runtime init failed for foreign completion owner check");
    }
    if (g_llam_runtime.nodes == NULL || g_llam_runtime.active_nodes == 0U) {
        llam_runtime_shutdown();
        return fail_msg("runtime initialized without an I/O node for foreign completion owner check");
    }

    memset(&foreign_runtime, 0, sizeof(foreign_runtime));
    node = &g_llam_runtime.nodes[0];
    memset(&req, 0, sizeof(req));
    req.kind = LLAM_IO_KIND_READ;
    req.owner_runtime = &foreign_runtime;
    req.owner_shard = 0U;
    atomic_init(&req.wait_mode, LLAM_IO_WAIT_MODE_NONE);
    atomic_init(&req.inflight_owner_shard, UINT_MAX);
    atomic_init(&req.abort_reason, LLAM_IO_ABORT_NONE);
    atomic_init(&req.cancel_queued, 0U);

    /*
     * A backend completion must never route a request through a node owned by
     * another runtime.  This models stale completion/user-data corruption and
     * must fail closed before any wakeup is attempted on the wrong scheduler.
     */
#if LLAM_RUNTIME_BACKEND_KQUEUE
    llam_io_complete_req(node, &req, 0, false);
#elif LLAM_RUNTIME_BACKEND_LINUX
    llam_io_complete_req(node, &req, 0, 0U, false);
#else
    llam_windows_complete_req(node, &req, 0, false);
#endif

    if (atomic_load_explicit(&g_llam_runtime.fatal_errno, memory_order_acquire) != EXDEV) {
        llam_runtime_shutdown();
        return fail_msg("foreign-runtime completion did not record EXDEV fatal");
    }

    llam_runtime_shutdown();
#endif
    return 0;
}

static int exercise_completion_rejects_unmatched_pending_decrement(void) {
#if LLAM_RUNTIME_BACKEND_KQUEUE || LLAM_RUNTIME_BACKEND_LINUX || LLAM_RUNTIME_BACKEND_WINDOWS
    llam_node_t *node;
    llam_io_req_t req;

    if (init_runtime() != 0) {
        return fail_errno("runtime init failed for unmatched completion pending check");
    }
    if (g_llam_runtime.nodes == NULL || g_llam_runtime.active_nodes == 0U) {
        llam_runtime_shutdown();
        return fail_msg("runtime initialized without an I/O node for unmatched completion pending check");
    }

    node = &g_llam_runtime.nodes[0];
    memset(&req, 0, sizeof(req));
    req.kind = LLAM_IO_KIND_READ;
    req.owner_runtime = &g_llam_runtime;
    req.owner_shard = 0U;
    atomic_init(&req.wait_mode, LLAM_IO_WAIT_MODE_NONE);
    atomic_init(&req.inflight_owner_shard, UINT_MAX);
    atomic_init(&req.abort_reason, LLAM_IO_ABORT_NONE);
    atomic_init(&req.cancel_queued, 0U);

    /*
     * A one-shot backend completion must have a matching pending op.  A stale
     * or forged packet with decrement_pending=true used to underflow the node
     * counter before validation; it must now fail closed without mutating it.
     */
#if LLAM_RUNTIME_BACKEND_KQUEUE
    llam_io_complete_req(node, &req, 0, true);
#elif LLAM_RUNTIME_BACKEND_LINUX
    llam_io_complete_req(node, &req, 0, 0U, true);
#else
    llam_windows_complete_req(node, &req, 0, true);
#endif

    if (atomic_load_explicit(&g_llam_runtime.fatal_errno, memory_order_acquire) != EINVAL ||
        atomic_load_explicit(&node->pending_ops, memory_order_acquire) != 0U) {
        llam_runtime_shutdown();
        return fail_msg("unmatched completion pending decrement was not rejected");
    }

    llam_runtime_shutdown();
#endif
    return 0;
}

static int exercise_pending_underflow_defers_fatal_under_watch_lock(void) {
#if LLAM_RUNTIME_BACKEND_KQUEUE || LLAM_RUNTIME_BACKEND_LINUX || LLAM_RUNTIME_BACKEND_WINDOWS
    llam_node_t *node;

    if (init_runtime() != 0) {
        return fail_errno("runtime init failed for locked pending underflow check");
    }
    if (g_llam_runtime.nodes == NULL || g_llam_runtime.active_nodes == 0U) {
        llam_runtime_shutdown();
        return fail_msg("runtime initialized without an I/O node for locked pending underflow check");
    }
    node = &g_llam_runtime.nodes[0];
    pthread_mutex_lock(&node->watch_lock);
    if (llam_node_complete_pending_ops(node, 1U)) {
        pthread_mutex_unlock(&node->watch_lock);
        llam_runtime_shutdown();
        return fail_msg("pending underflow succeeded under watch lock");
    }
    pthread_mutex_unlock(&node->watch_lock);
    if (atomic_load_explicit(&node->pending_ops,
                             memory_order_acquire) != 0U ||
        atomic_load_explicit(&g_llam_runtime.fatal_errno,
                             memory_order_acquire) != EINVAL) {
        llam_runtime_shutdown();
        return fail_msg("locked pending underflow was not deferred safely");
    }
    llam_runtime_shutdown();
#endif
    return 0;
}

static int exercise_submit_queue_rejects_foreign_runtime_request(void) {
#if LLAM_RUNTIME_BACKEND_KQUEUE || LLAM_RUNTIME_BACKEND_LINUX || LLAM_RUNTIME_BACKEND_WINDOWS
    llam_runtime_t foreign_runtime;
    llam_node_t *node;
    llam_io_req_t req;
    int lock_rc;
    bool queued;

    if (init_runtime() != 0) {
        return fail_errno("runtime init failed for foreign submit owner check");
    }
    if (g_llam_runtime.nodes == NULL || g_llam_runtime.active_nodes == 0U) {
        llam_runtime_shutdown();
        return fail_msg("runtime initialized without an I/O node for foreign submit owner check");
    }

    memset(&foreign_runtime, 0, sizeof(foreign_runtime));
    node = &g_llam_runtime.nodes[0];
    memset(&req, 0, sizeof(req));
    req.kind = LLAM_IO_KIND_READ;
    req.owner_runtime = &foreign_runtime;
    req.owner_shard = 0U;
    atomic_init(&req.wait_mode, LLAM_IO_WAIT_MODE_SUBMIT_QUEUE);
    atomic_init(&req.inflight_owner_shard, UINT_MAX);
    atomic_init(&req.abort_reason, LLAM_IO_ABORT_NONE);
    atomic_init(&req.cancel_queued, 0U);

    lock_rc = pthread_mutex_lock(&node->submit_lock);
    if (lock_rc != 0) {
        errno = lock_rc;
        llam_runtime_shutdown();
        return fail_errno("submit lock failed for foreign submit owner check");
    }
    queued = llam_queue_node_submit_locked(node, &req);
    lock_rc = pthread_mutex_unlock(&node->submit_lock);
    if (lock_rc != 0) {
        errno = lock_rc;
        llam_runtime_shutdown();
        return fail_errno("submit unlock failed for foreign submit owner check");
    }

    if (queued ||
        atomic_load_explicit(&g_llam_runtime.fatal_errno, memory_order_acquire) != EXDEV ||
        node->submit_head != NULL ||
        node->submit_tail != NULL) {
        llam_runtime_shutdown();
        return fail_msg("foreign-runtime submit request was accepted by node queue");
    }

    llam_runtime_shutdown();
#endif
    return 0;
}

static int exercise_submit_queue_rejects_pending_counter_overflow(void) {
#if LLAM_RUNTIME_BACKEND_KQUEUE || LLAM_RUNTIME_BACKEND_LINUX || LLAM_RUNTIME_BACKEND_WINDOWS
    llam_runtime_t runtime;
    llam_node_t node;
    llam_io_req_t req;
    int lock_rc;

    memset(&runtime, 0, sizeof(runtime));
    memset(&node, 0, sizeof(node));
    memset(&req, 0, sizeof(req));

    lock_rc = pthread_mutex_init(&node.submit_lock, NULL);
    if (lock_rc != 0) {
        errno = lock_rc;
        return fail_errno("submit lock init failed for pending counter overflow check");
    }

    node.runtime = &runtime;
    atomic_init(&node.pending_ops, UINT_MAX);
    req.kind = LLAM_IO_KIND_READ;
    req.owner_runtime = &runtime;
    req.owner_shard = 0U;
    atomic_init(&req.wait_mode, LLAM_IO_WAIT_MODE_SUBMIT_QUEUE);
    atomic_init(&req.inflight_owner_shard, UINT_MAX);
    atomic_init(&req.abort_reason, LLAM_IO_ABORT_NONE);
    atomic_init(&req.cancel_queued, 0U);

    /*
     * pending_ops gates worker sleep, shutdown diagnostics, and rehome
     * decisions.  A saturated counter must fail closed rather than wrapping to
     * zero while a request is linked into the submit queue.
     */
    errno = 0;
    if (llam_node_submit_io_req(&node, &req) ||
        errno != EOVERFLOW ||
        atomic_load_explicit(&node.pending_ops, memory_order_acquire) != UINT_MAX ||
        node.submit_head != NULL ||
        node.submit_tail != NULL) {
        (void)pthread_mutex_destroy(&node.submit_lock);
        return fail_msg("submit queue pending counter overflow was not rejected");
    }

    (void)pthread_mutex_destroy(&node.submit_lock);
#endif
    return 0;
}

static int exercise_block_worker_rejects_pending_counter_underflow(void) {
    llam_runtime_t runtime;
    llam_block_job_t job;
    int lock_rc;

    memset(&runtime, 0, sizeof(runtime));
    memset(&job, 0, sizeof(job));

    lock_rc = pthread_mutex_init(&runtime.block_lock, NULL);
    if (lock_rc != 0) {
        errno = lock_rc;
        return fail_errno("block lock init failed for pending underflow check");
    }

    atomic_init(&runtime.block_pending, 0U);
    atomic_init(&runtime.block_active, 0U);
    atomic_init(&runtime.block_active_peak, 0U);
    atomic_init(&runtime.block_threads_entered, 0U);
    atomic_init(&runtime.block_threads_exited, 0U);
    atomic_init(&runtime.block_threads_live, 0U);
    atomic_init(&runtime.block_job_free, NULL);
    atomic_init(&runtime.shutdown_requested, true);
    atomic_init(&runtime.fatal_errno, 0);

    /*
     * This models a stale/cancelled block job found on the worker queue without
     * a matching pending-job credit.  The worker must fail closed and preserve
     * the counter instead of wrapping it to UINT_MAX.
     */
    atomic_init(&job.state, LLAM_BLOCK_JOB_ABORTED);
    runtime.block_head = &job;
    runtime.block_tail = &job;

    (void)llam_block_worker_main(&runtime);
    if (atomic_load_explicit(&runtime.block_pending, memory_order_acquire) != 0U ||
        atomic_load_explicit(&runtime.fatal_errno, memory_order_acquire) != EINVAL ||
        atomic_load_explicit(&runtime.block_threads_entered, memory_order_acquire) != 1U ||
        atomic_load_explicit(&runtime.block_threads_exited, memory_order_acquire) != 1U ||
        atomic_load_explicit(&runtime.block_threads_live, memory_order_acquire) != 0U) {
        (void)pthread_mutex_destroy(&runtime.block_lock);
        return fail_msg("block worker pending counter underflow was not rejected");
    }

    (void)pthread_mutex_destroy(&runtime.block_lock);
    return 0;
}

static int exercise_block_worker_rejects_active_counter_overflow(void) {
    llam_runtime_t runtime;
    llam_block_job_t job;
    llam_task_t task;
    llam_wait_node_t node;
    atomic_uint callback_calls;
    int lock_rc;

    memset(&runtime, 0, sizeof(runtime));
    memset(&job, 0, sizeof(job));
    memset(&task, 0, sizeof(task));
    memset(&node, 0, sizeof(node));

    lock_rc = pthread_mutex_init(&runtime.block_lock, NULL);
    if (lock_rc != 0) {
        errno = lock_rc;
        return fail_errno("block lock init failed for active overflow check");
    }

    atomic_init(&runtime.block_pending, 1U);
    atomic_init(&runtime.block_active, UINT_MAX);
    atomic_init(&runtime.block_active_peak, UINT_MAX);
    atomic_init(&runtime.block_threads_entered, 0U);
    atomic_init(&runtime.block_threads_exited, 0U);
    atomic_init(&runtime.block_threads_live, 0U);
    atomic_init(&runtime.block_job_free, NULL);
    atomic_init(&runtime.shutdown_requested, true);
    atomic_init(&runtime.fatal_errno, 0);
    atomic_init(&task.wake_error_code, 0);
    atomic_init(&node.wake_armed, 0U);
    atomic_init(&node.wake_completed, 0U);
    atomic_init(&node.wake_queued, 0U);
    atomic_init(&callback_calls, 0U);

    /*
     * A saturated active-worker counter used to wrap through zero and still run
     * user code.  The worker must fail closed before invoking the callback,
     * preserve the active counter, and consume the queued pending credit.
     */
    node.task = &task;
    job.fn = count_block_callback;
    job.arg = &callback_calls;
    job.task = &task;
    job.wait_node = &node;
    atomic_init(&job.result, NULL);
    atomic_init(&job.error_code, 0);
    atomic_init(&job.state, LLAM_BLOCK_JOB_QUEUED);
    runtime.block_head = &job;
    runtime.block_tail = &job;

    (void)llam_block_worker_main(&runtime);
    if (atomic_load_explicit(&callback_calls, memory_order_acquire) != 0U ||
        atomic_load_explicit(&runtime.block_active, memory_order_acquire) != UINT_MAX ||
        atomic_load_explicit(&runtime.block_pending, memory_order_acquire) != 0U ||
        atomic_load_explicit(&runtime.fatal_errno, memory_order_acquire) != EOVERFLOW ||
        atomic_load_explicit(&task.wake_error_code, memory_order_acquire) != EOVERFLOW ||
        atomic_load_explicit(&job.state, memory_order_acquire) != LLAM_BLOCK_JOB_ABORTED ||
        atomic_load_explicit(&runtime.block_threads_entered, memory_order_acquire) != 1U ||
        atomic_load_explicit(&runtime.block_threads_exited, memory_order_acquire) != 1U ||
        atomic_load_explicit(&runtime.block_threads_live, memory_order_acquire) != 0U) {
        (void)pthread_mutex_destroy(&runtime.block_lock);
        return fail_msg("block worker active counter overflow was not rejected");
    }

    (void)pthread_mutex_destroy(&runtime.block_lock);
    return 0;
}

static int exercise_task_scan_ref_counter_saturation_is_rejected(void) {
    llam_runtime_t runtime;
    llam_task_t task;

    memset(&runtime, 0, sizeof(runtime));
    memset(&task, 0, sizeof(task));
    atomic_init(&runtime.fatal_errno, 0);
    atomic_init(&task.scan_refs, UINT_MAX);

    /*
     * Shutdown/cancel scans pin raw task pointers after finding them under
     * shard/token locks.  A saturated counter must not wrap to zero, because
     * reclaim treats zero as permission to free the task object.
     */
    errno = 0;
    if (llam_task_scan_ref_try_acquire(&runtime, &task) ||
        errno != EOVERFLOW ||
        atomic_load_explicit(&task.scan_refs, memory_order_acquire) != UINT_MAX ||
        atomic_load_explicit(&runtime.fatal_errno, memory_order_acquire) != EOVERFLOW) {
        return fail_msg("task scan-ref overflow was not rejected");
    }

    atomic_store_explicit(&runtime.fatal_errno, 0, memory_order_release);
    atomic_store_explicit(&task.scan_refs, 0U, memory_order_release);
    errno = 0;
    if (llam_task_scan_ref_release(&runtime, &task) ||
        errno != EINVAL ||
        atomic_load_explicit(&task.scan_refs, memory_order_acquire) != 0U ||
        atomic_load_explicit(&runtime.fatal_errno, memory_order_acquire) != EINVAL) {
        return fail_msg("task scan-ref underflow was not rejected");
    }

    atomic_store_explicit(&runtime.fatal_errno, 0, memory_order_release);
    atomic_store_explicit(&task.scan_refs, UINT_MAX, memory_order_release);
    errno = 0;
    if (llam_task_wait_scan_refs_quiescent(&runtime, &task) != -1 ||
        errno != EOVERFLOW ||
        atomic_load_explicit(&runtime.fatal_errno, memory_order_acquire) != EOVERFLOW) {
        return fail_msg("task scan-ref reclaim wait did not fail closed on saturation");
    }

    atomic_store_explicit(&runtime.fatal_errno, 0, memory_order_release);
    atomic_store_explicit(&task.scan_refs, 0U, memory_order_release);
    if (llam_task_wait_scan_refs_quiescent(&runtime, &task) != 0 ||
        atomic_load_explicit(&runtime.fatal_errno, memory_order_acquire) != 0) {
        return fail_msg("task scan-ref quiescent wait rejected zero refs");
    }
    return 0;
}

static int exercise_norm_depth_counter_wrap_is_rejected(void) {
    llam_runtime_t runtime;
    llam_shard_t shard;
    llam_task_t task;
    llam_task_t *popped;

    memset(&runtime, 0, sizeof(runtime));
    memset(&shard, 0, sizeof(shard));
    memset(&task, 0, sizeof(task));
    runtime.active_shards = 1U;
    runtime.shards = &shard;
    atomic_init(&runtime.fatal_errno, 0);
    shard.runtime = &runtime;
    atomic_init(&shard.norm_depth, UINT_MAX);

    /*
     * Normal-queue depth feeds stealing, scaling, and drain heuristics.  If a
     * corrupted/saturated value wraps to zero, those paths can misclassify a
     * loaded shard as empty.
     */
    if (llam_norm_queue_push_owner_locked(&shard, &task) ||
        atomic_load_explicit(&shard.norm_depth, memory_order_acquire) != UINT_MAX ||
        atomic_load_explicit(&runtime.fatal_errno, memory_order_acquire) != EOVERFLOW) {
        return fail_msg("norm depth enqueue overflow was not rejected");
    }

    memset(&shard.norm_q, 0, sizeof(shard.norm_q));
    task.queue_next = NULL;
    task.queue_prev = NULL;
    atomic_store_explicit(&runtime.fatal_errno, 0, memory_order_release);
    atomic_store_explicit(&shard.norm_depth, 0U, memory_order_release);
    llam_queue_push_tail(&shard.norm_q, &task);
    popped = llam_norm_queue_pop_owner_locked(&shard);
    if (popped != &task ||
        atomic_load_explicit(&shard.norm_depth, memory_order_acquire) != 0U ||
        atomic_load_explicit(&runtime.fatal_errno, memory_order_acquire) != EINVAL) {
        return fail_msg("norm depth dequeue underflow was not rejected");
    }
    return 0;
}

typedef struct cldeque_delayed_thief_state {
    pthread_mutex_t lock;
    pthread_cond_t cv;
    unsigned hook_reached;
    unsigned release_hook;
    llam_shard_t *victim;
    llam_task_t *stolen;
} cldeque_delayed_thief_state_t;

static void cldeque_delayed_thief_hook(void *context) {
    cldeque_delayed_thief_state_t *state = context;

    pthread_mutex_lock(&state->lock);
    state->hook_reached = 1U;
    pthread_cond_broadcast(&state->cv);
    while (state->release_hook == 0U) {
        pthread_cond_wait(&state->cv, &state->lock);
    }
    pthread_mutex_unlock(&state->lock);
}

static void *cldeque_delayed_thief_thread_main(void *context) {
    cldeque_delayed_thief_state_t *state = context;

    state->stolen = llam_norm_queue_steal(state->victim);
    return NULL;
}

static int exercise_cldeque_delayed_thief_preserves_wrapped_task(void) {
    llam_runtime_t runtime;
    llam_shard_t shard;
    cldeque_delayed_thief_state_t state;
    llam_task_t *tasks = NULL;
    pthread_t thief;
    bool mutex_initialized = false;
    bool cv_initialized = false;
    bool thief_started = false;
    const char *failure = NULL;
    size_t i;

    memset(&runtime, 0, sizeof(runtime));
    memset(&shard, 0, sizeof(shard));
    memset(&state, 0, sizeof(state));
    tasks = calloc(LLAM_NORM_QUEUE_CAP + 1U, sizeof(*tasks));
    if (tasks == NULL) {
        return fail_errno("cldeque wraparound task allocation failed");
    }
    if (pthread_mutex_init(&state.lock, NULL) != 0) {
        failure = "cldeque wraparound mutex init failed";
        goto cleanup;
    }
    mutex_initialized = true;
    if (pthread_cond_init(&state.cv, NULL) != 0) {
        failure = "cldeque wraparound condition init failed";
        goto cleanup;
    }
    cv_initialized = true;

    runtime.experimental_lockfree_normq = 1U;
    shard.runtime = &runtime;
    state.victim = &shard;
    atomic_init(&runtime.fatal_errno, 0);
    atomic_init(&shard.norm_depth, 0U);
    llam_cldeque_init(&shard.norm_cldeque);
    llam_sched_test_set_cldeque_steal_claimed_hook(
        cldeque_delayed_thief_hook, &state);

    if (!llam_norm_queue_push_owner_locked(&shard, &tasks[0])) {
        failure = "cldeque initial task push failed";
        goto cleanup;
    }
    if (pthread_create(
            &thief,
            NULL,
            cldeque_delayed_thief_thread_main,
            &state) != 0) {
        failure = "cldeque delayed thief thread create failed";
        goto cleanup;
    }
    thief_started = true;

    pthread_mutex_lock(&state.lock);
    while (state.hook_reached == 0U) {
        pthread_cond_wait(&state.cv, &state.lock);
    }
    pthread_mutex_unlock(&state.lock);

    for (i = 1U; i <= LLAM_NORM_QUEUE_CAP; ++i) {
        if (!llam_norm_queue_push_owner_locked(&shard, &tasks[i])) {
            failure = "cldeque replacement task push failed";
            goto cleanup;
        }
    }
    if (atomic_load_explicit(
            &shard.norm_cldeque.buffer[0],
            memory_order_acquire) != &tasks[LLAM_NORM_QUEUE_CAP]) {
        failure = "cldeque final replacement did not wrap to slot zero";
        goto cleanup;
    }

    pthread_mutex_lock(&state.lock);
    state.release_hook = 1U;
    pthread_cond_broadcast(&state.cv);
    pthread_mutex_unlock(&state.lock);
    pthread_join(thief, NULL);
    thief_started = false;
    llam_sched_test_set_cldeque_steal_claimed_hook(NULL, NULL);

    if (state.stolen != &tasks[0]) {
        failure = "cldeque thief returned the wrong claimed task";
        goto cleanup;
    }
    for (i = LLAM_NORM_QUEUE_CAP; i > 0U; --i) {
        llam_task_t *popped =
            llam_norm_queue_pop_owner_locked(&shard);

        if (popped != &tasks[i]) {
            failure = i == LLAM_NORM_QUEUE_CAP
                          ? "cldeque delayed thief erased wrapped task"
                          : "cldeque replacement pop order was corrupted";
            goto cleanup;
        }
    }
    if (llam_norm_queue_pop_owner_locked(&shard) != NULL ||
        atomic_load_explicit(
            &shard.norm_depth,
            memory_order_acquire) != 0U) {
        failure = "cldeque wraparound drain did not finish empty";
    }

cleanup:
    if (thief_started) {
        pthread_mutex_lock(&state.lock);
        state.release_hook = 1U;
        pthread_cond_broadcast(&state.cv);
        pthread_mutex_unlock(&state.lock);
        pthread_join(thief, NULL);
    }
    llam_sched_test_set_cldeque_steal_claimed_hook(NULL, NULL);
    if (cv_initialized) {
        pthread_cond_destroy(&state.cv);
    }
    if (mutex_initialized) {
        pthread_mutex_destroy(&state.lock);
    }
    free(tasks);
    return failure != NULL ? fail_msg(failure) : 0;
}

static int exercise_channel_inflight_waiter_counter_overflow_is_rejected(void) {
    llam_channel_t *handle;
    llam_channel_t *channel;
    llam_wait_node_t node;

    if (init_runtime() != 0) {
        return fail_errno("runtime init failed for channel inflight overflow check");
    }

    handle = llam_channel_create(1U);
    if (handle == NULL) {
        llam_runtime_shutdown();
        return fail_errno("channel create failed for inflight overflow check");
    }
    channel = llam_channel_resolve_public_handle(handle);
    if (channel == NULL) {
        llam_channel_destroy(handle);
        llam_runtime_shutdown();
        return fail_errno("channel resolve failed for inflight overflow check");
    }

    memset(&node, 0, sizeof(node));
    llam_wait_node_reset(&node, channel->owner_runtime, UINT_MAX);
    node.error_code = 0;

    pthread_mutex_lock(&channel->lock);
    llam_wait_queue_push_tail(&channel->recv_waiters, &node);
    atomic_store_explicit(&channel->inflight_waiters, UINT_MAX, memory_order_release);
    pthread_mutex_unlock(&channel->lock);
    llam_channel_end_public_op(channel);

    /*
     * This forces the normal close path to pop a waiter while the destroy guard
     * is saturated.  Raw unsigned increment used to wrap the guard to zero,
     * allowing destroy to recycle a channel with an unconsumed popped waiter.
     */
    errno = 0;
    if (llam_channel_close(handle) != 0) {
        int saved_errno = errno;

        atomic_store_explicit(&channel->inflight_waiters, 0U, memory_order_release);
        (void)llam_channel_destroy(handle);
        llam_runtime_shutdown();
        errno = saved_errno;
        return fail_errno("channel close failed for inflight overflow check");
    }
    errno = 0;
    if (llam_channel_destroy(handle) == 0 || errno != EBUSY) {
        llam_runtime_shutdown();
        return fail_msg("channel inflight waiter overflow did not keep destroy busy");
    }

    atomic_store_explicit(&channel->inflight_waiters, 0U, memory_order_release);
    if (llam_channel_destroy(handle) != 0) {
        llam_runtime_shutdown();
        return fail_errno("channel cleanup destroy failed after inflight overflow check");
    }
    llam_runtime_shutdown();
    return 0;
}

static int exercise_channel_inflight_waiter_counter_underflow_is_rejected(void) {
    llam_channel_t *handle;
    llam_channel_t *channel;

    if (init_runtime() != 0) {
        return fail_errno("runtime init failed for channel inflight underflow check");
    }

    handle = llam_channel_create(1U);
    if (handle == NULL) {
        llam_runtime_shutdown();
        return fail_errno("channel create failed for inflight underflow check");
    }
    channel = llam_channel_resolve_public_handle(handle);
    if (channel == NULL) {
        llam_channel_destroy(handle);
        llam_runtime_shutdown();
        return fail_errno("channel resolve failed for inflight underflow check");
    }

    /*
     * A stale waiter-consume path must not wrap the destroy guard to UINT_MAX.
     * Preserve zero and record a fatal invariant violation instead.
     */
    errno = 0;
    llam_channel_waiter_consumed(channel);
    if (atomic_load_explicit(&channel->inflight_waiters, memory_order_acquire) != 0U ||
        atomic_load_explicit(&g_llam_runtime.fatal_errno, memory_order_acquire) != EINVAL) {
        llam_channel_end_public_op(channel);
        llam_channel_destroy(handle);
        llam_runtime_shutdown();
        return fail_msg("channel inflight waiter underflow was not rejected");
    }

    llam_channel_end_public_op(channel);
    if (llam_channel_destroy(handle) != 0) {
        llam_runtime_shutdown();
        return fail_errno("channel cleanup destroy failed after inflight underflow check");
    }
    llam_runtime_shutdown();
    return 0;
}

static int exercise_cond_inflight_waiter_counter_overflow_is_rejected(void) {
    llam_cond_t *handle;
    llam_cond_t *cond;
    llam_wait_node_t node;

    if (init_runtime() != 0) {
        return fail_errno("runtime init failed for cond inflight overflow check");
    }

    handle = llam_cond_create();
    if (handle == NULL) {
        llam_runtime_shutdown();
        return fail_errno("cond create failed for inflight overflow check");
    }
    cond = llam_cond_resolve_public_handle(handle);
    if (cond == NULL) {
        llam_cond_destroy(handle);
        llam_runtime_shutdown();
        return fail_errno("cond resolve failed for inflight overflow check");
    }

    memset(&node, 0, sizeof(node));
    llam_wait_node_reset(&node, cond->owner_runtime, UINT_MAX);
    node.error_code = 0;

    pthread_mutex_lock(&cond->lock);
    llam_wait_queue_push_tail(&cond->waiters, &node);
    atomic_store_explicit(&cond->inflight_waiters, UINT_MAX, memory_order_release);
    pthread_mutex_unlock(&cond->lock);
    llam_cond_end_public_op(cond);

    /*
     * Signal has the same lifetime window as channel close: the waiter is no
     * longer on the queue but has not returned from wait yet.  The in-flight
     * guard must fail closed if saturation is detected.
     */
    errno = 0;
    if (llam_cond_signal(handle) != 0) {
        int saved_errno = errno;

        atomic_store_explicit(&cond->inflight_waiters, 0U, memory_order_release);
        (void)llam_cond_destroy(handle);
        llam_runtime_shutdown();
        errno = saved_errno;
        return fail_errno("cond signal failed for inflight overflow check");
    }
    errno = 0;
    if (llam_cond_destroy(handle) == 0 || errno != EBUSY) {
        llam_runtime_shutdown();
        return fail_msg("cond inflight waiter overflow did not keep destroy busy");
    }

    atomic_store_explicit(&cond->inflight_waiters, 0U, memory_order_release);
    if (llam_cond_destroy(handle) != 0) {
        llam_runtime_shutdown();
        return fail_errno("cond cleanup destroy failed after inflight overflow check");
    }
    llam_runtime_shutdown();
    return 0;
}

static int exercise_inflight_waiter_counter_underflow_is_rejected(void) {
    if (init_runtime() != 0) {
        return fail_errno("runtime init failed for inflight waiter underflow check");
    }
    if (g_llam_runtime.active_shards == 0U) {
        llam_runtime_shutdown();
        return fail_msg("runtime initialized without a shard for inflight waiter underflow check");
    }

    /*
     * Backend completion and rehome paths maintain shard pressure counters by
     * paired +/- calls.  A stale completion or corrupted ownership transition
     * must not wrap the unsigned counter to UINT_MAX, otherwise diagnostics and
     * scaling decisions can believe the shard has permanent I/O pressure.
     */
    pthread_mutex_lock(&g_llam_runtime.shards[0].lock);
    llam_shard_note_inflight_io_waiter(&g_llam_runtime, 0U, -1);
    pthread_mutex_unlock(&g_llam_runtime.shards[0].lock);
    if (atomic_load_explicit(&g_llam_runtime.shards[0].inflight_io_waiters, memory_order_acquire) != 0U ||
        atomic_load_explicit(&g_llam_runtime.fatal_errno, memory_order_acquire) != EINVAL) {
        llam_runtime_shutdown();
        return fail_msg("inflight waiter underflow was not rejected");
    }

    llam_runtime_shutdown();
    return 0;
}

static int exercise_inflight_waiter_counter_overflow_is_rejected(void) {
    if (init_runtime() != 0) {
        return fail_errno("runtime init failed for inflight waiter overflow check");
    }
    if (g_llam_runtime.active_shards == 0U) {
        llam_runtime_shutdown();
        return fail_msg("runtime initialized without a shard for inflight waiter overflow check");
    }

    atomic_store_explicit(&g_llam_runtime.shards[0].inflight_io_waiters, UINT_MAX, memory_order_release);
    pthread_mutex_lock(&g_llam_runtime.shards[0].lock);
    llam_shard_note_inflight_io_waiter(&g_llam_runtime, 0U, 1);
    pthread_mutex_unlock(&g_llam_runtime.shards[0].lock);
    if (atomic_load_explicit(&g_llam_runtime.shards[0].inflight_io_waiters, memory_order_acquire) != UINT_MAX ||
        atomic_load_explicit(&g_llam_runtime.fatal_errno, memory_order_acquire) != EOVERFLOW) {
        llam_runtime_shutdown();
        return fail_msg("inflight waiter overflow was not rejected");
    }

    llam_runtime_shutdown();
    return 0;
}

static int exercise_active_io_waiter_counter_overflow_is_rejected(void) {
    llam_task_t task;
    llam_io_req_t req;

    if (init_runtime() != 0) {
        return fail_errno("runtime init failed for active I/O waiter overflow check");
    }

    memset(&task, 0, sizeof(task));
    memset(&req, 0, sizeof(req));
    task.owner_runtime = &g_llam_runtime;
    atomic_init(&task.active_io_req, NULL);
    atomic_store_explicit(&g_llam_runtime.active_io_waiters, UINT_MAX, memory_order_release);
    atomic_store_explicit(&g_llam_runtime.fatal_errno, 0, memory_order_release);

    /*
     * active_io_waiters is a runtime-wide pressure signal.  A saturated value
     * must not wrap to zero when a task begins an I/O wait, otherwise shutdown
     * and scaler diagnostics can temporarily classify the runtime as idle.
     */
    llam_task_set_io_tracking(&task, &req, 0U);
    if (atomic_load_explicit(&g_llam_runtime.active_io_waiters, memory_order_acquire) != UINT_MAX ||
        atomic_load_explicit(&g_llam_runtime.fatal_errno, memory_order_acquire) != EOVERFLOW ||
        atomic_load_explicit(&task.active_io_req, memory_order_acquire) != NULL) {
        llam_runtime_shutdown();
        return fail_msg("active I/O waiter overflow was not rejected");
    }

    llam_runtime_shutdown();
    return 0;
}

static int exercise_active_io_waiter_counter_underflow_is_rejected(void) {
    llam_task_t task;
    llam_io_req_t req;

    if (init_runtime() != 0) {
        return fail_errno("runtime init failed for active I/O waiter underflow check");
    }

    memset(&task, 0, sizeof(task));
    memset(&req, 0, sizeof(req));
    task.owner_runtime = &g_llam_runtime;
    atomic_init(&task.active_io_req, &req);
    atomic_store_explicit(&g_llam_runtime.active_io_waiters, 0U, memory_order_release);
    atomic_store_explicit(&g_llam_runtime.fatal_errno, 0, memory_order_release);

    /*
     * Clearing a stale active I/O owner must not poison the runtime-wide
     * pressure counter by wrapping it to UINT_MAX.  The owner pointer is still
     * cleared so teardown can make progress after recording the invariant
     * violation.
     */
    llam_task_clear_wait_tracking_or_abort(&task);
    if (atomic_load_explicit(&g_llam_runtime.active_io_waiters, memory_order_acquire) != 0U ||
        atomic_load_explicit(&g_llam_runtime.fatal_errno, memory_order_acquire) != EINVAL ||
        atomic_load_explicit(&task.active_io_req, memory_order_acquire) != NULL) {
        llam_runtime_shutdown();
        return fail_msg("active I/O waiter underflow was not rejected");
    }

    llam_runtime_shutdown();
    return 0;
}

static int exercise_live_task_sum_saturates_on_overflow(void) {
#if !LLAM_RUNTIME_BACKEND_WINDOWS
    llam_runtime_t runtime;
    llam_shard_t shards[2];

    /*
     * Shutdown diagnostics and parked-waiter cancellation size temporary
     * snapshots from llam_runtime_live_tasks().  If shard-local counters are
     * corrupted or extremely high, the diagnostic total must saturate instead
     * of wrapping to a small value that can hide live parked work.
     */
    memset(&runtime, 0, sizeof(runtime));
    memset(shards, 0, sizeof(shards));
    runtime.shards = shards;
    runtime.active_shards = 2U;

    atomic_init(&shards[0].live_tasks, UINT_MAX);
    atomic_init(&shards[1].live_tasks, 1U);
    if (llam_runtime_live_tasks(&runtime) != UINT_MAX) {
        return fail_msg("live task sum wrapped past UINT_MAX");
    }

    atomic_store_explicit(&shards[0].live_tasks, UINT_MAX - 1U, memory_order_release);
    atomic_store_explicit(&shards[1].live_tasks, 2U, memory_order_release);
    if (llam_runtime_live_tasks(&runtime) != UINT_MAX) {
        return fail_msg("live task sum did not saturate at UINT_MAX");
    }

    atomic_store_explicit(&shards[0].live_tasks, 17U, memory_order_release);
    atomic_store_explicit(&shards[1].live_tasks, 23U, memory_order_release);
    if (llam_runtime_live_tasks(&runtime) != 40U) {
        return fail_msg("live task sum corrupted normal totals");
    }
#endif
    return 0;
}

static int exercise_task_live_counter_overflow_fails_closed(void) {
    llam_runtime_t runtime;
    llam_shard_t shard;

    memset(&runtime, 0, sizeof(runtime));
    memset(&shard, 0, sizeof(shard));
    runtime.shards = &shard;
    runtime.active_shards = 1U;
    atomic_init(&runtime.fatal_errno, 0);
    atomic_init(&shard.live_tasks, UINT_MAX);

    /*
     * A corrupted or saturated live counter must not wrap to zero when a new
     * task is marked live.  Zero would make runtime stop/shutdown believe the
     * runtime or shard is idle and can hide work from cancellation diagnostics.
     */
#if LLAM_RUNTIME_BACKEND_WINDOWS
    atomic_init(&runtime.live_tasks, UINT_MAX);
    llam_runtime_note_task_live(&runtime, &shard);
    if (atomic_load_explicit(&runtime.live_tasks, memory_order_acquire) != UINT_MAX ||
        atomic_load_explicit(&runtime.fatal_errno, memory_order_acquire) != EOVERFLOW) {
        return fail_msg("task live counter overflow was not rejected");
    }

    atomic_store_explicit(&runtime.fatal_errno, 0, memory_order_release);
    atomic_store_explicit(&runtime.live_tasks, 0U, memory_order_release);
    llam_runtime_note_task_live(&runtime, &shard);
    if (atomic_load_explicit(&runtime.live_tasks, memory_order_acquire) != 1U ||
        atomic_load_explicit(&runtime.fatal_errno, memory_order_acquire) != 0) {
        return fail_msg("task live counter normal increment failed");
    }
#else
    atomic_init(&runtime.live_task_shards, 0U);
    llam_runtime_note_task_live(&runtime, &shard);
    if (atomic_load_explicit(&shard.live_tasks, memory_order_acquire) != UINT_MAX ||
        atomic_load_explicit(&runtime.live_task_shards, memory_order_acquire) != 0U ||
        atomic_load_explicit(&runtime.fatal_errno, memory_order_acquire) != EOVERFLOW) {
        return fail_msg("task live counter overflow was not rejected");
    }

    atomic_store_explicit(&runtime.fatal_errno, 0, memory_order_release);
    atomic_store_explicit(&runtime.live_task_shards, 0U, memory_order_release);
    atomic_store_explicit(&shard.live_tasks, 0U, memory_order_release);
    llam_runtime_note_task_live(&runtime, &shard);
    if (atomic_load_explicit(&shard.live_tasks, memory_order_acquire) != 1U ||
        atomic_load_explicit(&runtime.live_task_shards, memory_order_acquire) != 1U ||
        atomic_load_explicit(&runtime.fatal_errno, memory_order_acquire) != 0) {
        return fail_msg("task live counter normal zero transition failed");
    }

    llam_runtime_note_task_live(&runtime, &shard);
    if (atomic_load_explicit(&shard.live_tasks, memory_order_acquire) != 2U ||
        atomic_load_explicit(&runtime.live_task_shards, memory_order_acquire) != 1U ||
        atomic_load_explicit(&runtime.fatal_errno, memory_order_acquire) != 0) {
        return fail_msg("task live counter normal increment failed");
    }
#endif
    return 0;
}

static int exercise_dynamic_scaler_live_saturation_fails_closed(void) {
    llam_runtime_t runtime;
    llam_shard_t shards[2];
    bool locks_initialized[2] = {false, false};
    bool opaque_locks_initialized[2] = {false, false};
    int rc = 1;

    memset(&runtime, 0, sizeof(runtime));
    memset(shards, 0, sizeof(shards));
    runtime.shards = shards;
    runtime.active_shards = 2U;
    runtime.active_nodes = 1U;
    runtime.experimental_dynamic_shards = 1U;
    runtime.dynamic_online_floor = 1U;
    runtime.dynamic_scale_cooldown = LLAM_DYNAMIC_SCALE_COOLDOWN_TICKS;
    /*
     * Windows keeps the live-task diagnostic total runtime-wide; POSIX sums
     * shard-local counters.  Set both so the overflow probe exercises the same
     * saturated scaler contract on every backend.
     */
    atomic_init(&runtime.live_tasks, UINT_MAX);
    atomic_init(&runtime.online_shards, 2U);
    atomic_init(&runtime.online_shards_min, 2U);
    atomic_init(&runtime.online_shards_max, 2U);
    atomic_init(&runtime.block_pending, 0U);
    atomic_init(&runtime.active_io_waiters, 0U);

    for (unsigned i = 0U; i < 2U; ++i) {
        shards[i].runtime = &runtime;
        shards[i].id = i;
        atomic_init(&shards[i].online, 1U);
        atomic_init(&shards[i].current, NULL);
        atomic_init(&shards[i].norm_depth, 0U);
        atomic_init(&shards[i].timer_callbacks_active, 0U);
        atomic_init(&shards[i].live_tasks, i == 0U ? UINT_MAX : 0U);
        if (pthread_mutex_init(&shards[i].lock, NULL) != 0) {
            goto done;
        }
        locks_initialized[i] = true;
        if (pthread_mutex_init(&shards[i].opaque_lock, NULL) != 0) {
            goto done;
        }
        opaque_locks_initialized[i] = true;
    }

    /*
     * Saturated live counts are fail-closed diagnostics.  The scaler must not
     * treat UINT_MAX + 1 as zero and begin a scale-down streak while liveness is
     * unknown; doing so can offline workers during a corrupted shutdown state.
     */
    llam_runtime_adjust_online_shards(&runtime);
    if (runtime.dynamic_scale_down_streak != 0U) {
        rc = fail_msg("dynamic scaler treated saturated live tasks as idle");
        goto done;
    }
    atomic_store_explicit(&shards[0].timer_callbacks_active, 1U, memory_order_release);
    llam_runtime_adjust_online_shards(&runtime);
    if (runtime.dynamic_scale_down_streak != 0U) {
        rc = fail_msg("dynamic scaler scaled down saturated live tasks with timers pending");
        goto done;
    }
    rc = 0;

done:
    for (unsigned i = 0U; i < 2U; ++i) {
        if (opaque_locks_initialized[i]) {
            pthread_mutex_destroy(&shards[i].opaque_lock);
        }
        if (locks_initialized[i]) {
            pthread_mutex_destroy(&shards[i].lock);
        }
    }
    return rc;
}

static int exercise_io_lifetime_invariants_are_lock_safe(void) {
    llam_runtime_t runtime;
    llam_node_t node;
    llam_io_req_t req;

    memset(&runtime, 0, sizeof(runtime));
    memset(&node, 0, sizeof(node));
    memset(&req, 0, sizeof(req));
    atomic_init(&runtime.fatal_errno, 0);
    node.runtime = &runtime;
    if (pthread_mutex_init(&node.watch_lock, NULL) != 0) {
        return fail_errno("lifetime invariant watch lock init failed");
    }
    req.owner_runtime = &runtime;
    atomic_init(&req.lifetime_refs, UINT_MAX - 1U);
    pthread_mutex_lock(&node.watch_lock);
    errno = 0;
    if (llam_io_req_lifetime_try_acquire(&req) || errno != EOVERFLOW) {
        pthread_mutex_unlock(&node.watch_lock);
        pthread_mutex_destroy(&node.watch_lock);
        return fail_msg("lifetime ref saturation was not rejected under owner lock");
    }
    pthread_mutex_unlock(&node.watch_lock);
    if (atomic_load_explicit(&runtime.fatal_errno,
                             memory_order_acquire) != EOVERFLOW ||
        atomic_load_explicit(&req.lifetime_refs,
                             memory_order_acquire) != UINT_MAX - 1U) {
        pthread_mutex_destroy(&node.watch_lock);
        return fail_msg("lifetime ref saturation did not latch safely");
    }
    pthread_mutex_destroy(&node.watch_lock);

#if !LLAM_RUNTIME_BACKEND_WINDOWS
    for (unsigned i = 0U; i < 2U; ++i) {
        pid_t pid = fork();
        int status = 0;

        if (pid < 0) {
            return fail_errno("lifetime invariant fork failed");
        }
        if (pid == 0) {
            llam_runtime_t child_runtime;
            llam_node_t child_node;
            llam_io_req_t child_req;

            (void)alarm(3U);
            memset(&child_runtime, 0, sizeof(child_runtime));
            memset(&child_node, 0, sizeof(child_node));
            memset(&child_req, 0, sizeof(child_req));
            child_req.owner_runtime = &child_runtime;
            atomic_init(&child_req.lifetime_refs,
                        i == 0U ? 0U : UINT_MAX);
            if (pthread_mutex_init(&child_node.watch_lock, NULL) != 0) {
                _exit(2);
            }
            pthread_mutex_lock(&child_node.watch_lock);
            (void)llam_io_req_lifetime_release(&child_req);
            _exit(3);
        }
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT) {
            return fail_msg("invalid lifetime release did not abort under owner lock");
        }
    }
#endif
    return 0;
}

#if defined(LLAM_ENABLE_TEST_HOOKS) && !LLAM_PLATFORM_WINDOWS
typedef struct submit_rehome_fixture {
    llam_runtime_t runtime;
    llam_shard_t shards[2];
    llam_node_t nodes[2];
    llam_task_t task;
    llam_task_t caller;
    llam_io_req_t req;
} submit_rehome_fixture_t;

typedef struct public_cancel_call {
    llam_cancel_token_t *token;
    llam_task_t *caller;
    llam_shard_t *caller_shard;
    int rc;
} public_cancel_call_t;

typedef struct setup_abort_call {
    llam_io_req_t *req;
    bool wait_for_completion;
    bool rc;
} setup_abort_call_t;

typedef struct submit_evacuation_call {
    submit_rehome_fixture_t *fixture;
    unsigned migrated;
    bool rc;
} submit_evacuation_call_t;

typedef struct inflight_owner_transfer_call {
    llam_io_req_t *req;
    unsigned from_shard;
    unsigned to_shard;
    bool result;
} inflight_owner_transfer_call_t;

typedef struct inflight_reuse_fixture {
    llam_runtime_t runtime;
    llam_shard_t shards[4];
    llam_node_t node;
    llam_task_t task;
} inflight_reuse_fixture_t;

typedef struct inflight_rehome_call {
    inflight_reuse_fixture_t *fixture;
    unsigned source_id;
    unsigned target_id;
    unsigned migrated;
    atomic_uint done;
} inflight_rehome_call_t;

typedef struct inflight_completion_call {
    llam_node_t *node;
    llam_io_req_t *req;
    atomic_uint done;
} inflight_completion_call_t;

static pthread_mutex_t g_submit_detach_hook_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_submit_detach_hook_cv = PTHREAD_COND_INITIALIZER;
static llam_io_req_t *g_submit_detach_hook_req;
static bool g_submit_detach_hook_reached;
static bool g_submit_detach_hook_release;
static pthread_mutex_t g_submit_evacuation_hook_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_submit_evacuation_hook_cv = PTHREAD_COND_INITIALIZER;
static bool g_submit_evacuation_hook_reached;
static bool g_submit_evacuation_hook_release;
static pthread_mutex_t g_inflight_owner_hook_lock =
    PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_inflight_owner_hook_cv =
    PTHREAD_COND_INITIALIZER;
static llam_io_req_t *g_inflight_owner_hook_req;
static bool g_inflight_owner_hook_reached;
static bool g_inflight_owner_hook_release;

static void submit_detach_snapshot_hook(llam_io_req_t *req,
                                        unsigned node_index) {
    pthread_mutex_lock(&g_submit_detach_hook_lock);
    if (req == g_submit_detach_hook_req && node_index == 0U) {
        g_submit_detach_hook_reached = true;
        pthread_cond_broadcast(&g_submit_detach_hook_cv);
        while (!g_submit_detach_hook_release) {
            pthread_cond_wait(&g_submit_detach_hook_cv,
                              &g_submit_detach_hook_lock);
        }
    }
    pthread_mutex_unlock(&g_submit_detach_hook_lock);
}

static void arm_submit_detach_hook(llam_io_req_t *req) {
    pthread_mutex_lock(&g_submit_detach_hook_lock);
    g_submit_detach_hook_req = req;
    g_submit_detach_hook_reached = false;
    g_submit_detach_hook_release = false;
    pthread_mutex_unlock(&g_submit_detach_hook_lock);
    llam_io_test_set_submit_detach_snapshot_hook(submit_detach_snapshot_hook);
}

static void wait_submit_detach_hook(void) {
    pthread_mutex_lock(&g_submit_detach_hook_lock);
    while (!g_submit_detach_hook_reached) {
        pthread_cond_wait(&g_submit_detach_hook_cv,
                          &g_submit_detach_hook_lock);
    }
    pthread_mutex_unlock(&g_submit_detach_hook_lock);
}

static void release_submit_detach_hook(void) {
    pthread_mutex_lock(&g_submit_detach_hook_lock);
    g_submit_detach_hook_release = true;
    pthread_cond_broadcast(&g_submit_detach_hook_cv);
    pthread_mutex_unlock(&g_submit_detach_hook_lock);
}

static void clear_submit_detach_hook(void) {
    llam_io_test_set_submit_detach_snapshot_hook(NULL);
    pthread_mutex_lock(&g_submit_detach_hook_lock);
    g_submit_detach_hook_req = NULL;
    g_submit_detach_hook_reached = false;
    g_submit_detach_hook_release = false;
    pthread_mutex_unlock(&g_submit_detach_hook_lock);
}

static void submit_evacuation_unlocked_hook(void) {
    pthread_mutex_lock(&g_submit_evacuation_hook_lock);
    g_submit_evacuation_hook_reached = true;
    pthread_cond_broadcast(&g_submit_evacuation_hook_cv);
    while (!g_submit_evacuation_hook_release) {
        pthread_cond_wait(&g_submit_evacuation_hook_cv,
                          &g_submit_evacuation_hook_lock);
    }
    pthread_mutex_unlock(&g_submit_evacuation_hook_lock);
}

static void arm_submit_evacuation_hook(void) {
    pthread_mutex_lock(&g_submit_evacuation_hook_lock);
    g_submit_evacuation_hook_reached = false;
    g_submit_evacuation_hook_release = false;
    pthread_mutex_unlock(&g_submit_evacuation_hook_lock);
    llam_io_test_set_submit_evacuation_unlocked_hook(
        submit_evacuation_unlocked_hook);
}

static void wait_submit_evacuation_hook(void) {
    pthread_mutex_lock(&g_submit_evacuation_hook_lock);
    while (!g_submit_evacuation_hook_reached) {
        pthread_cond_wait(&g_submit_evacuation_hook_cv,
                          &g_submit_evacuation_hook_lock);
    }
    pthread_mutex_unlock(&g_submit_evacuation_hook_lock);
}

static void release_submit_evacuation_hook(void) {
    pthread_mutex_lock(&g_submit_evacuation_hook_lock);
    g_submit_evacuation_hook_release = true;
    pthread_cond_broadcast(&g_submit_evacuation_hook_cv);
    pthread_mutex_unlock(&g_submit_evacuation_hook_lock);
}

static void clear_submit_evacuation_hook(void) {
    llam_io_test_set_submit_evacuation_unlocked_hook(NULL);
    pthread_mutex_lock(&g_submit_evacuation_hook_lock);
    g_submit_evacuation_hook_reached = false;
    g_submit_evacuation_hook_release = false;
    pthread_mutex_unlock(&g_submit_evacuation_hook_lock);
}

static void inflight_owner_published_hook(
    llam_io_req_t *req,
    unsigned from_shard,
    unsigned to_shard) {
    (void)from_shard;
    (void)to_shard;
    pthread_mutex_lock(&g_inflight_owner_hook_lock);
    if (req == g_inflight_owner_hook_req) {
        g_inflight_owner_hook_reached = true;
        pthread_cond_broadcast(&g_inflight_owner_hook_cv);
        while (!g_inflight_owner_hook_release) {
            pthread_cond_wait(
                &g_inflight_owner_hook_cv,
                &g_inflight_owner_hook_lock);
        }
    }
    pthread_mutex_unlock(&g_inflight_owner_hook_lock);
}

static void arm_inflight_owner_hook(llam_io_req_t *req) {
    pthread_mutex_lock(&g_inflight_owner_hook_lock);
    g_inflight_owner_hook_req = req;
    g_inflight_owner_hook_reached = false;
    g_inflight_owner_hook_release = false;
    pthread_mutex_unlock(&g_inflight_owner_hook_lock);
    llam_io_test_set_inflight_owner_published_hook(
        inflight_owner_published_hook);
}

static void wait_inflight_owner_hook(void) {
    pthread_mutex_lock(&g_inflight_owner_hook_lock);
    while (!g_inflight_owner_hook_reached) {
        pthread_cond_wait(
            &g_inflight_owner_hook_cv,
            &g_inflight_owner_hook_lock);
    }
    pthread_mutex_unlock(&g_inflight_owner_hook_lock);
}

static void release_inflight_owner_hook(void) {
    pthread_mutex_lock(&g_inflight_owner_hook_lock);
    g_inflight_owner_hook_release = true;
    pthread_cond_broadcast(&g_inflight_owner_hook_cv);
    pthread_mutex_unlock(&g_inflight_owner_hook_lock);
}

static void clear_inflight_owner_hook(void) {
    llam_io_test_set_inflight_owner_published_hook(NULL);
    pthread_mutex_lock(&g_inflight_owner_hook_lock);
    g_inflight_owner_hook_req = NULL;
    g_inflight_owner_hook_reached = false;
    g_inflight_owner_hook_release = false;
    pthread_mutex_unlock(&g_inflight_owner_hook_lock);
}

static bool wait_inflight_owner_hook_bounded(uint64_t timeout_ns) {
    uint64_t deadline = llam_now_ns() + timeout_ns;
    bool reached;

    pthread_mutex_lock(&g_inflight_owner_hook_lock);
    while (!g_inflight_owner_hook_reached &&
           llam_now_ns() < deadline) {
        struct timespec interval = {.tv_sec = 0, .tv_nsec = 1000000L};

        pthread_mutex_unlock(&g_inflight_owner_hook_lock);
        (void)nanosleep(&interval, NULL);
        pthread_mutex_lock(&g_inflight_owner_hook_lock);
    }
    reached = g_inflight_owner_hook_reached;
    pthread_mutex_unlock(&g_inflight_owner_hook_lock);
    return reached;
}

static bool wait_atomic_uint_mask_bounded(atomic_uint *value,
                                          unsigned mask,
                                          unsigned expected,
                                          uint64_t timeout_ns) {
    uint64_t deadline = llam_now_ns() + timeout_ns;

    while ((atomic_load_explicit(value, memory_order_acquire) & mask) !=
           expected) {
        if (llam_now_ns() >= deadline) {
            return false;
        }
        sched_yield();
    }
    return true;
}

static bool wait_completion_or_resolver_close_bounded(
    atomic_uint *completion_done,
    atomic_uint *resolver_state,
    uint64_t timeout_ns) {
    uint64_t deadline = llam_now_ns() + timeout_ns;
    unsigned state;

    for (;;) {
        if (atomic_load_explicit(completion_done,
                                 memory_order_acquire) != 0U) {
            return true;
        }
        state = atomic_load_explicit(resolver_state, memory_order_acquire);
        if ((state & LLAM_WAIT_RESOLVER_CLOSED_BIT) != 0U &&
            (state & LLAM_WAIT_RESOLVER_REF_MASK) != 0U) {
            return true;
        }
        if (llam_now_ns() >= deadline) {
            return false;
        }
        sched_yield();
    }
}

static int init_submit_rehome_fixture(submit_rehome_fixture_t *fixture,
                                      bool published) {
    llam_runtime_t *rt;
    llam_task_t *task;
    llam_io_req_t *req;

    if (fixture == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(fixture, 0, sizeof(*fixture));
    rt = &fixture->runtime;
    task = &fixture->task;
    req = &fixture->req;
    rt->shards = fixture->shards;
    rt->nodes = fixture->nodes;
    rt->active_shards = 2U;
    rt->active_nodes = 2U;
    atomic_init(&rt->initialized, false);
    atomic_init(&rt->fatal_errno, 0);
    atomic_init(&rt->deferred_fatal_pending, 0U);
    atomic_init(&rt->overflow_depth, 0U);
    atomic_init(&rt->active_io_waiters, 0U);

    for (unsigned i = 0U; i < 2U; ++i) {
        llam_shard_t *shard = &fixture->shards[i];
        llam_node_t *node = &fixture->nodes[i];

        shard->runtime = rt;
        shard->id = i;
        shard->io_node_index = i;
        shard->event_fd = LLAM_INVALID_FD;
        atomic_init(&shard->online, 1U);
        atomic_init(&shard->current, NULL);
        atomic_init(&shard->inflight_io_waiters, 0U);
        atomic_init(&shard->merge_pause_requested, 0U);
        atomic_init(&shard->merge_pause_ack, 0U);
        atomic_init(&shard->inject_depth, 0U);
        atomic_init(&shard->timer_count, 0U);
        atomic_init(&shard->timer_callbacks_active, 0U);
        if (pthread_mutex_init(&shard->lock, NULL) != 0) {
            return -1;
        }

        node->runtime = rt;
        node->index = i;
        node->event_fd = LLAM_INVALID_FD;
        node->ring_ready = true;
        node->supports_read = true;
        atomic_init(&node->pending_ops, 0U);
        if (pthread_mutex_init(&node->submit_lock, NULL) != 0) {
            return -1;
        }
    }

    task->owner_runtime = rt;
    task->home_shard = 0U;
    task->alloc_owner_shard = UINT_MAX;
    atomic_init(&task->state, LLAM_TASK_STATE_PARKED);
    atomic_init(&task->wait_reason, LLAM_WAIT_IO);
    atomic_init(&task->last_shard, 0U);
    atomic_init(&task->parked_shard, 0U);
    atomic_init(&task->task_class, LLAM_TASK_CLASS_DEFAULT);
    atomic_init(&task->wake_error_code, 0);
    atomic_init(&task->wait_resolver_state, 0U);
    atomic_init(&task->wait_generation, 1U);
    atomic_init(&task->scan_refs, 0U);
    atomic_init(&task->active_wait_node, NULL);
    atomic_init(&task->active_wait_queue, NULL);
    atomic_init(&task->active_wait_queue_lock, NULL);
    atomic_init(&task->active_select_state, NULL);
    atomic_init(&task->active_wait_lifetime_ops, NULL);
    atomic_init(&task->active_block_job, NULL);
    atomic_init(&task->join_target, NULL);
    task->active_timer = NULL;
    atomic_init(&task->active_io_generation, 1U);
    atomic_init(&task->active_io_req, req);

    req->owner_runtime = rt;
    req->kind = LLAM_IO_KIND_READ;
    req->fd = LLAM_INVALID_FD;
    req->handle = LLAM_INVALID_HANDLE;
    req->task = task;
    req->alloc_owner_shard = UINT_MAX;
    atomic_init(&req->owner_shard, 0U);
    atomic_init(&req->attached_node_index, 0U);
    atomic_init(&req->inflight_owner_shard, UINT_MAX);
    atomic_init(&req->wait_mode, LLAM_IO_WAIT_MODE_SUBMIT_QUEUE);
    atomic_init(&req->abort_reason, LLAM_IO_ABORT_NONE);
    atomic_init(&req->operation_generation, 1U);
    atomic_init(&req->lifetime_refs, 1U);
    atomic_init(&req->cancel_queued, 0U);
    if (published) {
        fixture->nodes[0].submit_head = req;
        fixture->nodes[0].submit_tail = req;
        atomic_store_explicit(&fixture->nodes[0].pending_ops,
                              1U,
                              memory_order_release);
    }

    fixture->caller.owner_runtime = rt;
    atomic_init(&fixture->caller.state, LLAM_TASK_STATE_RUNNING);
    return 0;
}

static void destroy_submit_rehome_fixture(submit_rehome_fixture_t *fixture) {
    if (fixture == NULL) {
        return;
    }
    for (unsigned i = 0U; i < 2U; ++i) {
        pthread_mutex_destroy(&fixture->nodes[i].submit_lock);
        pthread_mutex_destroy(&fixture->shards[i].lock);
    }
}

static int init_inflight_reuse_fixture(inflight_reuse_fixture_t *fixture,
                                       unsigned allocation_owner,
                                       unsigned source_id) {
    llam_runtime_t *rt;
    llam_task_t *task;
    llam_io_req_t *req;
    uint64_t operation_generation;

    if (fixture == NULL || allocation_owner >= 4U ||
        source_id >= 4U) {
        errno = EINVAL;
        return -1;
    }
    memset(fixture, 0, sizeof(*fixture));
    rt = &fixture->runtime;
    task = &fixture->task;
    req = &task->embedded_io_req;

    rt->shards = fixture->shards;
    rt->nodes = &fixture->node;
    rt->active_shards = 4U;
    rt->active_nodes = 1U;
    atomic_init(&rt->initialized, false);
    atomic_init(&rt->online_shards, 4U);
    atomic_init(&rt->fatal_errno, 0);
    atomic_init(&rt->deferred_fatal_pending, 0U);
    atomic_init(&rt->overflow_depth, 0U);
    atomic_init(&rt->active_io_waiters, 0U);

    for (unsigned i = 0U; i < 4U; ++i) {
        llam_shard_t *shard = &fixture->shards[i];

        shard->runtime = rt;
        shard->id = i;
        shard->io_node_index = 0U;
        shard->event_fd = LLAM_INVALID_FD;
        atomic_init(&shard->online, 1U);
        atomic_init(&shard->current, NULL);
        atomic_init(&shard->inflight_io_waiters, 0U);
        atomic_init(&shard->merge_pause_requested, 0U);
        atomic_init(&shard->merge_pause_ack, 0U);
        atomic_init(&shard->inject_depth, 0U);
        atomic_init(&shard->timer_count, 0U);
        atomic_init(&shard->timer_callbacks_active, 0U);
        if (pthread_mutex_init(&shard->lock, NULL) != 0) {
            return -1;
        }
    }

    fixture->node.runtime = rt;
    fixture->node.index = 0U;
    fixture->node.event_fd = LLAM_INVALID_FD;
    atomic_init(&fixture->node.pending_ops, 0U);
    if (pthread_mutex_init(&fixture->node.watch_lock, NULL) != 0) {
        return -1;
    }

    task->owner_runtime = rt;
    task->home_shard = source_id;
    task->live_shard = source_id;
    task->alloc_owner_shard = allocation_owner;
    atomic_init(&task->state, LLAM_TASK_STATE_PARKED);
    atomic_init(&task->wait_reason, LLAM_WAIT_IO);
    atomic_init(&task->last_shard, source_id);
    atomic_init(&task->parked_shard, source_id);
    atomic_init(&task->task_class, LLAM_TASK_CLASS_DEFAULT);
    atomic_init(&task->base_task_class, LLAM_TASK_CLASS_DEFAULT);
    atomic_init(&task->wake_error_code, 0);
    atomic_init(&task->wait_resolver_state, 0U);
    atomic_init(&task->wait_generation, 1U);
    atomic_init(&task->scan_refs, 0U);
    atomic_init(&task->active_wait_node, NULL);
    atomic_init(&task->active_wait_queue, NULL);
    atomic_init(&task->active_wait_queue_lock, NULL);
    atomic_init(&task->active_select_state, NULL);
    atomic_init(&task->active_wait_lifetime_ops, NULL);
    atomic_init(&task->active_block_job, NULL);
    atomic_init(&task->join_target, NULL);
    atomic_init(&task->active_io_req, NULL);
    atomic_init(&task->active_io_generation, 0U);
    task->active_timer = NULL;

    llam_io_req_reset(req, rt, source_id, UINT_MAX);
    if (!llam_io_req_lifetime_activate(req)) {
        return -1;
    }
    operation_generation = atomic_load_explicit(
        &req->operation_generation, memory_order_acquire);
    req->task = task;
    atomic_store_explicit(&req->owner_shard,
                          source_id,
                          memory_order_release);
    atomic_store_explicit(&req->inflight_owner_shard,
                          source_id,
                          memory_order_release);
    atomic_store_explicit(&req->wait_mode,
                          LLAM_IO_WAIT_MODE_INFLIGHT,
                          memory_order_release);
    atomic_store_explicit(&task->active_io_generation,
                          operation_generation,
                          memory_order_release);
    atomic_store_explicit(&task->active_io_req,
                          req,
                          memory_order_release);
    fixture->shards[allocation_owner].all_tasks = task;
    atomic_store_explicit(
        &fixture->shards[source_id].inflight_io_waiters,
        1U,
        memory_order_release);
    return 0;
}

static void destroy_inflight_reuse_fixture(
    inflight_reuse_fixture_t *fixture) {
    if (fixture == NULL) {
        return;
    }
    pthread_mutex_destroy(&fixture->node.watch_lock);
    for (unsigned i = 0U; i < 4U; ++i) {
        pthread_mutex_destroy(&fixture->shards[i].lock);
    }
}

static void *inflight_rehome_thread_main(void *opaque) {
    inflight_rehome_call_t *call = opaque;

    llam_rehome_inflight_io_waiters(
        &call->fixture->runtime,
        &call->fixture->shards[call->source_id],
        &call->fixture->shards[call->target_id],
        &call->migrated);
    atomic_store_explicit(&call->done, 1U, memory_order_release);
    return NULL;
}

static void *inflight_completion_thread_main(void *opaque) {
    inflight_completion_call_t *call = opaque;

#if LLAM_RUNTIME_BACKEND_KQUEUE
    llam_io_complete_req(call->node, call->req, 0, false);
#elif LLAM_RUNTIME_BACKEND_LINUX
    llam_io_complete_req(call->node, call->req, 0, 0U, false);
#else
#error "FR08-002 regression requires the kqueue or Linux backend"
#endif
    atomic_store_explicit(&call->done, 1U, memory_order_release);
    return NULL;
}

static void clear_inflight_reuse_inject_queue(
    inflight_reuse_fixture_t *fixture,
    unsigned shard_id) {
    llam_shard_t *shard = &fixture->shards[shard_id];

    pthread_mutex_lock(&shard->lock);
    while (llam_queue_pop_head(&shard->inject_q) != NULL) {
    }
    atomic_store_explicit(&shard->inject_depth,
                          0U,
                          memory_order_release);
    pthread_mutex_unlock(&shard->lock);
    fixture->task.queue_next = NULL;
    fixture->task.queue_prev = NULL;
}

static bool reuse_inflight_generation(inflight_reuse_fixture_t *fixture,
                                      unsigned source_id,
                                      unsigned completed_on,
                                      unsigned reuse_id,
                                      uint64_t old_generation,
                                      uint64_t *new_generation_out) {
    llam_task_t *task = &fixture->task;
    llam_io_req_t *old_req = &task->embedded_io_req;
    llam_io_req_t *new_req;
    uint64_t new_generation;

    if (llam_task_active_io_req_load(task) != NULL ||
        atomic_load_explicit(&task->state,
                             memory_order_acquire) !=
            LLAM_TASK_STATE_RUNNABLE ||
        fixture->shards[completed_on].inject_q.head != task) {
        return false;
    }

    clear_inflight_reuse_inject_queue(fixture, completed_on);
    atomic_store_explicit(&task->state,
                          LLAM_TASK_STATE_RUNNING,
                          memory_order_release);
    atomic_store_explicit(&task->last_shard,
                          reuse_id,
                          memory_order_release);
    /*
     * A stolen task may retain its prior home while beginning the fresh wait.
     * The old rehome must not rewrite this generation's placement metadata.
     */
    task->home_shard = source_id;
    g_llam_tls_task = task;
    g_llam_tls_shard = &fixture->shards[reuse_id];
    llam_api_io_req_release(g_llam_tls_shard, old_req);
    new_req = llam_api_io_req_acquire(g_llam_tls_shard);
    if (new_req != old_req) {
        g_llam_tls_task = NULL;
        g_llam_tls_shard = NULL;
        return false;
    }
    new_generation = atomic_load_explicit(
        &new_req->operation_generation, memory_order_acquire);
    if (new_generation == 0U || new_generation == old_generation ||
        !llam_task_set_io_tracking(task, new_req, reuse_id)) {
        g_llam_tls_task = NULL;
        g_llam_tls_shard = NULL;
        return false;
    }
    atomic_store_explicit(&new_req->wait_mode,
                          LLAM_IO_WAIT_MODE_INFLIGHT,
                          memory_order_release);
    atomic_store_explicit(&new_req->inflight_owner_shard,
                          reuse_id,
                          memory_order_release);
    atomic_store_explicit(
        &fixture->shards[reuse_id].inflight_io_waiters,
        1U,
        memory_order_release);
    g_llam_tls_task = NULL;
    g_llam_tls_shard = NULL;
    *new_generation_out = new_generation;
    return true;
}

static bool migrate_submit_rehome_fixture(submit_rehome_fixture_t *fixture,
                                          unsigned *rehomed_out,
                                          unsigned *evacuated_out) {
    bool rehomed;
    bool evacuated;

    rehomed = llam_rehome_node_submit_waiters(&fixture->nodes[0],
                                              &fixture->shards[0],
                                              &fixture->shards[1],
                                              rehomed_out);
    evacuated = llam_evacuate_rehomed_submit_waiters(&fixture->nodes[0],
                                                     &fixture->nodes[1],
                                                     &fixture->shards[0],
                                                     &fixture->shards[1],
                                                     evacuated_out);
    return rehomed && evacuated;
}

static void *public_cancel_thread_main(void *opaque) {
    public_cancel_call_t *call = opaque;

    g_llam_tls_task = call->caller;
    g_llam_tls_shard = call->caller_shard;
    call->rc = llam_cancel_token_cancel(call->token);
    g_llam_tls_task = NULL;
    g_llam_tls_shard = NULL;
    return NULL;
}

static void *setup_abort_thread_main(void *opaque) {
    setup_abort_call_t *call = opaque;

    call->rc = llam_io_test_abort_published_io_setup(
        call->req,
        LLAM_IO_ABORT_CANCEL,
        &call->wait_for_completion);
    return NULL;
}

static void *submit_evacuation_thread_main(void *opaque) {
    submit_evacuation_call_t *call = opaque;
    submit_rehome_fixture_t *fixture = call->fixture;

    call->rc = llam_evacuate_rehomed_submit_waiters(
        &fixture->nodes[0],
        &fixture->nodes[1],
        &fixture->shards[0],
        &fixture->shards[1],
        &call->migrated);
    return NULL;
}

static void *inflight_owner_transfer_thread_main(void *opaque) {
    inflight_owner_transfer_call_t *call = opaque;

    call->result = llam_io_req_transfer_inflight_owner(
        call->req,
        call->from_shard,
        call->to_shard);
    return NULL;
}

static void clear_fixture_task_queues(
    submit_rehome_fixture_t *fixture) {
    for (unsigned i = 0U; i < 2U; ++i) {
        fixture->shards[i].inject_q.head = NULL;
        fixture->shards[i].inject_q.tail = NULL;
        fixture->shards[i].inject_q.depth = 0U;
        fixture->shards[i].hot_q.head = NULL;
        fixture->shards[i].hot_q.tail = NULL;
        fixture->shards[i].hot_q.depth = 0U;
        fixture->shards[i].norm_q.head = NULL;
        fixture->shards[i].norm_q.tail = NULL;
        fixture->shards[i].norm_q.depth = 0U;
        atomic_store_explicit(
            &fixture->shards[i].inject_depth,
            0U,
            memory_order_release);
    }
    fixture->task.queue_next = NULL;
    fixture->task.queue_prev = NULL;
}

static int exercise_inflight_owner_credit_precedes_publication(void) {
    submit_rehome_fixture_t fixture;
    inflight_owner_transfer_call_t call;
    pthread_t thread;
    unsigned completion_owner;
    bool published_with_credit;
    int rc = 1;

    if (init_submit_rehome_fixture(&fixture, false) != 0) {
        return fail_errno(
            "inflight owner transaction fixture init failed");
    }
    atomic_store_explicit(
        &fixture.req.inflight_owner_shard,
        0U,
        memory_order_release);
    atomic_store_explicit(
        &fixture.shards[0].inflight_io_waiters,
        1U,
        memory_order_release);
    atomic_store_explicit(
        &fixture.shards[1].inflight_io_waiters,
        0U,
        memory_order_release);
    call.req = &fixture.req;
    call.from_shard = 0U;
    call.to_shard = 1U;
    call.result = false;

    arm_inflight_owner_hook(&fixture.req);
    if (pthread_create(
            &thread,
            NULL,
            inflight_owner_transfer_thread_main,
            &call) != 0) {
        clear_inflight_owner_hook();
        destroy_submit_rehome_fixture(&fixture);
        return fail_errno(
            "inflight owner transaction thread create failed");
    }
    wait_inflight_owner_hook();
    published_with_credit =
        atomic_load_explicit(
            &fixture.req.inflight_owner_shard,
            memory_order_acquire) == 1U &&
        atomic_load_explicit(
            &fixture.shards[0].inflight_io_waiters,
            memory_order_acquire) == 1U &&
        atomic_load_explicit(
            &fixture.shards[1].inflight_io_waiters,
            memory_order_acquire) == 1U;
    completion_owner = atomic_exchange_explicit(
        &fixture.req.inflight_owner_shard,
        UINT_MAX,
        memory_order_acq_rel);
    if (completion_owner < fixture.runtime.active_shards) {
        llam_shard_note_inflight_io_waiter(
            &fixture.runtime,
            completion_owner,
            -1);
    }
    release_inflight_owner_hook();
    pthread_join(thread, NULL);
    clear_inflight_owner_hook();

    if (!published_with_credit ||
        !call.result ||
        completion_owner != 1U ||
        atomic_load_explicit(
            &fixture.shards[0].inflight_io_waiters,
            memory_order_acquire) != 0U ||
        atomic_load_explicit(
            &fixture.shards[1].inflight_io_waiters,
            memory_order_acquire) != 0U ||
        atomic_load_explicit(
            &fixture.runtime.fatal_errno,
            memory_order_acquire) != 0 ||
        atomic_load_explicit(
            &fixture.runtime.deferred_fatal_pending,
            memory_order_acquire) != 0U) {
        fprintf(
            stderr,
            "inflight owner publication was uncredited: "
            "credit=%u moved=%u owner=%u source=%u target=%u fatal=%d "
            "deferred=%u\n",
            published_with_credit ? 1U : 0U,
            call.result ? 1U : 0U,
            completion_owner,
            atomic_load_explicit(
                &fixture.shards[0].inflight_io_waiters,
                memory_order_acquire),
            atomic_load_explicit(
                &fixture.shards[1].inflight_io_waiters,
                memory_order_acquire),
            atomic_load_explicit(
                &fixture.runtime.fatal_errno,
                memory_order_acquire),
            atomic_load_explicit(
                &fixture.runtime.deferred_fatal_pending,
                memory_order_acquire));
        goto done;
    }

    /*
     * If completion consumes the source before the CAS, the provisional
     * target unit was never authoritative and must roll back exactly once.
     */
    atomic_store_explicit(
        &fixture.req.inflight_owner_shard,
        UINT_MAX,
        memory_order_release);
    atomic_store_explicit(
        &fixture.shards[0].inflight_io_waiters,
        0U,
        memory_order_release);
    atomic_store_explicit(
        &fixture.shards[1].inflight_io_waiters,
        7U,
        memory_order_release);
    if (llam_io_req_transfer_inflight_owner(
            &fixture.req, 0U, 1U) ||
        atomic_load_explicit(
            &fixture.req.inflight_owner_shard,
            memory_order_acquire) != UINT_MAX ||
        atomic_load_explicit(
            &fixture.shards[0].inflight_io_waiters,
            memory_order_acquire) != 0U ||
        atomic_load_explicit(
            &fixture.shards[1].inflight_io_waiters,
            memory_order_acquire) != 7U ||
        atomic_load_explicit(
            &fixture.runtime.fatal_errno,
            memory_order_acquire) != 0) {
        goto done;
    }

    /*
     * Saturation must fail before the target owner is visible. Publishing
     * first would leave a request owned by a counter that could not be
     * credited.
     */
    atomic_store_explicit(
        &fixture.req.inflight_owner_shard,
        0U,
        memory_order_release);
    atomic_store_explicit(
        &fixture.shards[0].inflight_io_waiters,
        1U,
        memory_order_release);
    atomic_store_explicit(
        &fixture.shards[1].inflight_io_waiters,
        UINT_MAX,
        memory_order_release);
    atomic_store_explicit(
        &fixture.runtime.fatal_errno,
        0,
        memory_order_release);
    atomic_store_explicit(
        &fixture.runtime.deferred_fatal_pending,
        0U,
        memory_order_release);
    if (llam_io_req_transfer_inflight_owner(
            &fixture.req, 0U, 1U) ||
        atomic_load_explicit(
            &fixture.req.inflight_owner_shard,
            memory_order_acquire) != 0U ||
        atomic_load_explicit(
            &fixture.shards[0].inflight_io_waiters,
            memory_order_acquire) != 1U ||
        atomic_load_explicit(
            &fixture.shards[1].inflight_io_waiters,
            memory_order_acquire) != UINT_MAX ||
        atomic_load_explicit(
            &fixture.runtime.fatal_errno,
            memory_order_acquire) != EOVERFLOW) {
        goto done;
    }
    rc = 0;

done:
    destroy_submit_rehome_fixture(&fixture);
    return rc;
}

/*
 * LLAM-DIFF-FR08-002: a rehome for generation N must pin that wait until all
 * ownership stores finish.  The task allocation owner is deliberately
 * distinct from its parked shard, matching a supported stolen-task state.
 */
static int exercise_inflight_rehome_is_generation_bound(void) {
    enum {
        allocation_owner = 0U,
        source_id = 1U,
        target_id = 2U,
        reuse_id = 3U
    };
    const uint64_t timeout_ns = 2000000000ULL;
    inflight_reuse_fixture_t fixture;
    inflight_rehome_call_t rehome_call;
    inflight_completion_call_t completion_call;
    llam_task_t *task;
    llam_io_req_t *req;
    pthread_t rehome_thread;
    pthread_t completion_thread;
    uint64_t old_generation;
    uint64_t old_wait_generation;
    uint64_t new_generation = 0U;
    bool completion_finished_while_paused;
    bool rehome_started = false;
    bool rehome_joined = false;
    bool completion_started = false;
    bool completion_joined = false;
    bool hook_armed = false;
    bool fresh_started = false;
    int rc = 1;

    if (init_inflight_reuse_fixture(
            &fixture, allocation_owner, source_id) != 0) {
        return fail_errno("FR08-002 fixture init failed");
    }
    task = &fixture.task;
    req = &task->embedded_io_req;
    old_generation = atomic_load_explicit(
        &req->operation_generation, memory_order_acquire);
    old_wait_generation = atomic_load_explicit(
        &task->wait_generation, memory_order_acquire);

    memset(&rehome_call, 0, sizeof(rehome_call));
    rehome_call.fixture = &fixture;
    rehome_call.source_id = source_id;
    rehome_call.target_id = target_id;
    atomic_init(&rehome_call.done, 0U);
    memset(&completion_call, 0, sizeof(completion_call));
    completion_call.node = &fixture.node;
    completion_call.req = req;
    atomic_init(&completion_call.done, 0U);

    arm_inflight_owner_hook(req);
    hook_armed = true;
    if (pthread_create(&rehome_thread,
                       NULL,
                       inflight_rehome_thread_main,
                       &rehome_call) != 0) {
        (void)fail_errno("FR08-002 rehome thread create failed");
        goto done;
    }
    rehome_started = true;
    if (!wait_inflight_owner_hook_bounded(timeout_ns)) {
        (void)fail_msg("FR08-002 owner publication hook timed out");
        goto done;
    }
    if (atomic_load_explicit(&req->inflight_owner_shard,
                             memory_order_acquire) != target_id ||
        atomic_load_explicit(&task->parked_shard,
                             memory_order_acquire) != source_id ||
        atomic_load_explicit(&req->owner_shard,
                             memory_order_acquire) != source_id ||
        task->home_shard != source_id) {
        (void)fail_msg(
            "FR08-002 fixture missed the post-publication metadata window");
        goto done;
    }

    if (pthread_create(&completion_thread,
                       NULL,
                       inflight_completion_thread_main,
                       &completion_call) != 0) {
        (void)fail_errno("FR08-002 completion thread create failed");
        goto done;
    }
    completion_started = true;
    if (!wait_atomic_uint_mask_bounded(
            &req->wait_mode,
            UINT_MAX,
            LLAM_IO_WAIT_MODE_NONE,
            timeout_ns) ||
        atomic_load_explicit(&req->inflight_owner_shard,
                             memory_order_acquire) != UINT_MAX ||
        atomic_load_explicit(
            &fixture.shards[target_id].inflight_io_waiters,
            memory_order_acquire) != 0U) {
        (void)fail_msg(
            "FR08-002 completion did not consume the transferred owner");
        goto done;
    }

    if (!wait_completion_or_resolver_close_bounded(
            &completion_call.done,
            &task->wait_resolver_state,
            timeout_ns)) {
        (void)fail_msg(
            "FR08-002 completion reached neither reuse nor resolver drain");
        goto done;
    }
    completion_finished_while_paused =
        atomic_load_explicit(&completion_call.done,
                             memory_order_acquire) != 0U;
    if (completion_finished_while_paused) {
        if (pthread_join(completion_thread, NULL) != 0) {
            (void)fail_msg("FR08-002 early completion join failed");
            goto done;
        }
        completion_joined = true;
        /*
         * Vulnerable code reaches this branch: completion clears generation N,
         * so publish N+1 at the same embedded address before old rehome resumes.
         */
        if (!reuse_inflight_generation(&fixture,
                                       source_id,
                                       target_id,
                                       reuse_id,
                                       old_generation,
                                       &new_generation)) {
            (void)fail_msg(
                "FR08-002 could not reuse the prematurely cleared request");
            goto done;
        }
        fresh_started = true;
    } else if (!wait_atomic_uint_mask_bounded(
                   &task->wait_resolver_state,
                   LLAM_WAIT_RESOLVER_CLOSED_BIT,
                   LLAM_WAIT_RESOLVER_CLOSED_BIT,
                   timeout_ns) ||
               (atomic_load_explicit(&task->wait_resolver_state,
                                     memory_order_acquire) &
                LLAM_WAIT_RESOLVER_REF_MASK) == 0U ||
               llam_task_active_io_req_load(task) != req ||
               atomic_load_explicit(&task->active_io_generation,
                                    memory_order_acquire) != old_generation ||
               atomic_load_explicit(&task->wait_generation,
                                    memory_order_acquire) !=
                   old_wait_generation) {
        (void)fail_msg(
            "FR08-002 completion was blocked without retaining generation N");
        goto done;
    }

    release_inflight_owner_hook();
    if (pthread_join(rehome_thread, NULL) != 0) {
        (void)fail_msg("FR08-002 rehome join failed");
        goto done;
    }
    rehome_joined = true;
    clear_inflight_owner_hook();
    hook_armed = false;

    if (completion_started && !completion_joined) {
        if (pthread_join(completion_thread, NULL) != 0) {
            (void)fail_msg("FR08-002 completion join failed");
            goto done;
        }
        completion_joined = true;
    }
    if (!fresh_started) {
        if (!reuse_inflight_generation(&fixture,
                                       source_id,
                                       target_id,
                                       reuse_id,
                                       old_generation,
                                       &new_generation)) {
            (void)fail_msg(
                "FR08-002 could not start a fresh post-rehome generation");
            goto done;
        }
        fresh_started = true;
    }

    if (rehome_call.migrated != 1U ||
        atomic_load_explicit(&rehome_call.done,
                             memory_order_acquire) != 1U ||
        llam_task_active_io_req_load(task) != req ||
        atomic_load_explicit(&task->active_io_generation,
                             memory_order_acquire) != new_generation ||
        atomic_load_explicit(&req->operation_generation,
                             memory_order_acquire) != new_generation ||
        atomic_load_explicit(&task->wait_generation,
                             memory_order_acquire) ==
            old_wait_generation ||
        atomic_load_explicit(&req->inflight_owner_shard,
                             memory_order_acquire) != reuse_id ||
        atomic_load_explicit(&task->parked_shard,
                             memory_order_acquire) != reuse_id ||
        atomic_load_explicit(&req->owner_shard,
                             memory_order_acquire) != reuse_id ||
        task->home_shard != source_id ||
        atomic_load_explicit(
            &fixture.shards[reuse_id].inflight_io_waiters,
            memory_order_acquire) != 1U ||
        atomic_load_explicit(&fixture.runtime.fatal_errno,
                             memory_order_acquire) != 0 ||
        atomic_load_explicit(&fixture.runtime.deferred_fatal_pending,
                             memory_order_acquire) != 0U) {
        fprintf(
            stderr,
            "test_runtime_shutdown_internal: FR08-002 old generation "
            "overwrote fresh ownership: old_gen=%llu new_gen=%llu "
            "parked=%u owner=%u home=%u inflight=%u\n",
            (unsigned long long)old_generation,
            (unsigned long long)new_generation,
            atomic_load_explicit(&task->parked_shard,
                                 memory_order_acquire),
            atomic_load_explicit(&req->owner_shard,
                                 memory_order_acquire),
            task->home_shard,
            atomic_load_explicit(&req->inflight_owner_shard,
                                 memory_order_acquire));
        goto done;
    }

    rc = 0;

done:
    if (hook_armed) {
        release_inflight_owner_hook();
    }
    if (rehome_started && !rehome_joined) {
        (void)pthread_join(rehome_thread, NULL);
    }
    if (completion_started && !completion_joined) {
        (void)pthread_join(completion_thread, NULL);
    }
    if (hook_armed) {
        clear_inflight_owner_hook();
    }

    for (unsigned i = 0U; i < 4U; ++i) {
        atomic_store_explicit(&fixture.shards[i].inflight_io_waiters,
                              0U,
                              memory_order_release);
        clear_inflight_reuse_inject_queue(&fixture, i);
    }
    atomic_store_explicit(&req->inflight_owner_shard,
                          UINT_MAX,
                          memory_order_release);
    atomic_store_explicit(&req->wait_mode,
                          LLAM_IO_WAIT_MODE_NONE,
                          memory_order_release);
    if (llam_task_active_io_req_load(task) == req) {
        (void)llam_task_clear_wait_tracking(task);
    }
    if (atomic_load_explicit(&req->lifetime_refs,
                             memory_order_acquire) != 0U) {
        g_llam_tls_task = task;
        g_llam_tls_shard = &fixture.shards[reuse_id];
        llam_api_io_req_release(g_llam_tls_shard, req);
        g_llam_tls_task = NULL;
        g_llam_tls_shard = NULL;
    }
    destroy_inflight_reuse_fixture(&fixture);
    return rc;
}

static int exercise_inflight_rehome_generation_mismatch_fails_closed(void) {
    enum {
        allocation_owner = 0U,
        source_id = 1U,
        target_id = 2U
    };
    const uint64_t timeout_ns = 2000000000ULL;
    inflight_reuse_fixture_t fixture;
    inflight_rehome_call_t rehome_call;
    llam_task_t *task;
    llam_io_req_t *req;
    pthread_t rehome_thread;
    uint64_t operation_generation;
    uint64_t mismatched_generation;
    uint64_t wait_generation;
    bool rehome_started = false;
    bool rehome_joined = false;
    bool hook_armed = false;
    int rc = 1;

    if (init_inflight_reuse_fixture(
            &fixture, allocation_owner, source_id) != 0) {
        return fail_errno("FR08-002 mismatch fixture init failed");
    }
    task = &fixture.task;
    req = &task->embedded_io_req;
    operation_generation = atomic_load_explicit(
        &req->operation_generation, memory_order_acquire);
    wait_generation = atomic_load_explicit(
        &task->wait_generation, memory_order_acquire);
    mismatched_generation = operation_generation + 1U;

    memset(&rehome_call, 0, sizeof(rehome_call));
    rehome_call.fixture = &fixture;
    rehome_call.source_id = source_id;
    rehome_call.target_id = target_id;
    atomic_init(&rehome_call.done, 0U);

    arm_inflight_owner_hook(req);
    hook_armed = true;
    if (pthread_create(&rehome_thread,
                       NULL,
                       inflight_rehome_thread_main,
                       &rehome_call) != 0) {
        (void)fail_errno(
            "FR08-002 mismatch rehome thread create failed");
        goto done;
    }
    rehome_started = true;
    if (!wait_inflight_owner_hook_bounded(timeout_ns)) {
        (void)fail_msg(
            "FR08-002 mismatch owner publication hook timed out");
        goto done;
    }
    if (atomic_load_explicit(&req->inflight_owner_shard,
                             memory_order_acquire) != target_id ||
        atomic_load_explicit(
            &fixture.shards[source_id].inflight_io_waiters,
            memory_order_acquire) != 1U ||
        atomic_load_explicit(
            &fixture.shards[target_id].inflight_io_waiters,
            memory_order_acquire) != 1U) {
        (void)fail_msg(
            "FR08-002 mismatch missed credited publication window");
        goto done;
    }

    /*
     * operation_generation is immutable for a valid activation. Corrupt it
     * only at the deterministic post-publication hook to exercise the
     * fail-closed invariant branch after the owner move is irreversible.
     */
    atomic_store_explicit(&req->operation_generation,
                          mismatched_generation,
                          memory_order_release);
    release_inflight_owner_hook();
    if (pthread_join(rehome_thread, NULL) != 0) {
        (void)fail_msg("FR08-002 mismatch rehome join failed");
        goto done;
    }
    rehome_joined = true;
    clear_inflight_owner_hook();
    hook_armed = false;

    if (rehome_call.migrated != 0U ||
        atomic_load_explicit(&rehome_call.done,
                             memory_order_acquire) != 1U ||
        atomic_load_explicit(&req->inflight_owner_shard,
                             memory_order_acquire) != target_id ||
        atomic_load_explicit(
            &fixture.shards[source_id].inflight_io_waiters,
            memory_order_acquire) != 0U ||
        atomic_load_explicit(
            &fixture.shards[target_id].inflight_io_waiters,
            memory_order_acquire) != 1U ||
        atomic_load_explicit(&task->parked_shard,
                             memory_order_acquire) != source_id ||
        atomic_load_explicit(&req->owner_shard,
                             memory_order_acquire) != source_id ||
        task->home_shard != source_id ||
        llam_task_active_io_req_load(task) != req ||
        atomic_load_explicit(&task->active_io_generation,
                             memory_order_acquire) !=
            operation_generation ||
        atomic_load_explicit(&task->wait_generation,
                             memory_order_acquire) != wait_generation ||
        atomic_load_explicit(&req->operation_generation,
                             memory_order_acquire) !=
            mismatched_generation ||
        atomic_load_explicit(&task->wait_resolver_state,
                             memory_order_acquire) != 0U ||
        atomic_load_explicit(&fixture.runtime.fatal_errno,
                             memory_order_acquire) != EPROTO ||
        atomic_load_explicit(&fixture.runtime.deferred_fatal_pending,
                             memory_order_acquire) != 1U) {
        fprintf(
            stderr,
            "test_runtime_shutdown_internal: FR08-002 post-publication "
            "mismatch did not fail closed: migrated=%u inflight=%u "
            "source_count=%u target_count=%u parked=%u owner=%u home=%u "
            "resolver=%u fatal=%d deferred=%u\n",
            rehome_call.migrated,
            atomic_load_explicit(&req->inflight_owner_shard,
                                 memory_order_acquire),
            atomic_load_explicit(
                &fixture.shards[source_id].inflight_io_waiters,
                memory_order_acquire),
            atomic_load_explicit(
                &fixture.shards[target_id].inflight_io_waiters,
                memory_order_acquire),
            atomic_load_explicit(&task->parked_shard,
                                 memory_order_acquire),
            atomic_load_explicit(&req->owner_shard,
                                 memory_order_acquire),
            task->home_shard,
            atomic_load_explicit(&task->wait_resolver_state,
                                 memory_order_acquire),
            atomic_load_explicit(&fixture.runtime.fatal_errno,
                                 memory_order_acquire),
            atomic_load_explicit(&fixture.runtime.deferred_fatal_pending,
                                 memory_order_acquire));
        goto done;
    }
    rc = 0;

done:
    if (hook_armed) {
        release_inflight_owner_hook();
    }
    if (rehome_started && !rehome_joined) {
        (void)pthread_join(rehome_thread, NULL);
    }
    if (hook_armed) {
        clear_inflight_owner_hook();
    }
    atomic_store_explicit(&req->operation_generation,
                          operation_generation,
                          memory_order_release);
    atomic_store_explicit(&req->inflight_owner_shard,
                          UINT_MAX,
                          memory_order_release);
    atomic_store_explicit(&req->wait_mode,
                          LLAM_IO_WAIT_MODE_NONE,
                          memory_order_release);
    atomic_store_explicit(
        &fixture.shards[source_id].inflight_io_waiters,
        0U,
        memory_order_release);
    atomic_store_explicit(
        &fixture.shards[target_id].inflight_io_waiters,
        0U,
        memory_order_release);
    if (llam_task_active_io_req_load(task) == req) {
        (void)llam_task_clear_wait_tracking(task);
    }
    if (atomic_load_explicit(&req->lifetime_refs,
                             memory_order_acquire) != 0U) {
        g_llam_tls_task = task;
        g_llam_tls_shard = &fixture.shards[source_id];
        llam_api_io_req_release(g_llam_tls_shard, req);
        g_llam_tls_task = NULL;
        g_llam_tls_shard = NULL;
    }
    destroy_inflight_reuse_fixture(&fixture);
    return rc;
}

static int exercise_merge_request_before_ack_keeps_wake_on_source(void) {
    submit_rehome_fixture_t fixture;
    llam_task_t *task;
    llam_runtime_t *rt;
    bool request_before_ack_stayed;
    bool acknowledged_request_moved;
    int rc = 1;

    if (init_submit_rehome_fixture(&fixture, false) != 0) {
        return fail_errno(
            "merge admission transaction fixture init failed");
    }
    task = &fixture.task;
    rt = &fixture.runtime;
    rt->experimental_dynamic_shards = 1U;
    atomic_store_explicit(
        &fixture.shards[0].current,
        task,
        memory_order_release);
    atomic_store_explicit(
        &fixture.shards[0].merge_pause_requested,
        1U,
        memory_order_release);
    atomic_store_explicit(
        &fixture.shards[0].merge_pause_ack,
        0U,
        memory_order_release);
    if (!llam_task_set_join_tracking(
            task, &fixture.caller, 0U)) {
        goto done;
    }
    g_llam_tls_task = NULL;
    g_llam_tls_shard = NULL;
    llam_reinject_task_on_shard(
        rt,
        task,
        0U,
        true,
        LLAM_TRACE_WAKE,
        LLAM_WAIT_JOIN);
    request_before_ack_stayed =
        fixture.shards[0].inject_q.head == task &&
        fixture.shards[1].inject_q.head == NULL;

    clear_fixture_task_queues(&fixture);
    atomic_store_explicit(
        &fixture.shards[0].current,
        NULL,
        memory_order_release);
    atomic_store_explicit(
        &fixture.shards[0].merge_pause_ack,
        1U,
        memory_order_release);
    if (!llam_task_set_join_tracking(
            task, &fixture.caller, 0U)) {
        goto done;
    }
    llam_reinject_task_on_shard(
        rt,
        task,
        0U,
        true,
        LLAM_TRACE_WAKE,
        LLAM_WAIT_JOIN);
    acknowledged_request_moved =
        fixture.shards[0].inject_q.head == NULL &&
        fixture.shards[1].inject_q.head == task;

    if (!request_before_ack_stayed ||
        !acknowledged_request_moved ||
        atomic_load_explicit(
            &rt->fatal_errno,
            memory_order_acquire) != 0) {
        fprintf(
            stderr,
            "merge admission rerouted an executing waiter: "
            "before_ack_source=%u after_ack_target=%u fatal=%d\n",
            request_before_ack_stayed ? 1U : 0U,
            acknowledged_request_moved ? 1U : 0U,
            atomic_load_explicit(
                &rt->fatal_errno,
                memory_order_acquire));
        goto done;
    }
    rc = 0;

done:
    clear_fixture_task_queues(&fixture);
    atomic_store_explicit(
        &fixture.shards[0].current,
        NULL,
        memory_order_release);
    destroy_submit_rehome_fixture(&fixture);
    return rc;
}

static int run_public_cancel_submit_case(bool migrate) {
    submit_rehome_fixture_t fixture;
    llam_cancel_token_t *token;
    llam_cancel_token_t *raw_token = NULL;
    public_cancel_call_t call;
    pthread_t thread;
    unsigned rehomed = 0U;
    unsigned evacuated = 0U;
    unsigned target_index = migrate ? 1U : 0U;
    int rc = 1;

    if (init_submit_rehome_fixture(&fixture, true) != 0) {
        return fail_errno("submit rehome fixture init failed");
    }
    g_llam_tls_task = &fixture.caller;
    g_llam_tls_shard = &fixture.shards[1];
    token = llam_cancel_token_create();
    if (token == NULL ||
        llam_cancel_token_retain_task_ref(token, &raw_token) != 0) {
        g_llam_tls_task = NULL;
        g_llam_tls_shard = NULL;
        destroy_submit_rehome_fixture(&fixture);
        return fail_errno("cancel token setup failed for submit rehome");
    }
    fixture.task.cancel_token = raw_token;
    if (llam_cancel_token_register_task(&fixture.task) != 0) {
        g_llam_tls_task = NULL;
        g_llam_tls_shard = NULL;
        destroy_submit_rehome_fixture(&fixture);
        return fail_errno("cancel token register failed for submit rehome");
    }
    g_llam_tls_task = NULL;
    g_llam_tls_shard = NULL;

    call.token = token;
    call.caller = &fixture.caller;
    call.caller_shard = &fixture.shards[1];
    call.rc = -1;
    if (migrate) {
        arm_submit_detach_hook(&fixture.req);
        if (pthread_create(&thread, NULL, public_cancel_thread_main, &call) != 0) {
            clear_submit_detach_hook();
            goto cleanup_token;
        }
        wait_submit_detach_hook();
        if (!migrate_submit_rehome_fixture(&fixture,
                                           &rehomed,
                                           &evacuated)) {
            release_submit_detach_hook();
            pthread_join(thread, NULL);
            clear_submit_detach_hook();
            goto cleanup_token;
        }
        release_submit_detach_hook();
        pthread_join(thread, NULL);
        clear_submit_detach_hook();
    } else {
        (void)public_cancel_thread_main(&call);
    }

    if (call.rc != 0 || fixture.task.cancel_registered ||
        (migrate && (rehomed != 1U || evacuated != 1U)) ||
        fixture.nodes[0].submit_head != NULL ||
        fixture.nodes[1].submit_head != NULL ||
        atomic_load_explicit(&fixture.nodes[0].pending_ops,
                             memory_order_acquire) != 0U ||
        atomic_load_explicit(&fixture.nodes[1].pending_ops,
                             memory_order_acquire) != 0U ||
        atomic_load_explicit(&fixture.req.wait_mode,
                             memory_order_acquire) != LLAM_IO_WAIT_MODE_NONE ||
        fixture.req.result != -1 || fixture.req.error_code != ECANCELED ||
        atomic_load_explicit(&fixture.task.state,
                             memory_order_acquire) != LLAM_TASK_STATE_RUNNABLE ||
        llam_task_active_io_req_load(&fixture.task) != NULL ||
        (fixture.shards[target_index].inject_q.head != &fixture.task &&
         fixture.shards[target_index].hot_q.head != &fixture.task &&
         fixture.shards[target_index].norm_q.head != &fixture.task) ||
        atomic_load_explicit(&fixture.req.lifetime_refs,
                             memory_order_acquire) != 1U ||
        atomic_load_explicit(&fixture.runtime.fatal_errno,
                             memory_order_acquire) != 0) {
        fprintf(stderr,
                "submit cancel diag migrate=%d rc=%d registered=%d rehomed=%u evacuated=%u "
                "heads=%p/%p pending=%u/%u mode=%u result=%lld error=%d state=%u active=%p "
                "inject=%p hot=%p norm=%p refs=%u fatal=%d\n",
                migrate,
                call.rc,
                fixture.task.cancel_registered,
                rehomed,
                evacuated,
                (void *)fixture.nodes[0].submit_head,
                (void *)fixture.nodes[1].submit_head,
                atomic_load_explicit(&fixture.nodes[0].pending_ops, memory_order_acquire),
                atomic_load_explicit(&fixture.nodes[1].pending_ops, memory_order_acquire),
                atomic_load_explicit(&fixture.req.wait_mode, memory_order_acquire),
                (long long)fixture.req.result,
                fixture.req.error_code,
                atomic_load_explicit(&fixture.task.state, memory_order_acquire),
                (void *)llam_task_active_io_req_load(&fixture.task),
                (void *)fixture.shards[target_index].inject_q.head,
                (void *)fixture.shards[target_index].hot_q.head,
                (void *)fixture.shards[target_index].norm_q.head,
                atomic_load_explicit(&fixture.req.lifetime_refs, memory_order_acquire),
                atomic_load_explicit(&fixture.runtime.fatal_errno, memory_order_acquire));
        goto cleanup_token;
    }
    fixture.shards[target_index].inject_q.head = NULL;
    fixture.shards[target_index].inject_q.tail = NULL;
    fixture.shards[target_index].inject_q.depth = 0U;
    fixture.shards[target_index].hot_q.head = NULL;
    fixture.shards[target_index].hot_q.tail = NULL;
    fixture.shards[target_index].hot_q.depth = 0U;
    fixture.shards[target_index].norm_q.head = NULL;
    fixture.shards[target_index].norm_q.tail = NULL;
    fixture.shards[target_index].norm_q.depth = 0U;
    atomic_store_explicit(&fixture.shards[target_index].inject_depth,
                          0U,
                          memory_order_release);
    rc = 0;

cleanup_token:
    fixture.task.cancel_token = NULL;
    llam_cancel_token_release_task_ref(raw_token);
    g_llam_tls_task = &fixture.caller;
    g_llam_tls_shard = &fixture.shards[1];
    if (llam_cancel_token_destroy(token) != 0) {
        rc = 1;
    }
    g_llam_tls_task = NULL;
    g_llam_tls_shard = NULL;
    destroy_submit_rehome_fixture(&fixture);
    if (rc != 0) {
        return fail_msg(migrate
                            ? "public cancel lost an evacuated submit request"
                            : "public cancel submit control failed");
    }
    return 0;
}

static int run_setup_abort_submit_case(bool migrate) {
    submit_rehome_fixture_t fixture;
    setup_abort_call_t call;
    pthread_t thread;
    unsigned rehomed = 0U;
    unsigned evacuated = 0U;

    if (init_submit_rehome_fixture(&fixture, true) != 0) {
        return fail_errno("setup abort fixture init failed");
    }
    call.req = &fixture.req;
    call.wait_for_completion = true;
    call.rc = false;
    if (migrate) {
        arm_submit_detach_hook(&fixture.req);
        if (pthread_create(&thread, NULL, setup_abort_thread_main, &call) != 0) {
            clear_submit_detach_hook();
            destroy_submit_rehome_fixture(&fixture);
            return fail_errno("setup abort thread create failed");
        }
        wait_submit_detach_hook();
        if (!migrate_submit_rehome_fixture(&fixture,
                                           &rehomed,
                                           &evacuated)) {
            release_submit_detach_hook();
            pthread_join(thread, NULL);
            clear_submit_detach_hook();
            destroy_submit_rehome_fixture(&fixture);
            return fail_msg("setup abort rehome failed");
        }
        release_submit_detach_hook();
        pthread_join(thread, NULL);
        clear_submit_detach_hook();
    } else {
        (void)setup_abort_thread_main(&call);
    }

    if (!call.rc || call.wait_for_completion ||
        (migrate && (rehomed != 1U || evacuated != 1U)) ||
        fixture.nodes[0].submit_head != NULL ||
        fixture.nodes[1].submit_head != NULL ||
        atomic_load_explicit(&fixture.nodes[0].pending_ops,
                             memory_order_acquire) != 0U ||
        atomic_load_explicit(&fixture.nodes[1].pending_ops,
                             memory_order_acquire) != 0U ||
        atomic_load_explicit(&fixture.req.wait_mode,
                             memory_order_acquire) != LLAM_IO_WAIT_MODE_NONE ||
        fixture.req.result != -1 || fixture.req.error_code != ECANCELED ||
        atomic_load_explicit(&fixture.runtime.fatal_errno,
                             memory_order_acquire) != 0) {
        destroy_submit_rehome_fixture(&fixture);
        return fail_msg(migrate
                            ? "setup abort left an evacuated request owned"
                            : "setup abort submit control failed");
    }
    llam_cleanup_io_wait_setup(&fixture.task, &fixture.req);
    destroy_submit_rehome_fixture(&fixture);
    return 0;
}

static int exercise_prepublication_cancel_rejects_late_submit(void) {
    submit_rehome_fixture_t fixture;

    if (init_submit_rehome_fixture(&fixture, false) != 0) {
        return fail_errno("prepublication cancel fixture init failed");
    }
    llam_cancel_task_wait(&fixture.task);
    errno = 0;
    if (atomic_load_explicit(&fixture.req.abort_reason,
                             memory_order_acquire) != LLAM_IO_ABORT_CANCEL ||
        llam_node_submit_io_req(&fixture.nodes[0], &fixture.req) ||
        errno != ECANCELED ||
        fixture.nodes[0].submit_head != NULL ||
        atomic_load_explicit(&fixture.nodes[0].pending_ops,
                             memory_order_acquire) != 0U ||
        atomic_load_explicit(&fixture.runtime.fatal_errno,
                             memory_order_acquire) != 0) {
        destroy_submit_rehome_fixture(&fixture);
        return fail_msg("prepublication cancellation was lost to late submit");
    }
    llam_cleanup_io_wait_setup(&fixture.task, &fixture.req);
    if (atomic_load_explicit(&fixture.task.state,
                             memory_order_acquire) != LLAM_TASK_STATE_RUNNING ||
        llam_task_active_io_req_load(&fixture.task) != NULL ||
        atomic_load_explicit(&fixture.req.wait_mode,
                             memory_order_acquire) != LLAM_IO_WAIT_MODE_NONE) {
        destroy_submit_rehome_fixture(&fixture);
        return fail_msg("prepublication cancellation cleanup failed");
    }
    destroy_submit_rehome_fixture(&fixture);
    return 0;
}

static int exercise_evacuation_pending_transfer_is_atomic(void) {
    submit_rehome_fixture_t fixture;
    submit_evacuation_call_t call;
    pthread_t thread;
    unsigned rehomed = 0U;
    unsigned detached_node = UINT_MAX;
    llam_io_submit_detach_result_t detach_result;

    if (init_submit_rehome_fixture(&fixture, true) != 0) {
        return fail_errno("evacuation accounting fixture init failed");
    }
    if (!llam_rehome_node_submit_waiters(&fixture.nodes[0],
                                         &fixture.shards[0],
                                         &fixture.shards[1],
                                         &rehomed) ||
        rehomed != 1U) {
        destroy_submit_rehome_fixture(&fixture);
        return fail_msg("evacuation accounting rehome failed");
    }
    call.fixture = &fixture;
    call.migrated = 0U;
    call.rc = false;
    arm_submit_evacuation_hook();
    if (pthread_create(&thread,
                       NULL,
                       submit_evacuation_thread_main,
                       &call) != 0) {
        clear_submit_evacuation_hook();
        destroy_submit_rehome_fixture(&fixture);
        return fail_errno("evacuation accounting thread create failed");
    }
    wait_submit_evacuation_hook();
    detach_result = llam_detach_submit_req_current(&fixture.req,
                                                   &detached_node);
    release_submit_evacuation_hook();
    pthread_join(thread, NULL);
    clear_submit_evacuation_hook();

    if (!call.rc || call.migrated != 1U ||
        detach_result != LLAM_IO_SUBMIT_DETACH_REMOVED ||
        detached_node != 1U ||
        fixture.nodes[0].submit_head != NULL ||
        fixture.nodes[1].submit_head != NULL ||
        atomic_load_explicit(&fixture.nodes[0].pending_ops,
                             memory_order_acquire) != 0U ||
        atomic_load_explicit(&fixture.nodes[1].pending_ops,
                             memory_order_acquire) != 0U ||
        atomic_load_explicit(&fixture.runtime.fatal_errno,
                             memory_order_acquire) != 0) {
        destroy_submit_rehome_fixture(&fixture);
        return fail_msg("evacuation exposed queue/pending accounting gap");
    }
    llam_cleanup_io_wait_setup(&fixture.task, &fixture.req);
    destroy_submit_rehome_fixture(&fixture);
    return 0;
}

static int exercise_submit_cancel_rehome_regressions(void) {
    int failed = 0;

    if (exercise_inflight_owner_credit_precedes_publication() != 0) {
        failed = 1;
    }
    if (exercise_inflight_rehome_is_generation_bound() != 0) {
        failed = 1;
    }
    if (exercise_inflight_rehome_generation_mismatch_fails_closed() != 0) {
        failed = 1;
    }
    if (exercise_merge_request_before_ack_keeps_wake_on_source() != 0) {
        failed = 1;
    }
    if (run_public_cancel_submit_case(false) != 0 ||
        run_public_cancel_submit_case(true) != 0 ||
        run_setup_abort_submit_case(false) != 0 ||
        run_setup_abort_submit_case(true) != 0 ||
        exercise_prepublication_cancel_rejects_late_submit() != 0 ||
        exercise_evacuation_pending_transfer_is_atomic() != 0) {
        failed = 1;
    }
    return failed;
}
#else
static int exercise_submit_cancel_rehome_regressions(void) {
    return 0;
}
#endif

#if defined(LLAM_ENABLE_TEST_HOOKS) && !LLAM_PLATFORM_WINDOWS
static int exercise_exact_prewarm_failure_unwinds(void) {
    const char *current = getenv("LLAM_TASK_CACHE_PREWARM");
    char *saved = current != NULL ? strdup(current) : NULL;
    llam_runtime_opts_t opts;
    llam_runtime_stats_t stats;
    llam_runtime_t *runtime = NULL;
    int rc = 1;

    if (current != NULL && saved == NULL) {
        return fail_errno("saving task prewarm environment failed");
    }
    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        rc = fail_errno("prewarm rollback opts init failed");
        goto cleanup;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    opts.worker_min = 1U;
    opts.worker_count = 1U;
    opts.worker_max = 1U;
    opts.blocking_min = 1U;
    opts.blocking_max = 1U;
    opts.task_prewarm_total = 17U;
    opts.stack_prewarm_total = 3U;
    opts.timer_prewarm_total = 3U;

    for (unsigned kind = 0U; kind < LLAM_TEST_PREWARM_KIND_COUNT; ++kind) {
        uint64_t successful_objects =
            kind == LLAM_TEST_PREWARM_TASK ? 16U : 1U;

        llam_runtime_test_reset_prewarm_allocation_limits();
        llam_runtime_test_set_prewarm_allocation_limit(
            (llam_test_prewarm_kind_t)kind,
            successful_objects);
        errno = 0;
        if (llam_runtime_create(&opts,
                                LLAM_RUNTIME_OPTS_CURRENT_SIZE,
                                &runtime) != -1 ||
            errno != ENOMEM || runtime != NULL) {
            rc = fail_msg("exact prewarm allocation failure did not unwind initialization");
            goto cleanup;
        }
    }

    llam_runtime_test_reset_prewarm_allocation_limits();
    if (setenv("LLAM_TASK_CACHE_PREWARM", "1", 1) != 0) {
        rc = fail_errno("setting legacy task prewarm environment failed");
        goto cleanup;
    }
    opts.task_prewarm_total = 0U;
    opts.stack_prewarm_total = 1U;
    opts.timer_prewarm_total = 1U;
    llam_runtime_test_set_prewarm_allocation_limit(LLAM_TEST_PREWARM_TASK, 0U);
    if (llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime) != 0 ||
        llam_runtime_collect_stats_ex_handle(runtime, &stats, sizeof(stats)) != 0) {
        rc = fail_errno("best-effort legacy prewarm did not survive allocation exhaustion");
        goto cleanup;
    }
    if (stats.requested_task_prewarm_total != 1U ||
        stats.achieved_task_prewarm_total != 0U ||
        stats.task_prewarm_source != LLAM_RUNTIME_PREWARM_ENV_LEGACY) {
        rc = fail_msg("best-effort legacy prewarm diagnostics were inconsistent");
        goto cleanup;
    }
    llam_runtime_destroy(runtime);
    runtime = NULL;

    /*
     * A failed exact request was registered before allocation. A later create
     * proves teardown removed that partial handle and all prewarmed timer/task
     * storage rather than poisoning the process registry.
     */
    llam_runtime_test_reset_prewarm_allocation_limits();
    opts.task_prewarm_total = 1U;
    if (llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime) != 0) {
        rc = fail_errno("runtime create after exact prewarm rollback failed");
        goto cleanup;
    }
    rc = 0;

cleanup:
    llam_runtime_destroy(runtime);
    llam_runtime_test_reset_prewarm_allocation_limits();
    if (saved != NULL) {
        (void)setenv("LLAM_TASK_CACHE_PREWARM", saved, 1);
    } else {
        (void)unsetenv("LLAM_TASK_CACHE_PREWARM");
    }
    free(saved);
    return rc;
}
#endif

int main(void) {
    if (exercise_stack_cache_vm_cases() != 0) {
        return 1;
    }
#if defined(LLAM_ENABLE_TEST_HOOKS)
    if (exercise_first_block_worker_create_failure_rolls_back_submission() != 0) {
        return 1;
    }
    if (exercise_second_block_worker_create_failure_uses_existing_worker() != 0) {
        return 1;
    }
    if (exercise_block_pool_min_partial_failure_unwinds() != 0) {
        return 1;
    }
    if (exercise_affinity_policy_matrix() != 0) {
        return 1;
    }
    if (exercise_affinity_unsupported_policy() != 0) {
        return 1;
    }
    if (exercise_affinity_restore_on_worker_create_failure() != 0) {
        return 1;
    }
    if (exercise_affinity_restore_on_task_exit(false) != 0) {
        return 1;
    }
    if (exercise_affinity_restore_on_task_exit(true) != 0) {
        return 1;
    }
    if (exercise_native_thread_counter_saturates() != 0) {
        return 1;
    }
    if (exercise_native_thread_counter_rejects_underflow() != 0) {
        return 1;
    }
#endif
#if defined(LLAM_ENABLE_TEST_HOOKS) && !LLAM_PLATFORM_WINDOWS
    const char *fr08_mode = getenv("LLAM_VALIDATE_FR08_002_ONLY");

    if (fr08_mode != NULL &&
        strcmp(fr08_mode, "mismatch") == 0) {
        return exercise_inflight_rehome_generation_mismatch_fails_closed();
    }
    if (fr08_mode != NULL && strcmp(fr08_mode, "all") == 0) {
        if (exercise_inflight_rehome_is_generation_bound() != 0) {
            return 1;
        }
        return exercise_inflight_rehome_generation_mismatch_fails_closed();
    }
    if (fr08_mode != NULL) {
        return exercise_inflight_rehome_is_generation_bound();
    }
#endif
#if defined(LLAM_ENABLE_TEST_HOOKS) && !LLAM_PLATFORM_WINDOWS
    if (exercise_exact_prewarm_failure_unwinds() != 0) {
        return 1;
    }
#endif
    if (exercise_io_lifetime_invariants_are_lock_safe() != 0) {
        return 1;
    }
    if (exercise_park_completion_preserves_result() != 0) {
        return 1;
    }
    if (exercise_submit_cancel_rehome_regressions() != 0) {
        return 1;
    }
    if (exercise_recv_ready_copy_payload_shutdown() != 0) {
        return 1;
    }
    if (exercise_recv_ready_pop_without_transfer() != 0) {
        return 1;
    }
    if (exercise_close_purges_accept_watch_ready_fds() != 0) {
        return 1;
    }
    if (exercise_host_close_purges_explicit_runtime_accept_watch_ready_fds() != 0) {
        return 1;
    }
    if (exercise_managed_close_purges_peer_runtime_accept_watch_ready_fds() != 0) {
        return 1;
    }
    if (exercise_closed_watch_generation_is_deferred_and_not_reused() != 0) {
        return 1;
    }
    if (exercise_active_closed_watch_queues_one_deactivate() != 0) {
        return 1;
    }
    if (exercise_closed_watch_reclamation_stays_bounded() != 0) {
        return 1;
    }
    if (exercise_darwin_closed_live_generation_deletes_knote() != 0) {
        return 1;
    }
    if (exercise_darwin_poll_filter_specific_replacement() != 0) {
        return 1;
    }
    if (exercise_close_completes_parked_watch_waiter() != 0) {
        return 1;
    }
    if (exercise_linux_oversized_submit_preserves_sq_tail() != 0) {
        return 1;
    }
    if (exercise_linux_invalid_request_preserves_sq_tail() != 0) {
        return 1;
    }
    if (exercise_linux_invalid_control_preserves_sq_tail() != 0) {
        return 1;
    }
    if (exercise_linux_staged_cancel_control_retries_and_retires() != 0) {
        return 1;
    }
#if LLAM_RUNTIME_BACKEND_LINUX
    if (exercise_linux_accept_rejects_stale_deactivate_overlap() != 0) {
        return 1;
    }
    if (exercise_linux_terminal_watch_rearms() != 0) {
        return 1;
    }
    if (exercise_linux_activation_rejects_deactivate_overlap() != 0) {
        return 1;
    }
    if (exercise_linux_migration_finalize_holds_watch_pin() != 0) {
        return 1;
    }
    if (exercise_linux_migration_pin_saturation_fails_closed() != 0) {
        return 1;
    }
    if (exercise_linux_closed_watch_cqe_orders() != 0) {
        return 1;
    }
    if (exercise_linux_activation_terminal_teardown() != 0) {
        return 1;
    }
#endif
    if (exercise_linux_wait_cqe_interrupt_policy() != 0) {
        return 1;
    }
    if (exercise_empty_poll_watch_cancel_disarms_backend_work() != 0) {
        return 1;
    }
    if (exercise_completion_drops_stale_cancel_control() != 0) {
        return 1;
    }
    if (exercise_completion_rejects_foreign_runtime_request() != 0) {
        return 1;
    }
    if (exercise_completion_rejects_unmatched_pending_decrement() != 0) {
        return 1;
    }
    if (exercise_pending_underflow_defers_fatal_under_watch_lock() != 0) {
        return 1;
    }
    if (exercise_submit_queue_rejects_foreign_runtime_request() != 0) {
        return 1;
    }
    if (exercise_submit_queue_rejects_pending_counter_overflow() != 0) {
        return 1;
    }
    if (exercise_block_worker_rejects_pending_counter_underflow() != 0) {
        return 1;
    }
    if (exercise_block_worker_rejects_active_counter_overflow() != 0) {
        return 1;
    }
    if (exercise_task_scan_ref_counter_saturation_is_rejected() != 0) {
        return 1;
    }
    if (exercise_norm_depth_counter_wrap_is_rejected() != 0) {
        return 1;
    }
    if (exercise_cldeque_delayed_thief_preserves_wrapped_task() != 0) {
        return 1;
    }
    if (exercise_channel_inflight_waiter_counter_overflow_is_rejected() != 0) {
        return 1;
    }
    if (exercise_channel_inflight_waiter_counter_underflow_is_rejected() != 0) {
        return 1;
    }
    if (exercise_cond_inflight_waiter_counter_overflow_is_rejected() != 0) {
        return 1;
    }
    if (exercise_inflight_waiter_counter_underflow_is_rejected() != 0) {
        return 1;
    }
    if (exercise_inflight_waiter_counter_overflow_is_rejected() != 0) {
        return 1;
    }
    if (exercise_active_io_waiter_counter_overflow_is_rejected() != 0) {
        return 1;
    }
    if (exercise_active_io_waiter_counter_underflow_is_rejected() != 0) {
        return 1;
    }
    if (exercise_live_task_sum_saturates_on_overflow() != 0) {
        return 1;
    }
    if (exercise_task_live_counter_overflow_fails_closed() != 0) {
        return 1;
    }
    if (exercise_dynamic_scaler_live_saturation_fails_closed() != 0) {
        return 1;
    }
    if (exercise_canceled_blocking_results_are_disposed() != 0) {
        return 1;
    }
    if (exercise_close_unpublishes_detached_watch_waiters() != 0) {
        return 1;
    }
    printf("test_runtime_shutdown_internal ok\n");
    return 0;
}
