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
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct edge_state {
    atomic_uint failures;
    atomic_uint ran;
    atomic_uint blocking_calls;
    atomic_uint blocking_started;
    atomic_uint blocking_can_finish;
    atomic_uint blocking_finished;
    atomic_uint blocking_returned_before_finish;
    atomic_uint canceled_waits;
    int first_errno;
    char first_case[160];
    llam_channel_t *primary;
    llam_channel_t *secondary;
    llam_channel_t *drain;
    llam_mutex_t *mutex;
    llam_cond_t *cond;
    llam_cancel_token_t *token;
} edge_state_t;

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

static int init_runtime(void) {
    llam_runtime_opts_t opts;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    opts.experimental_flags = LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    return llam_runtime_init(&opts);
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

    errno = 0;
    if (llam_sleep_ns(1000000000ULL) != -1 || errno != ECANCELED) {
        task_fail(state, "pre-cancel sleep", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->canceled_waits, 1U, memory_order_relaxed);

    errno = 0;
    if (llam_channel_recv_result(state->primary, &received) != -1 || errno != ECANCELED) {
        task_fail(state, "pre-cancel channel recv", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->canceled_waits, 1U, memory_order_relaxed);
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
    errno = 0;
    if (llam_channel_try_send(channel, &state) != -1 || errno != ENOTSUP) {
        llam_runtime_shutdown();
        return fail_msg("unmanaged channel try_send did not fail with ENOTSUP");
    }
    errno = 0;
    if (llam_channel_recv_result(channel, &out) != -1 || errno != ENOTSUP) {
        llam_runtime_shutdown();
        return fail_msg("unmanaged channel recv did not fail with ENOTSUP");
    }
    errno = 0;
    if (llam_channel_try_recv_result(channel, &out) != -1 || errno != ENOTSUP) {
        llam_runtime_shutdown();
        return fail_msg("unmanaged channel try_recv did not fail with ENOTSUP");
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
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.canceled_waits, 0U);
    state.primary = llam_channel_create(1U);
    state.token = llam_cancel_token_create();
    if (state.primary == NULL || state.token == NULL) {
        goto cleanup_no_runtime;
    }
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
        atomic_load_explicit(&state.canceled_waits, memory_order_relaxed) != 2U) {
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    llam_runtime_shutdown();
cleanup_no_runtime:
    if (state.token != NULL && llam_cancel_token_destroy(state.token) != 0) {
        rc = 1;
    }
    if (state.primary != NULL && llam_channel_destroy(state.primary) != 0) {
        rc = 1;
    }
    if (rc != 0 && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U) {
        return fail_errno("pre-cancel wait edge failed");
    }
    return rc;
}

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

int main(void) {
    if (test_unmanaged_boundary_contracts() != 0) {
        return 1;
    }
    if (test_repeated_run_clears_internal_drain_stop() != 0) {
        return 1;
    }
    if (test_join_timeout_preserves_ownership() != 0) {
        return 1;
    }
    if (test_blocking_callback_edges() != 0) {
        return 1;
    }
    if (test_blocking_cancel_waits_for_running_callback() != 0) {
        return 1;
    }
    if (test_channel_close_and_select_edges() != 0) {
        return 1;
    }
    if (test_cond_mutex_deadline_edges() != 0) {
        return 1;
    }
    if (test_precancel_wait_edges() != 0) {
        return 1;
    }
    if (test_runtime_stop_cancels_parked_channel_wait() != 0) {
        return 1;
    }
    if (test_runtime_stop_cancels_late_channel_wait() != 0) {
        return 1;
    }
    puts("test_runtime_api_edges ok");
    return 0;
}
