/**
 * @file tests/test_external_drive_async_cases.inc
 * @brief External drive ownership, timer, blocking, and I/O readiness cases.
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

static void external_drive_concurrent_task(void *arg) {
    external_drive_concurrent_state_t *state = arg;

    atomic_store_explicit(&state->entered, 1U, memory_order_release);
    while (atomic_load_explicit(&state->release,
                                memory_order_acquire) == 0U) {
        llam_pause_cpu();
    }
    llam_yield();
    atomic_store_explicit(&state->task_done, 1U, memory_order_release);
}

static void *external_drive_thread_main(void *arg) {
    external_drive_thread_state_t *state = arg;

    errno = 0;
    state->rc = llam_runtime_drive_once(state->runtime, &state->result);
    state->err = errno;
    return NULL;
}

static int external_drive_until_task_done(llam_runtime_t *runtime,
                                          atomic_uint *done,
                                          uint32_t *result,
                                          const char *message) {
    uint64_t deadline_ns = llam_now_ns() + EXTERNAL_DRIVE_WAIT_NS;

    while (atomic_load_explicit(done, memory_order_acquire) == 0U) {
        if (llam_now_ns() >= deadline_ns) {
            errno = ETIMEDOUT;
            return external_drive_fail_errno(message);
        }
        if (external_drive_wait_native(runtime, 3000U) != 0 ||
            llam_runtime_drive_once(runtime, result) != 0) {
            return 1;
        }
        if (!external_drive_result_valid(*result) ||
            *result == LLAM_RUNTIME_DRIVE_DONE) {
            errno = EPROTO;
            return external_drive_fail_errno(message);
        }
        if (*result == LLAM_RUNTIME_DRIVE_IDLE) {
            external_drive_host_pause();
        }
    }
    return 0;
}

static int exercise_external_drive_ownership(void) {
    llam_runtime_opts_t opts;
    llam_runtime_t *runtime = NULL;
    llam_task_t *task = NULL;
    external_drive_concurrent_state_t task_state;
    external_drive_thread_state_t thread_state;
    pthread_t thread;
    uint32_t competing_result = UINT32_MAX;
    uint32_t migrated_result = UINT32_MAX;
    uint32_t final_result = UINT32_MAX;
    bool thread_started = false;
    int rc = 1;

    memset(&task_state, 0, sizeof(task_state));
    memset(&thread_state, 0, sizeof(thread_state));
    atomic_init(&task_state.entered, 0U);
    atomic_init(&task_state.release, 0U);
    atomic_init(&task_state.task_done, 0U);
    if (external_drive_init_opts(&opts,
                                 LLAM_RUNTIME_DRIVER_EXTERNAL) != 0 ||
        llam_runtime_create(&opts,
                            LLAM_RUNTIME_OPTS_CURRENT_SIZE,
                            &runtime) != 0) {
        return external_drive_fail_errno("drive ownership runtime create failed");
    }
    task = llam_runtime_spawn_ex(runtime,
                                 external_drive_concurrent_task,
                                 &task_state,
                                 NULL,
                                 0U);
    if (task == NULL) {
        rc = external_drive_fail_errno("drive ownership task spawn failed");
        goto cleanup;
    }
    thread_state.runtime = runtime;
    thread_state.result = UINT32_MAX;
    if (pthread_create(&thread,
                       NULL,
                       external_drive_thread_main,
                       &thread_state) != 0) {
        rc = external_drive_fail("drive ownership thread create failed");
        goto cleanup;
    }
    thread_started = true;
    if (external_drive_wait_atomic(&task_state.entered,
                                   1U,
                                   "first external driver did not enter task") != 0) {
        goto cleanup;
    }
    errno = 0;
    if (llam_runtime_drive_once(runtime, &competing_result) != -1 ||
        errno != EBUSY ||
        competing_result != UINT32_MAX) {
        rc = external_drive_fail("concurrent external drive did not return EBUSY");
        goto cleanup;
    }
    atomic_store_explicit(&task_state.release, 1U, memory_order_release);
    if (pthread_join(thread, NULL) != 0) {
        thread_started = false;
        rc = external_drive_fail("drive ownership thread join failed");
        goto cleanup;
    }
    thread_started = false;
    if (thread_state.rc != 0 ||
        thread_state.result != LLAM_RUNTIME_DRIVE_PROGRESS ||
        atomic_load_explicit(&task_state.task_done, memory_order_acquire) != 0U) {
        errno = thread_state.err;
        rc = external_drive_fail_errno("owning external drive failed");
        goto cleanup;
    }
    if (llam_runtime_drive_once(runtime, &migrated_result) != 0 ||
        migrated_result != LLAM_RUNTIME_DRIVE_PROGRESS ||
        atomic_load_explicit(&task_state.task_done, memory_order_acquire) != 1U) {
        rc = external_drive_fail_errno("successive host-thread drive migration failed");
        goto cleanup;
    }
    if (llam_runtime_drive_once(runtime, &final_result) != 0 ||
        final_result != LLAM_RUNTIME_DRIVE_DONE ||
        llam_join(task) != 0) {
        rc = external_drive_fail_errno("external ownership completion failed");
        goto cleanup;
    }
    task = NULL;
    rc = 0;

cleanup:
    atomic_store_explicit(&task_state.release, 1U, memory_order_release);
    if (thread_started) {
        (void)pthread_join(thread, NULL);
    }
    if (task != NULL) {
        (void)llam_detach(task);
    }
    llam_runtime_destroy(runtime);
    return rc;
}

static void external_drive_timer_task(void *arg) {
    external_drive_timer_state_t *state = arg;

    state->deadline_ns = llam_now_ns() + UINT64_C(200000000);
    atomic_store_explicit(&state->armed, 1U, memory_order_release);
    if (llam_sleep_until(state->deadline_ns) != 0) {
        if (atomic_fetch_add_explicit(&state->failures,
                                      1U,
                                      memory_order_relaxed) == 0U) {
            state->first_errno = errno;
        }
        return;
    }
    atomic_store_explicit(&state->done, 1U, memory_order_release);
}

static void external_drive_wait_until_ns(uint64_t deadline_ns) {
    while (llam_now_ns() < deadline_ns) {
        external_drive_host_pause();
    }
}

static int exercise_external_drive_deadline(void) {
    llam_runtime_opts_t opts;
    llam_runtime_t *runtime = NULL;
    llam_task_t *task = NULL;
    external_drive_timer_state_t state;
    uint64_t deadline = UINT64_MAX;
    uint32_t result = UINT32_MAX;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.armed, 0U);
    atomic_init(&state.done, 0U);
    atomic_init(&state.failures, 0U);
    if (external_drive_init_opts(&opts,
                                 LLAM_RUNTIME_DRIVER_EXTERNAL) != 0 ||
        llam_runtime_create(&opts,
                            LLAM_RUNTIME_OPTS_CURRENT_SIZE,
                            &runtime) != 0) {
        return external_drive_fail_errno("deadline runtime create failed");
    }
    task = llam_runtime_spawn_ex(runtime,
                                 external_drive_timer_task,
                                 &state,
                                 NULL,
                                 0U);
    if (task == NULL) {
        rc = external_drive_fail_errno("deadline task spawn failed");
        goto cleanup;
    }
    if (llam_runtime_drive_once(runtime, &result) != 0 ||
        result != LLAM_RUNTIME_DRIVE_PROGRESS ||
        atomic_load_explicit(&state.armed, memory_order_acquire) != 1U) {
        rc = external_drive_fail_errno("deadline task did not arm in first segment");
        goto cleanup;
    }
    if (llam_runtime_next_deadline(runtime, &deadline) != 0 ||
        deadline != state.deadline_ns) {
        rc = external_drive_fail("next_deadline did not expose timer root");
        goto cleanup;
    }
    if (llam_runtime_drive_once(runtime, &result) != 0 ||
        result != LLAM_RUNTIME_DRIVE_IDLE) {
        rc = external_drive_fail_errno("pre-deadline drive was not IDLE");
        goto cleanup;
    }
    external_drive_wait_until_ns(deadline + UINT64_C(1000000));
    if (llam_runtime_drive_once(runtime, &result) != 0 ||
        result != LLAM_RUNTIME_DRIVE_PROGRESS ||
        atomic_load_explicit(&state.done, memory_order_acquire) != 1U ||
        atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
        errno = state.first_errno;
        rc = external_drive_fail_errno("expired deadline was not dispatched");
        goto cleanup;
    }
    if (llam_runtime_next_deadline(runtime, &deadline) != 0 ||
        deadline != UINT64_MAX ||
        llam_runtime_drive_once(runtime, &result) != 0 ||
        result != LLAM_RUNTIME_DRIVE_DONE ||
        llam_join(task) != 0) {
        rc = external_drive_fail_errno("deadline completion contract failed");
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

static void *external_drive_blocking_callback(void *arg) {
    external_drive_blocking_state_t *state = arg;

    atomic_store_explicit(&state->callback_started, 1U, memory_order_release);
    while (atomic_load_explicit(&state->callback_release,
                                memory_order_acquire) == 0U) {
        llam_pause_cpu();
    }
    atomic_store_explicit(&state->callback_done, 1U, memory_order_release);
    return state;
}

static void external_drive_blocking_task(void *arg) {
    external_drive_blocking_state_t *state = arg;
    void *value = NULL;

    if (llam_call_blocking_result(external_drive_blocking_callback,
                                  state,
                                  &value) != 0 ||
        value != state) {
        if (atomic_fetch_add_explicit(&state->failures,
                                      1U,
                                      memory_order_relaxed) == 0U) {
            state->first_errno = errno != 0 ? errno : EPROTO;
        }
        return;
    }
    atomic_store_explicit(&state->task_done, 1U, memory_order_release);
}

static int exercise_external_blocking_readiness(void) {
    llam_runtime_opts_t opts;
    llam_runtime_t *runtime = NULL;
    llam_task_t *task = NULL;
    external_drive_blocking_state_t state;
    uint32_t result = UINT32_MAX;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.callback_started, 0U);
    atomic_init(&state.callback_release, 0U);
    atomic_init(&state.callback_done, 0U);
    atomic_init(&state.task_done, 0U);
    atomic_init(&state.failures, 0U);
    if (external_drive_init_opts(&opts,
                                 LLAM_RUNTIME_DRIVER_EXTERNAL) != 0 ||
        llam_runtime_create(&opts,
                            LLAM_RUNTIME_OPTS_CURRENT_SIZE,
                            &runtime) != 0) {
        return external_drive_fail_errno("blocking readiness runtime create failed");
    }
    task = llam_runtime_spawn_ex(runtime,
                                 external_drive_blocking_task,
                                 &state,
                                 NULL,
                                 0U);
    if (task == NULL) {
        rc = external_drive_fail_errno("blocking readiness task spawn failed");
        goto cleanup;
    }
    if (llam_runtime_drive_once(runtime, &result) != 0 ||
        result != LLAM_RUNTIME_DRIVE_PROGRESS ||
        external_drive_wait_atomic(&state.callback_started,
                                   1U,
                                   "blocking callback did not start") != 0) {
        goto cleanup;
    }
    atomic_store_explicit(&state.callback_release, 1U, memory_order_release);
    if (external_drive_until_task_done(
            runtime,
            &state.task_done,
            &result,
            "blocking completion drive timed out") != 0) {
        goto blocking_failed;
    }
    if (result != LLAM_RUNTIME_DRIVE_PROGRESS ||
        atomic_load_explicit(&state.callback_done, memory_order_acquire) != 1U ||
        atomic_load_explicit(&state.task_done, memory_order_acquire) != 1U ||
        atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
blocking_failed:
        errno = state.first_errno != 0 ? state.first_errno : EPROTO;
        rc = external_drive_fail_errno("blocking completion readiness failed");
        goto cleanup;
    }
    if (llam_runtime_drive_once(runtime, &result) != 0 ||
        result != LLAM_RUNTIME_DRIVE_DONE ||
        llam_join(task) != 0) {
        rc = external_drive_fail_errno("blocking readiness completion failed");
        goto cleanup;
    }
    task = NULL;
    rc = 0;

cleanup:
    atomic_store_explicit(&state.callback_release, 1U, memory_order_release);
    if (task != NULL) {
        (void)llam_detach(task);
    }
    llam_runtime_destroy(runtime);
    return rc;
}

#if LLAM_PLATFORM_POSIX
static void external_drive_io_reader_task(void *arg) {
    external_drive_io_state_t *state = arg;
    unsigned char byte = 0U;

    atomic_store_explicit(&state->reader_entered, 1U, memory_order_release);
    if (llam_read(state->read_fd, &byte, 1U) != 1 ||
        byte != state->expected) {
        if (atomic_fetch_add_explicit(&state->failures,
                                      1U,
                                      memory_order_relaxed) == 0U) {
            state->first_errno = errno != 0 ? errno : EPROTO;
        }
        return;
    }
    atomic_store_explicit(&state->reader_done, 1U, memory_order_release);
}
#endif

static int exercise_external_io_readiness(void) {
#if LLAM_PLATFORM_POSIX
    llam_runtime_opts_t opts;
    llam_runtime_t *runtime = NULL;
    llam_task_t *task = NULL;
    external_drive_io_state_t state;
    int sockets[2] = {-1, -1};
    uint32_t result = UINT32_MAX;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    state.read_fd = -1;
    state.expected = 0x6dU;
    atomic_init(&state.reader_entered, 0U);
    atomic_init(&state.reader_done, 0U);
    atomic_init(&state.failures, 0U);
    if (external_drive_init_opts(&opts,
                                 LLAM_RUNTIME_DRIVER_EXTERNAL) != 0) {
        return 1;
    }
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        return external_drive_fail_errno("external I/O socketpair failed");
    }
    state.read_fd = sockets[0];
    if (llam_runtime_create(&opts,
                            LLAM_RUNTIME_OPTS_CURRENT_SIZE,
                            &runtime) != 0) {
        rc = external_drive_fail_errno("I/O readiness runtime create failed");
        goto cleanup;
    }
    task = llam_runtime_spawn_ex(runtime,
                                 external_drive_io_reader_task,
                                 &state,
                                 NULL,
                                 0U);
    if (task == NULL) {
        rc = external_drive_fail_errno("I/O readiness task spawn failed");
        goto cleanup;
    }
    if (llam_runtime_drive_once(runtime, &result) != 0 ||
        result != LLAM_RUNTIME_DRIVE_PROGRESS ||
        atomic_load_explicit(&state.reader_entered, memory_order_acquire) != 1U) {
        rc = external_drive_fail_errno("I/O reader did not park in first segment");
        goto cleanup;
    }
    if (write(sockets[1], &state.expected, 1U) != 1) {
        rc = external_drive_fail_errno("host I/O producer write failed");
        goto cleanup;
    }
    if (external_drive_until_task_done(
            runtime,
            &state.reader_done,
            &result,
            "I/O completion drive timed out") != 0) {
        goto io_failed;
    }
    if (result != LLAM_RUNTIME_DRIVE_PROGRESS ||
        atomic_load_explicit(&state.reader_done, memory_order_acquire) != 1U ||
        atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
io_failed:
        fprintf(stderr,
                "[test_external_drive] I/O state result=%u entered=%u done=%u failures=%u task_errno=%d\n",
                result,
                atomic_load_explicit(&state.reader_entered,
                                     memory_order_acquire),
                atomic_load_explicit(&state.reader_done,
                                     memory_order_acquire),
                atomic_load_explicit(&state.failures,
                                     memory_order_relaxed),
                state.first_errno);
        errno = state.first_errno != 0 ? state.first_errno : EPROTO;
        rc = external_drive_fail_errno("I/O completion readiness failed");
        goto cleanup;
    }
    if (llam_runtime_drive_once(runtime, &result) != 0 ||
        result != LLAM_RUNTIME_DRIVE_DONE ||
        llam_join(task) != 0) {
        rc = external_drive_fail_errno("I/O readiness completion failed");
        goto cleanup;
    }
    task = NULL;
    rc = 0;

cleanup:
    if (task != NULL) {
        (void)llam_detach(task);
    }
    llam_runtime_destroy(runtime);
    if (sockets[0] >= 0) {
        close(sockets[0]);
    }
    if (sockets[1] >= 0) {
        close(sockets[1]);
    }
    return rc;
#else
    return 0;
#endif
}
