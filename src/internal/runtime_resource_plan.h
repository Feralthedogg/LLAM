/**
 * @file src/internal/runtime_resource_plan.h
 * @brief Pure runtime resource-plan resolution contract.
 *
 * @details
 * Initialization resolves a caller-sized public option prefix and discovered
 * platform capabilities into this fixed-size value before allocating shards,
 * mappings, or native threads. The resolver owns no resources and publishes no
 * partial result on failure.
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

#ifndef LLAM_RUNTIME_RESOURCE_PLAN_H
#define LLAM_RUNTIME_RESOURCE_PLAN_H

#include "llam/runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Maximum scheduler capacity and selected worker CPUs per runtime. */
#define LLAM_RUNTIME_MAX_WORKERS 256U
/** @brief Maximum blocking-worker capacity per runtime. */
#define LLAM_RUNTIME_MAX_BLOCKING_WORKERS 256U
/** @brief Maximum exact runtime-total stack prewarm target. */
#define LLAM_RUNTIME_MAX_STACK_PREWARM 4096U
/** @brief Conservative default-stack plus guard mapping estimate. */
#define LLAM_RUNTIME_STACK_MAPPING_ESTIMATE_BYTES (UINT64_C(128) * UINT64_C(1024))
/** @brief Maximum aggregate metadata estimate accepted by the first policy. */
#define LLAM_RUNTIME_METADATA_BUDGET_BYTES (UINT64_C(1024) * UINT64_C(1024) * UINT64_C(1024))

/** @brief Immutable capability and option inputs for resource planning. */
typedef struct llam_runtime_resource_plan_input {
    const llam_runtime_opts_t *opts; /**< Optional caller-sized public option prefix. */
    size_t opts_size;                /**< Bytes available through @c opts, or 0 when NULL. */
    const unsigned *allowed_cpus;    /**< Ordered process-allowed native CPU IDs. */
    unsigned allowed_cpu_count;      /**< Number of entries in @c allowed_cpus. */
    bool affinity_supported;         /**< Whether native thread affinity can be applied. */
    bool sqpoll_supported;           /**< Whether the platform can create an SQPOLL ring. */
    size_t page_size;                /**< Platform VM page size for byte-policy alignment. */
} llam_runtime_resource_plan_input_t;

/** @brief Fully resolved runtime resource authority. */
typedef struct llam_runtime_resource_plan {
    unsigned worker_min;      /**< Minimum online scheduler workers. */
    unsigned worker_count;    /**< Initial online scheduler workers. */
    unsigned worker_max;      /**< Allocated scheduler capacity. */
    unsigned blocking_min;    /**< Blocking workers created during initialization. */
    unsigned blocking_max;    /**< Maximum blocking-worker capacity. */
    unsigned affinity_policy; /**< Active ::llam_runtime_affinity_policy_t. */
    unsigned selected_cpu_count; /**< Worker CPU IDs stored in @c selected_cpus. */
    unsigned selected_cpus[LLAM_RUNTIME_MAX_WORKERS]; /**< Ordered worker CPU IDs. */
    bool sqpoll_reserved; /**< Whether @c sqpoll_cpu is excluded from worker CPUs. */
    int sqpoll_cpu;       /**< Resolved SQPOLL CPU, or -1 when automatic/unassigned. */
    uint64_t task_prewarm_total;  /**< Exact runtime-total task-object target. */
    uint64_t stack_prewarm_total; /**< Exact runtime-total stack target. */
    uint64_t timer_prewarm_total; /**< Exact runtime-total timer-slot target. */
    uint64_t estimated_metadata_bytes;      /**< Checked aggregate metadata estimate. */
    uint64_t estimated_stack_mapping_bytes; /**< Checked stack mapping estimate. */
    uint64_t stack_cache_budget_bytes;      /**< Runtime-wide retained mapping budget. */
    uint64_t stack_cache_high_watermark_bytes; /**< Automatic trim trigger. */
    uint64_t stack_cache_low_watermark_bytes;  /**< Automatic trim target. */
    uint64_t stack_cache_idle_ns;              /**< Opportunistic idle-trim age. */
    unsigned stack_cache_flags;                /**< LLAM_RUNTIME_STACK_CACHE_F_* policy. */
} llam_runtime_resource_plan_t;

/**
 * @brief Compute task objects physically reserved by shard-local slab rounding.
 *
 * @param logical_total Exact logical runtime-total target.
 * @param worker_count  Number of shard allocators receiving a share.
 * @param out_objects   Receives the checked physical task-object count.
 * @return true on success, false for invalid input or arithmetic overflow.
 */
bool llam_runtime_task_prewarm_storage_objects(uint64_t logical_total,
                                               unsigned worker_count,
                                               uint64_t *out_objects);

/**
 * @brief Resolve a complete immutable resource plan without side effects.
 *
 * @param input Discovered platform capabilities and optional public options.
 * @param out   Destination plan. Cleared before validation and left empty on
 *              every failure.
 * @return 0 on success, -1 with @c errno set on invalid, unsupported,
 *         over-budget, or overflowing input.
 */
int llam_runtime_resource_plan_resolve(const llam_runtime_resource_plan_input_t *input,
                                       llam_runtime_resource_plan_t *out);

#endif
