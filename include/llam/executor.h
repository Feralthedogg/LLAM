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

#ifndef LLAM_EXECUTOR_H
#define LLAM_EXECUTOR_H

#include "llam/runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Enter a short, nested no-preemption region on the current managed task.
 * Requires a live native M/G binding. The caller must keep the region bounded,
 * nonblocking and balanced, and must not suspend, allocate, spawn, unwind or call
 * unknown foreign code. Stateful LLAM calls are refused with ENOTSUP inside it.
 * This does not stop other OS threads or provide memory synchronization.
 * Host/guarded native callback entry returns ENOTSUP; exhausted nesting returns EOVERFLOW.
 * Pending cancellation/preemption is preserved. A compiler must validate bounded
 * control flow and transitive effects; this API does not certify an annotation.
 */
LLAM_API int llam_preempt_region_enter(void);

/** Leave one region owned by this exact native M/G execution. Underflow returns
 * EINVAL; guarded native callbacks cannot leave a caller's region. Final leave restores
 * ordinary execution and services a pending preemption request. A task returning
 * with an open region is a process-fatal contract violation (exit status 125).
 */
LLAM_API int llam_preempt_region_leave(void);

/** Reserved options; initialize with llam_local_executor_opts_init. */
typedef struct llam_local_executor_opts {
    uint32_t flags;
    uint32_t reserved0;
} llam_local_executor_opts_t;
#define LLAM_LOCAL_EXECUTOR_OPTS_CURRENT_SIZE ((size_t)sizeof(llam_local_executor_opts_t))

LLAM_API int llam_local_executor_opts_init(llam_local_executor_opts_t *out, size_t out_size);
/** Reserve one LLAM-owned native thread slot in a managed runtime. Async policies
 * require qualified task entries; explicit COOPERATIVE policy permits plain C.
 * Use spawn_opts.local_executor for the root and every local child. Cross-thread
 * submissions must transfer only data valid on the destination thread. */
LLAM_API int llam_runtime_local_executor_create_ex(llam_runtime_t *runtime,
    const llam_local_executor_opts_t *opts, size_t opts_size, llam_local_executor_t **out);
/** Seal admission; existing members continue running. Idempotent, nonblocking. */
LLAM_API int llam_local_executor_close(llam_local_executor_t *executor);
/** Seal and wait for all member execution and native-thread/TLS retirement.
 * Caller cancellation/deadlines are deferred, not cleared. A member cannot drain
 * its own executor (EDEADLK). Success alone authorizes releasing borrowed domain
 * storage; fatal failure does not. Never call from a native TLS destructor.
 * Native TLS is shared by the domain: do not retain pointers to a member's stack
 * after that member returns. Local tasks still interleave; use short regions to
 * protect local multi-instruction invariants. Runtime stop seals the executor. */
LLAM_API int llam_local_executor_drain(llam_local_executor_t *executor);
/** Consume a drained executor with no concurrent public operation. */
LLAM_API int llam_local_executor_destroy(llam_local_executor_t *executor);

#ifdef __cplusplus
}
#endif
#endif
