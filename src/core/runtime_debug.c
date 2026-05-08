/**
 * @file src/core/runtime_debug.c
 * @brief Public diagnostics: runtime stats collection, task inspection, and state dumps.
 *
 * @details
 * Diagnostic APIs intentionally expose snapshots rather than live references.
 * The stats path aggregates low-cost counters for benchmarks, while the state
 * dump path emits a verbose human-readable view of runtime, node, shard,
 * allocator, trace, and task state.
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

/**
 * @brief Return a task's runtime-assigned id.
 *
 * @param task Task handle, or NULL.
 * @return Task id, or 0 for NULL.
 */
uint64_t llam_task_id(const llam_task_t *task) {
    return task != NULL ? task->id : 0U;
}

/**
 * @brief Return a stable diagnostic name for a task's current state.
 *
 * @param task Task handle, or NULL.
 * @return State name, or "UNKNOWN" for NULL.
 */
const char *llam_task_state_name(const llam_task_t *task) {
    return task != NULL ? llam_state_name_from_id(task->state) : "UNKNOWN";
}

/**
 * @brief Return a task's scheduler class.
 *
 * @param task Task handle, or NULL.
 * @return Task class, or default class for NULL.
 */
uint32_t llam_task_class(const llam_task_t *task) {
    return task != NULL
               ? atomic_load_explicit(&((llam_task_t *)task)->task_class, memory_order_acquire)
               : (uint32_t)LLAM_TASK_CLASS_DEFAULT;
}

/**
 * @brief Return the current managed task for this thread.
 *
 * @return Current task, or NULL outside a runtime-managed task.
 */
llam_task_t *llam_current_task(void) {
    return g_llam_tls_task;
}

/**
 * @brief Fill the current library's full runtime statistics layout.
 */
static void llam_runtime_collect_stats_full(llam_runtime_stats_t *stats) {
    llam_runtime_t *rt = &g_llam_runtime;
    unsigned i;

    memset(stats, 0, sizeof(*stats));
    stats->active_workers = rt->active_shards;
    stats->online_workers = llam_runtime_online_shards(rt);
    stats->online_workers_floor = llam_runtime_online_shards_floor(rt);
    stats->online_workers_min = llam_runtime_online_shards_min(rt);
    stats->online_workers_max = llam_runtime_online_shards_max(rt);
    stats->active_nodes = rt->active_nodes;
    stats->dynamic_workers = rt->experimental_dynamic_shards != 0U ? 1U : 0U;
    stats->worker_rings = rt->experimental_shard_rings != 0U ? 1U : 0U;
    stats->worker_rings_multishot = rt->experimental_shard_rings_multishot != 0U ? 1U : 0U;
    stats->lockfree_normq = rt->experimental_lockfree_normq != 0U ? 1U : 0U;
    stats->huge_alloc = rt->experimental_huge_alloc_active != 0U ? 1U : 0U;
    stats->sqpoll = rt->experimental_sqpoll_active != 0U ? 1U : 0U;
    stats->overflow_depth = llam_runtime_overflow_depth(rt);

    // Shard metrics are protected by the shard lock so the snapshot does not
    // read partially updated multi-field groups.
    for (i = 0; i < rt->active_shards; ++i) {
        llam_shard_t *shard = &rt->shards[i];

        pthread_mutex_lock(&shard->lock);
        stats->ctx_switches += shard->metrics.ctx_switches;
        stats->yields += shard->metrics.yields;
        stats->parks += shard->metrics.parks;
        stats->wakes += shard->metrics.wakes;
        stats->steals += shard->metrics.steals;
        stats->migrations += shard->metrics.migrations;
        stats->blocking_calls += shard->metrics.blocking_calls;
        stats->blocking_completions += shard->metrics.blocking_completions;
        stats->io_submits += shard->metrics.io_submits;
        stats->io_completions += shard->metrics.io_completions;
        stats->idle_polls += shard->metrics.idle_polls;
        stats->idle_spin_loops += shard->metrics.idle_spin_loops;
        stats->idle_spin_hits += shard->metrics.idle_spin_hits;
        stats->idle_spin_fallbacks += shard->metrics.idle_spin_fallbacks;
        stats->idle_spin_ns += shard->metrics.idle_spin_ns;
        stats->yield_direct_attempts += shard->metrics.yield_direct_attempts;
        stats->yield_direct_fast_hits += shard->metrics.yield_direct_fast_hits;
        stats->yield_direct_locked_hits += shard->metrics.yield_direct_locked_hits;
        stats->yield_direct_fail_context += shard->metrics.yield_direct_fail_context;
        stats->yield_direct_fail_policy += shard->metrics.yield_direct_fail_policy;
        stats->yield_direct_fail_no_work += shard->metrics.yield_direct_fail_no_work;
        stats->yield_direct_fail_self += shard->metrics.yield_direct_fail_self;
        stats->yield_direct_fail_push += shard->metrics.yield_direct_fail_push;
        stats->queue_overflows += shard->metrics.queue_overflows;
        stats->opaque_block_ns += shard->metrics.opaque_block_ns;
        stats->opaque_block_samples += shard->metrics.opaque_block_samples;
        if (shard->metrics.opaque_block_max_ns > stats->opaque_block_max_ns) {
            stats->opaque_block_max_ns = shard->metrics.opaque_block_max_ns;
        }
        stats->opaque_enter_wait_ns += shard->metrics.opaque_enter_wait_ns;
        stats->opaque_enter_wait_samples += shard->metrics.opaque_enter_wait_samples;
        if (shard->metrics.opaque_enter_wait_max_ns > stats->opaque_enter_wait_max_ns) {
            stats->opaque_enter_wait_max_ns = shard->metrics.opaque_enter_wait_max_ns;
        }
        stats->opaque_leave_wait_ns += shard->metrics.opaque_leave_wait_ns;
        stats->opaque_leave_wait_samples += shard->metrics.opaque_leave_wait_samples;
        if (shard->metrics.opaque_leave_wait_max_ns > stats->opaque_leave_wait_max_ns) {
            stats->opaque_leave_wait_max_ns = shard->metrics.opaque_leave_wait_max_ns;
        }
        pthread_mutex_unlock(&shard->lock);
    }
    // Node submit counters are monotonic diagnostics; exact atomicity is less
    // important than keeping the collection path lightweight.
    for (i = 0; i < rt->active_nodes; ++i) {
        stats->io_submit_calls += rt->nodes[i].submit_calls;
        stats->io_submit_syscalls += rt->nodes[i].submit_syscalls;
    }
}

/**
 * @brief Collect a flat runtime statistics snapshot with a caller size.
 *
 * Benchmarks consume this format so they do not need to scrape the human dump.
 * Dynamic loaders and FFI bindings should call this form so future stats fields
 * appended at the tail cannot overflow an older caller's smaller struct.
 *
 * @param stats Destination snapshot.
 * @param stats_size Size of the caller's stats struct.
 * @return 0 on success, -1 with @c errno set on invalid arguments.
 */
int llam_runtime_collect_stats_ex(llam_runtime_stats_t *stats, size_t stats_size) {
    llam_runtime_stats_t full_stats;
    size_t copy_size;

    if (stats == NULL || stats_size == 0U) {
        errno = EINVAL;
        return -1;
    }

    llam_runtime_collect_stats_full(&full_stats);
    memset(stats, 0, stats_size);
    copy_size = stats_size < sizeof(full_stats) ? stats_size : sizeof(full_stats);
    memcpy(stats, &full_stats, copy_size);
    return 0;
}

int llam_runtime_collect_stats(llam_runtime_stats_t *stats) {
    return llam_runtime_collect_stats_ex(stats, stats != NULL ? sizeof(*stats) : 0U);
}

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

int llam_runtime_write_stats_json(int fd) {
    llam_runtime_stats_t stats;
    unsigned fields = 0U;

    if (fd < 0) {
        errno = EINVAL;
        return -1;
    }
    if (llam_runtime_collect_stats(&stats) != 0) {
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
        dprintf(fd, "}\n") < 0) {
        return -1;
    }
    return 0;
}

/**
 * @brief Write a human-readable runtime dump to an fd.
 *
 * @param fd Destination file descriptor.
 */
void llam_dump_runtime_state(int fd) {
    llam_runtime_t *rt = &g_llam_runtime;
    unsigned i;
    unsigned block_pending = atomic_load(&rt->block_pending);
    unsigned block_active = atomic_load(&rt->block_active);
    unsigned overflow_depth = llam_runtime_overflow_depth(rt);
    unsigned online_shards = llam_max_unsigned(1U, llam_runtime_online_shards(rt));
    unsigned online_floor = llam_runtime_online_shards_floor(rt);
    unsigned online_min = llam_runtime_online_shards_min(rt);
    unsigned online_max = llam_runtime_online_shards_max(rt);
    unsigned overflow_threshold = llam_max_unsigned(4U, online_shards * 2U);
    unsigned pressure = llam_runtime_pressure_signal(rt) ? 1U : 0U;
    const char *profile = rt->profile == LLAM_RUNTIME_PROFILE_RELEASE_FAST
                              ? "release-fast"
                              : (rt->profile == LLAM_RUNTIME_PROFILE_DEBUG_SAFE
                                     ? "debug-safe"
                                     : (rt->profile == LLAM_RUNTIME_PROFILE_IO_LATENCY ? "io-latency" : "balanced"));

    dprintf(fd,
            "runtime:\n"
            "  observed_shards=%u active_shards=%u online_shards=%u online_floor=%u online_min=%u online_max=%u active_nodes=%u profile=%s deterministic=%u forced_yield_every=%u dynamic_shards=%u dynamic_cooldown=%u shard_rings=%u shard_rings_multishot=%u lockfree_normq=%u huge_alloc=%u sqpoll=%u sqpoll_cpu=%d sqpoll_reserved=%u idle_spin_ns=%llu idle_spin_max_iters=%u pressure=%u overflow_depth=%u overflow_threshold=%u live_tasks=%u fatal_errno=%d xsave=%u xsave_mask=0x%llx xsave_area=%zu\n",
            rt->observed_shards,
            rt->active_shards,
            online_shards,
            online_floor,
            online_min,
            online_max,
            rt->active_nodes,
            profile,
            rt->deterministic,
            rt->forced_yield_every,
            rt->experimental_dynamic_shards != 0U ? 1U : 0U,
            rt->dynamic_scale_cooldown,
            rt->experimental_shard_rings != 0U ? 1U : 0U,
            rt->experimental_shard_rings_multishot != 0U ? 1U : 0U,
            rt->experimental_lockfree_normq != 0U ? 1U : 0U,
            rt->experimental_huge_alloc_active != 0U ? 1U : 0U,
            rt->experimental_sqpoll_active != 0U ? 1U : 0U,
            rt->sqpoll_cpu,
            rt->sqpoll_cpu_reserved,
            (unsigned long long)rt->idle_spin_ns,
            rt->idle_spin_max_iters,
            pressure,
            overflow_depth,
            overflow_threshold,
            llam_runtime_live_tasks(rt),
            atomic_load(&rt->fatal_errno),
            rt->xsave_enabled ? 1U : 0U,
            (unsigned long long)rt->xsave_mask,
            rt->xsave_area_size);

    // The dump format is intentionally text-first for bug reports and benchmark
    // logs; machine consumers should use llam_runtime_collect_stats().
    dprintf(fd,
            "block:\n"
            "  workers=%u pending=%u active=%u queued=%u peak_active=%u\n",
            rt->block_worker_count,
            block_pending,
            block_active,
            block_pending > block_active ? block_pending - block_active : 0U,
            atomic_load(&rt->block_active_peak));

    dprintf(fd, "nodes:\n");
    for (i = 0; i < rt->active_nodes; ++i) {
        dprintf(fd,
                "  node=%u kernel_node=%u ring_ready=%u sqpoll=%u sqpoll_cpu=%d supports(read=%u recv=%u write=%u accept=%u connect=%u poll=%u ms_recv=%u ms_accept=%u ms_poll=%u pbuf=%u) pending_ops=%u submit_batches=%llu submit_entries=%llu submit_calls=%llu submit_syscalls=%llu max_submit=%u cq_depth=%u cq_depth_max=%u unsupported_ops=%llu pbuf(acquire=%llu return=%llu)\n",
                rt->nodes[i].index,
                rt->nodes[i].kernel_node_id,
                rt->nodes[i].ring_ready ? 1U : 0U,
                rt->nodes[i].sqpoll_enabled ? 1U : 0U,
                rt->nodes[i].sqpoll_enabled ? (int)rt->nodes[i].sqpoll_cpu : -1,
                rt->nodes[i].supports_read ? 1U : 0U,
                rt->nodes[i].supports_recv ? 1U : 0U,
                rt->nodes[i].supports_write ? 1U : 0U,
                rt->nodes[i].supports_accept ? 1U : 0U,
                rt->nodes[i].supports_connect ? 1U : 0U,
                rt->nodes[i].supports_poll ? 1U : 0U,
                rt->nodes[i].supports_multishot_recv ? 1U : 0U,
                rt->nodes[i].supports_multishot_accept ? 1U : 0U,
                rt->nodes[i].supports_multishot_poll ? 1U : 0U,
                rt->nodes[i].supports_provided_buffers ? 1U : 0U,
                atomic_load(&rt->nodes[i].pending_ops),
                (unsigned long long)rt->nodes[i].submit_batches,
                (unsigned long long)rt->nodes[i].submit_entries,
                (unsigned long long)rt->nodes[i].submit_calls,
                (unsigned long long)rt->nodes[i].submit_syscalls,
                rt->nodes[i].max_submit_batch,
                rt->nodes[i].last_cq_depth,
                rt->nodes[i].max_cq_depth,
                (unsigned long long)rt->nodes[i].unsupported_ops,
                (unsigned long long)rt->nodes[i].provided_buf_acquires,
                (unsigned long long)rt->nodes[i].provided_buf_returns);
    }

    dprintf(fd, "shards:\n");
    for (i = 0; i < rt->active_shards; ++i) {
        llam_shard_t *shard = &rt->shards[i];
        size_t trace_count = shard->trace_head > LLAM_TRACE_RING_CAP ? LLAM_TRACE_RING_CAP : shard->trace_head;
        unsigned begin = shard->trace_head > LLAM_TRACE_RING_CAP ? shard->trace_head - LLAM_TRACE_RING_CAP : 0U;
        unsigned j;
        llam_task_t *current;

        pthread_mutex_lock(&shard->lock);
        current = atomic_load_explicit(&shard->current, memory_order_acquire);
        dprintf(fd,
                "  shard=%u online=%u cpu=%u node=%u io_node=%u current=%llu opaque_redirect=%u opaque_helper(started=%u ready=%u active=%u) opaque_depth=%u/%u redirect_depth=%u/%u queues(inject=%u hot=%u norm=%u timers=%u)\n",
                shard->id,
                llam_shard_is_online(shard) ? 1U : 0U,
                shard->cpu_id,
                shard->node_index,
                shard->io_node_index,
                current != NULL ? (unsigned long long)current->id : 0ULL,
                shard->opaque_redirect_active ? 1U : 0U,
                shard->opaque_helper_thread_started ? 1U : 0U,
                shard->opaque_helper_ready ? 1U : 0U,
                shard->opaque_helper_active ? 1U : 0U,
                shard->opaque_compensation_depth,
                shard->opaque_compensation_depth_peak,
                shard->opaque_redirect_depth,
                shard->opaque_redirect_depth_peak,
                shard->inject_q.depth,
                shard->hot_q.depth,
                llam_norm_queue_depth(shard),
                shard->timers != NULL ? 1U : 0U);
        dprintf(fd,
                "    metrics ctx_switches=%llu yields=%llu parks=%llu wakes=%llu sleeps=%llu joins=%llu steals=%llu migrations=%llu slice_budget_ns=%llu max_run_ns=%llu slice_overruns=%llu\n",
                (unsigned long long)shard->metrics.ctx_switches,
                (unsigned long long)shard->metrics.yields,
                (unsigned long long)shard->metrics.parks,
                (unsigned long long)shard->metrics.wakes,
                (unsigned long long)shard->metrics.sleeps,
                (unsigned long long)shard->metrics.joins,
                (unsigned long long)shard->metrics.steals,
                (unsigned long long)shard->metrics.migrations,
                (unsigned long long)shard->metrics.slice_budget_ns,
                (unsigned long long)shard->metrics.max_run_ns,
                (unsigned long long)shard->metrics.slice_overruns);
        dprintf(fd,
                "    metrics timeout_wakes=%llu cancel_wakes=%llu wake_latency_ns=%llu wake_samples=%llu\n",
                (unsigned long long)shard->metrics.timeout_wakes,
                (unsigned long long)shard->metrics.cancel_wakes,
                (unsigned long long)shard->metrics.wake_latency_ns,
                (unsigned long long)shard->metrics.wake_samples);
        dprintf(fd,
                "    metrics block_calls=%llu block_done=%llu io_submits=%llu io_done=%llu io_fallbacks=%llu io_latency_ns=%llu io_samples=%llu idle_polls=%llu idle_spin(loops=%llu hits=%llu fallbacks=%llu ns=%llu) direct_yield(attempts=%llu fast=%llu locked=%llu context=%llu policy=%llu no_work=%llu self=%llu push=%llu) watchdog_hits=%llu long_no_safepoint=%llu opaque_comp=%llu opaque_redirects=%llu queue_overflows=%llu deadlock_suspicions=%llu run_ns=%llu\n",
                (unsigned long long)shard->metrics.blocking_calls,
                (unsigned long long)shard->metrics.blocking_completions,
                (unsigned long long)shard->metrics.io_submits,
                (unsigned long long)shard->metrics.io_completions,
                (unsigned long long)shard->metrics.io_fallbacks,
                (unsigned long long)shard->metrics.io_completion_latency_ns,
                (unsigned long long)shard->metrics.io_completion_samples,
                (unsigned long long)shard->metrics.idle_polls,
                (unsigned long long)shard->metrics.idle_spin_loops,
                (unsigned long long)shard->metrics.idle_spin_hits,
                (unsigned long long)shard->metrics.idle_spin_fallbacks,
                (unsigned long long)shard->metrics.idle_spin_ns,
                (unsigned long long)shard->metrics.yield_direct_attempts,
                (unsigned long long)shard->metrics.yield_direct_fast_hits,
                (unsigned long long)shard->metrics.yield_direct_locked_hits,
                (unsigned long long)shard->metrics.yield_direct_fail_context,
                (unsigned long long)shard->metrics.yield_direct_fail_policy,
                (unsigned long long)shard->metrics.yield_direct_fail_no_work,
                (unsigned long long)shard->metrics.yield_direct_fail_self,
                (unsigned long long)shard->metrics.yield_direct_fail_push,
                (unsigned long long)shard->metrics.watchdog_hits,
                (unsigned long long)shard->metrics.long_no_safepoint,
                (unsigned long long)shard->metrics.opaque_compensations,
                (unsigned long long)shard->metrics.opaque_redirect_activations,
                (unsigned long long)shard->metrics.queue_overflows,
                (unsigned long long)shard->metrics.deadlock_suspicions,
                (unsigned long long)shard->metrics.total_run_ns);
        dprintf(fd,
                "    metrics opaque_block_ns=%llu opaque_block_samples=%llu opaque_block_max_ns=%llu opaque_enter_wait_ns=%llu opaque_enter_wait_samples=%llu opaque_enter_wait_max_ns=%llu opaque_leave_wait_ns=%llu opaque_leave_wait_samples=%llu opaque_leave_wait_max_ns=%llu wake_hist(join=%llu sleep=%llu blocking=%llu io=%llu cancel=%llu mutex=%llu cond=%llu ch_send=%llu ch_recv=%llu timeout=%llu)\n",
                (unsigned long long)shard->metrics.opaque_block_ns,
                (unsigned long long)shard->metrics.opaque_block_samples,
                (unsigned long long)shard->metrics.opaque_block_max_ns,
                (unsigned long long)shard->metrics.opaque_enter_wait_ns,
                (unsigned long long)shard->metrics.opaque_enter_wait_samples,
                (unsigned long long)shard->metrics.opaque_enter_wait_max_ns,
                (unsigned long long)shard->metrics.opaque_leave_wait_ns,
                (unsigned long long)shard->metrics.opaque_leave_wait_samples,
                (unsigned long long)shard->metrics.opaque_leave_wait_max_ns,
                (unsigned long long)shard->metrics.wake_reason_hist[LLAM_WAIT_JOIN],
                (unsigned long long)shard->metrics.wake_reason_hist[LLAM_WAIT_SLEEP],
                (unsigned long long)shard->metrics.wake_reason_hist[LLAM_WAIT_BLOCKING],
                (unsigned long long)shard->metrics.wake_reason_hist[LLAM_WAIT_IO],
                (unsigned long long)shard->metrics.wake_reason_hist[LLAM_WAIT_CANCEL],
                (unsigned long long)shard->metrics.wake_reason_hist[LLAM_WAIT_MUTEX],
                (unsigned long long)shard->metrics.wake_reason_hist[LLAM_WAIT_COND],
                (unsigned long long)shard->metrics.wake_reason_hist[LLAM_WAIT_CHANNEL_SEND],
                (unsigned long long)shard->metrics.wake_reason_hist[LLAM_WAIT_CHANNEL_RECV],
                (unsigned long long)shard->metrics.wake_reason_hist[LLAM_WAIT_TIMEOUT]);
        dprintf(fd,
                "    allocator lock(acq=%llu contention=%llu grows=%llu grow_fail=%llu) burst_max(task=%llu wait=%llu timer=%llu io_req=%llu io_buf=%llu)\n",
                (unsigned long long)shard->allocator.lock_acquires,
                (unsigned long long)shard->allocator.lock_contentions,
                (unsigned long long)shard->allocator.slab_grows,
                (unsigned long long)shard->allocator.slab_grow_failures,
                (unsigned long long)shard->allocator.task_remote_burst_max,
                (unsigned long long)shard->allocator.wait_remote_burst_max,
                (unsigned long long)shard->allocator.timer_remote_burst_max,
                (unsigned long long)shard->allocator.io_req_remote_burst_max,
                (unsigned long long)shard->allocator.io_buffer_remote_burst_max);
        dprintf(fd,
                "    allocator task(alloc=%llu reuse=%llu free=%llu remote_free=%llu drain=%llu) wait(alloc=%llu reuse=%llu free=%llu remote_free=%llu drain=%llu) timer(alloc=%llu reuse=%llu free=%llu remote_free=%llu drain=%llu) io_req(alloc=%llu reuse=%llu free=%llu remote_free=%llu drain=%llu) io_buf(alloc=%llu reuse=%llu free=%llu remote_free=%llu drain=%llu)\n",
                (unsigned long long)shard->allocator.task_allocs,
                (unsigned long long)shard->allocator.task_reuses,
                (unsigned long long)shard->allocator.task_frees,
                (unsigned long long)shard->allocator.task_remote_frees,
                (unsigned long long)shard->allocator.task_remote_drains,
                (unsigned long long)shard->allocator.wait_allocs,
                (unsigned long long)shard->allocator.wait_reuses,
                (unsigned long long)shard->allocator.wait_frees,
                (unsigned long long)shard->allocator.wait_remote_frees,
                (unsigned long long)shard->allocator.wait_remote_drains,
                (unsigned long long)shard->allocator.timer_allocs,
                (unsigned long long)shard->allocator.timer_reuses,
                (unsigned long long)shard->allocator.timer_frees,
                (unsigned long long)shard->allocator.timer_remote_frees,
                (unsigned long long)shard->allocator.timer_remote_drains,
                (unsigned long long)shard->allocator.io_req_allocs,
                (unsigned long long)shard->allocator.io_req_reuses,
                (unsigned long long)shard->allocator.io_req_frees,
                (unsigned long long)shard->allocator.io_req_remote_frees,
                (unsigned long long)shard->allocator.io_req_remote_drains,
                (unsigned long long)shard->allocator.io_buffer_allocs,
                (unsigned long long)shard->allocator.io_buffer_reuses,
                (unsigned long long)shard->allocator.io_buffer_frees,
                (unsigned long long)shard->allocator.io_buffer_remote_frees,
                (unsigned long long)shard->allocator.io_buffer_remote_drains);
        dprintf(fd, "    trace:\n");
        for (j = begin; j < begin + (unsigned)trace_count; ++j) {
            const llam_trace_event_t *event = &shard->trace_ring[j % LLAM_TRACE_RING_CAP];
            dprintf(fd,
                    "      ts=%llu kind=%s task=%llu %s->%s reason=%s\n",
                    (unsigned long long)event->ts_ns,
                    llam_trace_kind_name((llam_trace_kind_t)event->kind),
                    (unsigned long long)event->task_id,
                    llam_state_name_from_id((llam_task_state_id_t)event->from_state),
                    llam_state_name_from_id((llam_task_state_id_t)event->to_state),
                    llam_wait_reason_name((llam_wait_reason_t)event->reason));
        }
        pthread_mutex_unlock(&shard->lock);
    }

    dprintf(fd, "tasks:\n");
    for (i = 0; i < rt->active_shards; ++i) {
        llam_shard_t *shard = &rt->shards[i];

        pthread_mutex_lock(&shard->lock);
        for (llam_task_t *task = shard->all_tasks; task != NULL; task = task->all_next) {
            dprintf(fd,
                    "  id=%llu state=%s class=%d flags=0x%x home=%u last=%u wait=%s stack=%zu stack_used=%zu stack_peak=%zu stack_hint=%s last_run_ns=%llu total_run_ns=%llu opaque_last_ns=%llu opaque_max_ns=%llu opaque_count=%llu\n",
                    (unsigned long long)task->id,
                    llam_state_name_from_id(task->state),
                    (int)atomic_load_explicit(&task->task_class, memory_order_acquire),
                    task->flags,
                    task->home_shard,
                    task->last_shard,
                    llam_wait_reason_name(task->wait_reason),
                    task->stack_size,
                    task->last_stack_used,
                    task->stack_high_water,
                    llam_stack_profile_hint(task),
                    (unsigned long long)task->last_run_ns,
                    (unsigned long long)task->total_run_ns,
                    (unsigned long long)task->last_opaque_block_ns,
                    (unsigned long long)task->max_opaque_block_ns,
                    (unsigned long long)task->opaque_block_count);
        }
        pthread_mutex_unlock(&shard->lock);
    }
}
