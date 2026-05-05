/**
 * @file src/internal/runtime_proto_sched.h
 * @brief Internal prototypes for scheduler queues, worker execution, and task state transitions.
 *
 * @details
 * Scheduler helpers are separated into queue primitives, reinjection/wakeup,
 * parking/deadline bookkeeping, dynamic online-state accounting, and worker
 * loops. Most functions assume the caller already holds the shard lock when the
 * name ends in @c _locked.
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

#ifndef NM_RUNTIME_PROTO_SCHED_H
#define NM_RUNTIME_PROTO_SCHED_H

#include "runtime_state.h"

/*
 * Opaque-block redirect and helper compensation.
 */
void nm_activate_opaque_redirect_locked(nm_shard_t *shard, nm_task_t *current_task);
void nm_deactivate_opaque_redirect_locked(nm_shard_t *shard);
bool nm_enqueue_opaque_redirect_task_locked(nm_shard_t *blocked, nm_task_t *task, bool hot);
int nm_ensure_opaque_helper_locked(nm_shard_t *shard);
void *nm_opaque_helper_main(void *arg);

/*
 * Worker loops and idle handling.
 */
void *nm_block_worker_main(void *arg);
void *nm_ctrl_worker_main(void *arg);
void nm_idle_wait(nm_shard_t *shard);
void nm_scheduler_loop(nm_shard_t *shard);
void *nm_shard_worker_main(void *arg);

/*
 * Normal queue and deque primitives.
 */
void nm_cldeque_init(nm_cldeque_t *deque);
bool nm_lockfree_normq_enabled(const nm_runtime_t *rt);
unsigned nm_norm_queue_depth(const nm_shard_t *shard);
nm_task_t *nm_norm_queue_pop_owner_locked(nm_shard_t *shard);
bool nm_norm_queue_push_owner_locked(nm_shard_t *shard, nm_task_t *task);
bool nm_norm_queue_push_yield_locked(nm_shard_t *shard, nm_task_t *task);
nm_task_t *nm_norm_queue_steal(nm_shard_t *victim);
nm_task_t *nm_queue_pop_head(nm_queue_t *queue);
nm_task_t *nm_queue_pop_tail(nm_queue_t *queue);
bool nm_queue_push_bounded_locked(nm_shard_t *shard,
                                  nm_queue_t *queue,
                                  unsigned capacity,
                                  nm_task_t *task);
void nm_queue_push_tail(nm_queue_t *queue, nm_task_t *task);

/*
 * Runnable placement, overflow queues, and stealing.
 */
void nm_drain_inject_queue(nm_shard_t *shard);
void nm_enqueue_overflow_task(nm_runtime_t *rt, nm_task_t *task);
unsigned nm_hot_streak_cap_locked(nm_shard_t *shard, bool pressure);
void nm_mark_runnable_locked(nm_shard_t *shard,
                             nm_task_t *task,
                             bool hot,
                             nm_trace_kind_t kind,
                             nm_wait_reason_t reason,
                             bool direct_local);
unsigned nm_pick_runnable_shard(nm_runtime_t *rt, nm_task_t *task);
unsigned nm_pick_spawn_shard(nm_runtime_t *rt);
bool nm_should_enqueue_hot_locked(nm_shard_t *shard,
                                  const nm_task_t *task,
                                  bool hot_requested,
                                  bool pressure);
nm_task_t *nm_take_local_task(nm_shard_t *shard);
nm_task_t *nm_take_local_task_with_pressure(nm_shard_t *shard, bool pressure);
nm_task_t *nm_take_overflow_task(nm_runtime_t *rt);
nm_task_t *nm_try_steal_task(nm_runtime_t *rt, nm_shard_t *shard);

/*
 * Reinjecting parked or completed tasks back into scheduler queues.
 */
void nm_reinject_join_waiters(nm_runtime_t *rt, nm_task_t *task);
void nm_reinject_task(nm_runtime_t *rt,
                      nm_task_t *task,
                      bool hot,
                      nm_trace_kind_t kind,
                      nm_wait_reason_t reason);
void nm_reinject_task_on_shard(nm_runtime_t *rt,
                               nm_task_t *task,
                               unsigned target_id,
                               bool hot,
                               nm_trace_kind_t kind,
                               nm_wait_reason_t reason);
bool nm_reinject_task_on_shard_and_yield_current(nm_runtime_t *rt,
                                                 nm_task_t *task,
                                                 unsigned target_id,
                                                 bool hot,
                                                 nm_trace_kind_t kind,
                                                 nm_wait_reason_t reason);

/*
 * Runtime online-shard counters and pressure predicates.
 */
bool nm_runtime_steal_pause_active(const nm_runtime_t *rt);
unsigned nm_runtime_overflow_depth(nm_runtime_t *rt);
unsigned nm_runtime_online_shards(const nm_runtime_t *rt);
unsigned nm_runtime_online_shards_floor(const nm_runtime_t *rt);
unsigned nm_runtime_online_shards_max(const nm_runtime_t *rt);
unsigned nm_runtime_online_shards_min(const nm_runtime_t *rt);
void nm_runtime_note_online_shards(nm_runtime_t *rt, unsigned online);
bool nm_runtime_pressure_signal(nm_runtime_t *rt);

/*
 * Shard state predicates used by scheduler, watchdog, and merge code.
 */
bool nm_shard_accepts_new_work(const nm_shard_t *shard);
bool nm_shard_merge_pause_requested(const nm_shard_t *shard);
bool nm_shard_is_online(const nm_shard_t *shard);

/*
 * Wait deadline, cancellation, join, and timer dispatch.
 */
int nm_arm_task_wait_deadline(nm_task_t *task, nm_shard_t *shard, uint64_t deadline_ns);
void nm_cancel_task_wait(nm_task_t *task);
int nm_cancel_token_register_task(nm_task_t *task);
void nm_cancel_token_unregister_task(nm_task_t *task);
bool nm_deadline_passed(uint64_t deadline_ns);
void nm_disarm_task_wait_deadline(nm_task_t *task);
void nm_fire_expired_timers(nm_shard_t *shard);
bool nm_join_waiter_remove_locked(nm_task_t *target, nm_task_t *waiter);
void nm_park_current_task(nm_wait_reason_t reason, nm_trace_kind_t kind);
void nm_task_clear_wait_tracking(nm_task_t *task);
void nm_task_set_block_tracking(nm_task_t *task, nm_block_job_t *job, unsigned parked_shard);
void nm_task_set_join_tracking(nm_task_t *task, nm_task_t *target, unsigned parked_shard);
void nm_task_set_sleep_tracking(nm_task_t *task, unsigned parked_shard);
void nm_timer_insert_locked(nm_shard_t *shard, nm_task_t *task);
void nm_timer_node_free(nm_shard_t *shard, nm_timer_node_t *node);
bool nm_timer_remove_locked(nm_shard_t *shard, nm_task_t *task);

#endif
