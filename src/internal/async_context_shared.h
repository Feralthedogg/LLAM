/*
 * Copyright 2026 Feralthedogg
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

#ifndef LLAM_ASYNC_CONTEXT_SHARED_H
#define LLAM_ASYNC_CONTEXT_SHARED_H

#if LLAM_ASYNC_CONTEXT_SHARED
/* Cold native operations, never called through this table from a signal
 * handler. Native storage is opaque and allocated before any interruption. */
typedef struct llam_async_native_ops {
    size_t binding_size;
    size_t context_size;
    int (*bind_current)(void *binding);
    void (*close)(void *binding);
    int (*suspend)(void *binding);
    int (*read)(void *binding, void *context);
    int (*redirect)(void *binding, const void *context, const llam_ctx_t *scheduler,
                    uintptr_t begin, uintptr_t end, uintptr_t low, uintptr_t high);
    int (*restore)(void *binding, const void *context);
    void (*resume)(void *binding);
    uintptr_t (*pc)(const void *context);
    uintptr_t (*sp)(const void *context);
    bool (*at_gate)(const void *context);
    void (*save_gate)(llam_ctx_t *scheduler, atomic_uint *phase);
    bool signal_mailbox; /* Separate signal frontend, never suspend emulation. */
    /* Read-only return witness: no restorable snapshot or ISA admission. */
    int (*observe_sp)(void *binding, uintptr_t *sp);
} llam_async_native_ops_t;

extern const llam_async_native_ops_t g_llam_darwin_async_ops;

typedef struct llam_async_shared_stats {
    uint64_t requests;
    uint64_t captures;
    uint64_t restores;
    uint64_t witnesses;
    uint64_t retire_depth;
    uint64_t finalizer_depth;
    uint64_t execution_debt;
    uint64_t binding_ops;
    uint64_t charged_bytes;
    uint64_t bindings_created;
    uint64_t bindings_closed;
    uint64_t scheduler_error_checks;
} llam_async_shared_stats_t;

/* Private qualification entry: certified intervals must remain immutable and
 * executable until destroy; ALL code on this M obeys the native ISA profile.
 * Acknowledgement covers hosting-process fail-fast on ambiguous native state.
 * Call before spawning tasks, without concurrent config/spawn/run operations.
 * Legacy integration remains deterministic one-primary. Registered execution
 * prepares every selected M and retains each full continuation on its origin. */
/* Signal profile additionally reserves one RT signal, requires static-TLS module
 * availability, and forbids G changes to native TLS base, vector length or the
 * owned alternate stack. Keep the reserved signal unblocked in qualified code;
 * an incompatible G mask is never silently changed to claim async support.
 * Linux AArch64 additionally requires unchanged native-thread speculation
 * policy across LLAM, all G/peer/callback/signal/external code while any saved
 * continuation exists; a certified leaf alone does not establish that invariant. */
int llam_async_shared_enable(llam_runtime_t *runtime,
                              const llam_async_preempt_range_t *ranges,
                              size_t count, bool process_failfast_ack);
int llam_async_shared_prepare_registered(llam_runtime_t *runtime);
uint64_t llam_async_shared_effective_quantum(const llam_runtime_t *runtime);
int llam_async_shared_run_begin(llam_runtime_t *runtime);
int llam_async_shared_driver_ready(llam_runtime_t *runtime);
int llam_async_shared_run_commit(llam_runtime_t *runtime);
void llam_async_shared_run_abort(llam_runtime_t *runtime, int error);
bool llam_async_shared_driver_close(llam_runtime_t *runtime);
void llam_async_shared_run_end(llam_runtime_t *runtime);
void llam_async_shared_shutdown(llam_runtime_t *runtime);
int llam_async_shared_task_init(llam_task_t *task);
void llam_async_shared_task_destroy(llam_task_t *task);
bool llam_async_shared_task_quiescent(const llam_task_t *task);
void llam_async_shared_switch_scheduler_to_task(llam_ctx_t *scheduler,
                                                 llam_task_t *task, int scheduler_errno);
#if LLAM_PLATFORM_DARWIN
int llam_async_shared_block_native_signals(llam_worker_t *worker, sigset_t *previous);
void llam_async_shared_rearm_native_signals(llam_worker_t *worker);
#endif
void llam_async_shared_scheduler_return(llam_shard_t *shard, llam_task_t *task);
void llam_async_shared_retire_dead(llam_shard_t *shard, llam_task_t *task);
void llam_async_shared_service_retirements(llam_shard_t *shard);
bool llam_async_shared_primary_service(llam_shard_t *shard);
bool llam_async_shared_defer_claimed(llam_runtime_t *runtime, llam_task_t *task);
bool llam_async_shared_has_debt(const llam_runtime_t *runtime);
bool llam_async_shared_controller_tick(llam_runtime_t *runtime, uint64_t now_ns);
/* Advisory cadence only; all native service still validates its pinned tuple. */
bool llam_async_shared_controller_urgent(const llam_runtime_t *runtime);
bool llam_async_shared_controller_wait(const llam_runtime_t *runtime, uint64_t interval_ns);
bool llam_async_shared_worker_stats(const llam_worker_t *worker, llam_async_shared_stats_t *out);
bool llam_async_shared_memory_info(const llam_runtime_t *runtime, uint64_t *charged, uint64_t *per_task);
bool llam_async_shared_stats(const llam_runtime_t *runtime, llam_async_shared_stats_t *out);
/* Controller only, with the runtime's P lock pairs held; no locks or native calls. */
bool llam_async_shared_liveness_quiet_pair_locked(const llam_runtime_t *runtime);
bool llam_task_try_finalize_deferred_claim(llam_runtime_t *runtime, llam_task_t *task);

#if defined(LLAM_ENABLE_TEST_HOOKS) || defined(LLAM_ENABLE_ASYNC_PROFILE)
enum {
    LLAM_ASYNC_SHARED_TEST_CAPTURE = 1,
    LLAM_ASYNC_SHARED_TEST_READY,
    LLAM_ASYNC_SHARED_TEST_CAPTURE_END,
    LLAM_ASYNC_SHARED_TEST_RESTORE,
    LLAM_ASYNC_SHARED_TEST_LANDING
};
typedef struct llam_async_shared_event {
    unsigned kind;
    uint64_t task_id;
    uint64_t bank_generation;
    uint64_t runtime_run;
    uint64_t pstate_before;
    uint64_t pstate_after;
    uint64_t requested_ns;
    uint64_t observed_ns;
} llam_async_shared_event_t;
/* Bounded immutable prefix of the most recent run, reset before native bind. */
size_t llam_async_shared_profile_events(const llam_runtime_t *runtime,
                                        llam_async_shared_event_t *events, size_t capacity);
#endif
#if defined(LLAM_ENABLE_TEST_HOOKS)
int llam_async_shared_test_capture_admission(llam_task_t *task);
enum {
    LLAM_ASYNC_SHARED_TEST_CAPTURE_RETURN = 0,
    LLAM_ASYNC_SHARED_TEST_READY_HANDOFF,
    LLAM_ASYNC_SHARED_TEST_RESTORE_RETURN,
    LLAM_ASYNC_SHARED_TEST_TASK_READER,
    LLAM_ASYNC_SHARED_TEST_WITNESS_RETURN,
    LLAM_ASYNC_SHARED_TEST_SIGNAL_CAPTURE_ARMED,
    LLAM_ASYNC_SHARED_TEST_SIGNAL_WITNESS_ARMED,
    LLAM_ASYNC_SHARED_TEST_BEFORE_PROBE,
    LLAM_ASYNC_SHARED_TEST_POINT_COUNT
};
typedef struct llam_async_shared_checkpoint {
    unsigned capture_ops;
    unsigned restore_ops;
    unsigned ready_depth;
    unsigned pending_phase;
    unsigned capture_phase;
    unsigned mailbox_state;
    unsigned mailbox_kind;
    unsigned mailbox_cancellations;
    unsigned probe_phase;
} llam_async_shared_checkpoint_t;
size_t llam_async_shared_test_events(const llam_runtime_t *runtime,
                                      llam_async_shared_event_t *events, size_t capacity);
void llam_async_shared_test_scheduler_error(const llam_task_t *task, int expected);
void llam_async_shared_test_hold(llam_runtime_t *runtime, unsigned point,
                                  atomic_uint *observed, atomic_uint *release);
bool llam_async_shared_test_checkpoint(const llam_runtime_t *runtime,
                                        llam_async_shared_checkpoint_t *out);
#if LLAM_ASYNC_CONTEXT_SIGNAL
void llam_async_shared_test_signal_hold(llam_runtime_t *runtime, unsigned kind,
                                       atomic_uint *observed, atomic_uint *release);
#endif
#endif
#endif
#endif
