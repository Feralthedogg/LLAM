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
 * See LICENSES/OLD-LICENSE/Apache-2.0.txt.
 */

#ifndef LLAM_RUNTIME_PROTO_SCHED_H
#define LLAM_RUNTIME_PROTO_SCHED_H

#include "runtime_state.h"

/* Numeric values are available in production for the no-op pause calls. */
#define LLAM_LEASE_TEST_POINT_AFTER_TRANSFER_REQUEST 1U
#define LLAM_LEASE_TEST_POINT_AFTER_OWNER_CLEAR 2U
#define LLAM_LEASE_TEST_POINT_AFTER_GRANT 3U
#define LLAM_LEASE_TEST_POINT_AFTER_HANDBACK_ACK 4U
#define LLAM_LEASE_TEST_POINT_BEFORE_WATCHDOG_SIGNAL 5U
#define LLAM_LEASE_TEST_POINT_AFTER_HELPER_EXIT_ACK 6U
#define LLAM_LEASE_TEST_POINT_AFTER_TASK_STATE_PREPARE 7U
#define LLAM_LEASE_TEST_POINT_AFTER_ACTIVE_G_PUBLISH 8U
#define LLAM_LEASE_TEST_POINT_AFTER_DIRECT_YIELD 9U
#define LLAM_LEASE_TEST_POINT_AFTER_DIRECT_REINJECT 10U
#define LLAM_LEASE_TEST_POINT_AFTER_IO_PARK 11U
#define LLAM_LEASE_TEST_POINT_AFTER_SLEEP_PARK 12U
#define LLAM_LEASE_TEST_POINT_AFTER_TASK_EXIT_CLEAR 13U
#define LLAM_LEASE_TEST_POINT_COUNT 14U

/*
 * Legacy execution-lease and worker binding lifecycle. Every `_locked`
 * transition below requires `shard->opaque_lock`; when shard state is also
 * needed, callers acquire `shard->lock` only after that handoff lock.
 */
void llam_legacy_lease_init(llam_shard_t *shard);
void llam_legacy_lease_begin_stop_locked(llam_shard_t *shard);
void llam_legacy_lease_finalize_stop_locked(llam_shard_t *shard);
bool llam_legacy_lease_request_helper_locked(llam_shard_t *shard,
                                             llam_worker_t *primary,
                                             uint64_t *sequence_out);
bool llam_legacy_lease_grant_helper_locked(llam_shard_t *shard,
                                           llam_worker_t *helper,
                                           uint64_t sequence);
bool llam_legacy_lease_request_return_locked(llam_shard_t *shard,
                                             llam_worker_t *claimant,
                                             uint64_t claimant_incarnation,
                                             uint64_t *sequence_out);
bool llam_legacy_lease_complete_handback_locked(llam_shard_t *shard,
                                                llam_worker_t *worker,
                                                uint64_t sequence);
bool llam_legacy_lease_enter_redirect_locked(llam_shard_t *shard,
                                             llam_worker_t *return_worker,
                                             uint64_t return_incarnation);
bool llam_legacy_lease_can_reacquire_locked(const llam_shard_t *shard,
                                            const llam_worker_t *worker,
                                            uint64_t incarnation);
bool llam_legacy_lease_handback_acknowledged_locked(const llam_shard_t *shard,
                                                    const llam_worker_t *worker,
                                                    uint64_t incarnation,
                                                    uint64_t sequence);
bool llam_legacy_lease_teardown_return_locked(const llam_shard_t *shard,
                                              const llam_worker_t *worker,
                                              uint64_t incarnation);
bool llam_legacy_lease_clear_owner_locked(llam_shard_t *shard,
                                          llam_worker_t *worker,
                                          uint64_t incarnation,
                                          uint64_t sequence);
bool llam_legacy_lease_rollback_helper_locked(llam_shard_t *shard,
                                              llam_worker_t *primary,
                                              uint64_t incarnation,
                                              uint64_t sequence);
bool llam_worker_binding_live(const llam_worker_t *worker, uint64_t incarnation);
/*
 * Worker-local execution publication.  Active-G is the sole current-task
 * authority; callers publish only after task state, shard association, and
 * timestamps are complete.
 */
void llam_worker_publish_active_g(llam_worker_t *worker, llam_task_t *task);
llam_task_t *llam_worker_load_active_g(const llam_worker_t *worker);
void llam_worker_clear_active_g(llam_worker_t *worker);
/*
 * Return the validated lease owner's active-G.  The caller must hold
 * `shard->opaque_lock` and then `shard->lock`; the shard lock keeps the task
 * object alive while the returned pointer is inspected.  The pointer must not
 * be retained after the shard lock is released.
 */
llam_task_t *llam_shard_load_lease_current(llam_shard_t *shard);
void llam_scheduler_bind_identity(llam_shard_t *shard,
                                  llam_worker_t *worker,
                                  llam_ctx_t *ctx);
void llam_scheduler_unbind_identity(llam_shard_t *shard,
                                    llam_worker_t *worker);
void llam_scheduler_clear_identity(void);
/*
 * Opaque-block redirect and helper compensation.
 */
void llam_activate_opaque_redirect_locked(llam_shard_t *shard, llam_task_t *current_task);
void llam_deactivate_opaque_redirect_locked(llam_shard_t *shard);
bool llam_enqueue_opaque_redirect_task_locked(llam_shard_t *blocked, llam_task_t *task, bool hot);
int llam_ensure_opaque_helper_locked(llam_shard_t *shard);
void *llam_opaque_helper_main(void *arg);
#if defined(LLAM_ENABLE_TEST_HOOKS)
void llam_worker_test_force_helper_create_error(int error_code);

/** @brief Handoff points that a focused lease test may hold. */
typedef enum llam_lease_test_point {
    LLAM_LEASE_TEST_AFTER_TRANSFER_REQUEST = 1,
    LLAM_LEASE_TEST_AFTER_OWNER_CLEAR = 2,
    LLAM_LEASE_TEST_AFTER_GRANT = 3,
    LLAM_LEASE_TEST_AFTER_HANDBACK_ACK = 4,
    LLAM_LEASE_TEST_BEFORE_WATCHDOG_SIGNAL = 5,
    LLAM_LEASE_TEST_AFTER_HELPER_EXIT_ACK = 6,
    LLAM_LEASE_TEST_AFTER_TASK_STATE_PREPARE = 7,
    LLAM_LEASE_TEST_AFTER_ACTIVE_G_PUBLISH = 8,
    LLAM_LEASE_TEST_AFTER_DIRECT_YIELD = 9,
    LLAM_LEASE_TEST_AFTER_DIRECT_REINJECT = 10,
    LLAM_LEASE_TEST_AFTER_IO_PARK = 11,
    LLAM_LEASE_TEST_AFTER_SLEEP_PARK = 12,
    LLAM_LEASE_TEST_AFTER_TASK_EXIT_CLEAR = 13
} llam_lease_test_point_t;

/**
 * @brief Test-only observation of the execution lease boundary.
 *
 * The implementation takes `opaque_lock` before the shard lock and copies the
 * coordinator's lease state without deriving ownership from legacy flags.
 */
typedef struct llam_lease_test_snapshot {
    unsigned state;
    uintptr_t owner_worker;
    uint64_t owner_incarnation;
    uint64_t epoch;
    uintptr_t return_worker;
    uint64_t return_incarnation;
    uint64_t transfer_sequence;
    uint64_t handback_ack_sequence;
} llam_lease_test_snapshot_t;

void llam_lease_test_set_gate(llam_lease_test_point_t point, bool enabled);
/* Gate storage is test-hook-only; production transition calls compile to no-ops. */
bool llam_lease_test_wait_gate(llam_lease_test_point_t point, uint64_t timeout_ns);
/* True after production reached the corresponding gated transition. */
bool llam_lease_test_gate_reached(llam_lease_test_point_t point);
bool llam_legacy_lease_snapshot_locked(const llam_shard_t *shard,
                                       llam_lease_test_snapshot_t *out);
int llam_lease_test_snapshot(llam_runtime_t *runtime,
                             unsigned shard_id,
                             llam_lease_test_snapshot_t *out);
#endif

/**
 * @brief Pause at a lease publication point when test hooks are enabled.
 *
 * The helper is deliberately a no-op in production builds.  Callers invoke it
 * only after releasing scheduler locks so a focused test can inspect the
 * intermediate lease state without blocking a producer or worker.
 */
#if defined(LLAM_ENABLE_TEST_HOOKS)
static inline void llam_legacy_lease_test_pause(llam_lease_test_point_t point) {
    (void)llam_lease_test_wait_gate(point, UINT64_MAX);
}
#else
static inline void llam_legacy_lease_test_pause(unsigned point) {
    (void)point;
}
#endif

/*
 * Worker loops and idle handling.
 */
void *llam_block_worker_main(void *arg);
bool llam_runtime_note_block_pending(llam_runtime_t *rt, unsigned amount);
bool llam_runtime_complete_block_pending(llam_runtime_t *rt, unsigned amount);
bool llam_runtime_note_block_active(llam_runtime_t *rt, unsigned amount);
bool llam_runtime_complete_block_active(llam_runtime_t *rt, unsigned amount);
void llam_autotune_init(llam_runtime_t *rt);
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
bool llam_norm_queue_note_enqueue(llam_shard_t *shard);
bool llam_norm_queue_note_dequeue(llam_shard_t *shard);
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
bool llam_task_cancel_token_is_cancelled(const llam_task_t *task);
bool llam_deadline_passed(uint64_t deadline_ns);
void llam_disarm_task_wait_deadline(llam_task_t *task);
void llam_fire_expired_timers(llam_shard_t *shard);
bool llam_task_wait_deadline_active(llam_task_t *task);
bool llam_join_waiter_remove_locked(llam_task_t *target, llam_task_t *waiter);
void llam_park_current_task(llam_wait_reason_t reason, llam_trace_kind_t kind);
bool llam_task_clear_wait_tracking(llam_task_t *task);
void llam_task_clear_wait_tracking_or_abort(llam_task_t *task);
bool llam_task_wait_resolver_try_begin(llam_task_t *task);
void llam_task_wait_resolver_end(llam_task_t *task);
bool llam_task_close_wait_resolvers(llam_task_t *task);
bool llam_task_publish_wait_tracking(llam_task_t *task);
llam_block_job_t *llam_task_active_block_job_load(const llam_task_t *task);
bool llam_task_set_block_tracking(llam_task_t *task, llam_block_job_t *job, unsigned parked_shard);
bool llam_task_set_join_tracking(llam_task_t *task, llam_task_t *target, unsigned parked_shard);
bool llam_task_set_sleep_tracking(llam_task_t *task,
                                  llam_wait_node_t *node,
                                  unsigned parked_shard);
void llam_timer_insert_locked(llam_shard_t *shard, llam_task_t *task);
void llam_timer_node_free(llam_shard_t *shard, llam_timer_node_t *node);
bool llam_timer_remove_locked(llam_shard_t *shard, llam_task_t *task);

#endif
