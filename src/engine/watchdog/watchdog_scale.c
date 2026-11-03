/**
 * @file src/engine/watchdog/watchdog_scale.c
 * @brief Dynamic worker scaling decisions derived from watchdog observations.
 *
 * @details
 * Dynamic scaling adjusts the number of online scheduler shards while preserving
 * correctness for parked tasks and in-flight I/O. Scaling up simply marks one
 * offline shard online. Scaling down is conservative: it pauses stealing when
 * needed, pauses the source shard, rehomes waiters and I/O state, migrates
 * runnable queues, then marks the shard offline only if it is empty.
 *
 * @copyright Copyright 2026 Feralthedogg
 *
 * @par License
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

#include "engine/runtime_watchdog_internal.h"

static bool llam_registered_scaling_open(const llam_runtime_t *rt) {
    return rt->execution_lease_mode == LLAM_EXECUTION_LEASE_REGISTERED &&
        atomic_load_explicit(&rt->exec_started, memory_order_acquire) &&
        !atomic_load_explicit(&rt->stop_requested, memory_order_acquire) &&
        !atomic_load_explicit(&rt->shutdown_requested, memory_order_acquire);
}

/* P storage and its wake resource remain alive. Native readers may retain an
 * old P identity while their M waits for a fresh lease on another online P.
 * This operation never retires M, moves queues, or changes I/O ownership. */
bool llam_managed_offline_empty_processor(llam_runtime_t *rt) {
    if (rt == NULL || !llam_registered_scaling_open(rt)) return false;
    unsigned count = rt->managed_policy_pools ? rt->portable_processor_count : rt->active_shards;
    for (unsigned i = count; i-- > 1U;) {
        llam_shard_t *shard = &rt->shards[i];
        llam_processor_t *processor = &shard->processor;
        llam_processor_owner_record_t owner;
        bool changed = false;
        pthread_mutex_lock(&shard->opaque_lock);
        pthread_mutex_lock(&shard->lock);
        const llam_legacy_lease_t *lease = llam_shard_legacy_lease_locked(shard);
        if (llam_registered_scaling_open(rt) &&
            llam_runtime_online_shards(rt) > rt->dynamic_online_floor &&
            lease->state == LLAM_LEASE_REGISTERED_FREE && lease->owner == NULL &&
            lease->return_worker == NULL &&
            llam_processor_owner_snapshot_pair_locked(processor, &owner) &&
            owner.state == LLAM_PROCESSOR_OWNER_FREE && owner.owner_m == NULL &&
            llam_shard_load_lease_current(shard) == NULL &&
            atomic_load_explicit(&shard->live_tasks, memory_order_acquire) == 0U &&
            llam_shard_inflight_io_waiters(shard) == 0U &&
            atomic_load_explicit(&processor->timer_callbacks_active, memory_order_acquire) == 0U &&
            atomic_load_explicit(&processor->timer_count, memory_order_acquire) == 0U &&
            processor->timer_heap_len == 0U && processor->timers == NULL &&
            processor->inject_q.depth == 0U && processor->hot_q.depth == 0U &&
            llam_norm_queue_depth(shard) == 0U && !shard->opaque_redirect_active &&
            llam_processor_try_set_online(processor, false)) {
            llam_runtime_note_online_shards(rt,
                atomic_fetch_sub_explicit(&rt->online_shards, 1U, memory_order_acq_rel) - 1U);
            changed = true;
        }
        pthread_mutex_unlock(&shard->lock);
        pthread_mutex_unlock(&shard->opaque_lock);
        if (changed) return true;
    }
    return false;
}

/**
 * @brief Kick pending multishot watch migrations across I/O nodes.
 *
 * @param rt Runtime whose nodes may contain marked live-transferred watches.
 */
void llam_runtime_nudge_marked_watch_migrations(llam_runtime_t *rt) {
    unsigned i;

    if (rt == NULL || rt->experimental_shard_rings_multishot == 0U || rt->active_nodes <= 1U) {
        return;
    }

    for (i = 0U; i < rt->active_nodes; ++i) {
        unsigned fallback_index = (i + 1U) % rt->active_nodes;

        if (fallback_index == i) {
            continue;
        }
        (void)llam_io_rehome_marked_watch_state(&rt->nodes[i], &rt->nodes[fallback_index]);
    }
}

/**
 * @brief Bring one offline shard online.
 *
 * @param rt Runtime to scale.
 *
 * @return @c true when a shard was brought online.
 */
static bool llam_runtime_online_one_shard(llam_runtime_t *rt) {
    unsigned i;

    for (i = 1U; i < rt->active_shards; ++i) {
        llam_shard_t *shard = &rt->shards[i];

        if (llam_processor_online(&shard->processor)) {
            continue;
        }
        pthread_mutex_lock(&shard->lock);
        bool changed = llam_processor_try_set_online(&shard->processor, true);
        if (changed) {
            llam_runtime_note_online_shards(rt, atomic_fetch_add_explicit(&rt->online_shards, 1U, memory_order_acq_rel) + 1U);
        }
        pthread_mutex_unlock(&shard->lock);
        if (changed) {
            llam_kick_shard(shard);
            return true;
        }
    }
    return false;
}

/**
 * @brief Attempt to take one online shard offline.
 *
 * This is the expensive half of dynamic scaling. It validates a merge target,
 * moves I/O wait ownership, optionally evacuates submit queues across I/O nodes,
 * migrates runnable queues, and refuses to offline the shard unless it becomes
 * fully empty under the shard locks.
 *
 * @param rt Runtime to scale.
 *
 * @return @c true when one shard was successfully offlined.
 */
static bool llam_async_offline_idle_shard(llam_runtime_t *rt) {
    unsigned i;

    /* Single controller, bounded empty-domain commit. No backend/queue rehome. */
    for (i = rt->active_shards; i-- > 1U;) {
        llam_shard_t *shard = &rt->shards[i];
        uint64_t sequence;
        bool committed = false;
        uint64_t deadline;

        if (!llam_async_preempt_dynamic_allowed(rt)) return false;
        if (!llam_processor_online(&shard->processor) ||
            !llam_async_preempt_idle_domain(shard)) continue;
        sequence = llam_shard_request_merge_pause(shard);
        if (sequence == 0U) continue;
        deadline = llam_now_ns() + 8U * LLAM_WATCHDOG_INTERVAL_NS;
        if (llam_shard_wait_merge_pause_ack(shard, deadline)) {
            pthread_mutex_lock(&shard->opaque_lock);
            pthread_mutex_lock(&shard->lock);
            if (llam_async_preempt_dynamic_allowed(rt) &&
                atomic_load_explicit(&shard->merge_pause_request_sequence, memory_order_acquire) == sequence &&
                llam_shard_can_offline_locked(shard) && llam_async_preempt_idle_domain(shard) &&
                llam_shard_inflight_io_waiters(shard) == 0U &&
                llam_runtime_online_shards(rt) > rt->dynamic_online_floor &&
                llam_processor_try_set_online(&shard->processor, false)) {
                llam_runtime_note_online_shards(rt,
                    atomic_fetch_sub_explicit(&rt->online_shards, 1U, memory_order_acq_rel) - 1U);
                committed = true;
            }
            pthread_mutex_unlock(&shard->lock);
            pthread_mutex_unlock(&shard->opaque_lock);
        }
        /* No source mutation follows unlock. Cleanup never overwrites a reonline. */
        (void)llam_shard_release_merge_pause_if_sequence(shard, sequence);
        if (committed) return true;
    }
    return false;
}

static bool llam_runtime_offline_one_shard(llam_runtime_t *rt) {
    unsigned i;

    if (atomic_load_explicit(&rt->async_preempt_enabled, memory_order_acquire) != 0U) {
        return llam_async_offline_idle_shard(rt);
    }

    // Prefer taking high-index shards offline so shard 0 and low ids stay stable.
    for (i = rt->active_shards; i-- > 1U;) {
        llam_shard_t *shard = &rt->shards[i];
        llam_shard_t *target;
        llam_node_t *io_node;
        llam_shard_t *first;
        llam_shard_t *second;
        unsigned io_wait_migrated = 0U;
        unsigned submit_evacuated = 0U;
        unsigned inflight_migrated = 0U;
        unsigned parked_migrated = 0U;
        unsigned runnable_migrated = 0U;
        unsigned migrated = 0U;
        bool offlined = false;
        bool kick_target = false;
        bool kick_target_node = false;
        bool merge_pause_set = false;
        bool steal_pause_set = false;
        bool need_lockfree_quiesce;
        bool same_io_target;
        uint64_t pause_deadline_ns;
        uint64_t merge_pause_sequence = 0U;
        uint64_t merge_pause_epoch = 0U;
        uint64_t merge_pause_incarnation = 0U;
        uint64_t steal_pause_sequence = 0U;
        uint64_t steal_pause_epoch = 0U;
        uint64_t steal_pause_incarnation = 0U;
        const char *blocked_reason = NULL;

        if (!llam_processor_online(&shard->processor)) {
            continue;
        }
        target = llam_runtime_pick_merge_target(rt, shard);
        if (target == NULL) {
            if (llam_dynamic_trace_enabled()) {
                fprintf(stderr, "[dynamic] offline skip shard=%u reason=no_target\n", shard->id);
            }
            continue;
        }
        // Select the merge target before pausing; locked validation below will
        // reject it if the target stops accepting work in the meantime.
        io_node = &rt->nodes[llam_processor_io_node_index(&shard->processor)];
        same_io_target = llam_processor_io_node_index(&target->processor) ==
                         llam_processor_io_node_index(&shard->processor);
        need_lockfree_quiesce = llam_lockfree_normq_enabled(rt) && llam_norm_queue_depth(shard) > 0U;
        pause_deadline_ns = llam_now_ns() + (LLAM_WATCHDOG_INTERVAL_NS * 8U);
        if (need_lockfree_quiesce) {
            // Lock-free normal queues can be stolen concurrently, so pause
            // stealing globally before draining the source queue.
            llam_runtime_set_steal_pause(rt, true);
            steal_pause_set = true;
            if (!llam_runtime_wait_steal_pause_ack(rt, pause_deadline_ns)) {
                blocked_reason = "steal_pause_ack";
                goto release_pause;
            }
        }
        // Stop the source scheduler at a merge-safe point before touching queues.
        llam_shard_request_merge_pause(shard);
        merge_pause_set = true;
        if (!llam_shard_wait_merge_pause_ack(shard, pause_deadline_ns)) {
            blocked_reason = "merge_pause_ack";
            goto release_pause;
        }
        /* Capture the acknowledged tuple for the lock-protected final check. */
        pthread_mutex_lock(&shard->opaque_lock);
        merge_pause_sequence = atomic_load_explicit(
            &shard->merge_pause_request_sequence,
            memory_order_acquire);
        merge_pause_epoch = atomic_load_explicit(
            &shard->merge_pause_ack_lease_epoch,
            memory_order_acquire);
        merge_pause_incarnation = atomic_load_explicit(
            &shard->merge_pause_ack_worker_incarnation,
            memory_order_acquire);
        if (!llam_shard_merge_pause_ack_matches_locked(shard,
                                                       merge_pause_sequence,
                                                       merge_pause_epoch,
                                                       merge_pause_incarnation)) {
            pthread_mutex_unlock(&shard->opaque_lock);
            blocked_reason = "merge_pause_stale_ack";
            goto release_pause;
        }
        if (steal_pause_set) {
            steal_pause_sequence = atomic_load_explicit(
                &shard->steal_pause_request_sequence,
                memory_order_acquire);
            steal_pause_epoch = atomic_load_explicit(
                &shard->steal_pause_ack_lease_epoch,
                memory_order_acquire);
            steal_pause_incarnation = atomic_load_explicit(
                &shard->steal_pause_ack_worker_incarnation,
                memory_order_acquire);
            if (!llam_shard_steal_pause_ack_matches_locked(
                    shard,
                    steal_pause_sequence,
                    steal_pause_epoch,
                    steal_pause_incarnation)) {
                pthread_mutex_unlock(&shard->opaque_lock);
                blocked_reason = "steal_pause_stale_ack";
                goto release_pause;
            }
        }
        pthread_mutex_unlock(&shard->opaque_lock);
        if (llam_shard_inflight_io_waiters(shard) > 0U) {
            llam_rehome_inflight_io_waiters(rt, shard, target, &inflight_migrated);
            if (llam_shard_inflight_io_waiters(shard) > 0U) {
                blocked_reason = "inflight_io_waiters";
                goto release_pause;
            }
        }
        {
            unsigned submit_migrated = 0U;
            unsigned watch_migrated = 0U;

            // Submit queues and multishot watches carry owner_shard metadata;
            // rewrite that ownership before the shard can disappear.
            if (!llam_rehome_node_submit_waiters(io_node, shard, target, &submit_migrated) ||
                !llam_rehome_runtime_watch_waiters(rt, shard, target, &watch_migrated)) {
                blocked_reason = "rehome_io_waiters";
                goto release_pause;
            }
            if (!same_io_target &&
                !llam_evacuate_rehomed_submit_waiters(io_node,
                                                    &rt->nodes[llam_processor_io_node_index(&target->processor)],
                                                    shard,
                                                    target,
                                                    &submit_evacuated)) {
                blocked_reason = "evacuate_submit_waiters";
                goto release_pause;
            }
            io_wait_migrated = submit_migrated + watch_migrated;
            if (submit_evacuated > 0U) {
                kick_target_node = true;
            }
            if (watch_migrated > 0U) {
                llam_runtime_nudge_marked_watch_migrations(rt);
            }
        }
        if (llam_shard_inflight_io_waiters(shard) > 0U) {
            blocked_reason = "post_rehome_inflight_io_waiters";
            goto release_pause;
        }
        if (!same_io_target &&
            !llam_quiesce_cross_io_watch_state(
                io_node,
                &rt->nodes[llam_processor_io_node_index(&target->processor)],
                pause_deadline_ns)) {
            blocked_reason = "cross_io_watch_state";
            goto release_pause;
        }

        first = shard->id < target->id ? shard : target;
        second = first == shard ? target : shard;
        // Lock in shard-id order to avoid ABBA deadlocks with another merge;
        // each lease lock precedes its shard lock.
        pthread_mutex_lock(&first->opaque_lock);
        pthread_mutex_lock(&second->opaque_lock);
        pthread_mutex_lock(&first->lock);
        pthread_mutex_lock(&second->lock);

        if (!llam_processor_online(&shard->processor) ||
            !llam_shard_accepts_new_work(target) ||
            !llam_shard_can_start_merge_locked(shard) ||
            !llam_shard_merge_pause_ack_matches_locked(shard,
                                                       merge_pause_sequence,
                                                       merge_pause_epoch,
                                                       merge_pause_incarnation) ||
            (steal_pause_set &&
             !llam_shard_steal_pause_ack_matches_locked(
                 shard,
                 steal_pause_sequence,
                 steal_pause_epoch,
                 steal_pause_incarnation))) {
            blocked_reason = "locked_start_state";
            pthread_mutex_unlock(&second->lock);
            pthread_mutex_unlock(&first->lock);
            pthread_mutex_unlock(&second->opaque_lock);
            pthread_mutex_unlock(&first->opaque_lock);
            goto release_pause;
        }

        if (!llam_merge_runnable_queues_locked(shard, target, &runnable_migrated)) {
            blocked_reason = "merge_or_offline_state";
            pthread_mutex_unlock(&second->lock);
            pthread_mutex_unlock(&first->lock);
            pthread_mutex_unlock(&second->opaque_lock);
            pthread_mutex_unlock(&first->opaque_lock);
            goto release_pause;
        }
        if (runnable_migrated > 0U) {
            /* A later timer/offline rejection must still wake moved work. */
            kick_target = true;
        }
        if (!llam_merge_shard_timers_locked(shard, target, &migrated)) {
            blocked_reason = "merge_or_offline_state";
            pthread_mutex_unlock(&second->lock);
            pthread_mutex_unlock(&first->lock);
            pthread_mutex_unlock(&second->opaque_lock);
            pthread_mutex_unlock(&first->opaque_lock);
            goto release_pause;
        }
        if (migrated > 0U) {
            kick_target = true;
        }
        if (!llam_shard_can_offline_locked(shard)) {
            blocked_reason = "merge_or_offline_state";
            pthread_mutex_unlock(&second->lock);
            pthread_mutex_unlock(&first->lock);
            pthread_mutex_unlock(&second->opaque_lock);
            pthread_mutex_unlock(&first->opaque_lock);
            goto release_pause;
        }

        // Only publish offline after every locked state check has passed.
        llam_processor_set_online(&shard->processor, false);
        llam_runtime_note_online_shards(rt, atomic_fetch_sub_explicit(&rt->online_shards, 1U, memory_order_acq_rel) - 1U);
        offlined = true;
        kick_target = kick_target || inflight_migrated > 0U ||
                      io_wait_migrated > 0U || runnable_migrated > 0U ||
                      migrated > 0U;
        pthread_mutex_unlock(&second->lock);
        pthread_mutex_unlock(&first->lock);
        pthread_mutex_unlock(&second->opaque_lock);
        pthread_mutex_unlock(&first->opaque_lock);

        if (offlined) {
            llam_rehome_parked_waiters(rt, shard, target, &parked_migrated);
            if (parked_migrated > 0U) {
                kick_target = true;
            }
        }
release_pause:
        // One cleanup path keeps partially failed merge attempts from leaking
        // pause state. Pauses are released even when offlining succeeds.
        if (!offlined && blocked_reason != NULL && llam_dynamic_trace_enabled()) {
            unsigned norm_depth;
            unsigned hot_depth;
            unsigned inject_depth;
            unsigned has_current;
            unsigned has_timers;

            pthread_mutex_lock(&shard->opaque_lock);
            pthread_mutex_lock(&shard->lock);
            norm_depth = llam_norm_queue_depth(shard);
            hot_depth = shard->processor.hot_q.depth;
            inject_depth = shard->processor.inject_q.depth;
            has_current = llam_shard_load_lease_current(shard) != NULL ? 1U : 0U;
            has_timers = (shard->processor.timers != NULL ||
                          atomic_load_explicit(&shard->processor.timer_callbacks_active, memory_order_acquire) != 0U)
                             ? 1U
                             : 0U;
            pthread_mutex_unlock(&shard->lock);
            pthread_mutex_unlock(&shard->opaque_lock);
            fprintf(stderr,
                    "[dynamic] offline blocked shard=%u target=%u reason=%s online=%u live=%u active_io=%u norm=%u hot=%u inject=%u current=%u timers=%u\n",
                    shard->id,
                    (unsigned)(target != NULL ? target->id : UINT_MAX),
                    blocked_reason,
                    llam_runtime_online_shards(rt),
                    llam_runtime_live_tasks(rt),
                    llam_runtime_active_io_waiters(rt),
                    norm_depth,
                    hot_depth,
                    inject_depth,
                    has_current,
                    has_timers);
        }
        if (merge_pause_set) {
            llam_shard_release_merge_pause(shard);
        }
        if (steal_pause_set) {
            llam_runtime_set_steal_pause(rt, false);
        }
        if (kick_target) {
            llam_kick_shard(target);
        }
        if (kick_target_node) {
            llam_kick_node(&rt->nodes[llam_processor_io_node_index(&target->processor)]);
        }
        if (offlined) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Apply one watchdog tick of dynamic shard scaling policy.
 *
 * The policy scales up on runnable pressure, live-task pressure, or I/O waiters
 * with backlog. It scales down only after cooldown/streak checks and only when
 * no blocking jobs, runnable backlog, or opaque-block compensation is active.
 *
 * @param rt Runtime to adjust.
 */
void llam_runtime_adjust_online_shards(llam_runtime_t *rt) {
    unsigned online;
    unsigned live;
    unsigned active_io_waiters;
    unsigned effective_live;
    unsigned online_floor;
    bool timers_pending;
    bool pressure;
    bool runnable_backlog;
    bool live_saturated;
    bool live_pressure;
    bool idle_capacity;
    bool want_up;
    bool want_down;

    bool registered = rt->execution_lease_mode == LLAM_EXECUTION_LEASE_REGISTERED;
    if (rt->experimental_dynamic_shards == 0U || rt->active_shards < 2U ||
        (registered ? !llam_registered_scaling_open(rt) : !llam_async_preempt_dynamic_allowed(rt))) {
        return;
    }

    online = llam_runtime_online_shards(rt);
    if (online == 0U) {
        llam_shard_t *primary = &rt->shards[0];

        pthread_mutex_lock(&primary->lock);
        if (llam_processor_try_set_online(&primary->processor, true)) {
            llam_runtime_note_online_shards(rt,
                atomic_fetch_add_explicit(&rt->online_shards, 1U, memory_order_acq_rel) + 1U);
        }
        pthread_mutex_unlock(&primary->lock);
        llam_kick_shard(primary);
        return;
    }

    live = llam_runtime_live_tasks(rt);
    active_io_waiters = llam_runtime_active_io_waiters(rt);
    // I/O waiters are live tasks, but they are not consuming CPU while parked.
    effective_live = live > active_io_waiters ? live - active_io_waiters : 0U;
    timers_pending = llam_runtime_has_pending_timers(rt);
    online_floor = llam_runtime_online_shards_floor(rt);
    if (active_io_waiters > 0U && rt->active_shards > 8U) {
        // Keep more workers online during I/O-heavy phases so completions can
        // resume tasks without waiting for the scaler to ramp back up.
        online_floor = llam_max_unsigned(online_floor, rt->active_shards - (rt->active_shards / 4U));
    }
#if LLAM_RUNTIME_BACKEND_WINDOWS
    if (timers_pending && rt->active_shards <= 8U) {
        /*
         * Windows VM timer fanout is sensitive to parked worker count: if only
         * the dynamic floor is online, timer shards can wake late and create
         * large p99 spikes.  Keep small Windows runtimes fully online while
         * timers are armed, but preserve dynamic scaling for spawn/I/O phases.
         */
        online_floor = rt->active_shards;
    }
#endif
    pressure = llam_runtime_pressure_signal(rt);
    runnable_backlog = llam_runtime_has_runnable_backlog(rt);
    live_saturated = live == UINT_MAX;
    live_pressure = live_saturated ||
                    (online <= UINT_MAX / 2U && effective_live > online * 2U);
    /*
     * Avoid effective_live + 1 and fail closed on saturated live counts:
     * diagnostics can intentionally clamp live tasks at UINT_MAX, and wrapping
     * that value would make the scaler treat unknown/high liveness as idle.
     */
    idle_capacity = !live_saturated && online > 0U && effective_live < online - 1U;
    want_up = (pressure ||
               live_pressure ||
               (active_io_waiters > 0U && runnable_backlog) ||
               online < online_floor) &&
              online < rt->active_shards;
    want_down = !pressure &&
                online > online_floor &&
                !live_saturated &&
                (idle_capacity || timers_pending) &&
                atomic_load(&rt->block_pending) == 0U &&
                !runnable_backlog &&
                !llam_runtime_has_opaque_blocking(rt);
    if (llam_dynamic_trace_enabled() && online > online_floor) {
        fprintf(stderr,
                "[dynamic] adjust online=%u floor=%u live=%u active_io=%u effective=%u timers=%u pressure=%u backlog=%u block_pending=%u want_up=%u want_down=%u up_streak=%u down_streak=%u cooldown=%u\n",
                online,
                online_floor,
                live,
                active_io_waiters,
                effective_live,
                timers_pending ? 1U : 0U,
                pressure ? 1U : 0U,
                runnable_backlog ? 1U : 0U,
                atomic_load(&rt->block_pending),
                want_up ? 1U : 0U,
                want_down ? 1U : 0U,
                rt->dynamic_scale_up_streak,
                rt->dynamic_scale_down_streak,
                rt->dynamic_scale_cooldown);
    }

    if (rt->dynamic_scale_cooldown > 0U) {
        rt->dynamic_scale_cooldown -= 1U;
    }

    if (want_up) {
        rt->dynamic_scale_up_streak += 1U;
        rt->dynamic_scale_down_streak = 0U;
    } else if (want_down) {
        rt->dynamic_scale_down_streak += 1U;
        rt->dynamic_scale_up_streak = 0U;
    } else {
        rt->dynamic_scale_up_streak = 0U;
        rt->dynamic_scale_down_streak = 0U;
    }

    if (want_up &&
        rt->dynamic_scale_cooldown == 0U &&
        rt->dynamic_scale_up_streak >= LLAM_DYNAMIC_SCALE_UP_STREAK) {
        if (llam_runtime_online_one_shard(rt)) {
            rt->dynamic_scale_cooldown = LLAM_DYNAMIC_SCALE_COOLDOWN_TICKS;
        }
        rt->dynamic_scale_up_streak = 0U;
        return;
    }

    if (want_down &&
        rt->dynamic_scale_cooldown == 0U &&
        rt->dynamic_scale_down_streak >= LLAM_DYNAMIC_SCALE_DOWN_STREAK) {
        if (registered ? llam_managed_offline_empty_processor(rt) : llam_runtime_offline_one_shard(rt)) {
            rt->dynamic_scale_cooldown = LLAM_DYNAMIC_SCALE_COOLDOWN_TICKS;
        }
        rt->dynamic_scale_down_streak = 0U;
    }
}
