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
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

typedef struct task_local_delete_race_state {
    group_local_state_t core;
    atomic_uint ready;
    atomic_uint delete_done;
    int delete_rc;
    int delete_errno;
} task_local_delete_race_state_t;

typedef struct task_group_destroy_race_state {
    llam_task_group_t *group;
    atomic_uint go;
    int spawn_rc;
    int spawn_errno;
    int destroy_rc;
    int destroy_errno;
} task_group_destroy_race_state_t;

typedef struct task_group_spawn_mark_race_state {
    atomic_uint keepalive_started;
    atomic_uint stop_keepalive;
    atomic_uint child_ran;
    atomic_uint child_detach_succeeded;
    atomic_uint unexpected_child_errno;
    atomic_uint failures;
    int first_errno;
    char first_case[128];
} task_group_spawn_mark_race_state_t;

typedef struct task_group_run_thread_state {
    int rc;
    int err;
} task_group_run_thread_state_t;

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

static void *task_group_run_thread(void *arg) {
    task_group_run_thread_state_t *state = arg;

    state->rc = llam_run();
    state->err = errno;
    return NULL;
}

static void task_group_spawn_mark_fail(task_group_spawn_mark_race_state_t *state, const char *where, int err) {
    if (atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed) == 0U) {
        state->first_errno = err;
        (void)snprintf(state->first_case, sizeof(state->first_case), "%s", where);
    }
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

static void *task_local_delete_race_thread(void *arg) {
    task_local_delete_race_state_t *state = arg;

    while (atomic_load_explicit(&state->ready, memory_order_acquire) == 0U) {
    }
    errno = 0;
    state->delete_rc = llam_task_local_key_delete(state->core.key);
    state->delete_errno = errno;
    atomic_store_explicit(&state->delete_done, 1U, memory_order_release);
    return NULL;
}

static void task_local_delete_race_task(void *arg) {
    task_local_delete_race_state_t *state = arg;
    void *value = (void *)(uintptr_t)0xD1EC7U;

    if (llam_task_local_set(state->core.key, value) != 0) {
        task_fail(&state->core, "task local delete race setup set failed", errno);
        atomic_store_explicit(&state->ready, 1U, memory_order_release);
        return;
    }
    atomic_store_explicit(&state->ready, 1U, memory_order_release);

    while (atomic_load_explicit(&state->delete_done, memory_order_acquire) == 0U) {
        errno = 0;
        if (llam_task_local_get(state->core.key) != value || errno != 0) {
            if (errno != EINVAL) {
                task_fail(&state->core, "task local delete race get failed before delete completed", errno);
            }
            break;
        }
        errno = 0;
        if (llam_task_local_set(state->core.key, value) != 0 && errno != EINVAL) {
            task_fail(&state->core, "task local delete race set failed before delete completed", errno);
            break;
        }
        llam_yield();
    }

    while (atomic_load_explicit(&state->delete_done, memory_order_acquire) == 0U) {
        llam_yield();
    }

    /*
     * Deleted keys may keep stale entries on live tasks until explicit clear or
     * task exit. Public get/set must still fail closed once deletion completes.
     */
    errno = 0;
    if (llam_task_local_get(state->core.key) != NULL || errno != EINVAL) {
        task_fail(&state->core, "task local get observed value after key delete", errno);
        return;
    }
    errno = 0;
    if (llam_task_local_set(state->core.key, value) != -1 || errno != EINVAL) {
        task_fail(&state->core, "task local set accepted value after key delete", errno);
        return;
    }
    if (llam_task_local_set(state->core.key, NULL) != 0) {
        task_fail(&state->core, "task local clear after racing delete failed", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->core.ran, 1U, memory_order_relaxed);
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

static int test_task_local_key_create_enospc_clears_output(void) {
    enum {
        expected_key_budget = 65535U,
        key_cap = 70000U
    };
    llam_task_local_key_t *keys;
    llam_task_local_key_t stale_key = (llam_task_local_key_t)0x12345678U;
    unsigned created = 0U;
    int rc = 1;

    keys = calloc(key_cap, sizeof(*keys));
    if (keys == NULL) {
        return fail_errno("task local ENOSPC fixture allocation failed");
    }
    for (created = 0U; created < key_cap; ++created) {
        keys[created] = LLAM_TASK_LOCAL_INVALID_KEY;
        errno = 0;
        if (llam_task_local_key_create(&keys[created]) != 0) {
            if (errno != ENOSPC || keys[created] != LLAM_TASK_LOCAL_INVALID_KEY) {
                rc = fail_errno("task local key exhaustion failed unexpectedly");
                goto cleanup;
            }
            break;
        }
    }

    /*
     * Exhaustion must fail closed.  FFI callers commonly reuse output storage;
     * leaving a previous key there can make cleanup code delete the wrong key.
     */
    if (created == key_cap) {
        rc = fail_errno("task local key exhaustion did not reach ENOSPC");
        goto cleanup;
    }
    if (created != expected_key_budget) {
        errno = EINVAL;
        rc = fail_errno("task local key exhaustion happened before documented key budget");
        goto cleanup;
    }
    errno = 0;
    if (llam_task_local_key_create(&stale_key) != -1 ||
        errno != ENOSPC ||
        stale_key != LLAM_TASK_LOCAL_INVALID_KEY) {
        rc = fail_errno("task local ENOSPC did not clear output key");
        goto cleanup;
    }
    rc = 0;

cleanup:
    while (created > 0U) {
        --created;
        if (keys[created] != LLAM_TASK_LOCAL_INVALID_KEY &&
            llam_task_local_key_delete(keys[created]) != 0) {
            rc = 1;
        }
    }
    free(keys);
    return rc;
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

static int test_task_local_delete_race_invalidates_live_entry(void) {
    task_local_delete_race_state_t state;
    pthread_t delete_thread;
    llam_task_t *task;
    bool thread_started = false;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.core.failures, 0U);
    atomic_init(&state.core.ran, 0U);
    atomic_init(&state.ready, 0U);
    atomic_init(&state.delete_done, 0U);
    state.delete_rc = -1;
    state.core.key = LLAM_TASK_LOCAL_INVALID_KEY;
    if (llam_task_local_key_create(&state.core.key) != 0) {
        return fail_errno("task local delete race key create failed");
    }
    if (init_runtime() != 0) {
        goto cleanup_key;
    }
    task = llam_spawn(task_local_delete_race_task, &state, NULL);
    if (task == NULL) {
        goto cleanup_runtime;
    }
    if (pthread_create(&delete_thread, NULL, task_local_delete_race_thread, &state) != 0) {
        goto cleanup_runtime;
    }
    thread_started = true;

    if (llam_run() != 0 ||
        check_task_failures(&state.core) != 0 ||
        llam_join(task) != 0) {
        goto cleanup_runtime;
    }
    if (thread_started) {
        (void)pthread_join(delete_thread, NULL);
        thread_started = false;
    }
    if (state.delete_rc != 0 || state.delete_errno != 0) {
        errno = state.delete_errno;
        goto cleanup_runtime;
    }
    if (atomic_load_explicit(&state.core.ran, memory_order_relaxed) != 1U) {
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    if (thread_started) {
        atomic_store_explicit(&state.ready, 1U, memory_order_release);
        (void)pthread_join(delete_thread, NULL);
    }
    llam_runtime_shutdown();
cleanup_key:
    if (state.delete_rc != 0 && state.core.key != LLAM_TASK_LOCAL_INVALID_KEY) {
        (void)llam_task_local_key_delete(state.core.key);
    }
    if (rc != 0 && atomic_load_explicit(&state.core.failures, memory_order_relaxed) == 0U) {
        return fail_errno("task local delete race invalidation failed");
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

static int test_task_group_borrowed_child_handle_not_consumable(void) {
    group_local_state_t state;
    llam_task_group_t *group;
    llam_task_t *borrowed;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);

    group = llam_task_group_create();
    if (group == NULL) {
        return fail_errno("task group create for borrowed child guard failed");
    }
    if (init_runtime() != 0) {
        goto cleanup_group;
    }
    borrowed = llam_task_group_spawn(group, group_counter_task, &state, NULL);
    if (borrowed == NULL) {
        goto cleanup_runtime;
    }
    if (llam_run() != 0 || check_task_failures(&state) != 0) {
        goto cleanup_runtime;
    }

    /*
     * Group spawns return a borrowed diagnostics handle.  If regular join/detach
     * can consume it, the group keeps a stale child handle and can no longer join
     * or destroy cleanly.  Treat that as a public lifetime bug, not just misuse.
     */
    errno = 0;
    if (llam_join(borrowed) != -1 || errno != EBUSY) {
        goto cleanup_runtime;
    }
    errno = 0;
    if (llam_detach(borrowed) != -1 || errno != EBUSY) {
        goto cleanup_runtime;
    }
    if (llam_task_group_join(group) != 0) {
        goto cleanup_runtime;
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U) {
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
        return fail_errno("task group borrowed child handle guard failed");
    }
    return rc;
}

static int test_task_group_join_until_completed_children_wins_over_expired_deadline(void) {
    group_local_state_t state;
    llam_task_group_t *group;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);

    group = llam_task_group_create();
    if (group == NULL) {
        return fail_errno("task group create for completed join_until failed");
    }
    if (init_runtime() != 0) {
        goto cleanup_group;
    }
    for (unsigned i = 0U; i < 2U; ++i) {
        if (llam_task_group_spawn(group, group_counter_task, &state, NULL) == NULL) {
            goto cleanup_runtime;
        }
    }
    if (llam_run() != 0 || check_task_failures(&state) != 0) {
        goto cleanup_runtime;
    }
    /*
     * Match llam_join_until(): completion wins over an already-expired deadline
     * because no waiter has to be parked. A regression here strands completed
     * borrowed child handles in the group and turns later destroy into EBUSY.
     */
    errno = 0;
    if (llam_task_group_join_until(group, 0U) != 0) {
        goto cleanup_runtime;
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 2U) {
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
        return fail_errno("task group completed join_until contract failed");
    }
    return rc;
}

static void task_group_spawn_mark_keepalive_task(void *arg) {
    task_group_spawn_mark_race_state_t *state = arg;

    atomic_store_explicit(&state->keepalive_started, 1U, memory_order_release);
    while (atomic_load_explicit(&state->stop_keepalive, memory_order_acquire) == 0U) {
        if (llam_sleep_ns(1000000ULL) != 0) {
            task_group_spawn_mark_fail(state, "spawn mark keepalive sleep failed", errno);
            return;
        }
    }
}

static void task_group_spawn_mark_self_detach_task(void *arg) {
    task_group_spawn_mark_race_state_t *state = arg;
    llam_task_t *self = llam_current_task();

    atomic_fetch_add_explicit(&state->child_ran, 1U, memory_order_relaxed);
    errno = 0;
    if (llam_detach(self) == 0) {
        /*
         * Group-owned children must be marked before they can execute.  A
         * successful self-detach means the child observed itself as an ordinary
         * public task and consumed the handle outside the owning group.
         */
        atomic_fetch_add_explicit(&state->child_detach_succeeded, 1U, memory_order_relaxed);
        return;
    }
    if (errno != EBUSY) {
        atomic_fetch_add_explicit(&state->unexpected_child_errno, 1U, memory_order_relaxed);
        task_group_spawn_mark_fail(state, "group-owned child self-detach errno", errno);
    }
}

static int test_task_group_spawn_marks_child_before_execution(void) {
#if LLAM_PLATFORM_POSIX
    enum { rounds = 20000U };
    task_group_spawn_mark_race_state_t state;
    task_group_run_thread_state_t run_state;
    llam_task_group_t *group = NULL;
    llam_task_t *keepalive = NULL;
    pthread_t run_thread;
    bool run_thread_started = false;
    int err;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    memset(&run_state, 0, sizeof(run_state));
    atomic_init(&state.keepalive_started, 0U);
    atomic_init(&state.stop_keepalive, 0U);
    atomic_init(&state.child_ran, 0U);
    atomic_init(&state.child_detach_succeeded, 0U);
    atomic_init(&state.unexpected_child_errno, 0U);
    atomic_init(&state.failures, 0U);

    if (init_runtime() != 0) {
        return fail_errno("spawn mark race runtime init failed");
    }
    group = llam_task_group_create();
    keepalive = llam_spawn(task_group_spawn_mark_keepalive_task, &state, NULL);
    if (group == NULL || keepalive == NULL) {
        goto cleanup;
    }
    err = pthread_create(&run_thread, NULL, task_group_run_thread, &run_state);
    if (err != 0) {
        errno = err;
        goto cleanup;
    }
    run_thread_started = true;

    while (atomic_load_explicit(&state.keepalive_started, memory_order_acquire) == 0U) {
        if (llam_sleep_ns(1000000ULL) != 0) {
            goto cleanup;
        }
    }

    for (unsigned i = 0U; i < rounds; ++i) {
        errno = 0;
        if (llam_task_group_spawn(group, task_group_spawn_mark_self_detach_task, &state, NULL) == NULL) {
            fprintf(stderr,
                    "[test_runtime_group_local_edges] group spawn mark race failed at round=%u errno=%d (%s)\n",
                    i,
                    errno,
                    strerror(errno));
            goto cleanup;
        }
    }

    atomic_store_explicit(&state.stop_keepalive, 1U, memory_order_release);
    pthread_join(run_thread, NULL);
    run_thread_started = false;
    if (run_state.rc != 0) {
        errno = run_state.err;
        goto cleanup;
    }
    if (llam_join(keepalive) != 0) {
        keepalive = NULL;
        goto cleanup;
    }
    keepalive = NULL;
    if (llam_task_group_join(group) != 0) {
        goto cleanup;
    }
    if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U ||
        atomic_load_explicit(&state.child_detach_succeeded, memory_order_relaxed) != 0U ||
        atomic_load_explicit(&state.unexpected_child_errno, memory_order_relaxed) != 0U ||
        atomic_load_explicit(&state.child_ran, memory_order_relaxed) != rounds) {
        fprintf(stderr,
                "[test_runtime_group_local_edges] spawn mark race result: ran=%u detach_success=%u "
                "unexpected_errno=%u failures=%u first=%s errno=%d (%s)\n",
                atomic_load_explicit(&state.child_ran, memory_order_relaxed),
                atomic_load_explicit(&state.child_detach_succeeded, memory_order_relaxed),
                atomic_load_explicit(&state.unexpected_child_errno, memory_order_relaxed),
                atomic_load_explicit(&state.failures, memory_order_relaxed),
                state.first_case,
                state.first_errno,
                strerror(state.first_errno));
        errno = EINVAL;
        goto cleanup;
    }
    rc = 0;

cleanup:
    atomic_store_explicit(&state.stop_keepalive, 1U, memory_order_release);
    if (run_thread_started) {
        pthread_join(run_thread, NULL);
    }
    if (keepalive != NULL) {
        (void)llam_detach(keepalive);
    }
    if (group != NULL) {
        (void)llam_task_group_cancel(group);
        (void)llam_task_group_join(group);
        (void)llam_task_group_destroy(group);
    }
    llam_runtime_shutdown();
    if (rc != 0) {
        return fail_errno("task group child mark-before-execution race failed");
    }
    return 0;
#else
    return 0;
#endif
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
    if (test_task_local_key_create_enospc_clears_output() != 0) {
        return 1;
    }
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
    if (test_task_local_delete_race_invalidates_live_entry() != 0) {
        return 1;
    }
    if (test_task_group_spawn_destroy_race() != 0) {
        return 1;
    }
    if (test_task_group_join_and_destroy() != 0) {
        return 1;
    }
    if (test_task_group_borrowed_child_handle_not_consumable() != 0) {
        return 1;
    }
    if (test_task_group_join_until_completed_children_wins_over_expired_deadline() != 0) {
        return 1;
    }
    if (test_task_group_spawn_marks_child_before_execution() != 0) {
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
