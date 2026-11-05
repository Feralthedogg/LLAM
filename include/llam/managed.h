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

#ifndef LLAM_MANAGED_H
#define LLAM_MANAGED_H

#include "llam/runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LLAM_MANAGED_COMPILER_ABI_MAJOR 1U
#define LLAM_MANAGED_COMPILER_ABI_MINOR 0U

/** Explicit same-image managed call contract for a function declared in another
 * translation unit. Use LLAM_MANAGED_TARGET(function_name)(arguments) with the
 * LLVM managed driver. The final image must contain the exact typed managed
 * definition; a native-only library, missing metadata or ABI mismatch fails the
 * link/verifier. This marker cannot escape, be stored or select a dynamic target.
 * It has no runtime export and is unavailable in plain compiler mode. */
#if defined(__clang__)
extern void (*__llam_managed_target(void (*native_entry)(void)))(void);
#define LLAM_MANAGED_TARGET(function_name) \
    ((__typeof__(&(function_name)))__llam_managed_target((void (*)(void))(function_name)))
#endif

typedef struct llam_module llam_module_t;

/** Backend/admission policy, independent of llam_preempt_mode_t. */
typedef enum llam_preempt_policy {
    LLAM_PREEMPT_POLICY_ASYNC_AUTO = 0,
    LLAM_PREEMPT_POLICY_ASYNC_REQUIRED = 1,
    LLAM_PREEMPT_POLICY_COOPERATIVE = 2
} llam_preempt_policy_t;

/** Report-only effective policy: qualified async Gs and isolated legacy Gs. */
#define LLAM_PREEMPT_EFFECTIVE_MIXED UINT32_C(3)

typedef enum llam_managed_execution_class {
    LLAM_MANAGED_EXECUTION_LEGACY = 0,
    LLAM_MANAGED_EXECUTION_PORTABLE = 1,
    LLAM_MANAGED_EXECUTION_NATIVE_AFFINE = 2
} llam_managed_execution_class_t;

typedef enum llam_preempt_reason {
    LLAM_PREEMPT_REASON_NONE = 0,
    LLAM_PREEMPT_REASON_LEGACY_RUNTIME = 1,
    LLAM_PREEMPT_REASON_REQUEST_DISABLED = 2,
    LLAM_PREEMPT_REASON_COOPERATIVE_POLICY = 3,
    LLAM_PREEMPT_REASON_ADAPTER_UNAVAILABLE = 4,
    LLAM_PREEMPT_REASON_LEGACY_MIX_UNQUALIFIED = 5,
    LLAM_PREEMPT_REASON_ISA_UNAVAILABLE = 6,
    LLAM_PREEMPT_REASON_LEGACY_MIXED = 7,
    LLAM_PREEMPT_REASON_PROBE_PENDING = 8,
    LLAM_PREEMPT_REASON_STARTUP_FAILED = 9
} llam_preempt_reason_t;

/** Zero budget selects the 64 MiB bring-up profile; it is not an RSS limit.
 * All flags and reserved fields currently must be zero. Zero affine capacity
 * selects four dedicated native M slots; at most 1024 may be requested.
 * A slot is replenished with a fresh, probed OS thread after the prior thread
 * and its TLS destructors exit. Admission can return EAGAIN during refill. */
typedef struct llam_preempt_opts {
    uint32_t policy;
    uint32_t flags;
    uint64_t capsule_budget_bytes;
    uint32_t native_affine_capacity;
    uint32_t reserved0;
} llam_preempt_opts_t;

/** metadata_address is (uint64_t)(uintptr_t) of the image's LLPM2 section.
 * The caller keeps its native loader reference alive throughout registration.
 * LLAM obtains an independent reference and validates native image boundaries.
 * Heap blobs and writable metadata are never registration certificates. */
typedef struct llam_module_opts {
    uint64_t metadata_address;
    uint64_t metadata_bytes;
    uint32_t flags;
    uint32_t reserved0;
} llam_module_opts_t;

#define LLAM_PREEMPT_INFO_F_RUNNING UINT32_C(1)
#define LLAM_PREEMPT_INFO_F_COMMITTED UINT32_C(2)

/** Requested and effective policy are reported separately. Effective policy
 * describes the latest startup attempt; flags distinguish an active run and
 * its committed barrier. Zero per-G charge means no native allocation profile
 * is prepared. A positive charge with zero maximum_charged_tasks means the
 * configured budget cannot admit even one capsule; neither is unlimited.
 * Counts include in-flight admission and retained, not-yet-reclaimed tasks. */
typedef struct llam_preempt_info {
    uint32_t requested_policy;
    uint32_t effective_policy;
    uint32_t reason;
    uint32_t flags;
    uint64_t capsule_budget_bytes;
    uint64_t capsule_charge_bytes;
    uint64_t conservative_task_charge_bytes;
    uint64_t maximum_charged_tasks;
    uint64_t registered_modules;
    uint64_t registered_entries;
    uint64_t retained_references;
    uint32_t native_affine_capacity;
    uint32_t reserved0;
    uint64_t excluded_tasks; /**< Retained tasks excluded by NO_PREEMPT. */
    uint64_t legacy_tasks;
    uint64_t native_affine_tasks;
    uint32_t native_affine_available;
    uint32_t portable_processors;
    uint32_t compatibility_processors;
    uint32_t reserved1;
    /** Configured native request/check interval, indexed by llam_task_class_t.
     * All zero when inactive or unqualified; these are not observed latencies. */
    uint64_t effective_quantum_ns[3];
    uint64_t reserved_capsule_bytes; /**< Prepared storage, including free slots; not RSS. */
    uint64_t prepared_capsules;
    uint64_t available_capsules;
} llam_preempt_info_t;

#define LLAM_PREEMPT_OPTS_CURRENT_SIZE ((size_t)sizeof(llam_preempt_opts_t))
#define LLAM_MODULE_OPTS_CURRENT_SIZE ((size_t)sizeof(llam_module_opts_t))
#define LLAM_PREEMPT_INFO_CURRENT_SIZE ((size_t)sizeof(llam_preempt_info_t))

/**
 * @brief Fill managed options with their defaults: ASYNC_REQUIRED, 64 MiB, four slots.
 *
 * @details A zeroed ::llam_preempt_opts_t differs: its @c policy is ASYNC_AUTO.
 *
 * @param[out] out Options to initialize. Must not be NULL.
 * @param out_size Size of the caller's ::llam_preempt_opts_t; must be nonzero. LLAM
 * writes only the prefix it knows.
 * @return 0 on success, -1 with @c errno set.
 */
LLAM_API int llam_preempt_opts_init(llam_preempt_opts_t *out, size_t out_size);
/**
 * @brief Create a runtime with an explicit managed policy and resource limits.
 *
 * @details Advanced host-only policy constructor. The default policy is
 * ASYNC_REQUIRED, as for the ordinary constructors. ASYNC_AUTO and COOPERATIVE are
 * explicit alternatives. Creation may inspect and pin native loader images; calling
 * from a LLAM task returns EBUSY.
 *
 * @param runtime_opts Optional runtime options; pass NULL for defaults.
 * @param runtime_opts_size Size of @p runtime_opts when non-NULL; must then be nonzero.
 * @param preempt_opts Optional managed options; pass NULL with size 0 for the defaults
 * of ::llam_preempt_opts_init.
 * @param preempt_opts_size Size of @p preempt_opts; must be nonzero when it is non-NULL
 * and 0 when it is NULL.
 * @param[out] out Receives the runtime handle; set to NULL on failure. Must not be NULL.
 * @return 0 on success, -1 with @c errno set.
 */
LLAM_API int llam_runtime_create_managed(
    const llam_runtime_opts_t *runtime_opts, size_t runtime_opts_size,
    const llam_preempt_opts_t *preempt_opts, size_t preempt_opts_size,
    llam_runtime_t **out);
/**
 * @brief Copy the runtime's requested and effective policy and storage accounting.
 *
 * @details A compatibility runtime reports ::LLAM_PREEMPT_REASON_LEGACY_RUNTIME.
 *
 * @param runtime Live runtime; not NULL.
 * @param[out] out Receives the snapshot. Must not be NULL.
 * @param out_size Size of the caller's ::llam_preempt_info_t; must be nonzero. LLAM
 * writes only the compatible prefix.
 * @return 0 on success, -1 with @c errno set.
 */
LLAM_API int llam_runtime_preempt_info_ex(
    llam_runtime_t *runtime, llam_preempt_info_t *out, size_t out_size);
/**
 * @brief Register one loaded image's managed metadata with a stopped runtime.
 *
 * @details Host only; a task gets @c EBUSY. Keep the caller's loader reference alive
 * during this call. LLAM obtains its own reference and validates the image.
 *
 * @param runtime Runtime with stopped dispatch; not NULL.
 * @param opts Module options, with nonzero metadata fields and zero flags.
 * @param opts_size Size of @p opts; must include @c metadata_address and
 * @c metadata_bytes, so at least 16 bytes.
 * @param[out] out Receives the module handle; set to NULL on failure. Must not be NULL.
 * @return 0 on success, -1 with @c errno set.
 */
LLAM_API int llam_runtime_module_register_ex(
    llam_runtime_t *runtime, const llam_module_opts_t *opts, size_t opts_size,
    llam_module_t **out);
/**
 * @brief Unregister a module and release LLAM's loader reference.
 *
 * @details Success consumes the handle. EBUSY preserves it; another runtime is EXDEV.
 * Registration changes require host context, stopped dispatch, and no retained
 * G, capsule, native operation or catalog reader.
 *
 * @param runtime Runtime that registered the module; not NULL.
 * @param module Module handle; not NULL.
 * @return 0 on success, -1 with @c errno set.
 */
LLAM_API int llam_runtime_module_unregister(llam_runtime_t *runtime, llam_module_t *module);

/**
 * @brief Execute one native callback on the current task's dedicated native thread.
 *
 * @details The compiler classifies callers as NATIVE_AFFINE. A qualified affine task
 * releases its logical processor before the callback and reacquires a fresh
 * execute/drain lease on the same native thread before returning. The callback
 * may block and use native TLS; it cannot call stateful LLAM APIs or leave by
 * longjmp/exception. Its result, errno and Windows LastError are preserved.
 *
 * Returns 0 after callback completion, including a NULL callback result. A stop
 * or cancellation after callback entry does not skip reacquisition or discard
 * the result. Before callback entry, invalid arguments return EINVAL, a missing
 * affine execution context returns ENOTSUP, stop/cancellation returns ECANCELED,
 * and a racing processor-ownership change returns EBUSY. On those failures *out
 * is NULL when out is valid. Native callback
 * reentry is rejected before touching caller arguments, as for other LLAM APIs.
 *
 * @param fn Callback to run. Must not be NULL.
 * @param arg User pointer passed to @p fn.
 * @param[out] out Receives the callback result; NULL is a valid result. Must not be
 * NULL.
 * @return 0 after callback completion, -1 with @c errno set before callback entry.
 */
LLAM_API int llam_call_native_affine_result(llam_blocking_fn fn, void *arg, void **out);

/**
 * @brief Reserved for compiler-generated code. Application code must not call these.
 *
 * @details Compiler ABI, not task admission. The qualified compiler emits balanced
 * native entry/exit calls. They preserve errno/LastError and never allocate,
 * resolve dynamic TLS or acquire a lock. A native callback has no managed G/P
 * context. Unbalanced guards fail closed; they are not a public disable switch.
 * Runtime managed creation and LLPM2 admission are separate contracts.
 */
LLAM_API void llam_managed_native_enter(void);
LLAM_API void llam_managed_native_leave(void);

#ifdef __cplusplus
}
#endif
#endif /* LLAM_MANAGED_H */
