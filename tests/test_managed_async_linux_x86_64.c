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

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "runtime_internal.h"
#include "async_preempt_abi.h"
#include "managed_async_adapter.h"
#include "managed_catalog.h"
#include "managed_scheduler.h"
#include "../tools/preempt/metadata_v2.h"
#include <stdio.h>

#if !defined(__linux__) || !defined(__x86_64__) || !defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
int main(void) { puts("registered Linux x86-64: native adapter unavailable"); return 77; }
#else
extern unsigned llam_async_safe_leaf(atomic_uint *release);
extern char llam_async_safe_leaf_begin[], llam_async_safe_leaf_end[];

static bool oversubscribed_processors;
static bool automatic_preemption;
static bool dynamic_processors;
#if defined(LLAM_TEST_WRAP_CPU_COUNT)
unsigned __real_llam_count_allowed_cpus(unsigned **out_cpus);
unsigned __wrap_llam_count_allowed_cpus(unsigned **out_cpus) {
    if (!oversubscribed_processors) return __real_llam_count_allowed_cpus(out_cpus);
    unsigned *observed = NULL;
    unsigned count = __real_llam_count_allowed_cpus(&observed);
    if (count == 0U) return 0U;
    unsigned first = observed[0];
    free(observed);
    unsigned *placements = calloc(2U, sizeof(*placements));
    if (placements == NULL) return 0U;
    placements[0] = placements[1] = first;
    *out_cpus = placements;
    /* Two runtime Ps share one real CPU; this is not multicore qualification. */
    return 2U;
}
#endif

typedef struct registered_case registered_case_t;
typedef struct registered_leaf {
    registered_case_t *test;
    unsigned index;
    llam_worker_t *origin;
} registered_leaf_t;
struct registered_case {
    llam_runtime_t *runtime;
    unsigned count;
    atomic_uint release, started, finished, errors, timer_progress, rescue_done, rescued;
    registered_leaf_t leaves[3];
};

static void check_at(registered_case_t *test, bool valid, unsigned line) {
    if (!valid) {
        fprintf(stderr, "registered Linux x86-64 check failed at %u\n", line);
        atomic_fetch_add_explicit(&test->errors, 1U, memory_order_relaxed);
    }
}
#define CHECK(test, expr) check_at((test), (expr), __LINE__)

static llam_worker_t *worker_at(llam_runtime_t *rt, size_t i) {
    return i < rt->active_shards ? &rt->shards[i].primary_worker
        : &rt->managed_workers[i - rt->active_shards];
}

static void *offload(void *arg) {
    registered_case_t *test = arg;
    CHECK(test, g_llam_tls_worker == NULL && g_llam_tls_task == NULL);
    errno = E2BIG;
    return test;
}

static void leaf_task(void *arg) {
    registered_leaf_t *leaf = arg;
    registered_case_t *test = leaf->test;
    llam_worker_t *origin = g_llam_tls_worker;
    llam_processor_owner_ticket_t before, after;
    uint64_t incarnation = atomic_load_explicit(&origin->binding_incarnation, memory_order_acquire);
    pthread_t native = pthread_self();
    int *native_errno = &errno;
    int expected_errno = EDOM + (int)leaf->index;
    sigset_t original, mask;
    llam_async_preempt_state_t *state = (void *)atomic_load_explicit(&origin->async_preempt_state,
                                                                   memory_order_acquire);
    leaf->origin = origin;
    CHECK(test, state != NULL && atomic_load_explicit(&state->frame_probe_status, memory_order_acquire) == 2U);
    uint64_t acknowledged = atomic_load_explicit(&state->acknowledged_generation, memory_order_acquire);
    CHECK(test, llam_processor_owner_ticket_snapshot(g_llam_tls_processor, &before));
    for (size_t i = 0; i < test->runtime->active_shards + test->runtime->managed_worker_count; ++i) {
        llam_async_preempt_state_t *peer = (void *)atomic_load_explicit(&worker_at(test->runtime, i)->async_preempt_state,
                                                                      memory_order_acquire);
        CHECK(test, peer != NULL && atomic_load_explicit(&peer->frame_probe_status, memory_order_acquire) == 2U);
    }
    CHECK(test, pthread_sigmask(SIG_SETMASK, NULL, &original) == 0);
    mask = original;
    if ((leaf->index & 1U) != 0U) sigaddset(&mask, SIGUSR2);
    else sigdelset(&mask, SIGUSR2);
    CHECK(test, pthread_sigmask(SIG_SETMASK, &mask, NULL) == 0);
    atomic_fetch_add_explicit(&test->started, 1U, memory_order_release);
    errno = expected_errno;
    unsigned registers_valid = llam_async_safe_leaf(&test->release);
    CHECK(test, registers_valid == 1U && errno == expected_errno && &errno == native_errno);
    CHECK(test, origin == g_llam_tls_worker && pthread_equal(native, pthread_self()) &&
        incarnation == atomic_load_explicit(&origin->binding_incarnation, memory_order_acquire));
    CHECK(test, atomic_load_explicit(&state->acknowledged_generation, memory_order_acquire) > acknowledged);
    CHECK(test, pthread_sigmask(SIG_SETMASK, NULL, &mask) == 0 &&
        sigismember(&mask, SIGUSR2) == (int)(leaf->index & 1U));
    CHECK(test, pthread_sigmask(SIG_SETMASK, &original, NULL) == 0);
    CHECK(test, llam_processor_owner_ticket_snapshot(g_llam_tls_processor, &after) &&
        after.owner_m == origin && after.lease_state == LLAM_LEASE_REGISTERED_OWNED &&
        (after.processor != before.processor || after.p_generation > before.p_generation));
    llam_yield();
    CHECK(test, llam_sleep_ns(UINT64_C(100000)) == 0);
    void *result = NULL;
    CHECK(test, llam_call_blocking_result(offload, test, &result) == 0 && result == test && errno == E2BIG);
    CHECK(test, origin == g_llam_tls_worker && pthread_equal(native, pthread_self()));
    atomic_fetch_add_explicit(&test->finished, 1U, memory_order_release);
}

static void controller_task(void *arg) {
    registered_case_t *test = arg;
    while (atomic_load_explicit(&test->started, memory_order_acquire) < test->count &&
           atomic_load_explicit(&test->rescued, memory_order_acquire) == 0U) llam_yield();
    CHECK(test, llam_sleep_ns(UINT64_C(1000000)) == 0);
    CHECK(test, atomic_load_explicit(&test->finished, memory_order_acquire) == 0U);
    atomic_store_explicit(&test->timer_progress, 1U, memory_order_release);
    atomic_store_explicit(&test->release, 1U, memory_order_release);
}

static void *rescue(void *arg) {
    registered_case_t *test = arg;
    uint64_t deadline = llam_now_ns() + UINT64_C(8000000000);
    while (!atomic_load_explicit(&test->rescue_done, memory_order_acquire)) {
        if (llam_now_ns() > deadline) {
            atomic_store_explicit(&test->rescued, 1U, memory_order_release);
            atomic_store_explicit(&test->release, 1U, memory_order_release);
            break;
        }
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000L};
        (void)nanosleep(&delay, NULL);
    }
    return NULL;
}

static llam_runtime_t *make_runtime(bool multi) {
    llam_runtime_opts_t opts;
    llam_runtime_t *rt = calloc(1U, sizeof(*rt));
    if (rt == NULL || llam_runtime_opts_init(&opts, sizeof(opts)) != 0) { free(rt); return NULL; }
    opts.deterministic = multi ? 0U : 1U;
    opts.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    opts.preempt_mode = automatic_preemption ? LLAM_PREEMPT_AUTO : LLAM_PREEMPT_STRICT;
    opts.preempt_quantum_ns = UINT64_C(1000000);
    if (dynamic_processors) opts.experimental_flags |= LLAM_RUNTIME_EXPERIMENTAL_F_DYNAMIC_WORKERS;
    if (llam_runtime_init_registered_rt(rt, &opts, sizeof(opts), true, 1U) != 0) { free(rt); return NULL; }
    if (llam_async_preempt_register_safe_range(rt, (uintptr_t)llam_async_safe_leaf_begin,
                                              (uintptr_t)llam_async_safe_leaf_end) != 0 ||
        llam_async_preempt_enable_runtime(rt) != 0) { llam_runtime_destroy(rt); return NULL; }
    return rt;
}

static bool native_stopped(llam_runtime_t *rt) {
    for (size_t i = 0U; i < rt->active_shards + rt->managed_worker_count; ++i) {
        llam_worker_t *worker = worker_at(rt, i);
        if (atomic_load(&worker->native_thread_live) || atomic_load(&worker->current_p) != NULL ||
            atomic_load(&worker->async_preempt_state) != 0U || llam_worker_load_active_g(worker) != NULL ||
            worker->sigaltstack_installed) return false;
    }
    return true;
}

static int run_case(bool multi) {
    llam_runtime_t *rt = make_runtime(multi);
    if (rt == NULL) { perror("registered native setup"); return 1; }
    registered_case_t test = {.runtime = rt, .count = rt->active_shards + 1U};
    if (test.count > 3U) { llam_runtime_destroy(rt); return 1; }
    int result = 0;
    for (unsigned run = 0; run < 2U; ++run) {
        atomic_store(&test.release, 0U); atomic_store(&test.started, 0U);
        atomic_store(&test.finished, 0U); atomic_store(&test.errors, 0U);
        atomic_store(&test.timer_progress, 0U); atomic_store(&test.rescue_done, 0U);
        atomic_store(&test.rescued, 0U);
        llam_task_t *tasks[4] = {0};
        for (unsigned i = 0; i < test.count; ++i) {
            test.leaves[i] = (registered_leaf_t){&test, i, NULL};
            tasks[i] = llam_runtime_spawn_ex(rt, leaf_task, &test.leaves[i], NULL, 0U);
            if (tasks[i] == NULL) abort();
        }
        tasks[test.count] = llam_runtime_spawn_ex(rt, controller_task, &test, NULL, 0U);
        if (tasks[test.count] == NULL) abort();
        pthread_t timeout;
        if (pthread_create(&timeout, NULL, rescue, &test) != 0) abort();
        int run_result = llam_runtime_run_handle(rt);
        int run_error = errno;
        atomic_store_explicit(&test.rescue_done, 1U, memory_order_release);
        pthread_join(timeout, NULL);
        if (run_result != 0) {
            bool unsupported = run_error == ENOTSUP && atomic_load(&test.started) == 0U &&
                atomic_load(&rt->preempt_capability.failure_reason) == LLAM_PREEMPT_FAILURE_SIGNAL_FRAME_UNSUPPORTED;
            result = unsupported && native_stopped(rt) ? 77 : 1;
            fprintf(stderr, "registered Linux x86-64: startup refused errno=%d reason=%u; no native capture pass claimed\n",
                run_error, atomic_load(&rt->preempt_capability.failure_reason));
            break; /* NEW Gs are reclaimed by destroy; joining cannot execute them. */
        }
        CHECK(&test, atomic_load(&test.rescued) == 0U);
        for (unsigned i = 0; i <= test.count; ++i) CHECK(&test, llam_join(tasks[i]) == 0);
        uint64_t bytes = 1U, charge = 0U;
        CHECK(&test, llam_managed_async_memory_info(rt, &bytes, &charge) && bytes == 0U && charge != 0U);
        CHECK(&test, atomic_load(&test.finished) == test.count && atomic_load(&test.timer_progress) == 1U);
        for (size_t i = 0; i < rt->active_shards + rt->managed_worker_count; ++i) {
            llam_worker_t *worker = worker_at(rt, i);
            CHECK(&test, !atomic_load(&worker->native_thread_live) && atomic_load(&worker->current_p) == NULL &&
                atomic_load(&worker->async_preempt_state) == 0U && llam_worker_load_active_g(worker) == NULL &&
                atomic_load(&worker->incarnation) == run + 1U);
        }
        if (atomic_load(&test.errors) != 0U) { result = 1; break; }
    }
    if (result == 0) printf("registered Linux x86-64: %zu M/%u P, native registers/errno/mask, fresh P, timer/offload and repeated lifetime passed\n",
        rt->active_shards + rt->managed_worker_count, rt->active_shards);
    llam_runtime_destroy(rt);
    return result;
}

#if defined(LLAM_ENABLE_TEST_HOOKS)
static void startup_task(void *arg) {
    atomic_fetch_add_explicit((atomic_uint *)arg, 1U, memory_order_relaxed);
}
#endif

static int startup_abort(void) {
#if defined(LLAM_ENABLE_TEST_HOOKS)
    llam_runtime_t *rt = make_runtime(false);
    atomic_uint ran = ATOMIC_VAR_INIT(0U);
    if (rt == NULL) return 1;
    if (llam_runtime_spawn_ex(rt, startup_task, &ran, NULL, 0U) == NULL) {
        llam_runtime_destroy(rt); return 1;
    }
    llam_async_preempt_test_fail_worker(LLAM_ASYNC_TEST_FAIL_WORKER_PROBE, 0U);
    int rc = llam_runtime_run_handle(rt);
    int error = errno;
    bool good = rc == -1 && (error == EIO || error == ENOTSUP) && atomic_load(&ran) == 0U && native_stopped(rt);
    llam_runtime_destroy(rt);
    if (good) puts("registered Linux x86-64: failed all-M qualification admitted no G and retired native bindings");
    return good ? 0 : 1;
#else
    return 77;
#endif
}


#if defined(LLAM_ENABLE_TEST_HOOKS)
typedef struct affine_case {
    registered_case_t base;
    pthread_key_t key;
    pthread_t native;
    llam_worker_t *origin;
    llam_task_t *native_task;
    llam_task_t *handle;
    _Atomic(llam_async_preempt_state_t *) capture;
    uint64_t before_capture;
    atomic_uint callback_started, callback_release, peer_progress;
    atomic_uint destructor_started, destructor_release, destructor_done;
    atomic_uint consumer_done, finish_peer, run_done;
    int run_result, run_error;
} affine_case_t;

static void affine_destructor(void *arg) {
    affine_case_t *test = arg;
    CHECK(&test->base, pthread_equal(pthread_self(), test->native) &&
        test->native_task->stack_mapping != NULL &&
        atomic_load(&test->native_task->scan_refs) != 0U &&
        atomic_load(&test->native_task->reclaim_ready) == 0U);
    atomic_store_explicit(&test->destructor_started, 1U, memory_order_release);
    while (!atomic_load_explicit(&test->destructor_release, memory_order_acquire)) {
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
        (void)nanosleep(&delay, NULL);
    }
    atomic_store_explicit(&test->destructor_done, 1U, memory_order_release);
}

static void *affine_foreign(void *arg) {
    affine_case_t *test = arg;
    CHECK(&test->base, pthread_equal(pthread_self(), test->native) &&
        g_llam_tls_worker == test->origin && g_llam_tls_processor == NULL &&
        g_llam_tls_task == NULL && atomic_load(&test->origin->current_p) == NULL &&
        llam_worker_load_active_g(test->origin) == NULL);
    CHECK(&test->base, pthread_setspecific(test->key, test) == 0);
    atomic_store_explicit(&test->callback_started, 1U, memory_order_release);
    while (!atomic_load_explicit(&test->callback_release, memory_order_acquire)) {
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
        (void)nanosleep(&delay, NULL);
    }
    errno = ERANGE;
    return test;
}

static void affine_body(void *arg) {
    affine_case_t *test = arg;
    llam_processor_owner_ticket_t before, after;
    test->origin = g_llam_tls_worker;
    test->native = pthread_self();
    test->native_task = g_llam_tls_task;
    CHECK(&test->base, test->origin->role == LLAM_WORKER_ROLE_NATIVE_AFFINE &&
        test->native_task->managed_execution_class == LLAM_MANAGED_EXECUTION_NATIVE_AFFINE &&
        llam_managed_catalog_async_enabled(test->base.runtime));
    llam_async_preempt_state_t *state = (void *)atomic_load_explicit(
        &test->origin->async_preempt_state, memory_order_acquire);
    test->before_capture = atomic_load_explicit(&state->acknowledged_generation, memory_order_acquire);
    atomic_store_explicit(&test->capture, state, memory_order_release);
    atomic_store_explicit(&test->base.started, 1U, memory_order_release);
    CHECK(&test->base, llam_async_safe_leaf(&test->base.release) == 1U &&
        atomic_load_explicit(&state->acknowledged_generation, memory_order_acquire) > test->before_capture);
    CHECK(&test->base, llam_processor_owner_ticket_snapshot(g_llam_tls_processor, &before));
    void *result = NULL;
    CHECK(&test->base, llam_call_native_affine_result(affine_foreign, test, &result) == 0 &&
        result == test && errno == ERANGE);
    CHECK(&test->base, pthread_equal(pthread_self(), test->native) && g_llam_tls_worker == test->origin &&
        pthread_getspecific(test->key) == test &&
        llam_processor_owner_ticket_snapshot(g_llam_tls_processor, &after) &&
        (after.processor != before.processor || after.p_generation > before.p_generation));
    atomic_store_explicit(&test->base.finished, 1U, memory_order_release);
}

static void affine_peer(void *arg) {
    affine_case_t *test = arg;
    while (!atomic_load_explicit(&test->base.started, memory_order_acquire)) llam_yield();
    llam_async_preempt_state_t *state = atomic_load_explicit(&test->capture, memory_order_acquire);
    while (atomic_load_explicit(&state->acknowledged_generation, memory_order_acquire) <= test->before_capture &&
           !atomic_load_explicit(&test->base.rescued, memory_order_acquire)) llam_yield();
    atomic_store_explicit(&test->base.release, 1U, memory_order_release);
    while (!atomic_load_explicit(&test->callback_started, memory_order_acquire) &&
           !atomic_load_explicit(&test->base.rescued, memory_order_acquire)) llam_yield();
    CHECK(&test->base, atomic_load_explicit(&test->origin->current_p, memory_order_acquire) == NULL);
    atomic_store_explicit(&test->peer_progress, 1U, memory_order_release);
    atomic_store_explicit(&test->callback_release, 1U, memory_order_release);
    /* The affine callback can finish only after a fresh P claim. No ordinary
     * queue contains it: AUTO must observe genuine native-return pressure and
     * capture this portable CPU peer before the affine G can publish finished. */
    CHECK(&test->base, llam_async_safe_leaf(&test->base.finished) == 1U);
    while (!atomic_load_explicit(&test->finish_peer, memory_order_acquire)) llam_yield();
}

static void affine_consumer(void *arg) {
    affine_case_t *test = arg;
    CHECK(&test->base, llam_join(test->handle) == 0 && atomic_load(&test->destructor_done) == 1U);
    atomic_store_explicit(&test->consumer_done, 1U, memory_order_release);
}

static void *affine_driver(void *arg) {
    affine_case_t *test = arg;
    test->run_result = llam_runtime_run_handle(test->base.runtime);
    test->run_error = errno;
    atomic_store_explicit(&test->run_done, 1U, memory_order_release);
    return NULL;
}

static bool affine_wait(affine_case_t *test, atomic_uint *flag) {
    uint64_t deadline = llam_now_ns() + UINT64_C(8000000000);
    while (!atomic_load_explicit(flag, memory_order_acquire) &&
           !atomic_load_explicit(&test->run_done, memory_order_acquire) && llam_now_ns() < deadline) {
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
        (void)nanosleep(&delay, NULL);
    }
    return atomic_load_explicit(flag, memory_order_acquire) != 0U;
}

/* Test-only class descriptors do not certify arbitrary C bodies. Only the
 * existing assembly register oracle is in the signal-safe capture index. */
static int affine_register_fixture(llam_runtime_t *runtime) {
    llam_task_fn functions[3] = {affine_body, affine_peer, affine_consumer};
    llam_llpm2_entry_t entries[4] = {0};
    llam_llpm2_range_t range = {.entry_index = 3U, .kind = LLAM_LLPM2_SAFE,
        .begin = (uintptr_t)llam_async_safe_leaf_begin, .end = (uintptr_t)llam_async_safe_leaf_end,
        .stack_alignment_log2 = 4U};
    llam_llpm2_t record = {0};
    llam_module_t *module = NULL;
    record.header.architecture = 1U;
    record.header.native_abi = LLAM_LLPM2_ABI_SYSV;
    uint64_t type = llam_managed_task_type_id(record.header.architecture, record.header.native_abi);
    for (unsigned i = 0U; i < 3U; ++i) {
        entries[i].native_entry = entries[i].managed_entry = entries[i].code_begin = (uintptr_t)functions[i];
        entries[i].code_end = entries[i].code_begin + 1U;
        entries[i].execution_class = i == 0U ? LLAM_LLPM2_NATIVE_AFFINE : LLAM_LLPM2_PORTABLE;
        entries[i].type_id = type;
        entries[i].required_isa = LLAM_LLPM2_ISA_BASE | LLAM_LLPM2_ISA_FP_SIMD;
    }
    entries[3] = (llam_llpm2_entry_t){.native_entry = (uintptr_t)llam_async_safe_leaf,
        .managed_entry = (uintptr_t)llam_async_safe_leaf, .code_begin = range.begin, .code_end = range.end,
        .execution_class = LLAM_LLPM2_PORTABLE, .required_isa = LLAM_LLPM2_ISA_BASE | LLAM_LLPM2_ISA_FP_SIMD,
        .range_count = 1U, .flags = LLAM_LLPM2_ENTRY_INTERNAL, .type_id = type};
    record.entries = entries; record.entry_count = 4U; record.ranges = &range; record.range_count = 1U;
    return llam_managed_catalog_test_register(runtime, &record, &module);
}

static int affine_native_case(bool detached) {
    affine_case_t test = {0};
    llam_runtime_opts_t options;
    llam_preempt_opts_t policy;
    if (llam_runtime_opts_init(&options, sizeof(options)) != 0 ||
        llam_preempt_opts_init(&policy, sizeof(policy)) != 0) return 1;
    options.deterministic = 1U;
    options.preempt_mode = automatic_preemption ? LLAM_PREEMPT_AUTO : LLAM_PREEMPT_STRICT;
    options.preempt_quantum_ns = UINT64_C(1000000);
    policy.policy = LLAM_PREEMPT_POLICY_ASYNC_REQUIRED;
    policy.native_affine_capacity = 1U;
    if (pthread_key_create(&test.key, affine_destructor) != 0) return 1;
    if (llam_runtime_create_managed(&options, sizeof(options), &policy, sizeof(policy), &test.base.runtime) != 0)
        return 1;
    if (affine_register_fixture(test.base.runtime) != 0) abort();
    test.handle = llam_runtime_spawn_ex(test.base.runtime, affine_body, &test, NULL, 0U);
    llam_task_t *peer = llam_runtime_spawn_ex(test.base.runtime, affine_peer, &test, NULL, 0U);
    llam_task_t *consumer = detached ? NULL :
        llam_runtime_spawn_ex(test.base.runtime, affine_consumer, &test, NULL, 0U);
    if (test.handle == NULL || peer == NULL || (!detached && consumer == NULL)) abort();
    pthread_t driver;
    if (pthread_create(&driver, NULL, affine_driver, &test) != 0) abort();
    bool progress = affine_wait(&test, &test.peer_progress) && affine_wait(&test, &test.destructor_started);
    if (!progress && atomic_load_explicit(&test.run_done, memory_order_acquire) &&
        test.run_result == -1 && test.run_error == ENOTSUP && atomic_load(&test.base.started) == 0U &&
        atomic_load(&test.base.runtime->preempt_capability.failure_reason) == LLAM_PREEMPT_FAILURE_SIGNAL_FRAME_UNSUPPORTED) {
        pthread_join(driver, NULL);
        bool stopped = native_stopped(test.base.runtime);
        llam_runtime_destroy(test.base.runtime);
        pthread_key_delete(test.key);
        puts("registered Linux x86-64 affine: native signal ABI refused before user entry");
        return stopped ? 77 : 1;
    }
    CHECK(&test.base, progress && atomic_load(&test.base.finished) == 1U &&
        atomic_load(&test.consumer_done) == 0U && llam_managed_affine_available(test.base.runtime) == 0U);
    if (!progress) {
        atomic_store(&test.base.rescued, 1U); atomic_store(&test.base.release, 1U);
        atomic_store(&test.callback_release, 1U); atomic_store(&test.base.finished, 1U);
    }
    if (detached) CHECK(&test.base, llam_detach(test.handle) == 0);
    atomic_store_explicit(&test.destructor_release, 1U, memory_order_release);
    CHECK(&test.base, affine_wait(&test, &test.destructor_done));
    if (!detached) CHECK(&test.base, affine_wait(&test, &test.consumer_done));
    atomic_store_explicit(&test.finish_peer, 1U, memory_order_release);
    pthread_join(driver, NULL);
    CHECK(&test.base, test.run_result == 0 && llam_join(peer) == 0);
    if (consumer != NULL) CHECK(&test.base, llam_join(consumer) == 0);
    CHECK(&test.base, native_stopped(test.base.runtime) && llam_managed_affine_available(test.base.runtime) == 1U);
    uint64_t charged = 1U, per_task = 0U;
    CHECK(&test.base, llam_managed_async_memory_info(test.base.runtime, &charged, &per_task) && charged == 0U);
    llam_runtime_destroy(test.base.runtime);
    pthread_key_delete(test.key);
    return atomic_load(&test.base.errors) == 0U ? 0 : 1;
}

static int affine_native_suite(void) {
    int result = affine_native_case(false);
    if (result == 0) result = affine_native_case(true);
    if (result == 0) puts("registered Linux x86-64: captured affine G, no-P foreign callback, fresh-P return and join/detach TLS retirement passed");
    return result;
}
#else
static int affine_native_suite(void) { return 77; }
#endif

int main(int argc, char **argv) {
    if (argc == 2 && (strcmp(argv[1], "--native-affine") == 0 ||
                      strcmp(argv[1], "--native-affine-auto") == 0)) {
        automatic_preemption = strcmp(argv[1], "--native-affine-auto") == 0;
        return affine_native_suite();
    }
    if (argc == 2 && strcmp(argv[1], "--startup-abort") == 0) return startup_abort();
    bool multi = argc == 2 && strcmp(argv[1], "--multi-p") == 0;
    automatic_preemption = argc == 2 && strcmp(argv[1], "--auto") == 0;
    dynamic_processors = argc == 2 && strcmp(argv[1], "--dynamic") == 0;
    oversubscribed_processors = dynamic_processors ||
        (argc == 2 && strcmp(argv[1], "--oversubscribed-p") == 0);
#if !defined(LLAM_TEST_WRAP_CPU_COUNT)
    if (oversubscribed_processors) return 77;
#endif
    cpu_set_t original, selected;
    bool affinity = false;
    if (multi) {
        if (sched_getaffinity(0, sizeof(original), &original) != 0) return 1;
        CPU_ZERO(&selected);
        unsigned found = 0U;
        for (unsigned i = 0; i < CPU_SETSIZE && found < 2U; ++i)
            if (CPU_ISSET((int)i, &original)) { CPU_SET((int)i, &selected); ++found; }
        if (found < 2U) return 77;
        if (sched_setaffinity(0, sizeof(selected), &selected) != 0) return 1;
        affinity = true;
    }
    if (oversubscribed_processors) puts("registered Linux x86-64: explicit two-P oversubscription on one CPU");
    int rc = run_case(multi || oversubscribed_processors);
    if (affinity && sched_setaffinity(0, sizeof(original), &original) != 0) return 1;
    return rc;
}
#endif
