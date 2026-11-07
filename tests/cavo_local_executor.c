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
#include "core/task/local_executor_internal.h"
#include "llam/executor.h"
#include "llam/managed.h"
#include "cavo_execution_internal.h"
#include "fixtures/managed/cavo_execution.h"
#if defined(LLAM_CAVO_NATIVE_FIXTURE_PATH)
#include <dlfcn.h>
#endif
#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "local executor:%d: %s errno=%d\n", __LINE__, #expr, errno); return false; } } while (0)

typedef struct local_case local_case_t;
typedef struct local_child {
    local_case_t *test;
    pthread_t before, after, cleanup;
    unsigned value;
} local_child_t;

struct local_case {
    llam_runtime_t *runtime;
    llam_local_executor_t *executor;
    llam_task_local_key_t key;
    llam_cancel_token_t *token;
    atomic_uint failures;
    atomic_uint root_done, tls_entered, tls_release, tls_done, drain_started, drain_done;
    bool tls_test;
#if LLAM_PLATFORM_WINDOWS
    DWORD native_key;
#else
    pthread_key_t native_key;
#endif
    pthread_t tls_thread;
    int run_result;
    pthread_t root_thread;
    unsigned shared_value;
};

static void local_check(local_case_t *test, bool passed) {
    if (!passed) atomic_fetch_add(&test->failures, 1U);
}

static void local_cleanup(void *data) {
    local_child_t *child = data;
    child->cleanup = pthread_self();
    child->value += 1U;
}

static void local_child_entry(void *data) {
    local_child_t *child = data;
    local_case_t *test = child->test;
    child->before = pthread_self();
    local_check(test, llam_task_local_set_owned(test->key, child, local_cleanup) == 0);
    for (unsigned i = 0U; i != 20U; ++i) {
        local_check(test, llam_preempt_region_enter() == 0);
        test->shared_value += 1U; /* Non-atomic local state, protected against G interleaving. */
        local_check(test, llam_preempt_region_leave() == 0);
        llam_yield();
    }
    local_check(test, llam_sleep_ns(UINT64_C(1000000)) == 0);
    child->after = pthread_self();
    child->value = 42U;
}

static void local_tls_destructor(void *data) {
    local_case_t *test = data;
    test->tls_thread = pthread_self();
    atomic_store(&test->tls_entered, 1U);
    uint64_t limit = llam_now_ns() + UINT64_C(3000000000);
    while (!atomic_load(&test->tls_release) && llam_now_ns() < limit) {
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000L};
        nanosleep(&pause, NULL);
    }
    local_check(test, atomic_load(&test->tls_release) != 0U);
    atomic_store(&test->tls_done, 1U);
}

#if LLAM_PLATFORM_WINDOWS
static VOID CALLBACK local_fls_destructor(PVOID data) { local_tls_destructor(data); }
#endif

static void local_install_tls(local_case_t *test) {
    if (!test->tls_test) return;
#if LLAM_PLATFORM_WINDOWS
    local_check(test, FlsSetValue(test->native_key, test) != 0);
#else
    local_check(test, pthread_setspecific(test->native_key, test) == 0);
#endif
}

static void local_root(void *data) {
    local_case_t *test = data;
    test->root_thread = pthread_self();
    local_install_tls(test);
    local_check(test, llam_local_executor_drain(test->executor) == -1 && errno == EDEADLK);
    llam_task_group_t *group = llam_runtime_task_group_create(test->runtime);
    local_check(test, group != NULL);
    if (group == NULL) return;
    local_child_t children[3] = {{.test = test}, {.test = test}, {.test = test}};
    llam_spawn_opts_t options;
    local_check(test, llam_spawn_opts_init(&options, sizeof(options)) == 0);
    options.local_executor = test->executor;
    for (unsigned i = 0U; i != 3U; ++i)
        local_check(test, llam_task_group_spawn_ex(group, local_child_entry, &children[i], &options, sizeof(options)) != NULL);
    local_check(test, llam_local_executor_close(test->executor) == 0);
    local_check(test, llam_task_group_spawn_ex(group, local_child_entry, &children[0], &options, sizeof(options)) == NULL && errno == EPIPE);
    local_check(test, llam_cancel_token_cancel(test->token) == 0);
    local_check(test, llam_task_group_drain(group) == 0);
    for (unsigned i = 0U; i != 3U; ++i) {
        local_check(test, children[i].value == 43U);
        local_check(test, pthread_equal(test->root_thread, children[i].before) != 0);
        local_check(test, pthread_equal(children[i].before, children[i].after) != 0);
        local_check(test, pthread_equal(children[i].after, children[i].cleanup) != 0);
    }
    local_check(test, test->shared_value == 60U);
    local_check(test, llam_task_group_destroy(group) == 0);
    atomic_store(&test->root_done, 1U);
}

static bool local_case_create(local_case_t *test) {
    memset(test, 0, sizeof(*test));
    atomic_init(&test->failures, 0U);
    atomic_init(&test->root_done, 0U);
    atomic_init(&test->tls_entered, 0U);
    atomic_init(&test->tls_release, 0U);
    atomic_init(&test->tls_done, 0U);
    atomic_init(&test->drain_started, 0U);
    atomic_init(&test->drain_done, 0U);
    llam_runtime_opts_t options;
    llam_preempt_opts_t policy;
    CHECK(llam_runtime_opts_init(&options, sizeof(options)) == 0);
    CHECK(llam_preempt_opts_init(&policy, sizeof(policy)) == 0);
    options.worker_count = 2U;
    policy.policy = LLAM_PREEMPT_POLICY_COOPERATIVE;
    policy.native_affine_capacity = 1U;
    CHECK(llam_runtime_create_managed(&options, sizeof(options), &policy, sizeof(policy), &test->runtime) == 0);
    CHECK(llam_runtime_local_executor_create_ex(test->runtime, NULL, 0U, &test->executor) == 0);
    test->token = llam_runtime_cancel_token_create(test->runtime);
    CHECK(test->token != NULL && llam_task_local_key_create(&test->key) == 0);
    return true;
}

static bool local_shared_thread(void) {
    local_case_t test;
    CHECK(local_case_create(&test));
    llam_spawn_opts_t options;
    CHECK(llam_spawn_opts_init(&options, sizeof(options)) == 0);
    options.local_executor = test.executor;
    options.cancel_token = test.token;
    llam_task_t *root = llam_runtime_spawn_ex(test.runtime, local_root, &test, &options, sizeof(options));
    CHECK(root != NULL);
    CHECK(llam_local_executor_drain(test.executor) == -1 && errno == EBUSY);
    CHECK(llam_runtime_run_handle(test.runtime) == 0);
    CHECK(llam_local_executor_drain(test.executor) == 0);
    CHECK(atomic_load(&test.failures) == 0U);
    CHECK(pthread_equal(test.root_thread, pthread_self()) == 0);
    llam_local_executor_t *stale = test.executor;
    CHECK(llam_local_executor_destroy(test.executor) == 0);
    CHECK(llam_join(root) == 0); /* A retained member handle outlives the public executor. */
    CHECK(llam_local_executor_close(stale) == -1 && errno == EINVAL);
    CHECK(llam_cancel_token_destroy(test.token) == 0 && llam_task_local_key_delete(test.key) == 0);
    llam_runtime_destroy(test.runtime);
    return true;
}

static bool local_empty_and_shutdown(void) {
    local_case_t test;
    CHECK(local_case_create(&test));
    CHECK(llam_local_executor_close(test.executor) == 0);
    CHECK(llam_local_executor_drain(test.executor) == 0);
    CHECK(llam_local_executor_destroy(test.executor) == 0);
    CHECK(llam_runtime_local_executor_create_ex(test.runtime, NULL, 0U, &test.executor) == 0);
    llam_spawn_opts_t options;
    CHECK(llam_spawn_opts_init(&options, sizeof(options)) == 0);
    options.local_executor = test.executor;
    CHECK(llam_runtime_spawn_ex(test.runtime, local_root, &test, &options, sizeof(options)) != NULL);
    llam_runtime_destroy(test.runtime); /* Never-started members release admission without executing. */
    CHECK(llam_local_executor_drain(test.executor) == 0 && llam_local_executor_destroy(test.executor) == 0);
    CHECK(llam_cancel_token_destroy(test.token) == 0 && llam_task_local_key_delete(test.key) == 0);
    return true;
}

static void *local_driver(void *data) {
    local_case_t *test = data;
    test->run_result = llam_runtime_run_handle(test->runtime);
    return NULL;
}

static void *local_release_tls(void *data) {
    local_case_t *test = data;
    uint64_t limit = llam_now_ns() + UINT64_C(3000000000);
    while ((!atomic_load(&test->tls_entered) || !atomic_load(&test->drain_started)) && llam_now_ns() < limit) {
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000L};
        nanosleep(&pause, NULL);
    }
    local_check(test, atomic_load(&test->tls_entered) && atomic_load(&test->drain_started));
    struct timespec hold = {.tv_sec = 0, .tv_nsec = 20000000L};
    nanosleep(&hold, NULL);
    local_check(test, !atomic_load(&test->drain_done));
    atomic_store(&test->tls_release, 1U);
    return NULL;
}

static bool local_native_key(local_case_t *test, bool create) {
#if LLAM_PLATFORM_WINDOWS
    if (!create) return FlsFree(test->native_key) != 0;
    test->native_key = FlsAlloc(local_fls_destructor);
    return test->native_key != FLS_OUT_OF_INDEXES;
#else
    if (!create) return pthread_key_delete(test->native_key) == 0;
    return pthread_key_create(&test->native_key, local_tls_destructor) == 0;
#endif
}

static bool local_tls_lifetime(void) {
    local_case_t test;
    CHECK(local_case_create(&test));
    test.tls_test = true;
    CHECK(local_native_key(&test, true));
    llam_spawn_opts_t options;
    CHECK(llam_spawn_opts_init(&options, sizeof(options)) == 0);
    options.local_executor = test.executor;
    options.cancel_token = test.token;
    llam_task_t *root = llam_runtime_spawn_ex(test.runtime, local_root, &test, &options, sizeof(options));
    CHECK(root != NULL);
    pthread_t driver, release;
    CHECK(pthread_create(&release, NULL, local_release_tls, &test) == 0);
    CHECK(pthread_create(&driver, NULL, local_driver, &test) == 0);
    uint64_t limit = llam_now_ns() + UINT64_C(3000000000);
    while (!atomic_load(&test.root_done) && llam_now_ns() < limit) {
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000L};
        nanosleep(&pause, NULL);
    }
    CHECK(atomic_load(&test.root_done) && llam_join(root) == 0);
    atomic_store(&test.drain_started, 1U);
    CHECK(llam_local_executor_drain(test.executor) == 0);
    atomic_store(&test.drain_done, 1U);
    CHECK(pthread_join(release, NULL) == 0 && pthread_join(driver, NULL) == 0);
    CHECK(test.run_result == 0 && atomic_load(&test.failures) == 0U && atomic_load(&test.tls_done));
    CHECK(pthread_equal(test.root_thread, test.tls_thread) != 0);
    CHECK(llam_local_executor_destroy(test.executor) == 0 && local_native_key(&test, false));
    CHECK(llam_cancel_token_destroy(test.token) == 0 && llam_task_local_key_delete(test.key) == 0);
    llam_runtime_destroy(test.runtime);
    return true;
}

static bool local_rejects_wrong_owner_and_capacity(void) {
    local_case_t first, peer;
    CHECK(local_case_create(&first));
    llam_local_executor_t *unchanged = (llam_local_executor_t *)(uintptr_t)1U;
    CHECK(llam_runtime_local_executor_create_ex(first.runtime, NULL, 0U, &unchanged) == -1 && errno == EAGAIN);
    CHECK(unchanged == (llam_local_executor_t *)(uintptr_t)1U);
    CHECK(local_case_create(&peer));
    llam_spawn_opts_t options;
    CHECK(llam_spawn_opts_init(&options, sizeof(options)) == 0);
    options.local_executor = first.executor;
    CHECK(llam_runtime_spawn_ex(peer.runtime, local_root, &first, &options, sizeof(options)) == NULL && errno == EXDEV);
    llam_channel_t *channel = llam_runtime_channel_create(first.runtime, 1U);
    CHECK(channel != NULL);
    CHECK(llam_local_executor_close((llam_local_executor_t *)channel) == -1 && errno == EINVAL);
    CHECK(llam_channel_close(channel) == 0 && llam_channel_destroy(channel) == 0);
    local_case_t *cases[2] = {&peer, &first};
    for (unsigned i = 0U; i != 2U; ++i) {
        CHECK(llam_local_executor_drain(cases[i]->executor) == 0);
        CHECK(llam_local_executor_destroy(cases[i]->executor) == 0);
        CHECK(llam_cancel_token_destroy(cases[i]->token) == 0);
        CHECK(llam_task_local_key_delete(cases[i]->key) == 0);
        llam_runtime_destroy(cases[i]->runtime);
    }
    return true;
}

static bool local_rejects_closing_binding(void) {
    local_case_t test;
    CHECK(local_case_create(&test));
    llam_structured_object_t *object;
    CHECK(llam_structured_begin(test.executor, LLAM_PUBLIC_HANDLE_FAMILY_LOCAL_EXECUTOR, &object) == 0);
    llam_local_executor_t *executor = (llam_local_executor_t *)object;
    llam_spawn_opts_t options = {0};
    options.local_executor = test.executor;
    /* Freeze the published exit side of the scheduler/admission handshake. */
    atomic_store_explicit(&executor->worker->managed_slot_closing, true, memory_order_seq_cst);
    llam_task_t *task = llam_runtime_spawn_ex(test.runtime, local_root, &test, &options, sizeof(options));
    int error = errno;
    atomic_store_explicit(&executor->worker->managed_slot_closing, false, memory_order_seq_cst);
    CHECK(task == NULL && error == EPIPE && executor->pending == 0U);
    llam_structured_end(object);
    CHECK(llam_local_executor_drain(test.executor) == 0 && llam_local_executor_destroy(test.executor) == 0);
    CHECK(llam_cancel_token_destroy(test.token) == 0 && llam_task_local_key_delete(test.key) == 0);
    llam_runtime_destroy(test.runtime);
    return true;
}

/* A native-affine fail-stop hook for the fixture: fail without unwinding a
 * parent stack whose children might still own a loan. */
void cavo_fixture_abort(void) { _Exit(125); }

#if defined(LLAM_CAVO_NATIVE_FIXTURE_PATH)
static void *local_native_identity(void *argument) {
    (void)argument;
    return (void *)(uintptr_t)pthread_self();
}

static void local_native_cleanup(void *argument) {
    cavo_local_native_child_t *child = argument;
    child->cleanup = (uint64_t)(uintptr_t)pthread_self();
    child->value += 1U;
}

static bool native_root_parked(cavo_local_native_work_t *work) {
    llam_task_t *root = llam_task_resolve_public_handle(work->root);
    if (root == NULL) return false;
    bool parked = atomic_load(&root->borrowed_drain_depth) != 0U &&
                  atomic_load(&root->state) == LLAM_TASK_STATE_PARKED;
    llam_task_end_public_op(root);
    return parked;
}

static void *local_native_release(void *argument) {
    cavo_local_native_work_t *work = argument;
    uint64_t limit = llam_now_ns() + UINT64_C(5000000000);
    for (;;) {
        if (atomic_load(&work->errors)) cavo_fixture_abort();
        if (atomic_load(&work->draining) && native_root_parked(work)) break;
        if (llam_now_ns() >= limit) cavo_fixture_abort();
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000L};
        nanosleep(&pause, NULL);
    }
    atomic_store_explicit(&work->release_children, 1U, memory_order_release);
    return NULL;
}

static bool local_native_work_init(cavo_local_native_work_t *work, llam_runtime_t *runtime) {
    memset(work, 0, sizeof(*work));
    work->runtime = runtime;
    work->identity = local_native_identity;
    work->cleanup = local_native_cleanup;
    atomic_init(&work->go, 0U);
    atomic_init(&work->errors, 0U);
    atomic_init(&work->checked, 0U);
    atomic_init(&work->draining, 0U);
    atomic_init(&work->release_children, 0U);
    CHECK(llam_runtime_local_executor_create_ex(runtime, NULL, 0U, &work->executor) == 0);
    work->token = llam_runtime_cancel_token_create(runtime);
    CHECK(work->token != NULL && llam_task_local_key_create(&work->key) == 0);
    return true;
}

static bool local_native_execute(llam_runtime_t *runtime, llam_task_fn entry) {
    cavo_local_native_work_t work;
    CHECK(local_native_work_init(&work, runtime));
    llam_spawn_opts_t options;
    CHECK(llam_spawn_opts_init(&options, sizeof(options)) == 0);
    options.local_executor = work.executor;
    options.cancel_token = work.token;
    work.root = llam_runtime_spawn_ex(runtime, entry, &work, &options, sizeof(options));
    CHECK(work.root != NULL);
    pthread_t release;
    CHECK(pthread_create(&release, NULL, local_native_release, &work) == 0);
    CHECK(llam_runtime_run_handle(runtime) == 0 && pthread_join(release, NULL) == 0);
    CHECK(llam_local_executor_drain(work.executor) == 0);
    CHECK(atomic_load(&work.errors) == 0U && atomic_load(&work.checked) == 1U);
    llam_runtime_stats_t stats;
    CHECK(llam_runtime_collect_stats_ex_handle(runtime, &stats, sizeof(stats)) == 0 && stats.preempt_yields != 0U);
    CHECK(llam_local_executor_destroy(work.executor) == 0 && llam_join(work.root) == 0);
    CHECK(llam_cancel_token_destroy(work.token) == 0 && llam_task_local_key_delete(work.key) == 0);
    return true;
}

static bool local_native_case(void) {
    void *module = dlopen(LLAM_CAVO_NATIVE_FIXTURE_PATH, RTLD_NOW | RTLD_LOCAL);
    if (module == NULL) fprintf(stderr, "local fixture load: %s\n", dlerror());
    CHECK(module != NULL);
    void *symbol = dlsym(module, "cavo_local_root");
    llam_task_fn entry = NULL;
    _Static_assert(sizeof(entry) == sizeof(symbol), "native entry ABI");
    memcpy(&entry, &symbol, sizeof(entry));
    CHECK(entry != NULL);
    llam_runtime_opts_t options;
    llam_preempt_opts_t policy;
    CHECK(llam_runtime_opts_init(&options, sizeof(options)) == 0);
    CHECK(llam_preempt_opts_init(&policy, sizeof(policy)) == 0);
    options.worker_count = 1U;
    options.preempt_quantum_ns = UINT64_C(1000000);
    policy.native_affine_capacity = 1U;
    llam_runtime_t *runtime = NULL;
    CHECK(llam_runtime_create_managed(&options, sizeof(options), &policy, sizeof(policy), &runtime) == 0);
    bool success = local_native_execute(runtime, entry);
    llam_runtime_destroy(runtime);
    CHECK(dlclose(module) == 0 && success);
    puts("qualified local executor: one native M, call-free async progress, parked borrowed drain and same-thread cleanup passed");
    return true;
}
#endif

bool cavo_local_executor_cases(bool native) {
    if (native) {
#if defined(LLAM_CAVO_NATIVE_FIXTURE_PATH)
        return local_native_case();
#else
        fprintf(stderr, "qualified local fixture unavailable\n");
        return false;
#endif
    }
    CHECK(local_shared_thread());
    CHECK(local_empty_and_shutdown());
    CHECK(local_tls_lifetime());
    CHECK(local_rejects_wrong_owner_and_capacity());
    CHECK(local_rejects_closing_binding());
    puts("local executor: shared native thread, parked children, cleanup, native TLS drain, closed admission and independent handles passed");
    return true;
}
