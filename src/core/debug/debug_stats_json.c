/**
 * @file src/core/debug/debug_stats_json.c
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

typedef struct llam_autotune_json_snapshot {
    unsigned mode;
    unsigned phase;
    uint64_t recognized_domains;
    uint64_t observable_domains;
    uint64_t controllable_domains;
    uint64_t active_observation_domains;
    uint64_t active_control_domains;
    uint64_t supported_domains;
    uint64_t active_domains;
    uint64_t suspended_domains;
    uint64_t policy_epoch;
    uint64_t decisions;
    uint64_t commits;
    uint64_t rollbacks;
    uint64_t guardrail_trips;
    uint64_t decision_interval_ns;
    uint64_t min_hold_ns;
    uint64_t target_wake_p99_ns;
    unsigned sample_period;
    uint64_t last_decision_ns;
    uint64_t last_change_ns;
    uint64_t last_reason_mask;
    unsigned target_online_workers;
    unsigned current_online_workers;
    uint64_t idle_spin_ns;
    unsigned yield_handoff_budget;
    unsigned wake_handoff_budget;
    unsigned preempt_poll_period;
    uint64_t preempt_quantum_ns;
    uint64_t sampled_yield_handoff_attempts;
    uint64_t sampled_yield_handoff_hits;
    uint64_t sampled_yield_handoff_fail_policy;
    uint64_t sampled_yield_handoff_fail_no_work;
    uint64_t sampled_yield_handoff_fail_push;
    uint64_t sampled_wake_handoff_attempts;
    uint64_t sampled_wake_handoff_hits;
    uint64_t sampled_wake_handoff_fail_policy;
    uint64_t sampled_wake_handoff_fail_race;
    uint64_t sampled_wake_latency_samples;
    uint64_t sampled_wake_latency_p50_ns;
    uint64_t sampled_wake_latency_p99_ns;
    uint64_t sampled_idle_spin_hits;
    uint64_t sampled_idle_spin_fallbacks;
    uint64_t sampled_idle_spin_ns;
    uint64_t sampled_queue_overflows;
} llam_autotune_json_snapshot_t;

static const char *llam_autotune_mode_name(unsigned mode) {
    switch (mode) {
    case LLAM_AUTOTUNE_INTERNAL_OBSERVE:
        return "observe";
    case LLAM_AUTOTUNE_INTERNAL_ON:
        return "on";
    case LLAM_AUTOTUNE_INTERNAL_FROZEN:
        return "frozen";
    case LLAM_AUTOTUNE_INTERNAL_OFF:
    default:
        return "off";
    }
}

static const char *llam_autotune_phase_name(unsigned phase) {
    switch (phase) {
    case LLAM_AUTOTUNE_INTERNAL_PHASE_WARMUP:
        return "warmup";
    case LLAM_AUTOTUNE_INTERNAL_PHASE_HOLD:
        return "hold";
    case LLAM_AUTOTUNE_INTERNAL_PHASE_PROBE:
        return "probe";
    case LLAM_AUTOTUNE_INTERNAL_PHASE_EVALUATE:
        return "evaluate";
    case LLAM_AUTOTUNE_INTERNAL_PHASE_BACKOFF:
        return "backoff";
    case LLAM_AUTOTUNE_INTERNAL_PHASE_SUSPENDED:
        return "suspended";
    case LLAM_AUTOTUNE_INTERNAL_PHASE_OFF:
    default:
        return "off";
    }
}

static void llam_autotune_json_snapshot_read(llam_runtime_t *rt, llam_autotune_json_snapshot_t *snapshot) {
    llam_runtime_t *pinned_runtime = NULL;
    int saved_errno = errno;

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->mode = LLAM_AUTOTUNE_INTERNAL_OFF;
    snapshot->phase = LLAM_AUTOTUNE_INTERNAL_PHASE_OFF;
    if (rt == NULL) {
        rt = llam_runtime_default_storage();
    }
    if (llam_runtime_begin_public_op(rt, &pinned_runtime) != 0) {
        errno = saved_errno;
        return;
    }
    if (llam_runtime_lifecycle_trylock() == 0) {
        if (atomic_load_explicit(&pinned_runtime->initialized, memory_order_acquire)) {
            llam_autotune_control_t *tune = &pinned_runtime->autotune;

            snapshot->mode = atomic_load_explicit(&tune->mode, memory_order_acquire);
            snapshot->phase = atomic_load_explicit(&tune->phase, memory_order_acquire);
            snapshot->recognized_domains = atomic_load_explicit(
                &tune->recognized_domains, memory_order_acquire);
            snapshot->observable_domains = atomic_load_explicit(
                &tune->observable_domains, memory_order_acquire);
            snapshot->controllable_domains = atomic_load_explicit(
                &tune->controllable_domains, memory_order_acquire);
            snapshot->active_observation_domains = atomic_load_explicit(
                &tune->active_observation_domains, memory_order_acquire);
            snapshot->active_control_domains = atomic_load_explicit(
                &tune->active_control_domains, memory_order_acquire);
            snapshot->supported_domains = atomic_load_explicit(&tune->supported_domains, memory_order_acquire);
            snapshot->active_domains = atomic_load_explicit(&tune->active_domains, memory_order_acquire);
            snapshot->suspended_domains = atomic_load_explicit(&tune->suspended_domains, memory_order_acquire);
            snapshot->policy_epoch = atomic_load_explicit(&tune->policy_epoch, memory_order_acquire);
            snapshot->decisions = atomic_load_explicit(&tune->decisions, memory_order_acquire);
            snapshot->commits = atomic_load_explicit(&tune->commits, memory_order_acquire);
            snapshot->rollbacks = atomic_load_explicit(&tune->rollbacks, memory_order_acquire);
            snapshot->guardrail_trips = atomic_load_explicit(&tune->guardrail_trips, memory_order_acquire);
            snapshot->decision_interval_ns = atomic_load_explicit(&tune->decision_interval_ns, memory_order_acquire);
            snapshot->min_hold_ns = atomic_load_explicit(&tune->min_hold_ns, memory_order_acquire);
            snapshot->target_wake_p99_ns = atomic_load_explicit(&tune->target_wake_p99_ns, memory_order_acquire);
            snapshot->sample_period = pinned_runtime->direct_handoff_stats_sample_mask + 1U;
            if (pinned_runtime->autotune_wake_latency_enabled != 0U &&
                pinned_runtime->autotune_wake_latency_sample_mask > pinned_runtime->direct_handoff_stats_sample_mask) {
                snapshot->sample_period = pinned_runtime->autotune_wake_latency_sample_mask + 1U;
            }
            snapshot->last_decision_ns = atomic_load_explicit(&tune->last_decision_ns, memory_order_acquire);
            snapshot->last_change_ns = atomic_load_explicit(&tune->last_change_ns, memory_order_acquire);
            snapshot->last_reason_mask = atomic_load_explicit(&tune->last_reason_mask, memory_order_acquire);
            snapshot->target_online_workers = atomic_load_explicit(&tune->target_online_workers, memory_order_acquire);
            snapshot->current_online_workers = llam_runtime_online_shards(pinned_runtime);
            snapshot->idle_spin_ns = pinned_runtime->idle_spin_ns;
            snapshot->yield_handoff_budget = llam_runtime_direct_handoff_budget(pinned_runtime);
            snapshot->wake_handoff_budget =
                pinned_runtime->wake_handoff_enabled != 0U ? llam_runtime_direct_handoff_budget(pinned_runtime) : 0U;
            snapshot->preempt_poll_period = pinned_runtime->preempt_poll_period;
            snapshot->preempt_quantum_ns = pinned_runtime->preempt_quantum_ns;
            snapshot->sampled_yield_handoff_attempts =
                atomic_load_explicit(&tune->sampled_yield_handoff_attempts, memory_order_acquire);
            snapshot->sampled_yield_handoff_hits =
                atomic_load_explicit(&tune->sampled_yield_handoff_hits, memory_order_acquire);
            snapshot->sampled_yield_handoff_fail_policy =
                atomic_load_explicit(&tune->sampled_yield_handoff_fail_policy, memory_order_acquire);
            snapshot->sampled_yield_handoff_fail_no_work =
                atomic_load_explicit(&tune->sampled_yield_handoff_fail_no_work, memory_order_acquire);
            snapshot->sampled_yield_handoff_fail_push =
                atomic_load_explicit(&tune->sampled_yield_handoff_fail_push, memory_order_acquire);
            snapshot->sampled_wake_handoff_attempts =
                atomic_load_explicit(&tune->sampled_wake_handoff_attempts, memory_order_acquire);
            snapshot->sampled_wake_handoff_hits =
                atomic_load_explicit(&tune->sampled_wake_handoff_hits, memory_order_acquire);
            snapshot->sampled_wake_handoff_fail_policy =
                atomic_load_explicit(&tune->sampled_wake_handoff_fail_policy, memory_order_acquire);
            snapshot->sampled_wake_handoff_fail_race =
                atomic_load_explicit(&tune->sampled_wake_handoff_fail_race, memory_order_acquire);
            snapshot->sampled_wake_latency_samples =
                atomic_load_explicit(&tune->sampled_wake_latency_samples, memory_order_acquire);
            snapshot->sampled_wake_latency_p50_ns =
                atomic_load_explicit(&tune->sampled_wake_latency_p50_ns, memory_order_acquire);
            snapshot->sampled_wake_latency_p99_ns =
                atomic_load_explicit(&tune->sampled_wake_latency_p99_ns, memory_order_acquire);
            snapshot->sampled_idle_spin_hits =
                atomic_load_explicit(&tune->sampled_idle_spin_hits, memory_order_acquire);
            snapshot->sampled_idle_spin_fallbacks =
                atomic_load_explicit(&tune->sampled_idle_spin_fallbacks, memory_order_acquire);
            snapshot->sampled_idle_spin_ns =
                atomic_load_explicit(&tune->sampled_idle_spin_ns, memory_order_acquire);
            snapshot->sampled_queue_overflows =
                atomic_load_explicit(&tune->sampled_queue_overflows, memory_order_acquire);
        }
        llam_runtime_lifecycle_unlock();
    }
    llam_runtime_end_public_op(pinned_runtime);
    errno = saved_errno;
}

static int llam_stats_json_autotune(int fd, llam_runtime_t *rt, unsigned *field_count) {
    llam_autotune_json_snapshot_t snapshot;
    int rc;

    llam_autotune_json_snapshot_read(rt, &snapshot);
    rc = dprintf(fd,
                 "%s\"autotune\":{"
                 "\"mode\":\"%s\","
                 "\"phase\":\"%s\","
                 "\"recognized_domains\":%llu,"
                 "\"observable_domains\":%llu,"
                 "\"controllable_domains\":%llu,"
                 "\"active_observation_domains\":%llu,"
                 "\"active_control_domains\":%llu,"
                 "\"supported_domains\":%llu,"
                 "\"active_domains\":%llu,"
                 "\"suspended_domains\":%llu,"
                 "\"policy_epoch\":%llu,"
                 "\"decisions\":%llu,"
                 "\"commits\":%llu,"
                 "\"rollbacks\":%llu,"
                 "\"guardrail_trips\":%llu,"
                 "\"decision_interval_ns\":%llu,"
                 "\"min_hold_ns\":%llu,"
                 "\"target_wake_p99_ns\":%llu,"
                 "\"sample_period\":%u,"
                 "\"last_decision_ns\":%llu,"
                 "\"last_change_ns\":%llu,"
                 "\"last_reason_mask\":%llu,"
                 "\"target_online_workers\":%u,"
                 "\"current_online_workers\":%u,"
                 "\"idle_spin_ns\":%llu,"
                 "\"yield_handoff_budget\":%u,"
                 "\"wake_handoff_budget\":%u,"
                 "\"preempt_poll_period\":%u,"
                 "\"preempt_quantum_ns\":%llu,"
                 "\"sampled_yield_handoff_attempts\":%llu,"
                 "\"sampled_yield_handoff_hits\":%llu,"
                 "\"sampled_yield_handoff_fail_policy\":%llu,"
                 "\"sampled_yield_handoff_fail_no_work\":%llu,"
                 "\"sampled_yield_handoff_fail_push\":%llu,"
                 "\"sampled_wake_handoff_attempts\":%llu,"
                 "\"sampled_wake_handoff_hits\":%llu,"
                 "\"sampled_wake_handoff_fail_policy\":%llu,"
                 "\"sampled_wake_handoff_fail_race\":%llu,"
                 "\"sampled_wake_latency_samples\":%llu,"
                 "\"sampled_wake_latency_p50_ns\":%llu,"
                 "\"sampled_wake_latency_p99_ns\":%llu,"
                 "\"sampled_idle_spin_hits\":%llu,"
                 "\"sampled_idle_spin_fallbacks\":%llu,"
                 "\"sampled_idle_spin_ns\":%llu,"
                 "\"sampled_queue_overflows\":%llu"
                 "}",
                 *field_count == 0U ? "" : ",",
                 llam_autotune_mode_name(snapshot.mode),
                 llam_autotune_phase_name(snapshot.phase),
                 (unsigned long long)snapshot.recognized_domains,
                 (unsigned long long)snapshot.observable_domains,
                 (unsigned long long)snapshot.controllable_domains,
                 (unsigned long long)snapshot.active_observation_domains,
                 (unsigned long long)snapshot.active_control_domains,
                 (unsigned long long)snapshot.supported_domains,
                 (unsigned long long)snapshot.active_domains,
                 (unsigned long long)snapshot.suspended_domains,
                 (unsigned long long)snapshot.policy_epoch,
                 (unsigned long long)snapshot.decisions,
                 (unsigned long long)snapshot.commits,
                 (unsigned long long)snapshot.rollbacks,
                 (unsigned long long)snapshot.guardrail_trips,
                 (unsigned long long)snapshot.decision_interval_ns,
                 (unsigned long long)snapshot.min_hold_ns,
                 (unsigned long long)snapshot.target_wake_p99_ns,
                 snapshot.sample_period,
                 (unsigned long long)snapshot.last_decision_ns,
                 (unsigned long long)snapshot.last_change_ns,
                 (unsigned long long)snapshot.last_reason_mask,
                 snapshot.target_online_workers,
                 snapshot.current_online_workers,
                 (unsigned long long)snapshot.idle_spin_ns,
                 snapshot.yield_handoff_budget,
                 snapshot.wake_handoff_budget,
                 snapshot.preempt_poll_period,
                 (unsigned long long)snapshot.preempt_quantum_ns,
                 (unsigned long long)snapshot.sampled_yield_handoff_attempts,
                 (unsigned long long)snapshot.sampled_yield_handoff_hits,
                 (unsigned long long)snapshot.sampled_yield_handoff_fail_policy,
                 (unsigned long long)snapshot.sampled_yield_handoff_fail_no_work,
                 (unsigned long long)snapshot.sampled_yield_handoff_fail_push,
                 (unsigned long long)snapshot.sampled_wake_handoff_attempts,
                 (unsigned long long)snapshot.sampled_wake_handoff_hits,
                 (unsigned long long)snapshot.sampled_wake_handoff_fail_policy,
                 (unsigned long long)snapshot.sampled_wake_handoff_fail_race,
                 (unsigned long long)snapshot.sampled_wake_latency_samples,
                 (unsigned long long)snapshot.sampled_wake_latency_p50_ns,
                 (unsigned long long)snapshot.sampled_wake_latency_p99_ns,
                 (unsigned long long)snapshot.sampled_idle_spin_hits,
                 (unsigned long long)snapshot.sampled_idle_spin_fallbacks,
                 (unsigned long long)snapshot.sampled_idle_spin_ns,
                 (unsigned long long)snapshot.sampled_queue_overflows);
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
        llam_stats_json_u64(fd, "configured_worker_min", stats.configured_worker_min, &fields) != 0 ||
        llam_stats_json_u64(fd, "configured_worker_count", stats.configured_worker_count, &fields) != 0 ||
        llam_stats_json_u64(fd, "configured_worker_max", stats.configured_worker_max, &fields) != 0 ||
        llam_stats_json_u64(fd, "configured_blocking_min", stats.configured_blocking_min, &fields) != 0 ||
        llam_stats_json_u64(fd, "configured_blocking_max", stats.configured_blocking_max, &fields) != 0 ||
        llam_stats_json_u64(fd, "selected_cpu_count", stats.selected_cpu_count, &fields) != 0 ||
        llam_stats_json_u64(fd, "affinity_policy", stats.affinity_policy, &fields) != 0 ||
        llam_stats_json_u64(fd, "scheduler_threads", stats.scheduler_threads, &fields) != 0 ||
        llam_stats_json_u64(fd, "blocking_threads", stats.blocking_threads, &fields) != 0 ||
        llam_stats_json_u64(fd, "io_threads", stats.io_threads, &fields) != 0 ||
        llam_stats_json_u64(fd, "controller_threads", stats.controller_threads, &fields) != 0 ||
        llam_stats_json_u64(fd, "opaque_helper_threads", stats.opaque_helper_threads, &fields) != 0 ||
        llam_stats_json_u64(fd, "runtime_owned_threads", stats.runtime_owned_threads, &fields) != 0 ||
        llam_stats_json_u64(fd, "native_execution_threads", stats.native_execution_threads, &fields) != 0 ||
        llam_stats_json_u64(fd, "affinity_failures", stats.affinity_failures, &fields) != 0 ||
        llam_stats_json_u64(fd, "requested_task_prewarm_total", stats.requested_task_prewarm_total, &fields) != 0 ||
        llam_stats_json_u64(fd, "achieved_task_prewarm_total", stats.achieved_task_prewarm_total, &fields) != 0 ||
        llam_stats_json_u64(fd, "requested_stack_prewarm_total", stats.requested_stack_prewarm_total, &fields) != 0 ||
        llam_stats_json_u64(fd, "achieved_stack_prewarm_total", stats.achieved_stack_prewarm_total, &fields) != 0 ||
        llam_stats_json_u64(fd, "requested_timer_prewarm_total", stats.requested_timer_prewarm_total, &fields) != 0 ||
        llam_stats_json_u64(fd, "achieved_timer_prewarm_total", stats.achieved_timer_prewarm_total, &fields) != 0 ||
        llam_stats_json_u64(fd, "estimated_metadata_bytes", stats.estimated_metadata_bytes, &fields) != 0 ||
        llam_stats_json_u64(fd, "estimated_stack_mapping_bytes", stats.estimated_stack_mapping_bytes, &fields) != 0 ||
        llam_stats_json_u64(fd, "task_prewarm_source", stats.task_prewarm_source, &fields) != 0 ||
        llam_stats_json_u64(fd, "stack_prewarm_source", stats.stack_prewarm_source, &fields) != 0 ||
        llam_stats_json_u64(fd, "timer_prewarm_source", stats.timer_prewarm_source, &fields) != 0 ||
        llam_stats_json_u64(fd, "stack_cache_budget_bytes", stats.stack_cache_budget_bytes, &fields) != 0 ||
        llam_stats_json_u64(fd, "stack_cache_high_watermark_bytes", stats.stack_cache_high_watermark_bytes, &fields) != 0 ||
        llam_stats_json_u64(fd, "stack_cache_low_watermark_bytes", stats.stack_cache_low_watermark_bytes, &fields) != 0 ||
        llam_stats_json_u64(fd, "stack_cache_idle_ns", stats.stack_cache_idle_ns, &fields) != 0 ||
        llam_stats_json_u64(fd, "stack_cache_flags", stats.stack_cache_flags, &fields) != 0 ||
        llam_stats_json_u64(fd, "stack_cache_cached_bytes", stats.stack_cache_cached_bytes, &fields) != 0 ||
        llam_stats_json_u64(fd, "stack_cache_cached_mappings", stats.stack_cache_cached_mappings, &fields) != 0 ||
        llam_stats_json_u64(fd, "stack_cache_committed_bytes", stats.stack_cache_committed_bytes, &fields) != 0 ||
        llam_stats_json_u64(fd, "stack_cache_trim_requests", stats.stack_cache_trim_requests, &fields) != 0 ||
        llam_stats_json_u64(fd, "stack_cache_discarded_bytes", stats.stack_cache_discarded_bytes, &fields) != 0 ||
        llam_stats_json_u64(fd, "stack_cache_released_bytes", stats.stack_cache_released_bytes, &fields) != 0 ||
        llam_stats_json_u64(fd, "stack_cache_budget_rejections", stats.stack_cache_budget_rejections, &fields) != 0 ||
        llam_stats_json_u64(fd, "stack_cache_secure_return_failures", stats.stack_cache_secure_return_failures, &fields) != 0 ||
        llam_stats_json_u64(fd, "stack_cache_resident_valid", stats.stack_cache_resident_valid, &fields) != 0 ||
        llam_stats_json_u64(fd, "stack_cache_resident_bytes", stats.stack_cache_resident_bytes, &fields) != 0 ||
        llam_stats_json_u64(fd, "stack_cache_resident_sample_ns", stats.stack_cache_resident_sample_ns, &fields) != 0 ||
        llam_stats_json_u64(fd, "stack_cache_process_quarantine_bytes", stats.stack_cache_process_quarantine_bytes, &fields) != 0 ||
        llam_stats_json_u64(fd, "stack_cache_process_quarantine_mappings", stats.stack_cache_process_quarantine_mappings, &fields) != 0 ||
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
        llam_stats_json_u64(fd, "wake_handoff_attempts", stats.wake_handoff_attempts, &fields) != 0 ||
        llam_stats_json_u64(fd, "wake_handoff_hits", stats.wake_handoff_hits, &fields) != 0 ||
        llam_stats_json_u64(fd, "wake_handoff_fail_context", stats.wake_handoff_fail_context, &fields) != 0 ||
        llam_stats_json_u64(fd, "wake_handoff_fail_policy", stats.wake_handoff_fail_policy, &fields) != 0 ||
        llam_stats_json_u64(fd, "wake_handoff_fail_race", stats.wake_handoff_fail_race, &fields) != 0 ||
        llam_stats_json_u64(fd, "preempt_requests", stats.preempt_requests, &fields) != 0 ||
        llam_stats_json_u64(fd, "preempt_yields", stats.preempt_yields, &fields) != 0 ||
        llam_stats_json_u64(fd, "preempt_suppressed", stats.preempt_suppressed, &fields) != 0 ||
        llam_stats_json_u64(fd, "preempt_signals", stats.preempt_signals, &fields) != 0 ||
        llam_stats_json_u64(fd, "preempt_mode", stats.preempt_mode, &fields) != 0 ||
        llam_stats_json_u64(fd, "preempt_poll_period", stats.preempt_poll_period, &fields) != 0 ||
        llam_stats_json_u64(fd, "preempt_quantum_ns", stats.preempt_quantum_ns, &fields) != 0 ||
        llam_stats_json_autotune(fd, rt, &fields) != 0 ||
        dprintf(fd, "}\n") < 0) {
        return -1;
    }
    return 0;
}

int llam_runtime_write_stats_json(int fd) {
    return llam_runtime_write_stats_json_rt(llam_runtime_default_storage(), fd);
}
