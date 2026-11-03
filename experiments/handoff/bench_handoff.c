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
#include <inttypes.h>

#define SAMPLE_STRIDE 16U
#define OBSERVER_CAP 65536U
#define MAX_PAIRS 16U

typedef struct bench_state bench_state_t;

typedef struct bench_pair {
    bench_state_t *state;
    llam_channel_t *request;
    llam_channel_t *ack;
    uint64_t sent_ns;
    uint64_t finish_ns;
    uint64_t checksum;
    uint64_t *samples;
    unsigned sequence;
    unsigned sample_count;
    unsigned max_streak;
} bench_pair_t;

struct bench_state {
    unsigned pairs;
    unsigned rounds;
    unsigned observer; /* 0 none, 1 runnable competitor, 2 50us timer */
    atomic_uint ready;
    atomic_uint done;
    atomic_bool go;
    atomic_bool failed;
    uint64_t begin_ns;
    uint64_t *observer_samples;
    unsigned observer_count;
    bench_pair_t pair[MAX_PAIRS];
};

static unsigned sample_offset(unsigned block) {
    /* Vary the phase within each block to avoid aliasing power-of-two burst limits. */
    uint32_t mixed = block;

    mixed ^= mixed >> 16U;
    mixed *= UINT32_C(0x7feb352d);
    mixed ^= mixed >> 15U;
    return mixed % SAMPLE_STRIDE;
}

static bool sample_event(unsigned sequence) {
    return sequence % SAMPLE_STRIDE == sample_offset(sequence / SAMPLE_STRIDE);
}

static void fail(bench_state_t *state) {
    atomic_store_explicit(&state->failed, true, memory_order_release);
    (void)llam_runtime_request_stop();
}

static void await_start(bench_state_t *state) {
    atomic_fetch_add_explicit(&state->ready, 1U, memory_order_release);
    while (!atomic_load_explicit(&state->go, memory_order_acquire)) {
        llam_yield();
    }
}

static void sender(void *arg) {
    bench_pair_t *pair = arg;
    bench_state_t *state = pair->state;

    await_start(state);
    for (unsigned i = 0U; i < state->rounds; ++i) {
        void *ack = NULL;

        pair->sequence = i;
        if (sample_event(i)) {
            pair->sent_ns = llam_now_ns();
        }
        if (llam_channel_send(pair->request, pair) != 0 ||
            llam_channel_recv_result(pair->ack, &ack) != 0 || ack != pair) {
            fail(state);
            return;
        }
    }
    pair->finish_ns = llam_now_ns();
    atomic_fetch_add_explicit(&state->done, 1U, memory_order_release);
}

static void receiver(void *arg) {
    bench_pair_t *pair = arg;
    bench_state_t *state = pair->state;

    await_start(state);
    for (unsigned i = 0U; i < state->rounds; ++i) {
        void *message = NULL;

        if (llam_channel_recv_result(pair->request, &message) != 0 ||
            message != pair || pair->sequence != i) {
            fail(state);
            return;
        }
        if (sample_event(i)) {
            pair->samples[pair->sample_count++] = llam_now_ns() - pair->sent_ns;
        }
        pair->checksum += i + 1U;
        if (g_llam_tls_processor->direct_handoff_streak > pair->max_streak) {
            pair->max_streak = g_llam_tls_processor->direct_handoff_streak;
        }
        /* Ack publishes that all accesses to the sender-owned payload are finished. */
        if (llam_channel_send(pair->ack, pair) != 0) {
            fail(state);
            return;
        }
    }
}

static void observer(void *arg) {
    bench_state_t *state = arg;
    unsigned iteration = 0U;

    await_start(state);
    while (atomic_load_explicit(&state->done, memory_order_acquire) != state->pairs) {
        uint64_t begin = llam_now_ns();
        uint64_t elapsed;

        if (state->observer == 2U) {
            llam_sleep_ns(50000U);
        } else {
            llam_yield();
        }
        elapsed = llam_now_ns() - begin;
        if (!sample_event(iteration++)) {
            continue;
        }
        if (state->observer_count == OBSERVER_CAP) {
            fail(state); /* Never silently overwrite the experiment's event record. */
            return;
        }
        state->observer_samples[state->observer_count++] =
            state->observer == 2U && elapsed > 50000U ? elapsed - 50000U :
            state->observer == 2U ? 0U : elapsed;
    }
}

static void controller(void *arg) {
    bench_state_t *state = arg;
    llam_task_t *tasks[MAX_PAIRS * 2U + 1U];
    llam_spawn_opts_t options;
    unsigned count = 0U;

    if (llam_spawn_opts_init(&options, sizeof(options)) != 0) {
        fail(state);
        return;
    }
    options.flags = LLAM_SPAWN_F_PINNED;
    for (unsigned i = 0U; i < state->pairs; ++i) {
        bench_pair_t *pair = &state->pair[i];

        pair->request = llam_channel_create(1U);
        pair->ack = llam_channel_create(1U);
        if (pair->request == NULL || pair->ack == NULL) {
            fail(state);
            return;
        }
        tasks[count++] = llam_spawn_ex(receiver, pair, &options, sizeof(options));
        tasks[count++] = llam_spawn_ex(sender, pair, &options, sizeof(options));
    }
    if (state->observer != 0U) {
        tasks[count++] = llam_spawn_ex(observer, state, &options, sizeof(options));
    }
    for (unsigned i = 0U; i < count; ++i) {
        if (tasks[i] == NULL) {
            fail(state);
            return;
        }
    }
    while (atomic_load_explicit(&state->ready, memory_order_acquire) != count) {
        llam_yield();
    }
    state->begin_ns = llam_now_ns();
    atomic_store_explicit(&state->go, true, memory_order_release);
    for (unsigned i = 0U; i < count; ++i) {
        if (llam_join(tasks[i]) != 0) {
            fail(state);
            return;
        }
    }
}

static int compare_u64(const void *left, const void *right) {
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;

    return (a > b) - (a < b);
}

static uint64_t p99(uint64_t *samples, size_t count) {
    if (count == 0U) {
        return 0U;
    }
    qsort(samples, count, sizeof(*samples), compare_u64);
    return samples[(count * 99U + 99U) / 100U - 1U];
}

static unsigned number(const char *text, unsigned max) {
    char *end;
    unsigned long value;

    errno = 0;
    value = strtoul(text, &end, 10);
    return errno != 0 || text[0] < '0' || text[0] > '9' || *end != '\0' || value > max
               ? UINT_MAX : (unsigned)value;
}

static void print_samples(const uint64_t *samples, size_t count) {
    putchar('[');
    for (size_t i = 0U; i < count; ++i) {
        printf("%s%" PRIu64, i == 0U ? "" : ",", samples[i]);
    }
    putchar(']');
}

int main(int argc, char **argv) {
    bench_state_t state = {0};
    llam_runtime_opts_t options;
    llam_runtime_t *runtime = NULL;
    llam_runtime_stats_t stats;
    llam_task_t *root;
    uint64_t end = 0U;
    uint64_t checksum = 0U;
    uint64_t *latencies = NULL;
    size_t sample_count = 0U;
    size_t samples_per_pair;
    unsigned max_streak = 0U;
    int result = 1;

    if (argc != 4) {
        fprintf(stderr, "usage: %s pairs rounds observer(0|1|2)\n", argv[0]);
        return 2;
    }
    state.pairs = number(argv[1], MAX_PAIRS);
    state.rounds = number(argv[2], 1000000U);
    state.observer = number(argv[3], 2U);
    if (state.pairs == 0U || state.pairs == UINT_MAX || state.rounds == 0U ||
        state.rounds == UINT_MAX || state.observer == UINT_MAX) {
        return 2;
    }
    atomic_init(&state.ready, 0U);
    atomic_init(&state.done, 0U);
    atomic_init(&state.go, false);
    atomic_init(&state.failed, false);
    samples_per_pair = state.rounds / SAMPLE_STRIDE +
        (sample_offset(state.rounds / SAMPLE_STRIDE) < state.rounds % SAMPLE_STRIDE);
    latencies = calloc(state.pairs * samples_per_pair, sizeof(*latencies));
    state.observer_samples = calloc(OBSERVER_CAP, sizeof(*state.observer_samples));
    if (latencies == NULL || state.observer_samples == NULL) {
        goto cleanup;
    }
    for (unsigned i = 0U; i < state.pairs; ++i) {
        state.pair[i].state = &state;
        state.pair[i].samples = latencies + i * samples_per_pair;
    }
    if (llam_runtime_opts_init(&options, sizeof(options)) != 0) {
        goto cleanup;
    }
    options.deterministic = 1U; /* Isolate same-P wake/handoff; native races are a separate test. */
    options.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    options.experimental_flags = LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    if (llam_runtime_create(&options, sizeof(options), &runtime) != 0) {
        goto cleanup;
    }
    root = llam_runtime_spawn_ex(runtime, controller, &state, NULL, 0U);
    if (root == NULL || llam_runtime_run_handle(runtime) != 0 || llam_join(root) != 0 ||
        atomic_load_explicit(&state.failed, memory_order_acquire) ||
        atomic_load_explicit(&state.done, memory_order_acquire) != state.pairs) {
        goto cleanup;
    }
    for (unsigned i = 0U; i < state.pairs; ++i) {
        bench_pair_t *pair = &state.pair[i];
        uint64_t expected = (uint64_t)state.rounds * (state.rounds + 1U) / 2U;

        if (pair->checksum != expected || pair->sample_count != samples_per_pair) {
            goto cleanup;
        }
        checksum += pair->checksum;
        sample_count += pair->sample_count;
        if (pair->finish_ns > end) {
            end = pair->finish_ns;
        }
        if (pair->max_streak > max_streak) {
            max_streak = pair->max_streak;
        }
    }
    memset(&stats, 0, sizeof(stats));
    if (end <= state.begin_ns ||
        llam_runtime_collect_stats_ex_handle(runtime, &stats, sizeof(stats)) != 0) {
        goto cleanup;
    }
    printf("{\"messages\":%u,\"elapsed_ns\":%" PRIu64 ",\"checksum\":%" PRIu64
           ",\"workers\":%u,\"burst\":%u,\"max_observed_streak\":%u,"
           "\"wake_handoff_hits\":%" PRIu64 ",\"wake_handoff_attempts\":%" PRIu64
           ",\"p99_ns\":%" PRIu64 ",\"observer_p99_ns\":%" PRIu64 ",\"latencies_ns\":",
           state.pairs * state.rounds, end - state.begin_ns, checksum, stats.active_workers,
           llam_runtime_direct_handoff_budget(runtime), max_streak,
           stats.wake_handoff_hits, stats.wake_handoff_attempts,
           p99(latencies, sample_count), p99(state.observer_samples, state.observer_count));
    print_samples(latencies, sample_count);
    fputs(",\"observer_latencies_ns\":", stdout);
    print_samples(state.observer_samples, state.observer_count);
    puts("}");
    result = 0;
cleanup:
    if (result != 0) {
        fprintf(stderr, "handoff benchmark failed: errno=%d\n", errno);
    }
    if (runtime != NULL) {
        for (unsigned i = 0U; i < state.pairs; ++i) {
            if (state.pair[i].request != NULL) {
                (void)llam_channel_destroy(state.pair[i].request);
            }
            if (state.pair[i].ack != NULL) {
                (void)llam_channel_destroy(state.pair[i].ack);
            }
        }
        llam_runtime_destroy(runtime);
    }
    free(state.observer_samples);
    free(latencies);
    return result;
}
