/**
 * @file src/internal/runtime_state.h
 * @brief Default runtime storage, TLS cursors, and state ownership rules.
 *
 * @details
 * This header exposes the legacy process-default runtime object and scheduler
 * TLS used across implementation files. Explicit runtime handles allocate their
 * own state; no public API should include this file directly.
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
 * See LICENSE.
 */

#ifndef LLAM_RUNTIME_STATE_H
#define LLAM_RUNTIME_STATE_H

#include <assert.h>

#include "runtime_types.h"

// Process-default runtime used by legacy init/spawn/run/shutdown wrappers.
extern llam_runtime_t g_llam_runtime;

// Cached XSAVE/FPU capability bits discovered at runtime initialization.
extern uint32_t g_llam_xsave_mask_lo;
extern uint32_t g_llam_xsave_mask_hi;
extern uint32_t g_llam_fp_control_context;

// Scheduler TLS. These are set only while a runtime worker or managed task runs.
extern _Thread_local llam_shard_t *g_llam_tls_shard;
extern _Thread_local llam_worker_t *g_llam_tls_worker;
extern _Thread_local llam_processor_t *g_llam_tls_processor;
extern _Thread_local llam_task_t *g_llam_tls_task;
extern _Thread_local llam_ctx_t *g_llam_tls_scheduler_ctx;

#include "managed_domain.h"

/* Every stateful public API checks this before TLS access, argument-side pins,
 * allocation or locks. Compiler guard helpers and pure ABI/clock queries are
 * excluded. Internal scheduler code keeps its original TLS and ownership rules. */
#define LLAM_NATIVE_API_GUARD(failure) do { \
    if (LLAM_UNLIKELY(llam_native_domain_blocked())) { \
        errno = ENOTSUP; \
        return failure; \
    } \
} while (0)

/* Implemented by the scheduler lease owner; used by private ticket checks. */
bool llam_worker_binding_live(const llam_worker_t *worker, uint64_t incarnation);
llam_task_t *llam_worker_load_active_g(const llam_worker_t *worker);

/**
 * @brief Return the processor's immutable CPU placement after runtime init.
 *
 * The topology tuple is written once during initialization and stays stable
 * until processor teardown; callers need no scheduler lock.
 */
static inline unsigned llam_processor_cpu_id(const llam_processor_t *processor) {
    return processor != NULL ? processor->cpu_id : 0U;
}

/** @brief Return the processor's immutable scheduler-locality node index. */
static inline unsigned llam_processor_node_index(const llam_processor_t *processor) {
    return processor != NULL ? processor->node_index : 0U;
}

/** @brief Return the processor's immutable logical I/O route index. */
static inline unsigned llam_processor_io_node_index(const llam_processor_t *processor) {
    return processor != NULL ? processor->io_node_index : 0U;
}

/**
 * @brief Acquire-load whether a processor is currently online.
 *
 * Lifecycle and scaling writers publish this flag with release stores.
 */
static inline bool llam_processor_online(const llam_processor_t *processor) {
    return processor != NULL &&
           atomic_load_explicit(&processor->online, memory_order_acquire) != 0U;
}

/** @brief Release-publish a processor online/offline transition. */
static inline void llam_processor_set_online(llam_processor_t *processor, bool online) {
    if (processor != NULL) {
        atomic_store_explicit(&processor->online, online ? 1U : 0U, memory_order_release);
    }
}

/** @brief Claim one actual availability transition; only its winner updates counts. */
/*
 * Update the processor-owned online bit exactly once. Callers serialize this
 * with the shard lock and update runtime->online_shards only when the CAS wins;
 * readers acquiring the bit observe queue admissions committed before the
 * transition. The counter and bit are separate atomics, so an unlocked
 * concurrent diagnostic may briefly see adjacent transition values, while
 * locked commit and settled-state counts remain exact.
 */
static inline bool llam_processor_try_set_online(llam_processor_t *processor, bool online) {
    unsigned expected = online ? 0U : 1U;
    return processor != NULL && atomic_compare_exchange_strong_explicit(
        &processor->online, &expected, online ? 1U : 0U, memory_order_acq_rel, memory_order_acquire);
}

/**
 * @brief Return the ordinary scheduler wake descriptor.
 *
 * Initialization publishes the descriptor before workers start. It remains
 * open across online/offline transitions and is closed only after workers and
 * producers are quiescent during runtime teardown.
 */
static inline int llam_processor_event_fd(const llam_processor_t *processor) {
    return processor != NULL ? processor->event_fd : -1;
}

/**
 * @brief Return the atomic ordinary scheduler wake-pending word for mutation.
 *
 * The returned storage stays at a stable address for the processor lifetime.
 * Claim with acquire/release semantics, publish/reset with release semantics,
 * and use acquire or acq_rel loads/exchanges on the idle-wait path. Linux may
 * pass this same word to its private futex operations.
 */
static inline atomic_uint *llam_processor_event_pending(llam_processor_t *processor) {
    return processor != NULL ? &processor->event_pending : NULL;
}

/** @brief Const pointer form for acquire-only wake-pending observations. */
static inline const atomic_uint *llam_processor_event_pending_const(
    const llam_processor_t *processor) {
    return processor != NULL ? &processor->event_pending : NULL;
}

/**
 * @brief Return the compatibility shard's first P payload lock when initialized.
 *
 * The processor owner projection reuses this existing lock. No second
 * processor/payload mutex is introduced. Callers must not already hold the
 * shard lock; a caller that also holds @c opaque_lock follows the established
 * @c opaque_lock then @c shard->lock order.
 */
static inline pthread_mutex_t *llam_processor_owner_lock(llam_processor_t *processor) {
    llam_shard_t *shard;

    if (processor == NULL || processor->compat_shard == NULL) {
        return NULL;
    }
    shard = processor->compat_shard;
    if (!shard->lock_initialized) {
        return NULL;
    }
    return &shard->lock;
}

/**
 * @brief Copy the P-owner projection while holding its payload lock.
 *
 * The compatibility shard lock is initialized before runtime use and destroyed
 * only after its workers are stopped. This diagnostic snapshot does not
 * validate a task-execution ticket; use the paired ticket accessors for that.
 */
static inline bool llam_processor_owner_snapshot(
    llam_processor_t *processor,
    llam_processor_owner_record_t *snapshot) {
    pthread_mutex_t *owner_lock = llam_processor_owner_lock(processor);
    int unlock_rc;

    if (owner_lock == NULL || snapshot == NULL || pthread_mutex_lock(owner_lock) != 0) {
        return false;
    }
    *snapshot = processor->owner_record;
    unlock_rc = pthread_mutex_unlock(owner_lock);
    assert(unlock_rc == 0);
    return unlock_rc == 0;
}

/**
 * @brief Copy a projection with the caller already holding opaque_lock then lock.
 * This is observation, not an execute ticket or permission to grant ownership.
 */
static inline bool llam_processor_owner_snapshot_pair_locked(
    const llam_processor_t *processor, llam_processor_owner_record_t *snapshot) {
    if (processor == NULL || snapshot == NULL || processor->compat_shard == NULL ||
        !processor->compat_shard->lock_initialized ||
        !processor->compat_shard->opaque_lock_initialized) {
        return false;
    }
    *snapshot = processor->owner_record;
    return true;
}

/* Membership proves identity, not lifetime. Callers retain runtime/M storage. */
static inline bool llam_worker_registered_for(const llam_worker_t *worker,
                                             const llam_runtime_t *runtime) {
    uintptr_t address = (uintptr_t)worker;
    uintptr_t base;
    size_t index;
    size_t slot;

    if (worker == NULL || runtime == NULL || runtime->worker_slots == NULL) {
        return false;
    }
    /* Resolve against runtime-owned storage before dereferencing an owner pointer. */
    base = (uintptr_t)runtime->managed_workers;
    if (base != 0U && address >= base &&
        (address - base) % sizeof(*worker) == 0U &&
        (index = (address - base) / sizeof(*worker)) < runtime->managed_worker_count) {
        slot = (size_t)runtime->active_shards * 2U + index;
    } else {
        base = (uintptr_t)runtime->shards;
        if (base == 0U || address < base ||
            (index = (address - base) / sizeof(*runtime->shards)) >= runtime->active_shards) {
            return false;
        }
        size_t offset = (address - base) % sizeof(*runtime->shards);
        if (offset == offsetof(llam_shard_t, primary_worker)) {
            slot = index * 2U;
        } else if (offset == offsetof(llam_shard_t, opaque_helper_worker)) {
            slot = index * 2U + 1U;
        } else {
            return false;
        }
    }
    return slot < runtime->worker_slot_count && runtime->worker_slots[slot] == worker &&
           worker->registry_runtime == runtime && worker->registry_slot == slot;
}

static inline bool llam_worker_can_own_processor(const llam_worker_t *worker,
                                                const llam_processor_t *processor) {
    const llam_shard_t *shard = processor != NULL ? processor->compat_shard : NULL;

    if (worker == NULL || shard == NULL || processor->runtime == NULL) {
        return false;
    }
    if (processor->runtime->execution_lease_mode == LLAM_EXECUTION_LEASE_REGISTERED) {
        if (!llam_worker_registered_for(worker, processor->runtime) ||
            worker->role < LLAM_WORKER_ROLE_PRIMARY || worker->role > LLAM_WORKER_ROLE_LEGACY_COMPAT)
            return false;
        if (processor->runtime->managed_policy_pools) {
            bool compatibility = shard->id >= processor->runtime->portable_processor_count;
            return compatibility == (worker->role == LLAM_WORKER_ROLE_LEGACY_COMPAT);
        }
        return true;
    }
    if (processor->runtime->execution_lease_mode != LLAM_EXECUTION_LEASE_LEGACY) {
        return false;
    }
    if (worker != &shard->primary_worker && worker != &shard->opaque_helper_worker) {
        return false;
    }
    return worker->compat_shard == shard && worker->associated_processor == processor &&
           ((worker == &shard->primary_worker && worker->role == LLAM_WORKER_ROLE_PRIMARY) ||
            (worker == &shard->opaque_helper_worker &&
             worker->role == LLAM_WORKER_ROLE_OPAQUE_HELPER));
}

/**
 * @brief Match worker-local execution publication to its current P.
 *
 * Legacy workers retain their fixed home-shard identity. Registered workers may
 * borrow any P in their runtime; immutable registry membership is checked before
 * reading current_p. This observation is not an ownership ticket.
 */
static inline bool llam_worker_matches_shard(const llam_worker_t *worker,
                                             const llam_shard_t *shard) {
    if (worker == NULL || shard == NULL) {
        return false;
    }
    if (shard->runtime != NULL &&
        shard->runtime->execution_lease_mode == LLAM_EXECUTION_LEASE_REGISTERED) {
        return llam_worker_registered_for(worker, shard->runtime) &&
               atomic_load_explicit(&worker->current_p, memory_order_acquire) ==
                   &shard->processor;
    }
    return worker->compat_shard == shard;
}

static inline bool llam_execution_lease_state_owned(llam_legacy_lease_state_t state) {
    return state == LLAM_LEASE_PRIMARY_OWNED || state == LLAM_LEASE_HELPER_OWNED ||
           state == LLAM_LEASE_REGISTERED_OWNED;
}

/** @brief The immutable runtime mode must agree with every projected lease state. */
static inline bool llam_execution_lease_state_matches(const llam_runtime_t *runtime,
                                                      llam_legacy_lease_state_t state) {
    if (runtime == NULL) {
        return false;
    }
    switch (runtime->execution_lease_mode) {
        case LLAM_EXECUTION_LEASE_LEGACY:
            return state >= LLAM_LEASE_PRIMARY_OWNED && state <= LLAM_LEASE_STOPPING;
        case LLAM_EXECUTION_LEASE_REGISTERED:
            return state == LLAM_LEASE_REGISTERED_FREE ||
                   state == LLAM_LEASE_REGISTERED_OWNED || state == LLAM_LEASE_STOPPING;
        default:
            return false;
    }
}

/**
 * @brief Return the embedded lease coordinator while its lock is held.
 *
 * The suffix documents the ownership contract: callers must hold
 * @c shard->opaque_lock before dereferencing the returned record.
 */
static inline llam_legacy_lease_t *llam_shard_legacy_lease_locked(llam_shard_t *shard) {
    assert(shard != NULL);
    return shard != NULL ? &shard->legacy_lease : NULL;
}

/** @brief Const form of ::llam_shard_legacy_lease_locked. */
static inline const llam_legacy_lease_t *llam_shard_legacy_lease_const_locked(
    const llam_shard_t *shard) {
    assert(shard != NULL);
    return shard != NULL ? &shard->legacy_lease : NULL;
}

/** @brief Validate the finite lease state set at lock-protected boundaries. */
static inline bool llam_legacy_lease_state_valid(llam_legacy_lease_state_t state) {
    return state >= LLAM_LEASE_PRIMARY_OWNED && state <= LLAM_LEASE_REGISTERED_OWNED;
}

/**
 * @brief Assert lock-protected lease invariants in debug builds.
 *
 * Transfer states intentionally allow a temporary ownerless coordinator; the
 * two stable ownerless states are redirect and stopping.
 */
static inline void llam_legacy_lease_assert_consistent_locked(const llam_shard_t *shard) {
    const llam_legacy_lease_t *lease = llam_shard_legacy_lease_const_locked(shard);

    assert(lease != NULL);
    assert(llam_legacy_lease_state_valid(lease->state));
    if (lease->owner != NULL) {
        assert(lease->owner_incarnation != 0U);
    } else {
        assert(lease->state == LLAM_LEASE_TRANSFER_TO_HELPER ||
               lease->state == LLAM_LEASE_TRANSFER_TO_PRIMARY ||
               lease->state == LLAM_LEASE_REDIRECTED_NO_EXECUTOR ||
               lease->state == LLAM_LEASE_REGISTERED_FREE ||
               lease->state == LLAM_LEASE_STOPPING);
        assert(lease->owner_incarnation == 0U);
    }
    if (lease->return_worker == NULL) {
        assert(lease->return_incarnation == 0U);
    } else {
        assert(lease->return_incarnation != 0U);
    }
}

/**
 * @brief Validate one stored P-owner tuple without consulting the lease.
 *
 * TRANSFERRING is deliberately invalid at accessor boundaries. The composer
 * uses it only while holding both ownership locks and replaces it before
 * releasing either lock.
 */
static inline bool llam_processor_owner_record_well_formed(
    const llam_processor_owner_record_t *record) {
    if (record == NULL || !llam_legacy_lease_state_valid(record->lease_state)) {
        return false;
    }
    switch (record->state) {
        case LLAM_PROCESSOR_OWNER_FREE:
            return record->owner_m == NULL && record->worker_incarnation == 0U;
        case LLAM_PROCESSOR_OWNER_OWNED:
            return record->owner_m != NULL && record->worker_incarnation != 0U;
        case LLAM_PROCESSOR_OWNER_STOPPING:
            return (record->owner_m == NULL && record->worker_incarnation == 0U) ||
                   (record->owner_m != NULL && record->worker_incarnation != 0U);
        case LLAM_PROCESSOR_OWNER_TRANSFERRING:
        default:
            return false;
    }
}

/**
 * @brief Compose the P record from the authoritative lease with both locks held.
 *
 * Caller owns @c shard->opaque_lock followed by @c shard->lock. An offline
 * processor keeps the same single-owner reservation, but ticket validation
 * rejects it until the online flag is true. A STOPPING projection retains the
 * lease owner for legacy drain bookkeeping; the ticket API never exposes it.
 */
static inline bool llam_processor_owner_compose_pair_locked(llam_shard_t *shard) {
    llam_processor_t *processor;
    llam_processor_owner_record_t *current;
    const llam_legacy_lease_t *lease;
    llam_worker_t *desired_owner;
    llam_worker_t *previous_owner;
    uint64_t desired_incarnation;
    llam_processor_owner_state_t desired_state;
    bool current_p_mismatch;
    bool registered;
    bool retiring_owner = false;
    bool changed;

    if (shard == NULL || shard->runtime == NULL || !shard->lock_initialized ||
        !shard->opaque_lock_initialized) {
        return false;
    }
    processor = &shard->processor;
    current = &processor->owner_record;
    lease = llam_shard_legacy_lease_const_locked(shard);
    registered = shard->runtime->execution_lease_mode == LLAM_EXECUTION_LEASE_REGISTERED;
    desired_owner = lease->owner;
    desired_incarnation = lease->owner_incarnation;
    previous_owner = current->owner_m;
    if (!llam_execution_lease_state_matches(shard->runtime, lease->state)) {
        return false;
    }
    if (desired_owner != NULL) {
        llam_processor_t *owner_p;
        if (!llam_worker_can_own_processor(desired_owner, processor) ||
            desired_incarnation == 0U ||
            !llam_worker_binding_live(desired_owner, desired_incarnation)) {
            return false;
        }
        owner_p = atomic_load_explicit(&desired_owner->current_p, memory_order_acquire);
        if (owner_p != NULL && owner_p != processor) {
            return false;
        }
    } else if (desired_incarnation != 0U) {
        return false;
    }
    switch (lease->state) {
        case LLAM_LEASE_PRIMARY_OWNED:
        case LLAM_LEASE_HELPER_OWNED:
        case LLAM_LEASE_TRANSFER_TO_HELPER:
        case LLAM_LEASE_TRANSFER_TO_PRIMARY:
        case LLAM_LEASE_REGISTERED_OWNED:
            if (lease->state == LLAM_LEASE_REGISTERED_OWNED && desired_owner == NULL) {
                return false;
            }
            desired_state = desired_owner != NULL ? LLAM_PROCESSOR_OWNER_OWNED
                                                   : LLAM_PROCESSOR_OWNER_FREE;
            break;
        case LLAM_LEASE_REDIRECTED_NO_EXECUTOR:
        case LLAM_LEASE_REGISTERED_FREE:
            if (desired_owner != NULL) {
                return false;
            }
            desired_state = LLAM_PROCESSOR_OWNER_FREE;
            break;
        case LLAM_LEASE_STOPPING:
            desired_state = LLAM_PROCESSOR_OWNER_STOPPING;
            break;
        default:
            return false;
    }
    current_p_mismatch =
        (desired_owner != NULL &&
         atomic_load_explicit(&desired_owner->current_p, memory_order_acquire) != processor) ||
        (previous_owner != NULL && previous_owner != desired_owner &&
         llam_worker_can_own_processor(previous_owner, processor) &&
         atomic_load_explicit(&previous_owner->current_p, memory_order_acquire) == processor) ||
        (!registered && desired_owner != &shard->primary_worker &&
         atomic_load_explicit(&shard->primary_worker.current_p, memory_order_acquire) == processor) ||
        (!registered && desired_owner != &shard->opaque_helper_worker &&
         atomic_load_explicit(&shard->opaque_helper_worker.current_p, memory_order_acquire) == processor);
    if (!llam_processor_owner_record_well_formed(current)) {
        if (desired_owner != NULL) {
            return false;
        }
        changed = true;
    } else {
        changed = current->owner_m != desired_owner ||
                  current->worker_incarnation != desired_incarnation ||
                  current->lease_projection_epoch != lease->epoch ||
                  current->lease_state != lease->state ||
                  current->state != desired_state || current_p_mismatch;
    }
    if (!changed) {
        return true;
    }
    if (desired_owner != NULL && current->p_generation >= UINT64_MAX - 1U) {
        /* Shutdown/state revoke ordinary tickets even at the last generation.
         * Keep that generation only for the exact same M entering terminal
         * STOPPING, so it can drain before releasing P. This is never a new
         * owner grant; clearing the owner advances to the UINT64_MAX tombstone. */
        retiring_owner = registered && current->p_generation == UINT64_MAX - 1U &&
            desired_state == LLAM_PROCESSOR_OWNER_STOPPING &&
            current->state == LLAM_PROCESSOR_OWNER_OWNED &&
            current->lease_state == LLAM_LEASE_REGISTERED_OWNED &&
            current->owner_m == desired_owner &&
            current->worker_incarnation == desired_incarnation &&
            current->lease_projection_epoch == lease->epoch &&
            atomic_load_explicit(&desired_owner->current_p, memory_order_acquire) == processor &&
            atomic_load_explicit(&shard->runtime->shutdown_requested, memory_order_acquire);
        if (!retiring_owner) {
            return false;
        }
    }
    /* Revoke advisory signal tickets before changing the authoritative projection. */
    llam_worker_t *mirrors[] = {previous_owner, desired_owner,
                               &shard->primary_worker, &shard->opaque_helper_worker};
    for (size_t i = 0U; i < sizeof(mirrors) / sizeof(mirrors[0]); ++i) {
        llam_worker_t *worker = mirrors[i];
        if (!llam_worker_can_own_processor(worker, processor)) {
            continue;
        }
        if (worker != desired_owner &&
            atomic_load_explicit(&worker->current_p, memory_order_acquire) != processor) {
            continue;
        }
        atomic_store_explicit(&worker->async_preempt_ticket_lease_epoch, 0U,
                              memory_order_release);
        atomic_store_explicit(&worker->async_preempt_ticket_generation, 0U,
                              memory_order_release);
        if (worker != desired_owner) {
            atomic_store_explicit(&worker->current_p, NULL, memory_order_release);
        }
    }
    current->state = LLAM_PROCESSOR_OWNER_TRANSFERRING;
    current->owner_m = desired_owner;
    current->worker_incarnation = desired_incarnation;
    current->lease_projection_epoch = lease->epoch;
    current->lease_state = lease->state;
    if (current->p_generation != UINT64_MAX && !retiring_owner) {
        current->p_generation += 1U;
    }
    current->state = desired_state;
    if (desired_owner != NULL) {
        atomic_store_explicit(&desired_owner->async_preempt_ticket_lease_epoch,
                              lease->epoch, memory_order_release);
        atomic_store_explicit(&desired_owner->async_preempt_ticket_generation,
                              current->p_generation, memory_order_release);
        atomic_store_explicit(&desired_owner->current_p, processor, memory_order_release);
    }
    return true;
}

/**
 * @brief Compose the P projection while the caller holds @c opaque_lock.
 *
 * This acquires only @c shard->lock, preserving the established
 * opaque_lock -> shard->lock order. It is for lease writers that do not
 * already own the shard lock.
 */
static inline bool llam_processor_owner_compose_locked(llam_shard_t *shard) {
    bool composed;
    int unlock_rc;

    if (shard == NULL || !shard->opaque_lock_initialized ||
        !shard->lock_initialized || pthread_mutex_lock(&shard->lock) != 0) {
        return false;
    }
    composed = llam_processor_owner_compose_pair_locked(shard);
    unlock_rc = pthread_mutex_unlock(&shard->lock);
    assert(unlock_rc == 0);
    return composed && unlock_rc == 0;
}

/** @brief Compare every field in a caller-supplied P ticket. */
static inline bool llam_processor_owner_ticket_equal(
    const llam_processor_owner_ticket_t *left,
    const llam_processor_owner_ticket_t *right) {
    return left != NULL && right != NULL &&
           left->processor == right->processor && left->owner_m == right->owner_m &&
           left->worker_incarnation == right->worker_incarnation &&
           left->lease_projection_epoch == right->lease_projection_epoch &&
           left->p_generation == right->p_generation &&
           left->lease_state == right->lease_state && left->state == right->state;
}

/**
 * @brief Snapshot an ordinary execution ticket with both ownership locks held.
 *
 * Only a fully composed, online, stable PRIMARY_OWNED or HELPER_OWNED lease
 * yields a ticket. STOPPING remains a lease-only drain path for this phase.
 */
static inline bool llam_processor_owner_ticket_snapshot_pair_locked(
    llam_shard_t *shard,
    llam_processor_owner_ticket_t *ticket_out) {
    const llam_processor_owner_record_t *record;
    const llam_legacy_lease_t *lease;
    const llam_runtime_t *runtime;
    llam_worker_t *lease_owner;
    uint64_t lease_owner_incarnation;

    if (shard == NULL || ticket_out == NULL || !shard->lock_initialized ||
        !shard->opaque_lock_initialized || !llam_processor_online(&shard->processor)) {
        return false;
    }
    record = &shard->processor.owner_record;
    lease = llam_shard_legacy_lease_const_locked(shard);
    runtime = shard->runtime;
    lease_owner = lease->owner;
    lease_owner_incarnation = lease->owner_incarnation;
    if (runtime == NULL ||
        !llam_execution_lease_state_matches(runtime, lease->state) ||
        atomic_load_explicit(&runtime->shutdown_requested, memory_order_acquire) ||
        !llam_processor_owner_record_well_formed(record) ||
        record->state != LLAM_PROCESSOR_OWNER_OWNED || record->p_generation == 0U ||
        record->p_generation == UINT64_MAX ||
        record->owner_m == NULL || record->worker_incarnation == 0U ||
        record->lease_state != lease->state ||
        record->lease_projection_epoch != lease->epoch ||
        record->owner_m != lease_owner ||
        record->worker_incarnation != lease_owner_incarnation ||
        !llam_execution_lease_state_owned(lease->state) ||
        !llam_worker_can_own_processor(record->owner_m, &shard->processor)) {
        return false;
    }
    if (atomic_load_explicit(&record->owner_m->current_p,
                             memory_order_acquire) != &shard->processor) {
        return false;
    }
    if (!llam_worker_binding_live(record->owner_m,
                                  record->worker_incarnation)) {
        return false;
    }

    ticket_out->processor = &shard->processor;
    ticket_out->owner_m = record->owner_m;
    ticket_out->worker_incarnation = record->worker_incarnation;
    ticket_out->lease_projection_epoch = record->lease_projection_epoch;
    ticket_out->p_generation = record->p_generation;
    ticket_out->lease_state = record->lease_state;
    ticket_out->state = record->state;
    return true;
}

/** @brief Return whether this TLS M currently owns a processor-local fast path. */
static inline bool llam_processor_tls_owns_resources(
    const llam_processor_t *processor) {
    llam_shard_t *shard = processor != NULL ? processor->compat_shard : NULL;
    llam_worker_t *worker = g_llam_tls_worker;
    unsigned state;

    if (shard == NULL || processor != &shard->processor || worker == NULL ||
        g_llam_tls_shard != shard || g_llam_tls_processor != processor ||
        !llam_worker_can_own_processor(worker, processor) ||
        atomic_load_explicit(&worker->current_p, memory_order_acquire) != processor ||
        atomic_load_explicit(&worker->runtime_critical_depth, memory_order_acquire) != 0U ||
        atomic_load_explicit(&worker->preempt_disable_depth, memory_order_acquire) != 0U) {
        return false;
    }
    state = atomic_load_explicit(&worker->execution_state, memory_order_acquire);
    return state == LLAM_WORKER_EXEC_SCHEDULER ||
           state == LLAM_WORKER_EXEC_MANAGED_SAFE;
}

/** @brief Add one nested runtime-critical level without allowing counter wrap. */
static inline bool llam_worker_runtime_critical_enter(llam_worker_t *worker) {
    unsigned depth;

    if (worker == NULL) {
        return false;
    }
    depth = atomic_load_explicit(&worker->runtime_critical_depth,
                                 memory_order_acquire);
    for (;;) {
        if (depth == UINT_MAX ||
            !atomic_compare_exchange_weak_explicit(&worker->runtime_critical_depth,
                                                   &depth,
                                                   depth + 1U,
                                                   memory_order_acq_rel,
                                                   memory_order_acquire)) {
            if (depth == UINT_MAX) {
                return false;
            }
            continue;
        }
        atomic_store_explicit(&worker->execution_state,
                              LLAM_WORKER_EXEC_RUNTIME_UNSAFE,
                              memory_order_release);
        return true;
    }
}

/** @brief Remove one nested runtime-critical level and detect underflow. */
static inline bool llam_worker_runtime_critical_leave(llam_worker_t *worker) {
    unsigned depth;

    if (worker == NULL) {
        return false;
    }
    depth = atomic_load_explicit(&worker->runtime_critical_depth,
                                 memory_order_acquire);
    for (;;) {
        if (depth == 0U) {
            return false;
        }
        if (atomic_compare_exchange_weak_explicit(&worker->runtime_critical_depth,
                                                  &depth,
                                                  depth - 1U,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            return true;
        }
    }
}

/** @brief Add one nested preemption-disable level without allowing wrap. */
static inline bool llam_worker_preempt_disable(llam_worker_t *worker) {
    unsigned depth;

    if (worker == NULL) {
        return false;
    }
    depth = atomic_load_explicit(&worker->preempt_disable_depth,
                                 memory_order_acquire);
    for (;;) {
        if (depth == UINT_MAX ||
            !atomic_compare_exchange_weak_explicit(&worker->preempt_disable_depth,
                                                   &depth,
                                                   depth + 1U,
                                                   memory_order_acq_rel,
                                                   memory_order_acquire)) {
            if (depth == UINT_MAX) {
                return false;
            }
            continue;
        }
        atomic_store_explicit(&worker->execution_state,
                              LLAM_WORKER_EXEC_RUNTIME_UNSAFE,
                              memory_order_release);
        return true;
    }
}

/** @brief Remove one nested preemption-disable level and detect underflow. */
static inline bool llam_worker_preempt_enable(llam_worker_t *worker) {
    unsigned depth;

    if (worker == NULL) {
        return false;
    }
    depth = atomic_load_explicit(&worker->preempt_disable_depth,
                                 memory_order_acquire);
    for (;;) {
        if (depth == 0U) {
            return false;
        }
        if (atomic_compare_exchange_weak_explicit(&worker->preempt_disable_depth,
                                                  &depth,
                                                  depth - 1U,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            return true;
        }
    }
}

/**
 * @brief Take a safe-state/P-ticket snapshot for one relief or foreign handoff.
 *
 * The function never transfers execution rights. It reports why an unsafe,
 * unknown, stale, or unowned M must defer; the caller must still perform the
 * ordered lease transaction before releasing P.
 */
static inline bool llam_processor_relief_snapshot(
    llam_shard_t *shard,
    llam_worker_t *worker,
    llam_task_t *task,
    llam_processor_owner_ticket_t *ticket_out,
    llam_relief_defer_reason_t *reason_out) {
    llam_relief_defer_reason_t reason = LLAM_RELIEF_DEFER_STALE_P_TICKET;
    llam_processor_owner_ticket_t ticket = {0};
    unsigned exec_state;
    bool eligible = false;

    if (reason_out != NULL) {
        *reason_out = LLAM_RELIEF_DEFER_STALE_P_TICKET;
    }
    if (shard == NULL || worker == NULL || task == NULL || ticket_out == NULL ||
        !shard->opaque_lock_initialized || !shard->lock_initialized ||
        pthread_mutex_lock(&shard->opaque_lock) != 0) {
        if (reason_out != NULL && task == NULL) {
            *reason_out = LLAM_RELIEF_DEFER_NO_TASK;
        }
        return false;
    }
    if (pthread_mutex_lock(&shard->lock) != 0) {
        (void)pthread_mutex_unlock(&shard->opaque_lock);
        return false;
    }

    exec_state = atomic_load_explicit(&worker->execution_state,
                                      memory_order_acquire);
    if (exec_state > LLAM_WORKER_EXEC_STOPPING) {
        reason = LLAM_RELIEF_DEFER_UNKNOWN_STATE;
    } else if (atomic_load_explicit(&worker->preempt_disable_depth,
                                    memory_order_acquire) != 0U) {
        reason = LLAM_RELIEF_DEFER_PREEMPT_DISABLED;
    } else if (atomic_load_explicit(&worker->runtime_critical_depth,
                                    memory_order_acquire) != 0U) {
        reason = LLAM_RELIEF_DEFER_RUNTIME_CRITICAL;
    } else if (exec_state == LLAM_WORKER_EXEC_RUNTIME_UNSAFE ||
               exec_state == LLAM_WORKER_EXEC_FOREIGN ||
               exec_state == LLAM_WORKER_EXEC_STOPPING ||
               exec_state == LLAM_WORKER_EXEC_IDLE) {
        reason = LLAM_RELIEF_DEFER_UNSAFE_STATE;
    } else if (exec_state != LLAM_WORKER_EXEC_MANAGED_SAFE ||
               g_llam_tls_shard != shard || g_llam_tls_worker != worker ||
               g_llam_tls_processor != &shard->processor || g_llam_tls_task != task ||
               !llam_worker_can_own_processor(worker, &shard->processor) ||
               atomic_load_explicit(&worker->current_p, memory_order_acquire) !=
                   &shard->processor ||
               task->owner_runtime != shard->runtime ||
               atomic_load_explicit(&task->state, memory_order_acquire) !=
                   LLAM_TASK_STATE_RUNNING ||
               atomic_load_explicit(&task->run_generation,
                                    memory_order_acquire) == 0U ||
               atomic_load_explicit(&task->run_generation,
                                    memory_order_acquire) == UINT64_MAX ||
               atomic_load_explicit(&worker->run_generation,
                                    memory_order_acquire) == 0U ||
               atomic_load_explicit(&worker->run_generation,
                                    memory_order_acquire) == UINT64_MAX ||
               llam_worker_load_active_g(worker) != task ||
               worker->foreign_token.state != LLAM_FOREIGN_TOKEN_INACTIVE ||
               !llam_processor_owner_ticket_snapshot_pair_locked(shard, &ticket) ||
               ticket.owner_m != worker) {
        reason = LLAM_RELIEF_DEFER_STALE_P_TICKET;
    } else {
        *ticket_out = ticket;
        reason = LLAM_RELIEF_DEFER_NONE;
        eligible = true;
    }

    (void)pthread_mutex_unlock(&shard->lock);
    (void)pthread_mutex_unlock(&shard->opaque_lock);
    if (reason_out != NULL) {
        *reason_out = reason;
    }
    return eligible;
}

/** @brief Check a worker/G/P tuple before cooperative preemption or relief. */
static inline bool llam_worker_has_safe_managed_g(
    const llam_worker_t *worker,
    const llam_processor_t *processor,
    const llam_task_t *task) {
    return worker != NULL && processor != NULL && task != NULL &&
           llam_worker_can_own_processor(worker, processor) &&
           atomic_load_explicit(&worker->current_p, memory_order_acquire) == processor &&
           atomic_load_explicit(&worker->execution_state, memory_order_acquire) ==
               LLAM_WORKER_EXEC_MANAGED_SAFE &&
           atomic_load_explicit(&worker->runtime_critical_depth,
                                memory_order_acquire) == 0U &&
           atomic_load_explicit(&worker->preempt_disable_depth,
                                memory_order_acquire) == 0U &&
           atomic_load_explicit(&worker->native_domain_depth, memory_order_acquire) == 0U &&
           llam_worker_load_active_g(worker) == task &&
           atomic_load_explicit(&task->state, memory_order_acquire) ==
               LLAM_TASK_STATE_RUNNING;
}

/** @brief Validate the exact current ticket with both ownership locks held. */
static inline bool llam_processor_owner_ticket_validate_pair_locked(
    llam_shard_t *shard,
    const llam_processor_owner_ticket_t *ticket) {
    llam_processor_owner_ticket_t current = {0};

    return ticket != NULL &&
           llam_processor_owner_ticket_snapshot_pair_locked(shard, &current) &&
           llam_processor_owner_ticket_equal(ticket, &current);
}

/** @brief Copy one scheduler-thread P queue capability while both locks are held. */
static inline bool llam_processor_queue_access_snapshot_pair_locked(
    llam_shard_t *shard,
    llam_processor_queue_access_t *access_out) {
    llam_processor_owner_ticket_t ticket = {0};
    const llam_processor_owner_record_t *record;
    const llam_legacy_lease_t *lease;
    const llam_runtime_t *runtime;
    llam_worker_t *actor;
    uint64_t actor_incarnation;

    if (shard == NULL || access_out == NULL || !shard->lock_initialized ||
        !shard->opaque_lock_initialized || !llam_processor_online(&shard->processor) ||
        g_llam_tls_shard != shard || g_llam_tls_processor != &shard->processor) {
        return false;
    }
    actor = g_llam_tls_worker;
    record = &shard->processor.owner_record;
    lease = llam_shard_legacy_lease_const_locked(shard);
    runtime = shard->runtime;
    /* The execute ticket validates registry membership, the composed lease/P
     * projection and its live binding before actor is dereferenced. The same
     * owner cannot rebind while these ownership locks remain held. */
    if (llam_processor_owner_ticket_snapshot_pair_locked(shard, &ticket) &&
        ticket.owner_m == actor) {
        access_out->processor = &shard->processor;
        access_out->actor_m = actor;
        access_out->actor_incarnation = ticket.worker_incarnation;
        access_out->projected_owner_m = ticket.owner_m;
        access_out->projected_worker_incarnation = ticket.worker_incarnation;
        access_out->lease_projection_epoch = ticket.lease_projection_epoch;
        access_out->p_generation = ticket.p_generation;
        access_out->lease_state = ticket.lease_state;
        access_out->owner_state = ticket.state;
        access_out->mode = LLAM_PROCESSOR_QUEUE_ACCESS_EXECUTION;
        return true;
    }

    if (!llam_worker_can_own_processor(actor, &shard->processor)) return false;
    actor_incarnation = atomic_load_explicit(&actor->binding_incarnation,
                                              memory_order_acquire);
    if (!llam_worker_binding_live(actor, actor_incarnation) ||
        runtime == NULL || !llam_execution_lease_state_matches(runtime, lease->state) ||
        !llam_processor_owner_record_well_formed(record) ||
        record->p_generation == 0U || record->p_generation == UINT64_MAX) {
        return false;
    }

    if (atomic_load_explicit(&runtime->shutdown_requested, memory_order_acquire) &&
        lease->state == LLAM_LEASE_STOPPING && record->state == LLAM_PROCESSOR_OWNER_STOPPING &&
        record->owner_m == actor && record->worker_incarnation == actor_incarnation &&
        record->lease_state == LLAM_LEASE_STOPPING &&
        record->lease_projection_epoch == lease->epoch && lease->owner == actor &&
        lease->owner_incarnation == actor_incarnation) {
        access_out->processor = &shard->processor;
        access_out->actor_m = actor;
        access_out->actor_incarnation = actor_incarnation;
        access_out->projected_owner_m = record->owner_m;
        access_out->projected_worker_incarnation = record->worker_incarnation;
        access_out->lease_projection_epoch = record->lease_projection_epoch;
        access_out->p_generation = record->p_generation;
        access_out->lease_state = record->lease_state;
        access_out->owner_state = record->state;
        access_out->mode = LLAM_PROCESSOR_QUEUE_ACCESS_STOPPING_DRAIN;
        return true;
    }

    if (!atomic_load_explicit(&runtime->shutdown_requested, memory_order_acquire) &&
        lease->state == LLAM_LEASE_REDIRECTED_NO_EXECUTOR && lease->owner == NULL &&
        lease->owner_incarnation == 0U && lease->return_worker == actor &&
        lease->return_incarnation == actor_incarnation &&
        lease->transfer_sequence != 0U && record->state == LLAM_PROCESSOR_OWNER_FREE &&
        record->owner_m == NULL && record->worker_incarnation == 0U &&
        record->lease_state == LLAM_LEASE_REDIRECTED_NO_EXECUTOR &&
        record->lease_projection_epoch == lease->epoch) {
        access_out->processor = &shard->processor;
        access_out->actor_m = actor;
        access_out->actor_incarnation = actor_incarnation;
        access_out->projected_owner_m = NULL;
        access_out->projected_worker_incarnation = 0U;
        access_out->lease_projection_epoch = record->lease_projection_epoch;
        access_out->p_generation = record->p_generation;
        access_out->lease_state = record->lease_state;
        access_out->owner_state = record->state;
        access_out->mode = LLAM_PROCESSOR_QUEUE_ACCESS_REDIRECT_RELEASE;
        return true;
    }
    return false;
}

/**
 * @brief Snapshot queue authority and return with the shard queue lock held.
 *
 * Acquire opaque_lock before the queue lock to read the composed lease. Once
 * the queue lock is held, releasing opaque_lock cannot change the P projection:
 * lease writers need the same queue lock to publish it. Callers must release
 * shard->lock and must not reacquire opaque_lock while retaining that lock.
 */
static inline bool llam_processor_queue_access_lock(
    llam_processor_t *processor,
    llam_processor_queue_access_t *access_out) {
    llam_shard_t *shard = processor != NULL ? processor->compat_shard : NULL;
    bool valid;
    int opaque_unlock_rc;

    if (shard == NULL || processor != &shard->processor || access_out == NULL ||
        !shard->opaque_lock_initialized || !shard->lock_initialized ||
        pthread_mutex_lock(&shard->opaque_lock) != 0) {
        return false;
    }
    if (pthread_mutex_lock(&shard->lock) != 0) {
        (void)pthread_mutex_unlock(&shard->opaque_lock);
        return false;
    }
    valid = llam_processor_queue_access_snapshot_pair_locked(shard, access_out);
    opaque_unlock_rc = pthread_mutex_unlock(&shard->opaque_lock);
    assert(opaque_unlock_rc == 0);
    if (!valid || opaque_unlock_rc != 0) {
        int shard_unlock_rc = pthread_mutex_unlock(&shard->lock);
        assert(shard_unlock_rc == 0);
        (void)shard_unlock_rc;
        return false;
    }
    return true;
}

/** @brief Acquire an execute, STOPPING-drain, or redirect-release P queue access. */
static inline bool llam_processor_queue_access_snapshot(
    llam_processor_t *processor,
    llam_processor_queue_access_t *access_out) {
    if (!llam_processor_queue_access_lock(processor, access_out)) return false;
    int rc = pthread_mutex_unlock(&processor->compat_shard->lock);
    assert(rc == 0);
    return rc == 0;
}

/**
 * @brief Validate a queue access against the stable P projection.
 *
 * Caller holds `shard->lock`. Lease mutations require `opaque_lock` first and
 * cannot publish a new P projection until this lock is released.
 */
static inline bool llam_processor_queue_access_validate_locked(
    const llam_shard_t *shard,
    const llam_processor_queue_access_t *access) {
    const llam_processor_owner_record_t *record;
    const llam_runtime_t *runtime;
    bool allowed_state;

    if (shard == NULL || access == NULL || access->processor != &shard->processor ||
        !shard->lock_initialized || g_llam_tls_shard != shard ||
        g_llam_tls_processor != &shard->processor || g_llam_tls_worker != access->actor_m ||
        !llam_processor_online(&shard->processor)) {
        return false;
    }
    if (!llam_worker_can_own_processor(access->actor_m, &shard->processor) ||
        !llam_worker_binding_live(access->actor_m, access->actor_incarnation)) {
        return false;
    }
    runtime = shard->runtime;
    if (runtime == NULL || !llam_execution_lease_state_matches(runtime, access->lease_state)) {
        return false;
    }
    if (access->mode == LLAM_PROCESSOR_QUEUE_ACCESS_EXECUTION) {
        allowed_state = !atomic_load_explicit(&runtime->shutdown_requested,
                                              memory_order_acquire) &&
                        llam_execution_lease_state_owned(access->lease_state) &&
                        access->owner_state == LLAM_PROCESSOR_OWNER_OWNED &&
                        access->projected_owner_m == access->actor_m &&
                        access->projected_worker_incarnation == access->actor_incarnation;
    } else if (access->mode == LLAM_PROCESSOR_QUEUE_ACCESS_STOPPING_DRAIN) {
        allowed_state = atomic_load_explicit(&runtime->shutdown_requested,
                                             memory_order_acquire) &&
                        access->lease_state == LLAM_LEASE_STOPPING &&
                        access->owner_state == LLAM_PROCESSOR_OWNER_STOPPING &&
                        access->projected_owner_m == access->actor_m &&
                        access->projected_worker_incarnation == access->actor_incarnation;
    } else if (access->mode == LLAM_PROCESSOR_QUEUE_ACCESS_REDIRECT_RELEASE) {
        allowed_state = !atomic_load_explicit(&runtime->shutdown_requested,
                                              memory_order_acquire) &&
                        access->lease_state == LLAM_LEASE_REDIRECTED_NO_EXECUTOR &&
                        access->owner_state == LLAM_PROCESSOR_OWNER_FREE &&
                        access->projected_owner_m == NULL &&
                        access->projected_worker_incarnation == 0U;
    } else {
        return false;
    }
    if (!allowed_state || access->p_generation == 0U ||
        access->p_generation == UINT64_MAX) {
        return false;
    }
    record = &shard->processor.owner_record;
    return llam_processor_owner_record_well_formed(record) &&
           record->owner_m == access->projected_owner_m &&
           record->worker_incarnation == access->projected_worker_incarnation &&
           record->lease_projection_epoch == access->lease_projection_epoch &&
           record->p_generation == access->p_generation &&
           record->lease_state == access->lease_state &&
           record->state == access->owner_state;
}

/** @brief Check access that may publish work to the P owner's hot/normal lanes. */
static inline bool llam_processor_queue_access_validate_owner_mutation_locked(
    const llam_shard_t *shard,
    const llam_processor_queue_access_t *access) {
    return llam_processor_queue_access_validate_locked(shard, access) &&
           (access->mode == LLAM_PROCESSOR_QUEUE_ACCESS_EXECUTION ||
            access->mode == LLAM_PROCESSOR_QUEUE_ACCESS_STOPPING_DRAIN);
}

/** @brief Acquire both locks in order and snapshot the current P execute ticket. */
static inline bool llam_processor_owner_ticket_snapshot(
    llam_processor_t *processor,
    llam_processor_owner_ticket_t *ticket_out) {
    llam_shard_t *shard = processor != NULL ? processor->compat_shard : NULL;
    bool valid;
    int shard_unlock_rc;
    int opaque_unlock_rc;

    if (shard == NULL || ticket_out == NULL || !shard->opaque_lock_initialized ||
        !shard->lock_initialized || pthread_mutex_lock(&shard->opaque_lock) != 0) {
        return false;
    }
    if (pthread_mutex_lock(&shard->lock) != 0) {
        (void)pthread_mutex_unlock(&shard->opaque_lock);
        return false;
    }
    valid = processor == &shard->processor &&
            llam_processor_owner_ticket_snapshot_pair_locked(shard, ticket_out);
    shard_unlock_rc = pthread_mutex_unlock(&shard->lock);
    opaque_unlock_rc = pthread_mutex_unlock(&shard->opaque_lock);
    assert(shard_unlock_rc == 0 && opaque_unlock_rc == 0);
    return valid && shard_unlock_rc == 0 && opaque_unlock_rc == 0;
}

/** @brief Acquire both locks in order and validate one exact P execute ticket. */
static inline bool llam_processor_owner_ticket_validate(
    llam_processor_t *processor,
    const llam_processor_owner_ticket_t *ticket) {
    llam_shard_t *shard = processor != NULL ? processor->compat_shard : NULL;
    bool valid;
    int shard_unlock_rc;
    int opaque_unlock_rc;

    if (shard == NULL || ticket == NULL || !shard->opaque_lock_initialized ||
        !shard->lock_initialized || pthread_mutex_lock(&shard->opaque_lock) != 0) {
        return false;
    }
    if (pthread_mutex_lock(&shard->lock) != 0) {
        (void)pthread_mutex_unlock(&shard->opaque_lock);
        return false;
    }
    valid = processor == &shard->processor &&
            llam_processor_owner_ticket_validate_pair_locked(shard, ticket);
    shard_unlock_rc = pthread_mutex_unlock(&shard->lock);
    opaque_unlock_rc = pthread_mutex_unlock(&shard->opaque_lock);
    assert(shard_unlock_rc == 0 && opaque_unlock_rc == 0);
    return valid && shard_unlock_rc == 0 && opaque_unlock_rc == 0;
}

/**
 * @brief Match the current worker/P TLS tuple to one descriptive worker role.
 *
 * This is a legacy-role observation only. Lease ownership is validated by the
 * shard coordinator and current-task publication by worker active-G/current-P;
 * the processor cell owns its queues, timers, allocator, and local stack cache.
 */
static inline bool llam_tls_matches_worker_role(const llam_shard_t *shard,
                                                const llam_worker_t *worker,
                                                llam_worker_role_t role) {
    return shard != NULL && worker != NULL &&
           g_llam_tls_shard == shard &&
           g_llam_tls_worker == worker &&
           g_llam_tls_processor == &shard->processor &&
           shard->processor.runtime == shard->runtime &&
           shard->processor.compat_shard == shard &&
           worker->compat_shard == shard &&
           worker->associated_processor == &shard->processor &&
           worker->role == role;
}

/**
 * @brief Return whether the current TLS tuple describes a shard's primary.
 *
 * The assertion-only check keeps this new observation exactly equivalent to
 * the legacy scheduler-context address while allowing production role checks
 * to use the explicit worker identity tuple.
 */
static inline bool llam_tls_is_primary_scheduler_for(const llam_shard_t *shard) {
    bool matched = llam_tls_matches_worker_role(
        shard,
        shard != NULL ? &shard->primary_worker : NULL,
        LLAM_WORKER_ROLE_PRIMARY);

#ifndef NDEBUG
    if (shard != NULL) {
        bool legacy = g_llam_tls_scheduler_ctx == &shard->scheduler_ctx;

        if (matched || legacy) {
            assert(matched == legacy);
        }
    }
#endif
    return matched;
}

/**
 * @brief Return whether the current TLS tuple describes a shard's opaque helper.
 *
 * The assertion-only check keeps this new observation exactly equivalent to
 * the legacy opaque-helper scheduler-context address.
 */
static inline bool llam_tls_is_opaque_helper_for(const llam_shard_t *shard) {
    bool matched = llam_tls_matches_worker_role(
        shard,
        shard != NULL ? &shard->opaque_helper_worker : NULL,
        LLAM_WORKER_ROLE_OPAQUE_HELPER);

#ifndef NDEBUG
    if (shard != NULL) {
        bool legacy = g_llam_tls_scheduler_ctx == &shard->opaque_scheduler_ctx;

        if (matched || legacy) {
            assert(matched == legacy);
        }
    }
#endif
    return matched;
}

/**
 * @brief Return whether the current TLS tuple may use a legacy direct handoff.
 *
 * Direct handoff retains the legacy primary-context condition; this predicate
 * does not grant any ownership or execution authority beyond that observation.
 */
static inline bool llam_tls_has_legacy_direct_handoff_context(const llam_shard_t *shard) {
    return shard != NULL && shard->runtime != NULL &&
           shard->runtime->execution_lease_mode == LLAM_EXECUTION_LEASE_LEGACY &&
           llam_tls_is_primary_scheduler_for(shard);
}

/**
 * @brief Return the current managed runtime without touching default runtime state.
 *
 * @details
 * This helper is for hot paths that already require a managed LLAM task. The
 * full llam_runtime_current_owner() helper still owns host-thread fallback
 * semantics; this inline form only avoids an out-of-line call when TLS is
 * already installed by the scheduler.
 */
static inline llam_runtime_t *llam_runtime_tls_owner_fast(void) {
    llam_shard_t *shard = g_llam_tls_shard;
    llam_task_t *task;

    if (LLAM_LIKELY(shard != NULL && shard->runtime != NULL)) {
        return shard->runtime;
    }
    task = g_llam_tls_task;
    if (LLAM_UNLIKELY(task != NULL && task->owner_runtime != NULL)) {
        return task->owner_runtime;
    }
    return NULL;
}

// Recursion/fast-path hints used by channel handoff and opaque-block redirect.
extern _Thread_local unsigned g_llam_tls_io_handoff_yield;
extern _Thread_local unsigned g_llam_tls_opaque_redirect_hint;

/**
 * @brief Encoded completion user-data tags for backend I/O events.
 *
 * The low bits of an encoded user-data value identify which runtime object the
 * backend completion pointer refers to.
 */
enum {
    LLAM_IO_UDATA_REQ = 0U,
    LLAM_IO_UDATA_POLL_WATCH = 1U,
    LLAM_IO_UDATA_ACCEPT_WATCH = 2U,
    LLAM_IO_UDATA_RECV_WATCH = 3U,
    LLAM_IO_UDATA_CONTROL = 4U,
};

#endif
