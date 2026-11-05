/**
 * @file src/core/lifecycle/runtime.c
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

/** @brief Legacy process-default runtime state; behavior lives in subsystem modules. */
llam_runtime_t g_llam_runtime;
/** @brief Low 32 bits of the enabled x86 XSAVE mask. */
uint32_t g_llam_xsave_mask_lo;
/** @brief High 32 bits of the enabled x86 XSAVE mask. */
uint32_t g_llam_xsave_mask_hi;
/** @brief FP-control context flag used by platform context switch code. */
uint32_t g_llam_fp_control_context;
#if LLAM_PLATFORM_DARWIN
/* Dyld owns the storage. The libc key only caches its address for bound Ms;
 * it owns no allocation and installs no callback into an unloadable image. */
static _Thread_local llam_scheduler_tls_t g_llam_scheduler_tls;
static pthread_key_t g_llam_scheduler_tls_key;
static bool g_llam_scheduler_tls_key_ready;

__attribute__((constructor))
static void llam_scheduler_tls_cache_initialize(void) {
    g_llam_scheduler_tls_key_ready = pthread_key_create(&g_llam_scheduler_tls_key, NULL) == 0;
}

__attribute__((destructor))
static void llam_scheduler_tls_cache_destroy(void) {
    if (g_llam_scheduler_tls_key_ready) {
        (void)pthread_key_delete(g_llam_scheduler_tls_key);
        g_llam_scheduler_tls_key_ready = false;
    }
}

__attribute__((noinline, pure))
llam_scheduler_tls_t *llam_scheduler_tls_current(void) {
    /* Read the mutable libc TSD value, not a const native TLS address. Calls
     * may share this read until an opaque operation such as ctx_switch. */
    if (g_llam_scheduler_tls_key_ready) {
        llam_scheduler_tls_t *cached = pthread_getspecific(g_llam_scheduler_tls_key);
        if (cached != NULL) return cached;
    }
    return &g_llam_scheduler_tls;
}

void llam_scheduler_tls_cache_bind_current(void) {
    if (g_llam_scheduler_tls_key_ready) {
        /* Failure only loses the cache: the dyld TLS fallback remains valid. */
        (void)pthread_setspecific(g_llam_scheduler_tls_key, &g_llam_scheduler_tls);
    }
}
#else
static _Thread_local llam_scheduler_tls_t g_llam_scheduler_tls;

LLAM_NOINLINE llam_scheduler_tls_t *llam_scheduler_tls_current(void) {
    /* Keep the read-only lookup from becoming a const native TLS address
     * under interprocedural optimization. This compiler fence emits no CPU
     * synchronization and leaves shared-state publication to its own atomics. */
    atomic_signal_fence(memory_order_acq_rel);
    return &g_llam_scheduler_tls;
}
#endif
