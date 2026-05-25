/**
 * @file tests/test_runtime_invariants.c
 * @brief Seeded runtime invariant checks for bugs not covered by fixed cases.
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
#include "test_env.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INVARIANT_DEFAULT_SCENARIOS 32U
#define INVARIANT_MAX_SCENARIOS 1024U
#define INVARIANT_MAX_CHANNEL_VALUES 32U
#define INVARIANT_MAX_CANCEL_WAITERS 48U

typedef struct invariant_prng {
    uint64_t state;
} invariant_prng_t;

typedef struct invariant_error {
    atomic_uint failures;
    int first_errno;
    char first_case[160];
} invariant_error_t;

typedef struct channel_drain_state {
    invariant_error_t error;
    llam_channel_t *channel;
    void *values[INVARIANT_MAX_CHANNEL_VALUES];
    unsigned value_count;
} channel_drain_state_t;

typedef struct select_state {
    invariant_error_t error;
    llam_channel_t *primary;
    llam_channel_t *secondary;
} select_state_t;

typedef struct cancel_state {
    invariant_error_t error;
    llam_cancel_token_t *token;
    llam_channel_t *channel;
    llam_mutex_t *mutex;
    llam_cond_t *cond;
    atomic_uint waiting;
    atomic_uint canceled;
    unsigned expected_waiters;
} cancel_state_t;

typedef struct group_timeout_state {
    invariant_error_t error;
    llam_task_group_t *group;
    atomic_uint fast_done;
    atomic_uint slow_go;
    atomic_uint slow_done;
} group_timeout_state_t;

static uint64_t invariant_next(invariant_prng_t *prng) {
    uint64_t x = prng->state;

    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    prng->state = x == 0U ? UINT64_C(0x6c6c616d696e7673) : x;
    return prng->state;
}

static unsigned invariant_range(invariant_prng_t *prng, unsigned min_value, unsigned max_value) {
    uint64_t span = (uint64_t)max_value - (uint64_t)min_value + 1U;

    return min_value + (unsigned)(invariant_next(prng) % span);
}

static void invariant_error_init(invariant_error_t *error) {
    atomic_init(&error->failures, 0U);
    error->first_errno = 0;
    error->first_case[0] = '\0';
}

static void task_fail(invariant_error_t *error, const char *where, int err) {
    if (atomic_fetch_add_explicit(&error->failures, 1U, memory_order_relaxed) == 0U) {
        error->first_errno = err;
        (void)snprintf(error->first_case, sizeof(error->first_case), "%s", where);
    }
}

static int check_task_failures(const char *scenario, unsigned index, uint64_t seed, invariant_error_t *error) {
    if (atomic_load_explicit(&error->failures, memory_order_relaxed) == 0U) {
        return 0;
    }
    fprintf(stderr,
            "[test_runtime_invariants] scenario=%s index=%u seed=%llu failed at %s errno=%d (%s)\n",
            scenario,
            index,
            (unsigned long long)seed,
            error->first_case,
            error->first_errno,
            strerror(error->first_errno));
    return 1;
}

static int fail_errno(const char *scenario, unsigned index, uint64_t seed, const char *message) {
    fprintf(stderr,
            "[test_runtime_invariants] scenario=%s index=%u seed=%llu %s: errno=%d (%s)\n",
            scenario,
            index,
            (unsigned long long)seed,
            message,
            errno,
            strerror(errno));
    return 1;
}

static int fail_msg(const char *scenario, unsigned index, uint64_t seed, const char *message) {
    fprintf(stderr,
            "[test_runtime_invariants] scenario=%s index=%u seed=%llu %s\n",
            scenario,
            index,
            (unsigned long long)seed,
            message);
    return 1;
}

static int init_runtime(void) {
    llam_runtime_opts_t opts;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    opts.experimental_flags = LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    return llam_runtime_init_ex(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE);
}

static int spawn_detached(llam_task_fn fn, void *arg) {
    llam_task_t *task = llam_spawn(fn, arg, NULL);

    if (task == NULL) {
        return -1;
    }
    return llam_detach(task);
}

static int check_stats_sane(const char *scenario, unsigned index, uint64_t seed) {
    llam_runtime_stats_t stats;

    memset(&stats, 0, sizeof(stats));
    if (llam_runtime_collect_stats_ex(&stats, LLAM_RUNTIME_STATS_CURRENT_SIZE) != 0) {
        return fail_errno(scenario, index, seed, "stats collection failed");
    }
    if (stats.active_workers == 0U ||
        stats.online_workers > stats.active_workers ||
        stats.online_workers_floor > stats.active_workers ||
        stats.online_workers_min > stats.online_workers_max ||
        stats.online_workers_max > stats.active_workers) {
        return fail_msg(scenario, index, seed, "runtime stats violated worker-count invariants");
    }
    return 0;
}

static void channel_drain_task(void *arg) {
    channel_drain_state_t *state = arg;

    /*
     * A closed channel must still drain all buffered values exactly once,
     * including NULL payloads. After the buffer drains, both recv and send must
     * report EPIPE so FFI bindings can distinguish close from a NULL value.
     */
    for (unsigned i = 0U; i < state->value_count; ++i) {
        if (llam_channel_send(state->channel, state->values[i]) != 0) {
            task_fail(&state->error, "channel prefill send", errno);
            return;
        }
    }
    if (llam_channel_close(state->channel) != 0 ||
        llam_channel_close(state->channel) != 0) {
        task_fail(&state->error, "channel close idempotence", errno);
        return;
    }
    for (unsigned i = 0U; i < state->value_count; ++i) {
        void *received = (void *)(uintptr_t)0xBAD0U;

        if (llam_channel_recv_result(state->channel, &received) != 0 || received != state->values[i]) {
            task_fail(&state->error, "channel close/drain ordering", errno);
            return;
        }
    }
    errno = 0;
    {
        void *closed_value = NULL;

        if (llam_channel_recv_result(state->channel, &closed_value) != -1 || errno != EPIPE) {
            task_fail(&state->error, "channel recv after drained close", errno);
            return;
        }
    }
    errno = 0;
    if (llam_channel_send(state->channel, (void *)(uintptr_t)1U) != -1 || errno != EPIPE) {
        task_fail(&state->error, "channel send after close", errno);
    }
}

static int run_channel_drain(uint64_t seed, unsigned index, invariant_prng_t *prng) {
    const char *scenario = "channel-drain";
    channel_drain_state_t state;
    unsigned capacity = invariant_range(prng, 1U, 8U);
    int rc = 1;

    memset(&state, 0, sizeof(state));
    invariant_error_init(&state.error);
    state.value_count = invariant_range(prng, 0U, capacity);
    for (unsigned i = 0U; i < state.value_count; ++i) {
        if (i == 0U && (invariant_next(prng) & 1U) != 0U) {
            state.values[i] = NULL;
        } else {
            state.values[i] = (void *)(uintptr_t)(UINT64_C(0xC1000000) | ((uint64_t)index << 8) | i);
        }
    }
    state.channel = llam_channel_create(capacity);
    if (state.channel == NULL) {
        return fail_errno(scenario, index, seed, "channel create failed");
    }
    if (init_runtime() != 0) {
        goto cleanup;
    }
    if (spawn_detached(channel_drain_task, &state) != 0) {
        (void)llam_runtime_request_stop();
        goto cleanup_runtime;
    }
    if (llam_run() != 0 ||
        check_task_failures(scenario, index, seed, &state.error) != 0 ||
        check_stats_sane(scenario, index, seed) != 0) {
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    llam_runtime_shutdown();
cleanup:
    if (state.channel != NULL && llam_channel_destroy(state.channel) != 0) {
        rc = 1;
    }
    if (rc != 0 && atomic_load_explicit(&state.error.failures, memory_order_relaxed) == 0U) {
        return fail_errno(scenario, index, seed, "channel drain scenario failed");
    }
    return rc;
}

static void select_invariant_task(void *arg) {
    select_state_t *state = arg;
    llam_select_op_t ops[2];
    void *received = NULL;
    size_t selected = 99U;

    /*
     * Select has two invariants that catch lost wakeups: an empty immediate
     * scan must time out, and a closed receive arm must be selectable with
     * per-op EPIPE rather than parking forever.
     */
    memset(ops, 0, sizeof(ops));
    ops[0].kind = LLAM_SELECT_OP_RECV;
    ops[0].channel = state->primary;
    ops[0].recv_out = &received;
    ops[1].kind = LLAM_SELECT_OP_RECV;
    ops[1].channel = state->secondary;
    ops[1].recv_out = &received;
    errno = 0;
    if (llam_channel_select(ops, 2U, 0U, &selected) != -1 || errno != ETIMEDOUT) {
        task_fail(&state->error, "select empty immediate timeout", errno);
        return;
    }

    if (llam_channel_close(state->secondary) != 0) {
        task_fail(&state->error, "select close secondary", errno);
        return;
    }
    received = NULL;
    selected = 99U;
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
        task_fail(&state->error, "select closed recv arm", errno);
    }
}

static int run_select_invariant(uint64_t seed, unsigned index, invariant_prng_t *prng) {
    const char *scenario = "select";
    select_state_t state;
    int rc = 1;

    (void)prng;
    memset(&state, 0, sizeof(state));
    invariant_error_init(&state.error);
    state.primary = llam_channel_create(1U);
    state.secondary = llam_channel_create(1U);
    if (state.primary == NULL || state.secondary == NULL) {
        return fail_errno(scenario, index, seed, "channel create failed");
    }
    if (init_runtime() != 0) {
        goto cleanup;
    }
    if (spawn_detached(select_invariant_task, &state) != 0) {
        (void)llam_runtime_request_stop();
        goto cleanup_runtime;
    }
    if (llam_run() != 0 ||
        check_task_failures(scenario, index, seed, &state.error) != 0 ||
        check_stats_sane(scenario, index, seed) != 0) {
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    llam_runtime_shutdown();
cleanup:
    if (state.primary != NULL && llam_channel_destroy(state.primary) != 0) {
        rc = 1;
    }
    if (state.secondary != NULL && llam_channel_destroy(state.secondary) != 0) {
        rc = 1;
    }
    if (rc != 0 && atomic_load_explicit(&state.error.failures, memory_order_relaxed) == 0U) {
        return fail_errno(scenario, index, seed, "select scenario failed");
    }
    return rc;
}

static void cancel_sleep_waiter(void *arg) {
    cancel_state_t *state = arg;

    atomic_fetch_add_explicit(&state->waiting, 1U, memory_order_release);
    errno = 0;
    if (llam_sleep_ns(60ULL * 1000ULL * 1000ULL * 1000ULL) != -1 || errno != ECANCELED) {
        task_fail(&state->error, "cancel sleep waiter", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->canceled, 1U, memory_order_relaxed);
}

static void cancel_channel_waiter(void *arg) {
    cancel_state_t *state = arg;
    void *received = NULL;

    atomic_fetch_add_explicit(&state->waiting, 1U, memory_order_release);
    errno = 0;
    if (llam_channel_recv_result(state->channel, &received) != -1 || errno != ECANCELED) {
        task_fail(&state->error, "cancel channel waiter", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->canceled, 1U, memory_order_relaxed);
}

static void cancel_cond_waiter(void *arg) {
    cancel_state_t *state = arg;

    if (llam_mutex_lock(state->mutex) != 0) {
        task_fail(&state->error, "cancel cond lock", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->waiting, 1U, memory_order_release);
    errno = 0;
    if (llam_cond_wait(state->cond, state->mutex) != -1 || errno != ECANCELED) {
        task_fail(&state->error, "cancel cond waiter", errno);
        (void)llam_mutex_unlock(state->mutex);
        return;
    }
    if (llam_mutex_unlock(state->mutex) != 0) {
        task_fail(&state->error, "cancel cond unlock after reacquire", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->canceled, 1U, memory_order_relaxed);
}

static void cancel_trigger_task(void *arg) {
    cancel_state_t *state = arg;

    while (atomic_load_explicit(&state->waiting, memory_order_acquire) < state->expected_waiters) {
        llam_yield();
    }
    for (unsigned i = 0U; i < 3U; ++i) {
        llam_yield();
    }
    if (llam_cancel_token_cancel(state->token) != 0) {
        task_fail(&state->error, "cancel trigger", errno);
    }
}

static int spawn_cancel_waiter(llam_task_fn fn, cancel_state_t *state) {
    llam_spawn_opts_t opts;
    llam_task_t *task;

    if (llam_spawn_opts_init(&opts, LLAM_SPAWN_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts.cancel_token = state->token;
    task = llam_spawn(fn, state, &opts);
    if (task == NULL) {
        return -1;
    }
    return llam_detach(task);
}

static int run_cancel_invariant(uint64_t seed, unsigned index, invariant_prng_t *prng) {
    const char *scenario = "cancel";
    cancel_state_t state;
    unsigned sleep_waiters = invariant_range(prng, 1U, 16U);
    unsigned channel_waiters = invariant_range(prng, 1U, 16U);
    unsigned cond_waiters = invariant_range(prng, 1U, 16U);
    int rc = 1;

    memset(&state, 0, sizeof(state));
    invariant_error_init(&state.error);
    atomic_init(&state.waiting, 0U);
    atomic_init(&state.canceled, 0U);
    state.expected_waiters = sleep_waiters + channel_waiters + cond_waiters;
    if (state.expected_waiters > INVARIANT_MAX_CANCEL_WAITERS) {
        state.expected_waiters = INVARIANT_MAX_CANCEL_WAITERS;
    }
    state.token = llam_cancel_token_create();
    state.channel = llam_channel_create(1U);
    state.mutex = llam_mutex_create();
    state.cond = llam_cond_create();
    if (state.token == NULL || state.channel == NULL || state.mutex == NULL || state.cond == NULL) {
        goto cleanup;
    }
    if (init_runtime() != 0) {
        goto cleanup;
    }
    for (unsigned i = 0U; i < sleep_waiters; ++i) {
        if (spawn_cancel_waiter(cancel_sleep_waiter, &state) != 0) {
            (void)llam_runtime_request_stop();
            goto cleanup_runtime;
        }
    }
    for (unsigned i = 0U; i < channel_waiters; ++i) {
        if (spawn_cancel_waiter(cancel_channel_waiter, &state) != 0) {
            (void)llam_runtime_request_stop();
            goto cleanup_runtime;
        }
    }
    for (unsigned i = 0U; i < cond_waiters; ++i) {
        if (spawn_cancel_waiter(cancel_cond_waiter, &state) != 0) {
            (void)llam_runtime_request_stop();
            goto cleanup_runtime;
        }
    }
    if (spawn_detached(cancel_trigger_task, &state) != 0) {
        (void)llam_runtime_request_stop();
        goto cleanup_runtime;
    }
    if (llam_run() != 0 ||
        check_task_failures(scenario, index, seed, &state.error) != 0 ||
        check_stats_sane(scenario, index, seed) != 0) {
        goto cleanup_runtime;
    }
    if (atomic_load_explicit(&state.canceled, memory_order_relaxed) != state.expected_waiters) {
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    llam_runtime_shutdown();
cleanup:
    if (state.cond != NULL && llam_cond_destroy(state.cond) != 0) {
        rc = 1;
    }
    if (state.mutex != NULL && llam_mutex_destroy(state.mutex) != 0) {
        rc = 1;
    }
    if (state.channel != NULL && llam_channel_destroy(state.channel) != 0) {
        rc = 1;
    }
    if (state.token != NULL && llam_cancel_token_destroy(state.token) != 0) {
        rc = 1;
    }
    if (rc != 0 && atomic_load_explicit(&state.error.failures, memory_order_relaxed) == 0U) {
        return fail_errno(scenario, index, seed, "cancel scenario failed");
    }
    return rc;
}

static void group_fast_task(void *arg) {
    group_timeout_state_t *state = arg;

    llam_yield();
    atomic_store_explicit(&state->fast_done, 1U, memory_order_release);
}

static void group_slow_task(void *arg) {
    group_timeout_state_t *state = arg;

    while (atomic_load_explicit(&state->slow_go, memory_order_acquire) == 0U) {
        llam_yield();
    }
    atomic_store_explicit(&state->slow_done, 1U, memory_order_release);
}

static void group_joiner_task(void *arg) {
    group_timeout_state_t *state = arg;

    while (atomic_load_explicit(&state->fast_done, memory_order_acquire) == 0U) {
        llam_yield();
    }

    /*
     * This catches a subtle ownership bug class: a timed-out group join may
     * consume completed children, but incomplete children must remain owned by
     * the group so the next full join can finish cleanly.
     */
    errno = 0;
    if (llam_task_group_join_until(state->group, 0U) != -1 || errno != ETIMEDOUT) {
        task_fail(&state->error, "group join_until expired deadline", errno);
        atomic_store_explicit(&state->slow_go, 1U, memory_order_release);
        return;
    }
    atomic_store_explicit(&state->slow_go, 1U, memory_order_release);
    if (llam_task_group_join(state->group) != 0) {
        task_fail(&state->error, "group join after timeout", errno);
    }
}

static int run_group_timeout_invariant(uint64_t seed, unsigned index, invariant_prng_t *prng) {
    const char *scenario = "group-timeout";
    group_timeout_state_t state;
    int rc = 1;

    (void)prng;
    memset(&state, 0, sizeof(state));
    invariant_error_init(&state.error);
    atomic_init(&state.fast_done, 0U);
    atomic_init(&state.slow_go, 0U);
    atomic_init(&state.slow_done, 0U);
    state.group = llam_task_group_create();
    if (state.group == NULL) {
        return fail_errno(scenario, index, seed, "task group create failed");
    }
    if (init_runtime() != 0) {
        goto cleanup;
    }
    if (llam_task_group_spawn(state.group, group_fast_task, &state, NULL) == NULL ||
        llam_task_group_spawn(state.group, group_slow_task, &state, NULL) == NULL ||
        spawn_detached(group_joiner_task, &state) != 0) {
        (void)llam_runtime_request_stop();
        goto cleanup_runtime;
    }
    if (llam_run() != 0 ||
        check_task_failures(scenario, index, seed, &state.error) != 0 ||
        check_stats_sane(scenario, index, seed) != 0) {
        goto cleanup_runtime;
    }
    if (atomic_load_explicit(&state.fast_done, memory_order_acquire) != 1U ||
        atomic_load_explicit(&state.slow_done, memory_order_acquire) != 1U) {
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    if (rc != 0) {
        atomic_store_explicit(&state.slow_go, 1U, memory_order_release);
        (void)llam_task_group_cancel(state.group);
        (void)llam_task_group_join(state.group);
    }
    llam_runtime_shutdown();
cleanup:
    if (state.group != NULL && llam_task_group_destroy(state.group) != 0) {
        rc = 1;
    }
    if (rc != 0 && atomic_load_explicit(&state.error.failures, memory_order_relaxed) == 0U) {
        return fail_errno(scenario, index, seed, "group timeout scenario failed");
    }
    return rc;
}

static int run_scenario(uint64_t seed, unsigned index) {
    invariant_prng_t prng = {
        .state = seed ^ ((uint64_t)index * UINT64_C(0x9e3779b97f4a7c15)),
    };

    switch (index % 4U) {
    case 0U:
        return run_channel_drain(seed, index, &prng);
    case 1U:
        return run_select_invariant(seed, index, &prng);
    case 2U:
        return run_cancel_invariant(seed, index, &prng);
    default:
        return run_group_timeout_invariant(seed, index, &prng);
    }
}

int main(void) {
    uint64_t seed = llam_test_env_u64("LLAM_RUNTIME_INVARIANT_SEED", UINT64_C(0x4c4c414d494e5654));
    unsigned scenarios = llam_test_env_u32("LLAM_RUNTIME_INVARIANT_SCENARIOS",
                                           INVARIANT_DEFAULT_SCENARIOS,
                                           INVARIANT_MAX_SCENARIOS);

    for (unsigned i = 0U; i < scenarios; ++i) {
        if (run_scenario(seed, i) != 0) {
            fprintf(stderr,
                    "[test_runtime_invariants] failing seed=%llu scenario=%u\n",
                    (unsigned long long)seed,
                    i);
            return 1;
        }
    }
    printf("[test_runtime_invariants] ok seed=%llu scenarios=%u\n", (unsigned long long)seed, scenarios);
    return 0;
}
