// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Feralthedogg

/**
 * @file tests/test_asan_fiber_positive.c
 * @brief Positive control for AddressSanitizer's LLAM fiber-stack tracking.
 */

#include "runtime_internal.h"

#include <stdio.h>

static volatile size_t g_bad_fiber_index = 32U;

static void overflow_fiber_local(void *arg) {
    volatile unsigned char fiber_local[32] = {0U};
    volatile unsigned char *escaped =
        (volatile unsigned char *)((uintptr_t)&fiber_local[0] +
                                   g_bad_fiber_index);

    (void)arg;
    *escaped = 0x4cU;
}

int main(void) {
    llam_runtime_opts_t opts;
    llam_runtime_t *runtime = NULL;
    llam_task_t *task;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return 2;
    }
    opts.worker_min = 1U;
    opts.worker_count = 1U;
    opts.worker_max = 1U;
    opts.blocking_min = 0U;
    opts.blocking_max = 1U;
    if (llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime) != 0) {
        return 2;
    }
    task = llam_runtime_spawn_ex(runtime, overflow_fiber_local, NULL, NULL, 0U);
    if (task == NULL || llam_runtime_run_handle(runtime) != 0) {
        llam_runtime_destroy(runtime);
        return 2;
    }

    (void)llam_detach(task);
    llam_runtime_destroy(runtime);
    fputs("ASan fiber positive control did not report\n", stderr);
    return 2;
}
