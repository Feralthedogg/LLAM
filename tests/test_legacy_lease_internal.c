/**
 * @file tests/test_legacy_lease_internal.c
 * @brief Phase 1A characterization tests for the legacy scheduler handoff.
 *
 * These tests observe the legacy scheduler through the Phase 1A lease test
 * boundary.  The queue/timer/allocator behavior remains the legacy behavior;
 * the lease snapshots and gates make the handoff publication windows
 * deterministic enough to assert ordering.
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
#include <stdlib.h>
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
    atomic_uint release_root_leave;
    atomic_uint clean_stop_requested;
    bool hold_root_leave;
    bool request_clean_stop;
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
    uint64_t helper_incarnation;
    bool helper_binding_live;
    uintptr_t root_worker;
    uintptr_t helper_worker;
    llam_lease_test_snapshot_t root_snapshot;
    llam_lease_test_snapshot_t helper_snapshot;
    llam_lease_test_snapshot_t ownerless_redirect_snapshot;
    llam_lease_test_snapshot_t ownerless_helper_return_snapshot;
    llam_lease_test_snapshot_t ownerless_primary_return_snapshot;
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
    int fatal_errno[2];
    pthread_t drivers[2];
} repeat_run_state_t;

typedef struct repeat_driver_arg {
    repeat_run_state_t *state;
    unsigned index;
} repeat_driver_arg_t;

typedef struct lease_binding_observation {
    test_failure_state_t failure;
    llam_runtime_t *runtime;
    atomic_uint entered;
    uintptr_t worker;
    uintptr_t processor;
    uint64_t incarnation;
    uint64_t active_g;
    uint64_t last_run_started_ns;
    uint64_t last_safepoint_ns;
    uint64_t last_idle_wake_ns;
    uintptr_t native_thread;
    unsigned lease_state;
    uintptr_t lease_owner;
    uint64_t lease_owner_incarnation;
    bool binding_live;
} lease_binding_observation_t;

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
    int fatal_errno;
    pthread_t helper_thread_after;
} active_shutdown_state_t;

typedef struct invalid_handback_state {
    test_failure_state_t failure;
    llam_runtime_t *runtime;
    atomic_uint done;
    int enter_rc;
    int enter_errno;
    int leave_rc;
    int leave_errno;
    uintptr_t current_after_leave;
    uintptr_t active_g_after_leave;
    unsigned task_state_after_leave;
} invalid_handback_state_t;

typedef struct dynamic_pause_state {
    test_failure_state_t failure;
    llam_task_t *task;
    atomic_uint task_started;
    atomic_uint release_task;
    atomic_uint task_done;
    atomic_uint run_done;
    int run_rc;
    int run_errno;
    int fatal_errno;
} dynamic_pause_state_t;

typedef struct watchdog_overlap_state {
    test_failure_state_t failure;
    llam_task_t *task;
    atomic_uint task_started;
    atomic_uint task_done;
    atomic_uint monitor_stop;
    atomic_uint monitor_done;
    atomic_uint run_done;
    int run_rc;
    int run_errno;
    int fatal_errno;
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

static int wait_for_lease_gate_reached(llam_lease_test_point_t point) {
    uint64_t deadline_ns = legacy_deadline();

    while (llam_now_ns() < deadline_ns) {
        if (llam_lease_test_gate_reached(point)) {
            return 0;
        }
        llam_pause_cpu();
    }
    return -1;
}

typedef void (*cleanup_release_fn)(void *arg);

typedef struct cleanup_release_state {
    llam_runtime_t *runtime;
    atomic_uint *release_flag;
} cleanup_release_state_t;

static void fail_safe_cleanup(const char *message) {
    fprintf(stderr,
            "test_legacy_lease_internal: bounded cleanup failed: %s\n",
            message != NULL ? message : "unknown failure");
    fflush(stderr);
    _Exit(1);
}

static void release_flag_and_runtime(void *arg) {
    cleanup_release_state_t *state = arg;

    if (state == NULL) {
        return;
    }
    if (state->release_flag != NULL) {
        atomic_store_explicit(state->release_flag, 1U, memory_order_release);
    }
    if (state->runtime != NULL) {
        (void)llam_runtime_request_stop_rt(state->runtime);
    }
}

static void release_runtime_stop(void *arg) {
    llam_runtime_t *runtime = arg;

    if (runtime != NULL) {
        llam_request_stop(runtime);
    }
}

static void bounded_join_thread(pthread_t thread,
                                bool created,
                                const atomic_uint *done,
                                cleanup_release_fn release,
                                void *release_arg,
                                const char *message) {
    if (!created) {
        return;
    }
    if (wait_flag(done, legacy_deadline()) != 0) {
        if (release != NULL) {
            release(release_arg);
        }
        if (wait_flag(done, legacy_deadline()) != 0) {
            fail_safe_cleanup(message);
        }
    }
    if (pthread_join(thread, NULL) != 0) {
        fail_safe_cleanup(message);
    }
}

static void bounded_join_task(llam_task_t *task,
                              const atomic_uint *done,
                              const char *message) {
    if (task == NULL) {
        return;
    }
    if (wait_flag(done, legacy_deadline()) != 0) {
        fail_safe_cleanup(message);
    }
    if (llam_join_until(task, legacy_deadline()) != 0) {
        fail_safe_cleanup(message);
    }
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

static int create_runtime_with_preempt(llam_runtime_t **out,
                                       bool deterministic,
                                       bool dynamic_workers,
                                       uint32_t preempt_mode,
                                       uint64_t preempt_quantum_ns) {
    llam_runtime_opts_t opts;

    if (out == NULL || llam_runtime_opts_init(&opts, sizeof(opts)) != 0) {
        return -1;
    }
    opts.deterministic = deterministic ? 1U : 0U;
    opts.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    opts.preempt_mode = preempt_mode;
    opts.preempt_quantum_ns = preempt_quantum_ns;
    if (dynamic_workers) {
        opts.experimental_flags |= LLAM_RUNTIME_EXPERIMENTAL_F_DYNAMIC_WORKERS;
    }
    return llam_runtime_create(&opts, sizeof(opts), out);
}

static int create_runtime(llam_runtime_t **out, bool deterministic, bool dynamic_workers) {
    return create_runtime_with_preempt(out,
                                       deterministic,
                                       dynamic_workers,
                                       0U,
                                       0U);
}

static int snapshot_lease_locked_for_test(llam_shard_t *shard,
                                          llam_lease_test_snapshot_t *snapshot) {
    int rc;

    if (shard == NULL || snapshot == NULL) {
        return -1;
    }
    pthread_mutex_lock(&shard->opaque_lock);
    rc = llam_legacy_lease_snapshot_locked(shard, snapshot) ? 0 : -1;
    pthread_mutex_unlock(&shard->opaque_lock);
    return rc;
}

typedef struct destroy_runtime_call {
    llam_runtime_t *runtime;
    atomic_uint done;
} destroy_runtime_call_t;

static void *destroy_runtime_main(void *arg) {
    destroy_runtime_call_t *call = arg;

    llam_runtime_destroy(call->runtime);
    atomic_store_explicit(&call->done, 1U, memory_order_release);
    return NULL;
}

static void destroy_runtime(llam_runtime_t **runtime) {
    destroy_runtime_call_t call;
    pthread_t destroy_thread;

    if (runtime == NULL || *runtime == NULL) {
        return;
    }
    call.runtime = *runtime;
    atomic_init(&call.done, 0U);
    if (pthread_create(&destroy_thread, NULL, destroy_runtime_main, &call) != 0) {
        fail_safe_cleanup("runtime destroy thread could not start");
    }
    bounded_join_thread(destroy_thread,
                        true,
                        &call.done,
                        release_runtime_stop,
                        call.runtime,
                        "runtime destroy exceeded its deadline");
    *runtime = NULL;
}

static void reset_lease_test_gates(void) {
    unsigned point;

    for (point = (unsigned)LLAM_LEASE_TEST_AFTER_TRANSFER_REQUEST;
         point <= (unsigned)LLAM_LEASE_TEST_AFTER_HELPER_EXIT_ACK;
         ++point) {
        llam_lease_test_set_gate((llam_lease_test_point_t)point, false);
    }
}

static int wait_for_lease_state(llam_runtime_t *runtime,
                                unsigned shard_id,
                                unsigned expected_state,
                                uintptr_t expected_owner,
                                uint64_t minimum_sequence,
                                bool handback_ack,
                                llam_lease_test_snapshot_t *out) {
    uint64_t deadline = legacy_deadline();

    while (llam_now_ns() < deadline) {
        llam_lease_test_snapshot_t snapshot;

        if (llam_lease_test_snapshot(runtime, shard_id, &snapshot) == 0 &&
            snapshot.state == expected_state &&
            snapshot.owner_worker == expected_owner &&
            snapshot.transfer_sequence >= minimum_sequence &&
            (!handback_ack || snapshot.handback_ack_sequence >= minimum_sequence)) {
            if (out != NULL) {
                *out = snapshot;
            }
            return 0;
        }
        llam_pause_cpu();
    }
    return -1;
}

static int assert_snapshot_values(llam_runtime_t *runtime,
                                  unsigned shard_id,
                                  unsigned expected_state,
                                  uintptr_t expected_owner,
                                  uint64_t expected_incarnation,
                                  test_failure_state_t *failure,
                                  const char *message) {
    llam_lease_test_snapshot_t snapshot;

    /* The characterization calls passed zero for the old ownerless view. */
    if (expected_state == 0U) {
        expected_state = LLAM_LEASE_REDIRECTED_NO_EXECUTOR;
    }

    memset(&snapshot, 0, sizeof(snapshot));
    if (llam_lease_test_snapshot(runtime, shard_id, &snapshot) != 0 ||
        snapshot.state != expected_state ||
        snapshot.owner_worker != expected_owner ||
        (expected_owner != 0U && snapshot.owner_incarnation != expected_incarnation) ||
        (expected_owner == 0U && snapshot.owner_incarnation != 0U) ||
        (expected_owner == 0U &&
         (snapshot.return_worker != 0U || snapshot.return_incarnation != 0U))) {
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
        (run_rc < 0 && run_errno == 0) ||
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
    state->helper_incarnation = atomic_load_explicit(&shard->opaque_helper_worker.incarnation,
                                                     memory_order_acquire);
    state->helper_binding_live = llam_worker_binding_live(&shard->opaque_helper_worker,
                                                          state->helper_incarnation);
    if (!state->helper_binding_live) {
        test_fail(&state->failure,
                  "helper activation did not publish a live native binding");
    }
    if (llam_lease_test_snapshot(state->runtime, shard->id, &state->helper_snapshot) != 0 ||
        state->helper_snapshot.state != LLAM_LEASE_HELPER_OWNED ||
        state->helper_snapshot.owner_worker != (uintptr_t)&shard->opaque_helper_worker ||
        state->helper_snapshot.owner_incarnation !=
            atomic_load_explicit(&shard->opaque_helper_worker.incarnation, memory_order_acquire) ||
        state->helper_snapshot.epoch == 0U || state->helper_snapshot.return_worker != 0U ||
        state->helper_snapshot.return_incarnation != 0U ||
        state->helper_snapshot.transfer_sequence == 0U ||
        state->helper_snapshot.handback_ack_sequence != 0U) {
        test_fail(&state->failure, "helper activation snapshot did not identify the helper owner");
    }
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
    if (llam_lease_test_snapshot(state->runtime, shard->id, &state->root_snapshot) != 0 ||
        state->root_snapshot.state != LLAM_LEASE_PRIMARY_OWNED ||
        state->root_snapshot.owner_worker != (uintptr_t)&shard->primary_worker ||
        state->root_snapshot.owner_incarnation !=
            atomic_load_explicit(&shard->primary_worker.incarnation, memory_order_acquire) ||
        state->root_snapshot.epoch != 0U || state->root_snapshot.return_worker != 0U ||
        state->root_snapshot.return_incarnation != 0U ||
        state->root_snapshot.transfer_sequence != 0U ||
        state->root_snapshot.handback_ack_sequence != 0U) {
        test_fail(&state->failure, "primary activation snapshot did not identify the primary owner");
    }
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
        (void)llam_runtime_request_stop_rt(state->runtime);
        fail_safe_cleanup("helper activation task did not complete");
    }
    if (state->hold_root_leave &&
        state->request_clean_stop) {
        llam_lease_test_set_gate(LLAM_LEASE_TEST_AFTER_HANDBACK_ACK, true);
        if (llam_runtime_request_stop_rt(state->runtime) != 0) {
            test_fail(&state->failure, "clean stop request failed before handback");
        }
        atomic_store_explicit(&state->clean_stop_requested, 1U, memory_order_release);
    }
    if (state->hold_root_leave &&
        wait_flag(&state->release_root_leave, deadline) != 0) {
        (void)llam_runtime_request_stop_rt(state->runtime);
        fail_safe_cleanup("root handback release did not arrive");
    }
    errno = 0;
    state->root_leave_rc = llam_leave_blocking();
    state->root_leave_errno = errno;
    if (state->root_leave_rc != 0) {
        test_fail(&state->failure, "primary helper handback failed");
    }
    bounded_join_task(helper_task,
                      &state->helper_done,
                      "helper activation task join exceeded its deadline");
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
    if (assert_snapshot_values(runtime,
                               0U,
                               0U,
                               0U,
                               0U,
                               &state.failure,
                               "activation snapshot before run failed") != 0) {
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
        bounded_join_task(root,
                          &state.root_done,
                          "primary activation task join exceeded its deadline");
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
    if (pthread_equal(state.helper_thread, shard->opaque_helper_thread) == 0 ||
        !shard->opaque_helper_thread_started || !shard->opaque_helper_ready ||
        shard->opaque_helper_failed || shard->opaque_helper_active) {
        test_fail(&state.failure, "primary/helper activation created or retained an invalid helper");
    }
    state.helper_thread_after = shard->opaque_helper_thread;
    pthread_mutex_unlock(&shard->opaque_lock);
    if (pthread_equal(state.helper_thread, state.helper_thread_after) == 0 ||
        state.root_worker == 0U || state.helper_worker == 0U ||
        state.root_worker == state.helper_worker) {
        test_fail(&state.failure, "primary/helper activation did not preserve one helper identity");
    }
    assert_snapshot_values(runtime,
                           0U,
                           0U,
                           0U,
                           0U,
                           &state.failure,
                           "activation snapshot after run failed");
    if (!state.helper_binding_live ||
        !llam_worker_binding_live(&shard->opaque_helper_worker,
                                  state.helper_incarnation)) {
        test_fail(&state.failure,
                  "helper binding was not live before its native thread joined");
    }
    if (atomic_load_explicit(&state.helper_started, memory_order_acquire) != 0U) {
        pthread_mutex_lock(&shard->opaque_lock);
        shard->opaque_helper_stop = true;
        llam_legacy_lease_begin_stop_locked(shard);
        llam_opaque_wake_signal(shard);
        pthread_mutex_unlock(&shard->opaque_lock);
        llam_kick_shard(shard);
        if (pthread_join(state.helper_thread, NULL) != 0) {
            test_fail(&state.failure, "helper native thread join failed");
        }
        pthread_mutex_lock(&shard->opaque_lock);
        shard->opaque_helper_thread_started = false;
        shard->opaque_helper_ready = false;
        shard->opaque_helper_active = false;
        pthread_mutex_unlock(&shard->opaque_lock);
        if (llam_worker_binding_live(&shard->opaque_helper_worker,
                                     state.helper_incarnation)) {
            test_fail(&state.failure,
                      "helper binding remained live after native thread join");
        }
    }
    destroy_runtime(&runtime);
    return failure_result(&state.failure, "primary/helper activation characterization failed");
}

typedef struct gated_handoff_run_state {
    takeover_state_t takeover;
    atomic_uint run_done;
    int run_rc;
    int run_errno;
    int fatal_errno;
} gated_handoff_run_state_t;

static void *gated_handoff_run(void *arg) {
    gated_handoff_run_state_t *state = arg;

    errno = 0;
    state->run_rc = llam_runtime_run_handle(state->takeover.runtime);
    state->run_errno = errno;
    state->fatal_errno = atomic_load_explicit(&state->takeover.runtime->fatal_errno,
                                              memory_order_acquire);
    atomic_store_explicit(&state->run_done, 1U, memory_order_release);
    return NULL;
}

static int test_lease_handoff_windows(void) {
    gated_handoff_run_state_t state;
    llam_runtime_t *runtime = NULL;
    llam_task_t *root = NULL;
    llam_shard_t *shard;
    llam_lease_test_snapshot_t snapshot;
    pthread_t run_thread;
    bool run_created = false;
    uint64_t transfer_sequence;
    int rc;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.takeover.failure.failures, 0U);
    atomic_init(&state.takeover.root_done, 0U);
    atomic_init(&state.takeover.helper_started, 0U);
    atomic_init(&state.takeover.helper_done, 0U);
    atomic_init(&state.takeover.helper_entered, 0U);
    atomic_init(&state.takeover.release_helper, 0U);
    atomic_init(&state.takeover.release_root_leave, 0U);
    state.takeover.hold_root_leave = true;
    atomic_init(&state.run_done, 0U);
    memset(&snapshot, 0, sizeof(snapshot));
    transfer_sequence = 0U;
    reset_lease_test_gates();
    if (create_runtime(&runtime, true, false) != 0) {
        return 1;
    }
    state.takeover.runtime = runtime;
    shard = &runtime->shards[0];
    root = llam_runtime_spawn_ex(runtime, takeover_root_task, &state.takeover, NULL, 0U);
    if (root == NULL) {
        test_fail(&state.takeover.failure, "lease-window root task spawn failed");
        destroy_runtime(&runtime);
        return failure_result(&state.takeover.failure,
                              "lease-window characterization failed");
    }

    /* Each gate is released only after its lease snapshot is observable. */
    llam_lease_test_set_gate(LLAM_LEASE_TEST_AFTER_TRANSFER_REQUEST, true);
    llam_lease_test_set_gate(LLAM_LEASE_TEST_AFTER_OWNER_CLEAR, true);
    llam_lease_test_set_gate(LLAM_LEASE_TEST_AFTER_GRANT, true);
    if (pthread_create(&run_thread, NULL, gated_handoff_run, &state) != 0) {
        test_fail(&state.takeover.failure, "lease-window run thread setup failed");
        reset_lease_test_gates();
        destroy_runtime(&runtime);
        return failure_result(&state.takeover.failure,
                              "lease-window characterization failed");
    }
    run_created = true;

    if (wait_for_lease_state(runtime,
                             0U,
                             LLAM_LEASE_TRANSFER_TO_HELPER,
                             (uintptr_t)&shard->primary_worker,
                             0U,
                             false,
                             &snapshot) != 0 ||
        snapshot.owner_incarnation == 0U || snapshot.transfer_sequence == 0U) {
        test_fail(&state.takeover.failure,
                  "transfer-request gate did not retain the primary owner and sequence");
    }
    if (wait_for_lease_gate_reached(LLAM_LEASE_TEST_AFTER_TRANSFER_REQUEST) != 0) {
        test_fail(&state.takeover.failure,
                  "transfer-request gate was not reached by production code");
    }
    if (snapshot.epoch != 0U || snapshot.return_worker != 0U ||
        snapshot.return_incarnation != 0U || snapshot.handback_ack_sequence != 0U ||
        snapshot.transfer_sequence != 1U) {
        test_fail(&state.takeover.failure,
                  "transfer-request gate published an unexpected lease episode");
    }
    transfer_sequence = snapshot.transfer_sequence;
    llam_lease_test_set_gate(LLAM_LEASE_TEST_AFTER_TRANSFER_REQUEST, false);
    if (wait_for_lease_state(runtime,
                             0U,
                             LLAM_LEASE_TRANSFER_TO_HELPER,
                             0U,
                             transfer_sequence,
                             false,
                             &snapshot) != 0) {
        test_fail(&state.takeover.failure,
                  "owner-clear gate did not retire the primary before grant");
    }
    if (wait_for_lease_gate_reached(LLAM_LEASE_TEST_AFTER_OWNER_CLEAR) != 0) {
        test_fail(&state.takeover.failure,
                  "owner-clear gate was not reached by production code");
    }
    if (snapshot.epoch != 0U || snapshot.transfer_sequence != transfer_sequence ||
        snapshot.return_worker != 0U || snapshot.handback_ack_sequence != 0U) {
        test_fail(&state.takeover.failure,
                  "owner-clear gate published an unexpected lease episode");
    }
    pthread_mutex_lock(&shard->opaque_lock);
    if (llam_legacy_lease_can_reacquire_locked(
            shard,
            &shard->primary_worker,
            atomic_load_explicit(&shard->primary_worker.incarnation,
                                 memory_order_acquire)) ||
        llam_legacy_lease_can_reacquire_locked(
            shard,
            &shard->opaque_helper_worker,
            atomic_load_explicit(&shard->opaque_helper_worker.incarnation,
                                 memory_order_acquire))) {
        test_fail(&state.takeover.failure,
                  "ownerless transfer allowed an arbitrary worker to claim the lease");
    }
    pthread_mutex_unlock(&shard->opaque_lock);
    llam_lease_test_set_gate(LLAM_LEASE_TEST_AFTER_OWNER_CLEAR, false);
    if (wait_for_lease_state(runtime,
                             0U,
                             LLAM_LEASE_HELPER_OWNED,
                             (uintptr_t)&shard->opaque_helper_worker,
                             transfer_sequence,
                             false,
                             &snapshot) != 0 ||
        snapshot.epoch == 0U || snapshot.owner_incarnation == 0U) {
        test_fail(&state.takeover.failure,
                  "grant gate published helper before the primary clear was complete");
    }
    if (wait_for_lease_gate_reached(LLAM_LEASE_TEST_AFTER_GRANT) != 0) {
        test_fail(&state.takeover.failure,
                  "grant gate was not reached by production code");
    }
    if (snapshot.transfer_sequence != transfer_sequence || snapshot.epoch != 1U ||
        snapshot.return_worker != 0U || snapshot.handback_ack_sequence != 0U) {
        test_fail(&state.takeover.failure,
                  "grant gate published an unexpected lease episode");
    }
    llam_lease_test_set_gate(LLAM_LEASE_TEST_AFTER_GRANT, false);
    if (wait_flag(&state.takeover.helper_done, legacy_deadline()) != 0) {
        test_fail(&state.takeover.failure,
                  "helper did not complete its ownerless redirect handback");
    }
    if (llam_lease_test_snapshot(runtime, 0U, &snapshot) != 0 ||
        snapshot.state != LLAM_LEASE_HELPER_OWNED ||
        snapshot.owner_worker != (uintptr_t)&shard->opaque_helper_worker ||
        snapshot.return_worker != 0U || snapshot.return_incarnation != 0U ||
        snapshot.transfer_sequence != 3U || snapshot.handback_ack_sequence != 3U ||
        snapshot.epoch != 2U) {
        test_fail(&state.takeover.failure,
                  "nested helper redirect handback did not complete one exact episode");
    }
    llam_lease_test_set_gate(LLAM_LEASE_TEST_AFTER_HANDBACK_ACK, true);
    atomic_store_explicit(&state.takeover.release_root_leave, 1U, memory_order_release);
    if (wait_for_lease_state(runtime,
                             0U,
                             LLAM_LEASE_PRIMARY_OWNED,
                             (uintptr_t)&shard->primary_worker,
                             transfer_sequence,
                             true,
                             &snapshot) != 0 ||
        snapshot.epoch < 2U || snapshot.handback_ack_sequence == 0U) {
        test_fail(&state.takeover.failure,
                  "handback gate did not publish an acknowledgement before return");
    }
    if (wait_for_lease_gate_reached(LLAM_LEASE_TEST_AFTER_HANDBACK_ACK) != 0) {
        test_fail(&state.takeover.failure,
                  "handback gate was not reached by production code");
    }
    if (snapshot.transfer_sequence != 4U || snapshot.handback_ack_sequence != 4U ||
        snapshot.epoch != 3U || snapshot.return_worker != 0U ||
        snapshot.return_incarnation != 0U) {
        test_fail(&state.takeover.failure,
                  "handback gate published an unexpected lease episode");
    }
    pthread_mutex_lock(&shard->opaque_lock);
    if (!shard->opaque_helper_active || shard->opaque_compensation_depth != 0U) {
        test_fail(&state.takeover.failure,
                  "helper handback released active state before its acknowledgement gate");
    }
    pthread_mutex_lock(&shard->lock);
    if (atomic_load_explicit(&shard->opaque_helper_worker.active_g,
                             memory_order_acquire) != NULL ||
        atomic_load_explicit(&shard->primary_worker.active_g,
                             memory_order_acquire) != NULL ||
        atomic_load_explicit(&shard->current, memory_order_acquire) != NULL) {
        test_fail(&state.takeover.failure,
                  "handback ACK was published before worker/current cleanup");
    }
    pthread_mutex_unlock(&shard->lock);
    pthread_mutex_unlock(&shard->opaque_lock);
    llam_lease_test_set_gate(LLAM_LEASE_TEST_AFTER_HANDBACK_ACK, false);

    reset_lease_test_gates();
    bounded_join_thread(run_thread,
                        run_created,
                        &state.run_done,
                        release_runtime_stop,
                        runtime,
                        "lease-window run exceeded its deadline");
    run_created = false;
    bounded_join_task(root,
                      &state.takeover.root_done,
                      "lease-window root join exceeded its deadline");
    if (llam_lease_test_snapshot(runtime, 0U, &snapshot) != 0 ||
        snapshot.state != LLAM_LEASE_REDIRECTED_NO_EXECUTOR ||
        snapshot.owner_worker != 0U || snapshot.owner_incarnation != 0U ||
        snapshot.return_worker != 0U || snapshot.return_incarnation != 0U ||
        snapshot.transfer_sequence != 4U || snapshot.handback_ack_sequence != 4U ||
        snapshot.epoch != 3U) {
        test_fail(&state.takeover.failure,
                  "post-run lease cleanup did not retain exactly the completed episode");
    }
    if (state.run_rc != 0 || state.fatal_errno != 0) {
        test_fail(&state.takeover.failure,
                  "lease-window transfer changed run or fatal semantics");
    }
    rc = failure_result(&state.takeover.failure,
                        "lease handoff publication windows failed");
    destroy_runtime(&runtime);
    return rc;
}

static int test_clean_stop_completes_handback(void) {
    gated_handoff_run_state_t state;
    llam_runtime_t *runtime = NULL;
    llam_task_t *root = NULL;
    llam_shard_t *shard;
    llam_lease_test_snapshot_t snapshot;
    pthread_t run_thread;
    bool run_created = false;
    int rc = 0;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.takeover.failure.failures, 0U);
    atomic_init(&state.takeover.root_done, 0U);
    atomic_init(&state.takeover.helper_started, 0U);
    atomic_init(&state.takeover.helper_done, 0U);
    atomic_init(&state.takeover.helper_entered, 0U);
    atomic_init(&state.takeover.release_helper, 0U);
    atomic_init(&state.takeover.release_root_leave, 0U);
    atomic_init(&state.takeover.clean_stop_requested, 0U);
    state.takeover.hold_root_leave = true;
    state.takeover.request_clean_stop = true;
    atomic_init(&state.run_done, 0U);
    reset_lease_test_gates();
    if (create_runtime(&runtime, true, false) != 0) {
        return 1;
    }
    state.takeover.runtime = runtime;
    shard = &runtime->shards[0];
    root = llam_runtime_spawn_ex(runtime, takeover_root_task, &state.takeover, NULL, 0U);
    if (root == NULL || pthread_create(&run_thread, NULL, gated_handoff_run, &state) != 0) {
        test_fail(&state.takeover.failure, "clean-stop handback run setup failed");
        reset_lease_test_gates();
        destroy_runtime(&runtime);
        return 1;
    }
    run_created = true;

    if (wait_flag(&state.takeover.clean_stop_requested, legacy_deadline()) != 0) {
        test_fail(&state.takeover.failure,
                  "clean stop was not requested while the helper lease was live");
    }
    if (llam_lease_test_snapshot(runtime, 0U, &snapshot) != 0 ||
        snapshot.state != LLAM_LEASE_HELPER_OWNED ||
        snapshot.owner_worker != (uintptr_t)&shard->opaque_helper_worker ||
        snapshot.return_worker != 0U || snapshot.return_incarnation != 0U ||
        snapshot.transfer_sequence != 3U || snapshot.handback_ack_sequence != 3U ||
        snapshot.epoch != 2U) {
        test_fail(&state.takeover.failure,
                  "clean stop changed the recorded helper lease before leave");
    }

    atomic_store_explicit(&state.takeover.release_root_leave, 1U, memory_order_release);
    if (wait_for_lease_gate_reached(LLAM_LEASE_TEST_AFTER_HANDBACK_ACK) != 0) {
        test_fail(&state.takeover.failure,
                  "clean-stop handback did not reach its enabled acknowledgement gate");
    } else {
        bool handback_cleanup_ok;

        pthread_mutex_lock(&shard->opaque_lock);
        pthread_mutex_lock(&shard->lock);
        handback_cleanup_ok = llam_legacy_lease_snapshot_locked(shard, &snapshot) &&
                              snapshot.state == LLAM_LEASE_PRIMARY_OWNED &&
                              snapshot.owner_worker == (uintptr_t)&shard->primary_worker &&
                              snapshot.return_worker == 0U &&
                              snapshot.return_incarnation == 0U &&
                              snapshot.transfer_sequence == 4U &&
                              snapshot.handback_ack_sequence == 4U &&
                              snapshot.epoch == 3U &&
                              atomic_load_explicit(&shard->primary_worker.active_g,
                                                   memory_order_acquire) == NULL &&
                              atomic_load_explicit(&shard->opaque_helper_worker.active_g,
                                                   memory_order_acquire) == NULL &&
                              atomic_load_explicit(&shard->current,
                                                   memory_order_acquire) == NULL;
        pthread_mutex_unlock(&shard->lock);
        pthread_mutex_unlock(&shard->opaque_lock);
        if (!handback_cleanup_ok) {
            test_fail(&state.takeover.failure,
                      "clean-stop handback ACK was not after worker/current cleanup");
        }
    }
    llam_lease_test_set_gate(LLAM_LEASE_TEST_AFTER_HANDBACK_ACK, false);

    bounded_join_thread(run_thread,
                        run_created,
                        &state.run_done,
                        release_runtime_stop,
                        runtime,
                        "clean-stop handback run exceeded its deadline");
    run_created = false;
    bounded_join_task(root,
                      &state.takeover.root_done,
                      "clean-stop handback root join exceeded its deadline");
    if (state.run_rc != 0 || state.fatal_errno != 0) {
        test_fail(&state.takeover.failure,
                  "clean stop handback changed run or fatal semantics");
    }
    if (llam_lease_test_snapshot(runtime, 0U, &snapshot) != 0 ||
        snapshot.state != LLAM_LEASE_REDIRECTED_NO_EXECUTOR ||
        snapshot.owner_worker != 0U || snapshot.owner_incarnation != 0U ||
        snapshot.return_worker != 0U || snapshot.return_incarnation != 0U ||
        snapshot.transfer_sequence != 4U || snapshot.handback_ack_sequence != 4U ||
        snapshot.epoch != 3U) {
        test_fail(&state.takeover.failure,
                  "clean-stop handback left stale lease ownership or claimant state");
    }
    rc = failure_result(&state.takeover.failure,
                        "clean cooperative stop handback characterization failed");
    reset_lease_test_gates();
    if (run_created) {
        bounded_join_thread(run_thread,
                            true,
                            &state.run_done,
                            release_runtime_stop,
                            runtime,
                            "clean-stop handback cleanup exceeded its deadline");
    }
    destroy_runtime(&runtime);
    return rc;
}

static void invalid_handback_task(void *arg) {
    invalid_handback_state_t *state = arg;
    llam_shard_t *shard = g_llam_tls_shard;

    errno = 0;
    state->enter_rc = llam_enter_blocking();
    state->enter_errno = errno;
    if (state->enter_rc == 0 && shard != NULL) {
        /* Invalidate the helper incarnation before the return boundary. */
        pthread_mutex_lock(&shard->opaque_lock);
        atomic_store_explicit(&shard->opaque_helper_worker.native_thread_live,
                              false,
                              memory_order_release);
        atomic_store_explicit(&shard->opaque_helper_worker.binding_incarnation,
                              0U,
                              memory_order_release);
        pthread_mutex_unlock(&shard->opaque_lock);
    }
    errno = 0;
    state->leave_rc = llam_leave_blocking();
    state->leave_errno = errno;
    state->current_after_leave = shard != NULL
                                     ? (uintptr_t)atomic_load_explicit(
                                           &shard->current,
                                           memory_order_acquire)
                                     : 0U;
    state->active_g_after_leave = shard != NULL
                                      ? (uintptr_t)atomic_load_explicit(
                                            &shard->primary_worker.active_g,
                                            memory_order_acquire)
                                      : 0U;
    state->task_state_after_leave = g_llam_tls_task != NULL
                                        ? (unsigned)g_llam_tls_task->state
                                        : 0U;
    atomic_store_explicit(&state->done, 1U, memory_order_release);
}

static int test_failed_handback_does_not_republish(void) {
    invalid_handback_state_t state;
    llam_runtime_t *runtime = NULL;
    llam_task_t *task = NULL;
    int run_rc;
    int run_errno;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failure.failures, 0U);
    atomic_init(&state.done, 0U);
    reset_lease_test_gates();
    if (create_runtime(&runtime, true, false) != 0) {
        return 1;
    }
    state.runtime = runtime;
    task = llam_runtime_spawn_ex(runtime, invalid_handback_task, &state, NULL, 0U);
    errno = 0;
    run_rc = task == NULL ? -1 : llam_runtime_run_handle(runtime);
    run_errno = errno;
    if (task != NULL) {
        bounded_join_task(task,
                          &state.done,
                          "invalid handback task join exceeded its deadline");
    }
    assert_no_fatal(runtime,
                    run_rc,
                    run_errno,
                    &state.failure,
                    "failed handback changed run or fatal semantics");
    if (state.enter_rc != 0 || state.leave_rc != -1 || state.leave_errno == 0 ||
        state.current_after_leave != 0U || state.active_g_after_leave != 0U ||
        state.task_state_after_leave != LLAM_TASK_STATE_BLOCKED_OPAQUE) {
        test_fail(&state.failure,
                  "failed handback validation republished current/RUNNING state");
    }
    destroy_runtime(&runtime);
    return failure_result(&state.failure,
                          "failed handback republish regression failed");
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
    {
        llam_lease_test_snapshot_t snapshot;
        uint64_t helper_incarnation = atomic_load_explicit(
            &shard->opaque_helper_worker.incarnation,
            memory_order_acquire);

        if (llam_lease_test_snapshot(state->runtime, shard->id, &snapshot) != 0 ||
            snapshot.state != LLAM_LEASE_REDIRECTED_NO_EXECUTOR ||
            snapshot.owner_worker != 0U ||
            snapshot.return_worker != (uintptr_t)&shard->opaque_helper_worker ||
            snapshot.return_incarnation != helper_incarnation ||
            snapshot.transfer_sequence == 0U) {
            test_fail(&state->failure,
                      "helper re-blocking did not retain its lease return claimant");
        }
    }
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
        atomic_store_explicit(&state->release_helper, 1U, memory_order_release);
        (void)llam_runtime_request_stop_rt(state->runtime);
        fail_safe_cleanup("helper redirect task did not return");
    }
    errno = 0;
    state->root_leave_rc = llam_leave_blocking();
    state->root_leave_errno = errno;
    if (state->root_leave_rc != 0) {
        test_fail(&state->failure, "redirect root handback failed");
    }
    bounded_join_task(helper_task,
                      &state->helper_done,
                      "redirect helper join exceeded its deadline");
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
        bounded_join_task(root,
                          &state.root_done,
                          "redirect root join exceeded its deadline");
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
    if (!shard->opaque_helper_thread_started ||
        pthread_equal(state.helper_thread, shard->opaque_helper_thread) == 0 ||
        !shard->opaque_helper_ready || shard->opaque_helper_failed) {
        test_fail(&state.failure, "helper re-blocking created a second or invalid helper");
    }
    pthread_mutex_unlock(&shard->opaque_lock);
    assert_snapshot_values(runtime,
                           0U,
                           0U,
                           0U,
                           0U,
                           &state.failure,
                           "redirect snapshot after run failed");
    destroy_runtime(&runtime);
    return failure_result(&state.failure, "helper re-blocking characterization failed");
}

static void ownerless_helper_task(void *arg) {
    takeover_state_t *state = arg;
    llam_shard_t *shard = g_llam_tls_shard;

    errno = 0;
    state->helper_enter_rc = llam_enter_blocking();
    state->helper_enter_errno = errno;
    state->helper_worker = (uintptr_t)g_llam_tls_worker;
    state->helper_incarnation = atomic_load_explicit(&shard->opaque_helper_worker.incarnation,
                                                     memory_order_acquire);
    if (llam_lease_test_snapshot(state->runtime,
                                 shard->id,
                                 &state->ownerless_redirect_snapshot) != 0 ||
        state->ownerless_redirect_snapshot.state != LLAM_LEASE_REDIRECTED_NO_EXECUTOR ||
        state->ownerless_redirect_snapshot.owner_worker != 0U ||
        state->ownerless_redirect_snapshot.owner_incarnation != 0U ||
        state->ownerless_redirect_snapshot.epoch != 1U ||
        state->ownerless_redirect_snapshot.return_worker !=
            (uintptr_t)&shard->opaque_helper_worker ||
        state->ownerless_redirect_snapshot.return_incarnation !=
            state->helper_incarnation ||
        state->ownerless_redirect_snapshot.transfer_sequence != 2U ||
        state->ownerless_redirect_snapshot.transfer_sequence == 0U ||
        state->ownerless_redirect_snapshot.epoch == 0U ||
        state->ownerless_redirect_snapshot.handback_ack_sequence != 0U) {
        test_fail(&state->failure,
                  "ownerless redirect did not retain the exact helper return token");
    }
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
        atomic_store_explicit(&state->release_helper, 1U, memory_order_release);
        (void)llam_runtime_request_stop_rt(state->runtime);
        fail_safe_cleanup("ownerless helper did not complete its return");
    }
    pthread_mutex_lock(&shard->lock);
    if (redirect_active_before == 0U || redirect_depth_before == 0U ||
        shard->opaque_redirect_active) {
        test_fail(&state->failure, "ownerless return ordering released redirect out of order");
    }
    pthread_mutex_unlock(&shard->lock);
    if (llam_lease_test_snapshot(state->runtime,
                                 shard->id,
                                 &state->ownerless_helper_return_snapshot) != 0 ||
        state->ownerless_helper_return_snapshot.state != LLAM_LEASE_HELPER_OWNED ||
        state->ownerless_helper_return_snapshot.owner_worker !=
            (uintptr_t)&shard->opaque_helper_worker ||
        state->ownerless_helper_return_snapshot.owner_incarnation !=
            state->helper_incarnation ||
        state->ownerless_helper_return_snapshot.epoch != 2U ||
        state->ownerless_helper_return_snapshot.transfer_sequence != 3U ||
        state->ownerless_helper_return_snapshot.handback_ack_sequence != 3U ||
        state->ownerless_helper_return_snapshot.return_worker != 0U ||
        state->ownerless_helper_return_snapshot.return_incarnation != 0U) {
        test_fail(&state->failure,
                  "ownerless helper return did not complete its exact handback episode");
    }
    errno = 0;
    state->root_leave_rc = llam_leave_blocking();
    state->root_leave_errno = errno;
    if (state->root_leave_rc != 0) {
        test_fail(&state->failure, "ownerless primary return failed");
    }
    if (llam_lease_test_snapshot(state->runtime,
                                 shard->id,
                                 &state->ownerless_primary_return_snapshot) != 0 ||
        state->ownerless_primary_return_snapshot.state != LLAM_LEASE_PRIMARY_OWNED ||
        state->ownerless_primary_return_snapshot.owner_worker !=
            (uintptr_t)&shard->primary_worker ||
        state->ownerless_primary_return_snapshot.owner_incarnation == 0U ||
        state->ownerless_primary_return_snapshot.epoch != 3U ||
        state->ownerless_primary_return_snapshot.transfer_sequence != 4U ||
        state->ownerless_primary_return_snapshot.handback_ack_sequence != 4U ||
        state->ownerless_primary_return_snapshot.return_worker != 0U ||
        state->ownerless_primary_return_snapshot.return_incarnation != 0U) {
        test_fail(&state->failure,
                  "ownerless primary return did not complete its exact handback episode");
    }
    bounded_join_task(helper_task,
                      &state->helper_done,
                      "ownerless helper join exceeded its deadline");
    atomic_store_explicit(&state->root_done, 1U, memory_order_release);
}

static int test_ownerless_return_ordering(void) {
    takeover_state_t state;
    llam_runtime_t *runtime = NULL;
    llam_task_t *root = NULL;
    llam_shard_t *shard;
    llam_lease_test_snapshot_t final_snapshot;
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
        bounded_join_task(root,
                          &state.root_done,
                          "ownerless root join exceeded its deadline");
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
        pthread_equal(state.helper_thread, shard->opaque_helper_thread) == 0) {
        test_fail(&state.failure, "ownerless return did not preserve one helper lifecycle");
    }
    pthread_mutex_unlock(&shard->opaque_lock);
    if (llam_lease_test_snapshot(runtime, 0U, &final_snapshot) != 0 ||
        final_snapshot.state != LLAM_LEASE_REDIRECTED_NO_EXECUTOR ||
        final_snapshot.owner_worker != 0U ||
        final_snapshot.owner_incarnation != 0U ||
        final_snapshot.return_worker != 0U ||
        final_snapshot.return_incarnation != 0U ||
        final_snapshot.epoch != 3U ||
        final_snapshot.transfer_sequence != 4U ||
        final_snapshot.handback_ack_sequence != 4U) {
        test_fail(&state.failure,
                  "ownerless snapshot after run did not preserve exact sequence/epoch");
    }
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
    state->fatal_errno[index] = atomic_load_explicit(&state->runtime->fatal_errno,
                                                     memory_order_acquire);
    atomic_store_explicit(&state->run_done[index], 1U, memory_order_release);
    return NULL;
}

static int test_repeated_shard_zero_runs_two_callers(void) {
    repeat_run_state_t state;
    llam_runtime_t *runtime = NULL;
    llam_task_t *tasks[2] = {NULL, NULL};
    repeat_driver_arg_t drivers[2];
    cleanup_release_state_t release;
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
    release.runtime = runtime;
    release.release_flag = NULL;
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
        bounded_join_thread(state.drivers[i],
                            true,
                            &state.run_done[i],
                            release_flag_and_runtime,
                            &release,
                            "repeated shard-zero run thread exceeded its deadline");
        if (state.run_rc[i] != 0 ||
            (state.run_rc[i] < 0 && state.run_errno[i] == 0) ||
            state.fatal_errno[i] != 0) {
            test_fail(&state.failure, "repeated shard-zero run changed fatal semantics");
            rc = 1;
        }
        if (tasks[i] != NULL) {
            bounded_join_task(tasks[i],
                              &state.task_done[i],
                              "repeated shard-zero task join exceeded its deadline");
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
    if (assert_snapshot_values(runtime,
                               0U,
                               0U,
                               0U,
                               0U,
                               &state.failure,
                               "repeat snapshot after runs failed") != 0) {
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
    destroy_runtime_call_t destroy_call;
    llam_runtime_t *runtime = NULL;
    llam_task_t *task = NULL;
    llam_shard_t *shard;
    pthread_t destroy_thread;
    bool destroy_created = false;
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
        bounded_join_task(task,
                          &status.done,
                          "idle helper task join exceeded its deadline");
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
    assert_snapshot_values(runtime,
                           0U,
                           0U,
                           0U,
                           0U,
                           &failure,
                           "idle helper snapshot failed");
    destroy_call.runtime = runtime;
    atomic_init(&destroy_call.done, 0U);
    llam_lease_test_set_gate(LLAM_LEASE_TEST_AFTER_HELPER_EXIT_ACK, true);
    if (pthread_create(&destroy_thread, NULL, destroy_runtime_main, &destroy_call) != 0) {
        test_fail(&failure, "idle helper destroy thread setup failed");
        llam_lease_test_set_gate(LLAM_LEASE_TEST_AFTER_HELPER_EXIT_ACK, false);
    } else {
        destroy_created = true;
        if (wait_for_lease_gate_reached(LLAM_LEASE_TEST_AFTER_HELPER_EXIT_ACK) != 0) {
            test_fail(&failure,
                      "idle helper shutdown did not reach its enabled exit gate");
        }
        llam_lease_test_set_gate(LLAM_LEASE_TEST_AFTER_HELPER_EXIT_ACK, false);
        bounded_join_thread(destroy_thread,
                            true,
                            &destroy_call.done,
                            release_runtime_stop,
                            runtime,
                            "idle helper destroy thread exceeded its deadline");
        destroy_created = false;
        runtime = NULL;
    }
    if (destroy_created) {
        llam_lease_test_set_gate(LLAM_LEASE_TEST_AFTER_HELPER_EXIT_ACK, false);
        bounded_join_thread(destroy_thread,
                            true,
                            &destroy_call.done,
                            release_runtime_stop,
                            runtime,
                            "idle helper destroy cleanup exceeded its deadline");
        runtime = NULL;
    }
    if (runtime != NULL) {
        destroy_runtime(&runtime);
    }
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
        /* Keep the active shutdown task on its primary until the test releases it. */
        llam_pause_cpu();
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
    state->fatal_errno = atomic_load_explicit(&state->runtime->fatal_errno,
                                              memory_order_acquire);
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
    cleanup_release_state_t release;
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
    release.runtime = runtime;
    release.release_flag = &state.release_task;
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
    pthread_mutex_lock(&runtime->shards[0].opaque_lock);
    state.helper_thread_after = runtime->shards[0].opaque_helper_thread;
    pthread_mutex_unlock(&runtime->shards[0].opaque_lock);
    if (pthread_equal(state.helper_thread, state.helper_thread_after) == 0) {
        test_fail(&state.failure, "active helper lifecycle changed its helper identity");
        goto cleanup;
    }
    /* Arm before shutdown can stop the helper, so the handback window cannot race past us. */
    llam_lease_test_set_gate(LLAM_LEASE_TEST_AFTER_HANDBACK_ACK, true);
    if (pthread_create(&destroy_thread, NULL, active_shutdown_destroy, &state) != 0) {
        test_fail(&state.failure, "active helper destroy thread setup failed");
        goto cleanup;
    }
    destroy_created = true;
    if (wait_flag(&state.destroy_started, legacy_deadline()) != 0) {
        test_fail(&state.failure, "active helper destroy did not start");
        goto cleanup;
    }
    if (wait_for_lease_state(runtime,
                             0U,
                             LLAM_LEASE_STOPPING,
                             (uintptr_t)&runtime->shards[0].opaque_helper_worker,
                             1U,
                             false,
                             NULL) != 0) {
        test_fail(&state.failure,
                  "active helper destroy did not publish STOPPING before release");
        goto cleanup;
    }
    atomic_store_explicit(&state.release_task, 1U, memory_order_release);
    if (wait_for_lease_gate_reached(LLAM_LEASE_TEST_AFTER_HANDBACK_ACK) != 0) {
        test_fail(&state.failure,
                  "active helper shutdown did not reach its enabled handback gate");
    } else {
        llam_lease_test_snapshot_t snapshot;

        llam_shard_t *shard = &runtime->shards[0];
        bool handback_cleanup_ok;

        pthread_mutex_lock(&shard->opaque_lock);
        pthread_mutex_lock(&shard->lock);
        handback_cleanup_ok = llam_legacy_lease_snapshot_locked(shard, &snapshot) &&
                              snapshot.state == LLAM_LEASE_STOPPING &&
                              snapshot.owner_worker == (uintptr_t)&shard->primary_worker &&
                              snapshot.owner_incarnation != 0U &&
                              snapshot.return_worker == (uintptr_t)&shard->primary_worker &&
                              snapshot.return_incarnation == snapshot.owner_incarnation &&
                              snapshot.transfer_sequence == 2U &&
                              snapshot.handback_ack_sequence == 2U &&
                              snapshot.epoch == 2U &&
                              atomic_load_explicit(&shard->primary_worker.active_g,
                                                   memory_order_acquire) == NULL &&
                              atomic_load_explicit(&shard->opaque_helper_worker.active_g,
                                                   memory_order_acquire) == NULL &&
                              atomic_load_explicit(&shard->current,
                                                   memory_order_acquire) == NULL;
        pthread_mutex_unlock(&shard->lock);
        pthread_mutex_unlock(&shard->opaque_lock);
        if (!handback_cleanup_ok) {
            test_fail(&state.failure,
                      "active helper shutdown handback did not retain claimant after cleanup");
        }
    }
    llam_lease_test_set_gate(LLAM_LEASE_TEST_AFTER_HANDBACK_ACK, false);
    llam_lease_test_set_gate(LLAM_LEASE_TEST_AFTER_HELPER_EXIT_ACK, true);
    if (wait_for_lease_gate_reached(LLAM_LEASE_TEST_AFTER_HELPER_EXIT_ACK) != 0) {
        test_fail(&state.failure,
                  "active helper shutdown did not reach its enabled exit gate");
    } else {
        llam_shard_t *shard = &runtime->shards[0];
        bool claimant_retained;

        pthread_mutex_lock(&shard->opaque_lock);
        claimant_retained = shard->legacy_lease.state == LLAM_LEASE_STOPPING &&
                            shard->legacy_lease.return_worker ==
                                &shard->primary_worker &&
                            shard->legacy_lease.return_incarnation ==
                                shard->legacy_lease.owner_incarnation &&
                            atomic_load_explicit(&shard->opaque_helper_exit_ack,
                                                 memory_order_acquire);
        pthread_mutex_unlock(&shard->opaque_lock);
        if (!claimant_retained) {
            test_fail(&state.failure,
                      "active helper shutdown exit ACK cleared the return claimant");
        }
    }
    llam_lease_test_set_gate(LLAM_LEASE_TEST_AFTER_HELPER_EXIT_ACK, false);
    if (wait_flag(&state.destroy_done, legacy_deadline()) != 0 ||
        wait_flag(&state.run_done, legacy_deadline()) != 0 ||
        wait_flag(&state.task_done, legacy_deadline()) != 0) {
        test_fail(&state.failure, "active helper shutdown did not complete bounded teardown");
        goto cleanup;
    }
    bounded_join_thread(destroy_thread,
                        true,
                        &state.destroy_done,
                        release_flag_and_runtime,
                        &release,
                        "active helper destroy thread exceeded its deadline");
    destroy_created = false;
    runtime = NULL;
    bounded_join_thread(run_thread,
                        true,
                        &state.run_done,
                        NULL,
                        NULL,
                        "active helper run thread exceeded its deadline");
    run_created = false;
    if (state.run_rc != 0 ||
        (state.run_rc < 0 && state.run_errno == 0) ||
        state.fatal_errno != 0) {
        test_fail(&state.failure, "active helper shutdown changed run fatal semantics");
    }
    rc = failure_result(&state.failure, "active helper shutdown characterization failed");

cleanup:
    reset_lease_test_gates();
    atomic_store_explicit(&state.release_task, 1U, memory_order_release);
    if (destroy_created) {
        release.runtime = runtime;
        bounded_join_thread(destroy_thread,
                            true,
                            &state.destroy_done,
                            release_flag_and_runtime,
                            &release,
                            "active helper destroy cleanup exceeded its deadline");
        destroy_created = false;
        runtime = NULL;
    }
    if (run_created) {
        if (runtime != NULL) {
            release.runtime = runtime;
            bounded_join_thread(run_thread,
                                true,
                                &state.run_done,
                                release_flag_and_runtime,
                                &release,
                                "active helper run cleanup exceeded its deadline");
            runtime = NULL;
        } else {
            bounded_join_thread(run_thread,
                                true,
                                &state.run_done,
                                NULL,
                                NULL,
                                "active helper run cleanup exceeded its deadline");
        }
        run_created = false;
    }
    if (runtime != NULL) {
        destroy_runtime(&runtime);
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
    driver->state->fatal_errno = atomic_load_explicit(&driver->runtime->fatal_errno,
                                                      memory_order_acquire);
    atomic_store_explicit(&driver->state->run_done, 1U, memory_order_release);
    return NULL;
}

static int test_dynamic_merge_pause(void) {
    dynamic_pause_state_t state;
    dynamic_pause_driver_t driver;
    llam_runtime_t *runtime = NULL;
    pthread_t run_thread;
    cleanup_release_state_t release;
    bool run_created = false;
    bool task_joined = false;
    bool have_online_secondary = false;
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
        fprintf(stderr, "test_legacy_lease_internal: SKIP dynamic merge pause (one shard)\n");
        destroy_runtime(&runtime);
        return 77;
    }
    for (i = 1U; i < runtime->active_shards; ++i) {
        if (llam_shard_is_online(&runtime->shards[i])) {
            have_online_secondary = true;
            break;
        }
    }
    if (!have_online_secondary) {
        fprintf(stderr,
                "test_legacy_lease_internal: SKIP dynamic merge pause (no online secondary)\n");
        destroy_runtime(&runtime);
        return 77;
    }
    state.task = llam_runtime_spawn_ex(runtime, dynamic_pause_task, &state, NULL, 0U);
    if (state.task == NULL) {
        test_fail(&state.failure, "dynamic merge pause task spawn failed");
        destroy_runtime(&runtime);
        return 1;
    }
    driver.state = &state;
    driver.runtime = runtime;
    release.runtime = runtime;
    release.release_flag = &state.release_task;
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
        fprintf(stderr,
                "test_legacy_lease_internal: SKIP dynamic merge pause (secondary went offline)\n");
        atomic_store_explicit(&state.release_task, 1U, memory_order_release);
        bounded_join_task(state.task,
                          &state.task_done,
                          "dynamic merge skip task join exceeded its deadline");
        task_joined = true;
        bounded_join_thread(run_thread,
                            true,
                            &state.run_done,
                            NULL,
                            NULL,
                            "dynamic merge skip run thread exceeded its deadline");
        run_created = false;
        destroy_runtime(&runtime);
        return 77;
    }
    atomic_store_explicit(&state.release_task, 1U, memory_order_release);
    if (wait_flag(&state.task_done, legacy_deadline()) != 0 ||
        wait_flag(&state.run_done, legacy_deadline()) != 0) {
        test_fail(&state.failure, "dynamic merge pause task/run did not complete");
        goto cleanup;
    }
    bounded_join_task(state.task,
                      &state.task_done,
                      "dynamic merge pause task join exceeded its deadline");
    task_joined = true;
    /* A successful run has no errno postcondition; fatal_errno is authoritative. */
    if (state.run_rc != 0 ||
        (state.run_rc < 0 && state.run_errno == 0) ||
        state.fatal_errno != 0) {
        test_fail(&state.failure, "dynamic merge pause changed run fatal semantics");
    }
    assert_snapshot_values(runtime,
                           0U,
                           0U,
                           0U,
                           0U,
                           &state.failure,
                           "dynamic merge snapshot failed");
    bounded_join_thread(run_thread,
                        true,
                        &state.run_done,
                        NULL,
                        NULL,
                        "dynamic merge pause run thread exceeded its deadline");
    run_created = false;
    rc = failure_result(&state.failure, "dynamic merge pause characterization failed");

cleanup:
    atomic_store_explicit(&state.release_task, 1U, memory_order_release);
    if (run_created) {
        bounded_join_thread(run_thread,
                            true,
                            &state.run_done,
                            release_flag_and_runtime,
                            &release,
                            "dynamic merge pause run cleanup exceeded its deadline");
        run_created = false;
    }
    if (!task_joined && state.task != NULL) {
        bounded_join_task(state.task,
                          &state.task_done,
                          "dynamic merge pause task cleanup exceeded its deadline");
        task_joined = true;
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
    driver->state->fatal_errno = atomic_load_explicit(&driver->runtime->fatal_errno,
                                                      memory_order_acquire);
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
    cleanup_release_state_t run_release;
    cleanup_release_state_t monitor_release;
    bool run_created = false;
    bool monitor_created = false;
    bool task_joined = false;
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
    if (create_runtime_with_preempt(&runtime,
                                    true,
                                    false,
                                    LLAM_PREEMPT_STRICT,
                                    1U) != 0) {
        return 1;
    }
    state.task = llam_runtime_spawn_ex(runtime, watchdog_overlap_task, &state, NULL, 0U);
    if (state.task == NULL) {
        test_fail(&state.failure, "watchdog overlap task spawn failed");
        destroy_runtime(&runtime);
        return 1;
    }
    driver.state = &state;
    driver.runtime = runtime;
    run_release.runtime = runtime;
    run_release.release_flag = NULL;
    monitor_release.runtime = runtime;
    monitor_release.release_flag = &state.monitor_stop;
    if (pthread_create(&run_thread, NULL, watchdog_overlap_run_typed, &driver) != 0) {
        test_fail(&state.failure, "watchdog overlap run thread setup failed");
        goto cleanup;
    }
    run_created = true;
    if (pthread_create(&monitor_thread, NULL, watchdog_overlap_monitor, &driver) != 0) {
        test_fail(&state.failure, "watchdog overlap monitor thread setup failed");
        goto cleanup;
    }
    monitor_created = true;
    if (wait_flag(&state.task_started, legacy_deadline()) != 0 ||
        wait_flag(&state.task_done, legacy_deadline()) != 0 ||
        wait_flag(&state.run_done, legacy_deadline()) != 0) {
        test_fail(&state.failure, "watchdog/task-exit overlap did not complete");
        goto cleanup;
    }
    atomic_store_explicit(&state.monitor_stop, 1U, memory_order_release);
    bounded_join_thread(monitor_thread,
                        true,
                        &state.monitor_done,
                        release_flag_and_runtime,
                        &monitor_release,
                        "watchdog overlap monitor thread exceeded its deadline");
    monitor_created = false;
    bounded_join_task(state.task,
                      &state.task_done,
                      "watchdog overlap task join exceeded its deadline");
    task_joined = true;
    /* A successful run has no errno postcondition; fatal_errno is authoritative. */
    if (state.run_rc != 0 ||
        (state.run_rc < 0 && state.run_errno == 0) ||
        state.fatal_errno != 0) {
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
    assert_snapshot_values(runtime,
                           0U,
                           0U,
                           0U,
                           0U,
                           &state.failure,
                           "watchdog overlap snapshot failed");
    bounded_join_thread(run_thread,
                        true,
                        &state.run_done,
                        NULL,
                        NULL,
                        "watchdog overlap run thread exceeded its deadline");
    run_created = false;
    rc = failure_result(&state.failure, "watchdog/task-exit overlap characterization failed");

cleanup:
    atomic_store_explicit(&state.monitor_stop, 1U, memory_order_release);
    if (monitor_created) {
        bounded_join_thread(monitor_thread,
                            true,
                            &state.monitor_done,
                            release_flag_and_runtime,
                            &monitor_release,
                            "watchdog overlap monitor cleanup exceeded its deadline");
        monitor_created = false;
    }
    if (run_created) {
        bounded_join_thread(run_thread,
                            true,
                            &state.run_done,
                            release_flag_and_runtime,
                            &run_release,
                            "watchdog overlap run cleanup exceeded its deadline");
        run_created = false;
    }
    if (!task_joined && state.task != NULL) {
        bounded_join_task(state.task,
                          &state.task_done,
                          "watchdog overlap task cleanup exceeded its deadline");
        task_joined = true;
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
        bounded_join_task(task,
                          &status.done,
                          "partial helper failure task join exceeded its deadline");
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
        bounded_join_task(task,
                          &status.done,
                          "partial helper retry task join exceeded its deadline");
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
    assert_snapshot_values(runtime,
                           0U,
                           0U,
                           0U,
                           0U,
                           &failure,
                           "partial helper snapshot failed");
    destroy_runtime(&runtime);
    return failure_result(&failure, "partial helper-start failure characterization failed");
}

static void lease_binding_observer_task(void *arg) {
    lease_binding_observation_t *observation = arg;
    llam_shard_t *shard = g_llam_tls_shard;
    llam_worker_t *worker = g_llam_tls_worker;
    llam_lease_test_snapshot_t snapshot;

    if (shard == NULL || worker == NULL || worker != &shard->primary_worker ||
        g_llam_tls_processor != &shard->processor ||
        g_llam_tls_scheduler_ctx != &shard->scheduler_ctx) {
        test_fail(&observation->failure,
                  "primary scheduler entry did not publish a complete worker binding");
    }
    observation->runtime = shard != NULL ? shard->runtime : NULL;
    observation->worker = (uintptr_t)worker;
    observation->processor = (uintptr_t)g_llam_tls_processor;
    observation->incarnation = worker != NULL
                                   ? atomic_load_explicit(&worker->incarnation,
                                                          memory_order_acquire)
                                   : 0U;
    observation->binding_live = llam_worker_binding_live(worker,
                                                         observation->incarnation);
    observation->active_g = worker != NULL
                                ? (uint64_t)(uintptr_t)atomic_load_explicit(
                                      &worker->active_g,
                                      memory_order_acquire)
                                : 0U;
    observation->last_run_started_ns = worker != NULL
                                           ? atomic_load_explicit(
                                                 &worker->last_run_started_ns,
                                                 memory_order_acquire)
                                           : 0U;
    observation->last_safepoint_ns = worker != NULL
                                         ? atomic_load_explicit(&worker->last_safepoint_ns,
                                                                memory_order_acquire)
                                         : 0U;
    observation->last_idle_wake_ns = worker != NULL
                                         ? atomic_load_explicit(&worker->last_idle_wake_ns,
                                                                memory_order_acquire)
                                         : 0U;
    observation->native_thread = worker != NULL
                                     ? (uintptr_t)worker->native_thread
                                     : 0U;
    if (worker == NULL || pthread_equal(worker->native_thread, pthread_self()) == 0) {
        test_fail(&observation->failure,
                  "primary binding did not publish the caller's native thread");
    }
    if (snapshot_lease_locked_for_test(shard, &snapshot) != 0 ||
        snapshot.state != LLAM_LEASE_PRIMARY_OWNED ||
        snapshot.owner_worker != (uintptr_t)&shard->primary_worker ||
        snapshot.owner_incarnation != observation->incarnation ||
        snapshot.return_worker != 0U || snapshot.return_incarnation != 0U) {
        test_fail(&observation->failure,
                  "primary scheduler entry did not publish the primary lease");
    }
    observation->lease_state = snapshot.state;
    observation->lease_owner = snapshot.owner_worker;
    observation->lease_owner_incarnation = snapshot.owner_incarnation;
    atomic_store_explicit(&observation->entered, 1U, memory_order_release);
}

static int test_lease_binding_lifecycle(void) {
    lease_binding_observation_t observation;
    llam_runtime_t *runtime = NULL;
    llam_task_t *task = NULL;
    llam_shard_t *shard;
    llam_lease_test_snapshot_t snapshot;
    uint64_t initial_incarnation;
    int run_rc;
    int run_errno;

    memset(&observation, 0, sizeof(observation));
    atomic_init(&observation.failure.failures, 0U);
    atomic_init(&observation.entered, 0U);
    reset_lease_test_gates();
    if (create_runtime(&runtime, true, false) != 0) {
        return 1;
    }
    shard = &runtime->shards[0];
    initial_incarnation = atomic_load_explicit(&shard->primary_worker.incarnation,
                                               memory_order_acquire);
    if (initial_incarnation != 0U ||
        llam_worker_binding_live(&shard->primary_worker, initial_incarnation) ||
        llam_worker_binding_live(&shard->opaque_helper_worker,
                                 atomic_load_explicit(&shard->opaque_helper_worker.incarnation,
                                                      memory_order_acquire))) {
        test_fail(&observation.failure,
                  "runtime initialization published a live worker before scheduler entry");
    }
    if (snapshot_lease_locked_for_test(shard, &snapshot) != 0 ||
        snapshot.state != LLAM_LEASE_REDIRECTED_NO_EXECUTOR ||
        snapshot.owner_worker != 0U || snapshot.owner_incarnation != 0U ||
        snapshot.epoch != 0U || snapshot.return_worker != 0U ||
        snapshot.return_incarnation != 0U || snapshot.transfer_sequence != 0U ||
        snapshot.handback_ack_sequence != 0U) {
        test_fail(&observation.failure,
                  "runtime initialization did not leave an ownerless lease");
    }
    if (atomic_load_explicit(&shard->primary_worker.active_g, memory_order_acquire) != NULL ||
        atomic_load_explicit(&shard->opaque_helper_worker.active_g,
                             memory_order_acquire) != NULL ||
        atomic_load_explicit(&shard->primary_worker.last_run_started_ns,
                             memory_order_acquire) != 0U ||
        atomic_load_explicit(&shard->primary_worker.last_safepoint_ns,
                             memory_order_acquire) != 0U ||
        atomic_load_explicit(&shard->primary_worker.last_idle_wake_ns,
                             memory_order_acquire) != 0U) {
        test_fail(&observation.failure,
                  "runtime initialization did not clear worker-local execution state");
    }

    task = llam_runtime_spawn_ex(runtime,
                                 lease_binding_observer_task,
                                 &observation,
                                 NULL,
                                 0U);
    errno = 0;
    run_rc = task == NULL ? -1 : llam_runtime_run_handle(runtime);
    run_errno = errno;
    if (task != NULL) {
        bounded_join_task(task,
                          &observation.entered,
                          "lease binding observer task join exceeded its deadline");
    }
    assert_no_fatal(runtime,
                    run_rc,
                    run_errno,
                    &observation.failure,
                    "lease binding lifecycle changed run or fatal semantics");
    if (atomic_load_explicit(&observation.entered, memory_order_acquire) == 0U ||
        !observation.binding_live || observation.incarnation == 0U ||
        observation.lease_state != LLAM_LEASE_PRIMARY_OWNED ||
        observation.lease_owner != (uintptr_t)&shard->primary_worker ||
        observation.lease_owner_incarnation != observation.incarnation ||
        observation.native_thread == 0U) {
        test_fail(&observation.failure,
                  "primary binding observation was incomplete during scheduler entry");
    }
    if (llam_worker_binding_live(&shard->primary_worker, observation.incarnation)) {
        test_fail(&observation.failure,
                  "primary binding remained live after borrowed scheduler exit");
    }
    if (snapshot_lease_locked_for_test(shard, &snapshot) != 0 ||
        snapshot.state != LLAM_LEASE_REDIRECTED_NO_EXECUTOR ||
        snapshot.owner_worker != 0U || snapshot.owner_incarnation != 0U) {
        test_fail(&observation.failure,
                  "borrowed scheduler exit did not clear the primary lease");
    }
    destroy_runtime(&runtime);
    return failure_result(&observation.failure, "lease binding lifecycle failed");
}

static int test_shutdown_rejects_unbound_lease(void) {
    test_failure_state_t failure;
    llam_runtime_t *runtime = NULL;
    llam_shard_t *shard;
    llam_lease_test_snapshot_t snapshot;

    memset(&failure, 0, sizeof(failure));
    atomic_init(&failure.failures, 0U);
    reset_lease_test_gates();
    if (create_runtime(&runtime, true, false) != 0) {
        return 1;
    }
    shard = &runtime->shards[0];
    pthread_mutex_lock(&shard->opaque_lock);
    llam_legacy_lease_begin_stop_locked(shard);
    if (!llam_legacy_lease_snapshot_locked(shard, &snapshot) ||
        snapshot.state != LLAM_LEASE_STOPPING || snapshot.owner_worker != 0U ||
        snapshot.owner_incarnation != 0U || snapshot.return_worker != 0U ||
        snapshot.return_incarnation != 0U) {
        test_fail(&failure,
                  "shutdown stop transition granted or retained an unbound worker");
    }
    pthread_mutex_unlock(&shard->opaque_lock);
    if (llam_worker_binding_live(&shard->primary_worker, 0U) ||
        llam_worker_binding_live(&shard->opaque_helper_worker, 0U)) {
        test_fail(&failure,
                  "shutdown stop transition reported an unbound worker as live");
    }
    destroy_runtime(&runtime);
    return failure_result(&failure, "shutdown rejected-unbound lease check failed");
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
    static int (*const tests[])(void) = {
        test_lease_test_gate_contract,
        test_lease_binding_lifecycle,
        test_shutdown_rejects_unbound_lease,
        test_primary_helper_activation_and_return,
        test_lease_handoff_windows,
        test_clean_stop_completes_handback,
        test_failed_handback_does_not_republish,
        test_helper_reblocking_through_redirect,
        test_ownerless_return_ordering,
        test_repeated_shard_zero_runs_two_callers,
        test_helper_shutdown_while_idle,
        test_helper_shutdown_while_active,
        test_dynamic_merge_pause,
        test_watchdog_task_exit_overlap,
        test_partial_helper_start_failure,
    };
    unsigned i;
    unsigned skipped = 0U;

    for (i = 0U; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        int rc = tests[i]();

        if (rc == 77) {
            skipped += 1U;
        } else if (rc != 0) {
            return 1;
        }
    }
    if (skipped != 0U) {
        printf("test_legacy_lease_internal: checks passed; skipped=%u\n", skipped);
        return 77;
    }
    printf("test_legacy_lease_internal: all legacy lease characterization checks passed\n");
    return 0;
}
