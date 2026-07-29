/**
 * @file tests/test_tsan_fiber_positive.c
 * @brief Positive control for ThreadSanitizer's LLAM logical-fiber tracking.
 */

#include "runtime_internal.h"

#include <stdio.h>

typedef struct tsan_fiber_positive_state {
    atomic_uint ready;
    atomic_uint finished;
    volatile unsigned raced_value;
} tsan_fiber_positive_state_t;

typedef struct tsan_fiber_positive_arg {
    tsan_fiber_positive_state_t *state;
    unsigned value;
} tsan_fiber_positive_arg_t;

static void race_from_distinct_fiber(void *opaque) {
    tsan_fiber_positive_arg_t *arg = opaque;
    tsan_fiber_positive_state_t *state = arg->state;
    uint64_t deadline_ns = llam_now_ns() + UINT64_C(5000000000);

    atomic_fetch_add_explicit(&state->ready, 1U, memory_order_acq_rel);
    while (atomic_load_explicit(&state->ready, memory_order_acquire) != 2U) {
        if (llam_now_ns() >= deadline_ns) {
            abort();
        }
    }
    state->raced_value = arg->value;
    atomic_fetch_add_explicit(&state->finished, 1U, memory_order_release);
}

int main(void) {
    tsan_fiber_positive_state_t state;
    tsan_fiber_positive_arg_t args[2];
    llam_runtime_opts_t opts;
    llam_spawn_opts_t spawn_opts;
    llam_runtime_t *runtime = NULL;
    llam_task_t *tasks[2] = {NULL, NULL};
    unsigned i;

    atomic_init(&state.ready, 0U);
    atomic_init(&state.finished, 0U);
    state.raced_value = 0U;
    for (i = 0U; i < 2U; ++i) {
        args[i].state = &state;
        args[i].value = i + 1U;
    }

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0 ||
        llam_spawn_opts_init(&spawn_opts, LLAM_SPAWN_OPTS_CURRENT_SIZE) != 0) {
        return 2;
    }
    opts.worker_min = 2U;
    opts.worker_count = 2U;
    opts.worker_max = 2U;
    opts.blocking_min = 0U;
    opts.blocking_max = 1U;
    spawn_opts.flags = LLAM_SPAWN_F_PINNED;
    if (llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime) != 0) {
        return 2;
    }
    for (i = 0U; i < 2U; ++i) {
        tasks[i] = llam_runtime_spawn_ex(runtime,
                                         race_from_distinct_fiber,
                                         &args[i],
                                         &spawn_opts,
                                         LLAM_SPAWN_OPTS_CURRENT_SIZE);
        if (tasks[i] == NULL) {
            llam_runtime_destroy(runtime);
            return 2;
        }
    }
    if (llam_runtime_run_handle(runtime) != 0 ||
        atomic_load_explicit(&state.finished, memory_order_acquire) != 2U) {
        llam_runtime_destroy(runtime);
        return 2;
    }

    for (i = 0U; i < 2U; ++i) {
        (void)llam_detach(tasks[i]);
    }
    llam_runtime_destroy(runtime);
    fputs("TSan fiber positive control did not report\n", stderr);
    return 2;
}
