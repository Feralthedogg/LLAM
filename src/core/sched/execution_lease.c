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

/* Registry membership is not native-context/ISA qualification for async capture. */
static bool llam_execution_lease_native_owner(const llam_shard_t *shard,
                                             const llam_worker_t *worker,
                                             uint64_t incarnation) {
    return shard != NULL && shard->runtime != NULL &&
           shard->runtime->execution_lease_mode == LLAM_EXECUTION_LEASE_REGISTERED &&
           llam_worker_registered_for(worker, shard->runtime) &&
           g_llam_tls_worker == worker && g_llam_tls_task == NULL &&
           llam_worker_load_active_g(worker) == NULL &&
           llam_worker_binding_live(worker, incarnation) &&
           pthread_equal(worker->native_thread, pthread_self()) != 0;
}

bool llam_execution_lease_claim_locked(llam_shard_t *shard,
                                      llam_worker_t *worker,
                                      uint64_t incarnation) {
    llam_legacy_lease_t *lease;
    llam_legacy_lease_t before;
    llam_processor_owner_record_t record;
    llam_processor_t *expected_p = NULL;
    bool committed = false;

    if (!llam_execution_lease_native_owner(shard, worker, incarnation) ||
        !shard->lock_initialized || !llam_processor_online(&shard->processor) ||
        atomic_load_explicit(&shard->runtime->shutdown_requested, memory_order_acquire) ||
        atomic_load_explicit(&shard->merge_pause_requested, memory_order_acquire) != 0U ||
        atomic_load_explicit(&worker->execution_state, memory_order_acquire) !=
            LLAM_WORKER_EXEC_SCHEDULER) {
        return false;
    }
    pthread_mutex_lock(&shard->lock);
    lease = llam_shard_legacy_lease_locked(shard);
    if (atomic_load_explicit(&shard->runtime->shutdown_requested, memory_order_acquire) ||
        lease->state != LLAM_LEASE_REGISTERED_FREE || lease->owner != NULL ||
        lease->owner_incarnation != 0U || lease->return_worker != NULL ||
        lease->return_incarnation != 0U || lease->epoch >= UINT64_MAX - 1U ||
        !llam_processor_owner_snapshot_pair_locked(&shard->processor, &record) ||
        record.state != LLAM_PROCESSOR_OWNER_FREE || record.owner_m != NULL ||
        record.p_generation >= UINT64_MAX - 1U) {
        goto done;
    }
    /* Reserve this M before publishing any P owner: two P locks are independent. */
    if (!atomic_compare_exchange_strong_explicit(&worker->current_p, &expected_p,
                                                &shard->processor,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        goto done;
    }
    before = *lease;
    lease->owner = worker;
    lease->owner_incarnation = incarnation;
    lease->epoch += 1U;
    lease->state = LLAM_LEASE_REGISTERED_OWNED;
    committed = llam_processor_owner_compose_pair_locked(shard);
    if (!committed) {
        *lease = before;
        atomic_store_explicit(&worker->current_p, NULL, memory_order_release);
    } else {
        g_llam_tls_shard = shard;
        g_llam_tls_processor = &shard->processor;
    }
done:
    pthread_mutex_unlock(&shard->lock);
    return committed;
}

bool llam_execution_lease_claim_drain_locked(llam_shard_t *shard,
                                            llam_worker_t *worker,
                                            uint64_t incarnation) {
    llam_legacy_lease_t *lease;
    llam_legacy_lease_t before;
    llam_processor_owner_record_t record;
    llam_processor_t *expected_p = NULL;
    bool committed = false;

    if (!llam_execution_lease_native_owner(shard, worker, incarnation) ||
        !shard->lock_initialized ||
        !atomic_load_explicit(&shard->runtime->shutdown_requested, memory_order_acquire) ||
        atomic_load_explicit(&worker->execution_state, memory_order_acquire) !=
            LLAM_WORKER_EXEC_SCHEDULER) {
        return false;
    }
    pthread_mutex_lock(&shard->lock);
    lease = llam_shard_legacy_lease_locked(shard);
    if ((lease->state != LLAM_LEASE_STOPPING &&
         lease->state != LLAM_LEASE_REGISTERED_FREE) ||
        lease->owner != NULL || lease->owner_incarnation != 0U ||
        lease->return_worker != NULL || lease->return_incarnation != 0U ||
        lease->epoch >= UINT64_MAX - 1U ||
        !llam_processor_owner_snapshot_pair_locked(&shard->processor, &record) ||
        !llam_processor_owner_record_well_formed(&record) ||
        record.owner_m != NULL || record.worker_incarnation != 0U ||
        record.lease_state != lease->state ||
        record.lease_projection_epoch != lease->epoch ||
        record.state != (lease->state == LLAM_LEASE_STOPPING
                            ? LLAM_PROCESSOR_OWNER_STOPPING : LLAM_PROCESSOR_OWNER_FREE) ||
        record.p_generation >= UINT64_MAX - 1U) {
        goto done;
    }
    /* Each P has independent locks. Reserve current_p with acquire/release before
     * composing either projection, so one native M cannot drain two Ps. */
    if (!atomic_compare_exchange_strong_explicit(&worker->current_p, &expected_p,
                                                &shard->processor,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        goto done;
    }
    before = *lease;
    lease->owner = worker;
    lease->owner_incarnation = incarnation;
    lease->epoch += 1U;
    /* shutdown_requested may precede the shutdown thread's STOPPING projection.
     * Compose that projection under opaque_lock -> lock without reopening normal
     * admission or turning a revoked execution ticket into drain authority. */
    lease->state = LLAM_LEASE_STOPPING;
    committed = llam_processor_owner_compose_pair_locked(shard);
    if (!committed) {
        *lease = before;
        atomic_store_explicit(&worker->current_p, NULL, memory_order_release);
    } else {
        g_llam_tls_shard = shard;
        g_llam_tls_processor = &shard->processor;
    }
done:
    pthread_mutex_unlock(&shard->lock);
    return committed;
}

bool llam_execution_lease_return_locked(llam_shard_t *shard,
                                       llam_worker_t *worker,
                                       const llam_processor_owner_ticket_t *ticket) {
    llam_legacy_lease_t *lease;
    llam_legacy_lease_t before;
    llam_processor_owner_record_t record;
    uint64_t incarnation;
    bool committed = false;

    if (shard == NULL || !llam_worker_registered_for(worker, shard->runtime)) {
        return false;
    }
    incarnation = atomic_load_explicit(&worker->binding_incarnation, memory_order_acquire);
    if (!llam_execution_lease_native_owner(shard, worker, incarnation) ||
        !shard->lock_initialized) {
        return false;
    }
    pthread_mutex_lock(&shard->lock);
    lease = llam_shard_legacy_lease_locked(shard);
    if (lease->owner != worker || lease->owner_incarnation != incarnation ||
        atomic_load_explicit(&worker->current_p, memory_order_acquire) != &shard->processor) {
        goto done;
    }
    if (ticket != NULL) {
        if (lease->state != LLAM_LEASE_REGISTERED_OWNED ||
            !llam_processor_owner_ticket_validate_pair_locked(shard, ticket)) {
            goto done;
        }
    } else {
        /* The stop flag revokes tickets before the shutdown thread can take this
         * P's locks. The exact native owner may close that gap here, with no G
         * active, but an ordinary (possibly stale) ticket never enters this path. */
        if (lease->state == LLAM_LEASE_REGISTERED_OWNED &&
            atomic_load_explicit(&shard->runtime->shutdown_requested, memory_order_acquire)) {
            if (!llam_processor_owner_snapshot_pair_locked(&shard->processor, &record) ||
                !llam_processor_owner_record_well_formed(&record) ||
                record.state != LLAM_PROCESSOR_OWNER_OWNED ||
                record.owner_m != worker || record.worker_incarnation != incarnation ||
                record.lease_state != lease->state ||
                record.lease_projection_epoch != lease->epoch) {
                goto done;
            }
            before = *lease;
            lease->state = LLAM_LEASE_STOPPING;
            if (!llam_processor_owner_compose_pair_locked(shard)) {
                *lease = before;
                goto done;
            }
        }
        /* NULL is a restricted same-M STOPPING drain, never an ordinary grant. */
        if (lease->state != LLAM_LEASE_STOPPING ||
            !atomic_load_explicit(&shard->runtime->shutdown_requested, memory_order_acquire) ||
            !llam_processor_owner_snapshot_pair_locked(&shard->processor, &record) ||
            !llam_processor_owner_record_well_formed(&record) ||
            record.state != LLAM_PROCESSOR_OWNER_STOPPING ||
            record.lease_state != LLAM_LEASE_STOPPING || record.owner_m != worker ||
            record.worker_incarnation != incarnation ||
            record.lease_projection_epoch != lease->epoch) {
            goto done;
        }
    }
    before = *lease;
    lease->owner = NULL;
    lease->owner_incarnation = 0U;
    if (lease->epoch != UINT64_MAX) {
        lease->epoch += 1U;
    }
    if (lease->state != LLAM_LEASE_STOPPING) {
        lease->state = LLAM_LEASE_REGISTERED_FREE;
    }
    committed = llam_processor_owner_compose_pair_locked(shard);
    if (!committed) {
        *lease = before;
    } else {
        g_llam_tls_processor = NULL;
        g_llam_tls_shard = worker->compat_shard;
    }
done:
    pthread_mutex_unlock(&shard->lock);
    return committed;
}
