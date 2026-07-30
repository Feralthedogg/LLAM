/**
 * @file tests/test_external_drive_cases.inc
 * @brief Core bounded external runtime-driver contract tests.
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

#if LLAM_PLATFORM_POSIX
#include <poll.h>
#endif
#if defined(__APPLE__)
#include <pthread/qos.h>
#endif

#define EXTERNAL_DRIVE_WAIT_NS UINT64_C(3000000000)

typedef struct external_drive_segment_state {
    llam_runtime_t *runtime;
    atomic_uint segments;
    atomic_uint reentrant_checked;
    atomic_uint hook_reentrant_checked;
    atomic_uint failures;
    int first_errno;
} external_drive_segment_state_t;

typedef struct external_drive_concurrent_state {
    atomic_uint entered;
    atomic_uint release;
    atomic_uint task_done;
} external_drive_concurrent_state_t;

typedef struct external_drive_thread_state {
    llam_runtime_t *runtime;
    uint32_t result;
    int rc;
    int err;
} external_drive_thread_state_t;

typedef struct external_drive_timer_state {
    atomic_uint armed;
    atomic_uint done;
    atomic_uint failures;
    uint64_t deadline_ns;
    int first_errno;
} external_drive_timer_state_t;

typedef struct external_drive_blocking_state {
    atomic_uint callback_started;
    atomic_uint callback_release;
    atomic_uint callback_done;
    atomic_uint task_done;
    atomic_uint failures;
    int first_errno;
} external_drive_blocking_state_t;

#if LLAM_PLATFORM_POSIX
typedef struct external_drive_io_state {
    int read_fd;
    unsigned char expected;
    atomic_uint reader_entered;
    atomic_uint reader_done;
    atomic_uint failures;
    int first_errno;
} external_drive_io_state_t;
#endif

static int external_drive_fail(const char *message) {
    fprintf(stderr, "[test_external_drive] %s\n", message);
    return 1;
}

static int external_drive_fail_errno(const char *message) {
    fprintf(stderr,
            "[test_external_drive] %s: errno=%d (%s)\n",
            message,
            errno,
            strerror(errno));
    return 1;
}

static int external_drive_init_opts(llam_runtime_opts_t *opts,
                                    uint32_t mode) {
    if (llam_runtime_opts_init(opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return external_drive_fail_errno("runtime opts init failed");
    }
    opts->profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    opts->worker_min = 1U;
    opts->worker_count = 1U;
    opts->worker_max = 1U;
    opts->blocking_min = 1U;
    opts->blocking_max = 1U;
    opts->driver_mode = mode;
    return 0;
}

static void external_drive_host_pause(void) {
#if LLAM_PLATFORM_WINDOWS
    Sleep(1U);
#else
    struct timespec delay = {0, 1000000L};

    (void)nanosleep(&delay, NULL);
#endif
}

static int external_drive_wait_atomic(atomic_uint *value,
                                      unsigned expected,
                                      const char *message) {
    uint64_t deadline = llam_now_ns() + EXTERNAL_DRIVE_WAIT_NS;

    while (atomic_load_explicit(value, memory_order_acquire) != expected) {
        if (llam_now_ns() >= deadline) {
            errno = ETIMEDOUT;
            return external_drive_fail_errno(message);
        }
        external_drive_host_pause();
    }
    return 0;
}

static int external_drive_wait_native(llam_runtime_t *runtime,
                                      unsigned timeout_ms) {
    llam_runtime_readiness_t readiness;

    memset(&readiness, 0, sizeof(readiness));
    if (llam_runtime_get_readiness(runtime,
                                   &readiness,
                                   sizeof(readiness)) != 0) {
        return external_drive_fail_errno("readiness query failed");
    }
#if LLAM_PLATFORM_WINDOWS
    if (readiness.kind != LLAM_RUNTIME_READINESS_WINDOWS_HANDLE ||
        readiness.value == (uintptr_t)0U) {
        return external_drive_fail("Windows readiness object was invalid");
    }
    if (WaitForSingleObject((HANDLE)readiness.value, timeout_ms) !=
        WAIT_OBJECT_0) {
        errno = ETIMEDOUT;
        return external_drive_fail_errno("Windows readiness wait timed out");
    }
#else
    {
        struct pollfd item;
        int rc;

        if (readiness.kind != LLAM_RUNTIME_READINESS_FD ||
            readiness.value > (uintptr_t)INT_MAX) {
            return external_drive_fail("POSIX readiness object was invalid");
        }
        memset(&item, 0, sizeof(item));
        item.fd = (int)readiness.value;
        item.events = POLLIN;
        do {
            rc = poll(&item, 1U, (int)timeout_ms);
        } while (rc < 0 && errno == EINTR);
        if (rc != 1 || (item.revents & POLLIN) == 0) {
            if (rc == 0) {
                errno = ETIMEDOUT;
            }
            return external_drive_fail_errno("POSIX readiness wait failed");
        }
    }
#endif
    return 0;
}

static bool external_drive_result_valid(uint32_t result) {
    return result == LLAM_RUNTIME_DRIVE_PROGRESS ||
           result == LLAM_RUNTIME_DRIVE_IDLE ||
           result == LLAM_RUNTIME_DRIVE_DONE;
}

static int exercise_external_drive_resource_plan(void) {
    const unsigned allowed_cpus[] = {2U, 4U};
    llam_runtime_resource_plan_input_t input;
    llam_runtime_resource_plan_t plan;
    llam_runtime_opts_t opts;

    memset(&input, 0, sizeof(input));
    input.opts = &opts;
    input.opts_size = LLAM_RUNTIME_OPTS_CURRENT_SIZE;
    input.allowed_cpus = allowed_cpus;
    input.allowed_cpu_count = 2U;
    input.affinity_supported = true;
    input.sqpoll_supported = true;
    input.page_size = 4096U;

    if (external_drive_init_opts(&opts,
                                 LLAM_RUNTIME_DRIVER_EXTERNAL) != 0 ||
        llam_runtime_resource_plan_resolve(&input, &plan) != 0) {
        return external_drive_fail_errno("external resource plan failed");
    }
    if (plan.driver_mode != LLAM_RUNTIME_DRIVER_EXTERNAL ||
        plan.worker_min != 1U ||
        plan.worker_count != 1U ||
        plan.worker_max != 1U ||
        plan.selected_cpu_count != 1U) {
        return external_drive_fail("external plan did not resolve one worker");
    }

    if (external_drive_init_opts(&opts,
                                 LLAM_RUNTIME_DRIVER_EXTERNAL) != 0) {
        return 1;
    }
    opts.worker_count = 2U;
    errno = 0;
    if (llam_runtime_resource_plan_resolve(&input, &plan) != -1 ||
        errno != EINVAL) {
        return external_drive_fail("external plan accepted worker_count > 1");
    }

    if (external_drive_init_opts(&opts,
                                 LLAM_RUNTIME_DRIVER_EXTERNAL) != 0) {
        return 1;
    }
    opts.experimental_flags = LLAM_RUNTIME_EXPERIMENTAL_F_DYNAMIC_WORKERS;
    errno = 0;
    if (llam_runtime_resource_plan_resolve(&input, &plan) != -1 ||
        errno != EINVAL) {
        return external_drive_fail("external plan accepted dynamic workers");
    }

    if (external_drive_init_opts(&opts,
                                 LLAM_RUNTIME_DRIVER_EXTERNAL) != 0) {
        return 1;
    }
    opts.experimental_flags = LLAM_RUNTIME_EXPERIMENTAL_F_SQPOLL;
    opts.sqpoll_cpu = 4;
    errno = 0;
    if (llam_runtime_resource_plan_resolve(&input, &plan) != -1 ||
        errno != EINVAL) {
        return external_drive_fail("external plan accepted SQPOLL reservation");
    }

    if (external_drive_init_opts(&opts,
                                 LLAM_RUNTIME_DRIVER_EXTERNAL) != 0) {
        return 1;
    }
    opts.experimental_flags =
        LLAM_RUNTIME_EXPERIMENTAL_F_SQPOLL |
        LLAM_RUNTIME_EXPERIMENTAL_F_WORKER_RINGS;
    errno = 0;
    if (llam_runtime_resource_plan_resolve(&input, &plan) != -1 ||
        errno != EINVAL) {
        return external_drive_fail("external plan accepted masked SQPOLL");
    }
    if (external_drive_init_opts(&opts, UINT32_MAX) != 0) {
        return 1;
    }
    errno = 0;
    if (llam_runtime_resource_plan_resolve(&input, &plan) != -1 ||
        errno != EINVAL) {
        return external_drive_fail("resource plan accepted unknown driver mode");
    }
    return 0;
}

static int exercise_internal_driver_rejections(void) {
    llam_runtime_opts_t opts;
    llam_runtime_t *runtime = NULL;
    llam_runtime_readiness_t readiness;
    uint64_t deadline = 0U;
    uint32_t result = UINT32_MAX;
    int rc = 1;

    errno = 0;
    if (llam_runtime_drive_once(NULL, &result) != -1 ||
        errno != EINVAL) {
        return external_drive_fail("drive_once accepted a NULL runtime");
    }
    if (external_drive_init_opts(&opts,
                                 LLAM_RUNTIME_DRIVER_INTERNAL) != 0 ||
        llam_runtime_create(&opts,
                            LLAM_RUNTIME_OPTS_CURRENT_SIZE,
                            &runtime) != 0) {
        return external_drive_fail_errno("internal runtime create failed");
    }

    errno = 0;
    if (llam_runtime_drive_once(runtime, &result) != -1 ||
        errno != ENOTSUP) {
        rc = external_drive_fail("internal runtime accepted drive_once");
        goto cleanup;
    }
    errno = 0;
    if (llam_runtime_next_deadline(runtime, &deadline) != -1 ||
        errno != ENOTSUP) {
        rc = external_drive_fail("internal runtime accepted next_deadline");
        goto cleanup;
    }
    errno = 0;
    if (llam_runtime_get_readiness(runtime,
                                   &readiness,
                                   sizeof(readiness)) != -1 ||
        errno != ENOTSUP) {
        rc = external_drive_fail("internal runtime accepted readiness query");
        goto cleanup;
    }
    errno = 0;
    if (llam_runtime_wake(runtime) != -1 || errno != ENOTSUP) {
        rc = external_drive_fail("internal runtime accepted explicit wake");
        goto cleanup;
    }
    rc = 0;

cleanup:
    llam_runtime_destroy(runtime);
    return rc;
}

static int exercise_external_readiness_contract(void) {
    struct external_readiness_future {
        llam_runtime_readiness_t current;
        unsigned char future[16];
    } storage;
    llam_runtime_opts_t opts;
    llam_runtime_t *runtime = NULL;
    uint64_t deadline = 0U;
    uint32_t result = UINT32_MAX;
    int rc = 1;

    if (external_drive_init_opts(&opts,
                                 LLAM_RUNTIME_DRIVER_EXTERNAL) != 0 ||
        llam_runtime_create(&opts,
                            LLAM_RUNTIME_OPTS_CURRENT_SIZE,
                            &runtime) != 0) {
        return external_drive_fail_errno("external readiness runtime create failed");
    }
    memset(&storage, 0xA5, sizeof(storage));
    if (llam_runtime_get_readiness(runtime,
                                   &storage.current,
                                   sizeof(storage)) != 0) {
        rc = external_drive_fail_errno("future-size readiness query failed");
        goto cleanup;
    }
    for (size_t i = 0U; i < sizeof(storage.future); ++i) {
        if (storage.future[i] != 0xA5U) {
            rc = external_drive_fail("readiness query overwrote future tail");
            goto cleanup;
        }
    }
#if LLAM_PLATFORM_WINDOWS
    if (storage.current.kind != LLAM_RUNTIME_READINESS_WINDOWS_HANDLE ||
        storage.current.value == (uintptr_t)0U) {
        rc = external_drive_fail("external Windows readiness projection was invalid");
        goto cleanup;
    }
#else
    if (storage.current.kind != LLAM_RUNTIME_READINESS_FD ||
        storage.current.value > (uintptr_t)INT_MAX) {
        rc = external_drive_fail("external POSIX readiness projection was invalid");
        goto cleanup;
    }
#endif
    if (llam_runtime_next_deadline(runtime, &deadline) != 0 ||
        deadline != UINT64_MAX) {
        rc = external_drive_fail("empty external runtime exposed a deadline");
        goto cleanup;
    }
    if (llam_runtime_wake(runtime) != 0 ||
        external_drive_wait_native(runtime, 3000U) != 0) {
        rc = 1;
        goto cleanup;
    }
    if (llam_runtime_drive_once(runtime, &result) != 0 ||
        result != LLAM_RUNTIME_DRIVE_DONE) {
        rc = external_drive_fail_errno("empty external drive did not return DONE");
        goto cleanup;
    }
    rc = 0;

cleanup:
    llam_runtime_destroy(runtime);
    return rc;
}

static int exercise_external_host_qos_preservation(void) {
#if defined(__APPLE__)
    llam_runtime_opts_t opts;
    llam_runtime_t *runtime = NULL;
    qos_class_t before_class;
    qos_class_t after_class;
    int before_priority = 0;
    int after_priority = 0;
    uint32_t result = UINT32_MAX;
    int rc = 1;

    if (external_drive_init_opts(&opts,
                                 LLAM_RUNTIME_DRIVER_EXTERNAL) != 0 ||
        llam_runtime_create(&opts,
                            LLAM_RUNTIME_OPTS_CURRENT_SIZE,
                            &runtime) != 0) {
        return external_drive_fail_errno("QoS preservation runtime create failed");
    }
    if (pthread_get_qos_class_np(pthread_self(),
                                 &before_class,
                                 &before_priority) != 0) {
        rc = external_drive_fail("host QoS query failed before drive");
        goto cleanup;
    }
    if (llam_runtime_drive_once(runtime, &result) != 0 ||
        result != LLAM_RUNTIME_DRIVE_DONE) {
        rc = external_drive_fail_errno("QoS preservation drive failed");
        goto cleanup;
    }
    if (pthread_get_qos_class_np(pthread_self(),
                                 &after_class,
                                 &after_priority) != 0) {
        rc = external_drive_fail("host QoS query failed after drive");
        goto cleanup;
    }
    if (after_class != before_class ||
        after_priority != before_priority) {
        rc = external_drive_fail("external drive changed host-thread QoS");
        goto cleanup;
    }
    rc = 0;

cleanup:
    llam_runtime_destroy(runtime);
    return rc;
#else
    return 0;
#endif
}

static void external_drive_yielding_task(void *arg) {
    external_drive_segment_state_t *state = arg;

    atomic_fetch_add_explicit(&state->segments, 1U, memory_order_relaxed);
    llam_yield();
    atomic_fetch_add_explicit(&state->segments, 1U, memory_order_relaxed);
}

static void external_drive_plain_task(void *arg) {
    external_drive_segment_state_t *state = arg;

    atomic_fetch_add_explicit(&state->segments, 1U, memory_order_relaxed);
}

static int exercise_external_merge_pause_ack_rearm(void) {
    llam_runtime_opts_t opts;
    llam_runtime_t *runtime = NULL;
    llam_task_t *task = NULL;
    external_drive_segment_state_t state;
    uint32_t result = UINT32_MAX;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.segments, 0U);
    if (external_drive_init_opts(&opts,
                                 LLAM_RUNTIME_DRIVER_EXTERNAL) != 0 ||
        llam_runtime_create(&opts,
                            LLAM_RUNTIME_OPTS_CURRENT_SIZE,
                            &runtime) != 0) {
        return external_drive_fail_errno("merge-pause runtime create failed");
    }
    task = llam_runtime_spawn_ex(runtime,
                                 external_drive_plain_task,
                                 &state,
                                 NULL,
                                 0U);
    if (task == NULL) {
        rc = external_drive_fail_errno("merge-pause task spawn failed");
        goto cleanup;
    }

    atomic_store_explicit(&runtime->shards[0].merge_pause_requested,
                          1U,
                          memory_order_release);
    if (llam_runtime_drive_once(runtime, &result) != 0 ||
        result != LLAM_RUNTIME_DRIVE_IDLE ||
        atomic_load_explicit(&runtime->shards[0].merge_pause_ack,
                             memory_order_acquire) != 1U ||
        atomic_load_explicit(&state.segments,
                             memory_order_acquire) != 0U) {
        rc = external_drive_fail("external merge pause was not acknowledged");
        goto cleanup;
    }

    atomic_store_explicit(&runtime->shards[0].merge_pause_requested,
                          0U,
                          memory_order_release);
    if (llam_runtime_drive_once(runtime, &result) != 0 ||
        result != LLAM_RUNTIME_DRIVE_PROGRESS ||
        atomic_load_explicit(&runtime->shards[0].merge_pause_ack,
                             memory_order_acquire) != 0U ||
        atomic_load_explicit(&state.segments,
                             memory_order_acquire) != 1U) {
        rc = external_drive_fail(
            "external merge-pause acknowledgement stayed latched");
        goto cleanup;
    }
    if (llam_runtime_drive_once(runtime, &result) != 0 ||
        result != LLAM_RUNTIME_DRIVE_DONE ||
        llam_join(task) != 0) {
        rc = external_drive_fail_errno("merge-pause runtime did not drain");
        goto cleanup;
    }
    task = NULL;
    rc = 0;

cleanup:
    if (task != NULL) {
        (void)llam_detach(task);
    }
    llam_runtime_destroy(runtime);
    return rc;
}

static void external_drive_reentrant_task(void *arg) {
    external_drive_segment_state_t *state = arg;
    uint32_t nested_result = UINT32_MAX;
    int call_errno;

    atomic_fetch_add_explicit(&state->segments, 1U, memory_order_relaxed);
    errno = 0;
    if (llam_runtime_drive_once(state->runtime, &nested_result) == -1 &&
        errno == ENOTSUP &&
        nested_result == UINT32_MAX) {
        atomic_store_explicit(&state->reentrant_checked,
                              1U,
                              memory_order_release);
        return;
    }
    call_errno = errno != 0 ? errno : EPROTO;
    if (atomic_fetch_add_explicit(&state->failures,
                                  1U,
                                  memory_order_relaxed) == 0U) {
        state->first_errno = call_errno;
    }
}

static void external_drive_reentrant_hook(void *hook_context,
                                          void *task_context) {
    external_drive_segment_state_t *state = hook_context;
    uint32_t nested_result = UINT32_MAX;
    int call_errno;

    (void)task_context;
    errno = 0;
    if (state != NULL &&
        llam_runtime_drive_once(state->runtime, &nested_result) == -1 &&
        errno == ENOTSUP &&
        nested_result == UINT32_MAX) {
        atomic_store_explicit(&state->hook_reentrant_checked,
                              1U,
                              memory_order_release);
        return;
    }
    call_errno = errno != 0 ? errno : EPROTO;
    if (state != NULL &&
        atomic_fetch_add_explicit(&state->failures,
                                  1U,
                                  memory_order_relaxed) == 0U) {
        state->first_errno = call_errno;
    }
}

static int exercise_external_segment_bound_and_reuse(void) {
    llam_runtime_opts_t opts;
    llam_runtime_t *runtime = NULL;
    llam_task_t *yielding = NULL;
    llam_task_t *plain = NULL;
    llam_task_t *reentrant = NULL;
    external_drive_segment_state_t state;
    uint32_t result = UINT32_MAX;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.segments, 0U);
    atomic_init(&state.reentrant_checked, 0U);
    atomic_init(&state.hook_reentrant_checked, 0U);
    atomic_init(&state.failures, 0U);
    if (external_drive_init_opts(&opts,
                                 LLAM_RUNTIME_DRIVER_EXTERNAL) != 0) {
        return 1;
    }
    opts.on_task_resume = external_drive_reentrant_hook;
    opts.switch_hook_context = &state;
    if (llam_runtime_create(&opts,
                            LLAM_RUNTIME_OPTS_CURRENT_SIZE,
                            &runtime) != 0) {
        return external_drive_fail_errno("segment-bound runtime create failed");
    }
    state.runtime = runtime;
    if (!runtime->external_driver.enabled ||
        runtime->resource_plan.worker_min != 1U ||
        runtime->resource_plan.worker_count != 1U ||
        runtime->resource_plan.worker_max != 1U ||
        runtime->experimental_dynamic_shards != 0U ||
        runtime->direct_handoff_burst != 0U ||
        llam_runtime_direct_handoff_budget(runtime) != 0U ||
        runtime->direct_handoff_allow_timers != 0U ||
        runtime->wake_handoff_enabled != 0U ||
        runtime->channel_local_handoff_enabled != 0U) {
        rc = external_drive_fail("external runtime retained internal scheduling policy");
        goto cleanup;
    }
    errno = 0;
    if (llam_runtime_run_handle(runtime) != -1 || errno != ENOTSUP) {
        rc = external_drive_fail("external runtime accepted run_handle");
        goto cleanup;
    }
    if (llam_runtime_drive_once(runtime, &result) != 0 ||
        result != LLAM_RUNTIME_DRIVE_DONE) {
        rc = external_drive_fail_errno("initial empty drive did not return DONE");
        goto cleanup;
    }

    yielding = llam_runtime_spawn_ex(runtime,
                                     external_drive_yielding_task,
                                     &state,
                                     NULL,
                                     0U);
    plain = llam_runtime_spawn_ex(runtime,
                                  external_drive_plain_task,
                                  &state,
                                  NULL,
                                  0U);
    reentrant = llam_runtime_spawn_ex(runtime,
                                      external_drive_reentrant_task,
                                      &state,
                                      NULL,
                                      0U);
    if (yielding == NULL || plain == NULL || reentrant == NULL) {
        rc = external_drive_fail_errno("external segment tasks failed to spawn after DONE");
        goto cleanup;
    }

    for (unsigned i = 0U; i < 16U; ++i) {
        unsigned before =
            atomic_load_explicit(&state.segments, memory_order_acquire);
        unsigned after;

        result = UINT32_MAX;
        if (llam_runtime_drive_once(runtime, &result) != 0) {
            rc = external_drive_fail_errno("bounded external drive failed");
            goto cleanup;
        }
        after = atomic_load_explicit(&state.segments, memory_order_acquire);
        if (!external_drive_result_valid(result)) {
            rc = external_drive_fail("external drive returned an unknown result");
            goto cleanup;
        }
        if (after < before || after - before > 1U) {
            rc = external_drive_fail("one drive call executed multiple task segments");
            goto cleanup;
        }
        if (result == LLAM_RUNTIME_DRIVE_DONE) {
            break;
        }
    }
    if (result != LLAM_RUNTIME_DRIVE_DONE ||
        atomic_load_explicit(&state.segments, memory_order_acquire) != 4U ||
        atomic_load_explicit(&state.reentrant_checked, memory_order_acquire) != 1U ||
        atomic_load_explicit(&state.hook_reentrant_checked,
                             memory_order_acquire) != 1U ||
        atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
        errno = state.first_errno;
        rc = external_drive_fail_errno("external segment/reentrant contract failed");
        goto cleanup;
    }
    if (llam_join(reentrant) != 0 ||
        llam_join(plain) != 0 ||
        llam_join(yielding) != 0) {
        rc = external_drive_fail_errno("external segment task join failed");
        goto cleanup;
    }
    reentrant = NULL;
    plain = NULL;
    yielding = NULL;
    rc = 0;

cleanup:
    if (reentrant != NULL) {
        (void)llam_detach(reentrant);
    }
    if (plain != NULL) {
        (void)llam_detach(plain);
    }
    if (yielding != NULL) {
        (void)llam_detach(yielding);
    }
    llam_runtime_destroy(runtime);
    return rc;
}

#include "test_external_drive_async_cases.inc"

static int test_external_drive_contract(void) {
    if (exercise_external_drive_resource_plan() != 0 ||
        exercise_internal_driver_rejections() != 0 ||
        exercise_external_readiness_contract() != 0 ||
        exercise_external_host_qos_preservation() != 0 ||
        exercise_external_merge_pause_ack_rearm() != 0 ||
        exercise_external_segment_bound_and_reuse() != 0 ||
        exercise_external_drive_ownership() != 0 ||
        exercise_external_drive_deadline() != 0 ||
        exercise_external_blocking_readiness() != 0 ||
        exercise_external_io_readiness() != 0) {
        return 1;
    }
    return 0;
}
