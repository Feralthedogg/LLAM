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

typedef struct bench_state {
    llam_channel_t **channels;
    llam_select_op_t *ops;
    size_t *first_index;
    uint64_t *samples;
    llam_task_t *waiter_identity;
    atomic_uint request;
    unsigned count;
    unsigned unique;
    unsigned iterations;
    unsigned warmup;
    bool ready;
    uint64_t elapsed;
    uint64_t checksum;
    uint64_t parks;
} bench_state_t;

static void fail(const char *what) {
    fprintf(stderr, "bench_select_prepare: %s (errno=%d)\n", what, errno);
    exit(1);
}
static unsigned number(const char *text, unsigned maximum) {
    char *end;
    unsigned long value;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno || *text < '0' || *text > '9' || *end || value == 0 || value > maximum) {
        fail("invalid numeric argument");
    }
    return (unsigned)value;
}
static void sender(void *arg) {
    bench_state_t *state = arg;
    unsigned total = state->warmup + state->iterations;
    for (unsigned i = 0; i < total; ++i) {
        while (atomic_load_explicit(&state->request, memory_order_acquire) != i + 1U ||
               atomic_load_explicit(&state->waiter_identity->state, memory_order_acquire) !=
                   LLAM_TASK_STATE_PARKED) {
            llam_yield();
        }
        unsigned channel = i % state->unique;
        if (llam_channel_send(state->channels[channel], (void *)(uintptr_t)(i + 1U)) != 0) {
            fail("sender");
        }
    }
}
static void controller(void *arg) {
    bench_state_t *state = arg;
    llam_task_t *producer = NULL;
    void *received = NULL;
    uint64_t begin = 0, parks_before = 0;
    state->waiter_identity = g_llam_tls_task;
    for (unsigned i = 0; i < state->unique; ++i) {
        state->channels[i] = llam_channel_create(1U);
        if (state->channels[i] == NULL)
            fail("channel creation");
        state->first_index[i] = SIZE_MAX;
    }
    for (unsigned i = 0; i < state->count; ++i) {
        unsigned channel = i * 4051U % state->unique;
        state->ops[i].kind = LLAM_SELECT_OP_RECV;
        state->ops[i].channel = state->channels[channel];
        state->ops[i].recv_out = &received;
        if (state->first_index[channel] == SIZE_MAX)
            state->first_index[channel] = i;
    }
    for (unsigned i = 0; i < state->unique; ++i) {
        if (state->first_index[i] == SIZE_MAX)
            fail("channel permutation not complete");
    }
    if (!state->ready) {
        producer = llam_spawn(sender, state, NULL);
        if (producer == NULL)
            fail("sender spawn");
    }
    for (unsigned i = 0; i < state->warmup + state->iterations; ++i) {
        size_t selected = SIZE_MAX;
        unsigned channel = i % state->unique;
        if (i == state->warmup) {
            parks_before = atomic_load_explicit(&g_llam_tls_shard->metrics.parks, memory_order_relaxed);
            begin = llam_now_ns();
        }
        received = NULL;
        if (state->ready) {
            if (llam_channel_try_send(state->channels[channel], (void *)(uintptr_t)(i + 1U)) != 0)
                fail("ready setup");
        } else {
            atomic_store_explicit(&state->request, i + 1U, memory_order_release);
        }
        uint64_t start = llam_now_ns();
        if (llam_channel_select(state->ops, state->count, state->ready ? 0U : UINT64_MAX, &selected) != 0)
            fail("select");
        uint64_t elapsed = llam_now_ns() - start;
        if (selected != state->first_index[channel] || received != (void *)(uintptr_t)(i + 1U) ||
            state->ops[selected].result_errno != 0)
            fail("exact delivery/index");
        if (i >= state->warmup) {
            state->samples[i - state->warmup] = elapsed;
            state->checksum += (uintptr_t)received;
        }
    }
    state->elapsed = llam_now_ns() - begin;
    state->parks =
        atomic_load_explicit(&g_llam_tls_shard->metrics.parks, memory_order_relaxed) - parks_before;
    if (!state->ready && state->parks < state->iterations)
        fail("select preparation was bypassed");
    if (producer != NULL && llam_join(producer) != 0)
        fail("sender join");
    for (unsigned i = 0; i < state->unique; ++i) {
        if (llam_channel_destroy(state->channels[i]) != 0)
            fail("channel pins or waiters leaked");
    }
}
static int compare_u64(const void *left, const void *right) {
    uint64_t a = *(const uint64_t *)left, b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}
int main(int argc, char **argv) {
    bench_state_t state = {0};
    llam_runtime_opts_t options;
    llam_runtime_t *runtime = NULL;
    if (argc != 5 || (strcmp(argv[1], "ready") && strcmp(argv[1], "park"))) {
        fprintf(stderr, "usage: %s ready|park operations unique_channels iterations\n", argv[0]);
        return 2;
    }
    state.ready = strcmp(argv[1], "ready") == 0;
    state.count = number(argv[2], LLAM_CHANNEL_SELECT_MAX_OPS);
    state.unique = number(argv[3], state.count);
    state.iterations = number(argv[4], 2000000U);
    state.warmup = 32768U / state.count;
    if (state.warmup < 16U)
        state.warmup = 16U;
    if (state.warmup > 2048U)
        state.warmup = 2048U;
    atomic_init(&state.request, 0U);
    state.channels = calloc(state.unique, sizeof(*state.channels));
    state.first_index = calloc(state.unique, sizeof(*state.first_index));
    state.ops = calloc(state.count, sizeof(*state.ops));
    state.samples = calloc(state.iterations, sizeof(*state.samples));
    if (!state.channels || !state.first_index || !state.ops || !state.samples)
        fail("allocation");
    if (llam_runtime_opts_init(&options, sizeof(options)))
        fail("options");
    options.deterministic = 1U;
    options.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    options.experimental_flags = LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    if (llam_runtime_create_compat(&options, sizeof(options), &runtime))
        fail("runtime creation");
    llam_task_t *root = llam_runtime_spawn_ex(runtime, controller, &state, NULL, 0U);
    if (root == NULL || llam_runtime_run_handle(runtime) != 0 || llam_join(root) != 0)
        fail("runtime execution");
    qsort(state.samples, state.iterations, sizeof(*state.samples), compare_u64);
    printf("{\"mode\":\"%s\",\"operations\":%u,\"channels\":%u,\"iterations\":%u,"
           "\"warmup\":%u,\"workers\":%u,\"elapsed_ns\":%" PRIu64 ",\"parks\":%" PRIu64
           ",\"checksum\":%" PRIu64 ",\"p99_ns\":%" PRIu64 ",\"latencies_ns\":[",
           argv[1], state.count, state.unique, state.iterations, state.warmup, runtime->active_shards,
           state.elapsed, state.parks, state.checksum,
           state.samples[((size_t)state.iterations * 99U + 99U) / 100U - 1U]);
    for (unsigned i = 0; i < state.iterations; ++i)
        printf("%s%" PRIu64, i ? "," : "", state.samples[i]);
    puts("]}");
    llam_runtime_destroy(runtime);
    free(state.channels);
    free(state.first_index);
    free(state.ops);
    free(state.samples);
    return 0;
}
