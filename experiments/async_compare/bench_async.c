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
#include "bench_async.h"
#include <inttypes.h>

#if LLAM_PLATFORM_DARWIN && LLAM_ARCH_AARCH64 && LLAM_ASYNC_CONTEXT_SHARED
#include "../../tests/fixtures/async_darwin_leaf.h"
#define QUALIFICATION_ITERATIONS UINT64_C(300000000)

typedef struct qualification_item {
    llam_async_darwin_snapshot_t snapshot;
    bool valid;
} qualification_item_t;

static void qualification_task(void *arg) {
    qualification_item_t *item = arg;
    llam_task_t *task = g_llam_tls_task;
    uintptr_t low = (uintptr_t)task->stack_base;
    uintptr_t high = low + task->stack_size;
    int error = 121;
    errno = error;
    uint64_t result = llam_async_darwin_leaf(QUALIFICATION_ITERATIONS, &item->snapshot);
    item->valid = errno == error &&
                  result == QUALIFICATION_ITERATIONS * (QUALIFICATION_ITERATIONS + 1U) / 2U &&
                  llam_async_darwin_snapshot_valid(&item->snapshot, result, low, high);
}
#endif

void bench_async_options(llam_runtime_opts_t *options) {
    options->deterministic = 1U;
    options->experimental_flags = 0U;
    options->preempt_mode = LLAM_PREEMPT_AUTO;
    options->preempt_quantum_ns = UINT64_C(1000000);
}

int bench_async_prepare(llam_runtime_t *runtime, bench_async_evidence_t *evidence) {
#if LLAM_PLATFORM_DARWIN && LLAM_ARCH_AARCH64 && LLAM_ASYNC_CONTEXT_SHARED
    const llam_async_preempt_range_t range = {(uintptr_t)llam_async_darwin_leaf_begin,
                                              (uintptr_t)llam_async_darwin_leaf_end};
    qualification_item_t items[2] = {0};
    llam_task_t *tasks[2];
    llam_async_shared_stats_t stats;

    /* Controlled standalone benchmark process, default native ISA profile.
     * Only the existing audited no-call leaf is certified. Never claim that
     * arbitrary benchmark or runtime C instructions are async-safe. */
    if (llam_async_shared_enable(runtime, &range, 1U, true) != 0)
        return -1;
    for (unsigned i = 0U; i < 2U; ++i) {
        tasks[i] = llam_runtime_spawn_ex(runtime, qualification_task, &items[i], NULL, 0U);
        if (tasks[i] == NULL)
            return -1;
    }
    if (llam_runtime_run_handle(runtime) != 0)
        return -1;
    for (unsigned i = 0U; i < 2U; ++i) {
        if (llam_join(tasks[i]) != 0 || !items[i].valid) {
            errno = EPROTO;
            return -1;
        }
    }
    if (!llam_async_shared_stats(runtime, &stats) || stats.captures == 0U ||
        stats.captures != stats.restores || stats.execution_debt != 0U || stats.binding_ops != 0U ||
        stats.bindings_created != stats.bindings_closed) {
        errno = EPROTO;
        return -1;
    }
    evidence->qualification_captures = stats.captures;
    evidence->qualification_restores = stats.restores;
    return 0;
#else
    (void)runtime;
    (void)evidence;
    errno = ENOTSUP;
    return -1;
#endif
}

int bench_async_report(llam_runtime_t *runtime, const char *name, const bench_async_evidence_t *evidence) {
#if LLAM_PLATFORM_DARWIN && LLAM_ARCH_AARCH64 && LLAM_ASYNC_CONTEXT_SHARED
    llam_async_shared_stats_t stats;
    if (atomic_load_explicit(&runtime->async_preempt_enabled, memory_order_acquire) == 0U ||
        !llam_async_shared_stats(runtime, &stats) || stats.captures < evidence->qualification_captures ||
        stats.restores < evidence->qualification_restores || stats.captures != stats.restores ||
        stats.execution_debt != 0U || stats.binding_ops != 0U ||
        stats.bindings_created != stats.bindings_closed) {
        errno = EPROTO;
        return -1;
    }
    printf("[bench-async] case=%s enabled=1 workers=%u safe_ranges=%zu "
           "qualification_captures=%" PRIu64 " qualification_restores=%" PRIu64 " workload_captures=%" PRIu64
           " workload_restores=%" PRIu64 " execution_debt=%" PRIu64 " binding_ops=%" PRIu64 "\n",
           name, runtime->active_shards, runtime->async_preempt_range_count, evidence->qualification_captures,
           evidence->qualification_restores, stats.captures - evidence->qualification_captures,
           stats.restores - evidence->qualification_restores, stats.execution_debt, stats.binding_ops);
    return 0;
#else
    (void)runtime;
    (void)name;
    (void)evidence;
    errno = ENOTSUP;
    return -1;
#endif
}
