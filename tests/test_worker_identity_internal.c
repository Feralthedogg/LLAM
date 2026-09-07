/**
 * @file tests/test_worker_identity_internal.c
 * @brief Characterization tests for legacy scheduler worker roles and opaque helper lifecycle.
 *
 * @copyright Copyright 2026 Feralthedogg
 *
 * @par License
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
 * See LICENSES/OLD-LICENSE/Apache-2.0.txt.
 */

#include "runtime_internal.h"

#include <errno.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if LLAM_PLATFORM_POSIX
#include <pthread.h>
#else
#include <windows.h>
#endif

#define LEGACY_TEST_WAIT_NS (3ULL * 1000ULL * 1000ULL * 1000ULL)

typedef struct legacy_worker_observation {
    uintptr_t shard;
    uintptr_t scheduler_context;
    unsigned primary_by_context;
    unsigned helper_by_context;
} legacy_worker_observation_t;

typedef struct test_failure_state {
    atomic_uint failures;
    const char *first_failure;
} test_failure_state_t;

typedef struct helper_failure_state {
    test_failure_state_t failure;
    int enter_rc;
    int enter_errno;
    int leave_rc;
} helper_failure_state_t;

typedef struct role_state role_state_t;

struct role_state {
    test_failure_state_t failure;
    llam_runtime_t *runtime;
    legacy_worker_observation_t root_before;
    legacy_worker_observation_t root_after;
    legacy_worker_observation_t helper;
    legacy_worker_observation_t helper_after_enter;
    legacy_worker_observation_t helper_after_leave;
    atomic_uint helper_copied;
    atomic_uint helper_done;
    atomic_uint root_depth_after_nested_enter;
    atomic_uint root_depth_after_inner_leave;
    atomic_uint root_depth_after_outer_leave;
    atomic_uint root_compensation_after_nested_enter;
    atomic_int helper_enter_rc;
    atomic_int helper_leave_rc;
    atomic_uint helper_redirect_active;
    atomic_uint helper_redirect_depth;
    atomic_uint helper_ready;
    atomic_uint helper_thread_started;
    atomic_uint helper_driver_matches;
    atomic_uint helper_identity_preserved;
};

typedef struct repeat_task_arg {
    struct repeat_state *state;
    unsigned index;
} repeat_task_arg_t;

typedef struct repeat_state {
    test_failure_state_t failure;
    llam_runtime_t *runtime;
    legacy_worker_observation_t observations[2];
    atomic_uint first_seen;
    atomic_uint first_run_done;
    atomic_uint first_driver_done;
    atomic_uint second_seen;
    atomic_uint second_run_done;
    atomic_uint second_driver_done;
    pthread_mutex_t lock;
    pthread_cond_t condition;
    unsigned release_first;
    int first_run_rc;
    int second_run_rc;
    pthread_t first_driver;
    pthread_t second_driver;
} repeat_state_t;

typedef struct active_shutdown_state {
    test_failure_state_t failure;
    llam_runtime_t *runtime;
    atomic_uint primary_entered;
    atomic_uint helper_state_observed;
    atomic_uint helper_started;
    atomic_uint helper_ready;
    atomic_uint helper_active;
    atomic_uint release_primary;
    atomic_uint destroy_started;
    atomic_uint task_done;
    atomic_uint release_thread_done;
    atomic_uint destroy_done;
    atomic_int run_rc;
    atomic_uint run_done;
} active_shutdown_state_t;

static int create_deterministic_runtime(llam_runtime_t **out) {
    llam_runtime_opts_t opts;

    if (llam_runtime_opts_init(&opts, sizeof(opts)) != 0) {
        return -1;
    }
    opts.deterministic = 1U;
    opts.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    return llam_runtime_create(&opts, sizeof(opts), out);
}

static void observe_legacy_worker(legacy_worker_observation_t *out) {
    llam_shard_t *shard = g_llam_tls_shard;

    memset(out, 0, sizeof(*out));
    out->shard = (uintptr_t)shard;
    out->scheduler_context = (uintptr_t)g_llam_tls_scheduler_ctx;
    if (shard != NULL) {
        out->primary_by_context =
            g_llam_tls_scheduler_ctx == &shard->scheduler_ctx;
        out->helper_by_context =
            g_llam_tls_scheduler_ctx == &shard->opaque_scheduler_ctx;
    }
}

static uint64_t legacy_deadline(void) {
    return llam_now_ns() + LEGACY_TEST_WAIT_NS;
}

static int wait_flag(const atomic_uint *flag, uint64_t deadline_ns) {
    while (atomic_load_explicit(flag, memory_order_acquire) == 0U) {
        if (llam_now_ns() >= deadline_ns) {
            return -1;
        }
        llam_pause_cpu();
    }
    return 0;
}

static int repeat_cond_timedwait(repeat_state_t *state, uint64_t deadline_ns) {
    uint64_t now_ns = llam_now_ns();
    uint64_t remaining_ns;

    if (now_ns >= deadline_ns) {
        return ETIMEDOUT;
    }
    remaining_ns = deadline_ns - now_ns;
#if LLAM_PLATFORM_WINDOWS
    {
        uint64_t timeout_ms = (remaining_ns + 999999ULL) / 1000000ULL;
        DWORD timeout = timeout_ms >= (uint64_t)INFINITE ? INFINITE - 1U : (DWORD)timeout_ms;

        if (SleepConditionVariableSRW(&state->condition.cv,
                                      &state->lock.lock,
                                      timeout,
                                      0U)) {
            return 0;
        }
        return GetLastError() == ERROR_TIMEOUT ? ETIMEDOUT : EINVAL;
    }
#else
    {
        struct timespec deadline;

#if defined(__APPLE__)
        deadline.tv_sec = (time_t)(remaining_ns / 1000000000ULL);
        deadline.tv_nsec = (long)(remaining_ns % 1000000000ULL);
        return pthread_cond_timedwait_relative_np(&state->condition,
                                                  &state->lock,
                                                  &deadline);
#else
        if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
            return EINVAL;
        }
        deadline.tv_sec += (time_t)(remaining_ns / 1000000000ULL);
        deadline.tv_nsec += (long)(remaining_ns % 1000000000ULL);
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec += 1;
            deadline.tv_nsec -= 1000000000L;
        }
        return pthread_cond_timedwait(&state->condition, &state->lock, &deadline);
#endif
    }
#endif
}

static int repeat_condition_init(pthread_cond_t *condition) {
#if LLAM_PLATFORM_POSIX && !defined(__APPLE__)
    pthread_condattr_t attr;
    int rc;

    rc = pthread_condattr_init(&attr);
    if (rc != 0) {
        return rc;
    }
    rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (rc == 0) {
        rc = pthread_cond_init(condition, &attr);
    }
    (void)pthread_condattr_destroy(&attr);
    return rc;
#else
    return pthread_cond_init(condition, NULL);
#endif
}

static void test_fail(test_failure_state_t *state, const char *message) {
    if (atomic_fetch_add_explicit(&state->failures, 1U, memory_order_acq_rel) == 0U) {
        state->first_failure = message;
    }
}

static int failure_result(const test_failure_state_t *state, const char *fallback) {
    if (atomic_load_explicit(&state->failures, memory_order_acquire) != 0U) {
        fprintf(stderr,
                "test_worker_identity_internal: %s\n",
                state->first_failure != NULL ? state->first_failure : fallback);
        return 1;
    }
    return 0;
}

static void fail_safe_cleanup(const char *message) {
    fprintf(stderr, "test_worker_identity_internal: %s\n", message);
    fflush(stderr);
    _Exit(1);
}

static void require_flag_before_cleanup(const atomic_uint *flag, const char *message) {
    if (wait_flag(flag, legacy_deadline()) != 0) {
        fail_safe_cleanup(message);
    }
}

typedef struct bounded_destroy_call {
    llam_runtime_t *runtime;
    atomic_uint done;
} bounded_destroy_call_t;

static void *bounded_destroy_main(void *arg) {
    bounded_destroy_call_t *call = arg;

    llam_runtime_destroy(call->runtime);
    atomic_store_explicit(&call->done, 1U, memory_order_release);
    return NULL;
}

static void destroy_runtime_bounded(llam_runtime_t **runtime, const char *message) {
    bounded_destroy_call_t call;
    pthread_t destroy_thread;

    if (runtime == NULL || *runtime == NULL) {
        return;
    }
    call.runtime = *runtime;
    atomic_init(&call.done, 0U);
    if (pthread_create(&destroy_thread, NULL, bounded_destroy_main, &call) != 0) {
        fail_safe_cleanup(message);
    }
    if (wait_flag(&call.done, legacy_deadline()) != 0) {
        fail_safe_cleanup(message);
    }
    pthread_join(destroy_thread, NULL);
    *runtime = NULL;
}

static void helper_failure_task(void *arg) {
    helper_failure_state_t *state = arg;

    errno = 0;
    state->enter_rc = llam_enter_blocking();
    state->enter_errno = errno;
    state->leave_rc = llam_leave_blocking();
    if (state->enter_rc != 0 || state->leave_rc != 0) {
        test_fail(&state->failure, "helper-create failure did not preserve blocking lifecycle");
    }
}

static int test_helper_create_failure_redirect(void) {
    helper_failure_state_t state;
    llam_runtime_t *runtime = NULL;
    llam_task_t *task = NULL;
    llam_shard_t *shard;
    uint64_t redirect_before;
    bool helper_started;
    bool helper_ready;
    bool helper_failed;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failure.failures, 0U);
    if (create_deterministic_runtime(&runtime) != 0) {
        fprintf(stderr,
                "test_worker_identity_internal: runtime create failed: errno=%d (%s)\n",
                errno,
                strerror(errno));
        return 1;
    }
    shard = &runtime->shards[0];
    redirect_before = atomic_load_explicit(&shard->metrics.opaque_redirect_activations,
                                           memory_order_acquire);
    task = llam_runtime_spawn_ex(runtime, helper_failure_task, &state, NULL, 0U);
    if (task == NULL) {
        destroy_runtime_bounded(&runtime, "helper-failure runtime destroy exceeded the bounded deadline");
        fprintf(stderr,
                "test_worker_identity_internal: task spawn failed: errno=%d (%s)\n",
                errno,
                strerror(errno));
        return 1;
    }

    llam_worker_test_force_helper_create_error(EAGAIN);
    if (llam_runtime_run_handle(runtime) != 0 || llam_join(task) != 0) {
        destroy_runtime_bounded(&runtime, "helper-failure runtime destroy exceeded the bounded deadline");
        fprintf(stderr,
                "test_worker_identity_internal: helper-create run/join failed: errno=%d (%s)\n",
                errno,
                strerror(errno));
        return 1;
    }

    pthread_mutex_lock(&shard->opaque_lock);
    helper_started = shard->opaque_helper_thread_started;
    helper_ready = shard->opaque_helper_ready;
    helper_failed = shard->opaque_helper_failed;
    pthread_mutex_unlock(&shard->opaque_lock);
    if (failure_result(&state.failure, "helper-create task failed")) {
        destroy_runtime_bounded(&runtime, "helper-failure runtime destroy exceeded the bounded deadline");
        return 1;
    }
    if (state.enter_errno != EAGAIN) {
        destroy_runtime_bounded(&runtime, "helper-failure runtime destroy exceeded the bounded deadline");
        fprintf(stderr,
                "test_worker_identity_internal: helper-create failure did not preserve injected errno\n");
        return 1;
    }
    if (atomic_load_explicit(&shard->metrics.opaque_redirect_activations,
                             memory_order_acquire) <= redirect_before) {
        destroy_runtime_bounded(&runtime, "helper-failure runtime destroy exceeded the bounded deadline");
        fprintf(stderr,
                "test_worker_identity_internal: helper-create failure did not activate opaque redirect\n");
        return 1;
    }
    if (helper_started || helper_ready || !helper_failed) {
        destroy_runtime_bounded(&runtime, "helper-failure runtime destroy exceeded the bounded deadline");
        fprintf(stderr,
                "test_worker_identity_internal: helper-create failure published an invalid helper state\n");
        return 1;
    }

    /* The injected error is one-shot: a later enter may create the helper. */
    memset(&state, 0, sizeof(state));
    atomic_init(&state.failure.failures, 0U);
    task = llam_runtime_spawn_ex(runtime, helper_failure_task, &state, NULL, 0U);
    if (task == NULL || llam_runtime_run_handle(runtime) != 0 || llam_join(task) != 0) {
        destroy_runtime_bounded(&runtime, "helper-failure runtime destroy exceeded the bounded deadline");
        fprintf(stderr,
                "test_worker_identity_internal: helper-create one-shot rerun failed: errno=%d (%s)\n",
                errno,
                strerror(errno));
        return 1;
    }
    pthread_mutex_lock(&shard->opaque_lock);
    helper_started = shard->opaque_helper_thread_started;
    helper_ready = shard->opaque_helper_ready;
    pthread_mutex_unlock(&shard->opaque_lock);
    if (failure_result(&state.failure, "helper-create one-shot task failed")) {
        destroy_runtime_bounded(&runtime, "helper-failure runtime destroy exceeded the bounded deadline");
        return 1;
    }
    if (!helper_started || !helper_ready) {
        destroy_runtime_bounded(&runtime, "helper-failure runtime destroy exceeded the bounded deadline");
        fprintf(stderr,
                "test_worker_identity_internal: helper-create injection was not one-shot\n");
        return 1;
    }
    destroy_runtime_bounded(&runtime, "helper-failure runtime destroy exceeded the bounded deadline");
    return 0;
}

static void helper_role_task(void *arg) {
    role_state_t *state = arg;
    llam_shard_t *shard;
    pthread_t helper_before;
    pthread_t configured_before;
    pthread_t configured_after;
    bool helper_started;

    helper_before = pthread_self();
    observe_legacy_worker(&state->helper);
    shard = g_llam_tls_shard;
    pthread_mutex_lock(&shard->opaque_lock);
    atomic_store_explicit(&state->helper_ready,
                          shard->opaque_helper_ready ? 1U : 0U,
                          memory_order_release);
    atomic_store_explicit(&state->helper_thread_started,
                          shard->opaque_helper_thread_started ? 1U : 0U,
                          memory_order_release);
    helper_started = shard->opaque_helper_thread_started;
    configured_before = helper_started ? shard->opaque_helper_thread : helper_before;
    atomic_store_explicit(&state->helper_driver_matches,
                          helper_started &&
                                  pthread_equal(helper_before, shard->opaque_helper_thread) != 0
                              ? 1U
                              : 0U,
                          memory_order_release);
    pthread_mutex_unlock(&shard->opaque_lock);
    atomic_store_explicit(&state->helper_copied, 1U, memory_order_release);

    atomic_store_explicit(&state->helper_enter_rc, llam_enter_blocking(), memory_order_release);
    observe_legacy_worker(&state->helper_after_enter);
    if (atomic_load_explicit(&state->helper_enter_rc, memory_order_acquire) == 0) {
        pthread_mutex_lock(&shard->lock);
        atomic_store_explicit(&state->helper_redirect_active,
                              shard->opaque_redirect_active ? 1U : 0U,
                              memory_order_release);
        atomic_store_explicit(&state->helper_redirect_depth,
                              shard->opaque_redirect_depth,
                              memory_order_release);
        pthread_mutex_unlock(&shard->lock);
        atomic_store_explicit(&state->helper_leave_rc, llam_leave_blocking(), memory_order_release);
        observe_legacy_worker(&state->helper_after_leave);
    } else {
        atomic_store_explicit(&state->helper_leave_rc, -1, memory_order_release);
        memset(&state->helper_after_leave, 0, sizeof(state->helper_after_leave));
    }
    pthread_mutex_lock(&shard->opaque_lock);
    configured_after = helper_started ? shard->opaque_helper_thread : helper_before;
    pthread_mutex_unlock(&shard->opaque_lock);
    atomic_store_explicit(&state->helper_identity_preserved,
                          helper_started &&
                                  pthread_equal(helper_before, configured_before) != 0 &&
                                  pthread_equal(helper_before, pthread_self()) != 0 &&
                                  pthread_equal(configured_before, configured_after) != 0
                              ? 1U
                              : 0U,
                          memory_order_release);
    if (atomic_load_explicit(&state->helper_enter_rc, memory_order_acquire) != 0 ||
        atomic_load_explicit(&state->helper_leave_rc, memory_order_acquire) != 0) {
        test_fail(&state->failure, "helper-dispatched blocking lifecycle failed");
    }
    atomic_store_explicit(&state->helper_done, 1U, memory_order_release);
}

static void role_root_task(void *arg) {
    role_state_t *state = arg;
    llam_task_t *child;
    uint64_t deadline;

    observe_legacy_worker(&state->root_before);
    if (state->root_before.primary_by_context == 0U ||
        state->root_before.helper_by_context != 0U) {
        test_fail(&state->failure, "root task did not observe the primary scheduler context");
    }
    child = llam_runtime_spawn_ex(state->runtime, helper_role_task, state, NULL, 0U);
    if (child == NULL) {
        test_fail(&state->failure, "root task could not spawn helper observer");
        atomic_store_explicit(&state->helper_copied, 1U, memory_order_release);
        return;
    }
    if (llam_enter_blocking() != 0) {
        test_fail(&state->failure, "primary opaque enter failed");
        return;
    }
    if (g_llam_tls_task->opaque_blocking_depth != 1U) {
        test_fail(&state->failure, "primary opaque enter did not set depth one");
    }
    if (llam_enter_blocking() != 0) {
        test_fail(&state->failure, "nested primary opaque enter failed");
    }
    atomic_store_explicit(&state->root_depth_after_nested_enter,
                          g_llam_tls_task->opaque_blocking_depth,
                          memory_order_release);
    {
        llam_shard_t *shard = g_llam_tls_shard;

        pthread_mutex_lock(&shard->opaque_lock);
        atomic_store_explicit(&state->root_compensation_after_nested_enter,
                              shard->opaque_compensation_depth,
                              memory_order_release);
        pthread_mutex_unlock(&shard->opaque_lock);
    }
    deadline = legacy_deadline();
    if (wait_flag(&state->helper_copied, deadline) != 0) {
        test_fail(&state->failure, "helper observer did not run before the bounded deadline");
    }
    if (wait_flag(&state->helper_done, deadline) != 0) {
        test_fail(&state->failure, "helper observer did not return before the bounded deadline");
    }
    if (llam_leave_blocking() != 0) {
        test_fail(&state->failure, "inner primary opaque leave failed");
    }
    atomic_store_explicit(&state->root_depth_after_inner_leave,
                          g_llam_tls_task->opaque_blocking_depth,
                          memory_order_release);
    if (llam_leave_blocking() != 0) {
        test_fail(&state->failure, "outer primary opaque leave failed");
    }
    atomic_store_explicit(&state->root_depth_after_outer_leave,
                          g_llam_tls_task->opaque_blocking_depth,
                          memory_order_release);
    observe_legacy_worker(&state->root_after);
    if (state->root_after.primary_by_context == 0U ||
        state->root_after.helper_by_context != 0U) {
        test_fail(&state->failure, "primary did not regain its scheduler context after helper return");
    }
    if (llam_join(child) != 0) {
        test_fail(&state->failure, "root could not join helper observer");
    }
}

static int test_primary_helper_takeover(void) {
    role_state_t state;
    llam_runtime_t *runtime = NULL;
    llam_task_t *root = NULL;
    llam_shard_t *shard;
    uint64_t redirect_before;
    bool helper_same_driver;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failure.failures, 0U);
    atomic_init(&state.helper_copied, 0U);
    atomic_init(&state.helper_done, 0U);
    atomic_init(&state.root_depth_after_nested_enter, 0U);
    atomic_init(&state.root_depth_after_inner_leave, 0U);
    atomic_init(&state.root_depth_after_outer_leave, 0U);
    atomic_init(&state.root_compensation_after_nested_enter, 0U);
    atomic_init(&state.helper_enter_rc, 0);
    atomic_init(&state.helper_leave_rc, 0);
    atomic_init(&state.helper_redirect_active, 0U);
    atomic_init(&state.helper_redirect_depth, 0U);
    atomic_init(&state.helper_ready, 0U);
    atomic_init(&state.helper_thread_started, 0U);
    atomic_init(&state.helper_driver_matches, 0U);
    atomic_init(&state.helper_identity_preserved, 0U);
    if (create_deterministic_runtime(&runtime) != 0) {
        return 1;
    }
    state.runtime = runtime;
    shard = &runtime->shards[0];
    redirect_before = atomic_load_explicit(&shard->metrics.opaque_redirect_activations,
                                           memory_order_acquire);
    root = llam_runtime_spawn_ex(runtime, role_root_task, &state, NULL, 0U);
    if (root == NULL || llam_runtime_run_handle(runtime) != 0 || llam_join(root) != 0) {
        destroy_runtime_bounded(&runtime, "role-takeover runtime destroy exceeded the bounded deadline");
        fprintf(stderr,
                "test_worker_identity_internal: role takeover run/join failed: errno=%d (%s)\n",
                errno,
                strerror(errno));
        return 1;
    }
    if (failure_result(&state.failure, "role takeover task failed")) {
        destroy_runtime_bounded(&runtime, "role-takeover runtime destroy exceeded the bounded deadline");
        return 1;
    }
    helper_same_driver = atomic_load_explicit(&state.helper_driver_matches, memory_order_acquire) != 0U;
    if (state.root_before.shard == 0U || state.root_before.scheduler_context == 0U ||
        state.root_before.primary_by_context != 1U || state.root_before.helper_by_context != 0U ||
        state.root_after.shard != state.root_before.shard ||
        state.root_after.scheduler_context != state.root_before.scheduler_context ||
        state.root_after.primary_by_context != 1U || state.root_after.helper_by_context != 0U) {
        destroy_runtime_bounded(&runtime, "role-takeover runtime destroy exceeded the bounded deadline");
        fprintf(stderr,
                "test_worker_identity_internal: root legacy context truth table changed\n");
        return 1;
    }
    if (state.helper.shard != state.root_before.shard ||
        state.helper.scheduler_context == state.root_before.scheduler_context ||
        state.helper.primary_by_context != 0U || state.helper.helper_by_context != 1U ||
        atomic_load_explicit(&state.helper_ready, memory_order_acquire) == 0U || !helper_same_driver) {
        destroy_runtime_bounded(&runtime, "role-takeover runtime destroy exceeded the bounded deadline");
        fprintf(stderr,
                "test_worker_identity_internal: helper legacy context truth table changed\n");
        return 1;
    }
    if (atomic_load_explicit(&state.helper_identity_preserved, memory_order_acquire) == 0U ||
        state.helper_after_enter.shard != state.helper.shard ||
        state.helper_after_enter.scheduler_context != state.helper.scheduler_context ||
        state.helper_after_enter.primary_by_context != 0U ||
        state.helper_after_enter.helper_by_context != 1U ||
        state.helper_after_leave.shard != state.helper.shard ||
        state.helper_after_leave.scheduler_context != state.helper.scheduler_context ||
        state.helper_after_leave.primary_by_context != 0U ||
        state.helper_after_leave.helper_by_context != 1U) {
        destroy_runtime_bounded(&runtime, "role-takeover runtime destroy exceeded the bounded deadline");
        fprintf(stderr,
                "test_worker_identity_internal: helper identity changed during blocking enter/leave\n");
        return 1;
    }
    if (atomic_load_explicit(&state.root_depth_after_nested_enter, memory_order_acquire) != 2U ||
        atomic_load_explicit(&state.root_depth_after_inner_leave, memory_order_acquire) != 1U ||
        atomic_load_explicit(&state.root_depth_after_outer_leave, memory_order_acquire) != 0U ||
        atomic_load_explicit(&state.root_compensation_after_nested_enter, memory_order_acquire) != 1U) {
        destroy_runtime_bounded(&runtime, "role-takeover runtime destroy exceeded the bounded deadline");
        fprintf(stderr,
                "test_worker_identity_internal: nested primary blocking depth changed\n");
        return 1;
    }
    if (atomic_load_explicit(&state.helper_redirect_active, memory_order_acquire) == 0U ||
        atomic_load_explicit(&state.helper_redirect_depth, memory_order_acquire) == 0U ||
        atomic_load_explicit(&shard->metrics.opaque_redirect_activations,
                             memory_order_acquire) <= redirect_before) {
        destroy_runtime_bounded(&runtime, "role-takeover runtime destroy exceeded the bounded deadline");
        fprintf(stderr,
                "test_worker_identity_internal: helper blocking did not activate redirect\n");
        return 1;
    }
        destroy_runtime_bounded(&runtime, "role-takeover runtime destroy exceeded the bounded deadline");
    return 0;
}

static void repeat_observer_task(void *arg) {
    repeat_task_arg_t *task_arg = arg;
    repeat_state_t *state = task_arg->state;
    legacy_worker_observation_t *observation = &state->observations[task_arg->index];

    observe_legacy_worker(observation);
    if (observation->primary_by_context == 0U || observation->helper_by_context != 0U) {
        test_fail(&state->failure, "repeat-run task did not observe primary context");
    }
    if (task_arg->index == 0U) {
        atomic_store_explicit(&state->first_seen, 1U, memory_order_release);
    } else {
        atomic_store_explicit(&state->second_seen, 1U, memory_order_release);
    }
}

static void *first_runtime_driver(void *arg) {
    repeat_state_t *state = arg;
    uint64_t deadline_ns = legacy_deadline();

    state->first_driver = pthread_self();
    state->first_run_rc = llam_runtime_run_handle(state->runtime);
    atomic_store_explicit(&state->first_run_done, 1U, memory_order_release);
    pthread_mutex_lock(&state->lock);
    pthread_cond_broadcast(&state->condition);
    while (state->release_first == 0U) {
        int rc = repeat_cond_timedwait(state, deadline_ns);

        if (rc == ETIMEDOUT) {
            test_fail(&state->failure, "first run driver was not released by the bounded condition wait");
            state->release_first = 1U;
            break;
        }
        if (rc != 0) {
            test_fail(&state->failure, "first run driver condition wait failed");
            state->release_first = 1U;
            break;
        }
    }
    pthread_cond_broadcast(&state->condition);
    pthread_mutex_unlock(&state->lock);
    atomic_store_explicit(&state->first_driver_done, 1U, memory_order_release);
    return NULL;
}

static void *second_runtime_driver(void *arg) {
    repeat_state_t *state = arg;

    state->second_driver = pthread_self();
    state->second_run_rc = llam_runtime_run_handle(state->runtime);
    atomic_store_explicit(&state->second_run_done, 1U, memory_order_release);
    pthread_mutex_lock(&state->lock);
    pthread_cond_broadcast(&state->condition);
    pthread_mutex_unlock(&state->lock);
    atomic_store_explicit(&state->second_driver_done, 1U, memory_order_release);
    return NULL;
}

static int test_repeated_runs_distinct_host_threads(void) {
    repeat_state_t state;
    repeat_task_arg_t first_arg;
    repeat_task_arg_t second_arg;
    llam_runtime_t *runtime = NULL;
    llam_task_t *first_task = NULL;
    llam_task_t *second_task = NULL;
    pthread_t first_thread;
    pthread_t second_thread;
    bool first_created = false;
    bool second_created = false;
    bool mutex_created = false;
    bool condition_created = false;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failure.failures, 0U);
    atomic_init(&state.first_seen, 0U);
    atomic_init(&state.first_run_done, 0U);
    atomic_init(&state.first_driver_done, 0U);
    atomic_init(&state.second_seen, 0U);
    atomic_init(&state.second_run_done, 0U);
    atomic_init(&state.second_driver_done, 0U);
    if (pthread_mutex_init(&state.lock, NULL) != 0) {
        fprintf(stderr, "test_worker_identity_internal: repeat-run mutex setup failed\n");
        return 1;
    }
    mutex_created = true;
    if (repeat_condition_init(&state.condition) != 0) {
        pthread_mutex_destroy(&state.lock);
        fprintf(stderr, "test_worker_identity_internal: repeat-run condition setup failed\n");
        return 1;
    }
    condition_created = true;
    if (create_deterministic_runtime(&runtime) != 0) {
        pthread_cond_destroy(&state.condition);
        pthread_mutex_destroy(&state.lock);
        fprintf(stderr, "test_worker_identity_internal: repeat-run runtime setup failed\n");
        return 1;
    }
    state.runtime = runtime;
    first_arg.state = &state;
    first_arg.index = 0U;
    first_task = llam_runtime_spawn_ex(runtime, repeat_observer_task, &first_arg, NULL, 0U);
    if (first_task == NULL || pthread_create(&first_thread, NULL, first_runtime_driver, &state) != 0) {
        test_fail(&state.failure, "first repeat-run driver could not start");
        goto cleanup;
    }
    first_created = true;
    if (wait_flag(&state.first_run_done, legacy_deadline()) != 0 ||
        wait_flag(&state.first_seen, legacy_deadline()) != 0 ||
        state.first_run_rc != 0) {
        test_fail(&state.failure, "first repeat run did not complete its observation");
        goto cleanup;
    }
    if (first_task != NULL && llam_join(first_task) != 0) {
        test_fail(&state.failure, "first repeat-run task join failed");
        goto cleanup;
    }

    second_arg.state = &state;
    second_arg.index = 1U;
    second_task = llam_runtime_spawn_ex(runtime, repeat_observer_task, &second_arg, NULL, 0U);
    if (second_task == NULL || pthread_create(&second_thread, NULL, second_runtime_driver, &state) != 0) {
        test_fail(&state.failure, "second repeat-run driver could not start");
        goto cleanup;
    }
    second_created = true;
    if (wait_flag(&state.second_seen, legacy_deadline()) != 0 ||
        wait_flag(&state.second_run_done, legacy_deadline()) != 0 ||
        state.second_run_rc != 0) {
        test_fail(&state.failure, "second repeat run did not complete its observation");
        goto cleanup;
    }
    pthread_mutex_lock(&state.lock);
    state.release_first = 1U;
    pthread_cond_broadcast(&state.condition);
    pthread_mutex_unlock(&state.lock);
    if (second_task != NULL && llam_join(second_task) != 0) {
        test_fail(&state.failure, "second repeat-run task join failed");
        goto cleanup;
    }
    if (state.observations[0].shard != state.observations[1].shard ||
        state.observations[0].scheduler_context != state.observations[1].scheduler_context ||
        pthread_equal(state.first_driver, state.second_driver) != 0) {
        test_fail(&state.failure, "repeated runs changed legacy shard/context or recycled the host thread");
        goto cleanup;
    }
    rc = failure_result(&state.failure, "repeat-run characterization failed");

cleanup:
    if (mutex_created) {
        pthread_mutex_lock(&state.lock);
        state.release_first = 1U;
        pthread_cond_broadcast(&state.condition);
        pthread_mutex_unlock(&state.lock);
    }
    if (first_created) {
        require_flag_before_cleanup(&state.first_run_done,
                                    "first run driver did not stop during bounded cleanup");
    }
    if (second_created) {
        require_flag_before_cleanup(&state.second_run_done,
                                    "second run driver did not stop during bounded cleanup");
    }
    if (runtime != NULL) {
        destroy_runtime_bounded(&runtime, "repeat-run runtime destroy exceeded the bounded deadline");
        runtime = NULL;
    }
    if (second_created) {
        require_flag_before_cleanup(&state.second_driver_done,
                                    "second run thread did not finish during bounded cleanup");
        pthread_join(second_thread, NULL);
    }
    if (first_created) {
        require_flag_before_cleanup(&state.first_driver_done,
                                    "first run thread did not finish during bounded cleanup");
        pthread_join(first_thread, NULL);
    }
    if (state.failure.first_failure != NULL && rc == 1) {
        (void)failure_result(&state.failure, "repeat-run characterization failed");
    }
    if (condition_created) {
        pthread_cond_destroy(&state.condition);
    }
    if (mutex_created) {
        pthread_mutex_destroy(&state.lock);
    }
    return rc;
}

static void idle_helper_task(void *arg) {
    test_failure_state_t *failure = arg;

    if (llam_enter_blocking() != 0 || llam_leave_blocking() != 0) {
        test_fail(failure, "idle-helper blocking lifecycle failed");
    }
}

static int test_idle_helper_shutdown(void) {
    test_failure_state_t failure;
    llam_runtime_t *runtime = NULL;
    llam_task_t *task = NULL;
    llam_shard_t *shard;
    bool helper_started;
    bool helper_ready;
    bool helper_active;

    memset(&failure, 0, sizeof(failure));
    atomic_init(&failure.failures, 0U);
    if (create_deterministic_runtime(&runtime) != 0) {
        return 1;
    }
    task = llam_runtime_spawn_ex(runtime, idle_helper_task, &failure, NULL, 0U);
    if (task == NULL || llam_runtime_run_handle(runtime) != 0 || llam_join(task) != 0) {
        destroy_runtime_bounded(&runtime, "idle-helper runtime destroy exceeded the bounded deadline");
        fprintf(stderr,
                "test_worker_identity_internal: idle-helper run/join failed: errno=%d (%s)\n",
                errno,
                strerror(errno));
        return 1;
    }
    shard = &runtime->shards[0];
    pthread_mutex_lock(&shard->opaque_lock);
    helper_started = shard->opaque_helper_thread_started;
    helper_ready = shard->opaque_helper_ready;
    helper_active = shard->opaque_helper_active;
    pthread_mutex_unlock(&shard->opaque_lock);
    if (failure_result(&failure, "idle-helper task failed")) {
        destroy_runtime_bounded(&runtime, "idle-helper runtime destroy exceeded the bounded deadline");
        return 1;
    }
    if (!helper_started || !helper_ready || helper_active) {
        destroy_runtime_bounded(&runtime, "idle-helper runtime destroy exceeded the bounded deadline");
        fprintf(stderr,
                "test_worker_identity_internal: helper was not idle and ready before destruction\n");
        return 1;
    }
    destroy_runtime_bounded(&runtime, "idle-helper runtime destroy exceeded the bounded deadline");
    return 0;
}

static void active_shutdown_task(void *arg) {
    active_shutdown_state_t *state = arg;
    llam_shard_t *shard;
    uint64_t deadline;

    if (llam_enter_blocking() != 0) {
        test_fail(&state->failure, "active-helper primary enter failed");
        atomic_store_explicit(&state->primary_entered, 1U, memory_order_release);
        return;
    }
    shard = g_llam_tls_shard;
    pthread_mutex_lock(&shard->opaque_lock);
    atomic_store_explicit(&state->helper_started,
                          shard->opaque_helper_thread_started ? 1U : 0U,
                          memory_order_release);
    atomic_store_explicit(&state->helper_ready,
                          shard->opaque_helper_ready ? 1U : 0U,
                          memory_order_release);
    atomic_store_explicit(&state->helper_active,
                          shard->opaque_helper_active ? 1U : 0U,
                          memory_order_release);
    pthread_mutex_unlock(&shard->opaque_lock);
    atomic_store_explicit(&state->helper_state_observed, 1U, memory_order_release);
    atomic_store_explicit(&state->primary_entered, 1U, memory_order_release);
    deadline = legacy_deadline();
    if (wait_flag(&state->release_primary, deadline) != 0) {
        test_fail(&state->failure, "active-helper primary was not released before its bounded deadline");
    }
    if (llam_leave_blocking() != 0) {
        test_fail(&state->failure, "active-helper primary leave failed");
    }
    atomic_store_explicit(&state->task_done, 1U, memory_order_release);
}

static void *active_shutdown_release_thread(void *arg) {
    active_shutdown_state_t *state = arg;
    uint64_t deadline = legacy_deadline();

    if (wait_flag(&state->destroy_started, deadline) != 0) {
        test_fail(&state->failure, "release thread did not observe destroy publication");
    }
    atomic_store_explicit(&state->release_primary, 1U, memory_order_release);
    atomic_store_explicit(&state->release_thread_done, 1U, memory_order_release);
    return NULL;
}

static void *active_shutdown_run_thread(void *arg) {
    active_shutdown_state_t *state = arg;
    int run_rc;

    run_rc = llam_runtime_run_handle(state->runtime);
    atomic_store_explicit(&state->run_rc, run_rc, memory_order_release);
    atomic_store_explicit(&state->run_done, 1U, memory_order_release);
    return NULL;
}

static void *active_shutdown_destroy_thread(void *arg) {
    active_shutdown_state_t *state = arg;

    atomic_store_explicit(&state->destroy_started, 1U, memory_order_release);
    llam_runtime_destroy(state->runtime);
    atomic_store_explicit(&state->destroy_done, 1U, memory_order_release);
    return NULL;
}

static int test_active_helper_shutdown(void) {
    active_shutdown_state_t state;
    llam_runtime_t *runtime = NULL;
    llam_task_t *task = NULL;
    pthread_t run_thread;
    pthread_t release_thread;
    pthread_t destroy_thread;
    bool run_created = false;
    bool release_created = false;
    bool destroy_created = false;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failure.failures, 0U);
    atomic_init(&state.primary_entered, 0U);
    atomic_init(&state.helper_state_observed, 0U);
    atomic_init(&state.helper_started, 0U);
    atomic_init(&state.helper_ready, 0U);
    atomic_init(&state.helper_active, 0U);
    atomic_init(&state.release_primary, 0U);
    atomic_init(&state.destroy_started, 0U);
    atomic_init(&state.task_done, 0U);
    atomic_init(&state.release_thread_done, 0U);
    atomic_init(&state.destroy_done, 0U);
    atomic_init(&state.run_rc, 0);
    atomic_init(&state.run_done, 0U);
    if (create_deterministic_runtime(&runtime) != 0) {
        return 1;
    }
    state.runtime = runtime;
    task = llam_runtime_spawn_ex(runtime, active_shutdown_task, &state, NULL, 0U);
    if (task == NULL || pthread_create(&run_thread, NULL, active_shutdown_run_thread, &state) != 0) {
        test_fail(&state.failure, "active-helper run thread could not start");
        goto cleanup;
    }
    run_created = true;
    if (pthread_create(&release_thread, NULL, active_shutdown_release_thread, &state) != 0) {
        test_fail(&state.failure, "active-helper release thread could not start");
        goto cleanup;
    }
    release_created = true;
    if (wait_flag(&state.primary_entered, legacy_deadline()) != 0 ||
        wait_flag(&state.helper_state_observed, legacy_deadline()) != 0 ||
        atomic_load_explicit(&state.helper_started, memory_order_acquire) == 0U ||
        atomic_load_explicit(&state.helper_ready, memory_order_acquire) == 0U ||
        atomic_load_explicit(&state.helper_active, memory_order_acquire) == 0U) {
        test_fail(&state.failure, "active-helper state was not ready and active before destroy publication");
    }
    if (pthread_create(&destroy_thread, NULL, active_shutdown_destroy_thread, &state) != 0) {
        test_fail(&state.failure, "active-helper destroy thread could not start");
        goto cleanup;
    }
    destroy_created = true;
    if (wait_flag(&state.destroy_started, legacy_deadline()) != 0 ||
        wait_flag(&state.destroy_done, legacy_deadline()) != 0) {
        test_fail(&state.failure, "active-helper destroy did not complete before the bounded deadline");
        goto cleanup;
    }
    runtime = NULL;
    if (wait_flag(&state.run_done, legacy_deadline()) != 0 ||
        (atomic_load_explicit(&state.run_done, memory_order_acquire) != 0U &&
         atomic_load_explicit(&state.run_rc, memory_order_acquire) != 0) ||
        wait_flag(&state.task_done, legacy_deadline()) != 0 ||
        wait_flag(&state.release_thread_done, legacy_deadline()) != 0 ||
        atomic_load_explicit(&state.run_done, memory_order_acquire) == 0U) {
        test_fail(&state.failure, "active-helper shutdown did not drain the released primary");
    }
    if (failure_result(&state.failure, "active-helper shutdown characterization failed")) {
        goto cleanup;
    }
    if (run_created) {
        pthread_join(run_thread, NULL);
        run_created = false;
    }
    if (release_created) {
        pthread_join(release_thread, NULL);
        release_created = false;
    }
    if (destroy_created) {
        pthread_join(destroy_thread, NULL);
        destroy_created = false;
    }
    if (task == NULL) {
        return 1;
    }
    return 0;

cleanup:
    atomic_store_explicit(&state.destroy_started, 1U, memory_order_release);
    atomic_store_explicit(&state.release_primary, 1U, memory_order_release);
    if (destroy_created) {
        require_flag_before_cleanup(&state.destroy_done,
                                    "active-helper destroy thread did not finish during bounded cleanup");
        runtime = NULL;
    }
    if (runtime != NULL) {
        if (run_created) {
            require_flag_before_cleanup(&state.run_done,
                                        "active-helper run thread did not stop during bounded cleanup");
        }
        destroy_runtime_bounded(&runtime, "active-helper runtime destroy exceeded the bounded deadline");
        runtime = NULL;
    }
    if (run_created) {
        require_flag_before_cleanup(&state.run_done,
                                    "active-helper run thread did not finish during bounded cleanup");
        pthread_join(run_thread, NULL);
    }
    if (release_created) {
        require_flag_before_cleanup(&state.release_thread_done,
                                    "active-helper release thread did not finish during bounded cleanup");
        pthread_join(release_thread, NULL);
    }
    if (destroy_created) {
        pthread_join(destroy_thread, NULL);
    }
    return failure_result(&state.failure, "active-helper shutdown characterization failed");
}

int main(void) {
    if (test_helper_create_failure_redirect() != 0 ||
        test_primary_helper_takeover() != 0 ||
        test_repeated_runs_distinct_host_threads() != 0 ||
        test_idle_helper_shutdown() != 0 ||
        test_active_helper_shutdown() != 0) {
        return 1;
    }
    printf("test_worker_identity_internal: all legacy checks passed\n");
    return 0;
}
