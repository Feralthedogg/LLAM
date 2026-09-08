/**
 * @file src/core/sched/scheduler.c
 * @brief Worker scheduling loop, task dispatch, queue selection, and cooperative execution policy.
 *
 * @details
 * This translation unit contains the worker-side scheduler loops:
 *  - ::llam_scheduler_loop for primary shard workers,
 *  - ::llam_opaque_helper_main for temporary opaque-blocking compensation.
 *
 * Scheduler loop outline:
 *  - Establish thread-local shard/task/scheduler context.
 *  - Bind/tune the worker thread and install a signal stack for diagnostics.
 *  - Repeatedly drain inject work, fire timers, detect pressure, select a
 *    runnable task, switch into the task fiber, and reclaim dead tasks.
 *  - Cooperate with watchdog merge/steal pause requests.
 *  - Respect dynamic-worker online/offline transitions.
 *
 * Opaque helper behavior:
 *  - Sleeps until the primary worker enters an opaque blocking region.
 *  - Temporarily becomes the scheduler thread for the shard.
 *  - Yields ownership back to the primary worker once compensation depth
 *    reaches zero or shutdown begins.
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

#include "runtime_internal.h"

static bool llam_scheduler_signal_stack_failure_is_fatal(int err) {
    /*
     * The alternate signal stack is a diagnostics surface for guard-page fault
     * reports, not a correctness prerequisite for running tasks.  DragonFlyBSD
     * can transiently report EAGAIN when tests rapidly move shard 0 across host
     * pthreads; treating that as fatal makes the scheduler fail even though the
     * runtime can safely continue with reduced crash diagnostics.
     */
    return err != EAGAIN;
}

static int llam_scheduler_try_install_signal_stack(llam_worker_t *worker) {
    int saved_errno;

    if (llam_install_thread_signal_stack(worker) == 0) {
        return 0;
    }

    saved_errno = errno;
    if (!llam_scheduler_signal_stack_failure_is_fatal(saved_errno)) {
        errno = 0;
        return 0;
    }

    errno = saved_errno;
    return -1;
}

void llam_legacy_lease_init(llam_shard_t *shard) {
    llam_legacy_lease_t *lease;

    if (shard == NULL) {
        return;
    }
    lease = &shard->legacy_lease;
    lease->owner = NULL;
    lease->owner_incarnation = 0U;
    lease->epoch = 0U;
    lease->return_worker = NULL;
    lease->return_incarnation = 0U;
    lease->transfer_sequence = 0U;
    lease->handback_ack_sequence = 0U;
    /* No native worker is bound while a runtime is merely initialized. */
    lease->state = LLAM_LEASE_REDIRECTED_NO_EXECUTOR;
}

void llam_legacy_lease_begin_stop_locked(llam_shard_t *shard) {
    llam_legacy_lease_t *lease;

    if (shard == NULL) {
        return;
    }
    lease = llam_shard_legacy_lease_locked(shard);
    /*
     * Teardown blocks new grants, but it cannot erase the current boundary.
     * A helper may still need to clear its active task and acknowledge the
     * recorded return sequence before the shutdown caller joins it.
     */
    lease->state = LLAM_LEASE_STOPPING;
    llam_legacy_lease_assert_consistent_locked(shard);
}

void llam_legacy_lease_finalize_stop_locked(llam_shard_t *shard) {
    llam_legacy_lease_t *lease;

    if (shard == NULL) {
        return;
    }
    lease = llam_shard_legacy_lease_locked(shard);
    lease->owner = NULL;
    lease->owner_incarnation = 0U;
    lease->epoch = 0U;
    lease->return_worker = NULL;
    lease->return_incarnation = 0U;
    lease->transfer_sequence = 0U;
    lease->handback_ack_sequence = 0U;
    lease->state = LLAM_LEASE_STOPPING;
    llam_legacy_lease_assert_consistent_locked(shard);
}

/* Keep sequence/epoch values nonzero so zero remains the uninitialized value. */
static uint64_t llam_legacy_lease_next_counter(uint64_t value) {
    return value == UINT64_MAX ? 1U : value + 1U;
}

static bool llam_legacy_lease_runtime_stopping(const llam_shard_t *shard) {
    const llam_runtime_t *runtime = shard != NULL ? shard->runtime : NULL;

    /* Cooperative stop drains recorded handbacks; teardown is the hard gate. */
    return runtime != NULL &&
           atomic_load_explicit(&runtime->shutdown_requested, memory_order_acquire);
}

/* Merge pauses serialize queue mutation with new lease-transfer requests. */
static bool llam_legacy_lease_merge_paused(const llam_shard_t *shard) {
    return shard != NULL &&
           !llam_legacy_lease_runtime_stopping(shard) &&
           atomic_load_explicit(&shard->merge_pause_requested,
                                memory_order_acquire) != 0U;
}

static bool llam_legacy_lease_matches_worker(const llam_legacy_lease_t *lease,
                                             const llam_worker_t *worker,
                                             uint64_t incarnation) {
    return lease != NULL && worker != NULL && incarnation != 0U &&
           llam_worker_binding_live(worker, incarnation);
}

bool llam_legacy_lease_request_helper_locked(llam_shard_t *shard,
                                             llam_worker_t *primary,
                                             uint64_t *sequence_out) {
    llam_legacy_lease_t *lease;
    uint64_t sequence;

    if (sequence_out != NULL) {
        *sequence_out = 0U;
    }
    if (shard == NULL || primary == NULL || sequence_out == NULL) {
        return false;
    }
    if (llam_legacy_lease_runtime_stopping(shard)) {
        return false;
    }
    if (llam_legacy_lease_merge_paused(shard)) {
        return false;
    }
    lease = llam_shard_legacy_lease_locked(shard);
    if (lease->state != LLAM_LEASE_PRIMARY_OWNED ||
        lease->owner != primary ||
        !llam_legacy_lease_matches_worker(lease,
                                          primary,
                                          lease->owner_incarnation)) {
        return false;
    }
    if (primary != &shard->primary_worker ||
        primary->role != LLAM_WORKER_ROLE_PRIMARY ||
        primary->compat_shard != shard) {
        return false;
    }
    sequence = llam_legacy_lease_next_counter(lease->transfer_sequence);
    lease->transfer_sequence = sequence;
    lease->state = LLAM_LEASE_TRANSFER_TO_HELPER;
    *sequence_out = sequence;
    llam_legacy_lease_assert_consistent_locked(shard);
    return true;
}

bool llam_legacy_lease_grant_helper_locked(llam_shard_t *shard,
                                           llam_worker_t *helper,
                                           uint64_t sequence) {
    llam_legacy_lease_t *lease;
    uint64_t incarnation;

    if (shard == NULL || helper == NULL || sequence == 0U) {
        return false;
    }
    if (llam_legacy_lease_runtime_stopping(shard)) {
        return false;
    }
    lease = llam_shard_legacy_lease_locked(shard);
    incarnation = atomic_load_explicit(&helper->incarnation, memory_order_acquire);
    if (lease->state != LLAM_LEASE_TRANSFER_TO_HELPER ||
        lease->transfer_sequence != sequence ||
        lease->owner != NULL ||
        helper != &shard->opaque_helper_worker ||
        helper->role != LLAM_WORKER_ROLE_OPAQUE_HELPER ||
        helper->compat_shard != shard ||
        !llam_worker_binding_live(helper, incarnation) ||
        lease->state == LLAM_LEASE_STOPPING) {
        return false;
    }
    lease->owner = helper;
    lease->owner_incarnation = incarnation;
    lease->epoch = llam_legacy_lease_next_counter(lease->epoch);
    lease->return_worker = NULL;
    lease->return_incarnation = 0U;
    lease->state = LLAM_LEASE_HELPER_OWNED;
    llam_legacy_lease_assert_consistent_locked(shard);
    return true;
}

bool llam_legacy_lease_request_return_locked(llam_shard_t *shard,
                                             llam_worker_t *claimant,
                                             uint64_t claimant_incarnation,
                                             uint64_t *sequence_out) {
    llam_legacy_lease_t *lease;
    uint64_t sequence;

    if (sequence_out != NULL) {
        *sequence_out = 0U;
    }
    if (shard == NULL || claimant == NULL || sequence_out == NULL ||
        claimant_incarnation == 0U ||
        !llam_worker_binding_live(claimant, claimant_incarnation)) {
        return false;
    }
    lease = llam_shard_legacy_lease_locked(shard);
    if (lease->state == LLAM_LEASE_STOPPING) {
        /*
         * Shutdown may have marked the coordinator while a helper return was
         * in flight. Re-observing the same token is idempotent. Once a return
         * claimant exists, no other worker may reserve or overwrite that
         * boundary; a live helper can reserve the primary return only when the
         * STOPPING coordinator has no claimant yet.
         */
        if (lease->return_worker != NULL) {
            if (lease->return_worker == claimant &&
                lease->return_incarnation == claimant_incarnation &&
                lease->transfer_sequence != 0U) {
                *sequence_out = lease->transfer_sequence;
                return true;
            }
            return false;
        }
        if (lease->return_incarnation != 0U) {
            return false;
        }
        if (lease->owner != &shard->opaque_helper_worker ||
            claimant != &shard->primary_worker ||
            lease->owner_incarnation == 0U ||
            !llam_legacy_lease_matches_worker(lease,
                                              &shard->opaque_helper_worker,
                                              lease->owner_incarnation)) {
            return false;
        }
    } else if (lease->state == LLAM_LEASE_HELPER_OWNED) {
        if (lease->owner != &shard->opaque_helper_worker ||
            claimant != &shard->primary_worker ||
            lease->owner_incarnation == 0U ||
            !llam_legacy_lease_matches_worker(lease,
                                              &shard->opaque_helper_worker,
                                              lease->owner_incarnation)) {
            return false;
        }
    } else if (lease->state == LLAM_LEASE_REDIRECTED_NO_EXECUTOR) {
        if (lease->owner != NULL || lease->return_worker != claimant ||
            lease->return_incarnation != claimant_incarnation) {
            return false;
        }
    } else {
        return false;
    }
    sequence = llam_legacy_lease_next_counter(lease->transfer_sequence);
    lease->transfer_sequence = sequence;
    lease->return_worker = claimant;
    lease->return_incarnation = claimant_incarnation;
    if (lease->state != LLAM_LEASE_STOPPING) {
        lease->state = LLAM_LEASE_TRANSFER_TO_PRIMARY;
    }
    *sequence_out = sequence;
    llam_legacy_lease_assert_consistent_locked(shard);
    return true;
}

bool llam_legacy_lease_complete_handback_locked(llam_shard_t *shard,
                                                llam_worker_t *worker,
                                                uint64_t sequence) {
    llam_legacy_lease_t *lease;
    llam_worker_t *claimant;
    uint64_t claimant_incarnation;

    if (shard == NULL || worker == NULL || sequence == 0U) {
        return false;
    }
    lease = llam_shard_legacy_lease_locked(shard);
    if ((lease->state != LLAM_LEASE_TRANSFER_TO_PRIMARY &&
         lease->state != LLAM_LEASE_STOPPING) ||
        lease->transfer_sequence != sequence ||
        (lease->owner != NULL && lease->owner != worker) ||
        (lease->owner != NULL && lease->owner_incarnation == 0U) ||
        (lease->owner != NULL &&
         !llam_legacy_lease_matches_worker(lease,
                                           worker,
                                           lease->owner_incarnation)) ||
        worker->compat_shard != shard) {
        return false;
    }
    claimant = lease->return_worker;
    claimant_incarnation = lease->return_incarnation;
    if (lease->owner == NULL && claimant != worker) {
        return false;
    }
    if (!llam_worker_binding_live(claimant, claimant_incarnation)) {
        return false;
    }
    /* The acknowledgement is published only after the caller cleared active state. */
    lease->handback_ack_sequence = sequence;
    lease->owner = claimant;
    lease->owner_incarnation = claimant_incarnation;
    lease->epoch = llam_legacy_lease_next_counter(lease->epoch);
    if (lease->state != LLAM_LEASE_STOPPING) {
        lease->return_worker = NULL;
        lease->return_incarnation = 0U;
        lease->state = claimant == &shard->primary_worker
                           ? LLAM_LEASE_PRIMARY_OWNED
                           : LLAM_LEASE_HELPER_OWNED;
    }
    llam_legacy_lease_assert_consistent_locked(shard);
    return true;
}

bool llam_legacy_lease_handback_acknowledged_locked(const llam_shard_t *shard,
                                                    const llam_worker_t *worker,
                                                    uint64_t incarnation,
                                                    uint64_t sequence) {
    const llam_legacy_lease_t *lease;

    if (shard == NULL || worker == NULL || incarnation == 0U || sequence == 0U) {
        return false;
    }
    lease = llam_shard_legacy_lease_const_locked(shard);
    if (lease->state == LLAM_LEASE_STOPPING) {
        bool completed_before_stop =
            lease->handback_ack_sequence == sequence &&
            lease->transfer_sequence == sequence &&
            lease->return_worker == NULL &&
            lease->return_incarnation == 0U &&
            lease->owner == worker &&
            lease->owner_incarnation == incarnation &&
            worker == &shard->primary_worker;
        bool claimant_retained_for_stop =
            lease->handback_ack_sequence == sequence &&
            lease->transfer_sequence == sequence &&
            lease->return_worker == worker &&
            lease->return_incarnation == incarnation &&
            lease->owner == worker &&
            lease->owner_incarnation == incarnation;

        /*
         * STOPPING has two valid handback observations.  A stop can be
         * published while a transfer is still in flight; that path retains
         * the claimant until teardown, as above.  Alternatively, the helper
         * may have already acknowledged the primary and cleared the claimant
         * before shutdown changes PRIMARY_OWNED to STOPPING.  The matching
         * transfer/ACK token and live primary binding make that second shape
         * a completed handback rather than an ownerless grant.
         */
        return (completed_before_stop || claimant_retained_for_stop) &&
               llam_worker_binding_live(worker, incarnation);
    }
    return lease->handback_ack_sequence == sequence &&
           lease->transfer_sequence == sequence &&
           lease->return_worker == NULL &&
           lease->return_incarnation == 0U &&
           lease->owner == worker &&
           lease->owner_incarnation == incarnation &&
           (lease->state == LLAM_LEASE_PRIMARY_OWNED ||
            lease->state == LLAM_LEASE_HELPER_OWNED) &&
           llam_worker_binding_live(worker, incarnation);
}

bool llam_legacy_lease_teardown_return_locked(const llam_shard_t *shard,
                                              const llam_worker_t *worker,
                                              uint64_t incarnation) {
    const llam_runtime_t *runtime;
    const llam_legacy_lease_t *lease;

    if (shard == NULL || worker == NULL || incarnation == 0U) {
        return false;
    }
    runtime = shard->runtime;
    if (runtime == NULL ||
        !atomic_load_explicit(&runtime->shutdown_requested, memory_order_acquire)) {
        return false;
    }
    lease = llam_shard_legacy_lease_const_locked(shard);
    if (lease->state != LLAM_LEASE_STOPPING || lease->owner != worker ||
        lease->owner_incarnation != incarnation || lease->return_worker != worker ||
        lease->return_incarnation != incarnation ||
        (lease->transfer_sequence != 0U &&
         lease->handback_ack_sequence != lease->transfer_sequence)) {
        return false;
    }
    /* A teardown return is valid only after a helper has acknowledged quiescence. */
    return !shard->opaque_helper_thread_started ||
           (atomic_load_explicit(&shard->opaque_helper_exit_ack,
                                 memory_order_acquire) &&
            !shard->opaque_helper_active &&
            llam_worker_load_active_g(&shard->opaque_helper_worker) == NULL);
}

bool llam_legacy_lease_enter_redirect_locked(llam_shard_t *shard,
                                             llam_worker_t *return_worker,
                                             uint64_t return_incarnation) {
    llam_legacy_lease_t *lease;

    if (shard == NULL || return_worker == NULL || return_incarnation == 0U ||
        !llam_worker_binding_live(return_worker, return_incarnation)) {
        return false;
    }
    if (llam_legacy_lease_runtime_stopping(shard)) {
        return false;
    }
    lease = llam_shard_legacy_lease_locked(shard);
    if (lease->state == LLAM_LEASE_REDIRECTED_NO_EXECUTOR) {
        return lease->owner == NULL && lease->return_worker == return_worker &&
               lease->return_incarnation == return_incarnation;
    }
    /* A pause serializes new redirect mutations; reserved handoffs continue. */
    if (llam_legacy_lease_merge_paused(shard) ||
        lease->state == LLAM_LEASE_STOPPING) {
        return false;
    }
    if (lease->owner != return_worker ||
        lease->owner_incarnation != return_incarnation ||
        (lease->state != LLAM_LEASE_PRIMARY_OWNED &&
         lease->state != LLAM_LEASE_HELPER_OWNED &&
         lease->state != LLAM_LEASE_TRANSFER_TO_HELPER)) {
        return false;
    }
    lease->owner = NULL;
    lease->owner_incarnation = 0U;
    lease->return_worker = return_worker;
    lease->return_incarnation = return_incarnation;
    lease->transfer_sequence = llam_legacy_lease_next_counter(lease->transfer_sequence);
    lease->state = LLAM_LEASE_REDIRECTED_NO_EXECUTOR;
    llam_legacy_lease_assert_consistent_locked(shard);
    return true;
}

bool llam_legacy_lease_can_reacquire_locked(const llam_shard_t *shard,
                                            const llam_worker_t *worker,
                                            uint64_t incarnation) {
    const llam_legacy_lease_t *lease;

    if (shard == NULL || worker == NULL || incarnation == 0U) {
        return false;
    }
    if (llam_legacy_lease_runtime_stopping(shard)) {
        return false;
    }
    if (llam_legacy_lease_merge_paused(shard)) {
        return false;
    }
    lease = llam_shard_legacy_lease_const_locked(shard);
    return (lease->state == LLAM_LEASE_PRIMARY_OWNED ||
            lease->state == LLAM_LEASE_HELPER_OWNED) &&
           lease->owner == worker && lease->owner_incarnation == incarnation &&
           llam_worker_binding_live(worker, incarnation);
}

/*
 * Revalidate the primary at the scheduler dispatch boundary.  Binding a live
 * primary while a pause is already requested publishes the owner tuple so the
 * pause can be acknowledged, but leaves an atomic pending bit set until the
 * pause has been released.  The caller must hold opaque_lock; no work is
 * selected while this helper reports false.
 */
bool llam_legacy_lease_reacquire_primary_locked(llam_shard_t *shard,
                                                llam_worker_t *worker,
                                                uint64_t incarnation) {
    llam_legacy_lease_t *lease;
    bool pending;
    bool paused;

    if (shard == NULL || worker == NULL || worker != &shard->primary_worker ||
        incarnation == 0U ||
        !llam_worker_binding_live(worker, incarnation)) {
        if (worker != NULL) {
            atomic_store_explicit(&worker->lease_reacquire_pending,
                                  false,
                                  memory_order_release);
        }
        return false;
    }
    lease = llam_shard_legacy_lease_locked(shard);
    if (llam_legacy_lease_runtime_stopping(shard)) {
        /* A stopping coordinator may keep its already-live owner running so
         * parked tasks can observe cancellation and drain the scheduler. */
        if (lease->owner == worker && lease->owner_incarnation == incarnation &&
            lease->state == LLAM_LEASE_STOPPING) {
            atomic_store_explicit(&worker->lease_reacquire_pending,
                                  false,
                                  memory_order_release);
            return true;
        }
        atomic_store_explicit(&worker->lease_reacquire_pending,
                              false,
                              memory_order_release);
        return false;
    }

    paused = llam_legacy_lease_merge_paused(shard);
    pending = atomic_load_explicit(&worker->lease_reacquire_pending,
                                   memory_order_acquire);
    if (lease->owner == worker && lease->owner_incarnation == incarnation &&
        lease->state == LLAM_LEASE_PRIMARY_OWNED) {
        if (paused) {
            atomic_store_explicit(&worker->lease_reacquire_pending,
                                  true,
                                  memory_order_release);
            return false;
        }
        /* A pending binding is consumed only after this post-pause lock turn. */
        if (pending) {
            atomic_store_explicit(&worker->lease_reacquire_pending,
                                  false,
                                  memory_order_release);
            return true;
        }
        atomic_store_explicit(&worker->lease_reacquire_pending,
                              false,
                              memory_order_release);
        return true;
    }

    /* A return claimant or another owner is never displaced by startup. */
    if (lease->state != LLAM_LEASE_REDIRECTED_NO_EXECUTOR ||
        lease->owner != NULL || lease->return_worker != NULL) {
        atomic_store_explicit(&worker->lease_reacquire_pending,
                              false,
                              memory_order_release);
        return false;
    }

    /* Publish the live owner even during a pause so the pause can ACK. */
    lease->owner = worker;
    lease->owner_incarnation = incarnation;
    lease->state = LLAM_LEASE_PRIMARY_OWNED;
    atomic_store_explicit(&worker->lease_reacquire_pending,
                          paused,
                          memory_order_release);
    llam_legacy_lease_assert_consistent_locked(shard);
    return !paused;
}

bool llam_legacy_lease_clear_owner_locked(llam_shard_t *shard,
                                          llam_worker_t *worker,
                                          uint64_t incarnation,
                                          uint64_t sequence) {
    llam_legacy_lease_t *lease;

    if (shard == NULL || worker == NULL || incarnation == 0U || sequence == 0U) {
        return false;
    }
    lease = llam_shard_legacy_lease_locked(shard);
    if (lease->state != LLAM_LEASE_TRANSFER_TO_HELPER ||
        lease->transfer_sequence != sequence || lease->owner != worker ||
        lease->owner_incarnation != incarnation ||
        !llam_worker_binding_live(worker, incarnation)) {
        return false;
    }
    lease->owner = NULL;
    lease->owner_incarnation = 0U;
    llam_legacy_lease_assert_consistent_locked(shard);
    return true;
}

bool llam_legacy_lease_rollback_helper_locked(llam_shard_t *shard,
                                              llam_worker_t *primary,
                                              uint64_t incarnation,
                                              uint64_t sequence) {
    llam_legacy_lease_t *lease;

    if (shard == NULL || primary == NULL || incarnation == 0U || sequence == 0U) {
        return false;
    }
    if (primary != &shard->primary_worker ||
        primary->role != LLAM_WORKER_ROLE_PRIMARY ||
        primary->compat_shard != shard) {
        return false;
    }
    lease = llam_shard_legacy_lease_locked(shard);
    /* Rollback is valid only for this transfer token with no return claimant. */
    if (lease->transfer_sequence != sequence || lease->return_worker != NULL ||
        lease->return_incarnation != 0U ||
        !llam_worker_binding_live(primary, incarnation)) {
        return false;
    }
    if (lease->state == LLAM_LEASE_STOPPING) {
        if (lease->owner != NULL &&
            (lease->owner != primary || lease->owner_incarnation != incarnation)) {
            return false;
        }
        /* Keep STOPPING, but restore a valid primary owner projection. */
        lease->owner = primary;
        lease->owner_incarnation = incarnation;
        llam_legacy_lease_assert_consistent_locked(shard);
        return true;
    }
    if (lease->state != LLAM_LEASE_TRANSFER_TO_HELPER || lease->owner != NULL) {
        return false;
    }
    lease->owner = primary;
    lease->owner_incarnation = incarnation;
    lease->transfer_sequence = sequence == 1U ? UINT64_MAX : sequence - 1U;
    lease->state = LLAM_LEASE_PRIMARY_OWNED;
    llam_legacy_lease_assert_consistent_locked(shard);
    return true;
}

bool llam_worker_binding_live(const llam_worker_t *worker, uint64_t incarnation) {
    uint64_t worker_incarnation;
    uint64_t binding_incarnation;

    if (worker == NULL || incarnation == 0U ||
        !atomic_load_explicit(&worker->native_thread_live, memory_order_acquire)) {
        return false;
    }
    worker_incarnation =
        atomic_load_explicit(&worker->incarnation, memory_order_acquire);
    binding_incarnation =
        atomic_load_explicit(&worker->binding_incarnation, memory_order_acquire);
    return worker_incarnation == incarnation && binding_incarnation == incarnation;
}

/**
 * @brief Snapshot the live native target of the current lease owner.
 *
 * @details
 * The caller holds @c shard->opaque_lock.  This helper intentionally does not
 * sample active-G: the watchdog takes @c shard->lock next, pins the task, and
 * revalidates this tuple immediately before sending its cooperative signal.
 * Keeping the native handle in the worker binding makes a recycled pthread
 * handle ineligible until a new incarnation is published.
 */
bool llam_legacy_lease_snapshot_watchdog_locked(
    const llam_shard_t *shard,
    llam_worker_t **owner_out,
    uint64_t *incarnation_out,
    uint64_t *epoch_out,
    pthread_t *native_thread_out) {
    const llam_legacy_lease_t *lease;
    llam_worker_t *owner;
    uint64_t incarnation;
    pthread_t empty_thread;

    if (owner_out != NULL) {
        *owner_out = NULL;
    }
    if (incarnation_out != NULL) {
        *incarnation_out = 0U;
    }
    if (epoch_out != NULL) {
        *epoch_out = 0U;
    }
    if (native_thread_out != NULL) {
        memset(native_thread_out, 0, sizeof(*native_thread_out));
    }
    if (shard == NULL || owner_out == NULL || incarnation_out == NULL ||
        epoch_out == NULL || native_thread_out == NULL || shard->runtime == NULL) {
        return false;
    }
    if (atomic_load_explicit(&shard->runtime->shutdown_requested,
                             memory_order_acquire)) {
        return false;
    }
    memset(&empty_thread, 0, sizeof(empty_thread));

    lease = &shard->legacy_lease;
    owner = lease->owner;
    incarnation = lease->owner_incarnation;
    if (owner == NULL || incarnation == 0U || lease->state == LLAM_LEASE_STOPPING ||
        (owner != &shard->primary_worker && owner != &shard->opaque_helper_worker) ||
        owner->compat_shard != shard || owner->associated_processor != &shard->processor ||
        (owner == &shard->primary_worker && owner->role != LLAM_WORKER_ROLE_PRIMARY) ||
        (owner == &shard->opaque_helper_worker &&
         owner->role != LLAM_WORKER_ROLE_OPAQUE_HELPER) ||
        !llam_worker_binding_live(owner, incarnation) ||
        memcmp(&owner->native_thread, &empty_thread, sizeof(empty_thread)) == 0) {
        return false;
    }

    *owner_out = owner;
    *incarnation_out = incarnation;
    *epoch_out = lease->epoch;
    *native_thread_out = owner->native_thread;
    return true;
}

/**
 * @brief Validate a versioned merge/steal pause acknowledgement.
 *
 * @details
 * The caller holds @c shard->opaque_lock.  A boolean ACK alone is not a
 * lifetime proof: the request, lease epoch, owner incarnation, and live worker
 * binding all belong to the same publication episode.  Either pause channel is
 * accepted here so merge code can use one final validation helper while the
 * runtime-wide steal pause remains per-shard acknowledged.
 */
static bool llam_shard_pause_owner_matches_locked(const llam_shard_t *shard,
                                                  uint64_t epoch_value,
                                                  uint64_t worker_incarnation) {
    const llam_legacy_lease_t *lease;
    llam_worker_t *owner;

    if (shard == NULL || shard->runtime == NULL || worker_incarnation == 0U) {
        return false;
    }
    lease = &shard->legacy_lease;
    owner = lease->owner;
    return owner != NULL &&
           (lease->state == LLAM_LEASE_PRIMARY_OWNED ||
            lease->state == LLAM_LEASE_HELPER_OWNED) &&
           (owner == &shard->primary_worker ||
            owner == &shard->opaque_helper_worker) &&
           owner->compat_shard == shard &&
           ((owner == &shard->primary_worker &&
             owner->role == LLAM_WORKER_ROLE_PRIMARY) ||
            (owner == &shard->opaque_helper_worker &&
             owner->role == LLAM_WORKER_ROLE_OPAQUE_HELPER)) &&
           lease->owner_incarnation == worker_incarnation &&
           lease->epoch == epoch_value &&
           llam_worker_binding_live(owner, worker_incarnation);
}

bool llam_shard_merge_pause_ack_matches_locked(const llam_shard_t *shard,
                                               uint64_t request_sequence,
                                               uint64_t epoch_value,
                                               uint64_t worker_incarnation) {
    return shard != NULL && request_sequence != 0U &&
           atomic_load_explicit(&shard->merge_pause_requested,
                                memory_order_acquire) != 0U &&
           atomic_load_explicit(&shard->merge_pause_ack,
                                memory_order_acquire) != 0U &&
           atomic_load_explicit(&shard->merge_pause_request_sequence,
                                memory_order_acquire) == request_sequence &&
           atomic_load_explicit(&shard->merge_pause_ack_sequence,
                                memory_order_acquire) == request_sequence &&
           atomic_load_explicit(&shard->merge_pause_ack_lease_epoch,
                                       memory_order_acquire) == epoch_value &&
           atomic_load_explicit(&shard->merge_pause_ack_worker_incarnation,
                                memory_order_acquire) == worker_incarnation &&
           llam_shard_pause_owner_matches_locked(shard,
                                                 epoch_value,
                                                 worker_incarnation);
}

bool llam_shard_steal_pause_ack_matches_locked(const llam_shard_t *shard,
                                               uint64_t request_sequence,
                                               uint64_t epoch_value,
                                               uint64_t worker_incarnation) {
    return shard != NULL && request_sequence != 0U &&
           llam_runtime_steal_pause_active(shard->runtime) &&
           atomic_load_explicit(&shard->runtime->steal_pause_request_sequence,
                                memory_order_acquire) == request_sequence &&
           atomic_load_explicit(&shard->steal_pause_ack,
                                memory_order_acquire) != 0U &&
           atomic_load_explicit(&shard->steal_pause_request_sequence,
                                memory_order_acquire) == request_sequence &&
           atomic_load_explicit(&shard->steal_pause_ack_sequence,
                                memory_order_acquire) == request_sequence &&
           atomic_load_explicit(&shard->steal_pause_ack_lease_epoch,
                                       memory_order_acquire) == epoch_value &&
           atomic_load_explicit(&shard->steal_pause_ack_worker_incarnation,
                                memory_order_acquire) == worker_incarnation &&
           llam_shard_pause_owner_matches_locked(shard,
                                                 epoch_value,
                                                 worker_incarnation);
}

bool llam_shard_pause_ack_matches_locked(const llam_shard_t *shard,
                                         uint64_t request_sequence,
                                         uint64_t epoch_value,
                                         uint64_t worker_incarnation) {
    return llam_shard_merge_pause_ack_matches_locked(shard,
                                                     request_sequence,
                                                     epoch_value,
                                                     worker_incarnation) ||
           llam_shard_steal_pause_ack_matches_locked(shard,
                                                     request_sequence,
                                                     epoch_value,
                                                     worker_incarnation);
}

/** @brief Publish the task executing on a worker after its state is prepared. */
void llam_worker_publish_active_g(llam_worker_t *worker, llam_task_t *task) {
    if (worker == NULL) {
        return;
    }
    atomic_store_explicit(&worker->active_g, task, memory_order_release);
}

/** @brief Load a worker's current execution publication. */
llam_task_t *llam_worker_load_active_g(const llam_worker_t *worker) {
    if (worker == NULL) {
        return NULL;
    }
    return atomic_load_explicit(&worker->active_g, memory_order_acquire);
}

/** @brief Retire a worker's execution publication before task reclaim. */
void llam_worker_clear_active_g(llam_worker_t *worker) {
    if (worker == NULL) {
        return;
    }
    atomic_store_explicit(&worker->active_g, NULL, memory_order_release);
    atomic_store_explicit(&worker->last_run_started_ns,
                          0U,
                          memory_order_release);
}

/**
 * @brief Load the active-G belonging to a validated lease owner.
 *
 * @note The caller holds `shard->opaque_lock` followed by `shard->lock`.
 */
llam_task_t *llam_shard_load_lease_current(llam_shard_t *shard) {
    llam_legacy_lease_t *lease;
    llam_worker_t *owner;

    if (shard == NULL) {
        return NULL;
    }
    /*
     * The caller holds opaque_lock -> shard->lock.  Reading the plain lease
     * record and then loading the owner's active-G under that pair makes the
     * worker selection and task lifetime one protected observation.  In
     * particular, this is not an atomic pointer/epoch lifetime proof.
     */
    lease = &shard->legacy_lease;
    owner = lease->owner;
    if (owner == NULL ||
        (owner != &shard->primary_worker &&
         owner != &shard->opaque_helper_worker) ||
        !llam_worker_matches_shard(owner, shard) ||
        (owner == &shard->primary_worker &&
         owner->role != LLAM_WORKER_ROLE_PRIMARY) ||
        (owner == &shard->opaque_helper_worker &&
         owner->role != LLAM_WORKER_ROLE_OPAQUE_HELPER) ||
        lease->owner_incarnation == 0U ||
        !llam_worker_binding_live(owner, lease->owner_incarnation)) {
        return NULL;
    }
    return llam_worker_load_active_g(owner);
}

void llam_scheduler_bind_identity(llam_shard_t *shard,
                                  llam_worker_t *worker,
                                  llam_ctx_t *ctx) {
    uint64_t incarnation;

    if (shard == NULL || worker == NULL || ctx == NULL) {
        return;
    }
    /* Binding metadata and lease publication share the handoff lock. */
    pthread_mutex_lock(&shard->opaque_lock);
    incarnation = atomic_fetch_add_explicit(&worker->incarnation,
                                             1U,
                                             memory_order_acq_rel) + 1U;
    worker->native_thread = pthread_self();
    atomic_store_explicit(&worker->binding_incarnation,
                          incarnation,
                          memory_order_release);
    atomic_store_explicit(&worker->native_thread_live, true, memory_order_release);
    if (worker == &shard->primary_worker) {
        /*
         * Publish/revalidate the binding under the lease lock.  If a merge
         * pause is already active the helper publishes the owner tuple for a
         * valid ACK but records an atomic deferral; dispatch is gated until a
         * post-pause call observes the released boundary.
         */
        (void)llam_legacy_lease_reacquire_primary_locked(shard,
                                                         worker,
                                                         incarnation);
    }
    pthread_mutex_unlock(&shard->opaque_lock);

    g_llam_tls_shard = shard;
    g_llam_tls_worker = worker;
    g_llam_tls_processor = &shard->processor;
    g_llam_tls_task = NULL;
    g_llam_tls_scheduler_ctx = ctx;
}

void llam_scheduler_unbind_identity(llam_shard_t *shard, llam_worker_t *worker) {
    uint64_t incarnation;

    if (shard == NULL || worker == NULL) {
        return;
    }
    pthread_mutex_lock(&shard->opaque_lock);
    incarnation = atomic_load_explicit(&worker->binding_incarnation,
                                       memory_order_acquire);
    if (atomic_load_explicit(&worker->native_thread_live, memory_order_acquire) &&
        pthread_equal(worker->native_thread, pthread_self()) != 0) {
        /* Clear eligibility before the native handle can be reused. */
        atomic_store_explicit(&worker->native_thread_live, false, memory_order_release);
        atomic_store_explicit(&worker->binding_incarnation, 0U, memory_order_release);
        atomic_store_explicit(&worker->lease_reacquire_pending,
                              false,
                              memory_order_release);
        memset(&worker->native_thread, 0, sizeof(worker->native_thread));
    }
    llam_worker_clear_active_g(worker);
    atomic_store_explicit(&worker->last_run_started_ns, 0U, memory_order_release);
    atomic_store_explicit(&worker->last_safepoint_ns, 0U, memory_order_release);
    atomic_store_explicit(&worker->last_idle_wake_ns, 0U, memory_order_release);
    if (shard->legacy_lease.state != LLAM_LEASE_STOPPING &&
        shard->legacy_lease.owner == worker &&
        shard->legacy_lease.owner_incarnation == incarnation) {
        shard->legacy_lease.owner = NULL;
        shard->legacy_lease.owner_incarnation = 0U;
        shard->legacy_lease.state = LLAM_LEASE_REDIRECTED_NO_EXECUTOR;
    }
    if (shard->legacy_lease.state != LLAM_LEASE_STOPPING &&
        shard->legacy_lease.return_worker == worker &&
        shard->legacy_lease.return_incarnation == incarnation) {
        shard->legacy_lease.return_worker = NULL;
        shard->legacy_lease.return_incarnation = 0U;
    }
    llam_legacy_lease_assert_consistent_locked(shard);
    pthread_mutex_unlock(&shard->opaque_lock);
}

void llam_scheduler_clear_identity(void) {
    g_llam_tls_task = NULL;
    g_llam_tls_scheduler_ctx = NULL;
    g_llam_tls_processor = NULL;
    g_llam_tls_worker = NULL;
    g_llam_tls_shard = NULL;
}

#if defined(LLAM_ENABLE_TEST_HOOKS)
/*
 * Test-only primary-binding replacement request.  The request is claimed and
 * serviced by the owning scheduler context; the host thread never edits a
 * worker's native binding or lease matcher fields.  Zero is idle, one is a
 * pending request, two is a scheduler-owned transaction, three is complete,
 * and four cancels a request that the scheduler has not yet claimed.
 */
static _Atomic(llam_shard_t *) g_llam_scheduler_test_rebind_shard;
static atomic_uint g_llam_scheduler_test_rebind_state;
static atomic_bool g_llam_scheduler_test_rebind_result;

static void llam_scheduler_test_service_rebind_primary(llam_shard_t *shard) {
    llam_shard_t *requested_shard;
    unsigned expected_state = 1U;
    bool valid = false;
    uint64_t old_incarnation;
    uint64_t new_incarnation;

    requested_shard = atomic_load_explicit(&g_llam_scheduler_test_rebind_shard,
                                            memory_order_acquire);
    if (requested_shard != shard ||
        !atomic_compare_exchange_strong_explicit(&g_llam_scheduler_test_rebind_state,
                                                 &expected_state,
                                                 2U,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        return;
    }

    /* This function runs at the scheduler boundary, after active-G cleanup. */
    if (shard != NULL && shard->runtime != NULL &&
        g_llam_tls_shard == shard &&
        g_llam_tls_worker == &shard->primary_worker &&
        g_llam_tls_processor == &shard->processor &&
        g_llam_tls_task == NULL &&
        atomic_load_explicit(&shard->runtime->shutdown_requested,
                             memory_order_acquire) == 0U &&
        atomic_load_explicit(&shard->runtime->stop_requested,
                             memory_order_acquire) == 0U) {
        old_incarnation = atomic_load_explicit(&shard->primary_worker.incarnation,
                                               memory_order_acquire);
        /* Signal-stack ownership is part of the same worker incarnation. */
        if (shard->primary_worker.sigaltstack_installed) {
            llam_uninstall_thread_signal_stack(&shard->primary_worker);
            llam_scheduler_unbind_identity(shard, &shard->primary_worker);
            llam_scheduler_bind_identity(shard,
                                         &shard->primary_worker,
                                         &shard->scheduler_ctx);
            if (llam_scheduler_try_install_signal_stack(&shard->primary_worker) != 0) {
                llam_record_fatal(shard->runtime, errno);
            }
        }
        new_incarnation = atomic_load_explicit(&shard->primary_worker.incarnation,
                                               memory_order_acquire);
        valid = shard->primary_worker.sigaltstack_installed &&
                new_incarnation != 0U && new_incarnation > old_incarnation &&
                llam_worker_binding_live(&shard->primary_worker, new_incarnation);
        if (valid) {
            pthread_mutex_lock(&shard->opaque_lock);
            valid = shard->legacy_lease.state == LLAM_LEASE_PRIMARY_OWNED &&
                    shard->legacy_lease.owner == &shard->primary_worker &&
                    shard->legacy_lease.owner_incarnation == new_incarnation;
            pthread_mutex_unlock(&shard->opaque_lock);
        }
    }

    atomic_store_explicit(&g_llam_scheduler_test_rebind_result,
                          valid,
                          memory_order_release);
    atomic_store_explicit(&g_llam_scheduler_test_rebind_shard,
                          NULL,
                          memory_order_release);
    atomic_store_explicit(&g_llam_scheduler_test_rebind_state,
                          3U,
                          memory_order_release);
}

bool llam_scheduler_test_rebind_primary(llam_shard_t *shard,
                                        uint64_t timeout_ns) {
    llam_runtime_t *runtime;
    uint64_t start_ns;
    uint64_t deadline_ns;
    unsigned expected_state = 0U;

    if (shard == NULL || (runtime = shard->runtime) == NULL ||
        !atomic_load_explicit(&runtime->initialized, memory_order_acquire) ||
        !atomic_load_explicit(&runtime->exec_started, memory_order_acquire)) {
        return false;
    }
    if (!atomic_compare_exchange_strong_explicit(&g_llam_scheduler_test_rebind_state,
                                                 &expected_state,
                                                 1U,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        return false;
    }
    atomic_store_explicit(&g_llam_scheduler_test_rebind_result,
                          false,
                          memory_order_release);
    atomic_store_explicit(&g_llam_scheduler_test_rebind_shard,
                          shard,
                          memory_order_release);
    llam_kick_shard(shard);

    start_ns = llam_now_ns();
    deadline_ns = timeout_ns > UINT64_MAX - start_ns
                      ? UINT64_MAX
                      : start_ns + timeout_ns;
    for (;;) {
        unsigned state = atomic_load_explicit(&g_llam_scheduler_test_rebind_state,
                                              memory_order_acquire);

        if (state == 3U) {
            bool result = atomic_load_explicit(&g_llam_scheduler_test_rebind_result,
                                               memory_order_acquire);

            atomic_store_explicit(&g_llam_scheduler_test_rebind_state,
                                  0U,
                                  memory_order_release);
            return result;
        }
        if (llam_now_ns() >= deadline_ns) {
            expected_state = 1U;
            if (atomic_compare_exchange_strong_explicit(&g_llam_scheduler_test_rebind_state,
                                                        &expected_state,
                                                        4U,
                                                        memory_order_acq_rel,
                                                        memory_order_acquire)) {
                atomic_store_explicit(&g_llam_scheduler_test_rebind_shard,
                                      NULL,
                                      memory_order_release);
                atomic_store_explicit(&g_llam_scheduler_test_rebind_state,
                                      0U,
                                      memory_order_release);
                return false;
            }
            /* A claimed transaction has no wait points; let it publish done. */
        }
        llam_pause_cpu();
    }
}
#endif

/**
 * @brief Mark a task as running on a shard and update dispatch metrics.
 *
 * This prepares thread-local task state before switching from the scheduler
 * context into the task fiber.  Optional timing is sampled only when runtime
 * profiling requires it to keep the hot dispatch path cheap.
 *
 * @param shard Scheduler shard dispatching the task.
 * @param task Runnable task selected by the scheduler.
 *
 * @return Start timestamp in nanoseconds when run timing is enabled, otherwise
 *         0.
 */
static uint64_t llam_set_task_running(llam_shard_t *shard, llam_task_t *task) {
    llam_worker_t *worker = g_llam_tls_worker;
    bool run_timing = shard->runtime->run_timing_enabled != 0U;
    bool wake_timing = task->last_runnable_ns > 0U;
    bool preempt_timing = shard->runtime->preempt_mode >= LLAM_PREEMPT_AUTO;
    bool sample_safepoint = shard->runtime->profile == LLAM_RUNTIME_PROFILE_DEBUG_SAFE ||
                            (shard->metrics.ctx_switches & 63ULL) == 0U;
    uint64_t now_ns = (run_timing || wake_timing || preempt_timing || sample_safepoint) ? llam_now_ns() : 0U;

    g_llam_tls_task = task;
    atomic_store_explicit(&task->last_shard, shard->id, memory_order_relaxed);
    task->state = LLAM_TASK_STATE_RUNNING;
    task->last_started_ns = now_ns;
    if (wake_timing) {
        llam_runtime_record_dispatch_latency(shard, task, now_ns);
    }
    if (worker != NULL && worker->compat_shard == shard) {
        atomic_store_explicit(&worker->last_run_started_ns,
                              run_timing ? now_ns : 0U,
                              memory_order_release);
        if (now_ns != 0U) {
            atomic_store_explicit(&worker->last_safepoint_ns,
                                  now_ns,
                                  memory_order_release);
        }
        /* Pause with state, shard association, and timestamps complete. */
        llam_legacy_lease_test_pause(LLAM_LEASE_TEST_POINT_AFTER_TASK_STATE_PREPARE);
        /* Publish only after task state, shard association, and timestamps. */
        llam_worker_publish_active_g(worker, task);
        llam_legacy_lease_test_pause(LLAM_LEASE_TEST_POINT_AFTER_ACTIVE_G_PUBLISH);
    }
    llam_trace_shard(shard, task, LLAM_TRACE_STATE, LLAM_TASK_STATE_RUNNABLE, LLAM_TASK_STATE_RUNNING, LLAM_WAIT_NONE);
    return run_timing ? now_ns : 0U;
}

/**
 * @brief Check whether a shard still owns local work.
 *
 * Dynamic workers use this check before staying offline.  Timers, queued work,
 * a currently running task, or opaque redirect state all count as local work.
 *
 * @param shard Scheduler shard to inspect.
 * @return true if the shard has work that requires an online worker.
 */
static bool llam_shard_has_local_work(llam_shard_t *shard) {
    bool has_work;

    pthread_mutex_lock(&shard->opaque_lock);
    pthread_mutex_lock(&shard->lock);
    has_work = shard->inject_q.depth > 0U || shard->hot_q.depth > 0U || llam_norm_queue_depth(shard) > 0U ||
               shard->timers != NULL ||
               llam_shard_load_lease_current(shard) != NULL ||
               shard->opaque_redirect_active;
    pthread_mutex_unlock(&shard->lock);
    pthread_mutex_unlock(&shard->opaque_lock);
    return has_work;
}

/**
 * @brief Treat an empty live-task set as a drained runtime.
 *
 * Task exit normally requests stop when the last task completes.  This guard
 * makes the scheduler robust against missed or reordered wake/stop observation:
 * once no managed tasks remain, the run loop is complete and all idle workers
 * should be woken so they can leave their scheduler loops.
 */
static bool llam_runtime_drained(llam_runtime_t *rt) {
    if (llam_runtime_has_live_tasks(rt)) {
        return false;
    }
    llam_request_stop(rt);
    return true;
}

/**
 * @brief Clear the current task after returning from a task fiber.
 *
 * This records run timing, stack samples, and slice-overrun metrics before the
 * scheduler chooses the next task.  It does not reclaim the task; reclamation
 * is handled after the caller observes the task state.
 *
 * @param shard Scheduler shard that just ran a task.
 * @param run_ns Measured run duration, or 0 when timing is disabled.
 */
static void llam_clear_current_task(llam_shard_t *shard, uint64_t run_ns) {
    llam_task_t *task = g_llam_tls_task;
    llam_worker_t *worker = g_llam_tls_worker;
    bool watchdog_may_sample_current =
        shard != NULL && shard->runtime != NULL && shard->runtime->preempt_mode >= LLAM_PREEMPT_AUTO;

    if (run_ns == 0U && !watchdog_may_sample_current) {
        pthread_mutex_lock(&shard->lock);
        if (worker != NULL && worker->compat_shard == shard) {
            llam_worker_clear_active_g(worker);
        }
        pthread_mutex_unlock(&shard->lock);
        g_llam_tls_task = NULL;
        return;
    }

    /*
     * The auto-preemption watchdog samples the lease owner's active-G while
     * holding shard->lock.  Even when run timing is disabled, clear active-G
     * under the same lock so a completed task cannot be recycled while the
     * watchdog is still inspecting it.
     */
    pthread_mutex_lock(&shard->lock);
    if (task != NULL && run_ns != 0U) {
        uint64_t slice_ns = llam_slice_ns((llam_task_class_t)atomic_load_explicit(&task->task_class, memory_order_acquire));

        task->last_run_ns = run_ns;
        task->total_run_ns += run_ns;
#if ((LLAM_PLATFORM_LINUX || LLAM_PLATFORM_DARWIN || LLAM_PLATFORM_BSD) || LLAM_PLATFORM_WINDOWS) && LLAM_ARCH_X86_64
        llam_task_sample_stack_rsp(task, (uintptr_t)task->ctx.rsp);
#elif LLAM_ARCH_AARCH64
        llam_task_sample_stack_rsp(task, (uintptr_t)task->ctx.sp);
#endif
        shard->metrics.slice_budget_ns += slice_ns;
        if (run_ns > shard->metrics.max_run_ns) {
            shard->metrics.max_run_ns = run_ns;
        }
        if (run_ns > slice_ns) {
            shard->metrics.slice_overruns += 1U;
        }
    }
    if (run_ns != 0U) {
        shard->metrics.total_run_ns += run_ns;
    }
    if (worker != NULL && worker->compat_shard == shard) {
        llam_worker_clear_active_g(worker);
    }
    pthread_mutex_unlock(&shard->lock);
    g_llam_tls_task = NULL;
}

/* Publish a steal-pause ACK only for the worker that currently owns the lease. */
static void llam_scheduler_publish_steal_pause_ack_locked(llam_shard_t *shard) {
    llam_runtime_t *rt = shard != NULL ? shard->runtime : NULL;
    llam_worker_t *worker = g_llam_tls_worker;
    uint64_t request_sequence;
    uint64_t incarnation;

    if (shard == NULL || rt == NULL || !llam_runtime_steal_pause_active(rt)) {
        if (shard != NULL) {
            atomic_store_explicit(&shard->steal_pause_ack, 0U, memory_order_release);
            atomic_store_explicit(&shard->steal_pause_ack_sequence,
                                  0U,
                                  memory_order_release);
            atomic_store_explicit(&shard->steal_pause_ack_lease_epoch,
                                  0U,
                                  memory_order_release);
            atomic_store_explicit(&shard->steal_pause_ack_worker_incarnation,
                                  0U,
                                  memory_order_release);
        }
        return;
    }
    request_sequence = atomic_load_explicit(&shard->steal_pause_request_sequence,
                                            memory_order_acquire);
    incarnation = worker != NULL
                      ? atomic_load_explicit(&worker->binding_incarnation,
                                             memory_order_acquire)
                      : 0U;
    if (request_sequence == 0U || worker == NULL ||
        shard->legacy_lease.owner != worker ||
        shard->legacy_lease.owner_incarnation != incarnation ||
        !llam_worker_binding_live(worker, incarnation)) {
        atomic_store_explicit(&shard->steal_pause_ack, 0U, memory_order_release);
        atomic_store_explicit(&shard->steal_pause_ack_sequence,
                              0U,
                              memory_order_release);
        atomic_store_explicit(&shard->steal_pause_ack_lease_epoch,
                              0U,
                              memory_order_release);
        atomic_store_explicit(&shard->steal_pause_ack_worker_incarnation,
                              0U,
                              memory_order_release);
        return;
    }
    atomic_store_explicit(&shard->steal_pause_ack_lease_epoch,
                          shard->legacy_lease.epoch,
                          memory_order_release);
    atomic_store_explicit(&shard->steal_pause_ack_worker_incarnation,
                          incarnation,
                          memory_order_release);
    atomic_store_explicit(&shard->steal_pause_ack_sequence,
                          request_sequence,
                          memory_order_release);
    atomic_store_explicit(&shard->steal_pause_ack, 1U, memory_order_release);
}

/* Publish a merge-pause ACK with the same lease/worker tuple as steal ACKs. */
static void llam_scheduler_publish_merge_pause_ack_locked(llam_shard_t *shard) {
    llam_worker_t *worker = g_llam_tls_worker;
    uint64_t request_sequence;
    uint64_t incarnation;

    if (shard == NULL ||
        atomic_load_explicit(&shard->merge_pause_requested,
                             memory_order_acquire) == 0U) {
        if (shard != NULL) {
            atomic_store_explicit(&shard->merge_pause_ack, 0U, memory_order_release);
            atomic_store_explicit(&shard->merge_pause_ack_sequence,
                                  0U,
                                  memory_order_release);
            atomic_store_explicit(&shard->merge_pause_ack_lease_epoch,
                                  0U,
                                  memory_order_release);
            atomic_store_explicit(&shard->merge_pause_ack_worker_incarnation,
                                  0U,
                                  memory_order_release);
        }
        return;
    }
    request_sequence = atomic_load_explicit(&shard->merge_pause_request_sequence,
                                            memory_order_acquire);
    incarnation = worker != NULL
                      ? atomic_load_explicit(&worker->binding_incarnation,
                                             memory_order_acquire)
                      : 0U;
    if (request_sequence == 0U || worker == NULL ||
        shard->legacy_lease.owner != worker ||
        shard->legacy_lease.owner_incarnation != incarnation ||
        !llam_worker_binding_live(worker, incarnation)) {
        atomic_store_explicit(&shard->merge_pause_ack, 0U, memory_order_release);
        atomic_store_explicit(&shard->merge_pause_ack_sequence,
                              0U,
                              memory_order_release);
        atomic_store_explicit(&shard->merge_pause_ack_lease_epoch,
                              0U,
                              memory_order_release);
        atomic_store_explicit(&shard->merge_pause_ack_worker_incarnation,
                              0U,
                              memory_order_release);
        return;
    }
    atomic_store_explicit(&shard->merge_pause_ack_lease_epoch,
                          shard->legacy_lease.epoch,
                          memory_order_release);
    atomic_store_explicit(&shard->merge_pause_ack_worker_incarnation,
                          incarnation,
                          memory_order_release);
    atomic_store_explicit(&shard->merge_pause_ack_sequence,
                          request_sequence,
                          memory_order_release);
    atomic_store_explicit(&shard->merge_pause_ack, 1U, memory_order_release);
}

/**
 * @brief Cooperate with watchdog merge/steal pause requests.
 *
 * The watchdog can request a shard pause before merging or rehoming runtime
 * state.  The scheduler acknowledges the pause with a request/lease/worker
 * tuple, idles until the request clears, and then resumes normal work selection.
 *
 * @param shard Scheduler shard checking pause state.
 * @return true if the loop paused and should restart its iteration.
 */
static bool llam_shard_pause_for_merge(llam_shard_t *shard) {
    llam_runtime_t *rt = shard->runtime;
    bool merge_requested;

    if (rt == NULL) {
        return false;
    }

    pthread_mutex_lock(&shard->opaque_lock);
    llam_scheduler_publish_steal_pause_ack_locked(shard);
    merge_requested = atomic_load_explicit(&shard->merge_pause_requested,
                                           memory_order_acquire) != 0U;
    if (merge_requested) {
        llam_scheduler_publish_merge_pause_ack_locked(shard);
    }
    pthread_mutex_unlock(&shard->opaque_lock);

    if (!merge_requested) {
        return false;
    }

    while (llam_shard_merge_pause_requested(shard)) {
        if ((atomic_load(&rt->stop_requested) && !llam_runtime_has_live_tasks(rt)) ||
            llam_runtime_drained(rt)) {
            break;
        }
        llam_idle_wait(shard);
        pthread_mutex_lock(&shard->opaque_lock);
        llam_scheduler_publish_steal_pause_ack_locked(shard);
        if (llam_shard_merge_pause_requested(shard)) {
            llam_scheduler_publish_merge_pause_ack_locked(shard);
        }
        pthread_mutex_unlock(&shard->opaque_lock);
    }
    pthread_mutex_lock(&shard->opaque_lock);
    llam_scheduler_publish_merge_pause_ack_locked(shard);
    llam_scheduler_publish_steal_pause_ack_locked(shard);
    pthread_mutex_unlock(&shard->opaque_lock);
    return true;
}

/*
 * Keep the primary scheduler ownerful before selecting any task.  This check
 * is deliberately a short opaque-lock transaction: it may publish a startup
 * owner or clear a pause deferral, but it never waits or switches context while
 * the lease lock is held.
 */
static bool llam_scheduler_primary_lease_ready(llam_shard_t *shard) {
    uint64_t incarnation;
    bool ready;

    if (shard == NULL) {
        return false;
    }
    incarnation = atomic_load_explicit(&shard->primary_worker.binding_incarnation,
                                       memory_order_acquire);
    pthread_mutex_lock(&shard->opaque_lock);
    ready = llam_legacy_lease_reacquire_primary_locked(
        shard,
        &shard->primary_worker,
        incarnation);
    pthread_mutex_unlock(&shard->opaque_lock);
    return ready;
}

#if defined(__linux__)
/**
 * @brief Report whether the Linux opaque helper may block in the opaque wait path.
 *
 * @param shard Scheduler shard owned by the helper.
 * @return true when no timers require the helper to use the ordinary idle path.
 */
static bool llam_opaque_helper_can_opaque_wait(llam_shard_t *shard) {
    return atomic_load_explicit(&shard->timer_count, memory_order_acquire) == 0U;
}

/**
 * @brief Sleep the Linux opaque helper until work or a wake signal arrives.
 *
 * @param shard Scheduler shard owned by the helper.
 *
 * @note Caller holds shard->opaque_lock.  The function briefly publishes
 *       opaque-helper wait state so wake producers can avoid lost signals.
 */
static void llam_opaque_helper_wait_for_signal_locked(llam_shard_t *shard) {
    atomic_store_explicit(&shard->opaque_helper_opaque_wait, 1U, memory_order_release);
    if (atomic_exchange_explicit(&shard->event_pending, 0U, memory_order_acq_rel) != 0U) {
        atomic_store_explicit(&shard->opaque_helper_opaque_wait, 0U, memory_order_release);
        atomic_store_explicit(&g_llam_tls_worker->last_idle_wake_ns,
                              llam_now_ns(),
                              memory_order_release);
        return;
    }
    llam_opaque_wake_wait(shard);
    atomic_store_explicit(&shard->opaque_helper_opaque_wait, 0U, memory_order_release);
    if (atomic_exchange_explicit(&shard->event_pending, 0U, memory_order_acq_rel) != 0U) {
        atomic_store_explicit(&g_llam_tls_worker->last_idle_wake_ns,
                              llam_now_ns(),
                              memory_order_release);
    }
}
#endif

/**
 * @brief Primary scheduler loop executed by a shard worker thread.
 *
 * The loop runs until stop is requested and the live task count reaches zero.
 * It is responsible for all owner-side queue maintenance for the shard:
 * allocator quiescence, inject draining, timer firing, local task selection,
 * stealing, overflow draining, task context switching, and dead-task reclaim
 * publication.
 *
 * @param shard Scheduler shard assigned to this worker.
 *
 * @note This function never runs inside a LLAM task.  It owns
 *       g_llam_tls_scheduler_ctx for its worker thread.
 */
void llam_scheduler_loop(llam_shard_t *shard) {
    llam_runtime_t *rt = shard->runtime;

    if (g_llam_tls_shard != shard ||
        g_llam_tls_worker != &shard->primary_worker ||
        g_llam_tls_processor != &shard->processor) {
        llam_scheduler_bind_identity(shard, &shard->primary_worker, &shard->scheduler_ctx);
    }
    shard->primary_thread = pthread_self();
    llam_bind_current_thread_to_cpu(shard->cpu_id);
    llam_tune_scheduler_thread(shard, false);
    if (llam_scheduler_try_install_signal_stack(&shard->primary_worker) != 0) {
        llam_record_fatal(rt, errno);
        llam_scheduler_unbind_identity(shard, &shard->primary_worker);
        llam_scheduler_clear_identity();
        return;
    }

    while (!atomic_load(&rt->stop_requested) || llam_runtime_has_live_tasks(rt)) {
        llam_task_t *task;
        uint64_t started_ns;
        bool pressure;

#if defined(LLAM_ENABLE_TEST_HOOKS)
        llam_scheduler_test_service_rebind_primary(shard);
#endif
        if (llam_runtime_drained(rt)) {
            break;
        }
        if (!llam_scheduler_primary_lease_ready(shard)) {
            /* Never select work while startup/return leaves the shard ownerless. */
            if (llam_shard_pause_for_merge(shard)) {
                continue;
            }
            if ((atomic_load(&rt->stop_requested) && !llam_runtime_has_live_tasks(rt)) ||
                llam_runtime_drained(rt)) {
                break;
            }
            if (atomic_load_explicit(&rt->stop_requested, memory_order_acquire) &&
                llam_runtime_has_live_tasks(rt)) {
                /* Shutdown still needs to wake parked tasks while dispatch is deferred. */
                llam_runtime_cancel_parked_waiters(rt);
            }
            llam_idle_wait(shard);
            continue;
        }
        if (llam_shard_pause_for_merge(shard)) {
            continue;
        }
        if (atomic_load_explicit(&rt->stop_requested, memory_order_acquire) &&
            llam_runtime_has_live_tasks(rt)) {
            /*
             * A task can enter a blocking wait after another task has already
             * requested runtime stop.  Re-run the stop cancellation pass from
             * the scheduler loop so those late parkers observe ECANCELED
             * instead of waiting for a producer that may never arrive.
             */
            llam_runtime_cancel_parked_waiters(rt);
        }
        if (rt->experimental_dynamic_shards != 0U &&
            atomic_load_explicit(&shard->online, memory_order_acquire) == 0U) {
            if (!llam_shard_has_local_work(shard)) {
                if ((atomic_load(&rt->stop_requested) && !llam_runtime_has_live_tasks(rt)) ||
                    llam_runtime_drained(rt)) {
                    break;
                }
                llam_idle_wait(shard);
                continue;
            }
            atomic_store_explicit(&shard->online, 1U, memory_order_release);
            llam_runtime_note_online_shards(rt, atomic_fetch_add_explicit(&rt->online_shards, 1U, memory_order_acq_rel) + 1U);
        }

        llam_allocator_quiescent(shard);
        llam_drain_inject_queue(shard);
        llam_fire_expired_timers(shard);
        pressure = llam_runtime_pressure_signal(rt);

        task = llam_take_local_task_with_pressure(shard, pressure);
        if (task == NULL) {
            if (pressure) {
                task = llam_take_overflow_task(rt);
            }
        }
        if (task == NULL) {
            task = llam_try_steal_task(rt, shard);
        }
        if (task == NULL) {
            task = llam_take_overflow_task(rt);
        }

        if (task == NULL) {
            if ((atomic_load(&rt->stop_requested) && !llam_runtime_has_live_tasks(rt)) ||
                llam_runtime_drained(rt)) {
                break;
            }
            llam_idle_wait(shard);
            continue;
        }

        started_ns = llam_set_task_running(shard, task);
        shard->metrics.ctx_switches += 1U;
        llam_switch_scheduler_to_task(g_llam_tls_scheduler_ctx, task);
        task = g_llam_tls_task != NULL ? g_llam_tls_task : task;
        llam_clear_current_task(shard, started_ns != 0U ? llam_now_ns() - started_ns : 0U);
        if (task->state == LLAM_TASK_STATE_DEAD) {
            llam_task_release_stack(task);
            llam_task_mark_reclaim_ready(task);
            llam_try_reclaim_detached_task(rt, task);
        }
    }

    llam_uninstall_thread_signal_stack(&shard->primary_worker);
    llam_scheduler_unbind_identity(shard, &shard->primary_worker);
    llam_channel_tls_cache_drain();
    /*
     * The caller returns to unmanaged host code after driving shard 0. Leaving
     * the shard cursor installed makes later host-side cleanup look like a
     * managed task from the last runtime, which can falsely reject explicit
     * handles from another runtime with EXDEV.
     */
    llam_scheduler_clear_identity();
}

/**
 * @brief Stop helper dispatch and publish its handback acknowledgement.
 *
 * The helper is in its scheduler context when this runs.  It first drops the
 * opaque lock, clears its worker-local active-G publication under the shard
 * lock, and only then reacquires the opaque lock to complete the lease
 * transition.  The test gate is likewise outside both locks so it cannot hide
 * a lock-order violation in a focused transfer test.
 */
static void llam_opaque_helper_complete_handoff(llam_shard_t *shard,
                                                bool stopping) {
    uint64_t sequence = 0U;
    bool acknowledged = false;

    (void)stopping;
    pthread_mutex_lock(&shard->opaque_lock);
    if (shard->legacy_lease.state == LLAM_LEASE_HELPER_OWNED &&
        shard->legacy_lease.owner == &shard->opaque_helper_worker) {
        (void)llam_legacy_lease_request_return_locked(
            shard,
            &shard->primary_worker,
            atomic_load_explicit(&shard->primary_worker.incarnation,
                                 memory_order_acquire),
            &sequence);
    } else if ((shard->legacy_lease.state == LLAM_LEASE_TRANSFER_TO_PRIMARY ||
                shard->legacy_lease.state == LLAM_LEASE_STOPPING) &&
               shard->legacy_lease.return_worker != NULL) {
        sequence = shard->legacy_lease.transfer_sequence;
    }
    pthread_mutex_unlock(&shard->opaque_lock);

    /* No helper task may remain published when the handback is acknowledged. */
    pthread_mutex_lock(&shard->lock);
    llam_worker_clear_active_g(&shard->opaque_helper_worker);
    pthread_mutex_unlock(&shard->lock);

    if (sequence != 0U) {
        pthread_mutex_lock(&shard->opaque_lock);
        acknowledged = llam_legacy_lease_complete_handback_locked(
            shard,
            &shard->opaque_helper_worker,
            sequence);
        pthread_mutex_unlock(&shard->opaque_lock);
        if (acknowledged) {
            llam_legacy_lease_test_pause(LLAM_LEASE_TEST_POINT_AFTER_HANDBACK_ACK);
        }
    }

    pthread_mutex_lock(&shard->opaque_lock);
    shard->opaque_helper_active = false;
    atomic_store_explicit(&shard->opaque_helper_active_hint, 0U, memory_order_release);
    llam_opaque_wake_signal(shard);
    pthread_mutex_unlock(&shard->opaque_lock);
}

/**
 * @brief Opaque-blocking helper scheduler loop.
 *
 * A helper thread is created for shards that need compensation while their
 * primary worker is inside opaque blocking code.  The helper temporarily uses
 * the shard's opaque scheduler context, drains normal scheduler work, and then
 * returns control when compensation is no longer needed.
 *
 * @param arg Scheduler shard pointer.
 * @return NULL when the helper exits.
 */
void *llam_opaque_helper_main(void *arg) {
    llam_shard_t *shard = arg;
    llam_runtime_t *rt = shard->runtime;

    llam_scheduler_bind_identity(shard, &shard->opaque_helper_worker, &shard->opaque_scheduler_ctx);
    llam_bind_current_thread_to_cpu(shard->cpu_id);
    llam_tune_scheduler_thread(shard, true);
    if (llam_scheduler_try_install_signal_stack(&shard->opaque_helper_worker) != 0) {
        pthread_mutex_lock(&shard->opaque_lock);
        shard->opaque_helper_failed = true;
        shard->opaque_helper_ready = false;
        atomic_store_explicit(&shard->opaque_helper_active_hint, 0U, memory_order_release);
        atomic_store_explicit(&shard->opaque_helper_exit_ack, true, memory_order_release);
        llam_opaque_wake_signal(shard);
        pthread_mutex_unlock(&shard->opaque_lock);
        llam_record_fatal(rt, errno);
        llam_scheduler_unbind_identity(shard, &shard->opaque_helper_worker);
        llam_scheduler_clear_identity();
        return NULL;
    }

    pthread_mutex_lock(&shard->opaque_lock);
    shard->opaque_helper_failed = false;
    shard->opaque_helper_ready = true;
    shard->opaque_helper_active = false;
    atomic_store_explicit(&shard->opaque_helper_active_hint, 0U, memory_order_release);
    llam_opaque_wake_signal(shard);
    pthread_mutex_unlock(&shard->opaque_lock);

    for (;;) {
        pthread_mutex_lock(&shard->opaque_lock);
        while (!shard->opaque_helper_stop &&
               (shard->opaque_compensation_depth == 0U ||
                shard->legacy_lease.state != LLAM_LEASE_HELPER_OWNED ||
                shard->legacy_lease.owner != &shard->opaque_helper_worker)) {
            llam_opaque_wake_wait(shard);
        }
        if (shard->opaque_helper_stop) {
            pthread_mutex_unlock(&shard->opaque_lock);
            llam_opaque_helper_complete_handoff(shard, true);
            break;
        }
        shard->opaque_helper_active = true;
        atomic_store_explicit(&shard->opaque_helper_active_hint, 1U, memory_order_release);
        llam_opaque_wake_signal(shard);
        pthread_mutex_unlock(&shard->opaque_lock);

        while (!atomic_load(&rt->stop_requested) || llam_runtime_has_live_tasks(rt)) {
            llam_task_t *task;
            uint64_t started_ns;
            bool pressure;
            bool keep_running;
            bool stop_requested;

            if (llam_runtime_drained(rt)) {
                break;
            }
            if (llam_shard_pause_for_merge(shard)) {
                continue;
            }
            if (atomic_load_explicit(&rt->stop_requested, memory_order_acquire) &&
                llam_runtime_has_live_tasks(rt)) {
                llam_runtime_cancel_parked_waiters(rt);
            }
            llam_allocator_quiescent(shard);
            llam_drain_inject_queue(shard);
            llam_fire_expired_timers(shard);
            pressure = llam_runtime_pressure_signal(rt);

            task = llam_take_local_task_with_pressure(shard, pressure);
            if (task == NULL) {
                if (pressure) {
                    task = llam_take_overflow_task(rt);
                }
            }
            if (task == NULL) {
                if (pressure) {
                    task = llam_try_steal_task(rt, shard);
                }
            }
            if (task == NULL) {
                if (pressure) {
                    task = llam_take_overflow_task(rt);
                }
            }

            if (task == NULL) {
                pthread_mutex_lock(&shard->opaque_lock);
                stop_requested = shard->opaque_helper_stop;
                keep_running = shard->opaque_compensation_depth > 0U;
                if ((!keep_running && shard->legacy_lease.state != LLAM_LEASE_HELPER_OWNED) ||
                    !keep_running || stop_requested) {
                    pthread_mutex_unlock(&shard->opaque_lock);
                    llam_opaque_helper_complete_handoff(shard, stop_requested);
                    break;
                }
#if defined(__linux__)
                if (llam_opaque_helper_can_opaque_wait(shard)) {
                    llam_opaque_helper_wait_for_signal_locked(shard);
                    pthread_mutex_unlock(&shard->opaque_lock);
                    continue;
                }
#endif
                pthread_mutex_unlock(&shard->opaque_lock);

                if ((atomic_load(&rt->stop_requested) && !llam_runtime_has_live_tasks(rt)) ||
                    llam_runtime_drained(rt)) {
                    break;
                }
                llam_idle_wait(shard);
                continue;
            }

            started_ns = llam_set_task_running(shard, task);
            shard->metrics.ctx_switches += 1U;
            llam_switch_scheduler_to_task(g_llam_tls_scheduler_ctx, task);
            task = g_llam_tls_task != NULL ? g_llam_tls_task : task;
            llam_clear_current_task(shard, started_ns != 0U ? llam_now_ns() - started_ns : 0U);
            if (task->state == LLAM_TASK_STATE_DEAD) {
                llam_task_release_stack(task);
                llam_task_mark_reclaim_ready(task);
                llam_try_reclaim_detached_task(rt, task);
            }

            pthread_mutex_lock(&shard->opaque_lock);
            stop_requested = shard->opaque_helper_stop;
            keep_running = shard->opaque_compensation_depth > 0U;
            if (!keep_running || stop_requested) {
                pthread_mutex_unlock(&shard->opaque_lock);
                llam_opaque_helper_complete_handoff(shard, stop_requested);
                if (stop_requested) {
                    goto out;
                }
                break;
            }
            pthread_mutex_unlock(&shard->opaque_lock);
        }
    }

out:
    llam_uninstall_thread_signal_stack(&shard->opaque_helper_worker);
    llam_scheduler_unbind_identity(shard, &shard->opaque_helper_worker);
    llam_channel_tls_cache_drain();
    llam_scheduler_clear_identity();
    pthread_mutex_lock(&shard->opaque_lock);
    shard->opaque_helper_ready = false;
    shard->opaque_helper_active = false;
    atomic_store_explicit(&shard->opaque_helper_active_hint, 0U, memory_order_release);
    llam_opaque_wake_signal(shard);
    atomic_store_explicit(&shard->opaque_helper_exit_ack, true, memory_order_release);
    pthread_mutex_unlock(&shard->opaque_lock);
    llam_legacy_lease_test_pause(LLAM_LEASE_TEST_POINT_AFTER_HELPER_EXIT_ACK);
    return NULL;
}

void *llam_shard_worker_main(void *arg) {
    llam_shard_t *shard = arg;

    llam_scheduler_loop(shard);
    return NULL;
}
