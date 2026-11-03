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
#include "windows_async_lifetime.h"
#include <inttypes.h>
#include <stdio.h>

#if !LLAM_PLATFORM_WINDOWS || !LLAM_ARCH_X86_64 || !defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
int main(void) { return 77; }
#else
#define COMPUTE_ITERATIONS UINT64_C(240000000)
/* Existing compiler-certified DLL qualifies the native transport fixture.
 * Ordinary C SDK admission has a separate final-image/runtime test. */
typedef unsigned long long (*compute_fn_t)(unsigned long long);
typedef struct run_case run_case_t;
typedef struct compute_case {
    run_case_t *run;
    uintptr_t task_key;
    uintptr_t worker_key;
    uint64_t before_generation;
    const llam_processor_t *before_processor;
    unsigned index;
} compute_case_t;
struct run_case {
    llam_runtime_t *runtime;
    compute_fn_t compute;
    uint64_t expected;
    atomic_uint started;
    atomic_uint finished;
    atomic_uint observed;
    atomic_uint errors;
    bool stopping;
    bool destroying;
};

static void check_at(run_case_t *run, bool valid, unsigned line) {
    if (valid) return;
    fprintf(stderr, "registered Windows check line %u, errno=%d\n", line, errno);
    atomic_fetch_add_explicit(&run->errors, 1U, memory_order_relaxed);
}
#define check(run, valid) check_at((run), (valid), __LINE__)

static uint64_t current_generation(run_case_t *run, const llam_processor_t **processor_out) {
    llam_processor_owner_ticket_t ticket;
    llam_processor_t *processor = g_llam_tls_processor;
    llam_worker_t *worker = g_llam_tls_worker;
    *processor_out = processor;
    if (llam_processor_owner_ticket_snapshot(processor, &ticket)) {
        check(run, ticket.owner_m == worker && ticket.lease_state == LLAM_LEASE_REGISTERED_OWNED);
        return ticket.p_generation;
    }
    if (run->destroying && atomic_load_explicit(&run->runtime->shutdown_requested, memory_order_acquire)) {
        llam_shard_t *shard = processor->compat_shard;
        uint64_t deadline = llam_now_ns() + UINT64_C(1000000000);
        for (;;) {
            llam_processor_owner_record_t record = {0};
            pthread_mutex_lock(&shard->opaque_lock);
            pthread_mutex_lock(&shard->lock);
            bool provenance = llam_processor_owner_snapshot_pair_locked(processor, &record) &&
                llam_processor_owner_record_well_formed(&record) && record.owner_m == worker &&
                atomic_load_explicit(&worker->current_p, memory_order_acquire) == processor &&
                record.worker_incarnation == atomic_load_explicit(&worker->binding_incarnation, memory_order_acquire) &&
                record.p_generation == atomic_load_explicit(&worker->async_preempt_ticket_generation, memory_order_acquire) &&
                record.lease_projection_epoch == atomic_load_explicit(&worker->async_preempt_ticket_lease_epoch, memory_order_acquire);
            bool stopped = provenance && record.state == LLAM_PROCESSOR_OWNER_STOPPING &&
                record.lease_state == LLAM_LEASE_STOPPING;
            bool revoke_gap = provenance && record.state == LLAM_PROCESSOR_OWNER_OWNED &&
                record.lease_state == LLAM_LEASE_REGISTERED_OWNED;
            pthread_mutex_unlock(&shard->lock);
            pthread_mutex_unlock(&shard->opaque_lock);
            if (stopped) return record.p_generation;
            /* shutdown_requested precedes its locked STOPPING projection.
             * An already running G can observe this deliberate revoke gap;
             * do not mistake it for an execute grant or an invalid owner. */
            if (revoke_gap && llam_now_ns() < deadline) { Sleep(0U); continue; }
            fprintf(stderr, "invalid drain snapshot state=%u lease=%u provenance=%u\n",
                    (unsigned)record.state, (unsigned)record.lease_state, provenance ? 1U : 0U);
            check(run, false);
            return 0U;
        }
    }
    check(run, false);
    return 0U;
}

static uint64_t reference(uint64_t count) {
    uint64_t value = 1U;
    for (uint64_t index = 0U; index < count; ++index)
        value = ((value << 7U) ^ (value >> 3U)) + index;
    return value;
}

static void compute_task(void *argument) {
    compute_case_t *item = argument;
    run_case_t *run = item->run;
    llam_worker_t *origin = g_llam_tls_worker;
    DWORD thread_id = GetCurrentThreadId();
    item->task_key = (uintptr_t)g_llam_tls_task;
    item->worker_key = (uintptr_t)origin;
    item->before_generation = current_generation(run, &item->before_processor);
    atomic_fetch_add_explicit(&run->started, 1U, memory_order_release);
    int error = 79 + (int)item->index;
    DWORD last_error = 900U + item->index;
    errno = error;
    SetLastError(last_error);
    uint64_t actual = run->compute(COMPUTE_ITERATIONS);
    int observed_error = errno;
    DWORD observed_last_error = GetLastError();
    check(run, actual == run->expected && observed_error == error && observed_last_error == last_error);
    check(run, origin == g_llam_tls_worker && thread_id == GetCurrentThreadId());
    const llam_processor_t *after_processor;
    uint64_t after_generation = current_generation(run, &after_processor);
    /* P generations are local to P. A same-M native restore may borrow a
     * different P whose valid fresh generation is numerically smaller. */
    check(run, after_processor != item->before_processor
        ? after_generation != 0U : after_generation > item->before_generation);
    atomic_fetch_add_explicit(&run->finished, 1U, memory_order_release);
}

static void observer_task(void *argument) {
    run_case_t *run = argument;
    int slept = llam_sleep_ns(UINT64_C(1000000));
    check(run, slept == 0 || (run->stopping && slept == -1 && errno == ECANCELED));
    if (!run->stopping) check(run, atomic_load_explicit(&run->finished, memory_order_acquire) < 2U);
    atomic_store_explicit(&run->observed, 1U, memory_order_release);
}

static DWORD WINAPI stop_thread(void *argument) {
    run_case_t *run = argument;
    while (atomic_load_explicit(&run->started, memory_order_acquire) == 0U) Sleep(1U);
    if (run->destroying) llam_runtime_destroy(run->runtime);
    else llam_request_stop(run->runtime);
    return 0U;
}

static DWORD WINAPI deadline_thread(void *argument) {
    if (WaitForSingleObject((HANDLE)argument, 25000U) == WAIT_TIMEOUT) ExitProcess(124U);
    return 0U;
}

static bool trace_complete(llam_runtime_t *runtime, const compute_case_t *items, uint64_t run_id) {
    llam_windows_async_test_event_t *events = calloc(LLAM_WINDOWS_ASYNC_TEST_TRACE_CAPACITY, sizeof(*events));
    bool covered[2] = {false, false};
    if (events == NULL) return false;
    size_t count = llam_windows_async_lifetime_test_trace(runtime, events, LLAM_WINDOWS_ASYNC_TEST_TRACE_CAPACITY);
    for (size_t i = 0U; i < count; ++i) {
        if (events[i].kind != LLAM_WINDOWS_ASYNC_TEST_RESTORE || events[i].runtime_run != run_id) continue;
        for (unsigned j = 0U; j < 2U; ++j)
            if (events[i].task_key == items[j].task_key && events[i].worker_key == items[j].worker_key &&
                events[i].restore_task_run > events[i].capture_task_run &&
                events[i].restore_worker_run > events[i].capture_worker_run) covered[j] = true;
    }
    free(events);
    return covered[0] && covered[1];
}

int main(int argc, char **argv) {
    bool multi = false, automatic = false, startup_abort = false, stopping = false, destroying = false;
    bool dynamic = false;
    const char *library = NULL;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--multi-p") == 0) multi = true;
        else if (strcmp(argv[i], "--dynamic") == 0) { dynamic = true; multi = true; }
        else if (strcmp(argv[i], "--auto") == 0) automatic = true;
        else if (strcmp(argv[i], "--startup-abort") == 0) startup_abort = true;
        else if (strcmp(argv[i], "--stop") == 0) stopping = true;
        else if (strcmp(argv[i], "--destroy") == 0) { stopping = true; destroying = true; }
        else library = argv[i];
    }
    if (library == NULL) { puts("registered Windows: certified fixture DLL required"); return 77; }
    DWORD_PTR process_mask = 0U, system_mask = 0U, chosen = 0U;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask)) return 1;
    for (unsigned i = 0U, count = 0U; i < sizeof(DWORD_PTR) * CHAR_BIT && count < 2U; ++i) {
        DWORD_PTR bit = (DWORD_PTR)1U << i;
        if ((process_mask & bit) != 0U) { chosen |= bit; ++count; }
    }
    if (!SetProcessAffinityMask(GetCurrentProcess(), chosen)) return 1;
    HANDLE done = CreateEventA(NULL, TRUE, FALSE, NULL);
    HANDLE deadline = done != NULL ? CreateThread(NULL, 0U, deadline_thread, done, 0U, NULL) : NULL;
    if (deadline == NULL) return 1;
    HMODULE module = LoadLibraryA(library);
    FARPROC address = module != NULL ? GetProcAddress(module, "llam_r8_cpu_loop_v1") : NULL;
    llam_windows_async_pe_range_set_t ranges = {0};
    if (address == NULL || llam_windows_async_pe_collect_ranges(module, &ranges) != 0) return 1;
    run_case_t run = {.stopping = stopping, .destroying = destroying};
    memcpy(&run.compute, &address, sizeof(run.compute));
    run.expected = reference(COMPUTE_ITERATIONS);
    llam_runtime_opts_t options;
    if (llam_runtime_opts_init(&options, sizeof(options)) != 0) return 1;
    options.deterministic = multi ? 0U : 1U;
    options.preempt_mode = automatic ? LLAM_PREEMPT_AUTO : LLAM_PREEMPT_STRICT;
    options.preempt_quantum_ns = UINT64_C(1000000);
    if (dynamic) options.experimental_flags |= LLAM_RUNTIME_EXPERIMENTAL_F_DYNAMIC_WORKERS;
    run.runtime = calloc(1U, sizeof(*run.runtime));
    if (run.runtime == NULL || llam_runtime_init_registered_rt(run.runtime, &options, sizeof(options), true, 1U) != 0) return 1;
    for (size_t i = 0U; i < ranges.range_count; ++i)
        if (llam_windows_async_lifetime_register_safe_range(run.runtime,
              (uintptr_t)ranges.ranges[i].begin_pc, (uintptr_t)ranges.ranges[i].end_pc) != 0) return 1;
    if (llam_windows_async_prepare_registered(run.runtime) != 0) {
        int code = errno == ENOTSUP ? 77 : 1;
        llam_runtime_destroy(run.runtime);
        return code;
    }
    llam_windows_async_lifetime_test_error_sentinels(run.runtime);
    for (unsigned round = 0U; round < (stopping ? 1U : 2U); ++round) {
        compute_case_t items[2] = {{.run=&run,.index=0U}, {.run=&run,.index=1U}};
        llam_task_t *tasks[3];
        atomic_store(&run.started, 0U);
        atomic_store(&run.finished, 0U);
        atomic_store(&run.observed, 0U);
        for (unsigned i = 0U; i < 2U; ++i) {
            tasks[i] = llam_runtime_spawn_ex(run.runtime, compute_task, &items[i], NULL, 0U);
            if (tasks[i] == NULL) return 1;
        }
        tasks[2] = llam_runtime_spawn_ex(run.runtime, observer_task, &run, NULL, 0U);
        if (tasks[2] == NULL) return 1;
        if (startup_abort && round == 0U) {
            llam_windows_async_lifetime_test_fail_worker(LLAM_WINDOWS_ASYNC_TEST_FAIL_QUALIFICATION, 1U);
            check(&run, llam_runtime_run_handle(run.runtime) == -1 && errno == ENOTSUP);
            check(&run, atomic_load(&run.started) == 0U && atomic_load(&run.observed) == 0U);
        }
        HANDLE stopper = stopping ? CreateThread(NULL, 0U, stop_thread, &run, 0U, NULL) : NULL;
        if (stopping && stopper == NULL) return 1;
        if (llam_runtime_run_handle(run.runtime) != 0) { fprintf(stderr, "registered run failed errno=%d\n", errno); return 1; }
        if (stopper != NULL) {
            check(&run, WaitForSingleObject(stopper, 10000U) == WAIT_OBJECT_0);
            CloseHandle(stopper);
        }
        if (destroying) {
            run.runtime = NULL; /* Destroy consumed every runtime/task handle. */
            check(&run, atomic_load(&run.started) == 2U && atomic_load(&run.finished) == 2U);
            break;
        }
        for (unsigned i = 0U; i < 3U; ++i) check(&run, llam_join(tasks[i]) == 0);
        check(&run, atomic_load(&run.started) == 2U && atomic_load(&run.finished) == 2U && atomic_load(&run.observed) == 1U);
        check(&run, trace_complete(run.runtime, items, round + 1U + (startup_abort ? 1U : 0U)));
        llam_windows_async_stats_t stats;
        check(&run, llam_windows_async_lifetime_stats(run.runtime, &stats));
        check(&run, stats.captures >= 2U && stats.captures == stats.restores && stats.binding_ops == 0U &&
            stats.captured_bank_active == 0U && stats.pending_restore_active == 0U &&
            stats.execution_debt == 0U && stats.retire_count == 0U && stats.finalizer_count == 0U &&
            stats.native_handles_created == stats.native_handles_closed && stats.remote_transactions_max == 1U &&
            llam_windows_async_lifetime_charged_bytes(run.runtime) == 0U);
    }
    unsigned errors = atomic_load(&run.errors);
    if (run.runtime != NULL) llam_runtime_destroy(run.runtime);
    llam_windows_async_pe_release_ranges(&ranges);
    if (!FreeLibrary(module) || !SetEvent(done) || WaitForSingleObject(deadline, 1000U) != WAIT_OBJECT_0) return 1;
    CloseHandle(deadline);
    CloseHandle(done);
    if (errors != 0U) return 1;
    printf("registered Windows: %s, %s, exact finite results/error state, original M/fresh P, timer progress, native lifetime drained\n",
           multi ? "3 M / 2 P" : "2 M / 1 P", destroying ? "destructive shutdown" : stopping ? "stop drain" : "two runs");
    return 0;
}
#endif
