/**
 * @file src/core/runtime.c
 * @brief Defines default runtime storage and core runtime invariants.
 *
 * @details
 * The runtime keeps a process-default instance for legacy convenience APIs and
 * thread-local cursors for the currently executing shard/task/scheduler
 * context. Explicit runtime handles allocate their own state; keeping the
 * default storage in one translation unit avoids duplicate definitions and
 * makes legacy ownership explicit.
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

#include "runtime_internal.h"

/** @brief Legacy process-default runtime state; behavior lives in subsystem modules. */
llam_runtime_t g_llam_runtime;
/** @brief Low 32 bits of the enabled x86 XSAVE mask. */
uint32_t g_llam_xsave_mask_lo;
/** @brief High 32 bits of the enabled x86 XSAVE mask. */
uint32_t g_llam_xsave_mask_hi;
/** @brief FP-control context flag used by platform context switch code. */
uint32_t g_llam_fp_control_context;
/** @brief Thread-local shard currently running on this native thread. */
_Thread_local llam_shard_t *g_llam_tls_shard;
/** @brief Thread-local task currently executing on this native thread. */
_Thread_local llam_task_t *g_llam_tls_task;
/** @brief Thread-local scheduler context for returning from task fibers. */
_Thread_local llam_ctx_t *g_llam_tls_scheduler_ctx;
/** @brief Thread-local hint requesting a yield after I/O handoff. */
_Thread_local unsigned g_llam_tls_io_handoff_yield;
/** @brief Thread-local redirect target hint while leaving opaque blocking. */
_Thread_local unsigned g_llam_tls_opaque_redirect_hint;
