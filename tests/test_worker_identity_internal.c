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
 * See LICENSE.
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
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#include <windows.h>
#endif

#define LEGACY_TEST_WAIT_NS (3ULL * 1000ULL * 1000ULL * 1000ULL)

typedef struct legacy_worker_observation {
    uintptr_t shard;
    uintptr_t scheduler_context;
    unsigned primary_by_context;
    unsigned helper_by_context;
    unsigned primary_predicate;
    unsigned helper_predicate;
    unsigned direct_predicate;
} legacy_worker_observation_t;

typedef struct worker_identity_observation {
    uintptr_t shard;
    uintptr_t worker;
    uintptr_t processor;
    uint64_t incarnation;
    unsigned role;
} worker_identity_observation_t;

#if !LLAM_PLATFORM_WINDOWS
typedef struct worker_signal_observation {
    uintptr_t worker_stack_base;
    size_t worker_stack_size;
    uintptr_t installed_stack_base;
    size_t installed_stack_size;
    uint64_t worker_incarnation;
    uint64_t installed_incarnation;
    unsigned installed;
    int query_errno;
} worker_signal_observation_t;
#endif

typedef struct test_failure_state {
    atomic_uint failures;
    const char *first_failure;
} test_failure_state_t;

typedef struct helper_failure_state {
    test_failure_state_t failure;
    llam_runtime_t *runtime;
    int enter_rc;
    int enter_errno;
    int leave_rc;
    atomic_int run_rc;
    atomic_int run_errno;
    atomic_uint run_done;
    atomic_uint task_done;
} helper_failure_state_t;

typedef struct role_state role_state_t;

struct role_state {
    test_failure_state_t failure;
    llam_runtime_t *runtime;
    legacy_worker_observation_t root_before;
    legacy_worker_observation_t root_after;
    worker_identity_observation_t root_identity_before;
    worker_identity_observation_t root_identity_after;
    legacy_worker_observation_t helper;
    legacy_worker_observation_t helper_after_enter;
    legacy_worker_observation_t helper_after_leave;
    worker_identity_observation_t helper_identity;
    worker_identity_observation_t helper_identity_after_enter;
    worker_identity_observation_t helper_identity_after_leave;
#if !LLAM_PLATFORM_WINDOWS
    worker_signal_observation_t root_signal_before;
    worker_signal_observation_t root_signal_after;
    worker_signal_observation_t helper_signal;
#endif
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
    worker_identity_observation_t identity_observations[2];
    atomic_uint first_seen;
    atomic_uint first_run_done;
    atomic_uint first_driver_done;
    atomic_uint second_seen;
    atomic_uint second_run_done;
    atomic_uint second_driver_done;
    unsigned first_exit_tls_clear;
    unsigned second_exit_tls_clear;
    pthread_mutex_t lock;
    pthread_cond_t condition;
    unsigned release_first;
    int first_run_rc;
    int second_run_rc;
    pthread_t first_driver;
    pthread_t second_driver;
} repeat_state_t;

typedef struct managed_tls_tuple {
    llam_shard_t *shard;
    llam_worker_t *worker;
    llam_processor_t *processor;
    llam_task_t *task;
    llam_ctx_t *scheduler_ctx;
} managed_tls_tuple_t;

typedef struct nested_runtime_identity_state {
    test_failure_state_t failure;
    llam_runtime_t *nested_runtime;
    managed_tls_tuple_t before_create;
    managed_tls_tuple_t after_create;
    managed_tls_tuple_t after_yield;
} nested_runtime_identity_state_t;

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
    atomic_uint destroy_done;
    atomic_int run_rc;
    atomic_uint run_done;
    int leave_rc;
    int leave_errno;
} active_shutdown_state_t;

static int create_deterministic_runtime(llam_runtime_t **out) {
    llam_runtime_opts_t opts;

    if (llam_runtime_opts_init(&opts, sizeof(opts)) != 0) {
        return -1;
    }
    opts.deterministic = 1U;
    opts.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    return llam_runtime_create_compat(&opts, sizeof(opts), out);
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
    out->primary_predicate = llam_tls_is_primary_scheduler_for(shard);
    out->helper_predicate = llam_tls_is_opaque_helper_for(shard);
    out->direct_predicate = llam_tls_has_legacy_direct_handoff_context(shard);
}

static void observe_worker_identity(worker_identity_observation_t *out) {
    llam_worker_t *worker = g_llam_tls_worker;

    memset(out, 0, sizeof(*out));
    out->shard = (uintptr_t)g_llam_tls_shard;
    out->worker = (uintptr_t)worker;
    out->processor = (uintptr_t)g_llam_tls_processor;
    if (worker == NULL || g_llam_tls_processor == NULL) {
        return;
    }
    out->incarnation =
        atomic_load_explicit(&worker->incarnation, memory_order_acquire);
    out->role = (unsigned)worker->role;
}

#if !LLAM_PLATFORM_WINDOWS
static void observe_current_signal_stack(worker_signal_observation_t *out) {
    stack_t active;
    llam_worker_t *worker = g_llam_tls_worker;

    memset(out, 0, sizeof(*out));
    if (worker == NULL) {
        out->query_errno = EINVAL;
        return;
    }
    if (sigaltstack(NULL, &active) != 0) {
        out->query_errno = errno;
        return;
    }
    out->worker_stack_base = (uintptr_t)worker->signal_stack;
    out->worker_stack_size = worker->signal_stack_size;
    out->installed_stack_base = (uintptr_t)active.ss_sp;
    out->installed_stack_size = active.ss_size;
    out->worker_incarnation = atomic_load_explicit(
        &worker->incarnation, memory_order_acquire);
    out->installed_incarnation = worker->signal_stack_incarnation;
    out->installed = worker->sigaltstack_installed ? 1U : 0U;
}

static unsigned signal_range_contains(uintptr_t outer_base,
                                      size_t outer_size,
                                      uintptr_t inner_base,
                                      size_t inner_size) {
    if (outer_base > UINTPTR_MAX - outer_size ||
        inner_base > UINTPTR_MAX - inner_size) {
        return 0U;
    }
    return inner_base >= outer_base &&
                   inner_base + inner_size <= outer_base + outer_size
               ? 1U
               : 0U;
}

static unsigned signal_ranges_disjoint(const worker_signal_observation_t *left,
                                       const worker_signal_observation_t *right) {
    uintptr_t left_end;
    uintptr_t right_end;

    if (left == NULL || right == NULL ||
        left->worker_stack_base > UINTPTR_MAX - left->worker_stack_size ||
        right->worker_stack_base > UINTPTR_MAX - right->worker_stack_size) {
        return 0U;
    }
    left_end = left->worker_stack_base + left->worker_stack_size;
    right_end = right->worker_stack_base + right->worker_stack_size;
    return left_end <= right->worker_stack_base || right_end <= left->worker_stack_base
               ? 1U
               : 0U;
}

static unsigned signal_observation_valid(const worker_signal_observation_t *obs) {
    return obs != NULL && obs->query_errno == 0 && obs->installed != 0U &&
                   obs->worker_stack_base != 0U && obs->worker_stack_size >= (size_t)SIGSTKSZ &&
                   obs->installed_stack_base == obs->worker_stack_base &&
                   obs->installed_stack_size == obs->worker_stack_size &&
                   obs->worker_incarnation != 0U &&
                   obs->installed_incarnation == obs->worker_incarnation
               ? 1U
               : 0U;
}
#endif

static unsigned worker_identity_tuple_is_clear(void) {
    return g_llam_tls_shard == NULL &&
                   g_llam_tls_worker == NULL &&
                   g_llam_tls_processor == NULL &&
                   g_llam_tls_task == NULL &&
                   g_llam_tls_scheduler_ctx == NULL
               ? 1U
               : 0U;
}

static managed_tls_tuple_t capture_managed_tls_tuple(void) {
    managed_tls_tuple_t tuple;

    tuple.shard = g_llam_tls_shard;
    tuple.worker = g_llam_tls_worker;
    tuple.processor = g_llam_tls_processor;
    tuple.task = g_llam_tls_task;
    tuple.scheduler_ctx = g_llam_tls_scheduler_ctx;
    return tuple;
}

static unsigned managed_tls_tuple_complete(const managed_tls_tuple_t *tuple) {
    return tuple != NULL &&
                   tuple->shard != NULL &&
                   tuple->worker != NULL &&
                   tuple->processor != NULL &&
                   tuple->task != NULL &&
                   tuple->scheduler_ctx != NULL
               ? 1U
               : 0U;
}

static unsigned managed_tls_tuples_match(const managed_tls_tuple_t *expected,
                                         const managed_tls_tuple_t *actual) {
    return expected != NULL && actual != NULL &&
                   expected->shard == actual->shard &&
                   expected->worker == actual->worker &&
                   expected->processor == actual->processor &&
                   expected->task == actual->task &&
                   expected->scheduler_ctx == actual->scheduler_ctx
               ? 1U
               : 0U;
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

static int wait_for_lease_gate_reached(llam_lease_test_point_t point) {
    uint64_t deadline = legacy_deadline();

    while (!llam_lease_test_gate_reached(point)) {
        if (llam_now_ns() >= deadline) {
            return -1;
        }
        llam_pause_cpu();
    }
    return 0;
}

static bool active_shutdown_snapshot_matches(llam_runtime_t *runtime,
                                             unsigned expected_state,
                                             bool shutdown_expected,
                                             bool helper_active_expected,
                                             llam_lease_test_snapshot_t *out) {
    llam_shard_t *shard;
    llam_lease_test_snapshot_t snapshot;
    bool matches;

    if (runtime == NULL || runtime->shards == NULL || runtime->active_shards == 0U) {
        return false;
    }
    shard = &runtime->shards[0];
    pthread_mutex_lock(&shard->opaque_lock);
    pthread_mutex_lock(&shard->lock);
    matches = llam_legacy_lease_snapshot_locked(shard, &snapshot) &&
              snapshot.state == expected_state &&
              snapshot.owner_worker == (uintptr_t)&shard->primary_worker &&
              snapshot.owner_incarnation != 0U &&
              snapshot.owner_incarnation ==
                  atomic_load_explicit(&shard->primary_worker.incarnation,
                                       memory_order_acquire) &&
              snapshot.return_worker == 0U &&
              snapshot.return_incarnation == 0U &&
              snapshot.transfer_sequence != 0U &&
              snapshot.handback_ack_sequence == snapshot.transfer_sequence &&
              (shutdown_expected
                   ? atomic_load_explicit(&runtime->shutdown_requested,
                                          memory_order_acquire)
                   : !atomic_load_explicit(&runtime->shutdown_requested,
                                           memory_order_acquire)) &&
              shard->opaque_helper_active == helper_active_expected &&
              !atomic_load_explicit(&shard->opaque_helper_exit_ack,
                                    memory_order_acquire) &&
              llam_worker_binding_live(&shard->primary_worker,
                                       snapshot.owner_incarnation) &&
              llam_worker_load_active_g(&shard->primary_worker) == NULL &&
              llam_worker_load_active_g(&shard->opaque_helper_worker) == NULL &&
              llam_shard_load_lease_current(shard) == NULL;
    pthread_mutex_unlock(&shard->lock);
    pthread_mutex_unlock(&shard->opaque_lock);
    if (matches && out != NULL) {
        *out = snapshot;
    }
    return matches;
}

static int wait_for_active_shutdown_snapshot(llam_runtime_t *runtime,
                                             unsigned expected_state,
                                             bool shutdown_expected,
                                             bool helper_active_expected,
                                             llam_lease_test_snapshot_t *out) {
    uint64_t deadline = legacy_deadline();

    while (llam_now_ns() < deadline) {
        if (active_shutdown_snapshot_matches(runtime,
                                              expected_state,
                                              shutdown_expected,
                                              helper_active_expected,
                                              out)) {
            return 0;
        }
        llam_pause_cpu();
    }
    return -1;
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

static void verify_worker_role_observation(test_failure_state_t *failure,
                                           const legacy_worker_observation_t *obs) {
    if (failure == NULL || obs == NULL) {
        return;
    }
    if (obs->primary_predicate != obs->primary_by_context) {
        test_fail(failure, "primary predicate changed the legacy truth value");
    }
    if (obs->helper_predicate != obs->helper_by_context) {
        test_fail(failure, "helper predicate changed the legacy truth value");
    }
    if (obs->direct_predicate != obs->primary_by_context) {
        test_fail(failure, "direct-handoff predicate changed the legacy truth value");
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
    atomic_store_explicit(&state->task_done, 1U, memory_order_release);
}

#if !LLAM_PLATFORM_WINDOWS
static void role_root_task(void *arg);
#endif

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
    atomic_init(&state.task_done, 0U);
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
    atomic_init(&state.task_done, 0U);
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

#if LLAM_PLATFORM_POSIX
typedef struct wrong_owner_uninstall_state {
    test_failure_state_t failure;
} wrong_owner_uninstall_state_t;

static void wrong_owner_uninstall_task(void *arg) {
    wrong_owner_uninstall_state_t *state = arg;
    llam_shard_t *shard = g_llam_tls_shard;
    pid_t child;
    int status;
    pid_t waited;

    if (shard == NULL || g_llam_tls_worker != &shard->primary_worker) {
        test_fail(&state->failure,
                  "wrong-owner uninstall task did not run on the primary worker");
        return;
    }

    child = fork();
    if (child < 0) {
        test_fail(&state->failure, "wrong-owner uninstall fork failed");
        return;
    }
    if (child == 0) {
        /* The child inherits primary TLS; the helper worker is not current. */
        llam_uninstall_thread_signal_stack(&shard->opaque_helper_worker);
        _exit(0);
    }

    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        test_fail(&state->failure, "wrong-owner uninstall waitpid failed");
    } else if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT) {
        test_fail(&state->failure,
                  "wrong-owner uninstall did not terminate the child with SIGABRT");
    }
}

static int test_wrong_owner_uninstall_aborts(void) {
    wrong_owner_uninstall_state_t state;
    llam_runtime_t *runtime = NULL;
    llam_task_t *task = NULL;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failure.failures, 0U);
    if (create_deterministic_runtime(&runtime) != 0) {
        fprintf(stderr,
                "test_worker_identity_internal: wrong-owner runtime create failed: errno=%d (%s)\n",
                errno,
                strerror(errno));
        return 1;
    }
    task = llam_runtime_spawn_ex(runtime,
                                 wrong_owner_uninstall_task,
                                 &state,
                                 NULL,
                                 0U);
    if (task == NULL || llam_runtime_run_handle(runtime) != 0 || llam_join(task) != 0) {
        destroy_runtime_bounded(&runtime,
                                "wrong-owner runtime destroy exceeded the bounded deadline");
        fprintf(stderr,
                "test_worker_identity_internal: wrong-owner run/join failed: errno=%d (%s)\n",
                errno,
                strerror(errno));
        return 1;
    }
    if (failure_result(&state.failure, "wrong-owner uninstall regression failed")) {
        destroy_runtime_bounded(&runtime,
                                "wrong-owner runtime destroy exceeded the bounded deadline");
        return 1;
    }
    destroy_runtime_bounded(&runtime,
                            "wrong-owner runtime destroy exceeded the bounded deadline");
    return 0;
}
#endif

#if !LLAM_PLATFORM_WINDOWS
static void *helper_signal_failure_run_main(void *arg) {
    helper_failure_state_t *state = arg;
    int saved_errno;
    int rc;

    errno = 0;
    rc = llam_runtime_run_handle(state->runtime);
    saved_errno = errno;
    atomic_store_explicit(&state->run_rc, rc, memory_order_release);
    atomic_store_explicit(&state->run_errno, saved_errno, memory_order_release);
    atomic_store_explicit(&state->run_done, 1U, memory_order_release);
    return NULL;
}

static void init_helper_failure_state(helper_failure_state_t *state) {
    memset(state, 0, sizeof(*state));
    atomic_init(&state->failure.failures, 0U);
    atomic_init(&state->run_rc, 0);
    atomic_init(&state->run_errno, 0);
    atomic_init(&state->run_done, 0U);
    atomic_init(&state->task_done, 0U);
}

static void init_role_state(role_state_t *state) {
    memset(state, 0, sizeof(*state));
    atomic_init(&state->failure.failures, 0U);
    atomic_init(&state->helper_copied, 0U);
    atomic_init(&state->helper_done, 0U);
    atomic_init(&state->root_depth_after_nested_enter, 0U);
    atomic_init(&state->root_depth_after_inner_leave, 0U);
    atomic_init(&state->root_depth_after_outer_leave, 0U);
    atomic_init(&state->root_compensation_after_nested_enter, 0U);
    atomic_init(&state->helper_enter_rc, 0);
    atomic_init(&state->helper_leave_rc, 0);
    atomic_init(&state->helper_redirect_active, 0U);
    atomic_init(&state->helper_redirect_depth, 0U);
    atomic_init(&state->helper_ready, 0U);
    atomic_init(&state->helper_thread_started, 0U);
    atomic_init(&state->helper_driver_matches, 0U);
    atomic_init(&state->helper_identity_preserved, 0U);
}

static int test_helper_signal_stack_eagain(void) {
    role_state_t state;
    llam_runtime_t *runtime = NULL;
    llam_task_t *root = NULL;
    llam_shard_t *shard;

    init_role_state(&state);
    if (create_deterministic_runtime(&runtime) != 0) {
        return 1;
    }
    state.runtime = runtime;
    shard = &runtime->shards[0];
    llam_worker_test_force_sigaltstack_error(LLAM_WORKER_ROLE_OPAQUE_HELPER, EAGAIN);
    root = llam_runtime_spawn_ex(runtime, role_root_task, &state, NULL, 0U);
    if (root == NULL || llam_runtime_run_handle(runtime) != 0 || llam_join(root) != 0) {
        destroy_runtime_bounded(&runtime, "helper-sigaltstack EAGAIN runtime destroy exceeded the bounded deadline");
        fprintf(stderr,
                "test_worker_identity_internal: helper sigaltstack EAGAIN run/join failed: errno=%d (%s)\n",
                errno,
                strerror(errno));
        return 1;
    }
    if (failure_result(&state.failure, "helper sigaltstack EAGAIN task failed")) {
        destroy_runtime_bounded(&runtime, "helper-sigaltstack EAGAIN runtime destroy exceeded the bounded deadline");
        return 1;
    }
    pthread_mutex_lock(&shard->opaque_lock);
    if (!shard->opaque_helper_thread_started ||
        !shard->opaque_helper_ready || shard->opaque_helper_failed) {
        pthread_mutex_unlock(&shard->opaque_lock);
        destroy_runtime_bounded(&runtime, "helper-sigaltstack EAGAIN runtime destroy exceeded the bounded deadline");
        fprintf(stderr,
                "test_worker_identity_internal: helper sigaltstack EAGAIN did not leave helper ready\n");
        return 1;
    }
    pthread_mutex_unlock(&shard->opaque_lock);
    if (atomic_load_explicit(&runtime->fatal_errno, memory_order_acquire) != 0 ||
        state.root_signal_before.query_errno != 0 ||
        state.root_signal_before.installed == 0U ||
        state.helper_signal.query_errno != 0 ||
        state.helper_signal.worker_stack_base == 0U ||
        state.helper_signal.worker_stack_size < (size_t)SIGSTKSZ ||
        state.helper_signal.installed != 0U ||
        state.helper_signal.installed_incarnation != 0U) {
        destroy_runtime_bounded(&runtime, "helper-sigaltstack EAGAIN runtime destroy exceeded the bounded deadline");
        fprintf(stderr,
                "test_worker_identity_internal: helper sigaltstack EAGAIN changed fatal or installed state\n");
        return 1;
    }
    destroy_runtime_bounded(&runtime, "helper-sigaltstack EAGAIN runtime destroy exceeded the bounded deadline");
    return 0;
}

static int test_helper_signal_stack_einval(void) {
    helper_failure_state_t state;
    llam_runtime_t *runtime = NULL;
    llam_task_t *task = NULL;
    llam_shard_t *shard;
    pthread_t run_thread;
    bool run_created = false;
    int run_rc;
    int run_errno;

    init_helper_failure_state(&state);
    if (create_deterministic_runtime(&runtime) != 0) {
        return 1;
    }
    state.runtime = runtime;
    shard = &runtime->shards[0];
    llam_worker_test_force_sigaltstack_error(LLAM_WORKER_ROLE_OPAQUE_HELPER, EINVAL);
    /* A later helper write cannot repair a run result already returned to the
     * host. Hold the helper after publishing failure until that result exists. */
    llam_lease_test_set_gate(LLAM_LEASE_TEST_AFTER_HELPER_START_FAILURE, true);
    task = llam_runtime_spawn_ex(runtime, helper_failure_task, &state, NULL, 0U);
    if (task == NULL || pthread_create(&run_thread, NULL, helper_signal_failure_run_main, &state) != 0) {
        llam_lease_test_set_gate(LLAM_LEASE_TEST_AFTER_HELPER_START_FAILURE, false);
        destroy_runtime_bounded(&runtime, "helper-sigaltstack EINVAL runtime destroy exceeded the bounded deadline");
        fprintf(stderr,
                "test_worker_identity_internal: helper sigaltstack EINVAL setup failed: errno=%d (%s)\n",
                errno,
                strerror(errno));
        return 1;
    }
    run_created = true;
    if (wait_for_lease_gate_reached(LLAM_LEASE_TEST_AFTER_HELPER_START_FAILURE) != 0) {
        test_fail(&state.failure, "helper start failure was not published");
    }
    if (wait_flag(&state.run_done, legacy_deadline()) != 0) {
        test_fail(&state.failure, "helper sigaltstack EINVAL did not wake the primary run");
    }
    llam_lease_test_set_gate(LLAM_LEASE_TEST_AFTER_HELPER_START_FAILURE, false);
    if (run_created) {
        pthread_join(run_thread, NULL);
        run_created = false;
    }
    run_rc = atomic_load_explicit(&state.run_rc, memory_order_acquire);
    run_errno = atomic_load_explicit(&state.run_errno, memory_order_acquire);
    if (task != NULL) {
        errno = 0;
        if (llam_join(task) == 0 || errno != EINVAL ||
            atomic_load_explicit(&state.task_done, memory_order_acquire) == 0U) {
            test_fail(&state.failure, "helper sigaltstack EINVAL primary task did not complete");
        }
    }
    pthread_mutex_lock(&shard->opaque_lock);
    if (!shard->opaque_helper_thread_started || shard->opaque_helper_ready ||
        !shard->opaque_helper_failed) {
        test_fail(&state.failure, "helper sigaltstack EINVAL did not publish failed/not-ready");
    }
    pthread_mutex_unlock(&shard->opaque_lock);
    if (run_rc != -1 || run_errno != EINVAL ||
        atomic_load_explicit(&runtime->fatal_errno, memory_order_acquire) != EINVAL ||
        state.enter_rc != 0 || state.leave_rc != 0) {
        fprintf(stderr, "helper stack failure: run=%d errno=%d fatal=%d enter=%d leave=%d\n",
                run_rc, run_errno, atomic_load_explicit(&runtime->fatal_errno, memory_order_acquire),
                state.enter_rc, state.leave_rc);
        test_fail(&state.failure, "helper sigaltstack EINVAL did not preserve fatal runtime semantics");
    }
    if (failure_result(&state.failure, "helper sigaltstack EINVAL failure semantics changed")) {
        destroy_runtime_bounded(&runtime, "helper-sigaltstack EINVAL runtime destroy exceeded the bounded deadline");
        return 1;
    }
    destroy_runtime_bounded(&runtime, "helper-sigaltstack EINVAL runtime destroy exceeded the bounded deadline");
    return 0;
}
#endif

static void helper_role_task(void *arg) {
    role_state_t *state = arg;
    llam_shard_t *shard;
    pthread_t helper_before;
    pthread_t configured_before;
    pthread_t configured_after;
    bool helper_started;

    helper_before = pthread_self();
    observe_legacy_worker(&state->helper);
    verify_worker_role_observation(&state->failure, &state->helper);
    observe_worker_identity(&state->helper_identity);
#if !LLAM_PLATFORM_WINDOWS
    observe_current_signal_stack(&state->helper_signal);
#endif
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
    verify_worker_role_observation(&state->failure, &state->helper_after_enter);
    observe_worker_identity(&state->helper_identity_after_enter);
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
        verify_worker_role_observation(&state->failure, &state->helper_after_leave);
        observe_worker_identity(&state->helper_identity_after_leave);
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
    verify_worker_role_observation(&state->failure, &state->root_before);
    observe_worker_identity(&state->root_identity_before);
#if !LLAM_PLATFORM_WINDOWS
    observe_current_signal_stack(&state->root_signal_before);
#endif
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
    verify_worker_role_observation(&state->failure, &state->root_after);
    observe_worker_identity(&state->root_identity_after);
#if !LLAM_PLATFORM_WINDOWS
    observe_current_signal_stack(&state->root_signal_after);
#endif
    if (state->root_after.primary_by_context == 0U ||
        state->root_after.helper_by_context != 0U) {
        test_fail(&state->failure, "primary did not regain its scheduler context after helper return");
    }
    if (llam_join(child) != 0) {
        test_fail(&state->failure, "root could not join helper observer");
    }
}

static void *hold_helper_publication_tail(void *arg) {
    role_state_t *state = arg;
    uint64_t deadline = legacy_deadline();
    bool reached = false;

    while (llam_now_ns() < deadline) {
        if (llam_lease_test_gate_reached(LLAM_LEASE_TEST_BEFORE_HELPER_FOREIGN_PUBLISH) &&
            llam_lease_test_gate_reached(LLAM_LEASE_TEST_AFTER_REDIRECT_FOREIGN_PUBLISH)) {
            reached = true;
            break;
        }
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000L};
        (void)nanosleep(&delay, NULL);
    }
    if (!reached) {
        test_fail(&state->failure, "helper foreign-publication tail was not reached");
    }
    llam_lease_test_set_gate(LLAM_LEASE_TEST_BEFORE_HELPER_FOREIGN_PUBLISH, false);

    /* Keep the helper inside enter_blocking until the primary has observed its
     * published token, including the still-owned runtime/preemption guards. */
    deadline = legacy_deadline();
    reached = false;
    while (llam_now_ns() < deadline) {
        llam_shard_t *shard = &state->runtime->shards[0];
        pthread_mutex_lock(&shard->opaque_lock);
        pthread_mutex_lock(&shard->lock);
        reached = shard->primary_worker.foreign_token.state == LLAM_FOREIGN_TOKEN_ACTIVE ||
                  atomic_load_explicit(&state->runtime->fatal_errno, memory_order_acquire) != 0;
        pthread_mutex_unlock(&shard->lock);
        pthread_mutex_unlock(&shard->opaque_lock);
        if (reached) {
            break;
        }
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000L};
        (void)nanosleep(&delay, NULL);
    }
    if (!reached) {
        test_fail(&state->failure, "primary did not finish its helper-backed detach");
    }
    llam_lease_test_set_gate(LLAM_LEASE_TEST_AFTER_REDIRECT_FOREIGN_PUBLISH, false);
    return NULL;
}

static int test_primary_helper_takeover(bool hold_publication_tail) {
    role_state_t state;
    llam_runtime_t *runtime = NULL;
    llam_task_t *root = NULL;
    llam_shard_t *shard;
    uint64_t redirect_before;
    bool helper_same_driver;
    pthread_t gate_thread;
    int run_rc;

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
    if (hold_publication_tail) {
        llam_lease_test_set_gate(LLAM_LEASE_TEST_BEFORE_HELPER_FOREIGN_PUBLISH, true);
        llam_lease_test_set_gate(LLAM_LEASE_TEST_AFTER_REDIRECT_FOREIGN_PUBLISH, true);
        if (pthread_create(&gate_thread, NULL, hold_helper_publication_tail, &state) != 0) {
            llam_lease_test_set_gate(LLAM_LEASE_TEST_BEFORE_HELPER_FOREIGN_PUBLISH, false);
            llam_lease_test_set_gate(LLAM_LEASE_TEST_AFTER_REDIRECT_FOREIGN_PUBLISH, false);
            destroy_runtime_bounded(&runtime, "publication-tail runtime destroy exceeded the bounded deadline");
            return 1;
        }
    }
    root = llam_runtime_spawn_ex(runtime, role_root_task, &state, NULL, 0U);
    run_rc = root == NULL ? -1 : llam_runtime_run_handle(runtime);
    if (hold_publication_tail) {
        pthread_join(gate_thread, NULL);
    }
    if (root == NULL || run_rc != 0 || llam_join(root) != 0) {
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
    if (state.root_identity_before.shard != state.root_before.shard ||
        state.root_identity_before.worker == 0U ||
        state.root_identity_before.processor == 0U ||
        state.root_identity_before.incarnation == 0U ||
        state.root_identity_before.role != LLAM_WORKER_ROLE_PRIMARY ||
        state.root_identity_before.processor != (uintptr_t)&shard->processor ||
        state.root_identity_after.worker != state.root_identity_before.worker ||
        state.root_identity_after.processor != state.root_identity_before.processor ||
        state.root_identity_after.incarnation != state.root_identity_before.incarnation ||
        state.root_identity_after.role != LLAM_WORKER_ROLE_PRIMARY) {
        destroy_runtime_bounded(&runtime, "role-takeover runtime destroy exceeded the bounded deadline");
        fprintf(stderr,
                "test_worker_identity_internal: primary identity tuple changed during helper takeover\n");
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
    if (state.helper_identity.shard != state.root_identity_before.shard ||
        state.helper_identity.worker == 0U ||
        state.helper_identity.worker == state.root_identity_before.worker ||
        state.helper_identity.processor != state.root_identity_before.processor ||
        state.helper_identity.processor != (uintptr_t)&shard->processor ||
        state.helper_identity.incarnation == 0U ||
        state.helper_identity.role != LLAM_WORKER_ROLE_OPAQUE_HELPER ||
        state.helper_identity_after_enter.worker != state.helper_identity.worker ||
        state.helper_identity_after_enter.processor != state.helper_identity.processor ||
        state.helper_identity_after_enter.incarnation != state.helper_identity.incarnation ||
        state.helper_identity_after_enter.role != LLAM_WORKER_ROLE_OPAQUE_HELPER ||
        state.helper_identity_after_leave.worker != state.helper_identity.worker ||
        state.helper_identity_after_leave.processor != state.helper_identity.processor ||
        state.helper_identity_after_leave.incarnation != state.helper_identity.incarnation ||
        state.helper_identity_after_leave.role != LLAM_WORKER_ROLE_OPAQUE_HELPER) {
        destroy_runtime_bounded(&runtime, "role-takeover runtime destroy exceeded the bounded deadline");
        fprintf(stderr,
                "test_worker_identity_internal: helper identity tuple was not stable\n");
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
#if !LLAM_PLATFORM_WINDOWS
    if (!signal_observation_valid(&state.root_signal_before) ||
        !signal_observation_valid(&state.root_signal_after) ||
        !signal_observation_valid(&state.helper_signal) ||
        state.root_signal_before.worker_stack_base != state.root_signal_after.worker_stack_base ||
        state.root_signal_before.worker_stack_size != state.root_signal_after.worker_stack_size ||
        state.root_signal_before.worker_incarnation != state.root_signal_after.worker_incarnation ||
        !signal_ranges_disjoint(&state.root_signal_before, &state.helper_signal) ||
        !signal_range_contains((uintptr_t)shard->signal_stack_mapping,
                               shard->signal_stack_mapping_size,
                               state.root_signal_before.worker_stack_base,
                               state.root_signal_before.worker_stack_size) ||
        !signal_range_contains((uintptr_t)shard->signal_stack_mapping,
                               shard->signal_stack_mapping_size,
                               state.helper_signal.worker_stack_base,
                               state.helper_signal.worker_stack_size)) {
        destroy_runtime_bounded(&runtime, "role-takeover runtime destroy exceeded the bounded deadline");
        fprintf(stderr,
                "test_worker_identity_internal: primary/helper signal-stack ranges were not isolated\n");
        return 1;
    }
#endif
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
    verify_worker_role_observation(&state->failure, observation);
    observe_worker_identity(&state->identity_observations[task_arg->index]);
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
    state->first_exit_tls_clear = worker_identity_tuple_is_clear();
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
    state->second_exit_tls_clear = worker_identity_tuple_is_clear();
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
    if (state.identity_observations[0].shard != state.identity_observations[1].shard ||
        state.identity_observations[0].worker == 0U ||
        state.identity_observations[0].worker != state.identity_observations[1].worker ||
        state.identity_observations[0].processor == 0U ||
        state.identity_observations[0].processor != state.identity_observations[1].processor ||
        state.identity_observations[0].role != LLAM_WORKER_ROLE_PRIMARY ||
        state.identity_observations[1].role != LLAM_WORKER_ROLE_PRIMARY ||
        state.identity_observations[1].incarnation <= state.identity_observations[0].incarnation) {
        test_fail(&state.failure, "repeated runs did not preserve the primary slot or advance incarnation");
        goto cleanup;
    }
    if (state.first_exit_tls_clear == 0U || state.second_exit_tls_clear == 0U) {
        test_fail(&state.failure, "scheduler exit did not clear the complete identity TLS tuple");
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

static void nested_runtime_identity_task(void *arg) {
    nested_runtime_identity_state_t *state = arg;
    llam_runtime_opts_t opts;

    state->before_create = capture_managed_tls_tuple();
    if (!managed_tls_tuple_complete(&state->before_create)) {
        test_fail(&state->failure,
                  "nested runtime task did not start with a complete TLS tuple");
        return;
    }
    if (llam_runtime_opts_init(&opts, sizeof(opts)) != 0) {
        test_fail(&state->failure,
                  "nested runtime task could not initialize runtime options");
        return;
    }
    opts.deterministic = 1U;
    opts.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    if (llam_runtime_create_compat(&opts, sizeof(opts), &state->nested_runtime) != 0) {
        test_fail(&state->failure,
                  "nested runtime create failed from a managed task");
        return;
    }
    if (state->nested_runtime == NULL ||
        state->nested_runtime == llam_runtime_default()) {
        test_fail(&state->failure,
                  "nested runtime create returned an invalid runtime handle");
        if (state->nested_runtime == llam_runtime_default()) {
            state->nested_runtime = NULL;
        }
        return;
    }

    state->after_create = capture_managed_tls_tuple();
    if (managed_tls_tuples_match(&state->before_create, &state->after_create) == 0U) {
        test_fail(&state->failure,
                  "nested runtime create did not preserve all five TLS pointers");
        return;
    }
    llam_yield();
    state->after_yield = capture_managed_tls_tuple();
    if (managed_tls_tuples_match(&state->before_create, &state->after_yield) == 0U) {
        test_fail(&state->failure,
                  "nested runtime create TLS tuple changed after yield");
    }
}

static int test_nested_runtime_preserves_identity_tls(void) {
    nested_runtime_identity_state_t state;
    llam_runtime_t *outer_runtime = NULL;
    llam_task_t *outer_task = NULL;
    int rc = 0;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failure.failures, 0U);
    if (create_deterministic_runtime(&outer_runtime) != 0) {
        fprintf(stderr,
                "test_worker_identity_internal: nested identity outer runtime create failed: errno=%d (%s)\n",
                errno,
                strerror(errno));
        return 1;
    }
    outer_task = llam_runtime_spawn_ex(outer_runtime,
                                       nested_runtime_identity_task,
                                       &state,
                                       NULL,
                                       0U);
    if (outer_task == NULL) {
        fprintf(stderr,
                "test_worker_identity_internal: nested identity outer task spawn failed: errno=%d (%s)\n",
                errno,
                strerror(errno));
        rc = 1;
        goto cleanup;
    }
    if (llam_runtime_run_handle(outer_runtime) != 0) {
        fprintf(stderr,
                "test_worker_identity_internal: nested identity outer run failed: errno=%d (%s)\n",
                errno,
                strerror(errno));
        rc = 1;
        goto cleanup;
    }
    if (llam_join(outer_task) != 0) {
        fprintf(stderr,
                "test_worker_identity_internal: nested identity outer task join failed: errno=%d (%s)\n",
                errno,
                strerror(errno));
        rc = 1;
        goto cleanup;
    }
    if (state.nested_runtime == NULL) {
        test_fail(&state.failure,
                  "nested runtime task did not publish a runtime handle");
    }
    if (state.nested_runtime != NULL) {
        destroy_runtime_bounded(&state.nested_runtime,
                                "nested identity runtime destroy exceeded the bounded deadline");
    }
    if (failure_result(&state.failure, "nested runtime identity preservation failed")) {
        rc = 1;
    }

cleanup:
    if (state.nested_runtime != NULL) {
        destroy_runtime_bounded(&state.nested_runtime,
                                "nested identity runtime cleanup exceeded the bounded deadline");
    }
    if (outer_runtime != NULL) {
        destroy_runtime_bounded(&outer_runtime,
                                "nested identity outer runtime destroy exceeded the bounded deadline");
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
        atomic_store_explicit(&state->task_done, 1U, memory_order_release);
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
    errno = 0;
    state->leave_rc = llam_leave_blocking();
    state->leave_errno = errno;
    if (state->leave_rc != 0) {
        test_fail(&state->failure, "active-helper primary leave failed");
    }
    atomic_store_explicit(&state->task_done, 1U, memory_order_release);
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
    pthread_t destroy_thread;
    bool run_created = false;
    bool destroy_created = false;
    int rc = 1;

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
    atomic_init(&state.destroy_done, 0U);
    atomic_init(&state.run_rc, 0);
    atomic_init(&state.run_done, 0U);
    if (create_deterministic_runtime(&runtime) != 0) {
        return 1;
    }
    state.runtime = runtime;
    task = llam_runtime_spawn_ex(runtime, active_shutdown_task, &state, NULL, 0U);
    if (task == NULL) {
        test_fail(&state.failure, "active-helper run thread could not start");
        goto cleanup;
    }
    if (pthread_create(&run_thread, NULL, active_shutdown_run_thread, &state) != 0) {
        test_fail(&state.failure, "active-helper run thread could not start");
        goto cleanup;
    }
    run_created = true;
    if (wait_flag(&state.primary_entered, legacy_deadline()) != 0 ||
        wait_flag(&state.helper_state_observed, legacy_deadline()) != 0 ||
        atomic_load_explicit(&state.helper_started, memory_order_acquire) == 0U ||
        atomic_load_explicit(&state.helper_ready, memory_order_acquire) == 0U ||
        atomic_load_explicit(&state.helper_active, memory_order_acquire) == 0U) {
        test_fail(&state.failure, "active-helper state was not ready and active before destroy publication");
    }
    /* First complete the normal handback while the coordinator is still live. */
    llam_lease_test_set_gate(LLAM_LEASE_TEST_AFTER_HANDBACK_ACK, true);
    atomic_store_explicit(&state.release_primary, 1U, memory_order_release);
    if (wait_for_lease_gate_reached(LLAM_LEASE_TEST_AFTER_HANDBACK_ACK) != 0 ||
        wait_for_active_shutdown_snapshot(runtime,
                                          LLAM_LEASE_PRIMARY_OWNED,
                                          false,
                                          true,
                                          NULL) != 0) {
        test_fail(&state.failure,
                  "active-helper handback did not complete before shutdown publication");
        goto cleanup;
    }

    /* Publish STOPPING after the ACK, but keep the helper at the ACK gate. */
    if (pthread_create(&destroy_thread, NULL, active_shutdown_destroy_thread, &state) != 0) {
        test_fail(&state.failure, "active-helper destroy thread could not start");
        goto cleanup;
    }
    destroy_created = true;
    if (wait_flag(&state.destroy_started, legacy_deadline()) != 0 ||
        wait_for_active_shutdown_snapshot(runtime,
                                          LLAM_LEASE_STOPPING,
                                          true,
                                          true,
                                          NULL) != 0) {
        test_fail(&state.failure,
                  "active-helper shutdown did not publish STOPPING after handback ACK");
        goto cleanup;
    }

    llam_lease_test_set_gate(LLAM_LEASE_TEST_AFTER_HANDBACK_ACK, false);
    if (wait_flag(&state.task_done, legacy_deadline()) != 0 ||
        wait_flag(&state.run_done, legacy_deadline()) != 0 ||
        wait_flag(&state.destroy_done, legacy_deadline()) != 0) {
        test_fail(&state.failure, "active-helper shutdown did not drain the released primary");
        goto cleanup;
    }
    if (state.leave_rc != 0) {
        test_fail(&state.failure,
                  "active-helper primary leave did not accept the completed handback ACK");
    }
    if (state.run_rc != 0) {
        test_fail(&state.failure, "active-helper shutdown run returned an error");
    }

    runtime = NULL;
    pthread_join(run_thread, NULL);
    run_created = false;
    pthread_join(destroy_thread, NULL);
    destroy_created = false;
    rc = 0;

cleanup:
    llam_lease_test_set_gate(LLAM_LEASE_TEST_AFTER_HANDBACK_ACK, false);
    atomic_store_explicit(&state.release_primary, 1U, memory_order_release);
    if (destroy_created) {
        require_flag_before_cleanup(&state.destroy_done,
                                    "active-helper destroy thread did not finish during bounded cleanup");
        runtime = NULL;
        pthread_join(destroy_thread, NULL);
        destroy_created = false;
    }
    if (runtime != NULL) {
        destroy_runtime_bounded(&runtime, "active-helper runtime destroy exceeded the bounded deadline");
        runtime = NULL;
    }
    if (run_created) {
        require_flag_before_cleanup(&state.run_done,
                                    "active-helper run thread did not finish during bounded cleanup");
        pthread_join(run_thread, NULL);
    }
    if (destroy_created) {
        pthread_join(destroy_thread, NULL);
    }
    return rc != 0 || atomic_load_explicit(&state.failure.failures, memory_order_acquire) != 0U
               ? failure_result(&state.failure, "active-helper shutdown characterization failed")
               : 0;
}

#if LLAM_PLATFORM_WINDOWS && defined(_WIN64)
typedef struct win_identity_fixture {
    pthread_t self;
    atomic_uint ready;
    atomic_uint release;
} win_identity_fixture_t;

static void *win_identity_fixture_main(void *opaque) {
    win_identity_fixture_t *fixture = opaque;
    fixture->self = pthread_self();
    atomic_store_explicit(&fixture->ready, 1U, memory_order_release);
    while (atomic_load_explicit(&fixture->release, memory_order_acquire) == 0U) { Sleep(1U); }
    return NULL;
}

static int test_win64_private_identity_truth_table(void) {
    win_identity_fixture_t fixtures[2] = {0};
    pthread_t handles[2] = {0};
    HANDLE duplicate = NULL;
    uint64_t deadline = GetTickCount64() + 2000U;
    bool correct;
    for (unsigned i = 0U; i < 2U; ++i) {
        atomic_init(&fixtures[i].ready, 0U);
        atomic_init(&fixtures[i].release, 0U);
        if (pthread_create(&handles[i], NULL, win_identity_fixture_main, &fixtures[i]) != 0) {
            abort();
        }
    }
    while (atomic_load_explicit(&fixtures[0].ready, memory_order_acquire) == 0U ||
           atomic_load_explicit(&fixtures[1].ready, memory_order_acquire) == 0U) {
        if (GetTickCount64() >= deadline) { abort(); }
        Sleep(1U);
    }
    if (!DuplicateHandle(GetCurrentProcess(), (HANDLE)handles[0], GetCurrentProcess(),
                          &duplicate, 0U, FALSE, DUPLICATE_SAME_ACCESS)) { abort(); }
    errno = EDOM;
    SetLastError(0x5A18U);
    correct = pthread_equal(handles[0], fixtures[0].self) &&
        pthread_equal(fixtures[0].self, handles[0]) &&
        pthread_equal((pthread_t)duplicate, handles[0]) &&
        pthread_equal(fixtures[0].self, (pthread_t)duplicate) &&
        !pthread_equal(handles[0], handles[1]) &&
        !pthread_equal(fixtures[0].self, fixtures[1].self) &&
        !pthread_equal(handles[0], fixtures[1].self) &&
        GetLastError() == 0x5A18U && errno == EDOM;
    if (!CloseHandle(duplicate)) { abort(); }
    for (unsigned i = 0U; i < 2U; ++i) { atomic_store_explicit(&fixtures[i].release, 1U, memory_order_release); }
    for (unsigned i = 0U; i < 2U; ++i) {
        if (WaitForSingleObject((HANDLE)handles[i], 2000U) != WAIT_OBJECT_0 ||
            pthread_join(handles[i], NULL) != 0) { abort(); }
    }
    if (!correct) {
        fputs("test_worker_identity_internal: Win64 handle/self/duplicate identity or error sentinel mismatch\n", stderr);
        return 1;
    }
    return 0;
}
#endif

int main(void) {
#if LLAM_PLATFORM_WINDOWS && defined(_WIN64)
    if (test_win64_private_identity_truth_table() != 0) { return 1; }
#endif
    if (test_helper_create_failure_redirect() != 0 ||
#if LLAM_PLATFORM_POSIX
        test_wrong_owner_uninstall_aborts() != 0 ||
#endif
        test_primary_helper_takeover(false) != 0 ||
        test_primary_helper_takeover(true) != 0 ||
#if !LLAM_PLATFORM_WINDOWS
        test_helper_signal_stack_eagain() != 0 ||
        test_helper_signal_stack_einval() != 0 ||
#endif
        test_repeated_runs_distinct_host_threads() != 0 ||
        test_nested_runtime_preserves_identity_tls() != 0 ||
        test_idle_helper_shutdown() != 0 ||
        test_active_helper_shutdown() != 0) {
        return 1;
    }
    printf("test_worker_identity_internal: all legacy checks passed\n");
    return 0;
}
