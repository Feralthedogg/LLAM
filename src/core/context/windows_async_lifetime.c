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

/*
 * Win64 remote-context lifetime backend. Legacy expert runtimes retain their
 * fixed primary bindings and pinned range table; managed runtimes use LLPM2
 * admission and registered M/P leases. Bank and binding mutation uses ordered
 * service gates, all released before the controller suspends a native M.
 */
#include "runtime_internal.h"
#include "capsule_pool.h"
#include "windows_async_lifetime.h"
#include "managed_scheduler.h"
#include "managed_catalog.h"
#include "metadata_v2.h"
#include "engine/runtime_watchdog_internal.h"

#define LLAM_WINDOWS_ASYNC_CONTEXT_FLAGS (CONTEXT_ALL | CONTEXT_XSTATE)
#define LLAM_WINDOWS_ASYNC_SUPPORTED_XSTATE UINT64_C(0x807)
#define LLAM_WINDOWS_ASYNC_CAPSULE_BUDGET UINT64_C(67108864)

/*
 * Once remote context or suspension ownership is uncertain, neither normal
 * cancellation nor runtime teardown can prove a safe next M/G context. This
 * private experimental contract terminates the *hosting process*; it never
 * fabricates reference drain or resumes a possibly mutated native thread.
 */
static LLAM_NORETURN void llam_windows_async_uncertain_transaction(void) {
    RaiseFailFastException(NULL, NULL, FAIL_FAST_GENERATE_EXCEPTION_ADDRESS);
    __builtin_unreachable();
}

enum {
    LLAM_WINDOWS_ASYNC_START_CLOSED = 0,
    LLAM_WINDOWS_ASYNC_START_PREPARING,
    LLAM_WINDOWS_ASYNC_START_OPEN,
    LLAM_WINDOWS_ASYNC_START_ABORTED
};
enum {
    LLAM_WINDOWS_ASYNC_CONTEXT_COOPERATIVE = 0,
    LLAM_WINDOWS_ASYNC_CONTEXT_REMOTE
};

typedef struct llam_windows_async_binding {
    llam_worker_t *worker;
    llam_ctx_t *scheduler;
    unsigned index;
    atomic_uint probe_phase;
    atomic_int probe_error;
    atomic_bool restart_pending;
    atomic_uint execution_debt;
    atomic_uint retire_count;
    atomic_uint finalizer_count;
    llam_task_t *retire_head;
    llam_task_t *finalizer_head;
    llam_task_t *terminal_task;
    HANDLE native_thread;
    DWORD native_thread_id;
    uintptr_t native_stack_low;
    uintptr_t native_stack_high;
    uint64_t binding_incarnation;
    uint64_t runtime_run;
    atomic_uint binding_ops;
    _Atomic(llam_windows_async_bank_t *) captured_bank;
    _Atomic(llam_windows_async_bank_t *) pending_restore;
    atomic_uint ready_depth;
    llam_task_t *ready_head;
    llam_task_t *ready_tail;
    bool prefer_origin;
    bool binding_open;
    /* Runtime-owned, not a watchdog stack array or a shallow CONTEXT copy. */
    llam_windows_async_operation_t operation;
    llam_windows_async_context_buffer_t qualification;
    uint64_t next_probe_ns;
#if defined(LLAM_ENABLE_TEST_HOOKS)
    uintptr_t test_errno_address;
    atomic_uint test_ready_contexts; /* Observation only, never a lifetime pin. */
#endif
} llam_windows_async_binding_t;

typedef struct llam_windows_async_control {
    bool managed;
    DWORD context_length;
    llam_windows_async_binding_t *bindings;
    unsigned binding_count;
    unsigned controller_cursor;
    uint64_t run_generation;
    atomic_uint start_phase;
    atomic_uint registered;
    atomic_int start_error;
    atomic_uint controller_busy;
    atomic_uint remote_transactions;
    atomic_uint remote_transactions_max;
    atomic_uint_fast64_t native_handles_created;
    atomic_uint_fast64_t native_handles_closed;
    DWORD64 xstate_features;
    const llam_async_preempt_range_t *safe_ranges;
    HMODULE *safe_range_modules;
    size_t safe_range_count;
    uint64_t safe_range_generation;
    atomic_uint_fast64_t charged_bytes;
    llam_capsule_pool_t capsule_pool;
    atomic_uint_fast64_t capture_attempts;
    atomic_uint_fast64_t captures;
    atomic_uint_fast64_t unsafe_pc_refusals;
    atomic_uint_fast64_t restores;
    atomic_uint_fast64_t direct_witnesses;
#if defined(LLAM_ENABLE_TEST_HOOKS)
    atomic_uint test_error_sentinels;
    atomic_uint_fast64_t test_error_checks;
    atomic_uint test_trace_next;
    atomic_uint test_trace_overflow;
    atomic_uint test_trace_ready[LLAM_WINDOWS_ASYNC_TEST_TRACE_CAPACITY];
    llam_windows_async_test_event_t
        test_trace[LLAM_WINDOWS_ASYNC_TEST_TRACE_CAPACITY];
#endif
} llam_windows_async_control_t;

static llam_windows_async_binding_t *llam_windows_async_binding(
    llam_windows_async_control_t *control, unsigned id) {
    return control != NULL && id < control->binding_count
               ? &control->bindings[id] : NULL;
}

static llam_windows_async_control_t *llam_windows_async_control(llam_runtime_t *rt);
static const llam_managed_scheduler_ops_t g_windows_managed_ops;

static llam_windows_async_binding_t *llam_windows_async_worker_binding(
    llam_windows_async_control_t *control, const llam_worker_t *worker) {
    if (control == NULL || worker == NULL) return NULL;
    if (!control->managed) {
        llam_shard_t *home = worker->compat_shard;
        return home != NULL && worker == &home->primary_worker
            ? llam_windows_async_binding(control, home->id) : NULL;
    }
    size_t embedded = (size_t)worker->registry_runtime->active_shards * 2U;
    size_t index = worker->registry_slot < embedded ? worker->registry_slot / 2U
        : worker->registry_runtime->active_shards + worker->registry_slot - embedded;
    return index < control->binding_count && control->bindings[index].worker == worker
        ? &control->bindings[index] : NULL;
}

static llam_windows_async_binding_t *llam_windows_async_task_binding(
    llam_windows_async_control_t *control, const llam_task_t *task) {
    const llam_windows_async_capsule_t *capsule = task != NULL ? task->async_preempt_task : NULL;
    return capsule != NULL ? llam_windows_async_worker_binding(control,
        atomic_load_explicit(&capsule->service_worker, memory_order_acquire)) : NULL;
}

/* Registered service belongs to M even without P. Take every P's lease gate
 * before any payload gate; native suspension occurs only after releasing all.
 * Legacy callers retain their existing one-P lock order. */
static void llam_windows_async_lock(llam_shard_t *shard) {
    llam_runtime_t *rt = shard->runtime;
    if (rt->execution_lease_mode == LLAM_EXECUTION_LEASE_REGISTERED) {
        for (unsigned i = 0U; i < rt->active_shards; ++i)
            pthread_mutex_lock(&rt->shards[i].opaque_lock);
        for (unsigned i = 0U; i < rt->active_shards; ++i)
            pthread_mutex_lock(&rt->shards[i].lock);
    } else {
        pthread_mutex_lock(&shard->opaque_lock);
        pthread_mutex_lock(&shard->lock);
    }
}

static void llam_windows_async_unlock(llam_shard_t *shard) {
    llam_runtime_t *rt = shard->runtime;
    if (rt->execution_lease_mode == LLAM_EXECUTION_LEASE_REGISTERED) {
        for (unsigned i = rt->active_shards; i != 0U; --i)
            pthread_mutex_unlock(&rt->shards[i - 1U].lock);
        for (unsigned i = rt->active_shards; i != 0U; --i)
            pthread_mutex_unlock(&rt->shards[i - 1U].opaque_lock);
    } else {
        pthread_mutex_unlock(&shard->lock);
        pthread_mutex_unlock(&shard->opaque_lock);
    }
}

static llam_shard_t *llam_windows_async_worker_shard(llam_worker_t *worker) {
    llam_processor_t *processor = atomic_load_explicit(&worker->current_p, memory_order_acquire);
    return processor != NULL ? processor->compat_shard : worker->compat_shard;
}

static bool llam_windows_async_service_ticket(llam_windows_async_binding_t *binding,
                                        llam_processor_owner_ticket_t *ticket) {
    llam_shard_t *shard = llam_windows_async_worker_shard(binding->worker);
    llam_worker_t *worker = binding->worker;
    llam_processor_owner_ticket_t current;
    llam_processor_owner_record_t record;
    const llam_legacy_lease_t *lease = llam_shard_legacy_lease_const_locked(shard);

    llam_windows_async_control_t *control = llam_windows_async_control(binding->worker->registry_runtime);
    if (control == NULL || !binding->binding_open ||
        atomic_load_explicit(&control->start_phase, memory_order_acquire) != LLAM_WINDOWS_ASYNC_START_OPEN ||
        !llam_worker_binding_live(worker, binding->binding_incarnation)) return false;
    if (llam_processor_owner_ticket_snapshot_pair_locked(shard, &current)) {
        if (current.owner_m != worker || current.worker_incarnation != binding->binding_incarnation)
            return false;
        if (ticket != NULL) *ticket = current;
        return true;
    }
    /* Normal execute snapshots reject STOPPING and the shutdown revoke gap.
     * This separate exact drain tuple services previously admitted G roots;
     * it never revives a revoked ordinary ticket or mixes lease protocols. */
    if (!atomic_load_explicit(&binding->worker->registry_runtime->shutdown_requested, memory_order_acquire) ||
        !llam_processor_online(&shard->processor) ||
        !llam_execution_lease_state_matches(binding->worker->registry_runtime, lease->state) ||
        !llam_processor_owner_snapshot_pair_locked(&shard->processor, &record) ||
        !llam_processor_owner_record_well_formed(&record) ||
        record.state != LLAM_PROCESSOR_OWNER_STOPPING || record.lease_state != LLAM_LEASE_STOPPING ||
        lease->state != LLAM_LEASE_STOPPING || record.owner_m != worker || lease->owner != worker ||
        atomic_load_explicit(&worker->current_p, memory_order_acquire) != &shard->processor ||
        record.worker_incarnation != binding->binding_incarnation ||
        lease->owner_incarnation != binding->binding_incarnation ||
        record.lease_projection_epoch != lease->epoch ||
        record.p_generation == 0U || record.p_generation == UINT64_MAX) return false;
    if (ticket != NULL) {
        *ticket = (llam_processor_owner_ticket_t){
            .processor = &shard->processor, .owner_m = worker,
            .worker_incarnation = record.worker_incarnation,
            .lease_projection_epoch = record.lease_projection_epoch,
            .p_generation = record.p_generation,
            .lease_state = record.lease_state, .state = record.state
        };
    }
    return true;
}

static bool llam_windows_async_capsule_quiescent(const llam_windows_async_capsule_t *capsule) {
    if (capsule == NULL || capsule->origin_enqueued ||
        atomic_load_explicit(&capsule->context_kind, memory_order_acquire) != LLAM_WINDOWS_ASYNC_CONTEXT_COOPERATIVE)
        return false;
    for (unsigned i = 0U; i < 2U; ++i)
        if (atomic_load_explicit(&capsule->banks[i].phase, memory_order_acquire) != LLAM_WINDOWS_ASYNC_BANK_FREE ||
            capsule->banks[i].external_ops != 0U) return false;
    return true;
}

static bool llam_windows_async_binding_native_idle(const llam_windows_async_binding_t *binding) {
    return atomic_load_explicit(&binding->retire_count, memory_order_acquire) == 0U &&
        atomic_load_explicit(&binding->finalizer_count, memory_order_acquire) == 0U &&
        atomic_load_explicit(&binding->ready_depth, memory_order_acquire) == 0U &&
        atomic_load_explicit(&binding->binding_ops, memory_order_acquire) == 0U &&
        atomic_load_explicit(&binding->captured_bank, memory_order_acquire) == NULL &&
        atomic_load_explicit(&binding->pending_restore, memory_order_acquire) == NULL;
}

static bool llam_windows_async_terminal_drained(const llam_windows_async_binding_t *binding) {
    const llam_task_t *task = binding->terminal_task;
    return task != NULL && task->managed_execution_class == LLAM_MANAGED_EXECUTION_NATIVE_AFFINE &&
        atomic_load_explicit(&task->state, memory_order_acquire) == LLAM_TASK_STATE_DEAD &&
        atomic_load_explicit(&binding->worker->current_p, memory_order_acquire) == NULL &&
        llam_worker_load_active_g(binding->worker) == NULL &&
        atomic_load_explicit(&binding->execution_debt, memory_order_acquire) == 1U &&
        llam_windows_async_binding_native_idle(binding) &&
        llam_windows_async_capsule_quiescent(task->async_preempt_task);
}

static void llam_windows_async_close_native_handle(llam_windows_async_control_t *control,
                                                    HANDLE handle) {
    if (handle == NULL || !CloseHandle(handle)) {
        abort(); /* Exact owned handle: never claim successful release on error. */
    }
    atomic_fetch_add_explicit(&control->native_handles_closed, 1U, memory_order_release);
}

/* Host driver only, with no active G. This short run-transition gate prevents
 * a last controller sweep from racing next-generation slot initialization.
 * The controller never waits for it or for an operation phase. */
static void llam_windows_async_controller_exclude(llam_windows_async_control_t *control) {
    unsigned expected;
    do {
        expected = 0U;
        if (atomic_compare_exchange_weak_explicit(&control->controller_busy,
                &expected, 1U, memory_order_acquire, memory_order_relaxed)) {
            return;
        }
        Sleep(1U);
    } while (true);
}

#if defined(LLAM_ENABLE_TEST_HOOKS)
static atomic_uint g_windows_async_failure_kind = ATOMIC_VAR_INIT(0U);
static atomic_uint g_windows_async_failure_index = ATOMIC_VAR_INIT(UINT_MAX);
static _Atomic(llam_runtime_t *) g_windows_async_tail_runtime = ATOMIC_VAR_INIT(NULL);
static _Atomic(atomic_uint *) g_windows_async_tail_observed = ATOMIC_VAR_INIT(NULL);
static _Atomic(atomic_uint *) g_windows_async_tail_release = ATOMIC_VAR_INIT(NULL);
static _Atomic(llam_runtime_t *) g_windows_async_start_runtime = ATOMIC_VAR_INIT(NULL);
static atomic_uint g_windows_async_start_id = ATOMIC_VAR_INIT(UINT_MAX);
static _Atomic(atomic_uint *) g_windows_async_start_observed = ATOMIC_VAR_INIT(NULL);
static _Atomic(atomic_uint *) g_windows_async_start_release = ATOMIC_VAR_INIT(NULL);

void llam_windows_async_lifetime_test_start_gate(llam_runtime_t *rt, unsigned id,
                                                 atomic_uint *observed, atomic_uint *release) {
    atomic_store_explicit(&g_windows_async_start_id, id, memory_order_relaxed);
    atomic_store_explicit(&g_windows_async_start_observed, observed, memory_order_release);
    atomic_store_explicit(&g_windows_async_start_release, release, memory_order_release);
    atomic_store_explicit(&g_windows_async_start_runtime, rt, memory_order_release);
}

static void llam_windows_async_test_hold_start(llam_runtime_t *rt, unsigned id) {
    atomic_uint *observed;
    atomic_uint *release;
    if (atomic_load_explicit(&g_windows_async_start_runtime, memory_order_acquire) != rt ||
        atomic_load_explicit(&g_windows_async_start_id, memory_order_relaxed) != id) {
        return;
    }
    observed = atomic_load_explicit(&g_windows_async_start_observed, memory_order_acquire);
    release = atomic_load_explicit(&g_windows_async_start_release, memory_order_acquire);
    if (observed != NULL && release != NULL) {
        atomic_store_explicit(observed, 1U, memory_order_release);
        while (atomic_load_explicit(release, memory_order_acquire) == 0U) { Sleep(1U); }
    }
}

void llam_windows_async_lifetime_test_tail_gate(llam_runtime_t *rt,
                                                atomic_uint *observed,
                                                atomic_uint *release) {
    atomic_store_explicit(&g_windows_async_tail_observed, observed, memory_order_release);
    atomic_store_explicit(&g_windows_async_tail_release, release, memory_order_release);
    atomic_store_explicit(&g_windows_async_tail_runtime, rt, memory_order_release);
}

static void llam_windows_async_test_hold_tail(llam_runtime_t *rt) {
    atomic_uint *observed;
    atomic_uint *release;
    if (atomic_load_explicit(&g_windows_async_tail_runtime, memory_order_acquire) != rt) {
        return;
    }
    observed = atomic_load_explicit(&g_windows_async_tail_observed, memory_order_acquire);
    release = atomic_load_explicit(&g_windows_async_tail_release, memory_order_acquire);
    if (observed != NULL && release != NULL) {
        atomic_store_explicit(observed, 1U, memory_order_release);
        while (atomic_load_explicit(release, memory_order_acquire) == 0U) {
            Sleep(1U);
        }
    }
}

void llam_windows_async_lifetime_test_fail_worker(unsigned kind, unsigned index) {
    atomic_store_explicit(&g_windows_async_failure_index, index, memory_order_relaxed);
    atomic_store_explicit(&g_windows_async_failure_kind, kind, memory_order_release);
}

bool llam_windows_async_lifetime_test_take_worker_failure(unsigned kind, unsigned index) {
    unsigned expected = kind;
    return atomic_load_explicit(&g_windows_async_failure_index, memory_order_relaxed) == index &&
        atomic_compare_exchange_strong_explicit(&g_windows_async_failure_kind,
            &expected, 0U, memory_order_acq_rel, memory_order_acquire);
}

/* Observation only: no lock, allocation, scheduling or lifetime pin. A unique
 * slot is written once, then release-published; readers acquire the ready
 * flag before copying its immutable fields. The trace can safely be recorded
 * while the target M is suspended. Overflow is an oracle failure, not wrap. */
static void llam_windows_async_test_record(
    llam_windows_async_control_t *control,
    unsigned kind,
    const llam_windows_async_operation_t *op,
    llam_task_t *task,
    uint64_t bank_generation) {
    unsigned index = atomic_fetch_add_explicit(
        &control->test_trace_next, 1U, memory_order_relaxed);
    llam_windows_async_test_event_t *event;

    if (index >= LLAM_WINDOWS_ASYNC_TEST_TRACE_CAPACITY) {
        atomic_store_explicit(&control->test_trace_overflow, 1U,
                              memory_order_release);
        return;
    }
    event = &control->test_trace[index];
    event->kind = kind;
    event->task_key = (uintptr_t)task;
    event->bank_key = op != NULL ? (uintptr_t)op->bank : 0U;
    event->captured_pc = op != NULL && op->bank != NULL
                             ? (uintptr_t)op->bank->captured.context->Rip
                             : 0U;
    event->bank_generation = bank_generation;
    event->capture_task_run = op != NULL ? op->capture_task_run : 0U;
    event->capture_worker_run = op != NULL ? op->capture_worker_run : 0U;
    event->restore_task_run = op != NULL && op->bank != NULL
                                  ? op->bank->restore_task_run
                                  : 0U;
    event->restore_worker_run = op != NULL && op->bank != NULL
                                    ? op->bank->restore_worker_run
                                    : 0U;
    event->worker_key = (uintptr_t)(op != NULL ? op->worker : g_llam_tls_worker);
    event->binding_incarnation = op != NULL ? op->binding_incarnation : 0U;
    event->runtime_run = control->run_generation;
    event->shard_id = op != NULL ? op->shard->id
                                  : (g_llam_tls_shard != NULL
                                         ? g_llam_tls_shard->id : UINT_MAX);
    event->native_thread_id = op != NULL
        ? llam_windows_async_worker_binding(control, op->worker)->native_thread_id : GetCurrentThreadId();
    atomic_store_explicit(&control->test_trace_ready[index], 1U,
                          memory_order_release);
}

size_t llam_windows_async_lifetime_test_trace(
    const llam_runtime_t *runtime,
    llam_windows_async_test_event_t *events,
    size_t capacity) {
    const llam_windows_async_control_t *control;
    size_t limit;
    size_t index;

    if (runtime == NULL || events == NULL ||
        atomic_load_explicit(&runtime->async_preempt_enabled,
                             memory_order_acquire) == 0U ||
        (control = runtime->async_preempt_control) == NULL) {
        return 0U;
    }
    limit = atomic_load_explicit(&control->test_trace_next,
                                 memory_order_acquire);
    if (limit > LLAM_WINDOWS_ASYNC_TEST_TRACE_CAPACITY) {
        limit = LLAM_WINDOWS_ASYNC_TEST_TRACE_CAPACITY;
    }
    if (limit > capacity) {
        limit = capacity;
    }
    for (index = 0U; index < limit; ++index) {
        if (atomic_load_explicit(&control->test_trace_ready[index],
                                 memory_order_acquire) == 0U) {
            break;
        }
        events[index] = control->test_trace[index];
    }
    return index;
}

bool llam_windows_async_lifetime_test_trace_overflow(
    const llam_runtime_t *runtime) {
    const llam_windows_async_control_t *control;

    if (runtime == NULL ||
        atomic_load_explicit(&runtime->async_preempt_enabled,
                             memory_order_acquire) == 0U ||
        (control = runtime->async_preempt_control) == NULL) {
        return false;
    }
    return atomic_load_explicit(&control->test_trace_overflow,
                                memory_order_acquire) != 0U;
}

void llam_windows_async_lifetime_test_note_task(llam_runtime_t *runtime,
                                                unsigned kind,
                                                uint64_t bank_generation) {
    llam_windows_async_control_t *control;

    if (runtime == NULL || g_llam_tls_task == NULL ||
        (kind != LLAM_WINDOWS_ASYNC_TEST_PEER &&
         kind != LLAM_WINDOWS_ASYNC_TEST_RETURN) ||
        atomic_load_explicit(&runtime->async_preempt_enabled,
                             memory_order_acquire) == 0U ||
        (control = runtime->async_preempt_control) == NULL) {
        return;
    }
    llam_windows_async_test_record(control, kind, NULL, g_llam_tls_task,
                                   bank_generation);
}

static _Atomic(llam_task_t *) g_windows_async_ready_gate_task = ATOMIC_VAR_INIT(NULL);
static _Atomic(atomic_uint *) g_windows_async_ready_gate_observed = ATOMIC_VAR_INIT(NULL);
static _Atomic(atomic_uint *) g_windows_async_ready_gate_release = ATOMIC_VAR_INIT(NULL);
static _Atomic(llam_task_t *) g_windows_async_phase_gate_task = ATOMIC_VAR_INIT(NULL);
static atomic_uint g_windows_async_phase_gate_point = ATOMIC_VAR_INIT(0U);
static _Atomic(atomic_uint *) g_windows_async_phase_gate_observed = ATOMIC_VAR_INIT(NULL);
static _Atomic(atomic_uint *) g_windows_async_phase_gate_release = ATOMIC_VAR_INIT(NULL);
static _Atomic(llam_runtime_t *) g_windows_async_enable_gate_runtime =
    ATOMIC_VAR_INIT(NULL);
static _Atomic(atomic_uint *) g_windows_async_enable_gate_observed =
    ATOMIC_VAR_INIT(NULL);
static _Atomic(atomic_uint *) g_windows_async_enable_gate_release =
    ATOMIC_VAR_INIT(NULL);

void llam_windows_async_lifetime_test_enable_gate(llam_runtime_t *runtime,
                                                 atomic_uint *observed,
                                                 atomic_uint *release) {
    atomic_store_explicit(&g_windows_async_enable_gate_observed, observed,
                          memory_order_release);
    atomic_store_explicit(&g_windows_async_enable_gate_release, release,
                          memory_order_release);
    atomic_store_explicit(&g_windows_async_enable_gate_runtime, runtime,
                          memory_order_release);
}

static void llam_windows_async_lifetime_test_hold_enable(llam_runtime_t *rt) {
    atomic_uint *observed;
    atomic_uint *release;

    if (atomic_load_explicit(&g_windows_async_enable_gate_runtime,
                             memory_order_acquire) != rt) {
        return;
    }
    observed = atomic_load_explicit(&g_windows_async_enable_gate_observed,
                                    memory_order_acquire);
    release = atomic_load_explicit(&g_windows_async_enable_gate_release,
                                   memory_order_acquire);
    if (observed == NULL || release == NULL) {
        return;
    }
    atomic_store_explicit(observed, 1U, memory_order_release);
    while (atomic_load_explicit(release, memory_order_acquire) == 0U) {
        YieldProcessor();
    }
}

void llam_windows_async_lifetime_test_phase_gate(llam_task_t *task,
                                                unsigned phase,
                                                atomic_uint *observed,
                                                atomic_uint *release) {
    atomic_store_explicit(&g_windows_async_phase_gate_observed, observed,
                          memory_order_release);
    atomic_store_explicit(&g_windows_async_phase_gate_release, release,
                          memory_order_release);
    atomic_store_explicit(&g_windows_async_phase_gate_task, task,
                          memory_order_release);
    atomic_store_explicit(&g_windows_async_phase_gate_point, phase,
                          memory_order_release);
}

static void llam_windows_async_lifetime_test_hold_phase(llam_task_t *task,
                                                         unsigned phase) {
    atomic_uint *observed;
    atomic_uint *release;

    if (atomic_load_explicit(&g_windows_async_phase_gate_point,
                             memory_order_acquire) != phase ||
        atomic_load_explicit(&g_windows_async_phase_gate_task,
                             memory_order_acquire) != task) {
        return;
    }
    observed = atomic_load_explicit(&g_windows_async_phase_gate_observed,
                                    memory_order_acquire);
    release = atomic_load_explicit(&g_windows_async_phase_gate_release,
                                   memory_order_acquire);
    if (observed == NULL || release == NULL) {
        return;
    }
    atomic_store_explicit(observed, 1U, memory_order_release);
    while (atomic_load_explicit(release, memory_order_acquire) == 0U) {
        YieldProcessor();
    }
}

void llam_windows_async_lifetime_test_ready_gate(llam_task_t *task,
                                                 atomic_uint *observed,
                                                 atomic_uint *release) {
    atomic_store_explicit(&g_windows_async_ready_gate_observed, observed,
                          memory_order_release);
    atomic_store_explicit(&g_windows_async_ready_gate_release, release,
                          memory_order_release);
    atomic_store_explicit(&g_windows_async_ready_gate_task, task,
                          memory_order_release);
}

static void llam_windows_async_lifetime_test_hold_after_ready(llam_task_t *task) {
    atomic_uint *observed;
    atomic_uint *release;

    if (atomic_load_explicit(&g_windows_async_ready_gate_task,
                             memory_order_acquire) != task) {
        return;
    }
    observed = atomic_load_explicit(&g_windows_async_ready_gate_observed,
                                    memory_order_acquire);
    release = atomic_load_explicit(&g_windows_async_ready_gate_release,
                                   memory_order_acquire);
    if (observed == NULL || release == NULL) {
        return;
    }
    atomic_store_explicit(observed, 1U, memory_order_release);
    while (atomic_load_explicit(release, memory_order_acquire) == 0U) {
        YieldProcessor();
    }
}
#endif

static llam_windows_async_control_t *llam_windows_async_control(llam_runtime_t *rt) {
    return rt != NULL && atomic_load_explicit(&rt->async_preempt_enabled,
                                               memory_order_acquire) != 0U
               ? rt->async_preempt_control : NULL;
}

#if defined(LLAM_ENABLE_TEST_HOOKS)
void llam_windows_async_lifetime_test_error_sentinels(llam_runtime_t *rt) {
    llam_windows_async_control_t *control = llam_windows_async_control(rt);
    if (control != NULL) {
        atomic_store_explicit(&control->test_error_sentinels, 1U, memory_order_release);
    }
}

void llam_windows_async_lifetime_test_scheduler_error(llam_task_t *task, bool before) {
    llam_windows_async_control_t *control = llam_windows_async_control(task->owner_runtime);
    DWORD last_error;
    int error;
    llam_shard_t *shard;
    llam_windows_async_binding_t *binding;
    uintptr_t address;

    if (control == NULL || atomic_load_explicit(&control->test_error_sentinels,
                                               memory_order_acquire) == 0U) {
        return;
    }
    last_error = GetLastError();
    error = llam_thread_errno_load();
    shard = g_llam_tls_shard;
    binding = llam_windows_async_worker_binding(control, g_llam_tls_worker);
    address = (uintptr_t)&errno;
    if (binding == NULL || g_llam_tls_worker != binding->worker ||
        binding->native_thread_id != GetCurrentThreadId()) {
        abort();
    }
    if (before) {
        if (binding->test_errno_address != 0U && binding->test_errno_address != address) {
            abort();
        }
        binding->test_errno_address = address;
        llam_thread_errno_store((int)(300U + shard->id));
        SetLastError(0xD100U + shard->id);
    } else {
        if (error != (int)(300U + shard->id) || last_error != 0xD100U + shard->id ||
            binding->test_errno_address != address) {
            abort();
        }
        atomic_fetch_add_explicit(&control->test_error_checks, 1U, memory_order_release);
        llam_thread_errno_store(error);
        SetLastError(last_error);
    }
}
#endif

static bool llam_windows_async_range_executable(uintptr_t begin,
                                                uintptr_t end,
                                                HMODULE module) {
    uintptr_t cursor = begin;

    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info;
        uintptr_t base;
        uintptr_t next;
        DWORD protection;

        if (VirtualQuery((const void *)cursor, &info, sizeof(info)) == 0U ||
            info.State != MEM_COMMIT || info.Type != MEM_IMAGE ||
            (module != NULL && info.AllocationBase != module) ||
            info.RegionSize == 0U ||
            (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0U) {
            return false;
        }
        protection = info.Protect & 0xFFU;
        if (protection != PAGE_EXECUTE &&
            protection != PAGE_EXECUTE_READ &&
            protection != PAGE_EXECUTE_READWRITE &&
            protection != PAGE_EXECUTE_WRITECOPY) {
            return false;
        }
        base = (uintptr_t)info.BaseAddress;
        if (base > cursor || info.RegionSize > UINTPTR_MAX - base) {
            return false;
        }
        next = base + info.RegionSize;
        if (next <= cursor) {
            return false;
        }
        cursor = next;
    }
    return true;
}

/* Pin raw runtime storage before touching the per-runtime gate. The temporary
 * exec_started claim excludes run start; the host-spawn reservation closes
 * the gap before live_tasks publication. No global lifecycle lock is needed. */
static int llam_windows_async_config_begin(llam_runtime_t *rt,
                                           llam_runtime_t **pinned_out) {
    llam_runtime_t *pinned = NULL;
    bool expected = false;
    int lock_error;

    if (rt == NULL || pinned_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    *pinned_out = NULL;
    if (llam_runtime_begin_public_op(rt, &pinned) != 0) {
        return -1;
    }
    if (!pinned->windows_async_config_lock_initialized) {
        llam_runtime_end_public_op(pinned);
        errno = EBUSY;
        return -1;
    }
    lock_error = pthread_mutex_lock(&pinned->windows_async_config_lock);
    if (lock_error != 0) {
        llam_runtime_end_public_op(pinned);
        errno = lock_error;
        return -1;
    }
    if (!atomic_compare_exchange_strong_explicit(
            &pinned->exec_started, &expected, true,
            memory_order_acq_rel, memory_order_acquire)) {
        pthread_mutex_unlock(&pinned->windows_async_config_lock);
        llam_runtime_end_public_op(pinned);
        errno = EBUSY;
        return -1;
    }
    if (!atomic_load_explicit(&pinned->initialized,
                              memory_order_acquire) ||
        atomic_load_explicit(&pinned->stop_requested,
                             memory_order_acquire) ||
        atomic_load_explicit(&pinned->shutdown_requested,
                             memory_order_acquire) ||
        atomic_load_explicit(&pinned->destroy_claimed,
                             memory_order_acquire) ||
        pinned->windows_async_host_spawn_admits != 0U) {
        atomic_store_explicit(&pinned->exec_started, false,
                              memory_order_release);
        pthread_mutex_unlock(&pinned->windows_async_config_lock);
        llam_runtime_end_public_op(pinned);
        errno = EBUSY;
        return -1;
    }
    *pinned_out = pinned;
    return 0;
}

static void llam_windows_async_config_end(llam_runtime_t *rt) {
    atomic_store_explicit(&rt->exec_started, false,
                          memory_order_release);
    pthread_mutex_unlock(&rt->windows_async_config_lock);
    llam_runtime_end_public_op(rt);
}

static int llam_windows_async_register_safe_range_locked(llam_runtime_t *rt,
                                                         uintptr_t begin,
                                                         uintptr_t end) {
    llam_async_preempt_range_t *ranges;
    size_t capacity;
    size_t i;

    if (atomic_load_explicit(&rt->async_preempt_enabled,
                             memory_order_acquire) != 0U ||
        llam_runtime_has_live_tasks(rt) || rt->async_preempt_control != NULL) {
        errno = EBUSY;
        return -1;
    }
    if (rt->async_preempt_range_count >= 128U ||
        rt->windows_async_range_generation == UINT64_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    for (i = 0U; i < rt->async_preempt_range_count; ++i) {
        const llam_async_preempt_range_t *existing =
            &rt->async_preempt_ranges[i];

        if (begin < existing->end && existing->begin < end) {
            errno = EINVAL;
            return -1;
        }
    }
    if (rt->async_preempt_range_count == rt->async_preempt_range_capacity) {
        capacity = rt->async_preempt_range_capacity == 0U
                       ? 4U
                       : rt->async_preempt_range_capacity * 2U;
        if (capacity > 128U) {
            capacity = 128U;
        }
        ranges = realloc(rt->async_preempt_ranges,
                         capacity * sizeof(*ranges));
        if (ranges == NULL) {
            errno = ENOMEM;
            return -1;
        }
        rt->async_preempt_ranges = ranges;
        rt->async_preempt_range_capacity = capacity;
    }
    ranges = rt->async_preempt_ranges;
    ranges[rt->async_preempt_range_count].begin = begin;
    ranges[rt->async_preempt_range_count].end = end;
    rt->async_preempt_range_count += 1U;
    rt->windows_async_range_generation += 1U;
    return 0;
}

int llam_windows_async_lifetime_register_safe_range(llam_runtime_t *rt,
                                                    uintptr_t begin,
                                                    uintptr_t end) {
    llam_runtime_t *pinned = NULL;
    int result;
    int saved_errno;

    if (rt == NULL || begin == 0U || end <= begin) {
        errno = EINVAL;
        return -1;
    }
    if (llam_windows_async_config_begin(rt, &pinned) != 0) {
        return -1;
    }
    result = llam_windows_async_register_safe_range_locked(pinned, begin, end);
    saved_errno = errno;
    if (result == 0) {
        llam_preempt_capabilities_note_ranges(pinned,
                                               pinned->async_preempt_range_count);
    }
    llam_windows_async_config_end(pinned);
    if (result != 0) {
        errno = saved_errno;
    }
    return result;
}

bool llam_windows_async_lifetime_pc_allowed(const llam_windows_async_operation_t *op,
                                            uintptr_t rip) {
    const llam_windows_async_control_t *control;
    size_t i;

    if (op == NULL || op->runtime == NULL || op->bank == NULL ||
        atomic_load_explicit(&op->runtime->async_preempt_enabled,
                             memory_order_acquire) == 0U ||
        (control = llam_windows_async_control(op->runtime)) == NULL ||
        op->range_generation == 0U ||
        op->range_generation != control->safe_range_generation ||
        op->bank_generation != op->bank->generation) {
        return false;
    }
    if (op->task != NULL && op->task->managed_catalog != NULL) {
        return op->task->managed_execution_class != LLAM_MANAGED_EXECUTION_LEGACY &&
               llam_managed_catalog_async_enabled(op->runtime) &&
               llam_managed_catalog_find_safe(op->task->managed_catalog, rip) != NULL;
    }
    /* Immutable, bounded array lookup: no allocation or lock after suspend. */
    for (i = 0U; i < control->safe_range_count; ++i) {
        if (rip >= control->safe_ranges[i].begin &&
            rip < control->safe_ranges[i].end) {
            return true;
        }
    }
    return false;
}

static bool llam_windows_async_context_init(llam_windows_async_context_buffer_t *area,
                                            DWORD length, DWORD64 features) {
    DWORD actual = length;

    area->storage = calloc(1U, length);
    if (area->storage == NULL) {
        return false;
    }
    area->length = length;
    if (!InitializeContext(area->storage, LLAM_WINDOWS_ASYNC_CONTEXT_FLAGS,
                           &area->context, &actual) ||
        area->context == NULL || actual > length ||
        !SetXStateFeaturesMask(area->context, features)) {
        free(area->storage);
        memset(area, 0, sizeof(*area));
        errno = ENOTSUP;
        return false;
    }
    return true;
}

static bool llam_windows_async_context_reset(llam_windows_async_context_buffer_t *area,
                                             DWORD64 features) {
    DWORD length = area->length;
    PCONTEXT context = NULL;

    if (area->storage == NULL ||
        !InitializeContext(area->storage, LLAM_WINDOWS_ASYNC_CONTEXT_FLAGS,
                           &context, &length) ||
        context == NULL || length > area->length ||
        !SetXStateFeaturesMask(context, features)) {
        return false;
    }
    area->context = context;
    return true;
}

static void llam_windows_async_context_destroy(llam_windows_async_context_buffer_t *area) {
    free(area->storage);
    memset(area, 0, sizeof(*area));
}

static int llam_windows_capsule_prepare(void *object, void *argument) {
    llam_windows_async_capsule_t *capsule = object;
    const llam_windows_async_control_t *control = argument;
    for (unsigned i = 0U; i < 2U; ++i) {
        if (!llam_windows_async_context_init(&capsule->banks[i].captured,
                                             control->context_length, control->xstate_features) ||
            !llam_windows_async_context_init(&capsule->banks[i].redirected,
                                             control->context_length, control->xstate_features)) return -1;
    }
    return 0;
}

static void llam_windows_capsule_destroy(void *object, void *argument) {
    llam_windows_async_capsule_t *capsule = object;
    (void)argument;
    for (unsigned i = 0U; i < 2U; ++i) {
        llam_windows_async_context_destroy(&capsule->banks[i].captured);
        llam_windows_async_context_destroy(&capsule->banks[i].redirected);
    }
}

static bool llam_windows_async_profile_supported(DWORD64 *features_out) {
    PROCESS_MITIGATION_USER_SHADOW_STACK_POLICY policy = {0};
    DWORD64 features = GetEnabledXStateFeatures();
    if (!GetProcessMitigationPolicy(GetCurrentProcess(), ProcessUserShadowStackPolicy,
                                    &policy, sizeof(policy)) ||
        policy.EnableUserShadowStack || policy.AuditUserShadowStack ||
        policy.EnableUserShadowStackStrictMode ||
        policy.SetContextIpValidation || policy.AuditSetContextIpValidation ||
        policy.SetContextIpValidationRelaxedMode || features == 0U ||
        (features & ~LLAM_WINDOWS_ASYNC_SUPPORTED_XSTATE) != 0U) return false;
    *features_out = features;
    return true;
}

static int llam_windows_async_enable_locked(llam_runtime_t *rt) {
    llam_windows_async_control_t *control;
    DWORD64 features;
    DWORD context_length = 0U;
    PCONTEXT ignored = NULL;
    size_t i;
    int failure_error = ENOTSUP;

    if (llam_runtime_has_live_tasks(rt) || rt->active_shards == 0U ||
        (rt->execution_lease_mode != LLAM_EXECUTION_LEASE_REGISTERED &&
         rt->experimental_dynamic_shards != 0U) ||
        rt->experimental_shard_rings != 0U ||
        rt->experimental_shard_rings_multishot != 0U ||
        rt->windows_unsafe_skip_task_simd != 0U ||
        rt->async_preempt_control != NULL) {
        errno = EBUSY;
        return -1;
    }
    if (!llam_windows_async_profile_supported(&features)) {
        errno = ENOTSUP;
        return -1;
    }
    for (i = 0U; i < rt->async_preempt_range_count; ++i) {
        const llam_async_preempt_range_t *range =
            &rt->async_preempt_ranges[i];

        if (range->begin == 0U || range->end <= range->begin ||
            !llam_windows_async_range_executable(range->begin,
                                                  range->end, NULL)) {
            errno = ENOTSUP;
            return -1;
        }
    }
    control = calloc(1U, sizeof(*control));
    if (control == NULL) {
        errno = ENOMEM;
        return -1;
    }
    if (rt->async_preempt_range_count != 0U) {
        control->safe_range_modules = calloc(rt->async_preempt_range_count,
                                             sizeof(*control->safe_range_modules));
        if (control->safe_range_modules == NULL) {
            free(control);
            errno = ENOMEM;
            return -1;
        }
    }
    for (i = 0U; i < rt->async_preempt_range_count; ++i) {
        const llam_async_preempt_range_t *range =
            &rt->async_preempt_ranges[i];
        HMODULE module = NULL;

        /* Hold one loader reference through shutdown. A certified numeric PC
         * must not name a different image after an unload/reload during run. */
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                (LPCSTR)(const void *)range->begin,
                                &module) || module == NULL) {
            goto unsupported_range;
        }
        control->safe_range_modules[i] = module;
        if (!llam_windows_async_range_executable(range->begin, range->end,
                                                  module)) {
            goto unsupported_range;
        }
    }
    control->xstate_features = features;
    control->safe_ranges = rt->async_preempt_ranges;
    control->safe_range_count = rt->async_preempt_range_count;
    control->safe_range_generation = rt->windows_async_range_generation != 0U
        ? rt->windows_async_range_generation : 1U;
    control->managed = rt->execution_lease_mode == LLAM_EXECUTION_LEASE_REGISTERED;
    if (rt->managed_worker_count > UINT_MAX - rt->active_shards) {
        failure_error = EOVERFLOW;
        goto unsupported_range;
    }
    control->binding_count = rt->active_shards + (control->managed ? (unsigned)rt->managed_worker_count : 0U);
    control->bindings = calloc(control->binding_count, sizeof(*control->bindings));
    if (control->bindings == NULL) {
        failure_error = ENOMEM;
        goto unsupported_range;
    }
    if (InitializeContext(NULL, LLAM_WINDOWS_ASYNC_CONTEXT_FLAGS,
                          &ignored, &context_length) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
        context_length < sizeof(CONTEXT)) {
        goto unsupported_range;
    }
    control->context_length = context_length;
    for (i = 0U; i < control->binding_count; ++i) {
        llam_windows_async_binding_t *binding = &control->bindings[i];
        binding->worker = i < rt->active_shards ? &rt->shards[i].primary_worker
            : &rt->managed_workers[i - rt->active_shards];
        binding->scheduler = control->managed ? NULL : &rt->shards[i].scheduler_ctx;
        binding->index = (unsigned)i;
        atomic_init(&binding->probe_phase, 0U);
        atomic_init(&binding->probe_error, 0);
        atomic_init(&binding->restart_pending, false);
        atomic_init(&binding->execution_debt, 0U);
        atomic_init(&binding->retire_count, 0U);
        atomic_init(&binding->finalizer_count, 0U);
        atomic_init(&binding->binding_ops, 0U);
        atomic_init(&binding->captured_bank, NULL);
        atomic_init(&binding->pending_restore, NULL);
        atomic_init(&binding->ready_depth, 0U);
#if defined(LLAM_ENABLE_TEST_HOOKS)
        atomic_init(&binding->test_ready_contexts, 0U);
        if (llam_windows_async_lifetime_test_take_worker_failure(
                LLAM_WINDOWS_ASYNC_TEST_FAIL_SLOT_ALLOC, (unsigned)i)) {
            failure_error = ENOMEM;
            goto unsupported_range;
        }
#endif
        if (!llam_windows_async_context_init(&binding->qualification,
                                             context_length, features)) {
            failure_error = errno == ENOMEM ? ENOMEM : ENOTSUP;
            goto unsupported_range;
        }
    }
    atomic_init(&control->start_phase, LLAM_WINDOWS_ASYNC_START_CLOSED);
    atomic_init(&control->registered, 0U);
    atomic_init(&control->start_error, 0);
    atomic_init(&control->controller_busy, 0U);
    atomic_init(&control->remote_transactions, 0U);
    atomic_init(&control->remote_transactions_max, 0U);
    atomic_init(&control->native_handles_created, 0U);
    atomic_init(&control->native_handles_closed, 0U);
    atomic_init(&control->charged_bytes, 0U);
    atomic_init(&control->capture_attempts, 0U);
    atomic_init(&control->captures, 0U);
    atomic_init(&control->unsafe_pc_refusals, 0U);
    atomic_init(&control->restores, 0U);
    atomic_init(&control->direct_witnesses, 0U);
#if defined(LLAM_ENABLE_TEST_HOOKS)
    atomic_init(&control->test_trace_next, 0U);
    atomic_init(&control->test_error_sentinels, 0U);
    atomic_init(&control->test_error_checks, 0U);
    atomic_init(&control->test_trace_overflow, 0U);
    for (i = 0U; i < LLAM_WINDOWS_ASYNC_TEST_TRACE_CAPACITY; ++i) {
        atomic_init(&control->test_trace_ready[i], 0U);
    }
    /* Test-only pause enlarges the admitted configuration interval while
     * runtime pin, synthetic run claim and config gate remain owned. */
    llam_windows_async_lifetime_test_hold_enable(rt);
#endif
    if (atomic_load_explicit(&rt->stop_requested,
                             memory_order_acquire) ||
        atomic_load_explicit(&rt->shutdown_requested,
                             memory_order_acquire) ||
        atomic_load_explicit(&rt->destroy_claimed,
                             memory_order_acquire)) {
        failure_error = EBUSY;
        goto unsupported_range;
    }
    size_t stride = ((size_t)control->context_length + 63U) & ~(size_t)63U;
    uint64_t budget = llam_managed_catalog_budget(rt);
    if (budget == 0U) budget = LLAM_WINDOWS_ASYNC_CAPSULE_BUDGET;
    if (stride > SIZE_MAX / 4U || llam_capsule_pool_init(&control->capsule_pool,
            sizeof(llam_windows_async_capsule_t), 4U * stride, budget,
            (size_t)control->binding_count * 2U + 2U,
            llam_windows_capsule_prepare, llam_windows_capsule_destroy, control) != 0) {
        failure_error = errno;
        goto unsupported_range;
    }
    rt->async_preempt_control = control;
    rt->managed_capsule_pool = &control->capsule_pool;
    /* This release store is the sole lock-free publication gate for the
     * immutable range/control/module-pin snapshot. The watchdog acquires it
     * before touching the range count; task and operation readers do likewise. */
    atomic_store_explicit(&rt->async_preempt_enabled, 1U,
                          memory_order_release);
    return 0;

unsupported_range:
    llam_capsule_pool_destroy(&control->capsule_pool);
    if (control->bindings != NULL) {
        for (i = 0U; i < control->binding_count; ++i) {
            llam_windows_async_context_destroy(&control->bindings[i].qualification);
        }
        free(control->bindings);
    }
    for (i = 0U; i < rt->async_preempt_range_count; ++i) {
        if (control->safe_range_modules[i] != NULL) {
            FreeLibrary(control->safe_range_modules[i]);
        }
    }
    free(control->safe_range_modules);
    free(control);
    errno = failure_error;
    return -1;
}

int llam_windows_async_lifetime_enable(llam_runtime_t *rt) {
    llam_runtime_t *pinned = NULL;
    int result;
    int saved_errno;

    if (llam_windows_async_config_begin(rt, &pinned) != 0) {
        return -1;
    }
    result = llam_windows_async_enable_locked(pinned);
    saved_errno = errno;
    if (result == 0) {
        llam_preempt_capabilities_note_async_result(
            pinned, true, LLAM_PREEMPT_FAILURE_NONE);
    }
    llam_windows_async_config_end(pinned);
    if (result != 0) {
        errno = saved_errno;
    }
    return result;
}

/* Public expert opt-in is one transaction: no partial range registration is
 * observable on failure. The caller certifies code semantics; this function
 * verifies only numeric ranges and the backend's PE/machine preconditions. */
int llam_windows_async_lifetime_enable_certified_ranges(
    llam_runtime_t *rt,
    const llam_windows_async_preempt_range_t *input,
    size_t count) {
    llam_runtime_t *pinned = NULL;
    llam_async_preempt_range_t *staged = NULL;
    size_t i;
    int result = -1;
    int saved_errno = EINVAL;
    bool installed = false;

    if (rt == NULL || input == NULL || count == 0U || count > 128U) {
        errno = EINVAL;
        return -1;
    }
    /* A managed caller must never park its scheduler M behind another
     * runtime's configuration mutex. This is a host-only pre-run control. */
    if (g_llam_tls_task != NULL || g_llam_tls_scheduler_ctx != NULL) {
        errno = EBUSY;
        return -1;
    }
    if (llam_windows_async_config_begin(rt, &pinned) != 0) {
        return -1;
    }
    if (atomic_load_explicit(&pinned->async_preempt_enabled,
                             memory_order_acquire) != 0U) {
        saved_errno = EALREADY;
        goto finish;
    }
    if (pinned->async_preempt_ranges != NULL ||
        pinned->async_preempt_range_count != 0U ||
        pinned->async_preempt_control != NULL) {
        saved_errno = EBUSY;
        goto finish;
    }
    /* The public contract is first-run-only. A completed older run may still
     * have host-side handle reclamation in flight after live_tasks reaches 0;
     * it must not overlap first publication of the Windows control pointer. */
    for (i = 0U; i < pinned->active_shards; ++i) {
        if (atomic_load_explicit(&pinned->shards[i].primary_worker.run_generation,
                                 memory_order_acquire) != 0U) {
            saved_errno = EBUSY;
            goto finish;
        }
    }
    if (pinned->preempt_mode < LLAM_PREEMPT_AUTO ||
        pinned->active_shards == 0U ||
        pinned->experimental_dynamic_shards != 0U ||
        pinned->experimental_shard_rings != 0U ||
        pinned->experimental_shard_rings_multishot != 0U ||
        pinned->windows_unsafe_skip_task_simd != 0U) {
        saved_errno = ENOTSUP;
        goto finish;
    }
    if (pinned->windows_async_range_generation == UINT64_MAX) {
        saved_errno = EOVERFLOW;
        goto finish;
    }
    staged = calloc(count, sizeof(*staged));
    if (staged == NULL) {
        saved_errno = ENOMEM;
        goto finish;
    }
    for (i = 0U; i < count; ++i) {
        size_t previous;

        if (input[i].flags != 0U || input[i].reserved0 != 0U ||
            input[i].begin_pc == 0U ||
            input[i].end_pc <= input[i].begin_pc ||
            input[i].begin_pc > UINTPTR_MAX ||
            input[i].end_pc > UINTPTR_MAX) {
            saved_errno = EINVAL;
            goto finish;
        }
        staged[i].begin = (uintptr_t)input[i].begin_pc;
        staged[i].end = (uintptr_t)input[i].end_pc;
        for (previous = 0U; previous < i; ++previous) {
            if (staged[i].begin < staged[previous].end &&
                staged[previous].begin < staged[i].end) {
                saved_errno = EINVAL;
                goto finish;
            }
        }
    }

    pinned->async_preempt_ranges = staged;
    pinned->async_preempt_range_count = count;
    pinned->async_preempt_range_capacity = count;
    pinned->windows_async_range_generation += 1U;
    installed = true;
    if (llam_windows_async_enable_locked(pinned) != 0) {
        saved_errno = errno;
        goto finish;
    }
    llam_preempt_capabilities_note_ranges(pinned, count);
    llam_preempt_capabilities_note_async_result(
        pinned, true, LLAM_PREEMPT_FAILURE_NONE);
    result = 0;

finish:
    if (result != 0 && installed) {
        pinned->async_preempt_ranges = NULL;
        pinned->async_preempt_range_count = 0U;
        pinned->async_preempt_range_capacity = 0U;
        pinned->windows_async_range_generation -= 1U;
    }
    if (result != 0) {
        free(staged);
    }
    llam_windows_async_config_end(pinned);
    if (result != 0) {
        errno = saved_errno;
    }
    return result;
}

/* Run publication is separate from exec_started: configuration also borrows
 * that token. No G or controller transaction is admitted until every actual
 * primary native M has registered in this generation. */
int llam_windows_async_lifetime_run_begin(llam_runtime_t *rt) {
    llam_windows_async_control_t *control;
    unsigned i;

    if (rt == NULL || atomic_load_explicit(&rt->async_preempt_enabled,
                                           memory_order_acquire) == 0U) {
        return 0;
    }
    control = llam_windows_async_control(rt);
    if (control == NULL || control->binding_count != rt->active_shards + (control->managed ? rt->managed_worker_count : 0U) ||
        control->run_generation == UINT64_MAX ||
        atomic_load_explicit(&control->start_phase, memory_order_acquire) !=
            LLAM_WINDOWS_ASYNC_START_CLOSED ||
        llam_windows_async_lifetime_has_debt(rt)) {
        errno = EBUSY;
        return -1;
    }
    for (i = 0U; i < control->binding_count; ++i) {
        if (control->bindings[i].native_thread != NULL) {
            errno = EBUSY;
            return -1;
        }
    }
    for (i = 0U; i < control->safe_range_count; ++i) {
        if (!llam_windows_async_range_executable(control->safe_ranges[i].begin,
                control->safe_ranges[i].end, control->safe_range_modules[i])) {
            errno = ENOTSUP;
            return -1;
        }
    }
    llam_windows_async_controller_exclude(control);
    ++control->run_generation;
    atomic_store_explicit(&control->registered, 0U, memory_order_relaxed);
    atomic_store_explicit(&control->start_error, 0, memory_order_relaxed);
    atomic_store_explicit(&control->start_phase,
                          LLAM_WINDOWS_ASYNC_START_PREPARING, memory_order_release);
    atomic_store_explicit(&control->controller_busy, 0U, memory_order_release);
    return 0;
}

void llam_windows_async_lifetime_run_abort(llam_runtime_t *rt, int error) {
    llam_windows_async_control_t *control = llam_windows_async_control(rt);
    int expected = 0;
    unsigned i;

    if (control == NULL) {
        return;
    }
    if (error == 0) {
        error = ENOTSUP;
    }
    (void)atomic_compare_exchange_strong_explicit(
        &control->start_error, &expected, error,
        memory_order_acq_rel, memory_order_acquire);
    atomic_store_explicit(&control->start_phase,
                          LLAM_WINDOWS_ASYNC_START_ABORTED, memory_order_release);
    for (i = 0U; i < control->binding_count; ++i) {
        llam_kick_shard(control->bindings[i].worker->compat_shard);
    }
}

static int llam_windows_async_register_worker(llam_worker_t *worker, llam_ctx_t *scheduler, bool restart) {
    llam_shard_t *shard = worker->compat_shard;
    llam_runtime_t *rt = shard->runtime;
    llam_windows_async_control_t *control = llam_windows_async_control(rt);
    llam_windows_async_binding_t *binding;
    HANDLE handle = NULL;
    ULONG_PTR low = 0U;
    ULONG_PTR high = 0U;
    uint64_t incarnation;
    int error = ENOTSUP;
    unsigned expected_phase = restart ? LLAM_WINDOWS_ASYNC_START_OPEN : LLAM_WINDOWS_ASYNC_START_PREPARING;
    DWORD64 features = 0U;

    if (control == NULL) {
        return 0;
    }
    binding = llam_windows_async_worker_binding(control, worker);
    incarnation = atomic_load_explicit(&worker->binding_incarnation,
                                       memory_order_acquire);
    if (binding == NULL || g_llam_tls_worker != worker ||
        (!control->managed && g_llam_tls_shard != shard) || scheduler == NULL || incarnation == 0U) {
        goto failed;
    }
    /* Shard 0 is registered by its driver before creating auxiliaries. */
    if (binding->native_thread != NULL) {
        if (binding->native_thread_id != GetCurrentThreadId() ||
            binding->binding_incarnation != incarnation ||
            binding->runtime_run != control->run_generation) {
            abort();
        }
        return 0;
    }
    if (atomic_load_explicit(&control->start_phase, memory_order_acquire) !=
            expected_phase) {
        error = atomic_load_explicit(&control->start_error, memory_order_acquire);
        errno = error != 0 ? error : EBUSY;
        return -1;
    }
    if (!llam_windows_async_profile_supported(&features) || features != control->xstate_features)
        goto failed;
    GetCurrentThreadStackLimits(&low, &high);
#if defined(LLAM_ENABLE_TEST_HOOKS)
    if (llam_windows_async_lifetime_test_take_worker_failure(
            LLAM_WINDOWS_ASYNC_TEST_FAIL_DUPLICATE, binding->index)) {
        goto failed;
    }
#endif
    if (low == 0U || high <= low ||
        !llam_windows_async_context_reset(&binding->qualification,
                                          control->xstate_features) ||
        !DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                         GetCurrentProcess(), &handle,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                             THREAD_SET_CONTEXT | SYNCHRONIZE, FALSE, 0U)) {
        goto failed;
    }
    atomic_fetch_add_explicit(&control->native_handles_created, 1U, memory_order_release);
#if defined(LLAM_ENABLE_TEST_HOOKS)
    if (llam_windows_async_lifetime_test_take_worker_failure(
            LLAM_WINDOWS_ASYNC_TEST_FAIL_QUALIFICATION, binding->index)) {
        llam_windows_async_close_native_handle(control, handle);
        goto failed;
    }
    /* Own duplicated handle, but no pair lock and no registered publication. */
    llam_windows_async_test_hold_start(rt, shard->id);
#endif
    llam_windows_async_lock(shard);
    if (atomic_load_explicit(&control->start_phase, memory_order_acquire) !=
            expected_phase) {
        llam_windows_async_unlock(shard);
        llam_windows_async_close_native_handle(control, handle);
        errno = EBUSY;
        return -1;
    }
    atomic_store_explicit(&binding->probe_error, 0, memory_order_relaxed);
    atomic_store_explicit(&binding->restart_pending, restart, memory_order_relaxed);
    binding->scheduler = scheduler;
    binding->native_thread = handle;
    binding->native_thread_id = GetCurrentThreadId();
    binding->native_stack_low = (uintptr_t)low;
    binding->native_stack_high = (uintptr_t)high;
    binding->binding_incarnation = incarnation;
    binding->runtime_run = control->run_generation;
    binding->next_probe_ns = 0U;
#if defined(LLAM_ENABLE_TEST_HOOKS)
    binding->test_errno_address = 0U;
#endif
    binding->binding_open = true;
    if (control->managed)
        atomic_store_explicit(&worker->async_preempt_state, (uintptr_t)binding, memory_order_release);
    atomic_store_explicit(&binding->probe_phase, control->managed ? 1U : 2U, memory_order_release);
    /* Release/acquire RMW chain publishes all distinct per-M ready snapshots
     * to the driver, which then release-publishes OPEN to all dispatchers. */
    if (!restart) (void)atomic_fetch_add_explicit(&control->registered, 1U, memory_order_acq_rel);
    llam_windows_async_unlock(shard);
    while (control->managed && atomic_load_explicit(&binding->probe_phase, memory_order_acquire) == 1U &&
           atomic_load_explicit(&control->start_phase, memory_order_acquire) == expected_phase) {
        if (atomic_load_explicit(&rt->stop_requested, memory_order_acquire) ||
            atomic_load_explicit(&rt->shutdown_requested, memory_order_acquire)) {
            if (restart) atomic_store_explicit(&binding->probe_error, ECANCELED, memory_order_release);
            else llam_windows_async_lifetime_run_abort(rt, ECANCELED);
            break;
        }
        Sleep(1U);
    }
    if (control->managed && atomic_load_explicit(&binding->probe_phase, memory_order_acquire) != 2U) {
        error = atomic_load_explicit(restart ? &binding->probe_error : &control->start_error, memory_order_acquire);
        errno = error != 0 ? error : ENOTSUP;
        return -1;
    }
    if (restart) {
        atomic_store_explicit(&binding->restart_pending, false, memory_order_release);
        if (atomic_load_explicit(&rt->stop_requested, memory_order_acquire) ||
            atomic_load_explicit(&rt->shutdown_requested, memory_order_acquire)) {
            errno = ECANCELED;
            return -1;
        }
    }
    return 0;

failed:
    if (!restart) llam_windows_async_lifetime_run_abort(rt, error);
    errno = error;
    return -1;
}

int llam_windows_async_lifetime_register_worker(llam_shard_t *shard) {
    return llam_windows_async_register_worker(&shard->primary_worker, &shard->scheduler_ctx, false);
}

int llam_windows_async_lifetime_run_commit(llam_runtime_t *rt) {
    llam_windows_async_control_t *control = llam_windows_async_control(rt);
    unsigned expected = LLAM_WINDOWS_ASYNC_START_PREPARING;

    if (control == NULL) {
        return 0;
    }
    for (;;) {
        int error = atomic_load_explicit(&control->start_error, memory_order_acquire);

        if (error != 0) {
            errno = error;
            return -1;
        }
        if (atomic_load_explicit(&rt->stop_requested, memory_order_acquire) ||
            atomic_load_explicit(&rt->shutdown_requested, memory_order_acquire)) {
            llam_windows_async_lifetime_run_abort(rt, ECANCELED);
            errno = ECANCELED;
            return -1;
        }
        if (atomic_load_explicit(&control->registered, memory_order_acquire) ==
                control->binding_count) {
            break;
        }
        Sleep(1U);
    }
    if (control->managed) {
        for (unsigned i = 0U; i < control->binding_count; ++i) {
            if (atomic_load_explicit(&control->bindings[i].probe_phase, memory_order_acquire) != 2U) {
                errno = ENOTSUP;
                return -1;
            }
        }
        uint64_t isa = LLAM_LLPM2_ISA_BASE | LLAM_LLPM2_ISA_FP_SIMD;
        if ((control->xstate_features & UINT64_C(4)) != 0U) isa |= LLAM_LLPM2_ISA_AVX;
        if (rt->managed_catalog != NULL && llam_managed_catalog_async_commit(rt, isa) != 0) return -1;
    }
    if (!atomic_compare_exchange_strong_explicit(
            &control->start_phase, &expected, LLAM_WINDOWS_ASYNC_START_OPEN,
            memory_order_acq_rel, memory_order_acquire)) {
        errno = atomic_load_explicit(&control->start_error, memory_order_acquire);
        if (errno == 0) {
            errno = EBUSY;
        }
        return -1;
    }
    return 0;
}

int llam_windows_async_lifetime_wait_start(llam_shard_t *shard) {
    llam_windows_async_control_t *control = llam_windows_async_control(shard->runtime);

    if (control == NULL) {
        return 0;
    }
    for (;;) {
        unsigned phase = atomic_load_explicit(&control->start_phase,
                                              memory_order_acquire);
        if (phase == LLAM_WINDOWS_ASYNC_START_OPEN) {
            return 0;
        }
        if (phase != LLAM_WINDOWS_ASYNC_START_PREPARING) {
            errno = atomic_load_explicit(&control->start_error, memory_order_acquire);
            return -1;
        }
        Sleep(1U);
    }
}

bool llam_windows_async_lifetime_close_binding(llam_shard_t *shard) {
    llam_windows_async_control_t *control;
    llam_windows_async_binding_t *binding;
    bool closed = true;

    if (shard == NULL ||
        (control = llam_windows_async_control(shard->runtime)) == NULL ||
        (binding = llam_windows_async_worker_binding(control, g_llam_tls_worker)) == NULL) {
        return true;
    }
    llam_windows_async_lock(shard);
    /* The fixed pool stays in service until GLOBAL debt has drained. One M's
     * local empty queue does not justify unbinding ahead of peer finalizers. */
    bool retained = control->managed
        ? (!llam_windows_async_binding_native_idle(binding) ||
           (atomic_load_explicit(&binding->execution_debt, memory_order_acquire) != 0U &&
            !llam_windows_async_terminal_drained(binding)))
        : llam_windows_async_lifetime_has_debt(shard->runtime);
    if (retained) {
        closed = false;
    } else {
        binding->binding_open = false;
        if (control->managed)
            atomic_store_explicit(&binding->worker->async_preempt_state, 0U, memory_order_release);
    }
    llam_windows_async_unlock(shard);
    return closed;
}

void llam_windows_async_lifetime_run_end(llam_runtime_t *rt) {
    llam_windows_async_control_t *control = llam_windows_async_control(rt);
    unsigned i;

    if (control == NULL) {
        return;
    }
    llam_windows_async_controller_exclude(control);
    if (llam_windows_async_lifetime_has_debt(rt)) {
        abort();
    }
    for (i = 0U; i < control->binding_count; ++i) {
        llam_shard_t *shard = control->bindings[i].worker->compat_shard;
        llam_windows_async_binding_t *binding = &control->bindings[i];
        HANDLE handle;

        llam_windows_async_lock(shard);
        if (binding->binding_open) {
            abort(); /* Each actual M closes before its identity is unbound. */
        }
        handle = binding->native_thread;
        binding->native_thread = NULL;
        binding->native_thread_id = 0U;
        binding->native_stack_low = 0U;
        binding->native_stack_high = 0U;
        binding->binding_incarnation = 0U;
        binding->runtime_run = 0U;
        atomic_store_explicit(&binding->probe_phase, 0U, memory_order_release);
        if (control->managed) binding->scheduler = NULL;
        llam_windows_async_unlock(shard);
        if (handle != NULL) {
            llam_windows_async_close_native_handle(control, handle);
        }
    }
    atomic_store_explicit(&control->start_phase,
                          LLAM_WINDOWS_ASYNC_START_CLOSED, memory_order_release);
    atomic_store_explicit(&control->controller_busy, 0U, memory_order_release);
}

void llam_windows_async_lifetime_shutdown(llam_runtime_t *rt) {
    llam_windows_async_control_t *control = llam_windows_async_control(rt);
    size_t i;

    if (rt == NULL) {
        return;
    }
    if (control != NULL) {
        if (atomic_load_explicit(&control->native_handles_created, memory_order_acquire) !=
            atomic_load_explicit(&control->native_handles_closed, memory_order_acquire)) {
            abort();
        }
        if (llam_windows_async_lifetime_has_debt(rt) ||
            atomic_load_explicit(&control->charged_bytes, memory_order_acquire) != 0U) {
            abort();
        }
        rt->managed_capsule_pool = NULL;
        llam_capsule_pool_destroy(&control->capsule_pool);
        for (i = 0U; i < control->binding_count; ++i) {
            if (control->bindings[i].native_thread != NULL) {
                abort();
            }
            llam_windows_async_context_destroy(&control->bindings[i].qualification);
        }
        for (i = 0U; i < control->safe_range_count; ++i) {
            if (control->safe_range_modules[i] != NULL) {
                FreeLibrary(control->safe_range_modules[i]);
            }
        }
        free(control->bindings);
        free(control->safe_range_modules);
    }
    if (control != NULL && control->managed) {
        rt->managed_scheduler_ops = NULL;
        rt->managed_scheduler_data = NULL;
    }
    rt->async_preempt_control = NULL;
    atomic_store_explicit(&rt->async_preempt_enabled, 0U, memory_order_release);
    free(control);
    free(rt->async_preempt_ranges);
    rt->async_preempt_ranges = NULL;
    rt->async_preempt_range_count = 0U;
    rt->async_preempt_range_capacity = 0U;
    rt->windows_async_range_generation = 0U;
}

int llam_windows_async_lifetime_task_init(llam_task_t *task) {
    if (task == NULL || task->owner_runtime == NULL ||
        atomic_load_explicit(&task->owner_runtime->async_preempt_enabled, memory_order_acquire) == 0U ||
        (task->managed_catalog != NULL && task->managed_execution_class == LLAM_MANAGED_EXECUTION_LEGACY))
        return 0;
    llam_windows_async_control_t *control = llam_windows_async_control(task->owner_runtime);
    if (control == NULL) { errno = ENOTSUP; return -1; }
    llam_windows_async_capsule_t *capsule =
        llam_capsule_pool_take(&control->capsule_pool, g_llam_tls_worker == NULL);
    if (capsule == NULL) return -1;
    llam_windows_async_context_buffer_t buffers[2][2];
    for (unsigned i = 0U; i < 2U; ++i) {
        buffers[i][0] = capsule->banks[i].captured;
        buffers[i][1] = capsule->banks[i].redirected;
    }
    memset(capsule, 0, sizeof(*capsule));
    capsule->quota_bytes = control->capsule_pool.charge;
    atomic_init(&capsule->service_worker, NULL);
    atomic_init(&capsule->context_kind, LLAM_WINDOWS_ASYNC_CONTEXT_COOPERATIVE);
    for (unsigned i = 0U; i < 2U; ++i) {
        capsule->banks[i].captured = buffers[i][0];
        capsule->banks[i].redirected = buffers[i][1];
        atomic_init(&capsule->banks[i].phase, LLAM_WINDOWS_ASYNC_BANK_FREE);
        atomic_init(&capsule->banks[i].arrival_complete, 0U);
        capsule->banks[i].owner_task = task;
    }
    atomic_fetch_add_explicit(&control->charged_bytes, capsule->quota_bytes, memory_order_release);
    task->async_preempt_task = capsule;
    return 0;
}

void llam_windows_async_lifetime_task_destroy(llam_task_t *task) {
    llam_windows_async_capsule_t *capsule;
    llam_windows_async_control_t *control;
    unsigned i;

    if (task == NULL || (capsule = task->async_preempt_task) == NULL) {
        return;
    }
    control = llam_windows_async_control(task->owner_runtime);
    if (control == NULL || capsule->origin_enqueued) {
        abort();
    }
    for (i = 0U; i < 2U; ++i) {
        llam_windows_async_bank_t *bank = &capsule->banks[i];
        if (atomic_load_explicit(&bank->phase, memory_order_acquire) !=
                LLAM_WINDOWS_ASYNC_BANK_FREE || bank->external_ops != 0U) {
            abort();
        }
        for (unsigned id = 0U; id < control->binding_count; ++id) {
            llam_windows_async_binding_t *binding = &control->bindings[id];
            if (atomic_load_explicit(&binding->captured_bank, memory_order_acquire) == bank ||
                atomic_load_explicit(&binding->pending_restore, memory_order_acquire) == bank) {
                abort();
            }
        }
    }
    task->async_preempt_task = NULL;
    if (atomic_fetch_sub_explicit(&control->charged_bytes, capsule->quota_bytes,
                                   memory_order_acq_rel) < capsule->quota_bytes) abort();
    llam_capsule_pool_put(&control->capsule_pool, capsule);
}

uint64_t llam_windows_async_lifetime_charged_bytes(const llam_runtime_t *rt) {
    const llam_windows_async_control_t *control;

    if (rt == NULL ||
        atomic_load_explicit(&rt->async_preempt_enabled,
                             memory_order_acquire) == 0U) {
        return 0U;
    }
    control = rt->async_preempt_control;

    return control != NULL
               ? atomic_load_explicit(&control->charged_bytes,
                                      memory_order_acquire)
               : 0U;
}

unsigned llam_async_preempt_ready_depth(const llam_shard_t *shard) {
    llam_windows_async_control_t *control = shard != NULL
        ? llam_windows_async_control(shard->runtime) : NULL;
    if (control == NULL) return 0U;
    if (!control->managed) {
        llam_windows_async_binding_t *binding = llam_windows_async_binding(control, shard->id);
        return binding != NULL ? atomic_load_explicit(&binding->ready_depth, memory_order_acquire) : 0U;
    }
    unsigned depth = 0U;
    for (unsigned i = 0U; i < control->binding_count; ++i) {
        const llam_windows_async_binding_t *binding = &control->bindings[i];
        if (binding->worker->compat_shard != shard) continue;
        unsigned value = atomic_load_explicit(&binding->ready_depth, memory_order_acquire);
        if (value > UINT_MAX - depth) return UINT_MAX;
        depth += value;
    }
    return depth;
}

bool llam_async_preempt_has_origin_obligations(const llam_shard_t *shard) {
    llam_windows_async_control_t *control = shard != NULL
        ? llam_windows_async_control(shard->runtime) : NULL;
    if (control == NULL) return false;
    unsigned begin = control->managed ? 0U : shard->id;
    unsigned end = control->managed ? control->binding_count : shard->id + 1U;
    if (begin >= control->binding_count) return false;
    for (unsigned i = begin; i < end; ++i) {
        const llam_windows_async_binding_t *binding = &control->bindings[i];
        if (binding->worker->compat_shard == shard &&
            (atomic_load_explicit(&binding->ready_depth, memory_order_acquire) != 0U ||
             atomic_load_explicit(&binding->binding_ops, memory_order_acquire) != 0U ||
             atomic_load_explicit(&binding->pending_restore, memory_order_acquire) != NULL ||
             atomic_load_explicit(&binding->captured_bank, memory_order_acquire) != NULL)) return true;
    }
    return false;
}

unsigned llam_async_preempt_task_origin_shard(const llam_task_t *task) {
    /* Ordinary queues remain P-owned; the registered accept seam transfers
     * retained native continuations to M after a successful dequeue. */
    if (task != NULL && task->owner_runtime->execution_lease_mode == LLAM_EXECUTION_LEASE_REGISTERED)
        return UINT_MAX;
    llam_windows_async_capsule_t *capsule = task != NULL ? task->async_preempt_task : NULL;
    llam_worker_t *worker = capsule != NULL
        ? atomic_load_explicit(&capsule->service_worker, memory_order_acquire) : NULL;
    return worker != NULL ? worker->compat_shard->id : UINT_MAX;
}

void llam_async_preempt_bind_task(llam_worker_t *worker, llam_task_t *task) {
    llam_windows_async_control_t *control = llam_windows_async_control(task->owner_runtime);
    llam_windows_async_capsule_t *capsule;
    llam_windows_async_binding_t *binding;
    llam_shard_t *shard;
    llam_worker_t *origin;
    uint64_t incarnation;

    if (control == NULL) {
        return;
    }
    /* The compatibility lane never captures LEGACY code. Its G therefore
     * owns neither a prepared native capsule nor a native service binding. */
    if (control->managed && task->managed_catalog != NULL &&
        task->managed_execution_class == LLAM_MANAGED_EXECUTION_LEGACY) return;
    shard = worker != NULL ? worker->compat_shard : NULL;
    if (shard == NULL || (!control->managed && worker != &shard->primary_worker) ||
        shard->runtime != task->owner_runtime ||
        (capsule = task->async_preempt_task) == NULL ||
        (binding = llam_windows_async_worker_binding(control, worker)) == NULL ||
        atomic_load_explicit(&control->start_phase, memory_order_acquire) !=
            LLAM_WINDOWS_ASYNC_START_OPEN) {
        abort();
    }
    llam_windows_async_lock(shard);
    incarnation = atomic_load_explicit(&worker->binding_incarnation, memory_order_acquire);
    origin = atomic_load_explicit(&capsule->service_worker, memory_order_acquire);
    if (!binding->binding_open || binding->binding_incarnation != incarnation ||
        binding->runtime_run != control->run_generation || capsule->origin_enqueued ||
        !llam_worker_binding_live(worker, incarnation)) {
        abort();
    }
    if (origin == NULL) {
        capsule->service_incarnation = incarnation;
        capsule->service_run = control->run_generation;
        /* Publish the service epoch before active-G and remote wake exposure. */
        atomic_store_explicit(&capsule->service_worker, worker, memory_order_release);
    } else if (origin != worker || capsule->service_incarnation != incarnation ||
               capsule->service_run != control->run_generation) {
        abort(); /* Never execute an origin-owned continuation on a wrong M. */
    }
    capsule->yield_save_run = 0U;
    capsule->source_return_complete = false;
    llam_windows_async_unlock(shard);
}

bool llam_async_preempt_enqueue_locked(llam_shard_t *shard, llam_task_t *task) {
    llam_windows_async_capsule_t *capsule = task->async_preempt_task;
    llam_worker_t *origin = capsule != NULL
        ? atomic_load_explicit(&capsule->service_worker, memory_order_acquire) : NULL;
    llam_windows_async_binding_t *binding;
    unsigned depth;

    if (shard->runtime->execution_lease_mode == LLAM_EXECUTION_LEASE_REGISTERED) return false;
    if (origin == NULL) {
        return false;
    }
    binding = llam_windows_async_binding(llam_windows_async_control(shard->runtime), shard->id);
    if (origin != &shard->primary_worker || binding == NULL || capsule->origin_enqueued ||
        task->owner_runtime != shard->runtime ||
        !llam_worker_binding_live(origin, capsule->service_incarnation)) {
        abort();
    }
    depth = atomic_load_explicit(&binding->ready_depth, memory_order_relaxed);
    if (depth == UINT_MAX) {
        abort();
    }
    task->queue_next = NULL;
    task->queue_prev = binding->ready_tail;
    if (binding->ready_tail != NULL) {
        binding->ready_tail->queue_next = task;
    } else {
        binding->ready_head = task;
    }
    binding->ready_tail = task;
    capsule->origin_enqueued = true;
    atomic_store_explicit(&binding->ready_depth, depth + 1U, memory_order_release);
    return true; /* Eager intrusive storage; no bounded-queue overflow escape. */
}

static void llam_windows_async_enqueue_binding(llam_windows_async_binding_t *binding,
                                                 llam_task_t *task) {
    llam_windows_async_capsule_t *capsule = task->async_preempt_task;
    unsigned depth = atomic_load_explicit(&binding->ready_depth, memory_order_relaxed);
    if (capsule == NULL || capsule->origin_enqueued || depth == UINT_MAX ||
        atomic_load_explicit(&capsule->service_worker, memory_order_acquire) != binding->worker)
        llam_windows_async_uncertain_transaction();
    task->queue_next = NULL;
    task->queue_prev = binding->ready_tail;
    if (binding->ready_tail != NULL) binding->ready_tail->queue_next = task;
    else binding->ready_head = task;
    binding->ready_tail = task;
    capsule->origin_enqueued = true;
    atomic_store_explicit(&binding->ready_depth, depth + 1U, memory_order_release);
}

static void llam_windows_async_remove_ready_locked(llam_windows_async_binding_t *binding,
                                                    llam_task_t *task) {
    llam_windows_async_capsule_t *capsule = task->async_preempt_task;
    unsigned depth = atomic_load_explicit(&binding->ready_depth, memory_order_relaxed);

    if (capsule == NULL || !capsule->origin_enqueued || depth == 0U) {
        abort();
    }
    if (task->queue_prev != NULL) {
        task->queue_prev->queue_next = task->queue_next;
    } else {
        binding->ready_head = task->queue_next;
    }
    if (task->queue_next != NULL) {
        task->queue_next->queue_prev = task->queue_prev;
    } else {
        binding->ready_tail = task->queue_prev;
    }
    task->queue_next = NULL;
    task->queue_prev = NULL;
    capsule->origin_enqueued = false;
    atomic_store_explicit(&binding->ready_depth, depth - 1U, memory_order_release);
}

llam_task_t *llam_async_preempt_take_ready(llam_shard_t *shard, bool fallback) {
    llam_windows_async_binding_t *binding = llam_windows_async_worker_binding(
        llam_windows_async_control(shard->runtime), g_llam_tls_worker);
    llam_task_t *task;

    if (binding == NULL || g_llam_tls_worker != binding->worker ||
        atomic_load_explicit(&binding->ready_depth, memory_order_acquire) == 0U) {
        return NULL;
    }
    if (!fallback && !binding->prefer_origin) {
        binding->prefer_origin = true;
        return NULL;
    }
    llam_windows_async_lock(shard);
    task = binding->ready_head;
    if (task != NULL && atomic_load_explicit(&task->managed_dispatch_owner, memory_order_acquire) != NULL)
        task = NULL;
    if (task != NULL) {
        llam_windows_async_capsule_t *capsule = task->async_preempt_task;
        if (capsule == NULL) llam_windows_async_uncertain_transaction();
        if (atomic_load_explicit(&binding->pending_restore, memory_order_acquire) != NULL) {
            for (unsigned i = 0U; i < 2U; ++i) {
                if (atomic_load_explicit(&capsule->banks[i].phase, memory_order_acquire) ==
                        LLAM_WINDOWS_ASYNC_BANK_READY) {
                    /* The old observed operation clears this M slot and wakes
                     * us. Do not overwrite it or wait with either lock held. */
                    task = NULL;
                    goto ready_done;
                }
            }
        }
        if (llam_windows_async_task_binding(llam_windows_async_control(shard->runtime), task) != binding ||
            !llam_worker_binding_live(binding->worker, binding->binding_incarnation)) {
            abort();
        }
        llam_windows_async_remove_ready_locked(binding, task);
        binding->prefer_origin = false;
    }
ready_done:
    llam_windows_async_unlock(shard);
    return task;
}

void llam_windows_async_lifetime_prepare_yield(llam_task_t *task) {
    llam_windows_async_capsule_t *capsule = task->async_preempt_task;
    if (capsule != NULL && task->io_handoff_depth == 0U &&
        task->windows_async_internal_yield_depth == 0U &&
        g_llam_tls_worker != NULL &&
        atomic_load_explicit(&g_llam_tls_worker->runtime_critical_depth, memory_order_acquire) == 0U &&
        atomic_load_explicit(&g_llam_tls_worker->preempt_disable_depth, memory_order_acquire) == 0U) {
        llam_shard_t *shard = g_llam_tls_shard;
        llam_windows_async_lock(shard);
        capsule->yield_save_run = atomic_load_explicit(&task->run_generation, memory_order_acquire);
        capsule->source_return_complete = false;
        llam_windows_async_unlock(shard);
    }
}

/* Called with the source pair gate. The fresh context is published only at
 * the scheduler's LAST task access, never from yield before ctx_switch saves it.
 * A controller finishing an older bank may perform this publication later. */
static bool llam_windows_async_release_epoch_locked(llam_shard_t *shard,
                                                     llam_task_t *task) {
    if (shard->runtime->execution_lease_mode == LLAM_EXECUTION_LEASE_REGISTERED) return false;
    llam_windows_async_capsule_t *capsule = task->async_preempt_task;
    llam_windows_async_binding_t *binding = llam_windows_async_binding(
        llam_windows_async_control(shard->runtime), shard->id);

    if (capsule == NULL || binding == NULL || !capsule->source_return_complete ||
        atomic_load_explicit(&capsule->context_kind, memory_order_acquire) !=
            LLAM_WINDOWS_ASYNC_CONTEXT_COOPERATIVE ||
        capsule->yield_save_run == 0U ||
        capsule->yield_save_run != atomic_load_explicit(&task->run_generation, memory_order_acquire) ||
        atomic_load_explicit(&capsule->service_worker, memory_order_acquire) != &shard->primary_worker ||
        !capsule->origin_enqueued || task->state != LLAM_TASK_STATE_RUNNABLE ||
        task->wait_reason != LLAM_WAIT_NONE ||
        llam_worker_load_active_g(&shard->primary_worker) == task ||
        (task->flags & LLAM_TASK_FLAG_PINNED) != 0U ||
        atomic_load_explicit(&task->opaque_blocking_depth, memory_order_acquire) != 0U) {
        return false;
    }
    for (unsigned i = 0U; i < 2U; ++i) {
        llam_windows_async_bank_t *bank = &capsule->banks[i];
        if (atomic_load_explicit(&bank->phase, memory_order_acquire) != LLAM_WINDOWS_ASYNC_BANK_FREE ||
            bank->external_ops != 0U ||
            atomic_load_explicit(&binding->captured_bank, memory_order_acquire) == bank ||
            atomic_load_explicit(&binding->pending_restore, memory_order_acquire) == bank) {
            return false;
        }
    }
    llam_windows_async_remove_ready_locked(binding, task);
    capsule->yield_save_run = 0U;
    capsule->source_return_complete = false;
    /* Under the same gate as capture admission: no old-M capture can overlap
     * this one migratable READY publication or the destination's new epoch. */
    atomic_store_explicit(&capsule->service_worker, NULL, memory_order_release);
    (void)llam_queue_push_bounded_locked(shard, &shard->processor.inject_q,
                                         LLAM_INJECT_QUEUE_CAP, task, NULL);
    llam_kick_shard(shard);
    return true;
}

void llam_windows_async_lifetime_cooperative_handoff(llam_shard_t *shard,
                                                     llam_task_t *task) {
    bool released;
    bool saved_yield;
    if (task->async_preempt_task == NULL) {
        return;
    }
    llam_windows_async_lock(shard);
    ((llam_windows_async_capsule_t *)task->async_preempt_task)->source_return_complete = true;
    saved_yield = ((llam_windows_async_capsule_t *)task->async_preempt_task)->yield_save_run != 0U;
    released = llam_windows_async_release_epoch_locked(shard, task);
    llam_windows_async_unlock(shard);
    /* No task access: another M can already have resumed and retired it. */
#if defined(LLAM_ENABLE_TEST_HOOKS)
    if (released) {
        llam_windows_async_lifetime_test_hold_phase(
            task, LLAM_WINDOWS_ASYNC_TEST_COOPERATIVE_RELEASED);
    } else if (saved_yield) {
        llam_windows_async_lifetime_test_hold_phase(
            task, LLAM_WINDOWS_ASYNC_TEST_COOPERATIVE_SAVED);
    }
#else
    (void)released;
    (void)saved_yield;
#endif
}

void llam_windows_async_lifetime_first_dispatch(llam_worker_t *worker,
                                                llam_task_t *task) {
    unsigned expected = 0U;

    if (worker == NULL || task == NULL) {
        return;
    }
    if (task->async_preempt_task == NULL) {
        /* Pre-run enable and host-spawn admission must never publish a task
         * lacking its eager capsule into an enabled runtime. */
        if (task->managed_catalog != NULL &&
            task->managed_execution_class == LLAM_MANAGED_EXECUTION_LEGACY) return;
        if (atomic_load_explicit(&task->owner_runtime->async_preempt_enabled,
                                 memory_order_acquire) != 0U) {
            abort();
        }
        return;
    }
    if (atomic_compare_exchange_strong_explicit(
            &task->windows_async_execution_owed, &expected, 1U,
            memory_order_acq_rel, memory_order_acquire)) {
        llam_windows_async_control_t *control = llam_windows_async_control(task->owner_runtime);
        llam_windows_async_binding_t *binding = llam_windows_async_worker_binding(control, worker);
        if (binding == NULL) abort();
        task->windows_async_debt_owner = control->managed ? binding->index : worker->compat_shard->id;
        task->windows_async_debt_run =
            llam_windows_async_control(task->owner_runtime)->run_generation;
        /* This release precedes active-G publication and the last-live exit. */
        atomic_uint *debt = control->managed ? &binding->execution_debt
            : &worker->compat_shard->windows_async_execution_debt;
        (void)atomic_fetch_add_explicit(debt, 1U, memory_order_release);
    }
}

bool llam_windows_async_lifetime_has_debt(const llam_runtime_t *rt) {
    const llam_windows_async_control_t *control;
    unsigned i;

    if (rt == NULL || atomic_load_explicit(&rt->async_preempt_enabled,
                                           memory_order_acquire) == 0U ||
        (control = rt->async_preempt_control) == NULL) {
        return false;
    }
    for (i = 0U; i < control->binding_count; ++i) {
        /* ALL immutable execution-owner debts are inspected before ANY
         * finalizer/operation roots. A terminal M publishes its counted
         * finalizer before release-discharging a different debt owner. The
         * acquire of that discharge orders the later finalizer inspection;
         * an interleaved per-slot scan could otherwise report a false zero.
         * Close is used only after last-live stop closes new G execution. */
        if (atomic_load_explicit(control->managed ? &control->bindings[i].execution_debt : &rt->shards[i].windows_async_execution_debt,
                                 memory_order_acquire) != 0U) {
            return true;
        }
    }
    for (i = 0U; i < control->binding_count; ++i) {
        const llam_windows_async_binding_t *binding = &control->bindings[i];
        const llam_shard_t *shard = binding->worker->compat_shard;

        if (atomic_load_explicit(control->managed ? &binding->retire_count : &shard->windows_async_retire_count, memory_order_acquire) != 0U ||
            atomic_load_explicit(control->managed ? &binding->finalizer_count : &shard->windows_async_finalizer_count, memory_order_acquire) != 0U ||
            atomic_load_explicit(&binding->ready_depth, memory_order_acquire) != 0U ||
            atomic_load_explicit(&binding->binding_ops, memory_order_acquire) != 0U ||
            atomic_load_explicit(&binding->captured_bank, memory_order_acquire) != NULL ||
            atomic_load_explicit(&binding->pending_restore, memory_order_acquire) != NULL) {
            return true;
        }
    }
    return false;
}

static bool llam_windows_async_op_begin_on(llam_runtime_t *rt, unsigned id,
                                          llam_windows_async_operation_t *op) {
    llam_windows_async_control_t *control;
    llam_windows_async_binding_t *binding;
    llam_shard_t *origin;
    llam_worker_t *worker;
    llam_task_t *task;
    llam_windows_async_capsule_t *capsule;
    llam_windows_async_bank_t *bank = NULL;
    uint64_t incarnation;
    llam_processor_owner_ticket_t ticket = {0};
    unsigned i;

    if (rt == NULL || op == NULL ||
        atomic_load_explicit(&rt->async_preempt_enabled,
                             memory_order_acquire) == 0U) {
        errno = EINVAL;
        return false;
    }
    control = llam_windows_async_control(rt);
    if (control == NULL ||
        (binding = llam_windows_async_binding(control, id)) == NULL) {
        errno = EINVAL;
        return false;
    }
    memset(op, 0, sizeof(*op));
    worker = binding->worker;
    origin = worker->compat_shard;
    llam_windows_async_lock(origin);
    origin = llam_windows_async_worker_shard(worker);
    task = llam_worker_load_active_g(worker);
    incarnation = atomic_load_explicit(&worker->binding_incarnation,
                                       memory_order_acquire);
    if (atomic_load_explicit(&control->start_phase, memory_order_acquire) !=
            LLAM_WINDOWS_ASYNC_START_OPEN ||
        !binding->binding_open || binding->native_thread == NULL ||
        (!control->managed && (atomic_load_explicit(&rt->stop_requested,
                             memory_order_acquire) ||
         atomic_load_explicit(&rt->shutdown_requested, memory_order_acquire))) ||
        atomic_load_explicit(&binding->binding_ops,
                             memory_order_acquire) != 0U ||
        atomic_load_explicit(&binding->captured_bank,
                             memory_order_acquire) != NULL ||
        atomic_load_explicit(&binding->pending_restore,
                             memory_order_acquire) != NULL ||
        incarnation == 0U || task == NULL ||
        !llam_worker_has_safe_managed_g(worker, &origin->processor, task) ||
        (control->managed && task->managed_execution_class == LLAM_MANAGED_EXECUTION_LEGACY &&
         task->managed_catalog != NULL) ||
        (task->flags & LLAM_TASK_FLAG_NO_PREEMPT) != 0U ||
        atomic_load_explicit(&task->opaque_blocking_depth,
                             memory_order_acquire) != 0U ||
        atomic_load_explicit(&task->windows_async_execution_owed,
                             memory_order_acquire) == 0U ||
        (capsule = task->async_preempt_task) == NULL ||
        atomic_load_explicit(&capsule->service_worker, memory_order_acquire) != worker ||
        capsule->service_incarnation != incarnation ||
        capsule->service_run != control->run_generation ||
        capsule->admission_closed || capsule->next_generation == UINT64_MAX ||
        (binding->binding_incarnation != 0U &&
         binding->binding_incarnation != incarnation)) {
        errno = EBUSY;
        goto rejected;
    }
    if (control->managed && !llam_windows_async_service_ticket(binding, &ticket)) {
        errno = EBUSY;
        goto rejected;
    }
    for (i = 0U; i < 2U; ++i) {
        unsigned index = (unsigned)((capsule->next_generation + i) & 1U);

        if (capsule->banks[index].phase == LLAM_WINDOWS_ASYNC_BANK_FREE &&
            capsule->banks[index].external_ops == 0U) {
            bank = &capsule->banks[index];
            break;
        }
    }
    if (bank == NULL ||
        !llam_task_scan_ref_try_acquire(rt, task)) {
        errno = bank == NULL ? EBUSY : errno;
        goto rejected;
    }
    binding->binding_incarnation = incarnation;
    (void)atomic_fetch_add_explicit(&binding->binding_ops, 1U,
                                    memory_order_release);
    bank->generation = ++capsule->next_generation;
    bank->capture_worker = worker;
    bank->capture_binding_incarnation = incarnation;
    bank->capture_task_run = atomic_load_explicit(&task->run_generation,
                                                  memory_order_acquire);
    bank->capture_worker_run = atomic_load_explicit(&worker->run_generation,
                                                    memory_order_acquire);
    bank->capture_scheduler_rsp = binding->scheduler->rsp;
    bank->ticket = ticket;
    atomic_store_explicit(&bank->arrival_complete, 0U,
                          memory_order_relaxed);
    bank->external_ops = 1U;
    atomic_store_explicit(&bank->phase, LLAM_WINDOWS_ASYNC_BANK_CAPTURING,
                          memory_order_release);
    op->runtime = rt;
    op->shard = origin;
    op->worker = worker;
    op->task = task;
    op->bank = bank;
    op->native_thread = binding->native_thread;
    op->binding_incarnation = incarnation;
    op->bank_generation = bank->generation;
    op->range_generation = control->safe_range_generation;
    op->capture_task_run = bank->capture_task_run;
    op->capture_worker_run = bank->capture_worker_run;
    op->runtime_run = control->run_generation;
    op->kind = LLAM_WINDOWS_ASYNC_OP_CAPTURE;
    llam_windows_async_unlock(origin);
    if (!llam_windows_async_context_reset(&bank->captured,
                                          control->xstate_features) ||
        !llam_windows_async_context_reset(&bank->redirected,
                                          control->xstate_features)) {
        llam_windows_async_lifetime_op_abort(op);
        errno = ENOTSUP;
        return false;
    }
    return true;

rejected:
    llam_windows_async_unlock(origin);
    return false;
}

#if defined(LLAM_ENABLE_TEST_HOOKS)
int llam_windows_async_test_capture_admission(llam_task_t *task) {
    llam_windows_async_control_t *control = task != NULL ? llam_windows_async_control(task->owner_runtime) : NULL;
    llam_windows_async_binding_t *binding = llam_windows_async_worker_binding(control, g_llam_tls_worker);
    unsigned expected = 0U;
    if (control == NULL || !control->managed || binding == NULL || task != g_llam_tls_task) {
        errno = EINVAL;
        return -1;
    }
    if (!atomic_compare_exchange_strong_explicit(&control->controller_busy, &expected, 1U,
                                                  memory_order_acquire, memory_order_relaxed)) {
        errno = EAGAIN;
        return -1;
    }
    /* Exercise the production publication gate, without suspending any M.
     * If an erroneous admission succeeds, retire its exact reserved bank and
     * scan/binding references before reporting the regression to the test. */
    llam_windows_async_operation_t operation;
    bool admitted = llam_windows_async_op_begin_on(task->owner_runtime, binding->index, &operation);
    if (admitted) llam_windows_async_lifetime_op_abort(&operation);
    atomic_store_explicit(&control->controller_busy, 0U, memory_order_release);
    return admitted ? 1 : 0;
}
#endif

bool llam_windows_async_lifetime_op_begin(llam_runtime_t *rt,
                                          llam_windows_async_operation_t *op) {
    return llam_windows_async_op_begin_on(rt, 0U, op);
}

static bool llam_windows_async_stopped_tuple(const llam_windows_async_operation_t *op,
                                              bool restoring) {
    const llam_windows_async_control_t *control = llam_windows_async_control(op->runtime);
    const llam_processor_owner_ticket_t *ticket = &op->bank->ticket;
    if (!control->managed) return true;
    llam_worker_t *worker = op->worker;
    llam_task_t *task = op->task;
    return ticket->owner_m == worker && ticket->worker_incarnation == op->binding_incarnation &&
        atomic_load_explicit(&worker->current_p, memory_order_acquire) == ticket->processor &&
        atomic_load_explicit(&worker->async_preempt_ticket_lease_epoch, memory_order_acquire) == ticket->lease_projection_epoch &&
        atomic_load_explicit(&worker->async_preempt_ticket_generation, memory_order_acquire) == ticket->p_generation &&
        atomic_load_explicit(&worker->run_generation, memory_order_acquire) ==
            (restoring ? op->bank->restore_worker_run : op->capture_worker_run) &&
        atomic_load_explicit(&task->run_generation, memory_order_acquire) ==
            (restoring ? op->bank->restore_task_run : op->capture_task_run) &&
        atomic_load_explicit(&task->state, memory_order_acquire) == LLAM_TASK_STATE_RUNNING &&
        atomic_load_explicit(&worker->runtime_critical_depth, memory_order_acquire) == 0U &&
        atomic_load_explicit(&worker->preempt_disable_depth, memory_order_acquire) == 0U &&
        atomic_load_explicit(&worker->native_domain_depth, memory_order_acquire) == 0U &&
        atomic_load_explicit(&worker->execution_state, memory_order_acquire) == LLAM_WORKER_EXEC_MANAGED_SAFE &&
        atomic_load_explicit(&task->opaque_blocking_depth, memory_order_acquire) == 0U;
}

static DWORD llam_windows_async_remote_suspend(llam_windows_async_control_t *control,
                                                HANDLE thread) {
    if (atomic_fetch_add_explicit(&control->remote_transactions, 1U,
                                  memory_order_acq_rel) != 0U) {
        llam_windows_async_uncertain_transaction();
    }
    atomic_store_explicit(&control->remote_transactions_max, 1U, memory_order_release);
    return SuspendThread(thread);
}

static DWORD llam_windows_async_remote_resume(llam_windows_async_control_t *control,
                                               HANDLE thread) {
    DWORD previous = ResumeThread(thread);
    if (atomic_fetch_sub_explicit(&control->remote_transactions, 1U,
                                  memory_order_release) != 1U) {
        llam_windows_async_uncertain_transaction();
    }
    return previous;
}

static llam_windows_async_capture_result_t
llam_windows_async_try_capture_worker(llam_runtime_t *rt, unsigned id,
                                                llam_windows_async_operation_t *op) {
    llam_windows_async_control_t *control = llam_windows_async_control(rt);
    llam_windows_async_binding_t *binding = llam_windows_async_binding(control, id);
    llam_windows_async_context_buffer_t *captured;
    llam_windows_async_context_buffer_t *redirected;
    uintptr_t rsp;
    uintptr_t stack_base;
    DWORD64 valid_features = 0U;
    DWORD previous;

    if (control == NULL || binding == NULL || op == NULL ||
        !llam_windows_async_op_begin_on(rt, id, op)) {
        return LLAM_WINDOWS_ASYNC_CAPTURE_RETRY_LATER;
    }
    captured = &op->bank->captured;
    redirected = &op->bank->redirected;
    (void)atomic_fetch_add_explicit(&control->capture_attempts, 1U,
                                    memory_order_relaxed);
    previous = llam_windows_async_remote_suspend(control, op->native_thread);
    if (previous == (DWORD)-1) {
        llam_windows_async_uncertain_transaction();
    }
    if (previous != 0U) {
        if (previous == (DWORD)-2 ||
            llam_windows_async_remote_resume(control, op->native_thread) != previous + 1U) {
            llam_windows_async_uncertain_transaction();
        }
        llam_windows_async_lifetime_op_abort(op);
        return LLAM_WINDOWS_ASYNC_CAPTURE_RETRY_LATER;
    }
    if (!GetThreadContext(op->native_thread, captured->context)) {
        if (llam_windows_async_remote_resume(control, op->native_thread) != 1U) {
            llam_windows_async_uncertain_transaction();
        }
        llam_windows_async_lifetime_op_abort(op);
        return LLAM_WINDOWS_ASYNC_CAPTURE_RETRY_LATER;
    }
    if (!llam_windows_async_lifetime_pc_allowed(
            op, (uintptr_t)captured->context->Rip)) {
        (void)atomic_fetch_add_explicit(&control->unsafe_pc_refusals, 1U,
                                        memory_order_relaxed);
        if (llam_windows_async_remote_resume(control, op->native_thread) != 1U) {
            llam_windows_async_uncertain_transaction();
        }
        llam_windows_async_lifetime_op_abort(op);
        return LLAM_WINDOWS_ASYNC_CAPTURE_NOT_ELIGIBLE;
    }
    rsp = (uintptr_t)captured->context->Rsp;
    stack_base = (uintptr_t)op->task->stack_base;
    if (!llam_windows_async_stopped_tuple(op, false) || stack_base == 0U || rsp < stack_base ||
        rsp - stack_base >= op->task->stack_size ||
        binding->scheduler->rsp != op->bank->capture_scheduler_rsp ||
        op->bank->capture_scheduler_rsp < binding->native_stack_low ||
        op->bank->capture_scheduler_rsp >= binding->native_stack_high ||
        binding->scheduler->simd_flags != 0U ||
        atomic_load_explicit(&op->worker->native_domain_depth, memory_order_acquire) != 0U ||
        llam_worker_load_active_g(op->worker) != op->task ||
        atomic_load_explicit(&op->worker->execution_state,
                             memory_order_acquire) != LLAM_WORKER_EXEC_MANAGED_SAFE ||
        atomic_load_explicit(&op->worker->binding_incarnation,
                             memory_order_acquire) != op->binding_incarnation ||
        atomic_load_explicit(&op->task->run_generation,
                             memory_order_acquire) != op->capture_task_run ||
        atomic_load_explicit(&op->worker->run_generation,
                             memory_order_acquire) != op->capture_worker_run ||
        !GetXStateFeaturesMask(captured->context, &valid_features) ||
        (valid_features & ~control->xstate_features) != 0U ||
        !CopyContext(redirected->context,
                     captured->context->ContextFlags,
                     captured->context)) {
        if (llam_windows_async_remote_resume(control, op->native_thread) != 1U) {
            llam_windows_async_uncertain_transaction();
        }
        llam_windows_async_lifetime_op_abort(op);
        return LLAM_WINDOWS_ASYNC_CAPTURE_RETRY_LATER;
    }
    redirected->context->Rip =
        (DWORD64)(uintptr_t)llam_windows_async_restore_scheduler;
    redirected->context->Rcx =
        (DWORD64)(uintptr_t)binding->scheduler;
    redirected->context->EFlags &= ~0x400U;
    if (!llam_windows_async_lifetime_op_capture_committed(op)) {
        if (llam_windows_async_remote_resume(control, op->native_thread) != 1U) {
            llam_windows_async_uncertain_transaction();
        }
        llam_windows_async_lifetime_op_abort(op);
        return LLAM_WINDOWS_ASYNC_CAPTURE_RETRY_LATER;
    }
    if (!SetThreadContext(op->native_thread, redirected->context)) {
        llam_windows_async_uncertain_transaction();
    }
#if defined(LLAM_ENABLE_TEST_HOOKS)
    llam_windows_async_test_record(control, LLAM_WINDOWS_ASYNC_TEST_CAPTURE,
                                   op, op->task, op->bank_generation);
#endif
    if (llam_windows_async_remote_resume(control, op->native_thread) != 1U) {
        llam_windows_async_uncertain_transaction();
    }
    (void)atomic_fetch_add_explicit(&control->captures, 1U,
                                    memory_order_relaxed);
    return LLAM_WINDOWS_ASYNC_CAPTURED;
}

llam_windows_async_capture_result_t
llam_windows_async_lifetime_try_capture_primary(llam_runtime_t *rt,
                                                llam_windows_async_operation_t *op) {
    return llam_windows_async_try_capture_worker(rt, 0U, op);
}

/* Accepted restore service can survive shutdown revocation, but never an
 * unrelated G dispatch or M binding. Refresh only its lease projection while
 * the actual target runs; no runtime locks may be taken after suspension. */
static bool llam_windows_async_refresh_service(llam_windows_async_operation_t *op) {
    llam_windows_async_control_t *control = llam_windows_async_control(op->runtime);
    if (control == NULL) return false;
    if (!control->managed) return true;
    llam_windows_async_binding_t *binding = llam_windows_async_worker_binding(control, op->worker);
    llam_shard_t *home = op->worker->compat_shard;
    llam_processor_owner_ticket_t ticket;
    bool current;
    llam_windows_async_lock(home);
    current = binding != NULL && op->runtime_run == control->run_generation &&
        op->binding_incarnation == binding->binding_incarnation &&
        op->bank->generation == op->bank_generation &&
        llam_worker_load_active_g(op->worker) == op->task &&
        atomic_load_explicit(&op->task->run_generation, memory_order_acquire) == op->bank->restore_task_run &&
        atomic_load_explicit(&op->worker->run_generation, memory_order_acquire) == op->bank->restore_worker_run &&
        llam_windows_async_service_ticket(binding, &ticket);
    if (current) {
        op->bank->ticket = ticket;
        op->shard = ticket.processor->compat_shard;
    }
    llam_windows_async_unlock(home);
    return current;
}

bool llam_windows_async_lifetime_try_restore(llam_windows_async_operation_t *op) {
    llam_windows_async_control_t *control;
    llam_windows_async_binding_t *binding;
    llam_windows_async_context_buffer_t *gate;
    uintptr_t rip;
    uintptr_t rsp;
    uint64_t task_run;
    uint64_t worker_run;
    DWORD previous;

    if (op == NULL || op->bank == NULL ||
        (control = llam_windows_async_control(op->runtime)) == NULL ||
        op->shard == NULL ||
        (binding = llam_windows_async_worker_binding(control, op->worker)) == NULL ||
        atomic_load_explicit(&op->bank->phase,
                             memory_order_acquire) != LLAM_WINDOWS_ASYNC_BANK_GATE) {
        return false;
    }
#if defined(LLAM_ENABLE_TEST_HOOKS)
    llam_windows_async_lifetime_test_hold_phase(
        op->task, LLAM_WINDOWS_ASYNC_TEST_BEFORE_RESTORE);
#endif
    if (!llam_windows_async_refresh_service(op)) return false;
    gate = &op->bank->redirected;
    previous = llam_windows_async_remote_suspend(control, op->native_thread);
    if (previous == (DWORD)-1) {
        llam_windows_async_uncertain_transaction();
    }
    if (previous != 0U) {
        if (previous == (DWORD)-2 ||
            llam_windows_async_remote_resume(control, op->native_thread) != previous + 1U) {
            llam_windows_async_uncertain_transaction();
        }
        return false;
    }
    if (!GetThreadContext(op->native_thread, gate->context)) {
        if (llam_windows_async_remote_resume(control, op->native_thread) != 1U) {
            llam_windows_async_uncertain_transaction();
        }
        return false;
    }
    rip = (uintptr_t)gate->context->Rip;
    rsp = (uintptr_t)gate->context->Rsp;
    task_run = atomic_load_explicit(&op->task->run_generation,
                                    memory_order_acquire);
    worker_run = atomic_load_explicit(&op->worker->run_generation,
                                      memory_order_acquire);
    if (rip < (uintptr_t)llam_windows_async_gate_spin_begin ||
        rip >= (uintptr_t)llam_windows_async_gate_spin_end) {
        if (llam_windows_async_remote_resume(control, op->native_thread) != 1U) {
            llam_windows_async_uncertain_transaction();
        }
        return false;
    }
    if (!llam_windows_async_stopped_tuple(op, true)) {
        if (llam_windows_async_remote_resume(control, op->native_thread) != 1U)
            llam_windows_async_uncertain_transaction();
        return false;
    }
    if (rsp < binding->native_stack_low ||
        rsp >= binding->native_stack_high ||
        binding->scheduler->rsp != rsp ||
        op->bank->generation != op->bank_generation ||
        llam_worker_load_active_g(op->worker) != op->task ||
        atomic_load_explicit(&op->worker->binding_incarnation,
                             memory_order_acquire) != op->binding_incarnation ||
        task_run <= op->capture_task_run ||
        worker_run <= op->capture_worker_run) {
        llam_windows_async_uncertain_transaction();
    }
    if (!llam_windows_async_lifetime_op_restoring(op, task_run, worker_run)) {
        /* Shutdown may update the lease projection after the stopped check.
         * No context write occurred, so preserve GATE and retry with a fresh
         * service tuple after balancing this exact suspend increment. */
        if (llam_windows_async_remote_resume(control, op->native_thread) != 1U)
            llam_windows_async_uncertain_transaction();
        return false;
    }
    if (!SetThreadContext(op->native_thread,
                          op->bank->captured.context)) {
        llam_windows_async_uncertain_transaction();
    }
#if defined(LLAM_ENABLE_TEST_HOOKS)
    llam_windows_async_test_record(control, LLAM_WINDOWS_ASYNC_TEST_RESTORE,
                                   op, op->task, op->bank_generation);
#endif
    if (llam_windows_async_remote_resume(control, op->native_thread) != 1U) {
        llam_windows_async_uncertain_transaction();
    }
    (void)atomic_fetch_add_explicit(&control->restores, 1U,
                                    memory_order_relaxed);
#if defined(LLAM_ENABLE_TEST_HOOKS)
    llam_windows_async_lifetime_test_hold_phase(
        op->task, LLAM_WINDOWS_ASYNC_TEST_AFTER_RESTORE);
#endif
    return true;
}

static bool llam_windows_async_op_matches(const llam_windows_async_operation_t *op) {
    const llam_windows_async_capsule_t *capsule;

    if (op == NULL || op->task == NULL || op->bank == NULL ||
        (op->kind != LLAM_WINDOWS_ASYNC_OP_CAPTURE && op->kind != LLAM_WINDOWS_ASYNC_OP_RESTORE) ||
        (capsule = op->task->async_preempt_task) == NULL ||
        op->bank->capture_worker != op->worker ||
        op->bank->capture_binding_incarnation != op->binding_incarnation ||
        atomic_load_explicit(&capsule->service_worker, memory_order_acquire) != op->worker ||
        capsule->service_run != op->runtime_run ||
        op->bank_generation != op->bank->generation ||
        op->bank->external_ops != 1U) {
        return false;
    }
    return op->bank == &capsule->banks[0] || op->bank == &capsule->banks[1];
}

#if defined(LLAM_ENABLE_TEST_HOOKS)
bool llam_windows_async_lifetime_test_operation_matches(const llam_windows_async_operation_t *op) {
    return llam_windows_async_op_matches(op);
}

bool llam_windows_async_lifetime_test_copy_operation(llam_runtime_t *rt, unsigned id,
                                                     llam_windows_async_operation_t *out) {
    llam_windows_async_control_t *control = llam_windows_async_control(rt);
    llam_windows_async_binding_t *binding = llam_windows_async_binding(control, id);
    llam_shard_t *shard;
    bool copied = false;

    if (binding == NULL || out == NULL) {
        return false;
    }
    shard = binding->worker->compat_shard;
    llam_windows_async_lock(shard);
    /* Caller holds the actual capture controller before handoff. READY/GATE
     * remains pinned; this is a copied identity, not a new operation pin. */
    if (binding->operation.bank != NULL &&
        (atomic_load_explicit(&binding->operation.bank->phase, memory_order_acquire) == LLAM_WINDOWS_ASYNC_BANK_READY ||
         atomic_load_explicit(&binding->operation.bank->phase, memory_order_acquire) == LLAM_WINDOWS_ASYNC_BANK_GATE)) {
        *out = binding->operation;
        copied = true;
    }
    llam_windows_async_unlock(shard);
    return copied;
}
#endif

bool llam_windows_async_lifetime_op_capture_committed(llam_windows_async_operation_t *op) {
    llam_windows_async_control_t *control;
    llam_windows_async_binding_t *binding;
    llam_windows_async_bank_t *expected_bank = NULL;
    unsigned expected = LLAM_WINDOWS_ASYNC_BANK_CAPTURING;

    if (op == NULL || op->shard == NULL ||
        (control = llam_windows_async_control(op->runtime)) == NULL ||
        op->shard == NULL ||
        (binding = llam_windows_async_worker_binding(control, op->worker)) == NULL) {
        return false;
    }
    /* The target is suspended here: no lock or allocation may be taken. */
    if (!llam_windows_async_stopped_tuple(op, false) || !llam_windows_async_op_matches(op) ||
        op->runtime_run != control->run_generation ||
        binding->runtime_run != op->runtime_run ||
        llam_worker_load_active_g(op->worker) != op->task ||
        atomic_load_explicit(&op->worker->binding_incarnation,
                             memory_order_acquire) != op->binding_incarnation ||
        atomic_load_explicit(&op->task->run_generation,
                             memory_order_acquire) != op->capture_task_run ||
        atomic_load_explicit(&op->worker->run_generation,
                             memory_order_acquire) != op->capture_worker_run ||
        !llam_windows_async_lifetime_pc_allowed(
            op, (uintptr_t)op->bank->captured.context->Rip) ||
        !atomic_compare_exchange_strong_explicit(
            &binding->captured_bank, &expected_bank, op->bank,
            memory_order_acq_rel, memory_order_acquire)) {
        return false;
    }
    /* Release publishes committed ownership before remote context mutation. */
    if (!atomic_compare_exchange_strong_explicit(
            &op->bank->phase, &expected, LLAM_WINDOWS_ASYNC_BANK_CAPTURED,
            memory_order_release, memory_order_acquire)) {
        expected_bank = op->bank;
        (void)atomic_compare_exchange_strong_explicit(
            &binding->captured_bank, &expected_bank, NULL,
            memory_order_acq_rel, memory_order_acquire);
        return false;
    }
    atomic_store_explicit(
        &((llam_windows_async_capsule_t *)op->task->async_preempt_task)->context_kind,
        LLAM_WINDOWS_ASYNC_CONTEXT_REMOTE, memory_order_release);
    return true;
}

bool llam_windows_async_lifetime_op_restoring(llam_windows_async_operation_t *op,
                                              uint64_t task_run,
                                              uint64_t worker_run) {
    llam_windows_async_control_t *control;
    llam_windows_async_binding_t *binding;
    llam_windows_async_bank_t *expected_bank = NULL;
    unsigned expected_phase = LLAM_WINDOWS_ASYNC_BANK_GATE;

    if (op == NULL || op->shard == NULL ||
        (control = llam_windows_async_control(op->runtime)) == NULL ||
        op->shard == NULL ||
        (binding = llam_windows_async_worker_binding(control, op->worker)) == NULL) {
        return false;
    }
    /* Called after gate suspension; operation ownership keeps all storage live. */
    if (!llam_windows_async_stopped_tuple(op, true) || !llam_windows_async_op_matches(op) || !binding->binding_open ||
        atomic_load_explicit(&op->bank->phase, memory_order_acquire) != LLAM_WINDOWS_ASYNC_BANK_GATE ||
        op->runtime_run != control->run_generation ||
        binding->runtime_run != op->runtime_run ||
        binding->binding_incarnation != op->binding_incarnation ||
        llam_worker_load_active_g(op->worker) != op->task ||
        task_run <= op->capture_task_run ||
        worker_run <= op->capture_worker_run ||
        atomic_load_explicit(&op->task->run_generation,
                             memory_order_acquire) != task_run ||
        atomic_load_explicit(&op->worker->run_generation,
                             memory_order_acquire) != worker_run ||
        !(atomic_compare_exchange_strong_explicit(
            &binding->pending_restore, &expected_bank, op->bank,
            memory_order_acq_rel, memory_order_acquire) || expected_bank == op->bank)) {
        return false;
    }
    /* This bank generation owns exactly one fresh save-and-gate activation.
     * begin_g_run increments both run generations before active-G publication,
     * so a later dispatch cannot reuse this identity even if native RSP recurs. */
    op->bank->restore_task_run = task_run;
    op->bank->restore_worker_run = worker_run;
    op->bank->restore_scheduler_rsp = binding->scheduler->rsp;
    if (!atomic_compare_exchange_strong_explicit(
            &op->bank->phase, &expected_phase,
            LLAM_WINDOWS_ASYNC_BANK_RESTORING,
            memory_order_release, memory_order_acquire)) {
        if (expected_bank == NULL) {
            /* Only withdraw a slot installed by this call. A pre-published
             * M-owned GATE root must survive a retry without operation refs. */
            expected_bank = op->bank;
            (void)atomic_compare_exchange_strong_explicit(
                &binding->pending_restore, &expected_bank, NULL,
                memory_order_acq_rel, memory_order_acquire);
        }
        return false;
    }
    return true;
}

/*
 * A restored no-yield G cannot acknowledge its bank through scheduler_return.
 * Observe the exact restored processor context without mutating it. The bank
 * stays pinned while the M is suspended; op_finish retires it only after the
 * controller has balanced that suspension and completed all bank reads.
 */
bool llam_windows_async_lifetime_try_observe_running(
    llam_windows_async_operation_t *op) {
    llam_windows_async_control_t *control;
    llam_windows_async_binding_t *binding;
    llam_windows_async_context_buffer_t *observed;
    uintptr_t rip;
    uintptr_t rsp;
    uintptr_t stack_base;
    unsigned expected = LLAM_WINDOWS_ASYNC_BANK_RESTORING;
    DWORD previous;
    bool witnessed;

    if (op == NULL || op->bank == NULL || op->task == NULL ||
        (control = llam_windows_async_control(op->runtime)) == NULL ||
        op->shard == NULL ||
        (binding = llam_windows_async_worker_binding(control, op->worker)) == NULL ||
        atomic_load_explicit(&op->bank->phase,
                             memory_order_acquire) !=
            LLAM_WINDOWS_ASYNC_BANK_RESTORING) {
        return false;
    }
    if (!llam_windows_async_refresh_service(op)) return false;
    observed = &op->bank->redirected;
    if (!llam_windows_async_context_reset(observed,
                                          control->xstate_features)) {
        return false;
    }
    previous = llam_windows_async_remote_suspend(control, op->native_thread);
    if (previous == (DWORD)-1) {
        llam_windows_async_uncertain_transaction();
    }
    if (previous != 0U) {
        if (previous == (DWORD)-2 ||
            llam_windows_async_remote_resume(control, op->native_thread) != previous + 1U) {
            llam_windows_async_uncertain_transaction();
        }
        return false;
    }
    if (!GetThreadContext(op->native_thread, observed->context)) {
        if (llam_windows_async_remote_resume(control, op->native_thread) != 1U) {
            llam_windows_async_uncertain_transaction();
        }
        return false;
    }
    rip = (uintptr_t)observed->context->Rip;
    rsp = (uintptr_t)observed->context->Rsp;
    stack_base = (uintptr_t)op->task->stack_base;
    witnessed = llam_windows_async_stopped_tuple(op, true) && atomic_load_explicit(&op->worker->native_domain_depth, memory_order_acquire) == 0U &&
        stack_base != 0U && rsp >= stack_base &&
                rsp - stack_base < op->task->stack_size &&
                (rsp < binding->native_stack_low ||
                 rsp >= binding->native_stack_high) &&
                /* This is observation, not capture: certified RIP is not
                 * required. Exact G-stack/lease state proves the old remote
                 * restore landed, even if G moved to an unsafe code region. */
                rip != 0U &&
                (rip < (uintptr_t)llam_windows_async_restore_scheduler ||
                 rip >= (uintptr_t)llam_windows_async_restore_scheduler_end) &&
                (rip < (uintptr_t)llam_windows_async_gate_spin_begin ||
                 rip >= (uintptr_t)llam_windows_async_gate_spin_end) &&
                op->range_generation == control->safe_range_generation &&
                op->runtime_run == control->run_generation &&
                binding->runtime_run == op->runtime_run &&
                llam_windows_async_op_matches(op) &&
                atomic_load_explicit(&binding->pending_restore,
                                     memory_order_acquire) == op->bank &&
                binding->binding_open &&
                binding->binding_incarnation == op->binding_incarnation &&
                llam_worker_has_safe_managed_g(op->worker,
                                               &op->shard->processor,
                                               op->task) &&
                atomic_load_explicit(&op->worker->binding_incarnation,
                                     memory_order_acquire) ==
                    op->binding_incarnation &&
                atomic_load_explicit(&op->task->run_generation,
                                     memory_order_acquire) ==
                    op->bank->restore_task_run &&
                atomic_load_explicit(&op->worker->run_generation,
                                     memory_order_acquire) ==
                    op->bank->restore_worker_run &&
                /* RSP is a structural check; the exact pending bank plus
                 * task/worker dispatch generations are the identity proof. */
                op->bank->restore_scheduler_rsp != 0U &&
                op->bank->restore_scheduler_rsp ==
                    binding->scheduler->rsp &&
                atomic_compare_exchange_strong_explicit(
                    &op->bank->phase, &expected,
                    LLAM_WINDOWS_ASYNC_BANK_OBSERVED,
                    memory_order_acq_rel, memory_order_acquire);
#if defined(LLAM_ENABLE_TEST_HOOKS)
    if (witnessed) {
        llam_windows_async_test_record(control,
                                       LLAM_WINDOWS_ASYNC_TEST_WITNESS,
                                       op, op->task, op->bank_generation);
    }
#endif
    if (llam_windows_async_remote_resume(control, op->native_thread) != 1U) {
        llam_windows_async_uncertain_transaction();
    }
    if (witnessed) {
        (void)atomic_fetch_add_explicit(&control->direct_witnesses, 1U,
                                        memory_order_relaxed);
    }
    return witnessed;
}

uint64_t llam_windows_async_effective_quantum(const llam_runtime_t *rt) {
    if (rt == NULL) return 0U;
    return rt->preempt_quantum_ns < UINT64_C(5000000)
        ? UINT64_C(5000000) : rt->preempt_quantum_ns;
}

static uint64_t llam_windows_async_next_probe(uint64_t now_ns, const llam_runtime_t *rt) {
    uint64_t interval = llam_windows_async_effective_quantum(rt);
    return now_ns > UINT64_MAX - interval ? UINT64_MAX : now_ns + interval;
}

bool llam_windows_async_lifetime_slot_pending(llam_runtime_t *rt, unsigned id) {
    llam_windows_async_binding_t *binding = llam_windows_async_binding(
        llam_windows_async_control(rt), id);
    return binding != NULL &&
        (atomic_load_explicit(&binding->binding_ops, memory_order_acquire) != 0U ||
         atomic_load_explicit(&binding->captured_bank, memory_order_acquire) != NULL ||
         atomic_load_explicit(&binding->pending_restore, memory_order_acquire) != NULL);
}

#if defined(LLAM_ENABLE_TEST_HOOKS)
static bool llam_windows_async_test_protocol_idle(llam_runtime_t *rt) {
    llam_windows_async_control_t *control = llam_windows_async_control(rt);
    for (unsigned id = 0U; id < control->binding_count; ++id) {
        llam_windows_async_binding_t *binding = &control->bindings[id];
        llam_shard_t *shard = binding->worker->compat_shard;
        bool idle;
        llam_windows_async_lock(shard);
        idle = binding->operation.shard == NULL &&
            atomic_load_explicit(&binding->binding_ops, memory_order_acquire) == 0U &&
            atomic_load_explicit(&binding->captured_bank, memory_order_acquire) == NULL &&
            atomic_load_explicit(&binding->pending_restore, memory_order_acquire) == NULL &&
            atomic_load_explicit(&binding->test_ready_contexts, memory_order_acquire) == 0U;
        llam_windows_async_unlock(shard);
        if (!idle) return false;
    }
    return true;
}

static bool llam_windows_async_test_repeat_coverage(const llam_windows_async_control_t *control) {
    unsigned count = 0U;
    while (count < LLAM_WINDOWS_ASYNC_TEST_TRACE_CAPACITY &&
           atomic_load_explicit(&control->test_trace_ready[count], memory_order_acquire) != 0U) ++count;
    /* Inject only after the SAME capture/peer/restore/witness condition checked
     * by the final oracle. Witness totals alone can include a cycle in which
     * the yielding peer had not yet observed the published capture prefix. */
    for (unsigned id = 0U; id < control->binding_count; ++id) {
        unsigned chains = 0U;
        for (unsigned i = 0U; i < count; ++i) {
            const llam_windows_async_test_event_t *capture = &control->test_trace[i];
            bool peer = false, restored = false;
            if (capture->runtime_run != control->run_generation || capture->shard_id != id ||
                capture->kind != LLAM_WINDOWS_ASYNC_TEST_CAPTURE) continue;
            for (unsigned j = i + 1U; j < count; ++j) {
                const llam_windows_async_test_event_t *event = &control->test_trace[j];
                if (event->runtime_run != capture->runtime_run || event->shard_id != id ||
                    event->bank_generation != capture->bank_generation) continue;
                if (event->kind == LLAM_WINDOWS_ASYNC_TEST_PEER && event->task_key != capture->task_key) {
                    peer = true;
                }
                if (event->task_key != capture->task_key || event->bank_key != capture->bank_key) continue;
                if (peer && event->kind == LLAM_WINDOWS_ASYNC_TEST_RESTORE) restored = true;
                if (restored && event->kind == LLAM_WINDOWS_ASYNC_TEST_WITNESS) {
                    ++chains;
                    break;
                }
            }
        }
        if (chains < 2U) return false;
    }
    return true;
}
#endif

/* A completed capture transfers to the G-owned READY/GATE continuation, not
 * FREE. Its native payload and execution debt survive without an idle M op. */
static void llam_windows_async_capture_handoff(llam_windows_async_operation_t *op) {
    llam_runtime_t *rt = op->runtime;
    llam_shard_t *origin = op->shard;
    llam_task_t *task = op->task;
    llam_windows_async_control_t *control = llam_windows_async_control(rt);
    llam_windows_async_binding_t *binding = llam_windows_async_worker_binding(control, op->worker);
    unsigned phase;

#if defined(LLAM_ENABLE_TEST_HOOKS)
    llam_windows_async_lifetime_test_hold_phase(task, LLAM_WINDOWS_ASYNC_TEST_BEFORE_CAPTURE_HANDOFF);
#endif
    llam_windows_async_lock(origin);
    phase = atomic_load_explicit(&op->bank->phase, memory_order_acquire);
    if (op->kind != LLAM_WINDOWS_ASYNC_OP_CAPTURE || !llam_windows_async_op_matches(op) ||
        (phase != LLAM_WINDOWS_ASYNC_BANK_READY && phase != LLAM_WINDOWS_ASYNC_BANK_GATE) ||
        atomic_load_explicit(&op->bank->arrival_complete, memory_order_acquire) != 1U ||
        atomic_load_explicit(&binding->captured_bank, memory_order_acquire) == op->bank) {
        llam_windows_async_uncertain_transaction();
    }
    op->bank->external_ops = 0U; /* LAST bank access; never change its phase. */
    op->bank = NULL;
    op->task = NULL;
    llam_windows_async_unlock(origin);
    (void)llam_task_scan_ref_release(rt, task); /* LAST task access. */
    llam_windows_async_lock(origin);
    if (atomic_fetch_sub_explicit(&binding->binding_ops, 1U, memory_order_release) != 1U) {
        llam_windows_async_uncertain_transaction();
    }
    llam_kick_shard(origin); /* Final binding use and zero publication share close exclusion. */
    llam_windows_async_unlock(origin);
    op->shard = NULL;
}

/* The M-owned pending descriptor roots G before references are reacquired.
 * GATE publication follows assembly's fresh scheduler save. This is accepted
 * service even after stop; it is not new capture admission. */
static bool llam_windows_async_restore_begin(llam_runtime_t *rt, unsigned id,
                                              llam_windows_async_operation_t *op) {
    llam_windows_async_control_t *control = llam_windows_async_control(rt);
    llam_windows_async_binding_t *binding = llam_windows_async_binding(control, id);
    llam_worker_t *worker = binding != NULL ? binding->worker : NULL;
    llam_shard_t *origin = worker != NULL ? worker->compat_shard : NULL;
    llam_windows_async_bank_t *bank;
    llam_windows_async_capsule_t *capsule;
    llam_task_t *task;
    bool admitted = false;

    if (binding == NULL || op->shard != NULL) return false;
    llam_windows_async_lock(origin);
    bank = atomic_load_explicit(&binding->pending_restore, memory_order_acquire);
    if (bank == NULL || !binding->binding_open || binding->native_thread == NULL ||
        atomic_load_explicit(&binding->binding_ops, memory_order_acquire) != 0U ||
        atomic_load_explicit(&bank->phase, memory_order_acquire) != LLAM_WINDOWS_ASYNC_BANK_GATE ||
        bank->external_ops != 0U) goto done;
    if (control->managed) origin = bank->ticket.processor->compat_shard;
    task = bank->owner_task;
    capsule = task->async_preempt_task;
    if (capsule == NULL || bank->capture_worker != worker ||
        bank->capture_binding_incarnation != binding->binding_incarnation ||
        !llam_worker_binding_live(worker, binding->binding_incarnation) ||
        atomic_load_explicit(&capsule->service_worker, memory_order_acquire) != worker ||
        capsule->service_run != control->run_generation ||
        binding->runtime_run != control->run_generation ||
        llam_worker_load_active_g(worker) != task ||
        atomic_load_explicit(&task->run_generation, memory_order_acquire) != bank->restore_task_run ||
        atomic_load_explicit(&worker->run_generation, memory_order_acquire) != bank->restore_worker_run ||
        !llam_task_scan_ref_try_acquire(rt, task)) goto done;
    bank->external_ops = 1U;
    atomic_fetch_add_explicit(&binding->binding_ops, 1U, memory_order_release);
    *op = (llam_windows_async_operation_t){
        .runtime = rt, .shard = origin, .worker = worker, .task = task, .bank = bank,
        .native_thread = binding->native_thread,
        .binding_incarnation = binding->binding_incarnation,
        .bank_generation = bank->generation,
        .range_generation = control->safe_range_generation,
        .capture_task_run = bank->capture_task_run,
        .capture_worker_run = bank->capture_worker_run,
        .runtime_run = control->run_generation,
        .kind = LLAM_WINDOWS_ASYNC_OP_RESTORE
    };
    admitted = true;
done:
    llam_windows_async_unlock(origin);
    return admitted;
}

/* The target is still on its native stack behind the all-M startup barrier.
 * Probe the actual duplicated thread handle before any G may execute. The
 * binding operation remains counted through balanced suspend/resume. */
static void llam_windows_async_probe_binding(llam_runtime_t *rt,
                                             llam_windows_async_binding_t *binding) {
    llam_windows_async_control_t *control = llam_windows_async_control(rt);
    llam_shard_t *home = binding->worker->compat_shard;
    bool valid;
    DWORD64 features = 0U;
    llam_windows_async_lock(home);
    bool restarting = atomic_load_explicit(&binding->restart_pending, memory_order_acquire);
    unsigned phase = atomic_load_explicit(&control->start_phase, memory_order_acquire);
    if ((phase != LLAM_WINDOWS_ASYNC_START_PREPARING &&
         !(restarting && phase == LLAM_WINDOWS_ASYNC_START_OPEN)) || !binding->binding_open ||
        atomic_load_explicit(&binding->probe_phase, memory_order_acquire) != 1U) {
        llam_windows_async_unlock(home);
        return;
    }
    atomic_fetch_add_explicit(&binding->binding_ops, 1U, memory_order_release);
    llam_windows_async_unlock(home);
    DWORD previous = llam_windows_async_remote_suspend(control, binding->native_thread);
    if (previous == (DWORD)-1) llam_windows_async_uncertain_transaction();
    valid = previous == 0U;
    if (valid) {
        valid = GetThreadContext(binding->native_thread, binding->qualification.context) &&
            GetXStateFeaturesMask(binding->qualification.context, &features) &&
            (features & ~control->xstate_features) == 0U &&
            binding->qualification.context->Rsp >= binding->native_stack_low &&
            binding->qualification.context->Rsp < binding->native_stack_high;
        /* An unchanged context still exercises this M's native restore API.
         * A failed write has ambiguous mutation status and cannot be retried. */
        if (valid && !SetThreadContext(binding->native_thread, binding->qualification.context))
            llam_windows_async_uncertain_transaction();
    }
    if (previous == (DWORD)-2 ||
        llam_windows_async_remote_resume(control, binding->native_thread) != previous + 1U)
        llam_windows_async_uncertain_transaction();
    llam_windows_async_lock(home);
    atomic_store_explicit(&binding->probe_error, valid ? 0 : ENOTSUP, memory_order_relaxed);
    atomic_store_explicit(&binding->probe_phase, valid ? 2U : 3U, memory_order_release);
    if (atomic_fetch_sub_explicit(&binding->binding_ops, 1U, memory_order_release) != 1U) abort();
    llam_windows_async_unlock(home);
    if (!valid && !restarting) llam_windows_async_lifetime_run_abort(rt, ENOTSUP);
}

static bool llam_windows_async_runnable_pressure(llam_runtime_t *rt,
                                                 const llam_windows_async_control_t *control) {
    if (rt->preempt_mode == LLAM_PREEMPT_STRICT || llam_runtime_has_runnable_backlog(rt)) return true;
    if (!control->managed) return false;
    if (llam_managed_foreign_return_pending(rt)) return true;
    for (unsigned i = 0U; i < control->binding_count; ++i)
        if (atomic_load_explicit(&control->bindings[i].ready_depth, memory_order_acquire) != 0U)
            return true;
    for (unsigned i = 0U; i < rt->active_shards; ++i)
        if (atomic_load_explicit(&rt->shards[i].processor.timer_count, memory_order_acquire) != 0U)
            return true;
    return atomic_load_explicit(&rt->block_pending, memory_order_acquire) != 0U;
}

/* One serialized controller services short capture transactions and fresh
 * restore operations. READY Gs never monopolize another running G's M slot. */
bool llam_windows_async_lifetime_controller_tick(llam_runtime_t *rt, uint64_t now_ns) {
    llam_windows_async_control_t *control = llam_windows_async_control(rt);
    unsigned expected = 0U;
    unsigned probes = 0U;
    bool pending = false;

    if (control == NULL || !atomic_compare_exchange_strong_explicit(
            &control->controller_busy, &expected, 1U,
            memory_order_acquire, memory_order_relaxed)) {
        return false;
    }
    unsigned start_phase = atomic_load_explicit(&control->start_phase, memory_order_acquire);
    if (control->managed && (start_phase == LLAM_WINDOWS_ASYNC_START_PREPARING ||
                             start_phase == LLAM_WINDOWS_ASYNC_START_OPEN)) {
        for (unsigned i = 0U; i < control->binding_count; ++i)
            llam_windows_async_probe_binding(rt, &control->bindings[i]);
    }
    if (atomic_load_explicit(&control->start_phase, memory_order_acquire) !=
            LLAM_WINDOWS_ASYNC_START_OPEN) {
        atomic_store_explicit(&control->controller_busy, 0U, memory_order_release);
        return false;
    }
    for (unsigned offset = 0U; offset < control->binding_count; ++offset) {
        unsigned id = (control->controller_cursor + offset) % control->binding_count;
        llam_windows_async_binding_t *binding = &control->bindings[id];
        llam_windows_async_operation_t *op = &binding->operation;
        unsigned phase;

        if (op->shard == NULL && !llam_windows_async_restore_begin(rt, id, op)) continue;
        if (op->bank == NULL) {
            abort();
        }
        phase = atomic_load_explicit(&op->bank->phase, memory_order_acquire);
        if (op->kind == LLAM_WINDOWS_ASYNC_OP_CAPTURE &&
            (phase == LLAM_WINDOWS_ASYNC_BANK_READY || phase == LLAM_WINDOWS_ASYNC_BANK_GATE)) {
            llam_windows_async_capture_handoff(op);
            if (!llam_windows_async_restore_begin(rt, id, op)) continue;
            phase = atomic_load_explicit(&op->bank->phase, memory_order_acquire);
        }
        if (phase == LLAM_WINDOWS_ASYNC_BANK_OBSERVED) {
            llam_windows_async_lifetime_op_finish(op);
            binding->next_probe_ns = llam_windows_async_next_probe(now_ns, rt);
        } else if (phase == LLAM_WINDOWS_ASYNC_BANK_GATE) {
            if (llam_windows_async_lifetime_try_restore(op)) {
                binding->next_probe_ns = llam_windows_async_next_probe(now_ns, rt);
            }
        } else if (phase == LLAM_WINDOWS_ASYNC_BANK_RESTORING &&
                   now_ns >= binding->next_probe_ns) {
            binding->next_probe_ns = llam_windows_async_next_probe(now_ns, rt);
            if (llam_windows_async_lifetime_try_observe_running(op)) {
                llam_windows_async_lifetime_op_finish(op);
            }
        }
    }
    if ((control->managed || (!atomic_load_explicit(&rt->stop_requested, memory_order_acquire) &&
        !atomic_load_explicit(&rt->shutdown_requested, memory_order_acquire) &&
        atomic_load_explicit(&rt->deferred_fatal_pending, memory_order_acquire) == 0U)) &&
        (control->safe_range_count != 0U || rt->managed_catalog != NULL) && rt->preempt_mode >= LLAM_PREEMPT_AUTO &&
        llam_windows_async_runnable_pressure(rt, control)) {
        for (unsigned offset = 0U; offset < control->binding_count && probes < 2U; ++offset) {
            unsigned id = (control->controller_cursor + offset) % control->binding_count;
            llam_windows_async_binding_t *binding = &control->bindings[id];

            if (binding->operation.shard == NULL && now_ns >= binding->next_probe_ns) {
                ++probes; /* Refused/unsafe attempts consume the same bounded budget. */
                binding->next_probe_ns = llam_windows_async_next_probe(now_ns, rt);
                (void)llam_windows_async_try_capture_worker(rt, id, &binding->operation);
            }
        }
    }
    control->controller_cursor = (control->controller_cursor + 1U) % control->binding_count;
    pending = llam_windows_async_lifetime_has_debt(rt);
#if defined(LLAM_ENABLE_TEST_HOOKS)
    /* Arm after repeated automatic service, but before natural drain can win
     * the run-end gate. Finite Gs can finish without further controller work. */
    if (atomic_load_explicit(&g_windows_async_tail_runtime, memory_order_acquire) == rt &&
        llam_windows_async_test_repeat_coverage(control) &&
        llam_windows_async_test_protocol_idle(rt)) {
        llam_windows_async_test_hold_tail(rt);
    }
#endif
    atomic_store_explicit(&control->controller_busy, 0U, memory_order_release);
    return pending;
}

bool llam_windows_async_lifetime_stats(const llam_runtime_t *rt,
                                      llam_windows_async_stats_t *out) {
    const llam_windows_async_control_t *control;

    if (rt == NULL || out == NULL ||
        atomic_load_explicit(&rt->async_preempt_enabled,
                             memory_order_acquire) == 0U) {
        return false;
    }
    control = rt->async_preempt_control;
    if (control == NULL) {
        return false;
    }
    out->capture_attempts = atomic_load_explicit(
        &control->capture_attempts, memory_order_relaxed);
    out->captures = atomic_load_explicit(&control->captures,
                                         memory_order_relaxed);
    out->unsafe_pc_refusals = atomic_load_explicit(
        &control->unsafe_pc_refusals, memory_order_relaxed);
    out->restores = atomic_load_explicit(&control->restores,
                                         memory_order_relaxed);
    out->direct_witnesses = atomic_load_explicit(
        &control->direct_witnesses, memory_order_relaxed);
    out->remote_transactions_max = atomic_load_explicit(
        &control->remote_transactions_max, memory_order_acquire);
    out->native_handles_created = atomic_load_explicit(
        &control->native_handles_created, memory_order_acquire);
    out->native_handles_closed = atomic_load_explicit(
        &control->native_handles_closed, memory_order_acquire);
#if defined(LLAM_ENABLE_TEST_HOOKS)
    out->scheduler_error_checks = atomic_load_explicit(
        &control->test_error_checks, memory_order_acquire);
#else
    out->scheduler_error_checks = 0U;
#endif
    out->binding_ops = 0U;
    out->captured_bank_active = 0U;
    out->pending_restore_active = 0U;
    out->execution_debt = 0U;
    out->retire_count = 0U;
    out->finalizer_count = 0U;
    for (unsigned i = 0U; i < control->binding_count; ++i) {
        const llam_windows_async_binding_t *binding = &control->bindings[i];
        const llam_shard_t *shard = binding->worker->compat_shard;

        out->binding_ops += atomic_load_explicit(&binding->binding_ops, memory_order_acquire);
        out->captured_bank_active += atomic_load_explicit(&binding->captured_bank, memory_order_acquire) != NULL;
        out->pending_restore_active += atomic_load_explicit(&binding->pending_restore, memory_order_acquire) != NULL;
        out->execution_debt += atomic_load_explicit(control->managed ? &binding->execution_debt : &shard->windows_async_execution_debt, memory_order_acquire);
        out->retire_count += atomic_load_explicit(control->managed ? &binding->retire_count : &shard->windows_async_retire_count, memory_order_acquire);
        out->finalizer_count += atomic_load_explicit(control->managed ? &binding->finalizer_count : &shard->windows_async_finalizer_count, memory_order_acquire);
    }
    return true;
}

void llam_windows_async_lifetime_op_abort(llam_windows_async_operation_t *op) {
    bool safe = false;

    if (op == NULL || op->shard == NULL) {
        return;
    }
    if (atomic_load_explicit(&op->bank->phase,
                             memory_order_acquire) !=
        LLAM_WINDOWS_ASYNC_BANK_CAPTURING) {
        llam_windows_async_uncertain_transaction();
    }
    llam_windows_async_lock(op->shard);
    if (llam_windows_async_op_matches(op) &&
        op->bank->phase == LLAM_WINDOWS_ASYNC_BANK_CAPTURING) {
        /*
         * FREE is advisory until op_finish drops external_ops to zero.
         * Both admission and destruction require FREE && zero under this gate.
         */
        op->bank->phase = LLAM_WINDOWS_ASYNC_BANK_FREE;
        safe = true;
    }
    llam_windows_async_unlock(op->shard);
    if (safe) {
        llam_windows_async_lifetime_op_finish(op);
    } else {
        /* A committed remote context change is never a pre-mutation refusal. */
        llam_windows_async_uncertain_transaction();
    }
}

void llam_windows_async_lifetime_op_finish(llam_windows_async_operation_t *op) {
    llam_windows_async_control_t *control;
    llam_windows_async_binding_t *binding;
    llam_runtime_t *rt;
    llam_shard_t *origin;
    llam_task_t *task;
    bool released = false;
    bool epoch_released = false;
    bool was_observed;

    if (op == NULL || op->shard == NULL ||
        (control = llam_windows_async_control(op->runtime)) == NULL ||
        op->shard == NULL ||
        (binding = llam_windows_async_worker_binding(control, op->worker)) == NULL) {
        return;
    }
    if (atomic_load_explicit(&op->bank->phase,
                             memory_order_acquire) !=
            LLAM_WINDOWS_ASYNC_BANK_FREE &&
        atomic_load_explicit(&op->bank->phase,
                             memory_order_acquire) !=
            LLAM_WINDOWS_ASYNC_BANK_OBSERVED) {
        /* No lock is taken even if the target is still suspended. */
        llam_windows_async_uncertain_transaction();
    }
    rt = op->runtime;
    origin = op->shard;
    task = op->task;
    was_observed = atomic_load_explicit(&op->bank->phase, memory_order_acquire) ==
                       LLAM_WINDOWS_ASYNC_BANK_OBSERVED;
#if defined(LLAM_ENABLE_TEST_HOOKS)
    if (atomic_load_explicit(&op->bank->phase, memory_order_acquire) ==
            LLAM_WINDOWS_ASYNC_BANK_OBSERVED) {
        llam_windows_async_lifetime_test_hold_phase(
            task, LLAM_WINDOWS_ASYNC_TEST_BEFORE_OP_FINISH);
    }
#endif
    llam_windows_async_lock(origin);
    if (llam_windows_async_op_matches(op) &&
        (op->bank->phase == LLAM_WINDOWS_ASYNC_BANK_FREE ||
         op->bank->phase == LLAM_WINDOWS_ASYNC_BANK_OBSERVED)) {
        if (op->bank->phase == LLAM_WINDOWS_ASYNC_BANK_OBSERVED) {
            llam_windows_async_bank_t *pending = op->bank;

            if (!atomic_compare_exchange_strong_explicit(
                    &binding->pending_restore, &pending, NULL,
                    memory_order_acq_rel, memory_order_acquire)) {
                llam_windows_async_unlock(origin);
                llam_windows_async_uncertain_transaction();
            }
            /* No context path reads this generation after its exact witness. */
            atomic_store_explicit(&op->bank->phase,
                                  LLAM_WINDOWS_ASYNC_BANK_FREE,
                                  memory_order_release);
        }
        op->bank->external_ops = 0U;
        released = true;
    }
    if (released) {
        op->bank = NULL;
        op->task = NULL;
        epoch_released = llam_windows_async_release_epoch_locked(origin, task);
    }
    llam_windows_async_unlock(origin);
    if (!released) {
        /* Keep every reference and bank reserved on an unresolved mutation. */
        llam_windows_async_uncertain_transaction();
    }
    /* No further bank access follows the external-reference release. */
#if defined(LLAM_ENABLE_TEST_HOOKS)
    if (epoch_released) {
        llam_windows_async_lifetime_test_hold_phase(
            task, LLAM_WINDOWS_ASYNC_TEST_BEFORE_SCAN_RELEASE);
    } else if (was_observed) {
        llam_windows_async_lifetime_test_hold_phase(
            task, LLAM_WINDOWS_ASYNC_TEST_AFTER_BANK_FREE);
    }
#else
    (void)epoch_released;
    (void)was_observed;
#endif
    (void)llam_task_scan_ref_release(rt, task);
    /* No further task access follows the scan-reference release. */
    llam_windows_async_lock(origin);
    if (atomic_load_explicit(&binding->binding_ops,
                             memory_order_acquire) == 0U) {
        abort();
    }
    (void)atomic_fetch_sub_explicit(&binding->binding_ops, 1U,
                                    memory_order_release);
    /*
     * The zero predicate and SetEvent belong to one binding-gate transaction.
     * close_binding cannot retire the M/wake storage until this notification
     * is complete, and the M cannot recheck zero before the event is armed.
     */
    llam_kick_shard(origin);
    llam_windows_async_unlock(origin);
    op->shard = NULL;
}

bool llam_windows_async_lifetime_arrival(llam_task_t *task) {
    llam_windows_async_control_t *control;
    llam_windows_async_binding_t *binding;
    llam_windows_async_bank_t *bank;
    llam_worker_t *worker = g_llam_tls_worker;
    llam_shard_t *shard = g_llam_tls_shard;

    if (task == NULL || shard == NULL || worker == NULL ||
        (control = llam_windows_async_control(shard->runtime)) == NULL ||
        (binding = llam_windows_async_worker_binding(control, worker)) == NULL ||
        (bank = atomic_load_explicit(&binding->captured_bank,
                                     memory_order_acquire)) == NULL) {
        return false;
    }
    if (bank->owner_task != task || bank->capture_worker != worker ||
        atomic_load_explicit(&bank->phase,
                             memory_order_acquire) !=
            LLAM_WINDOWS_ASYNC_BANK_CAPTURED ||
        bank->capture_binding_incarnation != atomic_load_explicit(
            &worker->binding_incarnation, memory_order_acquire) ||
        bank->capture_task_run != atomic_load_explicit(
            &task->run_generation, memory_order_acquire) ||
        bank->capture_worker_run != atomic_load_explicit(
            &worker->run_generation, memory_order_acquire) ||
        bank->capture_scheduler_rsp != binding->scheduler->rsp ||
        atomic_load_explicit(&bank->arrival_complete,
                             memory_order_acquire) != 0U) {
        llam_windows_async_uncertain_transaction();
    }
    /* First native-return seam: preserve target-local state before errno.c
     * restores the scheduler's errno/LastError. The operation ref pins bank. */
    bank->captured_last_error = GetLastError();
    task->windows_async_saved_last_error = bank->captured_last_error;
    bank->captured_errno = llam_thread_errno_load();
    task->saved_errno = bank->captured_errno;
    atomic_store_explicit(&bank->arrival_complete, 1U,
                          memory_order_release);
    return true;
}

bool llam_windows_async_lifetime_resume_task(llam_ctx_t *scheduler_ctx,
                                             llam_task_t *task) {
    llam_windows_async_capsule_t *capsule;
    llam_windows_async_bank_t *bank = NULL;
    llam_worker_t *worker = g_llam_tls_worker;
    llam_shard_t *shard = g_llam_tls_shard;
    llam_windows_async_binding_t *binding;
    int captured_errno;
    DWORD captured_last_error;
    unsigned i;

    if (scheduler_ctx == NULL || task == NULL || worker == NULL || shard == NULL ||
        (capsule = task->async_preempt_task) == NULL) {
        return false;
    }
    /* Only this origin M publishes READY. Its executing G owns the capsule,
     * so this atomic pre-scan cannot race capsule destruction. Ordinary
     * cooperative dispatch takes no locks that could clobber G-local errors. */
    for (i = 0U; i < 2U; ++i) {
        if (atomic_load_explicit(&capsule->banks[i].phase,
                                 memory_order_acquire) ==
            LLAM_WINDOWS_ASYNC_BANK_READY) {
            bank = &capsule->banks[i];
            break;
        }
    }
    if (bank == NULL) {
        return false;
    }
    binding = llam_windows_async_worker_binding(llam_windows_async_control(task->owner_runtime), worker);
    if (binding == NULL) llam_windows_async_uncertain_transaction();
    llam_windows_async_lock(shard);
    if (bank->owner_task != task || bank->capture_worker != worker ||
        atomic_load_explicit(&bank->phase, memory_order_acquire) != LLAM_WINDOWS_ASYNC_BANK_READY ||
        bank->external_ops > 1U ||
        atomic_load_explicit(&binding->pending_restore, memory_order_acquire) != NULL ||
        bank->capture_task_run >= atomic_load_explicit(
            &task->run_generation, memory_order_acquire) ||
        bank->capture_worker_run >= atomic_load_explicit(
            &worker->run_generation, memory_order_acquire)) {
        llam_windows_async_uncertain_transaction();
    }
    bank->restore_task_run = atomic_load_explicit(&task->run_generation, memory_order_acquire);
    bank->restore_worker_run = atomic_load_explicit(&worker->run_generation, memory_order_acquire);
    if (task->owner_runtime->execution_lease_mode == LLAM_EXECUTION_LEASE_REGISTERED) {
        while (!llam_windows_async_service_ticket(binding, &bank->ticket)) {
            if (!atomic_load_explicit(&task->owner_runtime->shutdown_requested, memory_order_acquire) ||
                shard->legacy_lease.state == LLAM_LEASE_STOPPING) llam_windows_async_uncertain_transaction();
            /* Shutdown revokes normal authority before its locked STOPPING
             * projection. No gate/native mutation has occurred; let destroy
             * publish the drain tuple instead of treating this gap as fatal. */
            llam_windows_async_unlock(shard);
            Sleep(0U);
            llam_windows_async_lock(shard);
        }
    }
    captured_errno = bank->captured_errno;
    captured_last_error = bank->captured_last_error;
    /* G owns this persistent descriptor independently of a controller op.
     * Assembly's GATE release publishes the fresh M continuation afterward. */
    atomic_store_explicit(&binding->pending_restore, bank, memory_order_release);
#if defined(LLAM_ENABLE_TEST_HOOKS)
    /* Replace the observed READY root only after its pending-GATE root exists.
     * This counter lets the old-sweep test distinguish protocol-idle from
     * zero execution debt; it does not retain the G or authorize close. */
    if (atomic_fetch_sub_explicit(&binding->test_ready_contexts, 1U, memory_order_release) == 0U) {
        llam_windows_async_uncertain_transaction();
    }
#endif
    llam_windows_async_unlock(shard);
    llam_thread_errno_store(captured_errno);
    SetLastError(captured_last_error);
    /* Assembly publishes GATE only after a fresh live scheduler save. */
    llam_windows_async_save_gate(scheduler_ctx, &bank->phase);
    return true; /* Only reached after the restored G returns genuinely. */
}

void llam_windows_async_lifetime_scheduler_return(llam_shard_t *shard,
                                                  llam_task_t *task) {
    llam_windows_async_control_t *control;
    llam_windows_async_binding_t *binding;
    llam_windows_async_capsule_t *capsule;
    llam_windows_async_bank_t *captured;
    bool requeue = false;
    unsigned i;

    if (shard == NULL || task == NULL || task->async_preempt_task == NULL ||
        (control = llam_windows_async_control(shard->runtime)) == NULL ||
        (binding = llam_windows_async_worker_binding(control, g_llam_tls_worker)) == NULL) {
        return;
    }
    llam_windows_async_lock(shard);
    capsule = task->async_preempt_task;
    captured = atomic_load_explicit(&binding->captured_bank,
                                    memory_order_acquire);
    if (captured != NULL && captured->owner_task == task) {
        if (atomic_load_explicit(&captured->phase,
                                 memory_order_acquire) !=
                LLAM_WINDOWS_ASYNC_BANK_CAPTURED ||
            atomic_load_explicit(&captured->arrival_complete,
                                 memory_order_acquire) == 0U ||
            captured->capture_worker != binding->worker ||
            captured->capture_task_run != atomic_load_explicit(
                &task->run_generation, memory_order_acquire) ||
            captured->capture_worker_run != atomic_load_explicit(
                &binding->worker->run_generation, memory_order_acquire) ||
            task->state == LLAM_TASK_STATE_DEAD) {
            llam_windows_async_uncertain_transaction();
        }
        /* This scheduler owns the only CAPTURED -> READY publication. */
#if defined(LLAM_ENABLE_TEST_HOOKS)
        if (atomic_fetch_add_explicit(&binding->test_ready_contexts, 1U, memory_order_release) == UINT_MAX) {
            llam_windows_async_uncertain_transaction();
        }
#endif
        atomic_store_explicit(&captured->phase,
                              LLAM_WINDOWS_ASYNC_BANK_READY,
                              memory_order_release);
        atomic_store_explicit(&binding->captured_bank, NULL,
                              memory_order_release);
        requeue = true;
    }
    if (!requeue) {
        /* A genuine G -> scheduler ctx_switch has now saved task->ctx. */
        atomic_store_explicit(&capsule->context_kind,
                              LLAM_WINDOWS_ASYNC_CONTEXT_COOPERATIVE, memory_order_release);
    }
    for (i = 0U; i < 2U; ++i) {
        llam_windows_async_bank_t *bank = &capsule->banks[i];

        if (bank->phase == LLAM_WINDOWS_ASYNC_BANK_RESTORING &&
            atomic_load_explicit(&binding->pending_restore,
                                 memory_order_acquire) == bank &&
            bank->restore_task_run == atomic_load_explicit(
                &task->run_generation, memory_order_acquire) &&
            bank->restore_worker_run == atomic_load_explicit(
                &binding->worker->run_generation, memory_order_acquire)) {
            /* Genuine post-restore G return, not merely ResumeThread success.
             * The direct observer may have won RESTORING -> OBSERVED while G
             * was suspended; only the controller later clears pending/ref. */
            bank->phase = LLAM_WINDOWS_ASYNC_BANK_OBSERVED;
        }
    }
    llam_windows_async_unlock(shard);
    if (requeue) {
#if defined(LLAM_ENABLE_TEST_HOOKS)
        llam_windows_async_lifetime_test_hold_phase(
            task, LLAM_WINDOWS_ASYNC_TEST_READY);
#endif
        if (control->managed) {
            llam_windows_async_lock(shard);
            atomic_store_explicit(&task->state, LLAM_TASK_STATE_RUNNABLE, memory_order_release);
            task->wait_reason = LLAM_WAIT_NONE;
            task->enqueue_hot = 0U;
            llam_windows_async_enqueue_binding(binding, task);
            llam_windows_async_unlock(shard);
        } else {
            llam_reinject_task_on_shard(shard->runtime, task, shard->id, false,
                                        LLAM_TRACE_STATE, LLAM_WAIT_NONE);
        }
    }
}

static bool llam_windows_async_retire_banks_locked(llam_windows_async_binding_t *binding,
                                                   llam_windows_async_capsule_t *capsule) {
    unsigned i;
    bool all_free = true;

    for (i = 0U; i < 2U; ++i) {
        llam_windows_async_bank_t *bank = &capsule->banks[i];

        if (bank->phase == LLAM_WINDOWS_ASYNC_BANK_OBSERVED &&
            bank->external_ops == 0U &&
            atomic_load_explicit(&binding->pending_restore,
                                 memory_order_acquire) == bank) {
            atomic_store_explicit(&binding->pending_restore, NULL,
                                  memory_order_release);
            bank->phase = LLAM_WINDOWS_ASYNC_BANK_FREE;
        }
        if (bank->phase != LLAM_WINDOWS_ASYNC_BANK_FREE ||
            bank->external_ops != 0U ||
            atomic_load_explicit(&binding->pending_restore,
                                 memory_order_acquire) == bank) {
            all_free = false;
        }
    }
    return all_free;
}

bool llam_windows_async_lifetime_try_retire_dead(llam_shard_t *shard,
                                                 llam_task_t *task) {
    llam_windows_async_control_t *control;
    llam_windows_async_binding_t *binding;
    llam_windows_async_capsule_t *capsule;
    unsigned debt_owner = UINT_MAX;
    bool had_capsule;
    bool ready = true;

    if (shard == NULL || task == NULL) {
        return false;
    }
    control = llam_windows_async_control(shard->runtime);
    capsule = task->async_preempt_task;
    had_capsule = capsule != NULL;
    if (!had_capsule) {
        /* A managed compatibility G owns no native capsule. Keep its task
         * rooted through READY publication and any detached reclaim claim. */
        if (!llam_task_scan_ref_try_acquire(shard->runtime, task)) abort();
        if (control != NULL && control->managed) {
            llam_windows_async_binding_t *owner = llam_windows_async_worker_binding(control, g_llam_tls_worker);
            if (owner == NULL) abort();
            task->windows_async_finalizer_owner = owner->index;
        }
        llam_task_release_stack(task);
        llam_task_mark_reclaim_ready(task);
        unsigned expected = 0U;
        bool claimed = atomic_load_explicit(&task->detached, memory_order_acquire) != 0U &&
            atomic_compare_exchange_strong_explicit(&task->reclaim_claimed, &expected, 1U,
                                                      memory_order_acq_rel, memory_order_acquire);
        (void)llam_task_scan_ref_release(shard->runtime, task);
        if (claimed) llam_reclaim_claimed_task(shard->runtime, task);
        return true;
    }
    if (had_capsule) {
        if (control == NULL ||
            (binding = llam_windows_async_task_binding(control, task)) == NULL ||
            g_llam_tls_worker != binding->worker) {
            abort();
        }
        llam_windows_async_lock(shard);
        capsule->admission_closed = true;
        ready = llam_windows_async_retire_banks_locked(binding, capsule);
        if (!ready && !task->windows_async_retire_queued) {
            llam_task_t **head = control->managed ? &binding->retire_head : &shard->windows_async_retire_head;
            task->windows_async_retire_next = *head;
            *head = task;
            task->windows_async_retire_queued = true;
            (void)atomic_fetch_add_explicit(control->managed ? &binding->retire_count : &shard->windows_async_retire_count,
                                            1U, memory_order_release);
        }
        llam_windows_async_unlock(shard);
        if (!ready) {
            return false;
        }
        llam_windows_async_lifetime_task_destroy(task);
        debt_owner = task->windows_async_debt_owner;
        if (debt_owner >= control->binding_count ||
            task->windows_async_debt_run != control->run_generation) {
            abort();
        }
        task->windows_async_finalizer_owner = control->managed ? binding->index : shard->id;
    }
    llam_task_release_stack(task);
    /*
     * A host join may claim immediately on reclaim_ready. Keep this short
     * scheduler handoff pin until after the detached-claim attempt; the M
     * never waits for its own pin because claimed cleanup is deferred.
     */
    if (!llam_task_scan_ref_try_acquire(shard->runtime, task)) {
        abort();
    }
    if (had_capsule) {
        if (atomic_exchange_explicit(&task->windows_async_execution_owed,
                                     0U, memory_order_acq_rel) != 1U) {
            abort();
        }
    }
    llam_task_mark_reclaim_ready(task);
#if defined(LLAM_ENABLE_TEST_HOOKS)
    llam_windows_async_lifetime_test_hold_after_ready(task);
#endif
    llam_try_reclaim_detached_task(shard->runtime, task);
    (void)llam_task_scan_ref_release(shard->runtime, task);
    if (had_capsule) {
        llam_windows_async_binding_t *debt_binding = &control->bindings[debt_owner];
        llam_shard_t *owner = debt_binding->worker->compat_shard;

        /* Do not expose global zero before the last G access / exclusive
         * finalizer-root transfer. No allocation-owner lock nests here. */
        llam_windows_async_lock(owner);
        if (atomic_fetch_sub_explicit(control->managed ? &debt_binding->execution_debt : &owner->windows_async_execution_debt,
                                      1U, memory_order_release) == 0U) {
            abort();
        }
        llam_kick_shard(owner);
        llam_windows_async_unlock(owner);
        for (unsigned id = 0U; id < shard->runtime->active_shards; ++id) {
            llam_kick_shard(&shard->runtime->shards[id]);
        }
    }
    return true; /* Do not dereference task: detached reclaim may have freed it. */
}

void llam_windows_async_lifetime_service_retirements(llam_shard_t *shard) {
    llam_task_t *pending;
    llam_windows_async_control_t *control = shard != NULL ? llam_windows_async_control(shard->runtime) : NULL;
    if (control != NULL && control->managed) {
        llam_windows_async_binding_t *binding = llam_windows_async_worker_binding(control, g_llam_tls_worker);
        if (binding == NULL) abort();
        llam_windows_async_lock(shard);
        pending = binding->retire_head;
        binding->retire_head = NULL;
        llam_windows_async_unlock(shard);
        while (pending != NULL) {
            llam_task_t *next = pending->windows_async_retire_next;
            pending->windows_async_retire_next = NULL;
            pending->windows_async_retire_queued = false;
            (void)atomic_fetch_sub_explicit(&binding->retire_count, 1U, memory_order_release);
            (void)llam_windows_async_lifetime_try_retire_dead(shard, pending);
            pending = next;
        }
        return;
    }

    if (shard == NULL || shard->windows_async_retire_head == NULL) {
        return;
    }
    pending = shard->windows_async_retire_head;
    shard->windows_async_retire_head = NULL;
    while (pending != NULL) {
        llam_task_t *next = pending->windows_async_retire_next;

        pending->windows_async_retire_next = NULL;
        pending->windows_async_retire_queued = false;
        (void)atomic_fetch_sub_explicit(&shard->windows_async_retire_count,
                                        1U, memory_order_release);
        (void)llam_windows_async_lifetime_try_retire_dead(shard, pending);
        pending = next;
    }
}

void llam_windows_async_lifetime_defer_claimed(llam_runtime_t *rt,
                                               llam_task_t *task) {
    llam_shard_t *shard;
    llam_windows_async_control_t *control = llam_windows_async_control(rt);
    if (control != NULL && control->managed) {
        llam_windows_async_binding_t *binding = task != NULL
            ? llam_windows_async_binding(control, task->windows_async_finalizer_owner) : NULL;
        if (binding == NULL) abort();
        shard = binding->worker->compat_shard;
        llam_windows_async_lock(shard);
        if (!task->windows_async_finalizer_queued) {
            task->windows_async_finalizer_next = binding->finalizer_head;
            binding->finalizer_head = task;
            task->windows_async_finalizer_queued = true;
            atomic_fetch_add_explicit(&binding->finalizer_count, 1U, memory_order_release);
        }
        llam_windows_async_unlock(shard);
        return;
    }

    if (rt == NULL || task == NULL ||
        task->windows_async_finalizer_owner >= rt->active_shards) {
        abort();
    }
    shard = &rt->shards[task->windows_async_finalizer_owner];
    pthread_mutex_lock(&shard->lock);
    if (!task->windows_async_finalizer_queued) {
        task->windows_async_finalizer_next = shard->windows_async_finalizer_head;
        shard->windows_async_finalizer_head = task;
        task->windows_async_finalizer_queued = true;
        /* The existing exclusive reclaim claim now has a registered root. */
        (void)atomic_fetch_add_explicit(&shard->windows_async_finalizer_count,
                                        1U, memory_order_release);
        llam_kick_shard(shard);
    }
    pthread_mutex_unlock(&shard->lock);
}

static void llam_windows_async_service_binding_finalizers(llam_windows_async_binding_t *binding) {
    llam_shard_t *shard = binding->worker->compat_shard;
    llam_task_t *pending;
    llam_windows_async_lock(shard);
    pending = binding->finalizer_head;
    binding->finalizer_head = NULL;
    llam_windows_async_unlock(shard);
    while (pending != NULL) {
        llam_task_t *next = pending->windows_async_finalizer_next;
        pending->windows_async_finalizer_next = NULL;
        if (llam_task_try_finalize_deferred_claim(shard->runtime, pending)) {
            atomic_fetch_sub_explicit(&binding->finalizer_count, 1U, memory_order_release);
        } else {
            llam_windows_async_lock(shard);
            pending->windows_async_finalizer_next = binding->finalizer_head;
            binding->finalizer_head = pending;
            llam_windows_async_unlock(shard);
        }
        pending = next;
    }
}

void llam_windows_async_lifetime_service_finalizers(llam_shard_t *shard) {
    llam_task_t *pending;
    llam_windows_async_control_t *control = shard != NULL ? llam_windows_async_control(shard->runtime) : NULL;
    if (control != NULL && control->managed) {
        llam_windows_async_binding_t *binding = llam_windows_async_worker_binding(control, g_llam_tls_worker);
        if (binding == NULL) abort();
        llam_windows_async_service_binding_finalizers(binding);
        return;
    }

    if (shard == NULL) {
        return;
    }
    pthread_mutex_lock(&shard->lock);
    pending = shard->windows_async_finalizer_head;
    shard->windows_async_finalizer_head = NULL;
    pthread_mutex_unlock(&shard->lock);
    while (pending != NULL) {
        llam_task_t *next = pending->windows_async_finalizer_next;

        pending->windows_async_finalizer_next = NULL;
        if (atomic_load_explicit(&pending->scan_refs,
                                 memory_order_acquire) == 0U &&
            llam_public_active_op_count(&pending->active_ops) == 0U &&
            llam_task_try_finalize_deferred_claim(shard->runtime, pending)) {
            /* The helper consumed the only claim and may have freed pending. */
            (void)atomic_fetch_sub_explicit(&shard->windows_async_finalizer_count,
                                            1U, memory_order_release);
        } else {
            pthread_mutex_lock(&shard->lock);
            pending->windows_async_finalizer_next =
                shard->windows_async_finalizer_head;
            shard->windows_async_finalizer_head = pending;
            pthread_mutex_unlock(&shard->lock);
        }
        pending = next;
    }
}

static int llam_windows_managed_begin(llam_runtime_t *rt, void *data) {
    (void)data;
    return llam_windows_async_lifetime_run_begin(rt);
}

static int llam_windows_managed_start(llam_worker_t *worker, llam_ctx_t *scheduler, void *data) {
    (void)data;
    if (g_llam_tls_worker != worker || g_llam_tls_scheduler_ctx != scheduler) {
        errno = EINVAL;
        return -1;
    }
    return llam_windows_async_register_worker(worker, scheduler, false);
}

static int llam_windows_managed_restart(llam_worker_t *worker, llam_ctx_t *scheduler, void *data) {
    (void)data;
    llam_runtime_t *rt = worker->registry_runtime;
    llam_windows_async_control_t *control = llam_windows_async_control(rt);
    llam_windows_async_binding_t *binding = llam_windows_async_worker_binding(control, worker);
    HANDLE old_handle = NULL;
    if (control == NULL || !control->managed || binding == NULL ||
        worker->role != LLAM_WORKER_ROLE_NATIVE_AFFINE || g_llam_tls_worker != worker ||
        g_llam_tls_scheduler_ctx != scheduler) { errno = EINVAL; return -1; }
    llam_windows_async_controller_exclude(control);
    llam_windows_async_lock(worker->compat_shard);
    bool ready = atomic_load_explicit(&control->start_phase, memory_order_acquire) == LLAM_WINDOWS_ASYNC_START_OPEN &&
        !atomic_load_explicit(&rt->stop_requested, memory_order_acquire) &&
        !atomic_load_explicit(&rt->shutdown_requested, memory_order_acquire) &&
        !binding->binding_open && binding->terminal_task == NULL &&
        llam_windows_async_binding_native_idle(binding) &&
        atomic_load_explicit(&binding->execution_debt, memory_order_acquire) == 0U &&
        binding->operation.shard == NULL &&
        atomic_load_explicit(&worker->current_p, memory_order_acquire) == NULL &&
        llam_worker_load_active_g(worker) == NULL;
    if (ready) {
        old_handle = binding->native_thread;
        binding->native_thread = NULL;
        binding->native_thread_id = 0U;
        binding->binding_incarnation = 0U;
        binding->runtime_run = 0U;
        binding->scheduler = NULL;
        atomic_store_explicit(&binding->probe_phase, 0U, memory_order_relaxed);
        atomic_store_explicit(&binding->restart_pending, false, memory_order_release);
    }
    llam_windows_async_unlock(worker->compat_shard);
    if (old_handle != NULL) llam_windows_async_close_native_handle(control, old_handle);
    atomic_store_explicit(&control->controller_busy, 0U, memory_order_release);
    if (!ready) { errno = EBUSY; return -1; }
    return llam_windows_async_register_worker(worker, scheduler, true);
}

static int llam_windows_managed_commit(llam_runtime_t *rt, void *data) {
    (void)data;
    return llam_windows_async_lifetime_run_commit(rt);
}

static void llam_windows_managed_abort(llam_runtime_t *rt, int error, void *data) {
    (void)data;
    llam_windows_async_lifetime_run_abort(rt, error);
}

static void llam_windows_managed_end(llam_runtime_t *rt, void *data) {
    (void)data;
    llam_windows_async_lifetime_run_end(rt);
}

static void llam_windows_managed_service(llam_worker_t *worker, void *data) {
    (void)data;
    llam_windows_async_lifetime_service_retirements(worker->compat_shard);
    llam_windows_async_lifetime_service_finalizers(worker->compat_shard);
}

static bool llam_windows_managed_has_debt(llam_runtime_t *rt, void *data) {
    (void)data;
    return llam_windows_async_lifetime_has_debt(rt);
}

static llam_task_t *llam_windows_managed_take(llam_worker_t *worker, llam_processor_t *processor,
                                             bool fallback, void *data) {
    (void)data;
    if (g_llam_tls_worker != worker) abort();
    return llam_async_preempt_take_ready(processor->compat_shard, fallback);
}

static bool llam_windows_managed_accept(llam_worker_t *worker, llam_processor_t *processor,
                                       llam_task_t *task, void *data) {
    llam_windows_async_control_t *control = llam_windows_async_control(worker->registry_runtime);
    llam_windows_async_binding_t *origin;
    (void)data;
    if (control == NULL) abort();
    if (task->async_preempt_task == NULL) {
        if (task->managed_catalog != NULL &&
            task->managed_execution_class == LLAM_MANAGED_EXECUTION_LEGACY) return true;
        abort();
    }
    llam_windows_async_lock(processor->compat_shard);
    origin = llam_windows_async_task_binding(control, task);
    if (origin != NULL && origin->worker != worker) {
        llam_windows_async_enqueue_binding(origin, task);
        llam_windows_async_unlock(processor->compat_shard);
        return false;
    }
    llam_windows_async_unlock(processor->compat_shard);
    return true;
}

static void llam_windows_managed_returned(llam_worker_t *worker, llam_processor_t *processor,
                                          llam_task_t *task, void *data) {
    llam_shard_t *shard = processor->compat_shard;
    llam_windows_async_capsule_t *capsule = task->async_preempt_task;
    (void)worker;
    (void)data;
    llam_windows_async_lifetime_scheduler_return(shard, task);
    if (atomic_load_explicit(&task->state, memory_order_acquire) == LLAM_TASK_STATE_DEAD) {
        if (task->managed_execution_class == LLAM_MANAGED_EXECUTION_NATIVE_AFFINE) {
            llam_windows_async_control_t *control = llam_windows_async_control(task->owner_runtime);
            llam_windows_async_binding_t *binding = llam_windows_async_worker_binding(control, worker);
            llam_windows_async_lock(shard);
            if (binding == NULL || binding->terminal_task != NULL ||
                !llam_task_scan_ref_try_acquire(task->owner_runtime, task)) abort();
            binding->terminal_task = task;
            llam_windows_async_unlock(shard);
            return;
        }
        (void)llam_windows_async_lifetime_try_retire_dead(shard, task);
        return;
    }
    llam_windows_async_lock(shard);
    bool quiescent = capsule != NULL && !capsule->origin_enqueued &&
        task->managed_execution_class != LLAM_MANAGED_EXECUTION_NATIVE_AFFINE &&
        atomic_load_explicit(&capsule->context_kind, memory_order_acquire) ==
            LLAM_WINDOWS_ASYNC_CONTEXT_COOPERATIVE;
    for (unsigned i = 0U; quiescent && i < 2U; ++i)
        quiescent = atomic_load_explicit(&capsule->banks[i].phase, memory_order_acquire) ==
                        LLAM_WINDOWS_ASYNC_BANK_FREE && capsule->banks[i].external_ops == 0U;
    if (quiescent) {
        /* ctx_switch has saved every cooperative register, and all old native
         * readers are gone. The scheduler reservation publishes this release
         * before any other M can consume the already queued continuation. */
        capsule->source_return_complete = true;
        atomic_store_explicit(&capsule->service_worker, NULL, memory_order_release);
    }
    llam_windows_async_unlock(shard);
}

static bool llam_windows_managed_stop(llam_worker_t *worker, void *data) {
    (void)data;
    while (!llam_windows_async_lifetime_close_binding(worker->compat_shard)) Sleep(1U);
    return true;
}

static bool llam_windows_managed_can_exit(llam_worker_t *worker, void *data) {
    (void)data;
    llam_windows_async_binding_t *binding = llam_windows_async_worker_binding(
        llam_windows_async_control(worker->registry_runtime), worker);
    if (binding == NULL) return false;
    llam_windows_async_lock(worker->compat_shard);
    bool ready = llam_windows_async_terminal_drained(binding);
    llam_windows_async_unlock(worker->compat_shard);
    return ready;
}

static void llam_windows_managed_joined(llam_worker_t *worker, void *data) {
    (void)data;
    llam_windows_async_binding_t *binding = llam_windows_async_worker_binding(
        llam_windows_async_control(worker->registry_runtime), worker);
    bool claimed = false;
    if (binding == NULL) return;
    llam_windows_async_lock(worker->compat_shard);
    llam_task_t *task = binding->terminal_task;
    if (task != NULL) {
        if (g_llam_tls_worker != NULL || binding->binding_open ||
            atomic_load_explicit(&worker->native_thread_live, memory_order_acquire) ||
            !llam_windows_async_terminal_drained(binding)) abort();
        binding->terminal_task = NULL;
        llam_windows_async_lifetime_task_destroy(task);
    }
    llam_windows_async_unlock(worker->compat_shard);
    if (task == NULL) return;
    llam_task_release_stack(task);
    if (task->windows_async_debt_owner != binding->index ||
        atomic_exchange_explicit(&task->windows_async_execution_owed, 0U, memory_order_acq_rel) != 1U)
        abort();
    llam_task_mark_reclaim_ready(task);
    if (atomic_load_explicit(&task->detached, memory_order_acquire) != 0U) {
        unsigned expected = 0U;
        claimed = atomic_compare_exchange_strong_explicit(&task->reclaim_claimed, &expected, 1U,
                                                          memory_order_acq_rel, memory_order_acquire);
    }
    /* Do not enter the blocking host reclaimer while holding this terminal
     * scan pin. A consuming join may free G after release; only our exclusive
     * claim permits a subsequent task access. Native TLS exit is already over. */
    (void)llam_task_scan_ref_release(worker->registry_runtime, task);
    if (claimed) llam_reclaim_claimed_task(worker->registry_runtime, task);
    if (atomic_fetch_sub_explicit(&binding->execution_debt, 1U, memory_order_release) != 1U) abort();
}

bool llam_windows_async_task_quiescent(const llam_task_t *task) {
    llam_windows_async_control_t *control = task != NULL ? llam_windows_async_control(task->owner_runtime) : NULL;
    llam_windows_async_binding_t *binding = llam_windows_async_task_binding(control, task);
    if (control == NULL || !control->managed || binding == NULL ||
        binding->worker != g_llam_tls_worker ||
        task->managed_execution_class != LLAM_MANAGED_EXECUTION_NATIVE_AFFINE) return false;
    llam_windows_async_lock(binding->worker->compat_shard);
    bool ready = llam_windows_async_capsule_quiescent(task->async_preempt_task) &&
        llam_windows_async_binding_native_idle(binding) &&
        atomic_load_explicit(&binding->execution_debt, memory_order_acquire) == 1U;
    llam_windows_async_unlock(binding->worker->compat_shard);
    return ready;
}

static bool llam_windows_managed_needs_processor(llam_worker_t *worker, void *data) {
    (void)data;
    llam_windows_async_binding_t *binding = llam_windows_async_worker_binding(
        llam_windows_async_control(worker->registry_runtime), worker);
    return binding != NULL && atomic_load_explicit(&binding->ready_depth, memory_order_acquire) != 0U;
}

static const llam_managed_scheduler_ops_t g_windows_managed_ops = {
    .native_async = true,
    .run_begin = llam_windows_managed_begin,
    .worker_start = llam_windows_managed_start,
    .worker_restart = llam_windows_managed_restart,
    .run_commit = llam_windows_managed_commit,
    .run_abort = llam_windows_managed_abort,
    .run_end = llam_windows_managed_end,
    .service = llam_windows_managed_service,
    .has_obligations = llam_windows_managed_has_debt,
    .take_ready = llam_windows_managed_take,
    .accept_task = llam_windows_managed_accept,
    .returned = llam_windows_managed_returned,
    .worker_stop = llam_windows_managed_stop,
    .worker_can_exit = llam_windows_managed_can_exit,
    .worker_joined = llam_windows_managed_joined,
    .worker_needs_processor = llam_windows_managed_needs_processor
};

int llam_windows_async_prepare_registered(llam_runtime_t *rt) {
    if (rt == NULL || rt->execution_lease_mode != LLAM_EXECUTION_LEASE_REGISTERED) {
        errno = EINVAL;
        return -1;
    }
    if (llam_windows_async_enable_locked(rt) != 0) return -1;
    rt->managed_scheduler_ops = &g_windows_managed_ops;
    rt->managed_scheduler_data = rt->async_preempt_control;
    return 0;
}

bool llam_windows_async_memory_info(const llam_runtime_t *runtime, uint64_t *charged,
                                     uint64_t *per_task) {
    const llam_windows_async_control_t *control = runtime != NULL ? runtime->async_preempt_control : NULL;
    if (control == NULL || charged == NULL || per_task == NULL) return false;
    *charged = atomic_load_explicit(&control->charged_bytes, memory_order_acquire);
    *per_task = control->capsule_pool.charge;
    return true;
}
