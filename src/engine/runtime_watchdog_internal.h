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

#ifndef LLAM_RUNTIME_WATCHDOG_INTERNAL_H
#define LLAM_RUNTIME_WATCHDOG_INTERNAL_H

#include "runtime_internal.h"

/*
 * Probe and controller helpers.
 *
 * These are read-mostly checks used by the controller thread. Keep them cheap:
 * they run periodically and should not take long-lived locks or mutate queues
 * except where explicitly documented in the implementation.
 */
bool llam_dynamic_trace_enabled(void);
void llam_watchdog_check_shard(llam_shard_t *shard, uint64_t now_ns);
void llam_watchdog_pause_briefly(void);
unsigned llam_watchdog_snapshot_shard_load(llam_shard_t *shard);

// Runtime-level pressure signals used by deadlock detection and dynamic scaling.
bool llam_runtime_has_pending_timers(llam_runtime_t *rt);
bool llam_runtime_has_runnable_work(llam_runtime_t *rt);
bool llam_runtime_has_runnable_backlog(llam_runtime_t *rt);
bool llam_runtime_has_io_pending(llam_runtime_t *rt);
unsigned llam_runtime_active_io_waiters(llam_runtime_t *rt);
bool llam_runtime_has_opaque_blocking(llam_runtime_t *rt);
void llam_runtime_nudge_marked_watch_migrations(llam_runtime_t *rt);
void llam_runtime_adjust_online_shards(llam_runtime_t *rt);

/*
 * Shard merge/offline helpers.
 *
 * Dynamic downscaling is conservative: a shard is paused, its ownership is
 * migrated, and it is marked offline only after its runnable queues and current
 * task state are empty under lock.
 */
llam_shard_t *llam_runtime_pick_merge_target(llam_runtime_t *rt, llam_shard_t *source);
void llam_runtime_set_steal_pause(llam_runtime_t *rt, bool active);
bool llam_runtime_wait_steal_pause_ack(llam_runtime_t *rt, uint64_t deadline_ns);

// Per-shard pause handshake used before moving queues out of a source shard.
void llam_shard_request_merge_pause(llam_shard_t *shard);
void llam_shard_release_merge_pause(llam_shard_t *shard);
bool llam_shard_wait_merge_pause_ack(llam_shard_t *shard, uint64_t deadline_ns);

// Locked-state predicates and migration helpers used by the offlining path.
bool llam_shard_can_offline_locked(const llam_shard_t *shard);
bool llam_shard_can_start_merge_locked(const llam_shard_t *shard);
void llam_merge_rehome_task(llam_shard_t *source, llam_shard_t *target, llam_task_t *task);
bool llam_merge_runnable_queues_locked(llam_shard_t *source, llam_shard_t *target, unsigned *migrated_out);
bool llam_merge_shard_timers_locked(llam_shard_t *source, llam_shard_t *target, unsigned *migrated_out);

/*
 * Parked waiter and I/O ownership rehome helpers.
 *
 * These functions move wait ownership before a shard goes offline. Completion
 * paths must be able to reinject parked tasks on the new shard after migration.
 */
unsigned llam_shard_inflight_io_waiters(const llam_shard_t *shard);
void llam_rehome_parked_waiters(llam_runtime_t *rt, llam_shard_t *source, llam_shard_t *target, unsigned *migrated_out);
void llam_rehome_inflight_io_waiters(llam_runtime_t *rt,
                                   llam_shard_t *source,
                                   llam_shard_t *target,
                                   unsigned *migrated_out);
bool llam_rehome_node_submit_waiters(llam_node_t *node,
                                   llam_shard_t *source,
                                   llam_shard_t *target,
                                   unsigned *migrated_out);
bool llam_evacuate_rehomed_submit_waiters(llam_node_t *source_node,
                                        llam_node_t *target_node,
                                        llam_shard_t *source,
                                        llam_shard_t *target,
                                        unsigned *migrated_out);
bool llam_rehome_runtime_watch_waiters(llam_runtime_t *rt,
                                     llam_shard_t *source,
                                     llam_shard_t *target,
                                     unsigned *migrated_out);
bool llam_quiesce_cross_io_watch_state(llam_node_t *source, llam_node_t *target, uint64_t deadline_ns);

#endif
