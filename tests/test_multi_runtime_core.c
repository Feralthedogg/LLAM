/**
 * @file tests/test_multi_runtime_core.c
 * @brief Explicit runtime-handle concurrency and cross-owner regression tests.
 *
 * @details
 * These tests intentionally avoid the legacy singleton API once runtimes are
 * created.  They pin the 2.x contract that heap-backed runtime handles can be
 * created together, driven from separate host threads, and rejected with
 * EXDEV when a managed task attempts to consume another runtime's task handle.
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
#include <stdio.h>
#include <string.h>

#if LLAM_PLATFORM_POSIX
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#define MULTI_RUNTIME_TASKS 64U

typedef struct multi_counter_state {
    atomic_uint ran_a;
    atomic_uint ran_b;
    atomic_uint failures;
} multi_counter_state_t;

typedef struct runtime_runner {
    llam_runtime_t *runtime;
    int rc;
    int err;
} runtime_runner_t;

typedef struct cross_owner_state {
    llam_task_t *foreign_task;
    atomic_uint failures;
    int first_errno;
} cross_owner_state_t;

typedef struct cross_object_state {
    llam_channel_t *foreign_channel;
    llam_mutex_t *foreign_mutex;
    atomic_uint failures;
    int first_errno;
} cross_object_state_t;

typedef struct multi_io_state {
    llam_fd_t left_fd;
    llam_fd_t right_fd;
    unsigned char payload;
    atomic_uint echoed;
    atomic_uint failures;
} multi_io_state_t;

static int test_fail(const char *message) {
    fprintf(stderr, "[test_multi_runtime_core] %s\n", message);
    return 1;
}

static int test_fail_errno(const char *message) {
    fprintf(stderr, "[test_multi_runtime_core] %s: errno=%d (%s)\n", message, errno, strerror(errno));
    return 1;
}

static void counter_task_a(void *arg) {
    multi_counter_state_t *state = arg;
    llam_channel_t *channel;
    void *out = NULL;
    int payload = 7;

    llam_yield();
    if (llam_sleep_ns(1000U) != 0) {
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
        return;
    }
    channel = llam_channel_create(1U);
    if (channel == NULL ||
        llam_channel_try_send(channel, &payload) != 0 ||
        llam_channel_try_recv_result(channel, &out) != 0 ||
        out != &payload ||
        llam_channel_destroy(channel) != 0) {
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
        if (channel != NULL) {
            (void)llam_channel_destroy(channel);
        }
        return;
    }
    atomic_fetch_add_explicit(&state->ran_a, 1U, memory_order_relaxed);
}

static void counter_task_b(void *arg) {
    multi_counter_state_t *state = arg;
    llam_channel_t *channel;
    void *out = NULL;
    int payload = 11;

    llam_yield();
    if (llam_sleep_ns(1000U) != 0) {
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
        return;
    }
    channel = llam_channel_create(1U);
    if (channel == NULL ||
        llam_channel_try_send(channel, &payload) != 0 ||
        llam_channel_try_recv_result(channel, &out) != 0 ||
        out != &payload ||
        llam_channel_destroy(channel) != 0) {
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
        if (channel != NULL) {
            (void)llam_channel_destroy(channel);
        }
        return;
    }
    atomic_fetch_add_explicit(&state->ran_b, 1U, memory_order_relaxed);
}

static void foreign_target_task(void *arg) {
    atomic_uint *ran = arg;

    llam_yield();
    atomic_fetch_add_explicit(ran, 1U, memory_order_relaxed);
}

static void cross_owner_task(void *arg) {
    cross_owner_state_t *state = arg;

    errno = 0;
    if (llam_join_until(state->foreign_task, 0U) != -1 || errno != EXDEV) {
        state->first_errno = errno;
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
    }
    errno = 0;
    if (llam_detach(state->foreign_task) != -1 || errno != EXDEV) {
        state->first_errno = errno;
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
    }
}

static void foreign_object_creator_task(void *arg) {
    cross_object_state_t *state = arg;

    state->foreign_channel = llam_channel_create(1U);
    state->foreign_mutex = llam_mutex_create();
    if (state->foreign_channel == NULL || state->foreign_mutex == NULL) {
        state->first_errno = errno;
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
    }
}

static void cross_object_task(void *arg) {
    cross_object_state_t *state = arg;
    int payload = 1;

    errno = 0;
    if (llam_channel_try_send(state->foreign_channel, &payload) != -1 || errno != EXDEV) {
        state->first_errno = errno;
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
    }
    errno = 0;
    if (llam_mutex_trylock(state->foreign_mutex) != -1 || errno != EXDEV) {
        state->first_errno = errno;
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
    }
}

static void io_client_task(void *arg) {
    multi_io_state_t *state = arg;
    unsigned char value = 0U;

    if (llam_write(state->left_fd, &state->payload, 1U) != 1 ||
        llam_read(state->left_fd, &value, 1U) != 1 ||
        value != state->payload) {
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
        return;
    }
    atomic_fetch_add_explicit(&state->echoed, 1U, memory_order_relaxed);
}

static void io_peer_task(void *arg) {
    multi_io_state_t *state = arg;
    unsigned char value = 0U;

    if (llam_read(state->right_fd, &value, 1U) != 1 ||
        llam_write(state->right_fd, &value, 1U) != 1) {
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
    }
}

#if LLAM_PLATFORM_POSIX
static void *run_runtime_thread(void *arg) {
    runtime_runner_t *runner = arg;

    errno = 0;
    runner->rc = llam_runtime_run_handle(runner->runtime);
    runner->err = errno;
    return NULL;
}
#endif

static int init_runtime_opts(llam_runtime_opts_t *opts) {
    if (opts == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (llam_runtime_opts_init(opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts->deterministic = 1U;
    opts->profile = (uint32_t)LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    return 0;
}

static int run_two_runtimes(llam_runtime_t *runtime_a, llam_runtime_t *runtime_b) {
#if LLAM_PLATFORM_POSIX
    runtime_runner_t runner_a;
    runtime_runner_t runner_b;
    pthread_t thread_a;
    pthread_t thread_b;
    int rc;

    memset(&runner_a, 0, sizeof(runner_a));
    memset(&runner_b, 0, sizeof(runner_b));
    runner_a.runtime = runtime_a;
    runner_b.runtime = runtime_b;

    rc = pthread_create(&thread_a, NULL, run_runtime_thread, &runner_a);
    if (rc != 0) {
        errno = rc;
        return -1;
    }
    rc = pthread_create(&thread_b, NULL, run_runtime_thread, &runner_b);
    if (rc != 0) {
        (void)llam_runtime_request_stop_rt(runtime_a);
        pthread_join(thread_a, NULL);
        errno = rc;
        return -1;
    }
    pthread_join(thread_a, NULL);
    pthread_join(thread_b, NULL);
    if (runner_a.rc != 0) {
        errno = runner_a.err;
        return -1;
    }
    if (runner_b.rc != 0) {
        errno = runner_b.err;
        return -1;
    }
    return 0;
#else
    if (llam_runtime_run_handle(runtime_a) != 0) {
        return -1;
    }
    return llam_runtime_run_handle(runtime_b);
#endif
}

static int test_concurrent_spawn_join(void) {
    llam_runtime_opts_t opts;
    multi_counter_state_t state;
    llam_runtime_t *runtime_a = NULL;
    llam_runtime_t *runtime_b = NULL;
    llam_task_t *tasks_a[MULTI_RUNTIME_TASKS];
    llam_task_t *tasks_b[MULTI_RUNTIME_TASKS];
    unsigned i;
    int rc = 1;

    memset(tasks_a, 0, sizeof(tasks_a));
    memset(tasks_b, 0, sizeof(tasks_b));
    atomic_init(&state.ran_a, 0U);
    atomic_init(&state.ran_b, 0U);
    atomic_init(&state.failures, 0U);

    if (init_runtime_opts(&opts) != 0) {
        return test_fail_errno("runtime opts init failed");
    }
    if (llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime_a) != 0 ||
        llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime_b) != 0) {
        rc = test_fail_errno("runtime create failed");
        goto cleanup;
    }
    if (runtime_a == runtime_b || runtime_a == llam_runtime_default() || runtime_b == llam_runtime_default()) {
        rc = test_fail("runtime handles are not independent heap handles");
        goto cleanup;
    }

    for (i = 0U; i < MULTI_RUNTIME_TASKS; ++i) {
        tasks_a[i] = llam_runtime_spawn_ex(runtime_a, counter_task_a, &state, NULL, 0U);
        tasks_b[i] = llam_runtime_spawn_ex(runtime_b, counter_task_b, &state, NULL, 0U);
        if (tasks_a[i] == NULL || tasks_b[i] == NULL) {
            rc = test_fail_errno("runtime spawn failed");
            goto cleanup;
        }
    }
    if (run_two_runtimes(runtime_a, runtime_b) != 0) {
        rc = test_fail_errno("concurrent runtime run failed");
        goto cleanup;
    }
    for (i = 0U; i < MULTI_RUNTIME_TASKS; ++i) {
        if (llam_join(tasks_a[i]) != 0 || llam_join(tasks_b[i]) != 0) {
            rc = test_fail_errno("host join after concurrent run failed");
            goto cleanup;
        }
        tasks_a[i] = NULL;
        tasks_b[i] = NULL;
    }
    if (atomic_load_explicit(&state.ran_a, memory_order_relaxed) != MULTI_RUNTIME_TASKS ||
        atomic_load_explicit(&state.ran_b, memory_order_relaxed) != MULTI_RUNTIME_TASKS) {
        rc = test_fail("not all explicit-runtime tasks ran");
        goto cleanup;
    }
    if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
        rc = test_fail("explicit-runtime task-local channel/sleep check failed");
        goto cleanup;
    }
    rc = 0;

cleanup:
    llam_runtime_destroy(runtime_b);
    llam_runtime_destroy(runtime_a);
    return rc;
}

static int test_cross_runtime_task_owner(void) {
    llam_runtime_opts_t opts;
    llam_runtime_t *runtime_a = NULL;
    llam_runtime_t *runtime_b = NULL;
    llam_task_t *diag_task = NULL;
    llam_task_t *foreign_task = NULL;
    cross_owner_state_t cross;
    atomic_uint foreign_ran;
    int rc = 1;

    memset(&cross, 0, sizeof(cross));
    atomic_init(&cross.failures, 0U);
    atomic_init(&foreign_ran, 0U);

    if (init_runtime_opts(&opts) != 0) {
        return test_fail_errno("runtime opts init failed");
    }
    if (llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime_a) != 0 ||
        llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime_b) != 0) {
        rc = test_fail_errno("runtime create for cross-owner test failed");
        goto cleanup;
    }

    foreign_task = llam_runtime_spawn_ex(runtime_b, foreign_target_task, &foreign_ran, NULL, 0U);
    if (foreign_task == NULL) {
        rc = test_fail_errno("foreign runtime spawn failed");
        goto cleanup;
    }
    cross.foreign_task = foreign_task;
    diag_task = llam_runtime_spawn_ex(runtime_a, cross_owner_task, &cross, NULL, 0U);
    if (diag_task == NULL) {
        rc = test_fail_errno("diagnostic runtime spawn failed");
        goto cleanup;
    }
    if (run_two_runtimes(runtime_a, runtime_b) != 0) {
        rc = test_fail_errno("cross-owner runtime run failed");
        goto cleanup;
    }
    if (llam_join(diag_task) != 0) {
        rc = test_fail_errno("join diagnostic task failed");
        goto cleanup;
    }
    diag_task = NULL;
    if (llam_join(foreign_task) != 0) {
        rc = test_fail_errno("join foreign task failed");
        goto cleanup;
    }
    foreign_task = NULL;
    if (atomic_load_explicit(&cross.failures, memory_order_relaxed) != 0U) {
        errno = cross.first_errno;
        rc = test_fail_errno("cross-runtime task operation did not fail with EXDEV");
        goto cleanup;
    }
    if (atomic_load_explicit(&foreign_ran, memory_order_relaxed) != 1U) {
        rc = test_fail("foreign runtime task did not run");
        goto cleanup;
    }
    rc = 0;

cleanup:
    llam_runtime_destroy(runtime_b);
    llam_runtime_destroy(runtime_a);
    return rc;
}

static int test_cross_runtime_object_owner(void) {
    llam_runtime_opts_t opts;
    llam_runtime_t *runtime_a = NULL;
    llam_runtime_t *runtime_b = NULL;
    llam_task_t *creator_task = NULL;
    llam_task_t *probe_task = NULL;
    cross_object_state_t state;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);

    if (init_runtime_opts(&opts) != 0) {
        return test_fail_errno("runtime opts init failed");
    }
    if (llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime_a) != 0 ||
        llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime_b) != 0) {
        rc = test_fail_errno("runtime create for cross-object test failed");
        goto cleanup;
    }

    creator_task = llam_runtime_spawn_ex(runtime_b, foreign_object_creator_task, &state, NULL, 0U);
    if (creator_task == NULL) {
        rc = test_fail_errno("foreign object creator spawn failed");
        goto cleanup;
    }
    if (llam_runtime_run_handle(runtime_b) != 0 || llam_join(creator_task) != 0) {
        creator_task = NULL;
        rc = test_fail_errno("foreign object creator did not complete");
        goto cleanup;
    }
    creator_task = NULL;
    if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U ||
        state.foreign_channel == NULL ||
        state.foreign_mutex == NULL) {
        errno = state.first_errno;
        rc = test_fail_errno("foreign object creation failed");
        goto cleanup;
    }

    probe_task = llam_runtime_spawn_ex(runtime_a, cross_object_task, &state, NULL, 0U);
    if (probe_task == NULL) {
        rc = test_fail_errno("cross-object probe spawn failed");
        goto cleanup;
    }
    if (llam_runtime_run_handle(runtime_a) != 0 || llam_join(probe_task) != 0) {
        probe_task = NULL;
        rc = test_fail_errno("cross-object probe did not complete");
        goto cleanup;
    }
    probe_task = NULL;
    if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
        errno = state.first_errno;
        rc = test_fail_errno("cross-runtime object operation did not fail with EXDEV");
        goto cleanup;
    }
    rc = 0;

cleanup:
    if (creator_task != NULL) {
        (void)llam_detach(creator_task);
    }
    if (probe_task != NULL) {
        (void)llam_detach(probe_task);
    }
    if (state.foreign_mutex != NULL) {
        (void)llam_mutex_destroy(state.foreign_mutex);
    }
    if (state.foreign_channel != NULL) {
        (void)llam_channel_destroy(state.foreign_channel);
    }
    llam_runtime_destroy(runtime_b);
    llam_runtime_destroy(runtime_a);
    return rc;
}

static int test_concurrent_runtime_io(void) {
#if LLAM_PLATFORM_POSIX
    llam_runtime_opts_t opts;
    llam_runtime_t *runtime_a = NULL;
    llam_runtime_t *runtime_b = NULL;
    llam_task_t *tasks[4] = {NULL, NULL, NULL, NULL};
    multi_io_state_t state_a;
    multi_io_state_t state_b;
    int fds_a[2] = {-1, -1};
    int fds_b[2] = {-1, -1};
    int rc = 1;

    memset(&state_a, 0, sizeof(state_a));
    memset(&state_b, 0, sizeof(state_b));
    state_a.left_fd = LLAM_INVALID_FD;
    state_a.right_fd = LLAM_INVALID_FD;
    state_b.left_fd = LLAM_INVALID_FD;
    state_b.right_fd = LLAM_INVALID_FD;
    state_a.payload = 0x5aU;
    state_b.payload = 0xa5U;
    atomic_init(&state_a.echoed, 0U);
    atomic_init(&state_a.failures, 0U);
    atomic_init(&state_b.echoed, 0U);
    atomic_init(&state_b.failures, 0U);

    if (init_runtime_opts(&opts) != 0) {
        return test_fail_errno("runtime opts init failed");
    }
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds_a) != 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, fds_b) != 0) {
        rc = test_fail_errno("socketpair for multi-runtime I/O failed");
        goto cleanup;
    }
    state_a.left_fd = fds_a[0];
    state_a.right_fd = fds_a[1];
    state_b.left_fd = fds_b[0];
    state_b.right_fd = fds_b[1];

    if (llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime_a) != 0 ||
        llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime_b) != 0) {
        rc = test_fail_errno("runtime create for multi-runtime I/O failed");
        goto cleanup;
    }
    tasks[0] = llam_runtime_spawn_ex(runtime_a, io_client_task, &state_a, NULL, 0U);
    tasks[1] = llam_runtime_spawn_ex(runtime_a, io_peer_task, &state_a, NULL, 0U);
    tasks[2] = llam_runtime_spawn_ex(runtime_b, io_client_task, &state_b, NULL, 0U);
    tasks[3] = llam_runtime_spawn_ex(runtime_b, io_peer_task, &state_b, NULL, 0U);
    if (tasks[0] == NULL || tasks[1] == NULL || tasks[2] == NULL || tasks[3] == NULL) {
        rc = test_fail_errno("spawn for multi-runtime I/O failed");
        goto cleanup;
    }
    if (run_two_runtimes(runtime_a, runtime_b) != 0) {
        rc = test_fail_errno("multi-runtime I/O run failed");
        goto cleanup;
    }
    for (unsigned i = 0U; i < 4U; ++i) {
        if (llam_join(tasks[i]) != 0) {
            rc = test_fail_errno("multi-runtime I/O join failed");
            goto cleanup;
        }
        tasks[i] = NULL;
    }
    if (atomic_load_explicit(&state_a.echoed, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state_b.echoed, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state_a.failures, memory_order_relaxed) != 0U ||
        atomic_load_explicit(&state_b.failures, memory_order_relaxed) != 0U) {
        rc = test_fail("multi-runtime I/O echo failed");
        goto cleanup;
    }
    rc = 0;

cleanup:
    for (unsigned i = 0U; i < 4U; ++i) {
        if (tasks[i] != NULL) {
            (void)llam_detach(tasks[i]);
        }
    }
    if (fds_a[0] >= 0) {
        close(fds_a[0]);
    }
    if (fds_a[1] >= 0) {
        close(fds_a[1]);
    }
    if (fds_b[0] >= 0) {
        close(fds_b[0]);
    }
    if (fds_b[1] >= 0) {
        close(fds_b[1]);
    }
    llam_runtime_destroy(runtime_b);
    llam_runtime_destroy(runtime_a);
    return rc;
#else
    return 0;
#endif
}

int main(void) {
    if (test_concurrent_spawn_join() != 0) {
        return 1;
    }
    if (test_cross_runtime_task_owner() != 0) {
        return 1;
    }
    if (test_cross_runtime_object_owner() != 0) {
        return 1;
    }
    if (test_concurrent_runtime_io() != 0) {
        return 1;
    }
    printf("[test_multi_runtime_core] ok\n");
    return 0;
}
