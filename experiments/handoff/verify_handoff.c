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
 * See LICENSES/Apache-2.0.txt.
 */

#include "runtime_internal.h"

#if LLAM_PLATFORM_POSIX
#include <sched.h>

#define HANDSHAKE_TRIALS 3000U

typedef struct handshake {
    llam_task_t task;
    llam_wait_node_t node;
    atomic_uint epoch;
    atomic_uint done;
    atomic_bool stop;
    bool results[3];
} handshake_t;

typedef struct actor {
    handshake_t *state;
    unsigned index;
} actor_t;

static void reset_node(handshake_t *state) {
    llam_wait_node_reset(&state->node, NULL, UINT_MAX);
    state->node.task = &state->task;
    atomic_store_explicit(&state->task.state, LLAM_TASK_STATE_RUNNING, memory_order_relaxed);
    memset(state->results, 0, sizeof(state->results));
}

static void act(handshake_t *state, unsigned index) {
    state->results[index] = index == 0U
                               ? llam_wait_node_should_park(&state->node)
                               : llam_wait_node_prepare_wake(&state->node);
}

static bool valid(const handshake_t *state) {
    unsigned wakes = (unsigned)state->results[1] + (unsigned)state->results[2];

    return wakes <= 1U && state->results[0] == (wakes == 1U) &&
           atomic_load_explicit(&state->node.wake_completed, memory_order_acquire) == 1U;
}

static void *race_actor(void *arg) {
    actor_t *actor = arg;
    handshake_t *state = actor->state;

    for (unsigned trial = 1U; trial <= HANDSHAKE_TRIALS; ++trial) {
        while (atomic_load_explicit(&state->epoch, memory_order_acquire) != trial) {
            if (atomic_load_explicit(&state->stop, memory_order_acquire)) {
                return NULL;
            }
            sched_yield();
        }
        act(state, actor->index);
        /* Release publishes this actor's result; main acquires the completed round. */
        atomic_fetch_add_explicit(&state->done, 1U, memory_order_acq_rel);
    }
    return NULL;
}

int main(void) {
    static const unsigned orders[6][3] = {
        {0U, 1U, 2U}, {0U, 2U, 1U}, {1U, 0U, 2U},
        {1U, 2U, 0U}, {2U, 0U, 1U}, {2U, 1U, 0U}
    };
    handshake_t *state = calloc(1U, sizeof(*state));
    pthread_t threads[3];
    actor_t actors[3];
    unsigned created = 0U;
    int result = 1;

    if (state == NULL) {
        return 1;
    }
    atomic_init(&state->epoch, 0U);
    atomic_init(&state->done, 0U);
    atomic_init(&state->stop, false);
    atomic_init(&state->task.state, LLAM_TASK_STATE_RUNNING);
    atomic_init(&state->node.wake_armed, 0U);
    atomic_init(&state->node.wake_completed, 0U);
    atomic_init(&state->node.wake_queued, 0U);
    for (unsigned order = 0U; order < 6U; ++order) {
        reset_node(state);
        for (unsigned step = 0U; step < 3U; ++step) {
            act(state, orders[order][step]);
        }
        if (!valid(state)) {
            fprintf(stderr, "wake protocol replay failed at permutation %u\n", order);
            goto cleanup;
        }
    }
    for (unsigned i = 0U; i < 3U; ++i) {
        actors[i] = (actor_t){state, i};
        if (pthread_create(&threads[i], NULL, race_actor, &actors[i]) != 0) {
            goto cleanup;
        }
        ++created;
    }
    for (unsigned trial = 1U; trial <= HANDSHAKE_TRIALS; ++trial) {
        reset_node(state);
        atomic_store_explicit(&state->done, 0U, memory_order_relaxed);
        /* Previous done acquire drains users before reset; release starts the new epoch. */
        atomic_store_explicit(&state->epoch, trial, memory_order_release);
        while (atomic_load_explicit(&state->done, memory_order_acquire) != 3U) {
            sched_yield();
        }
        if (!valid(state)) {
            fprintf(stderr, "wake protocol concurrent trial %u failed\n", trial);
            goto cleanup;
        }
    }
    printf("{\"permutations\":6,\"concurrent_trials\":%u,\"passed\":true}\n",
           HANDSHAKE_TRIALS);
    result = 0;
cleanup:
    atomic_store_explicit(&state->stop, true, memory_order_release);
    for (unsigned i = 0U; i < created; ++i) {
        if (pthread_join(threads[i], NULL) != 0) {
            abort(); /* Thread lifetime must end before freeing shared state. */
        }
    }
    free(state);
    return result;
}
#else
int main(void) {
    fputs("wake protocol pthread experiment requires POSIX\n", stderr);
    return 77;
}
#endif
