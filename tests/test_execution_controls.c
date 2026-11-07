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
#include "execution_controls_internal.h"
#include "llam/executor.h"
#include "llam/managed.h"
#include "fixtures/managed/execution_controls.h"
#include <stdio.h>
#include <string.h>
#if defined(LLAM_EXECUTION_FIXTURE_PATH)
#include <dlfcn.h>
#endif

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "LLAM execution:%d: %s errno=%d\n", __LINE__, #expr, errno); return false; } } while (0)
#define TASK_CHECK(errors, expr) do { bool passed = (expr); if (!passed) { \
    fprintf(stderr, "LLAM critical:%d: %s errno=%d\n", __LINE__, #expr, errno); ++*(errors); } } while (0)

static void critical_compat_entry(void *argument) {
    unsigned *errors = argument;
    llam_worker_t *worker = g_llam_tls_worker;
    TASK_CHECK(errors, llam_preempt_region_leave() == -1 && errno == EINVAL);
    TASK_CHECK(errors, llam_preempt_region_enter() == 0);
    TASK_CHECK(errors, llam_preempt_region_enter() == 0);
    TASK_CHECK(errors, worker->preempt_region_depth == 2U);
    TASK_CHECK(errors, llam_sleep_ns(1U) == -1 && errno == ENOTSUP);
    TASK_CHECK(errors, llam_channel_create(1U) == NULL && errno == ENOTSUP);
    llam_managed_native_enter();
    TASK_CHECK(errors, llam_preempt_region_enter() == -1 && errno == ENOTSUP);
    TASK_CHECK(errors, llam_preempt_region_leave() == -1 && errno == ENOTSUP);
    llam_managed_native_leave();
    TASK_CHECK(errors, llam_preempt_region_leave() == 0);
    TASK_CHECK(errors, worker->preempt_region_depth == 1U);
    worker->preempt_region_depth = 65535U;
    atomic_store(&worker->native_domain_depth, 65535U);
    TASK_CHECK(errors, llam_preempt_region_enter() == -1 && errno == EOVERFLOW);
    atomic_store(&worker->native_domain_depth, 1U);
    worker->preempt_region_depth = 1U;
    TASK_CHECK(errors, llam_preempt_region_leave() == 0);
    TASK_CHECK(errors, worker->preempt_region_depth == 0U && worker->preempt_region_task == NULL);
    TASK_CHECK(errors, atomic_load(&worker->native_domain_depth) == 0U);
    TASK_CHECK(errors, llam_sleep_ns(1U) == 0);
}

static void critical_unbalanced_entry(void *argument) {
    (void)argument;
    if (llam_preempt_region_enter() != 0) _Exit(126);
}

static bool critical_compat(bool unbalanced) {
    CHECK(llam_preempt_region_enter() == -1 && errno == ENOTSUP);
    CHECK(llam_preempt_region_leave() == -1 && errno == ENOTSUP);
    llam_runtime_opts_t options;
    CHECK(llam_runtime_opts_init(&options, sizeof(options)) == 0);
    options.deterministic = 1U;
    options.worker_count = 1U;
    llam_runtime_t *runtime = NULL;
    CHECK(llam_runtime_create_compat(&options, sizeof(options), &runtime) == 0);
    unsigned errors = 0U;
    llam_task_fn entry = unbalanced ? critical_unbalanced_entry : critical_compat_entry;
    llam_task_t *task = llam_runtime_spawn_ex(runtime, entry, &errors, NULL, 0U);
    CHECK(task != NULL && llam_runtime_run_handle(runtime) == 0 && llam_join(task) == 0);
    llam_runtime_destroy(runtime);
    CHECK(errors == 0U && !unbalanced);
    puts("critical API: nested ownership, native callback refusal, suspension refusal and balanced restoration passed");
    return true;
}

#if defined(LLAM_EXECUTION_FIXTURE_PATH)
static bool task_symbol(void *module, const char *name, llam_task_fn *out) {
    void *symbol = dlsym(module, name);
    _Static_assert(sizeof(symbol) == sizeof(*out), "native function pointer ABI");
    memcpy(out, &symbol, sizeof(*out));
    return symbol != NULL;
}

static void *critical_probe(void *argument) {
    execution_controls_work_t *work = argument;
    uint64_t limit = llam_now_ns() + UINT64_C(3000000000);
    while (atomic_load(&work->phase) != 2U && llam_now_ns() < limit) {
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000L};
        nanosleep(&pause, NULL);
    }
    llam_runtime_stats_t before = {0}, after = {0};
    bool valid = atomic_load(&work->phase) == 2U &&
        llam_runtime_collect_stats_ex_handle(work->runtime, &before, sizeof(before)) == 0;
    if (llam_cancel_token_cancel(work->token) != 0) valid = false;
    struct timespec delay = {.tv_sec = 0, .tv_nsec = 20000000L};
    nanosleep(&delay, NULL);
    valid = llam_runtime_collect_stats_ex_handle(work->runtime, &after, sizeof(after)) == 0 &&
            valid && atomic_load(&work->phase) == 2U && before.preempt_yields != 0U &&
            before.preempt_yields == after.preempt_yields;
    if (!valid) {
        fprintf(stderr, "critical capture window: phase=%u preemptions=%llu/%llu\n", atomic_load(&work->phase),
                (unsigned long long)before.preempt_yields, (unsigned long long)after.preempt_yields);
        atomic_store(&work->errors, 1U);
    }
    atomic_store_explicit(&work->probe_done, 1U, memory_order_release);
    return NULL;
}

static bool critical_native_run(llam_runtime_t *runtime, llam_task_fn entry, llam_task_fn peer_entry) {
    execution_controls_work_t work = {.runtime = runtime};
    atomic_init(&work.peer_ready, 0U);
    atomic_init(&work.phase, 0U);
    atomic_init(&work.probe_done, 0U);
    atomic_init(&work.peer_done, 0U);
    atomic_init(&work.errors, 0U);
    atomic_init(&work.cancellation_seen, 0U);
    work.token = llam_runtime_cancel_token_create(runtime);
    CHECK(work.token != NULL);
    llam_spawn_opts_t spawn;
    CHECK(llam_spawn_opts_init(&spawn, sizeof(spawn)) == 0);
    spawn.cancel_token = work.token;
    llam_task_t *task = llam_runtime_spawn_ex(runtime, entry, &work, &spawn, sizeof(spawn));
    llam_task_t *peer = llam_runtime_spawn_ex(runtime, peer_entry, &work, NULL, 0U);
    CHECK(task != NULL && peer != NULL);
    pthread_t probe;
    CHECK(pthread_create(&probe, NULL, critical_probe, &work) == 0);
    CHECK(llam_runtime_run_handle(runtime) == 0 && pthread_join(probe, NULL) == 0);
    CHECK(llam_join(task) == 0 && llam_join(peer) == 0);
    fprintf(stderr, "critical result: errors=%u cancellation=%u iterations=%llu phase=%u peer=%u\n",
            atomic_load(&work.errors), atomic_load(&work.cancellation_seen),
            (unsigned long long)work.iterations, atomic_load(&work.phase), atomic_load(&work.peer_done));
    CHECK(atomic_load(&work.errors) == 0U && atomic_load(&work.cancellation_seen) == 1U && work.iterations != 0U);
    CHECK(llam_cancel_token_destroy(work.token) == 0);
    return true;
}

static bool critical_native(void) {
    void *module = dlopen(LLAM_EXECUTION_FIXTURE_PATH, RTLD_NOW | RTLD_LOCAL);
    if (module == NULL) fprintf(stderr, "fixture load: %s\n", dlerror());
    CHECK(module != NULL);
    llam_task_fn entry, peer;
    CHECK(task_symbol(module, "fixture_critical_entry", &entry));
    CHECK(task_symbol(module, "fixture_critical_peer", &peer));
    llam_runtime_opts_t options;
    CHECK(llam_runtime_opts_init(&options, sizeof(options)) == 0);
    options.deterministic = 1U;
    options.worker_count = 1U;
    options.preempt_quantum_ns = UINT64_C(1000000);
    llam_runtime_t *runtime = NULL;
    CHECK(llam_runtime_create(&options, sizeof(options), &runtime) == 0);
    bool success = critical_native_run(runtime, entry, peer);
    llam_runtime_destroy(runtime);
    CHECK(dlclose(module) == 0);
    CHECK(success);
    puts("qualified critical region: native progress outside, none inside, nested restore and pending cancellation passed");
    return true;
}
#endif

int main(int argc, char **argv) {
    if (argc == 1) return critical_compat(false) && local_executor_cases(false) ? 0 : 1;
    if (argc == 2 && strcmp(argv[1], "--local-cooperative") == 0) return local_executor_cases(false) ? 0 : 1;
    if (argc == 2 && strcmp(argv[1], "--local") == 0) return local_executor_cases(true) ? 0 : 1;
    if (argc == 2 && strcmp(argv[1], "--unbalanced") == 0) return critical_compat(true) ? 0 : 1;
#if defined(LLAM_EXECUTION_FIXTURE_PATH)
    if (argc == 2 && strcmp(argv[1], "--critical") == 0) return critical_native() ? 0 : 1;
#endif
    fprintf(stderr, "requested native execution fixture is unavailable\n");
    return 77;
}
