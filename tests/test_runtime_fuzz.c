/**
 * @file tests/test_runtime_fuzz.c
 * @brief Deterministic randomized runtime stress scenarios for CI fuzz coverage.
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
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef LLAM_TEST_NO_SANITIZE_THREAD
#if defined(__SANITIZE_THREAD__)
#define LLAM_TEST_TSAN_BUILD 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define LLAM_TEST_TSAN_BUILD 1
#endif
#endif
#if defined(LLAM_TEST_TSAN_BUILD)
#if defined(__has_attribute)
#if __has_attribute(no_sanitize)
#define LLAM_TEST_NO_SANITIZE_THREAD __attribute__((no_sanitize("thread")))
#elif __has_attribute(no_sanitize_thread)
#define LLAM_TEST_NO_SANITIZE_THREAD __attribute__((no_sanitize_thread))
#endif
#endif
#endif
#ifndef LLAM_TEST_NO_SANITIZE_THREAD
#define LLAM_TEST_NO_SANITIZE_THREAD
#endif
#undef LLAM_TEST_TSAN_BUILD
#endif

/*
 * The fuzz test intentionally pounds stackful fiber switches under TSan.
 * Access errno through tiny no-TSan wrappers so sanitizer noise stays focused
 * on LLAM shared state instead of implementation-defined errno TLS storage.
 */
static inline LLAM_TEST_NO_SANITIZE_THREAD int fuzz_errno_load(void) {
    return errno;
}

static inline LLAM_TEST_NO_SANITIZE_THREAD void fuzz_errno_store(int value) {
    errno = value;
}

#if LLAM_PLATFORM_POSIX
#include <pthread.h>
#elif LLAM_PLATFORM_WINDOWS
#include <windows.h>
#endif

#define FUZZ_MAX_COUNTER_TASKS 256U
#define FUZZ_MAX_CHANNEL_PAIRS 16U
#define FUZZ_MAX_CANCEL_WAITERS 128U
#define FUZZ_DEFAULT_SCENARIOS 48U
#define FUZZ_MAX_SCENARIOS 512U
#define MULTI_FUZZ_DEFAULT_SCENARIOS 16U
#define MULTI_FUZZ_MAX_SCENARIOS 128U
#define MULTI_FUZZ_MAX_MESSAGES 128U

typedef struct fuzz_prng {
    uint64_t state;
} fuzz_prng_t;

typedef struct fuzz_state fuzz_state_t;

typedef struct fuzz_counter_arg {
    fuzz_state_t *state;
    unsigned yields;
} fuzz_counter_arg_t;

typedef struct fuzz_channel_pair {
    fuzz_state_t *state;
    llam_channel_t *channel;
    unsigned messages;
    atomic_uint sent;
    atomic_uint received;
} fuzz_channel_pair_t;

struct fuzz_state {
    atomic_uint failures;
    atomic_uint counters_done;
    atomic_uint waiting_count;
    atomic_uint canceled_count;
    unsigned expected_counters;
    unsigned expected_cancel_waiters;
    llam_cancel_token_t *cancel_token;
    fuzz_counter_arg_t counters[FUZZ_MAX_COUNTER_TASKS];
    fuzz_channel_pair_t pairs[FUZZ_MAX_CHANNEL_PAIRS];
    int first_errno;
    char first_case[128];
};

typedef struct multi_fuzz_runner {
    llam_runtime_t *runtime;
    int rc;
    int err;
} multi_fuzz_runner_t;

typedef struct multi_fuzz_state {
    llam_channel_t *local_channel;
    llam_channel_t *foreign_channel;
    atomic_uint failures;
    atomic_uint sent;
    atomic_uint received;
    atomic_uint cross_checked;
    unsigned capacity;
    unsigned messages;
    unsigned yield_mask;
    int first_errno;
    char first_case[96];
} multi_fuzz_state_t;

static uint64_t fuzz_next(fuzz_prng_t *prng) {
    uint64_t x = prng->state;

    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    prng->state = x == 0U ? UINT64_C(0x9e3779b97f4a7c15) : x;
    return prng->state;
}

static unsigned fuzz_range(fuzz_prng_t *prng, unsigned min_value, unsigned max_value) {
    uint64_t span = (uint64_t)max_value - (uint64_t)min_value + 1U;

    return min_value + (unsigned)(fuzz_next(prng) % span);
}

static int fail_msg(const char *message) {
    fprintf(stderr, "[test_runtime_fuzz] %s\n", message);
    return 1;
}

static int fail_errno(const char *message) {
    fprintf(stderr, "[test_runtime_fuzz] %s: errno=%d (%s)\n", message, errno, strerror(errno));
    return 1;
}

static void task_fail(fuzz_state_t *state, const char *where, int err) {
    if (atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed) == 0U) {
        state->first_errno = err;
        (void)snprintf(state->first_case, sizeof(state->first_case), "%s", where);
    }
}

static void multi_task_fail(multi_fuzz_state_t *state, const char *where, int err) {
    if (atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed) == 0U) {
        state->first_errno = err;
        (void)snprintf(state->first_case, sizeof(state->first_case), "%s", where);
    }
}

#if LLAM_PLATFORM_POSIX
static void *multi_run_runtime_thread(void *arg) {
    multi_fuzz_runner_t *runner = arg;

    fuzz_errno_store(0);
    runner->rc = llam_runtime_run_handle(runner->runtime);
    runner->err = fuzz_errno_load();
    return NULL;
}
#elif LLAM_PLATFORM_WINDOWS
static DWORD WINAPI multi_run_runtime_thread(LPVOID arg) {
    multi_fuzz_runner_t *runner = arg;

    fuzz_errno_store(0);
    runner->rc = llam_runtime_run_handle(runner->runtime);
    runner->err = fuzz_errno_load();
    return 0;
}
#endif

static int multi_run_two_runtimes(llam_runtime_t *runtime_a, llam_runtime_t *runtime_b) {
#if LLAM_PLATFORM_POSIX
    multi_fuzz_runner_t runner_a;
    multi_fuzz_runner_t runner_b;
    pthread_t thread_a;
    pthread_t thread_b;
    int rc;

    memset(&runner_a, 0, sizeof(runner_a));
    memset(&runner_b, 0, sizeof(runner_b));
    runner_a.runtime = runtime_a;
    runner_b.runtime = runtime_b;
    rc = pthread_create(&thread_a, NULL, multi_run_runtime_thread, &runner_a);
    if (rc != 0) {
        fuzz_errno_store(rc);
        return -1;
    }
    rc = pthread_create(&thread_b, NULL, multi_run_runtime_thread, &runner_b);
    if (rc != 0) {
        (void)pthread_join(thread_a, NULL);
        fuzz_errno_store(rc);
        return -1;
    }
    (void)pthread_join(thread_a, NULL);
    (void)pthread_join(thread_b, NULL);
    if (runner_a.rc != 0) {
        fuzz_errno_store(runner_a.err);
        return -1;
    }
    if (runner_b.rc != 0) {
        fuzz_errno_store(runner_b.err);
        return -1;
    }
    return 0;
#elif LLAM_PLATFORM_WINDOWS
    multi_fuzz_runner_t runner_a;
    multi_fuzz_runner_t runner_b;
    HANDLE thread_a;
    HANDLE thread_b;

    memset(&runner_a, 0, sizeof(runner_a));
    memset(&runner_b, 0, sizeof(runner_b));
    runner_a.runtime = runtime_a;
    runner_b.runtime = runtime_b;
    thread_a = CreateThread(NULL, 0U, multi_run_runtime_thread, &runner_a, 0U, NULL);
    if (thread_a == NULL) {
        fuzz_errno_store(EAGAIN);
        return -1;
    }
    thread_b = CreateThread(NULL, 0U, multi_run_runtime_thread, &runner_b, 0U, NULL);
    if (thread_b == NULL) {
        (void)WaitForSingleObject(thread_a, INFINITE);
        (void)CloseHandle(thread_a);
        fuzz_errno_store(EAGAIN);
        return -1;
    }
    (void)WaitForSingleObject(thread_a, INFINITE);
    (void)WaitForSingleObject(thread_b, INFINITE);
    (void)CloseHandle(thread_a);
    (void)CloseHandle(thread_b);
    if (runner_a.rc != 0) {
        fuzz_errno_store(runner_a.err);
        return -1;
    }
    if (runner_b.rc != 0) {
        fuzz_errno_store(runner_b.err);
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

static int spawn_detached(llam_task_fn fn, void *arg) {
    llam_task_t *task = llam_spawn(fn, arg, NULL);

    if (task == NULL) {
        return -1;
    }
    return llam_detach(task);
}

static void multi_channel_setup_task(void *arg) {
    multi_fuzz_state_t *state = arg;

    state->local_channel = llam_channel_create(state->capacity);
    if (state->local_channel == NULL) {
        multi_task_fail(state, "multi channel create", fuzz_errno_load());
    }
}

static void multi_channel_sender_task(void *arg) {
    multi_fuzz_state_t *state = arg;

    for (uintptr_t i = 1U; i <= state->messages; ++i) {
        if (llam_channel_send(state->local_channel, (void *)i) != 0) {
            multi_task_fail(state, "multi channel send", fuzz_errno_load());
            return;
        }
        atomic_fetch_add_explicit(&state->sent, 1U, memory_order_relaxed);
        if ((i & state->yield_mask) == 0U) {
            llam_yield();
        }
    }
}

static void multi_channel_receiver_task(void *arg) {
    multi_fuzz_state_t *state = arg;

    for (uintptr_t i = 1U; i <= state->messages; ++i) {
        void *out = NULL;

        if (llam_channel_recv_result(state->local_channel, &out) != 0) {
            multi_task_fail(state, "multi channel recv", fuzz_errno_load());
            return;
        }
        if (out != (void *)i) {
            multi_task_fail(state, "multi channel payload", EINVAL);
            return;
        }
        atomic_fetch_add_explicit(&state->received, 1U, memory_order_relaxed);
    }
}

static void multi_cross_channel_probe_task(void *arg) {
    multi_fuzz_state_t *state = arg;
    llam_select_op_t op;
    int payload = 1;
    void *out = NULL;
    size_t selected = SIZE_MAX;

    fuzz_errno_store(0);
    if (llam_channel_try_send(state->foreign_channel, &payload) != -1 || fuzz_errno_load() != EXDEV) {
        multi_task_fail(state, "multi foreign channel send", fuzz_errno_load());
        return;
    }
    fuzz_errno_store(0);
    if (llam_channel_try_recv_result(state->foreign_channel, &out) != -1 || fuzz_errno_load() != EXDEV) {
        multi_task_fail(state, "multi foreign channel recv", fuzz_errno_load());
        return;
    }
    memset(&op, 0, sizeof(op));
    op.kind = LLAM_SELECT_OP_RECV;
    op.channel = state->foreign_channel;
    op.recv_out = &out;
    fuzz_errno_store(0);
    if (llam_channel_select(&op, 1U, 0U, &selected) != -1 || fuzz_errno_load() != EXDEV) {
        multi_task_fail(state, "multi foreign channel select", fuzz_errno_load());
        return;
    }
    atomic_fetch_add_explicit(&state->cross_checked, 1U, memory_order_relaxed);
}

static void counter_task(void *arg) {
    fuzz_counter_arg_t *counter = arg;

    for (unsigned i = 0U; i < counter->yields; ++i) {
        llam_yield();
    }
    atomic_fetch_add_explicit(&counter->state->counters_done, 1U, memory_order_relaxed);
}

static void channel_sender_task(void *arg) {
    fuzz_channel_pair_t *pair = arg;

    for (uintptr_t i = 1U; i <= pair->messages; ++i) {
        if (llam_channel_send(pair->channel, (void *)i) != 0) {
            task_fail(pair->state, "channel send", fuzz_errno_load());
            return;
        }
        atomic_fetch_add_explicit(&pair->sent, 1U, memory_order_relaxed);
        if ((i & 15U) == 0U) {
            llam_yield();
        }
    }
}

static void channel_receiver_task(void *arg) {
    fuzz_channel_pair_t *pair = arg;

    for (uintptr_t i = 1U; i <= pair->messages; ++i) {
        void *value = llam_channel_recv(pair->channel);

        if (value != (void *)i) {
            task_fail(pair->state, "channel recv payload", fuzz_errno_load());
            return;
        }
        atomic_fetch_add_explicit(&pair->received, 1U, memory_order_relaxed);
    }
}

static void cancel_waiter_task(void *arg) {
    fuzz_state_t *state = arg;

    atomic_fetch_add_explicit(&state->waiting_count, 1U, memory_order_release);
    fuzz_errno_store(0);
    if (llam_sleep_ns(60ULL * 1000ULL * 1000ULL * 1000ULL) != -1 || fuzz_errno_load() != ECANCELED) {
        task_fail(state, "cancel waiter sleep", fuzz_errno_load());
        return;
    }
    atomic_fetch_add_explicit(&state->canceled_count, 1U, memory_order_relaxed);
}

static void cancel_task(void *arg) {
    fuzz_state_t *state = arg;

    while (atomic_load_explicit(&state->waiting_count, memory_order_acquire) < state->expected_cancel_waiters) {
        llam_yield();
    }
    if (llam_cancel_token_cancel(state->cancel_token) != 0) {
        task_fail(state, "cancel token cancel", fuzz_errno_load());
    }
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

static int init_explicit_runtime_opts(llam_runtime_opts_t *opts, bool deterministic) {
    if (llam_runtime_opts_init(opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return -1;
    }
    opts->deterministic = deterministic ? 1U : 0U;
    opts->profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    opts->experimental_flags = LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    return 0;
}

static int join_task_checked(llam_task_t **task, const char *message) {
    if (task == NULL || *task == NULL) {
        errno = EINVAL;
        return fail_errno(message);
    }
    if (llam_join(*task) != 0) {
        *task = NULL;
        return fail_errno(message);
    }
    *task = NULL;
    return 0;
}

static void cleanup_pairs(fuzz_state_t *state, unsigned pair_count) {
    for (unsigned i = 0U; i < pair_count; ++i) {
        if (state->pairs[i].channel != NULL) {
            (void)llam_channel_destroy(state->pairs[i].channel);
            state->pairs[i].channel = NULL;
        }
    }
}

static void multi_fuzz_state_init(multi_fuzz_state_t *state,
                                  fuzz_prng_t *prng,
                                  unsigned min_messages,
                                  unsigned max_messages) {
    memset(state, 0, sizeof(*state));
    atomic_init(&state->failures, 0U);
    atomic_init(&state->sent, 0U);
    atomic_init(&state->received, 0U);
    atomic_init(&state->cross_checked, 0U);
    state->capacity = fuzz_range(prng, 1U, 8U);
    state->messages = fuzz_range(prng, min_messages, max_messages);
    state->yield_mask = (1U << fuzz_range(prng, 0U, 4U)) - 1U;
    if (state->yield_mask == 0U) {
        state->yield_mask = 1U;
    }
}

static int run_multi_runtime_scenario(uint64_t seed, unsigned index) {
    fuzz_prng_t prng = {
        .state = seed ^ ((uint64_t)index * UINT64_C(0xd1b54a32d192ed03)),
    };
    llam_runtime_opts_t opts_a;
    llam_runtime_opts_t opts_b;
    llam_runtime_t *runtime_a = NULL;
    llam_runtime_t *runtime_b = NULL;
    multi_fuzz_state_t state_a;
    multi_fuzz_state_t state_b;
    llam_task_t *setup_a = NULL;
    llam_task_t *setup_b = NULL;
    llam_task_t *sender_a = NULL;
    llam_task_t *receiver_a = NULL;
    llam_task_t *cross_a = NULL;
    llam_task_t *sender_b = NULL;
    llam_task_t *receiver_b = NULL;
    llam_task_t *cross_b = NULL;
    bool deterministic_a = (fuzz_next(&prng) & 1U) != 0U;
    bool deterministic_b = (fuzz_next(&prng) & 1U) != 0U;
    int rc = 1;

    multi_fuzz_state_init(&state_a, &prng, 1U, MULTI_FUZZ_MAX_MESSAGES);
    multi_fuzz_state_init(&state_b, &prng, 1U, MULTI_FUZZ_MAX_MESSAGES);
    if (init_explicit_runtime_opts(&opts_a, deterministic_a) != 0 ||
        init_explicit_runtime_opts(&opts_b, deterministic_b) != 0) {
        return fail_errno("multi-runtime fuzz opts init failed");
    }
    if (llam_runtime_create(&opts_a, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime_a) != 0 ||
        llam_runtime_create(&opts_b, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime_b) != 0) {
        rc = fail_errno("multi-runtime fuzz create failed");
        goto cleanup;
    }

    setup_a = llam_runtime_spawn_ex(runtime_a, multi_channel_setup_task, &state_a, NULL, 0U);
    setup_b = llam_runtime_spawn_ex(runtime_b, multi_channel_setup_task, &state_b, NULL, 0U);
    if (setup_a == NULL || setup_b == NULL || multi_run_two_runtimes(runtime_a, runtime_b) != 0) {
        rc = fail_errno("multi-runtime fuzz setup run failed");
        goto cleanup;
    }
    if (join_task_checked(&setup_a, "multi-runtime fuzz setup A join failed") != 0 ||
        join_task_checked(&setup_b, "multi-runtime fuzz setup B join failed") != 0) {
        goto cleanup;
    }
    if (atomic_load_explicit(&state_a.failures, memory_order_relaxed) != 0U ||
        atomic_load_explicit(&state_b.failures, memory_order_relaxed) != 0U ||
        state_a.local_channel == NULL ||
        state_b.local_channel == NULL) {
        fprintf(stderr,
                "[test_runtime_fuzz] multi scenario=%u seed=%llu setup failed A=%s/%d B=%s/%d\n",
                index,
                (unsigned long long)seed,
                state_a.first_case,
                state_a.first_errno,
                state_b.first_case,
                state_b.first_errno);
        goto cleanup;
    }
    state_a.foreign_channel = state_b.local_channel;
    state_b.foreign_channel = state_a.local_channel;

    sender_a = llam_runtime_spawn_ex(runtime_a, multi_channel_sender_task, &state_a, NULL, 0U);
    receiver_a = llam_runtime_spawn_ex(runtime_a, multi_channel_receiver_task, &state_a, NULL, 0U);
    cross_a = llam_runtime_spawn_ex(runtime_a, multi_cross_channel_probe_task, &state_a, NULL, 0U);
    sender_b = llam_runtime_spawn_ex(runtime_b, multi_channel_sender_task, &state_b, NULL, 0U);
    receiver_b = llam_runtime_spawn_ex(runtime_b, multi_channel_receiver_task, &state_b, NULL, 0U);
    cross_b = llam_runtime_spawn_ex(runtime_b, multi_cross_channel_probe_task, &state_b, NULL, 0U);
    if (sender_a == NULL ||
        receiver_a == NULL ||
        cross_a == NULL ||
        sender_b == NULL ||
        receiver_b == NULL ||
        cross_b == NULL ||
        multi_run_two_runtimes(runtime_a, runtime_b) != 0) {
        rc = fail_errno("multi-runtime fuzz workload run failed");
        goto cleanup;
    }
    if (join_task_checked(&sender_a, "multi-runtime fuzz sender A join failed") != 0 ||
        join_task_checked(&receiver_a, "multi-runtime fuzz receiver A join failed") != 0 ||
        join_task_checked(&cross_a, "multi-runtime fuzz cross A join failed") != 0 ||
        join_task_checked(&sender_b, "multi-runtime fuzz sender B join failed") != 0 ||
        join_task_checked(&receiver_b, "multi-runtime fuzz receiver B join failed") != 0 ||
        join_task_checked(&cross_b, "multi-runtime fuzz cross B join failed") != 0) {
        goto cleanup;
    }
    if (atomic_load_explicit(&state_a.failures, memory_order_relaxed) != 0U ||
        atomic_load_explicit(&state_b.failures, memory_order_relaxed) != 0U ||
        atomic_load_explicit(&state_a.sent, memory_order_relaxed) != state_a.messages ||
        atomic_load_explicit(&state_a.received, memory_order_relaxed) != state_a.messages ||
        atomic_load_explicit(&state_b.sent, memory_order_relaxed) != state_b.messages ||
        atomic_load_explicit(&state_b.received, memory_order_relaxed) != state_b.messages ||
        atomic_load_explicit(&state_a.cross_checked, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state_b.cross_checked, memory_order_relaxed) != 1U) {
        fprintf(stderr,
                "[test_runtime_fuzz] multi scenario=%u seed=%llu failed "
                "A(sent=%u/%u recv=%u/%u cross=%u fail=%u %s/%d) "
                "B(sent=%u/%u recv=%u/%u cross=%u fail=%u %s/%d)\n",
                index,
                (unsigned long long)seed,
                atomic_load_explicit(&state_a.sent, memory_order_relaxed),
                state_a.messages,
                atomic_load_explicit(&state_a.received, memory_order_relaxed),
                state_a.messages,
                atomic_load_explicit(&state_a.cross_checked, memory_order_relaxed),
                atomic_load_explicit(&state_a.failures, memory_order_relaxed),
                state_a.first_case,
                state_a.first_errno,
                atomic_load_explicit(&state_b.sent, memory_order_relaxed),
                state_b.messages,
                atomic_load_explicit(&state_b.received, memory_order_relaxed),
                state_b.messages,
                atomic_load_explicit(&state_b.cross_checked, memory_order_relaxed),
                atomic_load_explicit(&state_b.failures, memory_order_relaxed),
                state_b.first_case,
                state_b.first_errno);
        goto cleanup;
    }
    rc = 0;

cleanup:
    if (setup_a != NULL) {
        (void)llam_detach(setup_a);
    }
    if (setup_b != NULL) {
        (void)llam_detach(setup_b);
    }
    if (sender_a != NULL) {
        (void)llam_detach(sender_a);
    }
    if (receiver_a != NULL) {
        (void)llam_detach(receiver_a);
    }
    if (cross_a != NULL) {
        (void)llam_detach(cross_a);
    }
    if (sender_b != NULL) {
        (void)llam_detach(sender_b);
    }
    if (receiver_b != NULL) {
        (void)llam_detach(receiver_b);
    }
    if (cross_b != NULL) {
        (void)llam_detach(cross_b);
    }
    if (state_a.local_channel != NULL) {
        (void)llam_channel_destroy(state_a.local_channel);
    }
    if (state_b.local_channel != NULL) {
        (void)llam_channel_destroy(state_b.local_channel);
    }
    llam_runtime_destroy(runtime_b);
    llam_runtime_destroy(runtime_a);
    return rc;
}

static int run_scenario(uint64_t seed, unsigned index) {
    fuzz_prng_t prng = {
        .state = seed ^ ((uint64_t)index * UINT64_C(0x9e3779b97f4a7c15)),
    };
    fuzz_state_t state;
    unsigned counter_count = fuzz_range(&prng, 1U, FUZZ_MAX_COUNTER_TASKS);
    unsigned pair_count = fuzz_range(&prng, 1U, FUZZ_MAX_CHANNEL_PAIRS);
    unsigned waiter_count = fuzz_range(&prng, 1U, FUZZ_MAX_CANCEL_WAITERS);

    memset(&state, 0, sizeof(state));
    state.expected_counters = counter_count;
    state.expected_cancel_waiters = waiter_count;
    atomic_init(&state.failures, 0U);
    atomic_init(&state.counters_done, 0U);
    atomic_init(&state.waiting_count, 0U);
    atomic_init(&state.canceled_count, 0U);

    state.cancel_token = llam_cancel_token_create();
    if (state.cancel_token == NULL) {
        return fail_errno("cancel token create failed");
    }
    for (unsigned i = 0U; i < pair_count; ++i) {
        unsigned capacity = fuzz_range(&prng, 1U, 8U);

        state.pairs[i].state = &state;
        state.pairs[i].messages = fuzz_range(&prng, 1U, 256U);
        atomic_init(&state.pairs[i].sent, 0U);
        atomic_init(&state.pairs[i].received, 0U);
        state.pairs[i].channel = llam_channel_create(capacity);
        if (state.pairs[i].channel == NULL) {
            cleanup_pairs(&state, pair_count);
            (void)llam_cancel_token_destroy(state.cancel_token);
            return fail_errno("channel create failed");
        }
    }
    if (init_runtime() != 0) {
        cleanup_pairs(&state, pair_count);
        (void)llam_cancel_token_destroy(state.cancel_token);
        return fail_errno("runtime init failed");
    }

    for (unsigned i = 0U; i < counter_count; ++i) {
        state.counters[i].state = &state;
        state.counters[i].yields = fuzz_range(&prng, 0U, 7U);
        if (spawn_detached(counter_task, &state.counters[i]) != 0) {
            llam_runtime_shutdown();
            cleanup_pairs(&state, pair_count);
            (void)llam_cancel_token_destroy(state.cancel_token);
            return fail_errno("spawn counter task failed");
        }
    }
    for (unsigned i = 0U; i < pair_count; ++i) {
        if (spawn_detached(channel_receiver_task, &state.pairs[i]) != 0 ||
            spawn_detached(channel_sender_task, &state.pairs[i]) != 0) {
            llam_runtime_shutdown();
            cleanup_pairs(&state, pair_count);
            (void)llam_cancel_token_destroy(state.cancel_token);
            return fail_errno("spawn channel tasks failed");
        }
    }
    for (unsigned i = 0U; i < waiter_count; ++i) {
        llam_spawn_opts_t opts;
        llam_task_t *task;

        if (llam_spawn_opts_init(&opts, LLAM_SPAWN_OPTS_CURRENT_SIZE) != 0) {
            llam_runtime_shutdown();
            cleanup_pairs(&state, pair_count);
            (void)llam_cancel_token_destroy(state.cancel_token);
            return fail_errno("spawn opts init failed");
        }
        opts.cancel_token = state.cancel_token;
        task = llam_spawn(cancel_waiter_task, &state, &opts);
        if (task == NULL || llam_detach(task) != 0) {
            llam_runtime_shutdown();
            cleanup_pairs(&state, pair_count);
            (void)llam_cancel_token_destroy(state.cancel_token);
            return fail_errno("spawn cancel waiter failed");
        }
    }
    if (spawn_detached(cancel_task, &state) != 0) {
        llam_runtime_shutdown();
        cleanup_pairs(&state, pair_count);
        (void)llam_cancel_token_destroy(state.cancel_token);
        return fail_errno("spawn cancel task failed");
    }
    if (llam_run() != 0) {
        llam_runtime_shutdown();
        cleanup_pairs(&state, pair_count);
        (void)llam_cancel_token_destroy(state.cancel_token);
        return fail_errno("llam_run failed");
    }
    if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
        fprintf(stderr,
                "[test_runtime_fuzz] scenario=%u seed=%llu failed at %s errno=%d (%s)\n",
                index,
                (unsigned long long)seed,
                state.first_case,
                state.first_errno,
                strerror(state.first_errno));
        llam_runtime_shutdown();
        cleanup_pairs(&state, pair_count);
        (void)llam_cancel_token_destroy(state.cancel_token);
        return 1;
    }
    if (atomic_load_explicit(&state.counters_done, memory_order_relaxed) != counter_count ||
        atomic_load_explicit(&state.canceled_count, memory_order_relaxed) != waiter_count) {
        llam_runtime_shutdown();
        cleanup_pairs(&state, pair_count);
        (void)llam_cancel_token_destroy(state.cancel_token);
        return fail_msg("scenario counter/cancel mismatch");
    }
    for (unsigned i = 0U; i < pair_count; ++i) {
        if (atomic_load_explicit(&state.pairs[i].sent, memory_order_relaxed) != state.pairs[i].messages ||
            atomic_load_explicit(&state.pairs[i].received, memory_order_relaxed) != state.pairs[i].messages) {
            llam_runtime_shutdown();
            cleanup_pairs(&state, pair_count);
            (void)llam_cancel_token_destroy(state.cancel_token);
            return fail_msg("scenario channel count mismatch");
        }
    }
    cleanup_pairs(&state, pair_count);
    if (llam_cancel_token_destroy(state.cancel_token) != 0) {
        llam_runtime_shutdown();
        return fail_errno("cancel token destroy failed");
    }
    llam_runtime_shutdown();
    return 0;
}

int main(void) {
    uint64_t seed = llam_test_env_u64("LLAM_RUNTIME_FUZZ_SEED", UINT64_C(0x4c4c414d46555a5a));
    unsigned scenarios =
        llam_test_env_u32("LLAM_RUNTIME_FUZZ_SCENARIOS", FUZZ_DEFAULT_SCENARIOS, FUZZ_MAX_SCENARIOS);
    uint64_t multi_seed =
        llam_test_env_u64("LLAM_MULTI_RUNTIME_FUZZ_SEED", seed ^ UINT64_C(0x6d756c7469727466));
    unsigned multi_scenarios = llam_test_env_u32("LLAM_MULTI_RUNTIME_FUZZ_SCENARIOS",
                                                 MULTI_FUZZ_DEFAULT_SCENARIOS,
                                                 MULTI_FUZZ_MAX_SCENARIOS);

    for (unsigned i = 0U; i < scenarios; ++i) {
        if (run_scenario(seed, i) != 0) {
            fprintf(stderr, "[test_runtime_fuzz] failing seed=%llu scenario=%u\n", (unsigned long long)seed, i);
            return 1;
        }
    }
    for (unsigned i = 0U; i < multi_scenarios; ++i) {
        if (run_multi_runtime_scenario(multi_seed, i) != 0) {
            fprintf(stderr,
                    "[test_runtime_fuzz] failing multi seed=%llu scenario=%u\n",
                    (unsigned long long)multi_seed,
                    i);
            return 1;
        }
    }
    printf("[test_runtime_fuzz] ok seed=%llu scenarios=%u multi_seed=%llu multi_scenarios=%u\n",
           (unsigned long long)seed,
           scenarios,
           (unsigned long long)multi_seed,
           multi_scenarios);
    return 0;
}
