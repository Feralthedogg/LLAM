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
#include "managed_scheduler.h"
#include "managed_catalog.h"
#include "engine/runtime_watchdog_internal.h"
#include "../tools/preempt/metadata_v2.h"
#include "llam/managed.h"

#include <stdio.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "test_managed_scheduler_internal:%d: %s (errno=%d)\n", \
                __LINE__, #condition, errno); \
        return -1; \
    } \
} while (0)

#define TASK_COUNT 64U
#define TASK_ROUNDS 12U
#if defined(_MSC_VER)
#define TEST_NOINLINE __declspec(noinline)
#else
#define TEST_NOINLINE __attribute__((noinline))
#endif

#if LLAM_ARCH_AARCH64 || LLAM_ARCH_X86_64
typedef struct errno_migration_test {
    llam_task_t task;
    llam_task_t peer;
    _Alignas(16) llam_ctx_t source;
    _Alignas(16) llam_ctx_t destination;
    atomic_uint stage;
    unsigned mode;
    int source_error;
    int destination_error;
    _Alignas(16) unsigned char task_stack[65536];
    _Alignas(16) unsigned char peer_stack[65536];
} errno_migration_test_t;

static errno_migration_test_t *errno_migration;

static TEST_NOINLINE int current_thread_error(void) {
    return errno;
}

static void errno_migration_peer(void) {
    errno_migration_test_t *test = errno_migration;
    llam_ctx_switch(&test->peer.ctx, &test->source);
    abort();
}

static void errno_migration_task(void) {
    errno_migration_test_t *test = errno_migration;
    errno = EDOM;
    if (test->mode == 0U) llam_switch_task_to_scheduler(&test->task, &test->source);
    else if (test->mode == 1U) llam_switch_task_to_task(&test->task, &test->peer);
    else llam_switch_task_to_task_hot(&test->task, &test->peer);
    test->destination_error = current_thread_error();
    atomic_store_explicit(&test->stage, 2U, memory_order_release);
    llam_ctx_switch(&test->task.ctx, &test->destination);
    abort();
}

static void errno_migration_context(llam_ctx_t *context, unsigned char *stack,
                                     size_t size, void (*entry)(void)) {
    uintptr_t top = ((uintptr_t)stack + size) & ~(uintptr_t)15U;
#if LLAM_ARCH_AARCH64
    context->sp = top;
    context->lr = (uintptr_t)entry;
#else
    /* RET enters C with the ABI alignment; Windows also needs shadow space. */
    uintptr_t *start = (uintptr_t *)(top - (LLAM_PLATFORM_WINDOWS ? 48U : 16U));
    *start = (uintptr_t)entry;
    context->rsp = (uintptr_t)start;
    context->mxcsr = UINT32_C(0x1f80);
    context->x87_cw = UINT16_C(0x037f);
#endif
}

static void *errno_migration_source(void *argument) {
    errno_migration_test_t *test = argument;
    llam_ctx_switch(&test->source, &test->task.ctx);
    errno = ERANGE;
    /* Destination cannot resume until this native M owns the sentinel. */
    atomic_store_explicit(&test->stage, 1U, memory_order_release);
    while (atomic_load_explicit(&test->stage, memory_order_acquire) != 2U) llam_pause_cpu();
    test->source_error = current_thread_error();
    return NULL;
}

static void *errno_migration_destination(void *argument) {
    errno_migration_test_t *test = argument;
    while (atomic_load_explicit(&test->stage, memory_order_acquire) != 1U) llam_pause_cpu();
    errno = E2BIG;
    llam_ctx_switch(&test->destination, &test->task.ctx);
    return NULL;
}

static int errno_migration_cases(void) {
    for (unsigned mode = 0U; mode < 3U; ++mode) {
        errno_migration_test_t *test = calloc(1U, sizeof(*test));
        pthread_t source, destination;
        CHECK(test != NULL);
        test->mode = mode;
        test->peer.saved_errno = ENOEXEC;
        errno_migration = test;
        errno_migration_context(&test->task.ctx, test->task_stack, sizeof(test->task_stack),
                                 errno_migration_task);
        errno_migration_context(&test->peer.ctx, test->peer_stack, sizeof(test->peer_stack),
                                 errno_migration_peer);
        CHECK(pthread_create(&source, NULL, errno_migration_source, test) == 0);
        CHECK(pthread_create(&destination, NULL, errno_migration_destination, test) == 0);
        CHECK(pthread_join(source, NULL) == 0 && pthread_join(destination, NULL) == 0);
        int source_error = test->source_error, destination_error = test->destination_error;
        free(test);
        errno_migration = NULL;
        if (source_error != ERANGE || destination_error != EDOM)
            fprintf(stderr, "errno migration mode %u: source=%d/%d destination=%d/%d\n",
                mode, source_error, ERANGE, destination_error, EDOM);
        CHECK(source_error == ERANGE && destination_error == EDOM);
    }
    return 0;
}
#else
static int errno_migration_cases(void) { return 0; }
#endif

typedef struct scheduler_test {
    llam_runtime_t *runtime;
    atomic_uint failures;
    atomic_uint entries;
    atomic_uint completed;
    atomic_uint callbacks;
    atomic_uint migrations;
    atomic_uint starts;
    atomic_uint closes;
    atomic_uint gate_reached;
    atomic_uint gate_release;
    atomic_uint run_done;
    atomic_uint children_release;
    atomic_uint children_done;
    _Atomic(llam_task_t *) source_task;
    atomic_uint *processor_active;
    bool gate_enabled;
    bool fail_start;
    bool drain_gate;
#if LLAM_PLATFORM_DARWIN
    bool check_native_masks;
    sigset_t caller_mask;
#endif
    size_t target_slot;
    int run_result;
    int run_error;
} scheduler_test_t;

/* Routing/lifetime fixtures only: these have no SAFE PC ranges and cannot
 * qualify native captures. Compiler/native qualification has separate tests. */
static int register_task_class(llam_runtime_t *runtime, llam_task_fn entry, uint32_t execution_class) {
#if defined(LLAM_ENABLE_TEST_HOOKS)
    llam_llpm2_entry_t descriptor = {0};
    llam_llpm2_t record = {0};
    llam_module_t *module = NULL;
    record.header.architecture = LLAM_ARCH_AARCH64 ? 2U : 1U;
    record.header.native_abi = LLAM_PLATFORM_WINDOWS ? LLAM_LLPM2_ABI_WIN64 :
        (LLAM_PLATFORM_DARWIN ? LLAM_LLPM2_ABI_DARWIN : LLAM_LLPM2_ABI_SYSV);
    descriptor.native_entry = (uintptr_t)entry;
    descriptor.managed_entry = (uintptr_t)entry;
    descriptor.code_begin = (uintptr_t)entry;
    descriptor.code_end = (uintptr_t)entry + 1U;
    descriptor.execution_class = execution_class;
    descriptor.type_id = llam_managed_task_type_id(record.header.architecture, record.header.native_abi);
    descriptor.required_isa = LLAM_LLPM2_ISA_BASE;
    record.entries = &descriptor;
    record.entry_count = 1U;
    return llam_managed_catalog_test_register(runtime, &record, &module);
#else
    (void)runtime; (void)entry; (void)execution_class;
    return 0;
#endif
}

static void task_check_at(scheduler_test_t *test, bool passed, unsigned line) {
    if (!passed && atomic_fetch_add_explicit(&test->failures, 1U, memory_order_relaxed) < 16U)
        fprintf(stderr, "managed scheduler task check failed at line %u\n", line);
}
#define task_check(test, passed) task_check_at((test), (passed), __LINE__)

/* Native TLS addresses can be hoisted for a function's entire activation.
 * Inspect from a fresh frame after migration, never cache a source M's TLV. */
static TEST_NOINLINE uint64_t current_run_generation(void) {
    return atomic_load_explicit(&g_llam_tls_task->run_generation, memory_order_acquire);
}

static TEST_NOINLINE bool current_has_reservation(void) {
    return atomic_load_explicit(&g_llam_tls_task->managed_dispatch_owner,
                                memory_order_acquire) == g_llam_tls_worker;
}

static TEST_NOINLINE void inspect_execution(scheduler_test_t *test,
                               llam_processor_owner_ticket_t *previous,
                               uint64_t *previous_run) {
    llam_shard_t *shard = g_llam_tls_shard;
    llam_worker_t *worker = g_llam_tls_worker;
    llam_processor_owner_ticket_t ticket = {0};
    bool valid = shard != NULL && worker != NULL && g_llam_tls_task != NULL &&
                 shard->runtime == test->runtime &&
                 llam_worker_registered_for(worker, test->runtime) &&
                 llam_worker_has_safe_managed_g(worker, &shard->processor, g_llam_tls_task);
    task_check(test, valid);
    if (!valid) return;
    task_check(test, current_has_reservation());
    task_check(test, atomic_fetch_add_explicit(&test->processor_active[shard->id], 1U,
                                               memory_order_acq_rel) == 0U);
    task_check(test, llam_processor_owner_ticket_snapshot(&shard->processor, &ticket));
    task_check(test, ticket.owner_m == worker && ticket.lease_state == LLAM_LEASE_REGISTERED_OWNED);
    task_check(test, g_llam_tls_scheduler_ctx != NULL &&
                    g_llam_tls_scheduler_ctx != &shard->scheduler_ctx &&
                    g_llam_tls_scheduler_ctx != &shard->opaque_scheduler_ctx);
    uint64_t generation = atomic_load_explicit(&g_llam_tls_task->run_generation, memory_order_acquire);
    if (previous->owner_m != NULL && generation != *previous_run)
        task_check(test, !llam_processor_owner_ticket_equal(previous, &ticket));
    if (previous->owner_m != NULL && previous->owner_m != worker)
        atomic_fetch_add_explicit(&test->migrations, 1U, memory_order_relaxed);
    *previous = ticket;
    *previous_run = generation;
    task_check(test, atomic_fetch_sub_explicit(&test->processor_active[shard->id], 1U,
                                               memory_order_acq_rel) == 1U);
}

static void *blocking_callback(void *argument) {
    scheduler_test_t *test = argument;
    task_check(test, g_llam_tls_task == NULL && g_llam_tls_worker == NULL);
    atomic_fetch_add_explicit(&test->callbacks, 1U, memory_order_relaxed);
    return test;
}

static void exercising_task(void *argument) {
    scheduler_test_t *test = argument;
    llam_processor_owner_ticket_t previous = {0};
    uint64_t previous_run = 0U;
    atomic_fetch_add_explicit(&test->entries, 1U, memory_order_relaxed);
    for (unsigned i = 0U; i < TASK_ROUNDS; ++i) {
        inspect_execution(test, &previous, &previous_run);
        llam_yield();
        task_check(test, current_run_generation() != previous_run);
    }
    inspect_execution(test, &previous, &previous_run);
    task_check(test, llam_sleep_ns(UINT64_C(1000000)) == 0);
    inspect_execution(test, &previous, &previous_run);
    void *result = NULL;
    task_check(test, llam_call_blocking_result(blocking_callback, test, &result) == 0);
    task_check(test, result == test);
    inspect_execution(test, &previous, &previous_run);
    atomic_fetch_add_explicit(&test->completed, 1U, memory_order_relaxed);
}

static int start_worker(llam_worker_t *worker, llam_ctx_t *context, void *argument) {
    scheduler_test_t *test = argument;
    task_check(test, g_llam_tls_worker == worker && g_llam_tls_task == NULL &&
                    g_llam_tls_processor == NULL && g_llam_tls_scheduler_ctx == context);
    atomic_fetch_add_explicit(&test->starts, 1U, memory_order_release);
    if (worker->registry_slot != test->target_slot) return 0;
    if (test->gate_enabled) {
        atomic_store_explicit(&test->gate_reached, 1U, memory_order_release);
        while (atomic_load_explicit(&test->gate_release, memory_order_acquire) == 0U) {
            const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
            (void)nanosleep(&delay, NULL);
        }
    }
    if (test->fail_start) {
        errno = ENOTSUP;
        return -1;
    }
    return 0;
}

static bool stop_worker(llam_worker_t *worker, void *argument) {
    scheduler_test_t *test = argument;
    task_check(test, g_llam_tls_worker == worker && g_llam_tls_task == NULL &&
                    g_llam_tls_processor == NULL && llam_worker_load_active_g(worker) == NULL);
    atomic_fetch_add_explicit(&test->closes, 1U, memory_order_release);
    return true;
}

static void service_worker(llam_worker_t *worker, void *argument) {
    scheduler_test_t *test = argument;
    /* Hold the M whose returned P the test inspects. Another M may service
     * its idle loop while that M still owns P and finishes the task return. */
    if (worker->registry_slot != test->target_slot || !test->drain_gate ||
        atomic_load_explicit(&test->entries, memory_order_acquire) == 0U)
        return;
    atomic_store_explicit(&test->gate_reached, 1U, memory_order_release);
    while (atomic_load_explicit(&test->gate_release, memory_order_acquire) == 0U) {
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
        (void)nanosleep(&delay, NULL);
    }
}

static const llam_managed_scheduler_ops_t test_ops = {
    .native_async = false,
    .worker_start = start_worker,
    .service = service_worker,
    .worker_stop = stop_worker
};

static void *run_runtime(void *argument) {
    scheduler_test_t *test = argument;
    test->run_result = llam_runtime_run_handle(test->runtime);
    test->run_error = errno;
    atomic_store_explicit(&test->run_done, 1U, memory_order_release);
    return NULL;
}

static int assert_unbound(scheduler_test_t *test) {
    llam_runtime_t *runtime = test->runtime;
    CHECK(!atomic_load_explicit(&runtime->exec_started, memory_order_acquire));
    for (size_t i = 0U; i < runtime->worker_slot_count; ++i) {
        llam_worker_t *worker = runtime->worker_slots[i];
        CHECK(!atomic_load_explicit(&worker->native_thread_live, memory_order_acquire));
        CHECK(atomic_load_explicit(&worker->current_p, memory_order_acquire) == NULL);
        CHECK(llam_worker_load_active_g(worker) == NULL);
        CHECK(worker->native_domain_slot == 0U && !worker->sigaltstack_installed);
    }
    for (unsigned i = 0U; i < runtime->active_shards; ++i) {
        CHECK(runtime->shards[i].legacy_lease.state == LLAM_LEASE_REGISTERED_FREE);
        CHECK(runtime->shards[i].legacy_lease.owner == NULL);
        CHECK(atomic_load_explicit(&test->processor_active[i], memory_order_acquire) == 0U);
    }
    CHECK(atomic_load_explicit(&test->failures, memory_order_acquire) == 0U);
    return 0;
}

static int run_case(bool deterministic, bool dynamic) {
    llam_runtime_opts_t opts;
    llam_preempt_opts_t policy;
    scheduler_test_t test = {0};
    llam_task_t *tasks[TASK_COUNT];
    uint64_t first_incarnation;
    pthread_t driver;

    CHECK(llam_runtime_opts_init(&opts, sizeof(opts)) == 0);
    CHECK(llam_preempt_opts_init(&policy, sizeof(policy)) == 0);
    opts.deterministic = deterministic ? 1U : 0U;
    opts.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    opts.preempt_mode = LLAM_PREEMPT_COOPERATIVE;
    if (dynamic) opts.experimental_flags |= LLAM_RUNTIME_EXPERIMENTAL_F_DYNAMIC_WORKERS;
    policy.policy = LLAM_PREEMPT_POLICY_COOPERATIVE;
    CHECK(llam_runtime_create_managed(&opts, sizeof(opts), &policy, sizeof(policy),
                                       &test.runtime) == 0);
    CHECK(register_task_class(test.runtime, exercising_task, LLAM_MANAGED_EXECUTION_PORTABLE) == 0);
    test.processor_active = calloc(test.runtime->active_shards, sizeof(*test.processor_active));
    CHECK(test.processor_active != NULL);
    for (unsigned i = 0U; i < test.runtime->active_shards; ++i)
        atomic_init(&test.processor_active[i], 0U);
    test.target_slot = test.runtime->active_shards > 1U ? 2U : 0U;
    test.runtime->managed_scheduler_ops = &test_ops;
    test.runtime->managed_scheduler_data = &test;
    for (unsigned i = 0U; i < TASK_COUNT; ++i) {
        tasks[i] = llam_runtime_spawn_ex(test.runtime, exercising_task, &test, NULL, 0U);
        CHECK(tasks[i] != NULL);
    }
    test.fail_start = true;
    CHECK(llam_runtime_run_handle(test.runtime) == -1 && errno == ENOTSUP);
    CHECK(atomic_load_explicit(&test.entries, memory_order_acquire) == 0U);
    CHECK(atomic_load_explicit(&test.starts, memory_order_acquire) ==
          atomic_load_explicit(&test.closes, memory_order_acquire));
    CHECK(assert_unbound(&test) == 0);
    first_incarnation = atomic_load_explicit(&test.runtime->shards[0].primary_worker.incarnation,
                                             memory_order_acquire);

    test.fail_start = false;
    test.gate_enabled = true;
    CHECK(pthread_create(&driver, NULL, run_runtime, &test) == 0);
    uint64_t deadline = llam_now_ns() + UINT64_C(5000000000);
    while (atomic_load_explicit(&test.gate_reached, memory_order_acquire) == 0U &&
           atomic_load_explicit(&test.run_done, memory_order_acquire) == 0U &&
           llam_now_ns() < deadline) {
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
        (void)nanosleep(&delay, NULL);
    }
    bool reached = atomic_load_explicit(&test.gate_reached, memory_order_acquire) != 0U;
    bool no_dispatch = atomic_load_explicit(&test.entries, memory_order_acquire) == 0U;
    atomic_store_explicit(&test.gate_release, 1U, memory_order_release);
    CHECK(pthread_join(driver, NULL) == 0);
    CHECK(reached && no_dispatch && test.run_result == 0);
    CHECK(atomic_load_explicit(&test.completed, memory_order_acquire) == TASK_COUNT);
    CHECK(atomic_load_explicit(&test.callbacks, memory_order_acquire) == TASK_COUNT);
    CHECK(assert_unbound(&test) == 0);
    CHECK(atomic_load_explicit(&test.runtime->shards[0].primary_worker.incarnation,
                               memory_order_acquire) > first_incarnation);
    for (unsigned i = 0U; i < TASK_COUNT; ++i) CHECK(llam_join(tasks[i]) == 0);

    test.gate_enabled = false;
    first_incarnation = atomic_load_explicit(&test.runtime->shards[0].primary_worker.incarnation,
                                             memory_order_acquire);
    tasks[0] = llam_runtime_spawn_ex(test.runtime, exercising_task, &test, NULL, 0U);
    CHECK(tasks[0] != NULL && llam_runtime_run_handle(test.runtime) == 0);
    CHECK(llam_join(tasks[0]) == 0 && assert_unbound(&test) == 0);
    CHECK(atomic_load_explicit(&test.runtime->shards[0].primary_worker.incarnation,
                               memory_order_acquire) > first_incarnation);
    test.runtime->managed_scheduler_ops = NULL;
    test.runtime->managed_scheduler_data = NULL;
    llam_runtime_destroy(test.runtime);
    free(test.processor_active);
    return 0;
}

static void shutdown_waiter(void *argument) {
    scheduler_test_t *test = argument;
    atomic_store_explicit(&test->entries, 1U, memory_order_release);
    int result = llam_sleep_ns(UINT64_C(5000000000));
    task_check(test, result == -1 && errno == ECANCELED);
    atomic_store_explicit(&test->completed, 1U, memory_order_release);
}

static void *destroy_runtime(void *argument) {
    scheduler_test_t *test = argument;
    llam_runtime_destroy(test->runtime);
    return NULL;
}

static int shutdown_without_owner(void) {
    llam_runtime_opts_t opts;
    llam_preempt_opts_t policy;
    scheduler_test_t test = {0};
    pthread_t driver, destroyer;

    CHECK(llam_runtime_opts_init(&opts, sizeof(opts)) == 0);
    CHECK(llam_preempt_opts_init(&policy, sizeof(policy)) == 0);
    opts.deterministic = 1U;
    opts.preempt_mode = LLAM_PREEMPT_COOPERATIVE;
    policy.policy = LLAM_PREEMPT_POLICY_COOPERATIVE;
    CHECK(llam_runtime_create_managed(&opts, sizeof(opts), &policy, sizeof(policy),
                                       &test.runtime) == 0);
    CHECK(register_task_class(test.runtime, shutdown_waiter, LLAM_MANAGED_EXECUTION_PORTABLE) == 0);
    test.drain_gate = true;
    test.runtime->managed_scheduler_ops = &test_ops;
    test.runtime->managed_scheduler_data = &test;
    llam_task_t *task = llam_runtime_spawn_ex(test.runtime, shutdown_waiter, &test, NULL, 0U);
    CHECK(task != NULL && llam_detach(task) == 0);
    CHECK(pthread_create(&driver, NULL, run_runtime, &test) == 0);
    uint64_t deadline = llam_now_ns() + UINT64_C(5000000000);
    while (atomic_load_explicit(&test.gate_reached, memory_order_acquire) == 0U &&
           llam_now_ns() < deadline) {
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
        (void)nanosleep(&delay, NULL);
    }
    CHECK(atomic_load_explicit(&test.gate_reached, memory_order_acquire) != 0U);
    /* Scheduler callback is held after returning P. Destroy must grant a
     * separate STOPPING drain owner to complete the previously accepted G. */
    CHECK(atomic_load_explicit(&test.runtime->shards[0].primary_worker.current_p,
                               memory_order_acquire) == NULL);
    CHECK(pthread_create(&destroyer, NULL, destroy_runtime, &test) == 0);
    while (!atomic_load_explicit(&test.runtime->shutdown_requested, memory_order_acquire) &&
           llam_now_ns() < deadline) {
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
        (void)nanosleep(&delay, NULL);
    }
    bool stopping = atomic_load_explicit(&test.runtime->shutdown_requested, memory_order_acquire);
    atomic_store_explicit(&test.gate_release, 1U, memory_order_release);
    CHECK(pthread_join(driver, NULL) == 0 && pthread_join(destroyer, NULL) == 0);
    CHECK(stopping && test.run_result == 0);
    CHECK(atomic_load_explicit(&test.completed, memory_order_acquire) == 1U);
    CHECK(atomic_load_explicit(&test.failures, memory_order_acquire) == 0U);
    CHECK(atomic_load_explicit(&test.starts, memory_order_acquire) ==
          atomic_load_explicit(&test.closes, memory_order_acquire));
    return 0;
}

#if defined(LLAM_ENABLE_TEST_HOOKS)
#define PUBLICATION_CHILDREN 32U

static void publication_child(void *argument) {
    scheduler_test_t *test = argument;
    while (atomic_load_explicit(&test->children_release, memory_order_acquire) == 0U)
        llam_pause_cpu();
    atomic_fetch_add_explicit(&test->children_done, 1U, memory_order_release);
}

static void publication_source(void *argument) {
    scheduler_test_t *test = argument;
    atomic_store_explicit(&test->source_task, g_llam_tls_task, memory_order_release);
    for (unsigned i = 0U; i < PUBLICATION_CHILDREN; ++i) {
        llam_task_t *child = llam_spawn(publication_child, test, NULL);
        task_check(test, child != NULL);
        if (child != NULL) task_check(test, llam_detach(child) == 0);
    }
    llam_yield();
    task_check(test, current_has_reservation());
    atomic_fetch_add_explicit(&test->completed, 1U, memory_order_release);
}

static int publication_before_context_save(bool lockfree) {
    llam_runtime_opts_t opts;
    llam_preempt_opts_t policy;
    scheduler_test_t test = {0};
    pthread_t driver;
    CHECK(llam_runtime_opts_init(&opts, sizeof(opts)) == 0);
    CHECK(llam_preempt_opts_init(&policy, sizeof(policy)) == 0);
    opts.deterministic = 0U;
    opts.preempt_mode = LLAM_PREEMPT_COOPERATIVE;
    opts.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    if (lockfree) opts.experimental_flags = LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    policy.policy = LLAM_PREEMPT_POLICY_COOPERATIVE;
    CHECK(llam_runtime_create_managed(&opts, sizeof(opts), &policy, sizeof(policy),
                                       &test.runtime) == 0);
    CHECK(register_task_class(test.runtime, publication_source, LLAM_MANAGED_EXECUTION_PORTABLE) == 0);
    CHECK(register_task_class(test.runtime, publication_child, LLAM_MANAGED_EXECUTION_PORTABLE) == 0);
    if (test.runtime->portable_processor_count < 2U) {
        llam_runtime_destroy(test.runtime);
        return 0;
    }
    llam_task_t *handle = llam_runtime_spawn_ex(test.runtime, publication_source, &test, NULL, 0U);
    CHECK(handle != NULL);
    llam_lease_test_set_gate(LLAM_LEASE_TEST_AFTER_REGISTERED_YIELD, true);
    CHECK(pthread_create(&driver, NULL, run_runtime, &test) == 0);
    uint64_t deadline = llam_now_ns() + UINT64_C(5000000000);
    while (!llam_lease_test_gate_reached(LLAM_LEASE_TEST_AFTER_REGISTERED_YIELD) &&
           llam_now_ns() < deadline) {
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
        (void)nanosleep(&delay, NULL);
    }
    bool reached = llam_lease_test_gate_reached(LLAM_LEASE_TEST_AFTER_REGISTERED_YIELD);
    llam_task_t *source = atomic_load_explicit(&test.source_task, memory_order_acquire);
    bool reserved = source != NULL &&
        atomic_load_explicit(&source->managed_dispatch_owner, memory_order_acquire) != NULL &&
        atomic_load_explicit(&source->state, memory_order_acquire) == LLAM_TASK_STATE_RUNNABLE;
    /* Real competing Ms skip the reserved source at the normal tail and
     * finish its children while the source's context is still unsaved. */
    atomic_store_explicit(&test.children_release, 1U, memory_order_release);
    /* Chase-Lev stealing deliberately leaves shallow backlogs to their owner.
     * Require substantial peer progress, then exact completion after release. */
    unsigned progress_target = lockfree ? PUBLICATION_CHILDREN / 2U : PUBLICATION_CHILDREN;
    while (atomic_load_explicit(&test.children_done, memory_order_acquire) < progress_target &&
           llam_now_ns() < deadline) {
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
        (void)nanosleep(&delay, NULL);
    }
    bool peers_finished = atomic_load_explicit(&test.children_done, memory_order_acquire) >=
                          progress_target;
    bool source_not_resumed = atomic_load_explicit(&test.completed, memory_order_acquire) == 0U;
    llam_lease_test_set_gate(LLAM_LEASE_TEST_AFTER_REGISTERED_YIELD, false);
    CHECK(pthread_join(driver, NULL) == 0);
    if (!reached || !reserved || !peers_finished || !source_not_resumed || test.run_result != 0)
        fprintf(stderr, "publication lockfree=%u reached=%u reserved=%u children=%u source=%u run=%d\n",
                lockfree, reached, reserved, atomic_load(&test.children_done),
                atomic_load(&test.completed), test.run_result);
    CHECK(reached && reserved && peers_finished && source_not_resumed && test.run_result == 0);
    CHECK(atomic_load_explicit(&test.completed, memory_order_acquire) == 1U);
    CHECK(atomic_load_explicit(&test.children_done, memory_order_acquire) == PUBLICATION_CHILDREN);
    CHECK(atomic_load_explicit(&test.failures, memory_order_acquire) == 0U);
    CHECK(llam_join(handle) == 0);
    llam_runtime_destroy(test.runtime);
    return 0;
}
typedef struct affine_lifetime_test {
    scheduler_test_t base;
#if LLAM_PLATFORM_WINDOWS
    DWORD key;
#else
    pthread_key_t key;
#endif
    pthread_t native_thread;
    llam_task_t *native_task;
    llam_task_t *handle;
    atomic_uint destructor_entered;
    atomic_uint destructor_release;
    atomic_uint destructor_done;
    atomic_uint portable_progress;
    atomic_uint consumer_done;
    atomic_uint finish_run;
} affine_lifetime_test_t;

#if LLAM_PLATFORM_WINDOWS
static void CALLBACK affine_destructor(void *argument) {
#else
static void affine_destructor(void *argument) {
#endif
    affine_lifetime_test_t *test = argument;
    task_check(&test->base, pthread_equal(pthread_self(), test->native_thread) != 0);
    task_check(&test->base, test->native_task->stack_mapping != NULL &&
               atomic_load_explicit(&test->native_task->reclaim_ready, memory_order_acquire) == 0U &&
               atomic_load_explicit(&test->native_task->scan_refs, memory_order_acquire) != 0U);
    atomic_store_explicit(&test->destructor_entered, 1U, memory_order_release);
    while (atomic_load_explicit(&test->destructor_release, memory_order_acquire) == 0U) {
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
        (void)nanosleep(&delay, NULL);
    }
    atomic_store_explicit(&test->destructor_done, 1U, memory_order_release);
}

static void *affine_tls_get(affine_lifetime_test_t *test) {
#if LLAM_PLATFORM_WINDOWS
    return FlsGetValue(test->key);
#else
    return pthread_getspecific(test->key);
#endif
}

static bool affine_tls_set(affine_lifetime_test_t *test) {
#if LLAM_PLATFORM_WINDOWS
    return FlsSetValue(test->key, test) != 0;
#else
    return pthread_setspecific(test->key, test) == 0;
#endif
}

static void affine_lifetime_task(void *argument) {
    affine_lifetime_test_t *test = argument;
    task_check(&test->base, affine_tls_get(test) == NULL);
    test->native_thread = pthread_self();
    test->native_task = g_llam_tls_task;
    task_check(&test->base, g_llam_tls_worker->role == LLAM_WORKER_ROLE_NATIVE_AFFINE);
    task_check(&test->base, affine_tls_set(test));
    llam_yield();
    task_check(&test->base, pthread_equal(pthread_self(), test->native_thread) != 0 &&
                              affine_tls_get(test) == test);
}

static void affine_join_consumer(void *argument) {
    affine_lifetime_test_t *test = argument;
    task_check(&test->base, llam_join(test->handle) == 0);
    task_check(&test->base, atomic_load_explicit(&test->destructor_done, memory_order_acquire) == 1U);
    atomic_store_explicit(&test->consumer_done, 1U, memory_order_release);
}

static void affine_portable_peer(void *argument) {
    affine_lifetime_test_t *test = argument;
    while (atomic_load_explicit(&test->destructor_entered, memory_order_acquire) == 0U)
        llam_yield();
    atomic_store_explicit(&test->portable_progress, 1U, memory_order_release);
    while (atomic_load_explicit(&test->finish_run, memory_order_acquire) == 0U)
        llam_yield();
}

static bool wait_for_flag(atomic_uint *flag) {
    uint64_t deadline = llam_now_ns() + UINT64_C(5000000000);
    while (atomic_load_explicit(flag, memory_order_acquire) == 0U && llam_now_ns() < deadline) {
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
        (void)nanosleep(&delay, NULL);
    }
    return atomic_load_explicit(flag, memory_order_acquire) != 0U;
}

static bool wait_for_affine_capacity(llam_runtime_t *runtime) {
    uint64_t deadline = llam_now_ns() + UINT64_C(5000000000);
    while (llam_managed_affine_available(runtime) == 0U && llam_now_ns() < deadline) {
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
        (void)nanosleep(&delay, NULL);
    }
    return llam_managed_affine_available(runtime) != 0U;
}

static int affine_native_lifetime(bool detach, bool async_native) {
    llam_runtime_opts_t opts;
    llam_preempt_opts_t policy;
    affine_lifetime_test_t test = {0};
    pthread_t driver;
    CHECK(llam_runtime_opts_init(&opts, sizeof(opts)) == 0);
    CHECK(llam_preempt_opts_init(&policy, sizeof(policy)) == 0);
    opts.deterministic = 1U;
    opts.preempt_mode = async_native ? LLAM_PREEMPT_AUTO : LLAM_PREEMPT_COOPERATIVE;
    policy.policy = async_native ? LLAM_PREEMPT_POLICY_ASYNC_REQUIRED : LLAM_PREEMPT_POLICY_COOPERATIVE;
    policy.native_affine_capacity = 1U;
#if LLAM_PLATFORM_WINDOWS
    test.key = FlsAlloc(affine_destructor);
    CHECK(test.key != FLS_OUT_OF_INDEXES);
#else
    CHECK(pthread_key_create(&test.key, affine_destructor) == 0);
#endif
    CHECK(llam_runtime_create_managed(&opts, sizeof(opts), &policy, sizeof(policy),
                                       &test.base.runtime) == 0);
    CHECK(register_task_class(test.base.runtime, affine_lifetime_task, LLAM_MANAGED_EXECUTION_NATIVE_AFFINE) == 0);
    CHECK(register_task_class(test.base.runtime, affine_join_consumer, LLAM_MANAGED_EXECUTION_PORTABLE) == 0);
    CHECK(register_task_class(test.base.runtime, affine_portable_peer, LLAM_MANAGED_EXECUTION_PORTABLE) == 0);
    test.handle = llam_runtime_spawn_ex(test.base.runtime, affine_lifetime_task, &test, NULL, 0U);
    CHECK(test.handle != NULL);
    llam_task_t *peer = llam_runtime_spawn_ex(test.base.runtime, affine_portable_peer, &test, NULL, 0U);
    CHECK(peer != NULL);
    llam_task_t *consumer = detach ? NULL :
        llam_runtime_spawn_ex(test.base.runtime, affine_join_consumer, &test, NULL, 0U);
    CHECK(detach || consumer != NULL);
    CHECK(pthread_create(&driver, NULL, run_runtime, &test.base) == 0);
    CHECK(wait_for_flag(&test.destructor_entered) && wait_for_flag(&test.portable_progress));
    if (detach) CHECK(llam_detach(test.handle) == 0);
    CHECK(llam_managed_affine_available(test.base.runtime) == 0U);
    atomic_store_explicit(&test.destructor_release, 1U, memory_order_release);
    CHECK(wait_for_flag(&test.destructor_done));
    if (!detach) CHECK(wait_for_flag(&test.consumer_done));
    CHECK(wait_for_affine_capacity(test.base.runtime));
    uint64_t first_native_incarnation = atomic_load_explicit(
        &test.base.runtime->managed_workers[0].incarnation, memory_order_acquire);
    atomic_store_explicit(&test.destructor_entered, 0U, memory_order_relaxed);
    atomic_store_explicit(&test.destructor_done, 0U, memory_order_relaxed);
    atomic_store_explicit(&test.destructor_release, 0U, memory_order_release);
    llam_task_t *retained = llam_runtime_spawn_ex(test.base.runtime,
        affine_lifetime_task, &test, NULL, 0U);
    CHECK(retained != NULL && wait_for_flag(&test.destructor_entered));
    CHECK(atomic_load_explicit(&test.base.runtime->managed_workers[0].incarnation,
                               memory_order_acquire) == first_native_incarnation);
    atomic_store_explicit(&test.destructor_release, 1U, memory_order_release);
    CHECK(wait_for_affine_capacity(test.base.runtime));
    CHECK(atomic_load_explicit(&test.base.runtime->managed_workers[0].incarnation,
                               memory_order_acquire) > first_native_incarnation);
    atomic_store_explicit(&test.destructor_entered, 0U, memory_order_relaxed);
    atomic_store_explicit(&test.destructor_done, 0U, memory_order_relaxed);
    atomic_store_explicit(&test.destructor_release, 0U, memory_order_release);
    llam_task_t *replacement = llam_runtime_spawn_ex(test.base.runtime,
        affine_lifetime_task, &test, NULL, 0U);
    CHECK(replacement != NULL && wait_for_flag(&test.destructor_entered));
    uint64_t new_reservation = atomic_load_explicit(
        &test.base.runtime->managed_workers[0].managed_reservation, memory_order_acquire);
    CHECK((new_reservation & 1U) != 0U);
    /* A completed G's late consuming join must not release its replacement's
     * native M credit, even though both use the same immutable registry slot. */
    CHECK(llam_join(retained) == 0);
    CHECK(atomic_load_explicit(&test.base.runtime->managed_workers[0].managed_reservation,
                               memory_order_acquire) == new_reservation);
    CHECK(llam_managed_affine_available(test.base.runtime) == 0U);
    atomic_store_explicit(&test.destructor_release, 1U, memory_order_release);
    CHECK(llam_join(replacement) == 0 && wait_for_affine_capacity(test.base.runtime));
    atomic_store_explicit(&test.finish_run, 1U, memory_order_release);
    CHECK(pthread_join(driver, NULL) == 0 && test.base.run_result == 0);
    CHECK(llam_join(peer) == 0);
    if (consumer != NULL) CHECK(llam_join(consumer) == 0);
    CHECK(atomic_load_explicit(&test.base.failures, memory_order_acquire) == 0U);
    CHECK(llam_managed_affine_available(test.base.runtime) == 1U);
    uint64_t previous_incarnation = atomic_load_explicit(
        &test.base.runtime->managed_workers[0].incarnation, memory_order_acquire);
    test.handle = llam_runtime_spawn_ex(test.base.runtime, affine_lifetime_task, &test, NULL, 0U);
    CHECK(test.handle != NULL && llam_runtime_run_handle(test.base.runtime) == 0);
    CHECK(llam_join(test.handle) == 0);
    CHECK(atomic_load_explicit(&test.base.runtime->managed_workers[0].incarnation,
                               memory_order_acquire) > previous_incarnation);
    CHECK(atomic_load_explicit(&test.base.failures, memory_order_acquire) == 0U);
    CHECK(llam_managed_affine_available(test.base.runtime) == 1U);
    llam_runtime_destroy(test.base.runtime);
#if LLAM_PLATFORM_WINDOWS
    CHECK(FlsFree(test.key) != 0);
#else
    CHECK(pthread_key_delete(test.key) == 0);
#endif
    return 0;
}

static void legacy_busy_task(void *argument) {
    scheduler_test_t *test = argument;
#if LLAM_PLATFORM_DARWIN
    if (test->check_native_masks) {
        sigset_t mask;
        task_check(test, pthread_sigmask(SIG_SETMASK, NULL, &mask) == 0 &&
            sigismember(&mask, SIGUSR2) == sigismember(&test->caller_mask, SIGUSR2) &&
            sigismember(&mask, LLAM_PREEMPT_SIGNAL) == sigismember(&test->caller_mask, LLAM_PREEMPT_SIGNAL));
    }
#endif
    task_check(test, g_llam_tls_worker->role == LLAM_WORKER_ROLE_LEGACY_COMPAT &&
                    g_llam_tls_shard->id >= test->runtime->portable_processor_count);
    atomic_store_explicit(&test->gate_reached, 1U, memory_order_release);
    uint64_t deadline = llam_now_ns() + UINT64_C(5000000000);
    while (atomic_load_explicit(&test->gate_release, memory_order_acquire) == 0U) {
        if (llam_now_ns() >= deadline) { task_check(test, false); break; }
        llam_pause_cpu();
    }
    atomic_fetch_add_explicit(&test->completed, 1U, memory_order_release);
}

static void portable_capacity_task(void *argument) {
    scheduler_test_t *test = argument;
#if LLAM_PLATFORM_DARWIN
    if (test->check_native_masks) {
        sigset_t mask;
        task_check(test, pthread_sigmask(SIG_SETMASK, NULL, &mask) == 0 &&
            sigismember(&mask, SIGUSR2) == 1 && sigismember(&mask, LLAM_PREEMPT_SIGNAL) == 1);
    }
#endif
    while (atomic_load_explicit(&test->gate_reached, memory_order_acquire) == 0U) llam_yield();
    task_check(test, g_llam_tls_worker->role != LLAM_WORKER_ROLE_LEGACY_COMPAT &&
                    g_llam_tls_shard->id < test->runtime->portable_processor_count);
    atomic_fetch_add_explicit(&test->entries, 1U, memory_order_release);
    atomic_store_explicit(&test->gate_release, 1U, memory_order_release);
}

static int isolated_compatibility_processor(bool native) {
    llam_runtime_opts_t opts;
    llam_preempt_opts_t policy;
    scheduler_test_t test = {0};
#if LLAM_PLATFORM_DARWIN
    test.check_native_masks = native;
    CHECK(pthread_sigmask(SIG_SETMASK, NULL, &test.caller_mask) == 0);
#endif
    CHECK(llam_runtime_opts_init(&opts, sizeof(opts)) == 0);
    CHECK(llam_preempt_opts_init(&policy, sizeof(policy)) == 0);
    opts.deterministic = 1U;
    opts.preempt_mode = native ? LLAM_PREEMPT_AUTO : LLAM_PREEMPT_COOPERATIVE;
    policy.policy = native ? LLAM_PREEMPT_POLICY_ASYNC_AUTO : LLAM_PREEMPT_POLICY_COOPERATIVE;
    CHECK(llam_runtime_create_managed(&opts, sizeof(opts), &policy, sizeof(policy), &test.runtime) == 0);
    CHECK(test.runtime->portable_processor_count == 1U && test.runtime->compatibility_processor_count == 1U);
    CHECK(register_task_class(test.runtime, portable_capacity_task, LLAM_MANAGED_EXECUTION_PORTABLE) == 0);
    llam_task_t *legacy = llam_runtime_spawn_ex(test.runtime, legacy_busy_task, &test, NULL, 0U);
    llam_task_t *portable = llam_runtime_spawn_ex(test.runtime, portable_capacity_task, &test, NULL, 0U);
    CHECK(legacy != NULL && portable != NULL && llam_runtime_run_handle(test.runtime) == 0);
    CHECK(llam_join(legacy) == 0 && llam_join(portable) == 0);
    CHECK(atomic_load_explicit(&test.entries, memory_order_acquire) == 1U &&
          atomic_load_explicit(&test.completed, memory_order_acquire) == 1U &&
          atomic_load_explicit(&test.failures, memory_order_acquire) == 0U);
    llam_runtime_destroy(test.runtime);
    return 0;
}

static void early_wake_source(void *argument) {
    scheduler_test_t *test = argument;
    atomic_store_explicit(&test->source_task, g_llam_tls_task, memory_order_release);
    int result = llam_sleep_ns(UINT64_C(5000000000));
    task_check(test, result == -1 && errno == ECANCELED);
    atomic_fetch_add_explicit(&test->completed, 1U, memory_order_release);
}

static int wake_before_context_save(void) {
    llam_runtime_opts_t opts;
    llam_preempt_opts_t policy;
    scheduler_test_t test = {0};
    pthread_t driver;
    CHECK(llam_runtime_opts_init(&opts, sizeof(opts)) == 0);
    CHECK(llam_preempt_opts_init(&policy, sizeof(policy)) == 0);
    opts.deterministic = 1U;
    opts.preempt_mode = LLAM_PREEMPT_COOPERATIVE;
    policy.policy = LLAM_PREEMPT_POLICY_COOPERATIVE;
    CHECK(llam_runtime_create_managed(&opts, sizeof(opts), &policy, sizeof(policy),
                                       &test.runtime) == 0);
    CHECK(register_task_class(test.runtime, early_wake_source, LLAM_MANAGED_EXECUTION_PORTABLE) == 0);
    llam_task_t *handle = llam_runtime_spawn_ex(test.runtime, early_wake_source, &test, NULL, 0U);
    CHECK(handle != NULL);
    llam_lease_test_set_gate(LLAM_LEASE_TEST_AFTER_SLEEP_PARK, true);
    CHECK(pthread_create(&driver, NULL, run_runtime, &test) == 0);
    uint64_t deadline = llam_now_ns() + UINT64_C(5000000000);
    while (!llam_lease_test_gate_reached(LLAM_LEASE_TEST_AFTER_SLEEP_PARK) &&
           llam_now_ns() < deadline) {
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
        (void)nanosleep(&delay, NULL);
    }
    bool reached = llam_lease_test_gate_reached(LLAM_LEASE_TEST_AFTER_SLEEP_PARK);
    llam_task_t *source = atomic_load_explicit(&test.source_task, memory_order_acquire);
    bool protected = false;
    if (reached && source != NULL) {
        /* Actual resolver completion wins after active-G clear and before
         * context save. Its inject publication must remain source-reserved. */
        llam_cancel_task_wait(source);
        llam_shard_t *shard = &test.runtime->shards[0];
        pthread_mutex_lock(&shard->lock);
        unsigned before = shard->processor.inject_q.depth;
        llam_task_t *removed = llam_queue_pop_head(&shard->processor.inject_q);
        protected = before == 1U && removed == NULL &&
                    shard->processor.inject_q.depth == before &&
                    atomic_load_explicit(&source->managed_dispatch_owner,
                                         memory_order_acquire) != NULL;
        if (removed != NULL) llam_queue_push_tail(&shard->processor.inject_q, removed);
        pthread_mutex_unlock(&shard->lock);
    }
    bool not_resumed = atomic_load_explicit(&test.completed, memory_order_acquire) == 0U;
    llam_lease_test_set_gate(LLAM_LEASE_TEST_AFTER_SLEEP_PARK, false);
    CHECK(pthread_join(driver, NULL) == 0);
    CHECK(reached && protected && not_resumed && test.run_result == 0);
    CHECK(atomic_load_explicit(&test.completed, memory_order_acquire) == 1U);
    CHECK(atomic_load_explicit(&test.failures, memory_order_acquire) == 0U);
    CHECK(llam_join(handle) == 0);
    llam_runtime_destroy(test.runtime);
    return 0;
}
#endif

static int dynamic_empty_processors(void) {
    llam_runtime_opts_t opts;
    llam_preempt_opts_t policy;
    llam_runtime_t *runtime = NULL;
    CHECK(llam_runtime_opts_init(&opts, sizeof(opts)) == 0);
    CHECK(llam_preempt_opts_init(&policy, sizeof(policy)) == 0);
    opts.experimental_flags |= LLAM_RUNTIME_EXPERIMENTAL_F_DYNAMIC_WORKERS;
    opts.preempt_mode = LLAM_PREEMPT_COOPERATIVE;
    policy.policy = LLAM_PREEMPT_POLICY_COOPERATIVE;
    CHECK(llam_runtime_create_managed(&opts, sizeof(opts), &policy, sizeof(policy), &runtime) == 0);
    CHECK(llam_processor_online(&runtime->shards[runtime->portable_processor_count].processor));
    unsigned observed = 0U;
    for (unsigned i = 0U; i < runtime->active_shards; ++i)
        if (llam_processor_online(&runtime->shards[i].processor)) ++observed;
    CHECK(observed == llam_runtime_online_shards(runtime));
    CHECK(!llam_managed_offline_empty_processor(runtime));
    if (runtime->portable_processor_count < 2U) {
        llam_runtime_destroy(runtime);
        puts("registered dynamic: one CPU, empty multi-P retirement case unavailable");
        return 0;
    }
    /* Isolate the empty-P transaction from controller timing. Keeping the
     * compatibility P and P0 online leaves one candidate whose native roots,
     * timers, I/O and execution lease can each prevent retirement. */
    runtime->experimental_dynamic_shards = 1U;
    runtime->dynamic_online_floor = 1U + runtime->compatibility_processor_count;
    for (unsigned i = 1U; i < runtime->portable_processor_count; ++i) {
        pthread_mutex_lock(&runtime->shards[i].lock);
        if (llam_processor_try_set_online(&runtime->shards[i].processor, true))
            atomic_fetch_add_explicit(&runtime->online_shards, 1U, memory_order_relaxed);
        pthread_mutex_unlock(&runtime->shards[i].lock);
    }
    atomic_store_explicit(&runtime->exec_started, true, memory_order_release);
    while (llam_runtime_online_shards(runtime) > runtime->dynamic_online_floor + 1U)
        CHECK(llam_managed_offline_empty_processor(runtime));
    llam_shard_t *candidate = NULL;
    for (unsigned i = 1U; i < runtime->portable_processor_count; ++i)
        if (llam_processor_online(&runtime->shards[i].processor)) candidate = &runtime->shards[i];
    CHECK(candidate != NULL);
    atomic_store_explicit(&candidate->live_tasks, 1U, memory_order_release);
    CHECK(!llam_managed_offline_empty_processor(runtime));
    atomic_store_explicit(&candidate->live_tasks, 0U, memory_order_release);
    atomic_store_explicit(&candidate->inflight_io_waiters, 1U, memory_order_release);
    CHECK(!llam_managed_offline_empty_processor(runtime));
    atomic_store_explicit(&candidate->inflight_io_waiters, 0U, memory_order_release);
    atomic_store_explicit(&candidate->processor.timer_callbacks_active, 1U, memory_order_release);
    CHECK(!llam_managed_offline_empty_processor(runtime));
    atomic_store_explicit(&candidate->processor.timer_callbacks_active, 0U, memory_order_release);
    llam_ctx_t scheduler = {0};
    llam_worker_t *worker = &candidate->primary_worker;
    CHECK(llam_worker_bind_registered(worker, &scheduler) == 0);
    uint64_t incarnation = atomic_load_explicit(&worker->binding_incarnation, memory_order_acquire);
    pthread_mutex_lock(&candidate->opaque_lock);
    bool claimed = llam_execution_lease_claim_locked(candidate, worker, incarnation);
    pthread_mutex_unlock(&candidate->opaque_lock);
    CHECK(claimed && !llam_managed_offline_empty_processor(runtime));
    llam_processor_owner_ticket_t ticket;
    CHECK(llam_processor_owner_ticket_snapshot(&candidate->processor, &ticket));
    pthread_mutex_lock(&candidate->opaque_lock);
    bool returned = llam_execution_lease_return_locked(candidate, worker, &ticket);
    pthread_mutex_unlock(&candidate->opaque_lock);
    CHECK(returned && llam_worker_unbind_registered(worker) == 0);
    CHECK(llam_managed_offline_empty_processor(runtime));
    CHECK(!llam_processor_online(&candidate->processor));
    CHECK(llam_runtime_online_shards(runtime) == runtime->dynamic_online_floor);
    CHECK(llam_processor_online(&runtime->shards[runtime->portable_processor_count].processor));
    atomic_store_explicit(&runtime->exec_started, false, memory_order_release);
    llam_runtime_destroy(runtime);
    return 0;
}

#if defined(LLAM_ENABLE_TEST_HOOKS) && LLAM_ASYNC_CONTEXT_SHARED
static int native_bookkeeping(void) {
    llam_runtime_opts_t opts;
    llam_preempt_opts_t policy;
    scheduler_test_t test = {0};
    llam_task_t *tasks[TASK_COUNT];
    CHECK(llam_runtime_opts_init(&opts, sizeof(opts)) == 0);
    CHECK(llam_preempt_opts_init(&policy, sizeof(policy)) == 0);
    opts.preempt_mode = LLAM_PREEMPT_AUTO;
    opts.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    CHECK(llam_runtime_create_managed(&opts, sizeof(opts), &policy, sizeof(policy), &test.runtime) == 0);
    CHECK(atomic_load_explicit(&test.runtime->async_preempt_enabled, memory_order_acquire) != 0U);
    CHECK(register_task_class(test.runtime, exercising_task, LLAM_MANAGED_EXECUTION_PORTABLE) == 0);
    test.processor_active = calloc(test.runtime->active_shards, sizeof(*test.processor_active));
    CHECK(test.processor_active != NULL);
    for (unsigned i = 0U; i < test.runtime->active_shards; ++i) atomic_init(&test.processor_active[i], 0U);
    for (unsigned i = 0U; i < TASK_COUNT; ++i) {
        tasks[i] = llam_runtime_spawn_ex(test.runtime, exercising_task, &test, NULL, 0U);
        CHECK(tasks[i] != NULL);
    }
    CHECK(llam_runtime_run_handle(test.runtime) == 0);
    for (unsigned i = 0U; i < TASK_COUNT; ++i) CHECK(llam_join(tasks[i]) == 0);
    CHECK(assert_unbound(&test) == 0 && atomic_load(&test.completed) == TASK_COUNT &&
          atomic_load(&test.callbacks) == TASK_COUNT);
    llam_async_shared_stats_t stats;
    CHECK(llam_async_shared_stats(test.runtime, &stats) && stats.captures == 0U &&
          stats.execution_debt == 0U && stats.charged_bytes == 0U && stats.binding_ops == 0U &&
          stats.bindings_created == stats.bindings_closed);
    if (test.runtime->portable_processor_count > 1U) CHECK(atomic_load(&test.migrations) > 0U);
    printf("native bookkeeping: %u portable Ps, %u cooperative M changes, timers/offload and balanced roots\n",
        test.runtime->portable_processor_count, atomic_load(&test.migrations));
    free(test.processor_active);
    llam_runtime_destroy(test.runtime);
    return isolated_compatibility_processor(true);
}
#endif

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--errno-migration") == 0)
        return errno_migration_cases() == 0 ? 0 : 1;
#if defined(LLAM_ENABLE_TEST_HOOKS)
    if (argc == 2 && strcmp(argv[1], "--async-bookkeeping") == 0) {
#if LLAM_ASYNC_CONTEXT_SHARED
        return native_bookkeeping() == 0 ? 0 : 1;
#else
        puts("native bookkeeping: shared adapter unavailable");
        return 77;
#endif
    }
    if (argc == 2 && strcmp(argv[1], "--async-affine") == 0) {
#if LLAM_ASYNC_CONTEXT_COMPILED
        if (affine_native_lifetime(false, true) != 0 || affine_native_lifetime(true, true) != 0) return 1;
        puts("registered async affine: native TLS destructors retain G stack and pins through join/detach");
        return 0;
#else
        puts("registered async affine: native adapter unavailable");
        return 77;
#endif
    }
#else
    (void)argc;
    (void)argv;
#endif
    if (errno_migration_cases() != 0 || dynamic_empty_processors() != 0 || run_case(true, false) != 0 ||
        run_case(false, false) != 0 || run_case(false, true) != 0 || shutdown_without_owner() != 0) return 1;
#if defined(LLAM_ENABLE_TEST_HOOKS)
    if (publication_before_context_save(false) != 0 || publication_before_context_save(true) != 0)
        return 1;
    if (wake_before_context_save() != 0) return 1;
    if (affine_native_lifetime(false, false) != 0 || affine_native_lifetime(true, false) != 0) return 1;
    if (isolated_compatibility_processor(false) != 0) return 1;
#endif
    puts("test_managed_scheduler_internal: dispatch, publication, affine TLS lifetime and compatibility isolation passed");
    return 0;
}
