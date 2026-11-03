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
 * See LICENSE.
 */

#ifndef LLAM_WINDOWS_ASYNC_LIFETIME_H
#define LLAM_WINDOWS_ASYNC_LIFETIME_H

#include "async_context_phase.h"

/* Private Win64 native-context lifetime and legacy PE-range substrate. */
#if LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)

typedef enum llam_windows_async_bank_phase {
    LLAM_WINDOWS_ASYNC_BANK_FREE = LLAM_ASYNC_BANK_FREE,
    LLAM_WINDOWS_ASYNC_BANK_CAPTURING = LLAM_ASYNC_BANK_CAPTURING,
    LLAM_WINDOWS_ASYNC_BANK_CAPTURED = LLAM_ASYNC_BANK_CAPTURED,
    LLAM_WINDOWS_ASYNC_BANK_READY = LLAM_ASYNC_BANK_READY,
    LLAM_WINDOWS_ASYNC_BANK_GATE = LLAM_ASYNC_BANK_GATE,
    LLAM_WINDOWS_ASYNC_BANK_RESTORING = LLAM_ASYNC_BANK_RESTORING,
    LLAM_WINDOWS_ASYNC_BANK_OBSERVED = LLAM_ASYNC_BANK_OBSERVED
} llam_windows_async_bank_phase_t;

_Static_assert(LLAM_WINDOWS_ASYNC_BANK_GATE == 4,
               "Windows transition assembly publishes phase 4");

typedef struct llam_windows_async_context_buffer {
    void *storage;
    DWORD length;
    PCONTEXT context;
} llam_windows_async_context_buffer_t;

typedef struct llam_windows_async_bank {
    llam_windows_async_context_buffer_t captured;
    llam_windows_async_context_buffer_t redirected;
    atomic_uint phase;
    llam_task_t *owner_task;
    llam_worker_t *capture_worker;
    uint64_t generation;
    uint64_t capture_binding_incarnation;
    uint64_t capture_task_run;
    uint64_t capture_worker_run;
    uint64_t capture_scheduler_rsp;
    uint64_t restore_task_run;
    uint64_t restore_worker_run;
    uint64_t restore_scheduler_rsp;
    llam_processor_owner_ticket_t ticket;
    int captured_errno;
    DWORD captured_last_error;
    atomic_uint arrival_complete;
    unsigned external_ops;
} llam_windows_async_bank_t;

typedef struct llam_windows_async_capsule {
    llam_windows_async_bank_t banks[2];
    uint64_t next_generation;
    size_t quota_bytes;
    bool admission_closed;
    /* Origin publication is release/acquire; queue and epoch mutation use
     * the origin opaque -> shard gate. FREE banks alone do not permit migration. */
    _Atomic(llam_worker_t *) service_worker;
    uint64_t service_incarnation;
    uint64_t service_run;
    uint64_t yield_save_run;
    bool origin_enqueued;
    bool source_return_complete;
    atomic_uint context_kind;
} llam_windows_async_capsule_t;

typedef enum llam_windows_async_operation_kind {
    LLAM_WINDOWS_ASYNC_OP_CAPTURE = 1,
    LLAM_WINDOWS_ASYNC_OP_RESTORE
} llam_windows_async_operation_kind_t;

typedef struct llam_windows_async_operation {
    llam_runtime_t *runtime;
    llam_shard_t *shard;
    llam_worker_t *worker;
    llam_task_t *task;
    llam_windows_async_bank_t *bank;
    HANDLE native_thread;
    uint64_t binding_incarnation;
    uint64_t bank_generation;
    uint64_t range_generation;
    uint64_t capture_task_run;
    uint64_t capture_worker_run;
    uint64_t runtime_run;
    llam_windows_async_operation_kind_t kind;
} llam_windows_async_operation_t;

typedef enum llam_windows_async_capture_result {
    LLAM_WINDOWS_ASYNC_CAPTURE_NOT_ELIGIBLE = 0,
    LLAM_WINDOWS_ASYNC_CAPTURED = 1,
    LLAM_WINDOWS_ASYNC_CAPTURE_RETRY_LATER = 2
} llam_windows_async_capture_result_t;

typedef struct llam_windows_async_stats {
    uint64_t capture_attempts;
    uint64_t captures;
    uint64_t unsafe_pc_refusals;
    uint64_t restores;
    uint64_t direct_witnesses;
    uint64_t binding_ops;
    uint64_t captured_bank_active;
    uint64_t pending_restore_active;
    uint64_t execution_debt;
    uint64_t retire_count;
    uint64_t finalizer_count;
    uint64_t remote_transactions_max;
    uint64_t scheduler_error_checks;
    uint64_t native_handles_created;
    uint64_t native_handles_closed;
} llam_windows_async_stats_t;

/* Private PE collector result. The caller must zero-initialize it and keep
 * its held module reference until public enable has copied/pinned the ranges. */
typedef struct llam_windows_async_pe_range_set {
    HMODULE held_module;
    llam_windows_async_preempt_range_t *ranges;
    size_t range_count;
} llam_windows_async_pe_range_set_t;

#define LLAM_WINDOWS_ASYNC_PE_METADATA_ABSENT 1
int llam_windows_async_pe_collect_ranges(
    HMODULE caller_held_module,
    llam_windows_async_pe_range_set_t *out);
void llam_windows_async_pe_release_ranges(
    llam_windows_async_pe_range_set_t *collected);

void llam_windows_async_restore_scheduler(void);
extern const unsigned char llam_windows_async_restore_scheduler_end[];
void llam_windows_async_save_gate(llam_ctx_t *scheduler_ctx,
                                  atomic_uint *phase);
extern const unsigned char llam_windows_async_gate_spin_begin[];
extern const unsigned char llam_windows_async_gate_spin_end[];

uint64_t llam_windows_async_effective_quantum(const llam_runtime_t *runtime);
bool llam_windows_async_task_quiescent(const llam_task_t *task);
int llam_windows_async_prepare_registered(llam_runtime_t *runtime);
bool llam_windows_async_memory_info(const llam_runtime_t *runtime, uint64_t *charged, uint64_t *per_task);
int llam_windows_async_lifetime_enable(llam_runtime_t *runtime);
int llam_windows_async_lifetime_enable_certified_ranges(
    llam_runtime_t *runtime,
    const llam_windows_async_preempt_range_t *ranges,
    size_t range_count);
int llam_windows_async_lifetime_register_safe_range(llam_runtime_t *runtime,
                                                    uintptr_t begin,
                                                    uintptr_t end);
bool llam_windows_async_lifetime_pc_allowed(const llam_windows_async_operation_t *op,
                                            uintptr_t rip);
llam_windows_async_capture_result_t
llam_windows_async_lifetime_try_capture_primary(llam_runtime_t *runtime,
                                                llam_windows_async_operation_t *op);
bool llam_windows_async_lifetime_try_restore(llam_windows_async_operation_t *op);
bool llam_windows_async_lifetime_try_observe_running(
    llam_windows_async_operation_t *op);
bool llam_windows_async_lifetime_stats(const llam_runtime_t *runtime,
                                      llam_windows_async_stats_t *out);
int llam_windows_async_lifetime_run_begin(llam_runtime_t *runtime);
int llam_windows_async_lifetime_register_worker(llam_shard_t *shard);
int llam_windows_async_lifetime_run_commit(llam_runtime_t *runtime);
void llam_windows_async_lifetime_run_abort(llam_runtime_t *runtime, int error);
int llam_windows_async_lifetime_wait_start(llam_shard_t *shard);
bool llam_windows_async_lifetime_controller_tick(llam_runtime_t *runtime,
                                                 uint64_t now_ns);
bool llam_windows_async_lifetime_slot_pending(llam_runtime_t *runtime,
                                              unsigned shard_id);
void llam_windows_async_lifetime_prepare_yield(llam_task_t *task);
void llam_windows_async_lifetime_cooperative_handoff(llam_shard_t *shard,
                                                     llam_task_t *task);
void llam_windows_async_lifetime_run_end(llam_runtime_t *runtime);
void llam_windows_async_lifetime_shutdown(llam_runtime_t *runtime);
bool llam_windows_async_lifetime_close_binding(llam_shard_t *shard);
int llam_windows_async_lifetime_task_init(llam_task_t *task);
void llam_windows_async_lifetime_task_destroy(llam_task_t *task);
void llam_windows_async_lifetime_first_dispatch(llam_worker_t *worker,
                                                llam_task_t *task);
bool llam_windows_async_lifetime_op_begin(llam_runtime_t *runtime,
                                          llam_windows_async_operation_t *op);
bool llam_windows_async_lifetime_op_capture_committed(llam_windows_async_operation_t *op);
bool llam_windows_async_lifetime_op_restoring(llam_windows_async_operation_t *op,
                                              uint64_t task_run,
                                              uint64_t worker_run);
void llam_windows_async_lifetime_op_abort(llam_windows_async_operation_t *op);
void llam_windows_async_lifetime_op_finish(llam_windows_async_operation_t *op);
bool llam_windows_async_lifetime_arrival(llam_task_t *task);
bool llam_windows_async_lifetime_resume_task(llam_ctx_t *scheduler_ctx,
                                             llam_task_t *task);
void llam_windows_async_lifetime_scheduler_return(llam_shard_t *shard,
                                                  llam_task_t *task);
bool llam_windows_async_lifetime_try_retire_dead(llam_shard_t *shard,
                                                 llam_task_t *task);
void llam_windows_async_lifetime_service_retirements(llam_shard_t *shard);
void llam_windows_async_lifetime_defer_claimed(llam_runtime_t *runtime,
                                               llam_task_t *task);
void llam_windows_async_lifetime_service_finalizers(llam_shard_t *shard);
bool llam_task_try_finalize_deferred_claim(llam_runtime_t *runtime,
                                           llam_task_t *task);
bool llam_windows_async_lifetime_has_debt(const llam_runtime_t *runtime);
uint64_t llam_windows_async_lifetime_charged_bytes(const llam_runtime_t *runtime);
#if defined(LLAM_ENABLE_TEST_HOOKS)
int llam_windows_async_test_capture_admission(llam_task_t *task);
/* Two complete runs share this immutable log; repeat fixtures can perform
 * additional finite batches while waiting for real per-M chain coverage. */
#define LLAM_WINDOWS_ASYNC_TEST_TRACE_CAPACITY 256U
typedef enum llam_windows_async_test_event_kind {
    LLAM_WINDOWS_ASYNC_TEST_CAPTURE = 1,
    LLAM_WINDOWS_ASYNC_TEST_RESTORE = 2,
    LLAM_WINDOWS_ASYNC_TEST_WITNESS = 3,
    LLAM_WINDOWS_ASYNC_TEST_PEER = 4,
    LLAM_WINDOWS_ASYNC_TEST_RETURN = 5
} llam_windows_async_test_event_kind_t;
typedef struct llam_windows_async_test_event {
    unsigned kind;
    uintptr_t task_key;
    uintptr_t bank_key;
    uintptr_t captured_pc;
    uint64_t bank_generation;
    uint64_t capture_task_run;
    uint64_t capture_worker_run;
    uint64_t restore_task_run;
    uint64_t restore_worker_run;
    uintptr_t worker_key;
    uint64_t binding_incarnation;
    uint64_t runtime_run;
    unsigned shard_id;
    DWORD native_thread_id;
} llam_windows_async_test_event_t;
_Static_assert(LLAM_WINDOWS_ASYNC_TEST_TRACE_CAPACITY *
                   sizeof(llam_windows_async_test_event_t) <= 32U * 1024U,
               "test trace snapshots must fit within half a default G stack");
/* A bounded, allocation-free observation log. Call only while the runtime is
 * alive. The returned prefix is immutable and publication-acquire complete. */
size_t llam_windows_async_lifetime_test_trace(
    const llam_runtime_t *runtime,
    llam_windows_async_test_event_t *events,
    size_t capacity);
bool llam_windows_async_lifetime_test_trace_overflow(
    const llam_runtime_t *runtime);
void llam_windows_async_lifetime_test_note_task(llam_runtime_t *runtime,
                                                unsigned kind,
                                                uint64_t bank_generation);
void llam_windows_async_test_ctrl_entry_gate(atomic_uint *observed,
                                            atomic_uint *release,
                                            atomic_uint *ticks);
typedef enum llam_windows_async_test_phase {
    LLAM_WINDOWS_ASYNC_TEST_READY = 1,
    LLAM_WINDOWS_ASYNC_TEST_BEFORE_RESTORE = 2,
    LLAM_WINDOWS_ASYNC_TEST_AFTER_RESTORE = 3,
    LLAM_WINDOWS_ASYNC_TEST_COOPERATIVE_RELEASED = 4,
    LLAM_WINDOWS_ASYNC_TEST_BEFORE_OP_FINISH = 5,
    LLAM_WINDOWS_ASYNC_TEST_COOPERATIVE_SAVED = 6,
    LLAM_WINDOWS_ASYNC_TEST_BEFORE_SCAN_RELEASE = 7,
    LLAM_WINDOWS_ASYNC_TEST_AFTER_BANK_FREE = 8,
    LLAM_WINDOWS_ASYNC_TEST_BEFORE_CAPTURE_HANDOFF = 9
} llam_windows_async_test_phase_t;
typedef enum llam_windows_async_test_failure {
    LLAM_WINDOWS_ASYNC_TEST_FAIL_SLOT_ALLOC = 1,
    LLAM_WINDOWS_ASYNC_TEST_FAIL_THREAD_START,
    LLAM_WINDOWS_ASYNC_TEST_FAIL_DUPLICATE,
    LLAM_WINDOWS_ASYNC_TEST_FAIL_QUALIFICATION
} llam_windows_async_test_failure_t;
void llam_windows_async_lifetime_test_fail_worker(unsigned kind, unsigned index);
bool llam_windows_async_lifetime_test_take_worker_failure(unsigned kind, unsigned index);
void llam_windows_async_lifetime_test_error_sentinels(llam_runtime_t *runtime);
void llam_windows_async_lifetime_test_scheduler_error(llam_task_t *task, bool before);
bool llam_windows_async_lifetime_test_operation_matches(const llam_windows_async_operation_t *op);
bool llam_windows_async_lifetime_test_copy_operation(llam_runtime_t *runtime, unsigned id,
                                                     llam_windows_async_operation_t *out);
void llam_windows_async_lifetime_test_tail_gate(llam_runtime_t *runtime,
                                                atomic_uint *observed,
                                                atomic_uint *release);
void llam_windows_async_lifetime_test_start_gate(llam_runtime_t *runtime,
                                                 unsigned id,
                                                 atomic_uint *observed,
                                                 atomic_uint *release);
void llam_windows_async_lifetime_test_phase_gate(llam_task_t *task,
                                                unsigned phase,
                                                atomic_uint *observed,
                                                atomic_uint *release);
void llam_windows_async_test_host_spawn_gate(llam_runtime_t *runtime,
                                            atomic_uint *observed,
                                            atomic_uint *release);
void llam_windows_async_lifetime_test_enable_gate(llam_runtime_t *runtime,
                                                 atomic_uint *observed,
                                                 atomic_uint *release);
void llam_windows_async_lifetime_test_ready_gate(llam_task_t *task,
                                                 atomic_uint *observed,
                                                 atomic_uint *release);
#endif

#endif
#endif
