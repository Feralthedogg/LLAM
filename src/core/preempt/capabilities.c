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
#if LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
#include "windows_async_lifetime.h"
#endif

static uint32_t llam_preempt_compiled_backends(void) {
    uint32_t backends = LLAM_PREEMPT_BACKEND_COOPERATIVE |
                        LLAM_PREEMPT_BACKEND_METADATA_V1;

#if LLAM_PLATFORM_LINUX && LLAM_ARCH_X86_64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
    backends |= LLAM_PREEMPT_BACKEND_ASYNC_CONTEXT;
#endif
#if LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
    backends |= LLAM_PREEMPT_BACKEND_ASYNC_CONTEXT;
#endif
    return backends;
}

static llam_preempt_failure_reason_t llam_preempt_unavailable_reason(
    uint32_t compiled_backends) {
    if ((compiled_backends & LLAM_PREEMPT_BACKEND_ASYNC_CONTEXT) != 0U) {
        return LLAM_PREEMPT_FAILURE_NONE;
    }
#if (LLAM_PLATFORM_LINUX || LLAM_PLATFORM_WINDOWS) && LLAM_ARCH_X86_64
    return LLAM_PREEMPT_FAILURE_BUILD_DISABLED;
#else
    return LLAM_PREEMPT_FAILURE_UNSUPPORTED_PLATFORM;
#endif
}

int llam_runtime_enable_experimental_windows_async_preempt_ex(
    llam_runtime_t *runtime,
    const llam_windows_async_preempt_opts_t *opts,
    size_t opts_size) {
    LLAM_NATIVE_API_GUARD(-1);

    llam_windows_async_preempt_opts_t copied;

    if (runtime == NULL || opts == NULL || opts_size < sizeof(copied)) {
        errno = EINVAL;
        return -1;
    }
    memcpy(&copied, opts, sizeof(copied));
    if (copied.flags != LLAM_WINDOWS_ASYNC_PREEMPT_ACK_PROCESS_FAILFAST ||
        copied.reserved0 != 0U || copied.ranges == NULL ||
        copied.range_count == 0U) {
        errno = EINVAL;
        return -1;
    }
    if (copied.range_count > 128U) {
        errno = EOVERFLOW;
        return -1;
    }
#if LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
    return llam_windows_async_lifetime_enable_certified_ranges(
        runtime, copied.ranges, (size_t)copied.range_count);
#else
    errno = ENOTSUP;
    return -1;
#endif
}

void llam_preempt_capabilities_init(llam_runtime_t *runtime) {
    uint32_t backends;
    llam_preempt_async_state_t state;
    llam_preempt_failure_reason_t reason;

    if (runtime == NULL) {
        return;
    }
    backends = llam_preempt_compiled_backends();
    reason = (backends & LLAM_PREEMPT_BACKEND_ASYNC_CONTEXT) != 0U
                 ? LLAM_PREEMPT_FAILURE_NO_SAFE_RANGES
                 : llam_preempt_unavailable_reason(backends);
    state = (backends & LLAM_PREEMPT_BACKEND_ASYNC_CONTEXT) != 0U
                ? LLAM_PREEMPT_ASYNC_DISABLED
                : LLAM_PREEMPT_ASYNC_UNAVAILABLE;
    runtime->preempt_capability.compiled_backends = backends;
    atomic_init(&runtime->preempt_capability.async_state, (unsigned)state);
    atomic_init(&runtime->preempt_capability.failure_reason, (unsigned)reason);
    atomic_init(&runtime->preempt_capability.registered_safe_ranges, 0U);
}

void llam_preempt_capabilities_note_ranges(llam_runtime_t *runtime, size_t range_count) {
    uint32_t backends;
    unsigned current_state;

    if (runtime == NULL) {
        return;
    }
    backends = runtime->preempt_capability.compiled_backends;
    atomic_store_explicit(&runtime->preempt_capability.registered_safe_ranges,
                          range_count,
                          memory_order_release);
    current_state = atomic_load_explicit(&runtime->preempt_capability.async_state,
                                         memory_order_acquire);
    if (current_state == LLAM_PREEMPT_ASYNC_ACTIVE) {
        return;
    }
    if ((backends & LLAM_PREEMPT_BACKEND_ASYNC_CONTEXT) == 0U) {
        atomic_store_explicit(&runtime->preempt_capability.async_state,
                              LLAM_PREEMPT_ASYNC_UNAVAILABLE,
                              memory_order_release);
        atomic_store_explicit(
            &runtime->preempt_capability.failure_reason,
            llam_preempt_unavailable_reason(backends),
            memory_order_release);
        return;
    }
    atomic_store_explicit(&runtime->preempt_capability.async_state,
                          range_count != 0U ? LLAM_PREEMPT_ASYNC_RANGES_READY
                                            : LLAM_PREEMPT_ASYNC_DISABLED,
                          memory_order_release);
    atomic_store_explicit(&runtime->preempt_capability.failure_reason,
                          range_count != 0U ? LLAM_PREEMPT_FAILURE_NONE
                                            : LLAM_PREEMPT_FAILURE_NO_SAFE_RANGES,
                          memory_order_release);
}

void llam_preempt_capabilities_note_async_result(
    llam_runtime_t *runtime,
    bool enabled,
    llam_preempt_failure_reason_t failure_reason) {
    uint32_t backends;
    size_t range_count;

    if (runtime == NULL) {
        return;
    }
    backends = runtime->preempt_capability.compiled_backends;
    range_count = atomic_load_explicit(
        &runtime->preempt_capability.registered_safe_ranges,
        memory_order_acquire);
    if (enabled && (backends & LLAM_PREEMPT_BACKEND_ASYNC_CONTEXT) != 0U &&
        range_count != 0U) {
        atomic_store_explicit(&runtime->preempt_capability.async_state,
                              LLAM_PREEMPT_ASYNC_ACTIVE,
                              memory_order_release);
        atomic_store_explicit(&runtime->preempt_capability.failure_reason,
                              LLAM_PREEMPT_FAILURE_NONE,
                              memory_order_release);
        return;
    }
    if ((backends & LLAM_PREEMPT_BACKEND_ASYNC_CONTEXT) == 0U) {
        atomic_store_explicit(&runtime->preempt_capability.async_state,
                              LLAM_PREEMPT_ASYNC_UNAVAILABLE,
                              memory_order_release);
        if (failure_reason == LLAM_PREEMPT_FAILURE_NONE) {
            failure_reason = llam_preempt_unavailable_reason(backends);
        }
    } else {
        atomic_store_explicit(&runtime->preempt_capability.async_state,
                              range_count != 0U ? LLAM_PREEMPT_ASYNC_RANGES_READY
                                                : LLAM_PREEMPT_ASYNC_DISABLED,
                              memory_order_release);
        if (failure_reason == LLAM_PREEMPT_FAILURE_NONE && range_count == 0U) {
            failure_reason = LLAM_PREEMPT_FAILURE_NO_SAFE_RANGES;
        }
    }
    atomic_store_explicit(&runtime->preempt_capability.failure_reason,
                          failure_reason,
                          memory_order_release);
}

llam_preempt_effective_policy_t llam_preempt_effective_policy_for(
    unsigned preempt_mode,
    uint32_t compiled_backends,
    bool async_enabled,
    size_t registered_safe_ranges) {
    if (preempt_mode == LLAM_PREEMPT_OFF) {
        return LLAM_PREEMPT_EFFECTIVE_OFF;
    }
    if (preempt_mode < LLAM_PREEMPT_AUTO ||
        (compiled_backends & LLAM_PREEMPT_BACKEND_ASYNC_CONTEXT) == 0U ||
        !async_enabled || registered_safe_ranges == 0U) {
        return LLAM_PREEMPT_EFFECTIVE_COOPERATIVE;
    }
    /* Registered PCs are async-safe; all other code remains cooperative. */
    return LLAM_PREEMPT_EFFECTIVE_HYBRID;
}

const char *llam_preempt_effective_policy_name(llam_preempt_effective_policy_t policy) {
    switch (policy) {
    case LLAM_PREEMPT_EFFECTIVE_OFF:
        return "off";
    case LLAM_PREEMPT_EFFECTIVE_COOPERATIVE:
        return "cooperative";
    case LLAM_PREEMPT_EFFECTIVE_HYBRID:
        return "hybrid";
    default:
        return "unknown";
    }
}

const char *llam_preempt_async_state_name(llam_preempt_async_state_t state) {
    switch (state) {
    case LLAM_PREEMPT_ASYNC_UNAVAILABLE:
        return "unavailable";
    case LLAM_PREEMPT_ASYNC_DISABLED:
        return "disabled";
    case LLAM_PREEMPT_ASYNC_RANGES_READY:
        return "ranges-ready";
    case LLAM_PREEMPT_ASYNC_ACTIVE:
        return "active";
    case LLAM_PREEMPT_ASYNC_PREPARED:
        return "prepared";
    default:
        return "unknown";
    }
}

const char *llam_preempt_failure_reason_name(llam_preempt_failure_reason_t reason) {
    switch (reason) {
    case LLAM_PREEMPT_FAILURE_NONE:
        return "none";
    case LLAM_PREEMPT_FAILURE_BUILD_DISABLED:
        return "build-disabled";
    case LLAM_PREEMPT_FAILURE_UNSUPPORTED_PLATFORM:
        return "unsupported-platform";
    case LLAM_PREEMPT_FAILURE_COOPERATIVE_MODE:
        return "cooperative-mode";
    case LLAM_PREEMPT_FAILURE_NO_SAFE_RANGES:
        return "no-safe-ranges";
    case LLAM_PREEMPT_FAILURE_SINGLE_SHARD_REQUIRED:
        return "single-shard-required";
    case LLAM_PREEMPT_FAILURE_DETERMINISTIC_REQUIRED:
        return "deterministic-required";
    case LLAM_PREEMPT_FAILURE_HELPER_ACTIVE:
        return "helper-active";
    case LLAM_PREEMPT_FAILURE_SIGNAL_CONFLICT:
        return "signal-conflict";
    case LLAM_PREEMPT_FAILURE_CONTEXT_UNSUPPORTED:
        return "context-unsupported";
    case LLAM_PREEMPT_FAILURE_CONTEXT_ALLOCATION:
        return "context-allocation";
    case LLAM_PREEMPT_FAILURE_CAPTURE_SLOT_BUSY:
        return "capture-slot-busy";
    case LLAM_PREEMPT_FAILURE_SIGNAL_FRAME_UNSUPPORTED:
        return "signal-frame-unsupported";
    case LLAM_PREEMPT_FAILURE_WORKER_START:
        return "worker-start";
    case LLAM_PREEMPT_FAILURE_DYNAMIC_UNSUPPORTED:
        return "dynamic-unsupported";
    case LLAM_PREEMPT_FAILURE_INVALID_CONFIG:
        return "invalid-config";
    default:
        return "unknown";
    }
}

#if !defined(__linux__) || !defined(__x86_64__)
int llam_preempt_module_register(llam_runtime_t *runtime,
                                 uintptr_t image_base,
                                 size_t image_size,
                                 const void *metadata,
                                 size_t metadata_size,
                                 bool required_mode) {
    (void)runtime;
    (void)image_base;
    (void)image_size;
    (void)metadata;
    (void)metadata_size;
    if (required_mode) {
        errno = ENOTSUP;
        return -1;
    }
    return 0;
}
#endif
