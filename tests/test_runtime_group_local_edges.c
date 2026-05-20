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
#include <pthread.h>
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

#define TASK_LOCAL_REUSE_ROUNDS 70000U
#define TASK_GROUP_DESTROY_RACE_ROUNDS 4000U

typedef struct task_group_destroy_race_state {
    llam_task_group_t *group;
    atomic_uint go;
    int spawn_rc;
    int spawn_errno;
    int destroy_rc;
    int destroy_errno;
} task_group_destroy_race_state_t;

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

static void task_local_reuse_stale_value_task(void *arg) {
    group_local_state_t *state = arg;
    llam_task_local_key_t first_key = LLAM_TASK_LOCAL_INVALID_KEY;
    llam_task_local_key_t second_key = LLAM_TASK_LOCAL_INVALID_KEY;
    void *stale_value = (void *)(uintptr_t)0xA55A1234U;

    if (llam_task_local_key_create(&first_key) != 0 ||
        llam_task_local_set(first_key, stale_value) != 0 ||
        llam_task_local_key_delete(first_key) != 0 ||
        llam_task_local_key_create(&second_key) != 0) {
        task_fail(state, "task local stale reuse setup failed", errno);
        return;
    }
    /*
     * Deleted keys may still exist as entries on live tasks. A later key
     * allocation must not reuse that id until the live entry is cleared at task
     * exit, otherwise the new key can read the old value.
     */
    if (second_key == first_key) {
        task_fail(state, "task local key reused while stale entry was live", EINVAL);
        (void)llam_task_local_key_delete(second_key);
        return;
    }
    errno = 0;
    if (llam_task_local_get(second_key) != NULL || errno != 0) {
        task_fail(state, "task local stale value leaked into reused key", errno);
        (void)llam_task_local_key_delete(second_key);
        return;
    }
    if (llam_task_local_key_delete(second_key) != 0) {
        task_fail(state, "task local stale reuse cleanup failed", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void task_local_clear_deleted_key_task(void *arg) {
    group_local_state_t *state = arg;
    llam_task_local_key_t first_key = LLAM_TASK_LOCAL_INVALID_KEY;
    llam_task_local_key_t second_key = LLAM_TASK_LOCAL_INVALID_KEY;
    void *value = (void *)(uintptr_t)0xC1EA4U;

    if (llam_task_local_key_create(&first_key) != 0 ||
        llam_task_local_set(first_key, value) != 0 ||
        llam_task_local_key_delete(first_key) != 0) {
        task_fail(state, "task local clear deleted key setup failed", errno);
        return;
    }
    /*
     * Public docs allow deleted-key values to be discarded when a live task
     * sets the key to NULL. This must also release the stale-entry reference so
     * the deleted key can be reused without waiting for task exit.
     */
    if (llam_task_local_set(first_key, NULL) != 0) {
        task_fail(state, "task local clear after delete failed", errno);
        return;
    }
    if (llam_task_local_key_create(&second_key) != 0) {
        task_fail(state, "task local key reuse after clear failed", errno);
        return;
    }
    if (second_key != first_key) {
        task_fail(state, "task local clear did not release deleted key reference", EINVAL);
        (void)llam_task_local_key_delete(second_key);
        return;
    }
    if (llam_task_local_key_delete(second_key) != 0) {
        task_fail(state, "task local clear deleted key cleanup failed", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void group_counter_task(void *arg) {
    group_local_state_t *state = arg;

    llam_yield();
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void task_group_race_noop_task(void *arg) {
    (void)arg;
}

static void *task_group_spawn_race_thread(void *arg) {
    task_group_destroy_race_state_t *state = arg;

    while (atomic_load_explicit(&state->go, memory_order_acquire) == 0U) {
    }
    errno = 0;
    state->spawn_rc = llam_task_group_spawn(state->group, task_group_race_noop_task, NULL, NULL) != NULL ? 0 : -1;
    state->spawn_errno = errno;
    return NULL;
}

static void *task_group_destroy_race_thread(void *arg) {
    task_group_destroy_race_state_t *state = arg;

    while (atomic_load_explicit(&state->go, memory_order_acquire) == 0U) {
    }
    errno = 0;
    state->destroy_rc = llam_task_group_destroy(state->group);
    state->destroy_errno = errno;
    return NULL;
}

static int test_task_group_spawn_destroy_race(void) {
    for (unsigned i = 0U; i < TASK_GROUP_DESTROY_RACE_ROUNDS; ++i) {
        task_group_destroy_race_state_t state;
        pthread_t spawn_thread;
        pthread_t destroy_thread;

        memset(&state, 0, sizeof(state));
        atomic_init(&state.go, 0U);
        state.group = llam_task_group_create();
        if (state.group == NULL) {
            return fail_errno("task group create for destroy race failed");
        }
        if (pthread_create(&spawn_thread, NULL, task_group_spawn_race_thread, &state) != 0 ||
            pthread_create(&destroy_thread, NULL, task_group_destroy_race_thread, &state) != 0) {
            (void)llam_task_group_destroy(state.group);
            return fail_errno("task group destroy race thread create failed");
        }
        atomic_store_explicit(&state.go, 1U, memory_order_release);
        (void)pthread_join(spawn_thread, NULL);
        (void)pthread_join(destroy_thread, NULL);

        if (state.spawn_rc == 0) {
            (void)llam_task_group_join(state.group);
        } else if (state.spawn_errno != EINVAL && state.spawn_errno != EBUSY) {
            (void)llam_task_group_destroy(state.group);
            errno = state.spawn_errno;
            return fail_errno("task group spawn/destroy race produced unexpected spawn error");
        }

        if (state.destroy_rc == 0) {
            continue;
        }
        if (state.destroy_errno != EBUSY) {
            errno = state.destroy_errno;
            (void)llam_task_group_destroy(state.group);
            return fail_errno("task group spawn/destroy race produced unexpected destroy error");
        }
        if (llam_task_group_destroy(state.group) != 0) {
            return fail_errno("task group destroy race cleanup failed");
        }
    }
    return 0;
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

static void group_explicit_token_sleep_task(void *arg) {
    group_local_state_t *state = arg;

    errno = 0;
    if (llam_sleep_ns(5000000ULL) != 0) {
        task_fail(state, "group cancel incorrectly cancelled explicit token child", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
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

static int test_task_local_key_reuse_after_delete(void) {
    llam_task_local_key_t key = LLAM_TASK_LOCAL_INVALID_KEY;

    /*
     * Key deletion must release the public key id. Without reuse, embedders
     * that create/delete task-local keys across plugin reloads eventually hit
     * ENOSPC even though no live keys remain.
     */
    for (unsigned i = 0U; i < TASK_LOCAL_REUSE_ROUNDS; ++i) {
        if (llam_task_local_key_create(&key) != 0) {
            return fail_errno("task local key create after delete reuse failed");
        }
        if (llam_task_local_key_delete(key) != 0) {
            return fail_errno("task local key delete during reuse failed");
        }
    }
    return 0;
}

static int test_task_local_key_reuse_ignores_stale_entries(void) {
    group_local_state_t state;
    llam_task_t *task;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    if (init_runtime() != 0) {
        return fail_errno("runtime init for task local stale reuse failed");
    }
    task = llam_spawn(task_local_reuse_stale_value_task, &state, NULL);
    if (task == NULL) {
        goto cleanup_runtime;
    }
    if (llam_run() != 0 ||
        check_task_failures(&state) != 0 ||
        llam_join(task) != 0) {
        goto cleanup_runtime;
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U) {
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    llam_runtime_shutdown();
    if (rc != 0 && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U) {
        return fail_errno("task local stale reuse test failed");
    }
    return rc;
}

static int test_task_local_clear_after_key_delete(void) {
    group_local_state_t state;
    llam_task_t *task;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    if (init_runtime() != 0) {
        return fail_errno("runtime init for task local clear deleted key failed");
    }
    task = llam_spawn(task_local_clear_deleted_key_task, &state, NULL);
    if (task == NULL) {
        goto cleanup_runtime;
    }
    if (llam_run() != 0 ||
        check_task_failures(&state) != 0 ||
        llam_join(task) != 0) {
        goto cleanup_runtime;
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U) {
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    llam_runtime_shutdown();
    if (rc != 0 && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U) {
        return fail_errno("task local clear after key delete test failed");
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

static int test_task_group_cancel_preserves_explicit_token(void) {
    group_local_state_t state;
    llam_task_group_t *group;
    llam_cancel_token_t *token = NULL;
    llam_spawn_opts_t opts;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    group = llam_task_group_create();
    if (group == NULL) {
        return fail_errno("task group create for explicit token cancel failed");
    }
    token = llam_cancel_token_create();
    if (token == NULL) {
        goto cleanup_group;
    }
    if (llam_spawn_opts_init(&opts, LLAM_SPAWN_OPTS_CURRENT_SIZE) != 0) {
        goto cleanup_token;
    }
    opts.cancel_token = token;

    if (init_runtime() != 0) {
        goto cleanup_token;
    }
    if (llam_task_group_spawn_ex(group,
                                 group_explicit_token_sleep_task,
                                 &state,
                                 &opts,
                                 LLAM_SPAWN_OPTS_CURRENT_SIZE) == NULL) {
        goto cleanup_runtime;
    }
    /*
     * Group cancellation must not cancel a caller-owned token. Without this
     * contract, cancelling one nursery could unexpectedly cancel unrelated
     * tasks that share the same external token.
     */
    if (llam_task_group_cancel(group) != 0 ||
        llam_cancel_token_is_cancelled(token) != 0 ||
        llam_run() != 0 ||
        check_task_failures(&state) != 0 ||
        llam_task_group_join(group) != 0) {
        goto cleanup_runtime;
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U) {
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    llam_runtime_shutdown();
cleanup_token:
    if (token != NULL && llam_cancel_token_destroy(token) != 0) {
        rc = 1;
    }
cleanup_group:
    if (llam_task_group_destroy(group) != 0) {
        rc = 1;
    }
    if (rc != 0 && atomic_load_explicit(&state.failures, memory_order_relaxed) == 0U) {
        return fail_errno("task group explicit token cancel contract failed");
    }
    return rc;
}

int main(void) {
    if (test_task_local_isolation() != 0) {
        return 1;
    }
    if (test_task_local_key_reuse_after_delete() != 0) {
        return 1;
    }
    if (test_task_local_key_reuse_ignores_stale_entries() != 0) {
        return 1;
    }
    if (test_task_local_clear_after_key_delete() != 0) {
        return 1;
    }
    if (test_task_group_spawn_destroy_race() != 0) {
        return 1;
    }
    if (test_task_group_join_and_destroy() != 0) {
        return 1;
    }
    if (test_task_group_cancel() != 0) {
        return 1;
    }
    if (test_task_group_cancel_preserves_explicit_token() != 0) {
        return 1;
    }
    puts("test_runtime_group_local_edges ok");
    return 0;
}
