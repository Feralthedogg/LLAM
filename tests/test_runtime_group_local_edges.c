/**
 * @file tests/test_runtime_group_local_edges.c
 * @brief Focused task-group and task-local edge tests.
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
#include <string.h>

typedef struct group_local_state {
    llam_task_local_key_t key;
    atomic_uint failures;
    atomic_uint ran;
    atomic_uint canceled;
    int first_errno;
    char first_case[128];
} group_local_state_t;

typedef struct local_task_args {
    group_local_state_t *state;
    uintptr_t value;
} local_task_args_t;

static int fail_errno(const char *message) {
    fprintf(stderr, "[test_runtime_group_local_edges] %s: errno=%d (%s)\n", message, errno, strerror(errno));
    return 1;
}

static void task_fail(group_local_state_t *state, const char *where, int err) {
    if (atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed) == 0U) {
        state->first_errno = err;
        (void)snprintf(state->first_case, sizeof(state->first_case), "%s", where);
    }
}

static int check_task_failures(group_local_state_t *state) {
    if (atomic_load_explicit(&state->failures, memory_order_relaxed) == 0U) {
        return 0;
    }
    fprintf(stderr,
            "[test_runtime_group_local_edges] task failed at %s errno=%d (%s)\n",
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
    return llam_runtime_init_ex(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE);
}

static void task_local_isolation_task(void *arg) {
    local_task_args_t *args = arg;
    group_local_state_t *state = args->state;
    void *value = (void *)args->value;

    errno = E2BIG;
    if (llam_task_local_get(state->key) != NULL || errno != 0) {
        task_fail(state, "initial task local was not empty", errno);
        return;
    }
    if (llam_task_local_set(state->key, value) != 0) {
        task_fail(state, "task local set failed", errno);
        return;
    }
    for (unsigned i = 0U; i < 8U; ++i) {
        llam_yield();
        if (llam_task_local_get(state->key) != value) {
            task_fail(state, "task local value was not isolated across yield", EINVAL);
            return;
        }
    }
    if (llam_task_local_set(state->key, NULL) != 0) {
        task_fail(state, "task local clear failed", errno);
        return;
    }
    errno = E2BIG;
    if (llam_task_local_get(state->key) != NULL || errno != 0) {
        task_fail(state, "task local clear failed", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void group_counter_task(void *arg) {
    group_local_state_t *state = arg;

    llam_yield();
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void group_canceled_sleep_task(void *arg) {
    group_local_state_t *state = arg;

    errno = 0;
    if (llam_sleep_ns(1000000000ULL) != -1 || errno != ECANCELED) {
        task_fail(state, "group cancel did not cancel sleep", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->canceled, 1U, memory_order_relaxed);
}

static int test_task_local_isolation(void) {
    group_local_state_t state;
    local_task_args_t a;
    local_task_args_t b;
    llam_task_t *task_a;
    llam_task_t *task_b;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    if (llam_task_local_key_create(&state.key) != 0) {
        return fail_errno("task local key create failed");
    }
    a.state = &state;
    a.value = 0xA11CEU;
    b.state = &state;
    b.value = 0xB0B00U;

    if (init_runtime() != 0) {
        goto cleanup_key;
    }
    task_a = llam_spawn(task_local_isolation_task, &a, NULL);
    task_b = llam_spawn(task_local_isolation_task, &b, NULL);
    if (task_a == NULL || task_b == NULL) {
        goto cleanup_runtime;
    }
    if (llam_run() != 0 ||
        check_task_failures(&state) != 0 ||
        llam_join(task_a) != 0 ||
        llam_join(task_b) != 0) {
        goto cleanup_runtime;
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 2U) {
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    llam_runtime_shutdown();
cleanup_key:
    if (llam_task_local_key_delete(state.key) != 0) {
        rc = 1;
    }
    if (rc != 0 && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U) {
        return fail_errno("task local isolation failed");
    }
    return rc;
}

static int test_task_group_join_and_destroy(void) {
    group_local_state_t state;
    llam_task_group_t *group;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    group = llam_task_group_create();
    if (group == NULL) {
        return fail_errno("task group create failed");
    }
    if (init_runtime() != 0) {
        goto cleanup_group;
    }
    for (unsigned i = 0U; i < 4U; ++i) {
        if (llam_task_group_spawn(group, group_counter_task, &state, NULL) == NULL) {
            goto cleanup_runtime;
        }
    }
    errno = 0;
    if (llam_task_group_destroy(group) != -1 || errno != EBUSY) {
        goto cleanup_runtime;
    }
    if (llam_task_group_join_until(group, 0U) != -1 || errno != ETIMEDOUT) {
        goto cleanup_runtime;
    }
    if (llam_run() != 0 ||
        check_task_failures(&state) != 0 ||
        llam_task_group_join(group) != 0) {
        goto cleanup_runtime;
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 4U) {
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    llam_runtime_shutdown();
cleanup_group:
    if (llam_task_group_destroy(group) != 0) {
        rc = 1;
    }
    if (rc != 0 && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U) {
        return fail_errno("task group join/destroy failed");
    }
    return rc;
}

static int test_task_group_cancel(void) {
    group_local_state_t state;
    llam_task_group_t *group;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.canceled, 0U);
    group = llam_task_group_create();
    if (group == NULL) {
        return fail_errno("task group create for cancel failed");
    }
    if (init_runtime() != 0) {
        goto cleanup_group;
    }
    for (unsigned i = 0U; i < 3U; ++i) {
        if (llam_task_group_spawn(group, group_canceled_sleep_task, &state, NULL) == NULL) {
            goto cleanup_runtime;
        }
    }
    if (llam_task_group_cancel(group) != 0 ||
        llam_run() != 0 ||
        check_task_failures(&state) != 0 ||
        llam_task_group_join(group) != 0) {
        goto cleanup_runtime;
    }
    if (atomic_load_explicit(&state.canceled, memory_order_relaxed) != 3U) {
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    llam_runtime_shutdown();
cleanup_group:
    if (llam_task_group_destroy(group) != 0) {
        rc = 1;
    }
    if (rc != 0 && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U) {
        return fail_errno("task group cancel failed");
    }
    return rc;
}

int main(void) {
    if (test_task_local_isolation() != 0) {
        return 1;
    }
    if (test_task_group_join_and_destroy() != 0) {
        return 1;
    }
    if (test_task_group_cancel() != 0) {
        return 1;
    }
    puts("test_runtime_group_local_edges ok");
    return 0;
}
