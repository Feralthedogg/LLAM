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
#include "managed_scheduler.h"
#include "llam/managed.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Called only for new, unpublished storage during serialized runtime init. */
static void llam_worker_init_registered(llam_worker_t *worker,
                                        llam_shard_t *home, llam_runtime_t *runtime) {
    worker->compat_shard = home;
    worker->associated_processor = &home->processor;
    worker->role = runtime->managed_policy_pools
        ? LLAM_WORKER_ROLE_NATIVE_AFFINE : LLAM_WORKER_ROLE_MANAGED;
    atomic_init(&worker->managed_reservation, 0U);
    atomic_init(&worker->managed_slot_used, false);
    atomic_init(&worker->managed_slot_closing, false);
    atomic_init(&worker->incarnation, 0U);
    atomic_init(&worker->native_thread_live, false);
    atomic_init(&worker->binding_incarnation, 0U);
    atomic_init(&worker->lease_reacquire_pending, false);
    atomic_init(&worker->active_g, NULL);
    atomic_init(&worker->current_p, NULL);
    atomic_init(&worker->execution_state, LLAM_WORKER_EXEC_IDLE);
    atomic_init(&worker->runtime_critical_depth, 0U);
    atomic_init(&worker->preempt_disable_depth, 0U);
    atomic_init(&worker->local_executor, NULL);
    llam_native_domain_worker_init(worker);
    atomic_init(&worker->run_generation, 0U);
    atomic_init(&worker->async_preempt_state, 0U);
    worker->async_preempt_control = NULL;
    atomic_init(&worker->async_preempt_ticket_lease_epoch, 0U);
    atomic_init(&worker->async_preempt_ticket_generation, 0U);
    atomic_init(&worker->last_run_started_ns, 0U);
    atomic_init(&worker->last_safepoint_ns, 0U);
    atomic_init(&worker->last_idle_wake_ns, 0U);
}

int llam_worker_registry_init(llam_runtime_t *runtime, size_t managed_count) {
    size_t embedded_count;
    size_t slot_count;
    llam_worker_t **slots;
    llam_worker_t *managed = NULL;

    if (runtime == NULL || runtime->shards == NULL || runtime->active_shards == 0U ||
        runtime->worker_slots != NULL || runtime->managed_workers != NULL) {
        errno = EINVAL;
        return -1;
    }
    embedded_count = runtime->active_shards;
    atomic_init(&runtime->managed_foreign_returns_pending, 0U);
    if (embedded_count > SIZE_MAX - embedded_count) {
        errno = EOVERFLOW;
        return -1;
    }
    embedded_count += embedded_count;
    if (managed_count > SIZE_MAX - embedded_count ||
        managed_count > SIZE_MAX / sizeof(*managed)) {
        errno = EOVERFLOW;
        return -1;
    }
    slot_count = embedded_count + managed_count;
    if (slot_count > SIZE_MAX / sizeof(*slots)) {
        errno = EOVERFLOW;
        return -1;
    }
    slots = calloc(slot_count, sizeof(*slots));
    if (slots == NULL) {
        errno = ENOMEM;
        return -1;
    }
    if (managed_count != 0U) {
        managed = calloc(managed_count, sizeof(*managed));
        if (managed == NULL) {
            free(slots);
            errno = ENOMEM;
            return -1;
        }
    }
    for (unsigned i = 0U; i < runtime->active_shards; ++i) {
        slots[(size_t)i * 2U] = &runtime->shards[i].primary_worker;
        slots[(size_t)i * 2U + 1U] = &runtime->shards[i].opaque_helper_worker;
    }
    for (size_t i = 0U; i < managed_count; ++i) {
        llam_worker_init_registered(&managed[i],
                                    &runtime->shards[i % runtime->portable_processor_count], runtime);
        slots[embedded_count + i] = &managed[i];
    }
    for (size_t i = 0U; i < slot_count; ++i) {
        slots[i]->registry_runtime = runtime;
        slots[i]->registry_slot = i;
    }
    /* Init publishes the complete immutable table before starting native threads. */
    runtime->worker_slots = slots;
    runtime->worker_slot_count = slot_count;
    runtime->managed_workers = managed;
    runtime->managed_worker_count = managed_count;
    return 0;
}

void llam_worker_registry_destroy(llam_runtime_t *runtime) {
    if (runtime == NULL) {
        return;
    }
    for (size_t i = 0U; i < runtime->worker_slot_count; ++i) {
        if (runtime->worker_slots[i]->native_domain_slot != 0U ||
            atomic_load_explicit(&runtime->worker_slots[i]->native_domain_depth,
                                 memory_order_acquire) != 0U) abort();
    }
    /* Native workers and all adapter readers must be joined/drained by the caller. */
    for (size_t i = 0U; i < runtime->managed_worker_count; ++i) {
        llam_worker_t *worker = &runtime->managed_workers[i];
        if (atomic_load_explicit(&worker->native_thread_live, memory_order_acquire) ||
            atomic_load_explicit(&worker->current_p, memory_order_acquire) != NULL ||
            llam_worker_load_active_g(worker) != NULL ||
            atomic_load_explicit(&worker->async_preempt_state, memory_order_acquire) != 0U ||
            worker->sigaltstack_installed) {
            abort();
        }
    }
    free(runtime->managed_workers);
    free(runtime->worker_slots);
    runtime->managed_workers = NULL;
    runtime->worker_slots = NULL;
    runtime->managed_worker_count = 0U;
    runtime->worker_slot_count = 0U;
}

static bool llam_worker_registry_admission_open(const llam_runtime_t *runtime) {
    return atomic_load_explicit(&runtime->initialized, memory_order_acquire) &&
           !atomic_load_explicit(&runtime->destroy_claimed, memory_order_acquire) &&
           !atomic_load_explicit(&runtime->shutdown_requested, memory_order_acquire) &&
           !atomic_load_explicit(&runtime->stop_requested, memory_order_acquire);
}

int llam_worker_bind_registered(llam_worker_t *worker, llam_ctx_t *scheduler_ctx) {
    llam_runtime_t *runtime = worker != NULL ? worker->registry_runtime : NULL;
    llam_shard_t *home = worker != NULL ? worker->compat_shard : NULL;
    uint64_t incarnation;

#if LLAM_PLATFORM_DARWIN
    llam_scheduler_tls_cache_bind_current();
#endif
    if (!llam_worker_registered_for(worker, runtime) || home == NULL ||
        !home->opaque_lock_initialized || scheduler_ctx == NULL ||
        runtime->execution_lease_mode != LLAM_EXECUTION_LEASE_REGISTERED ||
        g_llam_tls_worker != NULL || g_llam_tls_task != NULL ||
        g_llam_tls_scheduler_ctx != NULL) {
        errno = EINVAL;
        return -1;
    }
    if (!llam_worker_registry_admission_open(runtime)) {
        errno = EBUSY;
        return -1;
    }
    pthread_mutex_lock(&home->opaque_lock);
    if (!llam_worker_registry_admission_open(runtime)) {
        pthread_mutex_unlock(&home->opaque_lock);
        errno = EBUSY;
        return -1;
    }
    incarnation = atomic_load_explicit(&worker->incarnation, memory_order_acquire);
    if (atomic_load_explicit(&worker->native_thread_live, memory_order_acquire) ||
        atomic_load_explicit(&worker->current_p, memory_order_acquire) != NULL ||
        llam_worker_load_active_g(worker) != NULL ||
        atomic_load_explicit(&worker->async_preempt_state, memory_order_acquire) != 0U ||
        incarnation >= UINT64_MAX - 1U) {
        pthread_mutex_unlock(&home->opaque_lock);
        errno = incarnation >= UINT64_MAX - 1U ? EOVERFLOW : EBUSY;
        return -1;
    }
    incarnation += 1U;
    worker->native_thread = pthread_self();
    atomic_store_explicit(&worker->incarnation, incarnation, memory_order_release);
    atomic_store_explicit(&worker->binding_incarnation, incarnation, memory_order_release);
    if (llam_native_domain_bind(worker) != 0) {
        atomic_store_explicit(&worker->binding_incarnation, 0U, memory_order_release);
        memset(&worker->native_thread, 0, sizeof(worker->native_thread));
        pthread_mutex_unlock(&home->opaque_lock);
        return -1;
    }
    /* Acquiring live makes the immutable native handle and incarnation visible. */
    atomic_store_explicit(&worker->native_thread_live, true, memory_order_release);
    pthread_mutex_unlock(&home->opaque_lock);
    g_llam_tls_shard = home;
    g_llam_tls_worker = worker;
    g_llam_tls_processor = NULL;
    g_llam_tls_task = NULL;
    g_llam_tls_scheduler_ctx = scheduler_ctx;
    atomic_store_explicit(&worker->execution_state, LLAM_WORKER_EXEC_SCHEDULER,
                          memory_order_release);
    return 0;
}

int llam_worker_unbind_registered(llam_worker_t *worker) {
    llam_runtime_t *runtime = worker != NULL ? worker->registry_runtime : NULL;
    llam_shard_t *home = worker != NULL ? worker->compat_shard : NULL;

    if (!llam_worker_registered_for(worker, runtime) || home == NULL ||
        !home->opaque_lock_initialized || g_llam_tls_worker != worker ||
        g_llam_tls_task != NULL) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&home->opaque_lock);
    if (atomic_load_explicit(&worker->current_p, memory_order_acquire) != NULL ||
        llam_worker_load_active_g(worker) != NULL ||
        atomic_load_explicit(&worker->async_preempt_state, memory_order_acquire) != 0U ||
        worker->foreign_token.state != LLAM_FOREIGN_TOKEN_INACTIVE || worker->sigaltstack_installed) {
        pthread_mutex_unlock(&home->opaque_lock);
        errno = EBUSY;
        return -1;
    }
    if (atomic_load_explicit(&worker->native_domain_depth, memory_order_acquire) != 0U) {
        pthread_mutex_unlock(&home->opaque_lock);
        errno = EBUSY;
        return -1;
    }
    if (!atomic_load_explicit(&worker->native_thread_live, memory_order_acquire) ||
        pthread_equal(worker->native_thread, pthread_self()) == 0) {
        pthread_mutex_unlock(&home->opaque_lock);
        errno = EINVAL;
        return -1;
    }
    atomic_store_explicit(&worker->native_thread_live, false, memory_order_release);
    atomic_store_explicit(&worker->binding_incarnation, 0U, memory_order_release);
    atomic_store_explicit(&worker->execution_state, LLAM_WORKER_EXEC_IDLE,
                          memory_order_release);
    memset(&worker->native_thread, 0, sizeof(worker->native_thread));
    pthread_mutex_unlock(&home->opaque_lock);
    llam_scheduler_clear_identity();
    return 0;
}

int llam_managed_reserve_execution(llam_runtime_t *runtime, uint32_t execution_class,
                                    llam_worker_t **out, uint64_t *generation_out) {
    if (runtime == NULL || out == NULL || generation_out == NULL) { errno = EINVAL; return -1; }
    *out = NULL;
    *generation_out = 0U;
    if (!runtime->managed_policy_pools) {
        if (execution_class == LLAM_MANAGED_EXECUTION_NATIVE_AFFINE) { errno = ENOTSUP; return -1; }
        return 0;
    }
    if (execution_class == LLAM_MANAGED_EXECUTION_PORTABLE) return 0;
    if (execution_class == LLAM_MANAGED_EXECUTION_LEGACY) {
        if (runtime->compatibility_processor_count == 0U) { errno = ENOTSUP; return -1; }
        *out = &runtime->shards[runtime->portable_processor_count].primary_worker;
        return 0;
    }
    if (execution_class != LLAM_MANAGED_EXECUTION_NATIVE_AFFINE) { errno = EINVAL; return -1; }
    bool running = atomic_load_explicit(&runtime->exec_started, memory_order_acquire);
    bool reusable_generation = false;
    for (size_t i = 0U; i < runtime->managed_worker_count; ++i) {
        llam_worker_t *worker = &runtime->managed_workers[i];
        uint64_t expected = atomic_load_explicit(&worker->managed_reservation, memory_order_acquire);
        if (expected >= UINT64_MAX - 1U) continue;
        reusable_generation = true;
        if ((expected & 1U) != 0U ||
            atomic_load_explicit(&worker->managed_slot_used, memory_order_acquire) ||
            (running && (!atomic_load_explicit(&worker->native_thread_live, memory_order_acquire) ||
                         atomic_load_explicit(&worker->managed_slot_closing, memory_order_seq_cst)))) continue;
        uint64_t reserved = expected + 1U;
        /* SC reservation/closing publications prevent admission and native
         * exit from both missing each other. The odd token is the G's credit. */
        if (atomic_compare_exchange_strong_explicit(&worker->managed_reservation,
                &expected, reserved, memory_order_seq_cst, memory_order_seq_cst)) {
            if (running && (atomic_load_explicit(&worker->managed_slot_closing, memory_order_seq_cst) ||
                            atomic_load_explicit(&runtime->stop_requested, memory_order_acquire))) {
                llam_managed_release_execution(runtime, worker, execution_class, reserved);
                continue;
            }
            *out = worker;
            *generation_out = reserved;
            return 0;
        }
    }
    errno = reusable_generation ? EAGAIN : EOVERFLOW;
    return -1;
}

void llam_managed_release_execution(llam_runtime_t *runtime, llam_worker_t *worker,
                                     uint32_t execution_class, uint64_t generation) {
    if (execution_class != LLAM_MANAGED_EXECUTION_NATIVE_AFFINE || worker == NULL) return;
    if (!llam_worker_registered_for(worker, runtime) ||
        worker->role != LLAM_WORKER_ROLE_NATIVE_AFFINE ||
        (generation & 1U) == 0U || generation == UINT64_MAX) abort();
    /* Native retirement may already have returned this exact credit. An old
     * completed G may outlive a fresh native incarnation and reservation. */
    (void)atomic_compare_exchange_strong_explicit(&worker->managed_reservation,
        &generation, generation + 1U, memory_order_acq_rel, memory_order_acquire);
}

uint32_t llam_managed_affine_available(const llam_runtime_t *runtime) {
    uint32_t available = 0U;
    if (runtime == NULL || !runtime->managed_policy_pools) return 0U;
    bool running = atomic_load_explicit(&runtime->exec_started, memory_order_acquire);
    for (size_t i = 0U; i < runtime->managed_worker_count; ++i) {
        const llam_worker_t *worker = &runtime->managed_workers[i];
        uint64_t reservation = atomic_load_explicit(&worker->managed_reservation, memory_order_acquire);
        if ((reservation & 1U) == 0U && reservation < UINT64_MAX - 1U &&
            !atomic_load_explicit(&worker->managed_slot_used, memory_order_acquire) &&
            (!running || (atomic_load_explicit(&worker->native_thread_live, memory_order_acquire) &&
                          !atomic_load_explicit(&worker->managed_slot_closing, memory_order_acquire)))) ++available;
    }
    return available;
}

bool llam_managed_mixed_available(const llam_runtime_t *runtime) {
    return runtime != NULL && runtime->managed_policy_pools &&
           runtime->portable_processor_count != 0U && runtime->compatibility_processor_count != 0U;
}
