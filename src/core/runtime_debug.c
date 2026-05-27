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
#include "runtime_debug_dump_helpers.h"

/** @brief Return a task's runtime-assigned id, or 0 for invalid/foreign handles. */
uint64_t llam_task_id(const llam_task_t *task) {
    uint64_t id = 0U;
    int saved_errno = errno;

    task = llam_task_resolve_public_handle(task);
    if (task != NULL) {
        if (llam_runtime_check_object_owner(task->owner_runtime) == 0) {
            id = task->id;
        } else {
            errno = saved_errno;
        }
        llam_task_end_public_op((llam_task_t *)task);
    }
    return id;
}

/** @brief Return a stable task state name, or "UNKNOWN" for invalid/foreign handles. */
const char *llam_task_state_name(const llam_task_t *task) {
    const char *state = "UNKNOWN";
    int saved_errno = errno;

    task = llam_task_resolve_public_handle(task);
    if (task != NULL) {
        if (llam_runtime_check_object_owner(task->owner_runtime) == 0) {
            state = llam_state_name_from_id(task->state);
        } else {
            errno = saved_errno;
        }
        llam_task_end_public_op((llam_task_t *)task);
    }
    return state;
}

/** @brief Return a task's scheduler class, or the default for invalid/foreign handles. */
uint32_t llam_task_class(const llam_task_t *task) {
    uint32_t task_class = (uint32_t)LLAM_TASK_CLASS_DEFAULT;
    int saved_errno = errno;

    task = llam_task_resolve_public_handle(task);
    if (task != NULL) {
        if (llam_runtime_check_object_owner(task->owner_runtime) == 0) {
            task_class = atomic_load_explicit(&((llam_task_t *)task)->task_class, memory_order_acquire);
        } else {
            errno = saved_errno;
        }
        llam_task_end_public_op((llam_task_t *)task);
    }
    return task_class;
}

/** @brief Return the current managed task for this thread, or NULL outside LLAM. */
llam_task_t *llam_current_task(void) {
    return llam_task_public_handle(g_llam_tls_task);
}

/**
 * @brief Fill the current library's full runtime statistics layout.
 *
 * @param rt Runtime whose counters should be sampled. Legacy stats wrappers
 *           pass the default runtime; handle-scoped APIs pass their explicit
 *           owner.
 * @param stats Destination full-size statistics object.
 */
static void llam_runtime_collect_stats_full(llam_runtime_t *rt, llam_runtime_stats_t *stats) {
    llam_shard_t *shards = rt->shards;
    llam_node_t *nodes = rt->nodes;
    unsigned active_shards = rt->active_shards;
    unsigned active_nodes = rt->active_nodes;
    unsigned i;

    memset(stats, 0, sizeof(*stats));
    stats->active_workers = active_shards;
    stats->online_workers = llam_runtime_online_shards(rt);
    stats->online_workers_floor = llam_runtime_online_shards_floor(rt);
    stats->online_workers_min = llam_runtime_online_shards_min(rt);
    stats->online_workers_max = llam_runtime_online_shards_max(rt);
    stats->active_nodes = active_nodes;
    stats->dynamic_workers = rt->experimental_dynamic_shards != 0U ? 1U : 0U;
    stats->worker_rings = rt->experimental_shard_rings != 0U ? 1U : 0U;
    stats->worker_rings_multishot = rt->experimental_shard_rings_multishot != 0U ? 1U : 0U;
    stats->lockfree_normq = rt->experimental_lockfree_normq != 0U ? 1U : 0U;
    stats->huge_alloc = rt->experimental_huge_alloc_active != 0U ? 1U : 0U;
    stats->sqpoll = rt->experimental_sqpoll_active != 0U ? 1U : 0U;
    stats->preempt_mode = rt->preempt_mode;
    stats->preempt_poll_period = rt->preempt_poll_period;
    stats->preempt_quantum_ns = rt->preempt_quantum_ns;
    stats->overflow_depth = llam_runtime_overflow_depth(rt);

    /*
     * Diagnostics are callable from unmanaged host threads.  The public entry
     * serializes with init/shutdown, and the pointer guards below keep this
     * helper safe for recursive diagnostic calls made by partial-init cleanup.
     */
    if (shards != NULL) {
        // Shard metrics are atomic because many updates happen on scheduler hot
        // paths without the shard lock. The lock still stabilizes adjacent queue
        // state for the verbose dump path, but stats only require race-free loads.
        for (i = 0; i < active_shards; ++i) {
            llam_shard_t *shard = &shards[i];

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
            stats->preempt_requests += shard->metrics.preempt_requests;
            stats->preempt_yields += shard->metrics.preempt_yields;
            stats->preempt_suppressed += shard->metrics.preempt_suppressed;
            stats->preempt_signals += shard->metrics.preempt_signals;
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
    }
    // Node submit counters are backend-thread diagnostics. Use relaxed atomics:
    // callers need a race-free snapshot, not a synchronization edge.
    if (nodes != NULL) {
        for (i = 0; i < active_nodes; ++i) {
            stats->io_submit_calls += atomic_load_explicit(&nodes[i].submit_calls, memory_order_relaxed);
            stats->io_submit_syscalls += atomic_load_explicit(&nodes[i].submit_syscalls, memory_order_relaxed);
        }
    }
}

/** @brief Collect a size-aware flat runtime statistics snapshot. */
int llam_runtime_collect_stats_ex_rt(llam_runtime_t *rt, llam_runtime_stats_t *stats, size_t stats_size) {
    llam_runtime_t *pinned_runtime = NULL;
    llam_runtime_stats_t full_stats;
    size_t copy_size;
    int saved_errno;

    if (stats == NULL || stats_size == 0U) {
        errno = EINVAL;
        return -1;
    }
    /* Clear before handle validation so failed FFI snapshots cannot look fresh. */
    memset(stats, 0, stats_size);
    if (rt == NULL) {
        rt = llam_runtime_default_storage();
    }

    memset(&full_stats, 0, sizeof(full_stats));
    saved_errno = errno;

    /* Pin before touching storage; unregistered default stats are pre-init. */
    if (llam_runtime_begin_public_op(rt, &pinned_runtime) != 0) {
        if (rt == llam_runtime_default_storage() && errno == EINVAL) {
            errno = saved_errno;
            goto copy_snapshot;
        }
        return -1;
    }
    if (llam_runtime_lifecycle_trylock() == 0) {
        if (pinned_runtime == llam_runtime_default_storage() &&
            !atomic_load_explicit(&pinned_runtime->initialized, memory_order_acquire) &&
            !atomic_load_explicit(&pinned_runtime->exec_started, memory_order_acquire)) {
            errno = saved_errno;
        } else {
            llam_runtime_collect_stats_full(pinned_runtime, &full_stats);
        }
        llam_runtime_lifecycle_unlock();
    } else {
        /* Race-safe monitoring returns an empty snapshot instead of walking teardown. */
        errno = saved_errno;
    }
    llam_runtime_end_public_op(pinned_runtime);
copy_snapshot:
    copy_size = stats_size < sizeof(full_stats) ? stats_size : sizeof(full_stats);
    memcpy(stats, &full_stats, copy_size);
    return 0;
}

int llam_runtime_collect_stats_ex(llam_runtime_stats_t *stats, size_t stats_size) {
    return llam_runtime_collect_stats_ex_rt(llam_runtime_default_storage(), stats, stats_size);
}

int llam_runtime_collect_stats_ex_handle(llam_runtime_t *runtime, llam_runtime_stats_t *stats, size_t stats_size) {
    if (runtime == NULL) {
        if (stats != NULL && stats_size != 0U) {
            memset(stats, 0, stats_size);
        }
        errno = EINVAL;
        return -1;
    }
    return llam_runtime_collect_stats_ex_rt(runtime, stats, stats_size);
}

int llam_runtime_collect_stats(llam_runtime_stats_t *stats) {
    return llam_runtime_collect_stats_ex(stats, stats != NULL ? sizeof(*stats) : 0U);
}

/** @brief Write a human-readable runtime dump to an fd. */
void llam_dump_runtime_state(int fd) {
    llam_runtime_t *rt = llam_runtime_default_storage();
    llam_runtime_t *pinned_runtime = NULL;
    llam_shard_t *shards;
    llam_node_t *nodes;
    unsigned i;
    unsigned active_shards;
    unsigned active_nodes;
    unsigned block_pending;
    unsigned block_active;
    unsigned overflow_depth;
    unsigned online_shards;
    unsigned online_floor;
    unsigned online_min;
    unsigned online_max;
    unsigned overflow_threshold;
    unsigned pressure;
    const char *profile;

    /* Pin before diagnostics read storage that init/shutdown may rewrite. */
    if (llam_runtime_begin_public_op(rt, &pinned_runtime) != 0 ||
        llam_runtime_lifecycle_trylock() != 0) {
        dprintf(fd,
                "runtime:\n"
                "  lifecycle_lock=busy diagnostics_unavailable=partial_init_or_shutdown\n");
        if (pinned_runtime != NULL) {
            llam_runtime_end_public_op(pinned_runtime);
        }
        return;
    }
    rt = pinned_runtime;

    shards = rt->shards;
    nodes = rt->nodes;
    active_shards = rt->active_shards;
    active_nodes = rt->active_nodes;
    block_pending = atomic_load(&rt->block_pending);
    block_active = atomic_load(&rt->block_active);
    overflow_depth = llam_runtime_overflow_depth(rt);
    online_shards = llam_max_unsigned(1U, llam_runtime_online_shards(rt));
    online_floor = llam_runtime_online_shards_floor(rt);
    online_min = llam_runtime_online_shards_min(rt);
    online_max = llam_runtime_online_shards_max(rt);
    overflow_threshold = llam_max_unsigned(4U, online_shards * 2U);
    pressure = llam_runtime_pressure_signal(rt) ? 1U : 0U;
    profile = rt->profile == LLAM_RUNTIME_PROFILE_RELEASE_FAST
                              ? "release-fast"
                              : (rt->profile == LLAM_RUNTIME_PROFILE_DEBUG_SAFE
                                     ? "debug-safe"
                                     : (rt->profile == LLAM_RUNTIME_PROFILE_IO_LATENCY ? "io-latency" : "balanced"));

    dprintf(fd,
            "runtime:\n"
            "  observed_shards=%u active_shards=%u online_shards=%u online_floor=%u online_min=%u online_max=%u active_nodes=%u profile=%s deterministic=%u forced_yield_every=%u dynamic_shards=%u dynamic_cooldown=%u shard_rings=%u shard_rings_multishot=%u lockfree_normq=%u huge_alloc=%u sqpoll=%u sqpoll_cpu=%d sqpoll_reserved=%u idle_spin_ns=%llu idle_spin_max_iters=%u pressure=%u overflow_depth=%u overflow_threshold=%u live_tasks=%u fatal_errno=%d xsave=%u xsave_mask=0x%llx xsave_area=%zu\n",
            rt->observed_shards,
            active_shards,
            online_shards,
            online_floor,
            online_min,
            online_max,
            active_nodes,
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

    dprintf(fd,
            "lifecycle:\n"
            "  initialized=%u exec_started=%u stop_requested=%u shutdown_requested=%u live_tasks=%u live_task_shards=%u active_io_waiters=%u fatal_errno=%d global_epoch=%llu\n",
            atomic_load_explicit(&rt->initialized, memory_order_acquire) ? 1U : 0U,
            atomic_load_explicit(&rt->exec_started, memory_order_acquire) ? 1U : 0U,
            atomic_load_explicit(&rt->stop_requested, memory_order_acquire) ? 1U : 0U,
            atomic_load_explicit(&rt->shutdown_requested, memory_order_acquire) ? 1U : 0U,
            llam_runtime_live_tasks(rt),
            atomic_load_explicit(&rt->live_task_shards, memory_order_acquire),
            atomic_load_explicit(&rt->active_io_waiters, memory_order_acquire),
            atomic_load_explicit(&rt->fatal_errno, memory_order_acquire),
            (unsigned long long)atomic_load_explicit(&rt->global_epoch, memory_order_acquire));

    // The dump format is intentionally text-first for bug reports and benchmark
    // logs; machine consumers should use llam_runtime_collect_stats().
    dprintf(fd,
            "block:\n"
            "  workers=%u pending=%u active=%u queued=%u peak_active=%u wake_seq=%u\n",
            rt->block_worker_count,
            block_pending,
            block_active,
            block_pending > block_active ? block_pending - block_active : 0U,
            atomic_load(&rt->block_active_peak),
            atomic_load_explicit(&rt->block_wake_seq, memory_order_acquire));

    dprintf(fd, "nodes:\n");
    if (nodes == NULL && active_nodes != 0U) {
        dprintf(fd, "  nodes_unavailable=partial_init active_nodes=%u\n", active_nodes);
    }
    for (i = 0; nodes != NULL && i < active_nodes; ++i) {
        llam_node_t *node = &nodes[i];
        unsigned submit_depth = 0U;
        unsigned control_depth = 0U;
        unsigned poll_watch_count = 0U;
        unsigned poll_waiters = 0U;
        unsigned accept_watch_count = 0U;
        unsigned accept_waiters = 0U;
        unsigned accept_ready = 0U;
        unsigned recv_watch_count = 0U;
        unsigned recv_waiters = 0U;
        unsigned recv_ready = 0U;
        const char *submit_lock_state = "ok";
        const char *watch_lock_state = "ok";

        if (pthread_mutex_trylock(&node->submit_lock) == 0) {
            submit_depth = llam_io_req_list_count_diag(node->submit_head);
            control_depth = llam_io_control_list_count_diag(node->control_head);
            pthread_mutex_unlock(&node->submit_lock);
        } else {
            submit_lock_state = "busy";
        }

        if (pthread_mutex_trylock(&node->watch_lock) == 0) {
            for (llam_poll_watch_t *watch = node->poll_watches; watch != NULL; watch = watch->next) {
                poll_watch_count += 1U;
                poll_waiters += llam_io_req_list_count_diag(watch->wait_head);
            }
            for (llam_accept_watch_t *watch = node->accept_watches; watch != NULL; watch = watch->next) {
                accept_watch_count += 1U;
                accept_waiters += llam_io_req_list_count_diag(watch->wait_head);
                for (llam_accept_ready_t *ready = watch->ready_head; ready != NULL; ready = ready->next) {
                    accept_ready += 1U;
                }
            }
            for (llam_recv_watch_t *watch = node->recv_watches; watch != NULL; watch = watch->next) {
                recv_watch_count += 1U;
                recv_waiters += llam_io_req_list_count_diag(watch->wait_head);
                for (llam_recv_ready_t *ready = watch->ready_head; ready != NULL; ready = ready->next) {
                    recv_ready += 1U;
                }
            }
            pthread_mutex_unlock(&node->watch_lock);
        } else {
            watch_lock_state = "busy";
        }

        dprintf(fd,
                "  node=%u kernel_node=%u ring_ready=%u sqpoll=%u sqpoll_cpu=%d supports(read=%u recv=%u write=%u accept=%u connect=%u poll=%u ms_recv=%u ms_accept=%u ms_poll=%u pbuf=%u) pending_ops=%u locks(submit=%s watch=%s) io_queues(submit=%u control=%u poll_watch=%u poll_waiters=%u accept_watch=%u accept_waiters=%u accept_ready=%u recv_watch=%u recv_waiters=%u recv_ready=%u) submit_batches=%llu submit_entries=%llu submit_calls=%llu submit_syscalls=%llu cancel(ctrl=%llu ok=%llu fail=%llu not_found=%llu) windows_skip(success_handles=%llu failures=%llu immediate=%llu) max_submit=%u cq_depth=%u cq_depth_max=%u unsupported_ops=%llu pbuf(acquire=%llu return=%llu)\n",
                node->index,
                node->kernel_node_id,
                node->ring_ready ? 1U : 0U,
                node->sqpoll_enabled ? 1U : 0U,
                node->sqpoll_enabled ? (int)node->sqpoll_cpu : -1,
                node->supports_read ? 1U : 0U,
                node->supports_recv ? 1U : 0U,
                node->supports_write ? 1U : 0U,
                node->supports_accept ? 1U : 0U,
                node->supports_connect ? 1U : 0U,
                node->supports_poll ? 1U : 0U,
                node->supports_multishot_recv ? 1U : 0U,
                node->supports_multishot_accept ? 1U : 0U,
                node->supports_multishot_poll ? 1U : 0U,
                node->supports_provided_buffers ? 1U : 0U,
                atomic_load_explicit(&node->pending_ops, memory_order_acquire),
                submit_lock_state,
                watch_lock_state,
                submit_depth,
                control_depth,
                poll_watch_count,
                poll_waiters,
                accept_watch_count,
                accept_waiters,
                accept_ready,
                recv_watch_count,
                recv_waiters,
                recv_ready,
                (unsigned long long)atomic_load_explicit(&node->submit_batches, memory_order_relaxed),
                (unsigned long long)atomic_load_explicit(&node->submit_entries, memory_order_relaxed),
                (unsigned long long)atomic_load_explicit(&node->submit_calls, memory_order_relaxed),
                (unsigned long long)atomic_load_explicit(&node->submit_syscalls, memory_order_relaxed),
                (unsigned long long)atomic_load_explicit(&node->windows_cancel_controls, memory_order_relaxed),
                (unsigned long long)atomic_load_explicit(&node->windows_cancel_success, memory_order_relaxed),
                (unsigned long long)atomic_load_explicit(&node->windows_cancel_failures, memory_order_relaxed),
                (unsigned long long)atomic_load_explicit(&node->windows_cancel_not_found, memory_order_relaxed),
                (unsigned long long)atomic_load_explicit(&node->windows_skip_completion_handles, memory_order_relaxed),
                (unsigned long long)atomic_load_explicit(&node->windows_skip_completion_failures, memory_order_relaxed),
                (unsigned long long)atomic_load_explicit(&node->windows_immediate_completions, memory_order_relaxed),
                atomic_load_explicit(&node->max_submit_batch, memory_order_relaxed),
                atomic_load_explicit(&node->last_cq_depth, memory_order_relaxed),
                atomic_load_explicit(&node->max_cq_depth, memory_order_relaxed),
                (unsigned long long)atomic_load_explicit(&node->unsupported_ops, memory_order_relaxed),
                (unsigned long long)atomic_load_explicit(&node->provided_buf_acquires, memory_order_relaxed),
                (unsigned long long)atomic_load_explicit(&node->provided_buf_returns, memory_order_relaxed));
    }

    dprintf(fd, "shards:\n");
    if (shards == NULL && active_shards != 0U) {
        dprintf(fd, "  shards_unavailable=partial_init active_shards=%u\n", active_shards);
    }
    for (i = 0; shards != NULL && i < active_shards; ++i) {
        llam_shard_t *shard = &shards[i];
        size_t trace_count;
        unsigned trace_head;
        unsigned begin;
        unsigned j;
        llam_task_t *current;

        if (pthread_mutex_trylock(&shard->lock) != 0) {
            current = atomic_load_explicit(&shard->current, memory_order_acquire);
            dprintf(fd,
                    "  shard=%u lock=busy online=%u cpu=%u node=%u io_node=%u current=%llu live_tasks=%u inflight_io_waiters=%u event_pending=%u last_safepoint_ns=%llu last_run_started_ns=%llu\n",
                    shard->id,
                    llam_shard_is_online(shard) ? 1U : 0U,
                    shard->cpu_id,
                    shard->node_index,
                    shard->io_node_index,
                    current != NULL ? (unsigned long long)current->id : 0ULL,
                    atomic_load_explicit(&shard->live_tasks, memory_order_acquire),
                    atomic_load_explicit(&shard->inflight_io_waiters, memory_order_acquire),
                    atomic_load_explicit(&shard->event_pending, memory_order_acquire),
                    (unsigned long long)atomic_load_explicit(&shard->last_safepoint_ns, memory_order_acquire),
                    (unsigned long long)atomic_load_explicit(&shard->last_run_started_ns, memory_order_acquire));
            continue;
        }
        trace_head = atomic_load_explicit(&shard->trace_head, memory_order_acquire);
        trace_count = trace_head > LLAM_TRACE_RING_CAP ? LLAM_TRACE_RING_CAP : trace_head;
        begin = trace_head > LLAM_TRACE_RING_CAP ? trace_head - LLAM_TRACE_RING_CAP : 0U;
        current = atomic_load_explicit(&shard->current, memory_order_acquire);
        dprintf(fd,
                "  shard=%u online=%u cpu=%u node=%u io_node=%u current=%llu live_tasks=%u inflight_io_waiters=%u event_pending=%u last_safepoint_ns=%llu last_run_started_ns=%llu opaque_redirect=%u opaque_helper(started=%u ready=%u active=%u) opaque_depth=%u/%u redirect_depth=%u/%u queues(inject=%u hot=%u norm=%u timers=%u)\n",
                shard->id,
                llam_shard_is_online(shard) ? 1U : 0U,
                shard->cpu_id,
                shard->node_index,
                shard->io_node_index,
                current != NULL ? (unsigned long long)current->id : 0ULL,
                atomic_load_explicit(&shard->live_tasks, memory_order_acquire),
                atomic_load_explicit(&shard->inflight_io_waiters, memory_order_acquire),
                atomic_load_explicit(&shard->event_pending, memory_order_acquire),
                (unsigned long long)atomic_load_explicit(&shard->last_safepoint_ns, memory_order_acquire),
                (unsigned long long)atomic_load_explicit(&shard->last_run_started_ns, memory_order_acquire),
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
                "    metrics preempt(requests=%llu yields=%llu suppressed=%llu signals=%llu mode=%u poll_period=%u quantum_ns=%llu)\n",
                (unsigned long long)shard->metrics.preempt_requests,
                (unsigned long long)shard->metrics.preempt_yields,
                (unsigned long long)shard->metrics.preempt_suppressed,
                (unsigned long long)shard->metrics.preempt_signals,
                rt->preempt_mode,
                rt->preempt_poll_period,
                (unsigned long long)rt->preempt_quantum_ns);
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
            unsigned kind = atomic_load_explicit(&event->kind, memory_order_acquire);
            unsigned from_state = atomic_load_explicit(&event->from_state, memory_order_relaxed);
            unsigned to_state = atomic_load_explicit(&event->to_state, memory_order_relaxed);
            unsigned reason = atomic_load_explicit(&event->reason, memory_order_relaxed);
            dprintf(fd,
                    "      ts=%llu kind=%s task=%llu %s->%s reason=%s\n",
                    (unsigned long long)atomic_load_explicit(&event->ts_ns, memory_order_relaxed),
                    llam_trace_kind_name((llam_trace_kind_t)kind),
                    (unsigned long long)atomic_load_explicit(&event->task_id, memory_order_relaxed),
                    llam_state_name_from_id((llam_task_state_id_t)from_state),
                    llam_state_name_from_id((llam_task_state_id_t)to_state),
                    llam_wait_reason_name((llam_wait_reason_t)reason));
        }
        pthread_mutex_unlock(&shard->lock);
    }

    dprintf(fd, "tasks:\n");
    if (shards == NULL && active_shards != 0U) {
        dprintf(fd, "  tasks_unavailable=partial_init active_shards=%u\n", active_shards);
    }
    for (i = 0; shards != NULL && i < active_shards; ++i) {
        llam_shard_t *shard = &shards[i];

        if (pthread_mutex_trylock(&shard->lock) != 0) {
            dprintf(fd, "  shard=%u tasks_unavailable=lock_busy\n", shard->id);
            continue;
        }
        for (llam_task_t *task = shard->all_tasks; task != NULL; task = task->all_next) {
            llam_io_req_t *req = llam_task_active_io_req_load(task);
            llam_block_job_t *job = llam_task_active_block_job_load(task);

            dprintf(fd,
                    "  id=%llu state=%s class=%d flags=0x%x home=%u live=%u last=%u parked=%u wait=%s wait_owner=%s wait_node=%p select=%p join_target=%llu timer=%p deadline_ns=%llu cancel_token=%p cancel_registered=%u wake_error=%d completed=%u join_claimed=%u detached=%u stack=%zu stack_used=%zu stack_peak=%zu stack_hint=%s last_run_ns=%llu total_run_ns=%llu opaque_last_ns=%llu opaque_max_ns=%llu opaque_count=%llu\n",
                    (unsigned long long)task->id,
                    llam_state_name_from_id(task->state),
                    (int)atomic_load_explicit(&task->task_class, memory_order_acquire),
                    task->flags,
                    task->home_shard,
                    task->live_shard,
                    task->last_shard,
                    task->parked_shard,
                    llam_wait_reason_name(
                        (llam_wait_reason_t)atomic_load_explicit(&task->wait_reason, memory_order_acquire)),
                    llam_task_wait_owner_name_diag(task),
                    (void *)NULL,
                    (void *)NULL,
                    0ULL,
                    (void *)NULL,
                    (unsigned long long)task->deadline_ns,
                    (void *)task->cancel_token,
                    task->cancel_registered ? 1U : 0U,
                    task->wake_error_code,
                    atomic_load_explicit(&task->completed, memory_order_acquire),
                    atomic_load_explicit(&task->join_claimed, memory_order_acquire),
                    atomic_load_explicit(&task->detached, memory_order_acquire),
                    task->stack_size,
                    task->last_stack_used,
                    task->stack_high_water,
                    llam_stack_profile_hint(task),
                    (unsigned long long)task->last_run_ns,
                    (unsigned long long)task->total_run_ns,
                    (unsigned long long)task->last_opaque_block_ns,
                    (unsigned long long)task->max_opaque_block_ns,
                    (unsigned long long)task->opaque_block_count);
            if (req != NULL) {
                unsigned wait_mode = atomic_load_explicit(&req->wait_mode, memory_order_acquire);
                unsigned abort_reason = atomic_load_explicit(&req->abort_reason, memory_order_acquire);
                unsigned inflight_owner = atomic_load_explicit(&req->inflight_owner_shard, memory_order_acquire);

                dprintf(fd,
                        "    io_req=%p kind=%s fd=%lld handle=0x%llx owner_shard=%u alloc_owner=%u attached_node=%u inflight_owner=%u wait_mode=%s abort=%s cancel_queued=%u deadline_ns=%llu result=%lld errno=%d poll(events=0x%x revents=0x%x) owned_buffer=%p\n",
                        (void *)req,
                        llam_io_kind_name_diag(req->kind),
                        (long long)req->fd,
                        (unsigned long long)(uintptr_t)req->handle,
                        req->owner_shard,
                        req->alloc_owner_shard,
                        req->attached_node_index,
                        inflight_owner,
                        llam_io_wait_mode_name_diag(wait_mode),
                        llam_io_abort_reason_name_diag(abort_reason),
                        atomic_load_explicit(&req->cancel_queued, memory_order_acquire),
                        (unsigned long long)req->deadline_ns,
                        (long long)req->result,
                        req->error_code,
                        req->poll_events,
                        req->poll_revents,
                        (void *)req->owned_buffer);
            } else {
                dprintf(fd, "    io_req=none\n");
            }
            if (job != NULL) {
                dprintf(fd,
                        "    block_job=%p state=%s errno=%d result=%p\n",
                        (void *)job,
                        llam_block_job_state_name_diag(atomic_load_explicit(&job->state, memory_order_acquire)),
                        atomic_load_explicit(&job->error_code, memory_order_acquire),
                        atomic_load_explicit(&job->result, memory_order_acquire));
            } else {
                dprintf(fd, "    block_job=none\n");
            }
        }
        pthread_mutex_unlock(&shard->lock);
    }
    llam_runtime_lifecycle_unlock();
    llam_runtime_end_public_op(rt);
}
