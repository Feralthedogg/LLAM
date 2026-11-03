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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "test_execution_lease_internal:%d: %s\n", __LINE__, #condition); \
        return -1; \
    } \
} while (0)

static bool claim(llam_shard_t *shard, llam_worker_t *worker, uint64_t incarnation) {
    bool result;
    pthread_mutex_lock(&shard->opaque_lock);
    result = llam_execution_lease_claim_locked(shard, worker, incarnation);
    pthread_mutex_unlock(&shard->opaque_lock);
    return result;
}

static bool claim_drain(llam_shard_t *shard, llam_worker_t *worker,
                        uint64_t incarnation) {
    bool result;
    pthread_mutex_lock(&shard->opaque_lock);
    result = llam_execution_lease_claim_drain_locked(shard, worker, incarnation);
    pthread_mutex_unlock(&shard->opaque_lock);
    return result;
}

static bool release_p(llam_shard_t *shard, llam_worker_t *worker,
                      const llam_processor_owner_ticket_t *ticket) {
    bool result;
    pthread_mutex_lock(&shard->opaque_lock);
    result = llam_execution_lease_return_locked(shard, worker, ticket);
    pthread_mutex_unlock(&shard->opaque_lock);
    return result;
}

typedef struct foreign_native_attempt {
    llam_shard_t *shard;
    llam_worker_t *worker;
    const llam_processor_owner_ticket_t *ticket;
    uint64_t incarnation;
    bool rejected;
} foreign_native_attempt_t;

static void *try_foreign_native(void *argument) {
    foreign_native_attempt_t *attempt = argument;

    /* Even forged matching TLS cannot impersonate the bound native pthread. */
    g_llam_tls_worker = attempt->worker;
    g_llam_tls_shard = attempt->shard;
    g_llam_tls_processor = &attempt->shard->processor;
    attempt->rejected = !claim(attempt->shard, attempt->worker, attempt->incarnation) &&
                        !claim_drain(attempt->shard, attempt->worker, attempt->incarnation) &&
                        !release_p(attempt->shard, attempt->worker, attempt->ticket) &&
                        !release_p(attempt->shard, attempt->worker, NULL);
    g_llam_tls_processor = NULL;
    g_llam_tls_shard = NULL;
    g_llam_tls_worker = NULL;
    return NULL;
}

static int rejects_foreign_native(llam_shard_t *shard, llam_worker_t *worker,
                                  uint64_t incarnation,
                                  const llam_processor_owner_ticket_t *ticket) {
    foreign_native_attempt_t attempt = {shard, worker, ticket, incarnation, false};
    pthread_t thread;

    CHECK(pthread_create(&thread, NULL, try_foreign_native, &attempt) == 0);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(attempt.rejected);
    return 0;
}

static int exercise_managed_publication(llam_shard_t *shard, llam_worker_t *worker) {
    llam_task_t task = {0};
    llam_processor_owner_ticket_t ticket;
    llam_relief_defer_reason_t reason;

    task.owner_runtime = shard->runtime;
    atomic_init(&task.state, LLAM_TASK_STATE_RUNNING);
    atomic_init(&task.run_generation, 0U);
    llam_worker_begin_g_run(worker, &task);
    llam_worker_publish_active_g(worker, &task);
    g_llam_tls_task = &task;
    bool valid = llam_worker_matches_shard(worker, shard) &&
                 llam_worker_has_safe_managed_g(worker, &shard->processor, &task) &&
                 llam_processor_relief_snapshot(shard, worker, &task, &ticket, &reason);
    g_llam_tls_task = NULL;
    llam_worker_clear_active_g(worker);
    CHECK(valid && reason == LLAM_RELIEF_DEFER_NONE);
    return 0;
}

static bool rejects_other_protocol(llam_shard_t *shard,
                                   llam_legacy_lease_state_t other_state) {
    llam_processor_owner_ticket_t ticket;
    llam_processor_queue_access_t access, rejected;
    bool accepted = true;

    pthread_mutex_lock(&shard->opaque_lock);
    pthread_mutex_lock(&shard->lock);
    if (llam_processor_queue_access_snapshot_pair_locked(shard, &access)) {
        llam_legacy_lease_state_t before = shard->legacy_lease.state;
        llam_legacy_lease_state_t projected = shard->processor.owner_record.lease_state;
        /* Keep the owner/epochs coherent but inject the other protocol's state. */
        shard->legacy_lease.state = other_state;
        shard->processor.owner_record.lease_state = other_state;
        access.lease_state = other_state;
        accepted = llam_processor_owner_ticket_snapshot_pair_locked(shard, &ticket) ||
                   llam_processor_queue_access_snapshot_pair_locked(shard, &rejected) ||
                   llam_processor_queue_access_validate_locked(shard, &access);
        shard->legacy_lease.state = before;
        shard->processor.owner_record.lease_state = projected;
    }
    pthread_mutex_unlock(&shard->lock);
    pthread_mutex_unlock(&shard->opaque_lock);
    return !accepted;
}

static int exercise_tickets(llam_runtime_t *runtime, llam_runtime_t *peer) {
    llam_worker_t *worker = &runtime->managed_workers[0];
    llam_shard_t *first = &runtime->shards[0];
    llam_ctx_t context = {0};
    llam_processor_owner_ticket_t ticket, stale, forged;
    llam_processor_queue_access_t access;
    uint64_t incarnation;

    CHECK(llam_runtime_run_handle(runtime) == -1 && errno == ENOTSUP);
    CHECK(!llam_worker_registered_for((llam_worker_t *)(uintptr_t)1U, runtime));
    CHECK(!release_p(first, (llam_worker_t *)(uintptr_t)1U, NULL));
    CHECK(llam_worker_registered_for(worker, runtime));
    CHECK(!llam_worker_registered_for(worker, peer));
    CHECK(llam_worker_bind_registered(worker, &context) == 0);
    incarnation = atomic_load_explicit(&worker->binding_incarnation, memory_order_acquire);
    CHECK(incarnation != 0U && !claim(first, worker, incarnation + 1U));
    CHECK(!claim(&peer->shards[0], worker, incarnation));
    CHECK(!llam_worker_matches_shard(worker, first));
    CHECK(!claim_drain(first, worker, incarnation));
    CHECK(claim(first, worker, incarnation));
    CHECK(llam_worker_matches_shard(worker, first));
    CHECK(llam_processor_owner_ticket_snapshot(&first->processor, &ticket));
    if (!rejects_other_protocol(first, LLAM_LEASE_PRIMARY_OWNED)) {
        fprintf(stderr, "test_execution_lease_internal: mixed protocol ticket accepted\n");
        (void)release_p(first, worker, &ticket);
        (void)llam_worker_unbind_registered(worker);
        return -1;
    }
    CHECK(ticket.owner_m == worker && ticket.worker_incarnation == incarnation);
    CHECK(rejects_foreign_native(first, worker, incarnation, &ticket) == 0);
    CHECK(exercise_managed_publication(first, worker) == 0);
    CHECK(llam_processor_owner_ticket_validate(&first->processor, &ticket));
    CHECK(llam_processor_tls_owns_resources(&first->processor));
    CHECK(llam_processor_queue_access_snapshot(&first->processor, &access));
    CHECK(access.mode == LLAM_PROCESSOR_QUEUE_ACCESS_EXECUTION);
    pthread_mutex_lock(&first->lock);
    bool valid_access = llam_processor_queue_access_validate_locked(first, &access);
    pthread_mutex_unlock(&first->lock);
    CHECK(valid_access);
    CHECK(llam_worker_unbind_registered(worker) == -1 && errno == EBUSY);
    CHECK(!release_p(first, worker, NULL));
    forged = ticket;
    forged.worker_incarnation += 1U;
    CHECK(!release_p(first, worker, &forged));
    CHECK(!llam_processor_owner_ticket_validate(&first->processor, &forged));
    if (runtime->active_shards > 1U) {
        CHECK(!claim(&runtime->shards[1], worker, incarnation));
    }
    stale = ticket;
    CHECK(release_p(first, worker, &ticket));
    CHECK(!llam_processor_owner_ticket_validate(&first->processor, &stale));
    CHECK(!llam_processor_tls_owns_resources(&first->processor));
    CHECK(claim(first, worker, incarnation));
    CHECK(llam_processor_owner_ticket_snapshot(&first->processor, &ticket));
    CHECK(ticket.p_generation > stale.p_generation);
    CHECK(ticket.lease_projection_epoch > stale.lease_projection_epoch);
    CHECK(!release_p(first, worker, &stale));
    CHECK(release_p(first, worker, &ticket));
    if (runtime->active_shards > 1U) {
        llam_shard_t *second = &runtime->shards[1];
        CHECK(claim(second, worker, incarnation));
        CHECK(worker->compat_shard == first && g_llam_tls_shard == second);
        CHECK(worker->associated_processor == &first->processor);
        CHECK(!llam_worker_matches_shard(worker, first));
        CHECK(llam_worker_matches_shard(worker, second));
        CHECK(exercise_managed_publication(second, worker) == 0);
        CHECK(llam_processor_tls_owns_resources(&second->processor));
        CHECK(llam_processor_owner_ticket_snapshot(&second->processor, &ticket));
        CHECK(!llam_processor_owner_ticket_validate(&first->processor, &ticket));
        CHECK(llam_processor_queue_access_snapshot(&second->processor, &access));
        CHECK(release_p(second, worker, &ticket));
    }
    CHECK(llam_worker_unbind_registered(worker) == 0);
    CHECK(!llam_worker_binding_live(worker, incarnation));
    CHECK(llam_worker_bind_registered(worker, &context) == 0);
    CHECK(!claim(first, worker, incarnation));
    CHECK(llam_worker_unbind_registered(worker) == 0);
    llam_shard_t *peer_shard = &peer->shards[0];
    llam_scheduler_bind_identity(peer_shard, &peer_shard->primary_worker, &context);
    bool peer_rejected = rejects_other_protocol(peer_shard, LLAM_LEASE_REGISTERED_OWNED);
    llam_scheduler_unbind_identity(peer_shard, &peer_shard->primary_worker);
    llam_scheduler_clear_identity();
    CHECK(peer_rejected);
    return 0;
}

typedef struct contender {
    llam_runtime_t *runtime;
    llam_worker_t *worker;
    atomic_uint *inside;
    unsigned *counter;
    int result;
} contender_t;

static void *contend(void *argument) {
    contender_t *state = argument;
    llam_shard_t *shard = &state->runtime->shards[0];
    llam_ctx_t context = {0};
    llam_processor_owner_ticket_t ticket;
    uint64_t deadline = llam_now_ns() + UINT64_C(5000000000);
    uint64_t incarnation;
    unsigned completed = 0U;

    state->result = -1;
    if (llam_worker_bind_registered(state->worker, &context) != 0) {
        return NULL;
    }
    incarnation = atomic_load_explicit(&state->worker->binding_incarnation, memory_order_acquire);
    while (completed < 4000U && llam_now_ns() < deadline) {
        if (!claim(shard, state->worker, incarnation)) {
            continue;
        }
        if (atomic_fetch_add_explicit(state->inside, 1U, memory_order_acq_rel) != 0U ||
            !llam_processor_owner_ticket_snapshot(&shard->processor, &ticket)) {
            abort();
        }
        *state->counter += 1U;
        atomic_fetch_sub_explicit(state->inside, 1U, memory_order_release);
        if (!release_p(shard, state->worker, &ticket)) {
            abort();
        }
        completed += 1U;
    }
    if (llam_worker_unbind_registered(state->worker) == 0 && completed == 4000U) {
        state->result = 0;
    }
    return NULL;
}

static int exercise_contention(llam_runtime_t *runtime) {
    pthread_t threads[2];
    atomic_uint inside;
    unsigned counter = 0U;
    contender_t contenders[2];

    atomic_init(&inside, 0U);
    for (unsigned i = 0U; i < 2U; ++i) {
        contenders[i] = (contender_t){runtime, &runtime->managed_workers[i],
                                     &inside, &counter, -1};
        if (pthread_create(&threads[i], NULL, contend, &contenders[i]) != 0) {
            if (i != 0U) {
                pthread_join(threads[0], NULL);
            }
            return -1;
        }
    }
    for (unsigned i = 0U; i < 2U; ++i) {
        CHECK(pthread_join(threads[i], NULL) == 0);
        CHECK(contenders[i].result == 0);
    }
    CHECK(counter == 8000U && atomic_load_explicit(&inside, memory_order_acquire) == 0U);
    return 0;
}

static int exercise_stop(llam_runtime_t *runtime) {
    llam_worker_t *worker = &runtime->managed_workers[0];
    llam_shard_t *shard = &runtime->shards[0];
    llam_ctx_t context = {0};
    llam_task_t active_task = {0};
    llam_processor_owner_ticket_t ticket, rejected_ticket;
    llam_processor_queue_access_t access, newer, forged;
    uint64_t incarnation;

    CHECK(llam_worker_bind_registered(worker, &context) == 0);
    incarnation = atomic_load_explicit(&worker->binding_incarnation, memory_order_acquire);
    CHECK(claim(shard, worker, incarnation));
    CHECK(llam_processor_owner_ticket_snapshot(&shard->processor, &ticket));
    /* Deterministically exercise shutdown's flag-before-STOPPING interval. */
    atomic_store_explicit(&runtime->shutdown_requested, true, memory_order_release);
    CHECK(shard->legacy_lease.state == LLAM_LEASE_REGISTERED_OWNED);
    CHECK(!llam_processor_owner_ticket_validate(&shard->processor, &ticket));
    CHECK(!release_p(shard, worker, &ticket));
    CHECK(rejects_foreign_native(shard, worker, incarnation, &ticket) == 0);
    CHECK(release_p(shard, worker, NULL));
    CHECK(shard->legacy_lease.state == LLAM_LEASE_STOPPING);
    CHECK(shard->processor.owner_record.state == LLAM_PROCESSOR_OWNER_STOPPING);
    CHECK(!claim(shard, worker, incarnation));
    CHECK(!claim_drain(shard, worker, incarnation + 1U));
    CHECK(rejects_foreign_native(shard, worker, incarnation, &ticket) == 0);

    /* Neither TLS task state nor active-G publication may cross a drain claim. */
    g_llam_tls_task = &active_task;
    bool tls_task_rejected = !claim_drain(shard, worker, incarnation);
    g_llam_tls_task = NULL;
    CHECK(tls_task_rejected);
    llam_worker_publish_active_g(worker, &active_task);
    bool active_task_rejected = !claim_drain(shard, worker, incarnation);
    llam_worker_clear_active_g(worker);
    CHECK(active_task_rejected);
    CHECK(claim_drain(shard, worker, incarnation));
    CHECK(!llam_processor_owner_ticket_snapshot(&shard->processor, &rejected_ticket));
    CHECK(llam_processor_queue_access_snapshot(&shard->processor, &access));
    CHECK(access.mode == LLAM_PROCESSOR_QUEUE_ACCESS_STOPPING_DRAIN);
    CHECK(access.p_generation > ticket.p_generation);
    CHECK(access.lease_projection_epoch > ticket.lease_projection_epoch);
    CHECK(!release_p(shard, worker, &ticket));
    if (runtime->active_shards > 1U) {
        CHECK(!claim_drain(&runtime->shards[1], worker, incarnation));
        CHECK(atomic_load_explicit(&worker->current_p, memory_order_acquire) ==
              &shard->processor);
    }
    CHECK(release_p(shard, worker, NULL));
    pthread_mutex_lock(&shard->lock);
    bool stale_access_rejected = !llam_processor_queue_access_validate_locked(shard, &access);
    pthread_mutex_unlock(&shard->lock);
    CHECK(stale_access_rejected);

    if (runtime->active_shards > 1U) {
        llam_shard_t *second = &runtime->shards[1];
        /* The free P has not yet been visited by shutdown's projection loop. */
        CHECK(second->legacy_lease.state == LLAM_LEASE_REGISTERED_FREE);
        CHECK(!claim(second, worker, incarnation));
        CHECK(claim_drain(second, worker, incarnation));
        CHECK(second->legacy_lease.state == LLAM_LEASE_STOPPING);
        CHECK(llam_worker_matches_shard(worker, second));
        CHECK(llam_processor_queue_access_snapshot(&second->processor, &newer));
        CHECK(newer.mode == LLAM_PROCESSOR_QUEUE_ACCESS_STOPPING_DRAIN);
        CHECK(release_p(second, worker, NULL));
    }
    CHECK(claim_drain(shard, worker, incarnation));
    CHECK(llam_processor_queue_access_snapshot(&shard->processor, &newer));
    CHECK(newer.p_generation > access.p_generation);
    CHECK(newer.lease_projection_epoch > access.lease_projection_epoch);
    forged = newer;
    forged.actor_incarnation += 1U;
    pthread_mutex_lock(&shard->lock);
    bool old_and_forged_rejected =
        !llam_processor_queue_access_validate_locked(shard, &access) &&
        !llam_processor_queue_access_validate_locked(shard, &forged);
    pthread_mutex_unlock(&shard->lock);
    CHECK(old_and_forged_rejected);
    CHECK(!release_p(shard, worker, &ticket));
    CHECK(release_p(shard, worker, NULL));
    CHECK(llam_worker_unbind_registered(worker) == 0);
    CHECK(llam_worker_bind_registered(worker, &context) == -1 && errno == EBUSY);
    CHECK(!atomic_load_explicit(&worker->native_thread_live, memory_order_acquire));
    CHECK(atomic_load_explicit(&worker->binding_incarnation, memory_order_acquire) == 0U);
    return 0;
}

static int exercise_counter_exhaustion(bool processor_generation, bool stopping) {
    llam_runtime_opts_t opts;
    llam_runtime_t *runtime = calloc(1U, sizeof(*runtime));
    llam_processor_owner_ticket_t ticket;
    llam_ctx_t context = {0};

    CHECK(runtime != NULL);
    CHECK(llam_runtime_opts_init(&opts, sizeof(opts)) == 0);
    opts.deterministic = 1U;
    opts.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    CHECK(llam_runtime_init_registered_rt(runtime, &opts, sizeof(opts), true, 1U) == 0);
    llam_worker_t *worker = &runtime->managed_workers[0];
    llam_shard_t *shard = &runtime->shards[0];
    CHECK(llam_worker_bind_registered(worker, &context) == 0);
    uint64_t incarnation = atomic_load_explicit(&worker->binding_incarnation, memory_order_acquire);
    /* Each isolated fixture starts just below retirement; no counter is reset. */
    pthread_mutex_lock(&shard->opaque_lock);
    pthread_mutex_lock(&shard->lock);
    if (processor_generation) {
        shard->processor.owner_record.p_generation = UINT64_MAX - 2U;
    } else {
        shard->legacy_lease.epoch = UINT64_MAX - 2U;
        shard->processor.owner_record.lease_projection_epoch = UINT64_MAX - 2U;
    }
    pthread_mutex_unlock(&shard->lock);
    pthread_mutex_unlock(&shard->opaque_lock);
    CHECK(claim(shard, worker, incarnation));
    CHECK(llam_processor_owner_ticket_snapshot(&shard->processor, &ticket));
    CHECK((processor_generation ? ticket.p_generation : ticket.lease_projection_epoch) ==
          UINT64_MAX - 1U);
    if (stopping) {
        llam_processor_queue_access_t drain;
        atomic_store_explicit(&runtime->shutdown_requested, true, memory_order_release);
        CHECK(!release_p(shard, worker, &ticket));
        pthread_mutex_lock(&shard->opaque_lock);
        llam_legacy_lease_begin_stop_locked(shard);
        pthread_mutex_unlock(&shard->opaque_lock);
        CHECK(llam_processor_queue_access_snapshot(&shard->processor, &drain));
        CHECK(drain.mode == LLAM_PROCESSOR_QUEUE_ACCESS_STOPPING_DRAIN);
        CHECK(!llam_processor_owner_ticket_validate(&shard->processor, &ticket));
        CHECK(release_p(shard, worker, NULL));
        CHECK(shard->legacy_lease.state == LLAM_LEASE_STOPPING);
        CHECK(!claim_drain(shard, worker, incarnation));
    } else {
        CHECK(release_p(shard, worker, &ticket));
    }
    CHECK(!claim(shard, worker, incarnation));
    CHECK(!llam_processor_owner_ticket_validate(&shard->processor, &ticket));
    CHECK(atomic_load_explicit(&worker->current_p, memory_order_acquire) == NULL);
    CHECK(llam_worker_unbind_registered(worker) == 0);
    if (!stopping) {
        atomic_store_explicit(&worker->incarnation, UINT64_MAX - 1U, memory_order_release);
        CHECK(llam_worker_bind_registered(worker, &context) == -1 && errno == EOVERFLOW);
    }
    llam_runtime_destroy(runtime);
    return 0;
}

int main(void) {
    llam_runtime_opts_t opts;
    llam_runtime_t *runtime = calloc(1U, sizeof(*runtime));
    llam_runtime_t *peer = NULL;
    int result = 1;

    if (runtime == NULL || llam_runtime_opts_init(&opts, sizeof(opts)) != 0) {
        free(runtime);
        return 1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    opts.deterministic = 0U;
    if (llam_runtime_init_registered_rt(runtime, &opts, sizeof(opts), true, 2U) != 0) {
        free(runtime);
        return 1;
    }
    opts.deterministic = 1U;
    if (llam_runtime_create(&opts, sizeof(opts), &peer) != 0) {
        llam_runtime_destroy(runtime);
        return 1;
    }
    if (exercise_tickets(runtime, peer) == 0 && exercise_contention(runtime) == 0 &&
        exercise_stop(runtime) == 0) {
        result = 0;
    }
    llam_runtime_destroy(peer);
    llam_runtime_destroy(runtime);
    if (result == 0 && (exercise_counter_exhaustion(false, false) != 0 ||
                        exercise_counter_exhaustion(true, false) != 0 ||
                        exercise_counter_exhaustion(false, true) != 0 ||
                        exercise_counter_exhaustion(true, true) != 0)) {
        result = 1;
    }
    if (result == 0) {
        puts("test_execution_lease_internal: registered M/P ownership checks passed");
    }
    return result;
}
