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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FUZZ_MAX_COUNTER_TASKS 256U
#define FUZZ_MAX_CHANNEL_PAIRS 16U
#define FUZZ_MAX_CANCEL_WAITERS 128U
#define FUZZ_DEFAULT_SCENARIOS 48U
#define FUZZ_MAX_SCENARIOS 512U

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

static int spawn_detached(llam_task_fn fn, void *arg) {
    llam_task_t *task = llam_spawn(fn, arg, NULL);

    if (task == NULL) {
        return -1;
    }
    return llam_detach(task);
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
            task_fail(pair->state, "channel send", errno);
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
            task_fail(pair->state, "channel recv payload", errno);
            return;
        }
        atomic_fetch_add_explicit(&pair->received, 1U, memory_order_relaxed);
    }
}

static void cancel_waiter_task(void *arg) {
    fuzz_state_t *state = arg;

    atomic_fetch_add_explicit(&state->waiting_count, 1U, memory_order_release);
    errno = 0;
    if (llam_sleep_ns(60ULL * 1000ULL * 1000ULL * 1000ULL) != -1 || errno != ECANCELED) {
        task_fail(state, "cancel waiter sleep", errno);
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
        task_fail(state, "cancel token cancel", errno);
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

static void cleanup_pairs(fuzz_state_t *state, unsigned pair_count) {
    for (unsigned i = 0U; i < pair_count; ++i) {
        if (state->pairs[i].channel != NULL) {
            (void)llam_channel_destroy(state->pairs[i].channel);
            state->pairs[i].channel = NULL;
        }
    }
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

    for (unsigned i = 0U; i < scenarios; ++i) {
        if (run_scenario(seed, i) != 0) {
            fprintf(stderr, "[test_runtime_fuzz] failing seed=%llu scenario=%u\n", (unsigned long long)seed, i);
            return 1;
        }
    }
    printf("[test_runtime_fuzz] ok seed=%llu scenarios=%u\n", (unsigned long long)seed, scenarios);
    return 0;
}
