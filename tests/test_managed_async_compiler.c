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
#include "llam/managed.h"
#include "fixtures/managed/cpu_workload.h"

#include <stdio.h>
#include <inttypes.h>
#if !LLAM_PLATFORM_WINDOWS
#include <dlfcn.h>
#endif

#if !LLAM_ASYNC_CONTEXT_SHARED
int main(void) {
    puts("test_managed_async_compiler: skipped (shared native profile unavailable)");
    return 77;
}
#else
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "managed compiler async:%d: %s (errno=%d)\n", __LINE__, #condition, errno); \
    goto done; \
} } while (0)

static bool overlapping_native_captures(llam_runtime_t *runtime) {
    llam_async_shared_event_t events[512];
    size_t count = llam_async_shared_test_events(runtime, events, 512U);
    for (size_t i = 0U; i < count; ++i) {
        if (events[i].kind != LLAM_ASYNC_SHARED_TEST_READY) continue;
        for (size_t j = i + 1U; j < count; ++j) {
            if (events[j].kind == LLAM_ASYNC_SHARED_TEST_RESTORE &&
                events[j].task_id == events[i].task_id) break;
            if (events[j].kind == LLAM_ASYNC_SHARED_TEST_CAPTURE &&
                events[j].task_id != events[i].task_id) return true;
        }
    }
    return false;
}

static bool task_symbol(void *module, const char *name, llam_task_fn *out) {
    void *symbol = dlsym(module, name);
    _Static_assert(sizeof(symbol) == sizeof(*out), "qualified native function pointer ABI");
    memcpy(out, &symbol, sizeof(*out));
    return symbol != NULL;
}

typedef struct compiler_refill_run {
    llam_runtime_t *runtime;
    int result;
} compiler_refill_run_t;

static void *compiler_refill_driver(void *argument) {
    compiler_refill_run_t *run = argument;
    run->result = llam_runtime_run_handle(run->runtime);
    return NULL;
}

static int compiler_refill(const char *fixture) {
    llam_runtime_t *runtime = NULL;
    void *module = NULL;
    int result = 1;
    pthread_t driver;
    bool driver_started = false;
    compiler_refill_run_t run = {0};
    llam_runtime_opts_t options;
    llam_preempt_opts_t policy;
    llam_task_fn affine, keepalive_entry;
    llam_managed_cpu_workload_t keepalive = {.seed = 3U};
    llam_managed_cpu_workload_t work[3] = {
        {.iterations = 20000000U, .seed = 11U},
        {.iterations = 20000000U, .seed = 23U},
        {.iterations = 20000000U, .seed = 37U}
    };
    llam_task_t *keepalive_task = NULL;
    module = dlopen(fixture, RTLD_NOW | RTLD_LOCAL);
    CHECK(module != NULL && task_symbol(module, "managed_image_affine_refill_task", &affine) &&
          task_symbol(module, "managed_image_keepalive", &keepalive_entry));
    CHECK(llam_runtime_opts_init(&options, sizeof(options)) == 0 &&
          llam_preempt_opts_init(&policy, sizeof(policy)) == 0);
    options.deterministic = 1U;
    options.preempt_mode = LLAM_PREEMPT_AUTO;
    policy.policy = LLAM_PREEMPT_POLICY_ASYNC_REQUIRED;
    policy.native_affine_capacity = 1U;
    CHECK(llam_runtime_create_managed(&options, sizeof(options), &policy, sizeof(policy), &runtime) == 0);
    CHECK(dlclose(module) == 0);
    module = NULL;
    keepalive_task = llam_runtime_spawn_ex(runtime, keepalive_entry, &keepalive, NULL, 0U);
    CHECK(keepalive_task != NULL);
    run.runtime = runtime;
    CHECK(pthread_create(&driver, NULL, compiler_refill_driver, &run) == 0);
    driver_started = true;
    uint64_t startup_deadline = llam_now_ns() + UINT64_C(5000000000);
    while (!__atomic_load_n(&keepalive.started, __ATOMIC_ACQUIRE) && llam_now_ns() < startup_deadline) {
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
        (void)nanosleep(&delay, NULL);
    }
    CHECK(__atomic_load_n(&keepalive.started, __ATOMIC_ACQUIRE));
    uint64_t incarnation = 0U, captures = 0U;
    for (unsigned i = 0U; i < 3U; ++i) {
        llam_task_t *task = NULL;
        uint64_t current = 0U;
        uint64_t deadline = llam_now_ns() + UINT64_C(5000000000);
        do {
            if (llam_managed_affine_available(runtime) != 0U) {
                current = atomic_load_explicit(&runtime->managed_workers[0].incarnation,
                                               memory_order_acquire);
                task = llam_runtime_spawn_ex(runtime, affine, &work[i], NULL, 0U);
            } else {
                errno = EAGAIN;
            }
            if (task == NULL) {
                CHECK(errno == EAGAIN);
                const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
                (void)nanosleep(&delay, NULL);
            }
        } while (task == NULL && llam_now_ns() < deadline);
        CHECK(task != NULL);
        /* Hold this incarnation alive until its native capture is observed.
         * A fixed iteration count can finish between request samples. */
        llam_async_shared_stats_t observed = {0};
        do {
            CHECK(llam_async_shared_worker_stats(&runtime->managed_workers[0], &observed));
            if (observed.captures > captures) break;
            const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
            (void)nanosleep(&delay, NULL);
        } while (llam_now_ns() < deadline);
        __atomic_store_n(&work[i].capture_release, 1U, __ATOMIC_RELEASE);
        CHECK(observed.captures > captures && llam_join(task) == 0);
        llam_async_shared_stats_t stats = {0};
        CHECK(llam_async_shared_worker_stats(&runtime->managed_workers[0], &stats));
        if (current <= incarnation || stats.captures <= captures ||
            stats.captures != stats.restores || stats.restores != stats.witnesses) {
            fprintf(stderr, "refill wave %u: incarnation=%" PRIu64 "/%" PRIu64
                " captures=%" PRIu64 "/%" PRIu64 " restores=%" PRIu64 " witnesses=%" PRIu64 "\n",
                i, current, incarnation, stats.captures, captures, stats.restores, stats.witnesses);
        }
        CHECK(current > incarnation &&
              stats.captures > captures && stats.captures == stats.restores &&
              stats.restores == stats.witnesses);
        incarnation = current;
        captures = stats.captures;
        unsigned long long n = work[i].iterations;
        unsigned long long expected = n * (3U * work[i].seed + 7U) + 3U * (n * (n - 1U) / 2U);
        CHECK(work[i].result == expected);
    }
    __atomic_store_n(&keepalive.finished, 1U, __ATOMIC_RELEASE);
    CHECK(pthread_join(driver, NULL) == 0);
    driver_started = false;
    CHECK(run.result == 0 && llam_join(keepalive_task) == 0);
    llam_async_shared_stats_t total;
    CHECK(llam_async_shared_stats(runtime, &total) && total.charged_bytes == 0U &&
          total.execution_debt == 0U && total.binding_ops == 0U &&
          total.bindings_created == total.bindings_closed);
    puts("managed compiler refill: three captured TLS tasks, one affine slot, fresh native incarnations and exact results passed");
    result = 0;
done:
    for (unsigned i = 0U; i < 3U; ++i)
        __atomic_store_n(&work[i].capture_release, 1U, __ATOMIC_RELEASE);
    __atomic_store_n(&keepalive.finished, 1U, __ATOMIC_RELEASE);
    if (driver_started) {
        llam_request_stop(runtime);
        (void)pthread_join(driver, NULL);
    }
    llam_runtime_destroy(runtime);
    if (module != NULL) (void)dlclose(module);
    return result;
}

static int compiler_budget(const char *fixture) {
    llam_runtime_t *runtime = NULL;
    void *module = NULL;
    int result = 1;
    llam_runtime_opts_t options;
    llam_preempt_opts_t policy;
    llam_preempt_info_t info;
    llam_task_fn cpu;
    llam_managed_cpu_workload_t work = {.iterations = 100U, .seed = 7U};
    module = dlopen(fixture, RTLD_NOW | RTLD_LOCAL);
    CHECK(module != NULL && task_symbol(module, "managed_image_cpu_task", &cpu));
    CHECK(llam_runtime_opts_init(&options, sizeof(options)) == 0 &&
          llam_preempt_opts_init(&policy, sizeof(policy)) == 0);
    options.deterministic = 1U;
    policy.policy = LLAM_PREEMPT_POLICY_ASYNC_REQUIRED;
    policy.native_affine_capacity = 1U;
    policy.capsule_budget_bytes = 1U;
    CHECK(llam_runtime_create_managed(&options, sizeof(options), &policy, sizeof(policy), &runtime) == 0);
    CHECK(dlclose(module) == 0);
    module = NULL;
    CHECK(llam_runtime_preempt_info_ex(runtime, &info, sizeof(info)) == 0 &&
          info.conservative_task_charge_bytes > 1U && info.maximum_charged_tasks == 0U &&
          info.prepared_capsules == 0U && info.reserved_capsule_bytes == 0U);
    CHECK(llam_runtime_spawn_ex(runtime, cpu, &work, NULL, 0U) == NULL && errno == ENOMEM);
    CHECK(llam_runtime_preempt_info_ex(runtime, &info, sizeof(info)) == 0 &&
          info.retained_references == 0U && info.capsule_charge_bytes == 0U &&
          work.started == 0U && work.finished == 0U && work.result == 0U);
    result = 0;
    puts("managed compiler budget: quota refusal before G publication and complete admission rollback passed");
done:
    llam_runtime_destroy(runtime);
    if (module != NULL) (void)dlclose(module);
    return result;
}

static int compiler_cleanup(const char *fixture) {
    int result = 1;
    void *module = NULL;
    llam_runtime_t *runtime = NULL;
    llam_cancel_token_t *token = NULL;
    llam_managed_cleanup_workload_t work = {0};
    llam_runtime_opts_t options;
    llam_preempt_opts_t policy;
    llam_spawn_opts_t spawn;
    llam_task_fn entry, peer_entry;
    llam_managed_cpu_workload_t peer_work = {.iterations = 60000000U, .seed = 19U};
    module = dlopen(fixture, RTLD_NOW | RTLD_LOCAL);
    CHECK(module != NULL && task_symbol(module, "managed_image_cleanup_task", &entry) &&
          task_symbol(module, "managed_image_cpu_task", &peer_entry));
    CHECK(llam_runtime_opts_init(&options, sizeof(options)) == 0 &&
          llam_preempt_opts_init(&policy, sizeof(policy)) == 0 &&
          llam_spawn_opts_init(&spawn, sizeof(spawn)) == 0);
    options.deterministic = 1U;
    policy.policy = LLAM_PREEMPT_POLICY_ASYNC_REQUIRED;
    policy.native_affine_capacity = 1U;
    CHECK(llam_runtime_create_managed(&options, sizeof(options), &policy, sizeof(policy), &runtime) == 0);
    token = llam_runtime_cancel_token_create(runtime);
    CHECK(token != NULL);
    work.cancel_token = token;
    spawn.cancel_token = token;
    llam_task_t *peer = llam_runtime_spawn_ex(runtime, peer_entry, &peer_work, NULL, 0U);
    CHECK(peer != NULL);
    llam_task_t *task = llam_runtime_spawn_ex(runtime, entry, &work, &spawn, sizeof(spawn));
    CHECK(task != NULL && llam_runtime_run_handle(runtime) == 0);
    llam_task_result_t value;
    CHECK(llam_join_result(task, &value, sizeof(value)) == 0 && llam_join(peer) == 0);
    CHECK(work.error == 0U && work.cleaned == 1U && work.same_thread == 1U &&
          value.flags == LLAM_TASK_RESULT_F_SET && value.value == &work && value.error == 17);
    llam_async_shared_stats_t stats;
    CHECK(runtime->managed_worker_count == 1U &&
          llam_async_shared_worker_stats(&runtime->managed_workers[0], &stats) && stats.captures != 0U &&
          stats.captures == stats.restores && stats.restores == stats.witnesses);
    CHECK(llam_cancel_token_is_cancelled(token) == 1);
    puts("managed compiler cleanup: real image, async capture, cancellation, same-M cleanup and result passed");
    result = 0;
done:
    llam_runtime_destroy(runtime);
    if (token != NULL) (void)llam_cancel_token_destroy(token);
    if (work.key != 0U) (void)llam_task_local_key_delete(work.key);
    if (module != NULL) (void)dlclose(module);
    return result;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[2], "--cleanup") == 0) return compiler_cleanup(argv[1]);
    if (argc == 3 && strcmp(argv[2], "--refill") == 0) return compiler_refill(argv[1]);
    if (argc == 3 && strcmp(argv[2], "--budget") == 0) return compiler_budget(argv[1]);
    llam_runtime_t *runtime = NULL;
    llam_runtime_opts_t runtime_opts;
    llam_preempt_opts_t preempt_opts;
    llam_preempt_info_t info;
    llam_async_shared_stats_t stats;
    llam_task_t *tasks[3] = {0};
    llam_task_fn cpu, first_cpu, observer;
    bool affine = argc == 3 && strcmp(argv[2], "--affine") == 0;
    bool default_runtime = argc == 3 && strcmp(argv[2], "--default") == 0;
    bool default_global_prefix = argc == 3 && strcmp(argv[2], "--default-global-prefix") == 0;
    bool default_global = default_global_prefix ||
                          (argc == 3 && strcmp(argv[2], "--default-global") == 0);
    llam_managed_cpu_workload_t work[2] = {
        /* Finite native-capture oracle, including time spent sampling unsafe
         * call prologues/epilogues. Completion before capture is not a pass. */
        {.iterations = 60000000U, .seed = 11U},
        {.iterations = 60000000U, .seed = 29U}
    };
    llam_managed_cpu_observation_t observation = {.first = &work[0], .second = &work[1]};
    void *module = NULL;
    int result = 1;

    if (argc != 2 && !affine && !default_runtime && !default_global) return 2;
    module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (module == NULL) fprintf(stderr, "compiler fixture load: %s\n", dlerror());
    CHECK(module != NULL);
    CHECK(task_symbol(module, "managed_image_cpu_task", &cpu));
    CHECK(task_symbol(module, affine ? "managed_image_affine_task" : "managed_image_cpu_task", &first_cpu));
    CHECK(task_symbol(module, "managed_image_cpu_observer", &observer));
    CHECK(llam_runtime_opts_init(&runtime_opts, sizeof(runtime_opts)) == 0);
    CHECK(llam_preempt_opts_init(&preempt_opts, sizeof(preempt_opts)) == 0);
    runtime_opts.deterministic = 1U;
    if (default_global) {
        if (default_global_prefix) {
            runtime_opts.worker_count = UINT32_MAX;
            runtime_opts.blocking_worker_count = UINT32_MAX;
            CHECK(llam_runtime_init_ex(&runtime_opts, offsetof(llam_runtime_opts_t, worker_count)) == 0);
        } else CHECK(llam_runtime_init(&runtime_opts) == 0);
        runtime = llam_runtime_default();
    } else if (default_runtime) {
        CHECK(llam_runtime_create(&runtime_opts, sizeof(runtime_opts), &runtime) == 0);
    } else {
        runtime_opts.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
        runtime_opts.preempt_mode = LLAM_PREEMPT_AUTO;
        runtime_opts.preempt_quantum_ns = UINT64_C(1000000);
        preempt_opts.policy = LLAM_PREEMPT_POLICY_ASYNC_REQUIRED;
        preempt_opts.native_affine_capacity = 1U;
        CHECK(llam_runtime_create_managed(&runtime_opts, sizeof(runtime_opts),
            &preempt_opts, sizeof(preempt_opts), &runtime) == 0);
    }
    CHECK(llam_runtime_preempt_info_ex(runtime, &info, sizeof(info)) == 0);
    CHECK(info.registered_modules != 0U && info.registered_entries >= 3U &&
          info.conservative_task_charge_bytes != 0U && info.maximum_charged_tasks != 0U);
    /* Automatic discovery owns the image independently before host unload. */
    CHECK(dlclose(module) == 0);
    module = NULL;
    tasks[0] = default_global ? llam_spawn(first_cpu, &work[0], NULL)
                              : llam_runtime_spawn_ex(runtime, first_cpu, &work[0], NULL, 0U);
    tasks[1] = default_global ? llam_spawn(cpu, &work[1], NULL)
                              : llam_runtime_spawn_ex(runtime, cpu, &work[1], NULL, 0U);
    tasks[2] = default_global ? llam_spawn(observer, &observation, NULL)
                              : llam_runtime_spawn_ex(runtime, observer, &observation, NULL, 0U);
    CHECK(tasks[0] != NULL && tasks[1] != NULL && tasks[2] != NULL);
    CHECK(llam_runtime_preempt_info_ex(runtime, &info, sizeof(info)) == 0 &&
          info.capsule_charge_bytes == 3U * info.conservative_task_charge_bytes);
    CHECK((default_global ? llam_run() : llam_runtime_run_handle(runtime)) == 0);
    CHECK(llam_async_shared_stats(runtime, &stats));
    if (default_runtime || default_global) {
        llam_runtime_stats_t execution;
        CHECK(llam_runtime_collect_stats_ex_handle(runtime, &execution, sizeof(execution)) == 0);
        /* Shared native capture itself counts as a preempt yield. No user
         * yield or park may account for progress in this poll-free workload. */
        CHECK(execution.yields == stats.captures && execution.preempt_yields == stats.captures &&
              execution.parks == 0U);
    }
    if (!observation.both_started || !observation.both_unfinished) {
        fprintf(stderr, "ordinary C observer: started=%u unfinished=%u first_unfinished=%u requests=%" PRIu64
            " captures=%" PRIu64 " restores=%" PRIu64 " witnesses=%" PRIu64 "\n",
            observation.both_started, observation.both_unfinished, observation.first_unfinished,
            stats.requests, stats.captures, stats.restores, stats.witnesses);
    }
    CHECK(observation.both_started == 1U && observation.both_unfinished == 1U);
    if (!affine) CHECK(overlapping_native_captures(runtime));
    else {
        llam_async_shared_stats_t affine_stats;
        CHECK(runtime->managed_worker_count == 1U &&
              runtime->managed_workers[0].role == LLAM_WORKER_ROLE_NATIVE_AFFINE);
        CHECK(llam_async_shared_worker_stats(&runtime->managed_workers[0], &affine_stats) &&
              affine_stats.captures != 0U && affine_stats.captures == affine_stats.restores &&
              affine_stats.restores == affine_stats.witnesses);
    }
    for (unsigned i = 0U; i < 2U; ++i) {
        unsigned long long n = work[i].iterations;
        unsigned long long expected = n * (3U * work[i].seed + 7U) + 3U * (n * (n - 1U) / 2U);
        CHECK(work[i].result == expected && __atomic_load_n(&work[i].finished, __ATOMIC_ACQUIRE) == 1U);
    }
    for (unsigned i = 0U; i < 3U; ++i) {
        CHECK(llam_join(tasks[i]) == 0);
        tasks[i] = NULL;
    }
    CHECK(llam_async_shared_stats(runtime, &stats));
    CHECK(stats.captures >= 2U && stats.captures == stats.restores &&
          stats.restores == stats.witnesses && stats.execution_debt == 0U &&
          stats.binding_ops == 0U && stats.retire_depth == 0U && stats.finalizer_depth == 0U &&
          stats.charged_bytes == 0U &&
          stats.bindings_created >= runtime->portable_processor_count + runtime->managed_worker_count &&
          stats.bindings_closed == stats.bindings_created);
    CHECK(llam_runtime_preempt_info_ex(runtime, &info, sizeof(info)) == 0 &&
          info.requested_policy == LLAM_PREEMPT_POLICY_ASYNC_REQUIRED &&
          info.effective_policy == LLAM_PREEMPT_POLICY_ASYNC_REQUIRED &&
          info.reason == LLAM_PREEMPT_REASON_NONE && info.capsule_charge_bytes == 0U);
    puts(affine
        ? "managed compiler async affine: automatic admission, native TLS, borrowed P, peer progress and post-join drain passed"
        : "managed compiler async: automatic image admission, ordinary C calls/arrays/atomics, overlapping same-M captures, exact results and complete native drain passed");
    result = 0;
done:
    llam_runtime_destroy(runtime);
    if (module != NULL) (void)dlclose(module);
    return result;
}
#endif
