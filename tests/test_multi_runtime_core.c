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
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if LLAM_PLATFORM_WINDOWS
#include <windows.h>
#endif

#define MULTI_RUNTIME_TASKS 64U

typedef struct multi_counter_state {
    atomic_uint ran_a;
    atomic_uint ran_b;
    atomic_uint failures;
} multi_counter_state_t;

typedef int (*multi_runtime_test_fn)(void);

typedef struct multi_runtime_named_test {
    const char *name;
    multi_runtime_test_fn fn;
} multi_runtime_named_test_t;

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
    llam_cond_t *foreign_cond;
    llam_cancel_token_t *foreign_token;
    llam_task_group_t *foreign_group;
    atomic_uint failures;
    int first_errno;
} cross_object_state_t;

typedef struct cross_spawn_token_state {
    llam_cancel_token_t *token;
    atomic_uint failures;
} cross_spawn_token_state_t;

typedef struct cross_allocator_free_state {
    llam_runtime_t *foreign_runtime;
    llam_cancel_token_t *local_token;
    atomic_uint failures;
    int first_errno;
    char first_case[64];
} cross_allocator_free_state_t;

typedef struct explicit_group_state {
    llam_task_group_t *group;
    atomic_uint failures;
} explicit_group_state_t;

typedef struct multi_io_state {
    llam_fd_t left_fd;
    llam_fd_t right_fd;
    unsigned char payload;
    atomic_uint echoed;
    atomic_uint failures;
} multi_io_state_t;

typedef struct explicit_owned_buffer_state {
    int fd;
    const char *payload;
    size_t payload_len;
    llam_io_buffer_t *buffer;
    atomic_uint produced;
    atomic_uint failures;
    int first_errno;
} explicit_owned_buffer_state_t;

typedef struct post_destroy_cleanup_state {
    llam_channel_t *channel;
    llam_mutex_t *mutex;
    llam_cond_t *cond;
    llam_cancel_token_t *token;
    llam_task_group_t *group;
    atomic_uint failures;
    int first_errno;
} post_destroy_cleanup_state_t;

typedef struct post_destroy_channel_drain_state {
    llam_channel_t *channel;
    int payload;
    atomic_uint failures;
    int first_errno;
} post_destroy_channel_drain_state_t;

typedef struct legacy_stop_wrapper_state {
    atomic_uint sleeper_entered;
    atomic_uint sleeper_errno;
    atomic_uint stopper_failed;
    int stopper_errno;
    bool use_shutdown_wrapper;
} legacy_stop_wrapper_state_t;

typedef struct managed_foreign_destroy_state {
    llam_runtime_t *foreign_runtime;
    atomic_uint target_entered;
    atomic_uint destroyer_done;
    atomic_uint target_done;
    atomic_uint failures;
    int first_errno;
} managed_foreign_destroy_state_t;

#if LLAM_PLATFORM_POSIX
#define HOST_TRY_RACE_ITERS 4000U

typedef struct io_destroy_isolation_state {
    int cancel_read_fd;
    int live_read_fd;
    unsigned char live_payload;
    atomic_uint cancel_wait_entered;
    atomic_uint cancel_observed;
    atomic_uint live_read_done;
    atomic_uint failures;
    int first_errno;
} io_destroy_isolation_state_t;

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

typedef struct runtime_run_start_destroy_race_state {
    llam_runtime_t *runtime;
    pthread_mutex_t lock;
    pthread_cond_t cv;
    unsigned ready;
    bool go;
    int run_rc;
    int run_errno;
} runtime_run_start_destroy_race_state_t;

typedef struct runtime_stats_destroy_race_state {
    llam_runtime_t *runtime;
    pthread_mutex_t lock;
    pthread_cond_t cv;
    unsigned ready;
    bool go;
    atomic_uint failures;
    int first_errno;
} runtime_stats_destroy_race_state_t;

typedef struct host_try_default_race_state {
    llam_runtime_t *runtime;
    llam_channel_t *channel;
    llam_runtime_opts_t opts;
    atomic_uint failures;
    atomic_uint done;
    int first_errno;
} host_try_default_race_state_t;
#endif

static int test_fail(const char *message);
static int test_fail_errno(const char *message);
static int run_named_test(const multi_runtime_named_test_t *test);
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

static int test_idle_block_workers_destroy_repeat(void) {
    llam_runtime_opts_t opts;

    if (init_runtime_opts(&opts) != 0) {
        return test_fail_errno("runtime opts init failed");
    }
    /*
     * Explicit runtimes start their blocking-worker pool during init, even when
     * no user task ever submits blocking work. Repeated destroy keeps the idle
     * worker shutdown wake path covered directly; a lost wake there otherwise
     * presents as a host-thread hang with no failed assertion.
     */
    for (unsigned i = 0U; i < 32U; ++i) {
        llam_runtime_t *runtime = NULL;

        if (llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime) != 0) {
            return test_fail_errno("runtime create for idle block-worker destroy failed");
        }
        llam_runtime_destroy(runtime);
    }
    return 0;
}

static int test_fail(const char *message) {
    fprintf(stderr, "[test_multi_runtime_core] %s\n", message);
    return 1;
}

static int test_fail_errno(const char *message) {
    fprintf(stderr, "[test_multi_runtime_core] %s: errno=%d (%s)\n", message, errno, strerror(errno));
    return 1;
}

static int run_named_test(const multi_runtime_named_test_t *test) {
    if (test == NULL || test->name == NULL || test->fn == NULL) {
        return test_fail("invalid test descriptor");
    }
    /*
     * Multi-runtime failures can present as platform-specific hangs. Emit a
     * small progress marker before and after each case so CI logs identify the
     * exact ownership or shutdown boundary that stopped making progress.
     */
    printf("[test_multi_runtime_core] begin %s\n", test->name);
    fflush(stdout);
    if (test->fn() != 0) {
        fprintf(stderr, "[test_multi_runtime_core] fail %s\n", test->name);
        return 1;
    }
    printf("[test_multi_runtime_core] ok %s\n", test->name);
    fflush(stdout);
    return 0;
}

#if LLAM_PLATFORM_POSIX
static int write_all_native(int fd, const void *data, size_t len) {
    const unsigned char *bytes = data;
    size_t off = 0U;

    while (off < len) {
        ssize_t written = write(fd, bytes + off, len - off);

        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (written == 0) {
            errno = EPIPE;
            return -1;
        }
        off += (size_t)written;
    }
    return 0;
}
#endif

static void owned_buffer_task_fail(explicit_owned_buffer_state_t *state, int err) {
    if (atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed) == 0U) {
        state->first_errno = err;
    }
}

static void post_destroy_cleanup_task_fail(post_destroy_cleanup_state_t *state, int err) {
    if (atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed) == 0U) {
        state->first_errno = err;
    }
}

static void post_destroy_cleanup_task(void *arg) {
    post_destroy_cleanup_state_t *state = arg;

    state->channel = llam_channel_create(1U);
    state->mutex = llam_mutex_create();
    state->cond = llam_cond_create();
    state->token = llam_cancel_token_create();
    state->group = llam_task_group_create();
    if (state->channel == NULL ||
        state->mutex == NULL ||
        state->cond == NULL ||
        state->token == NULL ||
        state->group == NULL) {
        post_destroy_cleanup_task_fail(state, errno);
    }
}

static void post_destroy_channel_drain_task(void *arg) {
    post_destroy_channel_drain_state_t *state = arg;

    state->channel = llam_channel_create(1U);
    if (state->channel == NULL) {
        state->first_errno = errno;
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
        return;
    }
    if (llam_channel_try_send(state->channel, &state->payload) != 0) {
        state->first_errno = errno;
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
    }
}

static void legacy_stop_sleeper_task(void *arg) {
    legacy_stop_wrapper_state_t *state = arg;

    atomic_store_explicit(&state->sleeper_entered, 1U, memory_order_release);
    if (llam_sleep_ns(100000000ULL) != 0) {
        atomic_store_explicit(&state->sleeper_errno, (unsigned)errno, memory_order_release);
    } else {
        atomic_store_explicit(&state->sleeper_errno, 0U, memory_order_release);
    }
}

static void legacy_stop_requester_task(void *arg) {
    legacy_stop_wrapper_state_t *state = arg;

    while (atomic_load_explicit(&state->sleeper_entered, memory_order_acquire) == 0U) {
        llam_yield();
    }
    /*
     * Let the sibling task enter the timer wait before the stop request.  This
     * keeps the regression focused on which runtime the legacy wrapper targets,
     * not on a race between stop and the sibling's first park.
     */
    (void)llam_sleep_ns(1000000ULL);
    if (state->use_shutdown_wrapper) {
        llam_runtime_shutdown();
        return;
    }
    if (llam_runtime_request_stop() != 0) {
        state->stopper_errno = errno;
        atomic_store_explicit(&state->stopper_failed, 1U, memory_order_release);
    }
}

static void managed_foreign_destroy_target_task(void *arg) {
    managed_foreign_destroy_state_t *state = arg;

    atomic_store_explicit(&state->target_entered, 1U, memory_order_release);
    if (llam_sleep_ns(50000000ULL) != 0) {
        state->first_errno = errno;
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
    }
    atomic_store_explicit(&state->target_done, 1U, memory_order_release);
}

static void managed_foreign_destroy_requester_task(void *arg) {
    managed_foreign_destroy_state_t *state = arg;

    while (atomic_load_explicit(&state->target_entered, memory_order_acquire) == 0U) {
        llam_yield();
    }
    /*
     * A managed task may request teardown for its own runtime by calling the
     * legacy shutdown/destroy wrappers, but a foreign runtime handle must not
     * become a cross-runtime stop signal.  This keeps scheduler isolation from
     * depending on user code never sharing runtime handles between tasks.
     */
    (void)llam_sleep_ns(1000000ULL);
    llam_runtime_destroy(state->foreign_runtime);
    atomic_store_explicit(&state->destroyer_done, 1U, memory_order_release);
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

static void cross_spawn_token_creator_task(void *arg) {
    cross_spawn_token_state_t *state = arg;

    state->token = llam_cancel_token_create();
    if (state->token == NULL) {
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
    }
}

static void explicit_group_creator_task(void *arg) {
    explicit_group_state_t *state = arg;

    state->group = llam_task_group_create();
    if (state->group == NULL) {
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
    }
}

static void cross_allocator_free_task(void *arg) {
    cross_allocator_free_state_t *state = arg;
    llam_shard_t *foreign;
    llam_wait_node_t *wait_node;
    llam_timer_node_t *timer_node;
    llam_io_req_t *io_req;
    llam_io_buffer_t *io_buffer;
    llam_spawn_opts_t spawn_opts;
    llam_task_t *task;

    if (state == NULL) {
        return;
    }
    if (state->foreign_runtime == NULL || state->foreign_runtime->active_shards == 0U) {
        state->first_errno = EINVAL;
        (void)snprintf(state->first_case, sizeof(state->first_case), "foreign runtime unavailable");
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
        return;
    }

    /*
     * This task runs on runtime A but allocates objects from runtime B.  Freeing
     * them must use B's remote-free queues, not B's owner-local free lists, even
     * when both runtimes use shard id 0.
     */
    foreign = &state->foreign_runtime->shards[0];

    wait_node = llam_wait_node_alloc(foreign);
    if (wait_node == NULL) {
        state->first_errno = errno;
        (void)snprintf(state->first_case, sizeof(state->first_case), "wait node alloc");
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
        return;
    }
    llam_wait_node_free(g_llam_tls_shard, wait_node);

    timer_node = llam_timer_node_alloc(foreign);
    if (timer_node == NULL) {
        state->first_errno = errno;
        (void)snprintf(state->first_case, sizeof(state->first_case), "timer node alloc");
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
        return;
    }
    llam_timer_node_free(g_llam_tls_shard, timer_node);

    io_req = llam_io_req_alloc(foreign);
    if (io_req == NULL) {
        state->first_errno = errno;
        (void)snprintf(state->first_case, sizeof(state->first_case), "io req alloc");
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
        return;
    }
    llam_io_req_free(g_llam_tls_shard, io_req);

    io_buffer = llam_io_buffer_allocator_alloc(foreign, 1U);
    if (io_buffer == NULL) {
        state->first_errno = errno;
        (void)snprintf(state->first_case, sizeof(state->first_case), "io buffer alloc");
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
        return;
    }
    llam_io_buffer_allocator_free(io_buffer);

    state->local_token = llam_cancel_token_create();
    if (state->local_token == NULL ||
        llam_spawn_opts_init(&spawn_opts, LLAM_SPAWN_OPTS_CURRENT_SIZE) != 0) {
        state->first_errno = errno;
        (void)snprintf(state->first_case, sizeof(state->first_case), "local token/spawn opts");
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
        return;
    }

    spawn_opts.cancel_token = state->local_token;
    errno = 0;
    task = llam_runtime_spawn_ex(state->foreign_runtime,
                                 foreign_target_task,
                                 &state->failures,
                                 &spawn_opts,
                                 LLAM_SPAWN_OPTS_CURRENT_SIZE);
    if (task != NULL || errno != EXDEV) {
        if (task != NULL) {
            (void)llam_detach(task);
        }
        state->first_errno = errno;
        (void)snprintf(state->first_case, sizeof(state->first_case), "foreign spawn owner check");
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
    }
}

static void cross_owner_task(void *arg) {
    cross_owner_state_t *state = arg;

    /*
     * Introspection has no errno channel, so cross-runtime task handles must
     * fail closed to their neutral values instead of exposing another runtime's
     * task id, state, class, or flags to a managed task.
     */
    if (llam_task_id(state->foreign_task) != 0U ||
        strcmp(llam_task_state_name(state->foreign_task), "UNKNOWN") != 0 ||
        llam_task_class(state->foreign_task) != (uint32_t)LLAM_TASK_CLASS_DEFAULT ||
        llam_task_flags(state->foreign_task) != 0U) {
        state->first_errno = EXDEV;
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
    }

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
    state->foreign_cond = llam_cond_create();
    state->foreign_token = llam_cancel_token_create();
    state->foreign_group = llam_task_group_create();
    if (state->foreign_channel == NULL ||
        state->foreign_mutex == NULL ||
        state->foreign_cond == NULL ||
        state->foreign_token == NULL ||
        state->foreign_group == NULL) {
        state->first_errno = errno;
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
    }
}

static void cross_object_fail(cross_object_state_t *state) {
    if (atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed) == 0U) {
        state->first_errno = errno;
    }
}

static void cross_object_task(void *arg) {
    cross_object_state_t *state = arg;
    llam_mutex_t *local_mutex = NULL;
    llam_cond_t *local_cond = NULL;
    llam_select_op_t op;
    int payload = 1;
    void *recv_out = NULL;
    size_t selected = SIZE_MAX;

    errno = 0;
    if (llam_channel_try_send(state->foreign_channel, &payload) != -1 || errno != EXDEV) {
        cross_object_fail(state);
    }
    errno = 0;
    if (llam_mutex_trylock(state->foreign_mutex) != -1 || errno != EXDEV) {
        cross_object_fail(state);
    }
    errno = 0;
    if (llam_cond_signal(state->foreign_cond) != -1 || errno != EXDEV) {
        cross_object_fail(state);
    }
    errno = 0;
    if (llam_cancel_token_cancel(state->foreign_token) != -1 || errno != EXDEV) {
        cross_object_fail(state);
    }
    errno = 0;
    if (llam_task_group_cancel(state->foreign_group) != -1 || errno != EXDEV) {
        cross_object_fail(state);
    }

    memset(&op, 0, sizeof(op));
    op.kind = LLAM_SELECT_OP_RECV;
    op.channel = state->foreign_channel;
    op.recv_out = &recv_out;
    errno = 0;
    if (llam_channel_select(&op, 1U, 0U, &selected) != -1 || errno != EXDEV) {
        cross_object_fail(state);
    }

    local_mutex = llam_mutex_create();
    local_cond = llam_cond_create();
    if (local_mutex == NULL || local_cond == NULL) {
        cross_object_fail(state);
        goto cleanup;
    }
    if (llam_mutex_lock(local_mutex) != 0) {
        cross_object_fail(state);
        goto cleanup;
    }
    errno = 0;
    if (llam_cond_wait_until(state->foreign_cond, local_mutex, 0U) != -1 || errno != EXDEV) {
        cross_object_fail(state);
    }
    if (llam_mutex_unlock(local_mutex) != 0) {
        cross_object_fail(state);
    }
    errno = 0;
    if (llam_cond_wait_until(local_cond, state->foreign_mutex, 0U) != -1 || errno != EXDEV) {
        cross_object_fail(state);
    }

cleanup:
    if (local_cond != NULL && llam_cond_destroy(local_cond) != 0) {
        cross_object_fail(state);
    }
    if (local_mutex != NULL && llam_mutex_destroy(local_mutex) != 0) {
        cross_object_fail(state);
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

static void explicit_owned_buffer_reader_task(void *arg) {
    explicit_owned_buffer_state_t *state = arg;
    ssize_t bytes;

    bytes = llam_read_owned(state->fd, state->payload_len + 8U, &state->buffer);
    if (bytes != (ssize_t)state->payload_len ||
        state->buffer == NULL ||
        llam_io_buffer_size(state->buffer) != state->payload_len ||
        memcmp(llam_io_buffer_data(state->buffer), state->payload, state->payload_len) != 0) {
        if (state->buffer != NULL) {
            llam_io_buffer_release(state->buffer);
            state->buffer = NULL;
        }
        owned_buffer_task_fail(state, errno != 0 ? errno : EPROTO);
        return;
    }
    atomic_fetch_add_explicit(&state->produced, 1U, memory_order_relaxed);
}

#if LLAM_PLATFORM_POSIX
static void io_destroy_cancel_read_task(void *arg) {
    io_destroy_isolation_state_t *state = arg;
    unsigned char byte = 0U;

    atomic_store_explicit(&state->cancel_wait_entered, 1U, memory_order_release);
    errno = 0;
    if (llam_read(state->cancel_read_fd, &byte, 1U) != -1 || errno != ECANCELED) {
        if (atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed) == 0U) {
            state->first_errno = errno != 0 ? errno : EPROTO;
        }
        return;
    }
    atomic_fetch_add_explicit(&state->cancel_observed, 1U, memory_order_relaxed);
}

static void io_destroy_live_read_task(void *arg) {
    io_destroy_isolation_state_t *state = arg;
    unsigned char byte = 0U;

    if (llam_read(state->live_read_fd, &byte, 1U) != 1 || byte != state->live_payload) {
        if (atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed) == 0U) {
            state->first_errno = errno != 0 ? errno : EPROTO;
        }
        return;
    }
    atomic_fetch_add_explicit(&state->live_read_done, 1U, memory_order_relaxed);
}

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
#elif LLAM_PLATFORM_WINDOWS
static DWORD WINAPI run_runtime_thread(LPVOID arg) {
    runtime_runner_t *runner = arg;

    errno = 0;
    runner->rc = llam_runtime_run_handle(runner->runtime);
    runner->err = errno;
    return 0;
}
#endif

#if LLAM_PLATFORM_POSIX
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

static void runtime_run_start_race_wait(runtime_run_start_destroy_race_state_t *state) {
    pthread_mutex_lock(&state->lock);
    state->ready += 1U;
    pthread_cond_broadcast(&state->cv);
    while (!state->go) {
        pthread_cond_wait(&state->cv, &state->lock);
    }
    pthread_mutex_unlock(&state->lock);
}

static void *run_start_race_thread(void *arg) {
    runtime_run_start_destroy_race_state_t *state = arg;

    runtime_run_start_race_wait(state);
    errno = 0;
    state->run_rc = llam_runtime_run_handle(state->runtime);
    state->run_errno = errno;
    return NULL;
}

static void *destroy_start_race_thread(void *arg) {
    runtime_run_start_destroy_race_state_t *state = arg;

    runtime_run_start_race_wait(state);
    llam_runtime_destroy(state->runtime);
    return NULL;
}

static void runtime_stats_destroy_race_wait(runtime_stats_destroy_race_state_t *state) {
    pthread_mutex_lock(&state->lock);
    state->ready += 1U;
    pthread_cond_broadcast(&state->cv);
    while (!state->go) {
        pthread_cond_wait(&state->cv, &state->lock);
    }
    pthread_mutex_unlock(&state->lock);
}

static void *stats_destroy_race_thread(void *arg) {
    runtime_stats_destroy_race_state_t *state = arg;
    llam_runtime_stats_t stats;

    runtime_stats_destroy_race_wait(state);
    for (unsigned i = 0U; i < 4096U; ++i) {
        errno = 0;
        if (llam_runtime_collect_stats_ex_handle(state->runtime, &stats, sizeof(stats)) != 0 &&
            errno != EINVAL) {
            state->first_errno = errno;
            atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
            break;
        }
    }
    return NULL;
}

static void *destroy_stats_runtime_thread(void *arg) {
    runtime_stats_destroy_race_state_t *state = arg;

    runtime_stats_destroy_race_wait(state);
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

static void *explicit_channel_destroy_try_thread(void *arg) {
    host_try_default_race_state_t *state = arg;
    int payload = 29;
    bool pending = false;

    while (atomic_load_explicit(&state->done, memory_order_acquire) == 0U &&
           atomic_load_explicit(&state->failures, memory_order_acquire) == 0U) {
        void *out = NULL;

        if (!pending) {
            if (llam_channel_try_send(state->channel, &payload) == 0) {
                pending = true;
            } else if (errno != EAGAIN && errno != ENOTSUP && errno != EXDEV) {
                state->first_errno = errno;
                atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
                break;
            }
        }
        if (pending) {
            if (llam_channel_try_recv_result(state->channel, &out) == 0) {
                if (out != &payload) {
                    state->first_errno = EPROTO;
                    atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
                    break;
                }
                pending = false;
            } else if (errno != EAGAIN && errno != ENOTSUP) {
                state->first_errno = errno;
                atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
                break;
            }
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
#elif LLAM_PLATFORM_WINDOWS
    runtime_runner_t runner_a;
    runtime_runner_t runner_b;
    HANDLE thread_a;
    HANDLE thread_b;

    memset(&runner_a, 0, sizeof(runner_a));
    memset(&runner_b, 0, sizeof(runner_b));
    runner_a.runtime = runtime_a;
    runner_b.runtime = runtime_b;

    /*
     * Cross-runtime isolation tests require both runtimes to make scheduler
     * progress at the same time. Sequentially driving runtime A before runtime
     * B can deadlock tests where A waits for a task that only B can run.
     */
    thread_a = CreateThread(NULL, 0U, run_runtime_thread, &runner_a, 0U, NULL);
    if (thread_a == NULL) {
        errno = EAGAIN;
        return -1;
    }
    thread_b = CreateThread(NULL, 0U, run_runtime_thread, &runner_b, 0U, NULL);
    if (thread_b == NULL) {
        (void)llam_runtime_request_stop_rt(runtime_a);
        (void)WaitForSingleObject(thread_a, INFINITE);
        (void)CloseHandle(thread_a);
        errno = EAGAIN;
        return -1;
    }
    (void)WaitForSingleObject(thread_a, INFINITE);
    (void)WaitForSingleObject(thread_b, INFINITE);
    (void)CloseHandle(thread_a);
    (void)CloseHandle(thread_b);
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
        state.foreign_mutex == NULL ||
        state.foreign_cond == NULL ||
        state.foreign_token == NULL ||
        state.foreign_group == NULL) {
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
    if (state.foreign_group != NULL) {
        (void)llam_task_group_destroy(state.foreign_group);
    }
    if (state.foreign_token != NULL) {
        (void)llam_cancel_token_destroy(state.foreign_token);
    }
    if (state.foreign_cond != NULL) {
        (void)llam_cond_destroy(state.foreign_cond);
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

static int test_cross_runtime_spawn_cancel_token_owner(void) {
    llam_runtime_opts_t opts;
    llam_runtime_t *runtime_a = NULL;
    llam_runtime_t *runtime_b = NULL;
    llam_task_t *creator = NULL;
    llam_task_t *wrong_task = NULL;
    cross_spawn_token_state_t state;
    llam_spawn_opts_t spawn_opts;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);

    if (init_runtime_opts(&opts) != 0 ||
        llam_spawn_opts_init(&spawn_opts, LLAM_SPAWN_OPTS_CURRENT_SIZE) != 0) {
        return test_fail_errno("cross-token spawn opts init failed");
    }
    if (llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime_a) != 0 ||
        llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime_b) != 0) {
        rc = test_fail_errno("runtime create for cross-token spawn failed");
        goto cleanup;
    }

    creator = llam_runtime_spawn_ex(runtime_a, cross_spawn_token_creator_task, &state, NULL, 0U);
    if (creator == NULL ||
        llam_runtime_run_handle(runtime_a) != 0 ||
        llam_join(creator) != 0 ||
        state.token == NULL ||
        atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
        creator = NULL;
        rc = test_fail_errno("cross-token fixture setup failed");
        goto cleanup;
    }
    creator = NULL;

    /*
     * A task's cancellation token participates in wait and I/O ownership.
     * Spawning a runtime-B task with a runtime-A token must fail before the
     * task is published; otherwise cancellation can later cross owner domains.
     */
    spawn_opts.cancel_token = state.token;
    errno = 0;
    wrong_task = llam_runtime_spawn_ex(runtime_b,
                                       foreign_target_task,
                                       &state.failures,
                                       &spawn_opts,
                                       LLAM_SPAWN_OPTS_CURRENT_SIZE);
    if (wrong_task != NULL || errno != EXDEV) {
        rc = test_fail_errno("cross-runtime spawn accepted foreign cancel token");
        goto cleanup;
    }
    rc = 0;

cleanup:
    if (creator != NULL) {
        (void)llam_detach(creator);
    }
    if (wrong_task != NULL) {
        (void)llam_detach(wrong_task);
    }
    /*
     * A vulnerable runtime may have retained state.token on wrong_task. Destroy
     * runtime B before destroying the runtime-A token so task reclamation can
     * drop the accidental reference.
     */
    llam_runtime_destroy(runtime_b);
    if (state.token != NULL) {
        (void)llam_cancel_token_destroy(state.token);
    }
    llam_runtime_destroy(runtime_a);
    return rc;
}

static int test_cross_runtime_allocator_returns_are_remote(void) {
    llam_runtime_opts_t opts;
    llam_runtime_t *runtime_a = NULL;
    llam_runtime_t *runtime_b = NULL;
    llam_task_t *task = NULL;
    cross_allocator_free_state_t state;
    llam_allocator_t *allocator;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);

    if (init_runtime_opts(&opts) != 0) {
        return test_fail_errno("cross-allocator opts init failed");
    }
    if (llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime_a) != 0 ||
        llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime_b) != 0) {
        rc = test_fail_errno("runtime create for cross-allocator test failed");
        goto cleanup;
    }

    state.foreign_runtime = runtime_b;
    task = llam_runtime_spawn_ex(runtime_a, cross_allocator_free_task, &state, NULL, 0U);
    if (task == NULL ||
        llam_runtime_run_handle(runtime_a) != 0 ||
        llam_join(task) != 0 ||
        atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
        task = NULL;
        if (state.first_case[0] != '\0') {
            fprintf(stderr,
                    "[test_multi_runtime_core] cross-allocator task failed at %s: errno=%d (%s)\n",
                    state.first_case,
                    state.first_errno,
                    strerror(state.first_errno));
        }
        rc = test_fail_errno("cross-allocator fixture failed");
        goto cleanup;
    }
    task = NULL;

    allocator = &runtime_b->shards[0].allocator;
    errno = 0;
    {
        bool task_returned_to_external_cache;

        llam_allocator_lock(allocator);
        task_returned_to_external_cache = allocator->task_external_free != NULL;
        llam_allocator_unlock(allocator);
        if (!task_returned_to_external_cache &&
            atomic_load_explicit(&allocator->task_remote_free, memory_order_acquire) == NULL) {
            rc = test_fail_errno("foreign runtime task object bypassed safe non-local return path");
            goto cleanup;
        }
    }
    if (atomic_load_explicit(&allocator->wait_remote_free, memory_order_acquire) == NULL) {
        rc = test_fail_errno("foreign runtime wait node bypassed remote-free queue");
        goto cleanup;
    }
    if (atomic_load_explicit(&allocator->timer_remote_free, memory_order_acquire) == NULL) {
        rc = test_fail_errno("foreign runtime timer node bypassed remote-free queue");
        goto cleanup;
    }
    if (atomic_load_explicit(&allocator->io_req_remote_free, memory_order_acquire) == NULL) {
        rc = test_fail_errno("foreign runtime io request bypassed remote-free queue");
        goto cleanup;
    }
    if (atomic_load_explicit(&allocator->io_buffer_remote_free, memory_order_acquire) == NULL) {
        rc = test_fail_errno("foreign runtime objects bypassed remote-free queues");
        goto cleanup;
    }

    rc = 0;

cleanup:
    if (task != NULL) {
        (void)llam_detach(task);
    }
    if (state.local_token != NULL) {
        (void)llam_cancel_token_destroy(state.local_token);
    }
    llam_runtime_destroy(runtime_b);
    llam_runtime_destroy(runtime_a);
    return rc;
}

static int test_task_group_host_spawn_uses_group_runtime(void) {
    llam_runtime_opts_t opts;
    llam_runtime_t *runtime = NULL;
    llam_task_t *creator = NULL;
    llam_task_t *borrowed_child = NULL;
    explicit_group_state_t state;
    atomic_uint child_ran;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&child_ran, 0U);

    if (init_runtime_opts(&opts) != 0) {
        return test_fail_errno("explicit group opts init failed");
    }
    if (llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime) != 0) {
        rc = test_fail_errno("runtime create for explicit group failed");
        goto cleanup;
    }

    creator = llam_runtime_spawn_ex(runtime, explicit_group_creator_task, &state, NULL, 0U);
    if (creator == NULL ||
        llam_runtime_run_handle(runtime) != 0 ||
        llam_join(creator) != 0 ||
        state.group == NULL ||
        atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
        creator = NULL;
        rc = test_fail_errno("explicit group fixture setup failed");
        goto cleanup;
    }
    creator = NULL;

    /*
     * Host-thread group spawns must target the group's owner runtime.  Using
     * the legacy current/default runtime here either fails spuriously or
     * publishes children into the wrong owner domain.
     */
    borrowed_child = llam_task_group_spawn_ex(state.group, foreign_target_task, &child_ran, NULL, 0U);
    if (borrowed_child == NULL) {
        rc = test_fail_errno("explicit group host spawn failed");
        goto cleanup;
    }
    if (llam_runtime_run_handle(runtime) != 0 ||
        llam_task_group_join(state.group) != 0 ||
        atomic_load_explicit(&child_ran, memory_order_relaxed) != 1U) {
        rc = test_fail_errno("explicit group child did not run on owner runtime");
        goto cleanup;
    }
    borrowed_child = NULL;
    rc = 0;

cleanup:
    if (creator != NULL) {
        (void)llam_detach(creator);
    }
    if (rc != 0 && borrowed_child != NULL) {
        (void)llam_task_group_cancel(state.group);
        (void)llam_runtime_run_handle(runtime);
        (void)llam_task_group_join(state.group);
    }
    if (state.group != NULL) {
        (void)llam_task_group_destroy(state.group);
    }
    llam_runtime_destroy(runtime);
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

static int test_explicit_owned_buffers_survive_runtime_destroy(void) {
#if LLAM_PLATFORM_POSIX
    llam_runtime_opts_t opts;
    llam_runtime_t *runtime_a = NULL;
    llam_runtime_t *runtime_b = NULL;
    llam_task_t *task_a = NULL;
    llam_task_t *task_b = NULL;
    explicit_owned_buffer_state_t state_a;
    explicit_owned_buffer_state_t state_b;
    int pipe_a[2] = {-1, -1};
    int pipe_b[2] = {-1, -1};
    int rc = 1;

    memset(&state_a, 0, sizeof(state_a));
    memset(&state_b, 0, sizeof(state_b));
    state_a.fd = -1;
    state_b.fd = -1;
    state_a.payload = "owned-a";
    state_b.payload = "owned-b";
    state_a.payload_len = strlen(state_a.payload);
    state_b.payload_len = strlen(state_b.payload);
    atomic_init(&state_a.produced, 0U);
    atomic_init(&state_b.produced, 0U);
    atomic_init(&state_a.failures, 0U);
    atomic_init(&state_b.failures, 0U);

    if (init_runtime_opts(&opts) != 0) {
        return test_fail_errno("runtime opts init failed");
    }
    if (pipe(pipe_a) != 0 || pipe(pipe_b) != 0) {
        rc = test_fail_errno("owned-buffer explicit runtime pipe setup failed");
        goto cleanup;
    }
    if (write_all_native(pipe_a[1], state_a.payload, state_a.payload_len) != 0 ||
        write_all_native(pipe_b[1], state_b.payload, state_b.payload_len) != 0) {
        rc = test_fail_errno("owned-buffer explicit runtime pipe write failed");
        goto cleanup;
    }
    close(pipe_a[1]);
    pipe_a[1] = -1;
    close(pipe_b[1]);
    pipe_b[1] = -1;
    state_a.fd = pipe_a[0];
    state_b.fd = pipe_b[0];

    if (llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime_a) != 0 ||
        llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime_b) != 0) {
        rc = test_fail_errno("owned-buffer explicit runtime create failed");
        goto cleanup;
    }
    task_a = llam_runtime_spawn_ex(runtime_a, explicit_owned_buffer_reader_task, &state_a, NULL, 0U);
    task_b = llam_runtime_spawn_ex(runtime_b, explicit_owned_buffer_reader_task, &state_b, NULL, 0U);
    if (task_a == NULL || task_b == NULL) {
        rc = test_fail_errno("owned-buffer explicit runtime spawn failed");
        goto cleanup;
    }
    if (run_two_runtimes(runtime_a, runtime_b) != 0 ||
        llam_join(task_a) != 0 ||
        llam_join(task_b) != 0) {
        rc = test_fail_errno("owned-buffer explicit runtime run/join failed");
        goto cleanup;
    }
    task_a = NULL;
    task_b = NULL;
    if (atomic_load_explicit(&state_a.produced, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state_b.produced, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state_a.failures, memory_order_relaxed) != 0U ||
        atomic_load_explicit(&state_b.failures, memory_order_relaxed) != 0U) {
        errno = state_a.first_errno != 0 ? state_a.first_errno : state_b.first_errno;
        rc = test_fail_errno("owned-buffer explicit runtime task failed");
        goto cleanup;
    }

    /*
     * Owned buffers are public caller-owned handles after a successful read.
     * Releasing them after their producing explicit runtimes are destroyed must
     * not recycle through a freed runtime node or consume a sibling runtime's
     * buffer slot.
     */
    llam_runtime_destroy(runtime_b);
    runtime_b = NULL;
    llam_runtime_destroy(runtime_a);
    runtime_a = NULL;
    if (llam_io_buffer_size(state_a.buffer) != state_a.payload_len ||
        llam_io_buffer_size(state_b.buffer) != state_b.payload_len ||
        memcmp(llam_io_buffer_data(state_a.buffer), state_a.payload, state_a.payload_len) != 0 ||
        memcmp(llam_io_buffer_data(state_b.buffer), state_b.payload, state_b.payload_len) != 0) {
        rc = test_fail("explicit runtime owned buffer changed after destroy");
        goto cleanup;
    }
    llam_io_buffer_release(state_a.buffer);
    state_a.buffer = NULL;
    llam_io_buffer_release(state_b.buffer);
    state_b.buffer = NULL;
    rc = 0;

cleanup:
    if (task_a != NULL) {
        (void)llam_detach(task_a);
    }
    if (task_b != NULL) {
        (void)llam_detach(task_b);
    }
    if (state_a.buffer != NULL) {
        llam_io_buffer_release(state_a.buffer);
    }
    if (state_b.buffer != NULL) {
        llam_io_buffer_release(state_b.buffer);
    }
    if (pipe_a[0] >= 0) {
        close(pipe_a[0]);
    }
    if (pipe_a[1] >= 0) {
        close(pipe_a[1]);
    }
    if (pipe_b[0] >= 0) {
        close(pipe_b[0]);
    }
    if (pipe_b[1] >= 0) {
        close(pipe_b[1]);
    }
    llam_runtime_destroy(runtime_b);
    llam_runtime_destroy(runtime_a);
    return rc;
#else
    return 0;
#endif
}

static int test_destroyed_runtime_io_cancel_does_not_stop_peer_runtime(void) {
#if LLAM_PLATFORM_POSIX
    llam_runtime_opts_t opts;
    llam_runtime_t *runtime_a = NULL;
    llam_runtime_t *runtime_b = NULL;
    llam_task_t *task_a = NULL;
    llam_task_t *task_b = NULL;
    runtime_runner_t runner_a;
    runtime_runner_t runner_b;
    pthread_t thread_a;
    pthread_t thread_b;
    io_destroy_isolation_state_t state;
    int cancel_sv[2] = {-1, -1};
    int live_sv[2] = {-1, -1};
    int rc = 1;
    int err;

    memset(&runner_a, 0, sizeof(runner_a));
    memset(&runner_b, 0, sizeof(runner_b));
    memset(&state, 0, sizeof(state));
    state.cancel_read_fd = -1;
    state.live_read_fd = -1;
    state.live_payload = 0x7cU;
    atomic_init(&state.cancel_wait_entered, 0U);
    atomic_init(&state.cancel_observed, 0U);
    atomic_init(&state.live_read_done, 0U);
    atomic_init(&state.failures, 0U);

    if (init_runtime_opts(&opts) != 0) {
        return test_fail_errno("runtime opts init failed");
    }
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, cancel_sv) != 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, live_sv) != 0) {
        rc = test_fail_errno("runtime destroy I/O isolation socketpair setup failed");
        goto cleanup;
    }
    state.cancel_read_fd = cancel_sv[0];
    state.live_read_fd = live_sv[0];

    if (llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime_a) != 0 ||
        llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime_b) != 0) {
        rc = test_fail_errno("runtime destroy I/O isolation runtime create failed");
        goto cleanup;
    }
    task_a = llam_runtime_spawn_ex(runtime_a, io_destroy_cancel_read_task, &state, NULL, 0U);
    task_b = llam_runtime_spawn_ex(runtime_b, io_destroy_live_read_task, &state, NULL, 0U);
    if (task_a == NULL || task_b == NULL) {
        rc = test_fail_errno("runtime destroy I/O isolation spawn failed");
        goto cleanup;
    }
    runner_a.runtime = runtime_a;
    runner_b.runtime = runtime_b;
    err = pthread_create(&thread_a, NULL, run_runtime_thread, &runner_a);
    if (err != 0) {
        errno = err;
        rc = test_fail_errno("runtime destroy I/O isolation thread A failed");
        goto cleanup;
    }
    err = pthread_create(&thread_b, NULL, run_runtime_thread, &runner_b);
    if (err != 0) {
        errno = err;
        (void)llam_runtime_request_stop_rt(runtime_a);
        pthread_join(thread_a, NULL);
        rc = test_fail_errno("runtime destroy I/O isolation thread B failed");
        goto cleanup;
    }

    while (atomic_load_explicit(&state.cancel_wait_entered, memory_order_acquire) == 0U) {
        llam_pause_cpu();
    }
    llam_runtime_destroy(runtime_a);
    runtime_a = NULL;
    task_a = NULL;
    pthread_join(thread_a, NULL);
    if (runner_a.rc != 0 ||
        atomic_load_explicit(&state.cancel_observed, memory_order_relaxed) != 1U) {
        errno = runner_a.err != 0 ? runner_a.err : state.first_errno;
        rc = test_fail_errno("runtime A destroy did not cancel only its pending read");
        (void)llam_runtime_request_stop_rt(runtime_b);
        pthread_join(thread_b, NULL);
        goto cleanup;
    }

    if (write_all_native(live_sv[1], &state.live_payload, 1U) != 0) {
        rc = test_fail_errno("runtime destroy I/O isolation live write failed");
        (void)llam_runtime_request_stop_rt(runtime_b);
        pthread_join(thread_b, NULL);
        goto cleanup;
    }
    pthread_join(thread_b, NULL);
    if (runner_b.rc != 0 || llam_join(task_b) != 0) {
        errno = runner_b.err;
        rc = test_fail_errno("runtime B did not survive peer runtime I/O destroy");
        goto cleanup;
    }
    task_b = NULL;
    if (atomic_load_explicit(&state.live_read_done, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
        errno = state.first_errno;
        rc = test_fail_errno("runtime destroy I/O isolation counters failed");
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
    if (cancel_sv[0] >= 0) {
        close(cancel_sv[0]);
    }
    if (cancel_sv[1] >= 0) {
        close(cancel_sv[1]);
    }
    if (live_sv[0] >= 0) {
        close(live_sv[0]);
    }
    if (live_sv[1] >= 0) {
        close(live_sv[1]);
    }
    llam_runtime_destroy(runtime_b);
    llam_runtime_destroy(runtime_a);
    return rc;
#else
    return 0;
#endif
}

static int test_process_signal_handler_survives_peer_runtime_destroy(void) {
#if LLAM_PLATFORM_POSIX
    pid_t pid;
    int status = 0;

    pid = fork();
    if (pid < 0) {
        return test_fail_errno("signal handler isolation fork failed");
    }
    if (pid == 0) {
        llam_runtime_opts_t opts;
        llam_runtime_t *runtime_a = NULL;
        llam_runtime_t *runtime_b = NULL;
        struct sigaction default_action;

        memset(&default_action, 0, sizeof(default_action));
        sigemptyset(&default_action.sa_mask);
        default_action.sa_handler = SIG_DFL;
        if (sigaction(LLAM_PREEMPT_SIGNAL, &default_action, NULL) != 0) {
            _exit(9);
        }

        if (init_runtime_opts(&opts) != 0) {
            _exit(10);
        }
        if (llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime_a) != 0 ||
            llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime_b) != 0) {
            _exit(11);
        }

        llam_runtime_destroy(runtime_a);

        /*
         * Preemption uses a process-wide signal handler. Destroying one
         * runtime must not restore SIGUSR1 to the previous/default action
         * while another runtime can still receive watchdog preemption.
         */
        if (raise(LLAM_PREEMPT_SIGNAL) != 0) {
            _exit(12);
        }

        llam_runtime_destroy(runtime_b);
        _exit(0);
    }

    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return test_fail_errno("signal handler isolation waitpid failed");
        }
    }
    if (WIFSIGNALED(status)) {
        errno = 0;
        fprintf(stderr,
                "[test_multi_runtime_core] child died from signal %d after peer runtime destroy\n",
                WTERMSIG(status));
        return 1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        errno = WIFEXITED(status) ? WEXITSTATUS(status) : ECHILD;
        return test_fail_errno("signal handler isolation child failed");
    }
    return 0;
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

static int test_explicit_channel_host_try_races_runtime_destroy(void) {
#if LLAM_PLATFORM_POSIX
    host_try_default_race_state_t state;
    llam_task_t *creator = NULL;
    pthread_t try_thread;
    void *out;
    int rc = 1;
    int err;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.done, 0U);
    if (init_runtime_opts(&state.opts) != 0) {
        return test_fail_errno("runtime opts init failed");
    }
    if (llam_runtime_create(&state.opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &state.runtime) != 0) {
        return test_fail_errno("runtime create for explicit channel destroy race failed");
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

    err = pthread_create(&try_thread, NULL, explicit_channel_destroy_try_thread, &state);
    if (err != 0) {
        errno = err;
        rc = test_fail_errno("explicit channel destroy try thread create failed");
        goto cleanup;
    }
    usleep(10000);
    llam_runtime_destroy(state.runtime);
    state.runtime = NULL;
    atomic_store_explicit(&state.done, 1U, memory_order_release);
    pthread_join(try_thread, NULL);

    if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
        errno = state.first_errno;
        rc = test_fail_errno("explicit channel host try/runtime destroy race observed an invalid result");
        goto cleanup;
    }
    rc = 0;

cleanup:
    if (creator != NULL) {
        (void)llam_detach(creator);
    }
    if (state.channel != NULL) {
        while (llam_channel_try_recv_result(state.channel, &out) == 0) {
        }
        (void)llam_channel_destroy(state.channel);
    }
    llam_runtime_destroy(state.runtime);
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

static int test_runtime_run_start_destroy_race(void) {
#if LLAM_PLATFORM_POSIX
    enum { RUN_DESTROY_RACE_ITERS = 128 };
    llam_runtime_opts_t opts;

    if (init_runtime_opts(&opts) != 0) {
        return test_fail_errno("runtime opts init failed");
    }
    for (unsigned i = 0U; i < RUN_DESTROY_RACE_ITERS; ++i) {
        runtime_run_start_destroy_race_state_t state;
        pthread_t run_thread;
        pthread_t destroy_thread;
        int err;

        memset(&state, 0, sizeof(state));
        state.run_rc = -1;
        if (llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &state.runtime) != 0) {
            return test_fail_errno("runtime create for run/destroy start race failed");
        }
        err = pthread_mutex_init(&state.lock, NULL);
        if (err != 0) {
            errno = err;
            llam_runtime_destroy(state.runtime);
            return test_fail_errno("run/destroy race mutex init failed");
        }
        err = pthread_cond_init(&state.cv, NULL);
        if (err != 0) {
            errno = err;
            pthread_mutex_destroy(&state.lock);
            llam_runtime_destroy(state.runtime);
            return test_fail_errno("run/destroy race cond init failed");
        }
        err = pthread_create(&run_thread, NULL, run_start_race_thread, &state);
        if (err != 0) {
            errno = err;
            pthread_cond_destroy(&state.cv);
            pthread_mutex_destroy(&state.lock);
            llam_runtime_destroy(state.runtime);
            return test_fail_errno("run/destroy race run thread create failed");
        }
        err = pthread_create(&destroy_thread, NULL, destroy_start_race_thread, &state);
        if (err != 0) {
            errno = err;
            pthread_mutex_lock(&state.lock);
            state.go = true;
            pthread_cond_broadcast(&state.cv);
            pthread_mutex_unlock(&state.lock);
            pthread_join(run_thread, NULL);
            pthread_cond_destroy(&state.cv);
            pthread_mutex_destroy(&state.lock);
            llam_runtime_destroy(state.runtime);
            return test_fail_errno("run/destroy race destroy thread create failed");
        }

        pthread_mutex_lock(&state.lock);
        while (state.ready < 2U) {
            pthread_cond_wait(&state.cv, &state.lock);
        }
        state.go = true;
        pthread_cond_broadcast(&state.cv);
        pthread_mutex_unlock(&state.lock);

        pthread_join(run_thread, NULL);
        pthread_join(destroy_thread, NULL);
        pthread_cond_destroy(&state.cv);
        pthread_mutex_destroy(&state.lock);

        /*
         * Either host may claim the runtime first.  A run that starts before
         * destroy must drain cleanly; a run that loses to destroy must fail
         * closed with EINVAL, never touch torn-down runtime storage.
         */
        if (!(state.run_rc == 0 ||
              (state.run_rc == -1 && state.run_errno == EINVAL))) {
            errno = state.run_errno;
            return test_fail_errno("run/destroy start race returned unexpected run result");
        }
    }
    return 0;
#else
    return 0;
#endif
}

static int test_runtime_stats_destroy_race(void) {
#if LLAM_PLATFORM_POSIX
    enum { STATS_DESTROY_RACE_ITERS = 128 };
    llam_runtime_opts_t opts;

    if (init_runtime_opts(&opts) != 0) {
        return test_fail_errno("runtime opts init failed");
    }
    for (unsigned i = 0U; i < STATS_DESTROY_RACE_ITERS; ++i) {
        runtime_stats_destroy_race_state_t state;
        pthread_t stats_thread;
        pthread_t destroy_thread;
        int err;

        memset(&state, 0, sizeof(state));
        atomic_init(&state.failures, 0U);
        if (llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &state.runtime) != 0) {
            return test_fail_errno("runtime create for stats/destroy race failed");
        }
        err = pthread_mutex_init(&state.lock, NULL);
        if (err != 0) {
            errno = err;
            llam_runtime_destroy(state.runtime);
            return test_fail_errno("stats/destroy race mutex init failed");
        }
        err = pthread_cond_init(&state.cv, NULL);
        if (err != 0) {
            errno = err;
            pthread_mutex_destroy(&state.lock);
            llam_runtime_destroy(state.runtime);
            return test_fail_errno("stats/destroy race cond init failed");
        }
        err = pthread_create(&stats_thread, NULL, stats_destroy_race_thread, &state);
        if (err != 0) {
            errno = err;
            pthread_cond_destroy(&state.cv);
            pthread_mutex_destroy(&state.lock);
            llam_runtime_destroy(state.runtime);
            return test_fail_errno("stats/destroy race stats thread create failed");
        }
        err = pthread_create(&destroy_thread, NULL, destroy_stats_runtime_thread, &state);
        if (err != 0) {
            errno = err;
            pthread_mutex_lock(&state.lock);
            state.go = true;
            pthread_cond_broadcast(&state.cv);
            pthread_mutex_unlock(&state.lock);
            pthread_join(stats_thread, NULL);
            pthread_cond_destroy(&state.cv);
            pthread_mutex_destroy(&state.lock);
            llam_runtime_destroy(state.runtime);
            return test_fail_errno("stats/destroy race destroy thread create failed");
        }

        pthread_mutex_lock(&state.lock);
        while (state.ready < 2U) {
            pthread_cond_wait(&state.cv, &state.lock);
        }
        state.go = true;
        pthread_cond_broadcast(&state.cv);
        pthread_mutex_unlock(&state.lock);

        pthread_join(stats_thread, NULL);
        pthread_join(destroy_thread, NULL);
        pthread_cond_destroy(&state.cv);
        pthread_mutex_destroy(&state.lock);
        if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
            errno = state.first_errno;
            return test_fail_errno("stats/destroy race returned unexpected stats error");
        }
    }
    return 0;
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

static int test_public_cleanup_after_owner_runtime_destroy(void) {
    llam_runtime_opts_t opts;
    post_destroy_cleanup_state_t state;
    llam_runtime_t *runtime = NULL;
    llam_task_t *task = NULL;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    if (init_runtime_opts(&opts) != 0) {
        return test_fail_errno("runtime opts init failed");
    }
    if (llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime) != 0) {
        return test_fail_errno("runtime create for post-destroy cleanup failed");
    }
    task = llam_runtime_spawn_ex(runtime, post_destroy_cleanup_task, &state, NULL, 0U);
    if (task == NULL) {
        rc = test_fail_errno("post-destroy cleanup spawn failed");
        goto cleanup;
    }
    if (llam_runtime_run_handle(runtime) != 0 || llam_join(task) != 0) {
        rc = test_fail_errno("post-destroy cleanup task run/join failed");
        task = NULL;
        goto cleanup;
    }
    task = NULL;
    if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
        errno = state.first_errno;
        rc = test_fail_errno("post-destroy cleanup object creation failed");
        goto cleanup;
    }

    /*
     * Destroying the owner runtime invalidates scheduler/backend state, but
     * host-side cleanup handles for idle public objects must still be
     * consumable. Otherwise embedders can leak objects just by tearing down the
     * runtime before releasing final application references.
     */
    llam_runtime_destroy(runtime);
    runtime = NULL;

    /*
     * Stale owner-runtime pointers are accepted only by cleanup APIs below.
     * Non-cleanup operations would need a live scheduler to wake waiters or
     * propagate cancellation, so they must fail before touching retired runtime
     * storage.
     */
    if (llam_channel_close(state.channel) != 0) {
        rc = test_fail_errno("post-destroy idle channel close cleanup failed");
        goto cleanup;
    }
    if (llam_channel_try_send(state.channel, &state) != -1 || errno != EXDEV) {
        rc = test_fail_errno("post-destroy channel try_send did not fail with EXDEV");
        goto cleanup;
    }
    if (llam_cond_signal(state.cond) != -1 || errno != EXDEV) {
        rc = test_fail_errno("post-destroy cond signal did not fail with EXDEV");
        goto cleanup;
    }
    if (llam_cond_broadcast(state.cond) != -1 || errno != EXDEV) {
        rc = test_fail_errno("post-destroy cond broadcast did not fail with EXDEV");
        goto cleanup;
    }
    if (llam_cancel_token_cancel(state.token) != -1 || errno != EXDEV) {
        rc = test_fail_errno("post-destroy cancel token operation did not fail with EXDEV");
        goto cleanup;
    }
    if (llam_task_group_cancel(state.group) != -1 || errno != EXDEV) {
        rc = test_fail_errno("post-destroy task group cancel did not fail with EXDEV");
        goto cleanup;
    }

    if (llam_task_group_destroy(state.group) != 0 ||
        llam_cancel_token_destroy(state.token) != 0 ||
        llam_cond_destroy(state.cond) != 0 ||
        llam_mutex_destroy(state.mutex) != 0 ||
        llam_channel_destroy(state.channel) != 0) {
        rc = test_fail_errno("public cleanup after owner runtime destroy failed");
        goto cleanup;
    }
    state.group = NULL;
    state.token = NULL;
    state.cond = NULL;
    state.mutex = NULL;
    state.channel = NULL;
    rc = 0;

cleanup:
    if (task != NULL) {
        (void)llam_detach(task);
    }
    if (runtime != NULL) {
        llam_runtime_destroy(runtime);
    }
    if (state.group != NULL) {
        (void)llam_task_group_destroy(state.group);
    }
    if (state.token != NULL) {
        (void)llam_cancel_token_destroy(state.token);
    }
    if (state.cond != NULL) {
        (void)llam_cond_destroy(state.cond);
    }
    if (state.mutex != NULL) {
        (void)llam_mutex_destroy(state.mutex);
    }
    if (state.channel != NULL) {
        (void)llam_channel_destroy(state.channel);
    }
    return rc;
}

static int test_explicit_channel_drain_after_owner_runtime_destroy(void) {
    llam_runtime_opts_t opts;
    post_destroy_channel_drain_state_t state;
    llam_runtime_t *runtime = NULL;
    llam_task_t *task = NULL;
    void *out = NULL;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    state.payload = 0x51a7;
    atomic_init(&state.failures, 0U);
    if (init_runtime_opts(&opts) != 0) {
        return test_fail_errno("runtime opts init failed");
    }
    if (llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime) != 0) {
        return test_fail_errno("runtime create for post-destroy channel drain failed");
    }
    task = llam_runtime_spawn_ex(runtime, post_destroy_channel_drain_task, &state, NULL, 0U);
    if (task == NULL) {
        rc = test_fail_errno("post-destroy channel drain spawn failed");
        goto cleanup;
    }
    if (llam_runtime_run_handle(runtime) != 0 || llam_join(task) != 0) {
        rc = test_fail_errno("post-destroy channel drain run/join failed");
        task = NULL;
        goto cleanup;
    }
    task = NULL;
    if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U || state.channel == NULL) {
        errno = state.first_errno;
        rc = test_fail_errno("post-destroy channel drain setup failed");
        goto cleanup;
    }

    llam_runtime_destroy(runtime);
    runtime = NULL;

    /*
     * The channel API promises host cleanup can close and drain buffered values
     * after the producing runtime is gone.  This is a cold cleanup path, not a
     * cross-runtime managed receive, so it must not fail with EXDEV or touch the
     * retired runtime's scheduler state.
     */
    if (llam_channel_close(state.channel) != 0) {
        rc = test_fail_errno("explicit channel close after owner runtime destroy failed");
        goto cleanup;
    }
    if (llam_channel_try_recv_result(state.channel, &out) != 0 || out != &state.payload) {
        rc = test_fail_errno("explicit channel drain after owner runtime destroy failed");
        goto cleanup;
    }
    if (llam_channel_try_recv_result(state.channel, &out) != -1 || errno != EPIPE || out != NULL) {
        rc = test_fail_errno("closed post-destroy channel did not report EPIPE after drain");
        goto cleanup;
    }
    if (llam_channel_destroy(state.channel) != 0) {
        rc = test_fail_errno("post-drain channel destroy failed");
        goto cleanup;
    }
    state.channel = NULL;
    rc = 0;

cleanup:
    if (task != NULL) {
        (void)llam_detach(task);
    }
    if (runtime != NULL) {
        llam_runtime_destroy(runtime);
    }
    if (state.channel != NULL) {
        (void)llam_channel_destroy(state.channel);
    }
    return rc;
}

static int run_legacy_stop_wrapper_case(bool use_shutdown_wrapper) {
    llam_runtime_opts_t opts;
    legacy_stop_wrapper_state_t state;
    llam_runtime_t *runtime = NULL;
    llam_task_t *sleeper = NULL;
    llam_task_t *requester = NULL;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.sleeper_entered, 0U);
    atomic_init(&state.sleeper_errno, 0U);
    atomic_init(&state.stopper_failed, 0U);
    state.use_shutdown_wrapper = use_shutdown_wrapper;
    if (init_runtime_opts(&opts) != 0) {
        return test_fail_errno("runtime opts init failed");
    }
    if (llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime) != 0) {
        return test_fail_errno("runtime create for legacy stop wrapper failed");
    }
    sleeper = llam_runtime_spawn_ex(runtime, legacy_stop_sleeper_task, &state, NULL, 0U);
    requester = llam_runtime_spawn_ex(runtime, legacy_stop_requester_task, &state, NULL, 0U);
    if (sleeper == NULL || requester == NULL) {
        rc = test_fail_errno("legacy stop wrapper spawn failed");
        goto cleanup;
    }
    if (llam_runtime_run_handle(runtime) != 0) {
        rc = test_fail_errno("legacy stop wrapper runtime run failed");
        goto cleanup;
    }
    if (llam_join(requester) != 0) {
        rc = test_fail_errno("legacy stop requester join failed");
        requester = NULL;
        goto cleanup;
    }
    requester = NULL;
    if (llam_join(sleeper) != 0) {
        rc = test_fail_errno("legacy stop sleeper join failed");
        sleeper = NULL;
        goto cleanup;
    }
    sleeper = NULL;
    if (atomic_load_explicit(&state.stopper_failed, memory_order_acquire) != 0U) {
        errno = state.stopper_errno;
        rc = test_fail_errno("legacy request_stop wrapper failed inside explicit runtime");
        goto cleanup;
    }
    if (atomic_load_explicit(&state.sleeper_errno, memory_order_acquire) != (unsigned)ECANCELED) {
        rc = test_fail(use_shutdown_wrapper
                           ? "llam_runtime_shutdown did not stop the current explicit runtime"
                           : "llam_runtime_request_stop did not stop the current explicit runtime");
        goto cleanup;
    }
    rc = 0;

cleanup:
    if (requester != NULL) {
        (void)llam_detach(requester);
    }
    if (sleeper != NULL) {
        (void)llam_detach(sleeper);
    }
    if (runtime != NULL) {
        llam_runtime_destroy(runtime);
    }
    return rc;
}

static int test_legacy_stop_wrappers_target_current_runtime(void) {
    if (run_legacy_stop_wrapper_case(false) != 0) {
        return 1;
    }
    if (run_legacy_stop_wrapper_case(true) != 0) {
        return 1;
    }
    return 0;
}

static int test_managed_destroy_foreign_runtime_does_not_stop_peer(void) {
    llam_runtime_opts_t opts;
    llam_runtime_t *runtime_a = NULL;
    llam_runtime_t *runtime_b = NULL;
    llam_task_t *target = NULL;
    llam_task_t *destroyer = NULL;
    managed_foreign_destroy_state_t state;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.target_entered, 0U);
    atomic_init(&state.destroyer_done, 0U);
    atomic_init(&state.target_done, 0U);
    atomic_init(&state.failures, 0U);

    if (init_runtime_opts(&opts) != 0) {
        return test_fail_errno("runtime opts init failed");
    }
    if (llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime_a) != 0 ||
        llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime_b) != 0) {
        rc = test_fail_errno("runtime create for managed foreign destroy failed");
        goto cleanup;
    }
    state.foreign_runtime = runtime_b;
    target = llam_runtime_spawn_ex(runtime_b, managed_foreign_destroy_target_task, &state, NULL, 0U);
    destroyer = llam_runtime_spawn_ex(runtime_a, managed_foreign_destroy_requester_task, &state, NULL, 0U);
    if (target == NULL || destroyer == NULL) {
        rc = test_fail_errno("runtime spawn for managed foreign destroy failed");
        goto cleanup;
    }
    if (run_two_runtimes(runtime_a, runtime_b) != 0) {
        rc = test_fail_errno("managed foreign destroy runtimes failed");
        goto cleanup;
    }
    if (llam_join(destroyer) != 0 || llam_join(target) != 0) {
        rc = test_fail_errno("managed foreign destroy join failed");
        goto cleanup;
    }
    destroyer = NULL;
    target = NULL;
    if (atomic_load_explicit(&state.destroyer_done, memory_order_acquire) != 1U ||
        atomic_load_explicit(&state.target_done, memory_order_acquire) != 1U) {
        rc = test_fail("managed foreign destroy tasks did not finish");
        goto cleanup;
    }
    if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
        errno = state.first_errno;
        rc = test_fail_errno("managed foreign runtime destroy stopped peer runtime");
        goto cleanup;
    }
    rc = 0;

cleanup:
    if (destroyer != NULL) {
        (void)llam_detach(destroyer);
    }
    if (target != NULL) {
        (void)llam_detach(target);
    }
    llam_runtime_destroy(runtime_b);
    llam_runtime_destroy(runtime_a);
    return rc;
}

int main(void) {
    static const multi_runtime_named_test_t tests[] = {
        {"sync_handle_family_confusion", test_sync_handle_family_confusion},
        {"runtime_run_handle_rejects_null", test_runtime_run_handle_rejects_null},
        {"idle_block_workers_destroy_repeat", test_idle_block_workers_destroy_repeat},
        {"concurrent_spawn_join", test_concurrent_spawn_join},
        {"sequential_runtime_host_join_owner_cleanup", test_sequential_runtime_host_join_owner_cleanup},
        {"cross_runtime_task_owner", test_cross_runtime_task_owner},
        {"cross_runtime_object_owner", test_cross_runtime_object_owner},
        {"cross_runtime_spawn_cancel_token_owner", test_cross_runtime_spawn_cancel_token_owner},
        {"cross_runtime_allocator_returns_are_remote", test_cross_runtime_allocator_returns_are_remote},
        {"task_group_host_spawn_uses_group_runtime", test_task_group_host_spawn_uses_group_runtime},
        {"concurrent_runtime_io", test_concurrent_runtime_io},
        {"explicit_owned_buffers_survive_runtime_destroy", test_explicit_owned_buffers_survive_runtime_destroy},
        {"destroyed_runtime_io_cancel_does_not_stop_peer_runtime", test_destroyed_runtime_io_cancel_does_not_stop_peer_runtime},
        {"process_signal_handler_survives_peer_runtime_destroy",
         test_process_signal_handler_survives_peer_runtime_destroy},
        {"host_try_ops_ignore_default_runtime_race", test_host_try_ops_ignore_default_runtime_race},
        {"explicit_channel_host_try_races_runtime_destroy", test_explicit_channel_host_try_races_runtime_destroy},
        {"default_channel_host_try_ops_ignore_default_runtime_reinit", test_default_channel_host_try_ops_ignore_default_runtime_reinit},
        {"concurrent_runtime_destroy_is_single_owner", test_concurrent_runtime_destroy_is_single_owner},
        {"runtime_destroy_waits_for_active_run", test_runtime_destroy_waits_for_active_run},
        {"runtime_run_start_destroy_race", test_runtime_run_start_destroy_race},
        {"runtime_stats_destroy_race", test_runtime_stats_destroy_race},
        {"destroyed_runtime_handle_address_not_reused", test_destroyed_runtime_handle_address_not_reused},
        {"public_cleanup_after_owner_runtime_destroy", test_public_cleanup_after_owner_runtime_destroy},
        {"explicit_channel_drain_after_owner_runtime_destroy", test_explicit_channel_drain_after_owner_runtime_destroy},
        {"legacy_stop_wrappers_target_current_runtime", test_legacy_stop_wrappers_target_current_runtime},
        {"managed_destroy_foreign_runtime_does_not_stop_peer", test_managed_destroy_foreign_runtime_does_not_stop_peer},
    };
    size_t i;

    /*
     * CI timeout diagnostics depend on seeing the last completed subcase.
     * Keep progress lines visible even when the process is killed externally.
     */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    for (i = 0U; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        if (run_named_test(&tests[i]) != 0) {
            return 1;
        }
    }
    printf("[test_multi_runtime_core] ok\n");
    return 0;
}
