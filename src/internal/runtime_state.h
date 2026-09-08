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
 * See LICENSES/OLD-LICENSE/Apache-2.0.txt.
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

/**
 * @brief Check whether a worker slot is the execution slot for a shard.
 *
 * Active-G and its timestamps are worker-local.  This small identity check is
 * intentionally independent of lease ownership; callers that need ownership
 * truth use the coordinator while holding `opaque_lock`.
 */
static inline bool llam_worker_matches_shard(const llam_worker_t *worker,
                                             const llam_shard_t *shard) {
    return worker != NULL && shard != NULL && worker->compat_shard == shard;
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
    return state >= LLAM_LEASE_PRIMARY_OWNED && state <= LLAM_LEASE_STOPPING;
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
 * @brief Match the current worker/P TLS tuple to one descriptive worker role.
 *
 * This is a legacy-role observation only.  Lease ownership is validated by the
 * shard coordinator and current-task publication by the worker active-G; all
 * queue/resource state remains on @c llam_shard_t.
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
    return llam_tls_is_primary_scheduler_for(shard);
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
