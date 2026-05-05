/**
 * @file src/internal/runtime_state.h
 * @brief Global runtime state layout and worker/node/shard state ownership rules.
 *
 * @details
 * This header exposes the process-global runtime object and scheduler TLS used
 * across implementation files. No public API should include this file directly.
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

#ifndef NM_RUNTIME_STATE_H
#define NM_RUNTIME_STATE_H

#include "runtime_types.h"

// Process-wide runtime singleton. Public init/shutdown owns its lifetime.
extern nm_runtime_t g_nm_runtime;

// Cached XSAVE/FPU capability bits discovered at runtime initialization.
extern uint32_t g_nm_xsave_mask_lo;
extern uint32_t g_nm_xsave_mask_hi;
extern uint32_t g_nm_fp_control_context;

// Scheduler TLS. These are set only while a runtime worker or managed task runs.
extern _Thread_local nm_shard_t *g_nm_tls_shard;
extern _Thread_local nm_task_t *g_nm_tls_task;
extern _Thread_local nm_ctx_t *g_nm_tls_scheduler_ctx;

// Recursion/fast-path hints used by channel handoff and opaque-block redirect.
extern _Thread_local unsigned g_nm_tls_io_handoff_yield;
extern _Thread_local unsigned g_nm_tls_opaque_redirect_hint;

/**
 * @brief Encoded completion user-data tags for backend I/O events.
 *
 * The low bits of an encoded user-data value identify which runtime object the
 * backend completion pointer refers to.
 */
enum {
    NM_IO_UDATA_REQ = 0U,
    NM_IO_UDATA_POLL_WATCH = 1U,
    NM_IO_UDATA_ACCEPT_WATCH = 2U,
    NM_IO_UDATA_RECV_WATCH = 3U,
    NM_IO_UDATA_CONTROL = 4U,
};

#endif
