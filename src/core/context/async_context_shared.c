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

#include "runtime_internal.h"
#include "managed_scheduler.h"
#include "managed_async_adapter.h"
#include "managed_catalog.h"
#include "capsule_pool.h"
#include "metadata_v2.h"
#if LLAM_ASYNC_CONTEXT_SHARED && LLAM_PLATFORM_DARWIN
#include <sys/sysctl.h>
#elif LLAM_ASYNC_CONTEXT_SHARED && LLAM_PLATFORM_LINUX
#include <sys/auxv.h>
#include <asm/hwcap.h>
#endif
#if LLAM_ASYNC_CONTEXT_SHARED && !LLAM_PLATFORM_DARWIN
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

#if LLAM_ASYNC_CONTEXT_SHARED
#if LLAM_ASYNC_CONTEXT_SIGNAL
#include "async_context_signal.h"
#endif
#define LLAM_ASYNC_SHARED_BUDGET (UINT64_C(64) * 1024U * 1024U)

enum {
    LLAM_SHARED_CLOSED = 0,
    LLAM_SHARED_PREPARING,
    LLAM_SHARED_OPEN,
    LLAM_SHARED_ABORTED
};

typedef struct llam_shared_control llam_shared_control_t;

typedef struct llam_shared_bank {
    atomic_uint phase;
    atomic_uint arrival_complete;
    unsigned readers; /* Service lease/payload pairs; phase alone never frees storage. */
    uint64_t generation;
    llam_task_t *task;
    uint64_t capture_g_run;
    uint64_t capture_m_run;
    uintptr_t capture_scheduler_sp;
    uint64_t restore_g_run;
    uint64_t restore_m_run;
    uintptr_t restore_scheduler_sp;
    llam_processor_owner_ticket_t ticket;
    void *native;
#if defined(LLAM_ENABLE_TEST_HOOKS) || defined(LLAM_ENABLE_ASYNC_PROFILE)
    uint64_t requested_ns;
#endif
} llam_shared_bank_t;

typedef struct llam_shared_capsule {
    llam_shared_bank_t banks[2];
    llam_shared_bank_t *ready;
    struct llam_shared_capsule *retire_next;
    llam_task_t *task;
    _Atomic(llam_shared_control_t *) origin;
    uint64_t next_generation;
    size_t charge;
    sigset_t signal_mask;
    bool mask_valid;
    atomic_bool started;
    bool enqueued; /* Origin service locks. */
    bool retire_queued;
    _Alignas(64) unsigned char payload[];
} llam_shared_capsule_t;

struct llam_shared_control {
    llam_runtime_t *runtime;
    llam_shared_control_t *group;
    size_t control_count; /* Immutable contiguous M bindings; meaningful on group. */
    llam_worker_t *worker;
    llam_ctx_t *scheduler; /* Original M native frame, independent of current P. */
    bool registered;
    bool native_enabled; /* Compatibility Ms never own asynchronous native state. */
    uint64_t available_isa; /* Actual target M probe, intersected at run commit. */
    const llam_async_native_ops_t *ops;
    void *native_binding;
    void *probe_context;
    atomic_uint start_phase;
    atomic_uint probe_phase;
    atomic_int start_error;
    atomic_uint controller_busy;
    atomic_bool urgent_service; /* Scheduling hint only; never native/P authority. */
    atomic_bool native_return_active; /* Published before any native PC redirect. */
#if LLAM_PLATFORM_DARWIN
    dispatch_semaphore_t controller_wake; /* Head owns the one controller event. */
#else
    int controller_pipe[2];
#endif
    atomic_uint binding_ops;
    uint64_t run_generation;
    uint64_t binding_incarnation;
    bool binding_live; /* Service locks, or controller exclusion at run-end. */
    sigset_t scheduler_mask;
#if LLAM_PLATFORM_DARWIN
    sigset_t launch_mask; /* Head's host mask, before primary M policy setup. */
#endif
    llam_shared_bank_t *capture_op; /* reader + scan + binding refs until handoff. */
    llam_shared_bank_t *arrival_ticket; /* Consumed only by the exact native return. */
    llam_shared_bank_t *restore_op; /* reader + scan + binding refs through witness. */
    llam_shared_bank_t *pending_gate; /* M-owned root, guarded by the same locks. */
    llam_shared_capsule_t *retire_head;
    llam_task_t *finalizer_head; /* exclusive reclaim claims, queue_next available. */
    llam_task_t *terminal_task; /* Dedicated affine G scan root through native join. */
    llam_task_t *ready_head;
    llam_task_t *ready_tail;
    atomic_uint ready_depth;
    bool prefer_origin;
    atomic_uint retire_depth;
    atomic_uint finalizer_depth;
    atomic_uint execution_debt;
    atomic_size_t capsule_bytes;
    llam_capsule_pool_t capsule_pool; /* Owned by group; cold worker holds no M/P. */
    uint64_t next_probe_ns;
#if LLAM_ASYNC_CONTEXT_SIGNAL
    uint64_t signal_deadline_ns; /* Serialized ordinary controller only. */
    bool owns_process_signal;
#endif
    atomic_uint_fast64_t requests;
    atomic_uint_fast64_t captures;
    atomic_uint_fast64_t restores;
    atomic_uint_fast64_t witnesses;
    atomic_uint_fast64_t bindings_created;
    atomic_uint_fast64_t bindings_closed;
#if defined(LLAM_ENABLE_TEST_HOOKS)
    _Atomic(atomic_uint *) tail_observed[LLAM_ASYNC_SHARED_TEST_POINT_COUNT];
    _Atomic(atomic_uint *) tail_release[LLAM_ASYNC_SHARED_TEST_POINT_COUNT];
    bool handoff_pause;
    atomic_uint_fast64_t scheduler_error_checks;
#endif
#if defined(LLAM_ENABLE_TEST_HOOKS) || defined(LLAM_ENABLE_ASYNC_PROFILE)
    atomic_uint event_next;
    atomic_uint event_ready[512];
    llam_async_shared_event_t events[512];
#endif
};

static bool llam_shared_capsule_quiescent(llam_shared_capsule_t *capsule);
static bool llam_shared_portable_task(const llam_task_t *task);
static LLAM_NORETURN void llam_shared_uncertain(void);
#if defined(LLAM_ENABLE_TEST_HOOKS)
static void llam_shared_test_tail(llam_shared_control_t *control, unsigned point);
#endif

static void llam_shared_notify_controller(llam_shared_control_t *control) {
    llam_shared_control_t *group = control->group;
    int saved_errno = llam_thread_errno_load();
#if LLAM_PLATFORM_DARWIN
    (void)dispatch_semaphore_signal(group->controller_wake);
#else
    const unsigned char byte = 1U;
    ssize_t result = write(group->controller_pipe[1], &byte, sizeof(byte));
    /* A full pipe already owns a counted wake. EINTR may defer only the hint;
     * the bounded native-service timeout still rechecks every pinned tuple. */
    if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) llam_shared_uncertain();
#endif
    llam_thread_errno_store(saved_errno);
}

/* A completed affine G retains its charge, stack and catalog through native
 * TLS destructors. This root may outlive the binding, but every operation that
 * could touch native state must already have drained before M can exit. */
static bool llam_shared_terminal_drained(const llam_shared_control_t *control) {
    const llam_task_t *task = control->terminal_task;
    llam_shared_capsule_t *capsule = task != NULL ? task->async_preempt_task : NULL;
    return task != NULL && capsule != NULL &&
        atomic_load_explicit(&control->worker->current_p, memory_order_acquire) == NULL &&
        llam_worker_load_active_g(control->worker) == NULL &&
        task->managed_execution_class == LLAM_MANAGED_EXECUTION_NATIVE_AFFINE &&
        atomic_load_explicit(&task->state, memory_order_acquire) == LLAM_TASK_STATE_DEAD &&
        atomic_load_explicit(&control->execution_debt, memory_order_acquire) == 1U &&
        atomic_load_explicit(&control->retire_depth, memory_order_acquire) == 0U &&
        atomic_load_explicit(&control->finalizer_depth, memory_order_acquire) == 0U &&
        atomic_load_explicit(&control->ready_depth, memory_order_acquire) == 0U &&
        atomic_load_explicit(&control->binding_ops, memory_order_acquire) == 0U &&
        control->capture_op == NULL && control->arrival_ticket == NULL &&
        control->restore_op == NULL && control->pending_gate == NULL &&
        llam_shared_capsule_quiescent(capsule);
}

static llam_shared_control_t *llam_shared_control(const llam_runtime_t *runtime) {
    if (runtime == NULL ||
        atomic_load_explicit(&runtime->async_preempt_enabled, memory_order_acquire) == 0U) {
        return NULL;
    }
    return runtime->async_preempt_control;
}

static llam_shared_control_t *llam_shared_worker_control(const llam_worker_t *worker) {
    if (worker == NULL || worker->registry_runtime == NULL ||
        atomic_load_explicit(&worker->registry_runtime->async_preempt_enabled, memory_order_acquire) == 0U)
        return NULL;
    return worker->async_preempt_control;
}

static llam_shared_control_t *llam_shared_task_control(const llam_task_t *task) {
    const llam_shared_capsule_t *capsule = task != NULL ? task->async_preempt_task : NULL;
    if (task != NULL && task->managed_catalog != NULL &&
        task->managed_execution_class == LLAM_MANAGED_EXECUTION_LEGACY) return NULL;
    if (capsule != NULL && atomic_load_explicit(&capsule->started, memory_order_acquire)) {
        llam_shared_control_t *origin = atomic_load_explicit(&capsule->origin, memory_order_acquire);
        /* A started G retains its backend lifetime root until M retirement.
         * Ordinary runtime enable/registry lookup adds no authority here. */
        if (origin != NULL) return origin;
    }
    llam_shared_control_t *group = task != NULL ? llam_shared_control(task->owner_runtime) : NULL;
    if (group == NULL || !group->registered || capsule == NULL) return group;
    return atomic_load_explicit(&capsule->origin, memory_order_acquire);
}

static llam_shard_t *llam_shared_shard(llam_shared_control_t *control) {
    llam_processor_t *processor = atomic_load_explicit(&control->worker->current_p, memory_order_acquire);
    return processor != NULL ? processor->compat_shard : control->worker->compat_shard;
}

#if defined(LLAM_ENABLE_TEST_HOOKS) || defined(LLAM_ENABLE_ASYNC_PROFILE)
static void llam_shared_event_record(llam_shared_control_t *control, unsigned kind,
                                      const llam_shared_bank_t *bank,
                                      uint64_t pstate_before, uint64_t pstate_after,
                                      uint64_t observed_ns) {
    unsigned index = atomic_fetch_add_explicit(&control->event_next, 1U, memory_order_relaxed);
    if (index >= 512U) return; /* Bounded observation prefix, never a lifetime root. */
    control->events[index] = (llam_async_shared_event_t){
        .kind = kind, .task_id = bank->task->id,
        .bank_generation = bank->generation, .runtime_run = control->run_generation,
        .pstate_before = pstate_before, .pstate_after = pstate_after,
        .requested_ns = bank->requested_ns, .observed_ns = observed_ns
    };
    atomic_store_explicit(&control->event_ready[index], 1U, memory_order_release);
}
#define llam_shared_event(control, kind, bank) \
    llam_shared_event_record(control, kind, bank, 0U, 0U, 0U)
#define llam_shared_signal_event(control, kind, bank, before, after) \
    llam_shared_event_record(control, kind, bank, before, after, 0U)
#else
#define llam_shared_event(control, kind, bank) ((void)0)
#define llam_shared_signal_event(control, kind, bank, before, after) ((void)0)
#endif

/* Registered service belongs to M even while it has no P. Taking all P pairs
 * in stable order makes a current-P change and an origin-queue mutation one
 * transaction, without inventing a second execution grant. Native suspension
 * always occurs after these locks are released. */
static void llam_shared_lock(llam_shared_control_t *control) {
    llam_runtime_t *runtime = control->runtime;
    if (control->registered) {
        for (unsigned i = 0U; i < runtime->active_shards; ++i)
            pthread_mutex_lock(&runtime->shards[i].opaque_lock);
        for (unsigned i = 0U; i < runtime->active_shards; ++i)
            pthread_mutex_lock(&runtime->shards[i].lock);
    } else {
        pthread_mutex_lock(&control->worker->compat_shard->opaque_lock);
        pthread_mutex_lock(&control->worker->compat_shard->lock);
    }
}

static void llam_shared_unlock(llam_shared_control_t *control) {
    llam_runtime_t *runtime = control->runtime;
    if (control->registered) {
        for (unsigned i = runtime->active_shards; i != 0U; --i)
            pthread_mutex_unlock(&runtime->shards[i - 1U].lock);
        for (unsigned i = runtime->active_shards; i != 0U; --i)
            pthread_mutex_unlock(&runtime->shards[i - 1U].opaque_lock);
    } else {
        pthread_mutex_unlock(&control->worker->compat_shard->lock);
        pthread_mutex_unlock(&control->worker->compat_shard->opaque_lock);
    }
}

static LLAM_NORETURN void llam_shared_uncertain(void) {
    _Exit(125);
}

#if LLAM_PLATFORM_DARWIN
int llam_async_shared_block_native_signals(llam_worker_t *worker, sigset_t *previous) {
    sigset_t blocked;
    if (worker != NULL && worker->registry_runtime != NULL &&
        worker->registry_runtime->managed_catalog != NULL) {
        /* Mach snapshots cannot consume an outstanding Unix signal frame.
         * Route asynchronous handlers away from native managed Ms. Synchronous
         * fatal faults retain the platform's ordinary diagnostic path. */
        sigfillset(&blocked);
        sigdelset(&blocked, SIGSEGV);
        sigdelset(&blocked, SIGBUS);
        sigdelset(&blocked, SIGILL);
        sigdelset(&blocked, SIGFPE);
        sigdelset(&blocked, SIGTRAP);
        sigdelset(&blocked, SIGABRT);
    } else {
        sigemptyset(&blocked); /* Preserve caller-certified expert masks. */
    }
    sigaddset(&blocked, LLAM_PREEMPT_SIGNAL);
    int error = pthread_sigmask(SIG_BLOCK, &blocked, previous);
    if (error != 0) errno = error;
    return error == 0 ? 0 : -1;
}

void llam_async_shared_rearm_native_signals(llam_worker_t *worker) {
    if (worker != NULL && worker->async_preempt_control != NULL &&
        llam_async_shared_block_native_signals(worker, NULL) != 0) llam_shared_uncertain();
}
#endif

static uintptr_t llam_shared_scheduler_sp(const llam_ctx_t *context) {
#if LLAM_ARCH_AARCH64
    return (uintptr_t)context->sp;
#else
    return (uintptr_t)context->rsp;
#endif
}

static bool llam_shared_scheduler_compatible(const llam_ctx_t *context) {
#if LLAM_ARCH_AARCH64
    return context->simd_flags == 0U;
#else
    /* The SysV native gate saves its ABI-preserved scheduler state itself;
     * an independent cooperative XSAVE buffer is not part of that gate ABI. */
    return context->xsave_area == NULL;
#endif
}

static bool llam_shared_pc_allowed(const llam_shared_control_t *control,
                                    const llam_task_t *task, uintptr_t pc,
                                    uintptr_t *begin, uintptr_t *end) {
    const llam_runtime_t *runtime = control->runtime;
    if (task->managed_catalog != NULL) {
        const llam_llpm2_range_t *range = llam_managed_catalog_find_safe(task->managed_catalog, pc);
        if (range == NULL) return false;
        if (begin != NULL) *begin = range->begin;
        if (end != NULL) *end = range->end;
        return true;
    }
    for (size_t i = 0U; i < runtime->async_preempt_range_count; ++i) {
        const llam_async_preempt_range_t *range = &runtime->async_preempt_ranges[i];
        if (pc >= range->begin && pc < range->end) {
            if (begin != NULL) *begin = range->begin;
            if (end != NULL) *end = range->end;
            return true;
        }
    }
    return false;
}

static bool llam_shared_on_stack(const llam_task_t *task, uintptr_t sp) {
    uintptr_t low = (uintptr_t)task->stack_base;
    return low != 0U && sp >= low && sp - low < task->stack_size &&
           (sp & (LLAM_ARCH_AARCH64 ? 15U : 7U)) == 0U;
}

/* Caller holds opaque -> shard. This is existing service, not a new P grant. */
static bool llam_shared_service_locked(llam_shared_control_t *control,
                                        llam_processor_owner_ticket_t *ticket) {
    llam_shard_t *shard = llam_shared_shard(control);
    llam_worker_t *worker = control->worker;
    llam_processor_owner_ticket_t current;
    llam_processor_owner_record_t record;
    const llam_legacy_lease_t *lease = llam_shard_legacy_lease_const_locked(shard);

    if (!control->binding_live ||
        atomic_load_explicit(&control->start_phase, memory_order_acquire) != LLAM_SHARED_OPEN ||
        !llam_worker_binding_live(worker, control->binding_incarnation)) return false;
    if (llam_processor_owner_ticket_snapshot_pair_locked(shard, &current)) {
        if (current.owner_m != worker || current.worker_incarnation != control->binding_incarnation)
            return false;
        if (ticket != NULL) *ticket = current;
        return true;
    }
    /* Normal execute snapshots reject STOPPING and the shutdown revoke gap.
     * This separate exact drain tuple services previously admitted G roots;
     * it never revives a revoked ordinary ticket or mixes lease protocols. */
    if (!atomic_load_explicit(&control->runtime->shutdown_requested, memory_order_acquire) ||
        !llam_processor_online(&shard->processor) ||
        !llam_execution_lease_state_matches(control->runtime, lease->state) ||
        !llam_processor_owner_snapshot_pair_locked(&shard->processor, &record) ||
        !llam_processor_owner_record_well_formed(&record) ||
        record.state != LLAM_PROCESSOR_OWNER_STOPPING || record.lease_state != LLAM_LEASE_STOPPING ||
        lease->state != LLAM_LEASE_STOPPING || record.owner_m != worker || lease->owner != worker ||
        atomic_load_explicit(&worker->current_p, memory_order_acquire) != &shard->processor ||
        record.worker_incarnation != control->binding_incarnation ||
        lease->owner_incarnation != control->binding_incarnation ||
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

/* A shutdown request revokes an ordinary ticket before its STOPPING projection
 * acquires these locks. This live same-M dispatch waits for the separate drain
 * authority; it never treats that expected publication window as corruption. */
static void llam_shared_wait_dispatch_service_locked(llam_shared_control_t *control) {
    while (!llam_shared_service_locked(control, NULL)) {
        if (!control->registered || g_llam_tls_worker != control->worker ||
            !control->binding_live ||
            !atomic_load_explicit(&control->runtime->shutdown_requested, memory_order_acquire) ||
            atomic_load_explicit(&control->worker->current_p, memory_order_acquire) == NULL)
            llam_shared_uncertain();
        llam_shared_unlock(control);
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
        (void)nanosleep(&delay, NULL);
        llam_shared_lock(control);
    }
}

/* Native target is stopped, runtime locks are NOT held. Only immutable or
 * atomically published dispatch provenance is inspected in this transaction. */
static bool llam_shared_stopped_tuple(llam_shared_control_t *control,
                                       const llam_shared_bank_t *bank, bool restoring) {
    llam_worker_t *worker = control->worker;
    llam_task_t *task = bank->task;
    uint64_t g_run = restoring ? bank->restore_g_run : bank->capture_g_run;
    uint64_t m_run = restoring ? bank->restore_m_run : bank->capture_m_run;

    return atomic_load_explicit(&control->start_phase, memory_order_acquire) == LLAM_SHARED_OPEN &&
           llam_worker_binding_live(worker, control->binding_incarnation) &&
           llam_worker_load_active_g(worker) == task &&
           atomic_load_explicit(&worker->current_p, memory_order_acquire) == bank->ticket.processor &&
           atomic_load_explicit(&worker->run_generation, memory_order_acquire) == m_run &&
           bank->ticket.worker_incarnation == control->binding_incarnation &&
           atomic_load_explicit(&worker->async_preempt_ticket_lease_epoch, memory_order_acquire) ==
               bank->ticket.lease_projection_epoch &&
           atomic_load_explicit(&worker->async_preempt_ticket_generation, memory_order_acquire) ==
               bank->ticket.p_generation &&
           atomic_load_explicit(&task->run_generation, memory_order_acquire) == g_run &&
           atomic_load_explicit(&task->state, memory_order_acquire) == LLAM_TASK_STATE_RUNNING &&
           atomic_load_explicit(&worker->runtime_critical_depth, memory_order_acquire) == 0U &&
           atomic_load_explicit(&worker->preempt_disable_depth, memory_order_acquire) == 0U &&
           atomic_load_explicit(&worker->native_domain_depth, memory_order_acquire) == 0U &&
           atomic_load_explicit(&worker->execution_state, memory_order_acquire) == LLAM_WORKER_EXEC_MANAGED_SAFE &&
           atomic_load_explicit(&task->opaque_blocking_depth, memory_order_acquire) == 0U;
}


#if LLAM_ASYNC_CONTEXT_SIGNAL
/* Runs only after the stamped request has been claimed. Its pre-existing
 * operation owns the G/bank/binding; no ordinary gate is acquired here. */
static bool llam_shared_signal_identity(const llam_async_signal_request_t *request,
                                        const llam_shared_control_t *control,
                                        const llam_shared_bank_t *bank) {
    return request->runtime_run == control->run_generation &&
           request->binding_incarnation == control->binding_incarnation &&
           request->bank_generation != 0U && request->bank_generation == bank->generation;
}

int llam_async_shared_signal_handle(llam_async_signal_binding_t *binding,
                                   ucontext_t *frame, int entry_errno) {
    uintptr_t signal_low = (uintptr_t)binding->worker->signal_stack;
    uintptr_t signal_high = signal_low + binding->worker->signal_stack_size;
    uint64_t stamp = atomic_load_explicit(&binding->stamp, memory_order_acquire);
    llam_async_signal_request_t request;
    llam_shared_control_t *control;
    llam_shared_bank_t *bank;
    int error = EAGAIN;
    int return_errno = entry_errno;
#if defined(LLAM_ENABLE_TEST_HOOKS)
    atomic_uint *tail_observed = NULL, *tail_release = NULL;
#endif
    if ((stamp & LLAM_ASYNC_SIGNAL_STATE_MASK) != LLAM_ASYNC_SIGNAL_ARMED ||
        !atomic_compare_exchange_strong_explicit(&binding->stamp, &stamp,
            (stamp & ~(uint64_t)LLAM_ASYNC_SIGNAL_STATE_MASK) | LLAM_ASYNC_SIGNAL_CLAIMED,
            memory_order_acq_rel, memory_order_acquire)) return entry_errno;
    /* A single exact sequence+state CAS, never a generic ARMED retry. */
    request = binding->request;
    control = request.control;
    bank = request.bank;
    if (request.binding_incarnation != binding->incarnation) {
        error = EINVAL;
    } else if (request.kind == LLAM_ASYNC_SIGNAL_PROBE) {
#if defined(LLAM_ENABLE_TEST_HOOKS)
        if (binding->refuse_probe) error = ENOTSUP;
        else
#endif
        error = llam_signal_read(frame, entry_errno, signal_low, signal_high, binding->probe);
    } else if (control != NULL && bank != NULL &&
               llam_shared_signal_identity(&request, control, bank)) {
        unsigned phase = atomic_load_explicit(&bank->phase, memory_order_acquire);
        llam_shard_t *shard = llam_shared_shard(control);
        if (request.kind == LLAM_ASYNC_SIGNAL_WITNESS && phase == LLAM_ASYNC_BANK_OBSERVED) {
            /* Genuine return already observed the old restore. Do not compare
             * its dispatch tuple against a later cooperative dispatch. */
            error = 0;
        } else if (llam_shared_stopped_tuple(control, bank,
                                             request.kind != LLAM_ASYNC_SIGNAL_CAPTURE)) {
            if (request.kind == LLAM_ASYNC_SIGNAL_CAPTURE && phase == LLAM_ASYNC_BANK_CAPTURING &&
                control->capture_op == bank && control->arrival_ticket == bank &&
                llam_shared_on_stack(bank->task, llam_signal_frame_sp(frame)) &&
                llam_shared_pc_allowed(control, bank->task, llam_signal_frame_pc(frame), NULL, NULL) &&
                atomic_load_explicit(&binding->scheduler_run, memory_order_acquire) ==
                    bank->capture_m_run) {
                llam_async_signal_machine_t *machine = bank->native;
                sigset_t scheduler_mask = {0};
                uint64_t mask_bits[LLAM_ASYNC_SIGNAL_MASK_WORDS];
                for (size_t i = 0U; i < LLAM_ASYNC_SIGNAL_MASK_WORDS; ++i)
                    mask_bits[i] = atomic_load_explicit(&binding->scheduler_mask[i], memory_order_relaxed);
                int scheduler_errno = atomic_load_explicit(&binding->scheduler_errno, memory_order_relaxed);
                error = llam_signal_read(frame, entry_errno, signal_low, signal_high, machine);
                sigset_t task_mask;
                if (error == 0) llam_signal_machine_mask(machine, &task_mask);
                if (error == 0 && llam_signal_mask_blocks(&task_mask, binding->signal_number)) {
                    error = ENOTSUP; /* Never silently unblock an incompatible G mask. */
                }
                if (error == 0) {
                    memcpy(&scheduler_mask, mask_bits, sizeof(mask_bits));
                    error = llam_signal_redirect(frame, control->scheduler, &scheduler_mask, signal_low, signal_high);
                }
                if (error == 0) {
                    bank->capture_scheduler_sp = llam_shared_scheduler_sp(control->scheduler);
                    llam_shared_signal_event(control, LLAM_ASYNC_SHARED_TEST_CAPTURE, bank,
                                              llam_signal_machine_flags(machine), llam_signal_frame_flags(frame));
                    atomic_store_explicit(&bank->task->preempt_requested, 0U, memory_order_release);
                    atomic_fetch_add_explicit(&control->captures, 1U, memory_order_relaxed);
                    atomic_fetch_add_explicit(&shard->metrics.async_preempt_captures, 1U, memory_order_relaxed);
                    atomic_fetch_add_explicit(&shard->metrics.async_preempt_context_bytes,
                                              sizeof(*machine), memory_order_relaxed);
                    /* No other phase writer can enter before this actual M's
                     * normal OS return. Controller cannot cancel CLAIMED. */
                    atomic_store_explicit(&bank->phase, LLAM_ASYNC_BANK_CAPTURED, memory_order_release);
                    return_errno = scheduler_errno;
                }
            } else if (request.kind == LLAM_ASYNC_SIGNAL_RESTORE && phase == LLAM_ASYNC_BANK_GATE &&
                       control->restore_op == bank && control->pending_gate == bank &&
                       llam_signal_frame_pc(frame) >= (uintptr_t)llam_signal_gate_begin &&
                       llam_signal_frame_pc(frame) < (uintptr_t)llam_signal_gate_end &&
                       llam_signal_frame_sp(frame) == bank->restore_scheduler_sp &&
                       llam_signal_frame_sp(frame) == llam_shared_scheduler_sp(control->scheduler)) {
                const llam_async_signal_machine_t *machine = bank->native;
#if defined(LLAM_ENABLE_TEST_HOOKS) || defined(LLAM_ENABLE_ASYNC_PROFILE)
                uint64_t fresh_pstate = llam_signal_frame_flags(frame);
#endif
                error = llam_signal_restore(frame, machine, signal_low, signal_high);
                if (error == 0) {
                    return_errno = machine->entry_errno; /* BEFORE final publication. */
                    llam_shared_signal_event(control, LLAM_ASYNC_SHARED_TEST_RESTORE, bank,
                                              fresh_pstate, llam_signal_frame_flags(frame));
                    atomic_fetch_add_explicit(&control->restores, 1U, memory_order_relaxed);
                    atomic_fetch_add_explicit(&shard->metrics.async_preempt_resumes, 1U, memory_order_relaxed);
                    atomic_store_explicit(&bank->phase, LLAM_ASYNC_BANK_RESTORING, memory_order_release);
                }
            } else if (request.kind == LLAM_ASYNC_SIGNAL_WITNESS && phase == LLAM_ASYNC_BANK_RESTORING &&
                       llam_shared_on_stack(bank->task, llam_signal_frame_sp(frame))) {
                unsigned expected = LLAM_ASYNC_BANK_RESTORING;
                /* Read-only later native frame; not the restore response. */
                error = atomic_compare_exchange_strong_explicit(&bank->phase, &expected,
                    LLAM_ASYNC_BANK_OBSERVED, memory_order_acq_rel, memory_order_acquire) ? 0 : EAGAIN;
            }
        }
    } else {
        error = EINVAL;
    }
#if defined(LLAM_ENABLE_TEST_HOOKS)
    if (error != 0) {
        binding->refusal.pc = llam_signal_frame_pc(frame);
        binding->refusal.sp = llam_signal_frame_sp(frame);
        binding->refusal.pstate = llam_signal_frame_flags(frame);
#if LLAM_ARCH_AARCH64
        memcpy(binding->refusal.first_record, frame->uc_mcontext.__reserved,
               sizeof(binding->refusal.first_record));
#else
        binding->refusal.first_record[0] = frame->uc_mcontext.mc_flags;
        binding->refusal.first_record[1] = (uint32_t)frame->uc_mcontext.mc_fpformat;
#endif
    }
    if (error == 0 && request.kind < LLAM_ASYNC_SIGNAL_COMMAND_COUNT) {
        tail_observed = atomic_exchange_explicit(&binding->tail_observed[request.kind], NULL,
                                                 memory_order_acq_rel);
        tail_release = atomic_load_explicit(&binding->tail_release[request.kind], memory_order_acquire);
    }
#endif
    binding->result_error = error;
    /* LAST access to request/control/G/bank/binding memory. Reuse may begin
     * after this release, but M's handler/altstack remains live until OS return. */
    atomic_store_explicit(&binding->stamp,
        (stamp & ~(uint64_t)LLAM_ASYNC_SIGNAL_STATE_MASK) | LLAM_ASYNC_SIGNAL_RESULT,
        memory_order_release);
#if defined(LLAM_ENABLE_TEST_HOOKS)
    if (tail_observed != NULL && tail_release != NULL) {
        atomic_store_explicit(tail_observed, 1U, memory_order_release);
        unsigned spins = 0U;
        while (atomic_load_explicit(tail_release, memory_order_acquire) == 0U && spins < 1000000000U) {
#if LLAM_ARCH_AARCH64
            __asm__ volatile("yield");
#else
            __asm__ volatile("pause");
#endif
            ++spins;
        }
        if (atomic_load_explicit(tail_release, memory_order_acquire) == 0U) _Exit(125);
    }
#endif
    return return_errno; /* Only locals/own handler stack after RESULT. */
}
#endif
static bool llam_shared_pin_bank_locked(llam_shared_control_t *control, llam_shared_bank_t *bank) {
    if (bank->readers == UINT_MAX ||
        atomic_load_explicit(&control->binding_ops, memory_order_relaxed) == UINT_MAX ||
        !llam_task_scan_ref_try_acquire(control->runtime, bank->task)) {
        return false;
    }
    ++bank->readers;
    atomic_fetch_add_explicit(&control->binding_ops, 1U, memory_order_relaxed);
    return true;
}

#if defined(LLAM_ENABLE_TEST_HOOKS)
static void llam_shared_test_tail(llam_shared_control_t *control, unsigned point);
#endif

/* Last bank/task access and the final binding notification remain under the
 * close exclusion. A READY handoff does NOT free or reuse the native payload. */
static void llam_shared_finish_locked(llam_shared_control_t *control,
                                       llam_shared_bank_t **slot, bool discard) {
    llam_shared_bank_t *bank = *slot;
    llam_task_t *task;
    unsigned phase;
    if (bank == NULL || bank->readers != 1U) llam_shared_uncertain();
    task = bank->task;
    phase = atomic_load_explicit(&bank->phase, memory_order_acquire);
    if (discard || phase == LLAM_ASYNC_BANK_OBSERVED) {
        if (control->arrival_ticket == bank) control->arrival_ticket = NULL;
        if (control->pending_gate == bank) control->pending_gate = NULL;
        atomic_store_explicit(&bank->phase, LLAM_ASYNC_BANK_FREE, memory_order_release);
    }
    *slot = NULL;
    /* LAST bank access. The independent task scan pin remains valid even if
     * M retires the capsule before the final controller task access. */
    --bank->readers;
#if defined(LLAM_ENABLE_TEST_HOOKS)
    if (!discard && phase == LLAM_ASYNC_BANK_OBSERVED &&
        atomic_load_explicit(&control->tail_observed[LLAM_ASYNC_SHARED_TEST_TASK_READER],
                              memory_order_acquire) != NULL) {
        llam_shared_unlock(control);
        llam_shared_test_tail(control, LLAM_ASYNC_SHARED_TEST_TASK_READER);
        llam_shared_lock(control);
    }
#endif
    (void)llam_task_scan_ref_release(control->runtime, task);
    if (atomic_fetch_sub_explicit(&control->binding_ops, 1U, memory_order_release) == 0U) {
        llam_shared_uncertain();
    }
    bool active = control->capture_op != NULL || control->restore_op != NULL || control->pending_gate != NULL;
    /* Clearing follows bank phase/reader retirement, under the same service
     * locks as every publisher. False skips ordinary cooperative return work;
     * it never authorizes bank reuse, G reclamation or binding close. */
    atomic_store_explicit(&control->native_return_active, active, memory_order_release);
    atomic_store_explicit(&control->urgent_service, active, memory_order_release);
    llam_kick_shard(llam_shared_shard(control));
}

static int llam_shared_range_compare(const void *left, const void *right) {
    const llam_async_preempt_range_t *a = left, *b = right;
    return a->begin < b->begin ? -1 : a->begin != b->begin;
}

static const llam_managed_scheduler_ops_t g_llam_shared_managed_ops;

static void llam_shared_free_controls(llam_shared_control_t *group) {
    if (group == NULL) return;
    if (group->runtime != NULL && group->runtime->managed_capsule_pool == &group->capsule_pool)
        group->runtime->managed_capsule_pool = NULL;
    llam_capsule_pool_destroy(&group->capsule_pool);
#if LLAM_PLATFORM_DARWIN
    if (group->controller_wake != NULL) dispatch_release(group->controller_wake);
#else
    if (group->controller_pipe[0] >= 0) (void)close(group->controller_pipe[0]);
    if (group->controller_pipe[1] >= 0) (void)close(group->controller_pipe[1]);
#endif
    for (size_t i = 0U; i < group->control_count; ++i) {
        llam_worker_t *worker = group[i].worker;
        if (worker != NULL && worker->async_preempt_control == &group[i])
            worker->async_preempt_control = NULL;
        free(group[i].probe_context);
        free(group[i].native_binding);
    }
#if LLAM_ASYNC_CONTEXT_SIGNAL
    if (group->owns_process_signal)
        llam_async_signal_process_release(group->runtime->async_preempt_signal);
#endif
    free(group);
}

/* Host-side preparation precedes all task capsule allocation and native binds. */
static int llam_shared_prepare(llam_runtime_t *runtime) {
    bool registered = runtime->execution_lease_mode == LLAM_EXECUTION_LEASE_REGISTERED;
    size_t count = registered ? runtime->active_shards + runtime->managed_worker_count : 1U;
    llam_shared_control_t *group;
    int error = ENOMEM;
    if (count == 0U || count > SIZE_MAX / sizeof(*group) ||
        atomic_load_explicit(&runtime->async_preempt_enabled, memory_order_acquire) != 0U) {
        errno = ENOTSUP;
        return -1;
    }
    group = calloc(count, sizeof(*group));
    if (group == NULL) return -1;
    group->control_count = count;
    group->runtime = runtime;
#if LLAM_PLATFORM_DARWIN
    group->controller_wake = dispatch_semaphore_create(0L);
    if (group->controller_wake == NULL) goto failed;
#else
    group->controller_pipe[0] = group->controller_pipe[1] = -1;
    if (pipe2(group->controller_pipe, O_NONBLOCK | O_CLOEXEC) != 0) { error = errno; goto failed; }
#endif
    atomic_init(&group->capsule_bytes, 0U);
    for (size_t i = 0U; i < count; ++i) {
        llam_shared_control_t *control = &group[i];
        control->runtime = runtime;
        control->group = group;
        control->registered = registered;
        control->worker = !registered ? &runtime->shards[0].primary_worker :
            (i < runtime->active_shards ? &runtime->shards[i].primary_worker :
             &runtime->managed_workers[i - runtime->active_shards]);
        control->scheduler = registered ? NULL : &runtime->shards[0].scheduler_ctx;
        control->native_enabled = !registered || control->worker->role != LLAM_WORKER_ROLE_LEGACY_COMPAT;
#if LLAM_ASYNC_CONTEXT_SIGNAL
        control->ops = &g_llam_signal_async_ops;
#else
        control->ops = &g_llam_darwin_async_ops;
#endif
        control->native_binding = calloc(1U, control->ops->binding_size);
        if (posix_memalign(&control->probe_context, 64U, control->ops->context_size) == 0)
            memset(control->probe_context, 0, control->ops->context_size);
        if (control->native_binding == NULL || control->probe_context == NULL) goto failed;
        atomic_init(&control->start_phase, LLAM_SHARED_CLOSED);
        atomic_init(&control->probe_phase, 0U);
        atomic_init(&control->start_error, 0);
        atomic_init(&control->controller_busy, 0U);
        atomic_init(&control->urgent_service, false);
        atomic_init(&control->native_return_active, false);
        atomic_init(&control->binding_ops, 0U);
        atomic_init(&control->retire_depth, 0U);
        atomic_init(&control->ready_depth, 0U);
        atomic_init(&control->finalizer_depth, 0U);
        atomic_init(&control->execution_debt, 0U);
        atomic_init(&control->requests, 0U);
        atomic_init(&control->captures, 0U);
        atomic_init(&control->restores, 0U);
        atomic_init(&control->witnesses, 0U);
        atomic_init(&control->bindings_created, 0U);
        atomic_init(&control->bindings_closed, 0U);
#if defined(LLAM_ENABLE_TEST_HOOKS)
        for (unsigned i = 0U; i < LLAM_ASYNC_SHARED_TEST_POINT_COUNT; ++i) {
            atomic_init(&control->tail_observed[i], NULL);
            atomic_init(&control->tail_release[i], NULL);
        }
        atomic_init(&control->scheduler_error_checks, 0U);
#endif
#if defined(LLAM_ENABLE_TEST_HOOKS) || defined(LLAM_ENABLE_ASYNC_PROFILE)
        atomic_init(&control->event_next, 0U);
        for (unsigned i = 0U; i < 512U; ++i) atomic_init(&control->event_ready[i], 0U);
#endif
    }
    size_t stride = (group->ops->context_size + 63U) & ~(size_t)63U;
    uint64_t budget = llam_managed_catalog_budget(runtime);
    if (budget == 0U) budget = LLAM_ASYNC_SHARED_BUDGET;
    if (stride < group->ops->context_size || stride > (SIZE_MAX - sizeof(llam_shared_capsule_t)) / 2U) {
        error = EOVERFLOW;
        goto failed;
    }
    if (llam_capsule_pool_init(&group->capsule_pool,
            sizeof(llam_shared_capsule_t) + 2U * stride, 0U, budget, count * 2U + 2U,
            NULL, NULL, NULL) != 0) {
        error = errno;
        goto failed;
    }
#if LLAM_ASYNC_CONTEXT_SIGNAL
    if (llam_async_signal_process_acquire(&runtime->async_preempt_signal) != 0) {
        error = errno;
        goto failed;
    }
    group->owns_process_signal = true;
    for (size_t i = 0U; i < count; ++i)
        llam_async_signal_prepare(group[i].native_binding, group[i].worker,
                                  group[i].probe_context, runtime->async_preempt_signal);
#endif
    runtime->async_preempt_control = group;
    for (size_t i = 0U; i < count; ++i)
        group[i].worker->async_preempt_control = &group[i];
    runtime->managed_capsule_pool = &group->capsule_pool;
    if (registered) {
        runtime->managed_scheduler_ops = &g_llam_shared_managed_ops;
        runtime->managed_scheduler_data = group;
    }
    atomic_store_explicit(&runtime->async_preempt_enabled, 1U, memory_order_release);
    return 0;
failed:
    llam_shared_free_controls(group);
    errno = error;
    return -1;
}

int llam_async_shared_enable(llam_runtime_t *runtime,
                              const llam_async_preempt_range_t *ranges,
                              size_t count, bool process_failfast_ack) {
    llam_runtime_t *pinned = NULL;
    llam_async_preempt_range_t *copied = NULL;
    bool expected = false;
    int error = 0;
    if (!process_failfast_ack || ranges == NULL || count == 0U || count > 128U) {
        errno = EINVAL;
        return -1;
    }
    if (llam_runtime_begin_public_op(runtime, &pinned) != 0) return -1;
    runtime = pinned;
    if (!atomic_compare_exchange_strong_explicit(&runtime->exec_started, &expected, true,
            memory_order_acq_rel, memory_order_acquire)) {
        llam_runtime_end_public_op(pinned);
        errno = EBUSY;
        return -1;
    }
    bool registered = runtime->execution_lease_mode == LLAM_EXECUTION_LEASE_REGISTERED;
    if ((!registered && (runtime->active_shards != 1U || !runtime->deterministic)) ||
        runtime->preempt_mode < LLAM_PREEMPT_AUTO ||
        (!registered && runtime->experimental_dynamic_shards != 0U) ||
        runtime->experimental_shard_rings != 0U || llam_lockfree_normq_enabled(runtime) ||
        !llam_shared_scheduler_compatible(&runtime->shards[0].scheduler_ctx) ||
        runtime->shards[0].opaque_helper_thread_started || llam_runtime_has_live_tasks(runtime) ||
        atomic_load_explicit(&runtime->async_preempt_enabled, memory_order_acquire) != 0U) {
        error = ENOTSUP;
        goto done;
    }
    copied = malloc(count * sizeof(*copied));
    if (copied == NULL) { error = ENOMEM; goto done; }
    memcpy(copied, ranges, count * sizeof(*copied));
    qsort(copied, count, sizeof(*copied), llam_shared_range_compare);
    for (size_t i = 0U; i < count; ++i) {
        if (copied[i].begin == 0U || copied[i].end <= copied[i].begin ||
            (i != 0U && copied[i].begin < copied[i - 1U].end)) {
            error = EINVAL;
            goto done;
        }
    }
    if (llam_shared_prepare(runtime) != 0) { error = errno; goto done; }
    runtime->async_preempt_ranges = copied;
    runtime->async_preempt_range_count = count;
    runtime->async_preempt_range_capacity = count;
    llam_preempt_capabilities_note_ranges(runtime, count);
    copied = NULL;
done:
    free(copied);
    atomic_store_explicit(&runtime->exec_started, false, memory_order_release);
    llam_runtime_end_public_op(pinned);
    if (error != 0) errno = error;
    return error != 0 ? -1 : 0;
}

int llam_async_shared_prepare_registered(llam_runtime_t *runtime) {
    if (runtime == NULL || runtime->execution_lease_mode != LLAM_EXECUTION_LEASE_REGISTERED ||
        runtime->preempt_mode < LLAM_PREEMPT_AUTO ||
        runtime->experimental_shard_rings != 0U || llam_lockfree_normq_enabled(runtime) ||
        llam_runtime_has_live_tasks(runtime) ||
        atomic_load_explicit(&runtime->exec_started, memory_order_acquire)) {
        errno = ENOTSUP;
        return -1;
    }
    return llam_shared_prepare(runtime);
}

static bool llam_shared_control_has_debt(const llam_shared_control_t *control) {
    return atomic_load_explicit(&control->execution_debt, memory_order_acquire) != 0U ||
           atomic_load_explicit(&control->retire_depth, memory_order_acquire) != 0U ||
           atomic_load_explicit(&control->finalizer_depth, memory_order_acquire) != 0U ||
           atomic_load_explicit(&control->binding_ops, memory_order_acquire) != 0U;
}

bool llam_async_shared_has_debt(const llam_runtime_t *runtime) {
    llam_shared_control_t *group = llam_shared_control(runtime);
    if (group == NULL) return false;
    for (size_t i = 0U; i < group->control_count; ++i)
        if (llam_shared_control_has_debt(&group[i])) return true;
    return false;
}

int llam_async_shared_run_begin(llam_runtime_t *runtime) {
    llam_shared_control_t *group = llam_shared_control(runtime);
    if (group == NULL) return 0;
    for (size_t i = 0U; i < group->control_count; ++i) {
        llam_shared_control_t *control = &group[i];
        if (llam_shared_control_has_debt(control) || control->binding_live ||
            control->run_generation == UINT64_MAX) {
            errno = EBUSY;
            return -1;
        }
    }
#if LLAM_PLATFORM_DARWIN
    int mask_error = pthread_sigmask(SIG_SETMASK, NULL, &group->launch_mask);
    if (mask_error != 0) { errno = mask_error; return -1; }
#endif
    for (size_t i = 0U; i < group->control_count; ++i) {
        llam_shared_control_t *control = &group[i];
        ++control->run_generation;
#if defined(LLAM_ENABLE_TEST_HOOKS) || defined(LLAM_ENABLE_ASYNC_PROFILE)
        /* A bounded prefix belongs to this run. Prior Ms and controller have
         * joined before begin, so no old publisher can race the cold reset. */
        atomic_store_explicit(&control->event_next, 0U, memory_order_relaxed);
        for (unsigned event = 0U; event < 512U; ++event)
            atomic_store_explicit(&control->event_ready[event], 0U, memory_order_relaxed);
#endif
        control->next_probe_ns = 0U;
        atomic_store_explicit(&control->start_error, 0, memory_order_relaxed);
        atomic_store_explicit(&control->probe_phase, 0U, memory_order_relaxed);
        atomic_store_explicit(&control->start_phase, LLAM_SHARED_PREPARING, memory_order_release);
    }
    return 0;
}

void llam_async_shared_run_abort(llam_runtime_t *runtime, int error) {
    llam_shared_control_t *group = llam_shared_control(runtime);
    if (group == NULL) return;
    if (error == 0) error = ECANCELED;
    for (size_t i = 0U; i < group->control_count; ++i) {
        int expected = 0;
        llam_shared_control_t *control = &group[i];
        atomic_compare_exchange_strong_explicit(&control->start_error, &expected, error,
                                                 memory_order_acq_rel, memory_order_acquire);
        atomic_store_explicit(&control->start_phase, LLAM_SHARED_ABORTED, memory_order_release);
        llam_kick_shard(control->worker->compat_shard);
    }
}

static int llam_shared_driver_ready(llam_runtime_t *runtime, bool restarting) {
    llam_shared_control_t *control = llam_shared_worker_control(g_llam_tls_worker);
    int error;
    if (llam_shared_control(runtime) == NULL) return 0;
    if (control == NULL || control->runtime != runtime || g_llam_tls_scheduler_ctx == NULL) {
        errno = EINVAL;
        return -1;
    }
    control->scheduler = g_llam_tls_scheduler_ctx;
    if (!control->native_enabled) {
#if LLAM_PLATFORM_DARWIN
        /* Other Ms are created after the primary adopted its native policy.
         * The legacy lane keeps the original caller mask instead. */
        int mask_error = pthread_sigmask(SIG_SETMASK, &control->group->launch_mask, NULL);
        if (mask_error != 0) { errno = mask_error; return -1; }
#endif
        control->available_isa = UINT64_MAX; /* Excluded from the native profile intersection. */
        atomic_store_explicit(&control->probe_phase, 2U, memory_order_release);
        return 0;
    }
    control->available_isa = LLAM_LLPM2_ISA_BASE | LLAM_LLPM2_ISA_FP_SIMD;
#if LLAM_PLATFORM_DARWIN && LLAM_ARCH_AARCH64
    int lse = 0;
    size_t lse_size = sizeof(lse);
    if (sysctlbyname("hw.optional.arm.FEAT_LSE", &lse, &lse_size, NULL, 0U) == 0 &&
        lse_size == sizeof(lse) && lse != 0) control->available_isa |= LLAM_LLPM2_ISA_LSE;
#elif LLAM_PLATFORM_LINUX && LLAM_ARCH_AARCH64
    if ((getauxval(AT_HWCAP) & HWCAP_ATOMICS) != 0U) control->available_isa |= LLAM_LLPM2_ISA_LSE;
#endif
    if (!llam_shared_scheduler_compatible(control->scheduler)) { errno = ENOTSUP; return -1; }
    if (control->ops->bind_current(control->native_binding) != 0) return -1;
    error = pthread_sigmask(SIG_SETMASK, NULL, &control->scheduler_mask);
    if (error != 0) {
        control->ops->close(control->native_binding);
        errno = error;
        return -1;
    }
    llam_shared_lock(control);
    control->binding_incarnation = atomic_load_explicit(&control->worker->binding_incarnation,
                                                        memory_order_acquire);
    control->binding_live = true;
    if (control->registered) {
        /* Registry unbind must fail while this M owns native service, even
         * after it returned its P and has no active user continuation. */
        atomic_store_explicit(&control->worker->async_preempt_state,
                              (uintptr_t)control, memory_order_release);
    }
    atomic_fetch_add_explicit(&control->bindings_created, 1U, memory_order_relaxed);
#if LLAM_ASYNC_CONTEXT_SIGNAL
    atomic_store_explicit(&control->probe_phase, 2U, memory_order_release);
#endif
    llam_shared_unlock(control);
#if !LLAM_ASYNC_CONTEXT_SIGNAL
#if defined(LLAM_ENABLE_TEST_HOOKS)
    llam_shared_test_tail(control, LLAM_ASYNC_SHARED_TEST_BEFORE_PROBE);
#endif
    /* Arm only after the final native call has returned. A Mach read while
     * nanosleep/mutex code is running can observe a transient BTYPE or other
     * state outside the managed baseline. This release starts a syscall-free
     * wait; do not weaken native state admission to accept those snapshots. */
    atomic_store_explicit(&control->probe_phase, 1U, memory_order_release);
#endif
    while (atomic_load_explicit(&control->probe_phase, memory_order_acquire) == 1U &&
           atomic_load_explicit(&control->start_phase, memory_order_acquire) == LLAM_SHARED_PREPARING) {
        if (atomic_load_explicit(&runtime->stop_requested, memory_order_acquire) ||
            atomic_load_explicit(&runtime->shutdown_requested, memory_order_acquire)) {
            if (restarting) {
                atomic_store_explicit(&control->start_error, ECANCELED, memory_order_release);
                atomic_store_explicit(&control->start_phase, LLAM_SHARED_ABORTED, memory_order_release);
            } else {
                llam_async_shared_run_abort(runtime, ECANCELED);
            }
            break;
        }
#if !LLAM_ASYNC_CONTEXT_SIGNAL
#if LLAM_ARCH_AARCH64
        __asm__ volatile("yield" ::: "memory");
#else
        __asm__ volatile("pause" ::: "memory");
#endif
#else
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
        (void)nanosleep(&delay, NULL); /* No user G may run before all probes pass. */
#endif
    }
    error = atomic_load_explicit(&control->start_error, memory_order_acquire);
    if (error != 0 || atomic_load_explicit(&control->probe_phase, memory_order_acquire) != 2U) {
        errno = error != 0 ? error : ENOTSUP;
        return -1;
    }
    return 0;
}

int llam_async_shared_driver_ready(llam_runtime_t *runtime) {
    return llam_shared_driver_ready(runtime, false);
}

int llam_async_shared_run_commit(llam_runtime_t *runtime) {
    llam_shared_control_t *group = llam_shared_control(runtime);
    if (group == NULL) return 0;
    for (size_t i = 0U; i < group->control_count; ++i) {
        if (atomic_load_explicit(&group[i].probe_phase, memory_order_acquire) != 2U ||
            atomic_load_explicit(&group[i].start_phase, memory_order_acquire) != LLAM_SHARED_PREPARING ||
            atomic_load_explicit(&runtime->stop_requested, memory_order_acquire) ||
            atomic_load_explicit(&runtime->shutdown_requested, memory_order_acquire)) {
            errno = ECANCELED;
            return -1;
        }
    }
    if (runtime->managed_catalog != NULL) {
        uint64_t available = UINT64_MAX;
        for (size_t i = 0U; i < group->control_count; ++i) available &= group[i].available_isa;
        if (llam_managed_catalog_async_commit(runtime, available) != 0) return -1;
    }
    for (size_t i = 0U; i < group->control_count; ++i)
        atomic_store_explicit(&group[i].start_phase, LLAM_SHARED_OPEN, memory_order_release);
    bool enabled = runtime->managed_catalog == NULL || llam_managed_catalog_async_enabled(runtime);
    llam_preempt_capabilities_note_async_result(runtime, enabled,
        enabled ? LLAM_PREEMPT_FAILURE_NONE : LLAM_PREEMPT_FAILURE_COOPERATIVE_MODE);
    return 0;
}

bool llam_async_shared_driver_close(llam_runtime_t *runtime) {
    llam_shared_control_t *group = llam_shared_control(runtime);
    llam_shared_control_t *control = group != NULL && group->registered
        ? llam_shared_worker_control(g_llam_tls_worker) : group;
    if (group == NULL) return true;
    if (control == NULL || control->runtime != runtime) return false;
    llam_shared_lock(control);
#if LLAM_ASYNC_CONTEXT_SIGNAL
    if (!llam_async_signal_idle(control->native_binding)) {
        llam_shared_unlock(control);
        return false;
    }
#endif
    if ((llam_shared_control_has_debt(control) && !llam_shared_terminal_drained(control)) ||
        control->capture_op != NULL ||
        control->arrival_ticket != NULL || control->restore_op != NULL || control->pending_gate != NULL) {
        llam_shared_unlock(control);
        return false;
    }
    if (control->binding_live) {
        control->binding_live = false;
        atomic_store_explicit(&control->urgent_service, false, memory_order_release);
        atomic_store_explicit(&control->native_return_active, false, memory_order_release);
        control->ops->close(control->native_binding);
        if (control->registered)
            atomic_store_explicit(&control->worker->async_preempt_state, 0U, memory_order_release);
        atomic_fetch_add_explicit(&control->bindings_closed, 1U, memory_order_release);
    }
    llam_shared_unlock(control);
    return true;
}

void llam_async_shared_run_end(llam_runtime_t *runtime) {
    llam_shared_control_t *group = llam_shared_control(runtime);
    if (group == NULL) return;
    for (size_t i = 0U; i < group->control_count; ++i) {
        llam_shared_control_t *control = &group[i];
        unsigned expected;
        do {
            expected = 0U;
            if (atomic_compare_exchange_weak_explicit(&control->controller_busy, &expected, 1U,
                                                      memory_order_acquire, memory_order_relaxed)) break;
            const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
            (void)nanosleep(&delay, NULL);
        } while (true);
        if (control->registered) {
            if (control->binding_live || llam_shared_control_has_debt(control)) llam_shared_uncertain();
            control->scheduler = NULL;
        } else if (!llam_async_shared_driver_close(runtime)) llam_shared_uncertain();
        atomic_store_explicit(&control->start_phase, LLAM_SHARED_CLOSED, memory_order_release);
        atomic_store_explicit(&control->controller_busy, 0U, memory_order_release);
    }
}

int llam_async_shared_task_init(llam_task_t *task) {
    llam_shared_control_t *control = llam_shared_control(task->owner_runtime);
    llam_shared_capsule_t *capsule;
    size_t stride, charge;
    if (control == NULL || (task->managed_catalog != NULL &&
        task->managed_execution_class == LLAM_MANAGED_EXECUTION_LEGACY)) return 0;
    stride = (control->ops->context_size + 63U) & ~(size_t)63U;
    if (stride < control->ops->context_size || stride > (SIZE_MAX - sizeof(*capsule)) / 2U) {
        errno = EOVERFLOW;
        return -1;
    }
    charge = control->group->capsule_pool.charge;
    capsule = llam_capsule_pool_take(&control->group->capsule_pool, g_llam_tls_worker == NULL);
    if (capsule == NULL) return -1;
    memset(capsule, 0, sizeof(*capsule));
    atomic_fetch_add_explicit(&control->group->capsule_bytes, charge, memory_order_release);
    capsule->charge = charge;
    atomic_init(&capsule->started, false);
    atomic_init(&capsule->origin, NULL);
    capsule->task = task;
    for (unsigned i = 0U; i < 2U; ++i) {
        atomic_init(&capsule->banks[i].phase, LLAM_ASYNC_BANK_FREE);
        atomic_init(&capsule->banks[i].arrival_complete, 0U);
        capsule->banks[i].task = task;
        capsule->banks[i].native = capsule->payload + i * stride;
    }
    task->async_preempt_task = capsule;
    return 0;
}

void llam_async_shared_task_destroy(llam_task_t *task) {
    llam_shared_capsule_t *capsule = task->async_preempt_task;
    llam_shared_control_t *control;
    if (capsule == NULL) return;
    control = llam_shared_control(task->owner_runtime);
    if (control == NULL || capsule->ready != NULL || capsule->retire_queued) llam_shared_uncertain();
    for (unsigned i = 0U; i < 2U; ++i) {
        if (capsule->banks[i].readers != 0U ||
            atomic_load_explicit(&capsule->banks[i].phase, memory_order_acquire) != LLAM_ASYNC_BANK_FREE) {
            llam_shared_uncertain();
        }
    }
    task->async_preempt_task = NULL;
    if (atomic_fetch_sub_explicit(&control->group->capsule_bytes, capsule->charge, memory_order_release) <
        capsule->charge) llam_shared_uncertain();
    llam_capsule_pool_put(&control->group->capsule_pool, capsule);
}

bool llam_async_shared_primary_service(llam_shard_t *shard) {
    if (shard == NULL) return false;
    llam_shared_control_t *control = llam_shared_control(shard->runtime);
    bool ready;
    if (control == NULL) return false;
    llam_shared_lock(control);
    ready = llam_shared_service_locked(control, NULL);
    llam_shared_unlock(control);
    return ready;
}

unsigned llam_async_preempt_ready_depth(const llam_shard_t *shard) {
    llam_shared_control_t *control = shard != NULL ? llam_shared_control(shard->runtime) : NULL;
    return control != NULL ? atomic_load_explicit(&control->ready_depth, memory_order_acquire) : 0U;
}

bool llam_async_preempt_has_origin_obligations(const llam_shard_t *shard) {
    return shard != NULL && llam_async_shared_has_debt(shard->runtime);
}

unsigned llam_async_preempt_task_origin_shard(const llam_task_t *task) {
    llam_shared_control_t *control = llam_shared_task_control(task);
    const llam_shared_capsule_t *capsule = task != NULL ? task->async_preempt_task : NULL;
    return control != NULL && !control->registered && capsule != NULL &&
        atomic_load_explicit(&capsule->started, memory_order_acquire) ? 0U : UINT_MAX;
}

void llam_async_preempt_bind_task(llam_worker_t *worker, llam_task_t *task) {
    llam_shared_capsule_t *capsule = task->async_preempt_task;
    if (capsule != NULL && llam_shared_portable_task(task) &&
        atomic_load_explicit(&task->managed_dispatch_owner, memory_order_acquire) == worker &&
        atomic_load_explicit(&capsule->started, memory_order_acquire)) {
        llam_shared_control_t *origin = atomic_load_explicit(&capsule->origin, memory_order_acquire);
        /* The existing G root retains origin through every native M's join.
         * Matching its immutable worker avoids rebuilding the registry index. */
        if (origin != NULL && origin->registered && origin->worker == worker) return;
    }
    llam_shared_control_t *control = llam_shared_worker_control(worker);
    if (control == NULL) return;
    if (capsule == NULL) {
        if (task->managed_catalog != NULL && task->managed_execution_class == LLAM_MANAGED_EXECUTION_LEGACY)
            return;
        llam_shared_uncertain();
    }
    llam_shared_lock(control);
    llam_shared_wait_dispatch_service_locked(control);
    if (capsule->enqueued ||
        (atomic_load_explicit(&capsule->started, memory_order_acquire) &&
         atomic_load_explicit(&capsule->origin, memory_order_acquire) != control)) llam_shared_uncertain();
    if (!atomic_load_explicit(&capsule->started, memory_order_acquire)) {
        if (atomic_fetch_add_explicit(&control->execution_debt, 1U, memory_order_relaxed) == UINT_MAX) {
            llam_shared_uncertain();
        }
        atomic_store_explicit(&capsule->origin, control, memory_order_release);
        /* Admission of the service root precedes publication to wake producers. */
        atomic_store_explicit(&capsule->started, true, memory_order_release);
    }
    llam_shared_unlock(control);
}

static bool llam_shared_enqueue_locked(llam_shared_control_t *control, llam_task_t *task) {
    llam_shared_capsule_t *capsule = task->async_preempt_task;
    unsigned depth;
    if (control == NULL || capsule == NULL ||
        !atomic_load_explicit(&capsule->started, memory_order_acquire)) return false;
    if (atomic_load_explicit(&capsule->origin, memory_order_acquire) != control || capsule->enqueued)
        llam_shared_uncertain();
    depth = atomic_load_explicit(&control->ready_depth, memory_order_relaxed);
    if (depth == UINT_MAX) llam_shared_uncertain();
    task->queue_next = NULL;
    task->queue_prev = control->ready_tail;
    if (control->ready_tail != NULL) control->ready_tail->queue_next = task;
    else control->ready_head = task;
    control->ready_tail = task;
    capsule->enqueued = true;
    atomic_store_explicit(&control->ready_depth, depth + 1U, memory_order_release);
    return true;
}

bool llam_async_preempt_enqueue_locked(llam_shard_t *shard, llam_task_t *task) {
    llam_shared_control_t *control = llam_shared_control(shard->runtime);
    /* Ordinary producers hold only this P payload lock. A registered task is
     * routed by the dequeue reservation callback before another M may run it. */
    if (control == NULL || control->registered) return false;
    return llam_shared_enqueue_locked(control, task);
}

llam_task_t *llam_async_preempt_take_ready(llam_shard_t *shard, bool fallback) {
    llam_shared_control_t *control = llam_shared_worker_control(g_llam_tls_worker);
    llam_shared_capsule_t *capsule;
    llam_task_t *task;
    unsigned depth;
    if (control == NULL || control->runtime != shard->runtime || atomic_load_explicit(&control->ready_depth, memory_order_acquire) == 0U) return NULL;
    if (!fallback && !control->prefer_origin) {
        control->prefer_origin = true;
        return NULL;
    }
    llam_shared_lock(control);
    if (!llam_shared_service_locked(control, NULL)) {
        llam_shared_unlock(control);
        return NULL;
    }
    task = control->ready_head;
    if (task == NULL) {
        llam_shared_unlock(control);
        return NULL;
    }
    if (control->registered) {
        llam_worker_t *reserved = atomic_load_explicit(&task->managed_dispatch_owner, memory_order_acquire);
        if (reserved != NULL && reserved != control->worker) {
            llam_shared_unlock(control);
            return NULL;
        }
    }
    capsule = task->async_preempt_task;
    if (capsule != NULL && capsule->ready != NULL && control->pending_gate != NULL) {
        /* An old OBSERVED restore still owns this M slot. Keep the next READY
         * G queued; controller finalization wakes M without a lock-held wait. */
        llam_shared_unlock(control);
        return NULL;
    }
    depth = atomic_load_explicit(&control->ready_depth, memory_order_relaxed);
    if (capsule == NULL || !capsule->enqueued || depth == 0U) llam_shared_uncertain();
    control->ready_head = task->queue_next;
    if (control->ready_head != NULL) control->ready_head->queue_prev = NULL;
    else control->ready_tail = NULL;
    task->queue_next = NULL;
    task->queue_prev = NULL;
    capsule->enqueued = false;
    control->prefer_origin = false;
    atomic_store_explicit(&control->ready_depth, depth - 1U, memory_order_release);
    llam_shared_unlock(control);
    return task;
}

static bool llam_shared_portable_task(const llam_task_t *task) {
    /* Qualified PORTABLE effects cannot mutate native signal masks. Signal
     * subscription APIs and arbitrary native callbacks are affine contracts;
     * legacy/expert entries keep full per-G mask virtualization below. */
    return task->managed_catalog != NULL &&
        task->managed_execution_class == LLAM_MANAGED_EXECUTION_PORTABLE;
}

static __attribute__((noinline)) void llam_shared_before_dispatch(llam_shared_control_t *control,
                                         llam_shared_capsule_t *capsule,
                                         int scheduler_errno, bool portable_mask) {
    int rc;
    if (!portable_mask) {
        rc = pthread_sigmask(SIG_SETMASK, NULL, &control->scheduler_mask);
        if (rc != 0) llam_shared_uncertain();
    }
#if LLAM_ASYNC_CONTEXT_SIGNAL
    llam_async_signal_save_scheduler(control->native_binding,
        atomic_load_explicit(&g_llam_tls_worker->run_generation, memory_order_acquire),
        scheduler_errno, &control->scheduler_mask);
#else
    (void)scheduler_errno;
#endif
    if (!capsule->mask_valid) {
        capsule->signal_mask = control->scheduler_mask;
        capsule->mask_valid = true;
    }
    if (!portable_mask && pthread_sigmask(SIG_SETMASK, &capsule->signal_mask, NULL) != 0)
        llam_shared_uncertain();
}

/* Keep bank/transaction work out of the ordinary cooperative save frame. */
static __attribute__((noinline)) bool llam_shared_resume(llam_shared_control_t *control, llam_shared_capsule_t *capsule,
                                 llam_ctx_t *scheduler, llam_task_t *task) {
    llam_shared_bank_t *bank;
    if (capsule->ready == NULL) return false;
    llam_shared_lock(control);
    bank = capsule->ready;
    llam_shared_wait_dispatch_service_locked(control);
    if (control->pending_gate != NULL ||
        atomic_load_explicit(&bank->phase, memory_order_acquire) != LLAM_ASYNC_BANK_READY ||
        bank->task != task) llam_shared_uncertain();
    capsule->ready = NULL;
    bank->restore_g_run = atomic_load_explicit(&task->run_generation, memory_order_acquire);
    bank->restore_m_run = atomic_load_explicit(&g_llam_tls_worker->run_generation, memory_order_acquire);
    control->pending_gate = bank; /* Counted M root before assembly can publish GATE. */
    atomic_store_explicit(&control->native_return_active, true, memory_order_release);
    /* Publish under the same service locks used by controller clearing. A
     * later gate cannot be overwritten by an older "no work" observation. */
    atomic_store_explicit(&control->urgent_service, true, memory_order_release);
    llam_shared_unlock(control);
    llam_shared_notify_controller(control);
    control->ops->save_gate(scheduler, &bank->phase);
    return true; /* Only a genuine G→M return can reach this live native frame. */
}

static __attribute__((noinline)) void llam_shared_arrival(llam_shared_control_t *control, llam_shared_capsule_t *capsule,
                                 llam_task_t *task, bool portable_mask) {
    llam_shared_bank_t *bank;
    sigset_t mask;
    if (portable_mask && !atomic_load_explicit(&control->native_return_active, memory_order_acquire)) {
        /* A completed cooperative save already owns G's error state. A native
         * redirect publishes activity before changing PC (and before the
         * signal mailbox's release). Late uncommitted capture admission must
         * still reject this M stack/unsafe PC and cannot need an arrival. */
        return;
    }
    int task_errno = llam_thread_errno_load(); /* Before any mask/clock/libc operation. */
#if defined(LLAM_ENABLE_TEST_HOOKS) || defined(LLAM_ENABLE_ASYNC_PROFILE)
    /* Normal M context, before mask operations or service-lock contention.
     * Ordinary portable returns add no profiling clock. No clock/helper call
     * is added to a signal handler or suspended target. */
    uint64_t arrival_ns = llam_now_ns();
#endif
    if (portable_mask) mask = control->scheduler_mask;
    else if (pthread_sigmask(SIG_SETMASK, NULL, &mask) != 0) llam_shared_uncertain();
    llam_shared_lock(control);
    bank = control->arrival_ticket;
#if LLAM_ASYNC_CONTEXT_SIGNAL
    if (bank != NULL && bank->task == task &&
        atomic_load_explicit(&bank->phase, memory_order_acquire) == LLAM_ASYNC_BANK_CAPTURED) {
        const llam_async_signal_machine_t *native = bank->native;
        /* Signal return intentionally installed M state before reaching C.
         * Interrupted G sidecars, not current M sampling, are authoritative. */
        llam_signal_machine_mask(native, &mask);
        task_errno = native->entry_errno;
    }
#endif
    capsule->signal_mask = mask;
    capsule->mask_valid = true;
    task->saved_errno = task_errno;
    if (bank != NULL && bank->task == task &&
        atomic_load_explicit(&bank->phase, memory_order_acquire) == LLAM_ASYNC_BANK_CAPTURED) {
        if (bank->capture_g_run != atomic_load_explicit(&task->run_generation, memory_order_acquire) ||
            bank->capture_m_run != atomic_load_explicit(&g_llam_tls_worker->run_generation, memory_order_acquire) ||
            bank->capture_scheduler_sp != llam_shared_scheduler_sp(g_llam_tls_scheduler_ctx)) {
            llam_shared_uncertain();
        }
        llam_task_sample_stack_rsp(task, control->ops->sp(bank->native));
#if defined(LLAM_ENABLE_TEST_HOOKS) || defined(LLAM_ENABLE_ASYNC_PROFILE)
        llam_shared_event_record(control, LLAM_ASYNC_SHARED_TEST_LANDING, bank, 0U, 0U, arrival_ns);
#endif
        atomic_store_explicit(&bank->arrival_complete, 1U, memory_order_release);
    }
    bank = control->restore_op;
    if (bank != NULL && bank->task == task) {
        unsigned phase = atomic_load_explicit(&bank->phase, memory_order_acquire);
        if (phase != LLAM_ASYNC_BANK_RESTORING && phase != LLAM_ASYNC_BANK_OBSERVED) {
            llam_shared_uncertain();
        }
    }
    if (bank != NULL && bank->task == task &&
        atomic_load_explicit(&bank->phase, memory_order_acquire) == LLAM_ASYNC_BANK_RESTORING) {
        /* OBSERVED can retain a late controller reader while this same G
         * makes a newer cooperative dispatch. Only a still-unobserved restore
         * owns the old dispatch identity; never validate it against the new G. */
        unsigned expected = LLAM_ASYNC_BANK_RESTORING;
        if (bank->restore_g_run != atomic_load_explicit(&task->run_generation, memory_order_acquire) ||
            bank->restore_m_run != atomic_load_explicit(&g_llam_tls_worker->run_generation, memory_order_acquire) ||
            bank->restore_scheduler_sp != llam_shared_scheduler_sp(g_llam_tls_scheduler_ctx)) {
            llam_shared_uncertain();
        }
        atomic_compare_exchange_strong_explicit(&bank->phase, &expected, LLAM_ASYNC_BANK_OBSERVED,
                                                 memory_order_acq_rel, memory_order_acquire);
    }
    llam_shared_unlock(control);
    if (!portable_mask && pthread_sigmask(SIG_SETMASK, &control->scheduler_mask, NULL) != 0)
        llam_shared_uncertain();
}

void llam_async_shared_switch_scheduler_to_task(llam_ctx_t *scheduler,
                                                 llam_task_t *task, int scheduler_errno) {
    llam_shared_control_t *control = llam_shared_task_control(task);
    if (control == NULL) {
        llam_task_restore_errno(task);
        llam_ctx_switch(scheduler, &task->ctx);
        return;
    }
    llam_shared_capsule_t *capsule = task->async_preempt_task;
    bool portable_mask = llam_shared_portable_task(task);
    /* The per-M scheduler activation retains these roots through the complete
     * return seam. The dispatch reservation outlives arrival, so a cooperative
     * G cannot reparent its capsule while this activation still uses it. */
    if (capsule == NULL) llam_shared_uncertain();
#if LLAM_ASYNC_CONTEXT_SIGNAL
    llam_shared_before_dispatch(control, capsule, scheduler_errno, portable_mask);
#else
    if (!portable_mask || !capsule->mask_valid)
        llam_shared_before_dispatch(control, capsule, scheduler_errno, portable_mask);
#endif
    llam_task_restore_errno(task);
    if (capsule->ready == NULL || !llam_shared_resume(control, capsule, scheduler, task))
        llam_ctx_switch(scheduler, &task->ctx);
    if (!portable_mask || atomic_load_explicit(&control->native_return_active, memory_order_acquire))
        llam_shared_arrival(control, capsule, task, portable_mask);
}

void llam_async_shared_scheduler_return(llam_shard_t *shard, llam_task_t *task) {
    llam_shared_control_t *control = llam_shared_task_control(task);
    llam_shared_bank_t *bank;
    llam_shared_capsule_t *capsule;
    if (control == NULL) return;
    if (llam_shared_portable_task(task) &&
        !atomic_load_explicit(&control->native_return_active, memory_order_acquire)) return;
    llam_shared_lock(control);
    bank = control->arrival_ticket;
    if (bank != NULL && bank->task == task &&
        atomic_load_explicit(&bank->phase, memory_order_acquire) == LLAM_ASYNC_BANK_CAPTURED) {
        capsule = task->async_preempt_task;
        if (capsule == NULL || capsule->ready != NULL ||
            atomic_load_explicit(&bank->arrival_complete, memory_order_acquire) != 1U) {
            llam_shared_uncertain();
        }
        capsule->ready = bank;
        atomic_store_explicit(&task->state, LLAM_TASK_STATE_RUNNABLE, memory_order_release);
        task->wait_reason = LLAM_WAIT_NONE;
        task->last_yield_ns = 0U;
        task->last_runnable_ns = llam_now_ns();
        atomic_store_explicit(&bank->phase, LLAM_ASYNC_BANK_READY, memory_order_release);
        if (!llam_shared_enqueue_locked(control, task)) llam_shared_uncertain();
        control->arrival_ticket = NULL;
        llam_shared_event(control, LLAM_ASYNC_SHARED_TEST_READY, bank);
        llam_shared_notify_controller(control);
        ++shard->metrics.yields;
        ++shard->metrics.preempt_yields;
    }
    llam_shared_unlock(control);
}

bool llam_async_shared_defer_claimed(llam_runtime_t *runtime, llam_task_t *task) {
    llam_shared_control_t *control = llam_shared_worker_control(g_llam_tls_worker);
    if (control == NULL || control->runtime != runtime || g_llam_tls_task != NULL ||
        g_llam_tls_worker == NULL) return false;
    if (atomic_load_explicit(&task->reclaim_ready, memory_order_acquire) == 0U) llam_shared_uncertain();
    llam_task_invalidate_public_handle(task);
    llam_shared_lock(control);
    task->queue_next = control->finalizer_head;
    control->finalizer_head = task; /* Exclusive claim remains a root after capsule free. */
    atomic_fetch_add_explicit(&control->finalizer_depth, 1U, memory_order_release);
    llam_shared_unlock(control);
    return true;
}

static bool llam_shared_capsule_quiescent(llam_shared_capsule_t *capsule) {
    if (capsule->ready != NULL || capsule->enqueued) return false;
    for (unsigned i = 0U; i < 2U; ++i) {
        if (capsule->banks[i].readers != 0U ||
            atomic_load_explicit(&capsule->banks[i].phase, memory_order_acquire) != LLAM_ASYNC_BANK_FREE) return false;
    }
    return true;
}

bool llam_async_shared_task_quiescent(const llam_task_t *task) {
    llam_shared_control_t *control = llam_shared_task_control(task);
    llam_shared_capsule_t *capsule = task != NULL ? task->async_preempt_task : NULL;
    bool ready;
    if (task == NULL) return false;
    if (control == NULL) return capsule == NULL;
    if (capsule == NULL || control->worker != g_llam_tls_worker) return false;
    llam_shared_lock(control);
    ready = llam_shared_service_locked(control, NULL) &&
        atomic_load_explicit(&capsule->origin, memory_order_acquire) == control &&
        llam_shared_capsule_quiescent(capsule) &&
        atomic_load_explicit(&control->execution_debt, memory_order_acquire) == 1U &&
        atomic_load_explicit(&control->retire_depth, memory_order_acquire) == 0U &&
        atomic_load_explicit(&control->finalizer_depth, memory_order_acquire) == 0U &&
        atomic_load_explicit(&control->binding_ops, memory_order_acquire) == 0U &&
        control->capture_op == NULL && control->arrival_ticket == NULL &&
        control->restore_op == NULL && control->pending_gate == NULL &&
        control->terminal_task == NULL;
    llam_shared_unlock(control);
    return ready;
}

void llam_async_shared_retire_dead(llam_shard_t *shard, llam_task_t *task) {
    llam_shared_control_t *control = llam_shared_task_control(task);
    llam_shared_capsule_t *capsule = task->async_preempt_task;
    bool owed = false;
    if (control == NULL) {
        /* Default-OFF retains the ordinary path, including its reclaim rules. */
        bool claimed = false;
        llam_task_release_stack(task);
        if (!llam_task_scan_ref_try_acquire(shard->runtime, task)) llam_shared_uncertain();
        llam_task_mark_reclaim_ready(task);
        if (atomic_load_explicit(&task->detached, memory_order_acquire) != 0U) {
            unsigned expected = 0U;
            claimed = atomic_compare_exchange_strong_explicit(&task->reclaim_claimed, &expected, 1U,
                                                              memory_order_acq_rel, memory_order_acquire);
        }
        /* A consuming host join may reclaim immediately after readiness.
         * The pin protects our final claim check; only an exclusive claim
         * permits any task access after dropping that pin. */
        (void)llam_task_scan_ref_release(shard->runtime, task);
        if (claimed) llam_reclaim_claimed_task(shard->runtime, task);
        return;
    }
    if (capsule == NULL) llam_shared_uncertain();
    if (control != NULL && capsule != NULL) {
        llam_shared_lock(control);
        if (!llam_shared_capsule_quiescent(capsule)) {
            if (!capsule->retire_queued) {
                capsule->retire_queued = true;
                capsule->retire_next = control->retire_head;
                control->retire_head = capsule;
                atomic_fetch_add_explicit(&control->retire_depth, 1U, memory_order_release);
            }
            llam_shared_unlock(control);
            return; /* Never block an M waiting for the controller's last reader. */
        }
        owed = atomic_load_explicit(&capsule->started, memory_order_acquire);
        llam_async_shared_task_destroy(task);
        llam_shared_unlock(control);
    }
    llam_task_release_stack(task);
    if (!llam_task_scan_ref_try_acquire(shard->runtime, task)) llam_shared_uncertain();
    llam_task_mark_reclaim_ready(task);
    llam_try_reclaim_detached_task(shard->runtime, task);
    (void)llam_task_scan_ref_release(shard->runtime, task);
    /* LAST task access preceded debt release; a host join may already free G. */
    if (owed) {
        if (atomic_fetch_sub_explicit(&control->execution_debt, 1U, memory_order_release) == 0U) {
            llam_shared_uncertain();
        }
        llam_kick_shard(shard);
    }
}

void llam_async_shared_service_retirements(llam_shard_t *shard) {
    llam_shared_control_t *control = llam_shared_worker_control(g_llam_tls_worker);
    llam_shared_capsule_t *retired;
    llam_task_t *claimed;
    if (control == NULL) return;
    if (atomic_load_explicit(&control->retire_depth, memory_order_acquire) == 0U &&
        atomic_load_explicit(&control->finalizer_depth, memory_order_acquire) == 0U) {
        /* Advisory empty-queue hit only. Publication retains a counted root;
         * close_binding still rechecks every queue under its service locks. */
        return;
    }
    llam_shared_lock(control);
    retired = control->retire_head;
    control->retire_head = NULL;
    claimed = control->finalizer_head;
    control->finalizer_head = NULL;
    llam_shared_unlock(control);
    while (retired != NULL) {
        llam_shared_capsule_t *next = retired->retire_next;
        llam_task_t *task = retired->task;
        retired->retire_next = NULL;
        retired->retire_queued = false;
        /* Keep the counted old root until retirement or requeue has finished. */
        llam_async_shared_retire_dead(shard, task);
        atomic_fetch_sub_explicit(&control->retire_depth, 1U, memory_order_release);
        retired = next;
    }
    while (claimed != NULL) {
        llam_task_t *next = claimed->queue_next;
        claimed->queue_next = NULL;
        if (llam_task_try_finalize_deferred_claim(shard->runtime, claimed)) {
            /* Counter remains nonzero through actual task free. */
            atomic_fetch_sub_explicit(&control->finalizer_depth, 1U, memory_order_release);
        } else {
            llam_shared_lock(control);
            claimed->queue_next = control->finalizer_head;
            control->finalizer_head = claimed;
            llam_shared_unlock(control);
        }
        claimed = next;
    }
}

void llam_async_shared_shutdown(llam_runtime_t *runtime) {
    llam_shared_control_t *group = llam_shared_control(runtime);
    if (group == NULL) return;
    if (llam_async_shared_has_debt(runtime) ||
        atomic_load_explicit(&group->capsule_bytes, memory_order_acquire) != 0U) llam_shared_uncertain();
    for (size_t i = 0U; i < group->control_count; ++i)
        if (group[i].binding_live) llam_shared_uncertain();
    /* All native Ms and the controller joined; catalog remains pinned here. */
    atomic_store_explicit(&runtime->async_preempt_enabled, 0U, memory_order_release);
    runtime->async_preempt_control = NULL;
    if (group->registered) {
        runtime->managed_scheduler_ops = NULL;
        runtime->managed_scheduler_data = NULL;
    }
    llam_shared_free_controls(group);
#if LLAM_ASYNC_CONTEXT_SIGNAL
    runtime->async_preempt_signal = 0;
#endif
    free(runtime->async_preempt_ranges);
    runtime->async_preempt_ranges = NULL;
    runtime->async_preempt_range_count = 0U;
    runtime->async_preempt_range_capacity = 0U;
}

static bool llam_shared_control_quiet(const llam_shared_control_t *control) {
    if (control == NULL ||
        atomic_load_explicit(&control->start_phase, memory_order_acquire) != LLAM_SHARED_OPEN ||
        atomic_load_explicit(&control->controller_busy, memory_order_acquire) != 0U ||
        atomic_load_explicit(&control->binding_ops, memory_order_acquire) != 0U ||
        control->capture_op != NULL || control->arrival_ticket != NULL ||
        control->restore_op != NULL || control->pending_gate != NULL ||
        control->retire_head != NULL || control->finalizer_head != NULL || control->terminal_task != NULL ||
        atomic_load_explicit(&control->ready_depth, memory_order_acquire) != 0U ||
        atomic_load_explicit(&control->retire_depth, memory_order_acquire) != 0U ||
        atomic_load_explicit(&control->finalizer_depth, memory_order_acquire) != 0U) {
        return false;
    }
#if LLAM_ASYNC_CONTEXT_SIGNAL
    if (control->native_binding == NULL || !llam_async_signal_idle(control->native_binding)) {
        return false;
    }
#endif
    /* Allocated capsules/charged bytes keep storage alive, not user execution. */
    return true;
}

bool llam_async_shared_liveness_quiet_pair_locked(const llam_runtime_t *runtime) {
    llam_shared_control_t *group = llam_shared_control(runtime);
    if (group == NULL) return false;
    for (size_t i = 0U; i < group->control_count; ++i)
        if (!llam_shared_control_quiet(&group[i])) return false;
    return true;
}

static bool llam_shared_control_stats(const llam_shared_control_t *control, llam_async_shared_stats_t *out) {
    if (control == NULL || out == NULL) return false;
    *out = (llam_async_shared_stats_t){
        .requests = atomic_load_explicit(&control->requests, memory_order_acquire),
        .captures = atomic_load_explicit(&control->captures, memory_order_acquire),
        .restores = atomic_load_explicit(&control->restores, memory_order_acquire),
        .witnesses = atomic_load_explicit(&control->witnesses, memory_order_acquire),
        .retire_depth = atomic_load_explicit(&control->retire_depth, memory_order_acquire),
        .finalizer_depth = atomic_load_explicit(&control->finalizer_depth, memory_order_acquire),
        .execution_debt = atomic_load_explicit(&control->execution_debt, memory_order_acquire),
        .binding_ops = atomic_load_explicit(&control->binding_ops, memory_order_acquire),
        .charged_bytes = atomic_load_explicit(&control->group->capsule_bytes, memory_order_acquire),
        .bindings_created = atomic_load_explicit(&control->bindings_created, memory_order_acquire),
        .bindings_closed = atomic_load_explicit(&control->bindings_closed, memory_order_acquire)
    };
#if defined(LLAM_ENABLE_TEST_HOOKS)
    out->scheduler_error_checks = atomic_load_explicit(&control->scheduler_error_checks, memory_order_acquire);
#endif
    return true;
}

bool llam_async_shared_worker_stats(const llam_worker_t *worker, llam_async_shared_stats_t *out) {
    return llam_shared_control_stats(llam_shared_worker_control(worker), out);
}

bool llam_async_shared_memory_info(const llam_runtime_t *runtime, uint64_t *charged, uint64_t *per_task) {
    llam_shared_control_t *group = llam_shared_control(runtime);
    if (group == NULL || charged == NULL || per_task == NULL) return false;
    *charged = atomic_load_explicit(&group->capsule_bytes, memory_order_acquire);
    *per_task = group->capsule_pool.charge;
    return true;
}

bool llam_async_shared_stats(const llam_runtime_t *runtime, llam_async_shared_stats_t *out) {
    llam_shared_control_t *group = llam_shared_control(runtime);
    if (group == NULL || out == NULL) return false;
    memset(out, 0, sizeof(*out));
    for (size_t i = 0U; i < group->control_count; ++i) {
        llam_async_shared_stats_t item;
        (void)llam_shared_control_stats(&group[i], &item);
        out->requests += item.requests;
        out->captures += item.captures;
        out->restores += item.restores;
        out->witnesses += item.witnesses;
        out->retire_depth += item.retire_depth;
        out->finalizer_depth += item.finalizer_depth;
        out->execution_debt += item.execution_debt;
        out->binding_ops += item.binding_ops;
        out->bindings_created += item.bindings_created;
        out->bindings_closed += item.bindings_closed;
        out->scheduler_error_checks += item.scheduler_error_checks;
    }
    out->charged_bytes = atomic_load_explicit(&group->capsule_bytes, memory_order_acquire);
    return true;
}


#if defined(LLAM_ENABLE_TEST_HOOKS) || defined(LLAM_ENABLE_ASYNC_PROFILE)
size_t llam_async_shared_profile_events(const llam_runtime_t *runtime,
                                        llam_async_shared_event_t *events, size_t capacity) {
    llam_shared_control_t *control = llam_shared_control(runtime);
    size_t count = 0U;
    if (control == NULL || events == NULL) return 0U;
    while (count < capacity && count < 512U &&
           atomic_load_explicit(&control->event_ready[count], memory_order_acquire) != 0U) {
        events[count] = control->events[count];
        ++count;
    }
    return count;
}
#endif

#if defined(LLAM_ENABLE_TEST_HOOKS)
size_t llam_async_shared_test_events(const llam_runtime_t *runtime,
                                     llam_async_shared_event_t *events, size_t capacity) {
    return llam_async_shared_profile_events(runtime, events, capacity);
}

void llam_async_shared_test_scheduler_error(const llam_task_t *task, int expected) {
    int observed = llam_thread_errno_load();
    llam_shared_control_t *control = llam_shared_task_control(task);
    sigset_t mask;
    if (control == NULL) return;
    if (observed != expected || pthread_sigmask(SIG_SETMASK, NULL, &mask) != 0) llam_shared_uncertain();
#if LLAM_ASYNC_CONTEXT_SIGNAL
    const int signal_limit = (int)(LLAM_ASYNC_SIGNAL_MASK_WORDS * 64U) + 1;
#else
    const int signal_limit = NSIG;
#endif
    for (int signal_number = 1; signal_number < signal_limit; ++signal_number) {
        if (sigismember(&mask, signal_number) != sigismember(&control->scheduler_mask, signal_number)) {
            llam_shared_uncertain();
        }
    }
    atomic_fetch_add_explicit(&control->scheduler_error_checks, 1U, memory_order_relaxed);
    llam_thread_errno_store(observed);
}

void llam_async_shared_test_hold(llam_runtime_t *runtime, unsigned point,
                                  atomic_uint *observed, atomic_uint *release) {
    llam_shared_control_t *control = llam_shared_control(runtime);
    if (control == NULL || point >= LLAM_ASYNC_SHARED_TEST_POINT_COUNT) return;
    atomic_store_explicit(&control->tail_release[point], release, memory_order_release);
    atomic_store_explicit(&control->tail_observed[point], observed, memory_order_release);
}

bool llam_async_shared_test_checkpoint(const llam_runtime_t *runtime,
                                        llam_async_shared_checkpoint_t *out) {
    llam_shared_control_t *control = llam_shared_control(runtime);
    if (control == NULL || out == NULL) return false;
    llam_shared_lock(control);
    *out = (llam_async_shared_checkpoint_t){
        .capture_ops = control->capture_op != NULL,
        .restore_ops = control->restore_op != NULL,
        .ready_depth = atomic_load_explicit(&control->ready_depth, memory_order_acquire),
        .probe_phase = atomic_load_explicit(&control->probe_phase, memory_order_acquire),
        .capture_phase = control->capture_op != NULL
            ? atomic_load_explicit(&control->capture_op->phase, memory_order_acquire) : 0U,
        .pending_phase = control->pending_gate != NULL
            ? atomic_load_explicit(&control->pending_gate->phase, memory_order_acquire) : 0U
    };
#if LLAM_ASYNC_CONTEXT_SIGNAL
    {
        llam_async_signal_binding_t *binding = control->native_binding;
        uint64_t stamp = atomic_load_explicit(&binding->stamp, memory_order_acquire);
        out->mailbox_state = (unsigned)(stamp & LLAM_ASYNC_SIGNAL_STATE_MASK);
        /* Diagnostic read is under the same gate used to arm/reuse below. */
        out->mailbox_kind = control->binding_live ? binding->request.kind : 0U;
        out->mailbox_cancellations = atomic_load_explicit(&binding->cancellations, memory_order_relaxed);
    }
#endif
    llam_shared_unlock(control);
    return true;
}

#if LLAM_ASYNC_CONTEXT_SIGNAL
void llam_async_shared_test_signal_hold(llam_runtime_t *runtime, unsigned kind,
                                       atomic_uint *observed, atomic_uint *release) {
    llam_shared_control_t *control = llam_shared_control(runtime);
    llam_async_signal_binding_t *binding;
    if (control == NULL || kind >= LLAM_ASYNC_SIGNAL_COMMAND_COUNT) abort();
    binding = control->native_binding;
    atomic_store_explicit(&binding->tail_release[kind], release, memory_order_relaxed);
    atomic_store_explicit(&binding->tail_observed[kind], observed, memory_order_release);
}
#endif

static void llam_shared_test_tail(llam_shared_control_t *control, unsigned point) {
    atomic_uint *observed = atomic_exchange_explicit(&control->tail_observed[point], NULL,
                                                     memory_order_acq_rel);
    atomic_uint *release = atomic_load_explicit(&control->tail_release[point], memory_order_acquire);
    if (observed == NULL || release == NULL) return;
    atomic_store_explicit(observed, 1U, memory_order_release);
    while (atomic_load_explicit(release, memory_order_acquire) == 0U) {
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
        (void)nanosleep(&delay, NULL); /* Target has resumed; no runtime lock held. */
    }
}
#else
#define llam_shared_test_tail(control, point) ((void)0)
#endif

static bool llam_shared_pressure_locked(llam_shared_control_t *control) {
    llam_runtime_t *runtime = control->runtime;
    if (runtime->preempt_mode == LLAM_PREEMPT_STRICT ||
        llam_managed_foreign_return_pending(runtime) ||
        atomic_load_explicit(&runtime->block_pending, memory_order_acquire) != 0U ||
        llam_runtime_overflow_depth(runtime) != 0U) return true;
    for (size_t i = 0U; i < control->group->control_count; ++i)
        if (atomic_load_explicit(&control->group[i].ready_depth, memory_order_acquire) != 0U)
            return true;
    /* A READY M without P must be able to displace a CPU G on a peer M.
     * Registered callers hold all P pairs, so this is one coherent pressure
     * observation rather than a stale grant on an arbitrary home processor. */
    unsigned first = control->registered ? 0U : control->worker->compat_shard->id;
    unsigned end = control->registered ? runtime->active_shards : first + 1U;
    for (unsigned i = first; i < end; ++i) {
        llam_shard_t *shard = &runtime->shards[i];
        if (shard->processor.hot_q.depth != 0U || llam_norm_queue_depth(shard) != 0U ||
            atomic_load_explicit(&shard->processor.inject_depth, memory_order_acquire) != 0U ||
            atomic_load_explicit(&shard->processor.timer_count, memory_order_acquire) != 0U)
            return true;
    }
    return false;
}

#if !LLAM_ASYNC_CONTEXT_SIGNAL
static void llam_shared_probe_binding(llam_shared_control_t *control) {
    int result, error;
    llam_shared_lock(control);
    if (!control->binding_live ||
        atomic_load_explicit(&control->probe_phase, memory_order_acquire) != 1U) {
        llam_shared_unlock(control);
        return;
    }
    atomic_fetch_add_explicit(&control->binding_ops, 1U, memory_order_relaxed);
    llam_shared_unlock(control);
    result = control->ops->suspend(control->native_binding);
    if (result == 0) {
        result = control->ops->read(control->native_binding, control->probe_context);
        error = errno;
        control->ops->resume(control->native_binding); /* Read-only qualification. */
    } else {
        error = errno;
    }
    llam_shared_lock(control);
    atomic_fetch_sub_explicit(&control->binding_ops, 1U, memory_order_release);
    if (result != 0) {
        atomic_store_explicit(&control->start_error, error != 0 ? error : ENOTSUP, memory_order_release);
        atomic_store_explicit(&control->start_phase, LLAM_SHARED_ABORTED, memory_order_release);
    }
    atomic_store_explicit(&control->probe_phase, result == 0 ? 2U : 3U, memory_order_release);
    llam_kick_shard(llam_shared_shard(control)); /* Final binding use before close can enter. */
    llam_shared_unlock(control);
}
#endif

enum llam_shared_action {
    LLAM_SHARED_NO_ACTION = 0,
    LLAM_SHARED_CAPTURE,
    LLAM_SHARED_RESTORE,
    LLAM_SHARED_WITNESS
};

uint64_t llam_async_shared_effective_quantum(const llam_runtime_t *runtime) {
    uint64_t requested = runtime != NULL ? runtime->preempt_quantum_ns : 0U;
    return requested < UINT64_C(1000000) ? UINT64_C(1000000) : requested;
}


#if LLAM_ASYNC_CONTEXT_SIGNAL
static void llam_shared_signal_complete(llam_shared_control_t *control,
                                        const llam_async_signal_result_t *result) {
    llam_shared_bank_t *bank = result->request.bank;
    unsigned kind = result->request.kind;
    if (result->request.control != control || bank == NULL ||
        !llam_shared_signal_identity(&result->request, control, bank)) llam_shared_uncertain();
    if (result->error == 0) {
#if defined(LLAM_ENABLE_TEST_HOOKS)
        unsigned point = kind == LLAM_ASYNC_SIGNAL_CAPTURE ? LLAM_ASYNC_SHARED_TEST_CAPTURE_RETURN :
                         kind == LLAM_ASYNC_SIGNAL_RESTORE ? LLAM_ASYNC_SHARED_TEST_RESTORE_RETURN :
                                                           LLAM_ASYNC_SHARED_TEST_WITNESS_RETURN;
        llam_shared_test_tail(control, point);
#endif
        return; /* Bank reader remains until READY handoff or exact OBSERVED. */
    }
    llam_shared_lock(control);
    if (kind == LLAM_ASYNC_SIGNAL_CAPTURE && control->capture_op == bank) {
        atomic_fetch_add_explicit(&llam_shared_shard(control)->metrics.async_preempt_deferred_unsafe,
                                  1U, memory_order_relaxed);
        /* Cancellation/refusal proved no context mutation. */
        llam_shared_finish_locked(control, &control->capture_op, true);
    } else if (kind == LLAM_ASYNC_SIGNAL_RESTORE && control->restore_op == bank &&
               result->error == EAGAIN) {
        llam_shared_finish_locked(control, &control->restore_op, false);
    } else if (kind != LLAM_ASYNC_SIGNAL_WITNESS || control->restore_op != bank ||
               result->error != EAGAIN) {
        /* Unsupported native state at an admitted GATE is terminal. Never
         * discard the saved G or retry an impossible restore indefinitely. */
#if defined(LLAM_ENABLE_TEST_HOOKS)
        const llam_async_signal_binding_t *binding = control->native_binding;
        fprintf(stderr, "signal refusal: kind=%u error=%d pc=%llx sp=%llx pstate=%llx record=%x:%u\n",
                kind, result->error, (unsigned long long)binding->refusal.pc,
                (unsigned long long)binding->refusal.sp,
                (unsigned long long)binding->refusal.pstate,
                binding->refusal.first_record[0], binding->refusal.first_record[1]);
#endif
        llam_shared_uncertain();
    }
    llam_shared_unlock(control);
}

static void llam_shared_signal_pump(llam_shared_control_t *control, uint64_t now_ns) {
    llam_async_signal_binding_t *binding = control->native_binding;
    llam_async_signal_result_t result;
    uint64_t stamp = atomic_load_explicit(&binding->stamp, memory_order_acquire);
    if ((stamp & LLAM_ASYNC_SIGNAL_STATE_MASK) == LLAM_ASYNC_SIGNAL_ARMED) {
        llam_async_signal_request_t request = binding->request;
        llam_shared_bank_t *bank = request.bank; /* Existing op still owns it. */
        bool observed = request.kind == LLAM_ASYNC_SIGNAL_WITNESS && bank != NULL &&
            atomic_load_explicit(&bank->phase, memory_order_acquire) == LLAM_ASYNC_BANK_OBSERVED;
        bool close_capture = !control->registered && request.kind == LLAM_ASYNC_SIGNAL_CAPTURE &&
            (atomic_load_explicit(&control->runtime->stop_requested, memory_order_acquire) ||
             atomic_load_explicit(&control->runtime->shutdown_requested, memory_order_acquire));
        if ((observed || close_capture || now_ns >= control->signal_deadline_ns) &&
            llam_async_signal_cancel(binding)) {
            result = (llam_async_signal_result_t){.request = request, .error = EAGAIN};
            llam_shared_signal_complete(control, &result);
            return;
        }
    }
    if (llam_async_signal_take_result(binding, &result)) llam_shared_signal_complete(control, &result);
}

static void llam_shared_signal_start(llam_shared_control_t *control, llam_shared_bank_t *bank,
                                     enum llam_shared_action action, uint64_t now_ns) {
    llam_async_signal_binding_t *binding = control->native_binding;
    llam_async_signal_request_t request = {
        .control = control, .bank = bank, .runtime_run = control->run_generation,
        .binding_incarnation = control->binding_incarnation, .bank_generation = bank->generation,
        .kind = action == LLAM_SHARED_CAPTURE ? LLAM_ASYNC_SIGNAL_CAPTURE :
                action == LLAM_SHARED_RESTORE ? LLAM_ASYNC_SIGNAL_RESTORE : LLAM_ASYNC_SIGNAL_WITNESS
    };
    llam_shared_lock(control);
    if (llam_async_signal_arm(binding, &request) != 0) llam_shared_uncertain();
    control->signal_deadline_ns = now_ns <= UINT64_MAX - UINT64_C(10000000)
        ? now_ns + UINT64_C(10000000) : UINT64_MAX;
    llam_shared_unlock(control);
    if (action == LLAM_SHARED_CAPTURE) {
        llam_shared_test_tail(control, LLAM_ASYNC_SHARED_TEST_SIGNAL_CAPTURE_ARMED);
    } else if (action == LLAM_SHARED_WITNESS) {
        llam_shared_test_tail(control, LLAM_ASYNC_SHARED_TEST_SIGNAL_WITNESS_ARMED);
    }
    /* The target may have returned before the doorbell is sent. Withdraw the
     * exact UNCLAIMED envelope before releasing its existing reader. A stale
     * queued doorbell then sees IDLE or a new fully-authorized sequence. */
    if (((action == LLAM_SHARED_WITNESS &&
          atomic_load_explicit(&bank->phase, memory_order_acquire) == LLAM_ASYNC_BANK_OBSERVED) ||
         (action == LLAM_SHARED_CAPTURE &&
          atomic_load_explicit(&bank->task->state, memory_order_acquire) == LLAM_TASK_STATE_DEAD)) &&
        llam_async_signal_cancel(binding)) {
        const llam_async_signal_result_t canceled = {.request = request, .error = EAGAIN};
        llam_shared_signal_complete(control, &canceled);
        return;
    }
    if (llam_async_signal_send(binding) != 0) llam_shared_uncertain();
}
#endif
static enum llam_shared_action llam_shared_choose_locked(
    llam_shared_control_t *control, uint64_t now_ns, llam_shared_bank_t **selected) {
    llam_shard_t *shard = llam_shared_shard(control);
    llam_worker_t *worker = control->worker;
    llam_shared_bank_t *bank;
    llam_shared_capsule_t *capsule;
    llam_task_t *task;
    llam_processor_owner_ticket_t ticket;
    uint64_t quantum = llam_async_shared_effective_quantum(control->runtime);
    uint64_t last;

#if LLAM_ASYNC_CONTEXT_SIGNAL
    /* A witness can outlive G return. Cancel the exact unclaimed request or
     * consume its claimed final result BEFORE retiring the underlying reader. */
    if (!llam_async_signal_idle(control->native_binding)) return LLAM_SHARED_NO_ACTION;
#endif

    /* Capture's physical op ends at the READY-root transfer, NOT at restore.
     * Otherwise a second no-yield G could strand the first READY G forever. */
    bank = control->capture_op;
    if (bank != NULL && atomic_load_explicit(&bank->arrival_complete, memory_order_acquire) == 1U) {
        unsigned phase = atomic_load_explicit(&bank->phase, memory_order_acquire);
        if (phase == LLAM_ASYNC_BANK_READY || phase == LLAM_ASYNC_BANK_GATE) {
            llam_shared_event(control, LLAM_ASYNC_SHARED_TEST_CAPTURE_END, bank);
            llam_shared_finish_locked(control, &control->capture_op, false);
#if defined(LLAM_ENABLE_TEST_HOOKS)
            if (atomic_load_explicit(
                    &control->tail_observed[LLAM_ASYNC_SHARED_TEST_READY_HANDOFF],
                    memory_order_acquire) != NULL) {
                control->handoff_pause = true;
                return LLAM_SHARED_NO_ACTION;
            }
#endif
        }
    }
    bank = control->restore_op;
    if (bank != NULL &&
        atomic_load_explicit(&bank->phase, memory_order_acquire) == LLAM_ASYNC_BANK_OBSERVED) {
        atomic_fetch_add_explicit(&control->witnesses, 1U, memory_order_relaxed);
        llam_shared_finish_locked(control, &control->restore_op, false);
    }
    if (!llam_shared_service_locked(control, &ticket)) return LLAM_SHARED_NO_ACTION;
    if (control->capture_op != NULL) return LLAM_SHARED_NO_ACTION;
    bank = control->pending_gate;
    if (bank != NULL && control->restore_op == NULL &&
        atomic_load_explicit(&bank->phase, memory_order_acquire) == LLAM_ASYNC_BANK_GATE) {
        if (!llam_shared_pin_bank_locked(control, bank)) return LLAM_SHARED_NO_ACTION;
        bank->ticket = ticket; /* Fresh service epoch; the old CPU payload is immutable. */
        bank->restore_scheduler_sp = llam_shared_scheduler_sp(control->scheduler);
        control->restore_op = bank;
        *selected = bank;
        return LLAM_SHARED_RESTORE;
    }
    bank = control->restore_op;
    if (bank != NULL) {
        if (atomic_load_explicit(&bank->phase, memory_order_acquire) == LLAM_ASYNC_BANK_RESTORING) {
            *selected = bank;
            return LLAM_SHARED_WITNESS;
        }
        return LLAM_SHARED_NO_ACTION;
    }
    /* Compatibility capture closes on stop. Registered drain retains capture
     * for accepted Gs using the separately validated STOPPING service tuple,
     * so one CPU G cannot strand a peer's READY or finalizer obligation. */
    if ((control->runtime->managed_catalog != NULL &&
         !llam_managed_catalog_async_enabled(control->runtime)) ||
        (!control->registered &&
         (atomic_load_explicit(&control->runtime->stop_requested, memory_order_acquire) ||
          atomic_load_explicit(&control->runtime->shutdown_requested, memory_order_acquire))) ||
        control->pending_gate != NULL || now_ns < control->next_probe_ns ||
        !llam_shared_pressure_locked(control)) return LLAM_SHARED_NO_ACTION;
    task = llam_worker_load_active_g(worker);
    if (task == NULL || !llam_worker_has_safe_managed_g(worker, &shard->processor, task) ||
        (task->managed_catalog != NULL && task->managed_execution_class == LLAM_MANAGED_EXECUTION_LEGACY) ||
        (task->flags & LLAM_TASK_FLAG_NO_PREEMPT) != 0U ||
        (capsule = task->async_preempt_task) == NULL ||
        !atomic_load_explicit(&capsule->started, memory_order_acquire) ||
        capsule->next_generation == UINT64_MAX) return LLAM_SHARED_NO_ACTION;
    last = atomic_load_explicit(&worker->last_safepoint_ns, memory_order_acquire);
    if (last == 0U || now_ns <= last || now_ns - last < quantum) return LLAM_SHARED_NO_ACTION;
    control->next_probe_ns = now_ns <= UINT64_MAX - quantum ? now_ns + quantum : UINT64_MAX;
    for (unsigned i = 0U; i < 2U; ++i) {
        bank = &capsule->banks[i];
        if (bank->readers == 0U &&
            atomic_load_explicit(&bank->phase, memory_order_acquire) == LLAM_ASYNC_BANK_FREE) {
            if (!llam_shared_pin_bank_locked(control, bank)) return LLAM_SHARED_NO_ACTION;
            bank->generation = ++capsule->next_generation;
            bank->capture_g_run = atomic_load_explicit(&task->run_generation, memory_order_acquire);
            bank->capture_m_run = atomic_load_explicit(&worker->run_generation, memory_order_acquire);
            bank->ticket = ticket;
#if defined(LLAM_ENABLE_TEST_HOOKS) || defined(LLAM_ENABLE_ASYNC_PROFILE)
            bank->requested_ns = now_ns;
#endif
            atomic_store_explicit(&bank->arrival_complete, 0U, memory_order_relaxed);
            atomic_store_explicit(&control->native_return_active, true, memory_order_release);
            atomic_store_explicit(&bank->phase, LLAM_ASYNC_BANK_CAPTURING, memory_order_release);
            control->capture_op = bank;
            control->arrival_ticket = bank;
            atomic_store_explicit(&control->urgent_service, true, memory_order_release);
            atomic_fetch_add_explicit(&control->requests, 1U, memory_order_relaxed);
            atomic_fetch_add_explicit(&shard->metrics.async_preempt_requests, 1U, memory_order_relaxed);
            *selected = bank;
            return LLAM_SHARED_CAPTURE;
        }
    }
    return LLAM_SHARED_NO_ACTION;
}

#if defined(LLAM_ENABLE_TEST_HOOKS)
int llam_async_shared_test_capture_admission(llam_task_t *task) {
    llam_shared_control_t *control = llam_shared_task_control(task);
    llam_shared_capsule_t *capsule = task != NULL ? task->async_preempt_task : NULL;
    llam_shared_bank_t *bank = NULL;
    unsigned expected = 0U;
    int result = -1;
    if (control == NULL || capsule == NULL || control->worker != g_llam_tls_worker ||
        control->runtime->preempt_mode != LLAM_PREEMPT_STRICT) { errno = ENOTSUP; return -1; }
    if (!atomic_compare_exchange_strong_explicit(&control->controller_busy, &expected, 1U,
                                                 memory_order_acquire, memory_order_relaxed)) {
        errno = EAGAIN;
        return -1;
    }
    llam_shared_lock(control);
    if (!llam_shared_service_locked(control, NULL) || !llam_shared_capsule_quiescent(capsule) ||
        control->capture_op != NULL || control->arrival_ticket != NULL || control->restore_op != NULL ||
        control->pending_gate != NULL || llam_worker_load_active_g(control->worker) != task ||
        capsule->next_generation == UINT64_MAX ||
        atomic_load_explicit(&control->worker->last_safepoint_ns, memory_order_acquire) == 0U) {
        errno = EINVAL;
    } else {
        uint64_t deadline = control->next_probe_ns;
        enum llam_shared_action action = llam_shared_choose_locked(control, UINT64_MAX, &bank);
        control->next_probe_ns = deadline;
        result = action == LLAM_SHARED_CAPTURE ? 1 : 0;
        if (action == LLAM_SHARED_CAPTURE) {
            /* Exercise actual publication admission, never transport or a
             * synthetic arrival/witness. Roll back its exact pre-mutation root. */
            llam_shared_finish_locked(control, &control->capture_op, true);
        } else if (action != LLAM_SHARED_NO_ACTION) llam_shared_uncertain();
    }
    llam_shared_unlock(control);
    atomic_store_explicit(&control->controller_busy, 0U, memory_order_release);
    return result;
}
#endif

#if !LLAM_ASYNC_CONTEXT_SIGNAL
static void llam_shared_native_transaction(
    llam_shared_control_t *control, llam_shared_bank_t *bank, enum llam_shared_action action) {
    llam_shard_t *shard = llam_shared_shard(control);
    void *snapshot = action == LLAM_SHARED_CAPTURE ? bank->native : control->probe_context;
    bool discard = action == LLAM_SHARED_CAPTURE;
    bool retry_restore = false;
    bool resume_ok = false;
    bool witness_ok = false;
    uintptr_t begin = 0U, end = 0U;
    uintptr_t witness_sp = 0U;

    if (control->ops->suspend(control->native_binding) != 0) {
        /* Known foreign suspension is a pre-commit refusal. Never consume its
         * increment. A vanished bound M cannot safely service admitted Gs. */
        if (errno != EBUSY) llam_shared_uncertain();
        retry_restore = action == LLAM_SHARED_RESTORE;
        goto finish;
    }
    int read_result = action == LLAM_SHARED_WITNESS
        ? control->ops->observe_sp(control->native_binding, &witness_sp)
        : control->ops->read(control->native_binding, snapshot);
    if (read_result != 0) {
        /* Observation never mutates the target. A restored G may already be
         * in native code or have returned to M; retain the reader until a
         * later witness or genuine return observes it. Restore admission still
         * requires a complete supported destination state before any write. */
        if (action == LLAM_SHARED_RESTORE) llam_shared_uncertain();
        goto resume;
    }
    if (!llam_shared_stopped_tuple(control, bank, action != LLAM_SHARED_CAPTURE)) {
        retry_restore = action == LLAM_SHARED_RESTORE;
        goto resume;
    }
    if (action == LLAM_SHARED_CAPTURE) {
        if (!llam_shared_on_stack(bank->task, control->ops->sp(snapshot)) ||
            !llam_shared_pc_allowed(control, bank->task, control->ops->pc(snapshot), &begin, &end)) {
            atomic_fetch_add_explicit(&shard->metrics.async_preempt_deferred_unsafe, 1U, memory_order_relaxed);
            goto resume;
        }
        bank->capture_scheduler_sp = llam_shared_scheduler_sp(control->scheduler);
        atomic_store_explicit(&bank->phase, LLAM_ASYNC_BANK_CAPTURED, memory_order_release);
        if (control->ops->redirect(control->native_binding, snapshot, control->scheduler,
                begin, end, (uintptr_t)bank->task->stack_base,
                (uintptr_t)bank->task->stack_base + bank->task->stack_size) != 0) {
            /* The native adapter returns only on a proved pre-mutation refusal. */
            atomic_store_explicit(&bank->phase, LLAM_ASYNC_BANK_CAPTURING, memory_order_release);
            goto resume;
        }
        discard = false;
        llam_shared_event(control, LLAM_ASYNC_SHARED_TEST_CAPTURE, bank);
        atomic_store_explicit(&bank->task->preempt_requested, 0U, memory_order_release);
        atomic_fetch_add_explicit(&control->captures, 1U, memory_order_relaxed);
        atomic_fetch_add_explicit(&shard->metrics.async_preempt_captures, 1U, memory_order_relaxed);
        atomic_fetch_add_explicit(&shard->metrics.async_preempt_context_bytes,
                                   control->ops->context_size, memory_order_relaxed);
    } else if (action == LLAM_SHARED_RESTORE) {
        if (!control->ops->at_gate(snapshot) ||
            bank->restore_scheduler_sp != llam_shared_scheduler_sp(control->scheduler)) {
            retry_restore = true;
            goto resume;
        }
        atomic_store_explicit(&bank->phase, LLAM_ASYNC_BANK_RESTORING, memory_order_release);
        if (control->ops->restore(control->native_binding, bank->native) != 0) llam_shared_uncertain();
        atomic_fetch_add_explicit(&control->restores, 1U, memory_order_relaxed);
        atomic_fetch_add_explicit(&shard->metrics.async_preempt_resumes, 1U, memory_order_relaxed);
        resume_ok = true;
        llam_shared_event(control, LLAM_ASYNC_SHARED_TEST_RESTORE, bank);
    } else {
        unsigned expected = LLAM_ASYNC_BANK_RESTORING;
        /* Read-only witness needs exact restored G/run/stack, not permission
         * to interrupt unknown code. It never mutates that native context. */
        if (llam_shared_on_stack(bank->task, witness_sp)) {
            witness_ok = atomic_compare_exchange_strong_explicit(&bank->phase, &expected,
                LLAM_ASYNC_BANK_OBSERVED, memory_order_acq_rel, memory_order_acquire);
        }
    }
resume:
    control->ops->resume(control->native_binding);
    if (action == LLAM_SHARED_CAPTURE && !discard) {
        llam_shared_test_tail(control, LLAM_ASYNC_SHARED_TEST_CAPTURE_RETURN);
    } else if (resume_ok) {
        llam_shared_test_tail(control, LLAM_ASYNC_SHARED_TEST_RESTORE_RETURN);
    } else if (witness_ok) {
        llam_shared_test_tail(control, LLAM_ASYNC_SHARED_TEST_WITNESS_RETURN);
    }
finish:
    llam_shared_lock(control);
    if (action == LLAM_SHARED_CAPTURE && discard) {
        llam_shared_finish_locked(control, &control->capture_op, true);
    } else if (action == LLAM_SHARED_RESTORE && retry_restore) {
        /* GATE stays rooted, not FREE. A later service attempt takes new refs. */
        llam_shared_finish_locked(control, &control->restore_op, false);
    }
    llam_shared_unlock(control);
}

#endif

static bool llam_shared_capture_not_due(const llam_shared_control_t *control, uint64_t now_ns) {
    if (atomic_load_explicit(&control->native_return_active, memory_order_acquire) ||
        atomic_load_explicit(&control->binding_ops, memory_order_acquire) != 0U) return false;
#if LLAM_ASYNC_CONTEXT_SIGNAL
    if (control->native_binding == NULL || !llam_async_signal_idle(control->native_binding)) return false;
#endif
    uint64_t last = atomic_load_explicit(&control->worker->last_safepoint_ns, memory_order_acquire);
    uint64_t quantum = llam_async_shared_effective_quantum(control->runtime);
    /* Only reject fresh cooperative progress before taking all P locks.
     * No G is dereferenced and no execution/native authority is created here.
     * Pending native roots bypass the hint; admission still rechecks the exact
     * tuple, clock and bank under the original service locks. */
    return last == 0U || now_ns <= last || now_ns - last < quantum;
}

static bool llam_shared_controller_tick(llam_shared_control_t *control, uint64_t now_ns) {
    llam_runtime_t *runtime = control->runtime;
    llam_shared_bank_t *bank = NULL;
    enum llam_shared_action action;
    unsigned expected = 0U;
    unsigned phase;
    bool pending;
    if (control == NULL || !atomic_load_explicit(&runtime->exec_started, memory_order_acquire) ||
        !atomic_compare_exchange_strong_explicit(&control->controller_busy, &expected, 1U,
                                                  memory_order_acquire, memory_order_relaxed)) return false;
    phase = atomic_load_explicit(&control->start_phase, memory_order_acquire);
    if (phase == LLAM_SHARED_PREPARING) {
#if !LLAM_ASYNC_CONTEXT_SIGNAL
        llam_shared_probe_binding(control);
#endif
    } else if (phase == LLAM_SHARED_OPEN && !llam_shared_capture_not_due(control, now_ns)) {
#if LLAM_ASYNC_CONTEXT_SIGNAL
        llam_shared_signal_pump(control, now_ns);
#endif
        llam_shared_lock(control);
        action = llam_shared_choose_locked(control, now_ns, &bank);
        llam_shared_unlock(control);
#if defined(LLAM_ENABLE_TEST_HOOKS)
        if (control->handoff_pause) {
            control->handoff_pause = false;
            llam_shared_test_tail(control, LLAM_ASYNC_SHARED_TEST_READY_HANDOFF);
        }
#endif
        if (action != LLAM_SHARED_NO_ACTION) {
#if LLAM_ASYNC_CONTEXT_SIGNAL
            llam_shared_signal_start(control, bank, action, now_ns);
#else
            llam_shared_native_transaction(control, bank, action);
#endif
        }
    }
    pending = llam_shared_control_has_debt(control) ||
              (atomic_load_explicit(&control->start_phase, memory_order_acquire) == LLAM_SHARED_PREPARING &&
               atomic_load_explicit(&control->probe_phase, memory_order_acquire) == 1U);
    atomic_store_explicit(&control->controller_busy, 0U, memory_order_release);
    return pending;
}

bool llam_async_shared_controller_tick(llam_runtime_t *runtime, uint64_t now_ns) {
    llam_shared_control_t *group = llam_shared_control(runtime);
    bool pending = false;
    if (group == NULL) return false;
    /* One bounded native transaction per M per round. A pending signal on one
     * binding never blocks request service for another registered M. */
    for (size_t i = 0U; i < group->control_count; ++i)
        pending = llam_shared_controller_tick(&group[i], now_ns) || pending;
    return pending;
}

bool llam_async_shared_controller_urgent(const llam_runtime_t *runtime) {
    const llam_shared_control_t *group = llam_shared_control(runtime);
    if (group == NULL) return false;
    for (size_t i = 0U; i < group->control_count; ++i)
        if (atomic_load_explicit(&group[i].urgent_service, memory_order_acquire)) return true;
    return false;
}

bool llam_async_shared_controller_wait(const llam_runtime_t *runtime, uint64_t interval_ns) {
    const llam_shared_control_t *group = llam_shared_control(runtime);
    if (group == NULL) return false;
#if LLAM_PLATFORM_DARWIN
    (void)dispatch_semaphore_wait(group->controller_wake,
        dispatch_time(DISPATCH_TIME_NOW, (int64_t)interval_ns));
#else
    struct pollfd pollfd = {.fd = group->controller_pipe[0], .events = POLLIN};
    const struct timespec delay = {.tv_sec = 0, .tv_nsec = (long)interval_ns};
    if (ppoll(&pollfd, 1U, &delay, NULL) > 0) {
        unsigned char bytes[64];
        for (unsigned i = 0U; i < 8U; ++i)
            if (read(group->controller_pipe[0], bytes, sizeof(bytes)) <= 0) break;
    }
#endif
    return true;
}

static int llam_shared_managed_begin(llam_runtime_t *runtime, void *data) {
    (void)data;
    return llam_async_shared_run_begin(runtime);
}

static int llam_shared_managed_start(llam_worker_t *worker, llam_ctx_t *scheduler, void *data) {
    (void)data;
    if (g_llam_tls_worker != worker || g_llam_tls_scheduler_ctx != scheduler) { errno = EINVAL; return -1; }
    return llam_async_shared_driver_ready(worker->registry_runtime);
}

static int llam_shared_managed_restart(llam_worker_t *worker, llam_ctx_t *scheduler, void *data) {
    llam_shared_control_t *control = llam_shared_worker_control(worker);
    unsigned expected;
    (void)data;
    if (control == NULL || !control->registered ||
        worker->role != LLAM_WORKER_ROLE_NATIVE_AFFINE ||
        g_llam_tls_worker != worker || g_llam_tls_scheduler_ctx != scheduler) {
        errno = EINVAL;
        return -1;
    }
    do {
        expected = 0U;
        if (atomic_compare_exchange_weak_explicit(&control->controller_busy, &expected, 1U,
                                                  memory_order_acquire, memory_order_relaxed)) break;
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
        (void)nanosleep(&delay, NULL);
    } while (true);
    llam_shared_lock(control);
    bool ready = !control->binding_live && !llam_shared_control_has_debt(control) &&
        control->capture_op == NULL && control->arrival_ticket == NULL &&
        control->restore_op == NULL && control->pending_gate == NULL &&
        control->terminal_task == NULL && control->ready_head == NULL &&
        atomic_load_explicit(&control->ready_depth, memory_order_acquire) == 0U &&
        atomic_load_explicit(&control->start_phase, memory_order_acquire) == LLAM_SHARED_OPEN;
    if (ready) {
        control->next_probe_ns = 0U;
        atomic_store_explicit(&control->start_error, 0, memory_order_relaxed);
        atomic_store_explicit(&control->probe_phase, 0U, memory_order_relaxed);
        atomic_store_explicit(&control->start_phase, LLAM_SHARED_PREPARING, memory_order_release);
    }
    llam_shared_unlock(control);
    atomic_store_explicit(&control->controller_busy, 0U, memory_order_release);
    if (!ready) { errno = EBUSY; return -1; }
    if (llam_shared_driver_ready(control->runtime, true) != 0) return -1;
    llam_shared_lock(control);
    uint64_t required = llam_managed_catalog_required_isa(control->runtime);
    ready = (required & ~control->available_isa) == 0U &&
        atomic_load_explicit(&control->probe_phase, memory_order_acquire) == 2U &&
        !atomic_load_explicit(&control->runtime->stop_requested, memory_order_acquire) &&
        !atomic_load_explicit(&control->runtime->shutdown_requested, memory_order_acquire);
    if (ready) atomic_store_explicit(&control->start_phase, LLAM_SHARED_OPEN, memory_order_release);
    llam_shared_unlock(control);
    if (!ready) { errno = ENOTSUP; return -1; }
    return 0;
}

static int llam_shared_managed_commit(llam_runtime_t *runtime, void *data) {
    (void)data;
    return llam_async_shared_run_commit(runtime);
}

static void llam_shared_managed_abort(llam_runtime_t *runtime, int error, void *data) {
    (void)data;
    llam_async_shared_run_abort(runtime, error);
}

static void llam_shared_managed_end(llam_runtime_t *runtime, void *data) {
    (void)data;
    llam_async_shared_run_end(runtime);
}

static void llam_shared_managed_service(llam_worker_t *worker, void *data) {
    (void)data;
    const llam_shared_control_t *control = worker->async_preempt_control;
    if (control != NULL &&
        atomic_load_explicit(&control->retire_depth, memory_order_acquire) == 0U &&
        atomic_load_explicit(&control->finalizer_depth, memory_order_acquire) == 0U) return;
    /* Counted roots remain authoritative; skip TLS routing only for the
     * same empty-queue hint used by the ordinary retirement entry point. */
    llam_async_shared_service_retirements(worker->compat_shard);
}

static bool llam_shared_managed_debt(llam_runtime_t *runtime, void *data) {
    (void)data;
    return llam_async_shared_has_debt(runtime);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline, cold))
#endif
static llam_task_t *llam_shared_managed_take(llam_worker_t *worker, llam_processor_t *processor,
                                            bool fallback, void *data) {
    (void)data;
    if (g_llam_tls_worker != worker) llam_shared_uncertain();
    return llam_async_preempt_take_ready(processor->compat_shard, fallback);
}

/* Keep the existing empty/ordinary guards leaf-sized. Inlining rare native
 * work here otherwise gives every cooperative yield a callee-save frame. The
 * checks and all publication/ownership operations retain their original order. */
static llam_task_t *llam_shared_managed_take_fast(llam_worker_t *worker,
                                                 llam_processor_t *processor,
                                                 bool fallback, void *data) {
    const llam_shared_control_t *control = worker->async_preempt_control;
    if (control != NULL && atomic_load_explicit(&control->ready_depth, memory_order_acquire) == 0U)
        return NULL; /* A later publication retains its root and wakeup. */
    return llam_shared_managed_take(worker, processor, fallback, data);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline, cold))
#endif
static bool llam_shared_managed_accept(llam_worker_t *worker, llam_processor_t *processor,
                                       llam_task_t *task, void *data) {
    llam_shared_capsule_t *capsule = task->async_preempt_task;
    llam_shared_control_t *origin;
    (void)processor;
    (void)data;
    llam_shared_control_t *control = llam_shared_worker_control(worker);
    if (control == NULL) llam_shared_uncertain();
    if (capsule == NULL) {
        if (task->managed_catalog != NULL && task->managed_execution_class == LLAM_MANAGED_EXECUTION_LEGACY)
            return true;
        llam_shared_uncertain();
    }
    llam_shared_lock(control);
    origin = atomic_load_explicit(&capsule->origin, memory_order_acquire);
    if (origin != NULL && origin != control) {
        if (llam_shared_portable_task(task) &&
            atomic_load_explicit(&task->managed_dispatch_owner, memory_order_acquire) == NULL &&
            llam_shared_capsule_quiescent(capsule)) {
            /* NULL is the old M's release after its complete cooperative save.
             * No bank/reader remains. All P pairs exclude native admission and
             * queue mutation while this lifetime root changes M; this is not
             * an execution grant, which the scheduler must reserve afterward. */
            if (atomic_fetch_add_explicit(&control->execution_debt, 1U, memory_order_relaxed) == UINT_MAX)
                llam_shared_uncertain();
            atomic_store_explicit(&capsule->origin, control, memory_order_release);
            if (atomic_fetch_sub_explicit(&origin->execution_debt, 1U, memory_order_release) == 0U)
                llam_shared_uncertain();
            llam_shared_unlock(control);
            return true;
        }
        if (!llam_shared_enqueue_locked(origin, task)) llam_shared_uncertain();
        llam_kick_shard(llam_shared_shard(origin));
        llam_shared_unlock(control);
        return false;
    }
    llam_shared_unlock(control);
    return true;
}

static bool llam_shared_managed_accept_fast(llam_worker_t *worker,
                                             llam_processor_t *processor,
                                             llam_task_t *task, void *data) {
    llam_shared_capsule_t *capsule = task->async_preempt_task;
    if (capsule != NULL && llam_shared_portable_task(task) &&
        atomic_load_explicit(&capsule->started, memory_order_acquire)) {
        const llam_shared_control_t *origin = atomic_load_explicit(&capsule->origin, memory_order_acquire);
        if (origin != NULL && origin->worker == worker) return true;
    }
    return llam_shared_managed_accept(worker, processor, task, data);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline, cold))
#endif
static void llam_shared_managed_returned(llam_worker_t *worker, llam_processor_t *processor,
                                         llam_task_t *task, void *data) {
    llam_shared_capsule_t *capsule = task->async_preempt_task;
    (void)data;
    llam_async_shared_scheduler_return(processor->compat_shard, task);
    if (atomic_load_explicit(&task->state, memory_order_acquire) == LLAM_TASK_STATE_DEAD) {
        if (task->managed_execution_class == LLAM_MANAGED_EXECUTION_NATIVE_AFFINE) {
            llam_shared_control_t *control = llam_shared_worker_control(worker);
            llam_shared_lock(control);
            if (capsule == NULL || control->terminal_task != NULL ||
                !llam_task_scan_ref_try_acquire(control->runtime, task)) llam_shared_uncertain();
            control->terminal_task = task;
            llam_shared_unlock(control);
            return; /* DEAD does not authorize reclaim before pthread TLS exit. */
        }
        llam_async_shared_retire_dead(processor->compat_shard, task);
        return;
    }
    /* Keep the portable G's existing native root on same-M redispatch. A
     * competing M transfers it only after the released context reservation
     * and all bank/reader roots prove a complete cooperative save. */
    if (llam_shared_portable_task(task)) return;
    llam_shared_control_t *control = llam_shared_worker_control(worker);
    llam_shared_lock(control);
    if (capsule != NULL && task->managed_execution_class != LLAM_MANAGED_EXECUTION_NATIVE_AFFINE &&
        llam_shared_capsule_quiescent(capsule)) {
        /* The real cooperative save is complete. Only after every old native
         * reader/bank drained may another M adopt this portable continuation. */
        atomic_store_explicit(&capsule->origin, NULL, memory_order_release);
        atomic_store_explicit(&capsule->started, false, memory_order_release);
        if (atomic_fetch_sub_explicit(&control->execution_debt, 1U, memory_order_release) == 0U)
            llam_shared_uncertain();
    }
    llam_shared_unlock(control);
}

static void llam_shared_managed_returned_fast(llam_worker_t *worker,
                                              llam_processor_t *processor,
                                              llam_task_t *task, void *data) {
    if (atomic_load_explicit(&task->state, memory_order_acquire) != LLAM_TASK_STATE_DEAD &&
        llam_shared_portable_task(task)) {
        const llam_shared_control_t *origin = worker->async_preempt_control;
        /* Native redirection publishes activity before M can return. Ordinary
         * cooperative saves retain their existing portable root unchanged. */
        if (origin != NULL &&
            !atomic_load_explicit(&origin->native_return_active, memory_order_acquire)) return;
    }
    llam_shared_managed_returned(worker, processor, task, data);
}

static bool llam_shared_managed_stop(llam_worker_t *worker, void *data) {
    (void)data;
    /* Another M may have failed startup while this binding's read-only probe
     * was already in flight. Abort closes new probes; the owning M remains on
     * its system stack until that last native operation releases the binding. */
    while (!llam_async_shared_driver_close(worker->registry_runtime)) {
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
        (void)nanosleep(&delay, NULL);
    }
    return true;
}

static bool llam_shared_managed_can_exit(llam_worker_t *worker, void *data) {
    llam_shared_control_t *control = llam_shared_worker_control(worker);
    bool ready;
    (void)data;
    if (control == NULL) return false;
    llam_shared_lock(control);
    ready = llam_shared_terminal_drained(control);
    llam_shared_unlock(control);
    return ready;
}

static void llam_shared_managed_joined(llam_worker_t *worker, void *data) {
    llam_shared_control_t *control = llam_shared_worker_control(worker);
    llam_task_t *task;
    bool claimed = false;
    (void)data;
    if (control == NULL) return;
    llam_shared_lock(control);
    task = control->terminal_task;
    if (task != NULL) {
        if (g_llam_tls_worker != NULL || control->binding_live ||
            atomic_load_explicit(&worker->native_thread_live, memory_order_acquire) ||
            !llam_shared_terminal_drained(control)) llam_shared_uncertain();
        control->terminal_task = NULL;
        llam_async_shared_task_destroy(task);
    }
    llam_shared_unlock(control);
    if (task == NULL) return;
    llam_task_release_stack(task);
    llam_task_mark_reclaim_ready(task);
    if (atomic_load_explicit(&task->detached, memory_order_acquire) != 0U) {
        unsigned expected = 0U;
        claimed = atomic_compare_exchange_strong_explicit(&task->reclaim_claimed, &expected, 1U,
                                                          memory_order_acq_rel, memory_order_acquire);
    }
    /* Publish readiness and, if needed, obtain an exclusive reclaim root while
     * the terminal scan pin still protects G. Never call the blocking host
     * reclaimer while holding our own scan pin: detached affine Gs would wait
     * forever for this same callback. A competing join may free G after release. */
    (void)llam_task_scan_ref_release(control->runtime, task);
    if (claimed) llam_reclaim_claimed_task(control->runtime, task);
    /* Only the exclusive claim permits a task access after scan-pin release. */
    if (atomic_fetch_sub_explicit(&control->execution_debt, 1U, memory_order_release) != 1U)
        llam_shared_uncertain();
}

static bool llam_shared_managed_needs_processor(llam_worker_t *worker, void *data) {
    llam_shared_control_t *control = llam_shared_worker_control(worker);
    (void)data;
    /* Advisory only: exact queue consumption and fresh lease validation remain
     * under service locks. A published GATE already retains its granted P;
     * a terminal affine G needs native join, never another user-execution P. */
    return control != NULL &&
        atomic_load_explicit(&control->ready_depth, memory_order_acquire) != 0U;
}

static const llam_managed_scheduler_ops_t g_llam_shared_managed_ops = {
    .native_async = true,
    .run_begin = llam_shared_managed_begin,
    .worker_start = llam_shared_managed_start,
    .worker_restart = llam_shared_managed_restart,
    .run_commit = llam_shared_managed_commit,
    .run_abort = llam_shared_managed_abort,
    .run_end = llam_shared_managed_end,
    .service = llam_shared_managed_service,
    .has_obligations = llam_shared_managed_debt,
    .take_ready = llam_shared_managed_take_fast,
    .accept_task = llam_shared_managed_accept_fast,
    .returned = llam_shared_managed_returned_fast,
    .worker_stop = llam_shared_managed_stop,
    .worker_can_exit = llam_shared_managed_can_exit,
    .worker_joined = llam_shared_managed_joined,
    .worker_needs_processor = llam_shared_managed_needs_processor
};

#endif
