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

/**
 * @file src/core/context/async_context_linux_x86_64.c
 * @brief Linux x86-64 native async capture and registered-worker adapter.
 *
 * The signal path only reads a published request, validates atomic worker/G
 * state and a frozen catalog or expert range table, copies the kernel context into memory
 * prepared before the signal, and redirects sigreturn to an assembly bridge.
 * Any uncertain state leaves the task in the ordinary cooperative path.
 */

#include "runtime_internal.h"
#include "capsule_pool.h"

#if !defined(__linux__) || !defined(__x86_64__)
#error "async_context_linux_x86_64.c must only be built for Linux x86-64"
#endif

#include "async_preempt_abi.h"
#include "async_signal_registry.h"
#include "metadata.h"
#include "metadata_v2.h"
#include "managed_catalog.h"
#include "managed_scheduler.h"
#include "managed_async_adapter.h"

#if defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)

#include <stddef.h>
#include <ucontext.h>

#if defined(SYS_arch_prctl)
#include <sys/syscall.h>
#endif

#define LLAM_ASYNC_PREEMPT_FXSAVE_BYTES 512U
#define LLAM_ASYNC_PREEMPT_XSTATE_HEADER_BYTES 576U
#define LLAM_ASYNC_PREEMPT_XSTATE_MAGIC1 UINT32_C(0x46505853)
#define LLAM_ASYNC_PREEMPT_XSTATE_MAGIC2 UINT32_C(0x46505845)
#define LLAM_ASYNC_PREEMPT_XSTATE_SW_OFFSET 464U
#define LLAM_ASYNC_PREEMPT_XSTATE_SW_MAGIC_OFFSET 0U
#define LLAM_ASYNC_PREEMPT_XSTATE_SW_EXTENDED_SIZE_OFFSET 4U
#define LLAM_ASYNC_PREEMPT_XSTATE_SW_FEATURES_OFFSET 8U
#define LLAM_ASYNC_PREEMPT_XSTATE_SW_SIZE_OFFSET 16U
#define LLAM_ASYNC_PREEMPT_XSTATE_BV_OFFSET 512U
#define LLAM_ASYNC_PREEMPT_XCOMP_BV_OFFSET 520U
#define LLAM_ASYNC_PREEMPT_XSTATE_PKRU_MASK (UINT64_C(1) << 9U)
#define LLAM_ASYNC_PREEMPT_XSTATE_MAX_BYTES (64U * 1024U * 1024U)
#define LLAM_ASYNC_PREEMPT_TRAMPOLINE_STACK_BYTES (64U * 1024U)
#define LLAM_ASYNC_PREEMPT_RETRY_NS (8U * LLAM_WATCHDOG_INTERVAL_NS)

#if defined(ARCH_SHSTK_STATUS)
#define LLAM_ASYNC_ARCH_SHSTK_STATUS ARCH_SHSTK_STATUS
#else
#define LLAM_ASYNC_ARCH_SHSTK_STATUS 0x5005
#endif
#if defined(ARCH_SHSTK_SHSTK)
#define LLAM_ASYNC_ARCH_SHSTK_ENABLED ARCH_SHSTK_SHSTK
#else
#define LLAM_ASYNC_ARCH_SHSTK_ENABLED 0x1UL
#endif

_Static_assert(ATOMIC_INT_LOCK_FREE == 2,
               "async preemption requires lock-free integer atomics");
_Static_assert(ATOMIC_LONG_LOCK_FREE == 2,
               "async preemption requires lock-free native-width atomics");
_Static_assert(ATOMIC_POINTER_LOCK_FREE == 2,
               "async preemption requires lock-free pointer atomics");
_Static_assert(sizeof(uintptr_t) == sizeof(uint64_t),
               "async preemption requires the x86-64 pointer width");

static pthread_mutex_t g_llam_async_preempt_lock = PTHREAD_MUTEX_INITIALIZER;
static llam_runtime_t *g_llam_async_preempt_runtime;
static struct sigaction g_llam_async_preempt_previous_action;
static atomic_int g_llam_async_preempt_signal = ATOMIC_VAR_INIT(0);
static bool g_llam_async_preempt_action_installed;

#if defined(LLAM_ENABLE_TEST_HOOKS)
static atomic_uint g_llam_async_test_allocation = ATOMIC_VAR_INIT(0U);
static atomic_uint g_llam_async_test_allocation_skip = ATOMIC_VAR_INIT(0U);
static atomic_uint g_llam_async_test_ready_stop = ATOMIC_VAR_INIT(0U);
static atomic_uint_fast64_t g_llam_async_test_worker_failure = ATOMIC_VAR_INIT(0U);
static atomic_uint g_llam_async_test_block_gate = ATOMIC_VAR_INIT(0U);
static atomic_bool g_llam_async_test_hold_normal = ATOMIC_VAR_INIT(false);
static atomic_uint g_llam_async_test_prepare_gate = ATOMIC_VAR_INIT(0U);

unsigned llam_async_preempt_test_prepare_gate(unsigned state) {
    if (state == UINT_MAX) {
        return atomic_load_explicit(&g_llam_async_test_prepare_gate, memory_order_acquire);
    }
    return atomic_exchange_explicit(&g_llam_async_test_prepare_gate, state, memory_order_acq_rel);
}

void llam_async_preempt_test_hold_normal_requests(bool hold) {
    atomic_store_explicit(&g_llam_async_test_hold_normal, hold, memory_order_release);
}

void llam_async_preempt_test_fail_allocation(unsigned point) {
    atomic_store_explicit(&g_llam_async_test_allocation_skip, 0U, memory_order_relaxed);
    atomic_store_explicit(&g_llam_async_test_allocation, point, memory_order_release);
}

void llam_async_preempt_test_fail_allocation_after(unsigned point, unsigned matches) {
    atomic_store_explicit(&g_llam_async_test_allocation_skip, matches, memory_order_relaxed);
    atomic_store_explicit(&g_llam_async_test_allocation, point, memory_order_release);
}

void llam_async_preempt_test_fail_worker(unsigned kind, unsigned shard_id) {
    atomic_store_explicit(&g_llam_async_test_worker_failure,
                          kind != 0U ? ((uint64_t)kind << 32U) | ((uint64_t)shard_id + 1U) : 0U,
                          memory_order_release);
}

bool llam_async_preempt_test_take_worker_failure(unsigned kind, unsigned shard_id) {
    uint_fast64_t expected = ((uint64_t)kind << 32U) | ((uint64_t)shard_id + 1U);

    return atomic_compare_exchange_strong_explicit(
        &g_llam_async_test_worker_failure, &expected, 0U,
        memory_order_acq_rel, memory_order_acquire);
}

unsigned llam_async_preempt_test_stop_before_ready(unsigned state) {
    return atomic_exchange_explicit(&g_llam_async_test_ready_stop, state,
                                     memory_order_acq_rel);
}

static bool llam_async_test_fail_allocation(unsigned point) {
    unsigned skip;

    if (atomic_load_explicit(&g_llam_async_test_allocation, memory_order_acquire) != point) {
        return false;
    }
    skip = atomic_load_explicit(&g_llam_async_test_allocation_skip, memory_order_acquire);
    while (skip != 0U) {
        if (atomic_compare_exchange_weak_explicit(
                &g_llam_async_test_allocation_skip, &skip, skip - 1U,
                memory_order_acq_rel, memory_order_acquire)) {
            return false;
        }
    }
    return atomic_compare_exchange_strong_explicit(
        &g_llam_async_test_allocation, &point, 0U,
        memory_order_acq_rel, memory_order_acquire);
}

bool llam_async_preempt_test_block_alloc_failure(llam_runtime_t *runtime) {
    return atomic_load_explicit(&runtime->async_preempt_enabled, memory_order_acquire) != 0U &&
           llam_async_test_fail_allocation(LLAM_ASYNC_TEST_FAIL_BLOCK_JOB);
}

unsigned llam_async_preempt_test_block_reserve_gate(unsigned state) {
    if (state == UINT_MAX) {
        return atomic_load_explicit(&g_llam_async_test_block_gate, memory_order_acquire);
    }
    return atomic_exchange_explicit(&g_llam_async_test_block_gate, state, memory_order_acq_rel);
}
#else
#define llam_async_test_fail_allocation(point) false
#endif

extern void llam_async_preempt_trampoline(void);
extern void llam_async_preempt_resume_bridge(void);
extern LLAM_NORETURN void llam_async_preempt_restore(void *frame);

enum llam_async_capsule_phase {
    LLAM_ASYNC_CAPSULE_FREE = LLAM_ASYNC_BANK_FREE,
    LLAM_ASYNC_CAPSULE_CAPTURING = LLAM_ASYNC_BANK_CAPTURING,
    LLAM_ASYNC_CAPSULE_CAPTURED = LLAM_ASYNC_BANK_CAPTURED,
    LLAM_ASYNC_CAPSULE_READY = LLAM_ASYNC_BANK_READY,
    LLAM_ASYNC_CAPSULE_RESTORING = LLAM_ASYNC_BANK_RESTORING,
    LLAM_ASYNC_CAPSULE_OBSERVED = LLAM_ASYNC_BANK_OBSERVED
};

typedef struct llam_async_capsule {
    atomic_uint phase;
    llam_task_t *task;
    llam_worker_t *origin_m;
    uint64_t origin_incarnation;
    uint64_t capture_sequence;
    uint64_t restore_sequence;
    uint64_t restore_g_run;
    uint64_t restore_m_run;
    /* Immutable payload until the capture generation is retired. */
    unsigned long uc_flags;
    stack_t signal_stack;
    mcontext_t machine;
    uint64_t signal_mask;
    void *xstate_area;
    size_t xstate_bytes;
    int saved_errno;
} llam_async_capsule_t;

typedef struct llam_async_task_state {
    llam_async_capsule_t bank[2];
    /* Scheduler-owned, and set only after leaving the G/landing stack. */
    llam_async_capsule_t *ready;
    /* Published at first dispatch, immutable until off-stack task retirement. */
    _Atomic(llam_worker_t *) origin_m;
    uint64_t origin_incarnation;
    uint64_t run_generation;
    bool enqueued; /* Origin shard lock, not the signal handler. */
    size_t budget_charge;
} llam_async_task_state_t;

enum llam_async_start_phase {
    LLAM_ASYNC_START_IDLE = 0,
    LLAM_ASYNC_START_COLLECTING,
    LLAM_ASYNC_START_RUNNING,
    LLAM_ASYNC_START_ABORTED
};

typedef struct llam_async_runtime_control {
    atomic_uint start_phase;
    atomic_uint prepared_workers;
    atomic_int start_error;
    uint64_t run_generation;
    unsigned worker_count;
    bool registered;
    atomic_uint ready_tasks; /* Registered M queues, observed as runtime pressure. */
    size_t capsule_limit;
    size_t capsule_charge;
    atomic_size_t capsule_bytes;
    llam_capsule_pool_t capsule_pool;
    unsigned block_limit;
    atomic_uint block_slots;
    llam_async_preempt_state_t *states[];
} llam_async_runtime_control_t;

static int llam_linux_capsule_prepare(void *object, void *argument) {
    llam_async_task_state_t *storage = object;
    const llam_async_runtime_control_t *control = argument;
    size_t bytes = control->states[0]->xstate_capacity + sizeof(uint32_t);
    for (unsigned i = 0U; i < 2U; ++i) {
        int error = posix_memalign(&storage->bank[i].xstate_area, 64U, bytes);
        if (error != 0) { errno = error; return -1; }
        memset(storage->bank[i].xstate_area, 0, bytes);
    }
    return 0;
}

static void llam_linux_capsule_destroy(void *object, void *argument) {
    llam_async_task_state_t *storage = object;
    (void)argument;
    for (unsigned i = 0U; i < 2U; ++i) free(storage->bank[i].xstate_area);
}

static int llam_async_read_limit(const char *name, uint64_t fallback,
                                 uint64_t maximum, uint64_t *result) {
    const char *value = llam_env_get(name);
    uint64_t parsed = 0U;
    size_t i;

    if (value == NULL) {
        *result = fallback;
        return 0;
    }
    for (i = 0U; value[i] != '\0'; ++i) {
        unsigned digit = (unsigned char)value[i] - (unsigned)'0';

        if (digit > 9U || parsed > maximum / 10U ||
            (parsed == maximum / 10U && digit > maximum % 10U)) {
            errno = EINVAL;
            return -1;
        }
        parsed = parsed * 10U + digit;
    }
    if (i == 0U || parsed == 0U) {
        errno = EINVAL;
        return -1;
    }
    *result = parsed;
    return 0;
}

int llam_async_preempt_block_reserve(llam_runtime_t *runtime, bool *reserved) {
    llam_async_runtime_control_t *control;
    unsigned used;
    int error = 0;

    *reserved = false;
    if (atomic_load_explicit(&runtime->async_preempt_enabled, memory_order_acquire) == 0U) {
        return 0;
    }
    control = runtime->async_preempt_control;
    if (control == NULL) {
        errno = ENOTSUP;
        return -1;
    }
    if (g_llam_tls_task != NULL && llam_task_cancel_token_is_cancelled(g_llam_tls_task)) {
        errno = ECANCELED;
        return -1;
    }
    pthread_mutex_lock(&runtime->block_lock);
    used = atomic_load_explicit(&control->block_slots, memory_order_relaxed);
    if (atomic_load_explicit(&runtime->stop_requested, memory_order_acquire) ||
        atomic_load_explicit(&runtime->shutdown_requested, memory_order_acquire)) {
        error = ECANCELED;
    } else if (used >= control->block_limit) {
        error = EAGAIN;
    } else {
        /* This ticket includes preparation, queued/running work and final cleanup. */
        atomic_store_explicit(&control->block_slots, used + 1U, memory_order_release);
        *reserved = true;
    }
    pthread_mutex_unlock(&runtime->block_lock);
    if (error != 0) {
        errno = error;
        return -1;
    }
#if defined(LLAM_ENABLE_TEST_HOOKS)
    {
        unsigned expected = 1U;

        if (atomic_compare_exchange_strong_explicit(&g_llam_async_test_block_gate, &expected, 2U,
                                                    memory_order_acq_rel, memory_order_acquire)) {
            while (atomic_load_explicit(&g_llam_async_test_block_gate, memory_order_acquire) == 2U) {
                const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
                (void)nanosleep(&delay, NULL);
            }
        }
    }
#endif
    return 0;
}

void llam_async_preempt_block_release(llam_runtime_t *runtime) {
    llam_async_runtime_control_t *control = runtime->async_preempt_control;
    unsigned used;
    int saved_errno = errno;

    if (control == NULL) {
        abort();
    }
    pthread_mutex_lock(&runtime->block_lock);
    used = atomic_load_explicit(&control->block_slots, memory_order_relaxed);
    if (used == 0U) {
        abort();
    }
    atomic_store_explicit(&control->block_slots, used - 1U, memory_order_release);
    pthread_mutex_unlock(&runtime->block_lock);
    errno = saved_errno;
}

#if defined(LLAM_ENABLE_TEST_HOOKS)
uint64_t llam_async_preempt_test_capsule_bytes(llam_runtime_t *runtime) {
    llam_async_runtime_control_t *control = runtime->async_preempt_control;
    return control != NULL ? atomic_load_explicit(&control->capsule_bytes, memory_order_acquire) : 0U;
}
uint64_t llam_async_preempt_test_capsule_charge(llam_runtime_t *runtime) {
    llam_async_runtime_control_t *control = runtime->async_preempt_control;
    return control != NULL ? control->capsule_charge : 0U;
}
unsigned llam_async_preempt_test_block_slots(llam_runtime_t *runtime) {
    llam_async_runtime_control_t *control = runtime->async_preempt_control;
    return control != NULL ? atomic_load_explicit(&control->block_slots, memory_order_acquire) : 0U;
}
#endif

typedef struct llam_async_restore_frame {
    uintptr_t restorer;
    ucontext_t context;
} llam_async_restore_frame_t;

typedef struct llam_async_worker_bridge {
    llam_ctx_t landing_context;
    llam_ctx_t resume_context;
    llam_async_restore_frame_t restore_frame;
    void *restore_xstate;
    pthread_mutex_t ready_lock;
    llam_ctx_t *scheduler;
    uint64_t available_isa;
    llam_task_t *terminal_task; /* Scan root retained through native TLS exit. */
} llam_async_worker_bridge_t;

static const llam_managed_scheduler_ops_t g_llam_linux_managed_ops;

static llam_async_preempt_state_t *llam_async_registered_state(
    const llam_runtime_t *runtime, const llam_worker_t *worker) {
    llam_async_runtime_control_t *control = runtime->async_preempt_control;
    if (control == NULL || !control->registered ||
        !llam_worker_registered_for(worker, runtime)) return NULL;
    for (unsigned i = 0U; i < control->worker_count; ++i)
        if (control->states[i]->worker == worker) return control->states[i];
    return NULL;
}


/* Linux x86-64 consumes the kernel ABI prefix, not glibc's trailing scratch. */
_Static_assert(offsetof(llam_async_restore_frame_t, context) == sizeof(uintptr_t),
               "Linux rt_sigreturn frame prefix mismatch");
_Static_assert(offsetof(ucontext_t, uc_mcontext) == 40U,
               "Linux x86-64 signal mcontext offset mismatch");
_Static_assert(offsetof(ucontext_t, uc_sigmask) == 296U,
               "Linux x86-64 kernel signal mask offset mismatch");
_Static_assert(sizeof(mcontext_t) == 256U,
               "Linux x86-64 signal machine-context size mismatch");
_Static_assert(SYS_rt_sigreturn == 15, "Linux x86-64 sigreturn syscall mismatch");

static llam_async_preempt_state_t *llam_async_worker_state(llam_worker_t *worker) {
    return worker != NULL ? (llam_async_preempt_state_t *)atomic_load_explicit(
                                &worker->async_preempt_state, memory_order_acquire)
                          : NULL;
}

bool llam_async_preempt_drain_service_needed(const llam_runtime_t *runtime) {
    llam_async_runtime_control_t *control;

    if (runtime == NULL ||
        atomic_load_explicit(&runtime->async_preempt_enabled, memory_order_acquire) == 0U ||
        !atomic_load_explicit(&runtime->exec_started, memory_order_acquire)) {
        return false;
    }
    control = runtime->async_preempt_control;
    return control != NULL &&
        atomic_load_explicit(&control->start_phase, memory_order_acquire) == LLAM_ASYNC_START_RUNNING &&
        (atomic_load_explicit(&runtime->stop_requested, memory_order_acquire) ||
         atomic_load_explicit(&runtime->shutdown_requested, memory_order_acquire));
}

bool llam_async_preempt_dynamic_allowed(const llam_runtime_t *runtime) {
    llam_async_runtime_control_t *control;
    if (runtime == NULL) return false;
    if (atomic_load_explicit(&runtime->async_preempt_enabled, memory_order_acquire) == 0U) return true;
    control = runtime->async_preempt_control;
    return control != NULL && !control->registered &&
        atomic_load_explicit(&runtime->exec_started, memory_order_acquire) &&
        atomic_load_explicit(&control->start_phase, memory_order_acquire) == LLAM_ASYNC_START_RUNNING &&
        !atomic_load_explicit(&runtime->stop_requested, memory_order_acquire) &&
        !atomic_load_explicit(&runtime->shutdown_requested, memory_order_acquire) &&
        runtime->experimental_shard_rings == 0U;
}

bool llam_async_preempt_idle_domain(const llam_shard_t *shard) {
    llam_async_preempt_state_t *state = shard != NULL
        ? llam_async_worker_state((llam_worker_t *)&shard->primary_worker) : NULL;
    unsigned status;
    if (state == NULL) return false;
    status = atomic_load_explicit(&state->status, memory_order_acquire);
    return !llam_async_preempt_has_origin_obligations(shard) &&
        (status == LLAM_ASYNC_PREEMPT_STATUS_IDLE || status == LLAM_ASYNC_PREEMPT_STATUS_REQUESTED);
}

/*
 * Private async service snapshot, with opaque_lock then shard->lock held.
 * It observes an existing primary OWNED or STOPPING drain projection; it never
 * grants a P or broadens the ordinary execute-ticket API. Admission close may
 * precede OWNED->STOPPING publication, without revoking this M's run service.
 */
static bool llam_async_service_snapshot_pair_locked(
    llam_shard_t *shard, llam_worker_t *worker, llam_processor_owner_ticket_t *ticket) {
    llam_runtime_t *runtime = shard != NULL ? shard->runtime : NULL;
    llam_async_runtime_control_t *control;
    llam_processor_owner_record_t projection;
    const llam_processor_owner_record_t *record;
    const llam_legacy_lease_t *lease;
    uint64_t incarnation;
    bool owned;
    bool stopping;

    if (runtime != NULL && runtime->execution_lease_mode == LLAM_EXECUTION_LEASE_REGISTERED) {
        llam_async_preempt_state_t *state = llam_async_registered_state(runtime, worker);
        control = runtime->async_preempt_control;
        if (ticket == NULL || state == NULL || state != llam_async_worker_state(worker) ||
            atomic_load_explicit(&runtime->async_preempt_enabled, memory_order_acquire) == 0U ||
            !atomic_load_explicit(&runtime->exec_started, memory_order_acquire) ||
            atomic_load_explicit(&control->start_phase, memory_order_acquire) != LLAM_ASYNC_START_RUNNING ||
            atomic_load_explicit(&state->frame_probe_status, memory_order_acquire) != 2U) return false;
        if (!atomic_load_explicit(&runtime->shutdown_requested, memory_order_acquire))
            return llam_processor_owner_ticket_snapshot_pair_locked(shard, ticket) &&
                   ticket->owner_m == worker;
        if (!llam_processor_owner_snapshot_pair_locked(&shard->processor, &projection)) return false;
        record = &projection;
        lease = llam_shard_legacy_lease_const_locked(shard);
        incarnation = atomic_load_explicit(&worker->binding_incarnation, memory_order_acquire);
        if (!llam_processor_owner_record_well_formed(record) ||
            record->state != LLAM_PROCESSOR_OWNER_STOPPING ||
            record->lease_state != LLAM_LEASE_STOPPING || lease->state != LLAM_LEASE_STOPPING ||
            record->owner_m != worker || lease->owner != worker ||
            record->worker_incarnation != incarnation || lease->owner_incarnation != incarnation ||
            record->lease_projection_epoch != lease->epoch ||
            record->p_generation == 0U || record->p_generation == UINT64_MAX ||
            !llam_worker_binding_live(worker, incarnation) ||
            !llam_worker_can_own_processor(worker, &shard->processor) ||
            atomic_load_explicit(&worker->current_p, memory_order_acquire) != &shard->processor)
            return false;
        *ticket = (llam_processor_owner_ticket_t){.processor = &shard->processor,
            .owner_m = worker, .worker_incarnation = incarnation,
            .lease_projection_epoch = lease->epoch, .p_generation = record->p_generation,
            .lease_state = LLAM_LEASE_STOPPING, .state = LLAM_PROCESSOR_OWNER_STOPPING};
        return true;
    }
    if (runtime == NULL || worker != &shard->primary_worker || ticket == NULL ||
        worker->role != LLAM_WORKER_ROLE_PRIMARY || worker->compat_shard != shard ||
        worker->associated_processor != &shard->processor ||
        !llam_processor_online(&shard->processor) ||
        atomic_load_explicit(&runtime->async_preempt_enabled, memory_order_acquire) == 0U ||
        !atomic_load_explicit(&runtime->exec_started, memory_order_acquire)) {
        return false;
    }
    control = runtime->async_preempt_control;
    if (control == NULL || atomic_load_explicit(&control->start_phase, memory_order_acquire) !=
                               LLAM_ASYNC_START_RUNNING ||
        shard->id >= control->worker_count || &runtime->shards[shard->id] != shard ||
        control->states[shard->id] != llam_async_worker_state(worker) ||
        atomic_load_explicit(&control->states[shard->id]->frame_probe_status, memory_order_acquire) != 2U) {
        return false;
    }
    if (!llam_processor_owner_snapshot_pair_locked(&shard->processor, &projection)) {
        return false;
    }
    record = &projection;
    lease = llam_shard_legacy_lease_const_locked(shard);
    incarnation = atomic_load_explicit(&worker->binding_incarnation, memory_order_acquire);
    owned = record->state == LLAM_PROCESSOR_OWNER_OWNED && lease->state == LLAM_LEASE_PRIMARY_OWNED;
    stopping = record->state == LLAM_PROCESSOR_OWNER_STOPPING && lease->state == LLAM_LEASE_STOPPING &&
               llam_async_preempt_drain_service_needed(runtime);
    if ((!owned && !stopping) || !llam_processor_owner_record_well_formed(record) ||
        record->owner_m != worker || lease->owner != worker ||
        record->worker_incarnation != incarnation || lease->owner_incarnation != incarnation ||
        record->lease_state != lease->state || record->lease_projection_epoch != lease->epoch ||
        record->p_generation == 0U || record->p_generation == UINT64_MAX ||
        !llam_worker_binding_live(worker, incarnation) ||
        atomic_load_explicit(&worker->current_p, memory_order_acquire) != &shard->processor ||
        atomic_load_explicit(&worker->async_preempt_ticket_lease_epoch, memory_order_acquire) != lease->epoch ||
        atomic_load_explicit(&worker->async_preempt_ticket_generation, memory_order_acquire) != record->p_generation) {
        return false;
    }
    ticket->processor = &shard->processor;
    ticket->owner_m = worker;
    ticket->worker_incarnation = incarnation;
    ticket->lease_projection_epoch = record->lease_projection_epoch;
    ticket->p_generation = record->p_generation;
    ticket->lease_state = record->lease_state;
    ticket->state = record->state;
    return true;
}

unsigned llam_async_preempt_ready_depth(const llam_shard_t *shard) {
    if (shard != NULL && shard->runtime->execution_lease_mode == LLAM_EXECUTION_LEASE_REGISTERED) {
        llam_async_runtime_control_t *control = shard->runtime->async_preempt_control;
        /* Ready work belongs to M rather than a fixed P. Every borrowed P must
         * see this pressure so AUTO does not strand another M's continuation. */
        unsigned ready = control != NULL ? atomic_load_explicit(&control->ready_tasks, memory_order_acquire) : 0U;
        /* A completed inline callback waits on its original native stack rather
         * than a ready queue. That real return obligation also needs a P turn. */
        return ready != 0U ? ready : (llam_managed_foreign_return_pending(shard->runtime) ? 1U : 0U);
    }
    llam_async_preempt_state_t *state = shard != NULL
        ? llam_async_worker_state((llam_worker_t *)&shard->primary_worker) : NULL;

    return state != NULL ? atomic_load_explicit(&state->ready_depth, memory_order_acquire) : 0U;
}

static bool llam_async_runtime_has_debt(const llam_async_runtime_control_t *control) {
    if (control == NULL) return false;
    for (unsigned i = 0U; i < control->worker_count; ++i) {
        const llam_async_preempt_state_t *state = control->states[i];
        if (atomic_load_explicit(&state->affine_live_g, memory_order_acquire) != 0U ||
            atomic_load_explicit(&state->ready_depth, memory_order_acquire) != 0U ||
            atomic_load_explicit(&state->pending_restore, memory_order_acquire) != 0U) return true;
    }
    return false;
}

bool llam_async_preempt_has_origin_obligations(const llam_shard_t *shard) {
    if (shard != NULL && shard->runtime->execution_lease_mode == LLAM_EXECUTION_LEASE_REGISTERED)
        return llam_async_runtime_has_debt(shard->runtime->async_preempt_control);
    llam_async_preempt_state_t *state = shard != NULL
        ? llam_async_worker_state((llam_worker_t *)&shard->primary_worker) : NULL;

    return state != NULL &&
        (atomic_load_explicit(&state->affine_live_g, memory_order_acquire) != 0U ||
         atomic_load_explicit(&state->ready_depth, memory_order_acquire) != 0U ||
         atomic_load_explicit(&state->pending_restore, memory_order_acquire) != 0U);
}

unsigned llam_async_preempt_task_origin_shard(const llam_task_t *task) {
    llam_async_task_state_t *storage = task != NULL ? task->async_preempt_task : NULL;
    llam_worker_t *origin = storage != NULL
        ? atomic_load_explicit(&storage->origin_m, memory_order_acquire) : NULL;

    return origin != NULL && task->owner_runtime->execution_lease_mode != LLAM_EXECUTION_LEASE_REGISTERED
        ? origin->compat_shard->id : UINT_MAX;
}

void llam_async_preempt_bind_task(llam_worker_t *worker, llam_task_t *task) {
    llam_runtime_t *runtime = task->owner_runtime;
    llam_async_runtime_control_t *control;
    llam_async_preempt_state_t *state;
    llam_async_task_state_t *storage;
    llam_worker_t *origin;
    llam_shard_t *shard;
    llam_processor_owner_ticket_t service;
    uint64_t incarnation;

    if (atomic_load_explicit(&runtime->async_preempt_enabled, memory_order_acquire) == 0U) {
        return;
    }
    if (task->managed_catalog != NULL && task->managed_execution_class == LLAM_MANAGED_EXECUTION_LEGACY)
        return;
    control = runtime->async_preempt_control;
    state = llam_async_worker_state(worker);
    storage = task->async_preempt_task;
    shard = worker != NULL ? (control != NULL && control->registered
        ? g_llam_tls_shard : worker->compat_shard) : NULL;
    if (control == NULL || storage == NULL || state == NULL || shard == NULL ||
        (!control->registered && worker != &shard->primary_worker) || shard->runtime != runtime ||
        atomic_load_explicit(&control->start_phase, memory_order_acquire) != LLAM_ASYNC_START_RUNNING) {
        abort();
    }
    /* First binding and service pin are serialized with P pause/offline reservations. */
    pthread_mutex_lock(&shard->opaque_lock);
    pthread_mutex_lock(&shard->lock);
    incarnation = atomic_load_explicit(&worker->binding_incarnation, memory_order_acquire);
    if (!llam_async_service_snapshot_pair_locked(shard, worker, &service) ||
        storage->enqueued) {
        abort();
    }
    origin = atomic_load_explicit(&storage->origin_m, memory_order_acquire);
    if (origin == NULL) {
        unsigned live = atomic_load_explicit(&state->affine_live_g, memory_order_relaxed);

        if (live == UINT_MAX) {
            abort();
        }
        storage->origin_incarnation = incarnation;
        storage->run_generation = control->run_generation;
        atomic_store_explicit(&state->affine_live_g, live + 1U, memory_order_release);
        /* Release makes the immutable identity visible to remote wake producers. */
        atomic_store_explicit(&storage->origin_m, worker, memory_order_release);
    } else if (origin != worker || storage->origin_incarnation != incarnation ||
               storage->run_generation != control->run_generation) {
        /* A wrong-M dequeue is an invariant violation, never cooperative fallback. */
        abort();
    }
    pthread_mutex_unlock(&shard->lock);
    pthread_mutex_unlock(&shard->opaque_lock);
}

/* Registered M ready queues are native-owned, independent of the borrowed P.
 * A producer holds only this queue lock; no M lock is nested under another M. */
static bool llam_async_registered_enqueue(llam_async_preempt_state_t *state,
                                           llam_task_t *task) {
    llam_async_task_state_t *storage = task->async_preempt_task;
    llam_async_worker_bridge_t *bridge = state->bridge_storage;
    pthread_mutex_lock(&bridge->ready_lock);
    unsigned depth = atomic_load_explicit(&state->ready_depth, memory_order_relaxed);
    if (storage == NULL || storage->enqueued || depth == UINT_MAX ||
        atomic_load_explicit(&storage->origin_m, memory_order_acquire) != state->worker ||
        !llam_worker_binding_live(state->worker, storage->origin_incarnation)) abort();
    task->queue_next = NULL;
    task->queue_prev = state->ready_tail;
    if (state->ready_tail != NULL) state->ready_tail->queue_next = task;
    else state->ready_head = task;
    state->ready_tail = task;
    storage->enqueued = true;
    llam_async_runtime_control_t *control = task->owner_runtime->async_preempt_control;
    if (atomic_fetch_add_explicit(&control->ready_tasks, 1U, memory_order_release) == UINT_MAX) abort();
    atomic_store_explicit(&state->ready_depth, depth + 1U, memory_order_release);
    pthread_mutex_unlock(&bridge->ready_lock);
    llam_kick_shard(state->worker->compat_shard);
    return true;
}

/* The origin shard lock serializes all producers and the single M consumer. */
bool llam_async_preempt_enqueue_locked(llam_shard_t *shard, llam_task_t *task) {
    llam_async_task_state_t *storage = task->async_preempt_task;
    llam_worker_t *origin = storage != NULL
        ? atomic_load_explicit(&storage->origin_m, memory_order_acquire) : NULL;
    llam_async_preempt_state_t *state;
    unsigned depth;

    if (origin == NULL || task->owner_runtime->execution_lease_mode == LLAM_EXECUTION_LEASE_REGISTERED) {
        /* Registered producers publish to P first. Dequeue routes an off-stack
         * continuation to its original M after managed_dispatch_owner clears. */
        return false;
    }
    state = llam_async_worker_state(origin);
    if (origin != &shard->primary_worker || state == NULL || storage->enqueued ||
        task->owner_runtime != shard->runtime ||
        !llam_worker_binding_live(origin, storage->origin_incarnation)) {
        abort();
    }
    depth = atomic_load_explicit(&state->ready_depth, memory_order_relaxed);
    if (depth == UINT_MAX) {
        abort();
    }
    task->queue_next = NULL;
    task->queue_prev = state->ready_tail;
    if (state->ready_tail != NULL) {
        state->ready_tail->queue_next = task;
    } else {
        state->ready_head = task;
    }
    state->ready_tail = task;
    storage->enqueued = true;
    /* An admitted G owns its intrusive node; this queue never allocates or overflows. */
    atomic_store_explicit(&state->ready_depth, depth + 1U, memory_order_release);
    return true;
}

llam_task_t *llam_async_preempt_take_ready(llam_shard_t *shard, bool fallback) {
    llam_async_preempt_state_t *state = llam_async_worker_state(g_llam_tls_worker);
    llam_async_task_state_t *storage;
    llam_task_t *task;
    llam_processor_owner_ticket_t service;
    unsigned depth;

    if (state == NULL || g_llam_tls_worker != &shard->primary_worker) {
        return NULL;
    }
    if (atomic_load_explicit(&state->ready_depth, memory_order_acquire) == 0U) {
        return NULL;
    }
    /* Alternate origin service with NEW work; still drain timers/injection every turn. */
    if (!fallback && !state->prefer_origin) {
        state->prefer_origin = true;
        return NULL;
    }
    pthread_mutex_lock(&shard->opaque_lock);
    pthread_mutex_lock(&shard->lock);
    if (!llam_async_service_snapshot_pair_locked(shard, g_llam_tls_worker, &service)) {
        pthread_mutex_unlock(&shard->lock);
        pthread_mutex_unlock(&shard->opaque_lock);
        return NULL;
    }
    task = state->ready_head;
    if (task == NULL) {
        pthread_mutex_unlock(&shard->lock);
        pthread_mutex_unlock(&shard->opaque_lock);
        return NULL;
    }
    storage = task->async_preempt_task;
    depth = atomic_load_explicit(&state->ready_depth, memory_order_relaxed);
    if (storage == NULL || !storage->enqueued || depth == 0U ||
        atomic_load_explicit(&storage->origin_m, memory_order_acquire) != g_llam_tls_worker ||
        !llam_worker_binding_live(g_llam_tls_worker, storage->origin_incarnation)) {
        abort();
    }
    state->ready_head = task->queue_next;
    if (state->ready_head != NULL) {
        state->ready_head->queue_prev = NULL;
    } else {
        state->ready_tail = NULL;
    }
    task->queue_next = NULL;
    task->queue_prev = NULL;
    storage->enqueued = false;
    atomic_store_explicit(&state->ready_depth, depth - 1U, memory_order_release);
    state->prefer_origin = false;
    pthread_mutex_unlock(&shard->lock);
    pthread_mutex_unlock(&shard->opaque_lock);
    return task;
}

static bool llam_async_on_task_stack(const llam_task_t *task, uintptr_t sp) {
    uintptr_t bottom;

    if (task == NULL || task->stack_base == NULL || task->stack_size == 0U) {
        return false;
    }
    bottom = (uintptr_t)task->stack_base;
    return sp >= bottom && sp - bottom < task->stack_size;
}

static uint32_t llam_async_load_u32(const unsigned char *source) {
    uint32_t value = 0U;
    unsigned i;

    for (i = 0U; i < sizeof(value); ++i) {
        value |= (uint32_t)source[i] << (i * 8U);
    }
    return value;
}

static uint64_t llam_async_load_u64(const unsigned char *source) {
    uint64_t value = 0U;
    unsigned i;

    for (i = 0U; i < sizeof(value); ++i) {
        value |= (uint64_t)source[i] << (i * 8U);
    }
    return value;
}

static void llam_async_copy_bytes(unsigned char *destination,
                                  const unsigned char *source,
                                  size_t count) {
    size_t i;

    for (i = 0U; i < count; ++i) {
        destination[i] = source[i];
    }
}

static void llam_async_record_max_ack_latency(llam_metrics_t *metrics,
                                              uint64_t latency_ns) {
    uint_fast64_t observed;

    if (metrics == NULL) {
        return;
    }
    observed = atomic_load_explicit(&metrics->async_preempt_max_ack_latency_ns,
                                    memory_order_relaxed);
    while (latency_ns > observed &&
           !atomic_compare_exchange_weak_explicit(
               &metrics->async_preempt_max_ack_latency_ns,
               &observed,
               latency_ns,
               memory_order_relaxed,
               memory_order_relaxed)) {
    }
}

void llam_async_preempt_run_abort(llam_runtime_t *runtime, int error,
                                  llam_preempt_failure_reason_t reason) {
    llam_async_runtime_control_t *control;
    int expected = 0;
    unsigned i;

    if (runtime == NULL || (control = runtime->async_preempt_control) == NULL) {
        return;
    }
    if (error == 0) {
        error = ENOTSUP;
    }
    if (atomic_compare_exchange_strong_explicit(&control->start_error, &expected, error,
                                                memory_order_acq_rel, memory_order_acquire)) {
        llam_preempt_capabilities_note_async_result(runtime, false, reason);
    }
    /* Keep the routing guard enabled: an aborted start must not become fallback dispatch. */
    atomic_store_explicit(&control->start_phase, LLAM_ASYNC_START_ABORTED,
                          memory_order_release);
    for (i = 0U; i < control->worker_count; ++i) {
        llam_kick_shard(control->states[i]->worker->compat_shard);
    }
}

int llam_async_preempt_run_begin(llam_runtime_t *runtime) {
    llam_async_runtime_control_t *control;
    unsigned i;

    if (atomic_load_explicit(&runtime->async_preempt_enabled, memory_order_acquire) == 0U) {
        return 0;
    }
    control = runtime->async_preempt_control;
    if (control == NULL || control->run_generation == UINT64_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    if (atomic_load_explicit(&control->ready_tasks, memory_order_acquire) != 0U) {
        errno = EBUSY;
        return -1;
    }
    for (i = 0U; i < control->worker_count; ++i) {
        llam_async_preempt_state_t *state = control->states[i];

        if (atomic_load_explicit(&state->affine_live_g, memory_order_acquire) != 0U ||
            atomic_load_explicit(&state->ready_depth, memory_order_acquire) != 0U ||
            atomic_load_explicit(&state->pending_restore, memory_order_acquire) != 0U ||
            state->captured_capsule != NULL) {
            errno = EBUSY;
            return -1;
        }
        state->prefer_origin = false;
    }
    ++control->run_generation;
    atomic_store_explicit(&control->prepared_workers, 0U, memory_order_relaxed);
    atomic_store_explicit(&control->start_error, 0, memory_order_relaxed);
    /* Release publishes the new run before any secondary thread can qualify. */
    atomic_store_explicit(&control->start_phase, LLAM_ASYNC_START_COLLECTING,
                          memory_order_release);
    atomic_store_explicit(&runtime->preempt_capability.async_state,
                          LLAM_PREEMPT_ASYNC_PREPARED, memory_order_release);
    return 0;
}

void llam_async_preempt_run_end(llam_runtime_t *runtime) {
    llam_async_runtime_control_t *control = runtime->async_preempt_control;
    unsigned i;

    if (control == NULL) {
        return;
    }
    if (atomic_load_explicit(&control->ready_tasks, memory_order_acquire) != 0U) abort();
    /* Called after all scheduler threads have exited, before releasing the run gate. */
    for (i = 0U; i < control->worker_count; ++i) {
        llam_async_preempt_state_t *state = control->states[i];

        if (atomic_load_explicit(&state->affine_live_g, memory_order_acquire) != 0U ||
            atomic_load_explicit(&state->ready_depth, memory_order_acquire) != 0U ||
            atomic_load_explicit(&state->pending_restore, memory_order_acquire) != 0U ||
            state->captured_capsule != NULL) {
            abort();
        }
    }
    atomic_store_explicit(&control->start_phase, LLAM_ASYNC_START_IDLE, memory_order_release);
    if (atomic_load_explicit(&control->start_error, memory_order_acquire) == 0) {
        atomic_store_explicit(&runtime->preempt_capability.async_state,
                              LLAM_PREEMPT_ASYNC_PREPARED, memory_order_release);
    }
}

static bool llam_async_pc_is_registered(const llam_runtime_t *runtime,
                                        uintptr_t pc) {
    size_t i;

    if (runtime->managed_catalog != NULL)
        return llam_managed_catalog_find_safe(runtime->managed_catalog, pc) != NULL;
    for (i = 0U; i < runtime->async_preempt_range_count; ++i) {
        const llam_async_preempt_range_t *range = &runtime->async_preempt_ranges[i];

        if (pc >= range->begin && pc < range->end) {
            return true;
        }
    }
    return false;
}

/*
 * A software epoch alone is not a restore ACK. This observes the interrupted
 * user context, before the usual request claim. A stale doorbell may observe
 * reentry, but does not grant a new capture request. Manual safe-range authors
 * must exclude host/foreign callbacks as well as native-affine call scopes.
 */
static bool llam_async_observe_restore(llam_async_preempt_state_t *state,
                                       const ucontext_t *context) {
    llam_async_capsule_t *capsule = (llam_async_capsule_t *)atomic_load_explicit(
        &state->pending_restore, memory_order_acquire);
    llam_worker_t *worker = state->worker;

    if (capsule == NULL ||
        atomic_load_explicit(&state->observed_restore_sequence, memory_order_acquire) ==
            capsule->restore_sequence) {
        return true;
    }
    if (worker == NULL || capsule->task != g_llam_tls_task ||
        atomic_load_explicit(&worker->native_domain_depth, memory_order_acquire) != 0U ||
        capsule->origin_m != worker ||
        atomic_load_explicit(&worker->binding_incarnation, memory_order_acquire) !=
            capsule->origin_incarnation ||
        atomic_load_explicit(&worker->run_generation, memory_order_acquire) !=
            capsule->restore_m_run ||
        atomic_load_explicit(&capsule->task->run_generation, memory_order_acquire) !=
            capsule->restore_g_run ||
        llam_worker_load_active_g(worker) != capsule->task ||
        atomic_load_explicit(&worker->execution_state, memory_order_acquire) !=
            LLAM_WORKER_EXEC_MANAGED_SAFE ||
        !llam_async_on_task_stack(capsule->task,
                                  (uintptr_t)context->uc_mcontext.gregs[REG_RSP]) ||
        !llam_async_pc_is_registered(capsule->task->owner_runtime,
                                     (uintptr_t)context->uc_mcontext.gregs[REG_RIP])) {
        return false;
    }
    /* Only observation is published here. The scheduler retires these bytes. */
    atomic_store_explicit(&capsule->phase, LLAM_ASYNC_CAPSULE_OBSERVED,
                          memory_order_release);
    atomic_store_explicit(&state->observed_restore_sequence, capsule->restore_sequence,
                          memory_order_release);
    return true;
}

static llam_async_capsule_t *llam_async_claim_capsule(llam_task_t *task) {
    llam_async_task_state_t *storage = task != NULL ? task->async_preempt_task : NULL;
    unsigned i;

    if (storage == NULL || g_llam_tls_worker == NULL ||
        atomic_load_explicit(&storage->origin_m, memory_order_acquire) != g_llam_tls_worker ||
        storage->origin_incarnation != atomic_load_explicit(
            &g_llam_tls_worker->binding_incarnation, memory_order_acquire)) {
        return NULL;
    }
    for (i = 0U; i < 2U; ++i) {
        unsigned expected = LLAM_ASYNC_CAPSULE_FREE;

        if (atomic_compare_exchange_strong_explicit(
                &storage->bank[i].phase, &expected, LLAM_ASYNC_CAPSULE_CAPTURING,
                memory_order_acq_rel, memory_order_acquire)) {
            return &storage->bank[i];
        }
    }
    return NULL;
}

static int llam_async_range_compare(const void *left, const void *right) {
    const llam_async_preempt_range_t *left_range = left;
    const llam_async_preempt_range_t *right_range = right;

    if (left_range->begin < right_range->begin) {
        return -1;
    }
    if (left_range->begin > right_range->begin) {
        return 1;
    }
    if (left_range->end < right_range->end) {
        return -1;
    }
    if (left_range->end > right_range->end) {
        return 1;
    }
    return 0;
}

/* Caller holds g_llam_async_preempt_lock; no failure occurs after realloc. */
static int llam_async_append_ranges_locked(
    llam_runtime_t *runtime,
    const llam_async_preempt_range_t *ranges,
    size_t range_count) {
    size_t old_count;
    size_t new_count;
    size_t index;
    size_t other;

    if (runtime == NULL || (range_count != 0U && ranges == NULL)) {
        errno = EINVAL;
        return -1;
    }
    old_count = runtime->async_preempt_range_count;
    if (range_count > SIZE_MAX - old_count) {
        errno = EOVERFLOW;
        return -1;
    }
    new_count = old_count + range_count;
    for (index = 0U; index < range_count; ++index) {
        if (ranges[index].begin == 0U || ranges[index].end <= ranges[index].begin) {
            errno = EINVAL;
            return -1;
        }
        for (other = 0U; other < old_count; ++other) {
            const llam_async_preempt_range_t *existing =
                &runtime->async_preempt_ranges[other];

            if (ranges[index].begin < existing->end &&
                ranges[index].end > existing->begin) {
                errno = EEXIST;
                return -1;
            }
        }
        for (other = 0U; other < index; ++other) {
            if (ranges[index].begin < ranges[other].end &&
                ranges[index].end > ranges[other].begin) {
                errno = EEXIST;
                return -1;
            }
        }
    }
    if (new_count > runtime->async_preempt_range_capacity) {
        size_t new_capacity = runtime->async_preempt_range_capacity == 0U
                                  ? 4U
                                  : runtime->async_preempt_range_capacity;
        llam_async_preempt_range_t *new_ranges;

        while (new_capacity < new_count) {
            if (new_capacity > SIZE_MAX / 2U) {
                new_capacity = new_count;
                break;
            }
            new_capacity *= 2U;
        }
        if (new_capacity > SIZE_MAX / sizeof(*new_ranges)) {
            errno = EOVERFLOW;
            return -1;
        }
        new_ranges = realloc(runtime->async_preempt_ranges,
                             new_capacity * sizeof(*new_ranges));
        if (new_ranges == NULL) {
            errno = ENOMEM;
            return -1;
        }
        runtime->async_preempt_ranges = new_ranges;
        runtime->async_preempt_range_capacity = new_capacity;
    }
    if (range_count != 0U) {
        memcpy(&runtime->async_preempt_ranges[old_count],
               ranges,
               range_count * sizeof(*ranges));
    }
    runtime->async_preempt_range_count = new_count;
    qsort(runtime->async_preempt_ranges,
          new_count,
          sizeof(*runtime->async_preempt_ranges),
          llam_async_range_compare);
    return 0;
}

static bool llam_async_capture_xstate(llam_async_preempt_state_t *state,
                                     const ucontext_t *context) {
    const unsigned char *source;
    unsigned char *destination;
    uint32_t magic;
    size_t copy_size;
    size_t i;

    if (context == NULL || context->uc_mcontext.fpregs == NULL ||
        state == NULL || state->xstate_area == NULL ||
        state->xstate_capacity < LLAM_ASYNC_PREEMPT_FXSAVE_BYTES) {
        return false;
    }

    source = (const unsigned char *)context->uc_mcontext.fpregs;
    destination = (unsigned char *)state->xstate_area;
    magic = llam_async_load_u32(source + LLAM_ASYNC_PREEMPT_XSTATE_SW_OFFSET +
                                LLAM_ASYNC_PREEMPT_XSTATE_SW_MAGIC_OFFSET);
    if (magic == LLAM_ASYNC_PREEMPT_XSTATE_MAGIC1) {
        uint32_t extended_size = llam_async_load_u32(
            source + LLAM_ASYNC_PREEMPT_XSTATE_SW_OFFSET +
            LLAM_ASYNC_PREEMPT_XSTATE_SW_EXTENDED_SIZE_OFFSET);
        uint64_t xfeatures = llam_async_load_u64(
            source + LLAM_ASYNC_PREEMPT_XSTATE_SW_OFFSET +
            LLAM_ASYNC_PREEMPT_XSTATE_SW_FEATURES_OFFSET);
        uint32_t xstate_size = llam_async_load_u32(
            source + LLAM_ASYNC_PREEMPT_XSTATE_SW_OFFSET +
            LLAM_ASYNC_PREEMPT_XSTATE_SW_SIZE_OFFSET);
        uint64_t xstate_bv;
        uint64_t xcomp_bv;

        /* Linux also writes magic1 on non-XSAVE CPUs: save_xstate_epilog
         * publishes a 512-byte FXSAVE image and advertises four extra bytes,
         * but returns before writing an XSAVE header or magic2 trailer.
         * The independently qualified hardware capacity distinguishes this
         * exact legacy ABI from a truncated modern XSAVE frame. */
        if (state->xstate_capacity == LLAM_ASYNC_PREEMPT_FXSAVE_BYTES &&
            state->xsave_mask == UINT64_C(3) &&
            context->uc_flags == (LLAM_ASYNC_PREEMPT_UC_SIGCONTEXT_SS |
                                  LLAM_ASYNC_PREEMPT_UC_STRICT_RESTORE_SS) &&
            xstate_size == LLAM_ASYNC_PREEMPT_FXSAVE_BYTES &&
            extended_size == LLAM_ASYNC_PREEMPT_FXSAVE_BYTES + sizeof(uint32_t) &&
            xfeatures == 0U) {
            copy_size = LLAM_ASYNC_PREEMPT_FXSAVE_BYTES;
            state->xstate_format = LLAM_ASYNC_PREEMPT_XSTATE_FORMAT_LEGACY;
            goto copy_state;
        }
        if (xstate_size < LLAM_ASYNC_PREEMPT_XSTATE_HEADER_BYTES ||
            (size_t)xstate_size > state->xstate_capacity ||
            (size_t)extended_size < (size_t)xstate_size + sizeof(uint32_t) ||
            extended_size > state->xstate_capacity + sizeof(uint32_t) ||
            llam_async_load_u32(source + extended_size - sizeof(uint32_t)) !=
                LLAM_ASYNC_PREEMPT_XSTATE_MAGIC2 ||
            (xfeatures & ~state->xsave_mask) != 0U) {
            return false;
        }
        xstate_bv = llam_async_load_u64(source + LLAM_ASYNC_PREEMPT_XSTATE_BV_OFFSET);
        xcomp_bv = llam_async_load_u64(source + LLAM_ASYNC_PREEMPT_XCOMP_BV_OFFSET);
        /* The signal ABI currently supplies standard-format XSAVE images only. */
        if ((xstate_bv & ~state->xsave_mask) != 0U || xcomp_bv != 0U) {
            return false;
        }
        /* Preserve the signal ABI trailer as well as the XSAVE register image. */
        copy_size = extended_size;
        state->xstate_format = LLAM_ASYNC_PREEMPT_XSTATE_FORMAT_XSAVE;
    } else if (magic == 0U && (state->xsave_mask & ~UINT64_C(3)) == 0U) {
        copy_size = LLAM_ASYNC_PREEMPT_FXSAVE_BYTES;
        state->xstate_format = LLAM_ASYNC_PREEMPT_XSTATE_FORMAT_LEGACY;
    } else {
        return false;
    }

copy_state:
    llam_async_copy_bytes(destination, source, copy_size);
    for (i = copy_size; i < state->xstate_capacity + sizeof(uint32_t); ++i) {
        destination[i] = 0U;
    }
    state->captured_xstate_size = copy_size;
    return true;
}

#if defined(LLAM_ENABLE_TEST_HOOKS)
bool llam_async_preempt_test_capture_xstate(llam_async_preempt_state_t *state,
                                            const void *context) {
    return llam_async_capture_xstate(state, context);
}
#endif

static bool llam_async_signal_frame_supported(const ucontext_t *context) {
    uint64_t segments = (uint64_t)context->uc_mcontext.gregs[REG_CSGSFS];

    /*
     * This backend restores both user CS and SS. Some translation runtimes
     * supply a partial signal context (CS only) and cannot consume a standalone
     * native frame. Refuse it; never invent the missing selector. Older native
     * ABIs without a saved SS are outside this initial backend as well.
     */
    return (segments & UINT64_C(3)) == UINT64_C(3) &&
           ((segments >> 48U) & UINT64_C(3)) == UINT64_C(3);
}

#if defined(LLAM_ENABLE_TEST_HOOKS)
bool llam_async_preempt_test_frame_supported(const void *context) {
    return context != NULL && llam_async_signal_frame_supported(context);
}
#endif

static void llam_async_capture_gregs(llam_async_preempt_context_t *capture,
                                     const ucontext_t *context) {
    const greg_t *registers = context->uc_mcontext.gregs;

    capture->r8 = (uint64_t)registers[REG_R8];
    capture->r9 = (uint64_t)registers[REG_R9];
    capture->r10 = (uint64_t)registers[REG_R10];
    capture->r11 = (uint64_t)registers[REG_R11];
    capture->r12 = (uint64_t)registers[REG_R12];
    capture->r13 = (uint64_t)registers[REG_R13];
    capture->r14 = (uint64_t)registers[REG_R14];
    capture->r15 = (uint64_t)registers[REG_R15];
    capture->rdi = (uint64_t)registers[REG_RDI];
    capture->rsi = (uint64_t)registers[REG_RSI];
    capture->rbp = (uint64_t)registers[REG_RBP];
    capture->rbx = (uint64_t)registers[REG_RBX];
    capture->rdx = (uint64_t)registers[REG_RDX];
    capture->rax = (uint64_t)registers[REG_RAX];
    capture->rcx = (uint64_t)registers[REG_RCX];
    capture->rsp = (uint64_t)registers[REG_RSP];
    capture->rip = (uint64_t)registers[REG_RIP];
    capture->rflags = (uint64_t)registers[REG_EFL];
}

static void llam_async_capture_defer(llam_async_preempt_state_t *state,
                                     llam_task_t *current_task,
                                     bool stale,
                                     unsigned reason,
                                     bool retryable) {
    llam_worker_t *worker = state != NULL ? state->worker : NULL;
    llam_shard_t *shard = worker != NULL ? worker->compat_shard : NULL;

    if (state != NULL && state->task == current_task && current_task != NULL) {
        atomic_store_explicit(&current_task->preempt_requested,
                              1U,
                              memory_order_relaxed);
    }
    if (shard != NULL) {
        atomic_fetch_add_explicit(stale ? &shard->metrics.async_preempt_deferred_stale
                                       : &shard->metrics.async_preempt_deferred_unsafe,
                                  1U,
                                  memory_order_relaxed);
    }
    if (state != NULL) {
        atomic_store_explicit(&state->last_defer_reason,
                              reason,
                              memory_order_relaxed);
        atomic_store_explicit(&state->status,
                              retryable ? LLAM_ASYNC_PREEMPT_STATUS_REQUESTED
                                        : LLAM_ASYNC_PREEMPT_STATUS_IDLE,
                              memory_order_release);
    }
}

static void llam_async_preempt_signal_handler(int signo,
                                              siginfo_t *info,
                                              void *opaque_context) {
    llam_worker_t *worker = g_llam_tls_worker;
    llam_shard_t *shard = g_llam_tls_shard;
    llam_task_t *task = g_llam_tls_task;
    llam_processor_t *processor = g_llam_tls_processor;
    llam_async_preempt_state_t *state;
    llam_async_capsule_t *capsule;
    llam_runtime_t *runtime;
    ucontext_t *context = (ucontext_t *)opaque_context;
    uintptr_t interrupted_pc;
    unsigned expected_status = LLAM_ASYNC_PREEMPT_STATUS_REQUESTED;
    int interrupted_errno = errno;

    (void)info;
    if (signo <= 0 || signo > 64 ||
        signo != atomic_load_explicit(&g_llam_async_preempt_signal,
                                      memory_order_relaxed) ||
        context == NULL || worker == NULL || shard == NULL) {
        return;
    }
    state = (llam_async_preempt_state_t *)atomic_load_explicit(
        &worker->async_preempt_state, memory_order_acquire);
    if (state == NULL) {
        return;
    }
    if (atomic_load_explicit(&state->frame_probe_status, memory_order_acquire) == 1U) {
        bool supported = llam_async_signal_frame_supported(context) &&
                         llam_async_capture_xstate(state, context);

        atomic_store_explicit(&state->frame_probe_status, supported ? 2U : 3U,
                              memory_order_release);
        return;
    }
    if (task == NULL || processor == NULL) {
        return;
    }
    if (!llam_async_observe_restore(state, context)) {
        return;
    }
    if (!atomic_compare_exchange_strong_explicit(&state->status,
                                                &expected_status,
                                                LLAM_ASYNC_PREEMPT_STATUS_CAPTURING,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        atomic_fetch_add_explicit(&shard->metrics.async_preempt_collisions,
                                  1U,
                                  memory_order_relaxed);
        return;
    }

    runtime = shard->runtime;
    if (runtime == NULL ||
        (runtime->execution_lease_mode == LLAM_EXECUTION_LEASE_REGISTERED
            ? !llam_worker_matches_shard(worker, shard) : worker->compat_shard != shard) ||
        state->worker != worker || state->task != task ||
        state->processor != processor || state->processor != &shard->processor ||
        runtime != worker->compat_shard->runtime) {
        llam_async_capture_defer(state, task, true, 1U, false);
        return;
    }
    if (atomic_load_explicit(&runtime->async_preempt_enabled,
                             memory_order_acquire) == 0U ||
        (state->drain_request
            ? !llam_async_preempt_drain_service_needed(runtime)
            : (atomic_load_explicit(&runtime->stop_requested, memory_order_acquire) != 0U ||
               atomic_load_explicit(&runtime->shutdown_requested, memory_order_acquire) != 0U)) ||
        runtime->async_preempt_control == NULL ||
        state->request_run_generation !=
            ((llam_async_runtime_control_t *)runtime->async_preempt_control)->run_generation) {
        llam_async_capture_defer(state, task, true, 7U, false);
        return;
    }
    if (!atomic_load_explicit(&worker->native_thread_live, memory_order_acquire) ||
        atomic_load_explicit(&worker->binding_incarnation, memory_order_acquire) !=
            state->worker_incarnation) {
        llam_async_capture_defer(state, task, true, 8U, false);
        return;
    }
    if (atomic_load_explicit(&worker->async_preempt_ticket_lease_epoch,
                             memory_order_acquire) != state->lease_projection_epoch ||
        atomic_load_explicit(&worker->async_preempt_ticket_generation,
                             memory_order_acquire) != state->p_generation) {
        llam_async_capture_defer(state, task, true, 10U, false);
        return;
    }
    if (state->p_generation == 0U) {
        llam_async_capture_defer(state, task, true, 9U, false);
        return;
    }
    if (atomic_load_explicit(&worker->run_generation, memory_order_acquire) !=
            state->worker_run_generation ||
        atomic_load_explicit(&task->run_generation, memory_order_acquire) !=
            state->task_run_generation) {
        llam_async_capture_defer(state, task, true, 2U, false);
        return;
    }
    if (atomic_load_explicit(&worker->current_p, memory_order_acquire) != processor ||
        atomic_load_explicit(&worker->runtime_critical_depth, memory_order_acquire) != 0U ||
        atomic_load_explicit(&worker->preempt_disable_depth, memory_order_acquire) != 0U ||
        atomic_load_explicit(&worker->native_domain_depth, memory_order_acquire) != 0U ||
        atomic_load_explicit(&worker->execution_state, memory_order_acquire) !=
            LLAM_WORKER_EXEC_MANAGED_SAFE ||
        llam_worker_load_active_g(worker) != task ||
        atomic_load_explicit(&task->state, memory_order_acquire) != LLAM_TASK_STATE_RUNNING ||
        (task->flags & LLAM_TASK_FLAG_NO_PREEMPT) != 0U ||
        atomic_load_explicit(&task->opaque_blocking_depth, memory_order_acquire) != 0U) {
        llam_async_capture_defer(state, task, false, 3U, true);
        return;
    }

    interrupted_pc = (uintptr_t)context->uc_mcontext.gregs[REG_RIP];
    if (state->trampoline_stack_top == 0U ||
        !llam_async_on_task_stack(task, (uintptr_t)context->uc_mcontext.gregs[REG_RSP]) ||
        (llam_async_load_u64((const unsigned char *)&context->uc_sigmask) &
         (UINT64_C(1) << (unsigned)(signo - 1))) != 0U ||
        !llam_async_pc_is_registered(runtime, interrupted_pc)) {
        llam_async_capture_defer(state, task, false, 4U, true);
        return;
    }

    capsule = llam_async_claim_capsule(task);
    if (capsule == NULL) {
        llam_async_capture_defer(state, task, false, 11U, true);
        return;
    }

    llam_async_capture_gregs(&state->context, context);
    if (!llam_async_signal_frame_supported(context) ||
        !llam_async_capture_xstate(state, context)) {
        atomic_store_explicit(&capsule->phase, LLAM_ASYNC_CAPSULE_FREE,
                              memory_order_release);
        atomic_store_explicit(&state->disabled, 1U, memory_order_release);
        /* Retain scheduler-only dispatch/foreign guards until capsules drain. */
        /* The signal path may publish only lock-free scalar capability state. */
        atomic_store_explicit(&runtime->preempt_capability.async_state,
                              LLAM_PREEMPT_ASYNC_DISABLED,
                              memory_order_release);
        atomic_store_explicit(&runtime->preempt_capability.failure_reason,
                              LLAM_PREEMPT_FAILURE_CONTEXT_UNSUPPORTED,
                              memory_order_release);
        llam_async_capture_defer(state, task, false, 5U, false);
        return;
    }
    if (atomic_exchange_explicit(&task->preempt_requested,
                                 0U,
                                 memory_order_acq_rel) == 0U) {
        atomic_store_explicit(&capsule->phase, LLAM_ASYNC_CAPSULE_FREE,
                              memory_order_release);
        llam_async_capture_defer(state, task, true, 6U, false);
        return;
    }

    capsule->task = task;
    capsule->origin_m = worker;
    capsule->origin_incarnation = state->worker_incarnation;
    capsule->capture_sequence = state->request_generation;
    capsule->uc_flags = context->uc_flags;
    capsule->signal_stack = context->uc_stack;
    llam_async_copy_bytes((unsigned char *)&capsule->machine,
                          (const unsigned char *)&context->uc_mcontext,
                          sizeof(capsule->machine));
    capsule->signal_mask = llam_async_load_u64(
        (const unsigned char *)&context->uc_sigmask);
    capsule->xstate_bytes = state->captured_xstate_size;
    llam_async_copy_bytes(capsule->xstate_area, state->xstate_area,
                          capsule->xstate_bytes);
    capsule->machine.fpregs = capsule->xstate_area;
    capsule->saved_errno = interrupted_errno;
    state->captured_capsule = capsule;
    atomic_store_explicit(&capsule->phase, LLAM_ASYNC_CAPSULE_CAPTURED,
                          memory_order_release);

    atomic_fetch_add_explicit(&shard->metrics.async_preempt_context_bytes,
                              sizeof(state->context) + state->captured_xstate_size,
                              memory_order_relaxed);

    atomic_store_explicit(&state->acknowledged_generation,
                          state->request_generation,
                          memory_order_release);
    atomic_fetch_add_explicit(&shard->metrics.async_preempt_captures,
                              1U,
                              memory_order_relaxed);
    context->uc_mcontext.gregs[REG_RIP] = (greg_t)(uintptr_t)llam_async_preempt_trampoline;
    context->uc_mcontext.gregs[REG_RSP] = (greg_t)state->trampoline_stack_top;
    context->uc_mcontext.gregs[REG_RDI] = (greg_t)(uintptr_t)state;
    context->uc_mcontext.gregs[REG_EFL] &=
        (greg_t)~((UINT64_C(1) << 8U) |  /* Trap flag. */
                  (UINT64_C(1) << 10U) | /* Direction flag. */
                  (UINT64_C(1) << 14U) | /* Nested-task flag. */
                  (UINT64_C(1) << 16U) | /* Resume flag. */
                  (UINT64_C(1) << 18U)); /* Alignment-check flag. */
    atomic_store_explicit(&state->status,
                          LLAM_ASYNC_PREEMPT_STATUS_CAPTURED,
                          memory_order_release);
}

static int llam_async_install_signal_locked(int *out_signal) {
    struct sigaction action;
    int signo;

    memset(&action, 0, sizeof(action));
    action.sa_sigaction = llam_async_preempt_signal_handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
    for (signo = SIGRTMIN + 2; signo <= SIGRTMAX; ++signo) {
        struct sigaction current_action;
        struct sigaction installed_previous_action;
        struct sigaction installed_action;

        if (sigaction(signo, NULL, &current_action) != 0 ||
            current_action.sa_handler != SIG_DFL) {
            continue;
        }
        /* DFL is also used by wait-sets. Owner lock -> signal registry; waiter
         * creation never takes this owner lock, and handlers take neither. */
        if (llam_async_signal_claim_number(signo) != 0) continue;
        sigemptyset(&action.sa_mask);
        (void)sigaddset(&action.sa_mask, signo);
        if (sigaction(signo, &action, &installed_previous_action) != 0) {
            int saved_error = errno;
            llam_async_signal_release_number(signo);
            errno = saved_error;
            if (saved_error == EINVAL) continue;
            return -1;
        }
        if (installed_previous_action.sa_handler != SIG_DFL) {
            /* Preserve an observed foreign action if ours is still installed.
             * This is not a sigaction CAS; concurrent foreign mutation is outside
             * the profile, including between the query and this rollback. */
            if (sigaction(signo, NULL, &installed_action) != 0) _Exit(125);
            if ((installed_action.sa_flags & SA_SIGINFO) != 0 &&
                installed_action.sa_sigaction == llam_async_preempt_signal_handler &&
                sigaction(signo, &installed_previous_action, NULL) != 0) {
                /* Do not release a claim with our uncertain handler installed. */
                _Exit(125);
            }
            llam_async_signal_release_number(signo);
            continue;
        }
        g_llam_async_preempt_previous_action = installed_previous_action;
        atomic_store_explicit(&g_llam_async_preempt_signal,
                              signo,
                              memory_order_release);
        g_llam_async_preempt_action_installed = true;
        *out_signal = signo;
        return 0;
    }
    errno = ENOTSUP;
    return -1;
}

static bool llam_async_cet_disabled(void) {
#if defined(__CET__) && (__CET__ != 0)
    return false;
#else
    unsigned long status = 0UL;
    long rc = syscall(SYS_arch_prctl, LLAM_ASYNC_ARCH_SHSTK_STATUS, &status);

    if (rc == 0) {
        return (status & LLAM_ASYNC_ARCH_SHSTK_ENABLED) == 0UL;
    }
    return errno == EINVAL || errno == ENOSYS;
#endif
}

static int llam_async_state_init_xstate(llam_async_preempt_state_t *state) {
    unsigned eax;
    unsigned ebx;
    unsigned ecx;
    unsigned edx;
    unsigned max_leaf;
    uint64_t supported_mask;
    uint64_t xcr0;
    unsigned xcr0_lo;
    unsigned xcr0_hi;
    size_t area_size;
    int rc;

    max_leaf = __get_cpuid_max(0U, NULL);
    if (max_leaf < 1U || !__get_cpuid(1U, &eax, &ebx, &ecx, &edx)) {
        errno = ENOTSUP;
        return -1;
    }
    if ((ecx & bit_XSAVE) == 0U || (ecx & bit_OSXSAVE) == 0U || max_leaf < 0xDU) {
        state->xsave_mask = UINT64_C(3);
        area_size = LLAM_ASYNC_PREEMPT_FXSAVE_BYTES;
        state->xstate_format = LLAM_ASYNC_PREEMPT_XSTATE_FORMAT_LEGACY;
    } else {
        __asm__ volatile("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0U));
        xcr0 = ((uint64_t)xcr0_hi << 32U) | (uint64_t)xcr0_lo;
        if (!__get_cpuid_count(0xDU, 0U, &eax, &ebx, &ecx, &edx)) {
            errno = ENOTSUP;
            return -1;
        }
        supported_mask = ((uint64_t)edx << 32U) | (uint64_t)eax;
        state->xsave_mask = xcr0 & supported_mask;
        if ((state->xsave_mask & UINT64_C(3)) != UINT64_C(3)) {
            errno = ENOTSUP;
            return -1;
        }
        /* PKRU changes data-page access rights; this trampoline cannot restore it last yet. */
        if ((state->xsave_mask & LLAM_ASYNC_PREEMPT_XSTATE_PKRU_MASK) != 0U) {
            errno = ENOTSUP;
            return -1;
        }
        area_size = ebx > ecx ? (size_t)ebx : (size_t)ecx;
        state->xstate_format = LLAM_ASYNC_PREEMPT_XSTATE_FORMAT_XSAVE;
        if (area_size < LLAM_ASYNC_PREEMPT_XSTATE_HEADER_BYTES ||
            area_size > LLAM_ASYNC_PREEMPT_XSTATE_MAX_BYTES) {
            errno = ENOTSUP;
            return -1;
        }
    }
    state->xstate_capacity = area_size;
    rc = llam_async_test_fail_allocation(LLAM_ASYNC_TEST_FAIL_XSTATE)
             ? ENOMEM : posix_memalign(&state->xstate_area, 64U,
                                      area_size + sizeof(uint32_t));
    if (rc != 0) {
        errno = rc;
        return -1;
    }
    memset(state->xstate_area, 0, area_size + sizeof(uint32_t));
    return 0;
}

static int llam_async_state_init_trampoline_stack(llam_async_preempt_state_t *state) {
    long page_size = llam_page_size();
    size_t stack_size = LLAM_ASYNC_PREEMPT_TRAMPOLINE_STACK_BYTES;
    size_t mapping_size;
    void *mapping;

    if (llam_async_test_fail_allocation(LLAM_ASYNC_TEST_FAIL_LANDING)) {
        errno = ENOMEM;
        return -1;
    }
    if (page_size <= 0 || (size_t)page_size > SIZE_MAX / 2U ||
        stack_size > SIZE_MAX - 2U * (size_t)page_size) {
        errno = EOVERFLOW;
        return -1;
    }
    mapping_size = stack_size + 2U * (size_t)page_size;
    mapping = mmap(NULL,
                   mapping_size,
                   PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK,
                   -1,
                   0);
    if (mapping == MAP_FAILED) {
        return -1;
    }
    if (mprotect((unsigned char *)mapping + page_size,
                 stack_size,
                 PROT_READ | PROT_WRITE) != 0) {
        int saved_errno = errno;

        (void)munmap(mapping, mapping_size);
        errno = saved_errno;
        return -1;
    }
    state->trampoline_stack_mapping = mapping;
    state->trampoline_stack_mapping_size = mapping_size;
    state->trampoline_stack_top =
        ((uintptr_t)mapping + (size_t)page_size + stack_size) & ~(uintptr_t)15U;
    return 0;
}

static llam_async_preempt_state_t *llam_async_state_create(llam_worker_t *worker) {
    llam_async_preempt_state_t *state;
    llam_async_worker_bridge_t *bridge;
    int allocation_rc;

    state = llam_async_test_fail_allocation(LLAM_ASYNC_TEST_FAIL_WORKER)
                ? NULL : calloc(1U, sizeof(*state));
    if (state == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    atomic_init(&state->status, LLAM_ASYNC_PREEMPT_STATUS_IDLE);
    atomic_init(&state->next_request_generation, 0U);
    atomic_init(&state->acknowledged_generation, 0U);
    atomic_init(&state->last_defer_reason, 0U);
    atomic_init(&state->disabled, 0U);
    atomic_init(&state->last_request_signal_ns, 0U);
    atomic_init(&state->pending_restore, 0U);
    atomic_init(&state->observed_restore_sequence, 0U);
    atomic_init(&state->frame_probe_status, 0U);
    atomic_init(&state->affine_live_g, 0U);
    atomic_init(&state->ready_depth, 0U);
    state->prefer_origin = false;
    state->worker = worker;
    if (!llam_async_cet_disabled()) {
        free(state);
        errno = ENOTSUP;
        return NULL;
    }
    if (llam_async_state_init_xstate(state) != 0 ||
        llam_async_state_init_trampoline_stack(state) != 0) {
        int saved_errno = errno != 0 ? errno : ENOTSUP;

        if (state->xstate_area != NULL) {
            free(state->xstate_area);
        }
        if (state->trampoline_stack_mapping != NULL) {
            (void)munmap(state->trampoline_stack_mapping,
                         state->trampoline_stack_mapping_size);
        }
        free(state);
        errno = saved_errno;
        return NULL;
    }
    bridge = calloc(1U, sizeof(*bridge));
    state->bridge_storage = bridge;
    if (bridge != NULL && pthread_mutex_init(&bridge->ready_lock, NULL) != 0) {
        free(bridge);
        bridge = NULL;
        state->bridge_storage = NULL;
    }
    allocation_rc = posix_memalign(&state->scheduler_fxstate, 16U,
                                   LLAM_ASYNC_PREEMPT_FXSAVE_BYTES);
    if (bridge != NULL && allocation_rc == 0) {
        allocation_rc = llam_async_test_fail_allocation(LLAM_ASYNC_TEST_FAIL_RESTORE)
                            ? ENOMEM : posix_memalign(&bridge->restore_xstate, 64U,
                                           state->xstate_capacity + sizeof(uint32_t));
    }
    if (bridge == NULL || allocation_rc != 0) {
        free(state->scheduler_fxstate);
        if (bridge != NULL) pthread_mutex_destroy(&bridge->ready_lock);
        free(bridge);
        free(state->xstate_area);
        (void)munmap(state->trampoline_stack_mapping,
                     state->trampoline_stack_mapping_size);
        free(state);
        errno = allocation_rc != 0 ? allocation_rc : ENOMEM;
        return NULL;
    }
    memset(state->scheduler_fxstate, 0, LLAM_ASYNC_PREEMPT_FXSAVE_BYTES);
    memset(bridge->restore_xstate, 0, state->xstate_capacity + sizeof(uint32_t));
    return state;
}

static void llam_async_state_destroy(llam_async_preempt_state_t *state) {
    llam_async_worker_bridge_t *bridge;

    if (state == NULL) {
        return;
    }
    /* Shutdown is after worker/run drain; a live restore cannot be discarded. */
    if (atomic_load_explicit(&state->pending_restore, memory_order_acquire) != 0U ||
        state->captured_capsule != NULL ||
        atomic_load_explicit(&state->affine_live_g, memory_order_acquire) != 0U ||
        atomic_load_explicit(&state->ready_depth, memory_order_acquire) != 0U ||
        state->ready_head != NULL || state->ready_tail != NULL) {
        abort();
    }
    bridge = state->bridge_storage;
    if (bridge != NULL) {
        free(bridge->restore_xstate);
        pthread_mutex_destroy(&bridge->ready_lock);
        free(bridge);
    }
    free(state->scheduler_fxstate);
    free(state->xstate_area);
    if (state->trampoline_stack_mapping != NULL) {
        (void)munmap(state->trampoline_stack_mapping,
                     state->trampoline_stack_mapping_size);
    }
    free(state);
}

int llam_async_preempt_register_safe_range(llam_runtime_t *runtime,
                                           uintptr_t begin,
                                           uintptr_t end) {
    llam_runtime_t *pinned_runtime = NULL;
    bool expected_exec_started = false;
    int result = -1;

    if (begin == 0U || end <= begin) {
        errno = EINVAL;
        return -1;
    }
    if (llam_runtime_begin_public_op(runtime, &pinned_runtime) != 0) {
        return -1;
    }
    if (!atomic_compare_exchange_strong_explicit(&pinned_runtime->exec_started,
                                                 &expected_exec_started,
                                                 true,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        llam_runtime_end_public_op(pinned_runtime);
        errno = EBUSY;
        return -1;
    }

    pthread_mutex_lock(&g_llam_async_preempt_lock);
    if (!atomic_load_explicit(&pinned_runtime->initialized, memory_order_acquire) ||
        atomic_load_explicit(&pinned_runtime->async_preempt_enabled, memory_order_acquire) != 0U) {
        errno = EBUSY;
        goto unlock;
    }

    {
        const llam_async_preempt_range_t range = {begin, end};

        result = llam_async_append_ranges_locked(pinned_runtime, &range, 1U);
    }

unlock:
    pthread_mutex_unlock(&g_llam_async_preempt_lock);
    if (result == 0) {
        llam_preempt_capabilities_note_ranges(pinned_runtime,
                                               pinned_runtime->async_preempt_range_count);
    }
    atomic_store_explicit(&pinned_runtime->exec_started,
                          false,
                          memory_order_release);
    llam_runtime_end_public_op(pinned_runtime);
    return result;
}

int llam_preempt_module_register(llam_runtime_t *runtime,
                                 uintptr_t image_base,
                                 size_t image_size,
                                 const void *opaque_metadata,
                                 size_t metadata_size,
                                 bool required_mode) {
    llam_runtime_t *pinned_runtime = NULL;
    llam_async_preempt_range_t *module_ranges = NULL;
    size_t module_range_count = 0U;
    size_t module_range_capacity = 0U;
    size_t offset = 0U;
    bool expected_exec_started = false;
    int result = -1;
    int saved_errno = 0;
    const unsigned char *metadata = opaque_metadata;

    if (metadata == NULL || metadata_size == 0U) {
        if (required_mode) {
            errno = ENOTSUP;
            return -1;
        }
        return 0;
    }
    if (llam_runtime_begin_public_op(runtime, &pinned_runtime) != 0) {
        return -1;
    }
    if (!atomic_compare_exchange_strong_explicit(&pinned_runtime->exec_started,
                                                 &expected_exec_started,
                                                 true,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        llam_runtime_end_public_op(pinned_runtime);
        errno = EBUSY;
        return -1;
    }
    if (!atomic_load_explicit(&pinned_runtime->initialized, memory_order_acquire)) {
        saved_errno = ENOTSUP;
        goto finish;
    }
    if (pinned_runtime->preempt_mode < LLAM_PREEMPT_AUTO) {
        if (!required_mode) {
            result = 0;
            goto finish;
        }
        saved_errno = ENOTSUP;
        goto finish;
    }

    while (offset < metadata_size) {
        llam_preempt_metadata_t parsed;
        size_t consumed = 0U;
        llam_preempt_metadata_result_t parse_result =
            llam_preempt_metadata_parse_one(metadata + offset,
                                             metadata_size - offset,
                                             required_mode,
                                             LLAM_PREEMPT_METADATA_ARCH_X86_64,
                                             image_base,
                                             image_size,
                                             &parsed,
                                             &consumed);
        size_t range_index;

        if (parse_result == LLAM_PREEMPT_METADATA_IGNORED_OPTIONAL) {
            if (consumed == 0U) {
                saved_errno = EINVAL;
                goto finish;
            }
            offset += consumed;
            continue;
        }
        if (parse_result != LLAM_PREEMPT_METADATA_OK) {
            saved_errno = parse_result == LLAM_PREEMPT_METADATA_UNSUPPORTED
                              ? ENOTSUP
                              : (errno != 0 ? errno : EINVAL);
            llam_preempt_metadata_destroy(&parsed);
            goto finish;
        }
        for (range_index = 0U; range_index < parsed.range_count; ++range_index) {
            const llam_preempt_metadata_range_t *range = &parsed.ranges[range_index];

            if ((range->flags & LLAM_PREEMPT_RANGE_ASYNC_SAFE) == 0U) {
                continue;
            }
            if (module_range_count == module_range_capacity) {
                size_t new_capacity = module_range_capacity == 0U
                                          ? 4U
                                          : module_range_capacity * 2U;
                llam_async_preempt_range_t *new_ranges;

                if (new_capacity < module_range_capacity ||
                    new_capacity > SIZE_MAX / sizeof(*new_ranges)) {
                    saved_errno = EOVERFLOW;
                    llam_preempt_metadata_destroy(&parsed);
                    goto finish;
                }
                new_ranges = realloc(module_ranges,
                                     new_capacity * sizeof(*new_ranges));
                if (new_ranges == NULL) {
                    saved_errno = ENOMEM;
                    llam_preempt_metadata_destroy(&parsed);
                    goto finish;
                }
                module_ranges = new_ranges;
                module_range_capacity = new_capacity;
            }
            module_ranges[module_range_count].begin = range->start_pc;
            module_ranges[module_range_count].end = range->end_pc;
            module_range_count += 1U;
        }
        llam_preempt_metadata_destroy(&parsed);
        if (consumed == 0U) {
            saved_errno = EINVAL;
            goto finish;
        }
        offset += consumed;
    }
    if (module_range_count == 0U) {
        if (!required_mode) {
            result = 0;
            goto finish;
        }
        saved_errno = ENOTSUP;
        goto finish;
    }

    pthread_mutex_lock(&g_llam_async_preempt_lock);
    if (atomic_load_explicit(&pinned_runtime->async_preempt_enabled,
                             memory_order_acquire) != 0U) {
        saved_errno = EBUSY;
    } else if (llam_async_append_ranges_locked(pinned_runtime,
                                               module_ranges,
                                               module_range_count) == 0) {
        result = 0;
    } else {
        saved_errno = errno != 0 ? errno : EINVAL;
    }
    pthread_mutex_unlock(&g_llam_async_preempt_lock);
    if (result == 0) {
        llam_preempt_capabilities_note_ranges(pinned_runtime,
                                               pinned_runtime->async_preempt_range_count);
    }

finish:
    free(module_ranges);
    atomic_store_explicit(&pinned_runtime->exec_started,
                          false,
                          memory_order_release);
    llam_runtime_end_public_op(pinned_runtime);
    if (result != 0) {
        errno = saved_errno != 0 ? saved_errno : EINVAL;
    }
    return result;
}

int llam_async_preempt_enable_runtime(llam_runtime_t *runtime) {
    llam_runtime_t *pinned_runtime = NULL;
    llam_worker_t *worker = NULL;
    llam_async_runtime_control_t *control = NULL;
    unsigned i;
    uint64_t capsule_limit;
    uint64_t block_limit;
    bool expected_exec_started = false;
    int signo = 0;
    int result = -1;
    int saved_errno;
    llam_preempt_failure_reason_t capability_failure = LLAM_PREEMPT_FAILURE_NONE;

    if (llam_runtime_begin_public_op(runtime, &pinned_runtime) != 0) {
        return -1;
    }
    if (!atomic_compare_exchange_strong_explicit(&pinned_runtime->exec_started,
                                                 &expected_exec_started,
                                                 true,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        llam_runtime_end_public_op(pinned_runtime);
        errno = EBUSY;
        return -1;
    }
    pthread_mutex_lock(&g_llam_async_preempt_lock);
    if (g_llam_async_preempt_runtime != NULL) {
        capability_failure = LLAM_PREEMPT_FAILURE_CAPTURE_SLOT_BUSY;
        errno = EBUSY;
        goto unlock;
    }
    if (pinned_runtime->preempt_mode < LLAM_PREEMPT_AUTO) {
        capability_failure = LLAM_PREEMPT_FAILURE_COOPERATIVE_MODE;
        errno = ENOTSUP;
        goto unlock;
    }
    if (pinned_runtime->async_preempt_range_count == 0U && pinned_runtime->managed_catalog == NULL) {
        capability_failure = LLAM_PREEMPT_FAILURE_NO_SAFE_RANGES;
        errno = ENOTSUP;
        goto unlock;
    }
    if (pinned_runtime->experimental_dynamic_shards != 0U &&
        pinned_runtime->experimental_shard_rings != 0U) {
        capability_failure = LLAM_PREEMPT_FAILURE_DYNAMIC_UNSUPPORTED;
        errno = ENOTSUP;
        goto unlock;
    }
    if (llam_runtime_has_live_tasks(pinned_runtime)) {
        capability_failure = LLAM_PREEMPT_FAILURE_CONTEXT_UNSUPPORTED;
        errno = ENOTSUP;
        goto unlock;
    }
    for (i = 0U; i < pinned_runtime->active_shards; ++i) {
        if (pinned_runtime->shards[i].opaque_helper_thread_started) {
            capability_failure = LLAM_PREEMPT_FAILURE_HELPER_ACTIVE;
            errno = ENOTSUP;
            goto unlock;
        }
    }
    if (!atomic_load_explicit(&pinned_runtime->initialized, memory_order_acquire) ||
        pinned_runtime->active_shards == 0U ||
        atomic_load_explicit(&pinned_runtime->async_preempt_enabled, memory_order_acquire) != 0U) {
        capability_failure = LLAM_PREEMPT_FAILURE_CAPTURE_SLOT_BUSY;
        errno = ENOTSUP;
        goto unlock;
    }
    if (llam_async_read_limit("LLAM_ASYNC_CAPSULE_BUDGET_BYTES", UINT64_C(64) * 1024U * 1024U,
                              SIZE_MAX, &capsule_limit) != 0 ||
        llam_async_read_limit("LLAM_ASYNC_BLOCK_JOB_LIMIT", 1024U, UINT_MAX, &block_limit) != 0) {
        capability_failure = LLAM_PREEMPT_FAILURE_INVALID_CONFIG;
        goto unlock;
    }
    worker = &pinned_runtime->shards[0].primary_worker;
    if (!atomic_is_lock_free(&worker->async_preempt_state) ||
        !atomic_is_lock_free(&worker->run_generation) ||
        !atomic_is_lock_free(&pinned_runtime->async_preempt_enabled) ||
        !llam_async_cet_disabled()) {
        capability_failure = LLAM_PREEMPT_FAILURE_CONTEXT_UNSUPPORTED;
        errno = ENOTSUP;
        goto unlock;
    }
    size_t native_count = pinned_runtime->active_shards +
        (pinned_runtime->execution_lease_mode == LLAM_EXECUTION_LEASE_REGISTERED
            ? pinned_runtime->managed_worker_count : 0U);
    if (native_count > UINT_MAX || native_count > (SIZE_MAX - sizeof(*control)) / sizeof(control->states[0])) {
        errno = EOVERFLOW;
        goto unlock;
    }
    control = calloc(1U, sizeof(*control) + native_count * sizeof(control->states[0]));
    if (control == NULL) {
        capability_failure = LLAM_PREEMPT_FAILURE_CONTEXT_ALLOCATION;
        errno = ENOMEM;
        goto unlock;
    }
    control->worker_count = (unsigned)native_count;
    control->registered = pinned_runtime->execution_lease_mode == LLAM_EXECUTION_LEASE_REGISTERED;
    atomic_init(&control->start_phase, LLAM_ASYNC_START_IDLE);
    atomic_init(&control->prepared_workers, 0U);
    atomic_init(&control->ready_tasks, 0U);
    atomic_init(&control->start_error, 0);
    atomic_init(&control->capsule_bytes, 0U);
    atomic_init(&control->block_slots, 0U);
    if (pinned_runtime->managed_catalog != NULL) {
        uint64_t catalog_budget = llam_managed_catalog_budget(pinned_runtime);
        if (catalog_budget != 0U && catalog_budget < capsule_limit) capsule_limit = catalog_budget;
    }
    control->capsule_limit = (size_t)capsule_limit;
    control->block_limit = (unsigned)block_limit;
    for (i = 0U; i < control->worker_count; ++i) {
        worker = i < pinned_runtime->active_shards ? &pinned_runtime->shards[i].primary_worker
            : &pinned_runtime->managed_workers[i - pinned_runtime->active_shards];
        control->states[i] = llam_async_state_create(worker);
        if (control->states[i] == NULL) {
            capability_failure = errno == ENOTSUP ? LLAM_PREEMPT_FAILURE_CONTEXT_UNSUPPORTED
                                                  : LLAM_PREEMPT_FAILURE_CONTEXT_ALLOCATION;
            goto unlock;
        }
        if (control->states[i]->xstate_capacity != control->states[0]->xstate_capacity ||
            control->states[i]->xsave_mask != control->states[0]->xsave_mask ||
            control->states[i]->xstate_format != control->states[0]->xstate_format) {
            capability_failure = LLAM_PREEMPT_FAILURE_CONTEXT_UNSUPPORTED;
            errno = ENOTSUP;
            goto unlock;
        }
    }
    if (control->states[0]->xstate_capacity >
        (SIZE_MAX - sizeof(llam_async_task_state_t)) / 2U - sizeof(uint32_t)) {
        capability_failure = LLAM_PREEMPT_FAILURE_CONTEXT_ALLOCATION;
        errno = ENOMEM;
        goto unlock;
    }
    size_t stride = (control->states[0]->xstate_capacity + sizeof(uint32_t) + 63U) & ~(size_t)63U;
    if (stride > SIZE_MAX / 2U || llam_capsule_pool_init(&control->capsule_pool,
            sizeof(llam_async_task_state_t), 2U * stride, control->capsule_limit,
            (size_t)control->worker_count * 2U + 2U,
            llam_linux_capsule_prepare, llam_linux_capsule_destroy, control) != 0) {
        capability_failure = LLAM_PREEMPT_FAILURE_CONTEXT_ALLOCATION;
        goto unlock;
    }
    control->capsule_charge = control->capsule_pool.charge;
    if (llam_async_install_signal_locked(&signo) != 0) {
        capability_failure = LLAM_PREEMPT_FAILURE_SIGNAL_CONFLICT;
        goto unlock;
    }
    pinned_runtime->async_preempt_control = control;
    pinned_runtime->managed_capsule_pool = &control->capsule_pool;
    for (i = 0U; i < control->worker_count; ++i) {
        if (!control->registered)
            atomic_store_explicit(&control->states[i]->worker->async_preempt_state,
                                  (uintptr_t)control->states[i], memory_order_release);
    }
    pinned_runtime->async_preempt_signal = signo;
    g_llam_async_preempt_runtime = pinned_runtime;
    atomic_store_explicit(&pinned_runtime->async_preempt_enabled,
                          1U,
                          memory_order_release);
    if (control->registered) {
        pinned_runtime->managed_scheduler_ops = &g_llam_linux_managed_ops;
        pinned_runtime->managed_scheduler_data = control;
    }
    control = NULL;
    result = 0;

unlock:
    saved_errno = errno;
    pthread_mutex_unlock(&g_llam_async_preempt_lock);
    if (control != NULL) {
        llam_capsule_pool_destroy(&control->capsule_pool);
        if (atomic_load_explicit(&control->capsule_bytes, memory_order_acquire) != 0U ||
            atomic_load_explicit(&control->block_slots, memory_order_acquire) != 0U) {
            abort();
        }
        for (i = 0U; i < control->worker_count; ++i) {
            llam_async_state_destroy(control->states[i]);
        }
        free(control);
    }
    atomic_store_explicit(&pinned_runtime->exec_started,
                          false,
                          memory_order_release);
    llam_preempt_capabilities_note_async_result(
        pinned_runtime,
        result == 0,
        result == 0 ? LLAM_PREEMPT_FAILURE_NONE
                    : (capability_failure != LLAM_PREEMPT_FAILURE_NONE
                           ? capability_failure
                           : LLAM_PREEMPT_FAILURE_CONTEXT_UNSUPPORTED));
    if (result == 0) {
        /* Storage is ready; actual native-thread ABI qualification is per run. */
        atomic_store_explicit(&pinned_runtime->preempt_capability.async_state,
                              LLAM_PREEMPT_ASYNC_PREPARED, memory_order_release);
    }
    llam_runtime_end_public_op(pinned_runtime);
    if (result != 0) {
        errno = saved_errno;
    }
    return result;
}

void llam_async_preempt_shutdown_runtime(llam_runtime_t *runtime) {
    llam_async_runtime_control_t *control;
    struct sigaction current_action;
    int signo;
    unsigned i;

    if (runtime == NULL) {
        return;
    }
    (void)atomic_exchange_explicit(&runtime->async_preempt_enabled,
                                   0U,
                                   memory_order_acq_rel);
    control = runtime->async_preempt_control;
    if (control != NULL) {
        for (i = 0U; i < control->worker_count; ++i)
            (void)atomic_exchange_explicit(&control->states[i]->worker->async_preempt_state,
                                           0U, memory_order_acq_rel);
    }

    pthread_mutex_lock(&g_llam_async_preempt_lock);
    if (g_llam_async_preempt_runtime == runtime) {
        signo = atomic_load_explicit(&g_llam_async_preempt_signal,
                                     memory_order_relaxed);
        if (g_llam_async_preempt_action_installed && signo > 0) {
            if (sigaction(signo, NULL, &current_action) != 0) _Exit(125);
            if ((current_action.sa_flags & SA_SIGINFO) != 0 &&
                current_action.sa_sigaction == llam_async_preempt_signal_handler &&
                sigaction(signo, &g_llam_async_preempt_previous_action, NULL) != 0) {
                _Exit(125);
            }
            llam_async_signal_release_number(signo);
        }
        g_llam_async_preempt_action_installed = false;
        atomic_store_explicit(&g_llam_async_preempt_signal,
                              0,
                              memory_order_release);
        g_llam_async_preempt_runtime = NULL;
        runtime->async_preempt_signal = 0;
    }
    free(runtime->async_preempt_ranges);
    runtime->async_preempt_ranges = NULL;
    runtime->async_preempt_range_count = 0U;
    runtime->async_preempt_range_capacity = 0U;
    pthread_mutex_unlock(&g_llam_async_preempt_lock);

    if (control != NULL) {
        if (atomic_load_explicit(&control->capsule_bytes, memory_order_acquire) != 0U ||
            atomic_load_explicit(&control->block_slots, memory_order_acquire) != 0U) {
            abort();
        }
        runtime->managed_capsule_pool = NULL;
        llam_capsule_pool_destroy(&control->capsule_pool);
        for (i = 0U; i < control->worker_count; ++i) {
            llam_async_state_destroy(control->states[i]);
        }
        free(control);
        runtime->async_preempt_control = NULL;
        if (runtime->managed_scheduler_ops == &g_llam_linux_managed_ops) {
            runtime->managed_scheduler_ops = NULL;
            runtime->managed_scheduler_data = NULL;
        }
    }
    llam_preempt_capabilities_note_ranges(runtime, 0U);
    llam_preempt_capabilities_note_async_result(runtime,
                                                false,
                                                LLAM_PREEMPT_FAILURE_NONE);
}

int llam_async_preempt_worker_ready(llam_worker_t *worker) {
    llam_async_preempt_state_t *state;
    llam_runtime_t *runtime;
    sigset_t current_mask;
    uint64_t incarnation;
    llam_async_runtime_control_t *control;
    llam_preempt_failure_reason_t reason = LLAM_PREEMPT_FAILURE_CONTEXT_UNSUPPORTED;
    int rc;

    if (worker == NULL || worker->compat_shard == NULL ||
        (worker->role != LLAM_WORKER_ROLE_PRIMARY &&
         worker->compat_shard->runtime->execution_lease_mode != LLAM_EXECUTION_LEASE_REGISTERED)) {
        return 0;
    }
    runtime = worker->compat_shard->runtime;
    if (runtime == NULL ||
        atomic_load_explicit(&runtime->async_preempt_enabled,
                             memory_order_acquire) == 0U) {
        return 0;
    }
    control = runtime->async_preempt_control;
    if (control == NULL) {
        errno = ENOTSUP;
        return -1;
    }
    state = (llam_async_preempt_state_t *)atomic_load_explicit(
        &worker->async_preempt_state, memory_order_acquire);
    if (state != NULL &&
        atomic_load_explicit(&state->disabled, memory_order_acquire) != 0U) {
        errno = ENOTSUP;
        goto failed;
    }
    incarnation = atomic_load_explicit(&worker->binding_incarnation,
                                       memory_order_acquire);
    if (state == NULL || state->worker != worker || incarnation == 0U ||
        !worker->sigaltstack_installed ||
        worker->signal_stack_incarnation != incarnation ||
        !atomic_load_explicit(&worker->native_thread_live, memory_order_acquire) ||
        (control->registered && (g_llam_tls_worker != worker ||
                                pthread_equal(worker->native_thread, pthread_self()) == 0))) {
        errno = ENOTSUP;
        goto failed;
    }
    rc = pthread_sigmask(SIG_SETMASK, NULL, &current_mask);
    if (rc != 0) {
        errno = rc;
        goto failed;
    }
    if (sigismember(&current_mask, runtime->async_preempt_signal) == 1) {
        reason = LLAM_PREEMPT_FAILURE_SIGNAL_CONFLICT;
        errno = EBUSY;
        goto failed;
    }
    if (sigismember(&current_mask, runtime->async_preempt_signal) < 0) {
        errno = EINVAL;
        goto failed;
    }
#if defined(LLAM_ENABLE_TEST_HOOKS)
    if (llam_async_preempt_test_take_worker_failure(LLAM_ASYNC_TEST_FAIL_WORKER_PROBE,
                                                   worker->compat_shard->id)) {
        reason = LLAM_PREEMPT_FAILURE_WORKER_START;
        errno = EIO;
        goto failed;
    }
    if (llam_async_preempt_test_take_worker_failure(LLAM_ASYNC_TEST_HOLD_WORKER_PROBE,
                                                   worker->compat_shard->id)) {
        atomic_store_explicit(&g_llam_async_test_prepare_gate, 2U, memory_order_release);
        while (atomic_load_explicit(&g_llam_async_test_prepare_gate, memory_order_acquire) == 2U) {
            const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
            (void)nanosleep(&delay, NULL);
        }
    }
#endif
    /* Validate the actual thread's signal ABI before dispatching any user G. */
    atomic_store_explicit(&state->frame_probe_status, 1U, memory_order_release);
    rc = raise(runtime->async_preempt_signal);
    if (rc != 0 ||
        atomic_load_explicit(&state->frame_probe_status, memory_order_acquire) != 2U) {
        atomic_store_explicit(&state->frame_probe_status, 3U, memory_order_release);
        reason = LLAM_PREEMPT_FAILURE_SIGNAL_FRAME_UNSUPPORTED;
        errno = ENOTSUP;
        goto failed;
    }
    if (atomic_load_explicit(&control->start_phase, memory_order_acquire) ==
        LLAM_ASYNC_START_RUNNING) {
        /* Requalification of an idle, unpinned binding is not a new startup arrival. */
        return 0;
    }
    if (atomic_load_explicit(&control->start_phase, memory_order_acquire) == LLAM_ASYNC_START_ABORTED) {
        errno = atomic_load_explicit(&control->start_error, memory_order_acquire);
        return -1;
    }
    if (atomic_load_explicit(&runtime->stop_requested, memory_order_acquire) ||
        atomic_load_explicit(&runtime->shutdown_requested, memory_order_acquire)) {
        /* A pre-dispatch close cannot wait for NEW Gs that were never allowed to run. */
        reason = LLAM_PREEMPT_FAILURE_WORKER_START;
        errno = ECANCELED;
        goto failed;
    }
    if (control->registered) {
        atomic_fetch_add_explicit(&control->prepared_workers, 1U, memory_order_release);
        return 0; /* The managed startup barrier commits all native probes. */
    }
    if (atomic_fetch_add_explicit(&control->prepared_workers, 1U, memory_order_acq_rel) + 1U ==
        control->worker_count) {
        unsigned expected = LLAM_ASYNC_START_COLLECTING;

        if (atomic_compare_exchange_strong_explicit(
                &control->start_phase, &expected, LLAM_ASYNC_START_RUNNING,
                memory_order_acq_rel, memory_order_acquire)) {
            llam_preempt_capabilities_note_async_result(runtime, true, LLAM_PREEMPT_FAILURE_NONE);
        }
    }
    while (atomic_load_explicit(&control->start_phase, memory_order_acquire) ==
           LLAM_ASYNC_START_COLLECTING) {
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};

        /* Startup-only native wait: no G has been admitted to any worker yet. */
        (void)nanosleep(&delay, NULL);
    }
    if (atomic_load_explicit(&control->start_phase, memory_order_acquire) ==
        LLAM_ASYNC_START_RUNNING) {
        return 0;
    }
    errno = atomic_load_explicit(&control->start_error, memory_order_acquire);
    if (errno == 0) {
        errno = ENOTSUP;
    }
    return -1;
failed:
    rc = errno;
    if (control->registered &&
        atomic_load_explicit(&control->start_phase, memory_order_acquire) == LLAM_ASYNC_START_RUNNING) {
        /* A replacement M has no accepted G yet. Quarantine only this slot;
         * aborting the shared run would strand other Ms' accepted restores. */
        if (state != NULL) atomic_store_explicit(&state->disabled, 1U, memory_order_release);
        errno = rc;
        return -1;
    }
    llam_async_preempt_run_abort(runtime, rc, reason);
    errno = rc;
    return -1;
}

void llam_async_preempt_worker_stopped(llam_worker_t *worker) {
    llam_async_preempt_state_t *state = llam_async_worker_state(worker);
    sigset_t reserved;
    sigset_t previous;
    const struct timespec no_wait = {.tv_sec = 0, .tv_nsec = 0};
    int saved_errno = errno;
    int signo;
    int result;

    if (state == NULL) {
        return;
    }
    if (atomic_load_explicit(&worker->native_thread_live, memory_order_acquire) ||
        atomic_load_explicit(&state->pending_restore, memory_order_acquire) != 0U ||
        state->captured_capsule != NULL ||
        atomic_load_explicit(&state->affine_live_g, memory_order_acquire) != 0U ||
        atomic_load_explicit(&state->ready_depth, memory_order_acquire) != 0U) {
        abort();
    }
    /*
     * Unbind acquired the same opaque/shard lock pair held through watchdog
     * sends, so all prior senders finished and new senders reject this binding.
     * Drain while this M's signal stack and TLS still exist, before restoring
     * the borrowed caller's mask and eventually the process signal action.
     */
    signo = worker->compat_shard->runtime->async_preempt_signal;
    sigemptyset(&reserved);
    sigaddset(&reserved, signo);
    if (pthread_sigmask(SIG_BLOCK, &reserved, &previous) != 0) {
        abort();
    }
    do {
        result = sigtimedwait(&reserved, NULL, &no_wait);
    } while (result == signo || (result < 0 && errno == EINTR));
    if (result != -1 || errno != EAGAIN ||
        pthread_sigmask(SIG_SETMASK, &previous, NULL) != 0) {
        abort();
    }
    errno = saved_errno;
}

static bool llam_async_prepare_request_impl(
    llam_worker_t *worker,
    llam_task_t *task,
    const llam_processor_owner_ticket_t *ticket,
    uint64_t worker_run_generation,
    uint64_t task_run_generation,
    uint64_t *out_request_generation,
    bool drain) {
    llam_async_preempt_state_t *state;
    llam_runtime_t *runtime;
    unsigned expected_status = LLAM_ASYNC_PREEMPT_STATUS_IDLE;
    unsigned state_status;
    uint64_t request_generation;
    uint64_t now_ns;

    if (worker == NULL || task == NULL || ticket == NULL ||
        out_request_generation == NULL || worker->compat_shard == NULL) {
        return false;
    }
    runtime = worker->compat_shard->runtime;
    if (runtime == NULL ||
        (task->managed_catalog != NULL && task->managed_execution_class == LLAM_MANAGED_EXECUTION_LEGACY) ||
        atomic_load_explicit(&runtime->async_preempt_enabled,
                             memory_order_acquire) == 0U ||
        runtime->async_preempt_control == NULL ||
        (drain ? !llam_async_preempt_drain_service_needed(runtime)
               : (atomic_load_explicit(&runtime->stop_requested, memory_order_acquire) ||
                  atomic_load_explicit(&runtime->shutdown_requested, memory_order_acquire))) ||
        (runtime->execution_lease_mode == LLAM_EXECUTION_LEASE_REGISTERED
            ? !llam_worker_can_own_processor(worker, ticket->processor)
            : worker != &worker->compat_shard->primary_worker) ||
        ticket->processor != atomic_load_explicit(&worker->current_p, memory_order_acquire) ||
        ticket->owner_m != worker ||
        !((ticket->state == LLAM_PROCESSOR_OWNER_OWNED &&
           ticket->lease_state == (runtime->execution_lease_mode == LLAM_EXECUTION_LEASE_REGISTERED
               ? LLAM_LEASE_REGISTERED_OWNED : LLAM_LEASE_PRIMARY_OWNED)) ||
          (drain && ticket->state == LLAM_PROCESSOR_OWNER_STOPPING && ticket->lease_state == LLAM_LEASE_STOPPING)) ||
        ticket->worker_incarnation !=
            atomic_load_explicit(&worker->binding_incarnation, memory_order_acquire) ||
        ticket->lease_projection_epoch !=
            atomic_load_explicit(&worker->async_preempt_ticket_lease_epoch,
                                 memory_order_acquire) ||
        ticket->p_generation !=
            atomic_load_explicit(&worker->async_preempt_ticket_generation,
                                 memory_order_acquire)) {
        return false;
    }
    state = (llam_async_preempt_state_t *)atomic_load_explicit(
        &worker->async_preempt_state, memory_order_acquire);
    if (state == NULL || state->worker != worker ||
        atomic_load_explicit(&state->disabled, memory_order_acquire) != 0U) {
        return false;
    }
    if (drain) {
        llam_async_task_state_t *storage = task->async_preempt_task;
        if (task->owner_runtime != runtime || storage == NULL ||
            atomic_load_explicit(&storage->origin_m, memory_order_acquire) != worker ||
            storage->origin_incarnation != ticket->worker_incarnation ||
            storage->run_generation != ((llam_async_runtime_control_t *)runtime->async_preempt_control)->run_generation ||
            !llam_worker_has_safe_managed_g(worker, ticket->processor, task)) {
            return false;
        }
    }

    state_status = atomic_load_explicit(&state->status, memory_order_acquire);
    if (state_status == LLAM_ASYNC_PREEMPT_STATUS_REQUESTED) {
        bool same_request = state->task == task &&
                            state->processor == ticket->processor &&
                            state->worker_incarnation == ticket->worker_incarnation &&
                            state->worker_run_generation == worker_run_generation &&
                            state->task_run_generation == task_run_generation &&
                            state->lease_projection_epoch ==
                                ticket->lease_projection_epoch &&
                            state->p_generation == ticket->p_generation &&
                            state->drain_request == drain &&
                            state->request_owner_state == (unsigned)ticket->state;

        if (same_request) {
            uint64_t last_signal_ns = atomic_load_explicit(
                &state->last_request_signal_ns,
                memory_order_acquire);
            uint64_t retry_expected = last_signal_ns;

            now_ns = llam_now_ns();
            if (now_ns == 0U || now_ns <= last_signal_ns ||
                now_ns - last_signal_ns < LLAM_ASYNC_PREEMPT_RETRY_NS ||
                !atomic_compare_exchange_strong_explicit(
                    &state->last_request_signal_ns,
                    &retry_expected,
                    now_ns,
                    memory_order_acq_rel,
                    memory_order_acquire)) {
                return false;
            }
            *out_request_generation = state->request_generation;
            return true;
        }
        atomic_fetch_add_explicit(&worker->compat_shard->metrics.async_preempt_collisions,
                                  1U,
                                  memory_order_relaxed);
        expected_status = LLAM_ASYNC_PREEMPT_STATUS_REQUESTED;
        if (!atomic_compare_exchange_strong_explicit(&state->status,
                                                     &expected_status,
                                                     LLAM_ASYNC_PREEMPT_STATUS_IDLE,
                                                     memory_order_acq_rel,
                                                     memory_order_acquire)) {
            return false;
        }
        expected_status = LLAM_ASYNC_PREEMPT_STATUS_IDLE;
        state_status = LLAM_ASYNC_PREEMPT_STATUS_IDLE;
    }
    if (state_status != LLAM_ASYNC_PREEMPT_STATUS_IDLE ||
        !atomic_compare_exchange_strong_explicit(&state->status,
                                                 &expected_status,
                                                 LLAM_ASYNC_PREEMPT_STATUS_WRITING,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        atomic_fetch_add_explicit(&worker->compat_shard->metrics.async_preempt_collisions,
                                  1U,
                                  memory_order_relaxed);
        return false;
    }

    request_generation = atomic_fetch_add_explicit(&state->next_request_generation,
                                                    1U,
                                                    memory_order_relaxed) + 1U;
    if (request_generation == 0U) {
        /* Never recycle a capture sequence after wraparound. */
        atomic_store_explicit(&state->disabled, 1U, memory_order_release);
        atomic_store_explicit(&state->status,
                              LLAM_ASYNC_PREEMPT_STATUS_IDLE,
                              memory_order_release);
        return false;
    }
    state->task = task;
    state->processor = (llam_processor_t *)ticket->processor;
    state->worker_incarnation = ticket->worker_incarnation;
    state->worker_run_generation = worker_run_generation;
    state->task_run_generation = task_run_generation;
    state->lease_projection_epoch = ticket->lease_projection_epoch;
    state->p_generation = ticket->p_generation;
    state->request_generation = request_generation;
    state->drain_request = drain;
    state->request_owner_state = (unsigned)ticket->state;
    state->request_run_generation =
        ((llam_async_runtime_control_t *)runtime->async_preempt_control)->run_generation;
    now_ns = llam_now_ns();
    state->request_started_ns = now_ns != 0U ? now_ns : 1U;
    atomic_store_explicit(&state->last_request_signal_ns,
                          now_ns != 0U ? now_ns : 1U,
                          memory_order_release);
    atomic_store_explicit(&state->status,
                          LLAM_ASYNC_PREEMPT_STATUS_REQUESTED,
                          memory_order_release);
    atomic_fetch_add_explicit(&worker->compat_shard->metrics.async_preempt_requests,
                              1U,
                              memory_order_relaxed);
    *out_request_generation = request_generation;
    return true;
}

bool llam_async_preempt_prepare_request(
    llam_worker_t *worker, llam_task_t *task,
    const llam_processor_owner_ticket_t *ticket,
    uint64_t worker_run_generation, uint64_t task_run_generation,
    uint64_t *out_request_generation) {
    bool prepared = llam_async_prepare_request_impl(worker, task, ticket, worker_run_generation,
                                                     task_run_generation, out_request_generation, false);
#if defined(LLAM_ENABLE_TEST_HOOKS)
    if (prepared && atomic_load_explicit(&g_llam_async_test_hold_normal, memory_order_acquire)) {
        /* Publish a real normal descriptor but delay its reserved-signal delivery. */
        return false;
    }
#endif
    return prepared;
}

#if defined(LLAM_ENABLE_TEST_HOOKS)
bool llam_async_preempt_test_drain_guards(llam_worker_t *worker) {
    llam_shard_t *shard = worker->compat_shard;
    llam_task_t *task = g_llam_tls_task;
    llam_async_preempt_state_t *state = llam_async_worker_state(worker);
    llam_processor_owner_ticket_t ticket;
    llam_processor_owner_ticket_t bad;
    llam_task_t *foreign = calloc(1U, sizeof(*foreign));
    sigset_t reserved;
    sigset_t previous;
    uint64_t request = 0U;
    uint64_t m_run = atomic_load_explicit(&worker->run_generation, memory_order_acquire);
    uint64_t g_run = atomic_load_explicit(&task->run_generation, memory_order_acquire);
    bool good = true;
    unsigned i;

    if (foreign == NULL) {
        return false;
    }
    foreign->owner_runtime = NULL;
    sigemptyset(&reserved);
    sigaddset(&reserved, shard->runtime->async_preempt_signal);
    if (pthread_sigmask(SIG_BLOCK, &reserved, &previous) != 0) {
        free(foreign);
        return false;
    }
    pthread_mutex_lock(&shard->opaque_lock);
    pthread_mutex_lock(&shard->lock);
    llam_async_preempt_cancel_request(worker, state->request_generation);
    good = llam_async_service_snapshot_pair_locked(shard, worker, &ticket) &&
           !llam_async_service_snapshot_pair_locked(shard, &shard->opaque_helper_worker, &bad) &&
           !llam_async_service_snapshot_pair_locked(NULL, worker, &bad) &&
           !llam_async_service_snapshot_pair_locked(shard, NULL, &bad);
    if (good) {
        good = llam_async_prepare_request_impl(worker, task, &ticket, m_run, g_run, &request, true);
        llam_async_preempt_cancel_request(worker, request);
        for (i = 0U; i < 5U; ++i) {
            bad = ticket;
            if (i == 0U) bad.worker_incarnation += 1U;
            if (i == 1U) bad.p_generation = 0U;
            if (i == 2U) bad.lease_projection_epoch ^= UINT64_C(1);
            if (i == 3U) bad.owner_m = &shard->opaque_helper_worker;
            if (i == 4U) bad.state = LLAM_PROCESSOR_OWNER_TRANSFERRING;
            if (llam_async_prepare_request_impl(worker, task, &bad, m_run, g_run, &request, true)) {
                good = false;
                llam_async_preempt_cancel_request(worker, request);
            }
        }
        if (llam_async_prepare_request_impl(worker, foreign, &ticket, m_run, 0U, &request, true)) {
            good = false;
            llam_async_preempt_cancel_request(worker, request);
        }
    }
    pthread_mutex_unlock(&shard->lock);
    pthread_mutex_unlock(&shard->opaque_lock);
    if (pthread_sigmask(SIG_SETMASK, &previous, NULL) != 0) {
        good = false;
    }
    free(foreign);
    return good;
}
#endif

uint64_t llam_async_preempt_effective_quantum(const llam_runtime_t *runtime,
                                             llam_task_class_t task_class) {
    if (runtime == NULL) return 0U;
    return runtime->preempt_quantum_ns != 0U ? runtime->preempt_quantum_ns : llam_slice_ns(task_class);
}

static bool llam_async_prepare_drain_locked(llam_shard_t *shard, uint64_t now_ns,
                                            llam_worker_t **request_worker,
                                            pthread_t *native_thread, uint64_t *request_generation) {
    llam_runtime_t *runtime = shard->runtime;
    llam_worker_t *worker = &shard->primary_worker;
    llam_processor_owner_record_t record;
    llam_task_t *task;
    llam_async_task_state_t *storage;
    llam_async_preempt_state_t *state;
    llam_processor_owner_ticket_t ticket;
    uint64_t last;
    uint64_t budget;

    if (runtime->execution_lease_mode == LLAM_EXECUTION_LEASE_REGISTERED) {
        if (!llam_processor_owner_snapshot_pair_locked(&shard->processor, &record)) return false;
        worker = record.owner_m;
    }
    if (!llam_async_preempt_drain_service_needed(runtime) || runtime->async_preempt_signal <= 0 ||
        runtime->async_preempt_signal > 64 ||
        !llam_async_service_snapshot_pair_locked(shard, worker, &ticket)) {
        return false;
    }
    task = llam_worker_load_active_g(worker);
    storage = task != NULL ? task->async_preempt_task : NULL;
    state = llam_async_worker_state(worker);
    if (task == NULL || storage == NULL || state == NULL || state->worker != worker ||
        atomic_load_explicit(&state->frame_probe_status, memory_order_acquire) != 2U ||
        task->owner_runtime != runtime ||
        atomic_load_explicit(&storage->origin_m, memory_order_acquire) != worker ||
        storage->origin_incarnation != ticket.worker_incarnation ||
        storage->run_generation != ((llam_async_runtime_control_t *)runtime->async_preempt_control)->run_generation ||
        !llam_worker_has_safe_managed_g(worker, &shard->processor, task) ||
        (task->flags & LLAM_TASK_FLAG_NO_PREEMPT) != 0U ||
        atomic_load_explicit(&task->opaque_blocking_depth, memory_order_acquire) != 0U) {
        return false;
    }
    last = atomic_load_explicit(&worker->last_safepoint_ns, memory_order_acquire);
    budget = llam_async_preempt_effective_quantum(runtime,
        (llam_task_class_t)atomic_load_explicit(&task->task_class, memory_order_acquire));
    if (last == 0U || now_ns <= last || now_ns - last < budget) {
        return false;
    }
    /* Drain requests use the dispatch budget, not normal pressure suppression. */
    if (atomic_exchange_explicit(&task->preempt_requested, 1U, memory_order_acq_rel) == 0U) {
        atomic_fetch_add_explicit(&shard->metrics.preempt_requests, 1U, memory_order_relaxed);
    }
    if (!llam_async_prepare_request_impl(worker, task, &ticket,
            atomic_load_explicit(&worker->run_generation, memory_order_acquire),
            atomic_load_explicit(&task->run_generation, memory_order_acquire), request_generation, true)) {
        return false;
    }
    *request_worker = worker;
    *native_thread = worker->native_thread;
    return true;
}

void llam_async_preempt_drain_probe(llam_shard_t *shard, uint64_t now_ns) {
    pthread_t native_thread;
    llam_worker_t *request_worker = NULL;
    uint64_t request_generation = 0U;

    if (shard == NULL || !llam_async_preempt_drain_service_needed(shard->runtime)) {
        return;
    }
    /* These locks protect active-G lifetime AND native target withdrawal through send. */
    pthread_mutex_lock(&shard->opaque_lock);
    pthread_mutex_lock(&shard->lock);
    if (llam_async_prepare_drain_locked(shard, now_ns, &request_worker, &native_thread, &request_generation)) {
        int signal_rc = pthread_kill(native_thread, shard->runtime->async_preempt_signal);
        if (signal_rc == 0) {
            atomic_fetch_add_explicit(&shard->metrics.preempt_signals, 1U, memory_order_relaxed);
        } else {
            llam_async_preempt_cancel_request(request_worker, request_generation);
        }
    }
    pthread_mutex_unlock(&shard->lock);
    pthread_mutex_unlock(&shard->opaque_lock);
}

void llam_async_preempt_cancel_request(llam_worker_t *worker,
                                      uint64_t request_generation) {
    llam_async_preempt_state_t *state;
    unsigned expected_status = LLAM_ASYNC_PREEMPT_STATUS_REQUESTED;

    if (worker == NULL || request_generation == 0U) {
        return;
    }
    state = (llam_async_preempt_state_t *)atomic_load_explicit(
        &worker->async_preempt_state, memory_order_acquire);
    if (state != NULL && state->request_generation == request_generation) {
        (void)atomic_compare_exchange_strong_explicit(&state->status,
                                                      &expected_status,
                                                      LLAM_ASYNC_PREEMPT_STATUS_IDLE,
                                                      memory_order_acq_rel,
                                                      memory_order_acquire);
    }
}

bool llam_async_preempt_sample_task_stack(llam_task_t *task) {
    llam_worker_t *worker = g_llam_tls_worker;
    llam_async_preempt_state_t *state;
    unsigned status;

    if (worker == NULL || task == NULL) {
        return false;
    }
    state = (llam_async_preempt_state_t *)atomic_load_explicit(
        &worker->async_preempt_state, memory_order_acquire);
    if (state == NULL) {
        return false;
    }
    status = atomic_load_explicit(&state->status, memory_order_acquire);
    if (status != LLAM_ASYNC_PREEMPT_STATUS_CAPTURED || state->task != task ||
        state->context.rsp == 0U) {
        return false;
    }
    llam_task_sample_stack_rsp(task, (uintptr_t)state->context.rsp);
    return true;
}

int llam_async_preempt_task_init(llam_task_t *task) {
    llam_runtime_t *runtime = task->owner_runtime;
    llam_async_runtime_control_t *control;
    llam_async_preempt_state_t *state;
    llam_async_task_state_t *storage;
    unsigned i;

    if (task->managed_catalog != NULL && task->managed_execution_class == LLAM_MANAGED_EXECUTION_LEGACY)
        return 0;
    if (atomic_load_explicit(&runtime->async_preempt_enabled, memory_order_acquire) == 0U) {
        return 0;
    }
    if (atomic_load_explicit(&runtime->stop_requested, memory_order_acquire) ||
        atomic_load_explicit(&runtime->shutdown_requested, memory_order_acquire)) {
        errno = ECANCELED;
        return -1;
    }
    control = runtime->async_preempt_control;
    state = control != NULL ? control->states[0] : NULL;
    if (state == NULL || control == NULL) {
        errno = ENOTSUP;
        return -1;
    }
    if (llam_async_test_fail_allocation(LLAM_ASYNC_TEST_FAIL_TASK) ||
        llam_async_test_fail_allocation(LLAM_ASYNC_TEST_FAIL_BANK0) ||
        llam_async_test_fail_allocation(LLAM_ASYNC_TEST_FAIL_BANK1)) {
        errno = ENOMEM;
        return -1;
    }
    storage = llam_capsule_pool_take(&control->capsule_pool, g_llam_tls_worker == NULL);
    if (storage == NULL) return -1;
    void *areas[2] = {storage->bank[0].xstate_area, storage->bank[1].xstate_area};
    memset(storage, 0, sizeof(*storage));
    atomic_init(&storage->origin_m, NULL);
    storage->budget_charge = control->capsule_charge;
    for (i = 0U; i < 2U; ++i) {
        storage->bank[i].xstate_area = areas[i];
        atomic_init(&storage->bank[i].phase, LLAM_ASYNC_CAPSULE_FREE);
    }
    atomic_fetch_add_explicit(&control->capsule_bytes, control->capsule_charge, memory_order_release);
    task->async_preempt_task = storage;
    return 0;
}

void llam_async_preempt_task_destroy(llam_task_t *task) {
    llam_async_task_state_t *storage = task->async_preempt_task;
    llam_worker_t *origin;
    llam_async_runtime_control_t *control;
    size_t charge;
    unsigned i;

    if (storage == NULL) {
        return;
    }
    if (storage->ready != NULL || storage->enqueued) {
        abort();
    }
    control = task->owner_runtime->async_preempt_control;
    charge = storage->budget_charge;
    if (control == NULL || charge == 0U) {
        abort();
    }
    for (i = 0U; i < 2U; ++i) {
        if (atomic_load_explicit(&storage->bank[i].phase, memory_order_acquire) !=
            LLAM_ASYNC_CAPSULE_FREE) {
            /* A live continuation is never reclaimed merely because stop was requested. */
            abort();
        }
    }
    origin = atomic_load_explicit(&storage->origin_m, memory_order_acquire);
    if (origin != NULL) {
        llam_async_preempt_state_t *state = llam_async_worker_state(origin);

        if (state == NULL || atomic_load_explicit(&state->pending_restore, memory_order_acquire) != 0U ||
            atomic_load_explicit(&task->state, memory_order_acquire) != LLAM_TASK_STATE_DEAD ||
            !llam_worker_binding_live(origin, storage->origin_incarnation)) {
            abort();
        }
        /* Off-stack terminal G: execution service ends, not its public-handle lifetime. */
        atomic_store_explicit(&storage->origin_m, NULL, memory_order_release);
        if (atomic_fetch_sub_explicit(&state->affine_live_g, 1U, memory_order_acq_rel) == 0U) {
            abort();
        }
    }
    task->async_preempt_task = NULL;
    /* Return active charge only after every native continuation/reader retired. */
    if (atomic_fetch_sub_explicit(&control->capsule_bytes, charge, memory_order_acq_rel) < charge) {
        abort();
    }
    llam_capsule_pool_put(&control->capsule_pool, storage);
}

void llam_async_preempt_save_scheduler_state(void) {
    llam_async_preempt_state_t *state = llam_async_worker_state(g_llam_tls_worker);

    if (state != NULL) {
        __asm__ volatile("fxsave64 (%0)" : : "r"(state->scheduler_fxstate) : "memory");
    }
}

/* Runs only on this M's ordinary scheduler stack, after its handler returned. */
static void llam_async_retire_restore(llam_async_preempt_state_t *state) {
    llam_async_capsule_t *capsule = (llam_async_capsule_t *)atomic_load_explicit(
        &state->pending_restore, memory_order_acquire);
    sigset_t reserved;
    sigset_t previous;
    int saved_errno = errno;

    if (capsule == NULL) {
        return;
    }
    sigemptyset(&reserved);
    sigaddset(&reserved, state->worker->compat_shard->runtime->async_preempt_signal);
    if (pthread_sigmask(SIG_BLOCK, &reserved, &previous) != 0) {
        abort();
    }
    if (atomic_load_explicit(&state->observed_restore_sequence, memory_order_acquire) !=
            capsule->restore_sequence ||
        atomic_load_explicit(&capsule->phase, memory_order_acquire) != LLAM_ASYNC_CAPSULE_OBSERVED) {
        abort();
    }
    /*
     * Each M's capsules have no external native readers. Its signal handler
     * has returned before scheduler entry, so that reader
     * obligation is over. A future debug/scan API must not borrow these bytes
     * without extending this retirement protocol.
     */
    atomic_store_explicit(&state->pending_restore, 0U, memory_order_release);
    atomic_store_explicit(&state->observed_restore_sequence, 0U, memory_order_relaxed);
    atomic_store_explicit(&capsule->phase, LLAM_ASYNC_CAPSULE_FREE,
                          memory_order_release);
    if (pthread_sigmask(SIG_SETMASK, &previous, NULL) != 0) {
        abort();
    }
    errno = saved_errno;
}

void llam_async_preempt_scheduler_return(llam_shard_t *shard, llam_task_t *task) {
    llam_async_preempt_state_t *state = llam_async_worker_state(g_llam_tls_worker);
    llam_async_capsule_t *capsule;
    llam_async_task_state_t *storage;
    llam_processor_owner_ticket_t service;
    uint64_t now_ns;

    if (state == NULL) {
        return;
    }
    llam_async_retire_restore(state);
    if (atomic_load_explicit(&state->status, memory_order_acquire) !=
        LLAM_ASYNC_PREEMPT_STATUS_LANDING) {
        return;
    }
#if defined(LLAM_ENABLE_TEST_HOOKS)
    {
        unsigned expected = 1U;

        if (atomic_compare_exchange_strong_explicit(
                &g_llam_async_test_ready_stop, &expected, 2U,
                memory_order_acq_rel, memory_order_acquire)) {
            /* Exercise stop after active-G withdrawal but before READY publication. */
            llam_request_stop(shard->runtime);
        }
    }
#endif
    capsule = state->captured_capsule;
    storage = task->async_preempt_task;
    if (capsule == NULL || capsule->task != task || storage == NULL ||
        storage->ready != NULL ||
        atomic_load_explicit(&capsule->phase, memory_order_acquire) !=
            LLAM_ASYNC_CAPSULE_CAPTURED) {
        abort();
    }
    now_ns = llam_now_ns();
    if (now_ns >= state->request_started_ns) {
        llam_async_record_max_ack_latency(&shard->metrics,
                                          now_ns - state->request_started_ns);
    }
    task->saved_errno = capsule->saved_errno;
    llam_task_sample_stack_rsp(task, (uintptr_t)capsule->machine.gregs[REG_RSP]);
    storage->ready = capsule;
    atomic_store_explicit(&capsule->phase, LLAM_ASYNC_CAPSULE_READY,
                          memory_order_release);

    pthread_mutex_lock(&shard->opaque_lock);
    pthread_mutex_lock(&shard->lock);
    if (!llam_async_service_snapshot_pair_locked(shard, g_llam_tls_worker, &service)) {
        abort();
    }
    task->state = LLAM_TASK_STATE_RUNNABLE;
    task->wait_reason = LLAM_WAIT_NONE;
    task->last_yield_ns = 0U;
    task->last_runnable_ns = now_ns;
    /* The G is off-stack and its capsule is complete BEFORE queue publication. */
    if (!(shard->runtime->execution_lease_mode == LLAM_EXECUTION_LEASE_REGISTERED
        ? llam_async_registered_enqueue(state, task) : llam_async_preempt_enqueue_locked(shard, task))) {
        pthread_mutex_unlock(&shard->lock);
        abort();
    }
    shard->metrics.preempt_yields += 1U;
    shard->metrics.yields += 1U;
    pthread_mutex_unlock(&shard->lock);
    pthread_mutex_unlock(&shard->opaque_lock);
    state->captured_capsule = NULL;
    atomic_store_explicit(&state->status, LLAM_ASYNC_PREEMPT_STATUS_IDLE,
                          memory_order_release);
}

void llam_async_preempt_trampoline_c(void *opaque_state) {
    llam_async_preempt_state_t *state = opaque_state;
    llam_async_worker_bridge_t *bridge;
    llam_async_capsule_t *capsule;

    if (state == NULL || g_llam_tls_scheduler_ctx == NULL ||
        state->worker != g_llam_tls_worker ||
        atomic_load_explicit(&state->status, memory_order_acquire) !=
            LLAM_ASYNC_PREEMPT_STATUS_CAPTURED) {
        abort();
    }
    bridge = state->bridge_storage;
    capsule = state->captured_capsule;
    if (bridge == NULL || capsule == NULL || capsule->task != g_llam_tls_task) {
        abort();
    }
    atomic_store_explicit(&state->status, LLAM_ASYNC_PREEMPT_STATUS_LANDING,
                          memory_order_release);
    /* Save only disposable M landing state, never the G's cooperative context. */
    llam_ctx_switch(&bridge->landing_context, g_llam_tls_scheduler_ctx);
    abort();
}

/* Entered on an M-owned stack with its scheduler continuation already saved. */
LLAM_NORETURN void llam_async_preempt_restore_c(void *opaque_state) {
    llam_async_preempt_state_t *state = opaque_state;
    llam_async_worker_bridge_t *bridge = state->bridge_storage;
    llam_async_capsule_t *capsule = (llam_async_capsule_t *)atomic_load_explicit(
        &state->pending_restore, memory_order_acquire);
    sigset_t blocked;
    ucontext_t *context = &bridge->restore_frame.context;

    if (capsule == NULL || capsule->task != g_llam_tls_task ||
        capsule->origin_m != g_llam_tls_worker) {
        abort();
    }
    sigemptyset(&blocked);
    sigaddset(&blocked, capsule->task->owner_runtime->async_preempt_signal);
    if (pthread_sigmask(SIG_BLOCK, &blocked, NULL) != 0) {
        abort();
    }
    memset(&bridge->restore_frame, 0, sizeof(bridge->restore_frame));
    context->uc_flags = capsule->uc_flags;
    context->uc_stack = capsule->signal_stack;
    context->uc_mcontext = capsule->machine;
    memcpy(&context->uc_sigmask, &capsule->signal_mask, sizeof(capsule->signal_mask));
    memcpy(bridge->restore_xstate, capsule->xstate_area, capsule->xstate_bytes);
    context->uc_mcontext.fpregs = bridge->restore_xstate;
    errno = capsule->saved_errno;
    /* No READY retry after this point: an ambiguous restore failure is fatal. */
    llam_async_preempt_restore(&bridge->restore_frame);
}

bool llam_async_preempt_resume_task(llam_ctx_t *scheduler_ctx, llam_task_t *task) {
    llam_async_task_state_t *storage = task->async_preempt_task;
    llam_async_preempt_state_t *state;
    llam_async_worker_bridge_t *bridge;
    llam_async_capsule_t *capsule;
    uint64_t *sp;
    unsigned expected = LLAM_ASYNC_CAPSULE_READY;
    sigset_t reserved;
    sigset_t previous;

    if (storage == NULL || storage->ready == NULL) {
        return false;
    }
    capsule = storage->ready;
    state = llam_async_worker_state(g_llam_tls_worker);
    if (task->owner_runtime->execution_lease_mode == LLAM_EXECUTION_LEASE_REGISTERED) {
        llam_processor_owner_ticket_t fresh;
        llam_shard_t *current = g_llam_tls_shard;
        llam_async_worker_bridge_t *native = state != NULL ? state->bridge_storage : NULL;
        if (native == NULL || native->scheduler != scheduler_ctx || current == NULL) abort();
        pthread_mutex_lock(&current->opaque_lock);
        pthread_mutex_lock(&current->lock);
        bool valid = llam_async_service_snapshot_pair_locked(current, g_llam_tls_worker, &fresh);
        pthread_mutex_unlock(&current->lock);
        pthread_mutex_unlock(&current->opaque_lock);
        if (!valid) abort();
    }
    /* Check origin eligibility before consuming the unique resume claim. */
    if (state == NULL || capsule->origin_m != g_llam_tls_worker ||
        atomic_load_explicit(&g_llam_tls_worker->binding_incarnation,
                             memory_order_acquire) != capsule->origin_incarnation ||
        atomic_load_explicit(&state->pending_restore, memory_order_acquire) != 0U ||
        state->next_restore_sequence == UINT64_MAX) {
        abort();
    }
    /* Block before replacing any part of the signal-visible restore record. */
    sigemptyset(&reserved);
    sigaddset(&reserved, task->owner_runtime->async_preempt_signal);
    if (pthread_sigmask(SIG_BLOCK, &reserved, &previous) != 0 ||
        !atomic_compare_exchange_strong_explicit(
            &capsule->phase, &expected, LLAM_ASYNC_CAPSULE_RESTORING,
            memory_order_acq_rel, memory_order_acquire)) {
        abort();
    }
    storage->ready = NULL;
    capsule->restore_sequence = ++state->next_restore_sequence;
    capsule->restore_g_run = atomic_load_explicit(&task->run_generation, memory_order_acquire);
    capsule->restore_m_run = atomic_load_explicit(&g_llam_tls_worker->run_generation,
                                                memory_order_acquire);
    atomic_store_explicit(&state->observed_restore_sequence, 0U, memory_order_relaxed);
    atomic_store_explicit(&state->pending_restore, (uintptr_t)capsule, memory_order_release);

    bridge = state->bridge_storage;
    memset(&bridge->resume_context, 0, sizeof(bridge->resume_context));
    memcpy(&bridge->resume_context.mxcsr, (unsigned char *)state->scheduler_fxstate + 24U,
           sizeof(bridge->resume_context.mxcsr));
    memcpy(&bridge->resume_context.x87_cw, state->scheduler_fxstate,
           sizeof(bridge->resume_context.x87_cw));
    sp = (uint64_t *)state->trampoline_stack_top;
    *--sp = 0U;
    *--sp = (uint64_t)(uintptr_t)llam_async_preempt_resume_bridge;
    bridge->resume_context.rsp = (uint64_t)(uintptr_t)sp;
    bridge->resume_context.r12 = (uint64_t)(uintptr_t)state;
    atomic_fetch_add_explicit(&g_llam_tls_shard->metrics.async_preempt_resumes,
                              1U, memory_order_relaxed);
    llam_ctx_switch(scheduler_ctx, &bridge->resume_context);
    /*
     * The restore bridge cannot return here. This return is a scheduler-entry
     * witness after this G's user execution or a new capture/landing. Direct
     * G-to-G handoffs are disabled. Retirement happens in scheduler_return.
     */
    if (pthread_sigmask(SIG_BLOCK, &reserved, NULL) != 0 ||
        atomic_load_explicit(&state->pending_restore, memory_order_acquire) != (uintptr_t)capsule) {
        abort();
    }
    atomic_store_explicit(&capsule->phase, LLAM_ASYNC_CAPSULE_OBSERVED,
                          memory_order_release);
    atomic_store_explicit(&state->observed_restore_sequence, capsule->restore_sequence,
                          memory_order_release);
    if (pthread_sigmask(SIG_SETMASK, &previous, NULL) != 0) {
        abort();
    }
    return true;
}


int llam_async_preempt_prepare_registered(llam_runtime_t *runtime) {
    if (runtime == NULL || runtime->execution_lease_mode != LLAM_EXECUTION_LEASE_REGISTERED ||
        runtime->managed_catalog == NULL) { errno = EINVAL; return -1; }
    return llam_async_preempt_enable_runtime(runtime);
}

bool llam_async_preempt_memory_info(const llam_runtime_t *runtime,
                                    uint64_t *charged, uint64_t *per_task) {
    const llam_async_runtime_control_t *control = runtime != NULL ? runtime->async_preempt_control : NULL;
    if (control == NULL || charged == NULL || per_task == NULL) return false;
    *charged = atomic_load_explicit(&control->capsule_bytes, memory_order_acquire);
    *per_task = control->capsule_charge;
    return true;
}

static bool llam_async_task_banks_quiet(const llam_async_task_state_t *storage) {
    if (storage == NULL || storage->ready != NULL || storage->enqueued) return false;
    for (unsigned i = 0U; i < 2U; ++i)
        if (atomic_load_explicit(&storage->bank[i].phase, memory_order_acquire) != LLAM_ASYNC_CAPSULE_FREE)
            return false;
    return true;
}

/* Called on M after all of its signal handlers have returned. The terminal G
 * remains rooted even after withdrawing the native binding, through TLS exit. */
static bool llam_linux_terminal_quiet(const llam_async_preempt_state_t *state) {
    const llam_async_worker_bridge_t *bridge = state->bridge_storage;
    const llam_task_t *task = bridge->terminal_task;
    return task != NULL && task->managed_execution_class == LLAM_MANAGED_EXECUTION_NATIVE_AFFINE &&
        atomic_load_explicit(&task->state, memory_order_acquire) == LLAM_TASK_STATE_DEAD &&
        llam_async_task_banks_quiet(task->async_preempt_task) &&
        atomic_load_explicit(&state->affine_live_g, memory_order_acquire) == 1U &&
        atomic_load_explicit(&state->ready_depth, memory_order_acquire) == 0U &&
        atomic_load_explicit(&state->pending_restore, memory_order_acquire) == 0U &&
        state->captured_capsule == NULL &&
        atomic_load_explicit(&state->status, memory_order_acquire) == LLAM_ASYNC_PREEMPT_STATUS_IDLE &&
        atomic_load_explicit(&state->worker->current_p, memory_order_acquire) == NULL &&
        llam_worker_load_active_g(state->worker) == NULL;
}

bool llam_async_preempt_task_quiescent(const llam_task_t *task) {
    llam_worker_t *worker = g_llam_tls_worker;
    llam_shard_t *shard = g_llam_tls_shard;
    llam_async_preempt_state_t *state = llam_async_worker_state(worker);
    llam_async_task_state_t *storage = task != NULL ? task->async_preempt_task : NULL;
    llam_processor_owner_ticket_t ticket;
    sigset_t reserved, previous;
    bool quiet;
    if (task == NULL || g_llam_tls_task != task || state == NULL || storage == NULL || shard == NULL ||
        atomic_load_explicit(&storage->origin_m, memory_order_acquire) != worker) return false;
    sigemptyset(&reserved);
    sigaddset(&reserved, task->owner_runtime->async_preempt_signal);
    if (pthread_sigmask(SIG_BLOCK, &reserved, &previous) != 0) return false;
    pthread_mutex_lock(&shard->opaque_lock);
    pthread_mutex_lock(&shard->lock);
    quiet = llam_async_service_snapshot_pair_locked(shard, worker, &ticket) &&
        llam_async_task_banks_quiet(storage) &&
        atomic_load_explicit(&state->status, memory_order_acquire) == LLAM_ASYNC_PREEMPT_STATUS_IDLE &&
        atomic_load_explicit(&state->affine_live_g, memory_order_acquire) == 1U &&
        atomic_load_explicit(&state->pending_restore, memory_order_acquire) == 0U &&
        atomic_load_explicit(&state->ready_depth, memory_order_acquire) == 0U &&
        state->captured_capsule == NULL &&
        ((llam_async_worker_bridge_t *)state->bridge_storage)->terminal_task == NULL;
    pthread_mutex_unlock(&shard->lock);
    pthread_mutex_unlock(&shard->opaque_lock);
    if (pthread_sigmask(SIG_SETMASK, &previous, NULL) != 0) abort();
    return quiet;
}

static int llam_linux_managed_begin(llam_runtime_t *runtime, void *data) {
    (void)data;
    return llam_async_preempt_run_begin(runtime);
}

static int llam_linux_managed_start(llam_worker_t *worker, llam_ctx_t *scheduler, void *data) {
    llam_async_runtime_control_t *control = data;
    llam_async_preempt_state_t *state = llam_async_registered_state(worker->registry_runtime, worker);
    llam_async_worker_bridge_t *bridge;
    if (state == NULL || !control->registered || g_llam_tls_worker != worker ||
        g_llam_tls_scheduler_ctx != scheduler || g_llam_tls_processor != NULL ||
        atomic_load_explicit(&worker->current_p, memory_order_acquire) != NULL ||
        !llam_async_cet_disabled()) { errno = ENOTSUP; return -1; }
    bridge = state->bridge_storage;
    bridge->scheduler = scheduler;
    bridge->available_isa = LLAM_LLPM2_ISA_BASE | LLAM_LLPM2_ISA_FP_SIMD;
    unsigned eax, ebx, ecx, edx;
    uint64_t native_mask = UINT64_C(3);
    if (!__get_cpuid(1U, &eax, &ebx, &ecx, &edx)) { errno = ENOTSUP; return -1; }
    if ((ecx & (bit_XSAVE | bit_OSXSAVE)) == (bit_XSAVE | bit_OSXSAVE)) {
        unsigned low, high;
        __asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0U));
        native_mask = ((uint64_t)high << 32U) | low;
    }
    if (native_mask != state->xsave_mask) { errno = ENOTSUP; return -1; }
    if ((ecx & bit_AVX) != 0U && (native_mask & UINT64_C(6)) == UINT64_C(6)) {
        bridge->available_isa |= LLAM_LLPM2_ISA_AVX;
        if (__get_cpuid_count(7U, 0U, &eax, &ebx, &ecx, &edx) &&
            (ebx & (1U << 16U)) != 0U && (state->xsave_mask & UINT64_C(0xe6)) == UINT64_C(0xe6))
            bridge->available_isa |= LLAM_LLPM2_ISA_AVX512;
    }
    atomic_store_explicit(&state->status, LLAM_ASYNC_PREEMPT_STATUS_IDLE, memory_order_release);
    atomic_store_explicit(&worker->async_preempt_state, (uintptr_t)state, memory_order_release);
    return llam_async_preempt_worker_ready(worker);
}

static int llam_linux_managed_restart(llam_worker_t *worker, llam_ctx_t *scheduler, void *data) {
    llam_async_preempt_state_t *state = llam_async_registered_state(worker->registry_runtime, worker);
    llam_async_worker_bridge_t *bridge = state != NULL ? state->bridge_storage : NULL;
    if (state == NULL || bridge == NULL || worker->role != LLAM_WORKER_ROLE_NATIVE_AFFINE) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&bridge->ready_lock);
    bool ready = bridge->terminal_task == NULL && state->ready_head == NULL &&
        atomic_load_explicit(&state->affine_live_g, memory_order_acquire) == 0U &&
        atomic_load_explicit(&state->ready_depth, memory_order_acquire) == 0U &&
        atomic_load_explicit(&state->pending_restore, memory_order_acquire) == 0U &&
        state->captured_capsule == NULL &&
        atomic_load_explicit(&state->status, memory_order_acquire) == LLAM_ASYNC_PREEMPT_STATUS_IDLE;
    if (ready) atomic_store_explicit(&state->disabled, 0U, memory_order_release);
    pthread_mutex_unlock(&bridge->ready_lock);
    if (!ready) { errno = EBUSY; return -1; }
    if (llam_linux_managed_start(worker, scheduler, data) != 0) return -1;
    if ((llam_managed_catalog_required_isa(worker->registry_runtime) & ~bridge->available_isa) != 0U ||
        atomic_load_explicit(&worker->registry_runtime->stop_requested, memory_order_acquire) ||
        atomic_load_explicit(&worker->registry_runtime->shutdown_requested, memory_order_acquire)) {
        atomic_store_explicit(&state->disabled, 1U, memory_order_release);
        errno = ENOTSUP;
        return -1;
    }
    return 0;
}

static int llam_linux_managed_commit(llam_runtime_t *runtime, void *data) {
    llam_async_runtime_control_t *control = data;
    uint64_t available = UINT64_MAX;
    if (atomic_load_explicit(&control->prepared_workers, memory_order_acquire) != control->worker_count ||
        atomic_load_explicit(&control->start_phase, memory_order_acquire) != LLAM_ASYNC_START_COLLECTING) {
        errno = EIO; return -1;
    }
    for (unsigned i = 0U; i < control->worker_count; ++i) {
        llam_async_worker_bridge_t *bridge = control->states[i]->bridge_storage;
        available &= bridge->available_isa;
    }
    if (runtime->managed_catalog != NULL && llam_managed_catalog_async_commit(runtime, available) != 0)
        return -1;
    atomic_store_explicit(&control->start_phase, LLAM_ASYNC_START_RUNNING, memory_order_release);
    llam_preempt_capabilities_note_async_result(runtime, true, LLAM_PREEMPT_FAILURE_NONE);
    return 0;
}

static void llam_linux_managed_abort(llam_runtime_t *runtime, int error, void *data) {
    (void)data;
    llam_async_preempt_run_abort(runtime, error, LLAM_PREEMPT_FAILURE_WORKER_START);
}

static void llam_linux_managed_end(llam_runtime_t *runtime, void *data) {
    (void)data;
    llam_async_preempt_run_end(runtime);
}

static void llam_linux_managed_service(llam_worker_t *worker, void *data) {
    (void)data;
    if (g_llam_tls_worker != worker || g_llam_tls_task != NULL ||
        atomic_load_explicit(&worker->current_p, memory_order_acquire) != NULL) abort();
    /* Signal readers execute only on this M. A scheduler entry is their return
     * witness, and returned() retires their bank before relinquishing P. */
}

static bool llam_linux_managed_debt(llam_runtime_t *runtime, void *data) {
    (void)runtime;
    return llam_async_runtime_has_debt(data);
}

static llam_task_t *llam_linux_managed_take(llam_worker_t *worker, llam_processor_t *processor,
                                            bool fallback, void *data) {
    llam_async_preempt_state_t *state = llam_async_worker_state(worker);
    llam_async_worker_bridge_t *bridge = state != NULL ? state->bridge_storage : NULL;
    llam_shard_t *shard = processor->compat_shard;
    llam_processor_owner_ticket_t ticket;
    llam_task_t *task = NULL;
    if (bridge == NULL || atomic_load_explicit(&state->ready_depth, memory_order_acquire) == 0U)
        return NULL;
    if (!fallback && !state->prefer_origin) { state->prefer_origin = true; return NULL; }
    pthread_mutex_lock(&shard->opaque_lock);
    pthread_mutex_lock(&shard->lock);
    if (llam_async_service_snapshot_pair_locked(shard, worker, &ticket)) {
        pthread_mutex_lock(&bridge->ready_lock);
        task = state->ready_head;
        if (task != NULL) {
            llam_async_task_state_t *storage = task->async_preempt_task;
            if (atomic_load_explicit(&task->managed_dispatch_owner, memory_order_acquire) != NULL)
                task = NULL;
            else {
                if (storage == NULL || !storage->enqueued ||
                    atomic_load_explicit(&storage->origin_m, memory_order_acquire) != worker) abort();
                state->ready_head = task->queue_next;
                if (state->ready_head != NULL) state->ready_head->queue_prev = NULL;
                else state->ready_tail = NULL;
                task->queue_next = task->queue_prev = NULL;
                storage->enqueued = false;
                atomic_fetch_sub_explicit(&state->ready_depth, 1U, memory_order_release);
                llam_async_runtime_control_t *control = data;
                if (atomic_fetch_sub_explicit(&control->ready_tasks, 1U, memory_order_release) == 0U) abort();
                state->prefer_origin = false;
            }
        }
        pthread_mutex_unlock(&bridge->ready_lock);
    }
    pthread_mutex_unlock(&shard->lock);
    pthread_mutex_unlock(&shard->opaque_lock);
    return task;
}

static bool llam_linux_managed_accept(llam_worker_t *worker, llam_processor_t *processor,
                                      llam_task_t *task, void *data) {
    llam_async_task_state_t *storage = task->async_preempt_task;
    llam_worker_t *origin = storage != NULL
        ? atomic_load_explicit(&storage->origin_m, memory_order_acquire) : NULL;
    (void)processor; (void)data;
    if (storage == NULL) {
        if (task->managed_catalog != NULL && task->managed_execution_class == LLAM_MANAGED_EXECUTION_LEGACY)
            return true;
        abort();
    }
    if (origin == NULL || origin == worker) return true;
    llam_async_preempt_state_t *state = llam_async_worker_state(origin);
    if (state == NULL) abort();
    (void)llam_async_registered_enqueue(state, task);
    return false;
}

/* Caller owns one scan pin. Publish readiness without allowing a consuming
 * join to free G before our last read; never wait for our own pin in reclaim. */
static void llam_linux_finish_dead(llam_runtime_t *runtime, llam_task_t *task) {
    bool claimed = false;
    llam_task_release_stack(task);
    llam_task_mark_reclaim_ready(task);
    if (atomic_load_explicit(&task->detached, memory_order_acquire) != 0U) {
        unsigned expected = 0U;
        claimed = atomic_compare_exchange_strong_explicit(&task->reclaim_claimed, &expected, 1U,
                                                          memory_order_acq_rel, memory_order_acquire);
    }
    if (!llam_task_scan_ref_release(runtime, task)) abort();
    if (claimed) llam_reclaim_claimed_task(runtime, task);
}

static void llam_linux_managed_returned(llam_worker_t *worker, llam_processor_t *processor,
                                        llam_task_t *task, void *data) {
    llam_async_preempt_state_t *state = llam_async_worker_state(worker);
    (void)data;
    llam_async_preempt_scheduler_return(processor->compat_shard, task);
    /* A cooperative scheduler return closes an unconsumed old dispatch request;
     * it does not manufacture a native restore witness. */
    llam_async_preempt_cancel_request(worker, state->request_generation);
    if (atomic_load_explicit(&task->state, memory_order_acquire) == LLAM_TASK_STATE_DEAD) {
        if (task->managed_execution_class == LLAM_MANAGED_EXECUTION_NATIVE_AFFINE) {
            llam_async_worker_bridge_t *bridge = state->bridge_storage;
            pthread_mutex_lock(&bridge->ready_lock);
            if (bridge->terminal_task != NULL || !llam_task_scan_ref_try_acquire(processor->runtime, task)) abort();
            bridge->terminal_task = task;
            pthread_mutex_unlock(&bridge->ready_lock);
            return;
        }
        if (!llam_task_scan_ref_try_acquire(processor->runtime, task)) abort();
        llam_linux_finish_dead(processor->runtime, task);
    }
}

static bool llam_linux_managed_stop(llam_worker_t *worker, void *data) {
    llam_async_preempt_state_t *state = llam_async_worker_state(worker);
    sigset_t reserved, previous;
    const struct timespec no_wait = {0};
    (void)data;
    if (state == NULL) return true; /* start failed before native publication */
    if (g_llam_tls_worker != worker || g_llam_tls_task != NULL ||
        atomic_load_explicit(&worker->current_p, memory_order_acquire) != NULL ||
        (atomic_load_explicit(&state->affine_live_g, memory_order_acquire) != 0U &&
         !llam_linux_terminal_quiet(state)) ||
        atomic_load_explicit(&state->ready_depth, memory_order_acquire) != 0U ||
        atomic_load_explicit(&state->pending_restore, memory_order_acquire) != 0U ||
        state->captured_capsule != NULL) return false;
    int signo = worker->registry_runtime->async_preempt_signal;
    sigemptyset(&reserved);
    sigaddset(&reserved, signo);
    if (pthread_sigmask(SIG_BLOCK, &reserved, &previous) != 0) return false;
    /* Returning the last P serialized every watchdog sender. With no current P,
     * no new target can be admitted. Withdraw the signal-visible M record before
     * draining pending delivery, while its altstack/native binding remain live. */
    atomic_store_explicit(&worker->async_preempt_state, 0U, memory_order_release);
    int rc;
    do { rc = sigtimedwait(&reserved, NULL, &no_wait); }
    while (rc == signo || (rc < 0 && errno == EINTR));
    if (rc != -1 || errno != EAGAIN) abort();
    atomic_store_explicit(&state->status, LLAM_ASYNC_PREEMPT_STATUS_IDLE, memory_order_release);
    ((llam_async_worker_bridge_t *)state->bridge_storage)->scheduler = NULL;
    return pthread_sigmask(SIG_SETMASK, &previous, NULL) == 0;
}

static bool llam_linux_managed_needs_processor(llam_worker_t *worker, void *data) {
    const llam_async_preempt_state_t *state = llam_async_worker_state(worker);
    (void)data;
    /* Advisory availability only; dispatch still requires a fresh exact P lease. */
    return state != NULL &&
        (atomic_load_explicit(&state->ready_depth, memory_order_acquire) != 0U ||
         atomic_load_explicit(&state->pending_restore, memory_order_acquire) != 0U);
}

static bool llam_linux_managed_can_exit(llam_worker_t *worker, void *data) {
    llam_async_preempt_state_t *state = llam_async_registered_state(worker->registry_runtime, worker);
    (void)data;
    return state != NULL && llam_linux_terminal_quiet(state);
}

static void llam_linux_managed_joined(llam_worker_t *worker, void *data) {
    llam_runtime_t *runtime = worker->registry_runtime;
    llam_async_preempt_state_t *state = llam_async_registered_state(runtime, worker);
    llam_async_worker_bridge_t *bridge = state != NULL ? state->bridge_storage : NULL;
    llam_task_t *task;
    (void)data;
    if (bridge == NULL) return;
    pthread_mutex_lock(&bridge->ready_lock);
    task = bridge->terminal_task;
    if (task != NULL) {
        llam_async_task_state_t *storage = task->async_preempt_task;
        if (g_llam_tls_worker != NULL || atomic_load_explicit(&worker->native_thread_live, memory_order_acquire) ||
            atomic_load_explicit(&worker->async_preempt_state, memory_order_acquire) != 0U ||
            !llam_linux_terminal_quiet(state) ||
            atomic_load_explicit(&storage->origin_m, memory_order_acquire) != worker) abort();
        bridge->terminal_task = NULL;
        atomic_store_explicit(&storage->origin_m, NULL, memory_order_release);
    }
    pthread_mutex_unlock(&bridge->ready_lock);
    if (task == NULL) return;
    llam_linux_finish_dead(runtime, task);
    if (atomic_fetch_sub_explicit(&state->affine_live_g, 1U, memory_order_release) != 1U) abort();
}

static const llam_managed_scheduler_ops_t g_llam_linux_managed_ops = {
    .native_async = true,
    .run_begin = llam_linux_managed_begin,
    .worker_start = llam_linux_managed_start,
    .worker_restart = llam_linux_managed_restart,
    .run_commit = llam_linux_managed_commit,
    .run_abort = llam_linux_managed_abort,
    .run_end = llam_linux_managed_end,
    .service = llam_linux_managed_service,
    .has_obligations = llam_linux_managed_debt,
    .take_ready = llam_linux_managed_take,
    .accept_task = llam_linux_managed_accept,
    .returned = llam_linux_managed_returned,
    .worker_stop = llam_linux_managed_stop,
    .worker_can_exit = llam_linux_managed_can_exit,
    .worker_joined = llam_linux_managed_joined,
    .worker_needs_processor = llam_linux_managed_needs_processor
};

#else

int llam_preempt_module_register(llam_runtime_t *runtime,
                                 uintptr_t image_base,
                                 size_t image_size,
                                 const void *metadata,
                                 size_t metadata_size,
                                 bool required_mode) {
    (void)runtime;
    (void)image_base;
    (void)image_size;
    (void)metadata;
    (void)metadata_size;
    if (required_mode) {
        errno = ENOTSUP;
        return -1;
    }
    return 0;
}

int llam_async_preempt_register_safe_range(llam_runtime_t *runtime,
                                           uintptr_t begin,
                                           uintptr_t end) {
    (void)runtime;
    (void)begin;
    (void)end;
    errno = ENOTSUP;
    return -1;
}

int llam_async_preempt_enable_runtime(llam_runtime_t *runtime) {
    (void)runtime;
    errno = ENOTSUP;
    return -1;
}

void llam_async_preempt_shutdown_runtime(llam_runtime_t *runtime) {
    if (runtime != NULL) {
        free(runtime->async_preempt_ranges);
        runtime->async_preempt_ranges = NULL;
        runtime->async_preempt_range_count = 0U;
        runtime->async_preempt_range_capacity = 0U;
    }
}

int llam_async_preempt_worker_ready(llam_worker_t *worker) {
    (void)worker;
    return 0;
}

bool llam_async_preempt_prepare_request(
    llam_worker_t *worker,
    llam_task_t *task,
    const llam_processor_owner_ticket_t *ticket,
    uint64_t worker_run_generation,
    uint64_t task_run_generation,
    uint64_t *out_request_generation) {
    (void)worker;
    (void)task;
    (void)ticket;
    (void)worker_run_generation;
    (void)task_run_generation;
    (void)out_request_generation;
    return false;
}

void llam_async_preempt_cancel_request(llam_worker_t *worker,
                                      uint64_t request_generation) {
    (void)worker;
    (void)request_generation;
}

bool llam_async_preempt_sample_task_stack(llam_task_t *task) {
    (void)task;
    return false;
}

void llam_async_preempt_trampoline_c(void *opaque_state) {
    (void)opaque_state;
}

#endif
