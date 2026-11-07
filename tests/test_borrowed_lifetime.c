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
#include "borrowed_lifetime_internal.h"
#include "core/task/task_group_internal.h"
#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "%s:%d: %s errno=%d\n", __FILE__, __LINE__, #expr, errno); return false; } } while (0)

typedef struct lifetime_case {
    llam_runtime_t *runtime;
    llam_task_group_t *group;
    llam_cancel_token_t *parent_token;
    llam_task_local_key_t key;
    llam_task_t *parent;
    atomic_uint failures, entered, release, callback_done, cleanup_done, parent_done, children;
    bool stop_runtime;
    unsigned hook_point;
    llam_task_t *spawned;
    int spawn_error;
} lifetime_case_t;

typedef struct borrowed_capture {
    lifetime_case_t *test;
    unsigned value;
} borrowed_capture_t;

static void record_task_check(lifetime_case_t *test, bool passed) {
    if (!passed) atomic_fetch_add(&test->failures, 1U);
}
#define task_check(test, expression) do { \
    /* MSVC C11 atomics need evaluation outside the bool call argument. */ \
    bool checked = (expression); \
    record_task_check((test), checked); \
} while (0)

static bool case_create(lifetime_case_t *test) {
    memset(test, 0, sizeof(*test));
    atomic_init(&test->failures, 0U);
    atomic_init(&test->entered, 0U);
    atomic_init(&test->release, 0U);
    atomic_init(&test->callback_done, 0U);
    atomic_init(&test->cleanup_done, 0U);
    atomic_init(&test->parent_done, 0U);
    atomic_init(&test->children, 0U);
    llam_runtime_opts_t options;
    CHECK(llam_runtime_opts_init(&options, sizeof(options)) == 0);
    options.deterministic = 1U;
    options.worker_count = 1U;
    CHECK(llam_runtime_create_compat(&options, sizeof(options), &test->runtime) == 0);
    test->group = llam_runtime_task_group_create(test->runtime);
    test->parent_token = llam_runtime_cancel_token_create(test->runtime);
    CHECK(test->group != NULL && test->parent_token != NULL);
    CHECK(llam_task_local_key_create(&test->key) == 0);
    return true;
}

static bool case_destroy(lifetime_case_t *test) {
    CHECK(atomic_load(&test->failures) == 0U);
    CHECK(llam_task_group_destroy(test->group) == 0);
    CHECK(llam_cancel_token_destroy(test->parent_token) == 0);
    CHECK(llam_task_local_key_delete(test->key) == 0);
    llam_runtime_destroy(test->runtime);
    return true;
}

static void borrowed_cleanup(void *data) {
    borrowed_capture_t *capture = data;
    capture->value += 1U;
    atomic_store(&capture->test->cleanup_done, 1U);
}

static void *borrowed_callback(void *data) {
    borrowed_capture_t *capture = data;
    lifetime_case_t *test = capture->test;
    atomic_store(&test->entered, 1U);
    uint64_t limit = llam_now_ns() + UINT64_C(3000000000);
    while (!atomic_load(&test->release) && llam_now_ns() < limit) {
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000L};
        nanosleep(&pause, NULL);
    }
    capture->value = 42U;
    atomic_store(&test->callback_done, 1U);
    return data;
}

static void borrowed_child(void *data) {
    borrowed_capture_t *capture = data;
    lifetime_case_t *test = capture->test;
    task_check(test, llam_task_local_set_owned(test->key, capture, borrowed_cleanup) == 0);
    void *result = NULL;
    int status = llam_call_blocking_result(borrowed_callback, data, &result);
    task_check(test, status == 0 || (status == -1 && errno == ECANCELED));
    task_check(test, capture->value == 42U && atomic_load(&test->callback_done));
}

static void borrowed_parent(void *data) {
    lifetime_case_t *test = data;
    borrowed_capture_t capture = {.test = test};
    task_check(test, llam_task_group_spawn(test->group, borrowed_child, &capture, NULL) != NULL);
    int drained = llam_task_group_drain(test->group);
    if (drained != 0) fprintf(stderr, "borrowed drain errno=%d\n", errno);
    task_check(test, drained == 0 && capture.value == 43U && atomic_load(&test->cleanup_done));
    task_check(test, llam_cancel_token_is_cancelled(test->parent_token) == 1);
    int slept = llam_sleep_ns(UINT64_C(1000000));
    task_check(test, slept == -1 && (errno == ECANCELED || errno == ETIMEDOUT));
    atomic_store(&test->parent_done, 1U);
}

static bool parent_parked(lifetime_case_t *test) {
    llam_task_t *parent = llam_task_resolve_public_handle(test->parent);
    if (parent == NULL) return false;
    bool parked = atomic_load(&parent->borrowed_drain_depth) != 0U &&
                  atomic_load(&parent->state) == LLAM_TASK_STATE_PARKED;
    llam_task_end_public_op(parent);
    return parked;
}

static void *cancel_during_drain(void *data) {
    lifetime_case_t *test = data;
    uint64_t limit = llam_now_ns() + UINT64_C(2000000000);
    while ((!atomic_load(&test->entered) || !parent_parked(test)) && llam_now_ns() < limit) {
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000L};
        nanosleep(&pause, NULL);
    }
    task_check(test, atomic_load(&test->entered) && parent_parked(test));
    task_check(test, llam_cancel_token_cancel(test->parent_token) == 0);
    if (test->stop_runtime) task_check(test, llam_runtime_request_stop_handle(test->runtime) == 0);
    struct timespec hold = {.tv_sec = 0, .tv_nsec = 20000000L};
    nanosleep(&hold, NULL);
    task_check(test, !atomic_load(&test->parent_done) && !atomic_load(&test->callback_done));
    atomic_store(&test->release, 1U);
    return NULL;
}

static bool borrowed_drain_case(bool cancelled_before, bool stop_runtime) {
    lifetime_case_t test;
    CHECK(case_create(&test));
    test.stop_runtime = stop_runtime;
    if (cancelled_before) CHECK(llam_cancel_token_cancel(test.parent_token) == 0);
    llam_spawn_opts_t options;
    CHECK(llam_spawn_opts_init(&options, sizeof(options)) == 0);
    options.cancel_token = test.parent_token;
    options.deadline_ns = 1U;
    test.parent = llam_runtime_spawn_ex(test.runtime, borrowed_parent, &test, &options, sizeof(options));
    CHECK(test.parent != NULL);
    pthread_t stopper;
    CHECK(pthread_create(&stopper, NULL, cancel_during_drain, &test) == 0);
    CHECK(llam_runtime_run_handle(test.runtime) == 0);
    CHECK(pthread_join(stopper, NULL) == 0);
    CHECK(llam_join(test.parent) == 0 && atomic_load(&test.parent_done));
    return case_destroy(&test);
}

static void count_child(void *data) {
    lifetime_case_t *test = data;
    atomic_fetch_add(&test->children, 1U);
}

static bool join_reuse_and_sealed_drain(void) {
    lifetime_case_t test;
    CHECK(case_create(&test));
    CHECK(llam_task_group_spawn(test.group, count_child, &test, NULL) != NULL);
    CHECK(llam_task_group_drain(test.group) == -1 && errno == EBUSY);
    CHECK(llam_task_group_spawn(test.group, count_child, &test, NULL) != NULL);
    CHECK(llam_runtime_run_handle(test.runtime) == 0 && llam_task_group_join(test.group) == 0);
    llam_task_t *borrowed = llam_task_group_spawn(test.group, count_child, &test, NULL);
    CHECK(borrowed != NULL && llam_runtime_run_handle(test.runtime) == 0);
    CHECK(llam_task_group_drain(test.group) == 0);
    CHECK(atomic_load(&test.children) == 3U);
    CHECK(llam_task_group_spawn(test.group, count_child, &test, NULL) == NULL && errno == EPIPE);
    CHECK(llam_join(borrowed) == -1 && errno == EINVAL);
    CHECK(llam_task_group_drain(test.group) == 0);
    return case_destroy(&test);
}

static void self_drain_child(void *data) {
    lifetime_case_t *test = data;
    task_check(test, llam_task_group_drain(test->group) == -1 && errno == EDEADLK);
    task_check(test, llam_task_group_spawn(test->group, count_child, test, NULL) != NULL);
    atomic_fetch_add(&test->children, 1U);
}

static bool reject_self_drain(void) {
    lifetime_case_t test;
    CHECK(case_create(&test));
    CHECK(llam_task_group_spawn(test.group, self_drain_child, &test, NULL) != NULL);
    CHECK(llam_runtime_run_handle(test.runtime) == 0 && llam_task_group_drain(test.group) == 0);
    CHECK(atomic_load(&test.children) == 2U);
    llam_task_group_t *stale = test.group;
    CHECK(case_destroy(&test));
    CHECK(llam_task_group_drain(stale) == -1 && errno == EINVAL);
    return true;
}

static void peer_drain(void *data) {
    lifetime_case_t *test = data;
    task_check(test, llam_task_group_drain(test->group) == -1 && errno == EXDEV);
}

static bool reject_peer_runtime(void) {
    lifetime_case_t test;
    CHECK(case_create(&test));
    llam_runtime_t *peer = NULL;
    CHECK(llam_runtime_create_compat(NULL, 0U, &peer) == 0);
    llam_task_t *task = llam_runtime_spawn_ex(peer, peer_drain, &test, NULL, 0U);
    CHECK(task != NULL && llam_runtime_run_handle(peer) == 0 && llam_join(task) == 0);
    llam_runtime_destroy(peer);
    CHECK(llam_task_group_drain(test.group) == 0);
    return case_destroy(&test);
}

static void admission_hook(unsigned point, void *data) {
    lifetime_case_t *test = data;
    if (point != test->hook_point) return;
    atomic_store(&test->entered, 1U);
    uint64_t limit = llam_now_ns() + UINT64_C(3000000000);
    while (!atomic_load(&test->release) && llam_now_ns() < limit) {
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000L};
        nanosleep(&pause, NULL);
    }
}

static void *admission_spawner(void *data) {
    lifetime_case_t *test = data;
    test->spawned = llam_task_group_spawn(test->group, count_child, test, NULL);
    test->spawn_error = errno;
    return NULL;
}

static void drain_racing_parent(void *data) {
    lifetime_case_t *test = data;
    task_check(test, llam_task_group_drain(test->group) == 0);
    atomic_store(&test->parent_done, 1U);
}

static void *release_admission(void *data) {
    lifetime_case_t *test = data;
    uint64_t limit = llam_now_ns() + UINT64_C(2000000000);
    bool reached = false;
    while (llam_now_ns() < limit) {
        reached = test->hook_point == 1U ? atomic_load(&test->parent_done) != 0U : parent_parked(test);
        if (reached) break;
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000L};
        nanosleep(&pause, NULL);
    }
    task_check(test, reached);
    if (test->hook_point == 2U) task_check(test, !atomic_load(&test->parent_done));
    atomic_store(&test->release, 1U);
    return NULL;
}

static bool drain_races_admission(unsigned point) {
    lifetime_case_t test;
    CHECK(case_create(&test));
    test.hook_point = point;
    llam_task_group_test_spawn_hook(admission_hook, &test);
    pthread_t spawner, releaser;
    CHECK(pthread_create(&spawner, NULL, admission_spawner, &test) == 0);
    uint64_t limit = llam_now_ns() + UINT64_C(2000000000);
    while (!atomic_load(&test.entered) && llam_now_ns() < limit) {
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000L};
        nanosleep(&pause, NULL);
    }
    CHECK(atomic_load(&test.entered));
    test.parent = llam_runtime_spawn_ex(test.runtime, drain_racing_parent, &test, NULL, 0U);
    CHECK(test.parent != NULL && pthread_create(&releaser, NULL, release_admission, &test) == 0);
    CHECK(llam_runtime_run_handle(test.runtime) == 0);
    CHECK(pthread_join(releaser, NULL) == 0 && pthread_join(spawner, NULL) == 0);
    llam_task_group_test_spawn_hook(NULL, NULL);
    CHECK(llam_join(test.parent) == 0);
    if (point == 1U) CHECK(test.spawned == NULL && test.spawn_error == EPIPE && !atomic_load(&test.children));
    else CHECK(test.spawned != NULL && atomic_load(&test.children) == 1U);
    return case_destroy(&test);
}

static bool empty_legacy_group(void) {
    llam_task_group_t *group = llam_task_group_create();
    CHECK(group != NULL);
    CHECK(llam_task_group_drain(group) == 0);
    CHECK(llam_task_group_destroy(group) == 0);
    return true;
}

static bool group_cases(void) {
    CHECK(empty_legacy_group());
    CHECK(borrowed_drain_case(true, false));
    CHECK(borrowed_drain_case(false, false));
    CHECK(borrowed_drain_case(false, true));
    CHECK(join_reuse_and_sealed_drain());
    CHECK(reject_self_drain());
    CHECK(reject_peer_runtime());
    CHECK(drain_races_admission(1U));
    CHECK(drain_races_admission(2U));
    puts("LLAM group drain: borrowed callback/cleanup retirement, cancellation, deadlines, stop and ownership passed");
    return true;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--group") == 0) return group_cases() ? 0 : 1;
    if (argc == 2 && strcmp(argv[1], "--io") == 0) return borrowed_io_lifetime_cases(false) ? 0 : 1;
    if (argc == 2 && strcmp(argv[1], "--io-helper") == 0) return borrowed_io_lifetime_cases(true) ? 0 : 1;
    if (argc != 1) { fprintf(stderr, "unsupported lifetime test selection\n"); return 2; }
    return group_cases() && borrowed_io_lifetime_cases(false) ? 0 : 1;
}
