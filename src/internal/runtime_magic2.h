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

#ifndef LLAM_RUNTIME_MAGIC2_H
#define LLAM_RUNTIME_MAGIC2_H

#include "magic2.h"

/*
 * Private source interface; neither this header nor magic2 symbols form part
 * of the installed C ABI. Use this bridge instead of calling magic2 directly.
 *
 * A managed caller parks on the existing blocking pool. Unmanaged callers run
 * synchronously. Call without runtime locks, outside runtime-critical,
 * preemption-disabled, and announced foreign regions. Callbacks execute as
 * unmanaged code and must not depend on the originating scheduler thread/TLS.
 *
 * Return 0 when dispatch completed, or -1 with LLAM submission/cancellation
 * errno. Independently, *dispatch_status receives the magic2 result, or
 * MAGIC2_ENOTPROCESSED if dispatch never started. An executing request drains
 * before cancellation returns: its magic2 result and any prior writes remain
 * observable even when this bridge returns -1/ECANCELED. Never retry solely
 * because LLAM reported cancellation. The request's implementation_status
 * remains the kernel result under magic2's operation-specific contract.
 *
 * Keep the request, result, referenced descriptors, callbacks, and all their
 * storage live until return. Async operations may retain their own resources
 * after dispatch; their magic2 handle/release lifetimes still apply. Serialize
 * mutation/destruction of one context or frame as required by magic2. Adaptive
 * training may replay eligible kernels; the bridge adds no retries or tuning.
 */
int llam_magic2_dispatch(magic2_dispatch_request *request, int *dispatch_status);

/*
 * Direct CPU plans use one existing managed G and its current P. Only
 * MAGIC2_CPU_SCHEDULE_CALLER with worker_limit == 1 is supported; other valid
 * schedules return MAGIC2_EUNSUPPORTED. Parallel activations use ordinary
 * LLAM tasks, each with exclusive regions/workspace. No native CPU pool or
 * runtime-owned magic2 state is created. These four ownership/metadata helpers
 * return magic2 results directly and preserve its retain/release contract.
 */
int llam_magic2_cpu_plan_create(const magic2_cpu_plan_spec *spec,
                               magic2_cpu_plan **out_plan);
int llam_magic2_cpu_plan_retain(magic2_cpu_plan *plan);
void llam_magic2_cpu_plan_release(magic2_cpu_plan **plan);
int llam_magic2_cpu_plan_get_info(const magic2_cpu_plan *plan,
                                 magic2_cpu_plan_info *info);

/*
 * Managed-only activation. As with dispatch, return 0 for a completed magic2
 * invocation or -1 with LLAM errno, and report the independent magic2 result
 * in *run_status (required, disjoint from all other storage). status is optional
 * and otherwise follows MAGIC2_CPU_RUN_STATUS_INIT and magic2's alias rules.
 * An unsafe/unmanaged context fails with EBUSY before execution.
 *
 * A retained plan and the current task's runtime stay live across tile yields.
 * Cancellation/stop before admission leaves run_status ENOTPROCESSED and does
 * not initialize output ports. Once admitted, every started tile finishes;
 * cancellation stops acquisition of further tiles and reports ECANCELLED in
 * magic2 status plus -1/ECANCELED from LLAM. Kernel/runtime failures take
 * precedence in magic2 status even if LLAM also observes cancellation. Prior
 * writes remain visible and no tile is retried. Keep all regions/workspace
 * exclusively owned until return; a cancelled join of the containing task is
 * not completion and does not end those lifetimes.
 *
 * The bridge yields between tiles, including with automatic preemption OFF,
 * and rechecks CPU/FP eligibility after migration. Choose bounded CPU tiles:
 * callbacks must not block, wait, start native pools, retain scratch/ports,
 * depend on native-thread affinity, or leave runtime/foreign guards unbalanced.
 * Long callbacks and initial ZERO fills can delay progress and shutdown.
 * Owner retain/release callbacks must also be safe in their calling domain.
 */
int llam_magic2_cpu_run(magic2_cpu_plan *plan,
                       const magic2_region *regions, size_t region_count,
                       void *workspace, size_t workspace_bytes,
                       magic2_cpu_run_status *status, int *run_status);

/* Internal, explicitly described CPU work; never inferred from a task entry. */
typedef struct llam_magic2_cpu_work llam_magic2_cpu_work_t;
typedef int (*llam_magic2_cpu_baseline_fn)(void *arg,
                                         magic2_cpu_run_status *status,
                                         int *run_status);

enum {
    LLAM_MAGIC2_CPU_WORK_TRANSFORM = 1,
    LLAM_MAGIC2_CPU_WORK_ZERO = 2,
    LLAM_MAGIC2_CPU_AUTO_BASELINE = 0,
    LLAM_MAGIC2_CPU_AUTO_FAMILY = 1,
    LLAM_MAGIC2_CPU_AUTO_BUSY = 2,
    LLAM_MAGIC2_CPU_AUTO_NO_CANDIDATE = 3
};

typedef struct llam_magic2_cpu_work_spec {
    uint32_t kind;
    const magic2_cpu_plan_spec *plans;
    size_t plan_count;              /* 1..8 equivalent, bounded CALLER plans. */
    llam_magic2_cpu_baseline_fn baseline;
    uint64_t estimated_cost_ns;     /* Internal producer's hint, zero is unknown. */
    uint64_t minimum_cost_ns;       /* Zero selects a conservative 50 us floor. */
} llam_magic2_cpu_work_spec_t;

typedef struct llam_magic2_cpu_auto_info {
    uint32_t route;
    uint32_t bucket;                /* 0: quiet, 1: runnable, 2: I/O/timer; UINT32_MAX: unsampled. */
    uint32_t slot_budget;           /* Always one existing LLAM G/P slot. */
    uint32_t selected_index;        /* UINT32_MAX when no family plan ran. */
    uint32_t observation_valid;
    int admission_status;          /* EBUSY means reservation lost before effects. */
    uint64_t minimum_cost_ns;
    uint64_t elapsed_ns;
} llam_magic2_cpu_auto_info_t;

/*
 * Private plumbing for known internal transforms/zero kernels. The producer
 * guarantees equivalent exact-shape semantics for every plan and baseline,
 * bounded cooperative tiles, no native pools, and cancellation/partial-failure
 * handling compatible with cpu_run. Baseline uses the same two result channels.
 * This does not make arbitrary callbacks eligible for automatic decomposition.
 * Create/release return magic2 ownership semantics; descriptors are not public
 * handles. The producer keeps descriptor/baseline state live through return on
 * the below-floor fast path. Heavier activations additionally retain their own
 * reference across admission and tile yields.
 */
int llam_magic2_cpu_work_create(const llam_magic2_cpu_work_spec_t *spec,
                                llam_magic2_cpu_work_t **out_work,
                                magic2_storage *workspace);
void llam_magic2_cpu_work_release(llam_magic2_cpu_work_t **work);

/*
 * Managed-only. Pressure raises the cost floor by 2x/4x. A nonblocking family
 * reservation serializes observations for this descriptor; concurrent calls
 * use baseline immediately. It is not a lock held by a waiting G. Only actual
 * successful activations train the family; kernel failures disable that plan,
 * cancellation does not. No failure after admission falls back or replays work.
 * Baseline also runs exactly once for small/unknown work or unavailable family
 * admission. All outputs, baseline_arg, regions, and workspace must be disjoint
 * where their contracts require and remain live through return. info is optional
 * and must not overlap any other storage. No new installed API or runtime state.
 */
int llam_magic2_cpu_auto(llam_magic2_cpu_work_t *work, void *baseline_arg,
                        const magic2_region *regions, size_t region_count,
                        void *workspace, size_t workspace_bytes,
                        magic2_cpu_run_status *status, int *run_status,
                        llam_magic2_cpu_auto_info_t *info);

#endif
