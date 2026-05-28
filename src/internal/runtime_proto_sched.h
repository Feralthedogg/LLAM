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

#ifndef LLAM_RUNTIME_PROTO_SCHED_H
#define LLAM_RUNTIME_PROTO_SCHED_H

#include "runtime_state.h"

/*
 * Opaque-block redirect and helper compensation.
 */
void llam_activate_opaque_redirect_locked(llam_shard_t *shard, llam_task_t *current_task);
void llam_deactivate_opaque_redirect_locked(llam_shard_t *shard);
bool llam_enqueue_opaque_redirect_task_locked(llam_shard_t *blocked, llam_task_t *task, bool hot);
int llam_ensure_opaque_helper_locked(llam_shard_t *shard);
void *llam_opaque_helper_main(void *arg);

/*
 * Worker loops and idle handling.
 */
void *llam_block_worker_main(void *arg);
bool llam_runtime_note_block_pending(llam_runtime_t *rt, unsigned amount);
bool llam_runtime_complete_block_pending(llam_runtime_t *rt, unsigned amount);
void *llam_ctrl_worker_main(void *arg);
void llam_idle_wait(llam_shard_t *shard);
void llam_scheduler_loop(llam_shard_t *shard);
void *llam_shard_worker_main(void *arg);

/*
 * Normal queue and deque primitives.
 */
void llam_cldeque_init(llam_cldeque_t *deque);
bool llam_lockfree_normq_enabled(const llam_runtime_t *rt);
unsigned llam_norm_queue_depth(const llam_shard_t *shard);
llam_task_t *llam_norm_queue_pop_owner_locked(llam_shard_t *shard);
llam_task_t *llam_norm_queue_pop_owner_unlocked(llam_shard_t *shard);
bool llam_norm_queue_exchange_yield_unlocked(llam_shard_t *shard,
                                           llam_task_t *current,
                                           llam_task_t **out_next,
                                           bool *out_push_failed);
bool llam_norm_queue_push_owner_locked(llam_shard_t *shard, llam_task_t *task);
bool llam_norm_queue_push_owner_unlocked(llam_shard_t *shard, llam_task_t *task);
bool llam_norm_queue_push_yield_locked(llam_shard_t *shard, llam_task_t *task);
bool llam_norm_queue_push_yield_unlocked(llam_shard_t *shard, llam_task_t *task);
llam_task_t *llam_norm_queue_steal(llam_shard_t *victim);
llam_task_t *llam_queue_pop_head(llam_queue_t *queue);
llam_task_t *llam_queue_pop_tail(llam_queue_t *queue);
bool llam_queue_push_bounded_locked(llam_shard_t *shard,
                                  llam_queue_t *queue,
                                  unsigned capacity,
                                  llam_task_t *task);
void llam_queue_push_tail(llam_queue_t *queue, llam_task_t *task);

/*
 * Runnable placement, overflow queues, and stealing.
 */
void llam_drain_inject_queue(llam_shard_t *shard);
void llam_enqueue_overflow_task(llam_runtime_t *rt, llam_task_t *task);
unsigned llam_hot_streak_cap_locked(llam_shard_t *shard, bool pressure);
void llam_mark_runnable_locked(llam_shard_t *shard,
                             llam_task_t *task,
                             bool hot,
                             llam_trace_kind_t kind,
                             llam_wait_reason_t reason,
                             bool direct_local);
unsigned llam_pick_runnable_shard(llam_runtime_t *rt, llam_task_t *task);
unsigned llam_pick_spawn_shard(llam_runtime_t *rt);
bool llam_should_enqueue_hot_locked(llam_shard_t *shard,
                                  const llam_task_t *task,
                                  bool hot_requested,
                                  bool pressure);
llam_task_t *llam_take_local_task(llam_shard_t *shard);
llam_task_t *llam_take_local_task_with_pressure(llam_shard_t *shard, bool pressure);
llam_task_t *llam_take_overflow_task(llam_runtime_t *rt);
llam_task_t *llam_try_steal_task(llam_runtime_t *rt, llam_shard_t *shard);

/*
 * Reinjecting parked or completed tasks back into scheduler queues.
 */
void llam_reinject_join_waiters(llam_runtime_t *rt, llam_task_t *task);
void llam_reinject_task(llam_runtime_t *rt,
                      llam_task_t *task,
                      bool hot,
                      llam_trace_kind_t kind,
                      llam_wait_reason_t reason);
void llam_reinject_task_on_shard(llam_runtime_t *rt,
                               llam_task_t *task,
                               unsigned target_id,
                               bool hot,
                               llam_trace_kind_t kind,
                               llam_wait_reason_t reason);
bool llam_reinject_task_on_shard_and_yield_current(llam_runtime_t *rt,
                                                 llam_task_t *task,
                                                 unsigned target_id,
                                                 bool hot,
                                                 llam_trace_kind_t kind,
                                                 llam_wait_reason_t reason);
bool llam_yield_to_local_runnable(void);

/*
 * Runtime online-shard counters and pressure predicates.
 */
bool llam_runtime_steal_pause_active(const llam_runtime_t *rt);
unsigned llam_runtime_overflow_depth(llam_runtime_t *rt);
unsigned llam_runtime_online_shards(const llam_runtime_t *rt);
unsigned llam_runtime_online_shards_floor(const llam_runtime_t *rt);
unsigned llam_runtime_online_shards_max(const llam_runtime_t *rt);
unsigned llam_runtime_online_shards_min(const llam_runtime_t *rt);
void llam_runtime_note_online_shards(llam_runtime_t *rt, unsigned online);
bool llam_runtime_pressure_signal(llam_runtime_t *rt);

/*
 * Shard state predicates used by scheduler, watchdog, and merge code.
 */
bool llam_shard_accepts_new_work(const llam_shard_t *shard);
bool llam_shard_merge_pause_requested(const llam_shard_t *shard);
bool llam_shard_is_online(const llam_shard_t *shard);

/*
 * Wait deadline, cancellation, join, and timer dispatch.
 *
 * Cancellation-token public handles use the shared slot+family generation
 * scheme so sync/task handles cannot be mistaken for a token by FFI callers.
 */
#if UINTPTR_MAX <= UINT32_MAX
#error "LLAM cancellation-token public handles require uintptr_t wider than 32 bits"
#define LLAM_CANCEL_TOKEN_PUBLIC_HANDLE_SHIFT 0U
#else
#define LLAM_CANCEL_TOKEN_PUBLIC_HANDLE_SHIFT 32U
#endif

static inline llam_cancel_token_t *llam_cancel_token_public_handle(llam_cancel_token_t *token) {
    if (token == NULL) {
        return NULL;
    }
    return (llam_cancel_token_t *)llam_public_slot_encode_handle(token->public_handle_slot,
                                                                 token->public_handle_generation,
                                                                 LLAM_CANCEL_TOKEN_PUBLIC_HANDLE_SHIFT);
}

int llam_arm_task_wait_deadline(llam_task_t *task, llam_shard_t *shard, uint64_t deadline_ns);
void llam_cancel_task_wait(llam_task_t *task);
void llam_runtime_cancel_parked_waiters(llam_runtime_t *rt);
int llam_cancel_token_register_task(llam_task_t *task);
void llam_cancel_token_unregister_task(llam_task_t *task);
int llam_cancel_token_retain_task_ref(llam_cancel_token_t *token, llam_cancel_token_t **out_token);
int llam_cancel_token_retain_task_ref_for_runtime(llam_cancel_token_t *token,
                                                  llam_runtime_t *owner_runtime,
                                                  llam_cancel_token_t **out_token);
void llam_cancel_token_release_task_ref(llam_cancel_token_t *token);
bool llam_deadline_passed(uint64_t deadline_ns);
void llam_disarm_task_wait_deadline(llam_task_t *task);
void llam_fire_expired_timers(llam_shard_t *shard);
bool llam_task_wait_deadline_active(llam_task_t *task);
bool llam_join_waiter_remove_locked(llam_task_t *target, llam_task_t *waiter);
void llam_park_current_task(llam_wait_reason_t reason, llam_trace_kind_t kind);
void llam_task_clear_wait_tracking(llam_task_t *task);
llam_block_job_t *llam_task_active_block_job_load(const llam_task_t *task);
void llam_task_set_block_tracking(llam_task_t *task, llam_block_job_t *job, unsigned parked_shard);
void llam_task_set_join_tracking(llam_task_t *task, llam_task_t *target, unsigned parked_shard);
void llam_task_set_sleep_tracking(llam_task_t *task, unsigned parked_shard);
void llam_timer_insert_locked(llam_shard_t *shard, llam_task_t *task);
void llam_timer_node_free(llam_shard_t *shard, llam_timer_node_t *node);
bool llam_timer_remove_locked(llam_shard_t *shard, llam_task_t *task);

#endif
