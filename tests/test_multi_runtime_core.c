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

#if LLAM_PLATFORM_POSIX
#define HOST_TRY_RACE_ITERS 4000U

typedef struct runtime_destroy_race_state {
    llam_runtime_t *runtime;
    pthread_mutex_t lock;
    pthread_cond_t cv;
    unsigned ready;
    bool go;
} runtime_destroy_race_state_t;

typedef struct runtime_destroy_running_state {
    atomic_uint entered;
} runtime_destroy_running_state_t;

typedef struct host_try_default_race_state {
    llam_runtime_t *runtime;
    llam_channel_t *channel;
    llam_runtime_opts_t opts;
    atomic_uint failures;
} host_try_default_race_state_t;
#endif

static int test_fail(const char *message);
static int test_fail_errno(const char *message);
static int init_runtime_opts(llam_runtime_opts_t *opts);

static int test_sync_handle_family_confusion(void) {
    llam_channel_t *channel;
    llam_mutex_t *mutex;
    llam_cond_t *cond;
    llam_cancel_token_t *token;
    llam_task_group_t *group;
    uintptr_t handles[5];
    int rc = 1;

    channel = llam_channel_create(1U);
    mutex = llam_mutex_create();
    cond = llam_cond_create();
    token = llam_cancel_token_create();
    group = llam_task_group_create();
    if (channel == NULL || mutex == NULL || cond == NULL || token == NULL || group == NULL) {
        rc = test_fail_errno("sync handle family-confusion setup failed");
        goto cleanup;
    }
    handles[0] = (uintptr_t)channel;
    handles[1] = (uintptr_t)mutex;
    handles[2] = (uintptr_t)cond;
    handles[3] = (uintptr_t)token;
    handles[4] = (uintptr_t)group;
    for (size_t i = 0U; i < sizeof(handles) / sizeof(handles[0]); ++i) {
        for (size_t j = i + 1U; j < sizeof(handles) / sizeof(handles[0]); ++j) {
            if (handles[i] == handles[j]) {
                rc = test_fail("public handle families produced colliding encoded values");
                goto cleanup;
            }
        }
    }

    /*
     * Encoded public handles must carry enough family information that an FFI
     * caller cannot pass a channel handle to the mutex API and accidentally
     * operate on a live object occupying the same slot/generation in another
     * registry table.  Exercise several destructive APIs so a regression cannot
     * hide behind one correctly-hardened family.
     */
    errno = 0;
    if (llam_mutex_destroy((llam_mutex_t *)channel) != -1 || errno != EINVAL) {
        rc = test_fail_errno("channel handle was accepted by mutex destroy");
        mutex = NULL;
        goto cleanup;
    }
    errno = 0;
    if (llam_channel_destroy((llam_channel_t *)mutex) != -1 || errno != EINVAL) {
        rc = test_fail_errno("mutex handle was accepted by channel destroy");
        channel = NULL;
        goto cleanup;
    }
    errno = 0;
    if (llam_cond_destroy((llam_cond_t *)token) != -1 || errno != EINVAL) {
        rc = test_fail_errno("cancel token handle was accepted by cond destroy");
        cond = NULL;
        goto cleanup;
    }
    errno = 0;
    if (llam_cancel_token_destroy((llam_cancel_token_t *)cond) != -1 || errno != EINVAL) {
        rc = test_fail_errno("cond handle was accepted by cancel token destroy");
        token = NULL;
        goto cleanup;
    }
    errno = 0;
    if (llam_task_group_destroy((llam_task_group_t *)token) != -1 || errno != EINVAL) {
        rc = test_fail_errno("cancel token handle was accepted by task group destroy");
        group = NULL;
        goto cleanup;
    }
    errno = 0;
    if (llam_cancel_token_cancel((llam_cancel_token_t *)group) != -1 || errno != EINVAL) {
        rc = test_fail_errno("task group handle was accepted by cancel token cancel");
        goto cleanup;
    }

    rc = 0;

cleanup:
    if (group != NULL) {
        (void)llam_task_group_destroy(group);
    }
    if (token != NULL) {
        (void)llam_cancel_token_destroy(token);
    }
    if (cond != NULL) {
        (void)llam_cond_destroy(cond);
    }
    if (mutex != NULL) {
        (void)llam_mutex_destroy(mutex);
    }
    if (channel != NULL) {
        (void)llam_channel_destroy(channel);
    }
    return rc;
}

static int test_runtime_run_handle_rejects_null(void) {
    llam_runtime_opts_t opts;
    int rc = 1;

    if (init_runtime_opts(&opts) != 0) {
        return test_fail_errno("runtime opts init failed");
    }
    if (llam_runtime_init_ex(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return test_fail_errno("default runtime init failed for NULL handle check");
    }

    errno = 0;
    if (llam_runtime_run_handle(NULL) != -1 || errno != EINVAL) {
        rc = test_fail_errno("llam_runtime_run_handle(NULL) did not fail with EINVAL");
        goto cleanup;
    }
    rc = 0;

cleanup:
    llam_runtime_shutdown();
    return rc;
}

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
static void stop_wait_task(void *arg) {
    runtime_destroy_running_state_t *state = arg;

    atomic_store_explicit(&state->entered, 1U, memory_order_release);
    while (llam_sleep_ns(1000000000ULL) == 0) {
        /* Wait until external runtime destruction requests cooperative stop. */
    }
}
#endif

#if LLAM_PLATFORM_POSIX
static void *run_runtime_thread(void *arg) {
    runtime_runner_t *runner = arg;

    errno = 0;
    runner->rc = llam_runtime_run_handle(runner->runtime);
    runner->err = errno;
    return NULL;
}

static void *destroy_runtime_thread(void *arg) {
    runtime_destroy_race_state_t *state = arg;

    pthread_mutex_lock(&state->lock);
    state->ready += 1U;
    pthread_cond_broadcast(&state->cv);
    while (!state->go) {
        pthread_cond_wait(&state->cv, &state->lock);
    }
    pthread_mutex_unlock(&state->lock);

    llam_runtime_destroy(state->runtime);
    return NULL;
}

static void explicit_channel_creator_task(void *arg) {
    host_try_default_race_state_t *state = arg;

    state->channel = llam_channel_create(1U);
    if (state->channel == NULL) {
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
    }
}

static void *default_runtime_toggle_thread(void *arg) {
    host_try_default_race_state_t *state = arg;

    for (unsigned i = 0U;
         i < HOST_TRY_RACE_ITERS && atomic_load_explicit(&state->failures, memory_order_acquire) == 0U;
         ++i) {
        if (llam_runtime_init_ex(&state->opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
            atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
            break;
        }
        llam_runtime_shutdown();
    }
    return NULL;
}

static void *explicit_channel_host_try_thread(void *arg) {
    host_try_default_race_state_t *state = arg;
    int payload = 23;

    for (unsigned i = 0U;
         i < HOST_TRY_RACE_ITERS && atomic_load_explicit(&state->failures, memory_order_acquire) == 0U;
         ++i) {
        void *out = NULL;

        /*
         * This is intentionally unmanaged host code.  The channel belongs to an
         * explicit runtime, so its nonblocking public operations must not sample
         * or race with the legacy default runtime's mutable tuning fields.
         */
        if (llam_channel_try_send(state->channel, &payload) != 0 ||
            llam_channel_try_recv_result(state->channel, &out) != 0 ||
            out != &payload) {
            atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
            break;
        }
    }
    return NULL;
}

static void *default_channel_host_try_thread(void *arg) {
    host_try_default_race_state_t *state = arg;
    int payload = 41;
    bool pending = false;

    for (unsigned i = 0U;
         i < HOST_TRY_RACE_ITERS && atomic_load_explicit(&state->failures, memory_order_acquire) == 0U;
         ++i) {
        void *out = NULL;

        /*
         * The default runtime can be initialized and shut down around a
         * default-owned channel that is still being drained from host code.
         * Host nonblocking operations must not read mutable managed-runtime
         * tuning fields while that default storage is being rebuilt.
         */
        if (!pending) {
            if (llam_channel_try_send(state->channel, &payload) == 0) {
                pending = true;
            } else if (errno != ENOTSUP && errno != EAGAIN) {
                atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
                break;
            }
        }
        if (pending) {
            if (llam_channel_try_recv_result(state->channel, &out) == 0) {
                if (out != &payload) {
                    atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
                    break;
                }
                pending = false;
            } else if (errno != ENOTSUP && errno != EAGAIN) {
                atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
                break;
            }
        }
    }
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

static int test_sequential_runtime_host_join_owner_cleanup(void) {
    llam_runtime_opts_t opts;
    multi_counter_state_t state;
    llam_runtime_t *runtime_a = NULL;
    llam_runtime_t *runtime_b = NULL;
    llam_task_t *task_a = NULL;
    llam_task_t *task_b = NULL;
    int rc = 1;

    atomic_init(&state.ran_a, 0U);
    atomic_init(&state.ran_b, 0U);
    atomic_init(&state.failures, 0U);

    if (init_runtime_opts(&opts) != 0) {
        return test_fail_errno("runtime opts init failed");
    }
    if (llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime_a) != 0 ||
        llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime_b) != 0) {
        rc = test_fail_errno("runtime create for sequential run failed");
        goto cleanup;
    }
    task_a = llam_runtime_spawn_ex(runtime_a, counter_task_a, &state, NULL, 0U);
    task_b = llam_runtime_spawn_ex(runtime_b, counter_task_b, &state, NULL, 0U);
    if (task_a == NULL || task_b == NULL) {
        rc = test_fail_errno("runtime spawn for sequential run failed");
        goto cleanup;
    }

    /*
     * Windows drives this test path without pthread worker wrappers. A finished
     * scheduler run must clear TLS shard/task cursors so unmanaged host cleanup
     * can join handles from either explicit runtime without seeing a false
     * cross-runtime EXDEV.
     */
    if (llam_runtime_run_handle(runtime_a) != 0 || llam_runtime_run_handle(runtime_b) != 0) {
        rc = test_fail_errno("sequential runtime run failed");
        goto cleanup;
    }
    if (llam_join(task_a) != 0 || llam_join(task_b) != 0) {
        rc = test_fail_errno("host join after sequential explicit runs failed");
        goto cleanup;
    }
    task_a = NULL;
    task_b = NULL;
    if (atomic_load_explicit(&state.ran_a, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.ran_b, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
        rc = test_fail("sequential explicit-runtime tasks did not finish cleanly");
        goto cleanup;
    }
    rc = 0;

cleanup:
    if (task_a != NULL) {
        (void)llam_detach(task_a);
    }
    if (task_b != NULL) {
        (void)llam_detach(task_b);
    }
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

static int test_host_try_ops_ignore_default_runtime_race(void) {
#if LLAM_PLATFORM_POSIX
    host_try_default_race_state_t state;
    llam_task_t *creator = NULL;
    pthread_t toggle_thread;
    pthread_t try_thread;
    int rc = 1;
    int err;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    if (init_runtime_opts(&state.opts) != 0) {
        return test_fail_errno("runtime opts init failed");
    }
    if (llam_runtime_create(&state.opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &state.runtime) != 0) {
        return test_fail_errno("runtime create for host try/default race failed");
    }

    creator = llam_runtime_spawn_ex(state.runtime, explicit_channel_creator_task, &state, NULL, 0U);
    if (creator == NULL ||
        llam_runtime_run_handle(state.runtime) != 0 ||
        llam_join(creator) != 0 ||
        state.channel == NULL ||
        atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
        creator = NULL;
        rc = test_fail_errno("explicit runtime channel setup failed");
        goto cleanup;
    }
    creator = NULL;

    err = pthread_create(&toggle_thread, NULL, default_runtime_toggle_thread, &state);
    if (err != 0) {
        errno = err;
        rc = test_fail_errno("default toggle thread create failed");
        goto cleanup;
    }
    err = pthread_create(&try_thread, NULL, explicit_channel_host_try_thread, &state);
    if (err != 0) {
        errno = err;
        atomic_fetch_add_explicit(&state.failures, 1U, memory_order_relaxed);
        pthread_join(toggle_thread, NULL);
        rc = test_fail_errno("explicit channel try thread create failed");
        goto cleanup;
    }
    pthread_join(toggle_thread, NULL);
    pthread_join(try_thread, NULL);
    if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
        rc = test_fail("host try/default runtime race observed an API failure");
        goto cleanup;
    }
    rc = 0;

cleanup:
    if (creator != NULL) {
        (void)llam_detach(creator);
    }
    if (state.channel != NULL) {
        (void)llam_channel_destroy(state.channel);
    }
    llam_runtime_destroy(state.runtime);
    llam_runtime_shutdown();
    return rc;
#else
    return 0;
#endif
}

static int test_default_channel_host_try_ops_ignore_default_runtime_reinit(void) {
#if LLAM_PLATFORM_POSIX
    host_try_default_race_state_t state;
    pthread_t toggle_thread;
    pthread_t try_thread;
    void *out;
    int rc = 1;
    int err;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    if (init_runtime_opts(&state.opts) != 0) {
        return test_fail_errno("runtime opts init failed");
    }

    state.channel = llam_channel_create(1U);
    if (state.channel == NULL) {
        return test_fail_errno("default-owned channel create failed");
    }

    err = pthread_create(&toggle_thread, NULL, default_runtime_toggle_thread, &state);
    if (err != 0) {
        errno = err;
        rc = test_fail_errno("default toggle thread create failed");
        goto cleanup;
    }
    err = pthread_create(&try_thread, NULL, default_channel_host_try_thread, &state);
    if (err != 0) {
        errno = err;
        atomic_fetch_add_explicit(&state.failures, 1U, memory_order_relaxed);
        pthread_join(toggle_thread, NULL);
        rc = test_fail_errno("default channel try thread create failed");
        goto cleanup;
    }
    pthread_join(toggle_thread, NULL);
    pthread_join(try_thread, NULL);
    if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
        rc = test_fail("default channel host try/default runtime reinit race observed an API failure");
        goto cleanup;
    }
    rc = 0;

cleanup:
    if (state.channel != NULL) {
        while (llam_channel_try_recv_result(state.channel, &out) == 0) {
        }
        (void)llam_channel_destroy(state.channel);
    }
    llam_runtime_shutdown();
    return rc;
#else
    return 0;
#endif
}

static int test_concurrent_runtime_destroy_is_single_owner(void) {
#if LLAM_PLATFORM_POSIX
    llam_runtime_opts_t opts;
    runtime_destroy_race_state_t state;
    pthread_t thread_a;
    pthread_t thread_b;
    int rc = 1;
    int err;

    memset(&state, 0, sizeof(state));
    if (init_runtime_opts(&opts) != 0) {
        return test_fail_errno("runtime opts init failed");
    }
    if (llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &state.runtime) != 0) {
        return test_fail_errno("runtime create for destroy race failed");
    }
    err = pthread_mutex_init(&state.lock, NULL);
    if (err != 0) {
        errno = err;
        goto cleanup_runtime;
    }
    err = pthread_cond_init(&state.cv, NULL);
    if (err != 0) {
        errno = err;
        goto cleanup_lock;
    }

    err = pthread_create(&thread_a, NULL, destroy_runtime_thread, &state);
    if (err != 0) {
        errno = err;
        goto cleanup_cv;
    }
    err = pthread_create(&thread_b, NULL, destroy_runtime_thread, &state);
    if (err != 0) {
        errno = err;
        pthread_mutex_lock(&state.lock);
        state.go = true;
        pthread_cond_broadcast(&state.cv);
        pthread_mutex_unlock(&state.lock);
        pthread_join(thread_a, NULL);
        goto cleanup_cv;
    }

    pthread_mutex_lock(&state.lock);
    while (state.ready < 2U) {
        pthread_cond_wait(&state.cv, &state.lock);
    }
    state.go = true;
    pthread_cond_broadcast(&state.cv);
    pthread_mutex_unlock(&state.lock);

    pthread_join(thread_a, NULL);
    pthread_join(thread_b, NULL);
    rc = 0;

cleanup_cv:
    pthread_cond_destroy(&state.cv);
cleanup_lock:
    pthread_mutex_destroy(&state.lock);
cleanup_runtime:
    if (rc != 0) {
        llam_runtime_destroy(state.runtime);
    }
    return rc;
#else
    return 0;
#endif
}

static int test_runtime_destroy_waits_for_active_run(void) {
#if LLAM_PLATFORM_POSIX
    llam_runtime_opts_t opts;
    runtime_runner_t runner;
    runtime_destroy_running_state_t state;
    llam_runtime_t *runtime = NULL;
    llam_task_t *task = NULL;
    pthread_t thread;
    int rc = 1;
    int err;

    memset(&runner, 0, sizeof(runner));
    atomic_init(&state.entered, 0U);
    if (init_runtime_opts(&opts) != 0) {
        return test_fail_errno("runtime opts init failed");
    }
    if (llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime) != 0) {
        return test_fail_errno("runtime create for active destroy failed");
    }
    task = llam_runtime_spawn_ex(runtime, stop_wait_task, &state, NULL, 0U);
    if (task == NULL) {
        rc = test_fail_errno("spawn for active destroy failed");
        goto cleanup;
    }
    runner.runtime = runtime;
    err = pthread_create(&thread, NULL, run_runtime_thread, &runner);
    if (err != 0) {
        errno = err;
        rc = test_fail_errno("run thread for active destroy failed");
        goto cleanup;
    }
    while (atomic_load_explicit(&state.entered, memory_order_acquire) == 0U) {
        llam_pause_cpu();
    }

    /*
     * Destroy from a host thread while another host is still inside
     * llam_runtime_run_handle().  This must request stop, wait until the run
     * function has finished all runtime-state reads, and only then free the
     * heap-backed handle.
     */
    llam_runtime_destroy(runtime);
    runtime = NULL;
    task = NULL;
    pthread_join(thread, NULL);
    if (runner.rc != 0) {
        errno = runner.err;
        rc = test_fail_errno("active runtime destroy made run fail");
        goto cleanup;
    }
    rc = 0;

cleanup:
    if (task != NULL) {
        (void)llam_detach(task);
    }
    if (runtime != NULL) {
        llam_runtime_destroy(runtime);
    }
    return rc;
#else
    return 0;
#endif
}

static int test_destroyed_runtime_handle_address_not_reused(void) {
    llam_runtime_opts_t opts;
    llam_runtime_t *stale = NULL;

    if (init_runtime_opts(&opts) != 0) {
        return test_fail_errno("runtime opts init failed");
    }
    if (llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &stale) != 0) {
        return test_fail_errno("runtime create for stale-address guard failed");
    }
    llam_runtime_destroy(stale);

    for (unsigned i = 0U; i < 128U; ++i) {
        llam_runtime_t *runtime = NULL;

        if (llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime) != 0) {
            return test_fail_errno("runtime recreate for stale-address guard failed");
        }
        if (runtime == stale) {
            llam_runtime_destroy(runtime);
            return test_fail("destroyed runtime handle address was reused");
        }
        llam_runtime_destroy(runtime);
    }
    return 0;
}

int main(void) {
    if (test_sync_handle_family_confusion() != 0) {
        return 1;
    }
    if (test_runtime_run_handle_rejects_null() != 0) {
        return 1;
    }
    if (test_concurrent_spawn_join() != 0) {
        return 1;
    }
    if (test_sequential_runtime_host_join_owner_cleanup() != 0) {
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
    if (test_host_try_ops_ignore_default_runtime_race() != 0) {
        return 1;
    }
    if (test_default_channel_host_try_ops_ignore_default_runtime_reinit() != 0) {
        return 1;
    }
    if (test_concurrent_runtime_destroy_is_single_owner() != 0) {
        return 1;
    }
    if (test_runtime_destroy_waits_for_active_run() != 0) {
        return 1;
    }
    if (test_destroyed_runtime_handle_address_not_reused() != 0) {
        return 1;
    }
    printf("[test_multi_runtime_core] ok\n");
    return 0;
}
