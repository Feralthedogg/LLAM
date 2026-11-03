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

#include "llam/runtime.h"
#include "llam/managed.h"
#include "fixtures/managed/cpu_workload.h"
#include <windows.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "Windows managed compiler:%d: %s errno=%d Win32=%lu\n", \
        __LINE__, #condition, errno, (unsigned long)GetLastError()); goto done; \
} } while (0)

static bool task_symbol(HMODULE module, const char *name, llam_task_fn *out) {
    FARPROC address = GetProcAddress(module, name);
    _Static_assert(sizeof(address) == sizeof(*out), "Windows native function pointer ABI");
    memcpy(out, &address, sizeof(*out));
    return address != NULL;
}

static bool fixture_mapped(const void *address, HMODULE expected) {
    MEMORY_BASIC_INFORMATION info;
    return VirtualQuery(address, &info, sizeof(info)) == sizeof(info) &&
        info.State == MEM_COMMIT && info.Type == MEM_IMAGE && info.AllocationBase == expected;
}

static DWORD WINAPI deadline_thread(void *argument) {
    if (WaitForSingleObject((HANDLE)argument, 25000U) == WAIT_TIMEOUT) ExitProcess(124U);
    return 0U;
}

typedef struct quantum_probe {
    llam_runtime_t *runtime;
    llam_managed_cpu_workload_t *work;
    llam_preempt_info_t snapshot;
    atomic_uint cancel;
    int result;
} quantum_probe_t;

static DWORD WINAPI observe_quantum(void *argument) {
    quantum_probe_t *probe = argument;
    while (!__atomic_load_n(&probe->work->started, __ATOMIC_ACQUIRE)) {
        if (atomic_load_explicit(&probe->cancel, memory_order_acquire) != 0U) return 0U;
        Sleep(1U);
    }
    probe->result = llam_runtime_preempt_info_ex(probe->runtime, &probe->snapshot, sizeof(probe->snapshot));
    return 0U;
}

int main(int argc, char **argv) {
    llam_runtime_t *runtime = NULL;
    llam_runtime_opts_t options;
    llam_preempt_opts_t preemption;
    llam_preempt_info_t info;
    llam_runtime_stats_t stats;
    llam_spawn_opts_t spawn;
    llam_task_t *tasks[3] = {0};
    llam_task_fn cpu = NULL, observer = NULL;
    llam_managed_cpu_workload_t work[2] = {
        {.iterations = 200000000U, .seed = 11U},
        {.iterations = 200000000U, .seed = 29U}
    };
    llam_managed_cpu_observation_t observation = {.first = &work[0], .second = &work[1]};
    HMODULE module = NULL, original_module = NULL;
    HANDLE completed = NULL, deadline = NULL, monitor = NULL;
    quantum_probe_t probe = {.result = -1};
    bool affine = false, automatic = false;
    const char *path = NULL;
    int result = 1;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--affine") == 0) affine = true;
        else if (strcmp(argv[i], "--auto") == 0) automatic = true;
        else path = argv[i];
    }
    if (path == NULL) return 2;
    completed = CreateEventA(NULL, TRUE, FALSE, NULL);
    CHECK(completed != NULL);
    deadline = CreateThread(NULL, 0U, deadline_thread, completed, 0U, NULL);
    CHECK(deadline != NULL);
    module = LoadLibraryA(path);
    CHECK(module != NULL);
    original_module = module;
    CHECK(task_symbol(module, "managed_image_cpu_task", &cpu));
    CHECK(task_symbol(module, "managed_image_cpu_observer", &observer));
    CHECK(llam_runtime_opts_init(&options, sizeof(options)) == 0);
    CHECK(llam_preempt_opts_init(&preemption, sizeof(preemption)) == 0);
    CHECK(llam_spawn_opts_init(&spawn, sizeof(spawn)) == 0);
    options.deterministic = 1U;
    options.preempt_mode = LLAM_PREEMPT_AUTO;
    options.preempt_quantum_ns = UINT64_C(1000000);
    preemption.policy = automatic ? LLAM_PREEMPT_POLICY_ASYNC_AUTO : LLAM_PREEMPT_POLICY_ASYNC_REQUIRED;
    preemption.native_affine_capacity = 2U;
    CHECK(llam_runtime_create_managed(&options, sizeof(options), &preemption, sizeof(preemption), &runtime) == 0);
    CHECK(llam_runtime_preempt_info_ex(runtime, &info, sizeof(info)) == 0);
    CHECK(info.registered_modules != 0U && info.registered_entries >= 3U &&
          info.conservative_task_charge_bytes != 0U && info.maximum_charged_tasks != 0U &&
          info.portable_processors == 1U);
    CHECK(FreeLibrary(module));
    module = NULL;
    CHECK(fixture_mapped(original_module, original_module));
    if (affine) spawn.flags = LLAM_SPAWN_F_NATIVE_AFFINE;
    tasks[0] = llam_runtime_spawn_ex(runtime, cpu, &work[0], &spawn, sizeof(spawn));
    tasks[1] = llam_runtime_spawn_ex(runtime, cpu, &work[1], &spawn, sizeof(spawn));
    tasks[2] = llam_runtime_spawn_ex(runtime, observer, &observation, NULL, 0U);
    CHECK(tasks[0] != NULL && tasks[1] != NULL && tasks[2] != NULL);
    CHECK(llam_runtime_preempt_info_ex(runtime, &info, sizeof(info)) == 0 &&
          info.capsule_charge_bytes == 3U * info.conservative_task_charge_bytes);
    probe.runtime = runtime;
    probe.work = &work[0];
    atomic_init(&probe.cancel, 0U);
    monitor = CreateThread(NULL, 0U, observe_quantum, &probe, 0U, NULL);
    CHECK(monitor != NULL);
    CHECK(llam_runtime_run_handle(runtime) == 0);
    CHECK(WaitForSingleObject(monitor, INFINITE) == WAIT_OBJECT_0);
    CHECK(CloseHandle(monitor));
    monitor = NULL;
    CHECK(probe.result == 0 &&
          (probe.snapshot.flags & (LLAM_PREEMPT_INFO_F_RUNNING | LLAM_PREEMPT_INFO_F_COMMITTED)) ==
              (LLAM_PREEMPT_INFO_F_RUNNING | LLAM_PREEMPT_INFO_F_COMMITTED));
    for (unsigned i = 0U; i < 3U; ++i) CHECK(probe.snapshot.effective_quantum_ns[i] == UINT64_C(5000000));
    /* Exactly one portable P, no cooperative calls in either compiled body:
     * observing both started and unfinished requires interruption of both CPU
     * tasks. The isolated compatibility P cannot execute these managed Gs. */
    if (observation.both_started != 1U || observation.both_unfinished != 1U) {
        (void)llam_runtime_collect_stats_ex_handle(runtime, &stats, sizeof(stats));
        (void)llam_runtime_preempt_info_ex(runtime, &info, sizeof(info));
        fprintf(stderr, "observation started=%u unfinished=%u final=%u/%u switches=%" PRIu64
            " yields=%" PRIu64 " parks=%" PRIu64 " policy=%u reason=%u\n",
            observation.both_started, observation.both_unfinished, work[0].finished, work[1].finished,
            stats.ctx_switches, stats.yields, stats.parks, info.effective_policy, info.reason);
    }
    CHECK(observation.both_started == 1U && observation.both_unfinished == 1U);
    for (unsigned i = 0U; i < 2U; ++i) {
        unsigned long long n = work[i].iterations;
        unsigned long long expected = n * (3U * work[i].seed + 7U) + 3U * (n * (n - 1U) / 2U);
        CHECK(work[i].result == expected && __atomic_load_n(&work[i].finished, __ATOMIC_ACQUIRE) == 1U);
    }
    for (unsigned i = 0U; i < 3U; ++i) {
        CHECK(llam_join(tasks[i]) == 0);
        tasks[i] = NULL;
    }
    CHECK(llam_runtime_collect_stats_ex_handle(runtime, &stats, sizeof(stats)) == 0 &&
          stats.ctx_switches >= 5U && stats.yields == 0U && stats.parks == 0U);
    CHECK(llam_runtime_preempt_info_ex(runtime, &info, sizeof(info)) == 0 &&
          info.requested_policy == preemption.policy && info.effective_policy == preemption.policy &&
          info.reason == LLAM_PREEMPT_REASON_NONE && info.capsule_charge_bytes == 0U &&
          info.retained_references == 0U &&
          info.effective_quantum_ns[0] == 0U &&
          info.effective_quantum_ns[1] == 0U &&
          info.effective_quantum_ns[2] == 0U);
    llam_runtime_destroy(runtime);
    runtime = NULL;
    CHECK(!fixture_mapped(original_module, original_module));
    puts("Windows ordinary C: shared SDK, sealed LLPM2 automatic admission, independent module pin, "
         "two busy bodies interrupted, exact calls/arrays/atomics, capsule and loader drain passed");
    result = 0;
done:
    atomic_store_explicit(&probe.cancel, 1U, memory_order_release);
    if (monitor != NULL) { (void)WaitForSingleObject(monitor, INFINITE); (void)CloseHandle(monitor); }
    llam_runtime_destroy(runtime);
    if (module != NULL) (void)FreeLibrary(module);
    if (completed != NULL) (void)SetEvent(completed);
    if (deadline != NULL) { (void)WaitForSingleObject(deadline, 1000U); (void)CloseHandle(deadline); }
    if (completed != NULL) (void)CloseHandle(completed);
    return result;
}
