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
 * See LICENSES/Apache-2.0.txt.
 */

#include "runtime_internal.h"
#include "managed_catalog.h"
#include "managed_scheduler.h"
#include "managed_async_adapter.h"
#include "runtime_test_managed_foreign.h"
#include "llam/managed.h"
#include "../tools/preempt/metadata_v2.h"

#include <stdio.h>

#if !defined(LLAM_ENABLE_TEST_HOOKS)
#error "managed foreign tests require test-only catalog registration"
#endif

#if defined(_MSC_VER)
#define TEST_NOINLINE __declspec(noinline)
#else
#define TEST_NOINLINE __attribute__((noinline))
#endif

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "test_managed_foreign_internal:%d: %s (errno=%d)\n", \
                __LINE__, #condition, errno); \
        return -1; \
    } \
} while (0)

typedef enum foreign_case {
    FOREIGN_NORMAL,
    FOREIGN_CANCEL_RUNNING,
    FOREIGN_STOP_RUNNING,
    FOREIGN_CANCEL_BEFORE,
    FOREIGN_STOP_BEFORE,
    FOREIGN_CAPTURE_GATE
} foreign_case_t;

typedef struct foreign_test {
    llam_runtime_t *runtime;
    llam_cancel_token_t *token;
    foreign_case_t mode;
    llam_task_local_key_t cleanup_key;
    atomic_uint cleanups;
    atomic_uint cleanup_gate_checked;
    atomic_uint failures;
    atomic_uint forbidden_calls;
    atomic_uint callback_started;
    atomic_uint callback_release;
    atomic_uint peer_progress;
    atomic_uint affine_returned;
    atomic_uint run_done;
    atomic_uint destroy_done;
    atomic_uint gate_checked;
    atomic_uint gate_reached;
    atomic_uint gate_release;
    llam_worker_t *origin_worker;
    llam_processor_t *origin_processor;
    pthread_t origin_thread;
    int run_result;
} foreign_test_t;

typedef struct foreign_frame {
    foreign_test_t *test;
    uintptr_t *tls_address;
    uint64_t value;
} foreign_frame_t;

static _Thread_local uintptr_t native_cookie;

static void task_check_at(foreign_test_t *test, bool passed, unsigned line) {
    if (!passed && atomic_fetch_add_explicit(&test->failures, 1U, memory_order_relaxed) < 16U)
        fprintf(stderr, "managed foreign case %u failed at line %u\n", (unsigned)test->mode, line);
}
#define task_check(test, passed) task_check_at((test), (passed), __LINE__)

static void host_pause(void) {
    const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
    (void)nanosleep(&delay, NULL);
}

static bool await_flag(atomic_uint *flag, const atomic_uint *finished) {
    uint64_t deadline = llam_now_ns() + UINT64_C(5000000000);
    while (atomic_load_explicit(flag, memory_order_acquire) == 0U &&
           (finished == NULL || atomic_load_explicit(finished, memory_order_acquire) == 0U) &&
           llam_now_ns() < deadline) host_pause();
    return atomic_load_explicit(flag, memory_order_acquire) != 0U;
}

#if LLAM_ASYNC_CONTEXT_SHARED
static void observe_foreign_gate(llam_task_t *task, unsigned point, void *data) {
    foreign_test_t *test = data;
    llam_worker_t *worker = g_llam_tls_worker;
    task_check(test, task == g_llam_tls_task && worker == test->origin_worker &&
                     g_llam_tls_processor != NULL && llam_worker_load_active_g(worker) == task);
    task_check(test, atomic_load_explicit(&worker->preempt_disable_depth, memory_order_acquire) == 1U &&
                     atomic_load_explicit(&worker->runtime_critical_depth, memory_order_acquire) == 0U &&
                     atomic_load_explicit(&worker->execution_state, memory_order_acquire) == LLAM_WORKER_EXEC_RUNTIME_UNSAFE);
    if (atomic_load_explicit(&test->callback_started, memory_order_acquire) != 0U) {
        /* Exit cleanup repeats the capture gate after the first callback has
         * returned. It must retain the same admission refusal while gated. */
        task_check(test, atomic_load_explicit(&test->affine_returned, memory_order_acquire) == 1U);
        if (point == LLAM_MANAGED_FOREIGN_TEST_QUIESCENT) {
            task_check(test, llam_managed_async_test_capture_admission(task) == 0);
            atomic_store_explicit(&test->cleanup_gate_checked, 1U, memory_order_release);
        }
        return;
    }
    task_check(test, atomic_load_explicit(&test->callback_started, memory_order_acquire) == 0U);
    if (point != LLAM_MANAGED_FOREIGN_TEST_QUIESCENT) return;
    /* Exercise actual controller admission after the successful query and
     * before P return, even though the current runtime PC itself is unsafe. */
    uint64_t deadline = llam_now_ns() + UINT64_C(5000000000);
    int admitted;
    do {
        admitted = llam_managed_async_test_capture_admission(task);
        if (admitted >= 0 || errno != EAGAIN) break;
        host_pause();
    } while (llam_now_ns() < deadline);
    task_check(test, admitted == 0);
    if (admitted == 0) atomic_store_explicit(&test->gate_checked, 1U, memory_order_release);
    atomic_store_explicit(&test->gate_reached, 1U, memory_order_release);
    task_check(test, await_flag(&test->gate_release, NULL));
    task_check(test, atomic_load_explicit(&test->callback_started, memory_order_acquire) == 0U);
}
#endif

/* This fixture selects scheduler classes only. It supplies no SAFE ranges and
 * uses cooperative policy; compiler/image and native capture have other tests. */
static int register_task(llam_runtime_t *runtime, llam_task_fn function, uint32_t execution_class) {
    llam_llpm2_entry_t entry = {0};
    llam_llpm2_t record = {0};
    llam_module_t *module = NULL;
    record.header.architecture = LLAM_ARCH_AARCH64 ? 2U : 1U;
    record.header.native_abi = LLAM_PLATFORM_WINDOWS ? LLAM_LLPM2_ABI_WIN64 :
        (LLAM_PLATFORM_DARWIN ? LLAM_LLPM2_ABI_DARWIN : LLAM_LLPM2_ABI_SYSV);
    entry.native_entry = (uintptr_t)function;
    entry.managed_entry = (uintptr_t)function;
    entry.code_begin = (uintptr_t)function;
    entry.code_end = (uintptr_t)function + 1U;
    entry.execution_class = execution_class;
    entry.effects = (1U << LLAM_LLPM2_EFFECT_MANAGED) |
        (execution_class == LLAM_MANAGED_EXECUTION_NATIVE_AFFINE ? 1U << LLAM_LLPM2_EFFECT_NATIVE_AFFINE : 0U);
    entry.type_id = llam_managed_task_type_id(record.header.architecture, record.header.native_abi);
    entry.required_isa = LLAM_LLPM2_ISA_BASE;
    record.entries = &entry;
    record.entry_count = 1U;
    return llam_managed_catalog_test_register(runtime, &record, &module);
}

static void *forbidden_callback(void *argument) {
    foreign_test_t *test = argument;
    atomic_fetch_add_explicit(&test->forbidden_calls, 1U, memory_order_relaxed);
    return argument;
}

static void *native_callback(void *argument) {
    foreign_frame_t *frame = argument;
    foreign_test_t *test = frame->test;
    int initial_errno = errno;
#if LLAM_PLATFORM_WINDOWS
    DWORD initial_last_error = GetLastError();
#endif
    task_check(test, initial_errno == EDOM);
#if LLAM_PLATFORM_WINDOWS
    task_check(test, initial_last_error == 0x4567U);
#endif
    task_check(test, pthread_equal(pthread_self(), test->origin_thread) != 0);
    task_check(test, &native_cookie == frame->tls_address && native_cookie == UINT64_C(0x13579));
    task_check(test, g_llam_tls_worker == test->origin_worker && g_llam_tls_task == NULL &&
                     g_llam_tls_processor == NULL);
    task_check(test, atomic_load_explicit(&test->origin_worker->current_p, memory_order_acquire) == NULL &&
                     llam_worker_load_active_g(test->origin_worker) == NULL);
    task_check(test, atomic_load_explicit(&test->origin_worker->execution_state, memory_order_acquire) ==
                     LLAM_WORKER_EXEC_FOREIGN && llam_native_domain_blocked());
    task_check(test, atomic_load_explicit(&test->origin_worker->preempt_disable_depth, memory_order_acquire) == 1U);
    if (test->mode == FOREIGN_CAPTURE_GATE)
        task_check(test, atomic_load_explicit(&test->gate_checked, memory_order_acquire) == 1U);
    errno = 0;
    task_check(test, llam_current_task() == NULL && errno == ENOTSUP);
    errno = 0;
    task_check(test, llam_sleep_ns(1U) == -1 && errno == ENOTSUP);
    void *nested_result = frame;
    errno = 0;
    task_check(test, llam_call_native_affine_result(forbidden_callback, test, &nested_result) == -1 &&
                     errno == ENOTSUP && nested_result == frame);
    frame->value ^= UINT64_C(0xfedcba9876543210);
    native_cookie = UINT64_C(0x2468a);
    atomic_store_explicit(&test->callback_started, 1U, memory_order_release);
    /* One portable P must be usable by a different M while this dedicated M
     * waits in native code. Both waits have a deadline so a failed release
     * becomes a test failure rather than an infinite foreign callback. */
    task_check(test, await_flag(&test->peer_progress, NULL));
    task_check(test, await_flag(&test->callback_release, NULL));
    task_check(test, atomic_load_explicit(&test->affine_returned, memory_order_acquire) == 0U);
#if LLAM_PLATFORM_DARWIN && LLAM_ASYNC_CONTEXT_SHARED
    if (test->mode == FOREIGN_CAPTURE_GATE) {
        sigset_t temporary;
        sigemptyset(&temporary);
        /* Native code may temporarily change masks with capture closed. */
        task_check(test, pthread_sigmask(SIG_SETMASK, &temporary, NULL) == 0);
    }
#endif
    errno = EILSEQ;
#if LLAM_PLATFORM_WINDOWS
    SetLastError(0x5678U);
#endif
    return frame;
}

/* A fresh frame avoids hoisting scheduler TLS addresses across a cooperative
 * context switch on Darwin. Native TLS itself is intentionally tested for the
 * affine task, whose M must not change. */
static TEST_NOINLINE void inspect_resumed(foreign_test_t *test, llam_task_t *task) {
    llam_worker_t *worker = g_llam_tls_worker;
    llam_processor_queue_access_t access;
#if LLAM_PLATFORM_DARWIN && LLAM_ASYNC_CONTEXT_SHARED
    if (test->mode == FOREIGN_CAPTURE_GATE) {
        sigset_t restored;
        task_check(test, pthread_sigmask(SIG_SETMASK, NULL, &restored) == 0 &&
            sigismember(&restored, LLAM_PREEMPT_SIGNAL) == 1 && sigismember(&restored, SIGUSR2) == 1);
    }
#endif
    task_check(test, worker == test->origin_worker && g_llam_tls_task == task &&
                     g_llam_tls_processor != NULL);
    if (worker == NULL || g_llam_tls_processor == NULL) return;
    task_check(test, atomic_load_explicit(&worker->current_p, memory_order_acquire) == g_llam_tls_processor &&
                     llam_worker_load_active_g(worker) == task);
    task_check(test, atomic_load_explicit(&worker->native_domain_depth, memory_order_acquire) == 0U);
    task_check(test, atomic_load_explicit(&worker->preempt_disable_depth, memory_order_acquire) == 0U &&
                     atomic_load_explicit(&worker->execution_state, memory_order_acquire) == LLAM_WORKER_EXEC_MANAGED_SAFE);
    task_check(test, llam_processor_queue_access_snapshot(g_llam_tls_processor, &access) &&
                     (access.mode == LLAM_PROCESSOR_QUEUE_ACCESS_EXECUTION ||
                      access.mode == LLAM_PROCESSOR_QUEUE_ACCESS_STOPPING_DRAIN));
}

static void affine_cleanup(void *argument) {
    foreign_test_t *test = argument;
    void *out = test;
    task_check(test, pthread_equal(pthread_self(), test->origin_thread) != 0);
    task_check(test, llam_call_blocking_result(forbidden_callback, test, &out) == -1 &&
                     errno == ENOTSUP && out == test);
    atomic_fetch_add_explicit(&test->cleanups, 1U, memory_order_relaxed);
}

static void affine_task(void *argument) {
    foreign_test_t *test = argument;
    llam_task_t *task = g_llam_tls_task;
    llam_processor_owner_ticket_t before = {0};
    test->origin_worker = g_llam_tls_worker;
    test->origin_processor = g_llam_tls_processor;
    test->origin_thread = pthread_self();
    task_check(test, llam_task_local_key_create(&test->cleanup_key) == 0);
    task_check(test, llam_task_local_set_owned(test->cleanup_key, test, affine_cleanup) == 0);
    task_check(test, task != NULL && task->managed_execution_class == LLAM_MANAGED_EXECUTION_NATIVE_AFFINE &&
                     test->origin_worker != NULL && test->origin_worker->role == LLAM_WORKER_ROLE_NATIVE_AFFINE &&
                     task->managed_execution_worker == test->origin_worker);
    task_check(test, llam_processor_owner_ticket_snapshot(test->origin_processor, &before));
    void *result = test;
    errno = 0;
    task_check(test, llam_call_native_affine_result(NULL, test, &result) == -1 && errno == EINVAL && result == NULL);
    errno = 0;
    task_check(test, llam_call_native_affine_result(forbidden_callback, test, NULL) == -1 && errno == EINVAL);
    if (test->mode == FOREIGN_CANCEL_BEFORE || test->mode == FOREIGN_STOP_BEFORE) {
        int stopped = test->mode == FOREIGN_CANCEL_BEFORE ? llam_cancel_token_cancel(test->token)
                                                         : llam_runtime_request_stop();
        task_check(test, stopped == 0);
        result = test;
        errno = 0;
        task_check(test, llam_call_native_affine_result(forbidden_callback, test, &result) == -1 &&
                         errno == ECANCELED && result == NULL);
        atomic_store_explicit(&test->affine_returned, 1U, memory_order_release);
        return;
    }
    native_cookie = UINT64_C(0x13579);
    foreign_frame_t frame = {test, &native_cookie, UINT64_C(0x0123456789abcdef)};
    errno = EDOM;
#if LLAM_PLATFORM_WINDOWS
    SetLastError(0x4567U);
#endif
    int status = llam_call_native_affine_result(native_callback, &frame, &result);
    int callback_errno = errno;
#if LLAM_PLATFORM_WINDOWS
    DWORD callback_last_error = GetLastError();
#endif
    task_check(test, status == 0 && result == &frame && callback_errno == EILSEQ);
#if LLAM_PLATFORM_WINDOWS
    task_check(test, callback_last_error == 0x5678U);
#endif
    task_check(test, frame.value == (UINT64_C(0x0123456789abcdef) ^ UINT64_C(0xfedcba9876543210)));
    task_check(test, pthread_equal(pthread_self(), test->origin_thread) != 0 &&
                     &native_cookie == frame.tls_address && native_cookie == UINT64_C(0x2468a));
    inspect_resumed(test, task);
    if (test->mode != FOREIGN_STOP_RUNNING) {
        llam_processor_owner_ticket_t after = {0};
        task_check(test, llam_processor_owner_ticket_snapshot(g_llam_tls_processor, &after) &&
                         !llam_processor_owner_ticket_equal(&before, &after));
    }
    atomic_store_explicit(&test->affine_returned, 1U, memory_order_release);
}

static TEST_NOINLINE void portable_inspect(foreign_test_t *test) {
    task_check(test, g_llam_tls_worker != test->origin_worker &&
                     g_llam_tls_processor == test->origin_processor);
}

static void affine_cleanup(void *argument);

static void portable_task(void *argument) {
    foreign_test_t *test = argument;
    task_check(test, llam_task_local_set_owned(LLAM_TASK_LOCAL_INVALID_KEY, test, affine_cleanup) == -1 &&
                     errno == ENOTSUP);
    void *result = test;
    errno = 0;
    task_check(test, llam_call_native_affine_result(forbidden_callback, test, &result) == -1 &&
                     errno == ENOTSUP && result == NULL);
    uint64_t deadline = llam_now_ns() + UINT64_C(5000000000);
    while (atomic_load_explicit(&test->callback_started, memory_order_acquire) == 0U &&
           atomic_load_explicit(&test->affine_returned, memory_order_acquire) == 0U &&
           llam_now_ns() < deadline) llam_yield();
    if (atomic_load_explicit(&test->callback_started, memory_order_acquire) != 0U) {
        portable_inspect(test);
        atomic_store_explicit(&test->peer_progress, 1U, memory_order_release);
    } else {
        task_check(test, atomic_load_explicit(&test->affine_returned, memory_order_acquire) != 0U);
    }
}

static void setup_task(void *argument) {
    foreign_test_t *test = argument;
    test->token = llam_cancel_token_create();
    task_check(test, test->token != NULL);
    if (test->token == NULL) return;
    llam_spawn_opts_t options;
    task_check(test, llam_spawn_opts_init(&options, sizeof(options)) == 0);
    options.cancel_token = test->token;
    llam_task_t *affine = llam_spawn_ex(affine_task, test, &options, sizeof(options));
    llam_task_t *portable = llam_spawn(portable_task, test, NULL);
    task_check(test, affine != NULL && portable != NULL);
    if (affine != NULL) task_check(test, llam_detach(affine) == 0);
    if (portable != NULL) task_check(test, llam_detach(portable) == 0);
}

static void *run_runtime(void *argument) {
    foreign_test_t *test = argument;
    test->run_result = llam_runtime_run_handle(test->runtime);
    atomic_store_explicit(&test->run_done, 1U, memory_order_release);
    return NULL;
}

static void *destroy_runtime(void *argument) {
    foreign_test_t *test = argument;
    llam_runtime_destroy(test->runtime);
    atomic_store_explicit(&test->destroy_done, 1U, memory_order_release);
    return NULL;
}

static int host_rejections(void) {
    foreign_test_t test = {0};
    void *result = &test;
    errno = 0;
    CHECK(llam_call_native_affine_result(forbidden_callback, &test, &result) == -1 &&
          errno == ENOTSUP && result == NULL);
    result = &test;
    errno = 0;
    CHECK(llam_call_native_affine_result(NULL, &test, &result) == -1 && errno == EINVAL && result == NULL);
    errno = 0;
    CHECK(llam_call_native_affine_result(forbidden_callback, &test, NULL) == -1 && errno == EINVAL);
    CHECK(atomic_load_explicit(&test.forbidden_calls, memory_order_relaxed) == 0U);
    return 0;
}

static int run_case(foreign_case_t mode) {
    foreign_test_t test = {.mode = mode};
    llam_runtime_opts_t options;
    llam_preempt_opts_t policy;
    pthread_t driver, destroyer;
    bool destroying = false;
    CHECK(llam_runtime_opts_init(&options, sizeof(options)) == 0);
    CHECK(llam_preempt_opts_init(&policy, sizeof(policy)) == 0);
    options.deterministic = 1U;
    options.preempt_mode = LLAM_PREEMPT_COOPERATIVE;
    policy.policy = LLAM_PREEMPT_POLICY_COOPERATIVE;
    policy.native_affine_capacity = 1U;
#if LLAM_ASYNC_CONTEXT_SHARED
    if (mode == FOREIGN_CAPTURE_GATE) {
        options.preempt_mode = LLAM_PREEMPT_STRICT;
        policy.policy = LLAM_PREEMPT_POLICY_ASYNC_REQUIRED;
    }
#endif
    CHECK(llam_runtime_create_managed(&options, sizeof(options), &policy, sizeof(policy), &test.runtime) == 0);
    CHECK(test.runtime->portable_processor_count == 1U);
    CHECK(register_task(test.runtime, setup_task, LLAM_MANAGED_EXECUTION_PORTABLE) == 0);
    CHECK(register_task(test.runtime, portable_task, LLAM_MANAGED_EXECUTION_PORTABLE) == 0);
    CHECK(register_task(test.runtime, affine_task, LLAM_MANAGED_EXECUTION_NATIVE_AFFINE) == 0);
    llam_task_t *setup = llam_runtime_spawn_ex(test.runtime, setup_task, &test, NULL, 0U);
    CHECK(setup != NULL && llam_detach(setup) == 0);
#if LLAM_ASYNC_CONTEXT_SHARED
    llam_managed_foreign_test_gate_t gate = {test.runtime, observe_foreign_gate, &test};
    if (mode == FOREIGN_CAPTURE_GATE) llam_managed_foreign_test_set_gate(&gate);
#endif
    CHECK(pthread_create(&driver, NULL, run_runtime, &test) == 0);
    bool started = true, progressed = true, held = true, stopped = true;
    if (mode == FOREIGN_CAPTURE_GATE) {
        held = await_flag(&test.gate_reached, &test.run_done) &&
            atomic_load_explicit(&test.gate_checked, memory_order_acquire) == 1U &&
            atomic_load_explicit(&test.callback_started, memory_order_acquire) == 0U;
        atomic_store_explicit(&test.gate_release, 1U, memory_order_release);
    }
    if (mode != FOREIGN_CANCEL_BEFORE && mode != FOREIGN_STOP_BEFORE) {
        started = await_flag(&test.callback_started, &test.run_done);
        progressed = started && await_flag(&test.peer_progress, &test.run_done);
        if (started && mode == FOREIGN_CANCEL_RUNNING) {
            task_check(&test, llam_cancel_token_cancel(test.token) == 0);
        } else if (started && mode == FOREIGN_STOP_RUNNING) {
            CHECK(pthread_create(&destroyer, NULL, destroy_runtime, &test) == 0);
            destroying = true;
            uint64_t deadline = llam_now_ns() + UINT64_C(5000000000);
            while (!atomic_load_explicit(&test.runtime->shutdown_requested, memory_order_acquire) &&
                   atomic_load_explicit(&test.destroy_done, memory_order_acquire) == 0U &&
                   llam_now_ns() < deadline) host_pause();
            stopped = atomic_load_explicit(&test.runtime->shutdown_requested, memory_order_acquire);
            held = atomic_load_explicit(&test.destroy_done, memory_order_acquire) == 0U;
        }
        held = held && atomic_load_explicit(&test.affine_returned, memory_order_acquire) == 0U;
        atomic_store_explicit(&test.callback_release, 1U, memory_order_release);
    }
    CHECK(pthread_join(driver, NULL) == 0);
    llam_managed_foreign_test_set_gate(NULL);
    if (destroying) CHECK(pthread_join(destroyer, NULL) == 0);
    bool completed = atomic_load_explicit(&test.affine_returned, memory_order_acquire) != 0U;
    unsigned failures = atomic_load_explicit(&test.failures, memory_order_acquire);
    unsigned forbidden = atomic_load_explicit(&test.forbidden_calls, memory_order_acquire);
    if (test.token != NULL) CHECK(llam_cancel_token_destroy(test.token) == 0);
    if (!destroying) llam_runtime_destroy(test.runtime);
    CHECK(started && progressed && held && stopped && completed && test.run_result == 0);
    CHECK(atomic_load_explicit(&test.cleanups, memory_order_acquire) == 1U);
    if (mode == FOREIGN_CAPTURE_GATE)
        CHECK(atomic_load_explicit(&test.cleanup_gate_checked, memory_order_acquire) == 1U);
    CHECK(llam_task_local_key_delete(test.cleanup_key) == 0);
    CHECK(failures == 0U && forbidden == 0U);
    return 0;
}

int main(void) {
    if (host_rejections() != 0) return 1;
    for (unsigned mode = FOREIGN_NORMAL; mode <= FOREIGN_STOP_BEFORE; ++mode) {
        if (run_case((foreign_case_t)mode) != 0) return 1;
    }
#if LLAM_ASYNC_CONTEXT_SHARED
    if (run_case(FOREIGN_CAPTURE_GATE) != 0) return 1;
#endif
    puts("test_managed_foreign_internal: native identity, P release, result state and stop/cancel drain passed");
    return 0;
}
