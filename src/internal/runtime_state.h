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
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef LLAM_RUNTIME_STATE_H
#define LLAM_RUNTIME_STATE_H

#include "runtime_types.h"

// Process-default runtime used by legacy init/spawn/run/shutdown wrappers.
extern llam_runtime_t g_llam_runtime;

// Cached XSAVE/FPU capability bits discovered at runtime initialization.
extern uint32_t g_llam_xsave_mask_lo;
extern uint32_t g_llam_xsave_mask_hi;
extern uint32_t g_llam_fp_control_context;

// Scheduler TLS. These are set only while a runtime worker or managed task runs.
extern _Thread_local llam_shard_t *g_llam_tls_shard;
extern _Thread_local llam_task_t *g_llam_tls_task;
extern _Thread_local llam_ctx_t *g_llam_tls_scheduler_ctx;

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
