/**
 * @file tests/test_runtime_unmanaged_join.c
 * @brief Focused tests for unmanaged OS-thread task join boundaries.
 *
 * This test is intentionally small so it remains useful under ThreadSanitizer:
 * an unmanaged OS thread joins a task while another OS thread drives
 * ::llam_run, exercising the public boundary that does not drive scheduler
 * progress itself.
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
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct repro_state {
    llam_task_t *task;
    atomic_uint ready;
    atomic_uint go;
    atomic_uint ran;
    int rc[2];
    int err[2];
} repro_state_t;

typedef struct joiner_args {
    repro_state_t *state;
    unsigned index;
} joiner_args_t;

static void task_main(void *arg) {
    repro_state_t *state = arg;

    for (unsigned i = 0U; i < 64U; ++i) {
        llam_yield();
    }
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void gated_task_main(void *arg) {
    repro_state_t *state = arg;

    while (atomic_load_explicit(&state->go, memory_order_acquire) == 0U) {
        llam_yield();
    }
    task_main(arg);
}

static void *run_thread_main(void *arg) {
    (void)arg;
    return (void *)(intptr_t)llam_run();
}

static void *joiner_thread_main(void *arg) {
    joiner_args_t *args = arg;
    repro_state_t *state = args->state;
    unsigned index = args->index;

    atomic_fetch_add_explicit(&state->ready, 1U, memory_order_release);
    while (atomic_load_explicit(&state->go, memory_order_acquire) == 0U) {
    }
    errno = 0;
    state->rc[index] = llam_join(state->task);
    state->err[index] = errno;
    return NULL;
}

static int init_runtime(void) {
    llam_runtime_opts_t opts;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        fprintf(stderr, "[test_runtime_unmanaged_join] opts init failed: %s\n", strerror(errno));
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    opts.experimental_flags = LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    if (llam_runtime_init(&opts) != 0) {
        fprintf(stderr, "[test_runtime_unmanaged_join] runtime init failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

static int test_single_unmanaged_join(void) {
    repro_state_t state;
    llam_task_t *task;
    pthread_t run_thread;
    void *run_result = NULL;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.ran, 0U);
    if (init_runtime() != 0) {
        return 1;
    }
    task = llam_spawn(task_main, &state, NULL);
    if (task == NULL) {
        fprintf(stderr, "[test_runtime_unmanaged_join] spawn failed: %s\n", strerror(errno));
        llam_runtime_shutdown();
        return 1;
    }
    if (pthread_create(&run_thread, NULL, run_thread_main, NULL) != 0) {
        fprintf(stderr, "[test_runtime_unmanaged_join] pthread_create failed\n");
        llam_runtime_shutdown();
        return 1;
    }
    if (llam_join(task) != 0) {
        fprintf(stderr, "[test_runtime_unmanaged_join] unmanaged join failed: %s\n", strerror(errno));
        (void)pthread_join(run_thread, &run_result);
        llam_runtime_shutdown();
        return 1;
    }
    if (pthread_join(run_thread, &run_result) != 0 || (intptr_t)run_result != 0) {
        fprintf(stderr, "[test_runtime_unmanaged_join] run thread failed\n");
        llam_runtime_shutdown();
        return 1;
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U) {
        fprintf(stderr, "[test_runtime_unmanaged_join] task did not run once\n");
        llam_runtime_shutdown();
        return 1;
    }
    llam_runtime_shutdown();
    return 0;
}

static int test_double_unmanaged_join_contract(void) {
    repro_state_t state;
    joiner_args_t args[2];
    pthread_t run_thread;
    pthread_t joiners[2];
    void *run_result = NULL;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.ready, 0U);
    atomic_init(&state.go, 0U);
    atomic_init(&state.ran, 0U);
    state.rc[0] = 999;
    state.rc[1] = 999;

    if (init_runtime() != 0) {
        return 1;
    }
    state.task = llam_spawn(gated_task_main, &state, NULL);
    if (state.task == NULL) {
        fprintf(stderr, "[test_runtime_unmanaged_join] double join spawn failed: %s\n", strerror(errno));
        llam_runtime_shutdown();
        return 1;
    }
    if (pthread_create(&run_thread, NULL, run_thread_main, NULL) != 0) {
        fprintf(stderr, "[test_runtime_unmanaged_join] run pthread_create failed\n");
        llam_runtime_shutdown();
        return 1;
    }

    /*
     * Two unmanaged OS threads contend for the same task handle.  The handle
     * must be consumed exactly once; the loser should fail before the winner
     * can reclaim the task object.
     */
    args[0].state = &state;
    args[0].index = 0U;
    args[1].state = &state;
    args[1].index = 1U;
    if (pthread_create(&joiners[0], NULL, joiner_thread_main, &args[0]) != 0 ||
        pthread_create(&joiners[1], NULL, joiner_thread_main, &args[1]) != 0) {
        fprintf(stderr, "[test_runtime_unmanaged_join] join pthread_create failed\n");
        llam_runtime_shutdown();
        return 1;
    }
    while (atomic_load_explicit(&state.ready, memory_order_acquire) != 2U) {
    }
    atomic_store_explicit(&state.go, 1U, memory_order_release);

    if (pthread_join(joiners[0], NULL) != 0 || pthread_join(joiners[1], NULL) != 0) {
        fprintf(stderr, "[test_runtime_unmanaged_join] joiner pthread_join failed\n");
        llam_runtime_shutdown();
        return 1;
    }
    if (pthread_join(run_thread, &run_result) != 0 || (intptr_t)run_result != 0) {
        fprintf(stderr, "[test_runtime_unmanaged_join] run thread failed in double join test\n");
        llam_runtime_shutdown();
        return 1;
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U) {
        fprintf(stderr, "[test_runtime_unmanaged_join] double join task did not run once\n");
        llam_runtime_shutdown();
        return 1;
    }
    if (!((state.rc[0] == 0 && state.rc[1] == -1 && state.err[1] == EBUSY) ||
          (state.rc[1] == 0 && state.rc[0] == -1 && state.err[0] == EBUSY))) {
        fprintf(stderr,
                "[test_runtime_unmanaged_join] expected one successful join and one EBUSY, got rc0=%d err0=%d rc1=%d err1=%d\n",
                state.rc[0],
                state.err[0],
                state.rc[1],
                state.err[1]);
        llam_runtime_shutdown();
        return 1;
    }

    llam_runtime_shutdown();
    return 0;
}

int main(void) {
    if (test_single_unmanaged_join() != 0) {
        return 1;
    }
    if (test_double_unmanaged_join_contract() != 0) {
        return 1;
    }
    return 0;
}
