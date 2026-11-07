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
#include "llam/scope.h"
#include "llam/io.h"
#include "core/task/structured_internal.h"
#if !LLAM_PLATFORM_WINDOWS
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#define CHECK(test) do { if (!(test)) { \
    fprintf(stderr, "test_scope:%u: %s errno=%d\n", (unsigned)__LINE__, #test, errno); \
    return false; \
} } while (0)

typedef struct scope_case scope_case_t;
typedef struct owned_argument {
    scope_case_t *test;
    uint64_t number;
    unsigned mode;
    unsigned char io_byte;
} owned_argument_t;

struct scope_case {
    llam_runtime_t *runtime;
    llam_scope_t *scope;
    llam_future_t *future;
    llam_task_t *parent;
    llam_cancel_token_t *parent_token;
    llam_cancel_token_t *child_token;
    llam_result_contract_t contract;
    atomic_uint failures;
    atomic_uint arguments_dropped;
    atomic_uint results_dropped;
    atomic_uint entered;
    atomic_uint release;
    atomic_uint callback_returned;
    atomic_uint drain_started;
    atomic_uint drain_done;
    atomic_uint peer_progress;
    _Atomic(llam_task_t *) io_task;
    llam_fd_t read_fd;
    llam_fd_t write_fd;
    bool check_cleanup_lock;
    bool timed_cleanup;
    bool unbounded_cleanup;
    bool native_io;
    int run_result;
    int drain_result;
    int drain_error;
    llam_scope_report_t report;
};

static const llam_type_descriptor_t number_type = {UINT64_C(0x6c6c616d73636f70), 1U, 8U, 8U, 0U};
static const llam_type_descriptor_t error_type = {UINT64_C(0x6c6c616d73636f70), 2U, 8U, 8U, 0U};
static const llam_type_descriptor_t argument_type = {
    UINT64_C(0x6c6c616d73636f70), 3U, sizeof(owned_argument_t), _Alignof(owned_argument_t), 0U
};

static void record_task_check(scope_case_t *test, bool passed, const char *expression, unsigned line) {
    if (!passed) {
        fprintf(stderr, "scope task:%u: %s errno=%d\n", line, expression, errno);
        atomic_fetch_add_explicit(&test->failures, 1U, memory_order_relaxed);
    }
}
#define task_check(test, expression) do { \
    /* Keep MSVC's C11 atomic intrinsic expansion out of the diagnostic call. */ \
    bool check_passed = (expression); \
    record_task_check((test), check_passed, #expression, __LINE__); \
} while (0)

static void argument_drop(void *data, void *context) {
    scope_case_t *test = context;
    if (test->check_cleanup_lock) {
        int result = llam_runtime_lifecycle_trylock();
        task_check(test, result == 0);
        if (result == 0) llam_runtime_lifecycle_unlock();
    }
    if (atomic_load(&test->entered)) task_check(test, atomic_load(&test->callback_returned) != 0U);
    atomic_fetch_add(&test->arguments_dropped, 1U);
    free(data);
}

static void result_drop(void *data, void *context) {
    scope_case_t *test = context;
    atomic_fetch_add(&test->results_dropped, 1U);
    free(data);
}

static bool case_create(scope_case_t *test, uint64_t max_tasks) {
    memset(test, 0, sizeof(*test));
    atomic_init(&test->failures, 0U);
    atomic_init(&test->arguments_dropped, 0U);
    atomic_init(&test->results_dropped, 0U);
    atomic_init(&test->entered, 0U);
    atomic_init(&test->release, 0U);
    atomic_init(&test->callback_returned, 0U);
    atomic_init(&test->drain_started, 0U);
    atomic_init(&test->drain_done, 0U);
    atomic_init(&test->peer_progress, 0U);
    atomic_init(&test->io_task, NULL);
    test->read_fd = LLAM_INVALID_FD;
    test->write_fd = LLAM_INVALID_FD;
    llam_runtime_opts_t opts;
    CHECK(llam_runtime_opts_init(&opts, sizeof(opts)) == 0);
    opts.worker_count = 1U;
    opts.blocking_worker_count = 1U;
    opts.max_tasks = max_tasks;
    CHECK(llam_runtime_create_compat(&opts, sizeof(opts), &test->runtime) == 0);
    CHECK(llam_runtime_scope_create_ex(test->runtime, NULL, 0U, &test->scope) == 0);
    CHECK(llam_result_contract_init(&test->contract, sizeof(test->contract)) == 0);
    return true;
}

static bool case_destroy(scope_case_t *test) {
    if (test->runtime != NULL) llam_runtime_destroy(test->runtime);
    CHECK(llam_scope_drain_until(test->scope, 0U, &test->report, sizeof(test->report)) == 0);
    CHECK(llam_scope_destroy(test->scope) == 0);
    if (test->future != NULL) CHECK(llam_future_release(test->future) == 0);
    if (test->parent_token != NULL) CHECK(llam_cancel_token_destroy(test->parent_token) == 0);
    if (test->child_token != NULL) CHECK(llam_cancel_token_destroy(test->child_token) == 0);
    CHECK(atomic_load(&test->failures) == 0U);
    return true;
}

static llam_value_t *new_argument(scope_case_t *test, unsigned mode) {
    owned_argument_t *argument = malloc(sizeof(*argument));
    if (argument == NULL) return NULL;
    *argument = (owned_argument_t){test, UINT64_C(0x12345678), mode, 0U};
    llam_value_spec_t spec = {argument_type, argument, argument_drop, test};
    llam_value_t *value = NULL;
    if (llam_value_create_ex(&spec, sizeof(spec), &value) != 0) free(argument);
    return value;
}

static int spawn_value(scope_case_t *test, llam_task_fn entry, llam_value_t *argument,
                       llam_future_t **out) {
    llam_typed_task_spec_t spec;
    if (llam_typed_task_spec_init(&spec, sizeof(spec)) != 0) return -1;
    llam_spawn_opts_t opts;
    if (llam_spawn_opts_init(&opts, sizeof(opts)) != 0) return -1;
    opts.cancel_token = test->child_token;
    spec.entry = entry;
    spec.argument = argument;
    spec.result = test->contract;
    spec.spawn_opts = &opts;
    spec.spawn_opts_size = sizeof(opts);
    return llam_scope_spawn_ex(test->scope, &spec, sizeof(spec), out);
}

static bool spawn_child(scope_case_t *test, llam_task_fn entry, unsigned mode) {
    llam_value_t *argument = new_argument(test, mode);
    CHECK(argument != NULL);
    if (spawn_value(test, entry, argument, &test->future) != 0) {
        (void)llam_value_release(argument);
        CHECK(false);
    }
    CHECK(llam_value_release(argument) == -1 && errno == EINVAL);
    return true;
}

static llam_value_t *new_result(scope_case_t *test, bool error) {
    uint64_t *number = malloc(sizeof(*number));
    if (number == NULL) return NULL;
    *number = 42U;
    llam_value_spec_t spec = {error ? error_type : number_type, number, result_drop, test};
    llam_value_t *value = NULL;
    if (llam_value_create_ex(&spec, sizeof(spec), &value) != 0) free(number);
    return value;
}

static void rejected_outcome_keeps_value(scope_case_t *test, bool duplicate) {
    llam_value_t *value = new_result(test, !duplicate);
    task_check(test, value != NULL);
    llam_outcome_t outcome = {.kind = LLAM_OUTCOME_OK, .value = value};
    int result = llam_task_set_outcome_ex(&outcome, sizeof(outcome));
    task_check(test, result == -1 && errno == (duplicate ? EBUSY : EINVAL));
    task_check(test, llam_value_release(value) == 0);
}

static void typed_entry(void *data) {
    owned_argument_t *argument = data;
    scope_case_t *test = argument->test;
    if (argument->mode == 3U) return; /* Missing a non-unit result is a runtime protocol error. */
    if (argument->mode == 4U) rejected_outcome_keeps_value(test, false);
    llam_outcome_t outcome = {0};
    outcome.kind = argument->mode == 2U ? LLAM_OUTCOME_CANCELLED
                                       : (argument->mode == 1U ? LLAM_OUTCOME_APP_ERROR : LLAM_OUTCOME_OK);
    if (outcome.kind != LLAM_OUTCOME_CANCELLED) outcome.value = new_result(test, argument->mode == 1U);
    task_check(test, outcome.kind == LLAM_OUTCOME_CANCELLED || outcome.value != NULL);
    task_check(test, llam_task_set_outcome_ex(&outcome, sizeof(outcome)) == 0);
    if (argument->mode == 4U) rejected_outcome_keeps_value(test, true);
    outcome.value = NULL;
    outcome.kind = LLAM_OUTCOME_CANCELLED;
    task_check(test, llam_task_set_outcome_ex(&outcome, sizeof(outcome)) == -1 && errno == EBUSY);
}

static bool take_result(scope_case_t *test, uint32_t expected_kind) {
    llam_outcome_t out;
    memset(&out, 0xa5, sizeof(out));
    CHECK(llam_future_take_ex(test->future, &test->contract, sizeof(test->contract), &out, 1U) == -1);
    CHECK(out.kind == UINT32_C(0xa5a5a5a5));
    llam_result_contract_t wrong = test->contract;
    wrong.success.id_low ^= UINT64_C(0x100);
    CHECK(llam_future_take_ex(test->future, &wrong, sizeof(wrong), &out, sizeof(out)) == -1);
    CHECK(out.kind == UINT32_C(0xa5a5a5a5));
    CHECK(llam_future_take_ex(test->future, &test->contract, sizeof(test->contract), &out, sizeof(out)) == 0);
    CHECK(out.kind == expected_kind && out.task_id != 0U);
    CHECK(llam_future_release(test->future) == -1 && errno == EINVAL);
    test->future = NULL;
    if (out.value != NULL) {
        const llam_type_descriptor_t *type = out.kind == LLAM_OUTCOME_OK ? &number_type : &error_type;
        void *data = NULL;
        CHECK(llam_value_data_ex(out.value, type, sizeof(*type), &data) == 0);
        CHECK(*(uint64_t *)data == 42U);
        CHECK(llam_value_release(out.value) == 0);
    } else CHECK(out.kind == LLAM_OUTCOME_CANCELLED || out.kind == LLAM_OUTCOME_RUNTIME_ERROR ||
                 test->contract.success.size == 0U);
    return true;
}

static bool outcome_and_scope_lifetime(unsigned mode) {
    scope_case_t test;
    CHECK(case_create(&test, 0U));
    test.contract.success = number_type;
    test.contract.error = error_type;
    CHECK(spawn_child(&test, typed_entry, mode));
    CHECK(llam_future_wait_until(test.future, 0U) == -1 && errno == ETIMEDOUT);
    CHECK(llam_scope_destroy(test.scope) == -1 && errno == EBUSY);
    CHECK(llam_runtime_run_handle(test.runtime) == 0);
    CHECK(llam_scope_drain_until(test.scope, 0U, &test.report, sizeof(test.report)) == 0);
    CHECK(test.report.pending_tasks == 0U && test.report.active_spawns == 0U);
    CHECK(test.report.flags & LLAM_SCOPE_REPORT_F_QUIESCENT);
    CHECK(atomic_load(&test.arguments_dropped) == 1U);
    CHECK(llam_scope_destroy(test.scope) == 0);
    CHECK(llam_scope_destroy(test.scope) == -1 && errno == EINVAL);
    llam_runtime_destroy(test.runtime);
    CHECK(llam_future_wait_until(test.future, 0U) == 0);
    uint32_t kind = mode == 0U || mode == 4U ? LLAM_OUTCOME_OK : mode == 1U ? LLAM_OUTCOME_APP_ERROR
                            : mode == 2U ? LLAM_OUTCOME_CANCELLED : LLAM_OUTCOME_RUNTIME_ERROR;
    CHECK(take_result(&test, kind));
    CHECK(atomic_load(&test.results_dropped) == (mode == 4U ? 3U : (mode < 2U ? 1U : 0U)));
    CHECK(atomic_load(&test.failures) == 0U);
    return true;
}

static bool admission_failure_preserves_argument(void) {
    scope_case_t test;
    CHECK(case_create(&test, 1U));
    test.contract.success = number_type;
    test.contract.error = error_type;
    CHECK(spawn_child(&test, typed_entry, 0U));
    llam_value_t *argument = new_argument(&test, 0U);
    CHECK(argument != NULL);
    llam_future_t *unchanged = (llam_future_t *)(uintptr_t)1U;
    CHECK(spawn_value(&test, typed_entry, argument, &unchanged) == -1 && errno == EAGAIN);
    CHECK(unchanged == (llam_future_t *)(uintptr_t)1U);
    CHECK(atomic_load(&test.arguments_dropped) == 0U);
    void *data = NULL;
    CHECK(llam_value_data_ex(argument, &argument_type, sizeof(argument_type), &data) == 0);
    CHECK(llam_value_release(argument) == 0);
    CHECK(llam_scope_cancel(test.scope) == 0);
    argument = new_argument(&test, 0U);
    CHECK(argument != NULL);
    CHECK(spawn_value(&test, typed_entry, argument, &unchanged) == -1 && errno == EPIPE);
    CHECK(llam_value_release(argument) == 0);
    CHECK(llam_runtime_run_handle(test.runtime) == 0);
    CHECK(llam_scope_drain_until(test.scope, 0U, &test.report, sizeof(test.report)) == 0);
    CHECK(test.report.admitted_tasks == 1U);
    CHECK(take_result(&test, LLAM_OUTCOME_OK)); /* A cancellation request does not overwrite explicit success. */
    CHECK(atomic_load(&test.arguments_dropped) == 3U);
    return case_destroy(&test);
}

static void cancelled_child(void *data) {
    owned_argument_t *argument = data;
    scope_case_t *test = argument->test;
    task_check(test, llam_sleep_ns(UINT64_C(1000000)) == -1 && errno == ECANCELED);
    task_check(test, llam_cancel_token_is_cancelled(test->child_token) == 0);
    llam_outcome_t outcome = {.kind = LLAM_OUTCOME_CANCELLED};
    task_check(test, llam_task_set_outcome_ex(&outcome, sizeof(outcome)) == 0);
}

static void already_cancelled_parent(void *data) {
    scope_case_t *test = data;
    task_check(test, spawn_child(test, cancelled_child, 0U));
    uint64_t deadline = test->unbounded_cleanup ? UINT64_MAX : llam_now_ns() + UINT64_C(1000000000);
    test->drain_result = llam_scope_cancel_and_drain_until(test->scope,
        deadline, &test->report, sizeof(test->report));
    test->drain_error = errno;
    task_check(test, test->drain_result == 0 && atomic_load(&test->arguments_dropped) == 1U);
    task_check(test, llam_cancel_token_is_cancelled(test->parent_token) == 1);
    int slept = llam_sleep_ns(UINT64_C(1000000));
    task_check(test, slept == -1 && (errno == ECANCELED || errno == ETIMEDOUT));
}

static bool cancelled_parent_case(bool unbounded) {
    scope_case_t test;
    CHECK(case_create(&test, 0U));
    test.unbounded_cleanup = unbounded;
    test.parent_token = llam_runtime_cancel_token_create(test.runtime);
    test.child_token = llam_runtime_cancel_token_create(test.runtime);
    CHECK(test.parent_token != NULL && test.child_token != NULL);
    CHECK(llam_cancel_token_cancel(test.parent_token) == 0);
    llam_spawn_opts_t opts;
    CHECK(llam_spawn_opts_init(&opts, sizeof(opts)) == 0);
    opts.cancel_token = test.parent_token;
    opts.deadline_ns = 1U;
    test.parent = llam_runtime_spawn_ex(test.runtime, already_cancelled_parent, &test, &opts, sizeof(opts));
    CHECK(test.parent != NULL && llam_runtime_run_handle(test.runtime) == 0);
    CHECK(llam_join(test.parent) == 0);
    CHECK(test.drain_result == 0 && test.report.cancelled == 1U);
    CHECK(take_result(&test, LLAM_OUTCOME_CANCELLED));
    return case_destroy(&test);
}

static bool cancelled_parent_uses_cleanup_budget(void) { return cancelled_parent_case(false); }
static bool cancelled_parent_unbounded(void) { return cancelled_parent_case(true); }

static bool dropped_future_is_still_scoped(void) {
    scope_case_t test;
    CHECK(case_create(&test, 0U));
    test.contract.success = number_type;
    test.contract.error = error_type;
    CHECK(spawn_child(&test, typed_entry, 0U));
    CHECK(llam_future_release(test.future) == 0);
    test.future = NULL;
    CHECK(llam_scope_get_report_ex(test.scope, &test.report, sizeof(test.report)) == 0);
    CHECK(test.report.pending_tasks == 1U);
    CHECK(llam_runtime_run_handle(test.runtime) == 0);
    CHECK(llam_scope_drain_until(test.scope, 0U, &test.report, sizeof(test.report)) == 0);
    CHECK(atomic_load(&test.arguments_dropped) == 1U && atomic_load(&test.results_dropped) == 1U);
    return case_destroy(&test);
}

static bool unstarted_runtime_shutdown(void) {
    scope_case_t test;
    CHECK(case_create(&test, 0U));
    test.check_cleanup_lock = true;
    test.contract.success = number_type;
    test.contract.error = error_type;
    CHECK(spawn_child(&test, typed_entry, 0U));
    CHECK(llam_scope_drain_until(test.scope, llam_now_ns() + UINT64_C(1000000),
                                  &test.report, sizeof(test.report)) == -1 && errno == EBUSY);
    CHECK(test.report.flags & LLAM_SCOPE_REPORT_F_NEEDS_RUNNER);
    llam_runtime_destroy(test.runtime);
    test.runtime = NULL;
    CHECK(atomic_load(&test.arguments_dropped) == 1U);
    CHECK(take_result(&test, LLAM_OUTCOME_RUNTIME_ERROR));
    return case_destroy(&test);
}

static void *held_callback(void *data) {
    owned_argument_t *argument = data;
    scope_case_t *test = argument->test;
    atomic_store(&test->entered, 1U);
    uint64_t limit = llam_now_ns() + UINT64_C(5000000000);
    while (!atomic_load(&test->release) && llam_now_ns() < limit) {
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000L};
        nanosleep(&pause, NULL);
    }
    task_check(test, argument->number == UINT64_C(0x12345678));
    atomic_store(&test->callback_returned, 1U);
    return NULL;
}

static void blocking_child(void *data) {
    owned_argument_t *argument = data;
    scope_case_t *test = argument->test;
    void *result = NULL;
    int status = llam_call_blocking_result(held_callback, argument, &result);
    task_check(test, status == 0 || errno == ECANCELED);
    llam_outcome_t outcome = {.kind = status == 0 ? LLAM_OUTCOME_OK : LLAM_OUTCOME_CANCELLED};
    task_check(test, llam_task_set_outcome_ex(&outcome, sizeof(outcome)) == 0);
}

static void blocking_parent(void *data) {
    scope_case_t *test = data;
    task_check(test, spawn_child(test, blocking_child, 0U));
    uint64_t limit = llam_now_ns() + UINT64_C(2000000000);
    while (!atomic_load(&test->entered) && llam_now_ns() < limit) {
        if (llam_sleep_ns(UINT64_C(1000000)) != 0) break;
    }
    task_check(test, atomic_load(&test->entered) != 0U);
    uint64_t budget = test->timed_cleanup ? UINT64_C(20000000) : UINT64_C(2000000000);
    atomic_store(&test->drain_started, 1U);
    if (test->timed_cleanup)
        test->drain_result = llam_scope_cancel_and_drain_until(test->scope, llam_now_ns() + budget,
                                                              &test->report, sizeof(test->report));
    else test->drain_result = llam_scope_drain_until(test->scope, llam_now_ns() + budget,
                                                    &test->report, sizeof(test->report));
    test->drain_error = errno;
    atomic_store(&test->drain_done, 1U);
}

static void peer_task(void *data) {
    scope_case_t *test = data;
    atomic_store(&test->peer_progress, 1U);
}

static void *runtime_runner(void *data) {
    scope_case_t *test = data;
    test->run_result = llam_runtime_run_handle(test->runtime);
    return NULL;
}

static bool host_wait(atomic_uint *flag) {
    uint64_t limit = llam_now_ns() + UINT64_C(5000000000);
    while (!atomic_load(flag) && llam_now_ns() < limit) {
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000L};
        nanosleep(&pause, NULL);
    }
    return atomic_load(flag) != 0U;
}

static bool exercise_blocking_drain(scope_case_t *test) {
    llam_spawn_opts_t opts;
    CHECK(llam_spawn_opts_init(&opts, sizeof(opts)) == 0);
    opts.cancel_token = test->parent_token;
    if (!test->timed_cleanup) opts.deadline_ns = llam_now_ns() + UINT64_C(100000000);
    test->parent = llam_runtime_spawn_ex(test->runtime, blocking_parent, test, &opts, sizeof(opts));
    llam_task_t *peer = llam_runtime_spawn_ex(test->runtime, peer_task, test, NULL, 0U);
    CHECK(test->parent != NULL && peer != NULL);
    pthread_t runner;
    CHECK(pthread_create(&runner, NULL, runtime_runner, test) == 0);
    bool passed = host_wait(&test->drain_started);
    if (test->timed_cleanup) {
        passed = host_wait(&test->drain_done) && passed;
        passed = passed && test->drain_result == -1 && test->drain_error == ETIMEDOUT;
        passed = passed && test->report.pending_tasks == 1U;
        passed = passed && llam_scope_destroy(test->scope) == -1 && errno == EBUSY;
    } else {
        passed = llam_cancel_token_cancel(test->parent_token) == 0 && passed;
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 150000000L};
        nanosleep(&pause, NULL);
        passed = passed && !atomic_load(&test->drain_done) && atomic_load(&test->peer_progress);
        llam_scope_report_t other;
        passed = passed && llam_scope_drain_until(test->scope, 0U, &other, sizeof(other)) == -1 && errno == EBUSY;
    }
    passed = passed && atomic_load(&test->arguments_dropped) == 0U;
    atomic_store(&test->release, 1U); /* Always let the callback retire, including failed checks. */
    CHECK(pthread_join(runner, NULL) == 0);
    CHECK(passed && test->run_result == 0);
    CHECK(llam_join(test->parent) == 0 && llam_join(peer) == 0);
    CHECK(atomic_load(&test->arguments_dropped) == 1U && atomic_load(&test->callback_returned));
    return true;
}

static bool blocking_retirement(bool timeout) {
    scope_case_t test;
    CHECK(case_create(&test, 0U));
    test.timed_cleanup = timeout;
    test.parent_token = llam_runtime_cancel_token_create(test.runtime);
    CHECK(test.parent_token != NULL);
    CHECK(exercise_blocking_drain(&test));
    if (!timeout) CHECK(test.drain_result == 0 && llam_cancel_token_is_cancelled(test.parent_token) == 1);
    CHECK(llam_scope_drain_until(test.scope, 0U, &test.report, sizeof(test.report)) == 0);
    CHECK(take_result(&test, timeout ? LLAM_OUTCOME_CANCELLED : LLAM_OUTCOME_OK));
    return case_destroy(&test);
}

static void self_drain_entry(void *data) {
    owned_argument_t *argument = data;
    scope_case_t *test = argument->test;
    task_check(test, llam_scope_drain_until(test->scope, 0U, NULL, 0U) == -1 && errno == EDEADLK);
    task_check(test, llam_scope_cancel_and_drain_until(test->scope, 0U, NULL, 0U) == -1 && errno == EDEADLK);
    task_check(test, llam_detach(llam_current_task()) == -1 && errno == EBUSY);
    task_check(test, llam_join(llam_current_task()) == -1 && errno == EBUSY);
    llam_scope_report_t report;
    task_check(test, llam_scope_get_report_ex(test->scope, &report, sizeof(report)) == 0);
    task_check(test, report.state == LLAM_SCOPE_OPEN && report.flags == 0U);
}

static bool self_drain_refused(void) {
    scope_case_t test;
    CHECK(case_create(&test, 0U));
    CHECK(spawn_child(&test, self_drain_entry, 0U));
    CHECK(llam_runtime_run_handle(test.runtime) == 0);
    CHECK(take_result(&test, LLAM_OUTCOME_OK));
    return case_destroy(&test);
}

static bool cancel_during_cleanup(void) { return blocking_retirement(false); }
static bool timeout_during_callback(void) { return blocking_retirement(true); }

typedef struct spawn_race {
    scope_case_t *test;
    llam_value_t *argument;
    unsigned point;
    atomic_uint entered;
    atomic_uint release;
    int result;
    int error;
} spawn_race_t;

static void hold_spawn(unsigned point, void *data) {
    spawn_race_t *race = data;
    if (point != race->point) return;
    atomic_store(&race->entered, 1U);
    if (!host_wait(&race->release)) task_check(race->test, false);
}

static void *spawn_from_host(void *data) {
    spawn_race_t *race = data;
    race->result = spawn_value(race->test, typed_entry, race->argument, &race->test->future);
    race->error = errno;
    return NULL;
}

static bool close_races_spawn(unsigned point) {
    scope_case_t test;
    CHECK(case_create(&test, 0U));
    test.contract.success = number_type;
    test.contract.error = error_type;
    spawn_race_t race = {.test = &test, .argument = new_argument(&test, 0U), .point = point};
    CHECK(race.argument != NULL);
    atomic_init(&race.entered, 0U);
    atomic_init(&race.release, 0U);
    llam_scope_test_spawn_hook(hold_spawn, &race);
    pthread_t producer;
    CHECK(pthread_create(&producer, NULL, spawn_from_host, &race) == 0);
    bool passed = host_wait(&race.entered) && llam_scope_cancel(test.scope) == 0;
    int drained = llam_scope_drain_until(test.scope, 0U, &test.report, sizeof(test.report));
    passed = passed && (point == 1U ? drained == 0 : (drained == -1 && errno == ETIMEDOUT));
    passed = passed && llam_scope_destroy(test.scope) == -1 && errno == EBUSY;
    atomic_store(&race.release, 1U);
    CHECK(pthread_join(producer, NULL) == 0);
    llam_scope_test_spawn_hook(NULL, NULL);
    CHECK(passed);
    if (point == 1U) {
        CHECK(race.result == -1 && race.error == EPIPE && test.future == NULL);
        CHECK(llam_value_release(race.argument) == 0);
    } else {
        CHECK(race.result == 0 && test.future != NULL);
        CHECK(llam_runtime_run_handle(test.runtime) == 0);
        CHECK(take_result(&test, LLAM_OUTCOME_OK));
    }
    CHECK(atomic_load(&test.arguments_dropped) == 1U);
    return case_destroy(&test);
}

static bool close_before_reservation(void) { return close_races_spawn(1U); }
static bool close_after_reservation(void) { return close_races_spawn(2U); }

static void wrong_owner_task(void *data) {
    scope_case_t *test = data;
    llam_scope_report_t report;
    llam_outcome_t outcome;
    task_check(test, llam_scope_get_report_ex(test->scope, &report, sizeof(report)) == -1 && errno == EXDEV);
    task_check(test, llam_scope_cancel(test->scope) == -1 && errno == EXDEV);
    task_check(test, llam_future_wait_until(test->future, 0U) == -1 && errno == EXDEV);
    task_check(test, llam_future_take_ex(test->future, &test->contract, sizeof(test->contract),
                                        &outcome, sizeof(outcome)) == -1 && errno == EXDEV);
    task_check(test, llam_future_release(test->future) == -1 && errno == EXDEV);
}

static bool peer_runtime_isolation(void) {
    scope_case_t test;
    CHECK(case_create(&test, 0U));
    test.contract.success = number_type;
    test.contract.error = error_type;
    CHECK(spawn_child(&test, typed_entry, 0U));
    CHECK(llam_runtime_run_handle(test.runtime) == 0);
    llam_runtime_t *peer;
    CHECK(llam_runtime_create_compat(NULL, 0U, &peer) == 0);
    llam_task_t *task = llam_runtime_spawn_ex(peer, wrong_owner_task, &test, NULL, 0U);
    CHECK(task != NULL && llam_runtime_run_handle(peer) == 0 && llam_join(task) == 0);
    llam_runtime_destroy(peer);
    CHECK(llam_scope_get_report_ex(test.scope, &test.report, sizeof(test.report)) == 0);
    CHECK(test.report.state == LLAM_SCOPE_OPEN && test.report.flags == 0U);
    CHECK(take_result(&test, LLAM_OUTCOME_OK));
    return case_destroy(&test);
}

typedef struct take_race {
    scope_case_t *test;
    atomic_uint *ready;
    atomic_uint *go;
    llam_outcome_t outcome;
    int result;
    int error;
} take_race_t;

static void *take_from_host(void *data) {
    take_race_t *race = data;
    atomic_fetch_add(race->ready, 1U);
    (void)host_wait(race->go);
    race->result = llam_future_take_ex(race->test->future, &race->test->contract,
        sizeof(race->test->contract), &race->outcome, sizeof(race->outcome));
    race->error = errno;
    return NULL;
}

static bool race_consumers(scope_case_t *test) {
    atomic_uint ready, go;
    atomic_init(&ready, 0U);
    atomic_init(&go, 0U);
    take_race_t races[2] = {{.test = test, .ready = &ready, .go = &go},
                            {.test = test, .ready = &ready, .go = &go}};
    pthread_t threads[2];
    unsigned started = 0U, successes = 0U;
    for (; started != 2U; ++started) {
        if (pthread_create(&threads[started], NULL, take_from_host, &races[started]) != 0) break;
    }
    uint64_t limit = llam_now_ns() + UINT64_C(1000000000);
    while (started == 2U && atomic_load(&ready) != 2U && llam_now_ns() < limit) {
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000L};
        nanosleep(&pause, NULL);
    }
    atomic_store(&go, 1U);
    for (unsigned i = 0U; i != started; ++i) {
        CHECK(pthread_join(threads[i], NULL) == 0);
        if (races[i].result == 0) {
            successes += 1U;
            CHECK(races[i].outcome.kind == LLAM_OUTCOME_OK);
            CHECK(llam_value_release(races[i].outcome.value) == 0);
        } else CHECK(races[i].error == EBUSY || races[i].error == EINVAL);
    }
    CHECK(started == 2U && successes <= 1U);
    if (successes == 0U) CHECK(take_result(test, LLAM_OUTCOME_OK));
    else test->future = NULL;
    CHECK(atomic_load(&test->results_dropped) == 1U);
    return true;
}

static bool single_consumer_ownership(void) {
    scope_case_t test;
    CHECK(case_create(&test, 0U));
    test.contract.success = number_type;
    test.contract.error = error_type;
    CHECK(spawn_child(&test, typed_entry, 0U));
    CHECK(llam_runtime_run_handle(test.runtime) == 0);
    CHECK(race_consumers(&test));
    return case_destroy(&test);
}

static void close_native(llam_fd_t *fd) {
    if (LLAM_FD_IS_INVALID(*fd)) return;
#if LLAM_PLATFORM_WINDOWS
    closesocket(*fd);
#else
    close(*fd);
#endif
    *fd = LLAM_INVALID_FD;
}

static bool connect_pair(scope_case_t *test) {
    llam_fd_t listener = socket(AF_INET, SOCK_STREAM, 0);
    if (LLAM_FD_IS_INVALID(listener)) return false;
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof(address);
    bool connected = false;
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(listener, 1) != 0 ||
        getsockname(listener, (struct sockaddr *)&address, &length) != 0) goto done;
    test->write_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (LLAM_FD_IS_INVALID(test->write_fd) ||
        connect(test->write_fd, (struct sockaddr *)&address, length) != 0) goto done;
    test->read_fd = accept(listener, NULL, NULL);
    connected = !LLAM_FD_IS_INVALID(test->read_fd);
done:
    close_native(&listener);
    if (!connected) { close_native(&test->write_fd); close_native(&test->read_fd); }
    return connected;
}

static void io_child(void *data) {
    owned_argument_t *argument = data;
    scope_case_t *test = argument->test;
    atomic_store(&test->io_task, llam_current_task());
    atomic_store(&test->entered, 1U);
    ssize_t result = test->native_io ? llam_poll_fd(test->read_fd, POLLIN, -1, NULL)
                                    : llam_read(test->read_fd, &argument->io_byte, 1U);
    task_check(test, result == -1 && errno == ECANCELED);
    atomic_store(&test->callback_returned, 1U);
    llam_outcome_t outcome = {.kind = LLAM_OUTCOME_CANCELLED};
    task_check(test, llam_task_set_outcome_ex(&outcome, sizeof(outcome)) == 0);
}

static bool child_waiting_for_io(scope_case_t *test) {
    llam_task_t *handle = atomic_load(&test->io_task);
    if (handle == NULL) return false;
    llam_task_t *task = llam_task_resolve_public_handle(handle);
    if (task == NULL) return false;
    unsigned reason = atomic_load(&task->wait_reason);
    bool waiting = (reason == LLAM_WAIT_IO && atomic_load(&task->active_io_req) != NULL) ||
                   (!test->native_io && reason == LLAM_WAIT_BLOCKING && atomic_load(&task->active_block_job) != NULL);
    llam_task_end_public_op(task);
    return waiting;
}

static void describe_io_child(scope_case_t *test) {
    llam_task_t *task = llam_task_resolve_public_handle(atomic_load(&test->io_task));
    if (task == NULL) { fprintf(stderr, "scope I/O child handle is no longer live\n"); return; }
    fprintf(stderr, "scope I/O state=%u wait=%u request=%p block=%p node=%p\n",
            atomic_load(&task->state), atomic_load(&task->wait_reason),
            (void *)atomic_load(&task->active_io_req), (void *)atomic_load(&task->active_block_job),
            (void *)atomic_load(&task->active_wait_node));
    llam_task_end_public_op(task);
}

static void io_parent(void *data) {
    scope_case_t *test = data;
    uint64_t limit = llam_now_ns() + UINT64_C(2000000000);
    while (!child_waiting_for_io(test) && llam_now_ns() < limit) {
        if (llam_sleep_ns(UINT64_C(1000000)) != 0) break;
    }
    if (!child_waiting_for_io(test)) describe_io_child(test);
    task_check(test, child_waiting_for_io(test));
    test->drain_result = llam_scope_cancel_and_drain_until(test->scope,
        llam_now_ns() + UINT64_C(1000000000), &test->report, sizeof(test->report));
    task_check(test, test->drain_result == 0 && test->report.pending_tasks == 0U);
    task_check(test, atomic_load(&test->arguments_dropped) == 1U);
}

static bool cancelled_io_case(bool native) {
    scope_case_t test;
    CHECK(case_create(&test, 0U));
    test.native_io = native;
    CHECK(connect_pair(&test));
    CHECK(spawn_child(&test, io_child, 0U));
    test.parent = llam_runtime_spawn_ex(test.runtime, io_parent, &test, NULL, 0U);
    CHECK(test.parent != NULL && llam_runtime_run_handle(test.runtime) == 0);
    CHECK(llam_join(test.parent) == 0 && test.drain_result == 0);
    close_native(&test.read_fd);
    close_native(&test.write_fd);
    CHECK(take_result(&test, LLAM_OUTCOME_CANCELLED));
    return case_destroy(&test);
}

static bool cancelled_io_retirement(void) { return cancelled_io_case(false); }

static void unit_entry(void *data) { (void)data; }

static void capacity_parent(void *data) {
    scope_case_t *test = data;
    task_check(test, spawn_child(test, unit_entry, 0U));
    task_check(test, llam_scope_drain_until(test->scope, UINT64_MAX, NULL, 0U) == 0);
    llam_runtime_resources_t resources;
    task_check(test, llam_runtime_get_resources_ex(test->runtime, &resources, sizeof(resources)) == 0);
    task_check(test, resources.admitted_tasks == 1U); /* Only this parent remains admitted. */
    llam_scope_t *next = NULL;
    task_check(test, llam_runtime_scope_create_ex(test->runtime, NULL, 0U, &next) == 0);
    llam_typed_task_spec_t spec;
    task_check(test, llam_typed_task_spec_init(&spec, sizeof(spec)) == 0);
    spec.entry = unit_entry;
    llam_future_t *future = NULL;
    task_check(test, llam_scope_spawn_ex(next, &spec, sizeof(spec), &future) == 0);
    task_check(test, llam_scope_drain_until(next, UINT64_MAX, NULL, 0U) == 0);
    llam_outcome_t outcome;
    task_check(test, llam_future_take_ex(future, &test->contract, sizeof(test->contract),
                                        &outcome, sizeof(outcome)) == 0);
    task_check(test, outcome.kind == LLAM_OUTCOME_OK && outcome.value == NULL);
    task_check(test, llam_scope_destroy(next) == 0);
}

static bool drain_releases_admission_capacity(void) {
    scope_case_t test;
    CHECK(case_create(&test, 2U));
    test.parent = llam_runtime_spawn_ex(test.runtime, capacity_parent, &test, NULL, 0U);
    CHECK(test.parent != NULL && llam_runtime_run_handle(test.runtime) == 0);
    CHECK(llam_join(test.parent) == 0);
    CHECK(take_result(&test, LLAM_OUTCOME_OK));
    return case_destroy(&test);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--io-native") == 0) return cancelled_io_case(true) ? 0 : 1;
    for (unsigned mode = 0U; mode != 5U; ++mode) {
        fprintf(stderr, "scope outcome case %u\n", mode);
        if (!outcome_and_scope_lifetime(mode)) return 1;
    }
    const struct { const char *name; bool (*run)(void); } cases[] = {
        {"admission rollback", admission_failure_preserves_argument},
        {"already cancelled parent", cancelled_parent_uses_cleanup_budget},
        {"unbounded cancelled parent", cancelled_parent_unbounded},
        {"dropped future", dropped_future_is_still_scoped},
        {"unstarted shutdown", unstarted_runtime_shutdown},
        {"cancel during cleanup", cancel_during_cleanup},
        {"timeout during callback", timeout_during_callback},
        {"self drain", self_drain_refused},
        {"close before reservation", close_before_reservation},
        {"close after reservation", close_after_reservation},
        {"peer runtime", peer_runtime_isolation},
        {"single consumer", single_consumer_ownership},
        {"cancelled I/O", cancelled_io_retirement},
        {"admission capacity after drain", drain_releases_admission_capacity}
    };
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        fprintf(stderr, "scope case: %s\n", cases[i].name);
        if (!cases[i].run()) return 1;
    }
    puts("scopes: typed outcomes, cancellation shielding, deadlines, callback retirement and ownership passed");
    return 0;
}
