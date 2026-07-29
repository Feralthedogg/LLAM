/**
 * @file src/core/debug/debug_stack_cache.c
 * @brief Stack-cache statistics and human-readable diagnostics.
 *
 * @copyright Copyright 2026 Feralthedogg
 *
 * @par License
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "runtime_internal.h"

void llam_runtime_collect_stack_cache_stats(
    const llam_runtime_t *rt,
    llam_runtime_stats_t *stats) {
    if (rt == NULL || stats == NULL) {
        return;
    }
    stats->stack_cache_budget_bytes =
        rt->resource_plan.stack_cache_budget_bytes;
    stats->stack_cache_high_watermark_bytes =
        rt->resource_plan.stack_cache_high_watermark_bytes;
    stats->stack_cache_low_watermark_bytes =
        rt->resource_plan.stack_cache_low_watermark_bytes;
    stats->stack_cache_idle_ns = rt->resource_plan.stack_cache_idle_ns;
    stats->stack_cache_flags = rt->resource_plan.stack_cache_flags;
    stats->stack_cache_resident_valid =
        atomic_load_explicit(&rt->stack_cache_resident_valid,
                             memory_order_acquire);
    llam_stack_cache_account_snapshot(
        rt,
        &stats->stack_cache_cached_bytes,
        &stats->stack_cache_cached_mappings,
        &stats->stack_cache_committed_bytes);
    stats->stack_cache_trim_requests =
        atomic_load_explicit(&rt->stack_cache_trim_requests,
                             memory_order_acquire);
    stats->stack_cache_discarded_bytes =
        atomic_load_explicit(&rt->stack_cache_discarded_bytes,
                             memory_order_acquire);
    stats->stack_cache_released_bytes =
        atomic_load_explicit(&rt->stack_cache_released_bytes,
                             memory_order_acquire);
    stats->stack_cache_budget_rejections =
        atomic_load_explicit(&rt->stack_cache_budget_rejections,
                             memory_order_acquire);
    stats->stack_cache_secure_return_failures =
        atomic_load_explicit(&rt->stack_cache_secure_return_failures,
                             memory_order_acquire);
    stats->stack_cache_resident_bytes =
        atomic_load_explicit(&rt->stack_cache_resident_bytes,
                             memory_order_acquire);
    stats->stack_cache_resident_sample_ns =
        atomic_load_explicit(&rt->stack_cache_resident_sample_ns,
                             memory_order_acquire);
    llam_stack_cache_quarantine_snapshot(
        &stats->stack_cache_process_quarantine_bytes,
        &stats->stack_cache_process_quarantine_mappings);
}

void llam_runtime_dump_stack_cache(int fd, const llam_runtime_t *rt) {
    llam_runtime_stats_t stats;

    memset(&stats, 0, sizeof(stats));
    llam_runtime_collect_stack_cache_stats(rt, &stats);
    dprintf(fd,
            "stack_cache:\n"
            "  budget_bytes=%llu high_watermark_bytes=%llu "
            "low_watermark_bytes=%llu idle_ns=%llu flags=%u "
            "cached_bytes=%llu cached_mappings=%llu committed_bytes=%llu "
            "trim_requests=%llu discarded_bytes=%llu released_bytes=%llu "
            "budget_rejections=%llu secure_return_failures=%llu "
            "resident_valid=%u resident_bytes=%llu "
            "resident_sample_ns=%llu process_quarantine_bytes=%llu "
            "process_quarantine_mappings=%llu\n",
            (unsigned long long)stats.stack_cache_budget_bytes,
            (unsigned long long)stats.stack_cache_high_watermark_bytes,
            (unsigned long long)stats.stack_cache_low_watermark_bytes,
            (unsigned long long)stats.stack_cache_idle_ns,
            stats.stack_cache_flags,
            (unsigned long long)stats.stack_cache_cached_bytes,
            (unsigned long long)stats.stack_cache_cached_mappings,
            (unsigned long long)stats.stack_cache_committed_bytes,
            (unsigned long long)stats.stack_cache_trim_requests,
            (unsigned long long)stats.stack_cache_discarded_bytes,
            (unsigned long long)stats.stack_cache_released_bytes,
            (unsigned long long)stats.stack_cache_budget_rejections,
            (unsigned long long)stats.stack_cache_secure_return_failures,
            stats.stack_cache_resident_valid,
            (unsigned long long)stats.stack_cache_resident_bytes,
            (unsigned long long)stats.stack_cache_resident_sample_ns,
            (unsigned long long)
                stats.stack_cache_process_quarantine_bytes,
            (unsigned long long)
                stats.stack_cache_process_quarantine_mappings);
}
