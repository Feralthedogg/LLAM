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
#if LLAM_PLATFORM_POSIX
#include <sched.h>
#define CHECK(condition)                                                                                     \
    do {                                                                                                     \
        if (!(condition)) {                                                                                  \
            fprintf(stderr, "large select: %s at %d (errno=%d)\n", #condition, __LINE__, errno);             \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)

enum { CLOSE_ONLY, CANCEL_ONLY, BOTH };
typedef struct race race_t;
typedef struct waiter_arg {
    race_t *race;
    unsigned id;
} waiter_arg_t;
struct race {
    llam_channel_t **channels;
    llam_cancel_token_t *tokens[2];
    _Atomic(llam_task_t *) identities[2];
    atomic_uint begun;
    atomic_uint actors_ready;
    atomic_uint finished;
    unsigned count;
    unsigned unique;
    unsigned mode;
    bool wait_for_park;
};
static void waiter(void *arg) {
    waiter_arg_t *waiter = arg;
    race_t *race = waiter->race;
    llam_select_op_t *ops = calloc(race->count, sizeof(*ops));
    void *received = (void *)(uintptr_t)0xBADU;
    size_t selected = SIZE_MAX;
    CHECK(ops != NULL);
    for (unsigned i = 0; i < race->count; ++i) {
        unsigned order = waiter->id == 0U ? i : race->count - 1U - i;
        ops[i].kind = LLAM_SELECT_OP_RECV;
        ops[i].channel = race->channels[order * 4051U % race->unique];
        ops[i].recv_out = &received;
    }
    atomic_store_explicit(&race->identities[waiter->id], g_llam_tls_task, memory_order_release);
    atomic_fetch_add_explicit(&race->begun, 1U, memory_order_release);
    int rc = llam_channel_select(ops, race->count, UINT64_MAX, &selected);
    int error = errno;
    if (rc == 0) {
        CHECK(race->mode != CANCEL_ONLY);
        CHECK(selected < race->count);
        CHECK(ops[selected].channel == race->channels[race->unique / 2U]);
        CHECK(ops[selected].result_errno == EPIPE && received == NULL);
        for (unsigned i = 0; i < race->count; ++i) {
            if (i != selected)
                CHECK(ops[i].result_errno == 0);
        }
    } else {
        CHECK(race->mode != CLOSE_ONLY && error == ECANCELED);
        CHECK(selected == SIZE_MAX && received == (void *)(uintptr_t)0xBADU);
    }
    free(ops);
    atomic_fetch_add_explicit(&race->finished, 1U, memory_order_release);
}
static bool parked(race_t *race) {
    for (unsigned i = 0; i < 2; ++i) {
        llam_task_t *task = atomic_load_explicit(&race->identities[i], memory_order_acquire);
        if (task == NULL ||
            atomic_load_explicit(&task->state, memory_order_acquire) != LLAM_TASK_STATE_PARKED ||
            atomic_load_explicit(&task->active_select_state, memory_order_acquire) == NULL)
            return false;
    }
    return true;
}
static void await_waiters(race_t *race) {
    uint64_t deadline = llam_now_ns() + UINT64_C(10000000000);
    while (atomic_load_explicit(&race->begun, memory_order_acquire) != 2U ||
           (race->wait_for_park && !parked(race))) {
        CHECK(llam_now_ns() < deadline);
        sched_yield();
    }
}
static void await_actor(race_t *race) {
    atomic_fetch_add_explicit(&race->actors_ready, 1U, memory_order_acq_rel);
    while (atomic_load_explicit(&race->actors_ready, memory_order_acquire) != 2U)
        sched_yield();
}
static void *closer(void *arg) {
    race_t *race = arg;
    await_waiters(race);
    if (race->wait_for_park) {
        /* Neither actor has fired yet: both joins retain task storage and every
         * channel has waiters/pins. A destroy attempt must fail without effects. */
        CHECK(llam_channel_destroy(race->channels[0]) == -1 && errno == EBUSY);
    }
    await_actor(race);
    if (race->mode != CANCEL_ONLY)
        CHECK(llam_channel_close(race->channels[race->unique / 2U]) == 0);
    return NULL;
}
static void *canceller(void *arg) {
    race_t *race = arg;
    await_waiters(race);
    await_actor(race);
    if (race->mode != CLOSE_ONLY) {
        for (unsigned i = 0; i < 2; ++i)
            CHECK(llam_cancel_token_cancel(race->tokens[i]) == 0);
    }
    return NULL;
}
static void run_race(unsigned count, unsigned unique, unsigned mode, bool wait_for_park) {
    race_t race = {0};
    waiter_arg_t args[2];
    llam_task_t *tasks[2];
    pthread_t close_thread, cancel_thread;
    race.count = count;
    race.unique = unique;
    race.mode = mode;
    race.wait_for_park = wait_for_park;
    atomic_init(&race.begun, 0U);
    atomic_init(&race.actors_ready, 0U);
    atomic_init(&race.finished, 0U);
    race.channels = calloc(unique, sizeof(*race.channels));
    CHECK(race.channels != NULL);
    for (unsigned i = 0; i < unique; ++i) {
        race.channels[i] = llam_channel_create(1);
        CHECK(race.channels[i]);
    }
    for (unsigned i = 0; i < 2; ++i) {
        atomic_init(&race.identities[i], NULL);
        race.tokens[i] = llam_cancel_token_create();
        CHECK(race.tokens[i]);
        llam_spawn_opts_t options;
        CHECK(llam_spawn_opts_init(&options, sizeof(options)) == 0);
        options.cancel_token = race.tokens[i];
        args[i] = (waiter_arg_t){&race, i};
        tasks[i] = llam_spawn_ex(waiter, &args[i], &options, sizeof(options));
        CHECK(tasks[i]);
    }
    CHECK(pthread_create(&close_thread, NULL, closer, &race) == 0);
    CHECK(pthread_create(&cancel_thread, NULL, canceller, &race) == 0);
    CHECK(llam_run() == 0);
    CHECK(pthread_join(close_thread, NULL) == 0 && pthread_join(cancel_thread, NULL) == 0);
    CHECK(atomic_load_explicit(&race.finished, memory_order_acquire) == 2U);
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(llam_join(tasks[i]) == 0);
        CHECK(llam_cancel_token_destroy(race.tokens[i]) == 0);
    }
    for (unsigned i = 0; i < unique; ++i)
        CHECK(llam_channel_destroy(race.channels[i]) == 0);
    free(race.channels);
}

typedef struct boundary {
    llam_channel_t *live;
    llam_channel_t *tail;
    unsigned count;
    bool timeout;
} boundary_t;
static void boundary_task(void *arg) {
    boundary_t *state = arg;
    llam_select_op_t *ops = calloc(state->count, sizeof(*ops));
    void *received = NULL;
    size_t selected = SIZE_MAX;
    CHECK(ops);
    for (unsigned i = 0; i < state->count; ++i) {
        ops[i].kind = LLAM_SELECT_OP_RECV;
        ops[i].channel = state->live;
        ops[i].recv_out = &received;
    }
    if (!state->timeout)
        ops[state->count - 1U].channel = state->tail;
    uint64_t deadline = state->timeout ? llam_now_ns() + UINT64_C(2000000) : UINT64_MAX;
    CHECK(llam_channel_select(ops, state->count, deadline, &selected) == -1);
    CHECK(errno == (state->timeout ? ETIMEDOUT : EINVAL));
    CHECK(selected == SIZE_MAX && received == NULL);
    free(ops);
}
static void run_boundary(unsigned count, bool timeout) {
    boundary_t state = {0};
    state.count = count;
    state.timeout = timeout;
    state.live = llam_channel_create(1);
    CHECK(state.live);
    if (!timeout) {
        state.tail = llam_channel_create(1);
        CHECK(state.tail);
        CHECK(llam_channel_destroy(state.tail) == 0);
        CHECK(llam_channel_try_send(state.live, (void *)(uintptr_t)0x1234U) == 0);
    }
    llam_task_t *task = llam_spawn(boundary_task, &state, NULL);
    CHECK(task);
    CHECK(llam_run() == 0 && llam_join(task) == 0);
    if (!timeout) {
        void *value = NULL;
        CHECK(llam_channel_try_recv_result(state.live, &value) == 0 && value == (void *)(uintptr_t)0x1234U);
    }
    CHECK(llam_channel_destroy(state.live) == 0);
}
int main(void) {
    static const unsigned widths[] = {2U, 8U, 9U, 64U, 65U, 512U, 4096U};
    llam_runtime_opts_t options;
    unsigned shapes = 0, cases = 0;
    CHECK(llam_runtime_opts_init(&options, sizeof(options)) == 0);
    options.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    options.experimental_flags = LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    CHECK(llam_runtime_init_compat_ex(&options, sizeof(options)) == 0);
    for (unsigned index = 0; index < sizeof(widths) / sizeof(*widths); ++index) {
        unsigned count = widths[index];
        unsigned unique_counts[] = {count, 3U, 8U, 9U, 64U, 65U};
        for (unsigned variant = 0; variant < sizeof(unique_counts) / sizeof(*unique_counts); ++variant) {
            unsigned unique = unique_counts[variant];
            if (variant != 0 && unique >= count)
                continue;
            ++shapes;
            run_race(count, unique, CLOSE_ONLY, true);
            ++cases;
            run_race(count, unique, CANCEL_ONLY, true);
            ++cases;
            for (unsigned repeat = 0; repeat < 4; ++repeat) {
                run_race(count, unique, BOTH, false);
                ++cases;
                run_race(count, unique, BOTH, true);
                ++cases;
            }
        }
        run_boundary(count, false);
        run_boundary(count, true);
    }
    llam_runtime_shutdown();
    printf("test_large_select: %u shapes, %u two-waiter close/cancel cases, 14 stale/timeout cases passed\n",
           shapes, cases);
    return 0;
}
#else
int main(void) {
    return 77;
}
#endif
