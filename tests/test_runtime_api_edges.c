/**
 * @file tests/test_runtime_api_edges.c
 * @brief Focused public API edge tests for lifecycle, ownership, cancellation, select, and boundaries.
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

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if !LLAM_PLATFORM_WINDOWS
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

typedef struct edge_state {
    atomic_uint failures;
    atomic_uint ran;
    atomic_uint blocking_calls;
    atomic_uint blocking_started;
    atomic_uint blocking_can_finish;
    atomic_uint blocking_finished;
    atomic_uint blocking_returned_before_finish;
    atomic_uint canceled_waits;
    atomic_uint external_waiter_started;
    atomic_uint external_waiter_done;
    atomic_uint channel_destroy_waiter_started;
    atomic_uint channel_destroy_busy_checked;
    int first_errno;
    char first_case[160];
    llam_channel_t *primary;
    llam_channel_t *secondary;
    llam_channel_t *drain;
    llam_mutex_t *mutex;
    llam_cond_t *cond;
    llam_cancel_token_t *token;
#if !LLAM_PLATFORM_WINDOWS
    int io_fds[2];
#endif
} edge_state_t;

typedef struct cancel_destroy_race_state {
    llam_cancel_token_t *token;
    atomic_uint ready;
    atomic_uint start;
    int cancel_rc;
    int cancel_errno;
    int destroy_rc;
    int destroy_errno;
} cancel_destroy_race_state_t;

static void ownership_task(void *arg);

#if !LLAM_PLATFORM_WINDOWS
typedef struct io_reorder_state {
    atomic_uint failures;
    atomic_uint reader_ready;
    atomic_uint read_successes;
    atomic_uint read_cancellations;
    llam_cancel_token_t *token;
    unsigned writer_delay;
    unsigned cancel_delay;
    int fds[2];
    int first_errno;
    char first_case[160];
} io_reorder_state_t;
#endif

static void cancel_destroy_race_wait_start(cancel_destroy_race_state_t *state) {
    atomic_fetch_add_explicit(&state->ready, 1U, memory_order_acq_rel);
    while (atomic_load_explicit(&state->start, memory_order_acquire) == 0U) {
    }
}

static void *cancel_token_race_cancel_thread(void *arg) {
    cancel_destroy_race_state_t *state = arg;

    cancel_destroy_race_wait_start(state);
    state->cancel_rc = llam_cancel_token_cancel(state->token);
    state->cancel_errno = errno;
    return NULL;
}

static void *cancel_token_race_destroy_thread(void *arg) {
    cancel_destroy_race_state_t *state = arg;

    cancel_destroy_race_wait_start(state);
    state->destroy_rc = llam_cancel_token_destroy(state->token);
    state->destroy_errno = errno;
    return NULL;
}

static int fail_msg(const char *message) {
    fprintf(stderr, "[test_runtime_api_edges] %s\n", message);
    return 1;
}

static int fail_errno(const char *message) {
    fprintf(stderr, "[test_runtime_api_edges] %s: errno=%d (%s)\n", message, errno, strerror(errno));
    return 1;
}

static void task_fail(edge_state_t *state, const char *where, int err) {
    if (atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed) == 0U) {
        state->first_errno = err;
        (void)snprintf(state->first_case, sizeof(state->first_case), "%s", where);
    }
}

static int check_task_failures(edge_state_t *state) {
    if (atomic_load_explicit(&state->failures, memory_order_relaxed) == 0U) {
        return 0;
    }
    fprintf(stderr,
            "[test_runtime_api_edges] task failed at %s errno=%d (%s)\n",
            state->first_case,
            state->first_errno,
            strerror(state->first_errno));
    return 1;
}

#if !LLAM_PLATFORM_WINDOWS
static void io_reorder_fail(io_reorder_state_t *state, const char *where, int err) {
    if (atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed) == 0U) {
        state->first_errno = err;
        (void)snprintf(state->first_case, sizeof(state->first_case), "%s", where);
    }
}

static int check_io_reorder_failures(io_reorder_state_t *state) {
    if (atomic_load_explicit(&state->failures, memory_order_relaxed) == 0U) {
        return 0;
    }
    fprintf(stderr,
            "[test_runtime_api_edges] io reorder failed at %s errno=%d (%s)\n",
            state->first_case,
            state->first_errno,
            strerror(state->first_errno));
    return 1;
}

static void io_reorder_wait_ready(io_reorder_state_t *state) {
    while (atomic_load_explicit(&state->reader_ready, memory_order_acquire) == 0U) {
        llam_yield();
    }
}

static void io_reorder_delay(unsigned yields) {
    for (unsigned i = 0U; i < yields; ++i) {
        llam_yield();
    }
}
#endif

static int wait_until_atomic_nonzero(atomic_uint *flag, uint64_t timeout_ns) {
    uint64_t deadline = llam_now_ns() + timeout_ns;

    while (llam_now_ns() < deadline) {
        if (atomic_load_explicit(flag, memory_order_acquire) != 0U) {
            return 0;
        }
        {
            struct timespec ts = {
                .tv_sec = 0,
                .tv_nsec = 1000000L,
            };

            nanosleep(&ts, NULL);
        }
    }
    errno = ETIMEDOUT;
    return -1;
}

static int init_runtime(void) {
    llam_runtime_opts_t opts;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    opts.experimental_flags = LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    return llam_runtime_init(&opts);
}

static int test_cancel_token_destroy_race(void) {
    for (unsigned i = 0U; i < 4000U; ++i) {
        cancel_destroy_race_state_t state;
        pthread_t canceler;
        pthread_t destroyer;

        memset(&state, 0, sizeof(state));
        atomic_init(&state.ready, 0U);
        atomic_init(&state.start, 0U);
        state.cancel_rc = -2;
        state.destroy_rc = -2;
        state.token = llam_cancel_token_create();
        if (state.token == NULL) {
            return fail_errno("cancel token race create failed");
        }

        if (pthread_create(&canceler, NULL, cancel_token_race_cancel_thread, &state) != 0 ||
            pthread_create(&destroyer, NULL, cancel_token_race_destroy_thread, &state) != 0) {
            return fail_errno("cancel token race pthread_create failed");
        }

        while (atomic_load_explicit(&state.ready, memory_order_acquire) != 2U) {
        }
        atomic_store_explicit(&state.start, 1U, memory_order_release);
        (void)pthread_join(canceler, NULL);
        (void)pthread_join(destroyer, NULL);

        if (!(state.cancel_rc == 0 ||
              (state.cancel_rc == -1 && state.cancel_errno == EINVAL))) {
            return fail_msg("cancel token race cancel returned unexpected result");
        }
        if (!(state.destroy_rc == 0 ||
              (state.destroy_rc == -1 && state.destroy_errno == EBUSY))) {
            return fail_msg("cancel token race destroy returned unexpected result");
        }

        /*
         * If cancellation retained the token long enough to make destroy return
         * EBUSY, the caller still owns the handle and must be able to destroy it
         * after the racing operation exits.
         */
        if (state.destroy_rc == -1 && llam_cancel_token_destroy(state.token) != 0) {
            return fail_errno("cancel token race cleanup destroy failed");
        }
    }
    return 0;
}

static int test_fault_boundary_contracts(void) {
    edge_state_t state;
    llam_runtime_opts_t runtime_opts;
    llam_spawn_opts_t spawn_opts;
    llam_runtime_stats_t stats;
    llam_task_group_t *group;
    llam_task_t *task;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);

    errno = 0;
    if (llam_runtime_opts_init(NULL, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != -1 || errno != EINVAL) {
        return fail_msg("runtime opts init NULL did not fail with EINVAL");
    }
    errno = 0;
    if (llam_spawn_opts_init(NULL, LLAM_SPAWN_OPTS_CURRENT_SIZE) != -1 || errno != EINVAL) {
        return fail_msg("spawn opts init NULL did not fail with EINVAL");
    }
    errno = 0;
    if (llam_runtime_collect_stats_ex(NULL, LLAM_RUNTIME_STATS_CURRENT_SIZE) != -1 || errno != EINVAL) {
        return fail_msg("stats NULL did not fail with EINVAL");
    }
    errno = 0;
    if (llam_channel_create(0U) != NULL || errno != EINVAL) {
        return fail_msg("channel create zero capacity did not fail with EINVAL");
    }
    errno = 0;
    if (llam_channel_create((SIZE_MAX / 2U) + 2U) != NULL || errno != ENOMEM) {
        return fail_msg("channel huge capacity did not fail with ENOMEM");
    }

    if (llam_runtime_opts_init(&runtime_opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return fail_errno("runtime opts init failed");
    }
    runtime_opts.profile = UINT32_MAX;
    errno = 0;
    if (llam_runtime_init_ex(&runtime_opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != -1 || errno != EINVAL) {
        llam_runtime_shutdown();
        return fail_msg("runtime invalid profile did not fail with EINVAL");
    }

    if (init_runtime() != 0) {
        return fail_errno("runtime init after invalid profile failed");
    }
    memset(&stats, 0, sizeof(stats));
    if (llam_runtime_collect_stats_ex(&stats, LLAM_RUNTIME_STATS_CURRENT_SIZE) != 0 ||
        stats.active_workers == 0U) {
        llam_runtime_shutdown();
        return fail_errno("stats after invalid-profile recovery failed");
    }

    if (llam_spawn_opts_init(&spawn_opts, LLAM_SPAWN_OPTS_CURRENT_SIZE) != 0) {
        llam_runtime_shutdown();
        return fail_errno("spawn opts init for invalid task class failed");
    }
    spawn_opts.task_class = UINT32_MAX;
    errno = 0;
    task = llam_spawn_ex(ownership_task, &state, &spawn_opts, LLAM_SPAWN_OPTS_CURRENT_SIZE);
    if (task != NULL || errno != EINVAL) {
        if (task != NULL) {
            (void)llam_detach(task);
        }
        llam_runtime_shutdown();
        return fail_msg("spawn invalid task class did not fail with EINVAL");
    }

    if (llam_spawn_opts_init(&spawn_opts, LLAM_SPAWN_OPTS_CURRENT_SIZE) != 0) {
        llam_runtime_shutdown();
        return fail_errno("spawn opts init for invalid stack class failed");
    }
    spawn_opts.stack_class = UINT32_MAX;
    errno = 0;
    task = llam_spawn_ex(ownership_task, &state, &spawn_opts, LLAM_SPAWN_OPTS_CURRENT_SIZE);
    if (task != NULL || errno != EINVAL) {
        if (task != NULL) {
            (void)llam_detach(task);
        }
        llam_runtime_shutdown();
        return fail_msg("spawn invalid stack class did not fail with EINVAL");
    }

    group = llam_task_group_create();
    if (group == NULL) {
        llam_runtime_shutdown();
        return fail_errno("task group create for fault boundary failed");
    }
    errno = 0;
    if (llam_task_group_spawn_ex(group, ownership_task, &state, &spawn_opts, 0U) != NULL ||
        errno != EINVAL) {
        (void)llam_task_group_destroy(group);
        llam_runtime_shutdown();
        return fail_msg("group spawn non-NULL opts with zero size did not fail with EINVAL");
    }
    if (llam_task_group_destroy(group) != 0) {
        llam_runtime_shutdown();
        return fail_errno("task group destroy after fault boundary failed");
    }
    llam_runtime_shutdown();
    return 0;
}

static void *blocking_echo(void *arg) {
    edge_state_t *state = arg;

    atomic_fetch_add_explicit(&state->blocking_calls, 1U, memory_order_relaxed);
    return arg;
}

static void *blocking_null(void *arg) {
    edge_state_t *state = arg;

    atomic_fetch_add_explicit(&state->blocking_calls, 1U, memory_order_relaxed);
    return NULL;
}

static void *blocking_delayed(void *arg) {
    edge_state_t *state = arg;

    atomic_store_explicit(&state->blocking_started, 1U, memory_order_release);
    while (atomic_load_explicit(&state->blocking_can_finish, memory_order_acquire) == 0U) {
        struct timespec ts = {
            .tv_sec = 0,
            .tv_nsec = 1000000L,
        };

        nanosleep(&ts, NULL);
    }
    atomic_store_explicit(&state->blocking_finished, 1U, memory_order_release);
    return state;
}

static void ownership_task(void *arg) {
    edge_state_t *state = arg;

    if (llam_sleep_ns(1000000ULL) != 0) {
        task_fail(state, "ownership task sleep", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void blocking_task(void *arg) {
    edge_state_t *state = arg;
    void *result = NULL;

    errno = 0;
    if (llam_call_blocking_result(NULL, state, &result) != -1 || errno != EINVAL) {
        task_fail(state, "managed blocking NULL fn", errno);
        return;
    }
    errno = 0;
    if (llam_call_blocking_result(blocking_echo, state, NULL) != -1 || errno != EINVAL) {
        task_fail(state, "managed blocking NULL out", errno);
        return;
    }
    if (llam_call_blocking_result(blocking_echo, state, &result) != 0 || result != state) {
        task_fail(state, "managed blocking echo", errno);
        return;
    }
    result = state;
    if (llam_call_blocking_result(blocking_null, state, &result) != 0 || result != NULL) {
        task_fail(state, "managed blocking NULL result", errno);
        return;
    }

    if (llam_enter_blocking() != 0 ||
        llam_enter_blocking() != 0 ||
        llam_leave_blocking() != 0 ||
        llam_leave_blocking() != 0) {
        task_fail(state, "managed nested opaque blocking", errno);
        return;
    }
    errno = 0;
    if (llam_leave_blocking() != -1 || errno != EINVAL) {
        task_fail(state, "managed unmatched leave blocking", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void blocking_cancel_caller_task(void *arg) {
    edge_state_t *state = arg;
    void *result = NULL;
    int rc;

    rc = llam_call_blocking_result(blocking_delayed, state, &result);
    if (atomic_load_explicit(&state->blocking_finished, memory_order_acquire) == 0U) {
        task_fail(state, "blocking cancellation returned before callback finished", EBUSY);
        atomic_store_explicit(&state->blocking_returned_before_finish, 1U, memory_order_release);
        return;
    }
    if (rc != 0 && errno != ECANCELED) {
        task_fail(state, "blocking cancellation unexpected errno", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void blocking_cancel_task(void *arg) {
    edge_state_t *state = arg;

    while (atomic_load_explicit(&state->blocking_started, memory_order_acquire) == 0U) {
        llam_yield();
    }
    if (llam_cancel_token_cancel(state->token) != 0) {
        task_fail(state, "blocking cancel token cancel", errno);
    }
}

static void blocking_release_task(void *arg) {
    edge_state_t *state = arg;

    while (atomic_load_explicit(&state->blocking_started, memory_order_acquire) == 0U) {
        llam_yield();
    }
    if (llam_sleep_ns(20ULL * 1000ULL * 1000ULL) != 0) {
        task_fail(state, "blocking release sleep", errno);
        return;
    }
    atomic_store_explicit(&state->blocking_can_finish, 1U, memory_order_release);
}

#if !LLAM_PLATFORM_WINDOWS
static void io_reorder_reader_task(void *arg) {
    io_reorder_state_t *state = arg;
    unsigned char byte = 0U;
    ssize_t rc;

    atomic_store_explicit(&state->reader_ready, 1U, memory_order_release);
    errno = 0;
    rc = llam_read_when_ready(state->fds[0], &byte, 1U, -1);
    if (rc == 1 && byte == 0x5aU) {
        atomic_fetch_add_explicit(&state->read_successes, 1U, memory_order_relaxed);
        return;
    }
    if (rc == -1 && errno == ECANCELED) {
        atomic_fetch_add_explicit(&state->read_cancellations, 1U, memory_order_relaxed);
        return;
    }
    io_reorder_fail(state, "read completion/cancel result", errno);
}

static void io_reorder_writer_task(void *arg) {
    io_reorder_state_t *state = arg;
    unsigned char byte = 0x5aU;
    ssize_t written;

    io_reorder_wait_ready(state);
    io_reorder_delay(state->writer_delay);
    do {
        written = write(state->fds[1], &byte, 1U);
    } while (written < 0 && errno == EINTR);
    if (written != 1) {
        io_reorder_fail(state, "write completion trigger", errno);
    }
}

static void io_reorder_cancel_task(void *arg) {
    io_reorder_state_t *state = arg;

    io_reorder_wait_ready(state);
    io_reorder_delay(state->cancel_delay);
    if (llam_cancel_token_cancel(state->token) != 0) {
        io_reorder_fail(state, "cancel completion trigger", errno);
    }
}
#endif

static void channel_close_select_task(void *arg) {
    edge_state_t *state = arg;
    void *received = (void *)(uintptr_t)0xBADU;
    size_t selected = SIZE_MAX;
    llam_select_op_t ops[2];

    if (llam_channel_send(state->drain, NULL) != 0 ||
        llam_channel_send(state->drain, (void *)(uintptr_t)0xC105EU) != 0) {
        task_fail(state, "channel close buffered send", errno);
        return;
    }
    if (llam_channel_close(state->drain) != 0 ||
        llam_channel_close(state->drain) != 0) {
        task_fail(state, "channel close idempotent", errno);
        return;
    }
    if (llam_channel_recv_result(state->drain, &received) != 0 || received != NULL) {
        task_fail(state, "channel drain NULL payload", errno);
        return;
    }
    received = NULL;
    if (llam_channel_recv_result(state->drain, &received) != 0 ||
        received != (void *)(uintptr_t)0xC105EU) {
        task_fail(state, "channel drain value payload", errno);
        return;
    }
    errno = 0;
    if (llam_channel_recv_result(state->drain, &received) != -1 || errno != EPIPE) {
        task_fail(state, "channel recv after closed drain", errno);
        return;
    }
    errno = 0;
    if (llam_channel_send(state->drain, (void *)(uintptr_t)1U) != -1 || errno != EPIPE) {
        task_fail(state, "channel send after close", errno);
        return;
    }

    memset(ops, 0, sizeof(ops));
    ops[0].kind = LLAM_SELECT_OP_RECV;
    ops[0].channel = state->primary;
    ops[0].recv_out = &received;
    ops[1].kind = LLAM_SELECT_OP_RECV;
    ops[1].channel = state->secondary;
    ops[1].recv_out = &received;
    errno = 0;
    if (llam_channel_select(ops, 2U, 0U, &selected) != -1 || errno != ETIMEDOUT) {
        task_fail(state, "channel select immediate timeout", errno);
        return;
    }

    if (llam_channel_close(state->secondary) != 0) {
        task_fail(state, "channel select close fixture", errno);
        return;
    }
    received = NULL;
    selected = SIZE_MAX;
    memset(ops, 0, sizeof(ops));
    ops[0].kind = LLAM_SELECT_OP_RECV;
    ops[0].channel = state->primary;
    ops[0].recv_out = &received;
    ops[1].kind = LLAM_SELECT_OP_RECV;
    ops[1].channel = state->secondary;
    ops[1].recv_out = &received;
    if (llam_channel_select(ops, 2U, UINT64_MAX, &selected) != 0 ||
        selected != 1U ||
        ops[1].result_errno != EPIPE) {
        task_fail(state, "channel select closed recv", errno);
        return;
    }

    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void channel_destroy_waiter_task(void *arg) {
    edge_state_t *state = arg;
    void *received = NULL;

    atomic_store_explicit(&state->channel_destroy_waiter_started, 1U, memory_order_release);
    if (llam_channel_recv_result(state->primary, &received) != -1 || errno != EPIPE) {
        task_fail(state, "channel destroy waiter close result", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void channel_destroy_probe_task(void *arg) {
    edge_state_t *state = arg;

    while (atomic_load_explicit(&state->channel_destroy_waiter_started, memory_order_acquire) == 0U) {
        llam_yield();
    }
    for (unsigned i = 0U; i < 16U; ++i) {
        llam_yield();
    }
    if (llam_channel_close(state->primary) != 0) {
        task_fail(state, "channel destroy probe close", errno);
        return;
    }
    /*
     * Close removes the waiter from the channel queue before the waiter resumes.
     * Destroy must still report EBUSY until that close wake is consumed;
     * otherwise select waiters can later clean up through a freed channel.
     */
    errno = 0;
    if (llam_channel_destroy(state->primary) != -1 || errno != EBUSY) {
        task_fail(state, "channel destroy close-woken waiter busy", errno);
        return;
    }
    atomic_store_explicit(&state->channel_destroy_busy_checked, 1U, memory_order_release);
}

static void cond_mutex_task(void *arg) {
    edge_state_t *state = arg;

    if (llam_mutex_lock(state->mutex) != 0) {
        task_fail(state, "mutex lock", errno);
        return;
    }
    errno = 0;
    if (llam_mutex_lock(state->mutex) != -1 || errno != EDEADLK) {
        task_fail(state, "mutex non-recursive lock", errno);
        (void)llam_mutex_unlock(state->mutex);
        return;
    }
    errno = 0;
    if (llam_mutex_trylock(state->mutex) != -1 || errno != EBUSY) {
        task_fail(state, "mutex trylock owned", errno);
        (void)llam_mutex_unlock(state->mutex);
        return;
    }
    errno = 0;
    if (llam_cond_wait_until(state->cond, state->mutex, 0U) != -1 || errno != ETIMEDOUT) {
        task_fail(state, "cond wait expired deadline", errno);
        (void)llam_mutex_unlock(state->mutex);
        return;
    }
    if (llam_mutex_unlock(state->mutex) != 0) {
        task_fail(state, "mutex unlock after cond timeout", errno);
        return;
    }
    if (llam_cond_signal(state->cond) != 0 ||
        llam_cond_broadcast(state->cond) != 0) {
        task_fail(state, "cond signal/broadcast no waiter", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void precancel_wait_task(void *arg) {
    edge_state_t *state = arg;
    void *received = NULL;
#if !LLAM_PLATFORM_WINDOWS
    char byte = '\0';
    short revents = 0;
#endif

    errno = 0;
    if (llam_sleep_ns(1000000000ULL) != -1 || errno != ECANCELED) {
        task_fail(state, "pre-cancel sleep", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->canceled_waits, 1U, memory_order_relaxed);

    errno = 0;
    /*
     * This duration intentionally overflows when added to the current
     * monotonic timestamp.  The runtime must saturate the absolute deadline so
     * cancellation still wins instead of treating the wrapped deadline as an
     * already-expired successful sleep.
     */
    if (llam_sleep_ns(UINT64_MAX) != -1 || errno != ECANCELED) {
        task_fail(state, "pre-cancel huge relative sleep", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->canceled_waits, 1U, memory_order_relaxed);

    errno = 0;
    if (llam_channel_recv_result(state->primary, &received) != -1 || errno != ECANCELED) {
        task_fail(state, "pre-cancel channel recv", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->canceled_waits, 1U, memory_order_relaxed);

    errno = 0;
    if (llam_channel_recv(state->primary) != NULL || errno != ECANCELED) {
        task_fail(state, "pre-cancel legacy channel recv", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->canceled_waits, 1U, memory_order_relaxed);

    /*
     * Immediate operations may still complete with a pre-cancelled token; the
     * token is observed only once the second send has to publish a wait node.
     */
    if (llam_channel_send(state->secondary, state) != 0) {
        task_fail(state, "pre-cancel legacy channel send setup", errno);
        return;
    }
    errno = 0;
    if (llam_channel_send(state->secondary, state) != -1 || errno != ECANCELED) {
        task_fail(state, "pre-cancel legacy channel send wait", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->canceled_waits, 1U, memory_order_relaxed);

#if !LLAM_PLATFORM_WINDOWS
    errno = 0;
    /*
     * The request is published before cancellation registration is checked.
     * A pre-cancelled token must detach it from the submit/watch owner before
     * returning, otherwise the backend can later observe a stale request.
     */
    if (llam_read(state->io_fds[0], &byte, 1U) != -1 || errno != ECANCELED) {
        task_fail(state, "pre-cancel io read", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->canceled_waits, 1U, memory_order_relaxed);

    errno = 0;
    revents = 0;
    if (llam_poll_fd(state->io_fds[0], POLLIN, 1000, &revents) != -1 || errno != ECANCELED) {
        task_fail(state, "pre-cancel timed poll", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->canceled_waits, 1U, memory_order_relaxed);
#endif
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void tiny_deadline_sleep_task(void *arg) {
    edge_state_t *state = arg;

    /*
     * Exercise the timer setup boundary where a watchdog or scheduler can pop
     * a just-armed timer before the sleeper reaches the context switch.  That
     * path must be treated as completion, not as timer allocation failure.
     */
    for (unsigned i = 0U; i < 4096U; ++i) {
        uint64_t deadline_ns = llam_now_ns() + 1U;

        if (llam_sleep_until(deadline_ns) != 0) {
            task_fail(state, "tiny deadline sleep", errno);
            return;
        }
    }
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void stop_waiter_task(void *arg) {
    edge_state_t *state = arg;
    void *received = NULL;

    errno = 0;
    if (llam_channel_recv_result(state->primary, &received) != -1 || errno != ECANCELED) {
        task_fail(state, "runtime stop channel waiter", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->canceled_waits, 1U, memory_order_relaxed);
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void stop_request_task(void *arg) {
    edge_state_t *state = arg;

    if (llam_sleep_ns(1000000ULL) != 0) {
        task_fail(state, "runtime stop trigger sleep", errno);
        return;
    }
    if (llam_runtime_request_stop() != 0) {
        task_fail(state, "runtime stop trigger request", errno);
    }
}

static void stop_late_waiter_task(void *arg) {
    edge_state_t *state = arg;

    for (unsigned i = 0U; i < 16U; ++i) {
        llam_yield();
    }
    stop_waiter_task(state);
}

#if !LLAM_PLATFORM_WINDOWS
static void stop_poll_handle_waiter_task(void *arg) {
    edge_state_t *state = arg;
    short revents = 0;

    errno = 0;
    /*
     * POSIX handles alias fds.  Infinite handle polling must therefore use the
     * fd poll path, where runtime stop can abort the parked I/O request instead
     * of waiting for an uninterruptible blocking-helper callback to return.
     */
    if (llam_poll_handle((llam_handle_t)state->io_fds[0], POLLIN, -1, &revents) != -1 ||
        errno != ECANCELED) {
        task_fail(state, "runtime stop POSIX poll_handle waiter", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->canceled_waits, 1U, memory_order_relaxed);
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void stop_direct_read_waiter_task(void *arg) {
    edge_state_t *state = arg;
    char byte = 0;

    errno = 0;
    /*
     * LLAM_DIRECT_BLOCKING_IO forces the compensated direct-read shortcut.  An
     * empty blocking socket must still observe runtime stop instead of pinning a
     * scheduler thread in an uninterruptible read(2)/recv(2).
     */
    if (llam_read(state->io_fds[0], &byte, 1U) != -1 || errno != ECANCELED) {
        task_fail(state, "runtime stop direct blocking read waiter", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->canceled_waits, 1U, memory_order_relaxed);
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void stop_accept_waiter_task(void *arg) {
    edge_state_t *state = arg;
    struct sockaddr_storage addr;
    socklen_t addr_len = sizeof(addr);
    llam_fd_t accepted;

    errno = 0;
    /*
     * LLAM_ACCEPT_DIRECT_BLOCKING forces the accept helper fallback for address
     * reporting calls.  With no inbound clients, runtime stop must cancel the
     * helper's sliced accept loop; otherwise a running block job keeps
     * llam_run() alive indefinitely.
     */
    accepted = llam_accept(state->io_fds[0], (struct sockaddr *)&addr, &addr_len);
    if (!LLAM_FD_IS_INVALID(accepted)) {
        (void)close(accepted);
        task_fail(state, "runtime stop blocking accept unexpectedly succeeded", 0);
        return;
    }
    if (errno != ECANCELED) {
        task_fail(state, "runtime stop blocking accept waiter", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->canceled_waits, 1U, memory_order_relaxed);
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}
#endif

static void second_run_waiter_task(void *arg) {
    edge_state_t *state = arg;
    void *received = NULL;

    errno = 0;
    if (llam_channel_recv_result(state->primary, &received) != 0 ||
        received != (void *)(uintptr_t)0x51EC0DULL) {
        task_fail(state, "second run channel recv", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void second_run_sender_task(void *arg) {
    edge_state_t *state = arg;

    for (unsigned i = 0U; i < 4U; ++i) {
        llam_yield();
    }
    if (llam_channel_send(state->primary, (void *)(uintptr_t)0x51EC0DULL) != 0) {
        task_fail(state, "second run channel send", errno);
    }
}

static void unmanaged_drain_sender_task(void *arg) {
    edge_state_t *state = arg;

    if (llam_channel_send(state->primary, (void *)(uintptr_t)0xD0A1U) != 0) {
        task_fail(state, "unmanaged drain sender", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void unmanaged_external_recv_waiter_task(void *arg) {
    edge_state_t *state = arg;
    void *received = NULL;

    atomic_store_explicit(&state->external_waiter_started, 1U, memory_order_release);
    if (llam_channel_recv_result(state->primary, &received) != 0 ||
        received != (void *)(uintptr_t)0x1234U) {
        task_fail(state, "unmanaged external channel recv", errno);
        return;
    }
    atomic_store_explicit(&state->external_waiter_done, 1U, memory_order_release);
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void *unmanaged_external_run_thread(void *arg) {
    edge_state_t *state = arg;

    if (llam_run() != 0) {
        task_fail(state, "unmanaged external channel run", errno);
    }
    return NULL;
}

static int test_unmanaged_boundary_contracts(void) {
    edge_state_t state;
    llam_channel_t *channel;
    llam_mutex_t *mutex;
    llam_cond_t *cond;
    llam_task_local_key_t key = LLAM_TASK_LOCAL_INVALID_KEY;
    llam_select_op_t ops[1];
    void *out = NULL;
    size_t selected = SIZE_MAX;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.blocking_calls, 0U);

    channel = llam_channel_create(1U);
    mutex = llam_mutex_create();
    cond = llam_cond_create();
    if (channel == NULL || mutex == NULL || cond == NULL) {
        if (channel != NULL) {
            (void)llam_channel_destroy(channel);
        }
        if (mutex != NULL) {
            (void)llam_mutex_destroy(mutex);
        }
        if (cond != NULL) {
            (void)llam_cond_destroy(cond);
        }
        return fail_errno("unmanaged fixture create failed");
    }
    if (llam_task_local_key_create(&key) != 0) {
        (void)llam_channel_destroy(channel);
        (void)llam_mutex_destroy(mutex);
        (void)llam_cond_destroy(cond);
        return fail_errno("task local key create failed");
    }
    if (init_runtime() != 0) {
        (void)llam_task_local_key_delete(key);
        (void)llam_channel_destroy(channel);
        (void)llam_mutex_destroy(mutex);
        (void)llam_cond_destroy(cond);
        return fail_errno("runtime init for unmanaged boundary failed");
    }

    if (llam_current_task() != NULL) {
        llam_runtime_shutdown();
        return fail_msg("current task outside managed context was not NULL");
    }
    llam_yield();
    llam_task_safepoint();
    if (llam_sleep_ns(0U) != 0 ||
        llam_enter_blocking() != 0 ||
        llam_leave_blocking() != 0) {
        llam_runtime_shutdown();
        return fail_errno("unmanaged no-op/blocking boundary failed");
    }
    if (llam_call_blocking_result(blocking_echo, &state, &out) != 0 ||
        out != &state ||
        atomic_load_explicit(&state.blocking_calls, memory_order_relaxed) != 1U) {
        llam_runtime_shutdown();
        return fail_errno("unmanaged blocking call did not run inline");
    }

    errno = 0;
    if (llam_mutex_lock(mutex) != -1 || errno != ENOTSUP) {
        llam_runtime_shutdown();
        return fail_msg("unmanaged mutex lock did not fail with ENOTSUP");
    }
    errno = 0;
    if (llam_mutex_trylock(mutex) != -1 || errno != ENOTSUP) {
        llam_runtime_shutdown();
        return fail_msg("unmanaged mutex trylock did not fail with ENOTSUP");
    }
    errno = 0;
    if (llam_mutex_unlock(mutex) != -1 || errno != ENOTSUP) {
        llam_runtime_shutdown();
        return fail_msg("unmanaged mutex unlock did not fail with ENOTSUP");
    }
    errno = 0;
    if (llam_cond_wait(cond, mutex) != -1 || errno != ENOTSUP) {
        llam_runtime_shutdown();
        return fail_msg("unmanaged cond wait did not fail with ENOTSUP");
    }
    if (llam_cond_signal(cond) != 0 || llam_cond_broadcast(cond) != 0) {
        llam_runtime_shutdown();
        return fail_errno("unmanaged cond signal/broadcast failed");
    }
    errno = 0;
    if (llam_channel_send(channel, &state) != -1 || errno != ENOTSUP) {
        llam_runtime_shutdown();
        return fail_msg("unmanaged channel send did not fail with ENOTSUP");
    }
    if (llam_channel_try_send(channel, &state) != 0) {
        llam_runtime_shutdown();
        return fail_errno("unmanaged channel try_send failed");
    }
    errno = 0;
    if (llam_channel_try_send(channel, &state) != -1 || errno != EAGAIN) {
        llam_runtime_shutdown();
        return fail_msg("unmanaged channel try_send full case did not fail with EAGAIN");
    }
    errno = 0;
    if (llam_channel_recv_result(channel, &out) != -1 || errno != ENOTSUP) {
        llam_runtime_shutdown();
        return fail_msg("unmanaged channel recv did not fail with ENOTSUP");
    }
    out = NULL;
    if (llam_channel_try_recv_result(channel, &out) != 0 || out != &state) {
        llam_runtime_shutdown();
        return fail_errno("unmanaged channel try_recv failed to drain host-sent value");
    }
    errno = 0;
    if (llam_channel_try_recv_result(channel, &out) != -1 || errno != EAGAIN) {
        llam_runtime_shutdown();
        return fail_msg("unmanaged channel try_recv empty case did not fail with EAGAIN");
    }
    memset(ops, 0, sizeof(ops));
    ops[0].kind = LLAM_SELECT_OP_RECV;
    ops[0].channel = channel;
    ops[0].recv_out = &out;
    errno = 0;
    if (llam_channel_select(ops, 1U, 0U, &selected) != -1 || errno != ENOTSUP) {
        llam_runtime_shutdown();
        return fail_msg("unmanaged channel select did not fail with ENOTSUP");
    }
    errno = 0;
    if (llam_task_local_get(key) != NULL || errno != ENOTSUP) {
        llam_runtime_shutdown();
        return fail_msg("unmanaged task local get did not fail with ENOTSUP");
    }
    errno = 0;
    if (llam_task_local_set(key, &state) != -1 || errno != ENOTSUP) {
        llam_runtime_shutdown();
        return fail_msg("unmanaged task local set did not fail with ENOTSUP");
    }

    llam_runtime_shutdown();
    if (llam_task_local_key_delete(key) != 0 ||
        llam_channel_destroy(channel) != 0 ||
        llam_mutex_destroy(mutex) != 0 ||
        llam_cond_destroy(cond) != 0) {
        return fail_errno("unmanaged fixture destroy failed");
    }
    return 0;
}

static int test_unmanaged_channel_try_drain_after_run(void) {
    edge_state_t state;
    llam_task_t *sender = NULL;
    void *out = NULL;

    memset(&state, 0, sizeof(state));
    state.primary = llam_channel_create(1U);
    if (state.primary == NULL) {
        return fail_errno("unmanaged channel drain fixture create failed");
    }
    if (init_runtime() != 0) {
        (void)llam_channel_destroy(state.primary);
        return fail_errno("runtime init for unmanaged channel drain failed");
    }

    sender = llam_spawn(unmanaged_drain_sender_task, &state, NULL);
    if (sender == NULL) {
        llam_runtime_shutdown();
        (void)llam_channel_destroy(state.primary);
        return fail_errno("unmanaged drain sender spawn failed");
    }
    if (llam_run() != 0 || llam_join(sender) != 0 || check_task_failures(&state) != 0) {
        llam_runtime_shutdown();
        (void)llam_channel_destroy(state.primary);
        return fail_errno("unmanaged drain sender run/join failed");
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U) {
        llam_runtime_shutdown();
        (void)llam_channel_destroy(state.primary);
        return fail_msg("unmanaged drain sender did not run");
    }

    /*
     * Host embedders must be able to drain buffered values after the scheduler
     * has quiesced; otherwise the documented destroy-EBUSY cleanup path is
     * impossible without spawning a synthetic managed task.
     */
    out = NULL;
    if (llam_channel_try_recv_result(state.primary, &out) != 0 ||
        out != (void *)(uintptr_t)0xD0A1U) {
        llam_runtime_shutdown();
        (void)llam_channel_destroy(state.primary);
        return fail_errno("unmanaged host drain of task-sent channel value failed");
    }

    llam_runtime_shutdown();
    if (llam_channel_destroy(state.primary) != 0) {
        return fail_errno("unmanaged drained channel destroy failed");
    }
    return 0;
}

static int test_unmanaged_channel_try_drain_after_shutdown(void) {
    edge_state_t state;
    llam_task_t *sender = NULL;
    void *out = NULL;

    memset(&state, 0, sizeof(state));
    state.primary = llam_channel_create(1U);
    if (state.primary == NULL) {
        return fail_errno("post-shutdown channel drain fixture create failed");
    }
    if (init_runtime() != 0) {
        (void)llam_channel_destroy(state.primary);
        return fail_errno("runtime init for post-shutdown channel drain failed");
    }

    sender = llam_spawn(unmanaged_drain_sender_task, &state, NULL);
    if (sender == NULL) {
        llam_runtime_shutdown();
        (void)llam_channel_destroy(state.primary);
        return fail_errno("post-shutdown drain sender spawn failed");
    }
    if (llam_run() != 0 || llam_join(sender) != 0 || check_task_failures(&state) != 0) {
        llam_runtime_shutdown();
        (void)llam_channel_destroy(state.primary);
        return fail_errno("post-shutdown drain sender run/join failed");
    }
    llam_runtime_shutdown();

    /*
     * Destroying a channel is independent of the runtime singleton.  Buffered
     * payloads therefore remain drainable after shutdown, but no parked sender
     * is woken without a live scheduler.
     */
    out = NULL;
    if (llam_channel_try_recv_result(state.primary, &out) != 0 ||
        out != (void *)(uintptr_t)0xD0A1U) {
        (void)llam_channel_destroy(state.primary);
        return fail_errno("post-shutdown host drain of task-sent channel value failed");
    }
    if (llam_channel_destroy(state.primary) != 0) {
        return fail_errno("post-shutdown drained channel destroy failed");
    }
    return 0;
}

static int test_unmanaged_try_send_wakes_parked_channel_waiter(void) {
    edge_state_t state;
    llam_task_t *waiter = NULL;
    pthread_t runner;
    int runner_created = 0;
    int runner_joined = 0;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.external_waiter_started, 0U);
    atomic_init(&state.external_waiter_done, 0U);
    state.primary = llam_channel_create(1U);
    if (state.primary == NULL) {
        return fail_errno("unmanaged external wake channel create failed");
    }
    if (init_runtime() != 0) {
        goto cleanup_no_runtime;
    }

    waiter = llam_spawn(unmanaged_external_recv_waiter_task, &state, NULL);
    if (waiter == NULL) {
        goto cleanup_runtime;
    }
    if (pthread_create(&runner, NULL, unmanaged_external_run_thread, &state) != 0) {
        goto cleanup_runtime;
    }
    runner_created = 1;

    if (wait_until_atomic_nonzero(&state.external_waiter_started, 1000000000ULL) != 0) {
        goto cleanup_runtime;
    }
    /*
     * Leave the receiver parked across several watchdog intervals.  Before the
     * watchdog learned about externally wakeable channel waits, this delay made
     * the runtime report EDEADLK even though the host thread could still
     * complete the wait with the documented non-parking try_send API.
     */
    {
        struct timespec ts = {
            .tv_sec = 0,
            .tv_nsec = 30000000L,
        };

        nanosleep(&ts, NULL);
    }
    if (llam_channel_try_send(state.primary, (void *)(uintptr_t)0x1234U) != 0) {
        goto cleanup_runtime;
    }
    if (pthread_join(runner, NULL) != 0) {
        goto cleanup_runtime;
    }
    runner_joined = 1;
    if (check_task_failures(&state) != 0 ||
        llam_join(waiter) != 0) {
        waiter = NULL;
        goto cleanup_runtime;
    }
    waiter = NULL;
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.external_waiter_done, memory_order_acquire) != 1U) {
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    if (runner_created != 0 && runner_joined == 0) {
        (void)llam_channel_close(state.primary);
        (void)llam_runtime_request_stop();
        (void)pthread_join(runner, NULL);
        runner_joined = 1;
    }
    if (runner_joined != 0 && waiter != NULL) {
        (void)llam_join(waiter);
        waiter = NULL;
    }
    llam_runtime_shutdown();
cleanup_no_runtime:
    if (state.primary != NULL && llam_channel_destroy(state.primary) != 0) {
        rc = 1;
    }
    if (rc != 0 && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U) {
        return fail_errno("unmanaged external channel wake edge failed");
    }
    return rc;
}

static int test_repeated_run_clears_internal_drain_stop(void) {
    edge_state_t state;
    llam_task_t *first = NULL;
    llam_task_t *waiter = NULL;
    llam_task_t *sender = NULL;
    llam_task_t *blocker = NULL;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    state.primary = llam_channel_create(1U);
    if (state.primary == NULL) {
        goto cleanup_no_runtime;
    }
    if (init_runtime() != 0) {
        goto cleanup_no_runtime;
    }

    first = llam_spawn(ownership_task, &state, NULL);
    if (first == NULL) {
        goto cleanup_runtime;
    }
    if (llam_run() != 0 ||
        check_task_failures(&state) != 0 ||
        llam_join(first) != 0) {
        first = NULL;
        goto cleanup_runtime;
    }
    first = NULL;

    /*
     * A naturally drained run wakes idle workers with the runtime stop flag.
     * The next run must not inherit that internal signal, or normal channel
     * waits in this second run are incorrectly completed with ECANCELED.
     */
    waiter = llam_spawn(second_run_waiter_task, &state, NULL);
    sender = llam_spawn(second_run_sender_task, &state, NULL);
    if (waiter == NULL || sender == NULL) {
        goto cleanup_runtime;
    }
    if (llam_run() != 0 ||
        check_task_failures(&state) != 0 ||
        llam_join(waiter) != 0 ||
        llam_join(sender) != 0) {
        waiter = NULL;
        sender = NULL;
        goto cleanup_runtime;
    }
    waiter = NULL;
    sender = NULL;

    /*
     * Block workers are initialized once and must survive natural run drains.
     * A previous race let a block worker consume the transient drain-stop wake
     * after llam_run() cleared the stop flag, then dereference a NULL job.
     */
    blocker = llam_spawn(blocking_task, &state, NULL);
    if (blocker == NULL) {
        goto cleanup_runtime;
    }
    if (llam_run() != 0 ||
        check_task_failures(&state) != 0 ||
        llam_join(blocker) != 0) {
        blocker = NULL;
        goto cleanup_runtime;
    }
    blocker = NULL;

    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 3U) {
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    if (first != NULL) {
        (void)llam_join(first);
    }
    if (waiter != NULL) {
        (void)llam_join(waiter);
    }
    if (sender != NULL) {
        (void)llam_join(sender);
    }
    if (blocker != NULL) {
        (void)llam_join(blocker);
    }
    llam_runtime_shutdown();
cleanup_no_runtime:
    if (state.primary != NULL && llam_channel_destroy(state.primary) != 0) {
        rc = 1;
    }
    if (rc != 0 && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U) {
        return fail_errno("repeated run drain-stop edge failed");
    }
    return rc;
}

static int test_join_timeout_preserves_ownership(void) {
    edge_state_t state;
    llam_task_t *task;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    if (init_runtime() != 0) {
        return fail_errno("runtime init for join ownership failed");
    }
    task = llam_spawn(ownership_task, &state, NULL);
    if (task == NULL) {
        llam_runtime_shutdown();
        return fail_errno("spawn ownership task failed");
    }
    errno = 0;
    if (llam_join_until(task, 0U) != -1 || errno != ETIMEDOUT) {
        llam_runtime_shutdown();
        return fail_msg("join_until timeout did not preserve handle");
    }
    if (llam_run() != 0) {
        llam_runtime_shutdown();
        return fail_errno("llam_run join ownership failed");
    }
    if (check_task_failures(&state) != 0) {
        llam_runtime_shutdown();
        return 1;
    }
    if (llam_join(task) != 0) {
        llam_runtime_shutdown();
        return fail_errno("join after timeout failed");
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U) {
        llam_runtime_shutdown();
        return fail_msg("ownership task did not run exactly once");
    }
    llam_runtime_shutdown();
    return 0;
}

static int test_blocking_callback_edges(void) {
    edge_state_t state;
    llam_task_t *task;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.blocking_calls, 0U);
    if (init_runtime() != 0) {
        return fail_errno("runtime init for blocking edges failed");
    }
    task = llam_spawn(blocking_task, &state, NULL);
    if (task == NULL) {
        llam_runtime_shutdown();
        return fail_errno("spawn blocking task failed");
    }
    if (llam_run() != 0) {
        llam_runtime_shutdown();
        return fail_errno("llam_run blocking edges failed");
    }
    if (check_task_failures(&state) != 0) {
        llam_runtime_shutdown();
        return 1;
    }
    if (llam_join(task) != 0) {
        llam_runtime_shutdown();
        return fail_errno("join blocking task failed");
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.blocking_calls, memory_order_relaxed) != 2U) {
        llam_runtime_shutdown();
        return fail_msg("blocking edge counters mismatch");
    }
    llam_runtime_shutdown();
    return 0;
}

static int test_blocking_cancel_waits_for_running_callback(void) {
    edge_state_t state;
    llam_spawn_opts_t opts;
    llam_task_t *caller;
    llam_task_t *canceller;
    llam_task_t *releaser;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.blocking_started, 0U);
    atomic_init(&state.blocking_can_finish, 0U);
    atomic_init(&state.blocking_finished, 0U);
    atomic_init(&state.blocking_returned_before_finish, 0U);
    state.token = llam_cancel_token_create();
    if (state.token == NULL ||
        llam_spawn_opts_init(&opts, LLAM_SPAWN_OPTS_CURRENT_SIZE) != 0) {
        goto cleanup_no_runtime;
    }
    opts.cancel_token = state.token;
    if (init_runtime() != 0) {
        goto cleanup_no_runtime;
    }

    caller = llam_spawn(blocking_cancel_caller_task, &state, &opts);
    canceller = llam_spawn(blocking_cancel_task, &state, NULL);
    releaser = llam_spawn(blocking_release_task, &state, NULL);
    if (caller == NULL || canceller == NULL || releaser == NULL) {
        goto cleanup_runtime;
    }
    if (llam_run() != 0 ||
        check_task_failures(&state) != 0 ||
        llam_join(caller) != 0 ||
        llam_join(canceller) != 0 ||
        llam_join(releaser) != 0) {
        goto cleanup_runtime;
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.blocking_finished, memory_order_acquire) == 0U ||
        atomic_load_explicit(&state.blocking_returned_before_finish, memory_order_acquire) != 0U) {
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    llam_runtime_shutdown();
cleanup_no_runtime:
    if (state.token != NULL && llam_cancel_token_destroy(state.token) != 0) {
        rc = 1;
    }
    if (rc != 0 && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U) {
        return fail_errno("blocking cancel running callback edge failed");
    }
    return rc;
}

static int test_channel_close_and_select_edges(void) {
    edge_state_t state;
    llam_task_t *task;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    state.drain = llam_channel_create(4U);
    state.primary = llam_channel_create(1U);
    state.secondary = llam_channel_create(1U);
    if (state.drain == NULL || state.primary == NULL || state.secondary == NULL) {
        goto cleanup_no_runtime;
    }
    if (init_runtime() != 0) {
        goto cleanup_no_runtime;
    }
    task = llam_spawn(channel_close_select_task, &state, NULL);
    if (task == NULL) {
        goto cleanup_runtime;
    }
    if (llam_run() != 0 || check_task_failures(&state) != 0 || llam_join(task) != 0) {
        goto cleanup_runtime;
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U) {
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    llam_runtime_shutdown();
cleanup_no_runtime:
    if (state.drain != NULL && llam_channel_destroy(state.drain) != 0) {
        rc = 1;
    }
    if (state.primary != NULL && llam_channel_destroy(state.primary) != 0) {
        rc = 1;
    }
    if (state.secondary != NULL && llam_channel_destroy(state.secondary) != 0) {
        rc = 1;
    }
    if (rc != 0 && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U) {
        return fail_errno("channel close/select edge failed");
    }
    return rc;
}

static int test_channel_destroy_rejects_close_woken_waiter(void) {
    edge_state_t state;
    llam_task_t *waiter = NULL;
    llam_task_t *probe = NULL;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.channel_destroy_waiter_started, 0U);
    atomic_init(&state.channel_destroy_busy_checked, 0U);
    state.primary = llam_channel_create(1U);
    if (state.primary == NULL) {
        goto cleanup_no_runtime;
    }
    if (init_runtime() != 0) {
        goto cleanup_no_runtime;
    }
    waiter = llam_spawn(channel_destroy_waiter_task, &state, NULL);
    probe = llam_spawn(channel_destroy_probe_task, &state, NULL);
    if (waiter == NULL || probe == NULL) {
        goto cleanup_runtime;
    }
    if (llam_run() != 0 ||
        check_task_failures(&state) != 0 ||
        llam_join(waiter) != 0 ||
        llam_join(probe) != 0) {
        waiter = NULL;
        probe = NULL;
        goto cleanup_runtime;
    }
    waiter = NULL;
    probe = NULL;
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.channel_destroy_busy_checked, memory_order_acquire) != 1U) {
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    llam_runtime_shutdown();
cleanup_no_runtime:
    if (state.primary != NULL && llam_channel_destroy(state.primary) != 0) {
        rc = 1;
    }
    if (rc != 0 && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U) {
        return fail_errno("channel destroy close-woken waiter edge failed");
    }
    return rc;
}

static int test_cond_mutex_deadline_edges(void) {
    edge_state_t state;
    llam_task_t *task;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    state.mutex = llam_mutex_create();
    state.cond = llam_cond_create();
    if (state.mutex == NULL || state.cond == NULL) {
        goto cleanup_no_runtime;
    }
    if (init_runtime() != 0) {
        goto cleanup_no_runtime;
    }
    task = llam_spawn(cond_mutex_task, &state, NULL);
    if (task == NULL) {
        goto cleanup_runtime;
    }
    if (llam_run() != 0 || check_task_failures(&state) != 0 || llam_join(task) != 0) {
        goto cleanup_runtime;
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U) {
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    llam_runtime_shutdown();
cleanup_no_runtime:
    if (state.cond != NULL && llam_cond_destroy(state.cond) != 0) {
        rc = 1;
    }
    if (state.mutex != NULL && llam_mutex_destroy(state.mutex) != 0) {
        rc = 1;
    }
    if (rc != 0 && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U) {
        return fail_errno("cond/mutex edge failed");
    }
    return rc;
}

static int test_precancel_wait_edges(void) {
    edge_state_t state;
    llam_spawn_opts_t opts;
    llam_task_t *task;
    int rc = 1;

    memset(&state, 0, sizeof(state));
#if !LLAM_PLATFORM_WINDOWS
    state.io_fds[0] = -1;
    state.io_fds[1] = -1;
#endif
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.canceled_waits, 0U);
    state.primary = llam_channel_create(1U);
    state.secondary = llam_channel_create(1U);
    state.token = llam_cancel_token_create();
    if (state.primary == NULL || state.secondary == NULL || state.token == NULL) {
        goto cleanup_no_runtime;
    }
#if !LLAM_PLATFORM_WINDOWS
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, state.io_fds) != 0) {
        goto cleanup_no_runtime;
    }
#endif
    if (llam_cancel_token_cancel(state.token) != 0 ||
        llam_spawn_opts_init(&opts, LLAM_SPAWN_OPTS_CURRENT_SIZE) != 0) {
        goto cleanup_no_runtime;
    }
    opts.cancel_token = state.token;
    if (init_runtime() != 0) {
        goto cleanup_no_runtime;
    }
    task = llam_spawn(precancel_wait_task, &state, &opts);
    if (task == NULL) {
        goto cleanup_runtime;
    }
    if (llam_run() != 0 || check_task_failures(&state) != 0 || llam_join(task) != 0) {
        goto cleanup_runtime;
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.canceled_waits, memory_order_relaxed) !=
#if LLAM_PLATFORM_WINDOWS
            5U
#else
            7U
#endif
    ) {
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    llam_runtime_shutdown();
cleanup_no_runtime:
#if !LLAM_PLATFORM_WINDOWS
    if (state.io_fds[0] >= 0) {
        close(state.io_fds[0]);
    }
    if (state.io_fds[1] >= 0) {
        close(state.io_fds[1]);
    }
#endif
    if (state.token != NULL && llam_cancel_token_destroy(state.token) != 0) {
        rc = 1;
    }
    if (state.primary != NULL && llam_channel_destroy(state.primary) != 0) {
        rc = 1;
    }
    if (state.secondary != NULL) {
        void *drained = NULL;

        (void)llam_channel_try_recv_result(state.secondary, &drained);
        if (llam_channel_destroy(state.secondary) != 0) {
            rc = 1;
        }
    }
    if (rc != 0 && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U) {
        return fail_errno("pre-cancel wait edge failed");
    }
    return rc;
}

static int test_tiny_deadline_sleep_race(void) {
    edge_state_t state;
    llam_task_t *task;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    if (init_runtime() != 0) {
        return fail_errno("runtime init for tiny deadline sleep failed");
    }

    task = llam_spawn(tiny_deadline_sleep_task, &state, NULL);
    if (task == NULL) {
        goto cleanup_runtime;
    }
    if (llam_run() != 0 || check_task_failures(&state) != 0 || llam_join(task) != 0) {
        goto cleanup_runtime;
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U) {
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    llam_runtime_shutdown();
    if (rc != 0 && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U) {
        return fail_errno("tiny deadline sleep race failed");
    }
    return rc;
}

#if !LLAM_PLATFORM_WINDOWS
static int run_io_completion_cancel_reorder_round(unsigned round, unsigned writer_delay, unsigned cancel_delay) {
    io_reorder_state_t state;
    llam_spawn_opts_t opts;
    llam_task_t *reader = NULL;
    llam_task_t *writer = NULL;
    llam_task_t *canceller = NULL;
    unsigned completions;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    state.fds[0] = -1;
    state.fds[1] = -1;
    state.writer_delay = writer_delay;
    state.cancel_delay = cancel_delay;
    atomic_init(&state.failures, 0U);
    atomic_init(&state.reader_ready, 0U);
    atomic_init(&state.read_successes, 0U);
    atomic_init(&state.read_cancellations, 0U);

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, state.fds) != 0) {
        return fail_errno("io reorder socketpair setup failed");
    }
    state.token = llam_cancel_token_create();
    if (state.token == NULL ||
        llam_spawn_opts_init(&opts, LLAM_SPAWN_OPTS_CURRENT_SIZE) != 0) {
        goto cleanup_no_runtime;
    }
    opts.cancel_token = state.token;
    if (init_runtime() != 0) {
        goto cleanup_no_runtime;
    }

    reader = llam_spawn(io_reorder_reader_task, &state, &opts);
    writer = llam_spawn(io_reorder_writer_task, &state, NULL);
    canceller = llam_spawn(io_reorder_cancel_task, &state, NULL);
    if (reader == NULL || writer == NULL || canceller == NULL) {
        goto cleanup_runtime;
    }
    if (llam_run() != 0 ||
        check_io_reorder_failures(&state) != 0 ||
        llam_join(reader) != 0 ||
        llam_join(writer) != 0 ||
        llam_join(canceller) != 0) {
        reader = NULL;
        writer = NULL;
        canceller = NULL;
        goto cleanup_runtime;
    }
    reader = NULL;
    writer = NULL;
    canceller = NULL;
    completions = atomic_load_explicit(&state.read_successes, memory_order_relaxed) +
                  atomic_load_explicit(&state.read_cancellations, memory_order_relaxed);
    if (completions != 1U) {
        fprintf(stderr,
                "[test_runtime_api_edges] io reorder round=%u produced %u terminal results\n",
                round,
                completions);
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    if (reader != NULL) {
        (void)llam_join(reader);
    }
    if (writer != NULL) {
        (void)llam_join(writer);
    }
    if (canceller != NULL) {
        (void)llam_join(canceller);
    }
    llam_runtime_shutdown();
cleanup_no_runtime:
    if (state.token != NULL && llam_cancel_token_destroy(state.token) != 0) {
        rc = 1;
    }
    if (state.fds[0] >= 0) {
        (void)close(state.fds[0]);
    }
    if (state.fds[1] >= 0) {
        (void)close(state.fds[1]);
    }
    if (rc != 0 && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U) {
        return fail_errno("io completion/cancel reorder round failed");
    }
    return rc;
}

static int test_posix_io_completion_cancel_reorder(void) {
    /*
     * Exercise both possible orderings around a parked read: backend readiness
     * can arrive before cancellation, or cancellation can detach the request
     * before the ready byte is consumed.  Either a one-byte read or ECANCELED is
     * a valid terminal result, but the request must complete exactly once.
     */
    for (unsigned i = 0U; i < 96U; ++i) {
        unsigned mode = i % 3U;
        unsigned writer_delay = mode == 0U ? 1U : (mode == 1U ? 8U : (i & 3U));
        unsigned cancel_delay = mode == 0U ? 8U : (mode == 1U ? 1U : ((i + 1U) & 3U));

        if (run_io_completion_cancel_reorder_round(i, writer_delay, cancel_delay) != 0) {
            return 1;
        }
    }
    return 0;
}
#endif

static int test_runtime_stop_cancels_parked_channel_wait(void) {
    edge_state_t state;
    llam_task_t *waiter;
    llam_task_t *stopper;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.canceled_waits, 0U);
    state.primary = llam_channel_create(1U);
    if (state.primary == NULL) {
        goto cleanup_no_runtime;
    }
    if (init_runtime() != 0) {
        goto cleanup_no_runtime;
    }

    waiter = llam_spawn(stop_waiter_task, &state, NULL);
    stopper = llam_spawn(stop_request_task, &state, NULL);
    if (waiter == NULL || stopper == NULL) {
        goto cleanup_runtime;
    }
    if (llam_run() != 0 ||
        check_task_failures(&state) != 0 ||
        llam_join(waiter) != 0 ||
        llam_join(stopper) != 0) {
        goto cleanup_runtime;
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.canceled_waits, memory_order_relaxed) != 1U) {
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    llam_runtime_shutdown();
cleanup_no_runtime:
    if (state.primary != NULL && llam_channel_destroy(state.primary) != 0) {
        rc = 1;
    }
    if (rc != 0 && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U) {
        return fail_errno("runtime stop parked channel edge failed");
    }
    return rc;
}

static int test_runtime_stop_cancels_late_channel_wait(void) {
    edge_state_t state;
    llam_spawn_opts_t pinned;
    llam_task_t *waiter;
    llam_task_t *stopper;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.canceled_waits, 0U);
    state.primary = llam_channel_create(1U);
    if (state.primary == NULL ||
        llam_spawn_opts_init(&pinned, LLAM_SPAWN_OPTS_CURRENT_SIZE) != 0) {
        goto cleanup_no_runtime;
    }
    pinned.flags = LLAM_SPAWN_F_PINNED;
    if (init_runtime() != 0) {
        goto cleanup_no_runtime;
    }

    /*
     * The stopper runs first and sets the runtime stop flag. The second task
     * then attempts to enter a fresh channel wait after stop is already active;
     * this used to park forever until the watchdog reported EDEADLK.
     */
    stopper = llam_spawn(stop_request_task, &state, &pinned);
    waiter = llam_spawn(stop_late_waiter_task, &state, &pinned);
    if (waiter == NULL || stopper == NULL) {
        goto cleanup_runtime;
    }
    if (llam_run() != 0 ||
        check_task_failures(&state) != 0 ||
        llam_join(waiter) != 0 ||
        llam_join(stopper) != 0) {
        goto cleanup_runtime;
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.canceled_waits, memory_order_relaxed) != 1U) {
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    llam_runtime_shutdown();
cleanup_no_runtime:
    if (state.primary != NULL && llam_channel_destroy(state.primary) != 0) {
        rc = 1;
    }
    if (rc != 0 && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U) {
        return fail_errno("runtime stop late channel edge failed");
    }
    return rc;
}

#if !LLAM_PLATFORM_WINDOWS
static int test_runtime_stop_cancels_posix_poll_handle_wait(void) {
    edge_state_t state;
    llam_task_t *waiter;
    llam_task_t *stopper;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    state.io_fds[0] = -1;
    state.io_fds[1] = -1;
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.canceled_waits, 0U);

    if (pipe(state.io_fds) != 0) {
        return fail_errno("runtime stop poll_handle pipe setup failed");
    }
    if (init_runtime() != 0) {
        goto cleanup_no_runtime;
    }

    waiter = llam_spawn(stop_poll_handle_waiter_task, &state, NULL);
    stopper = llam_spawn(stop_request_task, &state, NULL);
    if (waiter == NULL || stopper == NULL) {
        goto cleanup_runtime;
    }
    if (llam_run() != 0 ||
        check_task_failures(&state) != 0 ||
        llam_join(waiter) != 0 ||
        llam_join(stopper) != 0) {
        goto cleanup_runtime;
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.canceled_waits, memory_order_relaxed) != 1U) {
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    llam_runtime_shutdown();
cleanup_no_runtime:
    if (state.io_fds[0] >= 0) {
        (void)close(state.io_fds[0]);
    }
    if (state.io_fds[1] >= 0) {
        (void)close(state.io_fds[1]);
    }
    if (rc != 0 && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U) {
        return fail_errno("runtime stop POSIX poll_handle edge failed");
    }
    return rc;
}

static int test_runtime_stop_cancels_direct_blocking_read(void) {
    edge_state_t state;
    llam_task_t *waiter;
    llam_task_t *stopper;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    state.io_fds[0] = -1;
    state.io_fds[1] = -1;
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.canceled_waits, 0U);

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, state.io_fds) != 0) {
        return fail_errno("runtime stop direct blocking read socketpair setup failed");
    }
    if (init_runtime() != 0) {
        goto cleanup_no_runtime;
    }

    waiter = llam_spawn(stop_direct_read_waiter_task, &state, NULL);
    stopper = llam_spawn(stop_request_task, &state, NULL);
    if (waiter == NULL || stopper == NULL) {
        goto cleanup_runtime;
    }
    if (llam_run() != 0 ||
        check_task_failures(&state) != 0 ||
        llam_join(waiter) != 0 ||
        llam_join(stopper) != 0) {
        goto cleanup_runtime;
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.canceled_waits, memory_order_relaxed) != 1U) {
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    llam_runtime_shutdown();
cleanup_no_runtime:
    if (state.io_fds[0] >= 0) {
        (void)close(state.io_fds[0]);
    }
    if (state.io_fds[1] >= 0) {
        (void)close(state.io_fds[1]);
    }
    if (rc != 0 && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U) {
        return fail_errno("runtime stop direct blocking read edge failed");
    }
    return rc;
}

static int test_runtime_stop_cancels_blocking_accept(void) {
    edge_state_t state;
    struct sockaddr_in addr;
    llam_task_t *waiter;
    llam_task_t *stopper;
    int listener = -1;
    int one = 1;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    state.io_fds[0] = -1;
    state.io_fds[1] = -1;
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.canceled_waits, 0U);

    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        return fail_errno("runtime stop blocking accept socket setup failed");
    }
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(listener, 16) != 0) {
        goto cleanup_no_runtime;
    }
    state.io_fds[0] = listener;
    if (init_runtime() != 0) {
        goto cleanup_no_runtime;
    }

    waiter = llam_spawn(stop_accept_waiter_task, &state, NULL);
    stopper = llam_spawn(stop_request_task, &state, NULL);
    if (waiter == NULL || stopper == NULL) {
        goto cleanup_runtime;
    }
    if (llam_run() != 0 ||
        check_task_failures(&state) != 0 ||
        llam_join(waiter) != 0 ||
        llam_join(stopper) != 0) {
        goto cleanup_runtime;
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.canceled_waits, memory_order_relaxed) != 1U) {
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    llam_runtime_shutdown();
cleanup_no_runtime:
    if (listener >= 0) {
        (void)close(listener);
    }
    if (rc != 0 && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U) {
        return fail_errno("runtime stop blocking accept edge failed");
    }
    return rc;
}
#endif

typedef int (*edge_case_fn)(void);

static int run_edge_case(const char *name, edge_case_fn fn) {
    /*
     * CI repeats this binary as a race detector.  Emit case boundaries to stderr
     * so timeout artifacts identify the stuck contract instead of only reporting
     * a silent process-level timeout.
     */
    fprintf(stderr, "[test_runtime_api_edges] begin %s\n", name);
    fflush(stderr);
    if (fn() != 0) {
        fprintf(stderr, "[test_runtime_api_edges] fail %s\n", name);
        fflush(stderr);
        return 1;
    }
    fprintf(stderr, "[test_runtime_api_edges] ok %s\n", name);
    fflush(stderr);
    return 0;
}

int main(void) {
#if !LLAM_PLATFORM_WINDOWS
    /*
     * Force the compensated direct-poll path before any poll policy is cached.
     * Runtime stop must still cancel parked infinite waits; otherwise this mode
     * can pin a scheduler thread inside poll(2).
     */
    if (setenv("LLAM_DIRECT_BLOCKING_POLL", "1", 1) != 0) {
        return fail_errno("enable direct blocking poll edge mode failed");
    }
    if (setenv("LLAM_DIRECT_BLOCKING_IO", "1", 1) != 0) {
        return fail_errno("enable direct blocking I/O edge mode failed");
    }
    if (setenv("LLAM_ACCEPT_DIRECT_BLOCKING", "1", 1) != 0) {
        return fail_errno("enable blocking accept edge mode failed");
    }
#endif
    if (run_edge_case("cancel_token_destroy_race", test_cancel_token_destroy_race) != 0) {
        return 1;
    }
    if (run_edge_case("fault_boundary_contracts", test_fault_boundary_contracts) != 0) {
        return 1;
    }
    if (run_edge_case("unmanaged_boundary_contracts", test_unmanaged_boundary_contracts) != 0) {
        return 1;
    }
    if (run_edge_case("unmanaged_channel_try_drain_after_run",
                      test_unmanaged_channel_try_drain_after_run) != 0) {
        return 1;
    }
    if (run_edge_case("unmanaged_channel_try_drain_after_shutdown",
                      test_unmanaged_channel_try_drain_after_shutdown) != 0) {
        return 1;
    }
    if (run_edge_case("unmanaged_try_send_wakes_parked_channel_waiter",
                      test_unmanaged_try_send_wakes_parked_channel_waiter) != 0) {
        return 1;
    }
    if (run_edge_case("repeated_run_clears_internal_drain_stop",
                      test_repeated_run_clears_internal_drain_stop) != 0) {
        return 1;
    }
    if (run_edge_case("join_timeout_preserves_ownership", test_join_timeout_preserves_ownership) != 0) {
        return 1;
    }
    if (run_edge_case("blocking_callback_edges", test_blocking_callback_edges) != 0) {
        return 1;
    }
    if (run_edge_case("blocking_cancel_waits_for_running_callback",
                      test_blocking_cancel_waits_for_running_callback) != 0) {
        return 1;
    }
    if (run_edge_case("channel_close_and_select_edges", test_channel_close_and_select_edges) != 0) {
        return 1;
    }
    if (run_edge_case("channel_destroy_rejects_close_woken_waiter",
                      test_channel_destroy_rejects_close_woken_waiter) != 0) {
        return 1;
    }
    if (run_edge_case("cond_mutex_deadline_edges", test_cond_mutex_deadline_edges) != 0) {
        return 1;
    }
    if (run_edge_case("precancel_wait_edges", test_precancel_wait_edges) != 0) {
        return 1;
    }
    if (run_edge_case("tiny_deadline_sleep_race", test_tiny_deadline_sleep_race) != 0) {
        return 1;
    }
#if !LLAM_PLATFORM_WINDOWS
    if (run_edge_case("posix_io_completion_cancel_reorder",
                      test_posix_io_completion_cancel_reorder) != 0) {
        return 1;
    }
#endif
    if (run_edge_case("runtime_stop_cancels_parked_channel_wait",
                      test_runtime_stop_cancels_parked_channel_wait) != 0) {
        return 1;
    }
    if (run_edge_case("runtime_stop_cancels_late_channel_wait",
                      test_runtime_stop_cancels_late_channel_wait) != 0) {
        return 1;
    }
#if !LLAM_PLATFORM_WINDOWS
    if (run_edge_case("runtime_stop_cancels_posix_poll_handle_wait",
                      test_runtime_stop_cancels_posix_poll_handle_wait) != 0) {
        return 1;
    }
    if (run_edge_case("runtime_stop_cancels_direct_blocking_read",
                      test_runtime_stop_cancels_direct_blocking_read) != 0) {
        return 1;
    }
    if (run_edge_case("runtime_stop_cancels_blocking_accept",
                      test_runtime_stop_cancels_blocking_accept) != 0) {
        return 1;
    }
#endif
    puts("test_runtime_api_edges ok");
    return 0;
}
