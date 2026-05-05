/**
 * @file src/engine/runtime_watchdog_internal.h
 * @brief Internal watchdog and dynamic-worker helper declarations.
 *
 * @details
 * These declarations are shared by the watchdog implementation units. The
 * watchdog is split by concern: probing runtime pressure, scaling online shard
 * count, merging/offlining shard state, rehoming parked waiters, and running
 * the controller loop.
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

#ifndef NM_RUNTIME_WATCHDOG_INTERNAL_H
#define NM_RUNTIME_WATCHDOG_INTERNAL_H

#include "runtime_internal.h"

/*
 * Probe and controller helpers.
 *
 * These are read-mostly checks used by the controller thread. Keep them cheap:
 * they run periodically and should not take long-lived locks or mutate queues
 * except where explicitly documented in the implementation.
 */
bool nm_dynamic_trace_enabled(void);
void nm_watchdog_check_shard(nm_shard_t *shard, uint64_t now_ns);
void nm_watchdog_pause_briefly(void);
unsigned nm_watchdog_snapshot_shard_load(nm_shard_t *shard);

// Runtime-level pressure signals used by deadlock detection and dynamic scaling.
bool nm_runtime_has_pending_timers(nm_runtime_t *rt);
bool nm_runtime_has_runnable_work(nm_runtime_t *rt);
bool nm_runtime_has_runnable_backlog(nm_runtime_t *rt);
bool nm_runtime_has_io_pending(nm_runtime_t *rt);
unsigned nm_runtime_active_io_waiters(nm_runtime_t *rt);
bool nm_runtime_has_opaque_blocking(nm_runtime_t *rt);
void nm_runtime_nudge_marked_watch_migrations(nm_runtime_t *rt);
void nm_runtime_adjust_online_shards(nm_runtime_t *rt);

/*
 * Shard merge/offline helpers.
 *
 * Dynamic downscaling is conservative: a shard is paused, its ownership is
 * migrated, and it is marked offline only after its runnable queues and current
 * task state are empty under lock.
 */
nm_shard_t *nm_runtime_pick_merge_target(nm_runtime_t *rt, nm_shard_t *source);
void nm_runtime_set_steal_pause(nm_runtime_t *rt, bool active);
bool nm_runtime_wait_steal_pause_ack(nm_runtime_t *rt, uint64_t deadline_ns);

// Per-shard pause handshake used before moving queues out of a source shard.
void nm_shard_request_merge_pause(nm_shard_t *shard);
void nm_shard_release_merge_pause(nm_shard_t *shard);
bool nm_shard_wait_merge_pause_ack(nm_shard_t *shard, uint64_t deadline_ns);

// Locked-state predicates and migration helpers used by the offlining path.
bool nm_shard_can_offline_locked(const nm_shard_t *shard);
bool nm_shard_can_start_merge_locked(const nm_shard_t *shard);
void nm_merge_rehome_task(nm_shard_t *source, nm_shard_t *target, nm_task_t *task);
bool nm_merge_runnable_queues_locked(nm_shard_t *source, nm_shard_t *target, unsigned *migrated_out);
bool nm_merge_shard_timers_locked(nm_shard_t *source, nm_shard_t *target, unsigned *migrated_out);

/*
 * Parked waiter and I/O ownership rehome helpers.
 *
 * These functions move wait ownership before a shard goes offline. Completion
 * paths must be able to reinject parked tasks on the new shard after migration.
 */
unsigned nm_shard_inflight_io_waiters(const nm_shard_t *shard);
void nm_rehome_parked_waiters(nm_runtime_t *rt, nm_shard_t *source, nm_shard_t *target, unsigned *migrated_out);
void nm_rehome_inflight_io_waiters(nm_runtime_t *rt,
                                   nm_shard_t *source,
                                   nm_shard_t *target,
                                   unsigned *migrated_out);
bool nm_rehome_node_submit_waiters(nm_node_t *node,
                                   nm_shard_t *source,
                                   nm_shard_t *target,
                                   unsigned *migrated_out);
bool nm_evacuate_rehomed_submit_waiters(nm_node_t *source_node,
                                        nm_node_t *target_node,
                                        nm_shard_t *source,
                                        nm_shard_t *target,
                                        unsigned *migrated_out);
bool nm_rehome_runtime_watch_waiters(nm_runtime_t *rt,
                                     nm_shard_t *source,
                                     nm_shard_t *target,
                                     unsigned *migrated_out);
bool nm_quiesce_cross_io_watch_state(nm_node_t *source, nm_node_t *target, uint64_t deadline_ns);

#endif
