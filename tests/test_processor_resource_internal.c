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

#include "runtime_internal.h"

#include <errno.h>
#if LLAM_PLATFORM_POSIX
#include <pthread.h>
#endif
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if LLAM_PLATFORM_POSIX
#include <fcntl.h>
#endif

static int fail(const char *message) {
    fprintf(stderr, "test_processor_resource_internal: %s\n", message);
    return -1;
}

typedef struct helper_binding {
    llam_shard_t *shard;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    bool started;
    bool ready;
    bool stop;
    bool bound;
} helper_binding_t;

static void *helper_binding_main(void *argument) {
    helper_binding_t *binding = argument;
    llam_worker_t *worker = &binding->shard->opaque_helper_worker;

    /* The primary stays bound on the test's caller. Native-domain identity is
     * one M per pthread, just as it is for the runtime's actual helper. */
    llam_scheduler_bind_identity(binding->shard, worker, &binding->shard->opaque_scheduler_ctx);
    pthread_mutex_lock(&binding->lock);
    binding->bound = g_llam_tls_worker == worker &&
        llam_worker_binding_live(worker, atomic_load_explicit(&worker->binding_incarnation, memory_order_acquire));
    binding->ready = true;
    pthread_cond_broadcast(&binding->changed);
    while (binding->bound && !binding->stop)
        pthread_cond_wait(&binding->changed, &binding->lock);
    pthread_mutex_unlock(&binding->lock);
    if (binding->bound) {
        llam_scheduler_unbind_identity(binding->shard, worker);
        llam_scheduler_clear_identity();
    }
    return NULL;
}

static int helper_binding_start(helper_binding_t *binding) {
    if (binding->started) return fail("helper already has a live test pthread");
    binding->ready = false;
    binding->stop = false;
    binding->bound = false;
    if (pthread_create(&binding->thread, NULL, helper_binding_main, binding) != 0)
        return fail("could not start helper binding pthread");
    binding->started = true;
    pthread_mutex_lock(&binding->lock);
    while (!binding->ready) pthread_cond_wait(&binding->changed, &binding->lock);
    bool bound = binding->bound;
    pthread_mutex_unlock(&binding->lock);
    return bound && pthread_equal(binding->thread, pthread_self()) == 0
        ? 0 : fail("helper pthread did not publish its own native binding");
}

static int helper_binding_stop(helper_binding_t *binding) {
    if (!binding->started) return 0;
    pthread_mutex_lock(&binding->lock);
    binding->stop = true;
    pthread_cond_broadcast(&binding->changed);
    pthread_mutex_unlock(&binding->lock);
    if (pthread_join(binding->thread, NULL) != 0) return fail("could not join helper binding pthread");
    binding->started = false;
    if (atomic_load_explicit(&binding->shard->opaque_helper_worker.native_thread_live, memory_order_acquire))
        return fail("helper pthread did not retire its native binding");
    return 0;
}

static int wake_handle_is_valid(int handle) {
#if LLAM_PLATFORM_POSIX
    errno = 0;
    return fcntl(handle, F_GETFD) >= 0;
#else
    return handle >= 0;
#endif
}

static int lock_owner_pair(llam_shard_t *shard) {
    if (shard == NULL || pthread_mutex_lock(&shard->opaque_lock) != 0) {
        return -1;
    }
    if (pthread_mutex_lock(&shard->lock) != 0) {
        (void)pthread_mutex_unlock(&shard->opaque_lock);
        return -1;
    }
    return 0;
}

static void unlock_owner_pair(llam_shard_t *shard) {
    int shard_rc = pthread_mutex_unlock(&shard->lock);
    int opaque_rc = pthread_mutex_unlock(&shard->opaque_lock);

    if (shard_rc != 0 || opaque_rc != 0) {
        (void)fail("could not release owner/lease lock pair");
    }
}

static bool owner_record_equal(const llam_processor_owner_record_t *left,
                               const llam_processor_owner_record_t *right) {
    return left != NULL && right != NULL && left->owner_m == right->owner_m &&
           left->worker_incarnation == right->worker_incarnation &&
           left->lease_projection_epoch == right->lease_projection_epoch &&
           left->p_generation == right->p_generation &&
           left->lease_state == right->lease_state && left->state == right->state;
}

static bool lease_record_equal(const llam_legacy_lease_t *left,
                              const llam_legacy_lease_t *right) {
    return left != NULL && right != NULL && left->owner == right->owner &&
           left->owner_incarnation == right->owner_incarnation &&
           left->epoch == right->epoch && left->return_worker == right->return_worker &&
           left->return_incarnation == right->return_incarnation &&
           left->transfer_sequence == right->transfer_sequence &&
           left->handback_ack_sequence == right->handback_ack_sequence &&
           left->state == right->state;
}

static int assert_owner_projection(llam_shard_t *shard,
                                   llam_processor_owner_state_t expected_state,
                                   llam_worker_t *expected_owner,
                                   uint64_t expected_incarnation,
                                   bool expect_ticket,
                                   const char *message) {
    llam_processor_owner_record_t record;
    llam_legacy_lease_t lease;
    llam_processor_owner_ticket_t ticket;
    bool ticket_available;

    if (lock_owner_pair(shard) != 0) {
        return fail("could not snapshot lease and P owner under the lock pair");
    }
    record = shard->processor.owner_record;
    lease = shard->legacy_lease;
    unlock_owner_pair(shard);
    if (record.owner_m != expected_owner ||
        record.worker_incarnation != expected_incarnation ||
        record.lease_projection_epoch != lease.epoch ||
        record.lease_state != lease.state || record.state != expected_state ||
        (record.p_generation == 0U && expected_state != LLAM_PROCESSOR_OWNER_FREE)) {
        return fail(message);
    }

    ticket_available = llam_processor_owner_ticket_snapshot(&shard->processor, &ticket);
    if (ticket_available != expect_ticket ||
        (ticket_available &&
         (!llam_processor_owner_ticket_validate(&shard->processor, &ticket) ||
          ticket.owner_m != expected_owner ||
          ticket.worker_incarnation != expected_incarnation ||
          ticket.lease_projection_epoch != lease.epoch))) {
        return fail("P ticket did not agree with the current lease lifecycle");
    }
    return 0;
}

static int assert_ticket_stale_variants_rejected(
    llam_processor_t *processor,
    const llam_processor_owner_ticket_t *ticket,
    llam_worker_t *other_worker) {
    llam_processor_owner_ticket_t stale;

    stale = *ticket;
    stale.worker_incarnation += 1U;
    if (llam_processor_owner_ticket_validate(processor, &stale)) {
        return fail("ticket validator accepted a stale worker incarnation");
    }
    stale = *ticket;
    stale.lease_projection_epoch += 1U;
    if (llam_processor_owner_ticket_validate(processor, &stale)) {
        return fail("ticket validator accepted a stale lease epoch");
    }
    stale = *ticket;
    stale.p_generation += 1U;
    if (llam_processor_owner_ticket_validate(processor, &stale)) {
        return fail("ticket validator accepted a stale P generation");
    }
    stale = *ticket;
    stale.processor = (const llam_processor_t *)other_worker;
    if (llam_processor_owner_ticket_validate(processor, &stale)) {
        return fail("ticket validator accepted a mismatched processor identity");
    }
    stale = *ticket;
    stale.owner_m = other_worker;
    stale.worker_incarnation = 1U;
    if (llam_processor_owner_ticket_validate(processor, &stale)) {
        return fail("ticket validator accepted a different worker pointer");
    }
    stale = *ticket;
    stale.state = LLAM_PROCESSOR_OWNER_TRANSFERRING;
    if (llam_processor_owner_ticket_validate(processor, &stale)) {
        return fail("ticket validator accepted TRANSFERRING as operational");
    }
    stale = *ticket;
    stale.state = (llam_processor_owner_state_t)99;
    if (llam_processor_owner_ticket_validate(processor, &stale)) {
        return fail("ticket validator accepted an unknown P-owner state");
    }
    stale = *ticket;
    stale.lease_state = (llam_legacy_lease_state_t)99;
    if (llam_processor_owner_ticket_validate(processor, &stale)) {
        return fail("ticket validator accepted an unknown lease state");
    }
    return 0;
}

static int test_malformed_current_owner_rejected(
    llam_shard_t *shard,
    const llam_processor_owner_ticket_t *ticket) {
    llam_processor_owner_record_t original;
    llam_processor_owner_record_t malformed;
    llam_processor_owner_record_t after;
    llam_processor_owner_ticket_t rejected_ticket;
    llam_legacy_lease_t original_lease;
    llam_legacy_lease_t malformed_lease;
    llam_legacy_lease_t after_lease;
    unsigned mutation;

    if (lock_owner_pair(shard) != 0) {
        return fail("could not lock owner record before malformed-current checks");
    }
    original = shard->processor.owner_record;
    original_lease = shard->legacy_lease;
    unlock_owner_pair(shard);
    for (mutation = 0U; mutation < 6U; ++mutation) {
        if (lock_owner_pair(shard) != 0) {
            return fail("could not lock owner record for malformed-current check");
        }
        shard->processor.owner_record = original;
        switch (mutation) {
            case 0U:
                shard->processor.owner_record.owner_m = NULL;
                break;
            case 1U:
                shard->processor.owner_record.lease_projection_epoch += 1U;
                break;
            case 2U:
                shard->processor.owner_record.state = LLAM_PROCESSOR_OWNER_TRANSFERRING;
                break;
            case 3U:
                shard->processor.owner_record.state = (llam_processor_owner_state_t)99;
                break;
            case 4U:
                shard->processor.owner_record.owner_m = &shard->opaque_helper_worker;
                shard->processor.owner_record.worker_incarnation = 1U;
                shard->processor.owner_record.state = LLAM_PROCESSOR_OWNER_FREE;
                break;
            default:
                shard->processor.owner_record.owner_m = (llam_worker_t *)(uintptr_t)1U;
                shard->processor.owner_record.worker_incarnation = 1U;
                shard->legacy_lease.owner = (llam_worker_t *)(uintptr_t)1U;
                shard->legacy_lease.owner_incarnation = 1U;
                break;
        }
        malformed = shard->processor.owner_record;
        malformed_lease = shard->legacy_lease;
        if (mutation != 1U && mutation != 5U &&
            llam_processor_owner_compose_pair_locked(shard)) {
            unlock_owner_pair(shard);
            return fail("composer accepted a malformed current owner tuple");
        }
        if (!owner_record_equal(&shard->processor.owner_record, &malformed)) {
            unlock_owner_pair(shard);
            return fail("failed composition mutated a malformed current tuple");
        }
        unlock_owner_pair(shard);

        if (llam_processor_owner_ticket_snapshot(&shard->processor, &rejected_ticket) ||
            llam_processor_owner_ticket_validate(&shard->processor, ticket)) {
            return fail("ticket validator accepted an uncomposed or malformed current tuple");
        }
        if (!llam_processor_owner_snapshot(&shard->processor, &after) ||
            !owner_record_equal(&after, &malformed)) {
            return fail("ticket validation mutated a malformed current owner record");
        }
        if (lock_owner_pair(shard) != 0) {
            return fail("could not recheck lease after malformed-current validation");
        }
        after_lease = shard->legacy_lease;
        unlock_owner_pair(shard);
        if (!lease_record_equal(&after_lease, &malformed_lease)) {
            return fail("ticket validation mutated a malformed current lease tuple");
        }
        if (mutation == 0U) {
            uint64_t rejected_sequence = 777U;
            bool reserved;

            pthread_mutex_lock(&shard->opaque_lock);
            reserved = llam_legacy_lease_request_helper_locked(
                shard, &shard->primary_worker, &rejected_sequence);
            pthread_mutex_unlock(&shard->opaque_lock);
            if (reserved || rejected_sequence != 0U ||
                lock_owner_pair(shard) != 0) {
                return fail("failed helper composition leaked a transfer token");
            }
            after = shard->processor.owner_record;
            after_lease = shard->legacy_lease;
            unlock_owner_pair(shard);
            if (!owner_record_equal(&after, &malformed) ||
                !lease_record_equal(&after_lease, &malformed_lease)) {
                return fail("failed helper composition did not restore both state domains");
            }
        }
    }
    if (lock_owner_pair(shard) != 0) {
        return fail("could not restore owner record after malformed-current checks");
    }
    shard->processor.owner_record = original;
    shard->legacy_lease = original_lease;
    unlock_owner_pair(shard);
    return 0;
}

typedef struct online_ticket_race {
    llam_shard_t *shard;
    llam_processor_owner_ticket_t primary_ticket;
    atomic_uint ready;
    atomic_uint start;
    atomic_uint violations;
} online_ticket_race_t;

static void wait_for_race_start(online_ticket_race_t *race) {
    atomic_fetch_add_explicit(&race->ready, 1U, memory_order_release);
    while (atomic_load_explicit(&race->start, memory_order_acquire) == 0U) {
        atomic_signal_fence(memory_order_seq_cst);
    }
}

static void *online_toggle_racer(void *arg) {
    online_ticket_race_t *race = arg;
    unsigned i;

    wait_for_race_start(race);
    for (i = 0U; i < 256U; ++i) {
        if (lock_owner_pair(race->shard) != 0) {
            atomic_fetch_add_explicit(&race->violations, 1U, memory_order_relaxed);
            break;
        }
        llam_processor_set_online(&race->shard->processor, false);
        unlock_owner_pair(race->shard);
        if (lock_owner_pair(race->shard) != 0) {
            atomic_fetch_add_explicit(&race->violations, 1U, memory_order_relaxed);
            break;
        }
        llam_processor_set_online(&race->shard->processor, true);
        unlock_owner_pair(race->shard);
    }
    return NULL;
}

static void *online_ticket_probe_racer(void *arg) {
    online_ticket_race_t *race = arg;
    llam_processor_t *processor = &race->shard->processor;
    llam_processor_owner_ticket_t candidate;
    unsigned i;

    wait_for_race_start(race);
    for (i = 0U; i < 512U; ++i) {
        uint64_t offline_sequence = 0U;

        candidate = race->primary_ticket;
        if (llam_processor_owner_ticket_validate(processor, &candidate) &&
            (candidate.owner_m != &race->shard->primary_worker ||
             candidate.worker_incarnation != race->primary_ticket.worker_incarnation)) {
            atomic_fetch_add_explicit(&race->violations, 1U, memory_order_relaxed);
        }
        candidate.owner_m = &race->shard->opaque_helper_worker;
        if (llam_processor_owner_ticket_validate(processor, &candidate)) {
            atomic_fetch_add_explicit(&race->violations, 1U, memory_order_relaxed);
        }
        pthread_mutex_lock(&race->shard->opaque_lock);
        if (!llam_processor_online(processor) &&
            llam_legacy_lease_request_helper_locked(
                race->shard,
                &race->shard->primary_worker,
                &offline_sequence)) {
            atomic_fetch_add_explicit(&race->violations, 1U, memory_order_relaxed);
        }
        if (llam_legacy_lease_grant_helper_locked(
                race->shard,
                &race->shard->opaque_helper_worker,
                offline_sequence != 0U ? offline_sequence : 1U)) {
            atomic_fetch_add_explicit(&race->violations, 1U, memory_order_relaxed);
        }
        pthread_mutex_unlock(&race->shard->opaque_lock);
    }
    return NULL;
}

static int test_offline_ticket_race(llam_shard_t *shard,
                                    const llam_processor_owner_ticket_t *ticket) {
    online_ticket_race_t race;
    llam_processor_owner_ticket_t offline_ticket;
    pthread_t toggler;
    pthread_t probe;
    uint64_t sequence = 0U;
    bool transitioned;
    bool grant_succeeded;

    if (lock_owner_pair(shard) != 0) {
        return fail("could not lock online state for offline-ticket check");
    }
    llam_processor_set_online(&shard->processor, false);
    unlock_owner_pair(shard);
    if (llam_processor_owner_ticket_snapshot(&shard->processor, &offline_ticket) ||
        llam_processor_owner_ticket_validate(&shard->processor, ticket) ||
        assert_owner_projection(shard,
                                LLAM_PROCESSOR_OWNER_OWNED,
                                &shard->primary_worker,
                                ticket->worker_incarnation,
                                false,
                                "offline processor did not retain its owner reservation") != 0) {
        return fail("offline processor issued an execution ticket or lost its reservation");
    }

    memset(&race, 0, sizeof(race));
    race.shard = shard;
    race.primary_ticket = *ticket;
    atomic_init(&race.ready, 0U);
    atomic_init(&race.start, 0U);
    atomic_init(&race.violations, 0U);
    if (pthread_create(&toggler, NULL, online_toggle_racer, &race) != 0) {
        llam_processor_set_online(&shard->processor, true);
        errno = EAGAIN;
        return fail("could not start offline/online transition racer");
    }
    if (pthread_create(&probe, NULL, online_ticket_probe_racer, &race) != 0) {
        atomic_store_explicit(&race.start, 1U, memory_order_release);
        (void)pthread_join(toggler, NULL);
        llam_processor_set_online(&shard->processor, true);
        errno = EAGAIN;
        return fail("could not start ticket validation racer");
    }
    while (atomic_load_explicit(&race.ready, memory_order_acquire) != 2U) {
        atomic_signal_fence(memory_order_seq_cst);
    }
    atomic_store_explicit(&race.start, 1U, memory_order_release);
    if (pthread_join(toggler, NULL) != 0 || pthread_join(probe, NULL) != 0) {
        return fail("could not join offline/online ticket racers");
    }
    if (lock_owner_pair(shard) != 0) {
        return fail("could not restore processor online state after race");
    }
    llam_processor_set_online(&shard->processor, true);
    unlock_owner_pair(shard);
    if (atomic_load_explicit(&race.violations, memory_order_acquire) != 0U ||
        !llam_processor_owner_ticket_validate(&shard->processor, ticket)) {
        return fail("offline/online race issued a second owner ticket or lost the reservation");
    }

    pthread_mutex_lock(&shard->opaque_lock);
    transitioned = llam_legacy_lease_request_helper_locked(
        shard, &shard->primary_worker, &sequence);
    pthread_mutex_unlock(&shard->opaque_lock);
    if (!transitioned || sequence == 0U) {
        return fail("online primary could not reserve its ordinary helper transfer");
    }
    if (lock_owner_pair(shard) != 0) {
        return fail("could not publish offline state during helper grant race");
    }
    llam_processor_set_online(&shard->processor, false);
    unlock_owner_pair(shard);
    pthread_mutex_lock(&shard->opaque_lock);
    transitioned = llam_legacy_lease_clear_owner_locked(
        shard, &shard->primary_worker, ticket->worker_incarnation, sequence);
    pthread_mutex_unlock(&shard->opaque_lock);
    if (!transitioned ||
        assert_owner_projection(shard,
                                LLAM_PROCESSOR_OWNER_FREE,
                                NULL,
                                0U,
                                false,
                                "offline owner clear did not retire the primary reservation") != 0) {
        return fail("offline helper grant setup did not clear the primary first");
    }
    pthread_mutex_lock(&shard->opaque_lock);
    grant_succeeded = llam_legacy_lease_grant_helper_locked(
        shard, &shard->opaque_helper_worker, sequence);
    pthread_mutex_unlock(&shard->opaque_lock);
    if (grant_succeeded) {
        return fail("offline processor granted a new helper M reservation");
    }
    pthread_mutex_lock(&shard->opaque_lock);
    transitioned = llam_legacy_lease_rollback_helper_locked(
        shard, &shard->primary_worker, ticket->worker_incarnation, sequence);
    pthread_mutex_unlock(&shard->opaque_lock);
    if (!transitioned) {
        return fail("offline rejected helper grant did not roll back to its live primary");
    }
    if (lock_owner_pair(shard) != 0) {
        return fail("could not reactivate processor after rejected offline grant");
    }
    llam_processor_set_online(&shard->processor, true);
    unlock_owner_pair(shard);
    if (!llam_processor_owner_ticket_snapshot(&shard->processor, &offline_ticket) ||
        !llam_processor_owner_ticket_validate(&shard->processor, &offline_ticket) ||
        llam_processor_owner_ticket_validate(&shard->processor, ticket) ||
        assert_owner_projection(shard,
                                LLAM_PROCESSOR_OWNER_OWNED,
                                &shard->primary_worker,
                                ticket->worker_incarnation,
                                true,
                                "offline rollback did not restore the single primary reservation") != 0) {
        return fail("offline helper claim race did not restore the primary P ticket");
    }
    return 0;
}

typedef struct stop_claim_race {
    llam_shard_t *shard;
    uint64_t incarnation;
    uint64_t sequence;
    bool claim_succeeded;
    atomic_uint ready;
    atomic_uint start;
} stop_claim_race_t;

static void *lease_claim_racer(void *arg) {
    stop_claim_race_t *race = arg;

    atomic_fetch_add_explicit(&race->ready, 1U, memory_order_release);
    while (atomic_load_explicit(&race->start, memory_order_acquire) == 0U) {
        atomic_signal_fence(memory_order_seq_cst);
    }
    pthread_mutex_lock(&race->shard->opaque_lock);
    race->claim_succeeded = llam_legacy_lease_request_helper_locked(
        race->shard,
        &race->shard->primary_worker,
        &race->sequence);
    pthread_mutex_unlock(&race->shard->opaque_lock);
    return NULL;
}

static void *lease_stop_racer(void *arg) {
    stop_claim_race_t *race = arg;

    atomic_fetch_add_explicit(&race->ready, 1U, memory_order_release);
    while (atomic_load_explicit(&race->start, memory_order_acquire) == 0U) {
        atomic_signal_fence(memory_order_seq_cst);
    }
    pthread_mutex_lock(&race->shard->opaque_lock);
    llam_legacy_lease_begin_stop_locked(race->shard);
    pthread_mutex_unlock(&race->shard->opaque_lock);
    return NULL;
}

static int test_stop_vs_claim(llam_shard_t *shard, uint64_t incarnation) {
    stop_claim_race_t race;
    llam_processor_owner_ticket_t ticket;
    pthread_t claim_thread;
    pthread_t stop_thread;
    bool grant_succeeded;

    memset(&race, 0, sizeof(race));
    race.shard = shard;
    race.incarnation = incarnation;
    atomic_init(&race.ready, 0U);
    atomic_init(&race.start, 0U);
    if (pthread_create(&claim_thread, NULL, lease_claim_racer, &race) != 0) {
        return fail("could not start stop-versus-claim claimant");
    }
    if (pthread_create(&stop_thread, NULL, lease_stop_racer, &race) != 0) {
        atomic_store_explicit(&race.start, 1U, memory_order_release);
        (void)pthread_join(claim_thread, NULL);
        return fail("could not start stop-versus-claim race");
    }
    while (atomic_load_explicit(&race.ready, memory_order_acquire) != 2U) {
        atomic_signal_fence(memory_order_seq_cst);
    }
    atomic_store_explicit(&race.start, 1U, memory_order_release);
    if (pthread_join(claim_thread, NULL) != 0 || pthread_join(stop_thread, NULL) != 0) {
        return fail("could not join stop-versus-claim race");
    }
    pthread_mutex_lock(&shard->opaque_lock);
    grant_succeeded = race.claim_succeeded &&
                      llam_legacy_lease_grant_helper_locked(
                          shard,
                          &shard->opaque_helper_worker,
                          race.sequence);
    pthread_mutex_unlock(&shard->opaque_lock);
    if (grant_succeeded ||
        assert_owner_projection(shard,
                                LLAM_PROCESSOR_OWNER_STOPPING,
                                &shard->primary_worker,
                                incarnation,
                                false,
                                "STOPPING did not retain only the draining primary reservation") != 0 ||
        llam_processor_owner_ticket_snapshot(&shard->processor, &ticket)) {
        return fail("STOPPING-versus-claim race granted an execution P ticket");
    }
    return 0;
}

static int test_stopping_idempotent_return_failure_output(
    llam_shard_t *shard,
    uint64_t primary_incarnation,
    helper_binding_t *helper) {
    llam_processor_owner_record_t original_owner;
    llam_processor_owner_record_t malformed_owner;
    llam_processor_owner_record_t after_owner;
    llam_legacy_lease_t original_lease;
    llam_legacy_lease_t stopping_lease;
    llam_legacy_lease_t after_lease;
    uint64_t sequence = 0U;
    uint64_t sequence_out = 0U;
    bool helper_bound = false;
    bool transitioned = false;
    int result = -1;

    if (shard == NULL || primary_incarnation == 0U || lock_owner_pair(shard) != 0) {
        return fail("could not snapshot state before STOPPING return-token test");
    }
    original_owner = shard->processor.owner_record;
    original_lease = shard->legacy_lease;
    unlock_owner_pair(shard);

    if (helper_binding_start(helper) != 0) goto cleanup;
    helper_bound = true;

    pthread_mutex_lock(&shard->opaque_lock);
    transitioned = llam_legacy_lease_request_helper_locked(
        shard, &shard->primary_worker, &sequence);
    pthread_mutex_unlock(&shard->opaque_lock);
    if (!transitioned || sequence == 0U) {
        fail("could not reserve helper transfer for STOPPING return-token test");
        goto cleanup;
    }
    pthread_mutex_lock(&shard->opaque_lock);
    transitioned = llam_legacy_lease_clear_owner_locked(
        shard, &shard->primary_worker, primary_incarnation, sequence);
    pthread_mutex_unlock(&shard->opaque_lock);
    if (!transitioned) {
        fail("could not clear primary for STOPPING return-token test");
        goto cleanup;
    }
    pthread_mutex_lock(&shard->opaque_lock);
    transitioned = llam_legacy_lease_grant_helper_locked(
        shard, &shard->opaque_helper_worker, sequence);
    pthread_mutex_unlock(&shard->opaque_lock);
    if (!transitioned) {
        fail("could not grant helper for STOPPING return-token test");
        goto cleanup;
    }
    pthread_mutex_lock(&shard->opaque_lock);
    transitioned = llam_legacy_lease_request_return_locked(
        shard, &shard->primary_worker, primary_incarnation, &sequence);
    pthread_mutex_unlock(&shard->opaque_lock);
    if (!transitioned) {
        fail("could not reserve primary return for STOPPING return-token test");
        goto cleanup;
    }
    pthread_mutex_lock(&shard->opaque_lock);
    llam_legacy_lease_begin_stop_locked(shard);
    pthread_mutex_unlock(&shard->opaque_lock);

    if (lock_owner_pair(shard) != 0) {
        fail("could not lock state for malformed STOPPING owner injection");
        goto cleanup;
    }
    stopping_lease = shard->legacy_lease;
    shard->processor.owner_record.state = LLAM_PROCESSOR_OWNER_FREE;
    shard->processor.owner_record.owner_m = &shard->primary_worker;
    shard->processor.owner_record.worker_incarnation = primary_incarnation;
    malformed_owner = shard->processor.owner_record;
    unlock_owner_pair(shard);

    pthread_mutex_lock(&shard->opaque_lock);
    transitioned = llam_legacy_lease_request_return_locked(
        shard, &shard->primary_worker, primary_incarnation, &sequence_out);
    pthread_mutex_unlock(&shard->opaque_lock);
    if (transitioned || sequence_out != 0U) {
        fail("failed idempotent STOPPING return leaked a sequence token");
        goto cleanup;
    }
    if (lock_owner_pair(shard) == 0) {
        after_owner = shard->processor.owner_record;
        after_lease = shard->legacy_lease;
        unlock_owner_pair(shard);
        if (owner_record_equal(&after_owner, &malformed_owner) &&
            lease_record_equal(&after_lease, &stopping_lease)) {
            result = 0;
        } else {
            fail("failed idempotent STOPPING return mutated lease/P state");
        }
    } else {
        fail("could not verify STOPPING return-token rollback state");
    }

cleanup:
    if (lock_owner_pair(shard) == 0) {
        shard->processor.owner_record = original_owner;
        shard->legacy_lease = original_lease;
        unlock_owner_pair(shard);
    } else {
        result = -1;
    }
    if (helper_bound && helper_binding_stop(helper) != 0) result = -1;
    return result;
}

static int test_owner_ticket_lifecycle_bound(llam_runtime_t *runtime,
                                             llam_shard_t *shard,
                                             helper_binding_t *helper) {
    llam_processor_t *processor = &shard->processor;
    llam_processor_owner_ticket_t primary_ticket;
    llam_processor_owner_ticket_t stale_ticket;
    llam_processor_owner_ticket_t helper_ticket;
    uint64_t primary_incarnation;
    uint64_t helper_incarnation;
    uint64_t sequence = 0U;
    uint64_t new_incarnation;
    bool return_requested;

    if (assert_owner_projection(shard,
                                LLAM_PROCESSOR_OWNER_FREE,
                                NULL,
                                0U,
                                false,
                                "runtime init did not leave an unowned P projection") != 0) {
        return -1;
    }
    llam_scheduler_bind_identity(shard,
                                 &shard->primary_worker,
                                 &shard->scheduler_ctx);
    if (!llam_processor_owner_ticket_snapshot(processor, &primary_ticket) ||
        !llam_processor_owner_ticket_validate(processor, &primary_ticket)) {
        return fail("primary bind did not publish a valid composed P ticket");
    }
    primary_incarnation = atomic_load_explicit(&shard->primary_worker.incarnation,
                                              memory_order_acquire);
    if (primary_ticket.owner_m != &shard->primary_worker ||
        primary_ticket.worker_incarnation != primary_incarnation ||
        primary_ticket.processor != processor ||
        primary_ticket.lease_projection_epoch != 0U ||
        assert_owner_projection(shard,
                                LLAM_PROCESSOR_OWNER_OWNED,
                                &shard->primary_worker,
                                primary_incarnation,
                                true,
                                "primary lease and P projection did not agree") != 0 ||
        assert_ticket_stale_variants_rejected(
            processor,
            &primary_ticket,
            &shard->opaque_helper_worker) != 0 ||
        test_malformed_current_owner_rejected(shard, &primary_ticket) != 0) {
        return -1;
    }

    if (test_offline_ticket_race(shard, &primary_ticket) != 0) {
        return -1;
    }

    pthread_mutex_lock(&shard->opaque_lock);
    return_requested = llam_legacy_lease_request_helper_locked(
        shard, &shard->primary_worker, &sequence);
    pthread_mutex_unlock(&shard->opaque_lock);
    if (!return_requested ||
        assert_owner_projection(shard,
                                LLAM_PROCESSOR_OWNER_OWNED,
                                &shard->primary_worker,
                                primary_incarnation,
                                false,
                                "helper request did not compose its transitional primary tuple") != 0) {
        return fail("helper request published a P ticket during lease transfer");
    }
    pthread_mutex_lock(&shard->opaque_lock);
    return_requested = llam_legacy_lease_clear_owner_locked(
        shard, &shard->primary_worker, primary_incarnation, sequence);
    pthread_mutex_unlock(&shard->opaque_lock);
    if (!return_requested ||
        assert_owner_projection(shard,
                                LLAM_PROCESSOR_OWNER_FREE,
                                NULL,
                                0U,
                                false,
                                "owner clear did not compose an ownerless transfer") != 0) {
        return fail("owner clear left a P ticket during lease transfer");
    }
    pthread_mutex_lock(&shard->opaque_lock);
    return_requested = llam_legacy_lease_rollback_helper_locked(
        shard, &shard->primary_worker, primary_incarnation, sequence);
    pthread_mutex_unlock(&shard->opaque_lock);
    if (!return_requested ||
        !llam_processor_owner_ticket_snapshot(processor, &stale_ticket) ||
        !llam_processor_owner_ticket_validate(processor, &stale_ticket) ||
        stale_ticket.owner_m != &shard->primary_worker ||
        assert_owner_projection(shard,
                                LLAM_PROCESSOR_OWNER_OWNED,
                                &shard->primary_worker,
                                primary_incarnation,
                                true,
                                "rollback did not restore the primary P ticket") != 0) {
        return fail("helper rollback failed to compose the primary owner");
    }
    primary_ticket = stale_ticket;

    pthread_mutex_lock(&shard->opaque_lock);
    return_requested = llam_legacy_lease_request_helper_locked(
        shard, &shard->primary_worker, &sequence);
    pthread_mutex_unlock(&shard->opaque_lock);
    if (!return_requested) {
        return fail("second helper reservation failed");
    }
    pthread_mutex_lock(&shard->opaque_lock);
    return_requested = llam_legacy_lease_clear_owner_locked(
        shard, &shard->primary_worker, primary_incarnation, sequence);
    pthread_mutex_unlock(&shard->opaque_lock);
    if (!return_requested) {
        return fail("could not clear primary and bind helper for grant");
    }
    helper_incarnation = atomic_load_explicit(&shard->opaque_helper_worker.incarnation,
                                              memory_order_acquire);
    pthread_mutex_lock(&shard->opaque_lock);
    return_requested = llam_legacy_lease_grant_helper_locked(
        shard, &shard->opaque_helper_worker, sequence);
    pthread_mutex_unlock(&shard->opaque_lock);
    if (!return_requested ||
        !llam_processor_owner_ticket_snapshot(processor, &helper_ticket) ||
        !llam_processor_owner_ticket_validate(processor, &helper_ticket) ||
        helper_ticket.owner_m != &shard->opaque_helper_worker ||
        helper_ticket.worker_incarnation != helper_incarnation ||
        helper_ticket.lease_projection_epoch == primary_ticket.lease_projection_epoch) {
        return fail("helper grant did not publish its exact new P ticket");
    }
    if (assert_owner_projection(shard,
                                LLAM_PROCESSOR_OWNER_OWNED,
                                &shard->opaque_helper_worker,
                                helper_incarnation,
                                true,
                                "helper lease and P projection did not agree") != 0) {
        return -1;
    }

    {
        llam_processor_owner_record_t original_record;
        llam_processor_owner_record_t malformed_record;
        llam_legacy_lease_t original_lease;
        llam_legacy_lease_t after_lease;
        uint64_t rejected_sequence = 777U;

        if (lock_owner_pair(shard) != 0) {
            return fail("could not lock helper projection for return-token rollback test");
        }
        original_record = shard->processor.owner_record;
        original_lease = shard->legacy_lease;
        shard->processor.owner_record.owner_m = NULL;
        malformed_record = shard->processor.owner_record;
        unlock_owner_pair(shard);

        pthread_mutex_lock(&shard->opaque_lock);
        return_requested = llam_legacy_lease_request_return_locked(
            shard, &shard->primary_worker, primary_incarnation, &rejected_sequence);
        pthread_mutex_unlock(&shard->opaque_lock);
        if (return_requested || rejected_sequence != 0U || lock_owner_pair(shard) != 0) {
            return fail("failed handback composition leaked its transfer token");
        }
        after_lease = shard->legacy_lease;
        if (!owner_record_equal(&shard->processor.owner_record, &malformed_record) ||
            !lease_record_equal(&after_lease, &original_lease)) {
            unlock_owner_pair(shard);
            return fail("failed handback composition did not restore both state domains");
        }
        shard->processor.owner_record = original_record;
        unlock_owner_pair(shard);
    }

    pthread_mutex_lock(&shard->opaque_lock);
    return_requested = llam_legacy_lease_request_return_locked(
        shard, &shard->primary_worker, primary_incarnation, &sequence);
    pthread_mutex_unlock(&shard->opaque_lock);
    if (!return_requested ||
        llam_processor_owner_ticket_validate(processor, &helper_ticket) ||
        assert_owner_projection(shard,
                                LLAM_PROCESSOR_OWNER_OWNED,
                                &shard->opaque_helper_worker,
                                helper_incarnation,
                                false,
                                "helper return request did not invalidate ordinary tickets") != 0) {
        return fail("helper return request left an ordinary execution ticket");
    }
    pthread_mutex_lock(&shard->opaque_lock);
    return_requested = llam_legacy_lease_complete_handback_locked(
        shard, &shard->opaque_helper_worker, sequence);
    pthread_mutex_unlock(&shard->opaque_lock);
    if (!return_requested ||
        !llam_processor_owner_ticket_snapshot(processor, &primary_ticket) ||
        !llam_processor_owner_ticket_validate(processor, &primary_ticket) ||
        primary_ticket.owner_m != &shard->primary_worker ||
        primary_ticket.worker_incarnation != primary_incarnation) {
        return fail("primary handback ACK did not publish its new P ticket");
    }
    if (helper_binding_stop(helper) != 0) return -1;

    pthread_mutex_lock(&shard->opaque_lock);
    return_requested = llam_legacy_lease_enter_redirect_locked(
        shard, &shard->primary_worker, primary_incarnation);
    pthread_mutex_unlock(&shard->opaque_lock);
    if (!return_requested ||
        assert_owner_projection(shard,
                                LLAM_PROCESSOR_OWNER_FREE,
                                NULL,
                                0U,
                                false,
                                "ownerless redirect retained an operational P owner") != 0 ||
        llam_processor_owner_ticket_validate(processor, &primary_ticket)) {
        return fail("ownerless redirect issued a P ticket");
    }
    pthread_mutex_lock(&shard->opaque_lock);
    if (llam_legacy_lease_request_return_locked(
            shard, &shard->primary_worker, primary_incarnation + 1U, &sequence)) {
        pthread_mutex_unlock(&shard->opaque_lock);
        return fail("ownerless redirect accepted the claimant's stale incarnation");
    }
    return_requested = llam_legacy_lease_request_return_locked(
        shard, &shard->primary_worker, primary_incarnation, &sequence);
    pthread_mutex_unlock(&shard->opaque_lock);
    if (!return_requested ||
        assert_owner_projection(shard,
                                LLAM_PROCESSOR_OWNER_FREE,
                                NULL,
                                0U,
                                false,
                                "claimant return request exposed a ticket before reacquire") != 0) {
        return fail("redirect claimant could not reserve its exact handback");
    }
    pthread_mutex_lock(&shard->opaque_lock);
    return_requested = llam_legacy_lease_complete_handback_locked(
        shard, &shard->primary_worker, sequence);
    pthread_mutex_unlock(&shard->opaque_lock);
    if (!return_requested ||
        !llam_processor_owner_ticket_snapshot(processor, &primary_ticket) ||
        !llam_processor_owner_ticket_validate(processor, &primary_ticket)) {
        return fail("exact redirect claimant did not reacquire the P ticket");
    }

    stale_ticket = primary_ticket;
    llam_scheduler_unbind_identity(shard, &shard->primary_worker);
    if (assert_owner_projection(shard,
                                LLAM_PROCESSOR_OWNER_FREE,
                                NULL,
                                0U,
                                false,
                                "primary unbind left a dereferenceable P owner") != 0 ||
        llam_processor_owner_ticket_validate(processor, &stale_ticket)) {
        return fail("primary unbind retained an executable P ticket");
    }
    llam_scheduler_bind_identity(shard,
                                 &shard->primary_worker,
                                 &shard->scheduler_ctx);
    new_incarnation = atomic_load_explicit(&shard->primary_worker.incarnation,
                                          memory_order_acquire);
    if (new_incarnation == stale_ticket.worker_incarnation ||
        !llam_processor_owner_ticket_snapshot(processor, &primary_ticket) ||
        !llam_processor_owner_ticket_validate(processor, &primary_ticket) ||
        primary_ticket.owner_m != stale_ticket.owner_m ||
        primary_ticket.worker_incarnation == stale_ticket.worker_incarnation ||
        llam_processor_owner_ticket_validate(processor, &stale_ticket)) {
        return fail("same worker address with a new incarnation revived a stale ticket");
    }

    if (test_stopping_idempotent_return_failure_output(shard, new_incarnation, helper) != 0) {
        return -1;
    }

    if (test_stop_vs_claim(shard, new_incarnation) != 0) {
        return -1;
    }
    llam_scheduler_unbind_identity(shard, &shard->primary_worker);
    if (assert_owner_projection(shard,
                                LLAM_PROCESSOR_OWNER_STOPPING,
                                &shard->primary_worker,
                                new_incarnation,
                                false,
                                "STOPPING unbind did not retain the stable drain reservation") != 0) {
        return -1;
    }
    pthread_mutex_lock(&shard->opaque_lock);
    llam_legacy_lease_finalize_stop_locked(shard);
    pthread_mutex_unlock(&shard->opaque_lock);
    llam_scheduler_clear_identity();
    if (assert_owner_projection(shard,
                                LLAM_PROCESSOR_OWNER_STOPPING,
                                NULL,
                                0U,
                                false,
                                "runtime finalization retained a worker pointer") != 0 ||
        runtime == NULL) {
        return fail("final stop did not leave a terminal ownerless projection");
    }
    return 0;
}

static int test_owner_ticket_lifecycle(llam_runtime_t *runtime, llam_shard_t *shard) {
    helper_binding_t helper = {.shard = shard};
    int result = -1;
    if (pthread_mutex_init(&helper.lock, NULL) != 0) return fail("helper gate mutex init failed");
    if (pthread_cond_init(&helper.changed, NULL) != 0) {
        pthread_mutex_destroy(&helper.lock);
        return fail("helper gate condition init failed");
    }
    if (helper_binding_start(&helper) == 0)
        result = test_owner_ticket_lifecycle_bound(runtime, shard, &helper);
    if (helper_binding_stop(&helper) != 0) result = -1;
    if (g_llam_tls_worker == &shard->primary_worker) {
        llam_scheduler_unbind_identity(shard, &shard->primary_worker);
        llam_scheduler_clear_identity();
    }
    pthread_cond_destroy(&helper.changed);
    pthread_mutex_destroy(&helper.lock);
    return result;
}

static int test_processor_resources(void) {
    llam_runtime_opts_t opts;
    llam_runtime_t *runtime = NULL;
    llam_shard_t *shard;
    llam_processor_t *processor;
    llam_processor_t *identity;
    llam_processor_owner_record_t runtime_owner;
    llam_processor_owner_ticket_t initial_ticket;
    llam_runtime_t *owner_runtime;
    llam_shard_t *compat_shard;
    unsigned identity_id;
    unsigned cpu_id;
    unsigned node_index;
    unsigned io_node_index;
    int wake_handle;
    atomic_uint *pending;

    if (llam_runtime_opts_init(&opts, sizeof(opts)) != 0) {
        return fail("runtime options initialization failed");
    }
    opts.deterministic = 1U;
    opts.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    if (llam_runtime_create_compat(&opts, sizeof(opts), &runtime) != 0 || runtime == NULL ||
        runtime->shards == NULL || runtime->active_shards != 1U) {
        llam_runtime_destroy(runtime);
        return fail("could not create one-shard test runtime");
    }

    shard = &runtime->shards[0];
    processor = &shard->processor;
    identity = processor;
    owner_runtime = processor->runtime;
    compat_shard = processor->compat_shard;
    identity_id = processor->id;
    cpu_id = llam_processor_cpu_id(processor);
    node_index = llam_processor_node_index(processor);
    io_node_index = llam_processor_io_node_index(processor);
    wake_handle = llam_processor_event_fd(processor);
    pending = llam_processor_event_pending(processor);

    if (owner_runtime != runtime || compat_shard != shard || identity_id != shard->id ||
        cpu_id != runtime->allowed_cpus[0] || node_index >= runtime->active_nodes ||
        io_node_index >= runtime->active_nodes || !llam_processor_online(processor) ||
        wake_handle < 0 || pending == NULL || !wake_handle_is_valid(wake_handle)) {
        llam_runtime_destroy(runtime);
        return fail("processor accessors did not expose initialized P topology and wake state");
    }

    atomic_store_explicit(pending, 0U, memory_order_release);
    llam_kick_shard(shard);
    if (atomic_load_explicit(pending, memory_order_acquire) == 0U) {
        llam_runtime_destroy(runtime);
        return fail("first scheduler wake did not claim the coalescing bit");
    }
    llam_kick_shard(shard);
    if (atomic_load_explicit(pending, memory_order_acquire) != 1U) {
        llam_runtime_destroy(runtime);
        return fail("coalesced scheduler wake changed the pending bit");
    }
    llam_drain_shard_wake(shard);
    if (atomic_load_explicit(pending, memory_order_acquire) != 0U) {
        llam_runtime_destroy(runtime);
        return fail("scheduler wake drain did not clear the pending bit");
    }

    llam_processor_set_online(processor, false);
    if (llam_processor_online(processor) ||
        llam_processor_event_fd(processor) != wake_handle ||
        !wake_handle_is_valid(llam_processor_event_fd(processor))) {
        llam_runtime_destroy(runtime);
        return fail("going offline closed or replaced the processor wake descriptor");
    }
    llam_processor_set_online(processor, true);
    if (!llam_processor_online(processor) ||
        llam_processor_event_fd(processor) != wake_handle ||
        !wake_handle_is_valid(llam_processor_event_fd(processor))) {
        llam_runtime_destroy(runtime);
        return fail("reactivation did not preserve the processor wake descriptor");
    }

    if (&shard->processor != identity || processor->runtime != owner_runtime ||
        processor->compat_shard != compat_shard || processor->id != identity_id) {
        llam_runtime_destroy(runtime);
        return fail("P identity or compatibility fields changed during lifecycle transitions");
    }
    if (!llam_processor_owner_snapshot(processor, &runtime_owner) ||
        runtime_owner.owner_m != NULL || runtime_owner.worker_incarnation != 0U ||
        runtime_owner.lease_projection_epoch != 0U || runtime_owner.p_generation != 0U ||
        runtime_owner.lease_state != LLAM_LEASE_REDIRECTED_NO_EXECUTOR ||
        runtime_owner.state != LLAM_PROCESSOR_OWNER_FREE ||
        llam_processor_owner_ticket_snapshot(processor, &initial_ticket)) {
        llam_runtime_destroy(runtime);
        return fail("runtime initialization published an unbound P ticket");
    }
    if (test_owner_ticket_lifecycle(runtime, shard) != 0) {
        llam_runtime_destroy(runtime);
        return -1;
    }

    llam_runtime_destroy(runtime);
#if LLAM_PLATFORM_POSIX
    if (wake_handle_is_valid(wake_handle)) {
        return fail("runtime teardown left the processor wake descriptor open");
    }
#endif
    return 0;
}

int main(void) {
    if (test_processor_resources() != 0) {
        return 1;
    }
    puts("test_processor_resource_internal: ownership and wake checks passed");
    return 0;
}
