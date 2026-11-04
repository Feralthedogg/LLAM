/*
 * Copyright 2026 Feralthedogg
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
#include <stdio.h>

#if LLAM_PLATFORM_WINDOWS
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "test_api_usability:%u: %s (errno=%d)\n", \
                (unsigned)__LINE__, #expression, errno); \
        goto done; \
    } \
} while (0)

typedef struct object_case {
    llam_runtime_t *peer;
    llam_channel_t *channel;
    llam_mutex_t *mutex;
    llam_cond_t *cond;
    llam_cancel_token_t *token;
    llam_timer_t *timer;
    unsigned ran;
    unsigned failures;
} object_case_t;

static void check_task(object_case_t *state, bool passed) {
    if (!passed) ++state->failures;
}

static void object_task(void *argument) {
    object_case_t *state = argument;
    void *value = (void *)(uintptr_t)1U;
    check_task(state, llam_channel_send(state->channel, NULL) == 0);
    check_task(state, llam_channel_recv_result(state->channel, &value) == 0 && value == NULL);
    check_task(state, llam_mutex_lock(state->mutex) == 0);
    check_task(state, llam_mutex_unlock(state->mutex) == 0);
    check_task(state, llam_cond_signal(state->cond) == 0);
    check_task(state, llam_cancel_token_is_cancelled(state->token) == 0);
    uint64_t ticks = 0U;
    check_task(state, llam_timer_wait_until(state->timer, llam_now_ns() + UINT64_C(1000000000), &ticks) == 0);
    check_task(state, ticks != 0U);
    errno = 0;
    check_task(state, llam_runtime_channel_create(state->peer, 1U) == NULL && errno == EXDEV);
    errno = 0;
    check_task(state, llam_runtime_request_stop_handle(state->peer) == -1 && errno == EXDEV);
    ++state->ran;
}

static void increment_task(void *argument) {
    unsigned *value = argument;
    ++*value;
}

static int explicit_objects(void) {
    llam_runtime_opts_t opts;
    llam_runtime_t *runtime = NULL, *peer = NULL;
    llam_task_group_t *group = NULL;
    llam_channel_t *legacy = NULL;
    llam_task_t *peer_task = NULL;
    object_case_t state = {0};
    unsigned peer_ran = 0U;
    int sink = -1, result = 1;

    CHECK(llam_runtime_opts_init(&opts, sizeof(opts)) == 0);
    opts.deterministic = 1U;
    CHECK(llam_runtime_create_compat(&opts, sizeof(opts), &runtime) == 0);
    CHECK(llam_runtime_create_compat(&opts, sizeof(opts), &peer) == 0);
    state.peer = peer;
    state.channel = llam_runtime_channel_create(runtime, 2U);
    state.mutex = llam_runtime_mutex_create(runtime);
    state.cond = llam_runtime_cond_create(runtime);
    state.token = llam_runtime_cancel_token_create(runtime);
    group = llam_runtime_task_group_create(runtime);
    CHECK(state.channel != NULL && state.mutex != NULL && state.cond != NULL && state.token != NULL && group != NULL);
    llam_timer_opts_t timer_opts = {.interval_ns = UINT64_C(1000000)};
    CHECK(llam_runtime_timer_create_ex(runtime, &timer_opts, sizeof(timer_opts), &state.timer) == 0);
    CHECK(llam_task_group_spawn(group, object_task, &state, NULL) != NULL);
#if LLAM_PLATFORM_WINDOWS
    sink = _open("NUL", _O_WRONLY);
#else
    sink = open("/dev/null", O_WRONLY);
#endif
    CHECK(sink >= 0);
    CHECK(llam_runtime_write_stats_json_handle(runtime, sink) == 0);
    llam_dump_runtime_state_handle(runtime, sink);
    CHECK(state.ran == 0U); /* Host diagnostics and group ownership never drive G. */
    CHECK(llam_runtime_run_handle(runtime) == 0);
    CHECK(state.ran == 1U && state.failures == 0U);
    CHECK(llam_task_group_join(group) == 0);
    peer_task = llam_runtime_spawn_ex(peer, increment_task, &peer_ran, NULL, 0U);
    CHECK(peer_task != NULL && llam_runtime_run_handle(peer) == 0);
    CHECK(llam_join(peer_task) == 0);
    peer_task = NULL;
    CHECK(peer_ran == 1U);
    /* Legacy host constructors still work before the default runtime is init. */
    legacy = llam_channel_create(1U);
    CHECK(legacy != NULL && llam_channel_destroy(legacy) == 0);
    legacy = NULL;
    CHECK(llam_runtime_channel_create(NULL, 1U) == NULL && errno == EINVAL);
    CHECK(llam_channel_close(state.channel) == 0);
    result = 0;
done:
    if (legacy != NULL) (void)llam_channel_destroy(legacy);
    if (sink >= 0) {
#if LLAM_PLATFORM_WINDOWS
        (void)_close(sink);
#else
        (void)close(sink);
#endif
    }
    if (group != NULL) (void)llam_task_group_destroy(group);
    if (state.channel != NULL) (void)llam_channel_destroy(state.channel);
    if (state.cond != NULL) (void)llam_cond_destroy(state.cond);
    if (state.mutex != NULL) (void)llam_mutex_destroy(state.mutex);
    if (state.timer != NULL) (void)llam_timer_destroy(state.timer);
    if (state.token != NULL) (void)llam_cancel_token_destroy(state.token);
    if (peer != NULL) llam_runtime_destroy(peer);
    if (runtime != NULL) llam_runtime_destroy(runtime);
    return result;
}

typedef struct cancel_case {
    llam_task_group_t *group;
    atomic_uint started;
    bool linked;
    unsigned child_ok;
    unsigned outside_ok;
    unsigned cancel_ok;
} cancel_case_t;

static void group_child(void *argument) {
    cancel_case_t *state = argument;
    atomic_store_explicit(&state->started, 1U, memory_order_release);
    int result = llam_sleep_ns(UINT64_C(5000000));
    state->child_ok = state->linked ? result == -1 && errno == ECANCELED : result == 0;
}

static void group_canceller(void *argument) {
    cancel_case_t *state = argument;
    while (!atomic_load_explicit(&state->started, memory_order_acquire)) llam_yield();
    state->cancel_ok = llam_task_group_cancel(state->group) == 0;
}

static void outside_group(void *argument) {
    cancel_case_t *state = argument;
    state->outside_ok = llam_sleep_ns(UINT64_C(7000000)) == 0;
}

static int group_cancellation(bool linked) {
    llam_runtime_t *runtime = NULL;
    llam_cancel_token_t *external = NULL;
    llam_task_t *outside = NULL, *canceller = NULL;
    cancel_case_t state = {.linked = linked};
    atomic_init(&state.started, 0U);
    int result = 1;
    llam_runtime_opts_t opts;
    CHECK(llam_runtime_opts_init(&opts, sizeof(opts)) == 0);
    opts.deterministic = 1U;
    CHECK(llam_runtime_create_compat(&opts, sizeof(opts), &runtime) == 0);
    external = llam_runtime_cancel_token_create(runtime);
    CHECK(external != NULL);
    llam_task_group_opts_t group_opts = {.flags = linked ? LLAM_TASK_GROUP_F_LINK_CANCEL : 0U};
    state.group = llam_runtime_task_group_create_ex(runtime, &group_opts, sizeof(group_opts));
    CHECK(state.group != NULL);
    llam_spawn_opts_t child_opts;
    CHECK(llam_spawn_opts_init(&child_opts, sizeof(child_opts)) == 0);
    child_opts.cancel_token = external;
    CHECK(llam_task_group_spawn(state.group, group_child, &state, &child_opts) != NULL);
    outside = llam_runtime_spawn_ex(runtime, outside_group, &state, &child_opts, sizeof(child_opts));
    canceller = llam_runtime_spawn_ex(runtime, group_canceller, &state, NULL, 0U);
    CHECK(outside != NULL && canceller != NULL);
    CHECK(llam_runtime_run_handle(runtime) == 0);
    CHECK(llam_task_group_join(state.group) == 0);
    CHECK(llam_join(outside) == 0);
    outside = NULL;
    CHECK(llam_join(canceller) == 0);
    canceller = NULL;
    CHECK(state.child_ok && state.outside_ok && state.cancel_ok);
    CHECK(llam_cancel_token_is_cancelled(external) == 0);
    result = 0;
done:
    if (state.group != NULL) (void)llam_task_group_destroy(state.group);
    if (external != NULL) (void)llam_cancel_token_destroy(external);
    if (runtime != NULL) llam_runtime_destroy(runtime);
    return result;
}

static int byte_pair(llam_fd_t pair[2]) {
#if LLAM_PLATFORM_WINDOWS
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    SOCKET peer = INVALID_SOCKET, client = INVALID_SOCKET;
    struct sockaddr_in addr = {0};
    int len = sizeof(addr), rc = -1;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (listener == INVALID_SOCKET || bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        getsockname(listener, (struct sockaddr *)&addr, &len) != 0 || listen(listener, 1) != 0) goto done;
    client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client == INVALID_SOCKET || connect(client, (struct sockaddr *)&addr, sizeof(addr)) != 0) goto done;
    peer = accept(listener, NULL, NULL);
    if (peer == INVALID_SOCKET) goto done;
    pair[0] = client; pair[1] = peer; rc = 0;
done:
    if (listener != INVALID_SOCKET) closesocket(listener);
    if (rc != 0) {
        if (client != INVALID_SOCKET) closesocket(client);
        if (peer != INVALID_SOCKET) closesocket(peer);
    }
    return rc;
#else
    return socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
#endif
}

static void close_pair(llam_fd_t pair[2]) {
    for (unsigned i = 0U; i < 2U; ++i) {
        if (pair[i] == LLAM_INVALID_FD) continue;
#if LLAM_PLATFORM_WINDOWS
        (void)closesocket(pair[i]);
#else
        (void)close(pair[i]);
#endif
        pair[i] = LLAM_INVALID_FD;
    }
}

typedef struct io_case {
    llam_fd_t pair[2];
    llam_cancel_token_t *token;
    uint64_t request_deadline;
    atomic_uint started;
    unsigned passed;
} io_case_t;

static void bounded_read_task(void *argument) {
    io_case_t *state = argument;
    unsigned char data[8] = {0};
    size_t n = SIZE_MAX;
    if (llam_sleep_ns(UINT64_C(1000000)) != 0) return;
    llam_wait_opts_t opts;
    if (llam_wait_opts_init(&opts, sizeof(opts)) != 0) return;
    opts.deadline_ns = llam_now_ns() + UINT64_C(10000000);
    if (llam_read_exact_ex(state->pair[0], data, sizeof(data), &opts, sizeof(opts), &n) != -1 ||
        errno != ETIMEDOUT || n != 3U || memcmp(data, "abc", 3U) != 0) return;
    /* The first sleep and shorter call deadline must not erase the task budget. */
    if (llam_read_until(state->pair[0], data, 1U, llam_now_ns() + UINT64_C(2000000000)) != -1 ||
        errno != ETIMEDOUT || llam_now_ns() > state->request_deadline + UINT64_C(1000000000)) return;
    if (llam_write_until(state->pair[0], "x", 1U, llam_now_ns() + UINT64_C(100000000)) != -1 ||
        errno != ETIMEDOUT) return;
    state->passed = 1U;
}

static void cancellable_read_task(void *argument) {
    io_case_t *state = argument;
    unsigned char data[8] = {0};
    size_t n = 0U;
    llam_wait_opts_t opts = {.cancel_token = state->token};
    atomic_store_explicit(&state->started, 1U, memory_order_release);
    if (llam_read_exact_ex(state->pair[0], data, sizeof(data), &opts, sizeof(opts), &n) != -1 ||
        errno != ECANCELED || n != 3U || memcmp(data, "abc", 3U) != 0) return;
    /* A completed scope releases its token and does not cancel the next call. */
    opts.cancel_token = NULL;
    opts.deadline_ns = llam_now_ns() + UINT64_C(1000000000);
    if (llam_read_ex(state->pair[0], data, 1U, &opts, sizeof(opts)) != 1 || data[0] != 'z') return;
    state->passed = 1U;
}

static void *cancel_io_host(void *argument) {
    io_case_t *state = argument;
    while (!atomic_load_explicit(&state->started, memory_order_acquire)) llam_pause_cpu();
    const struct timespec delay = {.tv_sec = 0, .tv_nsec = 3000000L};
    (void)nanosleep(&delay, NULL);
    (void)llam_cancel_token_cancel(state->token);
    (void)nanosleep(&delay, NULL);
    (void)send(state->pair[1], "z", 1, 0);
    return NULL;
}

static int io_wait_options(bool cancel) {
    llam_runtime_t *runtime = NULL;
    llam_runtime_opts_t opts;
    llam_task_t *task = NULL;
    io_case_t state = {.pair = {LLAM_INVALID_FD, LLAM_INVALID_FD}};
    atomic_init(&state.started, 0U);
    pthread_t host;
    bool host_started = false;
    int result = 1;
    CHECK(llam_runtime_opts_init(&opts, sizeof(opts)) == 0);
    opts.deterministic = 1U;
    CHECK(llam_runtime_create_compat(&opts, sizeof(opts), &runtime) == 0);
    CHECK(byte_pair(state.pair) == 0);
    CHECK(send(state.pair[1], "abc", 3, 0) == 3);
    if (cancel) {
        state.token = llam_runtime_cancel_token_create(runtime);
        CHECK(state.token != NULL);
        CHECK(pthread_create(&host, NULL, cancel_io_host, &state) == 0);
        host_started = true;
    }
    llam_spawn_opts_t spawn_opts;
    CHECK(llam_spawn_opts_init(&spawn_opts, sizeof(spawn_opts)) == 0);
    state.request_deadline = llam_now_ns() + UINT64_C(500000000);
    spawn_opts.deadline_ns = cancel ? 0U : state.request_deadline;
    task = llam_runtime_spawn_ex(runtime, cancel ? cancellable_read_task : bounded_read_task,
                                 &state, &spawn_opts, sizeof(spawn_opts));
    CHECK(task != NULL && llam_runtime_run_handle(runtime) == 0);
    CHECK(llam_join(task) == 0);
    task = NULL;
    if (host_started) { CHECK(pthread_join(host, NULL) == 0); host_started = false; }
    CHECK(state.passed == 1U);
    if (state.token != NULL) { CHECK(llam_cancel_token_destroy(state.token) == 0); state.token = NULL; }
    result = 0;
done:
    if (host_started) {
        atomic_store_explicit(&state.started, 1U, memory_order_release);
        (void)pthread_join(host, NULL);
    }
    if (runtime != NULL) llam_runtime_destroy(runtime);
    if (state.token != NULL) (void)llam_cancel_token_destroy(state.token);
    close_pair(state.pair);
    return result;
}

static void no_work(void *argument) { (void)argument; }

static int resource_admission(void) {
    int result = 1;
    llam_runtime_t *runtime = NULL;
    llam_runtime_opts_t opts;
    llam_task_t *task = NULL;
    llam_runtime_resources_t info;
    CHECK(llam_runtime_opts_init(&opts, sizeof(opts)) == 0);
    opts.worker_count = 1U;
    opts.blocking_worker_count = 1U;
    opts.max_tasks = 1U;
    CHECK(llam_runtime_create_compat(&opts, sizeof(opts), &runtime) == 0);
    task = llam_runtime_spawn_ex(runtime, no_work, NULL, NULL, 0U);
    CHECK(task != NULL);
    CHECK(llam_runtime_drain_until(runtime, llam_now_ns(), &info, sizeof(info)) == -1 && errno == EBUSY);
    CHECK((info.flags & LLAM_RUNTIME_RESOURCE_F_NEEDS_RUNNER) != 0U);
    CHECK((info.flags & LLAM_RUNTIME_RESOURCE_F_DRAINING) == 0U);
    CHECK(llam_runtime_spawn_ex(runtime, no_work, NULL, NULL, 0U) == NULL && errno == EAGAIN);
    CHECK(llam_runtime_run_handle(runtime) == 0);
    CHECK(llam_runtime_get_resources_ex(runtime, &info, sizeof(info)) == 0);
    CHECK(info.worker_count == 1U && info.blocking_worker_count == 1U &&
          info.admitted_tasks == 1U && info.live_tasks == 0U);
    CHECK(llam_runtime_spawn_ex(runtime, no_work, NULL, NULL, 0U) == NULL && errno == EAGAIN);
    CHECK(llam_join(task) == 0);
    task = llam_runtime_spawn_ex(runtime, no_work, NULL, NULL, 0U);
    CHECK(task != NULL && llam_runtime_run_handle(runtime) == 0 && llam_join(task) == 0);
    task = NULL;
    CHECK(llam_runtime_drain_until(runtime, llam_now_ns(), &info, sizeof(info)) == 0);
    CHECK((info.flags & (LLAM_RUNTIME_RESOURCE_F_DRAINING | LLAM_RUNTIME_RESOURCE_F_QUIESCENT)) ==
          (LLAM_RUNTIME_RESOURCE_F_DRAINING | LLAM_RUNTIME_RESOURCE_F_QUIESCENT));
    CHECK(llam_runtime_spawn_ex(runtime, no_work, NULL, NULL, 0U) == NULL && errno == ECANCELED);
    CHECK(llam_runtime_mutex_create(runtime) == NULL && errno == ECANCELED);
    CHECK(llam_runtime_request_stop_handle(runtime) == 0);
    result = 0;
done:
    if (runtime != NULL) llam_runtime_destroy(runtime);
    return result;
}

typedef struct drain_case {
    llam_runtime_t *runtime;
    atomic_uint entered, release, rejected, finished, creation_closed;
    int run_result;
} drain_case_t;

static void *held_callback(void *argument) {
    drain_case_t *state = argument;
    atomic_store_explicit(&state->entered, 1U, memory_order_release);
    while (atomic_load_explicit(&state->release, memory_order_acquire) == 0U) {
        struct timespec interval = {0, 1000000L};
        (void)nanosleep(&interval, NULL);
    }
    atomic_store_explicit(&state->finished, 1U, memory_order_release);
    return state;
}

static void held_task(void *argument) {
    void *value = NULL;
    (void)llam_call_blocking_result(held_callback, argument, &value);
    drain_case_t *state = argument;
    llam_mutex_t *mutex = llam_mutex_create();
    if (mutex == NULL && errno == ECANCELED)
        atomic_store_explicit(&state->creation_closed, 1U, memory_order_release);
    else if (mutex != NULL) (void)llam_mutex_destroy(mutex);
}

static void *unused_callback(void *argument) {
    drain_case_t *state = argument;
    atomic_store_explicit(&state->rejected, 2U, memory_order_release);
    return NULL;
}

static void admission_task(void *argument) {
    drain_case_t *state = argument;
    while (atomic_load_explicit(&state->entered, memory_order_acquire) == 0U) llam_yield();
    void *value = NULL;
    if (llam_call_blocking_result(unused_callback, state, &value) == -1 && errno == EAGAIN)
        atomic_store_explicit(&state->rejected, 1U, memory_order_release);
}

static void *run_driver(void *argument) {
    drain_case_t *state = argument;
    state->run_result = llam_runtime_run_handle(state->runtime);
    return NULL;
}

static int bounded_drain(void) {
    int result = 1;
    drain_case_t state = {0};
    atomic_init(&state.entered, 0U); atomic_init(&state.release, 0U);
    atomic_init(&state.rejected, 0U); atomic_init(&state.finished, 0U);
    atomic_init(&state.creation_closed, 0U);
    llam_runtime_opts_t opts;
    llam_runtime_resources_t info;
    llam_task_t *held = NULL, *other = NULL;
    pthread_t driver;
    bool driver_started = false;
    CHECK(llam_runtime_opts_init(&opts, sizeof(opts)) == 0);
    opts.worker_count = 1U;
    opts.blocking_worker_count = 1U;
    opts.max_blocking_jobs = 1U;
    CHECK(llam_runtime_create_compat(&opts, sizeof(opts), &state.runtime) == 0);
    held = llam_runtime_spawn_ex(state.runtime, held_task, &state, NULL, 0U);
    other = llam_runtime_spawn_ex(state.runtime, admission_task, &state, NULL, 0U);
    CHECK(held != NULL && other != NULL);
    CHECK(pthread_create(&driver, NULL, run_driver, &state) == 0);
    driver_started = true;
    uint64_t deadline = llam_now_ns() + UINT64_C(3000000000);
    while (atomic_load_explicit(&state.rejected, memory_order_acquire) == 0U && llam_now_ns() < deadline) {
        struct timespec interval = {0, 1000000L};
        (void)nanosleep(&interval, NULL);
    }
    CHECK(atomic_load_explicit(&state.rejected, memory_order_acquire) == 1U);
    CHECK(llam_runtime_drain_until(state.runtime, llam_now_ns() + UINT64_C(10000000), &info, sizeof(info)) == -1 &&
          errno == ETIMEDOUT);
    CHECK(info.blocking_jobs == 1U && (info.flags & LLAM_RUNTIME_RESOURCE_F_DRAINING) != 0U);
    CHECK(atomic_load_explicit(&state.finished, memory_order_acquire) == 0U);
    atomic_store_explicit(&state.release, 1U, memory_order_release);
    CHECK(llam_runtime_drain_until(state.runtime, llam_now_ns() + UINT64_C(3000000000), &info, sizeof(info)) == 0);
    CHECK(pthread_join(driver, NULL) == 0); driver_started = false;
    CHECK(state.run_result == 0 && atomic_load_explicit(&state.finished, memory_order_acquire) == 1U &&
          atomic_load_explicit(&state.creation_closed, memory_order_acquire) == 1U);
    CHECK(llam_join(held) == 0); held = NULL;
    CHECK(llam_join(other) == 0); other = NULL;
    result = 0;
done:
    atomic_store_explicit(&state.release, 1U, memory_order_release);
    if (driver_started) (void)pthread_join(driver, NULL);
    if (state.runtime != NULL) llam_runtime_destroy(state.runtime);
    return result;
}

typedef struct result_case {
    llam_task_local_key_t owned_key, taken_key;
    unsigned cleaned, guarded, passed;
} result_case_t;

static void cleanup_value(void *argument) {
    result_case_t *state = argument;
    state->cleaned += 1U;
    if (llam_sleep_ns(1U) == -1 && errno == ENOTSUP) state->guarded += 1U;
}

static void result_task(void *argument) {
    result_case_t *state = argument;
    void *taken = NULL;
    if (llam_task_local_set_owned(state->owned_key, state, cleanup_value) != 0 ||
        llam_task_local_set(state->owned_key, NULL) != -1 || errno != EBUSY ||
        llam_task_local_set_owned(state->taken_key, state, cleanup_value) != 0 ||
        llam_task_local_take_result(state->taken_key, &taken) != 0 || taken != state ||
        llam_task_local_key_delete(state->owned_key) != 0 ||
        llam_runtime_request_stop() != 0 ||
        llam_task_set_result(NULL, 42) != 0) return;
    state->passed = 1U;
}

static int task_results_cleanup(void) {
    int result = 1;
    result_case_t state = {0};
    llam_runtime_t *runtime = NULL;
    llam_task_t *task = NULL;
    llam_task_result_t value = {(void *)(uintptr_t)1U, -1, UINT32_MAX};
    CHECK(llam_task_local_key_create(&state.owned_key) == 0);
    CHECK(llam_task_local_key_create(&state.taken_key) == 0);
    CHECK(llam_runtime_create_compat(NULL, 0U, &runtime) == 0);
    task = llam_runtime_spawn_ex(runtime, result_task, &state, NULL, 0U);
    CHECK(task != NULL);
    CHECK(llam_task_get_result(task, &value, sizeof(value)) == -1 && errno == EAGAIN);
    CHECK(llam_join_result_until(task, 0U, &value, sizeof(value)) == -1 && errno == ETIMEDOUT);
    CHECK(value.flags == UINT32_MAX && value.error == -1);
    CHECK(llam_runtime_run_handle(runtime) == 0);
    CHECK(state.passed == 1U && state.cleaned == 1U && state.guarded == 1U);
    CHECK(llam_task_get_result(task, &value, sizeof(value)) == 0);
    CHECK(value.value == NULL && value.error == 42 && value.flags == LLAM_TASK_RESULT_F_SET);
    CHECK(llam_join_result(task, &value, sizeof(value)) == 0);
    task = NULL;
    CHECK(value.value == NULL && value.error == 42 && value.flags == LLAM_TASK_RESULT_F_SET);
    result = 0;
done:
    if (runtime != NULL) llam_runtime_destroy(runtime);
    if (state.owned_key != 0U) (void)llam_task_local_key_delete(state.owned_key);
    if (state.taken_key != 0U) (void)llam_task_local_key_delete(state.taken_key);
    return result;
}

static int group_cleanup_after_destroy(void) {
    int result = 1;
    llam_runtime_t *runtime = NULL;
    llam_task_group_t *group = NULL;
    CHECK(llam_runtime_create_compat(NULL, 0U, &runtime) == 0);
    group = llam_runtime_task_group_create(runtime);
    CHECK(group != NULL && llam_task_group_spawn_ex(group, no_work, NULL, NULL, 0U) != NULL);
    CHECK(llam_task_group_destroy(group) == -1 && errno == EBUSY);
    llam_runtime_destroy(runtime); runtime = NULL;
    CHECK(llam_task_group_destroy(group) == 0); group = NULL;
    result = 0;
done:
    if (runtime != NULL) llam_runtime_destroy(runtime);
    if (group != NULL) (void)llam_task_group_destroy(group);
    return result;
}

typedef struct transfer_case {
    llam_fd_t pair[2];
    llam_cancel_token_t *foreign_token;
    unsigned passed;
} transfer_case_t;

static void transfer_edges_task(void *argument) {
    transfer_case_t *state = argument;
    unsigned char bytes[8] = {0};
    llam_wait_opts_t opts = {.cancel_token = state->foreign_token};
    if (llam_read_ex(state->pair[0], bytes, 1U, &opts, sizeof(opts)) != -1 || errno != EXDEV) return;
    opts.cancel_token = NULL;
    opts.flags = 1U;
    if (llam_read_ex(state->pair[0], bytes, 1U, &opts, sizeof(opts)) != -1 || errno != EINVAL) return;
    size_t n = SIZE_MAX;
    if (llam_read_exact(state->pair[0], bytes, sizeof(bytes), &n) != -1 ||
        errno != EPIPE || n != 3U || memcmp(bytes, "abc", 3U) != 0) return;
    if (llam_write_all(state->pair[0], "xyz", 3U, &n) != 0 || n != 3U) return;
    if (llam_write_all(state->pair[0], NULL, 0U, &n) != 0 || n != 0U) return;
    if (llam_read_exact(state->pair[0], NULL, 0U, &n) != 0 || n != 0U) return;
    if (llam_read_exact(state->pair[0], bytes, 1U, NULL) != -1 || errno != EINVAL) return;
    short revents = (short)-1;
    if (llam_poll_fd_ex(state->pair[0], -1, &revents, NULL, 0U) != -1 || errno != EINVAL) return;
    state->passed = 1U;
}

static int transfer_edges(void) {
    int result = 1;
    llam_runtime_t *runtime = NULL, *peer = NULL;
    transfer_case_t state = {.pair = {LLAM_INVALID_FD, LLAM_INVALID_FD}};
    llam_task_t *task = NULL;
    CHECK(llam_runtime_create_compat(NULL, 0U, &runtime) == 0);
    CHECK(llam_runtime_create_compat(NULL, 0U, &peer) == 0);
    state.foreign_token = llam_runtime_cancel_token_create(peer);
    CHECK(state.foreign_token != NULL && byte_pair(state.pair) == 0);
    CHECK(send(state.pair[1], "abc", 3, 0) == 3);
#if LLAM_PLATFORM_WINDOWS
    CHECK(shutdown(state.pair[1], SD_SEND) == 0);
#else
    CHECK(shutdown(state.pair[1], SHUT_WR) == 0);
#endif
    task = llam_runtime_spawn_ex(runtime, transfer_edges_task, &state, NULL, 0U);
    CHECK(task != NULL && llam_runtime_run_handle(runtime) == 0 && llam_join(task) == 0);
    CHECK(state.passed == 1U);
    char bytes[3];
    CHECK(recv(state.pair[1], bytes, 3, 0) == 3 && memcmp(bytes, "xyz", 3U) == 0);
    result = 0;
done:
    if (runtime != NULL) llam_runtime_destroy(runtime);
    if (state.foreign_token != NULL) (void)llam_cancel_token_destroy(state.foreign_token);
    if (peer != NULL) llam_runtime_destroy(peer);
    close_pair(state.pair);
    return result;
}

int main(void) {
    if (explicit_objects() != 0) return 1;
    if (group_cancellation(false) != 0 || group_cancellation(true) != 0) return 1;
    if (io_wait_options(false) != 0 || io_wait_options(true) != 0) return 1;
    if (resource_admission() != 0 || bounded_drain() != 0) return 1;
    if (task_results_cleanup() != 0 || group_cleanup_after_destroy() != 0 || transfer_edges() != 0) return 1;
    puts("test_api_usability: ownership, linked cancellation, budgets, progress, admission, drain, results and cleanup passed");
    return 0;
}
