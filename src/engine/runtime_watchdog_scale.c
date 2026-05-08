/**
 * @file src/engine/runtime_watchdog_scale.c
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

#include "runtime_watchdog_internal.h"

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

        if (atomic_load_explicit(&shard->online, memory_order_acquire) != 0U) {
            continue;
        }
        atomic_store_explicit(&shard->online, 1U, memory_order_release);
        llam_runtime_note_online_shards(rt, atomic_fetch_add_explicit(&rt->online_shards, 1U, memory_order_acq_rel) + 1U);
        llam_kick_shard(shard);
        return true;
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
static bool llam_runtime_offline_one_shard(llam_runtime_t *rt) {
    unsigned i;

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
        const char *blocked_reason = NULL;

        if (atomic_load_explicit(&shard->online, memory_order_acquire) == 0U) {
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
        io_node = &rt->nodes[shard->io_node_index];
        same_io_target = target->io_node_index == shard->io_node_index;
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
                                                    &rt->nodes[target->io_node_index],
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
            !llam_quiesce_cross_io_watch_state(io_node, &rt->nodes[target->io_node_index], pause_deadline_ns)) {
            blocked_reason = "cross_io_watch_state";
            goto release_pause;
        }

        first = shard->id < target->id ? shard : target;
        second = first == shard ? target : shard;
        // Lock in shard-id order to avoid ABBA deadlocks with another merge.
        pthread_mutex_lock(&first->lock);
        pthread_mutex_lock(&second->lock);

        if (atomic_load_explicit(&shard->online, memory_order_acquire) == 0U ||
            !llam_shard_accepts_new_work(target) ||
            !llam_shard_can_start_merge_locked(shard)) {
            blocked_reason = "locked_start_state";
            pthread_mutex_unlock(&second->lock);
            pthread_mutex_unlock(&first->lock);
            goto release_pause;
        }

        if (!llam_merge_runnable_queues_locked(shard, target, &runnable_migrated) ||
            !llam_merge_shard_timers_locked(shard, target, &migrated) ||
            !llam_shard_can_offline_locked(shard)) {
            blocked_reason = "merge_or_offline_state";
            pthread_mutex_unlock(&second->lock);
            pthread_mutex_unlock(&first->lock);
            goto release_pause;
        }

        // Only publish offline after every locked state check has passed.
        atomic_store_explicit(&shard->online, 0U, memory_order_release);
        llam_runtime_note_online_shards(rt, atomic_fetch_sub_explicit(&rt->online_shards, 1U, memory_order_acq_rel) - 1U);
        offlined = true;
        kick_target = inflight_migrated > 0U || io_wait_migrated > 0U || runnable_migrated > 0U || migrated > 0U;
        pthread_mutex_unlock(&second->lock);
        pthread_mutex_unlock(&first->lock);

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

            pthread_mutex_lock(&shard->lock);
            norm_depth = llam_norm_queue_depth(shard);
            hot_depth = shard->hot_q.depth;
            inject_depth = shard->inject_q.depth;
            has_current = atomic_load_explicit(&shard->current, memory_order_acquire) != NULL ? 1U : 0U;
            has_timers = shard->timers != NULL ? 1U : 0U;
            pthread_mutex_unlock(&shard->lock);
            fprintf(stderr,
                    "[dynamic] offline blocked shard=%u target=%u reason=%s online=%u live=%u active_io=%u norm=%u hot=%u inject=%u current=%u timers=%u\n",
                    shard->id,
                    target != NULL ? target->id : UINT_MAX,
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
            llam_kick_node(&rt->nodes[target->io_node_index]);
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
    bool want_up;
    bool want_down;

    if (rt->experimental_dynamic_shards == 0U || rt->active_shards < 2U) {
        return;
    }

    online = llam_runtime_online_shards(rt);
    if (online == 0U) {
        atomic_store_explicit(&rt->shards[0].online, 1U, memory_order_release);
        atomic_store_explicit(&rt->online_shards, 1U, memory_order_release);
        llam_runtime_note_online_shards(rt, 1U);
        llam_kick_shard(&rt->shards[0]);
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
    want_up = (pressure ||
               effective_live > online * 2U ||
               (active_io_waiters > 0U && runnable_backlog) ||
               online < online_floor) &&
              online < rt->active_shards;
    want_down = !pressure &&
                online > online_floor &&
                (effective_live + 1U < online || timers_pending) &&
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
        if (llam_runtime_offline_one_shard(rt)) {
            rt->dynamic_scale_cooldown = LLAM_DYNAMIC_SCALE_COOLDOWN_TICKS;
        }
        rt->dynamic_scale_down_streak = 0U;
    }
}
