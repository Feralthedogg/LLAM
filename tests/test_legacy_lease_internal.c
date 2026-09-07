/**
 * @file tests/test_legacy_lease_internal.c
 * @brief Phase 1A characterization tests for the legacy scheduler handoff.
 *
 * These tests deliberately observe the Phase 0 scheduler through its existing
 * worker identity, helper, redirect, pause, and watchdog state.  They do not
 * make a lease field authoritative; the lease snapshot calls only establish
 * the test-hook boundary consumed by later Phase 1A work.
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
#include "engine/runtime_watchdog_internal.h"

#include <errno.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if LLAM_PLATFORM_POSIX
#include <unistd.h>
#endif

#define LEGACY_LEASE_TEST_TIMEOUT_NS (3ULL * 1000ULL * 1000ULL * 1000ULL)

typedef struct test_failure_state {
    atomic_uint failures;
    const char *first_failure;
} test_failure_state_t;

typedef struct task_status {
    test_failure_state_t *failure;
    atomic_uint done;
    int run_errno;
    int leave_errno;
    int enter_rc;
    int enter_errno;
    int leave_rc;
    unsigned task_errno;
} task_status_t;

typedef struct takeover_state {
    test_failure_state_t failure;
    llam_runtime_t *runtime;
    atomic_uint root_done;
    atomic_uint helper_started;
    atomic_uint helper_done;
    atomic_uint helper_entered;
    atomic_uint release_helper;
    int root_enter_rc;
    int root_enter_errno;
    int root_leave_rc;
    int root_leave_errno;
    int helper_enter_rc;
    int helper_enter_errno;
    int helper_leave_rc;
    int helper_leave_errno;
    unsigned helper_redirect_active;
    unsigned helper_redirect_depth;
    pthread_t helper_thread;
    pthread_t helper_thread_after;
    uintptr_t root_worker;
    uintptr_t helper_worker;
} takeover_state_t;

typedef struct repeat_observation {
    uintptr_t shard;
    uintptr_t worker;
    uintptr_t processor;
    uint64_t incarnation;
    unsigned role;
} repeat_observation_t;

typedef struct repeat_run_state {
    test_failure_state_t failure;
    llam_runtime_t *runtime;
    repeat_observation_t observations[2];
    atomic_uint task_done[2];
    atomic_uint run_done[2];
    int run_rc[2];
    int run_errno[2];
    pthread_t drivers[2];
} repeat_run_state_t;

typedef struct repeat_driver_arg {
    repeat_run_state_t *state;
    unsigned index;
} repeat_driver_arg_t;

typedef struct active_shutdown_state {
    test_failure_state_t failure;
    llam_runtime_t *runtime;
    atomic_uint task_entered;
    atomic_uint release_task;
    atomic_uint task_done;
    atomic_uint destroy_started;
    atomic_uint destroy_done;
    atomic_uint run_done;
    pthread_t helper_thread;
    unsigned helper_started;
    unsigned helper_ready;
    unsigned helper_active;
    int run_rc;
    int run_errno;
} active_shutdown_state_t;

typedef struct dynamic_pause_state {
    test_failure_state_t failure;
    atomic_uint task_started;
    atomic_uint release_task;
    atomic_uint task_done;
    atomic_uint run_done;
    int run_rc;
    int run_errno;
} dynamic_pause_state_t;

typedef struct watchdog_overlap_state {
    test_failure_state_t failure;
    atomic_uint task_started;
    atomic_uint task_done;
    atomic_uint monitor_stop;
    atomic_uint monitor_done;
    atomic_uint run_done;
    int run_rc;
    int run_errno;
} watchdog_overlap_state_t;

static uint64_t legacy_deadline(void) {
    return llam_now_ns() + LEGACY_LEASE_TEST_TIMEOUT_NS;
}

static void test_fail(test_failure_state_t *state, const char *message) {
    if (state == NULL || message == NULL) {
        return;
    }
    if (atomic_fetch_add_explicit(&state->failures, 1U, memory_order_acq_rel) == 0U) {
        state->first_failure = message;
    }
}

static int wait_flag(const atomic_uint *flag, uint64_t deadline_ns) {
    if (flag == NULL) {
        return -1;
    }
    while (atomic_load_explicit(flag, memory_order_acquire) == 0U) {
        if (llam_now_ns() >= deadline_ns) {
            return -1;
        }
        llam_pause_cpu();
    }
    return 0;
}

static int failure_result(const test_failure_state_t *state, const char *fallback) {
    if (state != NULL && atomic_load_explicit(&state->failures, memory_order_acquire) != 0U) {
        fprintf(stderr,
                "test_legacy_lease_internal: %s\n",
                state->first_failure != NULL ? state->first_failure : fallback);
        return 1;
    }
    return 0;
}

static int create_runtime(llam_runtime_t **out, bool deterministic, bool dynamic_workers) {
    llam_runtime_opts_t opts;

    if (out == NULL || llam_runtime_opts_init(&opts, sizeof(opts)) != 0) {
        return -1;
    }
    opts.deterministic = deterministic ? 1U : 0U;
    opts.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    if (dynamic_workers) {
        opts.experimental_flags |= LLAM_RUNTIME_EXPERIMENTAL_F_DYNAMIC_WORKERS;
    }
    return llam_runtime_create(&opts, sizeof(opts), out);
}

static void destroy_runtime(llam_runtime_t **runtime) {
    if (runtime != NULL && *runtime != NULL) {
        llam_runtime_destroy(*runtime);
        *runtime = NULL;
    }
}

static void reset_lease_test_gates(void) {
    unsigned point;

    for (point = (unsigned)LLAM_LEASE_TEST_AFTER_TRANSFER_REQUEST;
         point <= (unsigned)LLAM_LEASE_TEST_BEFORE_WATCHDOG_SIGNAL;
         ++point) {
        llam_lease_test_set_gate((llam_lease_test_point_t)point, false);
    }
}

static int assert_snapshot(llam_runtime_t *runtime,
                           unsigned shard_id,
                           test_failure_state_t *failure,
                           const char *message) {
    llam_lease_test_snapshot_t snapshot;

    memset(&snapshot, 0xa5, sizeof(snapshot));
    if (llam_lease_test_snapshot(runtime, shard_id, &snapshot) != 0) {
        test_fail(failure, message);
        return -1;
    }
    return 0;
}

static int assert_no_fatal(llam_runtime_t *runtime,
                           int run_rc,
                           int run_errno,
                           test_failure_state_t *failure,
                           const char *message) {
    if (run_rc != 0 ||
        runtime == NULL ||
        atomic_load_explicit(&runtime->fatal_errno, memory_order_acquire) != 0) {
        fprintf(stderr,
                "test_legacy_lease_internal: run_rc=%d run_errno=%d fatal=%d\n",
                run_rc,
                run_errno,
                runtime != NULL
                    ? atomic_load_explicit(&runtime->fatal_errno, memory_order_acquire)
                    : -1);
        test_fail(failure, message);
        return -1;
    }
    return 0;
}

static void helper_activation_task(void *arg) {
    takeover_state_t *state = arg;
    llam_shard_t *shard = g_llam_tls_shard;

    if (shard == NULL || g_llam_tls_worker != &shard->opaque_helper_worker) {
        test_fail(&state->failure, "helper activation ran outside the opaque helper worker");
    }
    state->helper_worker = (uintptr_t)g_llam_tls_worker;
    errno = 0;
    state->helper_enter_rc = llam_enter_blocking();
    state->helper_enter_errno = errno;
    errno = 0;
    state->helper_leave_rc = llam_leave_blocking();
    state->helper_leave_errno = errno;
    atomic_store_explicit(&state->helper_done, 1U, memory_order_release);
}

static void takeover_root_task(void *arg) {
    takeover_state_t *state = arg;
    llam_task_t *helper_task;
    llam_shard_t *shard = g_llam_tls_shard;
    uint64_t deadline = legacy_deadline();

    if (shard == NULL || g_llam_tls_worker != &shard->primary_worker) {
        test_fail(&state->failure, "primary activation ran outside the primary worker");
    }
    state->root_worker = (uintptr_t)g_llam_tls_worker;
    helper_task = llam_runtime_spawn_ex(state->runtime, helper_activation_task, state, NULL, 0U);
    if (helper_task == NULL) {
        test_fail(&state->failure, "primary could not spawn helper activation task");
        atomic_store_explicit(&state->helper_done, 1U, memory_order_release);
        return;
    }

    errno = 0;
    state->root_enter_rc = llam_enter_blocking();
    state->root_enter_errno = errno;
    if (state->root_enter_rc != 0) {
        test_fail(&state->failure, "primary helper activation failed");
    }
    pthread_mutex_lock(&shard->opaque_lock);
    atomic_store_explicit(&state->helper_started,
                          shard->opaque_helper_thread_started ? 1U : 0U,
                          memory_order_release);
    if (shard->opaque_helper_thread_started) {
        state->helper_thread = shard->opaque_helper_thread;
    }
    pthread_mutex_unlock(&shard->opaque_lock);

    if (wait_flag(&state->helper_done, deadline) != 0) {
        test_fail(&state->failure, "helper activation task did not complete");
    }
    errno = 0;
    state->root_leave_rc = llam_leave_blocking();
    state->root_leave_errno = errno;
    if (state->root_leave_rc != 0) {
        test_fail(&state->failure, "primary helper handback failed");
    }
    errno = 0;
    if (llam_join(helper_task) != 0) {
        test_fail(&state->failure, "helper activation task join changed errno semantics");
    }
    atomic_store_explicit(&state->root_done, 1U, memory_order_release);
}

static int test_primary_helper_activation_and_return(void) {
    takeover_state_t state;
    llam_runtime_t *runtime = NULL;
    llam_task_t *root = NULL;
    llam_shard_t *shard;
    int run_rc;
    int run_errno;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failure.failures, 0U);
    atomic_init(&state.root_done, 0U);
    atomic_init(&state.helper_started, 0U);
    atomic_init(&state.helper_done, 0U);
    atomic_init(&state.helper_entered, 0U);
    atomic_init(&state.release_helper, 0U);
    reset_lease_test_gates();
    if (create_runtime(&runtime, true, false) != 0) {
        fprintf(stderr,
                "test_legacy_lease_internal: activation runtime create failed: errno=%d (%s)\n",
                errno,
                strerror(errno));
        return 1;
    }
    state.runtime = runtime;
    shard = &runtime->shards[0];
    if (assert_snapshot(runtime, 0U, &state.failure, "activation snapshot before run failed") != 0) {
        destroy_runtime(&runtime);
        return 1;
    }
    root = llam_runtime_spawn_ex(runtime, takeover_root_task, &state, NULL, 0U);
    if (root == NULL) {
        test_fail(&state.failure, "primary activation task spawn failed");
    }
    errno = 0;
    run_rc = root == NULL ? -1 : llam_runtime_run_handle(runtime);
    run_errno = errno;
    if (root != NULL) {
        errno = 0;
        if (llam_join(root) != 0) {
            test_fail(&state.failure, "primary activation task join failed");
        }
    }
    assert_no_fatal(runtime, run_rc, run_errno, &state.failure,
                    "primary/helper activation changed run or fatal semantics");
    if (atomic_load_explicit(&state.root_done, memory_order_acquire) == 0U ||
        atomic_load_explicit(&state.helper_done, memory_order_acquire) == 0U ||
        state.root_enter_rc != 0 || state.root_leave_rc != 0 ||
        state.helper_enter_rc != 0 || state.helper_leave_rc != 0) {
        test_fail(&state.failure, "primary/helper activation did not complete cleanly");
    }
    pthread_mutex_lock(&shard->opaque_lock);
    if (state.helper_thread != shard->opaque_helper_thread ||
        !shard->opaque_helper_thread_started || !shard->opaque_helper_ready ||
        shard->opaque_helper_failed || shard->opaque_helper_active) {
        test_fail(&state.failure, "primary/helper activation created or retained an invalid helper");
    }
    state.helper_thread_after = shard->opaque_helper_thread;
    pthread_mutex_unlock(&shard->opaque_lock);
    if (state.helper_thread != state.helper_thread_after ||
        state.root_worker == 0U || state.helper_worker == 0U ||
        state.root_worker == state.helper_worker) {
        test_fail(&state.failure, "primary/helper activation did not preserve one helper identity");
    }
    if (assert_snapshot(runtime, 0U, &state.failure, "activation snapshot after run failed") != 0) {
        /* Keep the first failure for the report. */
    }
    destroy_runtime(&runtime);
    return failure_result(&state.failure, "primary/helper activation characterization failed");
}

static void redirect_helper_task(void *arg) {
    takeover_state_t *state = arg;
    llam_shard_t *shard = g_llam_tls_shard;

    if (shard == NULL || g_llam_tls_worker != &shard->opaque_helper_worker) {
        test_fail(&state->failure, "redirect helper task ran outside the opaque helper worker");
    }
    state->helper_worker = (uintptr_t)g_llam_tls_worker;
    errno = 0;
    state->helper_enter_rc = llam_enter_blocking();
    state->helper_enter_errno = errno;
    pthread_mutex_lock(&shard->lock);
    state->helper_redirect_active = shard->opaque_redirect_active ? 1U : 0U;
    state->helper_redirect_depth = shard->opaque_redirect_depth;
    pthread_mutex_unlock(&shard->lock);
    atomic_store_explicit(&state->helper_entered, 1U, memory_order_release);
    while (atomic_load_explicit(&state->release_helper, memory_order_acquire) == 0U) {
        llam_yield();
    }
    errno = 0;
    state->helper_leave_rc = llam_leave_blocking();
    state->helper_leave_errno = errno;
    atomic_store_explicit(&state->helper_done, 1U, memory_order_release);
}

static void redirect_root_task(void *arg) {
    takeover_state_t *state = arg;
    llam_task_t *helper_task;
    llam_shard_t *shard = g_llam_tls_shard;
    uint64_t deadline = legacy_deadline();

    helper_task = llam_runtime_spawn_ex(state->runtime, redirect_helper_task, state, NULL, 0U);
    if (helper_task == NULL) {
        test_fail(&state->failure, "redirect root could not spawn helper task");
        atomic_store_explicit(&state->helper_done, 1U, memory_order_release);
        return;
    }
    errno = 0;
    state->root_enter_rc = llam_enter_blocking();
    state->root_enter_errno = errno;
    if (state->root_enter_rc != 0) {
        test_fail(&state->failure, "redirect root helper activation failed");
    }
    pthread_mutex_lock(&shard->opaque_lock);
    if (!shard->opaque_helper_thread_started) {
        test_fail(&state->failure, "redirect root did not start its one helper");
    } else {
        state->helper_thread = shard->opaque_helper_thread;
    }
    pthread_mutex_unlock(&shard->opaque_lock);
    if (wait_flag(&state->helper_entered, deadline) != 0 ||
        state->helper_enter_rc != 0) {
        test_fail(&state->failure, "helper re-block did not enter redirect cleanly");
    }
    atomic_store_explicit(&state->release_helper, 1U, memory_order_release);
    if (wait_flag(&state->helper_done, deadline) != 0) {
        test_fail(&state->failure, "helper redirect task did not return");
    }
    errno = 0;
    state->root_leave_rc = llam_leave_blocking();
    state->root_leave_errno = errno;
    if (state->root_leave_rc != 0) {
        test_fail(&state->failure, "redirect root handback failed");
    }
    errno = 0;
    if (llam_join(helper_task) != 0) {
        test_fail(&state->failure, "redirect helper join changed errno semantics");
    }
    atomic_store_explicit(&state->root_done, 1U, memory_order_release);
}

static int test_helper_reblocking_through_redirect(void) {
    takeover_state_t state;
    llam_runtime_t *runtime = NULL;
    llam_task_t *root = NULL;
    llam_shard_t *shard;
    uint64_t redirects_before;
    int run_rc;
    int run_errno;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failure.failures, 0U);
    atomic_init(&state.root_done, 0U);
    atomic_init(&state.helper_started, 0U);
    atomic_init(&state.helper_done, 0U);
    atomic_init(&state.helper_entered, 0U);
    atomic_init(&state.release_helper, 0U);
    reset_lease_test_gates();
    if (create_runtime(&runtime, true, false) != 0) {
        return 1;
    }
    state.runtime = runtime;
    shard = &runtime->shards[0];
    redirects_before = atomic_load_explicit(&shard->metrics.opaque_redirect_activations,
                                            memory_order_acquire);
    root = llam_runtime_spawn_ex(runtime, redirect_root_task, &state, NULL, 0U);
    errno = 0;
    run_rc = root == NULL ? -1 : llam_runtime_run_handle(runtime);
    run_errno = errno;
    if (root != NULL) {
        errno = 0;
        if (llam_join(root) != 0) {
            test_fail(&state.failure, "redirect root join failed");
        }
    }
    assert_no_fatal(runtime, run_rc, run_errno, &state.failure,
                    "helper redirect changed run or fatal semantics");
    if (atomic_load_explicit(&state.root_done, memory_order_acquire) == 0U ||
        atomic_load_explicit(&state.helper_done, memory_order_acquire) == 0U ||
        state.helper_redirect_active == 0U || state.helper_redirect_depth == 0U ||
        state.helper_enter_rc != 0 || state.helper_leave_rc != 0 ||
        atomic_load_explicit(&shard->metrics.opaque_redirect_activations,
                             memory_order_acquire) <= redirects_before) {
        test_fail(&state.failure, "helper re-blocking did not preserve redirect ordering");
    }
    pthread_mutex_lock(&shard->opaque_lock);
    if (!shard->opaque_helper_thread_started || state.helper_thread != shard->opaque_helper_thread ||
        !shard->opaque_helper_ready || shard->opaque_helper_failed) {
        test_fail(&state.failure, "helper re-blocking created a second or invalid helper");
    }
    pthread_mutex_unlock(&shard->opaque_lock);
    assert_snapshot(runtime, 0U, &state.failure, "redirect snapshot after run failed");
    destroy_runtime(&runtime);
    return failure_result(&state.failure, "helper re-blocking characterization failed");
}

static void ownerless_helper_task(void *arg) {
    takeover_state_t *state = arg;
    llam_shard_t *shard = g_llam_tls_shard;

    errno = 0;
    state->helper_enter_rc = llam_enter_blocking();
    state->helper_enter_errno = errno;
    pthread_mutex_lock(&shard->lock);
    state->helper_redirect_active = shard->opaque_redirect_active ? 1U : 0U;
    state->helper_redirect_depth = shard->opaque_redirect_depth;
    pthread_mutex_unlock(&shard->lock);
    atomic_store_explicit(&state->helper_entered, 1U, memory_order_release);
    while (atomic_load_explicit(&state->release_helper, memory_order_acquire) == 0U) {
        llam_yield();
    }
    errno = 0;
    state->helper_leave_rc = llam_leave_blocking();
    state->helper_leave_errno = errno;
    atomic_store_explicit(&state->helper_done, 1U, memory_order_release);
}

static void ownerless_root_task(void *arg) {
    takeover_state_t *state = arg;
    llam_task_t *helper_task;
    llam_shard_t *shard = g_llam_tls_shard;
    uint64_t deadline = legacy_deadline();
    unsigned redirect_active_before;
    unsigned redirect_depth_before;

    helper_task = llam_runtime_spawn_ex(state->runtime, ownerless_helper_task, state, NULL, 0U);
    if (helper_task == NULL) {
        test_fail(&state->failure, "ownerless root could not spawn helper task");
        atomic_store_explicit(&state->helper_done, 1U, memory_order_release);
        return;
    }
    errno = 0;
    state->root_enter_rc = llam_enter_blocking();
    state->root_enter_errno = errno;
    pthread_mutex_lock(&shard->opaque_lock);
    if (!shard->opaque_helper_thread_started) {
        test_fail(&state->failure, "ownerless root did not start its one helper");
    } else {
        state->helper_thread = shard->opaque_helper_thread;
    }
    pthread_mutex_unlock(&shard->opaque_lock);
    if (wait_flag(&state->helper_entered, deadline) != 0) {
        test_fail(&state->failure, "ownerless helper did not reach redirect");
    }
    pthread_mutex_lock(&shard->lock);
    redirect_active_before = shard->opaque_redirect_active ? 1U : 0U;
    redirect_depth_before = shard->opaque_redirect_depth;
    pthread_mutex_unlock(&shard->lock);
    atomic_store_explicit(&state->release_helper, 1U, memory_order_release);
    if (wait_flag(&state->helper_done, deadline) != 0) {
        test_fail(&state->failure, "ownerless helper did not complete its return");
    }
    pthread_mutex_lock(&shard->lock);
    if (redirect_active_before == 0U || redirect_depth_before == 0U ||
        shard->opaque_redirect_active) {
        test_fail(&state->failure, "ownerless return ordering released redirect out of order");
    }
    pthread_mutex_unlock(&shard->lock);
    errno = 0;
    state->root_leave_rc = llam_leave_blocking();
    state->root_leave_errno = errno;
    if (state->root_leave_rc != 0) {
        test_fail(&state->failure, "ownerless primary return failed");
    }
    errno = 0;
    if (llam_join(helper_task) != 0) {
        test_fail(&state->failure, "ownerless helper join changed errno semantics");
    }
    atomic_store_explicit(&state->root_done, 1U, memory_order_release);
}

static int test_ownerless_return_ordering(void) {
    takeover_state_t state;
    llam_runtime_t *runtime = NULL;
    llam_task_t *root = NULL;
    llam_shard_t *shard;
    int run_rc;
    int run_errno;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failure.failures, 0U);
    atomic_init(&state.root_done, 0U);
    atomic_init(&state.helper_started, 0U);
    atomic_init(&state.helper_done, 0U);
    atomic_init(&state.helper_entered, 0U);
    atomic_init(&state.release_helper, 0U);
    reset_lease_test_gates();
    if (create_runtime(&runtime, true, false) != 0) {
        return 1;
    }
    state.runtime = runtime;
    shard = &runtime->shards[0];
    root = llam_runtime_spawn_ex(runtime, ownerless_root_task, &state, NULL, 0U);
    errno = 0;
    run_rc = root == NULL ? -1 : llam_runtime_run_handle(runtime);
    run_errno = errno;
    if (root != NULL) {
        errno = 0;
        if (llam_join(root) != 0) {
            test_fail(&state.failure, "ownerless root join failed");
        }
    }
    assert_no_fatal(runtime, run_rc, run_errno, &state.failure,
                    "ownerless return changed run or fatal semantics");
    if (state.root_enter_rc != 0 || state.root_leave_rc != 0 ||
        state.helper_enter_rc != 0 || state.helper_leave_rc != 0 ||
        atomic_load_explicit(&state.helper_done, memory_order_acquire) == 0U ||
        atomic_load_explicit(&state.root_done, memory_order_acquire) == 0U) {
        test_fail(&state.failure, "ownerless return did not complete its task boundaries");
    }
    pthread_mutex_lock(&shard->opaque_lock);
    if (!shard->opaque_helper_thread_started || shard->opaque_helper_failed ||
        state.helper_thread != shard->opaque_helper_thread) {
        test_fail(&state.failure, "ownerless return did not preserve one helper lifecycle");
    }
    pthread_mutex_unlock(&shard->opaque_lock);
    assert_snapshot(runtime, 0U, &state.failure, "ownerless snapshot after run failed");
    destroy_runtime(&runtime);
    return failure_result(&state.failure, "ownerless return characterization failed");
}

static void repeat_observer_task(void *arg) {
    repeat_run_state_t *state = arg;
    unsigned index = atomic_load_explicit(&state->task_done[0], memory_order_relaxed) == 0U ? 0U : 1U;
    llam_worker_t *worker = g_llam_tls_worker;

    state->observations[index].shard = (uintptr_t)g_llam_tls_shard;
    state->observations[index].worker = (uintptr_t)worker;
    state->observations[index].processor = (uintptr_t)g_llam_tls_processor;
    state->observations[index].role = worker != NULL ? (unsigned)worker->role : 0U;
    state->observations[index].incarnation = worker != NULL
                                                  ? atomic_load_explicit(&worker->incarnation,
                                                                         memory_order_acquire)
                                                  : 0U;
    atomic_store_explicit(&state->task_done[index], 1U, memory_order_release);
}

static void *repeat_run_driver(void *arg) {
    repeat_driver_arg_t *driver = arg;
    repeat_run_state_t *state = driver->state;
    unsigned index = driver->index;
    int saved_errno;

    errno = 0;
    state->run_rc[index] = llam_runtime_run_handle(state->runtime);
    saved_errno = errno;
    state->run_errno[index] = saved_errno;
    atomic_store_explicit(&state->run_done[index], 1U, memory_order_release);
    return NULL;
}

static int test_repeated_shard_zero_runs_two_callers(void) {
    repeat_run_state_t state;
    llam_runtime_t *runtime = NULL;
    llam_task_t *tasks[2] = {NULL, NULL};
    repeat_driver_arg_t drivers[2];
    unsigned i;
    int rc = 0;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failure.failures, 0U);
    atomic_init(&state.task_done[0], 0U);
    atomic_init(&state.task_done[1], 0U);
    atomic_init(&state.run_done[0], 0U);
    atomic_init(&state.run_done[1], 0U);
    memset(drivers, 0, sizeof(drivers));
    reset_lease_test_gates();
    if (create_runtime(&runtime, true, false) != 0) {
        return 1;
    }
    state.runtime = runtime;
    for (i = 0U; i < 2U; ++i) {
        tasks[i] = llam_runtime_spawn_ex(runtime, repeat_observer_task, &state, NULL, 0U);
        if (tasks[i] == NULL) {
            test_fail(&state.failure, "repeated shard-zero task spawn failed");
            rc = 1;
            break;
        }
        drivers[i].state = &state;
        drivers[i].index = i;
        if (pthread_create(&state.drivers[i], NULL, repeat_run_driver, &drivers[i]) != 0) {
            test_fail(&state.failure, "repeated shard-zero caller thread creation failed");
            rc = 1;
            break;
        }
        if (wait_flag(&state.run_done[i], legacy_deadline()) != 0 ||
            wait_flag(&state.task_done[i], legacy_deadline()) != 0) {
            test_fail(&state.failure, "repeated shard-zero run did not complete");
            rc = 1;
        }
        pthread_join(state.drivers[i], NULL);
        if (state.run_rc[i] != 0) {
            test_fail(&state.failure, "repeated shard-zero run changed fatal semantics");
            rc = 1;
        }
        errno = 0;
        if (tasks[i] != NULL && llam_join(tasks[i]) != 0) {
            test_fail(&state.failure, "repeated shard-zero task join failed");
            rc = 1;
        }
    }
    if (state.observations[0].shard == 0U ||
        state.observations[0].shard != state.observations[1].shard ||
        state.observations[0].worker == 0U ||
        state.observations[0].worker != state.observations[1].worker ||
        state.observations[0].processor == 0U ||
        state.observations[0].processor != state.observations[1].processor ||
        state.observations[0].role != LLAM_WORKER_ROLE_PRIMARY ||
        state.observations[1].role != LLAM_WORKER_ROLE_PRIMARY ||
        state.observations[1].incarnation <= state.observations[0].incarnation) {
        test_fail(&state.failure, "repeated shard-zero runs did not advance one primary incarnation");
        rc = 1;
    }
    if (runtime != NULL) {
        llam_shard_t *shard = &runtime->shards[0];

        pthread_mutex_lock(&shard->opaque_lock);
        if (shard->opaque_helper_thread_started || shard->opaque_helper_ready ||
            shard->opaque_helper_active) {
            test_fail(&state.failure, "repeated shard-zero runs created a helper unexpectedly");
            rc = 1;
        }
        pthread_mutex_unlock(&shard->opaque_lock);
    }
    if (assert_snapshot(runtime, 0U, &state.failure, "repeat snapshot after runs failed") != 0) {
        rc = 1;
    }
    destroy_runtime(&runtime);
    return rc != 0 ? 1 : failure_result(&state.failure, "repeated shard-zero characterization failed");
}

static void idle_helper_task(void *arg) {
    task_status_t *status = arg;

    errno = 0;
    status->enter_rc = llam_enter_blocking();
    status->enter_errno = errno;
    errno = 0;
    status->leave_rc = llam_leave_blocking();
    status->leave_errno = errno;
    atomic_store_explicit(&status->done, 1U, memory_order_release);
}

static int test_helper_shutdown_while_idle(void) {
    test_failure_state_t failure;
    task_status_t status;
    llam_runtime_t *runtime = NULL;
    llam_task_t *task = NULL;
    llam_shard_t *shard;
    int run_rc;
    int run_errno;
    bool helper_started;
    bool helper_ready;
    bool helper_active;

    memset(&failure, 0, sizeof(failure));
    memset(&status, 0, sizeof(status));
    atomic_init(&failure.failures, 0U);
    atomic_init(&status.done, 0U);
    status.failure = &failure;
    reset_lease_test_gates();
    if (create_runtime(&runtime, true, false) != 0) {
        return 1;
    }
    task = llam_runtime_spawn_ex(runtime, idle_helper_task, &status, NULL, 0U);
    errno = 0;
    run_rc = task == NULL ? -1 : llam_runtime_run_handle(runtime);
    run_errno = errno;
    if (task != NULL) {
        errno = 0;
        if (llam_join(task) != 0) {
            test_fail(&failure, "idle helper task join failed");
        }
    }
    assert_no_fatal(runtime, run_rc, run_errno, &failure,
                    "idle helper shutdown changed run or fatal semantics");
    shard = &runtime->shards[0];
    pthread_mutex_lock(&shard->opaque_lock);
    helper_started = shard->opaque_helper_thread_started;
    helper_ready = shard->opaque_helper_ready;
    helper_active = shard->opaque_helper_active;
    pthread_mutex_unlock(&shard->opaque_lock);
    if (atomic_load_explicit(&status.done, memory_order_acquire) == 0U ||
        status.enter_rc != 0 || status.leave_rc != 0 ||
        !helper_started || !helper_ready || helper_active) {
        test_fail(&failure, "idle helper shutdown did not leave one ready idle helper");
    }
    assert_snapshot(runtime, 0U, &failure, "idle helper snapshot failed");
    destroy_runtime(&runtime);
    return failure_result(&failure, "idle helper shutdown characterization failed");
}

static void active_shutdown_task(void *arg) {
    active_shutdown_state_t *state = arg;

    errno = 0;
    if (llam_enter_blocking() != 0) {
        test_fail(&state->failure, "active helper enter failed");
        atomic_store_explicit(&state->task_entered, 1U, memory_order_release);
        return;
    }
    atomic_store_explicit(&state->task_entered, 1U, memory_order_release);
    while (atomic_load_explicit(&state->release_task, memory_order_acquire) == 0U) {
        llam_yield();
    }
    errno = 0;
    if (llam_leave_blocking() != 0) {
        test_fail(&state->failure, "active helper leave failed");
    }
    atomic_store_explicit(&state->task_done, 1U, memory_order_release);
}

static void *active_shutdown_run(void *arg) {
    active_shutdown_state_t *state = arg;

    errno = 0;
    state->run_rc = llam_runtime_run_handle(state->runtime);
    state->run_errno = errno;
    atomic_store_explicit(&state->run_done, 1U, memory_order_release);
    return NULL;
}

static void *active_shutdown_destroy(void *arg) {
    active_shutdown_state_t *state = arg;

    atomic_store_explicit(&state->destroy_started, 1U, memory_order_release);
    llam_runtime_destroy(state->runtime);
    atomic_store_explicit(&state->destroy_done, 1U, memory_order_release);
    return NULL;
}

static int test_helper_shutdown_while_active(void) {
    active_shutdown_state_t state;
    llam_runtime_t *runtime = NULL;
    llam_task_t *task = NULL;
    pthread_t run_thread;
    pthread_t destroy_thread;
    bool run_created = false;
    bool destroy_created = false;
    int rc = 0;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failure.failures, 0U);
    atomic_init(&state.task_entered, 0U);
    atomic_init(&state.release_task, 0U);
    atomic_init(&state.task_done, 0U);
    atomic_init(&state.destroy_started, 0U);
    atomic_init(&state.destroy_done, 0U);
    atomic_init(&state.run_done, 0U);
    reset_lease_test_gates();
    if (create_runtime(&runtime, true, false) != 0) {
        return 1;
    }
    state.runtime = runtime;
    task = llam_runtime_spawn_ex(runtime, active_shutdown_task, &state, NULL, 0U);
    if (task == NULL || pthread_create(&run_thread, NULL, active_shutdown_run, &state) != 0) {
        test_fail(&state.failure, "active helper run setup failed");
        goto cleanup;
    }
    run_created = true;
    if (wait_flag(&state.task_entered, legacy_deadline()) != 0) {
        test_fail(&state.failure, "active helper task did not enter blocking");
        goto cleanup;
    }
    {
        llam_shard_t *shard = &runtime->shards[0];

        pthread_mutex_lock(&shard->opaque_lock);
        state.helper_started = shard->opaque_helper_thread_started ? 1U : 0U;
        state.helper_ready = shard->opaque_helper_ready ? 1U : 0U;
        state.helper_active = shard->opaque_helper_active ? 1U : 0U;
        if (state.helper_started) {
            state.helper_thread = shard->opaque_helper_thread;
        }
        pthread_mutex_unlock(&shard->opaque_lock);
    }
    if (state.helper_started == 0U || state.helper_ready == 0U || state.helper_active == 0U) {
        test_fail(&state.failure, "active helper task did not publish one active helper");
        goto cleanup;
    }
    if (pthread_create(&destroy_thread, NULL, active_shutdown_destroy, &state) != 0) {
        test_fail(&state.failure, "active helper destroy thread setup failed");
        goto cleanup;
    }
    destroy_created = true;
    if (wait_flag(&state.destroy_started, legacy_deadline()) != 0) {
        test_fail(&state.failure, "active helper destroy did not start");
        goto cleanup;
    }
    atomic_store_explicit(&state.release_task, 1U, memory_order_release);
    if (wait_flag(&state.destroy_done, legacy_deadline()) != 0 ||
        wait_flag(&state.run_done, legacy_deadline()) != 0 ||
        wait_flag(&state.task_done, legacy_deadline()) != 0) {
        test_fail(&state.failure, "active helper shutdown did not complete bounded teardown");
        goto cleanup;
    }
    runtime = NULL;
    if (state.run_rc != 0) {
        test_fail(&state.failure, "active helper shutdown changed run fatal semantics");
    }
    rc = failure_result(&state.failure, "active helper shutdown characterization failed");

cleanup:
    atomic_store_explicit(&state.release_task, 1U, memory_order_release);
    if (runtime != NULL && run_created) {
        if (wait_flag(&state.run_done, legacy_deadline()) == 0) {
            runtime = NULL;
        }
    }
    if (runtime != NULL) {
        destroy_runtime(&runtime);
    }
    if (destroy_created) {
        pthread_join(destroy_thread, NULL);
    }
    if (run_created) {
        pthread_join(run_thread, NULL);
    }
    return rc != 0 ? rc : failure_result(&state.failure, "active helper shutdown characterization failed");
}

static void dynamic_pause_task(void *arg) {
    dynamic_pause_state_t *state = arg;

    atomic_store_explicit(&state->task_started, 1U, memory_order_release);
    while (atomic_load_explicit(&state->release_task, memory_order_acquire) == 0U) {
        llam_yield();
    }
    atomic_store_explicit(&state->task_done, 1U, memory_order_release);
}

typedef struct dynamic_pause_driver {
    dynamic_pause_state_t *state;
    llam_runtime_t *runtime;
} dynamic_pause_driver_t;

static void *dynamic_pause_run_typed(void *arg) {
    dynamic_pause_driver_t *driver = arg;

    errno = 0;
    driver->state->run_rc = llam_runtime_run_handle(driver->runtime);
    driver->state->run_errno = errno;
    atomic_store_explicit(&driver->state->run_done, 1U, memory_order_release);
    return NULL;
}

static int test_dynamic_merge_pause(void) {
    dynamic_pause_state_t state;
    dynamic_pause_driver_t driver;
    llam_runtime_t *runtime = NULL;
    pthread_t run_thread;
    bool run_created = false;
    unsigned i;
    unsigned paused = 0U;
    int rc = 0;

    memset(&state, 0, sizeof(state));
    memset(&driver, 0, sizeof(driver));
    atomic_init(&state.failure.failures, 0U);
    atomic_init(&state.task_started, 0U);
    atomic_init(&state.release_task, 0U);
    atomic_init(&state.task_done, 0U);
    atomic_init(&state.run_done, 0U);
    reset_lease_test_gates();
    if (create_runtime(&runtime, false, true) != 0) {
        return 1;
    }
    if (runtime->active_shards < 2U) {
        fprintf(stderr, "test_legacy_lease_internal: dynamic merge pause skipped with one shard\n");
        destroy_runtime(&runtime);
        return 0;
    }
    if (llam_runtime_spawn_ex(runtime, dynamic_pause_task, &state, NULL, 0U) == NULL) {
        test_fail(&state.failure, "dynamic merge pause task spawn failed");
        destroy_runtime(&runtime);
        return 1;
    }
    driver.state = &state;
    driver.runtime = runtime;
    if (pthread_create(&run_thread, NULL, dynamic_pause_run_typed, &driver) != 0) {
        test_fail(&state.failure, "dynamic merge pause run thread setup failed");
        destroy_runtime(&runtime);
        return 1;
    }
    run_created = true;
    if (wait_flag(&state.task_started, legacy_deadline()) != 0) {
        test_fail(&state.failure, "dynamic merge pause task did not start");
        goto cleanup;
    }
    for (i = 1U; i < runtime->active_shards; ++i) {
        uint64_t deadline = legacy_deadline();

        if (!llam_shard_is_online(&runtime->shards[i])) {
            continue;
        }
        llam_shard_request_merge_pause(&runtime->shards[i]);
        if (!llam_shard_wait_merge_pause_ack(&runtime->shards[i], deadline)) {
            test_fail(&state.failure, "dynamic merge pause did not receive an acknowledgement");
        }
        paused += 1U;
        llam_shard_release_merge_pause(&runtime->shards[i]);
        pthread_mutex_lock(&runtime->shards[i].opaque_lock);
        if (runtime->shards[i].opaque_helper_thread_started) {
            test_fail(&state.failure, "dynamic merge pause unexpectedly created a helper");
        }
        pthread_mutex_unlock(&runtime->shards[i].opaque_lock);
    }
    if (paused == 0U) {
        fprintf(stderr, "test_legacy_lease_internal: dynamic merge pause skipped with no online secondary\n");
    }
    atomic_store_explicit(&state.release_task, 1U, memory_order_release);
    if (wait_flag(&state.task_done, legacy_deadline()) != 0 ||
        wait_flag(&state.run_done, legacy_deadline()) != 0) {
        test_fail(&state.failure, "dynamic merge pause task/run did not complete");
        goto cleanup;
    }
    if (state.run_rc != 0 ||
        atomic_load_explicit(&runtime->fatal_errno, memory_order_acquire) != 0) {
        test_fail(&state.failure, "dynamic merge pause changed run fatal semantics");
    }
    assert_snapshot(runtime, 0U, &state.failure, "dynamic merge snapshot failed");
    rc = failure_result(&state.failure, "dynamic merge pause characterization failed");

cleanup:
    atomic_store_explicit(&state.release_task, 1U, memory_order_release);
    if (run_created) {
        wait_flag(&state.run_done, legacy_deadline());
        pthread_join(run_thread, NULL);
    }
    destroy_runtime(&runtime);
    return rc != 0 ? rc : failure_result(&state.failure, "dynamic merge pause characterization failed");
}

static void watchdog_overlap_task(void *arg) {
    watchdog_overlap_state_t *state = arg;
    unsigned i;

    atomic_store_explicit(&state->task_started, 1U, memory_order_release);
    for (i = 0U; i < 4096U; ++i) {
        llam_task_safepoint();
        llam_yield();
    }
    atomic_store_explicit(&state->task_done, 1U, memory_order_release);
}

typedef struct watchdog_overlap_driver {
    watchdog_overlap_state_t *state;
    llam_runtime_t *runtime;
} watchdog_overlap_driver_t;

static void *watchdog_overlap_run_typed(void *arg) {
    watchdog_overlap_driver_t *driver = arg;

    errno = 0;
    driver->state->run_rc = llam_runtime_run_handle(driver->runtime);
    driver->state->run_errno = errno;
    atomic_store_explicit(&driver->state->run_done, 1U, memory_order_release);
    return NULL;
}

static void *watchdog_overlap_monitor(void *arg) {
    watchdog_overlap_driver_t *driver = arg;
    watchdog_overlap_state_t *state = driver->state;
    llam_shard_t *shard = &driver->runtime->shards[0];

    while (atomic_load_explicit(&state->monitor_stop, memory_order_acquire) == 0U) {
        llam_watchdog_check_shard(shard, llam_now_ns());
        llam_pause_cpu();
    }
    atomic_store_explicit(&state->monitor_done, 1U, memory_order_release);
    return NULL;
}

static int test_watchdog_task_exit_overlap(void) {
    watchdog_overlap_state_t state;
    watchdog_overlap_driver_t driver;
    llam_runtime_t *runtime = NULL;
    pthread_t run_thread;
    pthread_t monitor_thread;
    bool run_created = false;
    bool monitor_created = false;
    int rc = 0;

    memset(&state, 0, sizeof(state));
    memset(&driver, 0, sizeof(driver));
    atomic_init(&state.failure.failures, 0U);
    atomic_init(&state.task_started, 0U);
    atomic_init(&state.task_done, 0U);
    atomic_init(&state.monitor_stop, 0U);
    atomic_init(&state.monitor_done, 0U);
    atomic_init(&state.run_done, 0U);
    reset_lease_test_gates();
    if (create_runtime(&runtime, true, false) != 0) {
        return 1;
    }
    runtime->preempt_mode = LLAM_PREEMPT_STRICT;
    runtime->preempt_quantum_ns = 1U;
    if (llam_runtime_spawn_ex(runtime, watchdog_overlap_task, &state, NULL, 0U) == NULL) {
        test_fail(&state.failure, "watchdog overlap task spawn failed");
        destroy_runtime(&runtime);
        return 1;
    }
    driver.state = &state;
    driver.runtime = runtime;
    if (pthread_create(&run_thread, NULL, watchdog_overlap_run_typed, &driver) != 0 ||
        pthread_create(&monitor_thread, NULL, watchdog_overlap_monitor, &driver) != 0) {
        test_fail(&state.failure, "watchdog overlap thread setup failed");
        atomic_store_explicit(&state.monitor_stop, 1U, memory_order_release);
        goto cleanup;
    }
    run_created = true;
    monitor_created = true;
    if (wait_flag(&state.task_started, legacy_deadline()) != 0 ||
        wait_flag(&state.task_done, legacy_deadline()) != 0 ||
        wait_flag(&state.run_done, legacy_deadline()) != 0) {
        test_fail(&state.failure, "watchdog/task-exit overlap did not complete");
        goto cleanup;
    }
    atomic_store_explicit(&state.monitor_stop, 1U, memory_order_release);
    wait_flag(&state.monitor_done, legacy_deadline());
    if (state.run_rc != 0 ||
        atomic_load_explicit(&runtime->fatal_errno, memory_order_acquire) != 0) {
        test_fail(&state.failure, "watchdog/task-exit overlap changed fatal semantics");
    }
    {
        llam_shard_t *shard = &runtime->shards[0];

        pthread_mutex_lock(&shard->opaque_lock);
        if (shard->opaque_helper_thread_started || shard->opaque_helper_ready ||
            shard->opaque_helper_active) {
            test_fail(&state.failure, "watchdog/task-exit overlap created a helper unexpectedly");
        }
        pthread_mutex_unlock(&shard->opaque_lock);
    }
    assert_snapshot(runtime, 0U, &state.failure, "watchdog overlap snapshot failed");
    rc = failure_result(&state.failure, "watchdog/task-exit overlap characterization failed");

cleanup:
    atomic_store_explicit(&state.monitor_stop, 1U, memory_order_release);
    if (monitor_created) {
        wait_flag(&state.monitor_done, legacy_deadline());
        pthread_join(monitor_thread, NULL);
    }
    if (run_created) {
        wait_flag(&state.run_done, legacy_deadline());
        pthread_join(run_thread, NULL);
    }
    destroy_runtime(&runtime);
    return rc != 0 ? rc : failure_result(&state.failure, "watchdog/task-exit overlap characterization failed");
}

static int test_partial_helper_start_failure(void) {
    test_failure_state_t failure;
    task_status_t status;
    llam_runtime_t *runtime = NULL;
    llam_task_t *task = NULL;
    llam_shard_t *shard;
    uint64_t redirect_before;
    int run_rc;
    int run_errno;
    bool helper_started;
    bool helper_ready;
    bool helper_failed;

    memset(&failure, 0, sizeof(failure));
    memset(&status, 0, sizeof(status));
    atomic_init(&failure.failures, 0U);
    atomic_init(&status.done, 0U);
    status.failure = &failure;
    reset_lease_test_gates();
    if (create_runtime(&runtime, true, false) != 0) {
        return 1;
    }
    shard = &runtime->shards[0];
    redirect_before = atomic_load_explicit(&shard->metrics.opaque_redirect_activations,
                                           memory_order_acquire);
    llam_worker_test_force_helper_create_error(EAGAIN);
    task = llam_runtime_spawn_ex(runtime, idle_helper_task, &status, NULL, 0U);
    errno = 0;
    run_rc = task == NULL ? -1 : llam_runtime_run_handle(runtime);
    run_errno = errno;
    if (task != NULL) {
        errno = 0;
        if (llam_join(task) != 0) {
            test_fail(&failure, "partial helper failure task join failed");
        }
    }
    assert_no_fatal(runtime, run_rc, run_errno, &failure,
                    "partial helper failure changed run or fatal semantics");
    pthread_mutex_lock(&shard->opaque_lock);
    helper_started = shard->opaque_helper_thread_started;
    helper_ready = shard->opaque_helper_ready;
    helper_failed = shard->opaque_helper_failed;
    pthread_mutex_unlock(&shard->opaque_lock);
    if (atomic_load_explicit(&status.done, memory_order_acquire) == 0U ||
        status.enter_rc != 0 || status.enter_errno != EAGAIN || status.leave_rc != 0 ||
        helper_started || helper_ready || !helper_failed ||
        atomic_load_explicit(&shard->metrics.opaque_redirect_activations,
                             memory_order_acquire) <= redirect_before) {
        test_fail(&failure, "partial helper-start failure did not preserve redirect semantics");
    }

    memset(&status, 0, sizeof(status));
    atomic_init(&status.done, 0U);
    status.failure = &failure;
    task = llam_runtime_spawn_ex(runtime, idle_helper_task, &status, NULL, 0U);
    errno = 0;
    run_rc = task == NULL ? -1 : llam_runtime_run_handle(runtime);
    run_errno = errno;
    if (task != NULL) {
        errno = 0;
        if (llam_join(task) != 0) {
            test_fail(&failure, "partial helper retry task join failed");
        }
    }
    assert_no_fatal(runtime, run_rc, run_errno, &failure,
                    "partial helper retry changed run or fatal semantics");
    pthread_mutex_lock(&shard->opaque_lock);
    helper_started = shard->opaque_helper_thread_started;
    helper_ready = shard->opaque_helper_ready;
    helper_failed = shard->opaque_helper_failed;
    pthread_mutex_unlock(&shard->opaque_lock);
    if (atomic_load_explicit(&status.done, memory_order_acquire) == 0U ||
        status.enter_rc != 0 || status.leave_rc != 0 ||
        !helper_started || !helper_ready || helper_failed) {
        test_fail(&failure, "partial helper-start failure was not one-shot");
    }
    assert_snapshot(runtime, 0U, &failure, "partial helper snapshot failed");
    destroy_runtime(&runtime);
    return failure_result(&failure, "partial helper-start failure characterization failed");
}

static int test_lease_test_gate_contract(void) {
    test_failure_state_t failure;
    llam_lease_test_snapshot_t snapshot;

    memset(&failure, 0, sizeof(failure));
    atomic_init(&failure.failures, 0U);
    reset_lease_test_gates();
    if (!llam_lease_test_wait_gate(LLAM_LEASE_TEST_AFTER_TRANSFER_REQUEST, 0U)) {
        test_fail(&failure, "disabled lease test gate did not pass immediately");
    }
    llam_lease_test_set_gate(LLAM_LEASE_TEST_AFTER_TRANSFER_REQUEST, true);
    if (llam_lease_test_wait_gate(LLAM_LEASE_TEST_AFTER_TRANSFER_REQUEST, 0U)) {
        test_fail(&failure, "enabled lease test gate ignored its zero timeout");
    }
    llam_lease_test_set_gate(LLAM_LEASE_TEST_AFTER_TRANSFER_REQUEST, false);
    if (!llam_lease_test_wait_gate(LLAM_LEASE_TEST_AFTER_TRANSFER_REQUEST, 0U)) {
        test_fail(&failure, "released lease test gate did not pass");
    }
    errno = 0;
    if (llam_lease_test_snapshot(NULL, 0U, &snapshot) != -1 || errno != EINVAL) {
        test_fail(&failure, "invalid lease snapshot did not fail with EINVAL");
    }
    return failure_result(&failure, "lease test-hook contract failed");
}

int main(void) {
    if (test_lease_test_gate_contract() != 0 ||
        test_primary_helper_activation_and_return() != 0 ||
        test_helper_reblocking_through_redirect() != 0 ||
        test_ownerless_return_ordering() != 0 ||
        test_repeated_shard_zero_runs_two_callers() != 0 ||
        test_helper_shutdown_while_idle() != 0 ||
        test_helper_shutdown_while_active() != 0 ||
        test_dynamic_merge_pause() != 0 ||
        test_watchdog_task_exit_overlap() != 0 ||
        test_partial_helper_start_failure() != 0) {
        return 1;
    }
    printf("test_legacy_lease_internal: all legacy lease characterization checks passed\n");
    return 0;
}
