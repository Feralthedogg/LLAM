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
#include "llam/managed.h"
#include "fixtures/managed/cpu_workload.h"
#include <stdio.h>

#if !LLAM_PLATFORM_LINUX || !LLAM_ARCH_X86_64 || !defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
int main(void) { puts("Linux x86-64 managed C: native profile unavailable"); return 77; }
#else
#include <dlfcn.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "Linux managed C:%d: %s (errno=%d)\n", __LINE__, #condition, errno); \
    goto done; \
} } while (0)

static bool entry(void *module, const char *name, llam_task_fn *out) {
    void *address = dlsym(module, name);
    _Static_assert(sizeof(address) == sizeof(*out), "native C function pointer ABI");
    memcpy(out, &address, sizeof(*out));
    return address != NULL;
}

int main(int argc, char **argv) {
    bool affine = argc == 3 && strcmp(argv[2], "--affine") == 0;
    if (argc != 2 && !affine) return 2;
    llam_runtime_t *runtime = NULL;
    void *module = NULL;
    llam_runtime_opts_t options;
    llam_preempt_opts_t policy;
    llam_preempt_info_t info;
    llam_task_fn cpu, first, observer;
    llam_task_t *tasks[3] = {0};
    llam_managed_cpu_workload_t work[2] = {
        {.iterations = 60000000U, .seed = 11U}, {.iterations = 60000000U, .seed = 29U}
    };
    llam_managed_cpu_observation_t observation = {.first = &work[0], .second = &work[1]};
    int result = 1;
    module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (module == NULL) fprintf(stderr, "Linux compiler image: %s\n", dlerror());
    CHECK(module != NULL && entry(module, "managed_image_cpu_task", &cpu) &&
          entry(module, affine ? "managed_image_affine_task" : "managed_image_cpu_task", &first) &&
          entry(module, "managed_image_cpu_observer", &observer));
    CHECK(llam_runtime_opts_init(&options, sizeof(options)) == 0 &&
          llam_preempt_opts_init(&policy, sizeof(policy)) == 0);
    options.deterministic = 1U;
    options.preempt_mode = LLAM_PREEMPT_AUTO;
    options.preempt_quantum_ns = 1000000U;
    policy.policy = LLAM_PREEMPT_POLICY_ASYNC_REQUIRED;
    policy.native_affine_capacity = 1U;
    CHECK(llam_runtime_create_managed(&options, sizeof(options), &policy, sizeof(policy), &runtime) == 0);
    CHECK(dlclose(module) == 0);
    module = NULL;
    tasks[0] = llam_runtime_spawn_ex(runtime, first, &work[0], NULL, 0U);
    tasks[1] = llam_runtime_spawn_ex(runtime, cpu, &work[1], NULL, 0U);
    tasks[2] = llam_runtime_spawn_ex(runtime, observer, &observation, NULL, 0U);
    CHECK(tasks[0] != NULL && tasks[1] != NULL && tasks[2] != NULL);
    CHECK(llam_runtime_run_handle(runtime) == 0);
    CHECK(observation.both_started && observation.both_unfinished);
    for (unsigned i = 0U; i < 2U; ++i) {
        uint64_t n = work[i].iterations;
        CHECK(work[i].result == n * (3U * work[i].seed + 7U) + 3U * (n * (n - 1U) / 2U));
        llam_task_t *pinned = llam_task_resolve_public_handle(tasks[i]);
        CHECK(pinned != NULL);
        uint64_t generations = atomic_load_explicit(&pinned->run_generation, memory_order_acquire);
        llam_task_end_public_op(pinned);
        /* These compiler bodies contain no LLAM calls or polls. More than one
         * dispatch proves this exact G was captured and actually restored. */
        CHECK(generations > 1U);
    }
    uint64_t captures = 0U, restores = 0U;
    for (unsigned i = 0U; i < runtime->active_shards; ++i) {
        captures += atomic_load_explicit(&runtime->shards[i].metrics.async_preempt_captures, memory_order_acquire);
        restores += atomic_load_explicit(&runtime->shards[i].metrics.async_preempt_resumes, memory_order_acquire);
    }
    CHECK(captures >= 2U && captures == restores);
    for (unsigned i = 0U; i < 3U; ++i) CHECK(llam_join(tasks[i]) == 0);
    CHECK(llam_runtime_preempt_info_ex(runtime, &info, sizeof(info)) == 0 &&
          info.effective_policy == LLAM_PREEMPT_POLICY_ASYNC_REQUIRED &&
          info.capsule_charge_bytes == 0U && info.retained_references == 0U);
    for (size_t i = 0U; i < runtime->worker_slot_count; ++i) {
        const llam_worker_t *worker = runtime->worker_slots[i];
        CHECK(!atomic_load_explicit(&worker->native_thread_live, memory_order_acquire) &&
              atomic_load_explicit(&worker->async_preempt_state, memory_order_acquire) == 0U &&
              atomic_load_explicit(&worker->current_p, memory_order_acquire) == NULL);
    }
    puts(affine ? "Linux managed affine C: actual ELF admission, TLS, exact per-G native redispatch and drain passed"
                : "Linux managed C: actual ELF admission, poll-free C calls/arrays/atomics, per-G native redispatch and drain passed");
    result = 0;
done:
    llam_runtime_destroy(runtime);
    if (module != NULL) (void)dlclose(module);
    return result;
}
#endif
