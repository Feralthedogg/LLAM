/**
 * @file tests/test_runtime_select_edges.c
 * @brief Focused channel select lost-wakeup and cancellation race tests.
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
#include <stdlib.h>
#include <string.h>

#define SELECT_RACE_DEFAULT_ROUNDS 16U
#define SELECT_RACE_MAX_ROUNDS 65536U

enum {
    SELECT_RACE_SEND = 1U,
    SELECT_RACE_CLOSE = 2U,
    SELECT_RACE_CANCEL = 3U,
};

typedef struct select_state {
    atomic_uint failures;
    atomic_uint waiting;
    atomic_uint completed;
    int first_errno;
    char first_case[160];
    llam_channel_t *primary;
    llam_channel_t *secondary;
    llam_cancel_token_t *token;
    unsigned mode;
} select_state_t;

typedef struct select_stale_second_state {
    atomic_uint failures;
    int first_errno;
    char first_case[160];
    llam_channel_t *ready;
    llam_channel_t *stale;
    void *received;
    size_t selected;
    int select_rc;
    int select_errno;
} select_stale_second_state_t;

static int fail_errno(const char *message) {
    fprintf(stderr, "[test_runtime_select_edges] %s: errno=%d (%s)\n", message, errno, strerror(errno));
    return 1;
}

static void task_fail(select_state_t *state, const char *where, int err) {
    if (atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed) == 0U) {
        state->first_errno = err;
        (void)snprintf(state->first_case, sizeof(state->first_case), "%s", where);
    }
}

static void stale_second_task_fail(select_stale_second_state_t *state, const char *where, int err) {
    if (atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed) == 0U) {
        state->first_errno = err;
        (void)snprintf(state->first_case, sizeof(state->first_case), "%s", where);
    }
}

static int check_task_failures(select_state_t *state) {
    if (atomic_load_explicit(&state->failures, memory_order_relaxed) == 0U) {
        return 0;
    }
    fprintf(stderr,
            "[test_runtime_select_edges] task failed at %s errno=%d (%s)\n",
            state->first_case,
            state->first_errno,
            strerror(state->first_errno));
    return 1;
}

static int check_stale_second_task_failures(select_stale_second_state_t *state) {
    if (atomic_load_explicit(&state->failures, memory_order_relaxed) == 0U) {
        return 0;
    }
    fprintf(stderr,
            "[test_runtime_select_edges] task failed at %s errno=%d (%s)\n",
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

/**
 * @brief Read the select-race intensity knob used by CI and local repro runs.
 *
 * Invalid values fall back to the fast default so a malformed environment does
 * not accidentally disable the edge test.
 */
static unsigned select_race_rounds_from_env(void) {
    const char *value = getenv("LLAM_SELECT_RACE_ROUNDS");
    char *end = NULL;
    unsigned long parsed;

    if (value == NULL || value[0] == '\0') {
        return SELECT_RACE_DEFAULT_ROUNDS;
    }
    if (value[0] == ' ' || value[0] == '\t' || value[0] == '\n' ||
        value[0] == '\r' || value[0] == '\f' || value[0] == '\v' ||
        value[0] == '-' || value[0] == '+') {
        return SELECT_RACE_DEFAULT_ROUNDS;
    }

    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0UL) {
        return SELECT_RACE_DEFAULT_ROUNDS;
    }
    if (parsed > SELECT_RACE_MAX_ROUNDS) {
        return SELECT_RACE_MAX_ROUNDS;
    }
    return (unsigned)parsed;
}

static void select_race_waiter_task(void *arg) {
    select_state_t *state = arg;
    void *received = (void *)(uintptr_t)0xBAD0C105U;
    size_t selected = SIZE_MAX;
    llam_select_op_t ops[2];

    memset(ops, 0, sizeof(ops));
    ops[0].kind = LLAM_SELECT_OP_RECV;
    ops[0].channel = state->primary;
    ops[0].recv_out = &received;
    ops[1].kind = LLAM_SELECT_OP_RECV;
    ops[1].channel = state->secondary;
    ops[1].recv_out = &received;

    atomic_store_explicit(&state->waiting, 1U, memory_order_release);
    errno = 0;
    if (llam_channel_select(ops, 2U, UINT64_MAX, &selected) != 0) {
        if (state->mode == SELECT_RACE_CANCEL && errno == ECANCELED) {
            atomic_fetch_add_explicit(&state->completed, 1U, memory_order_relaxed);
            return;
        }
        task_fail(state, "select race unexpected errno", errno);
        return;
    }

    if (state->mode == SELECT_RACE_SEND) {
        if (selected != 1U || received != (void *)(uintptr_t)0x5E1EC7U) {
            task_fail(state, "select race send selected wrong op", EINVAL);
            return;
        }
    } else if (state->mode == SELECT_RACE_CLOSE) {
        if (selected != 1U || ops[1].result_errno != EPIPE || received != NULL) {
            task_fail(state, "select race close selected wrong op", EINVAL);
            return;
        }
    } else {
        task_fail(state, "select race cancel unexpectedly selected", EINVAL);
        return;
    }
    atomic_fetch_add_explicit(&state->completed, 1U, memory_order_relaxed);
}

static void select_race_trigger_task(void *arg) {
    select_state_t *state = arg;
    unsigned i;

    while (atomic_load_explicit(&state->waiting, memory_order_acquire) == 0U) {
        llam_yield();
    }
    for (i = 0U; i < 4U; ++i) {
        llam_yield();
    }

    if (state->mode == SELECT_RACE_SEND) {
        if (llam_channel_send(state->secondary, (void *)(uintptr_t)0x5E1EC7U) != 0) {
            task_fail(state, "select race trigger send", errno);
        }
    } else if (state->mode == SELECT_RACE_CLOSE) {
        if (llam_channel_close(state->secondary) != 0) {
            task_fail(state, "select race trigger close", errno);
        }
    } else if (state->token != NULL) {
        if (llam_cancel_token_cancel(state->token) != 0) {
            task_fail(state, "select race trigger cancel", errno);
        }
    }
}

static int run_select_race_once(unsigned mode) {
    select_state_t state;
    llam_spawn_opts_t wait_opts;
    llam_task_t *waiter = NULL;
    llam_task_t *trigger = NULL;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.waiting, 0U);
    atomic_init(&state.completed, 0U);
    state.mode = mode;
    state.primary = llam_channel_create(1U);
    state.secondary = llam_channel_create(1U);
    if (state.primary == NULL || state.secondary == NULL) {
        goto cleanup;
    }
    if (mode == SELECT_RACE_CANCEL) {
        state.token = llam_cancel_token_create();
        if (state.token == NULL) {
            goto cleanup;
        }
    }
    if (llam_spawn_opts_init(&wait_opts, LLAM_SPAWN_OPTS_CURRENT_SIZE) != 0) {
        goto cleanup;
    }
    wait_opts.cancel_token = state.token;
    waiter = llam_spawn(select_race_waiter_task, &state, &wait_opts);
    trigger = llam_spawn(select_race_trigger_task, &state, NULL);
    if (waiter == NULL || trigger == NULL) {
        goto cleanup;
    }
    if (llam_run() != 0 || check_task_failures(&state) != 0) {
        goto cleanup;
    }
    if (llam_join(waiter) != 0) {
        waiter = NULL;
        goto cleanup;
    }
    waiter = NULL;
    if (llam_join(trigger) != 0) {
        trigger = NULL;
        goto cleanup;
    }
    trigger = NULL;
    if (atomic_load_explicit(&state.completed, memory_order_relaxed) != 1U) {
        goto cleanup;
    }
    rc = 0;

cleanup:
    if (rc != 0) {
        if (state.primary != NULL) {
            (void)llam_channel_close(state.primary);
        }
        if (state.secondary != NULL) {
            (void)llam_channel_close(state.secondary);
        }
        if (state.token != NULL) {
            (void)llam_cancel_token_cancel(state.token);
        }
        (void)llam_runtime_request_stop();
        (void)llam_run();
    }
    if (waiter != NULL) {
        (void)llam_join(waiter);
    }
    if (trigger != NULL) {
        (void)llam_join(trigger);
    }
    if (state.token != NULL && llam_cancel_token_destroy(state.token) != 0) {
        rc = 1;
    }
    if (state.primary != NULL && llam_channel_destroy(state.primary) != 0) {
        rc = 1;
    }
    if (state.secondary != NULL && llam_channel_destroy(state.secondary) != 0) {
        rc = 1;
    }
    if (rc != 0 && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U) {
        return fail_errno("select race failed");
    }
    return rc;
}

static void select_stale_second_ready_first_task(void *arg) {
    select_stale_second_state_t *state = arg;
    llam_select_op_t ops[2];

    memset(ops, 0, sizeof(ops));
    ops[0].kind = LLAM_SELECT_OP_RECV;
    ops[0].channel = state->ready;
    ops[0].recv_out = &state->received;
    ops[1].kind = LLAM_SELECT_OP_RECV;
    ops[1].channel = state->stale;
    ops[1].recv_out = &state->received;

    state->selected = SIZE_MAX;
    errno = 0;
    state->select_rc = llam_channel_select(ops, 2U, 0U, &state->selected);
    state->select_errno = errno;

    /*
     * The two-op fast path must validate every public handle before touching
     * channel state.  A stale second handle therefore rejects the whole call
     * and must not consume the ready value from op 0.
     */
    if (state->select_rc != -1 || state->select_errno != EINVAL ||
        state->selected != SIZE_MAX || state->received != NULL) {
        stale_second_task_fail(state, "stale second handle was not rejected atomically", EINVAL);
    }
}

static int run_select_stale_second_ready_first(void) {
    select_stale_second_state_t state;
    llam_task_t *task = NULL;
    void *value = NULL;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    state.selected = SIZE_MAX;
    state.ready = llam_channel_create(1U);
    state.stale = llam_channel_create(1U);
    if (state.ready == NULL || state.stale == NULL) {
        goto cleanup;
    }
    if (llam_channel_try_send(state.ready, (void *)(uintptr_t)0xC0FFEEU) != 0) {
        goto cleanup;
    }
    if (llam_channel_destroy(state.stale) != 0) {
        goto cleanup;
    }

    task = llam_spawn(select_stale_second_ready_first_task, &state, NULL);
    if (task == NULL) {
        goto cleanup;
    }
    if (llam_run() != 0 || check_stale_second_task_failures(&state) != 0) {
        goto cleanup;
    }
    if (llam_join(task) != 0) {
        task = NULL;
        goto cleanup;
    }
    task = NULL;
    if (llam_channel_try_recv_result(state.ready, &value) != 0 || value != (void *)(uintptr_t)0xC0FFEEU) {
        goto cleanup;
    }
    rc = 0;

cleanup:
    if (rc != 0) {
        if (state.ready != NULL) {
            (void)llam_channel_close(state.ready);
        }
        (void)llam_runtime_request_stop();
        (void)llam_run();
    }
    if (task != NULL) {
        (void)llam_join(task);
    }
    if (state.ready != NULL && llam_channel_destroy(state.ready) != 0) {
        rc = 1;
    }
    if (rc != 0 && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U) {
        return fail_errno("select stale second ready first failed");
    }
    return rc;
}

int main(void) {
    unsigned rounds = select_race_rounds_from_env();
    unsigned i;

    if (init_runtime() != 0) {
        return fail_errno("runtime init for select races failed");
    }
    if (run_select_stale_second_ready_first() != 0) {
        llam_runtime_shutdown();
        return 1;
    }
    for (i = 0U; i < rounds; ++i) {
        if (run_select_race_once(SELECT_RACE_SEND) != 0 ||
            run_select_race_once(SELECT_RACE_CLOSE) != 0 ||
            run_select_race_once(SELECT_RACE_CANCEL) != 0) {
            llam_runtime_shutdown();
            return 1;
        }
    }
    llam_runtime_shutdown();
    printf("test_runtime_select_edges ok: rounds=%u\n", rounds);
    return 0;
}
