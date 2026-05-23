/**
 * @file src/core/runtime_debug_stats_json.c
 * @brief JSON writer for public runtime statistics snapshots.
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

static int llam_stats_json_u64(int fd, const char *name, uint64_t value, unsigned *field_count) {
    int rc = dprintf(fd,
                     "%s\"%s\":%llu",
                     *field_count == 0U ? "" : ",",
                     name,
                     (unsigned long long)value);

    if (rc < 0) {
        return -1;
    }
    *field_count += 1U;
    return 0;
}

int llam_runtime_write_stats_json_rt(llam_runtime_t *rt, int fd) {
    llam_runtime_stats_t stats;
    unsigned fields = 0U;

    if (fd < 0) {
        errno = EINVAL;
        return -1;
    }
    if (llam_runtime_collect_stats_ex_rt(rt, &stats, sizeof(stats)) != 0) {
        return -1;
    }
    if (dprintf(fd, "{") < 0 ||
        llam_stats_json_u64(fd, "ctx_switches", stats.ctx_switches, &fields) != 0 ||
        llam_stats_json_u64(fd, "yields", stats.yields, &fields) != 0 ||
        llam_stats_json_u64(fd, "parks", stats.parks, &fields) != 0 ||
        llam_stats_json_u64(fd, "wakes", stats.wakes, &fields) != 0 ||
        llam_stats_json_u64(fd, "steals", stats.steals, &fields) != 0 ||
        llam_stats_json_u64(fd, "migrations", stats.migrations, &fields) != 0 ||
        llam_stats_json_u64(fd, "blocking_calls", stats.blocking_calls, &fields) != 0 ||
        llam_stats_json_u64(fd, "blocking_completions", stats.blocking_completions, &fields) != 0 ||
        llam_stats_json_u64(fd, "io_submits", stats.io_submits, &fields) != 0 ||
        llam_stats_json_u64(fd, "io_submit_calls", stats.io_submit_calls, &fields) != 0 ||
        llam_stats_json_u64(fd, "io_submit_syscalls", stats.io_submit_syscalls, &fields) != 0 ||
        llam_stats_json_u64(fd, "io_completions", stats.io_completions, &fields) != 0 ||
        llam_stats_json_u64(fd, "idle_polls", stats.idle_polls, &fields) != 0 ||
        llam_stats_json_u64(fd, "idle_spin_loops", stats.idle_spin_loops, &fields) != 0 ||
        llam_stats_json_u64(fd, "idle_spin_hits", stats.idle_spin_hits, &fields) != 0 ||
        llam_stats_json_u64(fd, "idle_spin_fallbacks", stats.idle_spin_fallbacks, &fields) != 0 ||
        llam_stats_json_u64(fd, "idle_spin_ns", stats.idle_spin_ns, &fields) != 0 ||
        llam_stats_json_u64(fd, "queue_overflows", stats.queue_overflows, &fields) != 0 ||
        llam_stats_json_u64(fd, "overflow_depth", stats.overflow_depth, &fields) != 0 ||
        llam_stats_json_u64(fd, "active_workers", stats.active_workers, &fields) != 0 ||
        llam_stats_json_u64(fd, "online_workers", stats.online_workers, &fields) != 0 ||
        llam_stats_json_u64(fd, "online_workers_floor", stats.online_workers_floor, &fields) != 0 ||
        llam_stats_json_u64(fd, "online_workers_min", stats.online_workers_min, &fields) != 0 ||
        llam_stats_json_u64(fd, "online_workers_max", stats.online_workers_max, &fields) != 0 ||
        llam_stats_json_u64(fd, "active_nodes", stats.active_nodes, &fields) != 0 ||
        llam_stats_json_u64(fd, "dynamic_workers", stats.dynamic_workers, &fields) != 0 ||
        llam_stats_json_u64(fd, "worker_rings", stats.worker_rings, &fields) != 0 ||
        llam_stats_json_u64(fd, "worker_rings_multishot", stats.worker_rings_multishot, &fields) != 0 ||
        llam_stats_json_u64(fd, "lockfree_normq", stats.lockfree_normq, &fields) != 0 ||
        llam_stats_json_u64(fd, "huge_alloc", stats.huge_alloc, &fields) != 0 ||
        llam_stats_json_u64(fd, "sqpoll", stats.sqpoll, &fields) != 0 ||
        llam_stats_json_u64(fd, "opaque_block_ns", stats.opaque_block_ns, &fields) != 0 ||
        llam_stats_json_u64(fd, "opaque_block_samples", stats.opaque_block_samples, &fields) != 0 ||
        llam_stats_json_u64(fd, "opaque_block_max_ns", stats.opaque_block_max_ns, &fields) != 0 ||
        llam_stats_json_u64(fd, "opaque_enter_wait_ns", stats.opaque_enter_wait_ns, &fields) != 0 ||
        llam_stats_json_u64(fd, "opaque_enter_wait_samples", stats.opaque_enter_wait_samples, &fields) != 0 ||
        llam_stats_json_u64(fd, "opaque_enter_wait_max_ns", stats.opaque_enter_wait_max_ns, &fields) != 0 ||
        llam_stats_json_u64(fd, "opaque_leave_wait_ns", stats.opaque_leave_wait_ns, &fields) != 0 ||
        llam_stats_json_u64(fd, "opaque_leave_wait_samples", stats.opaque_leave_wait_samples, &fields) != 0 ||
        llam_stats_json_u64(fd, "opaque_leave_wait_max_ns", stats.opaque_leave_wait_max_ns, &fields) != 0 ||
        /*
         * Keep JSON in lockstep with llam_runtime_stats_t.  These handoff
         * counters are used by CI and benchmark logs to explain yield/direct
         * regressions without parsing the human-readable dump.
         */
        llam_stats_json_u64(fd, "yield_direct_attempts", stats.yield_direct_attempts, &fields) != 0 ||
        llam_stats_json_u64(fd, "yield_direct_fast_hits", stats.yield_direct_fast_hits, &fields) != 0 ||
        llam_stats_json_u64(fd, "yield_direct_locked_hits", stats.yield_direct_locked_hits, &fields) != 0 ||
        llam_stats_json_u64(fd, "yield_direct_fail_context", stats.yield_direct_fail_context, &fields) != 0 ||
        llam_stats_json_u64(fd, "yield_direct_fail_policy", stats.yield_direct_fail_policy, &fields) != 0 ||
        llam_stats_json_u64(fd, "yield_direct_fail_no_work", stats.yield_direct_fail_no_work, &fields) != 0 ||
        llam_stats_json_u64(fd, "yield_direct_fail_self", stats.yield_direct_fail_self, &fields) != 0 ||
        llam_stats_json_u64(fd, "yield_direct_fail_push", stats.yield_direct_fail_push, &fields) != 0 ||
        llam_stats_json_u64(fd, "preempt_requests", stats.preempt_requests, &fields) != 0 ||
        llam_stats_json_u64(fd, "preempt_yields", stats.preempt_yields, &fields) != 0 ||
        llam_stats_json_u64(fd, "preempt_suppressed", stats.preempt_suppressed, &fields) != 0 ||
        llam_stats_json_u64(fd, "preempt_signals", stats.preempt_signals, &fields) != 0 ||
        llam_stats_json_u64(fd, "preempt_mode", stats.preempt_mode, &fields) != 0 ||
        llam_stats_json_u64(fd, "preempt_poll_period", stats.preempt_poll_period, &fields) != 0 ||
        llam_stats_json_u64(fd, "preempt_quantum_ns", stats.preempt_quantum_ns, &fields) != 0 ||
        dprintf(fd, "}\n") < 0) {
        return -1;
    }
    return 0;
}

int llam_runtime_write_stats_json(int fd) {
    return llam_runtime_write_stats_json_rt(llam_runtime_default_storage(), fd);
}
